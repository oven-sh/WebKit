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

    Condition isClosureOf(CellPointer, FunctionID);
    void branchOnIsClosureOf(CellPointer, FunctionID, BranchTargets);
    Condition isClosureOfAny(CellPointer, std::span<const FunctionID>);
    Condition hasFunctionCellType(CellPointer);
    Condition isSlimClosureAsCreated(CellPointer);
    Condition hasClosureStructure(CellPointer, FunctionID);
    CellPointer closureIfCreated(FunctionID);
    Word32 closureTableEntry(FunctionID);
    CellPointer closureInTableEntry(Word32);
    std::optional<uint32_t> mentionOfUpperHalfThatNames(FunctionID);

    bool closureWordNames(FunctionID) const;
    bool cellTypeTellsFullClosures(FunctionID) const;
    Word32 upperHalfOfWordAtSlimClosureWord(CellPointer);
    Word32 upperHalfOfClosureWordOf(FunctionID);
    struct ClosureCase {
        FunctionID function;
        BlockEdge target;
    };
    void branchOnChangedClosure(CellPointer, std::span<const ClosureCase>, BlockEdge otherwise);

private:
    FunctionKindData closureKindOf(FunctionID) const;
    void branchOnClosureTest(CellPointer, FunctionID, BranchTargets);
    struct ClosureNumberRun {
        FunctionID first;
        FunctionID last;
    };
    void branchOnIsClosureOfAny(CellPointer, std::span<const FunctionID>, BranchTargets);
    void branchOnClosureNumberInRuns(Word32, std::span<const ClosureNumberRun> ascendingRuns, int32_t addend, bool mayLieBelowFirstRun, BranchTargets);
    void branchOnIsChangedClosureWithNumber(CellPointer, bool isSlim, uint32_t mentionOfClosureNumber, BranchTargets);
    Word32 numberOfChangedClosure(CellPointer);
    ptrdiff_t offsetOfClosureHeader(FunctionKindData) const;
    TypedPointer closureCellOf(FunctionID);
    Condition hasClosureWordOf(CellPointer, bool isSlim, FunctionID);
    Word32 closureNumberOf(FunctionID, int32_t addend = 0);
    uint32_t mentionOfUpperHalfOfClosureWordOf(FunctionID);

};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
