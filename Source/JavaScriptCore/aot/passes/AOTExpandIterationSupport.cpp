/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

IterationExpansion::IterationExpansion(FunctionPassContext& pass, IRBuilder& builder, SSARepair& repair, IterationOrigin origin)
    : m_pass(&pass)
    , m_program(pass.program)
    , m_function(pass.function)
    , m_types(pass.types)
    , m_builder(builder)
    , m_repair(&repair)
    , m_origin(origin)
{
}

IterationExpansion::IterationExpansion(const Program& program, Function& sharedFunction, TypeArena& types, IRBuilder& builder)
    : m_pass(nullptr)
    , m_program(program)
    , m_function(sharedFunction)
    , m_types(types)
    , m_builder(builder)
    , m_repair(nullptr)
    , m_origin(IterationOrigin::Source)
{
}

StateVariable IterationExpansion::addVariable(ValueID initialDefinition)
{
    return { m_repair->addVariable(initialDefinition), initialDefinition };
}

void IterationExpansion::define(const StateVariable& variable, ValueID definition)
{
    if (m_returnsResultsOfStep) {
        m_positionBehindStep = definition;
        return;
    }
    m_repair->addDefinition(variable.variable, definition);
}

void IterationExpansion::beginOperation(const IRBuilder::Expansion& operation)
{
    m_operation = &operation;
}

void IterationExpansion::finishStep(ValueID done, ValueID value)
{
    if (m_returnsResultsOfStep) {
        static_assert(!static_cast<size_t>(IteratorResumeResult::Done) && static_cast<size_t>(IteratorResumeResult::Value) == 1);
        ValueID position = std::exchange(m_positionBehindStep, m_positionInFrontOfStep);
        m_builder.emitReturn(m_builder.append(Opcode::Results, { done, value, position }));
        return;
    }
    RELEASE_ASSERT(m_operation && m_operation->results.size() == 2);
    std::array<ValueID, 2> results;
    results[static_cast<size_t>(IteratorResumeResult::Done)] = done;
    results[static_cast<size_t>(IteratorResumeResult::Value)] = value;
    m_builder.endExpansion(*m_operation, results);
}

void IterationExpansion::finishStepAsDone()
{
    ValueID done = m_builder.appendBoolean(true);
    finishStep(done, m_builder.appendUndefined());
}

void IterationExpansion::finishClose()
{
    RELEASE_ASSERT(m_operation);
    m_builder.endExpansion(*m_operation, std::span<const ValueID> { });
}

ValueID IterationExpansion::appendResultAt(ValueID producer, uint32_t index)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { producer });
    read.setIndex(index);
    return m_builder.append(WTF::move(read));
}

ValueID IterationExpansion::appendIncrement(ValueID integer, int64_t amount)
{
    return m_builder.append(Opcode::Add, { integer, m_builder.appendInteger(amount) });
}

ValueID IterationExpansion::appendIsEqualToInteger(ValueID value, int64_t constant)
{
    return m_builder.append(Opcode::StrictEq, { value, m_builder.appendInteger(constant) });
}

ValueID IterationExpansion::appendBuiltin(BuiltinID id)
{
    Value builtin = IRBuilder::make(Opcode::Builtin);
    builtin.setBuiltin(id);
    return m_builder.append(WTF::move(builtin));
}

ValueID IterationExpansion::appendCall(ValueID callee, ValueID thisValue, std::span<const ValueID> arguments)
{
    Value call(Opcode::Call);
    call.setCalleeOperand(callee);
    call.setThisValueOperand(thisValue);
    call.setArgumentsOperands(arguments);
    return m_builder.append(WTF::move(call));
}

ValueID IterationExpansion::appendPropGet(ValueID object, PropertyKeyID key)
{
    Value read = IRBuilder::make(Opcode::PropGet, { object });
    read.setKey(key);
    return m_builder.append(WTF::move(read));
}

ValueID IterationExpansion::appendRequireObject(ValueID value, RuntimeErrorKind kind)
{
    Value check = IRBuilder::make(Opcode::RequireObject, { value });
    check.setRuntimeError(kind);
    return m_builder.append(WTF::move(check));
}

ValueID IterationExpansion::appendUnspecified(TypeID type)
{
    Value unspecified = IRBuilder::make(Opcode::Unspecified);
    unspecified.setValueType(type);
    return m_builder.append(WTF::move(unspecified));
}

ValueID IterationExpansion::appendPair(ValueID first, ValueID second)
{
    std::array<ValueID, 2> elements { first, second };
    Value pair(Opcode::NewArray);
    pair.setContents(TypeTable::anything);
    pair.setElementsOperands(elements);
    return m_builder.append(WTF::move(pair));
}

ValueID IterationExpansion::appendValueOfKind(JSC::IterationKind kind, ValueID key, ValueID value)
{
    switch (kind) {
    case JSC::IterationKind::Keys:
        return key;
    case JSC::IterationKind::Values:
        return value;
    case JSC::IterationKind::Entries:
        return appendPair(key, value);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PropertyKeyID IterationExpansion::keyOf(WellKnownSymbol symbol)
{
    return types().wellKnownSymbolKey(symbol);
}

PropertyKeyID IterationExpansion::keyOf(BuiltinName name)
{
    return program().keyOf(name);
}

ValueID chooseArgumentOfProtocolEntry(FunctionPassContext& pass, BlockID protocolEntry, ValueID narrowed, ValueID tested)
{
    TypingContext typing { pass.program, pass.function, pass.types };
    return typing.typeOf(narrowed).isSubsetOf(typing.typeOf(pass.function.at(protocolEntry).parameters[0]), pass.program) ? narrowed : tested;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
