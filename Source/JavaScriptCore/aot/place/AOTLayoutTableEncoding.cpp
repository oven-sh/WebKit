/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTLayoutData.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTObjectVisiting.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::FieldsInKeyOrder, hostArchitecture()) == maxFieldsInOrder);

namespace {

struct ReferenceOffsets {
    Vector<uint32_t> wideValues;
    Vector<uint32_t> wideCells;
    Vector<uint32_t> narrow;
    Vector<uint32_t> wideOutOfLine;
    Vector<uint32_t> narrowOutOfLine;
};

struct ObjectLayoutParts {
    Vector<FieldData> fields;
    Vector<uint32_t> fieldOperands;
    uint32_t numberOfOrders { 0 };
    uint32_t ordersOffset { 0 };
    uint32_t referencesOffset { 0 };
    bool hasExtension { false };
    bool hasExtraPropertiesSlot { false };
    bool hasFieldsWithSymbolKeys { false };
    bool hasOnlyPlainSlots { true };
    bool isCopiedByWords { true };
};

} // anonymous namespace

static int32_t distanceFromRecord(uint32_t recordOffset, uint32_t targetOffset)
{
    return targetOffset ? static_cast<int32_t>(targetOffset) - static_cast<int32_t>(recordOffset) : 0;
}

template<typename Record>
static uint32_t writeRecords(TableWriter& writer, uint32_t offset, const Vector<Record>& records)
{
    for (const Record& record : records) {
        writer.at<Record>(offset) = record;
        offset += sizeof(Record);
    }
    return offset;
}

static bool isDescribedVariable(TableEncoder& encoder, LayoutID id, FieldIndex index)
{
    const PlacedSlot& slot = encoder.slots.slot(id, index);
    if (!slot.hasStorage())
        return false;
    SlotTraits traits = traitsOf(slot.representation);
    return traits.holdsCell || (traits.hasAbsent && traits.absentBits);
}

static Vector<FieldIndex> collectDescribedVariables(TableEncoder& encoder, LayoutID id)
{
    Vector<FieldIndex> described;
    unsigned numberOfFields = encoder.slots.layout(id).numberOfFields();
    for (FieldIndex index = 0; index < numberOfFields; ++index) {
        if (isDescribedVariable(encoder, id, index))
            described.append(index);
    }
    return described;
}

static uint16_t fieldFlagsOf(TableEncoder& encoder, FieldReference reference)
{
    const Program& program = encoder.program;
    const Field& declaration = fieldAt(program, declarationOf(program, reference));
    const LocationFlow& location = flowOf(program, reference).location;
    OptionSet<FieldStability> stability = flowOf(program, reference).stability;
    const PlacedSlot& slot = encoder.slots.slot(reference);

    uint16_t flags = 0;
    if (!declaration.flags.contains(FieldFlags::IsRequired))
        flags |= fieldIsOptional;
    if (slot.isOutOfLine())
        flags |= fieldIsOutOfLine;
    // Object::tryStore(): a slot without an absent state holds a placeholder until its only store, which may come by name: such a store is like any other.
    if (location.stores == StoreCount::AtMostOnce && canLeaveSlot(slot.representation))
        flags |= fieldIsStoredOnce;
    if (slot.mayExistWithoutStorage)
        flags |= fieldMayExistWithoutStorage;
    if (slot.deferredClosureCapturesObject)
        flags |= fieldDeferredClosureCapturesObject;
    if (slot.deferredClosureCapturesEnvironment)
        flags |= fieldDeferredClosureCapturesEnvironment;
    bool isHiddenWithoutKey = declaration.flags.contains(FieldFlags::IsHidden) && !declaration.key;
    if (declaration.flags.contains(FieldFlags::IsPrivate) || isHiddenWithoutKey)
        flags |= fieldIsPrivate;
    if (declaration.flags.contains(FieldFlags::IsBrand))
        flags |= fieldIsBrand;
    if (stability.contains(FieldStability::IsNeverDeleted))
        flags |= fieldIsNeverDeleted;
    if (stability.contains(FieldStability::IsNeverRedefined))
        flags |= fieldIsNeverRedefined;
    if (stability.contains(FieldStability::IsNeverStoredWithOtherType))
        flags |= fieldIsNeverStoredWithOtherType;
    if (declaration.form.contains(FieldForm::IsReadOnly))
        flags |= fieldIsReadOnly;
    if (declaration.form.contains(FieldForm::IsNotEnumerable))
        flags |= fieldIsNotEnumerable;
    if (declaration.form.contains(FieldForm::IsNotConfigurable))
        flags |= fieldIsNotConfigurable;
    if (declaration.form.contains(FieldForm::IsAccessor))
        flags |= fieldIsAccessor;
    return flags;
}

static std::optional<uint32_t> fieldOperandOf(TableEncoder& encoder, const PlacedSlot& slot)
{
    switch (traitsOf(slot.representation).valueKind) {
    case SlotValueKind::DeferredClosure: {
        // Without a generic entrypoint no body creates a closure of the function, so no object with this field is created either.
        FunctionID entrypoint = encoder.findEntrypoint(slot.deferredFunction);
        return entrypoint ? entrypoint.index() : UINT32_MAX;
    }
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
    case SlotValueKind::Float:
    case SlotValueKind::Double:
    case SlotValueKind::Boolean:
    case SlotValueKind::Boxed:
    case SlotValueKind::Cell:
    case SlotValueKind::LiteralString:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void appendOperandsOfFlattenedObject(TableEncoder& encoder, FieldReference reference, Vector<uint32_t>& operands)
{
    const Vector<FlattenedObject>& flattenedObjects = encoder.program.at(reference.layout).flattenedObjects;
    size_t position = flattenedObjects.findIf([&](const FlattenedObject& flattened) {
        return flattened.field == reference.field;
    });
    RELEASE_ASSERT(position != notFound);
    const FlattenedObject& flattened = flattenedObjects[position];
    const PlacedFlattenedObject* placed = encoder.slots.layout(reference.layout).flattenedObjectIn(reference.field);
    AllocationID allocation = encoder.placement.allocations().find(flattened.layout, encoder.placement.shapes().shapeOf(flattened.layout, flattened.allocationClass), flattened.fields.span());
    RELEASE_ASSERT(placed);
    // The allocations that lay an object into the room have the number. Where the code has none of them any more, no slot says that an object lies there.
    uint32_t numberOfAllocation = allocation ? allocation.index() : LayoutData::FlattenedObjectData::noAllocation;
    operands.appendList({ LayoutData::FlattenedObjectData::noFunction, numberOfAllocation, placed->begin(), placed->beginInOwnCell, placed->endInOwnCell - placed->beginInOwnCell });
}

static SlotData closureEnvironmentSlotOf(TableEncoder& encoder, LayoutID id)
{
    std::optional<uint32_t> field = encoder.program.at(id).flow.closureEnvironmentField;
    RELEASE_ASSERT(field);
    const PlacedSlot& slot = encoder.slots.slot(id, *field);
    RELEASE_ASSERT(slot.storage == SlotStorage::Inline && slot.representation == SlotRepresentation::Cell32);
    return slot.data();
}

static bool isPlainSlot(const FieldData& field)
{
    constexpr uint16_t flagsOfOtherFields = fieldIsStoredOnce | fieldIsPrivate | fieldIsBrand | fieldAttributeFlags;
    return !(field.flags & flagsOfOtherFields) && field.slot.representation() != SlotRepresentation::DeferredClosure;
}

static bool isCopiedByWords(const FieldData& field)
{
    constexpr uint16_t flagsOfOtherFields = fieldIsPrivate | fieldIsBrand | fieldAttributeFlags;
    return !(field.flags & flagsOfOtherFields) && field.slot.representation() != SlotRepresentation::DeferredClosure;
}

static void collectFields(TableEncoder& encoder, LayoutID id, ObjectLayoutParts& parts)
{
    const Program& program = encoder.program;
    const PlannedLayout& planned = encoder.slots.layout(id);
    RELEASE_ASSERT(planned.numberOfRecordFields <= maxFieldsInOrder);
    parts.fields.fill(FieldData { }, planned.numberOfRecordFields);
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const PlacedSlot& slot = planned.slots[index];
        if (!slot.indexInLayoutRecord)
            continue;
        FieldReference reference { id, index };
        const Field& declaration = fieldAt(program, declarationOf(program, reference));

        FieldData& data = parts.fields[*slot.indexInLayoutRecord];
        data.name = declaration.key ? encoder.identifierNumber(declaration.key) : 0;
        data.isFilledAtBirth = isFilledAtBirth(program, reference);
        data.holdsRawValueFromBirth = holdsRawValueFromBirth(program, reference);
        data.keepsStateOfBirth = keepsStateOfBirth(program, reference);
        data.hasGuardOfStores = declaration.key && program.at(id).kind == LayoutKind::Instance && !!encoder.placement.guardBits().find(declaredLayoutOf(program, id), ClassSide::Prototype, MemberGuardKind::PassedByStores, declaration.key);
        data.flags = fieldFlagsOf(encoder, reference);
        data.slot = slot.data();
        data.type = encoder.typeOffset(recordedTypeOf(program, declaration));
        data.defersFlattenedObject = slot.defersFlattenedObject;
        if (slot.defersFlattenedObject) {
            data.operandIndex = static_cast<uint16_t>(parts.fieldOperands.size());
            appendOperandsOfFlattenedObject(encoder, reference, parts.fieldOperands);
        } else if (std::optional<uint32_t> operand = fieldOperandOf(encoder, slot)) {
            data.operandIndex = static_cast<uint16_t>(parts.fieldOperands.size());
            parts.fieldOperands.append(*operand);
        }
        if (slot.deferredClosureCapturesEnvironment)
            parts.fieldOperands.append(closureEnvironmentSlotOf(encoder, id).encoded());
        parts.hasFieldsWithSymbolKeys |= declaration.key && isSymbolKind(program.at(declaration.key).kind);
        parts.hasOnlyPlainSlots &= isPlainSlot(data);
        parts.isCopiedByWords &= isCopiedByWords(data);
    }
}

static Vector<uint32_t>& listFor(ReferenceOffsets& offsets, const PlacedSlot& slot)
{
    SlotTraits traits = traitsOf(slot.representation);
    if (slot.isOutOfLine())
        return traits.isNarrowReference ? offsets.narrowOutOfLine : offsets.wideOutOfLine;
    if (traits.isNarrowReference)
        return offsets.narrow;
    return traits.valueKind == SlotValueKind::Cell ? offsets.wideCells : offsets.wideValues;
}

static void appendAscending(Vector<uint32_t>& references, Vector<uint32_t>& offsets)
{
    AOT::sort(offsets);
    references.appendVector(offsets);
}

static uint32_t appendReferences(TableEncoder& encoder, LayoutID id)
{
    const PlannedLayout& planned = encoder.slots.layout(id);
    ReferenceOffsets offsets;
    for (FieldIndex row = 0; row < planned.slots.size(); ++row) {
        const PlacedSlot& slot = planned.slots[row];
        if (row == planned.extensionWordIndex() || !slot.hasStorage() || !traitsOf(slot.representation).holdsCell)
            continue;
        listFor(offsets, slot).append(slot.offset);
    }
    for (const PlacedFlattenedObject& flattened : planned.flattenedObjects) {
        for (const PlacedSlot& slot : encoder.slots.layout(flattened.layout).slots) {
            if (slot.hasStorage() && traitsOf(slot.representation).holdsCell && !encoder.build.recordedOptions().isSabotaged(Sabotage::RoomIsNotInReferenceList))
                listFor(offsets, slot).append(flattened.distance + slot.offset);
        }
    }

    // In front of the counts, read backwards from there: how many inner objects, their offsets, how many allocations make such objects, their numbers.
    Vector<uint32_t> references;
    if (!planned.innerObjects.isEmpty()) {
        for (const PlacedInnerObject& inner : planned.innerObjects) {
            if (!inner.layout)
                continue;
            for (AllocationID allocation : encoder.placement.allocations().ofLayout(inner.layout))
                references.append(allocation.index());
        }
        references.append(references.size());
        for (const PlacedInnerObject& inner : planned.innerObjects)
            references.append(inner.offset);
        references.append(planned.innerObjects.size());
    }
    uint32_t wordsInFront = references.size();
    references.appendList({ static_cast<uint32_t>(offsets.wideValues.size()), static_cast<uint32_t>(offsets.wideCells.size()), static_cast<uint32_t>(offsets.narrow.size()) });
    static_assert(numberOfInlineReferenceCounts == 3 && numberOfExtensionHeaderWords == 7);
    appendAscending(references, offsets.wideValues);
    appendAscending(references, offsets.wideCells);
    appendAscending(references, offsets.narrow);
    if (planned.hasExtension()) {
        ExtensionReferences sets { };
        for (uint32_t offset : offsets.wideOutOfLine) {
            RELEASE_ASSERT(!offsetInGranule(offset));
            sets.wide |= granuleBitOf(offset);
        }
        for (uint32_t offset : offsets.narrowOutOfLine)
            (offsetInGranule(offset) ? sets.narrowInSecondHalf : sets.narrowInFirstHalf) |= granuleBitOf(offset);
        references.append(encoder.slots.extensionWord(id).data().encoded());
        for (uint64_t set : { sets.wide, sets.narrowInFirstHalf, sets.narrowInSecondHalf })
            references.appendList({ static_cast<uint32_t>(set), static_cast<uint32_t>(set >> 32) });
    }
    return references.size() > numberOfInlineReferenceCounts ? encoder.writer.append(references) + wordsInFront * sizeof(uint32_t) : 0;
}

static uint32_t appendKeyOrders(TableEncoder& encoder, LayoutID id)
{
    Vector<uint32_t> orders;
    for (const Vector<FieldIndex>& order : encoder.placement.keyOrders().of(id)) {
        Vector<uint16_t> list;
        list.append(static_cast<uint16_t>(order.size()));
        for (FieldIndex field : order) {
            if (std::optional<uint16_t> indexInLayoutRecord = encoder.slots.slot(id, field).indexInLayoutRecord)
                list.append(*indexInLayoutRecord);
        }
        if (list.size() != order.size() + 1) {
            encoder.fail(ProgramTableRule::TableOutOfBounds, id);
            continue;
        }
        orders.append(encoder.writer.append(list));
    }
    return orders.isEmpty() ? 0 : encoder.writer.append(orders);
}

static uint32_t appendEnvironment(TableEncoder& encoder, LayoutID id, uint32_t shape)
{
    const Layout& layout = encoder.program.at(id);
    const PlannedLayout& planned = encoder.slots.layout(id);
    RELEASE_ASSERT(layout.isEnvironment());
    Vector<FieldIndex> described = collectDescribedVariables(encoder, id);
    uint32_t referencesOffset = appendReferences(encoder, id);

    uint32_t offset = encoder.writer.allocate(sizeof(EnvironmentData) + described.size() * sizeof(VariableData), alignof(EnvironmentData));
    auto& data = encoder.writer.at<EnvironmentData>(offset);
    data.kind = layout.kind;
    data.index = id.index();
    data.shape = shape;
    data.name = encoder.shownNameNumber(layout.name);
    data.size = planned.size;
    data.numberOfDescribedVariables = static_cast<uint32_t>(described.size());
    data.referencesOffset = distanceFromRecord(offset, referencesOffset);

    for (size_t number = 0; number < described.size(); ++number) {
        FieldIndex index = described[number];
        const PlacedSlot& slot = planned.slots[index];
        if (slot.isOutOfLine())
            encoder.fail(ProgramTableRule::TableOutOfBounds, id);
        PropertyKeyID key = layout.fields[index].key;
        auto& variable = encoder.writer.at<VariableData>(offset + sizeof(EnvironmentData) + number * sizeof(VariableData));
        variable.name = key ? encoder.identifiers.numberOf(key).value_or(0) : 0;
        variable.slot = slot.data();
    }
    return offset;
}

static ObjectLayoutParts collectObjectLayoutParts(TableEncoder& encoder, LayoutID id, const PlacedShape& shape)
{
    const PlannedLayout& planned = encoder.slots.layout(id);
    ObjectLayoutParts parts;
    collectFields(encoder, id, parts);
    if (!shape.keysAreNeverEnumerated) {
        parts.numberOfOrders = static_cast<uint32_t>(encoder.placement.keyOrders().of(id).size());
        parts.ordersOffset = appendKeyOrders(encoder, id);
    }
    parts.referencesOffset = appendReferences(encoder, id);
    parts.hasExtension = planned.hasExtension();
    parts.hasExtraPropertiesSlot = planned.hasExtraPropertiesWord();
    return parts;
}

static EvaluationState evaluationStateOf(const Program& program, const Layout& layout)
{
    if (layout.kind != LayoutKind::Instance)
        return EvaluationState { };
    const Layout& classLayout = layout.isDerived() ? program.at(declaredLayoutOf(program, layout.flow.base)) : layout;
    return isEvaluatedOnce(program, classLayout) ? EvaluationState::ProvenOnce : EvaluationState::PredictedOnce;
}

static ExtensionLayoutData extensionLayoutDataOf(TableEncoder& encoder, LayoutID id)
{
    const PlannedLayout& planned = encoder.slots.layout(id);
    uint64_t granules = 0;
    for (const PlacedSlot& slot : planned.slots.span().first(planned.numberOfFields())) {
        if (slot.isOutOfLine() && slot.isFilledByConstructor)
            granules |= granuleBitOf(slot.offset);
    }
    return {
        .extension = encoder.slots.extensionWord(id).data(),
        .extensionSize = planned.extensionSize,
        .granulesFilledByConstructorLow = static_cast<uint32_t>(granules),
        .granulesFilledByConstructorHigh = static_cast<uint32_t>(granules >> 32),
    };
}

static bool isUntyped(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Instance:
        if (layout.isDerived())
            return isUntyped(program, program.at(layout.flow.base));
        return layout.constructor && !program.at(layout.constructor).flags.contains(FunctionFlags::IsChecked);
    case LayoutKind::Record:
        return (!layout.name || program.string(layout.name).empty()) && std::ranges::all_of(layout.fields, [&](const Field& field) {
            return recordedTypeOf(program, field) == TypeTable::anything;
        });
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void fillLayoutData(TableEncoder& encoder, LayoutID id, const PlacedShape& shape, const ObjectLayoutParts& parts, LayoutData& data)
{
    const Layout& layout = encoder.program.at(id);
    const ClassCreationPlan& creation = encoder.slots.classCreation(id);

    data.kindBits = static_cast<uint32_t>(layout.kind);
    data.isDerived = layout.isDerived();
    data.impossibleMutationsRaw = shape.impossibleMutations.toRaw();
    data.hasInnerObjects = !encoder.slots.layout(id).innerObjects.isEmpty();
    data.holdsInnerObjectsInPlace = std::ranges::any_of(encoder.slots.layout(id).innerObjects, &PlacedInnerObject::isAlwaysInPlace);
    data.mayBeInnerObject = encoder.slots.layout(id).mayBeInnerObject;
    data.evaluationBits = static_cast<uint32_t>(evaluationStateOf(encoder.program, layout));
    data.keysAreNeverEnumerated = shape.keysAreNeverEnumerated;
    data.hasFieldsWithSymbolKeys = parts.hasFieldsWithSymbolKeys;
    data.hasOnlyPlainSlots = parts.hasOnlyPlainSlots;
    data.isCopiedByWords = parts.isCopiedByWords;
    data.isUntyped = isUntyped(encoder.program, layout);
    data.hasUntypedBase = layout.has(LayoutFlags::HasHeritage) && layout.flow.baseState != BaseState::Layout;
    data.createsMembersLazily = creation.createsMembersLazily;
    data.createsPrototypeBeforeClassObject = creation.createsPrototypeBeforeClassObject;
    data.createsClassObjectOnMutation = creation.createsClassObjectOnMutation;
    data.hasExtensionData = parts.hasExtension;
    data.hasFieldThatKeepsStateOfBirth = std::ranges::any_of(parts.fields, [](const FieldData& field) { return field.keepsStateOfBirth; });
    data.hasExtraPropertiesSlot = parts.hasExtraPropertiesSlot;
    data.index = id.index();
    data.name = encoder.shownNameNumber(layout.name);
    data.size = encoder.slots.layout(id).size;
    data.numberOfFields = static_cast<uint16_t>(parts.fields.size());
    data.numberOfFieldOperands = static_cast<uint16_t>(parts.fieldOperands.size());
    data.numberOfOrders = parts.numberOfOrders;
    data.ordersOffset = parts.ordersOffset;
}

static uint32_t appendObjectLayout(TableEncoder& encoder, LayoutID id, uint32_t shapeNumber)
{
    const Layout& layout = encoder.program.at(id);
    RELEASE_ASSERT(isObjectLayout(layout.kind));
    PlacedShape shape = shapeNumber ? encoder.placement.shapes().shape(shapeNumber) : PlacedShape { };
    ObjectLayoutParts parts = collectObjectLayoutParts(encoder, id, shape);
    bool isClass = layout.kind == LayoutKind::Instance;
    ClassRecordParts classParts;
    if (isClass)
        classParts = collectClassRecordParts(encoder, id);

    size_t prefixSize = LayoutData::prefixSize(isClass, parts.hasExtension, parts.hasExtraPropertiesSlot);
    uint64_t trailingSize = LayoutData::trailingSize({
        .fields = parts.fields.size(),
        .fieldOperands = parts.fieldOperands.size(),
        .methods = classParts.methods.size(),
        .staticFields = classParts.staticFields.size(),
        .memberGuards = classParts.memberGuards.size(),
        .memberGuardAggregates = classParts.memberGuardAggregates.size(),
    });
    uint32_t offset = encoder.writer.allocate(prefixSize + sizeof(LayoutData) + trailingSize, alignof(LayoutData)) + prefixSize;

    auto& data = encoder.writer.at<LayoutData>(offset);
    fillLayoutData(encoder, id, shape, parts, data);
    data.shape = shapeNumber;
    data.referencesOffset = distanceFromRecord(offset, parts.referencesOffset);
    if (isClass)
        data.classData() = classParts.data;
    if (parts.hasExtension)
        data.extensionData() = extensionLayoutDataOf(encoder, id);
    if (parts.hasExtraPropertiesSlot)
        data.slotOfExtraProperties() = encoder.slots.extraPropertiesWord(id).data();

    uint32_t end = writeRecords(encoder.writer, offset + sizeof(LayoutData), parts.fields);
    end = writeRecords(encoder.writer, end, parts.fieldOperands);
    end = writeRecords(encoder.writer, end, classParts.methods);
    end = writeRecords(encoder.writer, end, classParts.staticFields);
    end = writeRecords(encoder.writer, end, classParts.memberGuards);
    end = writeRecords(encoder.writer, end, classParts.memberGuardAggregates);
    RELEASE_ASSERT(end == offset + sizeof(LayoutData) + trailingSize);
    return offset;
}

static void encodeClasses(TableEncoder& encoder, const Vector<uint32_t>& classes)
{
    encoder.header.numberOfLayoutIndices = static_cast<uint32_t>(classes.size());
    Vector<uint32_t> classGroups(FillWith { }, (classes.size() + 31) / 32 * 2, 0);
    uint32_t numberOfClasses = 0;
    for (unsigned i = 0; i < classes.size(); ++i) {
        if (!(i % 32))
            classGroups[i / 32 * 2 + 1] = numberOfClasses;
        if (!classes[i])
            continue;
        classGroups[i / 32 * 2] |= 1u << i % 32;
        ++numberOfClasses;
    }
    for (uint32_t offset : classes) {
        if (offset)
            classGroups.append(offset);
    }
    encoder.setTable(ProgramTableIndex::classes, classGroups);
}

static Vector<Vector<uint32_t>> collectSubclasses(const Program& program, const Placement& placement)
{
    Vector<Vector<uint32_t>> subclasses(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || !placement.isInUse(LayoutID(index)))
            continue;
        LayoutID assumedBase = expectedBaseOf(layout);
        if (assumedBase && program.at(assumedBase).kind == LayoutKind::Instance)
            subclasses[assumedBase.index()].append(index);
    }
    return subclasses;
}

static void encodeLayoutRecords(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    encoder.subclasses = collectSubclasses(program, encoder.placement);
    Vector<uint32_t> layouts(FillWith { }, shapes.numberOfShapes() + 1, 0);
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        if (!shapes.isAssigned(shape))
            continue;
        LayoutID id = shapes.shape(shape).layout;
        if (!encoder.placement.isInUse(id))
            continue;
        layouts[shape] = shape < shapes.firstEnvironmentShape() ? appendObjectLayout(encoder, id, shape) : appendEnvironment(encoder, id, shape);
    }

    Vector<uint32_t> classes(FillWith { }, program.layouts.size(), 0);
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        LayoutID id(i);
        if (program.at(id).kind != LayoutKind::Instance || !encoder.placement.isInUse(id))
            continue;
        classes[i] = shapes.shapesOf(id).isEmpty() ? appendObjectLayout(encoder, id, 0) : layouts[shapes.generalShapeOf(id)];
    }
    encoder.build.statistics().count(StatisticID::LayoutRecordsEmitted, std::ranges::count_if(layouts, std::identity { }));
    encoder.setTable(ProgramTableIndex::layouts, layouts);
    encodeClasses(encoder, classes);
}

static bool leavesFieldOfBirthEmpty(const Program& program, const PlacedAllocation& allocation)
{
    if (!isObjectLayout(program.at(allocation.layout).kind))
        return false;
    for (FieldIndex field = 0; field < program.at(allocation.layout).fields.size(); ++field) {
        if (isFilledAtBirth(program, { allocation.layout, field }) && !allocation.initialFields.contains(field))
            return true;
    }
    return false;
}

static void encodeAllocations(TableEncoder& encoder)
{
    Vector<AllocationData> allocations;
    Vector<ConstructedAllocationData> constructedAllocations;
    allocations.reserveInitialCapacity(encoder.placement.numberOfAllocations());
    for (const PlacedAllocation& allocation : encoder.placement.allocations().allocations) {
        if (allocation.constructedBy)
            constructedAllocations.append({ Number24(allocations.size()), Number24(encoder.entrypointOf(allocation.constructedBy)) });
        allocations.append(AllocationData::create(allocation.shape, allocation.orderWord, allocation.size, leavesFieldOfBirthEmpty(encoder.program, allocation), !!allocation.constructedBy));
    }
    encoder.setTable(ProgramTableIndex::allocations, allocations);
    encoder.setTable(ProgramTableIndex::constructedAllocations, constructedAllocations);
}

void encodeShapeSetBits(TableEncoder& encoder)
{
    // Not shared with equal bytes further in front: the image builder takes the table off the end.
    std::span<const uint64_t> words = encoder.placement.shapes().shapeSetWords();
    encoder.setTableRange(ProgramTableIndex::shapeSetBits, encoder.writer.appendUnshared(asBytes(words), alignof(uint64_t)), static_cast<uint32_t>(words.size()));
}

static void encodeRemovedFields(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<RemovedFieldData> removedFields;
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        // Nothing looks a variable of an environment up by its name.
        if (program.at(id).isEnvironment())
            continue;
        bool everyObjectHadRemovedField = false;
        for (FieldIndex field = 0; field < program.at(id).fields.size(); ++field) {
            const Field& declaration = fieldAt(program, declarationOf(program, { id, field }));
            if (!declaration.flags.contains(FieldFlags::IsRemovedAsUnread) || !declaration.key)
                continue;
            everyObjectHadRemovedField |= declaration.flow.occupancy == Occupancy::AlwaysFilled;
            // The run time finds no field under a name without a number.
            if (std::optional<uint32_t> name = encoder.identifiers.numberOf(declaration.key))
                removedFields.append({ id.index(), *name });
        }
        // An object that never got the field lists its keys rightly.
        if (everyObjectHadRemovedField)
            removedFields.append({ id.index(), RemovedFieldData::everyObjectHadOne });
    }
    AOT::sort(removedFields);
    encoder.setTable(ProgramTableIndex::removedFields, removedFields);
}

static void encodeAddedNames(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<AddedNameData> addedNames;
    // The run time cannot tell a listed name without a number from a name that is not listed, nor can it tell which call has made a symbol.
    auto haveNumbers = [&](const Vector<PropertyKeyID>& keys) {
        return std::ranges::all_of(keys, [&](PropertyKeyID key) {
            return encoder.identifiers.numberOf(key) && !encoder.flow.standsForSeveralSymbols(key);
        });
    };
    const Vector<PropertyKeyID>* sharedKeys = program.keysStoredThroughUnknownReceivers();
    bool canListSharedKeys = sharedKeys && haveNumbers(*sharedKeys);
    bool listsSharedKeys = false;
    for (unsigned index = 0; encoder.build.validates() && index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const LayoutFlow& flow = program.at(id).flow;
        if (!flow.addedKeysAreComplete || encoder.slots.storageOf(id) != LayoutStorage::CompiledObject)
            continue;
        if (!haveNumbers(flow.addedKeys) || (flow.isReachedThroughUnknownReceivers && !canListSharedKeys))
            continue;
        for (PropertyKeyID key : flow.addedKeys)
            addedNames.append({ id.index(), *encoder.identifiers.numberOf(key) });
        if (flow.isReachedThroughUnknownReceivers) {
            addedNames.append({ id.index(), AddedNameData::getsSharedNames });
            listsSharedKeys = true;
        }
        addedNames.append({ id.index(), AddedNameData::listIsComplete });
    }
    if (listsSharedKeys) {
        for (PropertyKeyID key : *sharedKeys)
            addedNames.append({ AddedNameData::layoutOfSharedNames, *encoder.identifiers.numberOf(key) });
    }
    AOT::sort(addedNames);
    encoder.setTable(ProgramTableIndex::addedNames, addedNames);
}

static void encodeClassPrototypeNames(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<AddedNameData> names;
    for (unsigned index = 0; encoder.build.validates() && index < program.layouts.size(); ++index) {
        const ClassFlow& flow = program.layouts[index].classFlow;
        if (!flow.prototypeNamesAreComplete)
            continue;
        size_t sizeBefore = names.size();
        bool everyNameHasNumber = true;
        for (const Vector<ClassMember>* members : { &flow.members, &flow.namedMembers }) {
            for (const ClassMember& member : *members) {
                if (member.side != ClassSide::Prototype || !member.key)
                    continue;
                std::optional<uint32_t> name = encoder.identifiers.numberOf(member.key);
                // The key of a created symbol stands for every symbol that its call makes. The run time has no number for any of them, so it cannot find one in a list.
                everyNameHasNumber &= name && program.at(member.key).kind != PropertyKeyKind::CreatedSymbol;
                names.append({ index, name.value_or(0) });
            }
        }
        if (everyNameHasNumber)
            names.append({ index, AddedNameData::listIsComplete });
        else
            names.shrink(sizeBefore);
    }
    AOT::sort(names);
    encoder.setTable(ProgramTableIndex::classPrototypeNames, names);
}

// False where some validator accepts an object whatever its members are, or there are more rows than the table can number.
static bool appendMembersForValidators(TableEncoder& encoder, Vector<ParsedRecordFieldData>& rows)
{
    Vector<Vector<uint32_t, 8>> validatedObjects;
    for (const ExpectedResult& expected : encoder.program.expectedResults) {
        for (const ValidationNode& node : expected.validation) {
            if (node.shape.kind() == ValidationShape::Kind::AnyObject || node.shape.kind() == ValidationShape::Kind::Dictionary)
                return false;
            if (node.shape.kind() != ValidationShape::Kind::Object)
                continue;
            Vector<uint32_t, 8> names;
            bool canBeMet = true;
            for (const ValidationMember& member : node.shape.as<ValidationShape::Object>().members) {
                if (member.isOptional)
                    continue;
                std::optional<uint32_t> name = encoder.identifiers.numberOf(member.key);
                if (!name) {
                    canBeMet = false;
                    break;
                }
                names.appendIfNotContains(*name);
            }
            if (!canBeMet)
                continue;
            if (names.isEmpty() || names.size() > ParsedRecordFieldData::maxRequiredMembers)
                return false;
            AOT::sort(names);
            validatedObjects.appendIfNotContains(WTF::move(names));
        }
    }
    if (validatedObjects.size() > ParsedRecordFieldData::maxValidatedObjects)
        return false;
    for (uint32_t index = 0; index < validatedObjects.size(); ++index) {
        for (uint32_t name : validatedObjects[index])
            rows.append({ name, ParsedRecordFieldData::memberForValidator(index, validatedObjects[index].size()) });
    }
    return true;
}

static void encodeParsedRecordFields(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    Vector<ParsedRecordFieldData> rows;
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Record || !layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse) || shapes.shapesOf(id).isEmpty())
            continue;
        uint32_t shape = shapes.generalShapeOf(id);
        const PlacedShape& placed = shapes.shape(shape);
        if (placed.keysAreNeverEnumerated || !placed.impossibleMutations.isEmpty())
            continue;
        size_t sizeBefore = rows.size();
        for (FieldIndex fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
            const Field& field = layout.fields[fieldIndex];
            if (!encoder.slots.slot(id, fieldIndex).indexInLayoutRecord)
                continue;
            std::optional<uint32_t> name = field.key ? encoder.identifiers.numberOf(field.key) : std::nullopt;
            if (!name) {
                rows.shrink(sizeBefore);
                break;
            }
            rows.append({ *name, shape });
        }
    }
    if (!rows.isEmpty() && !appendMembersForValidators(encoder, rows))
        rows.clear();
    AOT::sort(rows);
    encoder.setTable(ProgramTableIndex::parsedRecordFields, rows);
}

void encodeLayoutTables(TableEncoder& encoder)
{
    encodeLayoutRecords(encoder);
    encodeRemovedFields(encoder);
    encodeAddedNames(encoder);
    encodeClassPrototypeNames(encoder);
    encodeParsedRecordFields(encoder);
    encodeAllocations(encoder);
    encodeConstructors(encoder);
    encodeBoundFunctions(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
