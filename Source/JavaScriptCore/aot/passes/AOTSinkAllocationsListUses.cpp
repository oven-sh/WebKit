/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::AppendSpread::array == OperandIndex::AppendElements::array);
static_assert(OperandIndex::AppendSpread::iterable == OperandIndex::AppendElements::source);
static_assert(OperandIndex::CallList::list == OperandIndex::ConstructList::list);
static_assert(OperandIndex::CallList::list == OperandIndex::SuperConstructList::list);

void convertListCall(Value& call, std::span<const ValueID> elements)
{
    Value listCall = call;
    switch (listCall.opcode) {
    case Opcode::CallList:
        call.become(Opcode::Call);
        call.setCalleeOperand(listCall.calleeOperand());
        call.setThisValueOperand(listCall.thisValueOperand());
        call.setCalleeKey(listCall.calleeKey());
        call.setCalleeName(listCall.calleeName());
        call.setExpectedResult(listCall.expectedResult());
        call.setIsDemotedTailCall(listCall.isDemotedTailCall());
        break;
    case Opcode::ConstructList:
        call.become(Opcode::Construct);
        call.setCalleeOperand(listCall.calleeOperand());
        call.setNewTargetOperand(listCall.newTargetOperand());
        call.setCalleeName(listCall.calleeName());
        break;
    case Opcode::SuperConstructList:
        call.become(Opcode::SuperConstruct);
        call.setLayout(listCall.layout());
        call.setParentOperand(listCall.parentOperand());
        call.setNewTargetOperand(listCall.newTargetOperand());
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    call.setAccessSite(listCall.accessSite());
    call.setCallees(listCall.callees());
    call.setArgumentsOperands(elements);
}

Execution ObjectInterpreter::executeListUse(ValueID id)
{
    switch (function().at(id).opcode) {
    case Opcode::AppendSpread:
    case Opcode::AppendElements:
        return executeAppendElements(id);
    case Opcode::CopyDataProperties:
        return executeCopyDataProperties(id);
    case Opcode::CallList:
    case Opcode::ConstructList:
    case Opcode::SuperConstructList:
        return executeCallWithList(id);
    case Opcode::DestructureIterable:
        return executeDestructuring(id);
    case Opcode::ResultAt:
        return executeResultRead(id);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

const VirtualObjectDescription* ObjectInterpreter::singleDescriptionOf(VirtualObjectID object) const
{
    const ObjectState* state = stateOf(object);
    if (!state || state->variants.size() != 1)
        return nullptr;
    const VirtualShape& shape = shapeOf(object, state->variants[0]);
    if (shape.kind != VirtualShapeKind::Fields)
        return nullptr;
    return &m_sinking.familyOf(object).descriptionOf(shape);
}

bool ObjectInterpreter::hasListedElements(VirtualObjectID object) const
{
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    return description && description->kind == VirtualObjectKind::Array;
}

std::optional<uint32_t> ObjectInterpreter::findListedElement(VirtualObjectID object, uint32_t position) const
{
    const VirtualShape& shape = singleShapeOf(object);
    if (position >= shape.length)
        return std::nullopt;
    std::optional<uint32_t> field = m_sinking.familyOf(object).findField(VirtualField::element(position));
    if (!field || !hasStored(shape, *field))
        return std::nullopt;
    return field;
}

Vector<ValueID, 8> ObjectInterpreter::listedElementsOf(VirtualObjectID object)
{
    RELEASE_ASSERT(isRewriting() && hasListedElements(object));
    uint32_t length = singleShapeOf(object).length;
    Vector<ValueID, 8> elements;
    elements.reserveInitialCapacity(length);
    ValueID valueOfHoles;
    for (uint32_t position = 0; position < length; ++position) {
        if (std::optional<uint32_t> field = findListedElement(object, position)) {
            elements.append(valueOfField(object, *field));
            continue;
        }
        if (!valueOfHoles)
            valueOfHoles = emitUndefined();
        elements.append(valueOfHoles);
    }
    return elements;
}

void ObjectInterpreter::requireCellOfHeldObject(VirtualObjectID holder, uint32_t field, const BuildCause& cause)
{
    VirtualObjectID held = stateOf(holder)->objectHeldIn(field);
    if (!held)
        return;

    build(held, cause);
    ObjectState& state = *stateOf(holder);
    if (state.isCell())
        return;

    state.release(field);
    if (isRewriting()) {
        ValueID cell = cellOf(held);
        values().ensure(holder).setField(field, cell);
    }
    stateDidChange(holder);
}

void ObjectInterpreter::requireCellsOfListedElements(VirtualObjectID object, const BuildCause& cause)
{
    Vector<HeldObject, 1> heldObjects = stateOf(object)->heldObjects;
    for (const HeldObject& held : heldObjects)
        requireCellOfHeldObject(object, held.field, cause);
    if (!hasListedElements(object))
        m_sinking.abandon(object);
}

bool ObjectInterpreter::canAppendListedElements(VirtualObjectID target, VirtualObjectID source)
{
    const ObjectState* state = stateOf(target);
    if (!state)
        return false;

    uint32_t length = singleShapeOf(source).length;
    uint32_t numberOfHoles = 0;
    for (uint32_t position = 0; position < length; ++position)
        numberOfHoles += !findListedElement(source, position);

    uint32_t targetLength = shapeOf(target, state->variants[0]).length;
    const Type& listedType = program().at(singleDescriptionOf(source)->elementType);
    const Type& typeOfHoles = program().at(TypeTable::undefined);
    bool admitsElements = std::ranges::all_of(state->variants, [&](const ObjectVariant& variant) {
        const VirtualShape& shape = shapeOf(target, variant);
        if (shape.kind != VirtualShapeKind::Fields || shape.length != targetLength)
            return false;
        const VirtualObjectDescription& description = m_sinking.familyOf(target).descriptionOf(shape);
        if (description.kind != VirtualObjectKind::Array)
            return false;
        const Type& elementType = program().at(description.elementType);
        if (numberOfHoles < length && !listedType.isSubsetOf(elementType, program()))
            return false;
        return !numberOfHoles || typeOfHoles.isSubsetOf(elementType, program());
    });
    if (!admitsElements)
        return false;

    uint64_t newLength = static_cast<uint64_t>(targetLength) + length;
    bool exceedsLength = m_mode == SinkingMode::Count
        ? m_sinking.exceeds(LimitID::MaxVirtualArrayLength, newLength, m_sinking.at(target).definition)
        : newLength > m_sinking.limit(LimitID::MaxVirtualArrayLength);
    if (exceedsLength)
        return false;

    for (uint32_t position = 0; position < length; ++position) {
        if (!fieldFor(target, VirtualField::element(targetLength + position)))
            return false;
    }
    return true;
}

Execution ObjectInterpreter::executeAppendElements(ValueID id)
{
    constexpr uint32_t arrayIndex = OperandIndex::AppendElements::array;
    constexpr uint32_t sourceIndex = OperandIndex::AppendElements::source;

    ValueID array = function().at(id).operand(arrayIndex);
    VirtualObjectID target = objectOf(array);
    VirtualObjectID source = objectOf(function().at(id).operand(sourceIndex));
    if (source == target || !hasListedElements(source))
        return executeOther(id);

    requireCellsOfListedElements(source, causeOf(id, sourceIndex));
    bool targetIsVirtual = hasListedElements(source) && canAppendListedElements(target, source);
    if (targetIsVirtual && m_state.isDestructured(target)) {
        m_sinking.abandon(target);
        targetIsVirtual = false;
    }
    if (!targetIsVirtual)
        requireCell(array, causeOf(id, arrayIndex));
    if (!hasListedElements(source))
        return executeOther(id);

    uint32_t length = singleShapeOf(source).length;
    Vector<ValueID, 8> elements;
    if (isRewriting()) {
        cursor().setOrigin(function().at(id).origin());
        elements = listedElementsOf(source);
    }

    if (targetIsVirtual) {
        for (uint32_t position = 0; position < length; ++position)
            appendElement(target, isRewriting() ? elements[position] : id);
        if (m_mode == SinkingMode::Count)
            ++m_sinking.at(target).numberOfFoldedStores;
        stateDidChange(target);
        return Execution::Interpreted;
    }

    forgetFieldsWrittenBy(id);
    if (isRewriting() && length) {
        cursor().willKeepThrowingValue(*this);
        ValueID cell = resolve(array);
        for (ValueID element : elements)
            cursor().emit(IRBuilder::make(Opcode::AppendElement, { cell, element }));
    }
    return Execution::Interpreted;
}

std::optional<Vector<CopiedField, 8>> ObjectInterpreter::collectCopiedFields(ValueID copy, VirtualObjectID source) const
{
    const Value& value = function().at(copy);
    if (!value.excludedOperands().empty())
        return std::nullopt;

    const VirtualObjectDescription* description = singleDescriptionOf(source);
    if (!description || !description->hasKeyOrder)
        return std::nullopt;
    if (description->kind != VirtualObjectKind::Object && description->kind != VirtualObjectKind::BuiltinFields)
        return std::nullopt;

    const VirtualFamily& family = m_sinking.familyOf(source);
    std::span<const PropertyKeyID> excludedKeys = value.excludedKeys();
    Vector<CopiedField, 8> fields;
    if (hasUnprovenField(family, singleShapeOf(source)))
        return std::nullopt;
    for (uint32_t field : m_sinking.orders().fieldsOf(singleShapeOf(source).order)) {
        const VirtualField& property = family.fieldAt(field);
        if (property.kind != VirtualFieldKind::Named)
            return std::nullopt;
        if (std::ranges::contains(excludedKeys, property.key))
            continue;
        if (!isEnumerable(program(), *description, property))
            return std::nullopt;
        fields.append(CopiedField { property.key, field, 0 });
    }
    return fields;
}

bool ObjectInterpreter::tryResolveTargetFields(VirtualObjectID target, const VirtualObjectDescription& source, std::span<CopiedField> fields)
{
    const ObjectState* state = stateOf(target);
    if (!state)
        return false;

    Vector<const VirtualObjectDescription*, 2> descriptions;
    for (const ObjectVariant& variant : state->variants) {
        const VirtualShape& shape = shapeOf(target, variant);
        if (shape.kind != VirtualShapeKind::Fields)
            return false;
        const VirtualObjectDescription& description = m_sinking.familyOf(target).descriptionOf(shape);
        if (description.kind != VirtualObjectKind::Object)
            return false;
        descriptions.append(&description);
    }

    const VirtualObjectContext& context = m_sinking.objectContext();
    for (CopiedField& field : fields) {
        VirtualField property = VirtualField::named(field.key);
        const Type& copiedType = program().at(typeOfField(context, source, property));
        bool isAdmitted = std::ranges::all_of(descriptions, [&](const VirtualObjectDescription* description) {
            return layoutFieldOf(program(), *description, property)
                && isEnumerable(program(), *description, property)
                && copiedType.isSubsetOf(program().at(typeOfField(context, *description, property)), program());
        });
        if (!isAdmitted)
            return false;
        std::optional<uint32_t> targetField = fieldFor(target, property);
        if (!targetField)
            return false;
        field.targetField = *targetField;
    }
    return true;
}

Execution ObjectInterpreter::executeCopyDataProperties(ValueID id)
{
    ValueID targetValue = function().at(id).targetOperand();
    VirtualObjectID target = objectOf(targetValue);
    VirtualObjectID source = objectOf(function().at(id).sourceOperand());
    if (source == target)
        return executeOther(id);
    std::optional<Vector<CopiedField, 8>> fields = collectCopiedFields(id, source);
    if (!fields)
        return executeOther(id);

    bool targetIsVirtual = tryResolveTargetFields(target, *singleDescriptionOf(source), fields->mutableSpan());
    BuildCause cause = causeOf(id, OperandIndex::CopyDataProperties::source);
    for (const CopiedField& field : *fields)
        requireCellOfHeldObject(source, field.sourceField, cause);
    targetIsVirtual = targetIsVirtual && stateOf(target)->isVirtual();
    if (!targetIsVirtual)
        requireCell(targetValue, causeOf(id, OperandIndex::CopyDataProperties::target));
    if (!singleDescriptionOf(source))
        return executeOther(id);

    if (targetIsVirtual) {
        for (const CopiedField& field : *fields)
            storeField(target, field.targetField, isRewriting() ? valueOfField(source, field.sourceField) : id);
        if (m_mode == SinkingMode::Count)
            m_sinking.at(target).numberOfFoldedStores += static_cast<unsigned>(fields->size());
        stateDidChange(target);
        return Execution::Interpreted;
    }

    forgetFieldsWrittenBy(id);
    if (isRewriting() && !fields->isEmpty()) {
        cursor().setOrigin(function().at(id).origin());
        cursor().willKeepThrowingValue(*this);
        ValueID cell = resolve(targetValue);
        for (const CopiedField& field : *fields) {
            Value define = IRBuilder::make(Opcode::PropDefine, { cell, valueOfField(source, field.sourceField) });
            define.setKey(field.key);
            cursor().emit(WTF::move(define));
        }
    }
    return Execution::Interpreted;
}

Execution ObjectInterpreter::executeCallWithList(ValueID id)
{
    constexpr uint32_t listIndex = OperandIndex::CallList::list;

    VirtualObjectID list = objectOf(function().at(id).operand(listIndex));
    const VirtualObjectDescription* description = singleDescriptionOf(list);
    if (description && description->kind == VirtualObjectKind::ArgumentList)
        return forwardArgumentList(id, list);
    if (!hasListedElements(list))
        return executeOther(id);

    requireCellsOfListedElements(list, causeOf(id, listIndex));
    Vector<std::pair<ValueID, uint32_t>, 2> otherOperands;
    function().at(id).forEachOperand([&](ValueID operand, uint32_t index) {
        if (index != listIndex)
            otherOperands.append({ operand, index });
    });
    for (auto [operand, index] : otherOperands)
        requireCell(operand, causeOf(id, index));
    if (!hasListedElements(list))
        return executeOther(id);

    if (isRewriting()) {
        cursor().setOrigin(function().at(id).origin());
        Vector<ValueID, 8> elements = listedElementsOf(list);
        convertListCall(cursor().builder().function().edit(id), elements.span());
    }
    forgetFieldsWrittenBy(id);
    keep(id);
    return Execution::Kept;
}

Execution ObjectInterpreter::executeDestructuring(ValueID id)
{
    VirtualObjectID object = objectOf(function().at(id).iterableOperand());
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    if (description && description->kind == VirtualObjectKind::ArgumentList)
        return destructureArgumentList(id, object);
    bool isWithinPrefix = description && description->kind == VirtualObjectKind::BuiltinPrefix && function().at(id).count() <= singleShapeOf(object).length;
    if (!hasListedElements(object) && !isWithinPrefix)
        return executeOther(id);

    bind(id, object, AliasKind::Results);
    return Execution::Interpreted;
}

Execution ObjectInterpreter::abandonDestructuredRead(ValueID read, VirtualObjectID object)
{
    m_sinking.abandon(object);
    if (isRewriting())
        replaceRead(read, emitUndefined());
    return Execution::Interpreted;
}

Execution ObjectInterpreter::executeResultRead(ValueID id)
{
    VirtualObjectID object = destructuredObjectOf(function().at(id).resultsOperand());
    if (!object)
        return executeOther(id);

    uint32_t index = function().at(id).index();
    const VirtualObjectDescription* description = singleDescriptionOf(object);
    if (description && description->kind == VirtualObjectKind::ArgumentList) {
        if (!program().at(TypeTable::anything).isSubsetOf(program().typeOf(function(), id), program()))
            return abandonDestructuredRead(id, object);
        if (isRewriting()) {
            Value argument(Opcode::GetArgument);
            argument.setArgumentIndex(description->firstArgument + index);
            replaceRead(id, cursor().emit(WTF::move(argument)));
        }
        return Execution::Interpreted;
    }
    bool isPrefix = description && description->kind == VirtualObjectKind::BuiltinPrefix;
    if (!hasListedElements(object) && !isPrefix)
        return abandonDestructuredRead(id, object);

    std::optional<uint32_t> field = findListedElement(object, index);
    if (isPrefix && !field)
        return abandonDestructuredRead(id, object);
    VirtualObjectID held = field ? stateOf(object)->objectHeldIn(*field) : VirtualObjectID();
    FieldRead read = !field ? FieldRead::Undefined : held ? FieldRead::HeldObject : FieldRead::Value;
    if (!fitsRead(id, object, field, read))
        return abandonDestructuredRead(id, object);

    if (held) {
        bind(id, held);
        return Execution::Interpreted;
    }
    if (isRewriting())
        replaceRead(id, field ? valueOfField(object, *field) : emitUndefined());
    return Execution::Interpreted;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
