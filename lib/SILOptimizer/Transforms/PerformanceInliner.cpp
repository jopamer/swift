//===--- PerformanceInliner.cpp - Basic cost based performance inlining ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-inliner"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/Projection.h"
#include "swift/SILOptimizer/Analysis/ColdBlockInfo.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/Local.h"
#include "swift/SILOptimizer/Utils/ConstantFolding.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/MapVector.h"
#include <functional>


using namespace swift;

STATISTIC(NumFunctionsInlined, "Number of functions inlined");

namespace {

  // Threshold for deterministic testing of the inline heuristic.
  // It specifies an instruction cost limit where a simplified model is used
  // for the instruction costs: only builtin instructions have a cost of exactly
  // 1.
  llvm::cl::opt<int> TestThreshold("sil-inline-test-threshold",
                                        llvm::cl::init(-1), llvm::cl::Hidden);

  // The following constants define the cost model for inlining.

  // The base value for every call: it represents the benefit of removing the
  // call overhead.
  // This value can be overridden with the -sil-inline-threshold option.
  const unsigned RemovedCallBenefit = 80;

  // The benefit if the condition of a terminator instruction gets constant due
  // to inlining.
  const unsigned ConstTerminatorBenefit = 2;

  // Benefit if the operand of an apply gets constant, e.g. if a closure is
  // passed to an apply instruction in the callee.
  const unsigned ConstCalleeBenefit = 150;

  // Additional benefit for each loop level.
  const unsigned LoopBenefitFactor = 40;

  // Approximately up to this cost level a function can be inlined without
  // increasing the code size.
  const unsigned TrivialFunctionThreshold = 20;

  // Configuration for the caller block limit.
  const unsigned BlockLimitDenominator = 10000;

  // Represents a value in integer constant evaluation.
  struct IntConst {
    IntConst() : isValid(false), isFromCaller(false) { }

    IntConst(const APInt &value, bool isFromCaller) :
    value(value), isValid(true), isFromCaller(isFromCaller) { }
    
    // The actual value.
    APInt value;
    
    // True if the value is valid, i.e. could be evaluated to a constant.
    bool isValid;
    
    // True if the value is only valid, because a constant is passed to the
    // callee. False if constant propagation could do the same job inside the
    // callee without inlining it.
    bool isFromCaller;
  };
  
  // Tracks constants in the caller and callee to get an estimation of what
  // values get constant if the callee is inlined.
  // This can be seen as a "simulation" of several optimizations: SROA, mem2reg
  // and constant propagation.
  // Note that this is only a simplified model and not correct in all cases.
  // For example aliasing information is not taken into account.
  class ConstantTracker {
    // Links between loaded and stored values.
    // The key is a load instruction, the value is the corresponding store
    // instruction which stores the loaded value. Both, key and value can also
    // be copy_addr instructions.
    llvm::DenseMap<SILInstruction *, SILInstruction *> links;
    
    // The current stored values at memory addresses.
    // The key is the base address of the memory (after skipping address
    // projections). The value are store (or copy_addr) instructions, which
    // store the current value.
    // This is only an estimation, because e.g. it does not consider potential
    // aliasing.
    llvm::DenseMap<SILValue, SILInstruction *> memoryContent;
    
    // Cache for evaluated constants.
    llvm::SmallDenseMap<BuiltinInst *, IntConst> constCache;

    // The caller/callee function which is tracked.
    SILFunction *F;
    
    // The constant tracker of the caller function (null if this is the
    // tracker of the callee).
    ConstantTracker *callerTracker;
    
    // The apply instruction in the caller (null if this is the tracker of the
    // callee).
    FullApplySite AI;
    
    // Walks through address projections and (optionally) collects them.
    // Returns the base address, i.e. the first address which is not a
    // projection.
    SILValue scanProjections(SILValue addr,
                             SmallVectorImpl<Projection> *Result = nullptr);
    
    // Get the stored value for a load. The loadInst can be either a real load
    // or a copy_addr.
    SILValue getStoredValue(SILInstruction *loadInst,
                            ProjectionPath &projStack);

    // Gets the parameter in the caller for a function argument.
    SILValue getParam(SILValue value) {
      if (SILArgument *arg = dyn_cast<SILArgument>(value)) {
        if (AI && arg->isFunctionArg() && arg->getFunction() == F) {
          // Continue at the caller.
          return AI.getArgument(arg->getIndex());
        }
      }
      return SILValue();
    }
    
    SILInstruction *getMemoryContent(SILValue addr) {
      // The memory content can be stored in this ConstantTracker or in the
      // caller's ConstantTracker.
      SILInstruction *storeInst = memoryContent[addr];
      if (storeInst)
        return storeInst;
      if (callerTracker)
        return callerTracker->getMemoryContent(addr);
      return nullptr;
    }
    
    // Gets the estimated definition of a value.
    SILInstruction *getDef(SILValue val, ProjectionPath &projStack);

    // Gets the estimated integer constant result of a builtin.
    IntConst getBuiltinConst(BuiltinInst *BI, int depth);
    
  public:
    
    // Constructor for the caller function.
    ConstantTracker(SILFunction *function) :
      F(function), callerTracker(nullptr), AI()
    { }
    
    // Constructor for the callee function.
    ConstantTracker(SILFunction *function, ConstantTracker *caller,
                    FullApplySite callerApply) :
       F(function), callerTracker(caller), AI(callerApply)
    { }
    
    void beginBlock() {
      // Currently we don't do any sophisticated dataflow analysis, so we keep
      // the memoryContent alive only for a single block.
      memoryContent.clear();
    }

    // Must be called for each instruction visited in dominance order.
    void trackInst(SILInstruction *inst);
    
    // Gets the estimated definition of a value.
    SILInstruction *getDef(SILValue val) {
      ProjectionPath projStack(val->getType());
      return getDef(val, projStack);
    }
    
    // Gets the estimated definition of a value if it is in the caller.
    SILInstruction *getDefInCaller(SILValue val) {
      SILInstruction *def = getDef(val);
      if (def && def->getFunction() != F)
        return def;
      return nullptr;
    }
    
    // Gets the estimated integer constant of a value.
    IntConst getIntConst(SILValue val, int depth = 0);
  };

  // Controls the decision to inline functions with @_semantics, @effect and
  // global_init attributes.
  enum class InlineSelection {
    Everything,
    NoGlobalInit, // and no availability semantics calls
    NoSemanticsAndGlobalInit
  };

  class SILPerformanceInliner {
    /// The inline threshold.
    const int InlineCostThreshold;
    /// Specifies which functions not to inline, based on @_semantics and
    /// global_init attributes.
    InlineSelection WhatToInline;

#ifndef NDEBUG
    SILFunction *LastPrintedCaller = nullptr;
    void dumpCaller(SILFunction *Caller) {
      if (Caller != LastPrintedCaller) {
        llvm::dbgs() << "\nInline into caller: " << Caller->getName() << '\n';
        LastPrintedCaller = Caller;
      }
    }
#endif

    SILFunction *getEligibleFunction(FullApplySite AI);

    bool isProfitableToInline(FullApplySite AI, unsigned loopDepthOfAI,
                              DominanceAnalysis *DA,
                              SILLoopAnalysis *LA,
                              ConstantTracker &constTracker,
                              unsigned &NumCallerBlocks);

    bool isProfitableInColdBlock(FullApplySite AI, SILFunction *Callee);

    void visitColdBlocks(SmallVectorImpl<FullApplySite> &AppliesToInline,
                         SILBasicBlock *root, DominanceInfo *DT);

    void collectAppliesToInline(SILFunction *Caller,
                                SmallVectorImpl<FullApplySite> &Applies,
                                DominanceAnalysis *DA, SILLoopAnalysis *LA);

  public:
    SILPerformanceInliner(int threshold, InlineSelection WhatToInline)
        : InlineCostThreshold(threshold), WhatToInline(WhatToInline) {}

    bool inlineCallsIntoFunction(SILFunction *F, DominanceAnalysis *DA,
                                 SILLoopAnalysis *LA);
  };
}

//===----------------------------------------------------------------------===//
//                               ConstantTracker
//===----------------------------------------------------------------------===//


void ConstantTracker::trackInst(SILInstruction *inst) {
  if (auto *LI = dyn_cast<LoadInst>(inst)) {
    SILValue baseAddr = scanProjections(LI->getOperand());
    if (SILInstruction *loadLink = getMemoryContent(baseAddr))
       links[LI] = loadLink;
  } else if (StoreInst *SI = dyn_cast<StoreInst>(inst)) {
    SILValue baseAddr = scanProjections(SI->getOperand(1));
    memoryContent[baseAddr] = SI;
  } else if (CopyAddrInst *CAI = dyn_cast<CopyAddrInst>(inst)) {
    if (!CAI->isTakeOfSrc()) {
      // Treat a copy_addr as a load + store
      SILValue loadAddr = scanProjections(CAI->getOperand(0));
      if (SILInstruction *loadLink = getMemoryContent(loadAddr)) {
        links[CAI] = loadLink;
        SILValue storeAddr = scanProjections(CAI->getOperand(1));
        memoryContent[storeAddr] = CAI;
      }
    }
  }
}

SILValue ConstantTracker::scanProjections(SILValue addr,
                                      SmallVectorImpl<Projection> *Result) {
  for (;;) {
    if (Projection::isAddressProjection(addr)) {
      SILInstruction *I = cast<SILInstruction>(addr);
      if (Result) {
        Result->push_back(Projection(I));
      }
      addr = I->getOperand(0);
      continue;
    }
    if (SILValue param = getParam(addr)) {
      // Go to the caller.
      addr = param;
      continue;
    }
    // Return the base address = the first address which is not a projection.
    return addr;
  }
}

SILValue ConstantTracker::getStoredValue(SILInstruction *loadInst,
                                         ProjectionPath &projStack) {
  SILInstruction *store = links[loadInst];
  if (!store && callerTracker)
    store = callerTracker->links[loadInst];
  if (!store) return SILValue();

  assert(isa<LoadInst>(loadInst) || isa<CopyAddrInst>(loadInst));

  // Push the address projections of the load onto the stack.
  SmallVector<Projection, 4> loadProjections;
  scanProjections(loadInst->getOperand(0), &loadProjections);
  for (const Projection &proj : loadProjections) {
    projStack.push_back(proj);
  }
  
  //  Pop the address projections of the store from the stack.
  SmallVector<Projection, 4> storeProjections;
  scanProjections(store->getOperand(1), &storeProjections);
  for (auto iter = storeProjections.rbegin(); iter != storeProjections.rend();
       ++iter) {
    const Projection &proj = *iter;
    // The corresponding load-projection must match the store-projection.
    if (projStack.empty() || projStack.back() != proj)
      return SILValue();
    projStack.pop_back();
  }
  
  if (isa<StoreInst>(store))
    return store->getOperand(0);

  // The copy_addr instruction is both a load and a store. So we follow the link
  // again.
  assert(isa<CopyAddrInst>(store));
  return getStoredValue(store, projStack);
}

// Get the aggregate member based on the top of the projection stack.
static SILValue getMember(SILInstruction *inst, ProjectionPath &projStack) {
  if (!projStack.empty()) {
    const Projection &proj = projStack.back();
    return proj.getOperandForAggregate(inst);
  }
  return SILValue();
}

SILInstruction *ConstantTracker::getDef(SILValue val,
                                        ProjectionPath &projStack) {
  
  // Track the value up the dominator tree.
  for (;;) {
    if (SILInstruction *inst = dyn_cast<SILInstruction>(val)) {
      if (Projection::isObjectProjection(inst)) {
        // Extract a member from a struct/tuple/enum.
        projStack.push_back(Projection(inst));
        val = inst->getOperand(0);
        continue;
      } else if (SILValue member = getMember(inst, projStack)) {
        // The opposite of a projection instruction: composing a struct/tuple.
        projStack.pop_back();
        val = member;
        continue;
      } else if (SILValue loadedVal = getStoredValue(inst, projStack)) {
        // A value loaded from memory.
        val = loadedVal;
        continue;
      } else if (isa<ThinToThickFunctionInst>(inst)) {
        val = inst->getOperand(0);
        continue;
      }
      return inst;
    } else if (SILValue param = getParam(val)) {
      // Continue in the caller.
      val = param;
      continue;
    }
    return nullptr;
  }
}

IntConst ConstantTracker::getBuiltinConst(BuiltinInst *BI, int depth) {
  const BuiltinInfo &Builtin = BI->getBuiltinInfo();
  OperandValueArrayRef Args = BI->getArguments();
  switch (Builtin.ID) {
    default: break;
      
      // Fold comparison predicates.
#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_PREDICATE(id, name, attrs, overload) \
case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
    {
      IntConst lhs = getIntConst(Args[0], depth);
      IntConst rhs = getIntConst(Args[1], depth);
      if (lhs.isValid && rhs.isValid) {
        return IntConst(constantFoldComparison(lhs.value, rhs.value,
                                              Builtin.ID),
                        lhs.isFromCaller || rhs.isFromCaller);
      }
      break;
    }
      
      
    case BuiltinValueKind::SAddOver:
    case BuiltinValueKind::UAddOver:
    case BuiltinValueKind::SSubOver:
    case BuiltinValueKind::USubOver:
    case BuiltinValueKind::SMulOver:
    case BuiltinValueKind::UMulOver: {
      IntConst lhs = getIntConst(Args[0], depth);
      IntConst rhs = getIntConst(Args[1], depth);
      if (lhs.isValid && rhs.isValid) {
        bool IgnoredOverflow;
        return IntConst(constantFoldBinaryWithOverflow(lhs.value, rhs.value,
                        IgnoredOverflow,
                        getLLVMIntrinsicIDForBuiltinWithOverflow(Builtin.ID)),
                          lhs.isFromCaller || rhs.isFromCaller);
      }
      break;
    }
      
    case BuiltinValueKind::SDiv:
    case BuiltinValueKind::SRem:
    case BuiltinValueKind::UDiv:
    case BuiltinValueKind::URem: {
      IntConst lhs = getIntConst(Args[0], depth);
      IntConst rhs = getIntConst(Args[1], depth);
      if (lhs.isValid && rhs.isValid && rhs.value != 0) {
        bool IgnoredOverflow;
        return IntConst(constantFoldDiv(lhs.value, rhs.value,
                                        IgnoredOverflow, Builtin.ID),
                        lhs.isFromCaller || rhs.isFromCaller);
      }
      break;
    }
      
    case BuiltinValueKind::And:
    case BuiltinValueKind::AShr:
    case BuiltinValueKind::LShr:
    case BuiltinValueKind::Or:
    case BuiltinValueKind::Shl:
    case BuiltinValueKind::Xor: {
      IntConst lhs = getIntConst(Args[0], depth);
      IntConst rhs = getIntConst(Args[1], depth);
      if (lhs.isValid && rhs.isValid) {
        return IntConst(constantFoldBitOperation(lhs.value, rhs.value,
                                                 Builtin.ID),
                        lhs.isFromCaller || rhs.isFromCaller);
      }
      break;
    }
      
    case BuiltinValueKind::Trunc:
    case BuiltinValueKind::ZExt:
    case BuiltinValueKind::SExt:
    case BuiltinValueKind::TruncOrBitCast:
    case BuiltinValueKind::ZExtOrBitCast:
    case BuiltinValueKind::SExtOrBitCast: {
      IntConst val = getIntConst(Args[0], depth);
      if (val.isValid) {
        return IntConst(constantFoldCast(val.value, Builtin), val.isFromCaller);
      }
      break;
    }
  }
  return IntConst();
}

// Tries to evaluate the integer constant of a value. The \p depth is used
// to limit the complexity.
IntConst ConstantTracker::getIntConst(SILValue val, int depth) {
  
  // Don't spend too much time with constant evaluation.
  if (depth >= 10)
    return IntConst();
  
  SILInstruction *I = getDef(val);
  if (!I)
    return IntConst();
  
  if (auto *IL = dyn_cast<IntegerLiteralInst>(I)) {
    return IntConst(IL->getValue(), IL->getFunction() != F);
  }
  if (auto *BI = dyn_cast<BuiltinInst>(I)) {
    if (constCache.count(BI) != 0)
      return constCache[BI];
    
    IntConst builtinConst = getBuiltinConst(BI, depth + 1);
    constCache[BI] = builtinConst;
    return builtinConst;
  }
  return IntConst();
}

//===----------------------------------------------------------------------===//
//                           Performance Inliner
//===----------------------------------------------------------------------===//

// Return true if the callee has self-recursive calls.
static bool calleeIsSelfRecursive(SILFunction *Callee) {
  for (auto &BB : *Callee)
    for (auto &I : BB)
      if (auto Apply = FullApplySite::isa(&I))
        if (Apply.getReferencedFunction() == Callee)
          return true;
  return false;
}

// Returns the callee of an apply_inst if it is basically inlineable.
SILFunction *SILPerformanceInliner::getEligibleFunction(FullApplySite AI) {

  SILFunction *Callee = AI.getReferencedFunction();

  if (!Callee) {
    return nullptr;
  }

  // Don't inline functions that are marked with the @_semantics or @effects
  // attribute if the inliner is asked not to inline them.
  if (Callee->hasSemanticsAttrs() || Callee->hasEffectsKind()) {
    if (WhatToInline == InlineSelection::NoSemanticsAndGlobalInit) {
      return nullptr;
    }
    // The "availability" semantics attribute is treated like global-init.
    if (Callee->hasSemanticsAttrs() &&
        WhatToInline != InlineSelection::Everything &&
        Callee->hasSemanticsAttrThatStartsWith("availability")) {
      return nullptr;
    }
  } else if (Callee->isGlobalInit()) {
    if (WhatToInline != InlineSelection::Everything) {
      return nullptr;
    }
  }

  // We can't inline external declarations.
  if (Callee->empty() || Callee->isExternalDeclaration()) {
    return nullptr;
  }

  // Explicitly disabled inlining.
  if (Callee->getInlineStrategy() == NoInline) {
    return nullptr;
  }
  
  if (!Callee->shouldOptimize()) {
    return nullptr;
  }

  // We don't support this yet.
  if (AI.hasSubstitutions()) {
    return nullptr;
  }

  // We don't support inlining a function that binds dynamic self because we
  // have no mechanism to preserve the original function's local self metadata.
  if (computeMayBindDynamicSelf(Callee)) {
    return nullptr;
  }

  SILFunction *Caller = AI.getFunction();

  // Detect self-recursive calls.
  if (Caller == Callee) {
    return nullptr;
  }

  // A non-fragile function may not be inlined into a fragile function.
  if (Caller->isFragile() && !Callee->isFragile()) {
    return nullptr;
  }

  // Inlining self-recursive functions into other functions can result
  // in excessive code duplication since we run the inliner multiple
  // times in our pipeline
  if (calleeIsSelfRecursive(Callee)) {
    return nullptr;
  }

  return Callee;
}

// Gets the cost of an instruction by using the simplified test-model: only
// builtin instructions have a cost and that's exactly 1.
static unsigned testCost(SILInstruction *I) {
  switch (I->getKind()) {
    case ValueKind::BuiltinInst:
      return 1;
    default:
      return 0;
  }
}

// Returns the taken block of a terminator instruction if the condition turns
// out to be constant.
static SILBasicBlock *getTakenBlock(TermInst *term,
                                    ConstantTracker &constTracker) {
  if (CondBranchInst *CBI = dyn_cast<CondBranchInst>(term)) {
    IntConst condConst = constTracker.getIntConst(CBI->getCondition());
    if (condConst.isFromCaller) {
      return condConst.value != 0 ? CBI->getTrueBB() : CBI->getFalseBB();
    }
    return nullptr;
  }
  if (SwitchValueInst *SVI = dyn_cast<SwitchValueInst>(term)) {
    IntConst switchConst = constTracker.getIntConst(SVI->getOperand());
    if (switchConst.isFromCaller) {
      for (unsigned Idx = 0; Idx < SVI->getNumCases(); ++Idx) {
        auto switchCase = SVI->getCase(Idx);
        if (auto *IL = dyn_cast<IntegerLiteralInst>(switchCase.first)) {
          if (switchConst.value == IL->getValue())
            return switchCase.second;
        } else {
          return nullptr;
        }
      }
      if (SVI->hasDefault())
          return SVI->getDefaultBB();
    }
    return nullptr;
  }
  if (SwitchEnumInst *SEI = dyn_cast<SwitchEnumInst>(term)) {
    if (SILInstruction *def = constTracker.getDefInCaller(SEI->getOperand())) {
      if (EnumInst *EI = dyn_cast<EnumInst>(def)) {
        for (unsigned Idx = 0; Idx < SEI->getNumCases(); ++Idx) {
          auto enumCase = SEI->getCase(Idx);
          if (enumCase.first == EI->getElement())
            return enumCase.second;
        }
        if (SEI->hasDefault())
          return SEI->getDefaultBB();
      }
    }
    return nullptr;
  }
  if (CheckedCastBranchInst *CCB = dyn_cast<CheckedCastBranchInst>(term)) {
    if (SILInstruction *def = constTracker.getDefInCaller(CCB->getOperand())) {
      if (UpcastInst *UCI = dyn_cast<UpcastInst>(def)) {
        SILType castType = UCI->getOperand()->getType();
        if (CCB->getCastType().isExactSuperclassOf(castType)) {
          return CCB->getSuccessBB();
        }
        if (!castType.isBindableToSuperclassOf(CCB->getCastType())) {
          return CCB->getFailureBB();
        }
      }
    }
  }
  return nullptr;
}

/// Return true if inlining this call site is profitable.
bool SILPerformanceInliner::isProfitableToInline(FullApplySite AI,
                                              unsigned loopDepthOfAI,
                                              DominanceAnalysis *DA,
                                              SILLoopAnalysis *LA,
                                              ConstantTracker &callerTracker,
                                              unsigned &NumCallerBlocks) {
  SILFunction *Callee = AI.getReferencedFunction();

  if (Callee->getInlineStrategy() == AlwaysInline)
    return true;
  
  ConstantTracker constTracker(Callee, &callerTracker, AI);
  
  DominanceInfo *DT = DA->get(Callee);
  SILLoopInfo *LI = LA->get(Callee);

  DominanceOrder domOrder(&Callee->front(), DT, Callee->size());
  
  // Calculate the inlining cost of the callee.
  unsigned CalleeCost = 0;
  unsigned Benefit = InlineCostThreshold > 0 ? InlineCostThreshold :
                                               RemovedCallBenefit;
  Benefit += loopDepthOfAI * LoopBenefitFactor;
  int testThreshold = TestThreshold;

  while (SILBasicBlock *block = domOrder.getNext()) {
    constTracker.beginBlock();
    for (SILInstruction &I : *block) {
      constTracker.trackInst(&I);
      
      if (testThreshold >= 0) {
        // We are in test-mode: use a simplified cost model.
        CalleeCost += testCost(&I);
      } else {
        // Use the regular cost model.
        CalleeCost += unsigned(instructionInlineCost(I));
      }
      
      if (ApplyInst *AI = dyn_cast<ApplyInst>(&I)) {
        
        // Check if the callee is passed as an argument. If so, increase the
        // threshold, because inlining will (probably) eliminate the closure.
        SILInstruction *def = constTracker.getDefInCaller(AI->getCallee());
        if (def && (isa<FunctionRefInst>(def) || isa<PartialApplyInst>(def))) {
          unsigned loopDepth = LI->getLoopDepth(block);
          Benefit += ConstCalleeBenefit + loopDepth * LoopBenefitFactor;
          testThreshold *= 2;
        }
      }
    }
    // Don't count costs in blocks which are dead after inlining.
    SILBasicBlock *takenBlock = getTakenBlock(block->getTerminator(),
                                              constTracker);
    if (takenBlock) {
      Benefit += ConstTerminatorBenefit;
      domOrder.pushChildrenIf(block, [=] (SILBasicBlock *child) {
        return child->getSinglePredecessor() != block || child == takenBlock;
      });
    } else {
      domOrder.pushChildren(block);
    }
  }

  unsigned Threshold = Benefit; // The default.
  if (testThreshold >= 0) {
    // We are in testing mode.
    Threshold = testThreshold;
  } else if (AI.getFunction()->isThunk()) {
    // Only inline trivial functions into thunks (which will not increase the
    // code size).
    Threshold = TrivialFunctionThreshold;
  } else {
    // The default case.
    // We reduce the benefit if the caller is too large. For this we use a
    // cubic function on the number of caller blocks. This starts to prevent
    // inlining at about 800 - 1000 caller blocks.
    unsigned blockMinus =
      (NumCallerBlocks * NumCallerBlocks) / BlockLimitDenominator *
                          NumCallerBlocks / BlockLimitDenominator;
    if (Threshold > blockMinus + TrivialFunctionThreshold)
      Threshold -= blockMinus;
    else
      Threshold = TrivialFunctionThreshold;
  }

  if (CalleeCost > Threshold) {
    return false;
  }
  NumCallerBlocks += Callee->size();

  DEBUG(
    dumpCaller(AI.getFunction());
    llvm::dbgs() << "    decision {" << CalleeCost << " < " << Threshold <<
        ", ld=" << loopDepthOfAI << ", bb=" << NumCallerBlocks << "} " <<
        Callee->getName() << '\n';
  );
  return true;
}

/// Return true if inlining this call site into a cold block is profitable.
bool SILPerformanceInliner::isProfitableInColdBlock(FullApplySite AI,
                                                    SILFunction *Callee) {
  if (Callee->getInlineStrategy() == AlwaysInline)
    return true;

  // Testing with the TestThreshold disables inlining into cold blocks.
  if (TestThreshold >= 0)
    return false;
  
  unsigned CalleeCost = 0;
  int testThreshold = TestThreshold;

  for (SILBasicBlock &Block : *Callee) {
    for (SILInstruction &I : Block) {
      if (testThreshold >= 0) {
        // We are in test-mode: use a simplified cost model.
        CalleeCost += testCost(&I);
        if (CalleeCost > 0)
          return false;
      } else {
        // Use the regular cost model.
        CalleeCost += unsigned(instructionInlineCost(I));
        if (CalleeCost > TrivialFunctionThreshold)
          return false;
      }
    }
  }
  DEBUG(
    dumpCaller(AI.getFunction());
    llvm::dbgs() << "    cold decision {" << CalleeCost << "} " <<
              Callee->getName() << '\n';
  );
  return true;
}

void SILPerformanceInliner::collectAppliesToInline(
    SILFunction *Caller, SmallVectorImpl<FullApplySite> &Applies,
    DominanceAnalysis *DA, SILLoopAnalysis *LA) {
  DominanceInfo *DT = DA->get(Caller);
  SILLoopInfo *LI = LA->get(Caller);

  ColdBlockInfo ColdBlocks(DA);
  ConstantTracker constTracker(Caller);
  DominanceOrder domOrder(&Caller->front(), DT, Caller->size());

  unsigned NumCallerBlocks = Caller->size();

  // Go through all instructions and find candidates for inlining.
  // We do this in dominance order for the constTracker.
  SmallVector<FullApplySite, 8> InitialCandidates;
  while (SILBasicBlock *block = domOrder.getNext()) {
    constTracker.beginBlock();
    unsigned loopDepth = LI->getLoopDepth(block);
    for (auto I = block->begin(), E = block->end(); I != E; ++I) {
      constTracker.trackInst(&*I);

      if (!FullApplySite::isa(&*I))
        continue;

      FullApplySite AI = FullApplySite(&*I);

      auto *Callee = getEligibleFunction(AI);
      if (Callee) {
        if (isProfitableToInline(AI, loopDepth, DA, LA, constTracker,
                                 NumCallerBlocks))
          InitialCandidates.push_back(AI);
      }
    }
    domOrder.pushChildrenIf(block, [&] (SILBasicBlock *child) {
      if (ColdBlocks.isSlowPath(block, child)) {
        // Handle cold blocks separately.
        visitColdBlocks(InitialCandidates, child, DT);
        return false;
      }
      return true;
    });
  }

  // Calculate how many times a callee is called from this caller.
  llvm::DenseMap<SILFunction *, unsigned> CalleeCount;
  for (auto AI : InitialCandidates) {
    SILFunction *Callee = AI.getReferencedFunction();
    assert(Callee && "apply_inst does not have a direct callee anymore");
    CalleeCount[Callee]++;
  }

  // Now copy each candidate callee that has a small enough number of
  // call sites into the final set of call sites.
  for (auto AI : InitialCandidates) {
    SILFunction *Callee = AI.getReferencedFunction();
    assert(Callee && "apply_inst does not have a direct callee anymore");

    const unsigned CallsToCalleeThreshold = 1024;
    if (CalleeCount[Callee] <= CallsToCalleeThreshold)
      Applies.push_back(AI);
  }
}

/// \brief Attempt to inline all calls smaller than our threshold.
/// returns True if a function was inlined.
bool SILPerformanceInliner::inlineCallsIntoFunction(SILFunction *Caller,
                                                    DominanceAnalysis *DA,
                                                    SILLoopAnalysis *LA) {
  // Don't optimize functions that are marked with the opt.never attribute.
  if (!Caller->shouldOptimize())
    return false;

  // First step: collect all the functions we want to inline.  We
  // don't change anything yet so that the dominator information
  // remains valid.
  SmallVector<FullApplySite, 8> AppliesToInline;
  collectAppliesToInline(Caller, AppliesToInline, DA, LA);

  if (AppliesToInline.empty())
    return false;

  // Second step: do the actual inlining.
  for (auto AI : AppliesToInline) {
    SILFunction *Callee = AI.getReferencedFunction();
    assert(Callee && "apply_inst does not have a direct callee anymore");

    if (!Callee->shouldOptimize()) {
      continue;
    }
    
    SmallVector<SILValue, 8> Args;
    for (const auto &Arg : AI.getArguments())
      Args.push_back(Arg);

    DEBUG(
      dumpCaller(Caller);
      llvm::dbgs() << "    inline [" << Callee->size() << "->" <<
          Caller->size() << "] " << Callee->getName() << "\n";
    );

    // Notice that we will skip all of the newly inlined ApplyInsts. That's
    // okay because we will visit them in our next invocation of the inliner.
    TypeSubstitutionMap ContextSubs;
    SILInliner Inliner(*Caller, *Callee,
                       SILInliner::InlineKind::PerformanceInline, ContextSubs,
                       AI.getSubstitutions());

    auto Success = Inliner.inlineFunction(AI, Args);
    (void) Success;
    // We've already determined we should be able to inline this, so
    // we expect it to have happened.
    assert(Success && "Expected inliner to inline this function!");

    recursivelyDeleteTriviallyDeadInstructions(AI.getInstruction(), true);

    NumFunctionsInlined++;
  }

  return true;
}

// Find functions in cold blocks which are forced to be inlined.
// All other functions are not inlined in cold blocks.
void SILPerformanceInliner::visitColdBlocks(
    SmallVectorImpl<FullApplySite> &AppliesToInline, SILBasicBlock *Root,
    DominanceInfo *DT) {
  DominanceOrder domOrder(Root, DT);
  while (SILBasicBlock *block = domOrder.getNext()) {
    for (SILInstruction &I : *block) {
      ApplyInst *AI = dyn_cast<ApplyInst>(&I);
      if (!AI)
        continue;

      auto *Callee = getEligibleFunction(AI);
      if (Callee && isProfitableInColdBlock(AI, Callee)) {
        AppliesToInline.push_back(AI);
      }
    }
    domOrder.pushChildren(block);
  }
}


//===----------------------------------------------------------------------===//
//                          Performance Inliner Pass
//===----------------------------------------------------------------------===//

namespace {
class SILPerformanceInlinerPass : public SILFunctionTransform {
  /// Specifies which functions not to inline, based on @_semantics and
  /// global_init attributes.
  InlineSelection WhatToInline;
  std::string PassName;

public:
  SILPerformanceInlinerPass(InlineSelection WhatToInline, StringRef LevelName):
    WhatToInline(WhatToInline), PassName(LevelName) {
    PassName.append(" Performance Inliner");
  }

  void run() override {
    DominanceAnalysis *DA = PM->getAnalysis<DominanceAnalysis>();
    SILLoopAnalysis *LA = PM->getAnalysis<SILLoopAnalysis>();

    if (getOptions().InlineThreshold == 0) {
      return;
    }

    SILPerformanceInliner Inliner(getOptions().InlineThreshold,
                                  WhatToInline);

    assert(getFunction()->isDefinition() &&
           "Expected only functions with bodies!");

    // Inline things into this function, and if we do so invalidate
    // analyses for this function and restart the pipeline so that we
    // can further optimize this function before attempting to inline
    // in it again.
    if (Inliner.inlineCallsIntoFunction(getFunction(), DA, LA)) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
      restartPassPipeline();
    }
  }

  StringRef getName() override { return PassName; }
};
} // end anonymous namespace

/// Create an inliner pass that does not inline functions that are marked with
/// the @_semantics, @effects or global_init attributes.
SILTransform *swift::createEarlyInliner() {
  return new SILPerformanceInlinerPass(
    InlineSelection::NoSemanticsAndGlobalInit, "Early");
}

/// Create an inliner pass that does not inline functions that are marked with
/// the global_init attribute or have an "availability" semantics attribute.
SILTransform *swift::createPerfInliner() {
  return new SILPerformanceInlinerPass(InlineSelection::NoGlobalInit, "Middle");
}

/// Create an inliner pass that inlines all functions that are marked with
/// the @_semantics, @effects or global_init attributes.
SILTransform *swift::createLateInliner() {
  return new SILPerformanceInlinerPass(InlineSelection::Everything, "Late");
}
