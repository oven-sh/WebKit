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
    Condition isClosureOf(CellPointer, FunctionID, const Type& tested);
    // The operation IsClosureOf, in whichever form it is emitted. It refuses no closure of the function.
    void branchOnIsClosureOf(CellPointer, FunctionID, const Type& tested, BranchTargets);
    // Reads the closure number once, whatever the number of functions.
    Condition isClosureOfAny(CellPointer, std::span<const FunctionID>, const Type& tested);
    Condition hasFunctionCellType(CellPointer);
    // Of whichever instance.
    Condition isSlimClosureAsCreated(CellPointer);
    // Of any value but the empty one. Taken: a closure whose word has ClosureWord::endsItsTailCallsBit.
    void branchOnEndsItsTailCalls(Boxed, BranchTargets);
    CellPointer closureIfCreated(FunctionID);
    // While it is set no test refuses a closure of the function that it asks for.
    bool& passesChangedClosures() { return m_passesChangedClosures; }

private:
    FunctionKindData closureKindOf(FunctionID) const;
    // Every slim closure is one of this instance, so the word of one that is as it was created names its function and no other cell has that word.
    bool closureWordNames(FunctionID) const;
    bool isDecidedByClosureWord(FunctionID, const Type& tested);
    Condition hasClosureWordThatDecides(CellPointer, FunctionID);
    void branchOnClosureTest(CellPointer, FunctionID, const Type& tested, BranchTargets);
    // Each closure number from that of the first function to that of the last is one of a function that the test asks for: CallingConventions::haveNeighboringClosureNumbers().
    struct ClosureNumberRun {
        FunctionID first;
        FunctionID last;
    };
    void branchOnIsClosureOfAny(CellPointer, std::span<const FunctionID>, const Type& tested, BranchTargets);
    // `addend` is what the word holds beside the closure number.
    void branchOnClosureNumberInRuns(Word32, std::span<const ClosureNumberRun> ascendingRuns, int32_t addend, bool mayLieBelowFirstRun, BranchTargets);
    // For a cell that is not as a closure with that number is created. Stubs answer, which every test shares: the block holds the number, the call and the branch.
    void branchOnIsChangedClosureWithNumber(CellPointer, bool isSlim, uint32_t mentionOfClosureNumber, BranchTargets);
    // noClosureNumber, which lies in no run, for what is no closure of this instance.
    Word32 numberOfChangedClosure(CellPointer);
    Word32 upperHalfOfWordAtSlimClosureWord(CellPointer);
    ptrdiff_t offsetOfClosureHeader(FunctionKindData) const;
    Word32 closureTableEntry(FunctionID);
    // NumberSpace::ClosureCell.
    TypedPointer closureCellOf(FunctionID);
    Condition hasClosureStructure(CellPointer, FunctionID);
    Condition hasClosureWordOf(CellPointer, bool isSlim, FunctionID);
    // Image's: NumberSpace::ClosureFunction.
    Word32 closureNumberOf(FunctionID, int32_t addend = 0);
    // The other fields of the half are the addend of the number.
    Word32 upperHalfOfClosureWordOf(FunctionID);

    bool m_passesChangedClosures { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
