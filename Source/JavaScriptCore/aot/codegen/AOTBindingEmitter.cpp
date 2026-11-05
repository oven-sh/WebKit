/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBindingEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
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

// No closure holds an environment that is never made, so what reads or writes one never runs: as SlotAccessEmitter::resultWithoutSlot() has it for objects.
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

// Whoever asks first fills the cell of the closure table, and every reader gets that one closure. No program can tell who was first,
// so the effects that the schema declares for a read of the variable name neither the table nor the allocator.
// AOTInstance_closureCells lies outside the declared effects for that reason, and a stub fills the cell.
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
    case Representation::LiteralString:
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
    switch (planned.storage) {
    case VariableStorage::Statics:
        break;
    case VariableStorage::ClosureTable:
        defineSingletonClosure(value, planned.function);
        return;
    }

    // The read is proven to give closures of the function that the variable is born as AND NOTHING ELSE, no undefined and no null (singleFunctionOf() asks the objects of a type alone), and that function has one closure:
    // no store has put anything else there, and the word is not asked. The variable stays named: a heap snapshot shows the variables that code names.
    if (Type read = typeOf(value.id()); planned.encoding == VariableEncoding::FunctionAsZero && !read.primitives && singleFunctionOf(types(), read) == planned.function) {
        m_mentions.add(NumberSpace::Variable, variable);
        defineSingletonClosure(value, planned.function);
        return;
    }

    Operand loaded = emitter<StaticsEmitter>().loadVariable(variable);
    switch (planned.encoding) {
    case VariableEncoding::Plain:
    case VariableEncoding::UndefinedAsZero:
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
        emitter<StaticsEmitter>().storeVariable(variable, get(value->valueOperand()), typeOf(value->valueOperand()));
        return;
    case VariableStorage::ClosureTable:
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// StaticFields::tryRead() behind JSObject::getOwnPropertySlot(): what somebody has put on the cell stands in front of a field of the same name, and lies in the butterfly.
void BindingEmitter::generateGetStaticFieldOrAbsent(const ValueContext& value)
{
    VariableID variable = value->variable();
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    if (!statics.holdsStaticField(variable) || value.representation() != Representation::JSValue) {
        failAndDefineZero(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    CellPointer classObject = in<CellPointer>(value->classObjectOperand());
    Boxed word = emitter<SlotEmitter>().load(statics.addressOfStaticField(variable), SlotRepresentation::JSValue).as<Boxed>();
    Condition isBare(m_out.isNull(loadField<Pointer>(classObject, m_heaps.JSObject_butterfly).value()));
    define(value, select(isBare, word, boxedConstant(JSValue())));
}

void BindingEmitter::generatePutStaticField(const ValueContext& value)
{
    VariableID variable = value->variable();
    StaticsEmitter& statics = emitter<StaticsEmitter>();
    if (!statics.holdsStaticField(variable)) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    statics.storeStaticField(variable, in<Boxed>(value->valueOperand()));
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
    CellPointer environment = in<CellPointer>(value->environmentOperand());
    define(value, emitter<SlotEmitter>().loadSlot(environment, slot, MemoryLocation::environmentField(layout, field)));
}

void BindingEmitter::generatePutEnvironmentField(const ValueContext& value)
{
    LayoutID layout = value->layout();
    uint32_t field = value->field();
    const PlacedSlot& slot = m_plan.slot(layout, field);
    if (endsWayAtEnvironmentThatIsNeverMade(layout))
        return;
    if (!isPlacedInEnvironment(slot)) {
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    CellPointer environment = in<CellPointer>(value->environmentOperand());
    ValueID storedValue = value->valueOperand();
    Operand stored = get(storedValue);
    emitter<SlotEmitter>().storeSlot(environment, slot, MemoryLocation::environmentField(layout, field), stored);
    barrierAfterStore(value, environment, stored, typeOf(storedValue));
}

void BindingEmitter::generateScopeHasBinding(const ValueContext& value)
{
    Boxed scope = boxCell(in<CellPointer>(value->scopeOperand()));
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::ScopeHasBinding, { scope, identifierNumber(value->key()) }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
