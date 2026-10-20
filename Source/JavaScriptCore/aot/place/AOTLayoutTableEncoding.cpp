/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTObjectVisiting.h"
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
    uint32_t typeStringsOffset { 0 };
    DeclarationLocation declaration;
    bool hasMessageData { false };
    bool hasRareData { false };
    bool hasFieldsWithSymbolKeys { false };
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
    uint16_t primitives = program.at(declaration.declared.type).primitives;

    uint16_t flags = 0;
    if (!declaration.flags.contains(FieldFlags::IsRequired))
        flags |= fieldIsOptional;
    if (slot.isOutOfLine())
        flags |= fieldIsOutOfLine;
    if (location.stores == StoreCount::AtMostOnce)
        flags |= fieldIsStoredOnce;
    if (primitives & Primitive::Undefined)
        flags |= fieldAllowsUndefined;
    if (slot.mayExistWithoutStorage)
        flags |= fieldMayExistWithoutStorage;
    if (slot.deferredClosureCapturesObject)
        flags |= fieldDeferredClosureCapturesObject;
    if (declaration.flags.contains(FieldFlags::IsPrivate))
        flags |= fieldIsPrivate;
    if (declaration.flags.contains(FieldFlags::IsBrand))
        flags |= fieldIsBrand;
    if (stability.contains(FieldStability::IsNeverDeleted))
        flags |= fieldIsNeverDeleted;
    if (stability.contains(FieldStability::IsNeverRedefined))
        flags |= fieldIsNeverRedefined;
    if (stability.contains(FieldStability::IsNeverStoredWithOtherType))
        flags |= fieldIsNeverStoredWithOtherType;
    return flags;
}

static std::optional<uint32_t> fieldOperandOf(TableEncoder& encoder, const PlacedSlot& slot)
{
    switch (traitsOf(slot.representation).valueKind) {
    case SlotValueKind::Int32OrDouble:
        return *slot.doubleOffsetInExtension;
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
        data.flags = fieldFlagsOf(encoder, reference);
        data.slot = slot.data();
        data.type = encoder.typeOffset(declaration.declared.type);
        if (std::optional<uint32_t> operand = fieldOperandOf(encoder, slot)) {
            data.flags |= fieldHasOperand;
            data.operandIndex = static_cast<uint16_t>(parts.fieldOperands.size());
            parts.fieldOperands.append(*operand);
        }
        parts.hasFieldsWithSymbolKeys |= declaration.key && isSymbolKind(program.at(declaration.key).kind);
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
    std::ranges::sort(offsets);
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

    Vector<uint32_t> references { static_cast<uint32_t>(offsets.wideValues.size()), static_cast<uint32_t>(offsets.wideCells.size()), static_cast<uint32_t>(offsets.narrow.size()) };
    static_assert(numberOfInlineReferenceCounts == 3 && numberOfExtensionHeaderWords == 3);
    appendAscending(references, offsets.wideValues);
    appendAscending(references, offsets.wideCells);
    appendAscending(references, offsets.narrow);
    if (planned.hasExtension()) {
        references.appendList({ encoder.slots.extensionWord(id).data().encoded(), static_cast<uint32_t>(offsets.wideOutOfLine.size()), static_cast<uint32_t>(offsets.narrowOutOfLine.size()) });
        appendAscending(references, offsets.wideOutOfLine);
        appendAscending(references, offsets.narrowOutOfLine);
    }
    return references.size() > numberOfInlineReferenceCounts ? encoder.writer.append(references) : 0;
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

static uint32_t appendTypeTexts(TableEncoder& encoder, LayoutID id)
{
    if (encoder.obfuscates())
        return 0;
    const Program& program = encoder.program;
    const PlannedLayout& planned = encoder.slots.layout(id);
    Vector<uint32_t> texts;
    bool hasText = false;
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        if (!planned.slots[index].indexInLayoutRecord)
            continue;
        TypeSiteID site = fieldAt(program, declarationOf(program, { id, index })).declared.site;
        StringID text = site ? program.at(site).text : StringID();
        hasText |= !!text;
        texts.append(text ? encoder.stringNumber(text) : UINT32_MAX);
    }
    return hasText ? encoder.writer.append(texts) : 0;
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
    data.name = encoder.stringNumber(layout.name);
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
    parts.typeStringsOffset = appendTypeTexts(encoder, id);

    bool retainsLocation = retainsDeclarationLocation(encoder, id);
    if (retainsLocation)
        parts.declaration = declarationLocationOf(encoder, id);
    parts.hasMessageData = parts.typeStringsOffset || retainsLocation;
    parts.hasRareData = planned.hasExtension() || planned.hasExtraPropertiesWord();
    return parts;
}

static EvaluationState evaluationStateOf(const Program& program, const Layout& layout)
{
    if (layout.kind != LayoutKind::Instance)
        return EvaluationState { };
    const Layout& classLayout = layout.isDerived() ? program.at(declaredLayoutOf(program, layout.flow.base)) : layout;
    return isEvaluatedOnce(program, classLayout) ? EvaluationState::ProvenOnce : EvaluationState::PredictedOnce;
}

static RareLayoutData rareLayoutDataOf(TableEncoder& encoder, LayoutID id)
{
    const PlannedLayout& planned = encoder.slots.layout(id);
    return {
        .extension = planned.hasExtension() ? encoder.slots.extensionWord(id).data() : SlotData { },
        .extraProperties = planned.hasExtraPropertiesWord() ? encoder.slots.extraPropertiesWord(id).data() : SlotData { },
        .extensionSize = planned.extensionSize,
    };
}

static bool hasWrittenType(const Field& field)
{
    return field.declared.written || field.declared.type != TypeTable::anything;
}

// What a type error calls an untyped object: an instance of a class of an unchecked file, or a literal whose type nobody wrote.
static bool isUntyped(const Program& program, const Layout& layout)
{
    switch (layout.kind) {
    case LayoutKind::Instance:
        if (layout.isDerived())
            return isUntyped(program, program.at(layout.flow.base));
        return layout.constructor && !program.at(layout.constructor).flags.contains(FunctionFlags::IsChecked);
    case LayoutKind::Record:
        return (!layout.name || program.string(layout.name).empty()) && std::ranges::none_of(layout.fields, hasWrittenType);
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
    data.evaluationBits = static_cast<uint32_t>(evaluationStateOf(encoder.program, layout));
    data.keysAreNeverEnumerated = shape.keysAreNeverEnumerated;
    data.hasFieldsWithSymbolKeys = parts.hasFieldsWithSymbolKeys;
    data.isUntyped = isUntyped(encoder.program, layout);
    data.hasUntypedBase = layout.has(LayoutFlags::HasHeritage) && layout.flow.baseState != BaseState::Layout;
    data.createsMembersLazily = creation.createsMembersLazily;
    data.createsPrototypeBeforeClassObject = creation.createsPrototypeBeforeClassObject;
    data.createsClassObjectOnMutation = creation.createsClassObjectOnMutation;
    data.hasRareData = parts.hasRareData;
    data.hasMessageData = parts.hasMessageData;
    data.index = id.index();
    data.name = encoder.stringNumber(layout.name);
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

    size_t prefixSize = LayoutData::prefixSize(isClass, parts.hasMessageData, parts.hasRareData);
    uint64_t trailingSize = LayoutData::trailingSize({
        .fields = parts.fields.size(),
        .fieldOperands = parts.fieldOperands.size(),
        .methods = classParts.methods.size(),
        .staticFields = classParts.staticFields.size(),
        .memberGuards = classParts.memberGuards.size(),
    });
    uint32_t offset = encoder.writer.allocate(prefixSize + sizeof(LayoutData) + trailingSize, alignof(LayoutData)) + prefixSize;

    auto& data = encoder.writer.at<LayoutData>(offset);
    fillLayoutData(encoder, id, shape, parts, data);
    data.shape = shapeNumber;
    data.referencesOffset = distanceFromRecord(offset, parts.referencesOffset);
    if (isClass)
        data.classData() = classParts.data;
    if (parts.hasMessageData)
        data.messageData() = { parts.typeStringsOffset, parts.declaration.path, parts.declaration.line };
    if (parts.hasRareData)
        data.rareData() = rareLayoutDataOf(encoder, id);

    uint32_t end = writeRecords(encoder.writer, offset + sizeof(LayoutData), parts.fields);
    end = writeRecords(encoder.writer, end, parts.fieldOperands);
    end = writeRecords(encoder.writer, end, classParts.methods);
    end = writeRecords(encoder.writer, end, classParts.staticFields);
    end = writeRecords(encoder.writer, end, classParts.memberGuards);
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

static void encodeLayoutRecords(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    const ShapeTable& shapes = encoder.placement.shapes();
    Vector<uint32_t> layouts(FillWith { }, shapes.numberOfShapes() + 1, 0);
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        if (!shapes.isAssigned(shape))
            continue;
        LayoutID id = shapes.shape(shape).layout;
        layouts[shape] = shape < shapes.firstEnvironmentShape() ? appendObjectLayout(encoder, id, shape) : appendEnvironment(encoder, id, shape);
    }

    Vector<uint32_t> classes(FillWith { }, program.layouts.size(), 0);
    for (unsigned i = 0; i < program.layouts.size(); ++i) {
        LayoutID id(i);
        if (program.at(id).kind != LayoutKind::Instance)
            continue;
        classes[i] = shapes.shapesOf(id).isEmpty() ? appendObjectLayout(encoder, id, 0) : layouts[shapes.generalShapeOf(id)];
    }
    encoder.setTable(ProgramTableIndex::layouts, layouts);
    encodeClasses(encoder, classes);
}

static void encodeAllocations(TableEncoder& encoder)
{
    Vector<AllocationData> allocations;
    allocations.reserveInitialCapacity(encoder.placement.numberOfAllocations());
    for (const PlacedAllocation& allocation : encoder.placement.allocations().allocations) {
        AllocationData data { };
        data.shape = allocation.shape;
        data.orderWord = allocation.orderWord;
        data.size = allocation.size;
        data.constructedBy = allocation.constructedBy ? encoder.entrypointOf(allocation.constructedBy) : AllocationData::notConstructed;
        allocations.append(data);
    }
    encoder.setTable(ProgramTableIndex::allocations, allocations);
}

static void encodeShapeSetBits(TableEncoder& encoder)
{
    const BitVector& bits = encoder.placement.shapes().shapeSetBits();
    Vector<uint32_t> words(FillWith { }, (bits.size() + 31) / 32, 0);
    for (size_t bit : bits)
        words[bit / 32] |= 1u << bit % 32;
    encoder.setTable(ProgramTableIndex::shapeSetBits, words);
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
    std::ranges::sort(removedFields);
    encoder.setTable(ProgramTableIndex::removedFields, removedFields);
}

static void encodeAddedNames(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<AddedNameData> addedNames;
    for (unsigned index = 0; encoder.build.validates() && index < program.layouts.size(); ++index) {
        LayoutID id(index);
        const LayoutFlow& flow = program.at(id).flow;
        if (!flow.addedKeysAreComplete || encoder.slots.storageOf(id) != LayoutStorage::CompiledObject)
            continue;
        // The run time cannot tell a listed name without a number from a name that is not listed.
        bool everyNameHasNumber = std::ranges::all_of(flow.addedKeys, [&](PropertyKeyID key) {
            return !!encoder.identifiers.numberOf(key);
        });
        if (!everyNameHasNumber)
            continue;
        for (PropertyKeyID key : flow.addedKeys)
            addedNames.append({ id.index(), *encoder.identifiers.numberOf(key) });
        addedNames.append({ id.index(), AddedNameData::listIsComplete });
    }
    std::ranges::sort(addedNames);
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
                everyNameHasNumber &= !!name;
                names.append({ index, name.value_or(0) });
            }
        }
        if (everyNameHasNumber)
            names.append({ index, AddedNameData::listIsComplete });
        else
            names.shrink(sizeBefore);
    }
    std::ranges::sort(names);
    encoder.setTable(ProgramTableIndex::classPrototypeNames, names);
}

void encodeLayoutTables(TableEncoder& encoder)
{
    encodeLayoutRecords(encoder);
    encodeRemovedFields(encoder);
    encodeAddedNames(encoder);
    encodeClassPrototypeNames(encoder);
    encodeAllocations(encoder);
    encodeShapeSetBits(encoder);
    encodeConstructors(encoder);
    encodeBoundFunctions(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
