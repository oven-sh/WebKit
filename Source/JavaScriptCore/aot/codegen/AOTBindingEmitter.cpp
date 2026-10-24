/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBindingEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTPlacement.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTSlotEmitter.h"
#include "AOTStaticsEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isPlacedInEnvironment(const PlacedSlot& slot)
{
    return slot.hasStorage() && !slot.needsExtension();
}

BindingEmitter::BindingEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void BindingEmitter::failAndDefineZero(const ValueContext& value, InternalErrorKind kind)
{
    fail(value, kind);
    define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
}

void BindingEmitter::generateParameter(const ValueContext&)
{
    RELEASE_ASSERT_NOT_REACHED();
}

void BindingEmitter::defineSingletonClosure(const ValueContext& value, FunctionID function)
{
    switch (value.representation()) {
    case Representation::Cell:
        define(value, emitter<ConstantEmitter>().singletonClosure(value, function));
        return;
    case Representation::JSValue:
        define(value, boxCell(emitter<ConstantEmitter>().singletonClosure(value, function)));
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
    const PlacedVariable& placed = m_placement.variable(variable);
    switch (placed.storage) {
    case VariableStorage::Statics:
        break;
    case VariableStorage::ClosureTable:
        defineSingletonClosure(value, placed.function);
        return;
    case VariableStorage::None:
        failAndDefineZero(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }

    Operand loaded = emitter<StaticsEmitter>().loadVariable(variable);
    switch (placed.encoding) {
    case VariableEncoding::Plain:
    case VariableEncoding::UndefinedAsZero:
        define(value, loaded);
        return;
    case VariableEncoding::FunctionAsZero: {
        Boxed word = loaded.as<Boxed>();
        define(value, either<Boxed>(isNotEmpty(word), [&] {
            return word;
        }, [&] {
            return boxCell(emitter<ConstantEmitter>().singletonClosure(value, placed.function));
        }));
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BindingEmitter::generateGetEncodedModuleVariable(const ValueContext& value)
{
    VariableID variable = value->variable();
    if (m_placement.variable(variable).encoding != VariableEncoding::UndefinedAsZero) {
        failAndDefineZero(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    define(value, emitter<StaticsEmitter>().loadEncodedVariable(variable));
}

void BindingEmitter::generatePutModuleVariable(const ValueContext& value)
{
    VariableID variable = value->variable();
    switch (m_placement.variable(variable).storage) {
    case VariableStorage::Statics:
        emitter<StaticsEmitter>().storeVariable(variable, get(value->valueOperand()), typeOf(value->valueOperand()));
        return;
    case VariableStorage::ClosureTable:
    case VariableStorage::None:
        fail(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BindingEmitter::generateGetEnvironmentField(const ValueContext& value)
{
    LayoutID layout = value->layout();
    uint32_t field = value->field();
    const PlacedSlot& slot = m_placement.slot(layout, field);
    if (!isPlacedInEnvironment(slot)) {
        failAndDefineZero(value, InternalErrorKind::SlotAccessNotPlaced);
        return;
    }
    CellPointer environment = in<CellPointer>(value->environmentOperand());
    bool isProvenToFollowStore = flowOf(m_program, FieldReference { layout, field }).location.initialization != InitializationState::Unknown;
    if (generator().validates() && isProvenToFollowStore && !(typeOf(value.id()).primitives & Primitive::Absent))
        emitter<RuntimeValidationEmitter>().checkBindingIsStored(value, emitter<SlotEmitter>().isFilled(environment, slot, MemoryLocation::environmentField(layout, field)));
    define(value, emitter<SlotEmitter>().loadSlot(environment, slot, MemoryLocation::environmentField(layout, field)));
}

void BindingEmitter::generatePutEnvironmentField(const ValueContext& value)
{
    LayoutID layout = value->layout();
    uint32_t field = value->field();
    const PlacedSlot& slot = m_placement.slot(layout, field);
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
