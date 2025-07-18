//===- llvm/Transforms/Utils/LowerMemIntrinsics.h ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lower memset, memcpy, memmov intrinsics to loops (e.g. for targets without
// library support).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_LOWERMEMINTRINSICS_H
#define LLVM_TRANSFORMS_UTILS_LOWERMEMINTRINSICS_H

#include "llvm/Support/Compiler.h"
#include <cstdint>
#include <optional>

namespace llvm {

class AnyMemCpyInst;
class ConstantInt;
class Instruction;
class MemCpyInst;
class MemMoveInst;
class MemSetInst;
class MemSetPatternInst;
class ScalarEvolution;
class TargetTransformInfo;
class Value;
struct Align;

/// Emit a loop implementing the semantics of llvm.memcpy where the size is not
/// a compile-time constant. Loop will be inserted at \p InsertBefore.
LLVM_ABI void createMemCpyLoopUnknownSize(
    Instruction *InsertBefore, Value *SrcAddr, Value *DstAddr, Value *CopyLen,
    Align SrcAlign, Align DestAlign, bool SrcIsVolatile, bool DstIsVolatile,
    bool CanOverlap, const TargetTransformInfo &TTI,
    std::optional<unsigned> AtomicSize = std::nullopt);

/// Emit a loop implementing the semantics of an llvm.memcpy whose size is a
/// compile time constant. Loop is inserted at \p InsertBefore.
LLVM_ABI void createMemCpyLoopKnownSize(
    Instruction *InsertBefore, Value *SrcAddr, Value *DstAddr,
    ConstantInt *CopyLen, Align SrcAlign, Align DestAlign, bool SrcIsVolatile,
    bool DstIsVolatile, bool CanOverlap, const TargetTransformInfo &TTI,
    std::optional<uint32_t> AtomicCpySize = std::nullopt);

/// Expand \p MemCpy as a loop. \p MemCpy is not deleted.
LLVM_ABI void expandMemCpyAsLoop(MemCpyInst *MemCpy,
                                 const TargetTransformInfo &TTI,
                                 ScalarEvolution *SE = nullptr);

/// Expand \p MemMove as a loop. \p MemMove is not deleted. Returns true if the
/// memmove was lowered.
LLVM_ABI bool expandMemMoveAsLoop(MemMoveInst *MemMove,
                                  const TargetTransformInfo &TTI);

/// Expand \p MemSet as a loop. \p MemSet is not deleted.
LLVM_ABI void expandMemSetAsLoop(MemSetInst *MemSet);

/// Expand \p MemSetPattern as a loop. \p MemSet is not deleted.
LLVM_ABI void expandMemSetPatternAsLoop(MemSetPatternInst *MemSet);

/// Expand \p AtomicMemCpy as a loop. \p AtomicMemCpy is not deleted.
LLVM_ABI void expandAtomicMemCpyAsLoop(AnyMemCpyInst *AtomicMemCpy,
                                       const TargetTransformInfo &TTI,
                                       ScalarEvolution *SE);

} // namespace llvm

#endif
