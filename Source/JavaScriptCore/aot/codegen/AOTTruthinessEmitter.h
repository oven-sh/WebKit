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
    void branchOnTruthiness(const OperationContext&, Operand, const Type&, BranchTargets, bool expectsObjectOrNothing = false, bool expectsInt32OrNothing = false, bool isAnswerOfCall = false, bool expectsBoolean = false);

private:
    Condition isTruthyCell(CellPointer, const Type&);
    Condition isTruthyBoxed(const OperationContext&, Boxed, const Type&);
    void branchOnTruthinessOfAnyValue(const OperationContext&, Boxed, const Type&, BranchTargets, bool expectsObjectOrNothing, bool expectsInt32OrNothing, bool isAnswerOfCall, bool expectsBoolean);
    Type withoutPrimitives(const Type&, uint16_t removed);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
