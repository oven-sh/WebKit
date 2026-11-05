/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class ClosureTestEmitter final : public OperationEmitter {
public:
    explicit ClosureTestEmitter(CodeGenerator&);

    Condition isClosureOf(CellPointer, FunctionID, const Type& tested);
    void branchOnIsClosureOf(CellPointer, FunctionID, const Type& tested, BranchTargets);
    Condition isClosureOfAny(CellPointer, std::span<const FunctionID>, const Type& tested);
    Condition hasFunctionCellType(CellPointer);
    Condition isSlimClosureAsCreated(CellPointer);
    void branchOnEndsItsTailCalls(Boxed, BranchTargets);
    CellPointer closureIfCreated(FunctionID);
    bool& passesChangedClosures() { return m_passesChangedClosures; }

private:
    FunctionKindData closureKindOf(FunctionID) const;
    bool closureWordNames(FunctionID) const;
    bool isDecidedByClosureWord(FunctionID, const Type& tested);
    Condition hasClosureWordThatDecides(CellPointer, FunctionID);
    void branchOnClosureTest(CellPointer, FunctionID, const Type& tested, BranchTargets);
    struct ClosureNumberRun {
        FunctionID first;
        FunctionID last;
    };
    void branchOnIsClosureOfAny(CellPointer, std::span<const FunctionID>, const Type& tested, BranchTargets);
    void branchOnClosureNumberInRuns(Word32, std::span<const ClosureNumberRun> ascendingRuns, int32_t addend, bool mayLieBelowFirstRun, BranchTargets);
    void branchOnIsChangedClosureWithNumber(CellPointer, bool isSlim, uint32_t mentionOfClosureNumber, BranchTargets);
    Word32 numberOfChangedClosure(CellPointer);
    Word32 upperHalfOfWordAtSlimClosureWord(CellPointer);
    ptrdiff_t offsetOfClosureHeader(FunctionKindData) const;
    Word32 closureTableEntry(FunctionID);
    TypedPointer closureCellOf(FunctionID);
    Condition hasClosureStructure(CellPointer, FunctionID);
    Condition hasClosureWordOf(CellPointer, bool isSlim, FunctionID);
    Word32 closureNumberOf(FunctionID, int32_t addend = 0);
    Word32 upperHalfOfClosureWordOf(FunctionID);

    bool m_passesChangedClosures { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
