/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTConstantEmitter.h"
#include "AOTFunctionAnalyses.h"
#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

void OperatorEmitter::generateInstanceOf(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    Boxed constructor = in<Boxed>(value->constructorOperand());
    defineTest(value, Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::Constructor), { object.value(), constructor.value() }).first)));
}

// What is no cell has no prototype chain: that is answered in line, and the prototype of a built-in class that only this walk asks for is fetched behind that answer.
bool OperatorEmitter::fetchesPrototypeItself(ValueID builtin)
{
    // Validation reads every operand in front of its operation.
    if (generator().validates())
        return false;
    std::optional<Use> use = analyses().useLists().singleUse(builtin);
    if (!use || use->kind != Use::Kind::Operand)
        return false;
    const Value& user = m_function.at(use->user);
    return user.opcode == Opcode::HasInPrototypeChain && user.prototypeOperand() == builtin && user.valueOperand() != builtin
        && use->block == analyses().useLists().locationOf(builtin).block && typeOf(user.valueOperand()).primitives;
}

void OperatorEmitter::generateHasInPrototypeChain(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->valueOperand());
    ValueID prototypeID = value->prototypeOperand();
    auto walksChain = [&] {
        const Value& definition = m_function.at(prototypeID);
        CellPointer prototype = definition.opcode == Opcode::Builtin && fetchesPrototypeItself(prototypeID)
            ? emitter<ConstantEmitter>().builtinCell(value, m_builtins.canonicalID(definition.builtin()))
            : in<CellPointer>(prototypeID);
        return Condition(m_out.castToInt32(callStub(value, AOT::instanceOf(InstanceOfOperand::Prototype), { object.value(), boxCell(prototype).value() }).first));
    };
    if (!typeOf(value->valueOperand()).primitives) {
        defineTest(value, walksChain());
        return;
    }
    defineTestByBranching(value, [&](BranchTargets targets) {
        LBasicBlock isCellBlock = newBlock();
        branch(isCell(object), { normalEdge(isCellBlock), targets.notTaken });
        m_out.appendTo(isCellBlock);
        branch(walksChain(), targets);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
