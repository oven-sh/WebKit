/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

bool OperatorEmitter::leaveConstructorToInstanceOf(const ValueContext& read, InstanceOfOperand operand, NumberedImmediate number)
{
    std::span<const ValueID> behind = read.operationsBehind();
    while (!behind.empty() && operationAt(behind.front()).traits().isPure())
        behind = behind.subspan(1);
    if (behind.empty() || behind.front() != m_code.onlyUserOf(read.id()) || opcodeOf(behind.front()) != Opcode::InstanceOf || operationAt(behind.front()).constructorOperand() != read.id())
        return false;
    m_constructorLeftToInstanceOf = ConstructorLeftToInstanceOf { read.id(), operand, number };
    return true;
}

void OperatorEmitter::generateInstanceOf(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    if (std::optional left = std::exchange(m_constructorLeftToInstanceOf, std::nullopt)) {
        RELEASE_ASSERT(left->read == value->constructorOperand());
        defineTest(value, Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(left->operand), { object.value() }, { .numberedImmediate = left->number }).first)));
        return;
    }
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
