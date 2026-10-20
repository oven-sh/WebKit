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
    Condition hasFunctionCellType(CellPointer);
    Condition isUnreifiedSlimClosure(CellPointer);
    CellPointer closureIfCreated(FunctionID genericEntrypoint);

private:
    Condition isTableCell(CellPointer, FunctionID genericEntrypoint);
    Word32 closureTableEntry(FunctionID genericEntrypoint);
    Condition hasClosureStructure(CellPointer, FunctionID);
    Condition hasClosureWordOf(CellPointer, bool isSlim, FunctionID genericEntrypoint);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
