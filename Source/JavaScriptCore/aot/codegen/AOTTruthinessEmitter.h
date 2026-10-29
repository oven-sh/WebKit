/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class TruthinessEmitter final : public OperationEmitter {
public:
    explicit TruthinessEmitter(CodeGenerator&);

    Condition isTruthy(const OperationContext&, Operand, const Type&);
    void branchOnTruthiness(const OperationContext&, Operand, const Type&, BranchTargets);

private:
    Condition isTruthyCell(CellPointer, const Type&);
    Condition isTruthyBoxed(const OperationContext&, Boxed, const Type&);
    void branchOnTruthinessOfAnyValue(const OperationContext&, Boxed, const Type&, BranchTargets);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
