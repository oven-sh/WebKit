/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

void OperatorEmitter::generateInstanceOf(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    Boxed constructor = in<Boxed>(value->constructorOperand());
    defineTest(value, Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::Constructor), { object.value(), constructor.value() }).first)));
}

void OperatorEmitter::generateHasInPrototypeChain(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    ValueID prototypeID = value->prototypeOperand();
    auto walksChain = [&] {
        CellPointer prototype = in<CellPointer>(prototypeID);
        return Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::Prototype), { object.value(), boxCell(prototype).value() }).first));
    };
    if (!typeOf(value->valueOperand()).primitives) {
        defineTest(value, walksChain());
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isCellBlock = newBlock();
        branch(isCell(object), { normalEdge(isCellBlock), targets.notTaken });
        m_out.appendToContinuation(isCellBlock);
        branch(walksChain(), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
