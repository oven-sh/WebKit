/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

bool OperatorEmitter::leaveOperandToInstanceOf(const ValueContext& read, InstanceOfOperand operand, NumberedImmediate number)
{
    std::span<const ValueID> behind = read.operationsBehind();
    while (!behind.empty() && operationAt(behind.front()).traits().isPure())
        behind = behind.subspan(1);
    if (behind.empty() || behind.front() != m_code.onlyUserOf(read.id()))
        return false;
    const Operation& test = operationAt(behind.front());
    bool walksChain = operand == InstanceOfOperand::PrototypeThatIsBuiltin;
    if (test.opcode != (walksChain ? Opcode::HasInPrototypeChain : Opcode::InstanceOf) || (walksChain ? test.prototypeOperand() : test.constructorOperand()) != read.id())
        return false;
    if (!test.traits().mayCallUnknownCode())
        return false;
    m_operandLeftToInstanceOf = OperandLeftToInstanceOf { read.id(), operand, number };
    return true;
}

void OperatorEmitter::generateInstanceOf(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    if (std::optional left = std::exchange(m_operandLeftToInstanceOf, std::nullopt)) {
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
    std::optional left = std::exchange(m_operandLeftToInstanceOf, std::nullopt);
    RELEASE_ASSERT(!left || left->read == prototypeID);
    auto walksChain = [&] {
        if (left)
            return Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(left->operand), { object.value() }, { .numberedImmediate = left->number, .runsCodeOfHost = true }).first));
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

void OperatorEmitter::generateTryFindInPrototypeChain(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    CellPointer prototype = in<CellPointer>(value->prototypeOperand());
    define(value, Word32(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::PrototypeWithoutTraps), { object.value(), boxCell(prototype).value() }).first)));
}

void OperatorEmitter::generateHasDefaultHasInstance(const ValueContext& value)
{
    defineTest(value, callLeaf<Condition>(RuntimeOperation::HasDefaultHasInstance, { in<Boxed>(value->constructorOperand()) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
