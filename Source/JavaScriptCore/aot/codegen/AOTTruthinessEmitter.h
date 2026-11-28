/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"
#include "AOTShapeTestEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class TruthinessEmitter final : public OperationEmitter {
public:
    explicit TruthinessEmitter(CodeGenerator&);

    Condition isTruthy(const OperationContext&, Operand, const Type&);
    Condition isTruthyOfAnyKind(const OperationContext&, Boxed);
    void branchOnTruthiness(const OperationContext&, Operand, const Type&, BranchTargets, bool expectsObjectOrNothing = false, bool expectsInt32OrNothing = false, bool isAnswerOfCall = false, bool expectsBoolean = false, bool isOfAnyKind = false);

    struct DeferredTruthTest {
        ValueID shapeTest;
        ValueID source;
        Boxed value;
        BlockEdge isFalsy;
    };
    bool tryDeferToShapeTest(const ValueContext& question, Boxed);
    std::optional<DeferredTruthTest> takeTruthTestOf(ValueID shapeTest);
    bool hasDeferredTruthTests() const { return !m_deferredTruthTests.isEmpty(); }
    void branchOnShapeOrTruth(const ValueContext& shapeTest, const DeferredTruthTest&, BranchTargets shape, BlockEdge isTruthy, MarkedObjects);

private:
    ValueID shapeTestAfter(const ValueContext& question);
    Condition isTruthyCell(CellPointer, const Type&);
    Condition isTruthyBoxed(const OperationContext&, Boxed, const Type&);
    void branchOnTruthinessOfAnyValue(const OperationContext&, Boxed, const Type&, BranchTargets, bool expectsObjectOrNothing, bool expectsInt32OrNothing, bool isAnswerOfCall, bool expectsBoolean, bool isOfAnyKind);
    Type withoutPrimitives(const Type&, uint16_t removed);

    Vector<DeferredTruthTest, 2> m_deferredTruthTests;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
