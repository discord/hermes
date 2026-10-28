/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "JSONLexer.h"

#include "hermes/VM/StringPrimitive.h"
#include "llvh/ADT/ScopeExit.h"

#include "dtoa/dtoa.h"

namespace hermes {
namespace vm {

static const char *TrueString = "true";
static const char *FalseString = "false";
static const char *NullString = "null";

static bool isJSONWhiteSpace(char16_t ch) {
  // JSONWhiteSpace includes <TAB>, <CR>, <LF>, <SP>.
  return (ch == u'\t' || ch == u'\r' || ch == u'\n' || ch == u' ');
}

ExecutionStatus JSONLexer::advance() {
  return advanceHelper(false);
}

ExecutionStatus JSONLexer::advanceStrAsSymbol() {
  return advanceHelper(true);
}

ExecutionStatus JSONLexer::advanceHelper(bool forKey) {
  if (ascii_)
    return advanceHelperAscii(forKey);
  // Skip whitespaces.
  while (curCharPtr_.hasChar() && isJSONWhiteSpace(*curCharPtr_)) {
    ++curCharPtr_;
  }

  // End of buffer.
  if (!curCharPtr_.hasChar()) {
    token_.setEof();
    return ExecutionStatus::RETURNED;
  }

  token_.setFirstChar(*curCharPtr_);

#define PUNC(ch, tok)          \
  case ch:                     \
    token_.setPunctuator(tok); \
    ++curCharPtr_;             \
    return ExecutionStatus::RETURNED

#define WORD(ch, word, tok) \
  case ch:                  \
    return scanWord(word, tok)

  switch (*curCharPtr_) {
    PUNC(u'{', JSONTokenKind::LBrace);
    PUNC(u'}', JSONTokenKind::RBrace);
    PUNC(u'[', JSONTokenKind::LSquare);
    PUNC(u']', JSONTokenKind::RSquare);
    PUNC(u',', JSONTokenKind::Comma);
    PUNC(u':', JSONTokenKind::Colon);
    WORD(u't', TrueString, JSONTokenKind::True);
    WORD(u'f', FalseString, JSONTokenKind::False);
    WORD(u'n', NullString, JSONTokenKind::Null);

    case u'-':
    case u'0':
    case u'1':
    case u'2':
    case u'3':
    case u'4':
    case u'5':
    case u'6':
    case u'7':
    case u'8':
    case u'9':
      return scanNumber();

    case u'"':
      if (forKey) {
        return scanString<StrAsSymbol>();
      } else {
        return scanString<StrAsValue>();
      }

    default:
      return errorWithChar(u"Unexpected character: ", *curCharPtr_);
  }
}

CallResult<char16_t> JSONLexer::consumeUnicode() {
  uint16_t val = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (!curCharPtr_.hasChar()) {
      return error("Unexpected end of input");
    }
    int ch = *curCharPtr_ | 32;
    if (ch >= '0' && ch <= '9') {
      ch -= '0';
    } else if (ch >= 'a' && ch <= 'f') {
      ch -= 'a' - 10;
    } else {
      return errorWithChar(u"Invalid unicode point character: ", *curCharPtr_);
    }
    val = (val << 4) + ch;
    ++curCharPtr_;
  }

  return static_cast<char16_t>(val);
}

ExecutionStatus JSONLexer::scanNumber() {
  llvh::SmallVector<char, 32> str8;
  while (curCharPtr_.hasChar()) {
    auto ch = *curCharPtr_;
    if (!(ch == u'-' || ch == u'+' || ch == u'.' || (ch | 32) == u'e' ||
          (ch >= u'0' && ch <= u'9'))) {
      break;
    }
    str8.push_back(ch);
    ++curCharPtr_;
  }

  size_t len = str8.size();
  assert(len > 0 && "scanNumber must be called on a number-looking char");
  if (str8[0] == '0' && len > 1 && str8[1] >= '0' && str8[1] <= '9') {
    // The integer part cannot start with 0, unless it's 0.
    return errorWithChar(u"Unexpected character in number: ", str8[1]);
  }

  str8.push_back('\0');

  char *endPtr;
  double value = ::hermes_g_strtod(str8.data(), &endPtr);
  if (endPtr != str8.data() + len) {
    return errorWithChar(u"Unexpected character in number: ", *endPtr);
  }
  token_.setNumber(value);
  return ExecutionStatus::RETURNED;
}

template <typename ForKey>
ExecutionStatus JSONLexer::scanString() {
  assert(*curCharPtr_ == '"');
  ++curCharPtr_;
  bool hasEscape = false;
  // Ideally we don't have to use tmpStorage. In the case of a plain string with
  // no escapes, we construct an ArrayRef at the end of scanning that points to
  // the beginning and end of the string.
  SmallU16String<32> tmpStorage;
  curCharPtr_.beginCapture();
  // Make sure we don't somehow leave a dangling open capture.
  auto ensureCaptureClosed =
      llvh::make_scope_exit([this] { curCharPtr_.cancelCapture(); });
  bool allAscii = true;
  hermes::JenkinsHash hash = hermes::JenkinsHashInit;

  while (curCharPtr_.hasChar()) {
    if (*curCharPtr_ == '"') {
      // End of string.
      llvh::ArrayRef<char16_t> strRef =
          hasEscape ? tmpStorage.arrayRef() : curCharPtr_.endCapture();
      ++curCharPtr_;
      if constexpr (ForKey::value) {
        auto symRes = runtime_.getIdentifierTable().getSymbolHandle(
            runtime_, strRef, hash);
        if (symRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        token_.setSymbol(symRes->get());
        return ExecutionStatus::RETURNED;
      }
      auto strRes =
          StringPrimitive::createWithKnownEncoding(runtime_, strRef, allAscii);
      if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      token_.setString(vmcast<StringPrimitive>(*strRes));
      return ExecutionStatus::RETURNED;
    } else if (*curCharPtr_ <= '\u001F') {
      return error(u"U+0000 thru U+001F is not allowed in string");
    }
    char16_t scannedChar = -1;
    if (*curCharPtr_ == u'\\') {
      if (!hasEscape) {
        // This is the first escape character encountered, so append everything
        // we've seen so far to tmpStorage.
        tmpStorage.append(curCharPtr_.endCapture());
      }
      hasEscape = true;
      ++curCharPtr_;
      if (!curCharPtr_.hasChar()) {
        return error("Unexpected end of input");
      }
      switch (*curCharPtr_) {
#define CONSUME_VAL(v)     \
  tmpStorage.push_back(v); \
  ++curCharPtr_;

        case u'"':
        case u'/':
        case u'\\':
          CONSUME_VAL(*curCharPtr_)
          break;
        case 'b':
          CONSUME_VAL(8)
          break;
        case 'f':
          CONSUME_VAL(12)
          break;
        case 'n':
          CONSUME_VAL(10)
          break;
        case 'r':
          CONSUME_VAL(13)
          break;
        case 't':
          CONSUME_VAL(9)
          break;
        case 'u': {
          ++curCharPtr_;
          CallResult<char16_t> cr = consumeUnicode();
          if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION)) {
            return ExecutionStatus::EXCEPTION;
          }
          tmpStorage.push_back(*cr);
          break;
        }

        default:
          return errorWithChar(u"Invalid escape sequence: ", *curCharPtr_);
      }
      scannedChar = tmpStorage.back();
    } else {
      scannedChar = *curCharPtr_;
      if (hasEscape)
        tmpStorage.push_back(scannedChar);
      ++curCharPtr_;
    }
    if constexpr (ForKey::value) {
      hash = hermes::updateJenkinsHash(hash, scannedChar);
    } else {
      allAscii &= isASCII(scannedChar);
    }
  }
  return error("Unexpected end of input");
}

ExecutionStatus JSONLexer::scanWord(const char *word, JSONTokenKind kind) {
  while (*word && curCharPtr_.hasChar()) {
    if (*curCharPtr_ != *word) {
      return errorWithChar(u"Unexpected character: ", *curCharPtr_);
    }
    ++curCharPtr_;
    ++word;
  }
  if (*word) {
    return error(u"Unexpected end of input");
  }
  token_.setPunctuator(kind);
  return ExecutionStatus::RETURNED;
}

// ---------------------------------------------------------------------------
// ASCII mode. Same grammar and errors as the UTF-16 path above, but
// scanning a stable 7-bit buffer directly: no widening copy of the input,
// identifier lookups on 8-bit keys, and 8-bit string values created without
// re-checking their encoding.

ExecutionStatus JSONLexer::advanceHelperAscii(bool forKey) {
  while (acur_ != aend_ &&
         (*acur_ == ' ' || *acur_ == '\n' || *acur_ == '\r' || *acur_ == '\t'))
    ++acur_;
  if (acur_ == aend_) {
    token_.setEof();
    return ExecutionStatus::RETURNED;
  }
  token_.setFirstChar((unsigned char)*acur_);
  switch (*acur_) {
#define APUNC(ch, tok)         \
  case ch:                     \
    token_.setPunctuator(tok); \
    ++acur_;                   \
    return ExecutionStatus::RETURNED
    APUNC('{', JSONTokenKind::LBrace);
    APUNC('}', JSONTokenKind::RBrace);
    APUNC('[', JSONTokenKind::LSquare);
    APUNC(']', JSONTokenKind::RSquare);
    APUNC(',', JSONTokenKind::Comma);
    APUNC(':', JSONTokenKind::Colon);
#undef APUNC
    case 't':
      return scanWordAscii(TrueString, JSONTokenKind::True);
    case 'f':
      return scanWordAscii(FalseString, JSONTokenKind::False);
    case 'n':
      return scanWordAscii(NullString, JSONTokenKind::Null);
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return scanNumberAscii();
    case '"':
      return forKey ? scanStringAscii<StrAsSymbol>()
                    : scanStringAscii<StrAsValue>();
    default:
      return errorWithChar(
          u"Unexpected character: ", (char16_t)(unsigned char)*acur_);
  }
}

CallResult<char16_t> JSONLexer::consumeUnicodeAscii() {
  uint16_t val = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (acur_ == aend_)
      return error("Unexpected end of input");
    int ch = *acur_ | 32;
    if (ch >= '0' && ch <= '9') {
      ch -= '0';
    } else if (ch >= 'a' && ch <= 'f') {
      ch -= 'a' - 10;
    } else {
      return errorWithChar(
          u"Invalid unicode point character: ",
          (char16_t)(unsigned char)*acur_);
    }
    val = (val << 4) + ch;
    ++acur_;
  }
  return static_cast<char16_t>(val);
}

ExecutionStatus JSONLexer::scanNumberAscii() {
  const char *start = acur_;
  while (acur_ != aend_) {
    char ch = *acur_;
    if (!(ch == '-' || ch == '+' || ch == '.' || (ch | 32) == 'e' ||
          (ch >= '0' && ch <= '9')))
      break;
    ++acur_;
  }
  size_t len = acur_ - start;
  assert(len > 0 && "scanNumber must be called on a number-looking char");
  if (start[0] == '0' && len > 1 && start[1] >= '0' && start[1] <= '9')
    return errorWithChar(u"Unexpected character in number: ", start[1]);
  // Fast path: plain non-negative/negative integers of up to 15 digits are
  // exact as doubles; skip strtod.
  {
    const char *p = start;
    bool neg = *p == '-';
    if (neg)
      ++p;
    size_t ndig = acur_ - p;
    if (ndig > 0 && ndig <= 15) {
      uint64_t v = 0;
      const char *q = p;
      for (; q != acur_ && *q >= '0' && *q <= '9'; ++q)
        v = v * 10 + (*q - '0');
      if (q == acur_) {
        double d = (double)v;
        token_.setNumber(neg ? -d : d);
        return ExecutionStatus::RETURNED;
      }
    }
  }
  llvh::SmallVector<char, 32> str8(start, acur_);
  str8.push_back('\0');
  char *endPtr;
  double value = ::hermes_g_strtod(str8.data(), &endPtr);
  if (endPtr != str8.data() + len)
    return errorWithChar(u"Unexpected character in number: ", *endPtr);
  token_.setNumber(value);
  return ExecutionStatus::RETURNED;
}

template <typename ForKey>
ExecutionStatus JSONLexer::scanStringAscii() {
  assert(*acur_ == '"');
  ++acur_;
  const char *start = acur_;
  hermes::JenkinsHash hash = hermes::JenkinsHashInit;
  // Common case: no escapes -> the string is a slice of the input.
  while (acur_ != aend_) {
    char c = *acur_;
    if (c == '"') {
      llvh::ArrayRef<char> ref(start, acur_);
      ++acur_;
      if constexpr (ForKey::value) {
        auto symRes =
            runtime_.getIdentifierTable().getSymbolHandle(runtime_, ref, hash);
        if (symRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        token_.setSymbol(symRes->get());
      } else {
        auto strRes = StringPrimitive::create(runtime_, ref);
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        token_.setString(vmcast<StringPrimitive>(*strRes));
      }
      return ExecutionStatus::RETURNED;
    }
    if ((unsigned char)c <= 0x1F)
      return error(u"U+0000 thru U+001F is not allowed in string");
    if (c == '\\')
      break;
    if constexpr (ForKey::value)
      hash = hermes::updateJenkinsHash(hash, (char16_t)c);
    ++acur_;
  }
  if (acur_ == aend_)
    return error("Unexpected end of input");

  // Escapes present: build UTF-16 (\u escapes may produce non-ASCII).
  SmallU16String<32> tmp;
  for (const char *p = start; p != acur_; ++p)
    tmp.push_back((unsigned char)*p);
  bool allAscii = true;
  while (acur_ != aend_) {
    char c = *acur_;
    if (c == '"') {
      ++acur_;
      if constexpr (ForKey::value) {
        auto symRes = runtime_.getIdentifierTable().getSymbolHandle(
            runtime_, tmp.arrayRef(), hash);
        if (symRes == ExecutionStatus::EXCEPTION)
          return ExecutionStatus::EXCEPTION;
        token_.setSymbol(symRes->get());
      } else {
        auto strRes = StringPrimitive::createWithKnownEncoding(
            runtime_, tmp.arrayRef(), allAscii);
        if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION))
          return ExecutionStatus::EXCEPTION;
        token_.setString(vmcast<StringPrimitive>(*strRes));
      }
      return ExecutionStatus::RETURNED;
    }
    if ((unsigned char)c <= 0x1F)
      return error(u"U+0000 thru U+001F is not allowed in string");
    char16_t scanned;
    if (c == '\\') {
      ++acur_;
      if (acur_ == aend_)
        return error("Unexpected end of input");
      switch (*acur_) {
        case '"':
        case '/':
        case '\\':
          scanned = *acur_;
          ++acur_;
          break;
        case 'b':
          scanned = 8;
          ++acur_;
          break;
        case 'f':
          scanned = 12;
          ++acur_;
          break;
        case 'n':
          scanned = 10;
          ++acur_;
          break;
        case 'r':
          scanned = 13;
          ++acur_;
          break;
        case 't':
          scanned = 9;
          ++acur_;
          break;
        case 'u': {
          ++acur_;
          CallResult<char16_t> cr = consumeUnicodeAscii();
          if (LLVM_UNLIKELY(cr == ExecutionStatus::EXCEPTION))
            return ExecutionStatus::EXCEPTION;
          scanned = *cr;
          break;
        }
        default:
          return errorWithChar(
              u"Invalid escape sequence: ", (char16_t)(unsigned char)*acur_);
      }
    } else {
      scanned = (unsigned char)c;
      ++acur_;
    }
    tmp.push_back(scanned);
    if constexpr (ForKey::value)
      hash = hermes::updateJenkinsHash(hash, scanned);
    else
      allAscii &= isASCII(scanned);
  }
  return error("Unexpected end of input");
}

ExecutionStatus JSONLexer::scanWordAscii(const char *word, JSONTokenKind kind) {
  while (*word && acur_ != aend_) {
    if (*acur_ != *word)
      return errorWithChar(
          u"Unexpected character: ", (char16_t)(unsigned char)*acur_);
    ++acur_;
    ++word;
  }
  if (*word)
    return error(u"Unexpected end of input");
  token_.setPunctuator(kind);
  return ExecutionStatus::RETURNED;
}

} // namespace vm
} // namespace hermes
