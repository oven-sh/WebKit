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

namespace JSC { namespace AOT { namespace Typed {

static const PlacedSlot& slotOf(const RepresentationInputs& inputs, FieldAccessID id)
{
    RELEASE_ASSERT(id);
    return inputs.placement.fieldAccess(id).slot;
}

static Representation storeRepresentationOf(const PlacedSlot& slot)
{
    bool isStoreElided = !slot.hasStorage() || slot.representation == SlotRepresentation::DeferredClosure;
    if (isStoreElided)
        return Representation::Unchosen;
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
    Representation stored = representationOf(slot.representation);
    if (isLegalRepresentation(type, stored))
        return stored;
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

static unsigned groupCallParameterIndex(const Value& call, uint32_t operandIndex)
{
    std::optional<uint32_t> argumentIndex = call.variadicOperandIndex(operandIndex);
    return argumentIndex ? *argumentIndex + 1 : 0;
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
        return storeRepresentationOf(slotOf(inputs, user.fieldAccess()));
    case Opcode::PutSlots:
        return storeRepresentationOf(slotOf(inputs, user.placedKeys()[variadicIndex()].fieldAccess));
    case Opcode::NewObject:
        return storeRepresentationOf(inputs.placement.slot(user.layout(), user.fields()[variadicIndex()]));
    case Opcode::CloneAs:
    case Opcode::PrivateDefine:
        return Representation::JSValue;
    case Opcode::PutEnvironmentField:
        return storeRepresentationOf(inputs.placement.slot(user.layout(), user.field()));
    case Opcode::PutModuleVariable:
        return storeRepresentationOf(inputs.placement.variable(user.variable()));
    case Opcode::NewClosure:
        return captureRepresentationOf(inputs, user.function(), variadicIndex());
    case Opcode::CallDirect:
    case Opcode::AwaitCallBySavingFrame:
        return inputs.conventions.parameterRepresentation(inputs.program, user.function(), variadicIndex());
    case Opcode::CallGroup:
        return inputs.conventions.parameterRepresentation(inputs.program, user.function(), groupCallParameterIndex(user, operandIndex));
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
        return loadRepresentationOf(slotOf(inputs, value.fieldAccess()), type);
    case Opcode::GetEnvironmentField:
        return representationOf(inputs.placement.slot(value.layout(), value.field()).representation);
    case Opcode::GetModuleVariable:
    case Opcode::GetEncodedModuleVariable:
        return loadRepresentationOf(inputs.placement.variable(value.variable()), type);
    case Opcode::GetCapture:
        return captureRepresentationOf(inputs, value.function(), value.index());
    case Opcode::CallDirect:
    case Opcode::CallGroup:
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
    case Opcode::CallGroup:
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
