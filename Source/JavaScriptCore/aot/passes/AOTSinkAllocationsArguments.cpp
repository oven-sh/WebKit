/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTFlowApply.h"
#include "AOTForwardedCalls.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "Interpreter.h"
#include <algorithm>
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::CallList::list == OperandIndex::ConstructList::list);
static_assert(OperandIndex::CallList::list == OperandIndex::SuperConstructList::list);

namespace {

enum class ArgumentHolder : uint8_t { RestArray, ArgumentsObject };

enum class ArgumentsPartKind : uint8_t { Unknown, HiddenPart, Parameter, SurplusArgument, DynamicElement };

struct ArgumentsPart {
    ArgumentsPartKind kind { ArgumentsPartKind::Unknown };
    VirtualField field;
    uint32_t index { 0 };
};

} // anonymous namespace

static bool isResultTypeWithin(const Program& program, const Function& function, const Value& value, const Type& bound)
{
    if (bound.isAnything())
        return true;
    ScratchTypeArena scratch(program.types);
    return computeResultType({ program, function, scratch.arena() }, value).isSubsetOf(bound, program);
}

static bool isElementTypeWithin(const Program& program, const Function& function, TypeID list, const Type& bound)
{
    if (bound.isAnything())
        return true;
    ScratchTypeArena scratch(program.types);
    return typeOfElementRead({ program, function, scratch.arena() }, program.at(list), program.at(TypeTable::integer)).isSubsetOf(bound, program);
}

static std::optional<Value> makeArgumentRead(uint32_t firstArgument, uint32_t position)
{
    uint64_t index = static_cast<uint64_t>(firstArgument) + position;
    if (index >= maxArguments)
        return std::nullopt;

    Value read = IRBuilder::make(Opcode::GetArgument);
    read.setArgumentIndex(static_cast<uint32_t>(index));
    return read;
}

static Value makeRestLength(uint32_t firstArgument)
{
    Value length = IRBuilder::make(Opcode::GetRestLength);
    length.setFirstArgument(firstArgument);
    return length;
}

static Value makeKeyedArgumentRead(ValueID key, uint32_t firstArgument, ArgumentHolder holder)
{
    Value read = IRBuilder::make(Opcode::GetMyArgumentByVal, { key });
    read.setFirstArgument(firstArgument);
    read.setReadsArgumentsObject(holder == ArgumentHolder::ArgumentsObject);
    return read;
}

static std::optional<Value> makeArgumentListRead(const VirtualFieldAccess& access, uint32_t firstArgument)
{
    switch (access.lookup) {
    case FieldLookup::Unknown:
    case FieldLookup::AbsentEverywhere:
        return std::nullopt;
    case FieldLookup::DynamicElement:
        return makeKeyedArgumentRead(access.dynamicIndex, firstArgument, ArgumentHolder::RestArray);
    case FieldLookup::Field:
        break;
    }

    switch (access.field.kind) {
    case VirtualFieldKind::Length:
        return makeRestLength(firstArgument);
    case VirtualFieldKind::Element:
        return makeArgumentRead(firstArgument, access.field.index);
    case VirtualFieldKind::Named:
    case VirtualFieldKind::UnprovenNamed:
    case VirtualFieldKind::LayoutField:
    case VirtualFieldKind::Capture:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
    case VirtualFieldKind::Operand:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ArgumentsPart findArgumentsPart(const Program& program, const VirtualObjectDescription& arguments, const VirtualFieldAccess& access)
{
    switch (access.lookup) {
    case FieldLookup::Unknown:
    case FieldLookup::AbsentEverywhere:
        return { };
    case FieldLookup::DynamicElement:
        return { ArgumentsPartKind::DynamicElement, VirtualField::part(VirtualFieldKind::RestList), 0 };
    case FieldLookup::Field:
        break;
    }

    const VirtualField& field = access.field;
    switch (field.kind) {
    case VirtualFieldKind::Length:
        return { ArgumentsPartKind::HiddenPart, VirtualField::part(VirtualFieldKind::ArgumentCount), 0 };
    case VirtualFieldKind::Named:
        if (!arguments.isMapped || field.key != program.keyOf(BuiltinNames::callee))
            return { };
        return { ArgumentsPartKind::HiddenPart, VirtualField::part(VirtualFieldKind::Callee), 0 };
    case VirtualFieldKind::Element:
        if (field.index < arguments.numberOfParameters)
            return { ArgumentsPartKind::Parameter, field, field.index };
        return { ArgumentsPartKind::SurplusArgument, VirtualField::part(VirtualFieldKind::RestList), field.index - arguments.numberOfParameters };
    case VirtualFieldKind::UnprovenNamed:
    case VirtualFieldKind::LayoutField:
    case VirtualFieldKind::Capture:
    case VirtualFieldKind::Capacity:
    case VirtualFieldKind::Prototype:
    case VirtualFieldKind::Callee:
    case VirtualFieldKind::ArgumentCount:
    case VirtualFieldKind::RestList:
    case VirtualFieldKind::StringBase:
    case VirtualFieldKind::RangeStart:
    case VirtualFieldKind::RangeEnd:
    case VirtualFieldKind::Operand:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const VirtualObjectDescription* findFrameList(const AllocationSinking& sinking, VirtualObjectID list)
{
    if (!list || sinking.at(list).isJoin)
        return nullptr;
    const VirtualObjectDescription& description = sinking.descriptionOf(list);
    return description.kind == VirtualObjectKind::ArgumentList ? &description : nullptr;
}

static bool areParametersFrameArguments(AllocationSinking& sinking, const VirtualObjectDescription& arguments, const VirtualObjectDescription& frameList)
{
    if (frameList.firstArgument < arguments.numberOfParameters)
        return false;

    const Function& function = sinking.function();
    uint32_t firstArgument = frameList.firstArgument - arguments.numberOfParameters;
    std::span<const ValueID> parameters = function.at(arguments.allocation).parametersOperands();
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        const Value& parameter = function.at(sinking.sameValueThroughTests(parameters[index]));
        if (parameter.opcode != Opcode::GetArgument || parameter.argumentIndex() != firstArgument + index)
            return false;
    }
    return true;
}

static std::optional<Value> makeFrameRead(const VirtualObjectDescription& arguments, const VirtualObjectDescription& frameList, const ArgumentsPart& part, ValueID dynamicIndex)
{
    switch (part.kind) {
    case ArgumentsPartKind::SurplusArgument:
        return makeArgumentRead(frameList.firstArgument, part.index);
    case ArgumentsPartKind::DynamicElement:
        if (frameList.firstArgument < arguments.numberOfParameters)
            return std::nullopt;
        return makeKeyedArgumentRead(dynamicIndex, frameList.firstArgument - arguments.numberOfParameters, ArgumentHolder::ArgumentsObject);
    case ArgumentsPartKind::Unknown:
    case ArgumentsPartKind::HiddenPart:
    case ArgumentsPartKind::Parameter:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeListElementRead(RewriteCursor& cursor, ValueID list, const ArgumentsPart& part, ValueID dynamicIndex, uint32_t numberOfParameters)
{
    IRBuilder& builder = cursor.builder();
    ValueID position = part.kind == ArgumentsPartKind::SurplusArgument
        ? builder.appendInteger(part.index)
        : cursor.emit(IRBuilder::make(Opcode::Sub, { dynamicIndex, builder.appendInteger(numberOfParameters) }));
    return IRBuilder::make(Opcode::ElemGet, { list, position });
}

static FieldRead classifyParameterRead(const VirtualObjectDescription& arguments, std::optional<bool> isPassed)
{
    if (!isPassed)
        return arguments.isMapped ? FieldRead::ValueOrUndefined : FieldRead::Value;
    return *isPassed ? FieldRead::Value : FieldRead::Undefined;
}

static ValueID emitReplacement(AllocationSinking& sinking, ObjectInterpreter& interpreter, ValueID read, Value&& replacement)
{
    RewriteCursor& cursor = sinking.cursor();
    bool handlerHasMetCurrentState = sinking.inputs().throwingValues.get(read.index());
    if (mayThrow(replacement.opcode) && handlerHasMetCurrentState)
        cursor.willKeepThrowingValue(interpreter);

    ValueID emitted = cursor.emit(WTF::move(replacement));
    Function& function = cursor.builder().function();
    if (!function.at(emitted).expected().predicted() && function.at(emitted).type == function.at(read).type)
        function.edit(emitted, ValueEdit::Type).setExpected(function.at(read).expected());
    return emitted;
}

static ValueID emitParameterIfPassed(RewriteCursor& cursor, uint32_t index, ValueID count, ValueID parameter, TypeID type)
{
    ValueID none = cursor.builder().appendUndefined();
    ValueID isPassed = cursor.emit(IRBuilder::make(Opcode::Less, { cursor.builder().appendInteger(index), count }));
    return cursor.emitSelection(isPassed, parameter, none, type);
}

static ValueID emitArgumentSelection(AllocationSinking& sinking, ObjectInterpreter& interpreter, ValueID read, ValueID index, ValueID count, std::span<const ValueID> parameters, Value&& surplusRead)
{
    RewriteCursor& cursor = sinking.cursor();
    IRBuilder& builder = cursor.builder();

    ValueID none = builder.appendUndefined();
    ValueID isPassed = cursor.emit(IRBuilder::make(Opcode::Less, { index, count }));
    BlockID passed = cursor.addUncoveredBlock();
    BlockID surplus = cursor.addUncoveredBlock();
    BlockID continuation = cursor.addUncoveredBlock();
    ValueID result = builder.addParameter(continuation, TypeTable::anything);
    builder.emitBranch(isPassed, Target { passed, { }, { } }, Target { continuation, { none }, { } });

    cursor.moveTo(passed);
    Vector<std::pair<int64_t, Target>> cases;
    cases.reserveInitialCapacity(parameters.size());
    for (size_t position = 0; position < parameters.size(); ++position)
        cases.append({ static_cast<int64_t>(position), Target { continuation, { parameters[position] }, { } } });
    if (cases.isEmpty())
        builder.emitJump(surplus);
    else
        builder.emitSwitch(index, WTF::move(cases), Target { surplus, { }, { } });

    cursor.moveTo(surplus);
    ValueID surplusArgument = emitReplacement(sinking, interpreter, read, WTF::move(surplusRead));
    builder.emitJump(continuation, { surplusArgument });
    cursor.moveTo(continuation);
    return result;
}

static void becomeForwardingCall(Value& call, ValueID thisValue, uint32_t firstArgument)
{
    Value listCall = call;
    call.become(Opcode::CallForwardVarargs);
    call.setCalleeOperand(listCall.calleeOperand());
    call.setThisValueOperand(thisValue);
    call.setFirstArgument(firstArgument);
    call.setAccessSite(listCall.accessSite());
    call.setCallees(listCall.callees());
}

static void becomeForwardingSuperConstruct(Value& construct, uint32_t firstArgument)
{
    Value listConstruct = construct;
    construct.become(Opcode::ConstructSuperForwardVarargs);
    construct.setLayout(listConstruct.layout());
    construct.setParentOperand(listConstruct.parentOperand());
    construct.setNewTargetOperand(listConstruct.newTargetOperand());
    construct.setFirstArgument(firstArgument);
    construct.setAccessSite(listConstruct.accessSite());
    construct.setCallees(listConstruct.callees());
}

static std::optional<uint32_t> numberOfArgumentsThatCalleesSee(const Program& program, const Value& call)
{
    CalleeSetID stamp = call.callees();
    if (!stamp)
        return std::nullopt;
    const CalleeSet& callees = program.at(stamp);
    if (!callees.isComplete || !callees.builtins.isEmpty() || callees.functions.isEmpty())
        return std::nullopt;

    uint32_t numberOfArguments = 0;
    for (FunctionID callee : callees.functions) {
        const Function& function = program.at(callee);
        if (function.has(FunctionFlags::HasRestParameter) || function.hiddenRestParameterIndex() || function.argumentCountParameterIndex())
            return std::nullopt;
        numberOfArguments = std::max<uint32_t>(numberOfArguments, function.numberOfPositionalParameters());
    }
    return numberOfArguments;
}

void ObjectInterpreter::readArgumentList(ValueID read, VirtualObjectID object, const VirtualFieldAccess& access)
{
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    std::optional<Value> replacement;
    if (description && description->kind == VirtualObjectKind::ArgumentList)
        replacement = makeArgumentListRead(access, description->firstArgument);
    if (!replacement || !isResultTypeWithin(program(), function(), *replacement, program().typeOf(function(), read)))
        return executeOnCell(read, 0);

    if (isRewriting()) {
        resolveOperands(*replacement);
        replaceRead(read, emitReplacement(m_sinking, *this, read, WTF::move(*replacement)));
    }
}

void ObjectInterpreter::readArguments(ValueID read, VirtualObjectID object, const VirtualFieldAccess& access)
{
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    if (!description || description->kind != VirtualObjectKind::Arguments)
        return executeOnCell(read, 0);

    const VirtualFamily& family = m_sinking.familyOf(object);
    ArgumentsPart part = findArgumentsPart(program(), *description, access);
    std::optional<uint32_t> field = family.findField(part.field);
    if (part.kind == ArgumentsPartKind::Unknown || !field)
        return executeOnCell(read, 0);

    TypeID type = function().at(read).type;
    uint32_t countField = *family.findField(VirtualField::part(VirtualFieldKind::ArgumentCount));
    if (part.kind == ArgumentsPartKind::HiddenPart || part.kind == ArgumentsPartKind::Parameter) {
        FieldRead kind = FieldRead::Value;
        if (part.kind == ArgumentsPartKind::Parameter && function().at(read).opcode != Opcode::GetMappedArgument)
            kind = classifyParameterRead(*description, isArgumentPassed(object, part.index));
        VirtualObjectID held = kind == FieldRead::Undefined ? VirtualObjectID() : stateOf(object)->objectHeldIn(*field);
        if (held)
            kind = kind == FieldRead::Value ? FieldRead::HeldObject : FieldRead::Unknown;
        if (!fitsRead(read, object, *field, kind))
            return executeOnCell(read, 0);

        if (held)
            bind(read, held);
        else if (isRewriting()) {
            ValueID value = kind == FieldRead::Undefined ? emitUndefined() : valueOfField(object, *field);
            if (kind == FieldRead::ValueOrUndefined)
                value = emitParameterIfPassed(cursor(), part.index, valueOfField(object, countField), value, type);
            replaceRead(read, value);
        }
        return;
    }
    if (!description->restListStaysAsCreated)
        return executeOnCell(read, 0);

    VirtualObjectID list = stateOf(object)->objectHeldIn(*field);
    const VirtualObjectDescription* frameList = findFrameList(m_sinking, list);
    std::optional<Value> surplusRead = frameList ? makeFrameRead(*description, *frameList, part, access.dynamicIndex) : std::nullopt;
    bool fits = frameList
        ? surplusRead && isResultTypeWithin(program(), function(), *surplusRead, program().at(type))
        : isElementTypeWithin(program(), function(), typeOfField(m_sinking.objectContext(), *description, part.field), program().at(type));
    bool selectsParameter = part.kind == ArgumentsPartKind::DynamicElement
        && !(frameList && !description->isMapped && areParametersFrameArguments(m_sinking, *description, *frameList));
    if (frameList && !surplusRead)
        return executeOnCell(read, 0);
    bool assumesTypeOfRead = !fits || (selectsParameter && !program().at(type).isAnything());

    Vector<uint32_t, 8> parameterFields;
    if (selectsParameter) {
        for (uint32_t index = 0; index < description->numberOfParameters; ++index)
            parameterFields.append(*family.findField(VirtualField::element(index)));
    }

    BuildCause cause = causeOf(read, 0);
    auto buildObjectHeldIn = [&](uint32_t holdingField) {
        if (VirtualObjectID held = stateOf(object)->objectHeldIn(holdingField))
            build(held, cause);
    };
    for (uint32_t parameterField : parameterFields)
        buildObjectHeldIn(parameterField);
    if (!frameList) {
        buildObjectHeldIn(*field);
        forgetFieldsWrittenBy(read);
    }
    if (!singleDescriptionOf(object))
        return executeOnCell(read, 0);
    if (!isRewriting())
        return;

    ValueID index = part.kind == ArgumentsPartKind::DynamicElement ? resolve(access.dynamicIndex) : ValueID();
    if (surplusRead)
        resolveOperands(*surplusRead);
    else
        surplusRead = makeListElementRead(cursor(), valueOfField(object, *field), part, index, description->numberOfParameters);
    auto replaceReadBy = [&](ValueID argument) {
        if (assumesTypeOfRead && m_sinking.context().build.validates())
            cursor().emit(makeValidateType(argument, type));
        replaceRead(read, assumesTypeOfRead ? cursor().emit(makeAssumeType(argument, Proven::byRulesWithoutChecks(type))) : argument);
    };
    if (!selectsParameter) {
        replaceReadBy(emitReplacement(m_sinking, *this, read, WTF::move(*surplusRead)));
        return;
    }

    ValueID count = valueOfField(object, countField);
    Vector<ValueID, 8> parameters;
    for (uint32_t parameterField : parameterFields)
        parameters.append(valueOfField(object, parameterField));
    replaceReadBy(emitArgumentSelection(m_sinking, *this, read, index, count, parameters.span(), WTF::move(*surplusRead)));
}

void ObjectInterpreter::storeArgument(ValueID store, VirtualObjectID object, const VirtualFieldAccess& access)
{
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    bool namesParameter = description && description->kind == VirtualObjectKind::Arguments && description->isMapped
        && access.lookup == FieldLookup::Field && access.field.kind == VirtualFieldKind::Element && access.field.index < description->numberOfParameters;
    bool storesToVariable = namesParameter
        && (function().at(store).opcode == Opcode::SetMappedArgument || isArgumentPassed(object, access.field.index).value_or(false));

    std::optional<uint32_t> field = storesToVariable ? m_sinking.familyOf(object).findField(access.field) : std::nullopt;
    if (!field || !canStore(object, access, *field, function().at(store).valueOperand()))
        return executeOnCell(store, 0);

    storeField(object, *field, function().at(store).valueOperand());
    return didFoldStores(object, 1);
}

void ObjectInterpreter::forwardArgumentList(ValueID call, VirtualObjectID object)
{
    constexpr uint32_t listIndex = OperandIndex::CallList::list;

    Opcode opcode = function().at(call).opcode;
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    bool isArgumentList = description && description->kind == VirtualObjectKind::ArgumentList;
    if (!isArgumentList || opcode == Opcode::ConstructList)
        return executeOnCell(call, listIndex);
    RELEASE_ASSERT(opcode == Opcode::CallList || opcode == Opcode::SuperConstructList);

    requireCellsOfOperandsBeside(call, listIndex);

    Vector<Value, 8> namedArguments;
    std::optional<uint32_t> numberOfNamedArguments = isRewriting() ? numberOfArgumentsThatCalleesSee(program(), function().at(call)) : std::nullopt;
    for (uint32_t index = 0; numberOfNamedArguments && index < *numberOfNamedArguments; ++index) {
        if (std::optional<Value> argument = makeArgumentRead(description->firstArgument, index))
            namedArguments.append(WTF::move(*argument));
        else
            numberOfNamedArguments = std::nullopt;
    }

    if (numberOfNamedArguments) {
        Vector<ValueID, 8> arguments;
        for (Value& argument : namedArguments)
            arguments.append(cursor().emit(WTF::move(argument)));
        convertListCall(cursor().builder().function().edit(call), arguments.span());
    } else if (isRewriting() && opcode == Opcode::CallList) {
        ValueID thisValue = function().at(call).thisValueOperand();
        if (!thisValue)
            thisValue = emitUndefined();
        becomeForwardingCall(cursor().builder().function().edit(call), thisValue, description->firstArgument);
    } else if (isRewriting())
        becomeForwardingSuperConstruct(cursor().builder().function().edit(call), description->firstArgument);

    forgetFieldsWrittenBy(call);
    keep(call);
}

ArgumentsParts ObjectInterpreter::partsOfArguments(VirtualObjectID arguments) const
{
    const VirtualObjectDescription& description = *singleDescriptionOf(arguments);
    uint32_t restField = partOf(arguments, VirtualFieldKind::RestList);
    uint32_t numberOfParameters = description.numberOfParameters;

    ArgumentsParts parts;
    const VirtualObjectDescription* frameList = findFrameList(m_sinking, stateOf(arguments)->objectHeldIn(restField));
    bool readsOwnFrame = frameList && frameList->firstArgument >= numberOfParameters
        && (!numberOfParameters || (!description.isMapped && areParametersFrameArguments(m_sinking, description, *frameList)));
    if (readsOwnFrame) {
        parts.firstFrameArgument = frameList->firstArgument - numberOfParameters;
        return parts;
    }

    if (!numberOfParameters || isArgumentPassed(arguments, numberOfParameters - 1).value_or(false)) {
        parts.numberOfPassedParameters = numberOfParameters;
        return parts;
    }
    const Type& count = program().typeOf(function(), function().at(description.allocation).countOperand());
    if (count.isInteger() && count.integers.isSingleValue()) {
        parts.numberOfPassedParameters = static_cast<uint32_t>(count.integers.minimum);
        parts.restListIsEmpty = true;
    }
    return parts;
}

std::optional<Vector<uint32_t, 8>> ObjectInterpreter::requireCellsOfNamedArguments(VirtualObjectID arguments, const ArgumentsParts& parts, uint32_t first, const BuildCause& cause)
{
    if (!parts.numberOfPassedParameters)
        return std::nullopt;

    const VirtualFamily& family = m_sinking.familyOf(arguments);
    Vector<uint32_t, 8> parameterFields;
    for (uint32_t index = first; index < *parts.numberOfPassedParameters; ++index)
        parameterFields.append(*family.findField(VirtualField::element(index)));
    for (uint32_t field : parameterFields) {
        if (singleDescriptionOf(arguments))
            requireCellOfHeldObject(arguments, field, cause);
    }
    if (!parts.restListIsEmpty && singleDescriptionOf(arguments)) {
        VirtualObjectID list = stateOf(arguments)->objectHeldIn(*family.findField(VirtualField::part(VirtualFieldKind::RestList)));
        if (!hasListedElements(list))
            return std::nullopt;
        requireCellsOfListedElements(list, cause);
        if (!hasListedElements(list))
            return std::nullopt;
    }
    if (!singleDescriptionOf(arguments))
        return std::nullopt;
    return parameterFields;
}

Vector<ValueID, 8> ObjectInterpreter::namedArgumentsOf(VirtualObjectID arguments, const ArgumentsParts& parts, std::span<const uint32_t> parameterFields, uint32_t first)
{
    Vector<ValueID, 8> values;
    for (uint32_t field : parameterFields)
        values.append(valueOfField(arguments, field));
    if (parts.restListIsEmpty)
        return values;

    VirtualObjectID list = stateOf(arguments)->objectHeldIn(partOf(arguments, VirtualFieldKind::RestList));
    Vector<ValueID, 8> elements = listedElementsOf(list);
    size_t skipped = first > *parts.numberOfPassedParameters ? first - *parts.numberOfPassedParameters : 0;
    if (skipped < elements.size())
        values.append(elements.subspan(skipped));
    return values;
}

bool ObjectInterpreter::requireCellOfRestList(VirtualObjectID arguments, const BuildCause& cause)
{
    if (!singleDescriptionOf(arguments))
        return false;
    requireCellOfHeldObject(arguments, partOf(arguments, VirtualFieldKind::RestList), cause);
    return !!singleDescriptionOf(arguments);
}

void ObjectInterpreter::executeCallOnArguments(ValueID call)
{
    const Value& value = function().at(call);
    auto isArguments = [&](ValueID operand) {
        const VirtualObjectDescription* description = operand ? singleDescriptionOf(objectOf(operand)) : nullptr;
        return description && description->kind == VirtualObjectKind::Arguments && description->restListStaysAsCreated;
    };
    if (value.opcode == Opcode::NewArgumentList && isArguments(value.arrayLikeOperand()))
        return listArguments(call);
    if (value.opcode == Opcode::CallBuiltin && value.builtinCallForm() == BuiltinCallForm::List)
        return passListedElements(call);
    if (value.opcode != Opcode::CallBuiltin)
        return executeOther(call);
    bool forwardsAnyReceiver = value.builtinCallForm() == BuiltinCallForm::AnyReceiver && forwardsCallOfAnyReceiver(program().builtins().at(value.overload()));
    if (value.builtinCallForm() != BuiltinCallForm::Whole && !forwardsAnyReceiver)
        return executeOther(call);

    BuiltinCall builtinCall = builtinCallOf(program(), value);
    if (std::optional<ForwardedCallPlan> plan = planForwardedCall(program(), function(), builtinCall); plan && isArguments(plan->list))
        return forwardArguments(call, *plan);
    if (value.builtinCallForm() != BuiltinCallForm::Whole)
        return executeOther(call);
    if (program().builtins().at(builtinCall.overload).builtin == Builtins::ArrayPrototypeSlice && isArguments(builtinCall.receiver))
        return sliceArguments(call);
    return executeOther(call);
}

void ObjectInterpreter::forwardArguments(ValueID call, const ForwardedCallPlan& plan)
{
    if (!program().typeOf(function(), call).isAnything())
        return executeOther(call);

    VirtualObjectID arguments = objectOf(plan.list);
    uint32_t numberOfParameters = singleDescriptionOf(arguments)->numberOfParameters;
    ArgumentsParts parts = partsOfArguments(arguments);

    uint32_t listIndex = 0;
    Vector<std::pair<ValueID, uint32_t>, 2> otherOperands;
    function().at(call).forEachOperand([&](ValueID operand, uint32_t index) {
        if (operand == plan.list)
            listIndex = index;
        else
            otherOperands.append({ operand, index });
    });
    BuildCause cause = causeOf(call, listIndex);
    for (auto [operand, index] : otherOperands)
        requireCell(operand, causeOf(call, index));

    std::optional<Vector<uint32_t, 8>> parameterFields;
    bool passesRestList = false;
    if (!parts.firstFrameArgument) {
        parameterFields = requireCellsOfNamedArguments(arguments, parts, 0, cause);
        passesRestList = !parameterFields && !numberOfParameters && requireCellOfRestList(arguments, cause);
        if (!parameterFields && !passesRestList) {
            requireCell(plan.list, cause);
            return executeOther(call);
        }
    }

    if (isRewriting()) {
        cursor().setOrigin(function().at(call).origin());
        Vector<ValueID, 8> values;
        if (parameterFields)
            values = namedArgumentsOf(arguments, parts, parameterFields->span(), 0);
        ValueID restList = passesRestList ? valueOfField(arguments, partOf(arguments, VirtualFieldKind::RestList)) : ValueID();
        ValueID thisValue = parts.firstFrameArgument && !plan.thisValue ? emitUndefined() : plan.thisValue;

        Value& forwarded = cursor().builder().function().edit(call);
        becomeForwardedCall(forwarded, plan);
        if (parts.firstFrameArgument)
            becomeForwardingCall(forwarded, thisValue, *parts.firstFrameArgument);
        else if (restList)
            forwarded.setListOperand(restList);
        else
            convertListCall(forwarded, values.span());
    }
    forgetFieldsWrittenBy(call);
    keep(call);
}

bool ObjectInterpreter::tryCopyArguments(ValueID copy, VirtualObjectID arguments, uint32_t first, const BuildCause& cause)
{
    ArgumentsParts parts = partsOfArguments(arguments);
    if (parts.firstFrameArgument) {
        if (static_cast<uint64_t>(*parts.firstFrameArgument) + first >= maxArguments)
            return false;
        if (isRewriting()) {
            Value rest = IRBuilder::make(Opcode::CreateRest);
            rest.setFirstArgument(*parts.firstFrameArgument + first);
            replaceRead(copy, emitReplacement(m_sinking, *this, copy, WTF::move(rest)));
        }
        return true;
    }

    std::optional<Vector<uint32_t, 8>> parameterFields = requireCellsOfNamedArguments(arguments, parts, first, cause);
    if (!parameterFields)
        return false;
    if (isRewriting()) {
        Vector<ValueID, 8> values = namedArgumentsOf(arguments, parts, parameterFields->span(), first);
        Value array = IRBuilder::make(Opcode::NewArray);
        array.setContents(TypeTable::anything);
        array.setElementsOperands(values.span());
        replaceRead(copy, emitReplacement(m_sinking, *this, copy, WTF::move(array)));
    }
    return true;
}

void ObjectInterpreter::listArguments(ValueID list)
{
    ValueID receiver = function().at(list).arrayLikeOperand();
    VirtualObjectID arguments = objectOf(receiver);
    BuildCause cause = causeOf(list, OperandIndex::NewArgumentList::arrayLike);
    requireCell(function().at(list).calleeOperand(), causeOf(list, OperandIndex::NewArgumentList::callee));
    if (singleDescriptionOf(arguments) && tryCopyArguments(list, arguments, 0, cause))
        return;

    if (!singleDescriptionOf(arguments) || singleDescriptionOf(arguments)->numberOfParameters || !requireCellOfRestList(arguments, cause)) {
        requireCell(receiver, cause);
        return executeOther(list);
    }
    if (isRewriting())
        replaceRead(list, valueOfField(arguments, partOf(arguments, VirtualFieldKind::RestList)));
}

void ObjectInterpreter::sliceArguments(ValueID call)
{
    BuiltinCall slice = builtinCallOf(program(), function().at(call));
    auto isOmitted = [&](size_t index) {
        return index >= slice.arguments.size() || program().typeOf(function(), slice.arguments[index]).hasOnlyPrimitives(Primitive::Undefined);
    };
    std::optional<int64_t> start = isOmitted(0) ? std::optional<int64_t>(0) : integerConstantThroughCasts(program(), function(), slice.arguments[0]);
    const TypeID contents[] { TypeTable::anything };
    bool fitsResult = Type::builtinClass(BuiltinClasses::Array, contents).isSubsetOf(program().typeOf(function(), call), program());
    if (!start || *start < 0 || *start >= static_cast<int64_t>(maxArguments) || !isOmitted(1) || !fitsResult)
        return executeOther(call);

    uint32_t first = static_cast<uint32_t>(*start);
    ValueID receiver = slice.receiver;
    VirtualObjectID arguments = objectOf(receiver);
    uint32_t numberOfParameters = singleDescriptionOf(arguments)->numberOfParameters;
    BuildCause cause = causeOf(call, OperandIndex::CallBuiltin::receiver);
    if (tryCopyArguments(call, arguments, first, cause))
        return;
    if (partsOfArguments(arguments).firstFrameArgument)
        return executeOther(call);

    if (first < numberOfParameters || !requireCellOfRestList(arguments, cause)) {
        requireCell(receiver, cause);
        return executeOther(call);
    }
    if (isRewriting()) {
        cursor().setOrigin(function().at(call).origin());
        ValueID restList = valueOfField(arguments, partOf(arguments, VirtualFieldKind::RestList));
        ValueID firstInList = cursor().builder().appendInteger(first - numberOfParameters);
        Value& sliceOfList = cursor().builder().function().edit(call);
        sliceOfList.setReceiverOperand(restList);
        sliceOfList.setArgumentsOperands(std::array { firstInList });
    }
    forgetFieldsWrittenBy(call);
    keep(call);
}

void ObjectInterpreter::passListedElements(ValueID call)
{
    BuiltinCall builtinCall = builtinCallOf(program(), function().at(call));
    BuiltinOverloadID overload = builtinCall.overload;
    ValueID listValue = builtinCall.arguments[0];
    VirtualObjectID list = objectOf(listValue);
    if (!hasListedElements(list))
        return executeOther(call);

    uint32_t listIndex = 0;
    function().at(call).forEachOperand([&](ValueID operand, uint32_t index) {
        if (operand == listValue)
            listIndex = index;
    });
    requireCellsOfListedElements(list, causeOf(call, listIndex));
    if (!hasListedElements(list))
        return executeOther(call);

    uint32_t length = singleShapeOf(list).length;
    const Type& elementType = program().at(singleDescriptionOf(list)->elementType);
    bool fitsParameters = true;
    for (uint32_t position = 0; position < length && fitsParameters; ++position)
        fitsParameters = findListedElement(list, position) && elementType.isSubsetOf(parameterTypeOfOverload(program(), overload, BuiltinOperand::argument(position)), program());

    std::optional<Value> callee = tryMakeBuiltinConstant(program().builtins(), program().builtins().at(overload).builtin);
    bool handlerHasMetCurrentState = m_sinking.inputs().throwingValues.get(call.index());
    if (!fitsParameters && (!callee || !handlerHasMetCurrentState))
        return executeOther(call);

    if (isRewriting()) {
        cursor().setOrigin(function().at(call).origin());
        Vector<ValueID, 8> elements = listedElementsOf(list);
        Value original = function().at(call);
        if (fitsParameters) {
            Value whole = original;
            whole.setBuiltinCallForm(BuiltinCallForm::Whole);
            whole.setNeedsPristineReceiver(false);
            whole.setArgumentsOperands(elements.span());
            replaceRead(call, emitReplacement(m_sinking, *this, call, WTF::move(whole)));
        } else {
            Value ordinary = IRBuilder::make(Opcode::Call);
            ordinary.setCalleeOperand(cursor().emit(WTF::move(*callee)));
            ordinary.setArgumentsOperands(elements.span());
            ordinary.setAccessSite(original.accessSite());
            ordinary.isTailCall = original.isTailCall;
            ValueID result = emitReplacement(m_sinking, *this, call, WTF::move(ordinary));
            if (m_sinking.context().build.validates())
                cursor().emit(makeValidateType(result, original.type));
            replaceRead(call, cursor().emit(makeAssumeType(result, Proven::byRulesWithoutChecks(original.type))));
        }
    }
    forgetFieldsWrittenBy(call);
}

void ObjectInterpreter::destructureArgumentList(ValueID destructuring, VirtualObjectID object)
{
    uint32_t count = function().at(destructuring).count();
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    bool isArgumentList = description && description->kind == VirtualObjectKind::ArgumentList;
    if (!isArgumentList || static_cast<uint64_t>(description->firstArgument) + count > maxArguments)
        return executeOnCell(destructuring, OperandIndex::DestructureIterable::iterable);

    bind(destructuring, object, AliasKind::Results);
}

std::optional<bool> ObjectInterpreter::isArgumentPassed(VirtualObjectID arguments, uint32_t index) const
{
    const VirtualObjectDescription* description = singleDescriptionOf(arguments);
    if (!description || description->kind != VirtualObjectKind::Arguments)
        return std::nullopt;

    const Type& count = program().typeOf(function(), function().at(description->allocation).countOperand());
    if (!count.isInteger())
        return std::nullopt;
    if (count.integers.minimum > static_cast<int64_t>(index))
        return true;
    if (count.integers.maximum <= static_cast<int64_t>(index))
        return false;
    return std::nullopt;
}

struct DeliveredField {
    uint32_t resultIndex { 0 };
    uint32_t field { 0 };
    bool isDeliveredAsBounds { false };
};

static uint64_t numberOfElementsInUse(const VirtualFamily& family)
{
    uint64_t numberOfElements = 0;
    for (uint32_t field = 0; field < family.numberOfFields(); ++field) {
        const VirtualField& virtualField = family.fieldAt(field);
        if (virtualField.kind == VirtualFieldKind::Element)
            numberOfElements = std::max(numberOfElements, static_cast<uint64_t>(virtualField.index) + 1);
    }
    return numberOfElements;
}

static const VirtualShape* findDeliveredShape(const VirtualFamily& family, uint32_t descriptionIndex)
{
    for (uint32_t index = 0; index < family.numberOfShapes(); ++index) {
        const VirtualShape& shape = family.at(VirtualShapeID(index));
        if (shape.kind == VirtualShapeKind::Fields && shape.descriptionIndex == descriptionIndex)
            return &shape;
    }
    return nullptr;
}

static bool deliversEveryField(const Program& program, LayoutID layout, uint32_t numberOfParts)
{
    BuiltinLayoutID builtinLayout = program.at(layout).builtinLayout;
    return builtinLayout && program.builtins().at(builtinLayout).fields.size() == numberOfParts;
}

static void retypeProducer(const Program& program, IRBuilder& builder, ValueID producer)
{
    Function& function = builder.function();
    Value& value = function.edit(producer);
    value.type = resultTypeOf(program, function, value, builder.types());
    value.setExpected(ExpectedType(expectedTypeOf(program, function, value, builder.types())));
}

static ValueID emitResult(IRBuilder& builder, ValueID producer, uint32_t resultIndex)
{
    Value result = IRBuilder::make(Opcode::ResultAt, { producer });
    result.setIndex(resultIndex);
    return builder.append(WTF::move(result));
}

static ValueID emitSubstringInBounds(IRBuilder& builder, ValueID string, ValueID producer, uint32_t resultIndexOfStart)
{
    ValueID start = emitResult(builder, producer, resultIndexOfStart);
    ValueID end = emitResult(builder, producer, resultIndexOfStart + 1);
    return builder.append(Opcode::SubstringOrUndefined, { string, start, end });
}

ValueID ObjectInterpreter::emitPresenceTag(ValueID isPresent, uint8_t tagBitWhenAbsent)
{
    IRBuilder& builder = cursor().builder();
    ValueID tagWhenAbsent = builder.appendInteger(int64_t(1) << tagBitWhenAbsent);
    ValueID shift = builder.append(Opcode::ToNumber, { isPresent });
    return builder.append(Opcode::LShift, { tagWhenAbsent, shift });
}

void ObjectInterpreter::executeBuiltinResult(ValueID call, VirtualObjectID object)
{
    auto leaveWhole = [&](LimitID limit = LimitID::None) {
        m_sinking.abandon(object, limit);
        return executeOther(call);
    };

    if (stateOf(object))
        return leaveWhole();

    const VirtualObjectDescription& description = m_sinking.descriptionOf(object);
    if (description.isDead)
        return;

    VirtualFamily& family = m_sinking.familyOf(object);
    bool isPrefix = description.kind == VirtualObjectKind::BuiltinPrefix;
    RELEASE_ASSERT(isPrefix || description.kind == VirtualObjectKind::BuiltinFields);
    if (isPrefix && numberOfElementsInUse(family) > m_sinking.limit(LimitID::MaxVirtualArrayLength))
        return leaveWhole(LimitID::MaxVirtualArrayLength);

    BuiltinCallForm form = isPrefix ? BuiltinCallForm::Prefix : BuiltinCallForm::Fields;
    uint32_t prefixLength = isPrefix ? prefixLengthNeededBy(object) : 0;
    const BuiltinOverload& overload = program().builtins().at(function().at(call).overload());
    std::optional<BuiltinResultIndexes> indexes = resultIndexesOf(program().builtins(), function().at(call).overload(), form, prefixLength);
    RELEASE_ASSERT(indexes);
    if (description.absentResult && !indexes->presence)
        return leaveWhole();
    if (!isPrefix && !deliversEveryField(program(), description.layout, indexes->numberOfParts))
        return leaveWhole();

    VirtualShape present;
    present.kind = VirtualShapeKind::Fields;
    present.descriptionIndex = m_sinking.at(object).descriptionIndex;
    present.length = prefixLength;
    Vector<DeliveredField, 8> deliveredFields;
    auto deliver = [&](uint32_t resultIndex, const VirtualField& virtualField, bool isDeliveredAsBounds = false) {
        std::optional<uint32_t> field = fieldFor(object, virtualField);
        if (!field)
            return false;
        present.stored.set(*field);
        if (description.hasKeyOrder && virtualField.isProperty())
            present.order = m_sinking.orders().append(present.order, *field);
        deliveredFields.append(DeliveredField { resultIndex, *field, isDeliveredAsBounds });
        return true;
    };

    bool hasEveryField = !indexes->length || deliver(*indexes->length, VirtualField::part(VirtualFieldKind::Length));
    for (uint32_t property = 0; hasEveryField && property < indexes->numberOfProperties; ++property)
        hasEveryField = deliver(indexes->firstProperty + property, VirtualField::named(program().keyOf(overload.code.prefixProperties[property].name)));
    for (uint32_t part = 0; hasEveryField && part < indexes->numberOfParts; ++part)
        hasEveryField = deliver(indexes->resultOfPart(part), isPrefix ? VirtualField::element(part) : fieldOfLayoutField(program(), description.layout, part), indexes->partsAreBounds);
    if (!hasEveryField)
        return leaveWhole(LimitID::MaxVirtualObjectFields);

    std::optional<uint8_t> tagBitWhenAbsent = m_sinking.at(object).tagBitWhenAbsent;
    ObjectState state = stateAtAllocation(object, family.intern(WTF::move(present)));

    requireCellsOfOperands(call, form);
    forgetFieldsWrittenBy(call);
    if (!isRewriting()) {
        m_state.add(object, WTF::move(state));
        return;
    }

    IRBuilder& builder = cursor().builder();
    builder.function().edit(call, ValueEdit::Definition).setBuiltinCallForm(form);
    if (isPrefix)
        builder.function().edit(call, ValueEdit::Definition).setPrefixLength(prefixLength);
    keep(call);
    retypeProducer(program(), builder, call);
    m_state.add(object, WTF::move(state));

    ValueID rangedString = indexes->partsAreBounds ? builtinCallOf(program(), builder.function().at(call)).operandAt(*overload.result.substringOf) : ValueID();
    ObjectValues& carried = values().ensure(object);
    if (tagBitWhenAbsent)
        carried.tag = emitPresenceTag(emitResult(builder, call, *indexes->presence), *tagBitWhenAbsent);
    for (const DeliveredField& delivered : deliveredFields)
        carried.setField(delivered.field, delivered.isDeliveredAsBounds ? emitSubstringInBounds(builder, rangedString, call, delivered.resultIndex) : emitResult(builder, call, delivered.resultIndex));
}

uint32_t ObjectInterpreter::prefixLengthNeededBy(VirtualObjectID object) const
{
    const VirtualFamily& family = m_sinking.familyOf(object);
    if (const VirtualShape* delivered = findDeliveredShape(family, m_sinking.at(object).descriptionIndex))
        return delivered->length;
    return static_cast<uint32_t>(std::min(numberOfElementsInUse(family), m_sinking.limit(LimitID::MaxVirtualArrayLength)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
