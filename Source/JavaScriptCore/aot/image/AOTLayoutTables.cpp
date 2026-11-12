/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTObjectVisiting.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

struct FieldOfLayoutRecord {
    PropertyKeyID key;
    OptionSet<Typed::FieldFlags> flags;
    OptionSet<Typed::FieldForm> form;
};

struct LayoutRecordParts {
    Vector<uint8_t> record;
    Vector<uint16_t> keyOrders;
    Vector<uint32_t> references;
    std::array<Vector<uint32_t>, numberOfClassSides> namedGuards;
    uint32_t prefixSize { 0 };
    uint32_t numberOfKeyOrders { 0 };
    uint32_t shapeThatParsersMake { 0 };
};

struct IDsNamedByLayoutRecords {
    void set(size_t id) { ids.append(static_cast<uint32_t>(id)); }

    Vector<uint32_t> ids;
};

struct NamesOfLayoutRecords {
    IDsNamedByLayoutRecords keys;
    IDsNamedByLayoutRecords strings;
    IDsNamedByLayoutRecords functionsWithClosureCell;
    Vector<TypeID> types;
};

struct AllocationRows {
    void append(const AllocationData&);

    Vector<AllocationRow> rows;
    Vector<AllocationData> forms;
    Vector<ConstructedAllocationData> constructedAllocations;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numbersOfForms;
};

} // anonymous namespace

static constexpr bool isPrivateNameKindForLayoutRecord(PropertyKeyKind kind)
{
    switch (kind) {
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return true;
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static ALWAYS_INLINE FieldOfLayoutRecord describeFieldOfLayoutRecord(const TableInputs& inputs, Typed::FieldReference reference, const Typed::FieldFacts& facts)
{
    const Typed::Program& program = inputs.program;
    FieldOfLayoutRecord field { { }, { }, facts.formOfValue };
    if (facts.facts.contains(Typed::FieldFact::IsAccessor))
        field.form.add(Typed::FieldForm::IsAccessor);
    if (reference.field < program.at(reference.layout).fields.size()) {
        const Typed::Field& declaration = Typed::fieldAt(program, Typed::declarationOf(program, reference));
        field.key = declaration.key;
        field.flags = declaration.flags;
        field.form.add(declaration.form);
        return field;
    }
    field.key = inputs.plan.slots().keyOf(program, reference);
    if (!field.key) {
        field.flags.add(Typed::FieldFlags::IsHidden);
        return field;
    }
    const Typed::PropertyKey& name = program.at(field.key);
    if (isPrivateNameKindForLayoutRecord(name.kind))
        field.flags.add(Typed::FieldFlags::IsPrivate);
    if (name.kind == PropertyKeyKind::PrivateName) {
        constexpr OptionSet<Typed::FieldFlags> flagsOfPrivateNames { Typed::FieldFlags::IsPrivate, Typed::FieldFlags::IsImmutable, Typed::FieldFlags::IsBrand, Typed::FieldFlags::IsHidden, Typed::FieldFlags::IsBrandOfMethods };
        const Typed::Layout& owner = program.at(name.owner);
        if (std::optional<uint32_t> declared = owner.findField(field.key))
            field.flags.add(owner.fields[*declared].flags & flagsOfPrivateNames);
    }
    return field;
}

static TypeID typeRecordedForSlot(const Typed::PlacedSlot& slot)
{
    TypeID type = slot.typeInLayoutRecord();
    return type ? Typed::TypeTable::valuesOf(type) : Typed::TypeTable::anything;
}

static bool hasRecordOfClass(const TableInputs& inputs, LayoutID id)
{
    return inputs.program.at(id).kind == LayoutKind::Instance && inputs.plan.hasClassRecord(id);
}

static void forEachLayoutRecord(const TableInputs& inputs, NOESCAPE const Invocable<void(LayoutID, uint32_t shape)> auto& functor)
{
    const Typed::ShapeTable& shapes = inputs.plan.shapes();
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        if (shapes.isAssigned(shape) && inputs.plan.isAllocatable(shapes.shape(shape).layout))
            functor(shapes.shape(shape).layout, shape);
    }
    for (size_t index = 0; index < inputs.program.layouts.size(); ++index) {
        LayoutID id(index);
        if (hasRecordOfClass(inputs, id) && shapes.shapesOf(id).isEmpty())
            functor(id, 0);
    }
}

static void forEachDescribedVariableOfEnvironment(const TableInputs& inputs, LayoutID id, bool obfuscates, NOESCAPE const Invocable<void(const Typed::PlacedSlot&, StringID shownName, bool isVariableOfModule)> auto& functor)
{
    const Typed::Layout& layout = inputs.program.at(id);
    const Typed::PlannedLayout& planned = inputs.plan.layout(id);
    bool showsVariables = !obfuscates && showsVariablesOf(inputs, layout.module);
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const Typed::PlacedSlot& slot = planned.slots[index];
        SlotTraits traits = traitsOf(slot.representation);
        if (!slot.hasStorage() || !(traits.holdsCell || (traits.hasAbsent && traits.absentBits)))
            continue;
        StringID name = index < layout.fields.size() ? layout.fields[index].variableName : StringID();
        functor(slot, showsVariables && traits.holdsCell ? name : StringID(), name && layout.fields[index].declared.has(Typed::PositionFlags::IsVariableOfModule));
    }
}

static void nameStringShownByLayoutTables(const TableInputs& inputs, StringID name, NamesOfLayoutRecords& names)
{
    StringID shown = name && !inputs.options.obfuscates() ? name : inputs.program.strings.emptyString();
    names.strings.set(shown.index());
}

static void nameWhatObjectLayoutRecordNames(const TableInputs& inputs, LayoutID id, NamesOfLayoutRecords& names)
{
    const Typed::Layout& layout = inputs.program.at(id);
    const Typed::PlannedLayout& planned = inputs.plan.layout(id);
    auto nameKey = [&](PropertyKeyID key) {
        if (key)
            names.keys.set(key.index());
    };
    auto nameClosureCell = [&](FunctionID function) {
        if (function && inputs.plan.hasClosureCell(function))
            names.functionsWithClosureCell.set(function.index());
    };
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const Typed::PlacedSlot& slot = planned.slots[index];
        if (!slot.indexInLayoutRecord)
            continue;
        Typed::FieldReference reference { id, index };
        nameKey(describeFieldOfLayoutRecord(inputs, reference, inputs.facts.field(reference)).key);
        names.types.append(typeRecordedForSlot(slot));
    }
    if (layout.kind != LayoutKind::Instance)
        return;
    for (const Typed::StaticField& field : layout.staticFields)
        nameKey(field.key);
    if (Typed::isKnownByStructureOfClass(layout)) {
        for (const Typed::Field& field : layout.fields)
            nameKey(field.key);
    }
    for (const Typed::ClassMember& member : inputs.facts.membersOf(id)) {
        nameKey(member.key);
        nameClosureCell(member.function);
    }
    nameClosureCell(layout.constructor);
}

static void nameWhatEnvironmentRecordNames(const TableInputs& inputs, LayoutID id, NamesOfLayoutRecords& names)
{
    forEachDescribedVariableOfEnvironment(inputs, id, inputs.options.obfuscates(), [&](const Typed::PlacedSlot&, StringID shownName, bool) {
        if (shownName)
            names.strings.set(shownName.index());
    });
    nameStringShownByLayoutTables(inputs, inputs.program.at(id).name, names);
}

static bool isAddedToPrototype(const TableInputs& inputs, const Typed::LinkedNameOnPrototype& laid)
{
    return laid.key && !inputs.facts.linked().layout(laid.layout).reachesUnseenCode && !inputs.facts.hasMemberUnder(laid.layout, ClassSide::Prototype, laid.key);
}

void nameWhatLayoutTablesName(const TableInputs& inputs, const GatheredMentions&, NamesOfTables& names)
{
    RELEASE_ASSERT(inputs.build);
    uint32_t firstEnvironmentShape = inputs.plan.shapes().firstEnvironmentShape();
    Vector<std::pair<LayoutID, uint32_t>> records;
    forEachLayoutRecord(inputs, [&](LayoutID id, uint32_t shape) {
        records.append({ id, shape });
    });
    ChunksOfIndices division = divideIntoChunks(static_cast<unsigned>(records.size()));
    Vector<NamesOfLayoutRecords> namesOfChunks(division.size());
    forEachIndexInParallel(inputs.build->parallelLoops(), division.size(), [&](unsigned chunk) {
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index) {
            auto [id, shape] = records[index];
            if (!shape || shape < firstEnvironmentShape)
                nameWhatObjectLayoutRecordNames(inputs, id, namesOfChunks[chunk]);
            else
                nameWhatEnvironmentRecordNames(inputs, id, namesOfChunks[chunk]);
        }
    });
    for (const NamesOfLayoutRecords& namesOfChunk : namesOfChunks) {
        for (uint32_t key : namesOfChunk.keys.ids)
            names.keys.set(key);
        for (uint32_t string : namesOfChunk.strings.ids)
            names.strings.set(string);
        for (uint32_t function : namesOfChunk.functionsWithClosureCell.ids)
            names.functionsWithClosureCell.set(function);
        names.types.appendVector(namesOfChunk.types);
    }
    for (const Typed::LinkedNameOnFunction& laid : inputs.facts.linked().namesOnFunctions) {
        if (inputs.plan.slots().closuresGetOnlyListedNames(laid.holder))
            names.keys.set(laid.key.index());
    }
    for (const Typed::LinkedNameOnPrototype& laid : inputs.facts.linked().namesOnPrototypes) {
        if (isAddedToPrototype(inputs, laid))
            names.keys.set(laid.key.index());
    }
}

static uint32_t appendOffsetsInLayoutTables(TablePart& part, std::span<const uint32_t> words, size_t numberOfWordsInFrontOfOffsets = 0)
{
    uint32_t place = part.appendUnshared(asBytes(words), alignof(uint32_t));
    part.linkWords(place + static_cast<uint32_t>(numberOfWordsInFrontOfOffsets * sizeof(uint32_t)), words.size() - numberOfWordsInFrontOfOffsets);
    return place;
}

static uint16_t flagsOfFieldInLayoutRecord(const FieldOfLayoutRecord& field, const Typed::FieldFacts& facts, const Typed::PlacedSlot& slot)
{
    using Typed::FieldFlags;
    using Typed::FieldForm;
    using Typed::FieldStability;
    const std::pair<bool, uint16_t> flags[] = {
        { !field.flags.contains(FieldFlags::IsRequired), fieldIsOptional },
        { slot.isOutOfLine(), fieldIsOutOfLine },
        { slot.mayExistWithoutStorage, fieldMayExistWithoutStorage },
        { slot.deferredClosureCapturesObject, fieldDeferredClosureCapturesObject },
        { slot.deferredClosureCapturesEnvironment, fieldDeferredClosureCapturesEnvironment },
        { field.flags.contains(FieldFlags::IsPrivate) || (field.flags.contains(FieldFlags::IsHidden) && !field.key), fieldIsPrivate },
        { field.flags.contains(FieldFlags::IsBrand), fieldIsBrand },
        { facts.stability.contains(FieldStability::IsNeverDeleted), fieldIsNeverDeleted },
        { facts.stability.contains(FieldStability::IsNeverRedefined), fieldIsNeverRedefined },
        { slot.everyStoreFits, fieldIsNeverStoredWithOtherType },
        { field.form.contains(FieldForm::IsReadOnly), fieldIsReadOnly },
        { field.form.contains(FieldForm::IsNotEnumerable), fieldIsNotEnumerable },
        { field.form.contains(FieldForm::IsNotConfigurable), fieldIsNotConfigurable },
        { field.form.contains(FieldForm::IsAccessor), fieldIsAccessor },
    };
    uint16_t result = 0;
    for (auto [holds, flag] : flags)
        result |= holds ? flag : 0;
    return result;
}

static uint32_t closureNumberForLayoutRecord(const TableEncoder& encoder, FunctionID function, LayoutID owner)
{
    std::optional<FunctionID> entry = function ? encoder.findEntrypoint(function) : std::nullopt;
    if (!entry)
        return UINT32_MAX;
    const FunctionNumbers& functions = encoder.numbers.functions();
    if (*entry < functions.firstClosureFunction() || *entry >= functions.endOfClosureFunctions()) {
        encoder.fail("DeferredClosureWithoutClosureNumber"_s, owner);
        return UINT32_MAX;
    }
    return functions.closureNumberOf(*entry);
}

static FieldsOfLayoutRecord collectFieldsOfLayoutRecord(const TableEncoder& encoder, LayoutID id)
{
    const Typed::Program& program = encoder.program;
    const Typed::PlannedLayout& planned = encoder.plan.layout(id);
    FieldsOfLayoutRecord result;
    Vector<FieldData>& fields = result.fields;
    Vector<uint32_t>& operands = result.operands;
    LayoutData& layoutData = result.flags;
    fields.reserveInitialCapacity(planned.numberOfRecordFields);
    layoutData.hasOnlyPlainSlots = true;
    layoutData.isCopiedByWords = true;
    bool reachesUnseenCode = encoder.facts.linked().layout(id).reachesUnseenCode;
    BitVector isHeldFilled;
    for (FieldIndex field : Typed::fieldsHeldFilledInSlots(program, encoder.facts.linked(), encoder.plan.slots(), id))
        isHeldFilled.set(field);
    TypeID typeOfFieldInFront;
    uint16_t precedingFieldTypeNumber = 0;
    const bool leavesTypesToRunTime = encoder.firstParts->shiftOfTypeNumbers != ProgramTables::shiftOfNearTypeNumbers;
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const Typed::PlacedSlot& slot = planned.slots[index];
        if (!slot.indexInLayoutRecord)
            continue;
        if (*slot.indexInLayoutRecord != fields.size())
            encoder.fail("FieldsOfLayoutRecordAreNotNumberedInOrder"_s, id);
        Typed::FieldReference reference { id, index };
        Typed::FieldFacts facts = encoder.facts.field(reference);
        FieldOfLayoutRecord field = describeFieldOfLayoutRecord(encoder.inputs, reference, facts);
        TypeID type = typeRecordedForSlot(slot);
        uint32_t name = field.key ? encoder.identifierNumber(field.key, "a field in the record of"_s, id) : 0;
        if (name >> FieldData::nameBits)
            encoder.fail("NameDoesNotFitLayoutRecord"_s, id);

        FieldData data { };
        data.name = name;
        data.isFilledAtBirth = slot.isFilledAtBirth;
        data.holdsRawValueFromBirth = planned.holdsRawValueFromBirth(index);
        data.isHeldFilled = isHeldFilled.get(index);
        data.isWrittenPrivateName = field.flags.contains(Typed::FieldFlags::IsPrivate) && !field.flags.contains(Typed::FieldFlags::IsHidden) && field.key && program.at(field.key).owner == id;
        data.isKeptFromComparisons = data.isWrittenPrivateName && encoder.facts.linked().field(reference).isKeptFromComparisons;
        data.slotHoldsUndefinedAndNull = facts.facts.contains(Typed::FieldFact::SlotHoldsUndefinedAndNull) && !slot.provenType;
        data.flags = flagsOfFieldInLayoutRecord(field, facts, slot);
        if (slot.holdsAccessor && field.key && Typed::takesAccessorsOfLibraryAlone(program, encoder.plan.slots(), id, field.key))
            data.flags |= fieldTakesAccessorsOfLibraryAlone;
        data.slot = slot.data();
        if (type != typeOfFieldInFront || !precedingFieldTypeNumber) {
            typeOfFieldInFront = type;
            precedingFieldTypeNumber = encoder.typeNumber(type);
        }
        data.typeNumber = precedingFieldTypeNumber;
        if (leavesTypesToRunTime)
            data.flags |= fieldTypeIsLeftToRunTime;
        if (const Typed::LinkedAccessorsOfField* laid = slot.holdsAccessor ? encoder.facts.linked().findAccessorsExpectedIn(reference) : nullptr) {
            data.getterIsProven = laid->getterIsProven;
            data.setterIsProven = laid->setterIsProven;
        }
        uint16_t openToUnseenCode = fieldIsNeverDeleted | fieldIsNeverRedefined | (data.flags & fieldIsPrivate ? 0 : fieldIsNeverStoredWithOtherType);
        if (reachesUnseenCode && (data.flags & openToUnseenCode || (slot.hasStorage() && !data.canLeaveSlot())))
            encoder.fail("RecordRulesOutWhatUnseenCodeMayDo"_s, id);
        if (slot.representation == SlotRepresentation::DeferredClosure) {
            operands.append(static_cast<uint32_t>(fields.size()));
            operands.append(closureNumberForLayoutRecord(encoder, slot.deferredFunction, id));
            uint32_t environmentSlotOfOperands = 0;
            if (slot.deferredClosureCapturesEnvironment) {
                std::optional<uint32_t> environmentField = planned.environmentOfDeferredClosures;
                const Typed::PlacedSlot& environmentSlot = encoder.plan.slot(id, environmentField.value_or(static_cast<uint32_t>(planned.slots.size())));
                if (environmentSlot.storage != Typed::SlotStorage::Inline || traitsOf(environmentSlot.representation).valueKind != SlotValueKind::Cell)
                    encoder.fail("DeferredClosureWithoutSlotOfEnvironment"_s, id);
                environmentSlotOfOperands = environmentSlot.data().encoded();
            }
            operands.append(environmentSlotOfOperands);
        } else if (slot.deferredClosureCapturesEnvironment || slot.deferredClosureCapturesObject)
            encoder.fail("CapturesOfClosureThatIsNotDeferred"_s, id);

        bool isNamedProto = field.key && program.at(field.key).kind == PropertyKeyKind::Text && program.nameClassOf(field.key) == NameClass::Proto;
        bool isCopiedByWords = !(data.flags & (fieldIsPrivate | fieldIsBrand | fieldAttributeFlags)) && slot.representation != SlotRepresentation::DeferredClosure && !isNamedProto;
        layoutData.isCopiedByWords &= isCopiedByWords;
        layoutData.hasOnlyPlainSlots &= isCopiedByWords;
        layoutData.hasFieldsWithSymbolKeys |= field.key && Typed::isSymbolKind(program.at(field.key).kind);
        bool typeTakesUndefinedAndNull = !(Typed::Primitive::UndefinedOrNull & ~program.at(type).primitives);
        layoutData.hasSlotWithUnusedMarkers |= traitsOf(slot.representation).hasUndefined && !data.slotHoldsUndefinedAndNull && !typeTakesUndefinedAndNull;
        fields.append(data);
    }
    if (fields.size() != planned.numberOfRecordFields || fields.size() > maxFieldsInOrder)
        encoder.fail("NumberOfFieldsInLayoutRecordDiffersFromPlan"_s, id);
    return result;
}

Vector<FieldsOfLayoutRecord> collectFieldsOfLayoutRecords(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    uint32_t firstEnvironmentShape = encoder.plan.shapes().firstEnvironmentShape();
    Vector<LayoutID> layouts;
    BitVector isListed(program.layouts.size());
    forEachLayoutRecord(encoder.inputs, [&](LayoutID id, uint32_t shape) {
        if (shape < firstEnvironmentShape && isObjectLayout(program.at(id).kind) && !isListed.quickSet(id.index()))
            layouts.append(id);
    });
    Vector<FieldsOfLayoutRecord> result(program.layouts.size());
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(layouts.size()), [&](unsigned index) {
        result[layouts[index].index()] = collectFieldsOfLayoutRecord(encoder, layouts[index]);
    });
    return result;
}

static Vector<uint32_t> collectReferencesOfLayout(const TableEncoder& encoder, LayoutID id)
{
    enum { WideValues, WideCells, Narrow, WideOutOfLine, NarrowOutOfLine, NumberOfLists };
    static_assert(numberOfInlineReferenceCounts == WideOutOfLine && numberOfExtensionHeaderWords == 7);
    const Typed::PlannedLayout& planned = encoder.plan.layout(id);
    Vector<uint32_t, 8> offsets[NumberOfLists];
    for (FieldIndex row = 0; row < planned.slots.size(); ++row) {
        const Typed::PlacedSlot& slot = planned.slots[row];
        SlotTraits traits = traitsOf(slot.representation);
        if (row == planned.extensionWordIndex() || !slot.hasStorage() || !traits.holdsCell)
            continue;
        if (slot.isOutOfLine())
            offsets[traits.isNarrowReference ? NarrowOutOfLine : WideOutOfLine].append(slot.offset);
        else
            offsets[traits.isNarrowReference ? Narrow : traits.valueKind == SlotValueKind::Cell ? WideCells : WideValues].append(slot.offset);
    }

    Vector<uint32_t> references;
    for (unsigned list = 0; list < numberOfInlineReferenceCounts; ++list)
        references.append(static_cast<uint32_t>(offsets[list].size()));
    for (unsigned list = 0; list < numberOfInlineReferenceCounts; ++list) {
        AOT::sort(offsets[list]);
        references.appendVector(offsets[list]);
    }
    if (planned.hasExtension()) {
        ExtensionReferences sets { };
        for (uint32_t offset : offsets[WideOutOfLine]) {
            if (offsetInGranule(offset))
                encoder.fail("WideReferenceAcrossGranules"_s, id);
            sets.wide |= granuleBitOf(offset);
        }
        for (uint32_t offset : offsets[NarrowOutOfLine])
            (offsetInGranule(offset) ? sets.narrowInSecondHalf : sets.narrowInFirstHalf) |= granuleBitOf(offset);
        references.append(encoder.plan.slots().extensionWord(id).data().encoded());
        for (uint64_t set : { sets.wide, sets.narrowInFirstHalf, sets.narrowInSecondHalf })
            references.appendList({ static_cast<uint32_t>(set), static_cast<uint32_t>(set >> 32) });
    } else if (!offsets[WideOutOfLine].isEmpty() || !offsets[NarrowOutOfLine].isEmpty())
        encoder.fail("SlotOutOfLineWithoutExtension"_s, id);
    if (references.size() <= numberOfInlineReferenceCounts)
        references.clear();
    return references;
}

static void collectKeyOrdersOfLayout(const TableEncoder& encoder, LayoutID id, LayoutRecordParts& parts)
{
    const Typed::KeyOrders& keyOrders = encoder.plan.keyOrders();
    parts.numberOfKeyOrders = keyOrders.numberOfOrdersOf(id);
    for (uint32_t order = 0; order < parts.numberOfKeyOrders; ++order) {
        std::span<const FieldIndex> fieldsInOrder = keyOrders.order(id, order);
        size_t sizeBefore = parts.keyOrders.size();
        parts.keyOrders.append(static_cast<uint16_t>(fieldsInOrder.size()));
        for (FieldIndex field : fieldsInOrder) {
            if (std::optional<uint16_t> indexInLayoutRecord = encoder.plan.slot(id, field).indexInLayoutRecord)
                parts.keyOrders.append(*indexInLayoutRecord);
        }
        if (fieldsInOrder.size() > maxFieldsInOrder || parts.keyOrders.size() - sizeBefore != fieldsInOrder.size() + 1)
            encoder.fail("KeyOrderListsFieldOutsideLayoutRecord"_s, id);
    }
}

template<typename T>
static void writeIntoRoomOfLayoutRecord(std::span<uint8_t>& room, std::span<const T> items)
{
    static_assert(std::has_unique_object_representations_v<T>, "Padding would make the bytes differ from build to build.");
    memcpySpan(room, asBytes(items));
    room = room.subspan(items.size_bytes());
}

static LayoutRecordParts collectPartsOfEnvironmentRecord(const TableEncoder& encoder, LayoutID id)
{
    const Typed::Layout& layout = encoder.program.at(id);
    Vector<VariableData, 8> described;
    forEachDescribedVariableOfEnvironment(encoder.inputs, id, encoder.obfuscates(), [&](const Typed::PlacedSlot& slot, StringID shownName, bool isVariableOfModule) {
        if (slot.isOutOfLine())
            encoder.fail("VariableOfEnvironmentOutOfLine"_s, id);
        isVariableOfModule &= !!shownName;
        RELEASE_ASSERT(!shownName || encoder.stringNumber(shownName) < VariableData::isVariableOfModuleFlag);
        described.append({ shownName ? encoder.stringNumber(shownName) | (isVariableOfModule ? VariableData::isVariableOfModuleFlag : 0) : VariableData::noName, slot.data() });
    });

    LayoutRecordParts parts;
    parts.references = collectReferencesOfLayout(encoder, id);
    parts.record = Vector<uint8_t>(FillWith { }, sizeof(EnvironmentData) + described.sizeInBytes(), 0);
    auto& data = reinterpretCastSpanStartTo<EnvironmentData>(parts.record.mutableSpan());
    data.kind = layout.kind;
    data.moduleAfterNone = Number24(!encoder.obfuscates() && showsVariablesOf(encoder.inputs, layout.module) ? layout.module.index() + 1 : 0);
    data.index = id.index();
    data.name = encoder.shownNameNumber(layout.name);
    data.size = encoder.plan.layout(id).size;
    data.numberOfDescribedVariables = static_cast<uint32_t>(described.size());
    std::span<uint8_t> room = parts.record.mutableSpan().subspan(sizeof(EnvironmentData));
    writeIntoRoomOfLayoutRecord(room, described.span());
    RELEASE_ASSERT(room.empty());
    return parts;
}

static bool isUntypedLayout(const TableInputs& inputs, LayoutID id)
{
    const Typed::Program& program = inputs.program;
    const Typed::Layout& layout = program.at(id);
    switch (layout.kind) {
    case LayoutKind::Instance: {
        LayoutID declared = Typed::declaredLayoutOf(program, id);
        FunctionID constructor = declared ? program.at(declared).constructor : FunctionID();
        return constructor && !program.at(constructor).has(Typed::FunctionFlags::IsChecked);
    }
    case LayoutKind::Record:
        if (layout.name && layout.name != program.strings.emptyString())
            return false;
        for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
            if (TypeID held = inputs.plan.slot(id, field).type; held && !program.at(Typed::TypeTable::valuesOf(held)).isAnything())
                return false;
        }
        return true;
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

static bool classOfLayoutRecordIsEvaluatedOnce(const TableInputs& inputs, LayoutID classLayout)
{
    if (!inputs.facts.isEvaluatedAtMostOnce(classLayout))
        return false;
    const Typed::Layout& layout = inputs.program.at(classLayout);
    if (!layout.has(Typed::LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && inputs.program.at(layout.constructor).captures.size() == inputs.plan.promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor);
}

static uint32_t findShapeThatParsersMake(const TableEncoder& encoder, LayoutID id, std::span<const FieldData> fields)
{
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    if (encoder.program.at(id).kind != LayoutKind::Record || !encoder.plan.isAllocatable(id) || !encoder.plan.layout(id).holdsCompiledObjects() || !encoder.facts.parsersMayMake(id))
        return 0;
    uint32_t shape = shapes.generalShapeOf(id);
    if (!shapes.isAssigned(shape) || !shapes.shape(shape).impossibleMutations.isEmpty())
        return 0;
    if (shape > Number24::max) {
        encoder.fail("ShapeDoesNotFitRowOfParsedRecordFields"_s, id);
        return 0;
    }
    bool everyFieldHasKey = std::ranges::all_of(fields, [](const FieldData& field) {
        return !!field.name;
    });
    return everyFieldHasKey ? shape : 0;
}

static LayoutRecordParts collectPartsOfObjectLayoutRecord(const TableEncoder& encoder, LayoutID id, bool keepsKeyOrder, LayoutID& parentThatListsClass)
{
    const Typed::Program& program = encoder.program;
    const Typed::Layout& layout = program.at(id);
    const Typed::PlannedLayout& planned = encoder.plan.layout(id);
    const Typed::ClassCreationPlan& creation = encoder.plan.slots().classCreation(id);
    bool isClass = layout.kind == LayoutKind::Instance;

    const FieldsOfLayoutRecord& fieldsOfRecord = encoder.firstParts->fieldsOfLayoutRecords[id.index()];
    LayoutData data = fieldsOfRecord.flags;
    const Vector<uint32_t>& fieldOperands = fieldsOfRecord.operands;
    const Vector<FieldData>& fields = fieldsOfRecord.fields;
    LayoutRecordParts parts;
    if (keepsKeyOrder)
        collectKeyOrdersOfLayout(encoder, id, parts);
    parts.references = collectReferencesOfLayout(encoder, id);
    parts.shapeThatParsersMake = findShapeThatParsersMake(encoder, id, fields.span());
    ClassRecordParts classParts;
    if (isClass) {
        classParts = collectClassRecordParts(encoder, id);
        if (classParts.methods.size() != static_cast<size_t>(classParts.data.numberOfMethods) + classParts.data.numberOfStaticMethods || classParts.staticFields.size() != classParts.data.numberOfStaticFields) {
            encoder.fail("CountsOfClassRecordDifferFromItsLists"_s, id);
            return { };
        }
        size_t endOfBirth = fields.size();
        while (endOfBirth && !fields[endOfBirth - 1].isFilledAtBirth && !fields[endOfBirth - 1].holdsRawValueFromBirth)
            --endOfBirth;
        classParts.data.numberOfFieldsToLastOfBirth = static_cast<uint32_t>(endOfBirth);
        parentThatListsClass = classParts.parentThatListsClass;
        parts.namedGuards = WTF::move(classParts.namedGuards);
        LayoutID classLayout = Typed::declaredLayoutOf(program, id);
        data.evaluationBits = static_cast<uint32_t>(classOfLayoutRecordIsEvaluatedOnce(encoder.inputs, classLayout ? classLayout : id) ? EvaluationState::ProvenOnce : EvaluationState::PredictedOnce);
    }
    data.kindBits = static_cast<uint32_t>(layout.kind);
    data.isDerived = layout.isDerived();
    data.isUntyped = isUntypedLayout(encoder.inputs, id);
    data.hasUntypedBase = layout.has(Typed::LayoutFlags::HasHeritage) && encoder.facts.classOf(id).baseState != Typed::BaseState::Layout;
    data.createsMembersLazily = creation.createsMembersLazily;
    data.createsPrototypeBeforeClassObject = creation.createsPrototypeBeforeClassObject;
    data.createsClassObjectOnMutation = creation.createsClassObjectOnMutation;
    data.hasExtensionData = planned.hasExtension();
    data.hasExtraPropertiesSlot = planned.hasExtraPropertiesWord();
    data.index = id.index();
    data.size = planned.size;
    data.numberOfFields = static_cast<uint16_t>(fields.size());
    data.numberOfFieldOperands = static_cast<uint16_t>(fieldOperands.size());

    parts.prefixSize = static_cast<uint32_t>(LayoutData::prefixSize(isClass, data.hasExtensionData, data.hasExtraPropertiesSlot));
    uint64_t trailingSize = LayoutData::trailingSize({
        .fields = fields.size(),
        .fieldOperands = fieldOperands.size(),
        .methods = classParts.methods.size(),
        .staticFields = classParts.staticFields.size(),
    });
    parts.record = Vector<uint8_t>(FillWith { }, parts.prefixSize + sizeof(LayoutData) + trailingSize, 0);
    auto& record = reinterpretCastSpanStartTo<LayoutData>(parts.record.mutableSpan().subspan(parts.prefixSize)) = data;
    if (isClass)
        record.classData() = classParts.data;
    if (data.hasExtensionData) {
        record.extensionData() = ExtensionLayoutData {
            encoder.plan.slots().extensionWord(id).data(),
            planned.extensionSize,
            static_cast<uint32_t>(planned.granulesOfFirstExtension),
            static_cast<uint32_t>(planned.granulesOfFirstExtension >> 32),
        };
    }
    if (data.hasExtraPropertiesSlot)
        record.slotOfExtraProperties() = encoder.plan.slots().extraPropertiesWord(id).data();

    std::span<uint8_t> room = parts.record.mutableSpan().subspan(parts.prefixSize + sizeof(LayoutData));
    writeIntoRoomOfLayoutRecord(room, fields.span());
    writeIntoRoomOfLayoutRecord(room, fieldOperands.span());
    writeIntoRoomOfLayoutRecord(room, classParts.methods.span());
    writeIntoRoomOfLayoutRecord(room, classParts.staticFields.span());
    RELEASE_ASSERT(room.empty());
    return parts;
}

static Vector<LayoutRecordParts> collectLayoutRecordParts(TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    Vector<LayoutID> layouts;
    BitVector isListed(program.layouts.size());
    BitVector keepsKeyOrder(program.layouts.size());
    forEachLayoutRecord(encoder.inputs, [&](LayoutID id, uint32_t shape) {
        const Typed::Layout& layout = program.at(id);
        bool isShapeOfObject = shape < shapes.firstEnvironmentShape();
        if (isShapeOfObject ? !isObjectLayout(layout.kind) : !layout.isEnvironment()) {
            encoder.fail(isShapeOfObject ? "ShapeOfObjectGivenToEnvironment"_s : "ShapeOfEnvironmentGivenToObjectLayout"_s, id);
            return;
        }
        if (isShapeOfObject)
            keepsKeyOrder.quickSet(id.index());
        if (!isListed.quickSet(id.index()))
            layouts.append(id);
    });

    askRowsOfClassesForClassRecords(encoder);
    Vector<LayoutRecordParts> records(program.layouts.size());
    Vector<LayoutID> parentsThatListClasses(FillWith { }, program.layouts.size(), LayoutID());
    forEachIndexInParallel(encoder.build.parallelLoops(), static_cast<unsigned>(layouts.size()), [&](unsigned index) {
        LayoutID id = layouts[index];
        if (program.at(id).isEnvironment())
            records[id.index()] = collectPartsOfEnvironmentRecord(encoder, id);
        else
            records[id.index()] = collectPartsOfObjectLayoutRecord(encoder, id, keepsKeyOrder.quickGet(id.index()), parentsThatListClasses[id.index()]);
    });
    groupClassesByParentInClassRecords(encoder, parentsThatListClasses.span());
    return records;
}

static int32_t distanceFromLayoutRecord(uint32_t recordOffset, uint32_t targetOffset)
{
    return targetOffset ? static_cast<int32_t>(targetOffset) - static_cast<int32_t>(recordOffset) : 0;
}

static uint32_t appendReferencesOfLayout(TablePart& part, const LayoutRecordParts& parts)
{
    return parts.references.isEmpty() ? 0 : part.append(parts.references);
}

static uint32_t appendKeyOrdersOfLayout(TablePart& part, const LayoutRecordParts& parts)
{
    Vector<uint32_t, 8> orders;
    for (std::span<const uint16_t> lists = parts.keyOrders.span(); !lists.empty();) {
        size_t sizeOfList = 1 + static_cast<size_t>(lists[0]);
        orders.append(part.append(lists.first(sizeOfList)));
        lists = lists.subspan(sizeOfList);
    }
    return orders.isEmpty() ? 0 : part.appendOffsets(orders.span());
}

static uint32_t appendEnvironmentRecord(TableEncoder& encoder, const LayoutRecordParts& parts, uint32_t shape)
{
    TablePart& part = encoder.part;
    uint32_t referencesOffset = appendReferencesOfLayout(part, parts);
    uint32_t offset = part.allocateUnfilled(parts.record.size(), alignof(EnvironmentData));
    part.write(offset, parts.record.span());
    auto& data = part.at<EnvironmentData>(offset);
    data.shape = shape;
    data.referencesOffset = distanceFromLayoutRecord(offset, referencesOffset);
    return offset;
}

static uint32_t appendObjectLayoutRecord(TableEncoder& encoder, LayoutID id, const LayoutRecordParts& parts, uint32_t shapeNumber)
{
    Typed::PlacedShape shape = shapeNumber ? encoder.plan.shapes().shape(shapeNumber) : Typed::PlacedShape { };
    const LayoutData& asked = reinterpretCastSpanStartTo<LayoutData>(parts.record.span().subspan(parts.prefixSize));
    TablePart& part = encoder.part;

    // Whatever appends to the part comes in front of the allocation of the record: an append may move the bytes.
    uint32_t ordersOffset = appendKeyOrdersOfLayout(part, parts);
    uint32_t referencesOffset = appendReferencesOfLayout(part, parts);
    ClassData classData { };
    if (asked.isClass()) {
        classData = asked.classData();
        appendListsOfClassRecord(encoder, id, parts.namedGuards, classData);
    }

    uint32_t offset = part.allocateUnfilled(parts.record.size(), alignof(LayoutData)) + parts.prefixSize;
    part.write(offset - parts.prefixSize, parts.record.span());
    auto& record = part.at<LayoutData>(offset);
    if (encoder.facts.linked().layout(id).reachesUnseenCode && !shape.impossibleMutations.isEmpty())
        encoder.fail("RecordRulesOutWhatUnseenCodeMayDo"_s, id);
    record.impossibleMutationsRaw = shape.impossibleMutations.toRaw();
    record.isSaidNeverToReachUnseenCode = AOT::hasFlag(encoder.build.options().testing, Wire::TestingSwitch::CheckProofsAtRunTime) && !encoder.facts.linked().layout(id).reachesUnseenCode;
    record.shape = shapeNumber;
    record.numberOfOrders = parts.numberOfKeyOrders;
    record.referencesOffset = distanceFromLayoutRecord(offset, referencesOffset);
    record.ordersOffset = ordersOffset;

    part.link(offset, &LayoutData::ordersOffset);
    if (record.isClass()) {
        record.classData() = classData;
        part.link(offset - parts.prefixSize, &ClassData::subclassesOffset);
        part.link(offset - parts.prefixSize, &ClassData::propertiesOfHeirsOffset);
        part.linkWords(offset - parts.prefixSize + static_cast<uint32_t>(offsetof(ClassData, namedGuardsOffset)), numberOfClassSides);
    }
    return offset;
}

static void encodeLayoutRecords(TableEncoder& encoder, std::span<const LayoutRecordParts> records)
{
    const Typed::Program& program = encoder.program;
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    Vector<uint32_t> layouts(FillWith { }, shapes.numberOfShapes() + 1, 0);
    Vector<uint32_t> classes(FillWith { }, program.layouts.size(), 0);
    uint64_t mostBytesOfTables = layouts.sizeInBytes() + 2 * classes.sizeInBytes() + program.layouts.size() * sizeof(ConstructorData);
    mostBytesOfTables += static_cast<uint64_t>(encoder.numbers.allocations().size()) * (sizeof(AllocationRow) + sizeof(AllocationData) + sizeof(ConstructedAllocationData));
    for (size_t index = 0; index < records.size(); ++index) {
        if (records[index].shapeThatParsersMake)
            mostBytesOfTables += encoder.firstParts->fieldsOfLayoutRecords[index].fields.size() * (sizeof(Number24) + sizeof(uint32_t));
    }
    forEachLayoutRecord(encoder.inputs, [&](LayoutID id, uint32_t) {
        const LayoutRecordParts& parts = records[id.index()];
        constexpr uint64_t mostPaddingOfRecord = 4 * alignof(uint64_t);
        mostBytesOfTables += parts.record.size() + parts.keyOrders.sizeInBytes() + parts.numberOfKeyOrders * (sizeof(uint32_t) + alignof(uint64_t)) + parts.references.sizeInBytes() + mostPaddingOfRecord;
    });
    encoder.part.reserveRoomFor(mostBytesOfTables);
    forEachLayoutRecord(encoder.inputs, [&](LayoutID id, uint32_t shape) {
        const LayoutRecordParts& parts = records[id.index()];
        if (!shape)
            classes[id.index()] = appendObjectLayoutRecord(encoder, id, parts, 0);
        else
            layouts[shape] = shape < shapes.firstEnvironmentShape() ? appendObjectLayoutRecord(encoder, id, parts, shape) : appendEnvironmentRecord(encoder, parts, shape);
    });

    size_t numberOfGroupWords = (classes.size() + 31) / 32 * 2;
    Vector<uint32_t> classWords(FillWith { }, numberOfGroupWords, 0);
    uint32_t numberOfClasses = 0;
    bool ranksAgree = true;
    for (size_t index = 0; index < classes.size(); ++index) {
        LayoutID id(index);
        if (!(index % 32))
            classWords[index / 32 * 2 + 1] = numberOfClasses;
        bool hasRecord = hasRecordOfClass(encoder.inputs, id);
        if (ranksAgree && encoder.numbers.rankOfClass(id) != (hasRecord ? std::optional(numberOfClasses) : std::nullopt)) {
            encoder.fail("RankOfClassDiffersFromItsPlaceAmongClassRecords"_s, id);
            ranksAgree = false;
        }
        if (!hasRecord)
            continue;
        if (!shapes.shapesOf(id).isEmpty())
            classes[index] = layouts[shapes.generalShapeOf(id)];
        if (!classes[index]) {
            encoder.fail("ClassWithoutRecord"_s, id);
            continue;
        }
        classWords[index / 32 * 2] |= 1u << index % 32;
        classWords.append(classes[index]);
        ++numberOfClasses;
    }

    TablePart& part = encoder.part;
    part.setTableRange(ProgramTableIndex::layouts, appendOffsetsInLayoutTables(part, layouts.span()), static_cast<uint32_t>(layouts.size()));
    part.setTableRange(ProgramTableIndex::classes, appendOffsetsInLayoutTables(part, classWords.span(), numberOfGroupWords), static_cast<uint32_t>(classWords.size()));
}

static bool allocationRowLeavesFieldOfBirthEmpty(const TableEncoder& encoder, const AllocationKey& key, BitVector& isInitialField)
{
    LayoutID layout = key.allocation.layout;
    if (!layout || !isObjectLayout(encoder.program.at(layout).kind))
        return false;
    const Typed::PlannedLayout& planned = encoder.plan.layout(layout);
    isInitialField.clearAll();
    for (FieldIndex field : key.initialFields) {
        if (field < planned.numberOfFields())
            isInitialField.set(field);
        else
            encoder.fail("AllocationListsFieldThatItsLayoutLacks"_s, layout);
    }
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].isFilledAtBirth && !isInitialField.get(field))
            return true;
    }
    return false;
}

void AllocationRows::append(const AllocationData& allocation)
{
    static_assert(AllocationData::maxSize / sizeof(uint64_t) < 1u << 30);
    uint64_t bits = allocation.orderWord() | static_cast<uint64_t>(allocation.size() / sizeof(uint64_t)) << 32 | static_cast<uint64_t>(allocation.isBornUnfinished) << 62 | static_cast<uint64_t>(allocation.isConstructed) << 63;
    uint32_t number = numbersOfForms.ensure(bits, [&] {
        AllocationData form = allocation;
        form.shape = Number24();
        forms.append(form);
        return static_cast<uint32_t>(forms.size() - 1);
    }).iterator->value;
    rows.append({ allocation.shape, Number24(number) });
}

static AllocationRows collectAllocationRows(const TableEncoder& encoder)
{
    const NumberedKeys& keys = encoder.numbers.allocations();
    AllocationRows rows;
    Vector<ConstructedAllocationData>& constructedAllocations = rows.constructedAllocations;
    rows.rows.reserveInitialCapacity(keys.size());
    BitVector isInitialField;
    for (uint32_t row = 0; row < keys.size(); ++row) {
        AllocationKey key = decodeAllocationKey(keys.keyAt(row));
        const Typed::PlacedAllocation& allocation = key.allocation;
        bool leavesFieldEmpty = allocationRowLeavesFieldOfBirthEmpty(encoder, key, isInitialField);
        if (leavesFieldEmpty && !allocation.isBornUnfinished)
            encoder.fail("AllocationBornFinishedLeavesFieldOfBirthEmpty"_s, allocation.layout);
        if (row > Number24::max || allocation.shape > Number24::max || allocation.size > AllocationData::maxSize || allocation.size % sizeof(uint64_t)) {
            encoder.fail("AllocationDoesNotFitItsRow"_s, allocation.layout);
            rows.append(AllocationData::create(0, 0, 0, true, false));
            continue;
        }
        uint32_t constructor = allocation.constructedBy ? encoder.entrypointOf(allocation.constructedBy, "the constructor of an allocation of"_s, allocation.layout) : UINT32_MAX;
        bool isConstructed = constructor <= Number24::max;
        if (isConstructed)
            constructedAllocations.append({ Number24(row), Number24(constructor) });
        rows.append(AllocationData::create(allocation.shape, allocation.orderWord, allocation.size, allocation.isBornUnfinished || leavesFieldEmpty, isConstructed));
    }
    return rows;
}

struct ParsedRecordTables {
    Vector<uint32_t> names;
    Vector<uint32_t> starts;
    Vector<Number24> shapes;
};

static ParsedRecordTables collectParsedRecords(const TableEncoder& encoder, std::span<const LayoutRecordParts> records)
{
    Vector<uint64_t> keys;
    for (size_t index = 0; index < records.size(); ++index) {
        uint32_t shape = records[index].shapeThatParsersMake;
        if (!shape)
            continue;
        const auto& fields = encoder.firstParts->fieldsOfLayoutRecords[index].fields;
        for (const FieldData& field : fields)
            keys.append(static_cast<uint64_t>(field.name) << 40 | static_cast<uint64_t>(static_cast<uint16_t>(fields.size())) << 24 | shape);
    }
    radixSortKeepingEqualKeysInOrder<64>(keys, [](uint64_t key) {
        return key;
    });
    if (keys.isEmpty())
        return { };
    Vector<uint32_t> placeOfName(FillWith { }, encoder.numbers.identifiers().count(), 0);
    Vector<uint32_t> starts;
    ParsedRecordTables tables;
    tables.shapes.reserveInitialCapacity(keys.size());
    for (uint64_t key : keys) {
        uint32_t& place = placeOfName[key >> 40];
        if (!place) {
            starts.append(tables.shapes.size());
            place = starts.size();
        }
        tables.shapes.append(Number24(static_cast<uint32_t>(key) & Number24::max));
    }
    starts.append(tables.shapes.size());
    tables.names = SparseNumberTable::create(placeOfName.span());
    tables.starts = AscendingNumberTable::create(starts.span());
    return tables;
}

static Vector<ConstructorData> collectConstructorsOfLayouts(const TableEncoder& encoder)
{
    Vector<ConstructorData> constructors;
    for (size_t index = 0; index < encoder.program.layouts.size(); ++index) {
        LayoutID id(index);
        FunctionID constructor = encoder.program.at(id).constructor;
        if (!constructor || !(encoder.plan.isAllocatable(id) || hasRecordOfClass(encoder.inputs, id)))
            continue;
        if (std::optional<FunctionID> entrypoint = encoder.findEntrypoint(constructor))
            constructors.append({ entrypoint->index(), static_cast<uint32_t>(index) });
    }
    AOT::sortInLine(constructors, [](const ConstructorData& a, const ConstructorData& b) {
        return std::tie(a.function, a.layoutIndex) < std::tie(b.function, b.layoutIndex);
    });
    return constructors;
}

static Vector<ClosurePropertyNameData> collectClosurePropertyNames(const TableEncoder& encoder)
{
    Vector<ClosurePropertyNameData> names;
    for (const Typed::LinkedNameOnFunction& laid : encoder.facts.linked().namesOnFunctions) {
        if (!encoder.plan.slots().closuresGetOnlyListedNames(laid.holder))
            continue;
        if (std::optional<FunctionID> entrypoint = encoder.findEntrypoint(laid.holder))
            names.append({ entrypoint->index(), encoder.identifierNumber(laid.key, "a name laid on the closures of a function"_s) });
    }
    AOT::sort(names);
    return names;
}

static Vector<AddedPrototypeNameData> collectAddedPrototypeNames(const TableEncoder& encoder)
{
    Vector<AddedPrototypeNameData> names;
    for (const Typed::LinkedNameOnPrototype& laid : encoder.facts.linked().namesOnPrototypes) {
        if (isAddedToPrototype(encoder.inputs, laid))
            names.append({ laid.layout.index(), encoder.identifierNumber(laid.key, "a name laid on the prototype object of"_s, laid.layout) });
    }
    AOT::sort(names);
    return names;
}

static Vector<FieldGetterData> collectFieldGetters(const TableEncoder& encoder)
{
    Vector<FieldGetterData> getters;
    for (const auto& [getter, field] : encoder.inputs.bodyFacts.fieldsReturnedByGetters) {
        std::optional<uint32_t> name = encoder.numbers.identifiers().numberOf(field);
        std::optional<FunctionID> entrypoint = name ? encoder.findEntrypoint(getter) : std::nullopt;
        if (entrypoint)
            getters.append({ entrypoint->index(), *name });
    }
    AOT::sort(getters);
    return getters;
}

void encodeLayoutTables(TableEncoder& encoder)
{
    static constexpr ASCIILiteral nameOfSteps = "image: layout tables"_s;
    StepTimes times(encoder.build, nameOfSteps);
    Vector<LayoutRecordParts> records = collectLayoutRecordParts(encoder);
    times.didFinish("parts of records"_s);
    if (encoder.build.diagnostics().hasErrors())
        return;
    ParsedRecordTables parsedRecords;
    AllocationRows allocationRows;
    Vector<ConstructorData> constructors;
    runLanes(encoder.build.parallelLoops(), {
        [&] {
            encodeLayoutRecords(encoder, records.span());
            times.didFinish("records"_s);
        },
        [&] {
            StepTimes timesOfLane(encoder.build, nameOfSteps);
            parsedRecords = collectParsedRecords(encoder, records.span());
            timesOfLane.didFinish("parsed records"_s);
        },
        [&] {
            StepTimes timesOfLane(encoder.build, nameOfSteps);
            allocationRows = collectAllocationRows(encoder);
            timesOfLane.didFinish("allocation rows"_s);
            constructors = collectConstructorsOfLayouts(encoder);
            timesOfLane.didFinish("constructors"_s);
        },
    });
    destroyInBackground(encoder.build.backgroundJobs(), WTF::move(records));
    encoder.part.setTable(ProgramTableIndex::parsedRecordNames, parsedRecords.names);
    encoder.part.setTable(ProgramTableIndex::parsedRecordStarts, parsedRecords.starts);
    encoder.part.setTable(ProgramTableIndex::parsedRecordShapes, parsedRecords.shapes);
    encoder.part.setTable(ProgramTableIndex::allocationRows, allocationRows.rows);
    encoder.part.setTable(ProgramTableIndex::allocationForms, allocationRows.forms);
    encoder.part.setTable(ProgramTableIndex::constructedAllocations, allocationRows.constructedAllocations);
    encoder.part.setTable(ProgramTableIndex::constructors, constructors);
    encoder.part.setTable(ProgramTableIndex::closurePropertyNames, collectClosurePropertyNames(encoder));
    encoder.part.setTable(ProgramTableIndex::addedPrototypeNames, collectAddedPrototypeNames(encoder));
    encoder.part.setTable(ProgramTableIndex::fieldGetters, collectFieldGetters(encoder));
    times.didFinish("append the tables behind the records"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
