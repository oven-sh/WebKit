/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTChooseCallingConventions.h"
#include "AOTPlacement.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

static const PlacedSlot& slotOf(const RepresentationInputs& inputs, FieldAccessID id)
{
    RELEASE_ASSERT(id);
    return inputs.placement.fieldAccess(id).slot;
}

// A closure that its slot leaves unmade has no value. Whatever else such a slot takes is a reference.
static Representation storeRepresentationOf(const RepresentationInputs& inputs, const PlacedSlot& slot, ValueID stored)
{
    if (!slot.hasStorage())
        return Representation::Unchosen;
    if (slot.representation == SlotRepresentation::DeferredClosure)
        return isDeferredClosure(inputs.function, stored) ? Representation::Unchosen : Representation::Cell32;
    return representationOf(slot.representation);
}

static Representation storeRepresentationOf(const PlacedVariable& variable)
{
    if (!variable.hasWordInStatics())
        return Representation::Unchosen;
    return representationOf(variable.representation);
}

static Representation loadRepresentationOf(const PlacedSlot& slot, const Type& type)
{
    Representation loaded = slot.isOutOfLine() ? representationOf(slot.representation) : loadedRepresentationOf(slot.representation, type);
    if (isLegalRepresentation(type, loaded))
        return loaded;
    return defaultRepresentationOf(type);
}

static Representation loadRepresentationOf(const PlacedVariable& variable, const Type& type)
{
    switch (variable.storage) {
    case VariableStorage::Statics:
    case VariableStorage::None:
        return representationOf(variable.representation);
    case VariableStorage::ClosureTable:
        return defaultRepresentationOf(type);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Representation captureRepresentationOf(const RepresentationInputs& inputs, FunctionID function, uint32_t captureIndex)
{
    return declaredRepresentationOf(inputs.program, inputs.program.at(function).captures[captureIndex]);
}

static bool isReturned(const Function& function, const Value& results)
{
    RELEASE_ASSERT(&function.values.first() <= &results && &results <= &function.values.last());

    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = block.terminator.valueOperand();
        if (returned && &function.at(returned) == &results)
            return true;
    }
    return false;
}

static Representation wantedByResults(const RepresentationInputs& inputs, const Value& results, uint32_t resultIndex)
{
    const Function& function = inputs.function;
    bool isFunctionResult = function.results.size() != 1 && resultIndex < function.results.size() && isReturned(function, results);
    if (isFunctionResult)
        return inputs.conventions.resultRepresentation(inputs.program, inputs.id, resultIndex);
    return declaredRepresentationOf(inputs.program, DeclaredType { .type = function.at(results.valuesOperands()[resultIndex]).type });
}

static unsigned entryParameterIndexOf(const Function& function, const Value& parameter)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    for (unsigned index = 0; index < parameters.size(); ++index) {
        if (&function.at(parameters[index]) == &parameter)
            return index;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation wantedByDeclaredPosition(const RepresentationInputs& inputs, const Value& user, uint32_t operandIndex)
{
    if (!user.operand(operandIndex))
        return Representation::Unchosen;

    auto variadicIndex = [&] {
        return *user.variadicOperandIndex(operandIndex);
    };

    switch (user.opcode) {
    case Opcode::PutSlot:
    case Opcode::PutSlotOrAdd:
        return storeRepresentationOf(inputs, slotOf(inputs, user.fieldAccess()), user.operand(operandIndex));
    case Opcode::PutSlots:
        return storeRepresentationOf(inputs, slotOf(inputs, user.placedKeys()[variadicIndex()].fieldAccess), user.operand(operandIndex));
    case Opcode::NewObject:
        // The address of the outer object follows from the address of the inner one.
        if (user.takesCellOfInnerObject() && inputs.placement.layout(user.layout()).innerObjectIn(user.fields()[variadicIndex()]) && inputs.program.typeOf(inputs.function, user.operand(operandIndex)).isObject())
            return Representation::Cell;
        return storeRepresentationOf(inputs, inputs.placement.slot(user.layout(), user.fields()[variadicIndex()]), user.operand(operandIndex));
    case Opcode::CloneAs:
    case Opcode::PrivateDefine:
        return Representation::JSValue;
    case Opcode::PutEnvironmentField:
        return storeRepresentationOf(inputs, inputs.placement.slot(user.layout(), user.field()), user.operand(operandIndex));
    case Opcode::PutModuleVariable:
        return storeRepresentationOf(inputs.placement.variable(user.variable()));
    case Opcode::NewClosure:
        return captureRepresentationOf(inputs, user.function(), variadicIndex());
    case Opcode::CallDirect:
    case Opcode::CallTable:
    case Opcode::AwaitCallBySavingFrame:
        return inputs.conventions.parameterRepresentation(inputs.program, user.function(), variadicIndex());
    case Opcode::Results:
        return wantedByResults(inputs, user, variadicIndex());
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation wantedByDeclaredPosition(const RepresentationInputs& inputs, const Terminator& user, uint32_t operandIndex)
{
    switch (user.kind) {
    case TerminatorKind::Return:
        break;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        RELEASE_ASSERT_NOT_REACHED();
    }

    ValueID returned = user.operand(operandIndex);
    if (!returned)
        return Representation::Unchosen;
    if (numberOfResultsOf(inputs.program, inputs.function, inputs.function.at(returned)))
        return Representation::Void;
    return inputs.conventions.resultRepresentation(inputs.program, inputs.id, 0);
}

Representation resultOfDeclaredPosition(const RepresentationInputs& inputs, const Value& value)
{
    const Type& type = inputs.program.at(value.type);
    switch (value.opcode) {
    case Opcode::GetSlot:
    case Opcode::GetSlotOrUndefined:
    case Opcode::GetDeferredClosureSlot:
    case Opcode::GetDeferredClosureSlotOrUndefined:
    case Opcode::GetFlattenedSlot:
    case Opcode::GetFlattenedSlotOrUndefined:
        return loadRepresentationOf(slotOf(inputs, value.fieldAccess()), type);
    case Opcode::GetSlotGetter:
        return Representation::Cell;
    case Opcode::GetEnvironmentField:
        return representationOf(inputs.placement.slot(value.layout(), value.field()).representation);
    case Opcode::GetModuleVariable:
    case Opcode::GetEncodedModuleVariable:
        return loadRepresentationOf(inputs.placement.variable(value.variable()), type);
    case Opcode::GetCapture:
        return captureRepresentationOf(inputs, value.function(), value.index());
    case Opcode::CallDirect:
    case Opcode::CallTable:
        return inputs.conventions.resultRepresentation(inputs.program, value.function(), 0);
    case Opcode::Parameter:
        return representationOfEntryParameter(inputs, entryParameterIndexOf(inputs.function, value));
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation resultOfDeclaredPosition(const RepresentationInputs& inputs, const Value& producer, uint32_t resultIndex)
{
    switch (producer.opcode) {
    case Opcode::CallDirect:
    case Opcode::CallTable:
        return inputs.conventions.resultRepresentation(inputs.program, producer.function(), resultIndex);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation representationOfEntryParameter(const RepresentationInputs& inputs, unsigned entryParameterIndex)
{
    return inputs.conventions.parameterRepresentation(inputs.program, inputs.id, entryParameterIndex);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
