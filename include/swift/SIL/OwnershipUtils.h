//===--- OwnershipUtils.h ------------------------------------*- C++ -*----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_OWNERSHIPUTILS_H
#define SWIFT_SIL_OWNERSHIPUTILS_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILBasicBlock;
class SILInstruction;
class SILModule;
class SILValue;
class DeadEndBlocks;
class BranchPropagatedUser;

namespace ownership {

struct ErrorBehaviorKind {
  enum inner_t {
    Invalid = 0,
    ReturnFalse = 1,
    PrintMessage = 2,
    Assert = 4,
    PrintMessageAndReturnFalse = PrintMessage | ReturnFalse,
    PrintMessageAndAssert = PrintMessage | Assert,
  } Value;

  ErrorBehaviorKind() : Value(Invalid) {}
  ErrorBehaviorKind(inner_t Inner) : Value(Inner) { assert(Value != Invalid); }

  bool shouldAssert() const {
    assert(Value != Invalid);
    return Value & Assert;
  }

  bool shouldPrintMessage() const {
    assert(Value != Invalid);
    return Value & PrintMessage;
  }

  bool shouldReturnFalse() const {
    assert(Value != Invalid);
    return Value & ReturnFalse;
  }
};

} // end namespace ownership

/// This class is a higher level interface to the ownership checker meant for
/// use with SILPasses. It uses the actual checker as an internal PImpl detail
/// so types/etc do not leak.
struct OwnershipChecker {
  /// The module that we are in.
  SILModule &Mod;

  /// A cache of dead-end basic blocks that we use to determine if we can
  /// ignore "leaks".
  DeadEndBlocks &DEBlocks;

  /// The list of regular users from the last run of the checker.
  SmallVector<SILInstruction *, 16> RegularUsers;

  /// The list of regular users from the last run of the checker.
  SmallVector<SILInstruction *, 16> LifetimeEndingUsers;

  /// The live blocks for the SILValue we processed. This can be used to
  /// determine if a block is in the "live" region of our SILInstruction.
  SmallPtrSet<SILBasicBlock *, 32> LiveBlocks;

  bool checkValue(SILValue Value);
};

/// Returns true if:
///
/// 1. No consuming uses are reachable from any other consuming use, from any
/// non-consuming uses, or from the producer instruction.
/// 2. The consuming use set jointly post dominates producers and all non
/// consuming uses.
bool valueHasLinearLifetime(SILValue value,
                            ArrayRef<BranchPropagatedUser> consumingUses,
                            ArrayRef<BranchPropagatedUser> nonConsumingUses,
                            SmallPtrSetImpl<SILBasicBlock *> &visitedBlocks,
                            DeadEndBlocks &deBlocks,
                            ownership::ErrorBehaviorKind errorBehavior);

} // namespace swift

#endif
