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

    // tested is the type of the value that the cell is.
    Condition isClosureOf(CellPointer, FunctionID, const Type& tested, CodeShape);
    void branchOnIsClosureOf(CellPointer, FunctionID, const Type& tested, BranchTargets, CodeShape);
    // Whether the test outside fast code is a stub that also refuses what is no cell.
    bool asksStubWhetherClosureOf(FunctionID, CodeShape) const;
    void branchOnIsClosureOfByStub(LValue cellOrOtherValue, FunctionID, BranchTargets);
    Condition isClosureOfAny(CellPointer, std::span<const FunctionID>);
    Condition hasFunctionCellType(CellPointer);
    // Of whichever instance.
    Condition isSlimClosureAsCreated(CellPointer);
    CellPointer closureIfCreated(FunctionID genericEntrypoint);

private:
    struct EntrypointRun {
        uint32_t first;
        uint32_t last;
    };

    // Every slim closure is one of this instance, so the word of one that is as it was created names its function and no other cell has that word.
    bool closureWordNames(FunctionID) const;
    bool isDecidedByClosureWord(FunctionID, const Type& tested) const;
    Word32 upperHalfOfWordAtSlimClosureWord(CellPointer);
    void branchOnIsClosureOfAny(CellPointer, std::span<const FunctionID>, BranchTargets);
    void branchOnIndexInRuns(Word32 index, std::span<const EntrypointRun>, BranchTargets);
    ptrdiff_t offsetOfClosureHeader(FunctionKindData) const;
    Condition isTableCell(CellPointer, FunctionID genericEntrypoint);
    Word32 closureTableEntry(FunctionID genericEntrypoint);
    TypedPointer closureCellOf(FunctionID genericEntrypoint);
    Condition hasClosureStructure(CellPointer, FunctionID);
    Condition hasClosureWordOf(CellPointer, bool isSlim, FunctionID genericEntrypoint);
    uint32_t upperHalfOfClosureWordOf(FunctionID genericEntrypoint);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
