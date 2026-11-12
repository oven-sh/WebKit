/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCodeSites.h"
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
#include <wtf/text/MakeString.h>

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
    case SlotPayload::Key:
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
    if (flags & Slot::needsHandler)
        return false;
    static_assert(Slot::inheritedIsOutOfLine == Slot::storageHasIndexingHeader);
    bool namesPlaceInHolder = Slot::payloadOf(kind) == SlotPayload::FieldAndHolder && Slot::locationOf(kind) == SlotLocation::WordIndex;
    if ((flags & Slot::storageHasIndexingHeader) && !(flags & Slot::growsStorage) && !namesPlaceInHolder)
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
    case SlotPayload::Key:
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

static void testFarIndexDegrades()
{
    Realms realms;
    Cells cells(realms);

    Slot nearSlot = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn, nullptr, lastOffsetInLocation);
    AOT_TEST_CHECK(nearSlot.kind == Slot::Kind::Value && nearSlot.wordIndex() == -32768 && nearSlot.has(Slot::isOutOfLine));

    Slot farSlot = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::isOwn | Slot::dependsOnChain, nullptr, firstFarOffset);
    AOT_TEST_CHECK(farSlot.kind == Slot::Kind::FarValue && farSlot.offsetInStorage() == firstFarOffset);
    AOT_TEST_CHECK(farSlot.flags == (Slot::isOwn | Slot::isOutOfLine | Slot::dependsOnChain | Slot::needsHandler));

    Slot inherited = Slot::createInStorage(cells.id(), Slot::Kind::Value, Slot::dependsOnChain, cells.holder, firstFarOffset);
    AOT_TEST_CHECK(inherited.kind == Slot::Kind::Uncacheable && inherited.structureID == cells.id() && !inherited.payloadBits());
    AOT_TEST_CHECK(inherited.flags == (Slot::dependsOnChain | Slot::needsHandler));

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

static void testForeignInstanceSlotStaysUnpacked()
{
    Realms realms;
    Cells cells(realms);
    SlotData slotData = SlotData::create(32, SlotRepresentation::Cell32);
    constexpr uint8_t flags = Slot::isOwn | Slot::isInModuleInstance;
    AOT_TEST_CHECK(packsAndUnpacks(Slot::createField(cells.id(), Slot::Kind::Variable, flags, slotData, 0, 0), cells.cageBase));
    AOT_TEST_CHECK(!PackedSlot::tryPack(Slot::createField(cells.id(), Slot::Kind::Variable, flags, slotData, 1, 0), testName, cells.cageBase));
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

static void testInstanceLayoutOffsets()
{
    static_assert(AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader) >= std::numeric_limits<int8_t>::min() && sizeof(InstancePrefix) <= 256);
    const InstanceSizes allSizes[] = {
        { 0, 0, 0 },
        { 1, 1, 1 },
        { 3 * 64 * KB + 8, 5000, 5000 },
        { 3 * 64 * KB + 8, 5000, 5000, 1, 4 },
        { 32 * MB, 1000000, 50000, 750000, 4 * MB + 4 },
    };
    for (InstanceSizes sizes : allSizes) {
        AOT_TEST_CHECK(InstanceLayout::isAddressable(sizes));

        ptrdiff_t cards = InstanceLayout::offsetOfCards(sizes);
        ptrdiff_t statics = InstanceLayout::offsetOfStatics(sizes);
        ptrdiff_t pollPage = InstanceLayout::offsetOfLoopPollPage();
        ptrdiff_t prefix = -static_cast<ptrdiff_t>(sizeof(InstancePrefix));
        AOT_TEST_CHECK(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance(sizes)) <= cards);
        AOT_TEST_CHECK(cards < statics && statics < pollPage && prefix < 0);
        AOT_TEST_CHECK(statics + static_cast<ptrdiff_t>(sizes.sizeOfStatics) <= pollPage);
        ptrdiff_t endOfPollPage = pollPage + static_cast<ptrdiff_t>(InstanceLayout::bytesReservedForLoopPollPage);
        if (pollPage < 0)
            AOT_TEST_CHECK(endOfPollPage <= prefix);
        else
            AOT_TEST_CHECK(static_cast<ptrdiff_t>(sizeof(Instance)) <= pollPage && endOfPollPage <= static_cast<ptrdiff_t>(InstanceLayout::offsetOfTablesAboveInstance) && pollPage + static_cast<ptrdiff_t>(4 * KB) <= 32 * static_cast<ptrdiff_t>(KB));
        AOT_TEST_CHECK(!isARM64() || !hasFaultingLoopPolls || pollPage > 0);
        AOT_TEST_CHECK(cards + static_cast<ptrdiff_t>((static_cast<size_t>(sizes.sizeOfStatics) + (1 << InstanceLayout::cardShift) - 1) >> InstanceLayout::cardShift) <= statics);

        size_t stringCells = InstanceLayout::offsetOfStringCell(0);
        size_t end = InstanceLayout::bytesReservedFromInstance(sizes);
        AOT_TEST_CHECK(sizeof(Instance) <= stringCells && stringCells == 384 * KB + InstanceLayout::shiftAboveLoopPollPage);
        AOT_TEST_CHECK(InstanceLayout::beginsAtPage(pollPage) && InstanceLayout::beginsAtPage(statics));
        AOT_TEST_CHECK(InstanceLayout::beginsAtPage(-static_cast<ptrdiff_t>(InstanceLayout::bytesReservedBelowInstance(sizes))) && InstanceLayout::beginsAtPage(end));
        AOT_TEST_CHECK(!(InstanceLayout::bytesReservedForLoopPollPage % InstanceLayout::largestPageSize));
        AOT_TEST_CHECK(CeilingOnPageSize <= InstanceLayout::largestPageSize || !hasFaultingLoopPolls);
        size_t bindings = InstanceLayout::offsetOfGlobalBindings(sizes);
        AOT_TEST_CHECK(InstanceLayout::offsetOfStringCell(sizes.numberOfStringCells) <= bindings && !(bindings % alignof(GlobalBinding)));
        AOT_TEST_CHECK(bindings + sizes.numberOfGlobalBindings * sizeof(GlobalBinding) <= end);
        size_t methodTable = InstanceLayout::offsetOfMethodTable(sizes);
        size_t sites = InstanceLayout::offsetOfSiteCaches(sizes);
        AOT_TEST_CHECK(InstanceLayout::endOfGlobalBindings(sizes) <= methodTable && InstanceLayout::beginsAtPage(methodTable));
        AOT_TEST_CHECK(methodTable + sizes.sizeOfMethodTable <= sites && InstanceLayout::beginsAtPage(sites));
        AOT_TEST_CHECK(InstanceLayout::offsetOfSiteCache(sizes, sizes.numberOfSiteCaches) <= end);
    }

    AOT_TEST_CHECK(InstanceLayout::offsetOfStringCell(7) == InstanceLayout::offsetOfStringCells + 7 * sizeof(uint32_t));
    AOT_TEST_CHECK(InstanceLayout::offsetOfStatics({ 64 * KB + 1, 0, 0 }) < InstanceLayout::offsetOfStatics({ 64 * KB, 0, 0 }));
    AOT_TEST_CHECK(InstanceLayout::bytesReservedFromInstance({ 0, 0, 1 }) < InstanceLayout::bytesReservedFromInstance({ 0, 0, 100000 }));

    size_t mostKeys = (InstanceLayout::offsetOfBuiltinValues - InstanceLayout::offsetOfBuiltinCells) / sizeof(JSCell*);
    AOT_TEST_CHECK(InstanceLayout::holdsBuiltinCells(mostKeys) && !InstanceLayout::holdsBuiltinCells(mostKeys + 1));
    AOT_TEST_CHECK(InstanceLayout::offsetOfBuiltinValues + mostKeys * sizeof(EncodedJSValue) == InstanceLayout::offsetOfStringCells);
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

template<LoadedProgramTest body>
static constexpr void (*withOrderedLayout)() = runNativeTestWithLoadedProgram<runtimeRecordTests, programWithOrderedLayout, body>;

std::span<const NativeTest> runtimeRecordTests()
{
    static constexpr NativeTest tests[] = {
        { "runtime-records/zero-is-empty"_s, testZeroIsEmpty },
        { "runtime-records/far-index-degrades"_s, testFarIndexDegrades },
        { "runtime-records/packed-slot-round-trip"_s, testPackedSlotRoundTrip },
        { "runtime-records/inherited-index-outside-eight-bits-does-not-pack"_s, testInheritedIndexOutsideEightBitsDoesNotPack },
        { "runtime-records/holder-outside-the-cage-does-not-pack"_s, testHolderOutsideTheCageDoesNotPack },
        { "runtime-records/slot-naming-other-instance-does-not-pack"_s, testForeignInstanceSlotStaysUnpacked },
        { "runtime-records/hidden-word-offset-over-1020-does-not-pack"_s, testHiddenWordOffsetOver1020DoesNotPack },
        { "runtime-records/transition-index-outside-eight-bits-does-not-pack"_s, testTransitionIndexOutsideEightBitsDoesNotPack },
        { "runtime-records/own-inline-value-has-zero-bytes"_s, testOwnInlineValueHasZeroBytes },
        { "runtime-records/instance-layout-offsets"_s, testInstanceLayoutOffsets },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
