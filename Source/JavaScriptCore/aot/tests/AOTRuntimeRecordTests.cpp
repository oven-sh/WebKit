/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeWords.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <array>
#include <bit>
#include <optional>
#include <wtf/HashSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

std::span<const NativeTest> runtimeRecordTests();

namespace {

struct Cells {
    explicit Cells(Realms& realms)
        : vm(realms.vm())
        , receiver(realms.object(realms.plain(), "({ a: 1 })"_s))
        , holder(realms.object(realms.plain(), "({ b: 2 })"_s))
        , structure(receiver->structure())
        , otherStructure(holder->structure())
        , cageBase(MarkedBlockCage::baseOf(holder))
    {
        AOT_TEST_CHECK(structure != otherStructure);
    }

    StructureID id() const { return structure->id(); }
    StructureID otherID() const { return otherStructure->id(); }

    VM& vm;
    JSObject* receiver;
    JSObject* holder;
    Structure* structure;
    Structure* otherStructure;
    uintptr_t cageBase;
};

} // anonymous namespace

static constexpr uint16_t testName = 77;
static constexpr PropertyOffset lastOffsetInLocation = firstOutOfLineOffset + 32766;
static constexpr PropertyOffset firstFarOffset = firstOutOfLineOffset + 32767;
static constexpr PropertyOffset lastOffsetInPackedLocation = firstOutOfLineOffset + 126;
static constexpr PropertyOffset firstOffsetOutsidePackedLocation = firstOutOfLineOffset + 127;

static bool hasSameBits(const Slot& a, const Slot& b)
{
    return a.headerBits() == b.headerBits() && a.payloadBits() == b.payloadBits();
}

static uint64_t bitsOf(const PackedSlot& packed)
{
    return std::bit_cast<uint64_t>(packed);
}

static bool packsAndUnpacks(const Slot& slot, uintptr_t cageBase)
{
    std::optional<PackedSlot> packed = PackedSlot::tryPack(slot, testName, cageBase);
    if (!packed)
        return false;
    AOT_TEST_CHECK(!packed->isEmpty() && packed->name == testName);
    AOT_TEST_CHECK(hasSameBits(packed->unpack(slot.structureID, cageBase, slot.has(Slot::dependsOnChain)), slot));
    return true;
}

static void testZeroIsEmpty()
{
    Slot slot { };
    AOT_TEST_CHECK(slot.isEmpty() && slot.kind == Slot::Kind::Empty);
    AOT_TEST_CHECK(!slot.headerBits() && !slot.payloadBits());
    AOT_TEST_CHECK(slot.namesOnlyLiveCells([](JSCell*) { return false; }, 0));

    PackedSlot packed { };
    AOT_TEST_CHECK(packed.isEmpty() && !bitsOf(packed));
    AOT_TEST_CHECK(packed.unpack(StructureID(), 0, false).isEmpty());
    AOT_TEST_CHECK(hasSameBits(packed.unpack(StructureID(), 0, true), Slot { }));

    packed.name = 1;
    AOT_TEST_CHECK(!packed.isEmpty());
}

static Slot createSlotOfKind(const Cells& cells, Slot::Kind kind, uint8_t flags)
{
    switch (Slot::payloadOf(kind)) {
    case SlotPayload::None:
        return Slot::createWithoutPayload(cells.id(), kind, flags);
    case SlotPayload::Holder:
        if (kind == Slot::Kind::InheritedField)
            return Slot::createInheritedField(cells.id(), flags, cells.holder);
        return Slot::createInStorage(cells.id(), kind, flags & ~Slot::isOutOfLine, flags & Slot::isOwn ? nullptr : cells.holder, flags & Slot::isOutOfLine ? firstOutOfLineOffset + 3 : 3);
    case SlotPayload::WordIndex:
        return Slot::createInStorage(cells.id(), Slot::Kind::Value, flags & ~Slot::isOutOfLine, nullptr, firstFarOffset);
    case SlotPayload::Field:
        return Slot::createField(cells.id(), kind, flags, SlotData::create(24, SlotRepresentation::Int32), 0, Slot::locationOf(kind) == SlotLocation::HiddenWordOffset ? 16 : 0);
    case SlotPayload::FieldAndHolder:
        if (kind == Slot::Kind::FieldOrInheritedField)
            return *Slot::tryCreateFieldOrInheritedField(cells.id(), flags, SlotData::create(24, SlotRepresentation::Int32), cells.holder, 16, cells.cageBase);
        return *Slot::tryCreateFieldAndHolder(cells.id(), kind, flags & ~Slot::inheritedIsOutOfLine, SlotData::create(24, SlotRepresentation::Int32), cells.holder, flags & Slot::inheritedIsOutOfLine ? firstOutOfLineOffset + 3 : 3, cells.cageBase);
    case SlotPayload::ExtraProperty:
        return Slot::createExtraProperty(cells.id(), kind, flags & ~Slot::isOutOfLine, StructureID(), flags & Slot::isOutOfLine ? firstOutOfLineOffset + 1 : 1, 20);
    case SlotPayload::ExtraPropertyAddition:
        return Slot::createExtraPropertyAddition(cells.id(), flags & ~Slot::isOutOfLine, cells.otherID(), flags & Slot::isOutOfLine ? firstOutOfLineOffset : 0, 20);
    case SlotPayload::Transition: {
        PropertyOffset offset = flags & Slot::isOutOfLine ? firstOutOfLineOffset + 4 : 2;
        uint32_t capacity = flags & Slot::growsStorage ? Slot::storageCapacityAfterAdding(Slot::wordIndexFor(offset)) : 0;
        return *Slot::tryCreateTransition(cells.id(), flags & ~(Slot::isOutOfLine | Slot::growsStorage), cells.otherID(), offset, capacity);
    }
    case SlotPayload::Removal:
        return *Slot::tryCreateRemoval(cells.id(), kind, flags & ~Slot::isOutOfLine, cells.otherID(), flags & Slot::isOutOfLine ? firstOutOfLineOffset + 4 : 2, 9);
    case SlotPayload::OrderWords:
        return Slot::createFieldRemoval(cells.id(), flags, AbsentPattern::ZeroInEightBytes, 24, 9, 10);
    case SlotPayload::CustomAccessor:
        return Slot::createCustomAccessor(cells.id(), kind, flags, 5);
    case SlotPayload::Constant:
        return Slot::createConstant(cells.id(), flags, jsNumber(7));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isLegalFlagSet(Slot::Kind kind, uint8_t flags)
{
    if (flags & (Slot::absentIsUndefined | Slot::growsStorage | Slot::inheritsGetter) & ~Slot::kindFlagOf(kind))
        return false;
    if ((flags & Slot::storageHasIndexingHeader) && !(flags & Slot::growsStorage))
        return false;
    if ((flags & Slot::inheritedIsOutOfLine) && (Slot::payloadOf(kind) != SlotPayload::FieldAndHolder || Slot::locationOf(kind) != SlotLocation::WordIndex))
        return false;
    if (kind == Slot::Kind::InheritedField)
        return !(flags & (Slot::isOwn | Slot::isOutOfLine));
    switch (Slot::payloadOf(kind)) {
    case SlotPayload::WordIndex:
        return (flags & Slot::isOwn) && (flags & Slot::isOutOfLine);
    case SlotPayload::Transition:
        if ((flags & Slot::growsStorage) && !(flags & Slot::isOutOfLine))
            return false;
        return flags & Slot::isOwn;
    case SlotPayload::Removal:
        return flags & Slot::isOwn;
    case SlotPayload::OrderWords:
        return (flags & Slot::isOwn) && !(flags & Slot::isOutOfLine);
    case SlotPayload::None:
    case SlotPayload::CustomAccessor:
    case SlotPayload::Constant:
        return !(flags & Slot::isOutOfLine);
    case SlotPayload::Holder:
    case SlotPayload::Field:
    case SlotPayload::FieldAndHolder:
    case SlotPayload::ExtraProperty:
    case SlotPayload::ExtraPropertyAddition:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void forEachLegalSlot(const Cells& cells, const Invocable<void(const Slot&, Slot::Kind, uint8_t)> auto& function)
{
    for (Slot::Kind kind : allSlotKinds) {
        if (kind == Slot::Kind::Empty)
            continue;
        for (unsigned flags = 0; flags <= Slot::allFlags; ++flags) {
            if (isLegalFlagSet(kind, static_cast<uint8_t>(flags)))
                function(createSlotOfKind(cells, kind, static_cast<uint8_t>(flags)), kind, static_cast<uint8_t>(flags));
        }
    }
}

static void testEveryKindReadsBack()
{
    Realms realms;
    Cells cells(realms);
    unsigned numberOfKindsSeen = 0;
    std::optional<Slot::Kind> previousKind;
    forEachLegalSlot(cells, [&](const Slot& slot, Slot::Kind kind, uint8_t flags) {
        if (previousKind != kind)
            ++numberOfKindsSeen;
        previousKind = kind;

        AOT_TEST_CHECK(!slot.isEmpty());
        AOT_TEST_CHECK(slot.kind == kind && slot.flags == flags && slot.structureID == cells.id());
        for (uint8_t flag : { Slot::isOwn, Slot::isOutOfLine, Slot::dependsOnChain, Slot::absentIsUndefined, Slot::growsStorage, Slot::inheritsGetter, Slot::storageHasIndexingHeader, Slot::inheritedIsOutOfLine })
            AOT_TEST_CHECK(slot.has(flag) == !!(flags & flag));

        uint64_t header = slot.headerBits();
        AOT_TEST_CHECK(static_cast<uint32_t>(header) == cells.id().bits());
        AOT_TEST_CHECK(static_cast<uint8_t>(header >> Slot::kindShiftInHeader) == static_cast<uint8_t>(kind));
        AOT_TEST_CHECK(static_cast<uint8_t>(header >> Slot::flagsShiftInHeader) == flags);
        AOT_TEST_CHECK(static_cast<uint16_t>(header >> Slot::locationShiftInHeader) == slot.location);

        bool isOutOfLine = flags & Slot::isOutOfLine;
        switch (Slot::payloadOf(kind)) {
        case SlotPayload::None:
            AOT_TEST_CHECK(!slot.payloadBits() && !slot.location);
            break;
        case SlotPayload::Holder:
            AOT_TEST_CHECK(slot.holder == (flags & Slot::isOwn ? nullptr : cells.holder));
            AOT_TEST_CHECK(slot.payloadBits() == std::bit_cast<uintptr_t>(slot.holder));
            if (Slot::locationOf(kind) == SlotLocation::None) {
                AOT_TEST_CHECK(!slot.location);
                break;
            }
            AOT_TEST_CHECK(slot.offsetInStorage() == (isOutOfLine ? firstOutOfLineOffset + 3 : 3));
            AOT_TEST_CHECK(static_cast<int64_t>(header) >> Slot::locationShiftInHeader == slot.wordIndex());
            break;
        case SlotPayload::WordIndex:
            AOT_TEST_CHECK(slot.farValue.wordIndex == Slot::wordIndexFor(firstFarOffset) && slot.offsetInStorage() == firstFarOffset);
            AOT_TEST_CHECK(slot.payloadBits() == static_cast<uint32_t>(slot.farValue.wordIndex));
            break;
        case SlotPayload::Field:
            AOT_TEST_CHECK(slot.field.slot == SlotData::create(24, SlotRepresentation::Int32) && !slot.field.type);
            AOT_TEST_CHECK(slot.payloadBits() == slot.field.slot.encoded());
            if (Slot::locationOf(kind) == SlotLocation::HiddenWordOffset)
                AOT_TEST_CHECK(slot.hiddenWordOffset() == 16);
            break;
        case SlotPayload::FieldAndHolder:
            AOT_TEST_CHECK(slot.fieldSlot() == SlotData::create(24, SlotRepresentation::Int32));
            AOT_TEST_CHECK(MarkedBlockCage::decode(cells.cageBase, slot.fieldAndHolder.holder) == cells.holder);
            AOT_TEST_CHECK(slot.payloadBits() == (static_cast<uint64_t>(slot.fieldAndHolder.holder) << 32 | slot.fieldSlot().encoded()));
            if (Slot::locationOf(kind) == SlotLocation::HiddenWordOffset)
                AOT_TEST_CHECK(slot.hiddenWordOffset() == 16);
            else
                AOT_TEST_CHECK(slot.offsetInStorage() == (flags & Slot::inheritedIsOutOfLine ? firstOutOfLineOffset + 3 : 3));
            break;
        case SlotPayload::ExtraProperty:
            AOT_TEST_CHECK(!slot.extraProperty.structureID && slot.hiddenWordOffset() == 20);
            AOT_TEST_CHECK(Slot::offsetFor(slot.extraProperty.wordIndex) == (isOutOfLine ? firstOutOfLineOffset + 1 : 1));
            break;
        case SlotPayload::ExtraPropertyAddition:
            AOT_TEST_CHECK(slot.extraPropertyAddition.newStructureID == cells.otherID() && slot.hiddenWordOffset() == 20);
            AOT_TEST_CHECK(Slot::offsetFor(slot.extraPropertyAddition.wordIndex) == (isOutOfLine ? firstOutOfLineOffset : 0));
            break;
        case SlotPayload::Transition:
            AOT_TEST_CHECK(slot.transition.newStructureID == cells.otherID());
            AOT_TEST_CHECK(slot.offsetInStorage() == (isOutOfLine ? firstOutOfLineOffset + 4 : 2));
            AOT_TEST_CHECK(slot.transition.newStorageCapacity == (flags & Slot::growsStorage ? 8u : 0u));
            break;
        case SlotPayload::Removal:
            AOT_TEST_CHECK(slot.removal.newStructureID == cells.otherID() && slot.removal.orderWord == 9);
            AOT_TEST_CHECK(slot.offsetInStorage() == (isOutOfLine ? firstOutOfLineOffset + 4 : 2));
            break;
        case SlotPayload::OrderWords:
            AOT_TEST_CHECK(slot.orderWords.before == 9 && slot.orderWords.after == 10 && slot.hiddenWordOffset() == 24);
            break;
        case SlotPayload::CustomAccessor:
            AOT_TEST_CHECK(slot.customAccessor.index == 5 && slot.payloadBits() == 5);
            break;
        case SlotPayload::Constant:
            AOT_TEST_CHECK(JSValue::decode(slot.constant) == jsNumber(7));
            AOT_TEST_CHECK(slot.payloadBits() == static_cast<uint64_t>(JSValue::encode(jsNumber(7))));
            break;
        }
    });
    AOT_TEST_CHECK(numberOfKindsSeen == numberOfSlotKinds - 1);
}

static void testPresenceByKind()
{
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::Value) == SlotPresence::Present);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::FarValue) == SlotPresence::Present);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::Getter) == SlotPresence::Present);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::Absent) == SlotPresence::Absent);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::Field) == SlotPresence::ReadsSlot);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::Empty) == SlotPresence::Unknown);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::Uncacheable) == SlotPresence::Unknown);
    AOT_TEST_CHECK(Slot::presenceOf(Slot::Kind::InheritedField) == SlotPresence::Unknown && Slot::presenceOf(Slot::Kind::FieldOrInheritedField) == SlotPresence::Unknown);

    AOT_TEST_CHECK(Slot::kindFlagOf(Slot::Kind::Field) == Slot::absentIsUndefined);
    AOT_TEST_CHECK(Slot::kindFlagOf(Slot::Kind::Transition) == Slot::growsStorage);
    AOT_TEST_CHECK(Slot::kindFlagOf(Slot::Kind::FieldOrInherited) == Slot::inheritsGetter && !Slot::kindFlagOf(Slot::Kind::FieldOrInheritedSetter));
    AOT_TEST_CHECK(!Slot::kindFlagOf(Slot::Kind::Value) && !Slot::kindFlagOf(Slot::Kind::InheritedField) && !Slot::kindFlagOf(Slot::Kind::FieldOrInheritedField));

    AOT_TEST_CHECK(accessFor(PutMode::Strict) == SlotAccess::Store);
    AOT_TEST_CHECK(accessFor(PutMode::Sloppy) == SlotAccess::Store);
    AOT_TEST_CHECK(accessFor(PutMode::Define) == SlotAccess::Define);
}

static void testWordIndexOfOffset()
{
    constexpr int32_t firstInlineWord = static_cast<int32_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
    struct Case {
        PropertyOffset offset;
        int32_t wordIndex;
        bool fitsInLocation;
    };
    const Case cases[] = {
        { 0, firstInlineWord, true },
        { 63, firstInlineWord + 63, true },
        { 64, -2, true },
        { 64 + 32765, -32767, true },
        { 64 + 32766, -32768, true },
        { 64 + 32767, -32769, false },
    };
    for (const Case& testCase : cases) {
        AOT_TEST_CHECK(Slot::wordIndexFor(testCase.offset) == testCase.wordIndex);
        AOT_TEST_CHECK(Slot::offsetFor(testCase.wordIndex) == testCase.offset);
        AOT_TEST_CHECK(Slot::fitsInLocation(testCase.wordIndex) == testCase.fitsInLocation);
    }
    AOT_TEST_CHECK(Slot::fitsInLocation(32767) && !Slot::fitsInLocation(32768));
}

static void testWordIndexAddressesTheProperty()
{
    Realms realms;
    VM& vm = realms.vm();
    JSObject* object = realms.object(realms.plain(), "(() => { let o = { }; for (let i = 0; i < 80; ++i) o['p' + i] = i; return o; })()"_s);
    unsigned numberOutOfLine = 0;
    for (unsigned i = 0; i < 80; ++i) {
        PropertyOffset offset = object->structure()->get(vm, Identifier::fromString(vm, makeString('p', i)));
        AOT_TEST_CHECK(isValidOffset(offset));
        int32_t wordIndex = Slot::wordIndexFor(offset);
        const EncodedJSValue* base = isInlineOffset(offset) ? std::bit_cast<const EncodedJSValue*>(object) : std::bit_cast<const EncodedJSValue*>(object->butterfly());
        AOT_TEST_CHECK(JSValue::decode(base[wordIndex]) == jsNumber(i));
        numberOutOfLine += !isInlineOffset(offset);
    }
    AOT_TEST_CHECK(numberOutOfLine && numberOutOfLine < 80);
}

static void testStorageCapacityIsTheEngines()
{
    for (PropertyOffset offset = firstOutOfLineOffset; offset < firstOutOfLineOffset + 300; ++offset)
        AOT_TEST_CHECK(Slot::storageCapacityAfterAdding(Slot::wordIndexFor(offset)) == Structure::outOfLineCapacity(offset));
    AOT_TEST_CHECK(Slot::storageCapacityAfterAdding(Slot::wordIndexFor(firstOutOfLineOffset + 3)) == 4);
    AOT_TEST_CHECK(Slot::storageCapacityAfterAdding(Slot::wordIndexFor(firstOutOfLineOffset + 4)) == 8);
    AOT_TEST_CHECK(Slot::storageCapacityAfterAdding(Slot::wordIndexFor(firstOutOfLineOffset + 8)) == 16);
}

static void testFarIndexDegrades()
{
    Realms realms;
    Cells cells(realms);

    Slot nearSlot = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, lastOffsetInLocation);
    AOT_TEST_CHECK(nearSlot.kind == Slot::Kind::Value && nearSlot.wordIndex() == -32768 && nearSlot.has(Slot::isOutOfLine));

    Slot farSlot = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn | Slot::dependsOnChain, nullptr, firstFarOffset);
    AOT_TEST_CHECK(farSlot.kind == Slot::Kind::FarValue && farSlot.offsetInStorage() == firstFarOffset);
    AOT_TEST_CHECK(farSlot.flags == (Slot::isOwn | Slot::isOutOfLine | Slot::dependsOnChain));

    Slot inherited = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::dependsOnChain, cells.holder, firstFarOffset);
    AOT_TEST_CHECK(inherited.kind == Slot::Kind::Uncacheable && inherited.structureID == cells.id() && !inherited.payloadBits());
    AOT_TEST_CHECK(inherited.flags == Slot::dependsOnChain);

    for (Slot::Kind kind : { Slot::Kind::Getter, Slot::Kind::Setter }) {
        AOT_TEST_CHECK(Slot::createInStorage(cells.id(), kind, Slot::isOwn, nullptr, lastOffsetInLocation).kind == kind);
        AOT_TEST_CHECK(Slot::createInStorage(cells.id(), kind, Slot::isOwn, nullptr, firstFarOffset).kind == Slot::Kind::Uncacheable);
    }

    AOT_TEST_CHECK(Slot::tryCreateTransition(cells.id(), 0, cells.otherID(), lastOffsetInLocation, 0));
    AOT_TEST_CHECK(!Slot::tryCreateTransition(cells.id(), 0, cells.otherID(), firstFarOffset, 0));

    SlotData slotData = SlotData::create(24, SlotRepresentation::Int32);
    AOT_TEST_CHECK(Slot::tryCreateFieldAndHolder(cells.id(), Slot::Kind::FieldOrInherited, 0, slotData, cells.holder, lastOffsetInLocation, cells.cageBase));
    AOT_TEST_CHECK(!Slot::tryCreateFieldAndHolder(cells.id(), Slot::Kind::FieldOrInherited, 0, slotData, cells.holder, firstFarOffset, cells.cageBase));
    AOT_TEST_CHECK(!Slot::tryCreateFieldAndHolder(cells.id(), Slot::Kind::FieldOrInherited, 0, slotData, cells.holder, 0, cells.cageBase + MarkedBlockCage::maxSize));
    AOT_TEST_CHECK(!Slot::tryCreateFieldOrInheritedField(cells.id(), 0, slotData, cells.holder, 16, cells.cageBase + MarkedBlockCage::maxSize));
}

static bool alwaysStaysOutsideTheFront(Slot::Kind kind)
{
    return Slot::payloadOf(kind) == SlotPayload::FieldAndHolder || Slot::payloadOf(kind) == SlotPayload::ExtraPropertyAddition || Slot::payloadOf(kind) == SlotPayload::Removal || Slot::payloadOf(kind) == SlotPayload::OrderWords || Slot::payloadOf(kind) == SlotPayload::Constant;
}

static void testPackedSlotRoundTrip()
{
    Realms realms;
    Cells cells(realms);
    unsigned numberPacked = 0;
    forEachLegalSlot(cells, [&](const Slot& slot, Slot::Kind kind, uint8_t flags) {
        AOT_TEST_CHECK(packsAndUnpacks(slot, cells.cageBase) == (!alwaysStaysOutsideTheFront(kind) && !(flags & Slot::storageHasIndexingHeader)));
        ++numberPacked;
    });
    AOT_TEST_CHECK(numberPacked > numberOfSlotKinds);
}

static void testFarValuePacksAsValue()
{
    Realms realms;
    Cells cells(realms);
    Slot farSlot = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, firstFarOffset);
    AOT_TEST_CHECK(farSlot.kind == Slot::Kind::FarValue);
    std::optional<PackedSlot> packed = PackedSlot::tryPack(farSlot, testName, cells.cageBase);
    AOT_TEST_CHECK(packed && packed->kind() == Slot::Kind::Value);
    AOT_TEST_CHECK(static_cast<int32_t>(packed->payload) == Slot::wordIndexFor(firstFarOffset) && !packed->location);
    AOT_TEST_CHECK(hasSameBits(packed->unpack(cells.id(), cells.cageBase, false), farSlot));
}

static void testInheritedIndexOutsideEightBitsDoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    for (Slot::Kind kind : { Slot::Kind::Value, Slot::Kind::Getter, Slot::Kind::Setter }) {
        AOT_TEST_CHECK(packsAndUnpacks(Slot::createInStorage(cells.id(), kind, 0, cells.holder, lastOffsetInPackedLocation), cells.cageBase));
        AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createInStorage(cells.id(), kind, 0, cells.holder, firstOffsetOutsidePackedLocation), testName, cells.cageBase));
        AOT_TEST_CHECK(packsAndUnpacks(Slot::createInStorage(cells.id(), kind, Slot::isOwn, nullptr, firstOffsetOutsidePackedLocation), cells.cageBase));
    }
}

static void testHolderOutsideTheCageDoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    uintptr_t otherCageBase = cells.cageBase + MarkedBlockCage::maxSize;
    for (Slot::Kind kind : { Slot::Kind::Value, Slot::Kind::Getter }) {
        Slot inherited = Slot::createInStorage(cells.id(), kind, 0, cells.holder, 1);
        AOT_TEST_CHECK(packsAndUnpacks(inherited, cells.cageBase));
        AOT_TEST_CHECK(!PackedSlot::tryPack(inherited, testName, otherCageBase));

        Slot own = Slot::createInStorage(cells.id(), kind, Slot::isOwn, nullptr, 1);
        AOT_TEST_CHECK(packsAndUnpacks(own, otherCageBase));
    }
    Slot inheritedField = Slot::createInheritedField(cells.id(), Slot::dependsOnChain, cells.holder);
    AOT_TEST_CHECK(packsAndUnpacks(inheritedField, cells.cageBase) && !PackedSlot::tryPack(inheritedField, testName, otherCageBase));
}

static void testFieldOfStoreDoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    SlotData slotData = SlotData::create(32, SlotRepresentation::Cell32);
    for (Slot::Kind kind : { Slot::Kind::Field, Slot::Kind::FieldCreation, Slot::Kind::Variable }) {
        AOT_TEST_CHECK(packsAndUnpacks(Slot::createField(cells.id(), kind, Slot::isOwn, slotData, 0, 0), cells.cageBase));
        AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createField(cells.id(), kind, Slot::isOwn, slotData, 8, 0), testName, cells.cageBase));
    }
}

static void testHiddenWordOffsetOver1020DoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    SlotData slotData = SlotData::create(32, SlotRepresentation::Cell32);
    for (Slot::Kind kind : { Slot::Kind::Field, Slot::Kind::FieldCreation }) {
        AOT_TEST_CHECK(packsAndUnpacks(Slot::createField(cells.id(), kind, Slot::isOwn, slotData, 0, 1020), cells.cageBase));
        AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createField(cells.id(), kind, Slot::isOwn, slotData, 0, 1024), testName, cells.cageBase));
    }
    for (Slot::Kind kind : { Slot::Kind::ExtraProperty, Slot::Kind::ExtraGetter, Slot::Kind::ExtraSetter }) {
        AOT_TEST_CHECK(packsAndUnpacks(Slot::createExtraProperty(cells.id(), kind, Slot::isOwn, StructureID(), 0, 1020), cells.cageBase));
        AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createExtraProperty(cells.id(), kind, Slot::isOwn, StructureID(), 0, 1024), testName, cells.cageBase));
    }
}

static void testExtraPropertyWithStructureDoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    for (Slot::Kind kind : { Slot::Kind::ExtraProperty, Slot::Kind::ExtraGetter, Slot::Kind::ExtraSetter }) {
        AOT_TEST_CHECK(packsAndUnpacks(Slot::createExtraProperty(cells.id(), kind, Slot::isOwn, StructureID(), 2, 16), cells.cageBase));
        AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createExtraProperty(cells.id(), kind, Slot::isOwn, cells.otherID(), 2, 16), testName, cells.cageBase));
    }
}

static void testTransitionIndexOutsideEightBitsDoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    AOT_TEST_CHECK(packsAndUnpacks(*Slot::tryCreateTransition(cells.id(), 0, cells.otherID(), lastOffsetInPackedLocation, 0), cells.cageBase));
    AOT_TEST_CHECK(!PackedSlot::tryPack(*Slot::tryCreateTransition(cells.id(), 0, cells.otherID(), firstOffsetOutsidePackedLocation, 0), testName, cells.cageBase));

    PropertyOffset growing = firstOutOfLineOffset + 64;
    Slot slot = *Slot::tryCreateTransition(cells.id(), 0, cells.otherID(), growing, 128);
    AOT_TEST_CHECK(slot.has(Slot::growsStorage) && slot.transition.newStorageCapacity == 128);
    AOT_TEST_CHECK(packsAndUnpacks(slot, cells.cageBase));
}

static void testAdditionAndConstantNeverPack()
{
    Realms realms;
    Cells cells(realms);
    AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createExtraPropertyAddition(cells.id(), Slot::isOwn, cells.otherID(), 0, 16), testName, cells.cageBase));
    AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createConstant(cells.id(), Slot::isOwn, jsNumber(1)), testName, cells.cageBase));
    AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createConstant(cells.id(), 0, cells.holder), testName, cells.cageBase));
}

static void testOwnInlineValueHasZeroBytes()
{
    Realms realms;
    Cells cells(realms);
    auto upperHalf = [](const PackedSlot& packed) {
        return static_cast<uint32_t>(bitsOf(packed) >> 32);
    };

    PackedSlot ownInline = *PackedSlot::tryPack(Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, 3), testName, cells.cageBase);
    AOT_TEST_CHECK(!ownInline.location && !ownInline.biasedKindAndFlags);
    AOT_TEST_CHECK(upperHalf(ownInline) == PackedSlot::upperHalfForOwnInlineValue(testName));
    AOT_TEST_CHECK(static_cast<int32_t>(ownInline.payload) == Slot::wordIndexFor(3));
    AOT_TEST_CHECK(upperHalf(ownInline) != PackedSlot::upperHalfForOwnInlineValue(testName + 1));

    const Slot others[] = {
        Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset),
        Slot::createInStorage(cells.id(), Slot::Kind::Value, 0, cells.holder, 3),
        Slot::createInStorage(cells.id(), Slot::Kind::Getter, Slot::isOwn, nullptr, 3),
        Slot::createWithoutPayload(cells.id(), Slot::Kind::Absent, 0),
        Slot::createWithoutPayload(cells.id(), Slot::Kind::Uncacheable, 0),
    };
    for (const Slot& other : others)
        AOT_TEST_CHECK(upperHalf(*PackedSlot::tryPack(other, testName, cells.cageBase)) != PackedSlot::upperHalfForOwnInlineValue(testName));
}

static void testChainBitIsOutsideTheWord()
{
    Realms realms;
    Cells cells(realms);
    Slot independent = Slot::createInStorage(cells.id(), Slot::Kind::Value, 0, cells.holder, 1);
    Slot dependent = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::dependsOnChain, cells.holder, 1);
    PackedSlot packedIndependent = *PackedSlot::tryPack(independent, testName, cells.cageBase);
    PackedSlot packedDependent = *PackedSlot::tryPack(dependent, testName, cells.cageBase);
    AOT_TEST_CHECK(bitsOf(packedIndependent) == bitsOf(packedDependent));
    AOT_TEST_CHECK(hasSameBits(packedIndependent.unpack(cells.id(), cells.cageBase, true), dependent));
    AOT_TEST_CHECK(hasSameBits(packedDependent.unpack(cells.id(), cells.cageBase, false), independent));
}

static void testFlagOfAnotherKindDoesNotPack()
{
    Realms realms;
    Cells cells(realms);
    SlotData slotData = SlotData::create(16, SlotRepresentation::Int32);
    AOT_TEST_CHECK(packsAndUnpacks(Slot::createField(cells.id(), Slot::Kind::Field, Slot::isOwn | Slot::absentIsUndefined, slotData, 0, 0), cells.cageBase));
    AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createField(cells.id(), Slot::Kind::FieldCreation, Slot::isOwn | Slot::absentIsUndefined, slotData, 0, 0), testName, cells.cageBase));
}

static void testLivenessNamesEveryCell()
{
    Realms realms;
    Cells cells(realms);
    JSCell* structureCell = cells.structure;
    JSCell* otherStructureCell = cells.otherStructure;

    struct Case {
        Slot slot;
        Vector<JSCell*, 2> named;
    };
    const Case cases[] = {
        { Slot::createWithoutPayload(cells.id(), Slot::Kind::Absent, 0), { structureCell } },
        { Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, 0), { structureCell } },
        { Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, firstFarOffset), { structureCell } },
        { Slot::createInStorage(cells.id(), Slot::Kind::Value, 0, cells.holder, 0), { structureCell, cells.holder } },
        { Slot::createInStorage(cells.id(), Slot::Kind::Getter, 0, cells.holder, 0), { structureCell, cells.holder } },
        { Slot::createInStorage(cells.id(), Slot::Kind::Setter, 0, cells.holder, 0), { structureCell, cells.holder } },
        { Slot::createField(cells.id(), Slot::Kind::Field, Slot::isOwn, SlotData::create(16, SlotRepresentation::Int32), 0, 0), { structureCell } },
        { *Slot::tryCreateFieldAndHolder(cells.id(), Slot::Kind::FieldOrInherited, 0, SlotData::create(16, SlotRepresentation::Int32), cells.holder, 0, cells.cageBase), { structureCell, cells.holder } },
        { *Slot::tryCreateFieldAndHolder(cells.id(), Slot::Kind::FieldOrInheritedSetter, 0, SlotData::create(16, SlotRepresentation::Int32), cells.holder, 0, cells.cageBase), { structureCell, cells.holder } },
        { Slot::createInheritedField(cells.id(), Slot::dependsOnChain, cells.holder), { structureCell, cells.holder } },
        { *Slot::tryCreateFieldOrInheritedField(cells.id(), Slot::dependsOnChain, SlotData::create(16, SlotRepresentation::Int32), cells.holder, 16, cells.cageBase), { structureCell, cells.holder } },
        { Slot::createCustomAccessor(cells.id(), Slot::Kind::CustomGetter, 0, 1), { structureCell } },
        { Slot::createExtraProperty(cells.id(), Slot::Kind::ExtraProperty, Slot::isOwn, StructureID(), 0, 16), { structureCell } },
        { Slot::createExtraProperty(cells.id(), Slot::Kind::ExtraProperty, Slot::isOwn, cells.otherID(), 0, 16), { structureCell, otherStructureCell } },
        { Slot::createExtraPropertyAddition(cells.id(), Slot::isOwn, cells.otherID(), 0, 16), { structureCell, otherStructureCell } },
        { *Slot::tryCreateTransition(cells.id(), 0, cells.otherID(), 0, 0), { structureCell, otherStructureCell } },
        { Slot::createConstant(cells.id(), 0, jsNumber(1)), { structureCell } },
        { Slot::createConstant(cells.id(), 0, cells.holder), { structureCell, cells.holder } },
    };

    JSCell* const allCells[] = { structureCell, otherStructureCell, cells.holder, cells.receiver };
    for (const Case& testCase : cases) {
        AOT_TEST_CHECK(testCase.slot.namesOnlyLiveCells([](JSCell*) { return true; }, cells.cageBase));
        for (JSCell* dead : allCells) {
            bool isLive = testCase.slot.namesOnlyLiveCells([&](JSCell* cell) { return cell != dead; }, cells.cageBase);
            AOT_TEST_CHECK(isLive == !testCase.named.contains(dead));
        }
    }
}

static void testMissesBeforeRefill()
{
    AOT_TEST_CHECK(SiteCacheState::missesBeforeRefill(0) == 3);
    AOT_TEST_CHECK(SiteCacheState::missesBeforeRefill(1) == 15);
    AOT_TEST_CHECK(SiteCacheState::missesBeforeRefill(2) == 63);
    AOT_TEST_CHECK(SiteCacheState::missesBeforeRefill(3) == 255);
    AOT_TEST_CHECK(SiteCacheState::missesBeforeRefill(4) == 255);
    AOT_TEST_CHECK(SiteCacheState::missesBeforeRefill(200) == 255);
}

static void testInstanceLayoutOffsets()
{
    static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) == std::numeric_limits<int8_t>::min() && sizeof(InstancePrefix) <= 256);
    const InstanceSizes allSizes[] = {
        { 0, 0, 0 },
        { 1, 1, 1 },
        { 3 * 64 * KB + 8, 5000, 70000 },
    };
    for (InstanceSizes sizes : allSizes) {
        AOT_TEST_CHECK(InstanceLayout::isAddressable(sizes));

        ptrdiff_t cards = InstanceLayout::offsetOfCards(sizes);
        ptrdiff_t statics = InstanceLayout::offsetOfStatics(sizes);
        ptrdiff_t pollPage = InstanceLayout::offsetOfLoopPollPage();
        ptrdiff_t prefix = -static_cast<ptrdiff_t>(sizeof(InstancePrefix));
        AOT_TEST_CHECK(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance(sizes)) <= cards);
        AOT_TEST_CHECK(cards < statics && statics < pollPage && pollPage < prefix && prefix < 0);
        AOT_TEST_CHECK(statics + static_cast<ptrdiff_t>(sizes.sizeOfStatics) <= pollPage);
        AOT_TEST_CHECK(pollPage + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForLoopPollPage) <= prefix);
        AOT_TEST_CHECK(cards + static_cast<ptrdiff_t>((static_cast<size_t>(sizes.sizeOfStatics) + (1 << InstanceLayout::cardShift) - 1) >> InstanceLayout::cardShift) <= statics);

        size_t stringCells = InstanceLayout::offsetOfStringCell(0);
        size_t siteCaches = InstanceLayout::offsetOfSiteCaches(sizes);
        size_t end = InstanceLayout::bytesReservedFromInstance(sizes);
        AOT_TEST_CHECK(sizeof(Instance) <= stringCells && stringCells == 384 * KB);
        AOT_TEST_CHECK(InstanceLayout::offsetOfStringCell(sizes.numberOfStringCells) <= siteCaches);
        AOT_TEST_CHECK(InstanceLayout::offsetOfSiteCache(sizes, 0) == siteCaches && !(siteCaches % sizeof(Slot)));
        AOT_TEST_CHECK(InstanceLayout::offsetOfSiteCache(sizes, sizes.numberOfSiteCaches) <= end);
    }

    InstanceSizes sizes = allSizes[2];
    AOT_TEST_CHECK(InstanceLayout::offsetOfStringCell(7) == 384 * KB + 7 * sizeof(uint32_t));
    AOT_TEST_CHECK(InstanceLayout::offsetOfSiteCache(sizes, 9) == InstanceLayout::offsetOfSiteCaches(sizes) + 9 * sizeof(Slot));
    AOT_TEST_CHECK(InstanceLayout::offsetOfSiteCaches({ 0, 5000, 0 }) > InstanceLayout::offsetOfSiteCaches({ 0, 1, 0 }));
    AOT_TEST_CHECK(InstanceLayout::offsetOfStatics({ 64 * KB + 1, 0, 0 }) < InstanceLayout::offsetOfStatics({ 64 * KB, 0, 0 }));

    // The small tables between the key order steps and the string cells: each ends where the next begins, and a table that is one entry too large is not held.
    size_t endOfKeyOrderSteps = InstanceLayout::offsetOfKeyOrderSteps + numberOfKeyOrderStepCaches * sizeof(KeyOrderStepCache);
    AOT_TEST_CHECK(endOfKeyOrderSteps <= InstanceLayout::offsetOfBuiltinLocationStates && InstanceLayout::holdsBuiltinLocationStates(maxNumberOfBuiltins));
    size_t roomForStates = InstanceLayout::offsetOfGlobalBindings - InstanceLayout::offsetOfBuiltinLocationStates;
    AOT_TEST_CHECK(InstanceLayout::holdsBuiltinLocationStates(roomForStates) && !InstanceLayout::holdsBuiltinLocationStates(roomForStates + 1));
    size_t roomForBindings = InstanceLayout::offsetOfBuiltinCells - InstanceLayout::offsetOfGlobalBindings;
    AOT_TEST_CHECK(InstanceLayout::holdsGlobalBindings(roomForBindings) && !InstanceLayout::holdsGlobalBindings(roomForBindings + sizeof(GlobalBinding)));
    size_t mostKeys = (InstanceLayout::offsetOfBuiltinValues - InstanceLayout::offsetOfBuiltinCells) / sizeof(JSCell*);
    AOT_TEST_CHECK(InstanceLayout::holdsBuiltinCells(mostKeys) && !InstanceLayout::holdsBuiltinCells(mostKeys + 1));
    AOT_TEST_CHECK(InstanceLayout::offsetOfBuiltinValues + mostKeys * sizeof(EncodedJSValue) == InstanceLayout::offsetOfStringCells);
}

static void testInstanceLayoutAddressability()
{
    AOT_TEST_CHECK(InstanceLayout::isAddressable({ 1u << 30, 1u << 20, 1u << 20 }));
    AOT_TEST_CHECK(!InstanceLayout::isAddressable({ UINT32_MAX, 0, 0 }));
    AOT_TEST_CHECK(!InstanceLayout::isAddressable({ 0, UINT32_MAX, 0 }));
    AOT_TEST_CHECK(!InstanceLayout::isAddressable({ 0, 0, UINT32_MAX }));
    AOT_TEST_CHECK(!InstanceLayout::isAddressable({ 0, 0, 1u << 27 }));
    AOT_TEST_CHECK(InstanceLayout::isAddressable({ 0, 0, 1u << 26 }));
}

static unsigned numberOfHashValues(const Invocable<uint32_t(uint32_t)> auto& slotKeyOf)
{
    constexpr uint32_t shape = 12;
    constexpr uint32_t order = 3;
    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> values;
    for (uint32_t i = 0; i < 64; ++i) {
        uint64_t before = static_cast<uint64_t>(shapeWordOf(shape)) << 32 | orderWordOf(order, i);
        values.add(KeyOrderTransition::hashOf(before, slotKeyOf(i)) & 1023);
    }
    return values.size();
}

static void testKeyOrderTransitionHashSpreads()
{
    AOT_TEST_CHECK(numberOfHashValues([](uint32_t i) { return 16 + 8 * i; }) >= 48);
    AOT_TEST_CHECK(numberOfHashValues([](uint32_t i) { return 16 + i; }) >= 48);
}

static void testSlotKeys()
{
    AOT_TEST_CHECK(slotKeyFor(16, false) == 16);
    AOT_TEST_CHECK(slotKeyFor(16, true) == (16 | outOfLineSlotKey));
    AOT_TEST_CHECK(slotKeyFor(16, true) != slotKeyFor(16, false));
    AOT_TEST_CHECK(slotKeyFor(outOfLineSlotKey - 1, false) == outOfLineSlotKey - 1);
    AOT_TEST_CHECK(slotKeyFor(outOfLineSlotKey - 1, true) == slotKeyMask);
    AOT_TEST_CHECK(slotKeyFor(outOfLineSlotKey, false) == noSlotKey);
    AOT_TEST_CHECK(slotKeyFor(outOfLineSlotKey, true) == noSlotKey);
}

static void testOrderWord()
{
    const uint32_t orders[] = { 0, 1, 255, maxKeyOrdersPerLayout - 1 };
    const uint32_t counts[] = { 0, 1, 16, maxFieldsInOrder };
    const uint32_t otherBits[] = { 0, orderWordHasFieldOutsideSlot, orderWordHasExtraProperties, orderWordIsFrozen, orderWordIsSealed, orderWordIsNotExtensible, orderWordIntegrityBits | orderWordHasExtraProperties };
    for (uint32_t order : orders) {
        for (uint32_t count : counts) {
            uint32_t word = orderWordOf(order, count);
            AOT_TEST_CHECK(!(word & ~orderAndCountMask));
            for (uint32_t bits : otherBits) {
                AOT_TEST_CHECK(orderOf(word | bits) == order && countOf(word | bits) == count);
                AOT_TEST_CHECK(!isOrderStoredOutOfLine(word | bits));
                AOT_TEST_CHECK(((word | bits) & orderAndCountMask) == word);
            }
        }
    }

    AOT_TEST_CHECK(orderStoredOutOfLine == 0x7fff && maxFieldsInOrder == 4095);
    AOT_TEST_CHECK(isOrderStoredOutOfLine(outOfLineOrderWord) && !countOf(outOfLineOrderWord));
    AOT_TEST_CHECK(!unorderedOrderWord);
    AOT_TEST_CHECK(orderWordHasFieldOutsideSlot == 1u << 27 && orderWordHasExtraProperties == 1u << 28 && orderWordIsFrozen == 1u << 29 && orderWordIsSealed == 1u << 30 && orderWordIsNotExtensible == 1u << 31);

    uint64_t words = static_cast<uint64_t>(shapeWordOf(9)) << 32 | orderWordOf(2, 5) | orderWordIntegrityBits | orderWordHasExtraProperties;
    AOT_TEST_CHECK((words & orderCountAndShapeMask) == (static_cast<uint64_t>(shapeWordOf(9)) << 32 | orderWordOf(2, 5)));
}

static void testShapeWord()
{
    for (uint32_t shape : { 1u, 2u, 255u, 65536u, (1u << 31) - 1 }) {
        AOT_TEST_CHECK(shapeWordOf(shape) == 0 - shape && shapeOf(shapeWordOf(shape)) == shape);
        AOT_TEST_CHECK(shapeWordOf(shape) & compiledObjectShapeBit);
        AOT_TEST_CHECK(canPassShapeTest(shapeWordOf(shape)));
    }
    AOT_TEST_CHECK(!canPassShapeTest(0));
    AOT_TEST_CHECK(!canPassShapeTest(pointerUpperHalfBits));
    AOT_TEST_CHECK(!canPassShapeTest(bigIntHashMask));
}

static void testOffsetsOfRuntimeWords()
{
    auto* instance = static_cast<Instance*>(fastZeroedMalloc(sizeof(Instance)));
    uint8_t* base = reinterpret_cast<uint8_t*>(instance);
    AOT_TEST_CHECK(!CounterWords::offsetOfValue(static_cast<RuntimeCounterID>(0)));
    for (unsigned index = 0; index < numberOfRuntimeCounters; ++index) {
        ptrdiff_t offset = AOT_INSTANCE_OFFSET(runtime.counters) + CounterWords::offsetOfValue(static_cast<RuntimeCounterID>(index));
        *reinterpret_cast<uint64_t*>(base + offset) += index + 1;
    }
    for (unsigned index = 0; index < numberOfRuntimeCounters; ++index)
        AOT_TEST_CHECK(instance->runtime.counters.values[index] == index + 1);
    AOT_TEST_CHECK(!instance->runtime.counters.branchCounts);
    fastFree(instance);

    AOT_TEST_CHECK(!OBJECT_OFFSETOF(ExceptionWords, thrownValue));
}

static constexpr ASCIILiteral programWithOrderedLayout =
    "layout L0 name=\"Pair\" fields=[{key=\"first\" declared={type=anything}}, {key=\"second\" declared={type=anything}}]\n"
    "module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"
    "entry module=m0\n"
    "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
    "  b0():\n"
    "    v0 = Builtin builtin=GlobalThis\n"
    "    v1 = PropGet v0 key=\"input\" [isFreeName]\n"
    "    v2 = NewObject -, v1, v1 layout=L0 fields=[0, 1]\n"
    "    v3 = PropGet v0 key=\"publish\" [isFreeName]\n"
    "    v4 = Call v3, -, v2\n"
    "    Return v2\n"
    "}\n"_s;

static const LayoutData& findLayoutWithTwoFields(const ProgramTables& tables)
{
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && tables.layoutForShape(shape).fields().size() == 2)
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, "the image has the layout with two fields");
}

static const KeyOrderTransition* probe(const ObjectWords& words, uint64_t before, uint32_t slotKey)
{
    uint32_t mask = words.keyOrderTransitionMask;
    for (uint32_t index = KeyOrderTransition::hashOf(before, slotKey) & mask; !words.keyOrderTransitions[index].isEmpty(); index = (index + 1) & mask) {
        const KeyOrderTransition& transition = words.keyOrderTransitions[index];
        if (transition.orderAndShapeWordsBefore == before && transition.key == slotKey)
            return &transition;
    }
    return nullptr;
}

static void testKeyOrderTransitionProbing(LoadedProgram& program)
{
    const LayoutData& layout = findLayoutWithTwoFields(program.state().tables);
    const FieldData& field = layout.fields()[1];
    uint32_t slotKey = slotKeyFor(field.slot.offset, field.isOutOfLine());
    AOT_TEST_CHECK(slotKey != noSlotKey);

    ObjectWords& words = program.instance().objects;
    uint32_t mask = words.keyOrderTransitionMask;
    AOT_TEST_CHECK(words.keyOrderTransitions && hasOneBitSet(mask + 1));
    auto wordsBefore = [&](uint32_t orderAndCount) {
        return static_cast<uint64_t>(shapeWordOf(layout.shape)) << 32 | orderAndCount;
    };
    auto homeOf = [&](uint32_t orderAndCount) {
        return KeyOrderTransition::hashOf(wordsBefore(orderAndCount), slotKey) & mask;
    };

    Vector<uint32_t, 3> colliding;
    uint32_t first = orderWordOf(100, 1);
    AOT_TEST_CHECK(words.keyOrderTransitions[homeOf(first)].isEmpty() && words.keyOrderTransitions[(homeOf(first) + 1) & mask].isEmpty());
    colliding.append(first);
    for (uint32_t order = 101; colliding.size() < 3; ++order) {
        AOT_TEST_CHECK(order < maxKeyOrdersPerLayout);
        if (homeOf(orderWordOf(order, 1)) == homeOf(first))
            colliding.append(orderWordOf(order, 1));
    }

    KeyOrderTable& keyOrders = program.state().objects->keyOrders;
    for (uint32_t orderAndCount : colliding)
        AOT_TEST_CHECK(!probe(words, wordsBefore(orderAndCount), slotKey));

    keyOrders.publishTransition(layout, colliding[0], field, orderWordOf(200, 2));
    keyOrders.publishTransition(layout, colliding[1], field, orderWordOf(201, 2));
    AOT_TEST_CHECK(words.keyOrderTransitionMask == mask);

    const KeyOrderTransition* inItsHome = probe(words, wordsBefore(colliding[0]), slotKey);
    const KeyOrderTransition* displaced = probe(words, wordsBefore(colliding[1]), slotKey);
    AOT_TEST_CHECK(inItsHome == words.keyOrderTransitions + homeOf(first) && inItsHome->orderAndCountAfter == orderWordOf(200, 2));
    AOT_TEST_CHECK(displaced == words.keyOrderTransitions + ((homeOf(first) + 1) & mask) && displaced->orderAndCountAfter == orderWordOf(201, 2));
    AOT_TEST_CHECK(!probe(words, wordsBefore(colliding[2]), slotKey));
    AOT_TEST_CHECK(!probe(words, wordsBefore(colliding[0]), slotKey + 8));

    keyOrders.publishTransition(layout, colliding[0], field, orderWordOf(200, 2));
    AOT_TEST_CHECK(probe(words, wordsBefore(colliding[0]), slotKey) == inItsHome);
    AOT_TEST_CHECK(words.keyOrderTransitions[(homeOf(first) + 2) & mask].isEmpty());
}

static void testInstanceFollowsItsLayout(LoadedProgram& program)
{
    Instance& instance = program.instance();
    InstanceSizes sizes = instance.tables().instanceSizes;
    uint8_t* base = reinterpret_cast<uint8_t*>(&instance);
    AOT_TEST_CHECK(InstanceLayout::isAddressable(sizes));
    AOT_TEST_CHECK(instance.statics() == base + InstanceLayout::offsetOfStatics(sizes));
    AOT_TEST_CHECK(instance.cards() == base + InstanceLayout::offsetOfCards(sizes));
    AOT_TEST_CHECK(reinterpret_cast<uint8_t*>(&instance.prefix()) == base - sizeof(InstancePrefix));
    AOT_TEST_CHECK(reinterpret_cast<uint8_t*>(instance.siteCaches().data()) == base + InstanceLayout::offsetOfSiteCaches(sizes));
    AOT_TEST_CHECK(instance.siteCaches().size() == sizes.numberOfSiteCaches);
    if (sizes.numberOfStringCells)
        AOT_TEST_CHECK(reinterpret_cast<uint8_t*>(&instance.stringCell(0)) == base + InstanceLayout::offsetOfStringCells);
    for (const RememberedSlot<Slot>& site : instance.siteCaches())
        AOT_TEST_CHECK(site.get().isEmpty());
}

template<LoadedProgramTest body>
static constexpr void (*withOrderedLayout)() = runNativeTestWithLoadedProgram<runtimeRecordTests, programWithOrderedLayout, body>;

std::span<const NativeTest> runtimeRecordTests()
{
    static constexpr NativeTest tests[] = {
        { "runtime-records/zero-is-empty"_s, testZeroIsEmpty },
        { "runtime-records/every-kind-reads-back"_s, testEveryKindReadsBack },
        { "runtime-records/presence-by-kind"_s, testPresenceByKind },
        { "runtime-records/word-index-of-offset"_s, testWordIndexOfOffset },
        { "runtime-records/word-index-addresses-the-property"_s, testWordIndexAddressesTheProperty },
        { "runtime-records/storage-capacity-is-the-engines"_s, testStorageCapacityIsTheEngines },
        { "runtime-records/far-index-degrades"_s, testFarIndexDegrades },
        { "runtime-records/packed-slot-round-trip"_s, testPackedSlotRoundTrip },
        { "runtime-records/far-value-packs-as-value"_s, testFarValuePacksAsValue },
        { "runtime-records/inherited-index-outside-eight-bits-does-not-pack"_s, testInheritedIndexOutsideEightBitsDoesNotPack },
        { "runtime-records/holder-outside-the-cage-does-not-pack"_s, testHolderOutsideTheCageDoesNotPack },
        { "runtime-records/field-of-store-does-not-pack"_s, testFieldOfStoreDoesNotPack },
        { "runtime-records/hidden-word-offset-over-1020-does-not-pack"_s, testHiddenWordOffsetOver1020DoesNotPack },
        { "runtime-records/extra-property-with-structure-does-not-pack"_s, testExtraPropertyWithStructureDoesNotPack },
        { "runtime-records/transition-index-outside-eight-bits-does-not-pack"_s, testTransitionIndexOutsideEightBitsDoesNotPack },
        { "runtime-records/addition-and-constant-never-pack"_s, testAdditionAndConstantNeverPack },
        { "runtime-records/own-inline-value-has-zero-bytes"_s, testOwnInlineValueHasZeroBytes },
        { "runtime-records/chain-bit-is-outside-the-word"_s, testChainBitIsOutsideTheWord },
        { "runtime-records/flag-of-another-kind-does-not-pack"_s, testFlagOfAnotherKindDoesNotPack },
        { "runtime-records/liveness-names-every-cell"_s, testLivenessNamesEveryCell },
        { "runtime-records/misses-before-refill"_s, testMissesBeforeRefill },
        { "runtime-records/instance-layout-offsets"_s, testInstanceLayoutOffsets },
        { "runtime-records/instance-layout-addressability"_s, testInstanceLayoutAddressability },
        { "runtime-records/instance-follows-its-layout"_s, withOrderedLayout<testInstanceFollowsItsLayout> },
        { "runtime-records/key-order-transition-hash-spreads"_s, testKeyOrderTransitionHashSpreads },
        { "runtime-records/key-order-transition-probing"_s, withOrderedLayout<testKeyOrderTransitionProbing> },
        { "runtime-records/slot-keys"_s, testSlotKeys },
        { "runtime-records/order-word"_s, testOrderWord },
        { "runtime-records/shape-word"_s, testShapeWord },
        { "runtime-records/offsets-of-runtime-words"_s, testOffsetsOfRuntimeWords },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
