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
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

// A field that the record of a layout lists: as the program declares it, or as Facts appended it.
struct FieldOfLayoutRecord {
    PropertyKeyID key;
    OptionSet<Typed::FieldFlags> flags;
    OptionSet<Typed::FieldForm> form;
};

// What the record of a layout says whatever the shape it is written for, and the lists that lie in front of it.
// The group appends on one thread. So all of this is asked in a loop in front of that, once a layout.
struct LayoutRecordParts {
    // As it lies in the part. Of an object: what LayoutData::prefixSize() counts, the LayoutData, what trailingSize() counts. Of an environment: the EnvironmentData, the described variables.
    // 0 where the shape decides, and where the place of a list stands. Empty: the layout has no record.
    Vector<uint8_t> record;
    // One KeyOrderData behind the other. None where no shape of the layout keeps an order.
    Vector<uint16_t> keyOrders;
    // What referencesOffset leads to. Empty: it is 0.
    Vector<uint32_t> references;
    // ClassRecordParts::namedGuards.
    std::array<Vector<uint32_t>, numberOfClassSides> namedGuards;
    uint32_t prefixSize { 0 };
    uint32_t numberOfKeyOrders { 0 };
    // Under which parsedRecordFields lists the fields of the record. 0: it lists none of them.
    uint32_t shapeThatParsersMake { 0 };
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

// The form is the one against which Facts says that a field is never redefined, and by which the walk plans accesses. What definitions alone give a declared field stands in no table of the program.
static FieldOfLayoutRecord describeFieldOfLayoutRecord(const TableInputs& inputs, Typed::FieldReference reference, const Typed::FieldFacts& facts)
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
    // No object is required to have an appended field.
    field.key = inputs.plan.slots().keyOf(program, reference);
    // PlannedLayout::environmentOfDeferredClosures: the compiler keeps it for itself, and no name finds it.
    if (!field.key) {
        field.flags.add(Typed::FieldFlags::IsHidden);
        return field;
    }
    const Typed::PropertyKey& name = program.at(field.key);
    if (isPrivateNameKindForLayoutRecord(name.kind))
        field.flags.add(Typed::FieldFlags::IsPrivate);
    // The class that declares a private name says what kind of name an object of another layout holds.
    if (name.kind == PropertyKeyKind::PrivateName) {
        constexpr OptionSet<Typed::FieldFlags> flagsOfPrivateNames { Typed::FieldFlags::IsPrivate, Typed::FieldFlags::IsImmutable, Typed::FieldFlags::IsBrand, Typed::FieldFlags::IsHidden, Typed::FieldFlags::IsBrandOfMethods };
        const Typed::Layout& owner = program.at(name.owner);
        if (std::optional<uint32_t> declared = owner.findField(field.key))
            field.flags.add(owner.fields[*declared].flags & flagsOfPrivateNames);
    }
    return field;
}

// FieldData::type: what the run time tests a value against that it stores into the slot.
static TypeID typeRecordedForSlot(const Typed::PlacedSlot& slot)
{
    TypeID type = slot.typeInLayoutRecord();
    return type ? Typed::TypeTable::valuesOf(type) : Typed::TypeTable::anything;
}

// A class of which no object is ever made is still defined, and the classes below it name it as their parent. So Plan alone says which classes have a record.
static bool hasRecordOfClass(const TableInputs& inputs, LayoutID id)
{
    return inputs.program.at(id).kind == LayoutKind::Instance && inputs.plan.hasClassRecord(id);
}

// The records in the order in which they are written. Shape 0: the record of a class whose objects have no shape.
static void forEachLayoutRecord(const TableInputs& inputs, NOESCAPE const Invocable<void(LayoutID, uint32_t shape)> auto& functor)
{
    const Typed::ShapeTable& shapes = inputs.plan.shapes();
    for (uint32_t shape = 1; shape <= shapes.numberOfShapes(); ++shape) {
        if (shapes.isAssigned(shape) && inputs.plan.isInUse(shapes.shape(shape).layout))
            functor(shapes.shape(shape).layout, shape);
    }
    for (size_t index = 0; index < inputs.program.layouts.size(); ++index) {
        LayoutID id(index);
        if (hasRecordOfClass(inputs, id) && shapes.shapesOf(id).isEmpty())
            functor(id, 0);
    }
}

// The variables that hold a cell, and those whose empty state is not zero.
static void forEachDescribedVariableOfEnvironment(const TableInputs& inputs, LayoutID id, bool obfuscates, NOESCAPE const Invocable<void(const Typed::PlacedSlot&, StringID shownName)> auto& functor)
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
        functor(slot, showsVariables && traits.holdsCell ? name : StringID());
    }
}

// ---- What the tables name

static void nameStringShownByLayoutTables(const TableInputs& inputs, StringID name, NamesOfTables& names)
{
    StringID shown = name && !inputs.options.obfuscates() ? name : inputs.program.strings.emptyString();
    names.strings.set(shown.index());
}

static void nameWhatObjectLayoutRecordNames(const TableInputs& inputs, LayoutID id, NamesOfTables& names)
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
    nameStringShownByLayoutTables(inputs, layout.name, names);
    if (layout.kind != LayoutKind::Instance)
        return;
    // For the parts of the record that AOTClassRecords.cpp collects, and for the rows of classes in the table of fields.
    for (const Typed::StaticField& field : layout.staticFields)
        nameKey(field.key);
    for (const Typed::ClassMember& member : inputs.facts.membersOf(id)) {
        nameKey(member.key);
        nameClosureCell(member.function);
    }
    nameClosureCell(layout.constructor);
}

static void nameWhatEnvironmentRecordNames(const TableInputs& inputs, LayoutID id, NamesOfTables& names)
{
    forEachDescribedVariableOfEnvironment(inputs, id, inputs.options.obfuscates(), [&](const Typed::PlacedSlot&, StringID shownName) {
        if (shownName)
            names.strings.set(shownName.index());
    });
    nameStringShownByLayoutTables(inputs, inputs.program.at(id).name, names);
}

void nameWhatLayoutTablesName(const TableInputs& inputs, const GatheredMentions&, NamesOfTables& names)
{
    uint32_t firstEnvironmentShape = inputs.plan.shapes().firstEnvironmentShape();
    forEachLayoutRecord(inputs, [&](LayoutID id, uint32_t shape) {
        if (!shape || shape < firstEnvironmentShape)
            nameWhatObjectLayoutRecordNames(inputs, id, names);
        else
            nameWhatEnvironmentRecordNames(inputs, id, names);
    });
}

// ---- Records of layouts

// A word that is linked changes when the parts are joined. So it is never shared with equal bytes, which may be no offsets.
static uint32_t appendOffsetsInLayoutTables(TablePart& part, std::span<const uint32_t> words, size_t numberOfWordsInFrontOfOffsets = 0)
{
    uint32_t place = part.appendUnshared(asBytes(words), alignof(uint32_t));
    part.linkWords(place + static_cast<uint32_t>(numberOfWordsInFrontOfOffsets * sizeof(uint32_t)), words.size() - numberOfWordsInFrontOfOffsets);
    return place;
}

static bool slotOfLayoutRecordTakesEveryValue(const Typed::Program& program, const FieldOfLayoutRecord& field, const Typed::FieldFacts& facts, const Typed::PlacedSlot& slot)
{
    if (field.form.contains(Typed::FieldForm::IsAccessor) || (facts.facts.contains(Typed::FieldFact::IsEnforced) && slot.type) || facts.location.deferral != Typed::ClosureDeferral::Unknown)
        return false;
    return slot.representation == SlotRepresentation::JSValue && program.at(typeRecordedForSlot(slot)).isAnything();
}

// The run time stops at whatever takes such a field out of the state that it was born in, so this claims no more than Facts has proven.
static bool fieldOfLayoutRecordKeepsStateOfBirth(const Typed::Program& program, const FieldOfLayoutRecord& field, const Typed::FieldFacts& facts, const Typed::PlacedSlot& slot)
{
    if (facts.occupancy == Typed::Occupancy::Unknown || !facts.stability.containsAll({ Typed::FieldStability::IsNeverDeleted, Typed::FieldStability::IsNeverRedefined }))
        return false;
    return facts.stability.contains(Typed::FieldStability::IsNeverStoredWithOtherType) || slotOfLayoutRecordTakesEveryValue(program, field, facts, slot);
}

// FieldStability::IsNeverStoredWithOtherType speaks of the type that linked() has proven of the field. The run time stops at a value that the SLOT refuses. A slot may be held to less than is proven (Facts::held()):
// there a value that does not fit is no false proof, the field leaves its slot, and the walk looks: describeFieldOfTypedAccess(), everyStoreFits.
static bool slotOfLayoutRecordTakesWhatIsProven(const TableEncoder& encoder, Typed::FieldReference reference, const Typed::PlacedSlot& slot)
{
    const Typed::LinkedCell* cell = encoder.facts.linked().findCellOfField(reference);
    if (!cell || !cell->proven)
        return false;
    return !slot.type || Typed::isSubset(Typed::TypeReader(encoder.program), encoder.program.at(cell->proven), encoder.program.at(slot.type));
}

static uint16_t flagsOfFieldInLayoutRecord(const FieldOfLayoutRecord& field, const Typed::FieldFacts& facts, const Typed::PlacedSlot& slot, bool slotTakesWhatIsProven)
{
    using Typed::FieldFlags;
    using Typed::FieldForm;
    using Typed::FieldStability;
    const std::pair<bool, uint16_t> flags[] = {
        { !field.flags.contains(FieldFlags::IsRequired), fieldIsOptional },
        { slot.isOutOfLine(), fieldIsOutOfLine },
        // Object::tryStore(): a slot without an absent state holds a placeholder until its only store, which may come by name: such a store is like any other.
        { facts.location.stores == Typed::StoreCount::AtMostOnce && canLeaveSlot(slot.representation), fieldIsStoredOnce },
        { slot.mayExistWithoutStorage, fieldMayExistWithoutStorage },
        { slot.deferredClosureCapturesObject, fieldDeferredClosureCapturesObject },
        { slot.deferredClosureCapturesEnvironment, fieldDeferredClosureCapturesEnvironment },
        { field.flags.contains(FieldFlags::IsPrivate) || (field.flags.contains(FieldFlags::IsHidden) && !field.key), fieldIsPrivate },
        { field.flags.contains(FieldFlags::IsBrand) && facts.location.reads != Typed::ReadState::NeverRead, fieldIsBrand },
        { facts.stability.contains(FieldStability::IsNeverDeleted), fieldIsNeverDeleted },
        { facts.stability.contains(FieldStability::IsNeverRedefined), fieldIsNeverRedefined },
        { facts.stability.contains(FieldStability::IsNeverStoredWithOtherType) && slotTakesWhatIsProven, fieldIsNeverStoredWithOtherType },
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

// UINT32_MAX: no code makes an object of the layout, so the entry of the function has no code and nothing ever asks for the closure.
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
    for (FieldIndex index = 0; index < planned.numberOfFields(); ++index) {
        const Typed::PlacedSlot& slot = planned.slots[index];
        if (!slot.indexInLayoutRecord)
            continue;
        // Compiled code has the place of a record by this number: SlotPlan::recordOfStoredField().
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
        data.keepsStateOfBirth = fieldOfLayoutRecordKeepsStateOfBirth(program, field, facts, slot);
        data.slotHoldsUndefinedAndNull = facts.facts.contains(Typed::FieldFact::SlotHoldsUndefinedAndNull);
        data.flags = flagsOfFieldInLayoutRecord(field, facts, slot, slotOfLayoutRecordTakesWhatIsProven(encoder, reference, slot));
        data.slot = slot.data();
        data.type = encoder.typeOffset(type);
        // validateMutation(), willLeaveStateOfBirth() and Object::moveFieldToExtraProperties() end the process at what the record rules out. Code that the build has not seen may do anything to what it gets hold of,
        // and what is held of such an object (Facts::held()) is no proof: there the deed is carried out and leaves its mark.
        if (reachesUnseenCode && (data.flags & (fieldIsNeverDeleted | fieldIsNeverRedefined | fieldIsNeverStoredWithOtherType) || data.keepsStateOfBirth || (slot.hasStorage() && !data.canLeaveSlot())))
            encoder.fail("RecordRulesOutWhatUnseenCodeMayDo"_s, id);
        if (slot.representation == SlotRepresentation::DeferredClosure) {
            data.operandIndex = static_cast<uint16_t>(operands.size());
            operands.append(closureNumberForLayoutRecord(encoder, slot.deferredFunction, id));
            if (slot.deferredClosureCapturesEnvironment) {
                std::optional<uint32_t> environmentField = planned.environmentOfDeferredClosures;
                const Typed::PlacedSlot& environmentSlot = encoder.plan.slot(id, environmentField.value_or(static_cast<uint32_t>(planned.slots.size())));
                if (environmentSlot.storage != Typed::SlotStorage::Inline || traitsOf(environmentSlot.representation).valueKind != SlotValueKind::Cell)
                    encoder.fail("DeferredClosureWithoutSlotOfEnvironment"_s, id);
                operands.append(environmentSlot.data().encoded());
            }
        } else if (slot.deferredClosureCapturesEnvironment || slot.deferredClosureCapturesObject)
            encoder.fail("CapturesOfClosureThatIsNotDeferred"_s, id);

        // A store under __proto__ changes the prototype, so a copy by stores looks at that name.
        bool isNamedProto = field.key && program.at(field.key).kind == PropertyKeyKind::Text && program.nameClassOf(field.key) == NameClass::Proto;
        bool isCopiedByWords = !(data.flags & (fieldIsPrivate | fieldIsBrand | fieldAttributeFlags)) && slot.representation != SlotRepresentation::DeferredClosure && !isNamedProto;
        layoutData.isCopiedByWords &= isCopiedByWords;
        layoutData.hasOnlyPlainSlots &= isCopiedByWords && !(data.flags & fieldIsStoredOnce);
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
    forEachDescribedVariableOfEnvironment(encoder.inputs, id, encoder.obfuscates(), [&](const Typed::PlacedSlot& slot, StringID shownName) {
        if (slot.isOutOfLine())
            encoder.fail("VariableOfEnvironmentOutOfLine"_s, id);
        described.append({ shownName ? encoder.stringNumber(shownName) : VariableData::noName, slot.data() });
    });

    LayoutRecordParts parts;
    parts.references = collectReferencesOfLayout(encoder, id);
    parts.record = Vector<uint8_t>(FillWith { }, sizeof(EnvironmentData) + described.sizeInBytes(), 0);
    auto& data = reinterpretCastSpanStartTo<EnvironmentData>(parts.record.mutableSpan());
    data.kind = layout.kind;
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
            if (!program.at(typeRecordedForSlot(inputs.plan.slot(id, field))).isAnything())
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

// A constructor function whose closures hold a capture has a function object for each environment. The run time makes none of them by itself, and takes the first one for the class object only of a class that is predicted.
// As Typed::classHasOneClassObject() has it: a capture that the plan has removed is no word of a closure.
static bool classOfLayoutRecordIsEvaluatedOnce(const TableInputs& inputs, LayoutID classLayout)
{
    if (!inputs.facts.isEvaluatedAtMostOnce(classLayout))
        return false;
    const Typed::Layout& layout = inputs.program.at(classLayout);
    if (!layout.has(Typed::LayoutFlags::IsConstructorFunction))
        return true;
    return layout.constructor && inputs.program.at(layout.constructor).captures.size() == inputs.plan.promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor);
}

// Facts has counted on parsers that make such objects. The run time asks ObjectBuilder::canBuildWithoutCompilerKnowledge() of the record of each layout that is listed, and tests each value against its slot.
static uint32_t findShapeThatParsersMake(const TableEncoder& encoder, LayoutID id, std::span<const FieldData> fields)
{
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    if (encoder.program.at(id).kind != LayoutKind::Record || !encoder.plan.isInUse(id) || !encoder.plan.layout(id).holdsCompiledObjects() || !encoder.facts.parsersMayMake(id))
        return 0;
    uint32_t shape = shapes.generalShapeOf(id);
    if (!shapes.isAssigned(shape) || !shapes.shape(shape).impossibleMutations.isEmpty())
        return 0;
    if (shape > Number24::max) {
        encoder.fail("ShapeDoesNotFitRowOfParsedRecordFields"_s, id);
        return 0;
    }
    // No text names a field without a key, which has the name 0.
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
    data.name = encoder.shownNameNumber(layout.name);
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

// [LayoutID]. Only a layout that forEachLayoutRecord() gives a record is asked about: of any other no type is encoded, and to ask for one stops the build.
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

// Both lie in one part, so the distance stays what it is when the parts are joined.
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
    record.shape = shapeNumber;
    record.numberOfOrders = parts.numberOfKeyOrders;
    record.referencesOffset = distanceFromLayoutRecord(offset, referencesOffset);
    record.ordersOffset = ordersOffset;

    part.link(offset, &LayoutData::ordersOffset);
    part.linkInEach(offset + static_cast<uint32_t>(sizeof(LayoutData)), record.numberOfFields, &FieldData::type, TablePartID::Types);
    if (record.isClass()) {
        record.classData() = classData;
        part.link(offset - parts.prefixSize, &ClassData::subclassesOffset);
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
    forEachLayoutRecord(encoder.inputs, [&](LayoutID id, uint32_t shape) {
        const LayoutRecordParts& parts = records[id.index()];
        if (!shape)
            classes[id.index()] = appendObjectLayoutRecord(encoder, id, parts, 0);
        else
            layouts[shape] = shape < shapes.firstEnvironmentShape() ? appendObjectLayoutRecord(encoder, id, parts, shape) : appendEnvironmentRecord(encoder, parts, shape);
    });

    // Two words for every 32 layouts: which of them have a record of a class, and how many such records lie in front of them. Then the records.
    // Compiled code holds the rank of a class as Image numbered it, and the run time counts it out of these words.
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

// ---- Allocations

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

static void encodeAllocationRows(TableEncoder& encoder)
{
    const NumberedKeys& keys = encoder.numbers.allocations();
    Vector<AllocationData> allocations;
    Vector<ConstructedAllocationData> constructedAllocations;
    allocations.reserveInitialCapacity(keys.size());
    BitVector isInitialField;
    for (uint32_t row = 0; row < keys.size(); ++row) {
        AllocationKey key = decodeAllocationKey(keys.keyAt(row));
        const Typed::PlacedAllocation& allocation = key.allocation;
        // A cell that is born finished is read without the test of the cell, so a field of birth that it lacks would be read as a value.
        bool leavesFieldEmpty = allocationRowLeavesFieldOfBirthEmpty(encoder, key, isInitialField);
        if (leavesFieldEmpty && !allocation.isBornUnfinished)
            encoder.fail("AllocationBornFinishedLeavesFieldOfBirthEmpty"_s, allocation.layout);
        if (row > Number24::max || allocation.shape > Number24::max || allocation.size > AllocationData::maxSize || allocation.size % sizeof(uint64_t)) {
            encoder.fail("AllocationDoesNotFitItsRow"_s, allocation.layout);
            allocations.append(AllocationData::create(0, 0, 0, true, false));
            continue;
        }
        uint32_t constructor = allocation.constructedBy ? encoder.entrypointOf(allocation.constructedBy, "the constructor of an allocation of"_s, allocation.layout) : UINT32_MAX;
        bool isConstructed = constructor <= Number24::max;
        if (isConstructed)
            constructedAllocations.append({ Number24(row), Number24(constructor) });
        allocations.append(AllocationData::create(allocation.shape, allocation.orderWord, allocation.size, allocation.isBornUnfinished || leavesFieldEmpty, isConstructed));
    }
    encoder.part.setTable(ProgramTableIndex::allocations, allocations);
    encoder.part.setTable(ProgramTableIndex::constructedAllocations, constructedAllocations);
}

template<typename T>
static void setSortedLayoutTable(TableEncoder& encoder, ProgramTableIndex index, Vector<T>& items, auto... order)
{
    AOT::sortInLine(items, order...);
    encoder.part.setTable(index, items);
}

// ---- What the parser of untyped values makes compiled objects of

// findShapeThatParsersMake() has asked. A name whose number is too wide was reported for the record of the layout.
static void encodeParsedRecordFields(TableEncoder& encoder, std::span<const LayoutRecordParts> records)
{
    Vector<ParsedRecordFieldData> rows;
    for (size_t index = 0; index < records.size(); ++index) {
        uint32_t shape = records[index].shapeThatParsersMake;
        if (!shape)
            continue;
        for (const FieldData& field : encoder.firstParts->fieldsOfLayoutRecords[index].fields)
            rows.append({ Number24(field.name), Number24(shape) });
    }
    setSortedLayoutTable(encoder, ProgramTableIndex::parsedRecordFields, rows, std::ranges::less { }, &ParsedRecordFieldData::key);
}

// ---- Tables that map a function to its layout

static void encodeConstructorsOfLayouts(TableEncoder& encoder)
{
    Vector<ConstructorData> constructors;
    for (size_t index = 0; index < encoder.program.layouts.size(); ++index) {
        LayoutID id(index);
        FunctionID constructor = encoder.program.at(id).constructor;
        if (!constructor || !(encoder.plan.isInUse(id) || hasRecordOfClass(encoder.inputs, id)))
            continue;
        if (std::optional<FunctionID> entrypoint = encoder.findEntrypoint(constructor))
            constructors.append({ entrypoint->index(), static_cast<uint32_t>(index) });
    }
    setSortedLayoutTable(encoder, ProgramTableIndex::constructors, constructors, [](const ConstructorData& a, const ConstructorData& b) {
        return std::tie(a.function, a.layoutIndex) < std::tie(b.function, b.layoutIndex);
    });
}

void encodeLayoutTables(TableEncoder& encoder)
{
    StepTimes times(encoder.build, "image: layout tables"_s);
    Vector<LayoutRecordParts> records = collectLayoutRecordParts(encoder);
    times.didFinish("parts of records"_s);
    // What appends takes the parts for whole.
    if (encoder.build.diagnostics().hasErrors())
        return;
    encodeLayoutRecords(encoder, records.span());
    times.didFinish("records"_s);
    encodeParsedRecordFields(encoder, records.span());
    times.didFinish("fields of parsed records"_s);
    forEachRangeOfIndicesInParallel(encoder.build.parallelLoops(), records.size(), [&](size_t first, size_t end) {
        for (LayoutRecordParts& parts : records.mutableSpan().subspan(first, end - first))
            parts = { };
    });
    times.didFinish("free the parts of records"_s);
    encodeAllocationRows(encoder);
    times.didFinish("allocation rows"_s);
    encodeConstructorsOfLayouts(encoder);
    encoder.part.setTable(ProgramTableIndex::boundFunctions, Vector<BoundFunctionData>());
    times.didFinish("constructors, bound functions"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
