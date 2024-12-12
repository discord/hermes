/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

//===----------------------------------------------------------------------===//
/// \file
///
/// OverflowGuard is designed to catch imminent native stack overflow. It does
/// this using two different heuristics, depending on HERMES_CHECK_NATIVE_STACK.
///
/// If HERMES_CHECK_NATIVE_STACK is defined, it will use real stack checking. It
/// calls into platform-specific APIs to obtain the upper stack bound of the
/// currently executing thread. It will then check the current stack address
/// against the upper limit, along with some user-defined gap. Overflow is
/// reported if the current stack address is close enough to the upper bound,
/// accounting for the supplied gap value.
///
/// If HERMES_CHECK_NATIVE_STACK is not defined, a simple depth counter is used.
/// Every time a recursive call is made, the counter should be bumped. Overflow
/// is reported if the counter reaches some user-defined max.
///
//===----------------------------------------------------------------------===//

#ifndef HERMES_SUPPORT_STACKOVERFLOWGUARD_H
#define HERMES_SUPPORT_STACKOVERFLOWGUARD_H

#include <cstddef>
#include "hermes/Support/OSCompat.h"
#include "llvh/Support/Compiler.h"
#include "llvh/Support/raw_ostream.h"

namespace hermes {

class StackOverflowGuard {
  explicit StackOverflowGuard(size_t max) : maxCallDepth(max) {}

 public:
  /// This is how many recursive calls have already been made.
  /// This grows towards maxCallDepth.
  size_t callDepth{0};
  /// If callDepth exceeds this value, it is considered overflow.
  size_t maxCallDepth;

  static StackOverflowGuard depthCounterGuard(unsigned stackGap) {
    return StackOverflowGuard(stackGap);
  }

  /// \return true if \c callDepth has exceeded the budget set by \c
  /// maxCallDepth.
  inline bool isOverflowing() {
    return callDepth > maxCallDepth;
  }
};

} // namespace hermes

#endif // HERMES_SUPPORT_STACKOVERFLOWGUARD_H
