/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBindingEmitter.h"

#if ENABLE(AOT)

#include "AOTClosureTestEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTHintEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isPlacedInEnvironment(const PlacedSlot& slot)
{
    return slot.hasStorage() && !slot.needsExtension();
}

void BindingEmitter::failAndDefineZero(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
}

bool BindingEmitter::endsWayAtEnvironmentThatIsNeverMade(LayoutID layout)
{
    if (m_plan.isInUse(layout))
        return false;
    m_out.unreachable();
    m_out.appendTo(newBlock());
    return true;
}

void BindingEmitter::generateParameter(const ValueContext&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

CellPointer BindingEmitter::singletonClosureOf(const ValueContext& value, FunctionID function)
{
    return emitter<ConstantEmitter>().singletonClosure(value, function);
}

void BindingEmitter::defineSingletonClosure(const ValueContext& value, FunctionID function)
{
    switch (value.representation()) {
    case Representation::Cell:
        define(value, singletonClosureOf(value, function));
        return;
    case Representation::JSValue:
        define(value, boxCell(singletonClosureOf(value, function)));
        return;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::RawWord:
        failAndDefineZero(value, InternalErrorKind::OpcodeWithoutCode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BindingEmitter::generateGetModuleVariable(const ValueContext& value)
{
    VariableID variable = value->variable();
    PlannedVariable planned = m_plan.variable(variable);
    if (value->returnsRawSlot()) {
        if (planned.hasWordInStatics())
            define(value, emitter<StaticsEmitter>().loadVariable(variable));
        else if (m_plan.hasClosureCell(planned.function))
            define(value, boxCell(emitter<ClosureTestEmitter>().closureIfCreated(planned.function)));
        else
            define(value, boxedConstant(JSValue()));
        return;
    }
    switch (planned.storage) {
    case VariableStorage::Statics:
        break;
    case VariableStorage::ClosureTable:
        defineSingletonClosure(value, planned.function);
        return;
    }

    if (Type read = typeOf(value.id()); planned.encoding == VariableEncoding::FunctionAsZero && !read.primitives && singleFunctionOf(types(), read) == planned.function) {
        m_mentions.add(NumberSpace::Variable, variable);
        defineSingletonClosure(value, planned.function);
        return;
    }

    Operand loaded = emitter<StaticsEmitter>().loadVariable(variable);
    switch (planned.encoding) {
    case VariableEncoding::Plain:
        define(value, loaded);
        return;
    case VariableEncoding::FunctionAsZero: {
        Boxed word = loaded.as<Boxed>();
        define(value, either<Boxed>(isNotEmpty(word), [&] {
            return word;
        }, [&] {
            return boxCell(singletonClosureOf(value, planned.function));
        }));
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BindingEmitter::generatePutModuleVariable(const ValueContext& value)
{
    VariableID variable = value->variable();
    switch (m_plan.variable(variable).storage) {
    case VariableStorage::Statics:
        emitter<StaticsEmitter>().storeVariable(variable, get(value->valueOperand()), typeOf(value->valueOperand()), m_code.writesWhatTurnHasMade(value.id()));
        return;
    case VariableStorage::ClosureTable:
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BindingEmitter::generateGetStaticFieldOrAbsent(const ValueContext& value)
{
    VariableID variable = value->variable();
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    if (!statics.holdsStaticField(variable) || value.representation() != Representation::JSValue) {
        failAndDefineZero(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    if (value->findsProperty()) {
        Operand held = emitter<SlotEmitter>().load(statics.addressOfStaticField(variable), SlotRepresentation::JSValue);
        LBasicBlock liesInVariable = newBlock();
        LBasicBlock isProperty = newBlock();
        OperandJoin join(m_out, Representation::JSValue);
        branch(Condition(m_out.isZero64(held.as<Boxed>().value())), { rareEdge(isProperty), normalEdge(liesInVariable) });

        m_out.appendTo(liesInVariable);
        join.add(held);

        m_out.appendTo(isProperty);
        join.add(callOperation<Boxed>(value, RuntimeOperation::MovedStaticField, { in<CellPointer>(value->classObjectOperand()), identifierNumber(value->key()) }));
        define(value, join.finish());
        return;
    }
    define(value, emitter<SlotEmitter>().load(statics.addressOfStaticField(variable), SlotRepresentation::JSValue));
}

void BindingEmitter::generatePutStaticField(const ValueContext& value)
{
    VariableID variable = value->variable();
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    if (!statics.holdsStaticField(variable)) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    statics.storeStaticField(variable, in<Boxed>(value->valueOperand()), typeOf(value->valueOperand()), m_code.writesWhatTurnHasMade(value.id()));
}

void BindingEmitter::generateGetEnvironmentField(const ValueContext& value)
{
    LayoutID layout = value->layout();
    uint32_t field = value->field();
    const PlacedSlot& slot = m_plan.slot(layout, field);
    if (endsWayAtEnvironmentThatIsNeverMade(layout)) {
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    if (!isPlacedInEnvironment(slot)) {
        failAndDefineZero(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    if (VariableID variable = m_plan.promotedEnvironments().variableOf(layout, field)) {
        define(value, emitter<StaticsEmitter>().loadFieldOfEnvironment(variable, layout, field));
        return;
    }
    CellPointer environment = in<CellPointer>(value->environmentOperand());
    emitter<HintEmitter>().crashIfLifetimeEnded(value, environment);
    define(value, emitter<SlotEmitter>().loadSlot(environment, slot, MemoryLocation::environmentField(layout, field)));
}

void BindingEmitter::generatePutEnvironmentField(const ValueContext& value)
{
    LayoutID layout = value->layout();
    uint32_t field = value->field();
    const PlacedSlot& slot = m_plan.slot(layout, field);
    if (endsWayAtEnvironmentThatIsNeverMade(layout)) {
        m_generator.noteThatOwnerIsRememberedBehind(value.id());
        return;
    }
    if (!isPlacedInEnvironment(slot)) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    if (VariableID variable = m_plan.promotedEnvironments().variableOf(layout, field)) {
        emitter<StaticsEmitter>().storeFieldOfEnvironment(variable, layout, field, get(value->valueOperand()), typeOf(value->valueOperand()), m_code.writesWhatTurnHasMade(value.id()));
        m_generator.noteThatOwnerIsRememberedBehind(value.id());
        return;
    }
    CellPointer environment = in<CellPointer>(value->environmentOperand());
    emitter<HintEmitter>().crashIfLifetimeEnded(value, environment);
    ValueID storedValue = value->valueOperand();
    Operand stored = get(storedValue);
    emitter<SlotEmitter>().storeSlot(environment, slot, MemoryLocation::environmentField(layout, field), stored);
    barrierAfterStore(value, environment, typeOf(storedValue));
}

void BindingEmitter::generateScopeHasBinding(const ValueContext& value)
{
    Boxed scope = boxCell(in<CellPointer>(value->scopeOperand()));
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::ScopeHasBinding, { scope, identifierNumber(value->key()) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
