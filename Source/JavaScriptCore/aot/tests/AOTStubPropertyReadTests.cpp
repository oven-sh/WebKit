/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTPropertyCachesWords.h"
#include "AOTFieldTable.h"
#include "AOTFunctionData.h"
#include "AOTImageText.h"
#include "AOTLayoutData.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeRecords.h"
#include "AOTSlotAccess.h"
#include "AOTSlotRepresentation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubRunner.h"
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyKeys.h"
#include "GetterSetter.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "StructureID.h"
#include "Symbol.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static constexpr uint64_t untouched = 0xa5a5a5a5a5a5a5a5ULL;

namespace {

class PropertyReads {
    WTF_MAKE_NONCOPYABLE(PropertyReads);
public:
    static constexpr uint16_t name = 4;
    // A symbol or a private name. The image has no atom for it.
    static constexpr uint16_t nameMadeAtRunTime = 6;
    static constexpr unsigned numberOfNames = 8;
    static constexpr uint32_t symbolMask = 3;
    static constexpr uint32_t structureID = 0x1000;
    static constexpr uint32_t structureIDOfHolder = 0x0800;

    PropertyReads()
    {
        Instance& instance = runner.instance();
        m_rows[structureIDOfHolder >> StructureRowWords::granuleShift] = numberOfNames;
        for (SlotAccess access : allSlotAccesses)
            instance.propertyCaches.front[static_cast<unsigned>(access)] = { m_rows.data(), m_planes[static_cast<unsigned>(access)].data() };
        instance.propertyCaches.hashedSlots = m_hashedSlots.data();
        instance.propertyCaches.hashedSlotOffsetMask = HashedSlot::offsetMaskFor(m_hashedSlots.size());
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        for (unsigned i = 1; i < numberOfNames; ++i) {
            if (i == nameMadeAtRunTime)
                continue;
            m_identifiers[i].header.length = 1;
            m_identifiers[i].number = i;
            m_identifierOffsets[i] = i * sizeof(LazyStringHeader);
        }
        instance.image.stringRecords = m_stringRecords.data();
        instance.image.lazyStringHeaderOffsets = m_identifierOffsets.data();
        instance.lazyStrings.headers = reinterpret_cast<uint8_t*>(m_identifiers.data());
        instance.lazyStrings.capacity = sizeof(m_identifiers);
        instance.image.programSymbols = &m_symbols;
        instance.cellCageBase = MarkedBlockCage::baseOf(this);
    }

    UniquedStringImpl* uid() { return reinterpret_cast<UniquedStringImpl*>(&m_identifiers[name]); }

    // The next header that is made takes the place that the name had.
    void forgetHeader(uint16_t number, ImageStringRecord record, const uint8_t* text)
    {
        m_identifiers[number] = { };
        m_identifierOffsets[number] = 0;
        m_stringRecords[number] = record;
        arena().end = number * sizeof(LazyStringHeader);
        runner.instance().image.text = text;
    }

    LazyStringHeaderArena& arena() { return reinterpret_cast<LazyStringHeaderArena&>(m_identifiers[0]); }
    LazyStringHeader& header(uint16_t number) { return m_identifiers[number]; }
    uint32_t offsetOfHeader(uint16_t number) const { return m_identifierOffsets[number]; }
#else
        for (unsigned i = 1; i < numberOfNames; ++i)
            m_identifiers[i].length = i != nameMadeAtRunTime;
        instance.image.programIdentifiers = reinterpret_cast<UniquedStringImpl*>(m_identifiers.data());
        instance.image.programSymbols = &m_symbols;
        instance.cellCageBase = MarkedBlockCage::baseOf(this);
    }

    UniquedStringImpl* uid() { return runner.instance().image.programIdentifiers + name; }
#endif

    // The entries stand from the first place of nameMadeAtRunTime on.
    void setSymbols(std::initializer_list<ProgramSymbol> entries)
    {
        m_symbolEntries.fill(ProgramSymbol { });
        std::ranges::copy(entries, m_symbolEntries.begin() + (nameMadeAtRunTime & symbolMask));
    }

    void clear()
    {
        m_hashedSlots.fill(HashedSlot { });
        m_planes[static_cast<unsigned>(SlotAccess::Read)].fill(PackedSlot { });
    }

    void file(const Slot& slot)
    {
        clear();
        fileBesideOthers(slot);
    }

    void file(const Slot& slot, UniquedStringImpl* uidOfName)
    {
        clear();
        fileBesideOthers(slot, uidOfName);
    }

    void fileBesideOthers(const Slot& slot) { fileBesideOthers(slot, uid()); }

    void fileBesideOthers(const Slot& slot, UniquedStringImpl* uidOfName)
    {
        uint32_t firstWay = HashedSlot::offsetOfFirstWay(slot.structureID, uidOfName, runner.instance().propertyCaches.hashedSlotOffsetMask);
        for (unsigned way = 0; way < HashedSlot::ways; ++way) {
            HashedSlot& entry = m_hashedSlots[HashedSlot::offsetOfWay(firstWay, way) >> HashedSlot::sizeShift];
            if (!entry.isEmpty())
                continue;
            entry.slot = slot;
            entry.key = HashedSlot::keyFor(uidOfName, SlotAccess::Read);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void fileInFrontBesideOthers(const Slot& slot)
    {
        uint32_t row = m_rows[slot.structureID.bits() >> StructureRowWords::granuleShift];
        m_planes[static_cast<unsigned>(SlotAccess::Read)][row + name] = *PackedSlot::tryPack(slot, name, runner.instance().cellCageBase);
    }

    StubRun read(uint64_t receiver) { return runner.run(getById(), { receiver }, name); }
    StubRun read(uint64_t receiver, uint16_t otherName) { return runner.run(getById(), { receiver }, otherName); }

    StubRunner runner;

private:
    std::array<uint32_t, (structureID >> StructureRowWords::granuleShift) + 1> m_rows { };
    std::array<std::array<PackedSlot, 2 * numberOfNames>, numberOfSlotAccesses> m_planes { };
    std::array<HashedSlot, 64> m_hashedSlots { };
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::array<LazyStringHeader, numberOfNames> m_identifiers { };
    std::array<uint32_t, numberOfNames> m_identifierOffsets { };
    std::array<ImageStringRecord, numberOfNames> m_stringRecords { };
#else
    std::array<ImageStringHeader, numberOfNames> m_identifiers { };
#endif
    std::array<ProgramSymbol, ProgramSymbols::numberOfEntriesFor(symbolMask + 1)> m_symbolEntries { };
    ProgramSymbols m_symbols { m_symbolEntries.data(), symbolMask, 0 };
};

struct FakeTypedObject : FakeObject<offsetOfFirstField + 2 * sizeof(uint64_t)> {
    static constexpr uint32_t offsetOfField = offsetOfFirstField;

    FakeTypedObject()
    {
        setHeader(PropertyReads::structureID, CompiledObjectType);
        field() = untouched;
    }

    uint64_t& field() { return at<uint64_t>(offsetOfField); }
};

} // anonymous namespace

static Slot createFieldSlot(SlotRepresentation representation)
{
    return Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn, SlotData::create(FakeTypedObject::offsetOfField, representation), 0, 0);
}

static bool tryWrite(SlotRepresentation representation, void* slot, JSValue value, uintptr_t cageBase)
{
    SlotTraits traits = traitsOf(representation);
    if (traits.valueKind == SlotValueKind::DeferredClosure)
        return false;
    if (traits.valueKind == SlotValueKind::Boxed) {
        *static_cast<uint64_t*>(slot) = bitsOf(value);
        return true;
    }
    if (value.isCell()) {
        if (!traits.holdsCell)
            return false;
        if (traits.isNarrowReference)
            *static_cast<uint32_t*>(slot) = MarkedBlockCage::encode(cageBase, value.asCell());
        else
            *static_cast<uint64_t*>(slot) = bitsOf(value);
        return true;
    }
    if (value.isInt32())
        return tryWriteInt32(representation, slot, value.asInt32());
    if (value.isDouble())
        return tryWriteDouble(representation, slot, value.asDouble());
    if (value.isBoolean())
        return tryWriteBoolean(representation, slot, value.asBoolean());
    return false;
}

static void testTypedFields()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(cell.bytes.data()) == cageBase);
    const JSValue values[] = {
        jsNumber(0), jsNumber(7), jsNumber(-1), jsNumber(200), jsNumber(-200), jsNumber(70000), jsNumber(INT32_MAX), jsNumber(INT32_MIN),
        jsDoubleNumber(0.5), jsDoubleNumber(1e300), jsDoubleNumber(-0.0), jsDoubleNumber(4294967295.0),
        jsBoolean(true), jsBoolean(false), JSValue(std::bit_cast<JSCell*>(cell.address())),
    };
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        reads.file(createFieldSlot(representation));
        bool wasRead = false;
        for (JSValue value : values) {
            FakeTypedObject object;
            if (!tryWrite(representation, &object.field(), value, cageBase))
                continue;
            StubRun run = reads.read(object.address());
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
            wasRead = true;
        }
        representationsRead += wasRead;
    }
    AOT_TEST_CHECK(representationsRead == numberOfEnumerators<SlotRepresentation> - 1);
}

namespace {

struct FakeHolder : FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(uint64_t)> {
    static constexpr PropertyOffset inlineOffset = 1;
    static constexpr PropertyOffset outOfLineOffset = firstOutOfLineOffset + 1;

    FakeHolder()
    {
        setHeader(0, FinalObjectType);
        at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uint64_t>(m_storage.data() + m_storage.size());
    }

    uint64_t& property(PropertyOffset offset)
    {
        int32_t wordIndex = Slot::wordIndexFor(offset);
        return isInlineOffset(offset) ? at<uint64_t>(wordIndex * sizeof(uint64_t)) : m_storage[m_storage.size() + wordIndex];
    }

private:
    std::array<uint64_t, 4> m_storage { };
};

} // anonymous namespace

static Slot createFieldOrInheritedSlot(SlotRepresentation representation, FakeHolder& holder, PropertyOffset offset, uint8_t flags, uintptr_t cageBase)
{
    SlotData field = SlotData::create(FakeTypedObject::offsetOfField, representation);
    return *Slot::tryCreateFieldAndHolder(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::FieldOrInherited, flags | Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), offset, cageBase);
}

static void testFieldsOrInheritedValues()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeHolder holder;
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(holder.bytes.data()) == cageBase);
    holder.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    holder.property(FakeHolder::outOfLineOffset) = bitsOf(jsNumber(12));
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        if (!traits.hasAbsent || traits.valueKind == SlotValueKind::DeferredClosure)
            continue;
        for (PropertyOffset offset : { FakeHolder::inlineOffset, FakeHolder::outOfLineOffset }) {
            reads.file(createFieldOrInheritedSlot(representation, holder, offset, 0, cageBase));

            FakeTypedObject objectWithoutField;
            writeSlotBits(representation, &objectWithoutField.field(), traits.absentBits);
            StubRun inherited = reads.read(objectWithoutField.address());
            AOT_TEST_CHECK(!inherited.calledOperation && inherited.result == holder.property(offset));

            for (JSValue value : { jsNumber(7), jsBoolean(true) }) {
                FakeTypedObject object;
                if (!tryWrite(representation, &object.field(), value, cageBase))
                    continue;
                StubRun own = reads.read(object.address());
                AOT_TEST_CHECK(!own.calledOperation && own.result == bitsOf(value));
                ++representationsRead;
            }
        }
    }
    AOT_TEST_CHECK(representationsRead);
}

// A field that may exist without storage is undefined where the key order lists it and its slot is empty. The stub answers where the objects above the receiver answer undefined too.
static void testEmptySlotsThatMayBeUndefined()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    constexpr SlotRepresentation representation = SlotRepresentation::JSValue;
    FakeTypedObject objectWithoutField;
    writeSlotBits(representation, &objectWithoutField.field(), traitsOf(representation).absentBits);
    FakeTypedObject objectWithField;
    AOT_TEST_CHECK(tryWrite(representation, &objectWithField.field(), jsNumber(7), cageBase));
    auto readsField = [&] {
        StubRun own = reads.read(objectWithField.address());
        return !own.calledOperation && own.result == bitsOf(jsNumber(7));
    };

    Slot field = createFieldSlot(representation);
    reads.file(field);
    AOT_TEST_CHECK(reads.read(objectWithoutField.address()).calledOperation && readsField());
    field.flags |= Slot::dependsOnChain | Slot::emptySlotMayBeUndefined;
    AOT_TEST_CHECK(!PackedSlot::tryPack(field, PropertyReads::name, cageBase));
    reads.file(field);
    StubRun nothingAbove = reads.read(objectWithoutField.address());
    AOT_TEST_CHECK(!nothingAbove.calledOperation && nothingAbove.result == bitsOf(jsUndefined()) && readsField());

    FakeHolder holder;
    for (PropertyOffset offset : { FakeHolder::inlineOffset, FakeHolder::outOfLineOffset }) {
        reads.file(createFieldOrInheritedSlot(representation, holder, offset, Slot::emptySlotMayBeUndefined, cageBase));
        holder.property(offset) = bitsOf(jsUndefined());
        StubRun undefinedAbove = reads.read(objectWithoutField.address());
        AOT_TEST_CHECK(!undefinedAbove.calledOperation && undefinedAbove.result == bitsOf(jsUndefined()) && readsField());
        for (JSValue inherited : { jsNumber(11), jsNull() }) {
            holder.property(offset) = bitsOf(inherited);
            AOT_TEST_CHECK(reads.read(objectWithoutField.address()).calledOperation && readsField());
        }
    }
}

static void testFieldsInExtensionOrInheritedValues()
{
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = 8;
    PropertyReads reads;
    Instance& instance = reads.runner.instance();
    uintptr_t cageBase = instance.cellCageBase;
    FakeObject<sizeof(CompiledStructure)> structure;
    instance.prefix().structureIDBase = structure.address() - PropertyReads::structureID;
    structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfExtensionWord, SlotRepresentation::Cell32).encoded();
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    SlotData field = SlotData::create(offsetInExtension, SlotRepresentation::JSValue);
    reads.file(*Slot::tryCreateFieldAndHolder(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::FieldOrInherited, Slot::isOutOfLine | Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), FakeHolder::inlineOffset, cageBase));

    // The page of zeros that stands for "no Extension" belongs to a real cage. Here an Extension without a granule takes its place.
    FakeTypedObject object;
    FakeObject<offsetInExtension + sizeof(uint64_t)> extension;
    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = 0;
    object.at<uint32_t>(offsetOfExtensionWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
    extension.at<uint64_t>(offsetInExtension) = bitsOf(jsNumber(7));
    StubRun run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(11)));

    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBitOf(offsetInExtension);
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));

    extension.at<uint64_t>(offsetInExtension) = JSValue::ValueEmpty;
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(11)));
}

static void testFieldsOrInheritedGetters()
{
    constexpr uint32_t slimClosureStructureID = 0x1080;
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    reads.runner.instance().slimClosureHeaders[0] = slimClosureStructureID;
    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> getter;
    getter.setHeader(slimClosureStructureID, SlimClosureType);
    getter.at<uint64_t>(offsetOfSlimClosureWord) = reads.runner.closureWord(0);
    FakeObject<sizeof(GetterSetter)> accessors;
    accessors.at<uint64_t>(GetterSetter::offsetOfGetter()) = getter.address();
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = accessors.address();
    reads.file(createFieldOrInheritedSlot(SlotRepresentation::JSValue, holder, FakeHolder::inlineOffset, Slot::inheritsGetter, cageBase));

    FakeTypedObject objectWithoutField;
    objectWithoutField.field() = JSValue::ValueEmpty;
    StubRun run = reads.read(objectWithoutField.address());
    AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::CompiledFunction && run.result == StubRunner::resultOfCompiledFunction);
    AOT_TEST_CHECK(run.landedCall.values.callee == getter.address() && run.landedCall.values.thisValue == objectWithoutField.address() && !static_cast<uint32_t>(run.landedCall.count));

    FakeTypedObject object;
    object.field() = bitsOf(jsNumber(7));
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere && run.result == bitsOf(jsNumber(7)));
}

namespace {

struct FakeTypedHolder : FakeTypedObject {
    FakeTypedHolder() { setHeader(PropertyReads::structureIDOfHolder, CompiledObjectType); }
};

enum class FiledIn : bool { HashedTable, Front };

} // anonymous namespace

static Slot createFieldSlotOfHolder(SlotRepresentation representation)
{
    return Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder), Slot::Kind::Field, Slot::isOwn, SlotData::create(FakeTypedObject::offsetOfField, representation), 0, 0);
}

static void fileBesideOthers(PropertyReads& reads, FiledIn filedIn, const Slot& slot)
{
    if (filedIn == FiledIn::Front)
        reads.fileInFrontBesideOthers(slot);
    else
        reads.fileBesideOthers(slot);
}

static void testInheritedFields()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = std::bit_cast<uintptr_t>(reads.uid());
    const JSValue values[] = { jsNumber(7), jsNumber(-200), jsNumber(70000), jsDoubleNumber(0.5), jsBoolean(true), jsBoolean(false) };
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        bool wasRead = false;
        for (JSValue value : values) {
            FakeTypedHolder holder;
            AOT_TEST_CHECK(MarkedBlockCage::baseOf(holder.bytes.data()) == cageBase);
            if (!tryWrite(representation, &holder.field(), value, cageBase))
                continue;
            const Slot inherited = Slot::createInheritedField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::dependsOnChain, std::bit_cast<JSCell*>(holder.address()));
            FakeTypedObject receiver;
            for (FiledIn receiverIsFiledIn : { FiledIn::HashedTable, FiledIn::Front }) {
                for (FiledIn holderIsFiledIn : { FiledIn::HashedTable, FiledIn::Front }) {
                    reads.clear();
                    fileBesideOthers(reads, receiverIsFiledIn, inherited);
                    fileBesideOthers(reads, holderIsFiledIn, createFieldSlotOfHolder(representation));
                    StubRun run = reads.read(receiver.address());
                    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value) && receiver.field() == untouched);
                }
            }

            reads.file(inherited);
            reads.fileBesideOthers(createFieldSlotOfHolder(representation));
            StubRun byKey = reads.runner.run(getByValCached(), { receiver.address(), key.address() });
            AOT_TEST_CHECK(!byKey.calledOperation && byKey.result == bitsOf(value));
            wasRead = true;
        }
        representationsRead += wasRead;
    }
    AOT_TEST_CHECK(representationsRead > 4);
}

// What the hashed table has answered once for the cell of a key and a Structure is answered again without it: an own value, a field in the cell, and that nothing is found.
static void testPlacesOfKeyedValues()
{
    PropertyReads reads;
    reads.runner.setKeepsKeyedValuePlacesAcrossRuns(true);
    Instance& instance = reads.runner.instance();
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = std::bit_cast<uintptr_t>(reads.uid());
    FakeObject<sizeof(Symbol)> otherKey;
    otherKey.setHeader(0, SymbolType);
    otherKey.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = std::bit_cast<uintptr_t>(reads.uid());
    constexpr PropertyOffset inlineOffset = 1;
    FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(EncodedJSValue)> receiver;
    receiver.setHeader(PropertyReads::structureID, FinalObjectType);
    receiver.at<uint64_t>(JSObject::offsetOfInlineStorage() + inlineOffset * sizeof(EncodedJSValue)) = bitsOf(jsNumber(41));
    std::array<uint64_t, 4> storage { bitsOf(jsNumber(1)), bitsOf(jsNumber(2)), bitsOf(jsNumber(3)), bitsOf(jsNumber(4)) };
    receiver.at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(storage.data() + storage.size());
    auto numberOfPlaces = [&] {
        return std::ranges::count_if(instance.keyedValuePlaces(), [](const KeyedValuePlace& place) { return !!place.key; });
    };
    auto read = [&](auto& keyCell) {
        return reads.runner.run(getByValCached(), { receiver.address(), keyCell.address() });
    };
    auto forget = [&] {
        zeroSpan(instance.keyedValuePlaces());
    };

    for (PropertyOffset offset : { inlineOffset, firstOutOfLineOffset, firstOutOfLineOffset + 1 }) {
        forget();
        uint64_t expected = isInlineOffset(offset) ? bitsOf(jsNumber(41)) : storage[storage.size() + Slot::wordIndexFor(offset)];
        reads.file(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, offset));
        StubRun first = read(key);
        AOT_TEST_CHECK(!first.calledOperation && first.result == expected && numberOfPlaces() == 1);
        reads.clear();
        StubRun second = read(key);
        AOT_TEST_CHECK(!second.calledOperation && second.result == expected && numberOfPlaces() == 1);
        for (PresenceQuestion question : { PresenceQuestion::In, PresenceQuestion::HasOwn }) {
            StubRun asked = reads.runner.run(inByValCached(question), { receiver.address(), key.address() });
            AOT_TEST_CHECK(!asked.calledOperation && asked.result == 1);
        }
        // Another cell with the same name has no place, and an object of another Structure finds none.
        AOT_TEST_CHECK(read(otherKey).calledOperation);
        receiver.setHeader(PropertyReads::structureIDOfHolder, FinalObjectType);
        AOT_TEST_CHECK(read(key).calledOperation);
        receiver.setHeader(PropertyReads::structureID, FinalObjectType);
    }

    // A value of an object above gets no place.
    forget();
    FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(EncodedJSValue)> holder;
    holder.setHeader(PropertyReads::structureIDOfHolder, FinalObjectType);
    holder.at<uint64_t>(JSObject::offsetOfInlineStorage() + inlineOffset * sizeof(EncodedJSValue)) = bitsOf(jsNumber(43));
    reads.file(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::dependsOnChain, std::bit_cast<JSCell*>(holder.address()), inlineOffset));
    StubRun inherited = read(key);
    AOT_TEST_CHECK(!inherited.calledOperation && inherited.result == bitsOf(jsNumber(43)) && !numberOfPlaces());

    reads.file(Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain));
    StubRun absent = read(key);
    AOT_TEST_CHECK(!absent.calledOperation && absent.result == bitsOf(jsUndefined()) && numberOfPlaces() == 1 && instance.propertyCaches.placesSayAbsence);
    reads.clear();
    StubRun stillAbsent = read(key);
    AOT_TEST_CHECK(!stillAbsent.calledOperation && stillAbsent.result == bitsOf(jsUndefined()));
    for (PresenceQuestion question : { PresenceQuestion::In, PresenceQuestion::HasOwn }) {
        StubRun asked = reads.runner.run(inByValCached(question), { receiver.address(), key.address() });
        AOT_TEST_CHECK(!asked.calledOperation && !asked.result);
    }
    // A read of a string fills a place as well. `in` throws for a string, so no place answers it.
    receiver.setHeader(PropertyReads::structureID, StringType);
    for (PresenceQuestion question : { PresenceQuestion::In, PresenceQuestion::HasOwn })
        AOT_TEST_CHECK(reads.runner.run(inByValCached(question), { receiver.address(), key.address() }).calledOperation);
    receiver.setHeader(PropertyReads::structureID, FinalObjectType);

    // A field in the cell, in every form that a slot has. An empty slot is left to the tables, and so is a field outside the cell.
    uintptr_t cageBase = instance.cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    const JSValue values[] = { jsNumber(7), jsNumber(-200), jsNumber(INT32_MIN), jsDoubleNumber(0.5), jsDoubleNumber(-0.0), jsBoolean(true), JSValue(std::bit_cast<JSCell*>(cell.address())) };
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        bool wasRead = false;
        for (JSValue value : values) {
            FakeTypedObject object;
            if (!tryWrite(representation, &object.field(), value, cageBase))
                continue;
            auto readField = [&] {
                return reads.runner.run(getByValCached(), { object.address(), key.address() });
            };
            forget();
            reads.file(createFieldSlot(representation));
            StubRun first = readField();
            AOT_TEST_CHECK(!first.calledOperation && first.result == bitsOf(value) && numberOfPlaces() == 1);
            reads.clear();
            StubRun second = readField();
            AOT_TEST_CHECK(!second.calledOperation && second.result == bitsOf(value));
            for (PresenceQuestion question : { PresenceQuestion::In, PresenceQuestion::HasOwn }) {
                StubRun asked = reads.runner.run(inByValCached(question), { object.address(), key.address() });
                AOT_TEST_CHECK(!asked.calledOperation && asked.result == 1);
            }
            AOT_TEST_CHECK(reads.runner.run(getByValCached(), { object.address(), otherKey.address() }).calledOperation);
            if (SlotTraits traits = traitsOf(representation); traits.hasAbsent) {
                memcpy(&object.field(), &traits.absentBits, traits.width);
                AOT_TEST_CHECK(readField().calledOperation);
                StubRun askedOfEmptySlot = reads.runner.run(inByValCached(PresenceQuestion::HasOwn), { object.address(), key.address() });
                AOT_TEST_CHECK(askedOfEmptySlot.calledOperation || !askedOfEmptySlot.result);
            }

            // The question whether the name is there leaves a place as a read does.
            forget();
            AOT_TEST_CHECK(tryWrite(representation, &object.field(), value, cageBase));
            reads.file(createFieldSlot(representation));
            StubRun asked = reads.runner.run(inByValCached(PresenceQuestion::In), { object.address(), key.address() });
            AOT_TEST_CHECK(!asked.calledOperation && asked.result == 1 && numberOfPlaces() == 1);
            reads.clear();
            StubRun readBehindQuestion = readField();
            AOT_TEST_CHECK(!readBehindQuestion.calledOperation && readBehindQuestion.result == bitsOf(value));
            wasRead = true;
        }
        representationsRead += wasRead;
    }
    AOT_TEST_CHECK(representationsRead == numberOfEnumerators<SlotRepresentation> - 1);

    forget();
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = 8;
    FakeTypedObject objectWithExtension;
    FakeObject<offsetInExtension + sizeof(uint64_t)> extension;
    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBitOf(offsetInExtension);
    extension.at<uint64_t>(offsetInExtension) = bitsOf(jsNumber(7));
    objectWithExtension.at<uint32_t>(offsetOfExtensionWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
    reads.file(Slot::createField(structureID, Slot::Kind::Field, Slot::isOwn | Slot::isOutOfLine, SlotData::create(offsetInExtension, SlotRepresentation::JSValue), 0, offsetOfExtensionWord));
    StubRun outsideCell = reads.runner.run(getByValCached(), { objectWithExtension.address(), key.address() });
    AOT_TEST_CHECK(!outsideCell.calledOperation && outsideCell.result == bitsOf(jsNumber(7)));
    AOT_TEST_CHECK(!numberOfPlaces());
}

// What the tables do not say of the holder is asked of the run time with the holder as the receiver.
static void testInheritedFieldsOfHolderThatIsNotFiled()
{
    PropertyReads reads;
    FakeTypedHolder holder;
    FakeTypedObject receiver;
    reads.file(Slot::createInheritedField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::dependsOnChain, std::bit_cast<JSCell*>(holder.address())));
    reads.runner.recordCallsOf(Entry::operationAOTGetByIdAndCache);
    reads.runner.setResultOfRecordedOperation(bitsOf(jsNumber(5)));
    StubRun run = reads.read(receiver.address());
    reads.runner.countCallsOf(Entry::operationAOTGetByIdAndCache);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == holder.address() && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name && run.result == bitsOf(jsNumber(5)));
}

static void testFieldsOrInheritedFields()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeTypedHolder holder;
    AOT_TEST_CHECK(tryWrite(SlotRepresentation::Int32, &holder.field(), jsNumber(11), cageBase));
    unsigned representationsRead = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        if (!traits.hasAbsent || traits.valueKind == SlotValueKind::DeferredClosure)
            continue;
        SlotData field = SlotData::create(FakeTypedObject::offsetOfField, representation);
        reads.file(*Slot::tryCreateFieldOrInheritedField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), 0, cageBase));
        reads.fileBesideOthers(createFieldSlotOfHolder(SlotRepresentation::Int32));

        FakeTypedObject objectWithoutField;
        writeSlotBits(representation, &objectWithoutField.field(), traits.absentBits);
        StubRun inherited = reads.read(objectWithoutField.address());
        AOT_TEST_CHECK(!inherited.calledOperation && inherited.result == bitsOf(jsNumber(11)));

        for (JSValue value : { jsNumber(7), jsBoolean(true) }) {
            FakeTypedObject object;
            if (!tryWrite(representation, &object.field(), value, cageBase))
                continue;
            StubRun own = reads.read(object.address());
            AOT_TEST_CHECK(!own.calledOperation && own.result == bitsOf(value));
            ++representationsRead;
        }
    }
    AOT_TEST_CHECK(representationsRead);
}

static void testFieldsInExtensionOrInheritedFields()
{
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = 8;
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeTypedHolder holder;
    AOT_TEST_CHECK(tryWrite(SlotRepresentation::Int32, &holder.field(), jsNumber(11), cageBase));
    SlotData field = SlotData::create(offsetInExtension, SlotRepresentation::JSValue);
    reads.file(*Slot::tryCreateFieldOrInheritedField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::isOutOfLine | Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), offsetOfExtensionWord, cageBase));
    reads.fileBesideOthers(createFieldSlotOfHolder(SlotRepresentation::Int32));

    // The page of zeros that stands for "no Extension" belongs to a real cage. Here an Extension without a granule takes its place.
    FakeTypedObject object;
    FakeObject<offsetInExtension + sizeof(uint64_t)> extension;
    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = 0;
    object.at<uint32_t>(offsetOfExtensionWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
    extension.at<uint64_t>(offsetInExtension) = bitsOf(jsNumber(7));
    StubRun run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(11)));

    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBitOf(offsetInExtension);
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));

    extension.at<uint64_t>(offsetInExtension) = JSValue::ValueEmpty;
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(11)));
}

static void testNamesMadeAtRunTime()
{
    constexpr uint16_t name = PropertyReads::nameMadeAtRunTime;
    constexpr uint16_t otherNameWithSamePlace = name - (PropertyReads::symbolMask + 1);
    static_assert(otherNameWithSamePlace && (otherNameWithSamePlace & PropertyReads::symbolMask) == (name & PropertyReads::symbolMask));
    PropertyReads reads;
    FakeObject<sizeof(StringImpl)> symbol;
    FakeObject<sizeof(StringImpl)> otherSymbol;
    auto* uid = std::bit_cast<UniquedStringImpl*>(static_cast<uintptr_t>(symbol.address()));
    auto* otherUID = std::bit_cast<UniquedStringImpl*>(static_cast<uintptr_t>(otherSymbol.address()));
    FakeTypedObject object;
    object.field() = bitsOf(jsNumber(7));
    reads.file(createFieldSlot(SlotRepresentation::JSValue), uid);
    auto isRead = [&] {
        StubRun run = reads.read(object.address(), name);
        return !run.calledOperation && run.result == bitsOf(jsNumber(7));
    };

    // Nobody has made the name yet.
    reads.setSymbols({ });
    AOT_TEST_CHECK(reads.read(object.address(), name).calledOperation);
    reads.setSymbols({ { otherNameWithSamePlace, otherUID } });
    AOT_TEST_CHECK(reads.read(object.address(), name).calledOperation);

    reads.setSymbols({ { name, uid } });
    AOT_TEST_CHECK(isRead());
    reads.setSymbols({ { otherNameWithSamePlace, otherUID }, { name, uid } });
    AOT_TEST_CHECK(isRead());
}

#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
static void testNamesFirstUsedByStub()
{
    constexpr uint16_t name = 5;
    constexpr uint32_t offsetInText = 2;
    constexpr uint32_t length = 4;
    constexpr uint32_t hash = 0x123456;
    static constexpr std::array<uint8_t, 8> text { '.', '.', 'l', 'a', 't', 'e', '.', '.' };
    const ImageStringRecord record = ImageStringRecord::create(offsetInText << ImageString::lengthBits | length, hash, false, true);
    PropertyReads reads;
    FakeTypedObject object;
    object.field() = bitsOf(jsNumber(7));
    reads.file(createFieldSlot(SlotRepresentation::JSValue), reinterpret_cast<UniquedStringImpl*>(&reads.header(name)));

    reads.forgetHeader(name, record, text.data());
    StubRun run = reads.read(object.address(), name);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    ImageStringHeader expected = ImageStringHeader::create(std::bit_cast<uint64_t>(text.data() + offsetInText), length, hash, false, StringImpl::StringAtom);
    AOT_TEST_CHECK(!memcmp(&reads.header(name).header, &expected, sizeof(expected)) && reads.header(name).number == name);
    AOT_TEST_CHECK(reads.offsetOfHeader(name) == name * sizeof(LazyStringHeader));
    AOT_TEST_CHECK(reads.arena().end == (name + 1) * sizeof(LazyStringHeader) && !reads.arena().isLocked.load());

    // Another thread is making a header.
    reads.forgetHeader(name, record, text.data());
    reads.arena().isLocked.store(1);
    AOT_TEST_CHECK(reads.read(object.address(), name).calledOperation);
    AOT_TEST_CHECK(!reads.offsetOfHeader(name) && reads.arena().end == name * sizeof(LazyStringHeader) && reads.arena().isLocked.load() == 1);
}
#endif

static void testDeferredClosures()
{
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> closure;
    closure.setHeader(0, JSFunctionType);
    const Slot slot = createFieldSlot(SlotRepresentation::DeferredClosure);
    reads.file(slot);

    FakeTypedObject objectWithClosure;
    objectWithClosure.at<uint32_t>(FakeTypedObject::offsetOfField) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(static_cast<uintptr_t>(closure.address())));
    StubRun run = reads.read(objectWithClosure.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == closure.address());

    // The stub looks up the field's record in the object's layout. This layout lists no field, so the read falls back to the operation, with every operand unchanged.
    FakeObject<sizeof(CompiledStructure)> structure;
    LayoutData layout { };
    structure.at<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &layout;
    reads.runner.instance().prefix().structureIDBase = structure.address() - PropertyReads::structureID;

    FakeTypedObject objectBeforeFirstRead;
    objectBeforeFirstRead.at<uint32_t>(FakeTypedObject::offsetOfField) = uncreatedDeferredClosure;
    reads.runner.recordCallsOf(Entry::operationAOTReadField);
    reads.runner.setResultOfRecordedOperation(closure.address());
    run = reads.read(objectBeforeFirstRead.address());
    reads.runner.countCallsOf(Entry::operationAOTReadField);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == closure.address());
    AOT_TEST_CHECK(run.recordedArguments[0] == objectBeforeFirstRead.address() && run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits());
    AOT_TEST_CHECK(objectBeforeFirstRead.at<uint32_t>(FakeTypedObject::offsetOfField) == uncreatedDeferredClosure);

    // The property has left the slot, which says what an empty slot of any other field says.
    FakeTypedObject objectWithoutProperty;
    objectWithoutProperty.at<uint32_t>(FakeTypedObject::offsetOfField) = absentDeferredClosure;
    reads.runner.recordCallsOf(Entry::operationAOTReadField);
    run = reads.read(objectWithoutProperty.address());
    reads.runner.countCallsOf(Entry::operationAOTReadField);
    AOT_TEST_CHECK(run.calledOperation && !run.numberOfRecordedCalls);

    reads.file(Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn | Slot::absentIsUndefined, SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure), 0, 0));
    run = reads.read(objectWithoutProperty.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    AOT_TEST_CHECK(objectWithoutProperty.at<uint32_t>(FakeTypedObject::offsetOfField) == absentDeferredClosure);
}

static void testTypedArrayLengths()
{
    constexpr uint64_t maxLengthRead = INT32_MAX;
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    reads.file(Slot::createWithoutPayload(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::TypedArrayLength, 0));
    FakeObject<sizeof(JSArrayBufferView)> view;
    view.setHeader(PropertyReads::structureID, Uint8ArrayType);
    auto readLength = [&](TypedArrayMode mode, uint64_t length) {
        view.at<uint8_t>(JSArrayBufferView::offsetOfMode()) = mode;
        view.at<uint64_t>(JSArrayBufferView::offsetOfLength()) = length;
        return reads.read(view.address());
    };
    auto isLeftToOperation = [&](TypedArrayMode mode, uint64_t length) {
        runner.recordCallsOf(Entry::operationAOTGetById);
        runner.setResultOfRecordedOperation(bitsOf(jsNumber(5)));
        StubRun run = readLength(mode, length);
        runner.countCallsOf(Entry::operationAOTGetById);
        return run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == view.address() && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name && run.result == bitsOf(jsNumber(5));
    };

    for (TypedArrayMode mode : { FastTypedArray, OversizeTypedArray, WastefulTypedArray }) {
        for (uint64_t length : { uint64_t(0), uint64_t(7), maxLengthRead }) {
            StubRun run = readLength(mode, length);
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(static_cast<int32_t>(length))));
        }
        AOT_TEST_CHECK(isLeftToOperation(mode, maxLengthRead + 1));
    }
    for (TypedArrayMode mode : { GrowableSharedWastefulTypedArray, GrowableSharedAutoLengthWastefulTypedArray, ResizableNonSharedWastefulTypedArray, ResizableNonSharedAutoLengthWastefulTypedArray })
        AOT_TEST_CHECK(isLeftToOperation(mode, 7));

    reads.file(Slot::createWithoutPayload(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::TypedArrayLength, Slot::readsByteOffset));
    view.at<uint64_t>(JSArrayBufferView::offsetOfByteOffset()) = 24;
    StubRun run = readLength(WastefulTypedArray, 7);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(24)));
    AOT_TEST_CHECK(isLeftToOperation(ResizableNonSharedWastefulTypedArray, 7));
}

namespace {

// PropertyReads::name is a value in the cell of shape 1, a narrow reference in the cell of shape 2, a value in the Extension of shape 3, some other field of shape 4 and an integer in the cell of shape 5. No other name has an entry.
class FieldTable {
    WTF_MAKE_NONCOPYABLE(FieldTable);
public:
    static constexpr uint32_t shapeWithValue = 1;
    static constexpr uint32_t shapeWithNarrowCell = 2;
    static constexpr uint32_t shapeWithOutOfLineValue = 3;
    static constexpr uint32_t shapeWithOtherField = 4;
    static constexpr uint32_t shapeWithInt32 = 5;
    static constexpr uint32_t shapeWithEncodedByte = 6;
    static constexpr uint32_t shapeWithBoolean = 7;
    static constexpr uint32_t shapeWithDouble = 8;
    static constexpr uint32_t shapeWithNarrowCellInExtension = 9;
    static constexpr uint32_t shapeWithNarrowCellInSecondHalfOfGranule = 10;
    static constexpr uint32_t shapeWithInt64 = 11;
    static constexpr uint32_t highestShape = 11;
    static constexpr uint32_t offsetInExtension = 2 * sizeof(uint64_t);

    explicit FieldTable(PropertyReads& reads)
    {
        using Entry = FieldTableEntry;
        auto entryOf = [&](uint32_t shape) -> uint32_t& {
            m_table[highestShape - shape] = shape;
            return m_table[highestShape + shape + PropertyReads::name];
        };
        entryOf(shapeWithValue) = Entry::create(PropertyReads::name, Entry::Form::InlineValue, FakeTypedObject::offsetOfField / sizeof(uint64_t)).bits;
        entryOf(shapeWithNarrowCell) = Entry::create(PropertyReads::name, Entry::Form::InlineNarrowCell, FakeTypedObject::offsetOfField / sizeof(uint32_t)).bits;
        entryOf(shapeWithOutOfLineValue) = Entry::create(PropertyReads::name, Entry::Form::OutOfLineValue, offsetInExtension / sizeof(uint64_t)).bits;
        entryOf(shapeWithOtherField) = Entry::create(PropertyReads::name, Entry::Form::Field, 0).bits;
        entryOf(shapeWithInt32) = Entry::create(PropertyReads::name, Entry::Form::InlineInt32, FakeTypedObject::offsetOfField / sizeof(uint32_t)).bits;
        entryOf(shapeWithEncodedByte) = Entry::create(PropertyReads::name, Entry::Form::Field, Entry::firstEncodedByte + FakeTypedObject::offsetOfField).bits;
        entryOf(shapeWithBoolean) = Entry::create(PropertyReads::name, Entry::Form::Field, Entry::firstBoolean + FakeTypedObject::offsetOfField).bits;
        entryOf(shapeWithDouble) = Entry::create(PropertyReads::name, Entry::Form::Field, Entry::firstDouble + FakeTypedObject::offsetOfField / sizeof(double)).bits;
        entryOf(shapeWithInt64) = Entry::create(PropertyReads::name, Entry::Form::Field, Entry::firstInt64 + FakeTypedObject::offsetOfField / sizeof(int64_t)).bits;
        entryOf(shapeWithNarrowCellInExtension) = Entry::create(PropertyReads::name, Entry::Form::Field, Entry::firstNarrowCellInExtension + offsetInExtension / sizeof(uint64_t)).bits;
        entryOf(shapeWithNarrowCellInSecondHalfOfGranule) = Entry::create(PropertyReads::name, Entry::Form::Field, Entry::firstNarrowCellInSecondHalfOfGranule + offsetInExtension / sizeof(uint64_t)).bits;
        Instance& instance = reads.runner.instance();
        instance.image.fieldTableEntries = m_table.data() + highestShape;
        // Every entry names field 0 of the one layout.
        instance.image.fieldRecordIndices = m_recordIndices.data();
        m_structure.at<const void*>(CompiledStructure::offsetOfLayout()) = m_layout.data();
        instance.prefix().structureIDBase = m_structure.address() - PropertyReads::structureID;
        instance.propertyCaches.namesOfObjectPrototypes = &m_namesOfObjectPrototypes;
    }

    void setFlagsOfStructure(OptionSet<CompiledStructure::Flag> flags) { m_structure.at<uint8_t>(CompiledStructure::offsetOfFlags()) = flags.toRaw(); }
    void setNamesOfObjectPrototypes(uint64_t bits) { m_namesOfObjectPrototypes = bits; }
    FieldData& field() { return *std::bit_cast<FieldData*>(m_layout.data() + sizeof(LayoutData)); }

private:
    static_assert(PropertyReads::numberOfNames <= 64);
    std::array<uint32_t, highestShape + highestShape + PropertyReads::numberOfNames> m_table { };
    std::array<uint16_t, highestShape + PropertyReads::numberOfNames> m_recordIndices { };
    alignas(LayoutData) std::array<uint8_t, sizeof(LayoutData) + sizeof(FieldData)> m_layout { };
    FakeObject<sizeof(CompiledStructure)> m_structure;
    uint64_t m_namesOfObjectPrototypes { 0 };
};

struct FakeObjectWithShape : FakeTypedObject {
    explicit FakeObjectWithShape(uint32_t shape) { at<uint32_t>(offsetOfShapeWord) = shapeWordOf(shape); }
};

// Holds one granule.
struct FakeExtension : FakeObject<extensionCellSizeFor(1)> {
    static constexpr uint64_t bitOfSlot = granuleBitOf(FieldTable::offsetInExtension);

    explicit FakeExtension(uint64_t granuleBit) { at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBit; }

    uint64_t& granule() { return at<uint64_t>(offsetOfFirstExtensionSlot); }
    uint32_t encode(uintptr_t cageBase) const { return MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(address() - offsetOfEmptyExtensionInCage)); }
};

} // anonymous namespace

static void testFieldsFoundInFieldTable()
{
    PropertyReads reads;
    FieldTable table(reads);
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    const JSValue values[] = { jsNumber(7), jsDoubleNumber(0.5), jsUndefined(), jsNull(), jsBoolean(true), JSValue(std::bit_cast<JSCell*>(cell.address())) };

    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    for (JSValue value : values) {
        withValue.field() = bitsOf(value);
        StubRun run = reads.read(withValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
    }

    FakeObjectWithShape withNarrowCell(FieldTable::shapeWithNarrowCell);
    auto readNarrow = [&](uint32_t word) {
        withNarrowCell.field() = untouched << 32 | word;
        StubRun run = reads.read(withNarrowCell.address());
        AOT_TEST_CHECK(!run.calledOperation);
        return run.result;
    };
    AOT_TEST_CHECK(readNarrow(MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address()))) == cell.address());
    AOT_TEST_CHECK(readNarrow(undefinedCell32) == bitsOf(jsUndefined()) && readNarrow(nullCell32) == bitsOf(jsNull()));

    FakeObjectWithShape withInt32(FieldTable::shapeWithInt32);
    for (int32_t value : { 0, 7, -1, INT32_MAX, minInlineInt32 }) {
        withInt32.field() = untouched << 32 | static_cast<uint32_t>(value);
        StubRun run = reads.read(withInt32.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(value)));
    }

    FakeExtension extension(FakeExtension::bitOfSlot);
    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.field() = untouched << 32 | extension.encode(cageBase);
    for (JSValue value : values) {
        extension.granule() = bitsOf(value);
        StubRun run = reads.read(withOutOfLineValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
    }

    const uint32_t cellWord = MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address()));
    for (bool isInSecondHalf : { false, true }) {
        FakeObjectWithShape withNarrowCellInExtension(isInSecondHalf ? FieldTable::shapeWithNarrowCellInSecondHalfOfGranule : FieldTable::shapeWithNarrowCellInExtension);
        withNarrowCellInExtension.field() = untouched << 32 | extension.encode(cageBase);
        auto readNarrowInExtension = [&](uint32_t word) {
            extension.granule() = isInSecondHalf ? static_cast<uint64_t>(word) << 32 | static_cast<uint32_t>(untouched) : untouched << 32 | word;
            StubRun run = reads.read(withNarrowCellInExtension.address());
            AOT_TEST_CHECK(!run.calledOperation);
            return run.result;
        };
        AOT_TEST_CHECK(readNarrowInExtension(cellWord) == cell.address());
        AOT_TEST_CHECK(readNarrowInExtension(undefinedCell32) == bitsOf(jsUndefined()) && readNarrowInExtension(nullCell32) == bitsOf(jsNull()));
    }
}

// With nothing in any cache: what is not answered by the table of fields alone ends in an operation.
static void testBooleansAndDoublesFoundInFieldTable()
{
    PropertyReads reads;
    FieldTable table(reads);

    FakeObjectWithShape withEncodedByte(FieldTable::shapeWithEncodedByte);
    for (JSValue value : { jsBoolean(false), jsBoolean(true), jsUndefined(), jsNull() }) {
        withEncodedByte.field() = untouched << 8 | bitsOf(value);
        StubRun run = reads.read(withEncodedByte.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(value));
    }

    FakeObjectWithShape withBoolean(FieldTable::shapeWithBoolean);
    for (bool value : { false, true }) {
        withBoolean.field() = untouched << 8 | value;
        StubRun run = reads.read(withBoolean.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsBoolean(value)));
    }

    FakeObjectWithShape withDouble(FieldTable::shapeWithDouble);
    for (double value : { 0.5, -0.5, 7.0, -0.0, 1e300, 4294967296.0, 5e-324 }) {
        withDouble.field() = std::bit_cast<uint64_t>(value);
        StubRun run = reads.read(withDouble.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsDoubleNumber(value)));
    }
    // What is no finite number may be a marker: the record of the field explains it.
    table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::OptionalDouble);
    for (double value : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        withDouble.field() = std::bit_cast<uint64_t>(value);
        AOT_TEST_CHECK(reads.read(withDouble.address()).calledOperation);
    }

    FakeObjectWithShape withInt64(FieldTable::shapeWithInt64);
    for (int32_t value : { 0, 7, -7, INT32_MAX, INT32_MIN }) {
        withInt64.field() = static_cast<uint64_t>(static_cast<int64_t>(value));
        StubRun run = reads.read(withInt64.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(value)));
    }
    // Beyond int32 it may be a marker.
    table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::OptionalInt64);
    for (int64_t value : { static_cast<int64_t>(INT32_MAX) + 1, static_cast<int64_t>(INT32_MIN) - 1, maxIntegerAddedTo, minIntegerAddedTo, absentInteger, undefinedInteger, nullInteger }) {
        withInt64.field() = static_cast<uint64_t>(value);
        AOT_TEST_CHECK(reads.read(withInt64.address()).calledOperation);
    }
}

// An optional field that is missing reads as undefined without a look into any cache, where the object has no field outside its slot and nothing above it has the name.
static void testMissingFieldsAnsweredByFieldTable()
{
    PropertyReads reads;
    FieldTable table(reads);
    struct Kind {
        uint32_t shape;
        SlotRepresentation representation;
    };
    const Kind kinds[] = {
        { FieldTable::shapeWithValue, SlotRepresentation::JSValue },
        { FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32 },
        { FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32 },
        { FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean },
        { FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble },
    };
    for (const Kind& kind : kinds) {
        const SlotTraits traits = traitsOf(kind.representation);
        table.field() = { };
        table.field().name = PropertyReads::name;
        table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, kind.representation);
        table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
        table.setNamesOfObjectPrototypes(0);
        FakeObjectWithShape object(kind.shape);
        object.at<uint32_t>(offsetOfOrderWord) = 0;
        object.field() = traits.width == sizeof(uint64_t) ? traits.absentBits : untouched << (8 * traits.width) | traits.absentBits;
        auto isAnswered = [&] {
            StubRun run = reads.read(object.address());
            AOT_TEST_CHECK(run.calledOperation || run.result == bitsOf(jsUndefined()));
            return !run.calledOperation;
        };
        AOT_TEST_CHECK(isAnswered());

        object.at<uint32_t>(offsetOfOrderWord) = orderWordHasFieldOutsideSlot;
        AOT_TEST_CHECK(!isAnswered());
        object.at<uint32_t>(offsetOfOrderWord) = 0;
        table.setNamesOfObjectPrototypes(uint64_t(1) << PropertyReads::name);
        AOT_TEST_CHECK(!isAnswered());
        table.setNamesOfObjectPrototypes(0);
        table.setFlagsOfStructure({ });
        AOT_TEST_CHECK(!isAnswered());
        table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
        AOT_TEST_CHECK(isAnswered());
    }

    // The same for a field in the Extension: there is no Extension, it has no granule for the field, or the slot in the granule is empty.
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    for (uint32_t shape : { FieldTable::shapeWithOutOfLineValue, FieldTable::shapeWithNarrowCellInExtension, FieldTable::shapeWithNarrowCellInSecondHalfOfGranule }) {
        FakeExtension withGranule(FakeExtension::bitOfSlot);
        FakeExtension withOtherGranule(FakeExtension::bitOfSlot << 1);
        withGranule.granule() = shape == FieldTable::shapeWithOutOfLineValue ? 0 : shape == FieldTable::shapeWithNarrowCellInExtension ? untouched << 32 : static_cast<uint32_t>(untouched);
        withOtherGranule.granule() = untouched;
        for (uint32_t extensionWord : { uint32_t(0), withGranule.encode(cageBase), withOtherGranule.encode(cageBase) }) {
            table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
            table.setNamesOfObjectPrototypes(0);
            FakeObjectWithShape object(shape);
            object.at<uint32_t>(offsetOfOrderWord) = 0;
            object.field() = untouched << 32 | extensionWord;
            auto isAnswered = [&] {
                StubRun run = reads.read(object.address());
                AOT_TEST_CHECK(run.calledOperation || run.result == bitsOf(jsUndefined()));
                return !run.calledOperation;
            };
            AOT_TEST_CHECK(isAnswered());
            object.at<uint32_t>(offsetOfOrderWord) = orderWordHasFieldOutsideSlot;
            AOT_TEST_CHECK(!isAnswered());
            object.at<uint32_t>(offsetOfOrderWord) = 0;
            table.setNamesOfObjectPrototypes(uint64_t(1) << PropertyReads::name);
            AOT_TEST_CHECK(!isAnswered());
            table.setNamesOfObjectPrototypes(0);
            table.setFlagsOfStructure({ });
            AOT_TEST_CHECK(!isAnswered());
        }
    }

    // A closure that is not made yet has the word of an empty reference.
    table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure);
    FakeObjectWithShape withClosure(FieldTable::shapeWithNarrowCell);
    withClosure.at<uint32_t>(offsetOfOrderWord) = 0;
    withClosure.field() = untouched << 32 | uncreatedDeferredClosure;
    AOT_TEST_CHECK(reads.read(withClosure.address()).calledOperation);
}

static void testInstructionsOfReadsByKindOfField()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    PropertyReads reads;
    FieldTable table(reads);
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    table.setNamesOfObjectPrototypes(0);
    reads.runner.setCountsInstructions(true);

    struct Read {
        ASCIILiteral what;
        uint32_t shape;
        SlotRepresentation representation;
        uint64_t slot;
        JSValue expected;
        uint32_t budget;
    };
    auto absent = [](SlotRepresentation representation) { return traitsOf(representation).absentBits; };
    const Read readsByKind[] = {
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, bitsOf(jsNumber(7)), jsNumber(7), 17 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 7, jsNumber(7), 22 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address())), JSValue(std::bit_cast<JSCell*>(cell.address())), 24 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, JSValue::ValueTrue, jsBoolean(true), 31 },
        { "a boolean"_s, FieldTable::shapeWithBoolean, SlotRepresentation::Boolean, 1, jsBoolean(true), 32 },
        { "a double"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, std::bit_cast<uint64_t>(0.5), jsDoubleNumber(0.5), 37 },
        { "a 64-bit integer"_s, FieldTable::shapeWithInt64, SlotRepresentation::OptionalInt64, 7, jsNumber(7), 35 },
        { "a boxed value that is missing"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, absent(SlotRepresentation::JSValue), jsUndefined(), 32 },
        { "an optional boolean that is missing"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, absent(SlotRepresentation::OptionalBoolean), jsUndefined(), 46 },
        { "an integer that is missing"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, absent(SlotRepresentation::OptionalInt32), jsUndefined(), 67 },
        { "a narrow reference that is missing"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, absent(SlotRepresentation::OptionalCell32), jsUndefined(), 74 },
        { "a double that is missing"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, absent(SlotRepresentation::OptionalDouble), jsUndefined(), 91 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByKind) {
        table.field() = { };
        table.field().name = PropertyReads::name;
        table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, read.representation);
        FakeObjectWithShape object(read.shape);
        object.at<uint32_t>(offsetOfOrderWord) = 0;
        object.field() = read.slot;
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected) && run.instructionsInStubs);
        if (*run.instructionsInStubs != read.budget)
            dataLogLn("a read by name of ", read.what, ": ", *run.instructionsInStubs, " instructions, the budget is ", read.budget);
        isWithinBudget &= *run.instructionsInStubs <= read.budget || AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE;
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

// The same for a field that the tables have: what the handler of the slot costs by what the field holds. A number here may fall.
static void testInstructionsOfReadsOfFiledFields()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    const JSValue cellValue(std::bit_cast<JSCell*>(cell.address()));
    reads.runner.setCountsInstructions(true);

    struct Read {
        SlotRepresentation representation;
        JSValue value;
        uint32_t budget;
    };
    const Read readsByRepresentation[] = {
        { SlotRepresentation::JSValue, jsNumber(7), 50 },
        { SlotRepresentation::Cell, cellValue, 50 },
        { SlotRepresentation::Cell32, cellValue, 52 },
        { SlotRepresentation::OptionalCell32, cellValue, 52 },
        { SlotRepresentation::OptionalInt32, jsNumber(7), 55 },
        { SlotRepresentation::Int32, jsNumber(7), 61 },
        { SlotRepresentation::Double, jsDoubleNumber(0.5), 69 },
        { SlotRepresentation::OptionalDouble, jsDoubleNumber(0.5), 69 },
        { SlotRepresentation::OptionalDouble, jsNumber(7), 69 },
        { SlotRepresentation::Boolean, jsBoolean(true), 61 },
        { SlotRepresentation::OptionalBoolean, jsBoolean(true), 62 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByRepresentation) {
        reads.file(createFieldSlot(read.representation));
        FakeTypedObject object;
        AOT_TEST_CHECK(tryWrite(read.representation, &object.field(), read.value.isInt32() && traitsOf(read.representation).valueKind == SlotValueKind::Double ? jsDoubleNumber(read.value.asInt32()) : read.value, cageBase));
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.value) && run.instructionsInStubs);
        if (*run.instructionsInStubs != read.budget)
            dataLogLn("a read by name of a filed field, ", nameOf(read.representation), ": ", *run.instructionsInStubs, " instructions, the budget is ", read.budget);
        isWithinBudget &= *run.instructionsInStubs <= read.budget || AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE;
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

// A field that is missing in an instance of a class or of a constructor function: the Structure does not say that nothing above has the name, the cell of the front table does.
// The slot has been seen to be empty by then, so the handler of the cell, which finds the slot and looks into it, has nothing to add. A number here may fall.
static void testInstructionsOfReadsOfMissingFieldsBelowClasses()
{
    // A build with assertions puts more instructions into the stubs, so the budget is held against the other builds only.
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    PropertyReads reads;
    FieldTable table(reads);
    table.setFlagsOfStructure({ });
    table.setNamesOfObjectPrototypes(0);
    reads.runner.setCountsInstructions(true);
    struct Read {
        ASCIILiteral what;
        uint32_t shape;
        SlotRepresentation representation;
        uint32_t budget;
    };
    const Read readsByKind[] = {
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, 37 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, 51 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 72 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, 83 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByKind) {
        const SlotTraits traits = traitsOf(read.representation);
        table.field() = { };
        table.field().name = PropertyReads::name;
        table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, read.representation);
        FakeObjectWithShape object(read.shape);
        object.at<uint32_t>(offsetOfOrderWord) = 0;
        object.field() = traits.width == sizeof(uint64_t) ? traits.absentBits : untouched << (8 * traits.width) | traits.absentBits;
        auto fileInFront = [&](uint8_t flags) {
            reads.clear();
            reads.fileInFrontBesideOthers(Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn | flags, table.field().slot, 0, 0));
        };
        fileInFront(Slot::absentIsUndefined);
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()) && run.instructionsInStubs);
        if (*run.instructionsInStubs != read.budget)
            dataLogLn("a read by name of ", read.what, " that is missing below a class: ", *run.instructionsInStubs, " instructions, the budget is ", read.budget);
        isWithinBudget &= *run.instructionsInStubs <= read.budget;

        // A cell that does not say so, a cell of another name and an object with a field outside its slot are left to whoever they were left to.
        fileInFront(0);
        AOT_TEST_CHECK(reads.read(object.address()).calledOperation);
        reads.clear();
        AOT_TEST_CHECK(reads.read(object.address()).calledOperation);
        fileInFront(Slot::absentIsUndefined);
        object.at<uint32_t>(offsetOfOrderWord) = orderWordHasFieldOutsideSlot;
        run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

static void testLookupsLeftToTheCaches()
{
    PropertyReads reads;
    FieldTable table(reads);
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    auto fieldSlot = [](SlotRepresentation representation, uint8_t flags) {
        return Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn | flags, SlotData::create(FakeTypedObject::offsetOfField, representation), 0, 0);
    };

    // An empty slot says nothing about the chain.
    for (auto [shape, representation] : { std::pair { FieldTable::shapeWithValue, SlotRepresentation::JSValue }, std::pair { FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32 } }) {
        FakeObjectWithShape object(shape);
        object.field() = 0;
        reads.file(fieldSlot(representation, Slot::absentIsUndefined));
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        reads.file(Slot { });
        AOT_TEST_CHECK(reads.read(object.address()).calledOperation);
    }

    // So does the slot of a closure that the property has left.
    FakeObjectWithShape withoutClosure(FieldTable::shapeWithNarrowCell);
    withoutClosure.field() = absentDeferredClosure;
    AOT_TEST_CHECK(reads.read(withoutClosure.address()).calledOperation);

    // So does an Extension that is not there, one that holds another granule, and an empty slot in it.
    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.field() = 0;
    AOT_TEST_CHECK(reads.read(withOutOfLineValue.address()).calledOperation);
    for (uint64_t granuleBit : { FakeExtension::bitOfSlot >> 1, FakeExtension::bitOfSlot << 1, FakeExtension::bitOfSlot }) {
        FakeExtension extension(granuleBit);
        extension.granule() = granuleBit == FakeExtension::bitOfSlot ? 0 : bitsOf(jsNumber(7));
        withOutOfLineValue.field() = extension.encode(cageBase);
        AOT_TEST_CHECK(reads.read(withOutOfLineValue.address()).calledOperation);
    }

    // A word below the integers says that the slot is empty, undefined or null, or is an integer of a field that has no such words.
    FakeObjectWithShape withInt32(FieldTable::shapeWithInt32);
    for (int32_t word : { absentInt32, undefinedInt32, nullInt32, INT32_MIN, minInlineInt32 - 1 }) {
        withInt32.at<int32_t>(FakeTypedObject::offsetOfField) = word;
        reads.file(Slot { });
        AOT_TEST_CHECK(reads.read(withInt32.address()).calledOperation);
        reads.file(fieldSlot(SlotRepresentation::Int32, 0));
        StubRun run = reads.read(withInt32.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(word)));
    }
    withInt32.at<int32_t>(FakeTypedObject::offsetOfField) = nullInt32;
    reads.file(fieldSlot(SlotRepresentation::OptionalInt32, 0));
    AOT_TEST_CHECK(reads.read(withInt32.address()).result == bitsOf(jsNull()));

    FakeObjectWithShape withOtherField(FieldTable::shapeWithOtherField);
    withOtherField.at<int32_t>(FakeTypedObject::offsetOfField) = 9;
    reads.file(fieldSlot(SlotRepresentation::Int32, 0));
    StubRun run = reads.read(withOtherField.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(9)));

    // The entries that another name meets are those of other rows.
    reads.file(Slot { });
    for (uint32_t shape = 1; shape <= FieldTable::highestShape; ++shape) {
        FakeObjectWithShape object(shape);
        object.field() = bitsOf(jsNumber(7));
        for (uint16_t otherName = 1; otherName < PropertyReads::numberOfNames; ++otherName)
            AOT_TEST_CHECK(otherName == PropertyReads::name || reads.read(object.address(), otherName).calledOperation);
    }

    FakeTypedObject withoutShape;
    withoutShape.field() = bitsOf(jsNumber(7));
    AOT_TEST_CHECK(reads.read(withoutShape.address()).calledOperation);
}

static void testNamesOutsideLayoutAreAbsent()
{
    using Flag = CompiledStructure::Flag;
    constexpr uint16_t inheritedName = 2;
    static_assert(inheritedName != PropertyReads::name);
    PropertyReads reads;
    FieldTable table(reads);
    auto forEachOtherName = [&](const auto& check) {
        for (uint32_t shape = 1; shape <= FieldTable::highestShape; ++shape) {
            FakeObjectWithShape object(shape);
            object.field() = bitsOf(jsNumber(7));
            for (uint16_t otherName = 1; otherName < PropertyReads::numberOfNames; ++otherName) {
                if (otherName != PropertyReads::name)
                    check(otherName, reads.read(object.address(), otherName));
            }
        }
    };

    table.setFlagsOfStructure({ Flag::HasOnlyFieldsBelowObjectPrototype, Flag::HasOnlyPlainSlots });
    table.setNamesOfObjectPrototypes(uint64_t { 1 } << inheritedName);
    forEachOtherName([&](uint16_t otherName, const StubRun& run) {
        // The question stands behind the hashed table, which a name that has no uid yet does not get to.
        if (otherName == inheritedName || otherName == PropertyReads::nameMadeAtRunTime)
            AOT_TEST_CHECK(run.calledOperation);
        else
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    });

    // What is answered here is never filed, so one answer in so many is left to the operation that files.
    constexpr uint16_t absentName = 1;
    static_assert(absentName != inheritedName && absentName != PropertyReads::name && absentName != PropertyReads::nameMadeAtRunTime);
    FakeObjectWithShape askedOften(FieldTable::shapeWithValue);
    constexpr unsigned readsBetweenMisses = MissesOfPairs::missesBetweenFilings;
    reads.runner.setCountsMissesAcrossRuns(true);
    for (unsigned read = 1; read <= 3 * readsBetweenMisses; ++read) {
        reads.runner.instance().propertyCaches.missIsDueForFiling = 0;
        AOT_TEST_CHECK(reads.read(askedOften.address(), absentName).calledOperation == !(read % readsBetweenMisses));
        AOT_TEST_CHECK(reads.runner.instance().propertyCaches.missIsDueForFiling == !(read % readsBetweenMisses));
    }
    reads.runner.setCountsMissesAcrossRuns(false);

    // An object with extra properties or another prototype has a Structure that does not say so.
    for (OptionSet<Flag> flags : { OptionSet<Flag> { }, OptionSet<Flag> { Flag::HasExtraProperties }, OptionSet<Flag> { Flag::HasOtherPrototype, Flag::HasOnlyPlainSlots } }) {
        table.setFlagsOfStructure(flags);
        forEachOtherName([&](uint16_t, const StubRun& run) {
            AOT_TEST_CHECK(run.calledOperation);
        });
    }

    // The field itself is read as ever. stubs/get-by-id/missing-fields-answered-by-field-table has its empty slot.
    table.setFlagsOfStructure(Flag::HasOnlyFieldsBelowObjectPrototype);
    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    withValue.field() = bitsOf(jsNumber(7));
    StubRun run = reads.read(withValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
}

namespace {

// Stands for the entrypoint of a function whose closures are slim. It takes what it needs out of the cell in calleeGPR while the call lasts: the cell may lie in the frame of a stub.
class CalleeThatReadsItsClosure {
    WTF_MAKE_NONCOPYABLE(CalleeThatReadsItsClosure);
public:
    static constexpr uint64_t distanceFromCode = 64;
    static constexpr uint64_t result = 0x3333;

    struct Call {
        uint64_t numberOfCalls;
        uint64_t callee;
        uint64_t stackPointer;
        uint64_t thisValue;
        uint64_t count;
        uint64_t firstArgument;
        uint64_t header;
        uint64_t word;
        uint64_t capture;
    };

    explicit CalleeThatReadsItsClosure(Instance& instance)
    {
        constexpr GPRReg word = thisGPR;
        CCallHelpers jit;
        CCallHelpers::Label entry = jit.label();
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(&m_call.numberOfCalls));
        jit.store64(calleeGPR, &m_call.callee);
        jit.store64(countGPR, &m_call.count);
        jit.store64(argumentGPR(0), &m_call.firstArgument);
        jit.store64(thisGPR, &m_call.thisValue);
        jit.move(CCallHelpers::stackPointerRegister, word);
        jit.store64(word, &m_call.stackPointer);
        jit.load64(CCallHelpers::Address(calleeGPR), word);
        jit.store64(word, &m_call.header);
        jit.load64(CCallHelpers::Address(calleeGPR, offsetOfSlimClosureWord), word);
        jit.store64(word, &m_call.word);
        jit.load64(CCallHelpers::Address(calleeGPR, offsetOfSlimClosureCaptures), word);
        jit.store64(word, &m_call.capture);
        jit.move(CCallHelpers::TrustedImm64(result), GPRInfo::returnValueGPR);
        jit.ret();

        LinkBuffer linkBuffer(jit, nullptr);
        instance.image.code = linkBuffer.locationOf<JSEntryPtrTag>(entry).untaggedPtr<const uint8_t*>() - distanceFromCode;
        m_code = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT callee that reads its closure");
    }

    Call takeCall() { return std::exchange(m_call, { }); }

private:
    Call m_call { };
    MacroAssemblerCodeRef<JSEntryPtrTag> m_code;
};

// The record of the layout behind FieldTable::shapeWithNarrowCell. Its second field is PropertyReads::name, which defers a closure of a function whose closures are slim.
class LayoutWithDeferredClosure {
    WTF_MAKE_NONCOPYABLE(LayoutWithDeferredClosure);
public:
    static constexpr uint32_t function = 2;
    static constexpr uint32_t structureIDOfSlimClosures = 0x2000;
    static constexpr uint32_t offsetOfEnvironment = FakeTypedObject::offsetOfField + sizeof(uint32_t);

    explicit LayoutWithDeferredClosure(PropertyReads& reads)
    {
        Instance& instance = reads.runner.instance();
        m_record.layout.numberOfFields = static_cast<uint16_t>(m_record.fields.size());
        m_record.layout.numberOfFieldOperands = static_cast<uint16_t>(m_record.operands.size());
        field() = FieldData {
            .name = PropertyReads::name,
            .flags = 0,
            .operandIndex = 1,
            .slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure),
            .type = 0,
        };
        m_record.operands = { UINT32_MAX, function, SlotData::create(offsetOfEnvironment, SlotRepresentation::Cell32).encoded() };
        m_structure.at<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &m_record.layout;
        instance.prefix().structureIDBase = m_structure.address() - PropertyReads::structureID;
        m_recordIndices[FieldTable::shapeWithNarrowCell + PropertyReads::name] = 1;
        instance.image.fieldRecordIndices = m_recordIndices.data();

        kind().isSlim = 1;
        instance.image.closureFunctionKindsOrigin = std::bit_cast<uintptr_t>(m_functionKinds.data());

        closureWord() = ClosureWord::encode({ CalleeThatReadsItsClosure::distanceFromCode, 1 }, function, { });
        instance.functionTables.closureWords = m_closureWords.data();
        FakeObject<sizeof(uint64_t)> slimClosure;
        slimClosure.setHeader(structureIDOfSlimClosures, SlimClosureType);
        instance.slimClosureHeaders[0] = slimClosure.at<uint64_t>(0);
    }

    FieldData& field() { return m_record.fields[1]; }
    FunctionKindData& kind() { return m_functionKinds[function]; }
    uint64_t& closureWord() { return m_closureWords[function]; }

private:
    struct Record {
        LayoutData layout;
        std::array<FieldData, 2> fields;
        std::array<uint32_t, 3> operands;
    };
    static_assert(OBJECT_OFFSETOF(Record, fields) == sizeof(LayoutData) && OBJECT_OFFSETOF(Record, operands) == sizeof(LayoutData) + 2 * sizeof(FieldData));

    Record m_record { };
    std::array<FunctionKindData, function + 1> m_functionKinds { };
    FakeObject<sizeof(CompiledStructure)> m_structure;
    std::array<uint16_t, FieldTable::highestShape + PropertyReads::numberOfNames> m_recordIndices { };
    std::array<uint64_t, function + 1> m_closureWords { };
};

} // anonymous namespace

// What a read at a site answers without a handler and without an operation: the value or the absence that the site holds, and at a site that holds another Structure's slot what the front table holds.
static void testReadsAtSites()
{
    PropertyReads reads;
    Instance& instance = reads.runner.instance();
    constexpr PropertyOffset inlineOffset = 1;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    const StructureID otherStructureID = std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder);
    std::array<SiteCacheState, SiteCacheState::numberOfStates> states;
    states.fill(SiteCacheState { UINT8_MAX });
    instance.propertyCaches.siteCacheStates = states.data();

    FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(EncodedJSValue)> receiver;
    receiver.setHeader(PropertyReads::structureID, FinalObjectType);
    receiver.at<uint64_t>(JSObject::offsetOfInlineStorage() + inlineOffset * sizeof(EncodedJSValue)) = bitsOf(jsNumber(42));
    Slot site = Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, inlineOffset);
    auto readAtSite = [&] { return reads.runner.run(getByIdAtSite(), { receiver.address(), std::bit_cast<uint64_t>(&site) }, PropertyReads::name); };
    auto isAnswered = [](const StubRun& run, JSValue expected) { return !run.calledOperation && run.result == bitsOf(expected); };

    AOT_TEST_CHECK(isAnswered(readAtSite(), jsNumber(42)));
    site = Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain);
    AOT_TEST_CHECK(isAnswered(readAtSite(), jsUndefined()));

    site = Slot::createWithoutPayload(otherStructureID, Slot::Kind::Absent, Slot::dependsOnChain);
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, inlineOffset));
    AOT_TEST_CHECK(isAnswered(readAtSite(), jsNumber(42)));
    AOT_TEST_CHECK(isAnswered(reads.read(receiver.address()), jsNumber(42)));
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain));
    AOT_TEST_CHECK(isAnswered(readAtSite(), jsUndefined()));
    AOT_TEST_CHECK(isAnswered(reads.read(receiver.address()), jsUndefined()));

    // The front table has another name in the cell: the hashed table answers.
    reads.file(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, inlineOffset));
    AOT_TEST_CHECK(isAnswered(readAtSite(), jsNumber(42)));
}

struct ObjectWithTwoValues : FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(EncodedJSValue)> {
    static constexpr PropertyOffset inlineOffset = 1;
    static constexpr PropertyOffset outOfLineOffset = firstOutOfLineOffset + 1;

    ObjectWithTwoValues(uint32_t structureID, int32_t valueInCell, int32_t valueOutOfLine)
    {
        setHeader(structureID, FinalObjectType);
        at<uint64_t>(JSObject::offsetOfInlineStorage() + inlineOffset * sizeof(EncodedJSValue)) = bitsOf(jsNumber(valueInCell));
        at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(storage.data() + storage.size());
        storage[storage.size() + Slot::wordIndexFor(outOfLineOffset)] = bitsOf(jsNumber(valueOutOfLine));
    }

    std::array<uint64_t, 4> storage { };
};

// A hit at a site, by what the slot says. A number here may fall.
static void testHitsAtSites()
{
    PropertyReads reads;
    constexpr PropertyOffset inlineOffset = ObjectWithTwoValues::inlineOffset;
    constexpr PropertyOffset outOfLineOffset = ObjectWithTwoValues::outOfLineOffset;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    ObjectWithTwoValues receiver(PropertyReads::structureID, 1, 2);
    ObjectWithTwoValues holder(PropertyReads::structureIDOfHolder, 3, 4);
    JSCell* holderCell = std::bit_cast<JSCell*>(holder.address());
    auto value = [&](uint8_t flags, JSCell* holderOrNull, PropertyOffset offset) { return Slot::createInStorage(structureID, Slot::Kind::Value, flags, holderOrNull, offset); };

    struct Hit {
        ASCIILiteral what;
        Slot slot;
        JSValue expected;
        uint32_t budget;
    };
    const Hit hits[] = {
        { "an own value in the cell"_s, value(Slot::isOwn, nullptr, inlineOffset), jsNumber(1), 10 },
        { "an own value in the cell that depends on other objects"_s, value(Slot::isOwn | Slot::dependsOnChain, nullptr, inlineOffset), jsNumber(1), 13 },
        { "an own value out of line"_s, value(Slot::isOwn, nullptr, outOfLineOffset), jsNumber(2), 14 },
        { "an own value out of line that depends on other objects"_s, value(Slot::isOwn | Slot::dependsOnChain, nullptr, outOfLineOffset), jsNumber(2), 14 },
        { "an inherited value in the cell"_s, value(0, holderCell, inlineOffset), jsNumber(3), 14 },
        { "an inherited value in the cell that depends on other objects"_s, value(Slot::dependsOnChain, holderCell, inlineOffset), jsNumber(3), 14 },
        { "an inherited value out of line"_s, value(0, holderCell, outOfLineOffset), jsNumber(4), 14 },
        { "an inherited value out of line that depends on other objects"_s, value(Slot::dependsOnChain, holderCell, outOfLineOffset), jsNumber(4), 14 },
        { "no property"_s, Slot::createWithoutPayload(structureID, Slot::Kind::Absent, 0), jsUndefined(), 12 },
        { "no property as long as other objects stay as they are"_s, Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain), jsUndefined(), 12 },
        { "a constant"_s, Slot::createConstant(structureID, Slot::isOwn, jsNumber(5)), jsNumber(5), 14 },
        { "a constant of an object above"_s, Slot::createConstant(structureID, Slot::dependsOnChain, jsNumber(5)), jsNumber(5), 14 },
    };
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    reads.runner.setCountsInstructions(true);
#endif
    bool isWithinBudget = true;
    constexpr uint32_t instructionsOfTestForCell = 2;
    for (const Hit& hit : hits) {
        for (StubKey stub : { getByIdAtSite(), getByIdOfObjectAtSite() }) {
            Slot site = hit.slot;
            StubRun run = reads.runner.run(stub, { receiver.address(), std::bit_cast<uint64_t>(&site) }, PropertyReads::name);
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(hit.expected));
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
            uint32_t budget = stub == getByIdAtSite() ? hit.budget : hit.budget - instructionsOfTestForCell;
            AOT_TEST_CHECK(run.instructionsInStubs);
            if (*run.instructionsInStubs != budget)
                dataLogLn("a read at a site that holds ", hit.what, ": ", *run.instructionsInStubs, " instructions, the budget is ", budget);
            isWithinBudget &= *run.instructionsInStubs <= budget || AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE;
#endif
        }
    }
    AOT_TEST_CHECK(isWithinBudget);
}

// A read at a site that holds the slot of another Structure, by what the front table holds for the Structure of the receiver. A number here may fall.
static void testSitesOfOtherStructures()
{
    PropertyReads reads;
    constexpr PropertyOffset inlineOffset = ObjectWithTwoValues::inlineOffset;
    constexpr PropertyOffset outOfLineOffset = ObjectWithTwoValues::outOfLineOffset;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    const StructureID otherStructureID = std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder);
    std::array<SiteCacheState, SiteCacheState::numberOfStates> states;
    states.fill(SiteCacheState { UINT8_MAX });
    reads.runner.instance().propertyCaches.siteCacheStates = states.data();
    ObjectWithTwoValues receiver(PropertyReads::structureID, 1, 2);
    auto ownValue = [&](StructureID id, PropertyOffset offset) { return Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, offset); };

    struct Read {
        ASCIILiteral what;
        Slot site;
        Slot inFront;
        JSValue expected;
        bool countsMiss;
        uint32_t budget;
    };
    const Read readsByFrontCell[] = {
        { "an own value at the place that the site names"_s, ownValue(otherStructureID, inlineOffset), ownValue(structureID, inlineOffset), jsNumber(1), false, 22 },
        { "an own value in the cell at another place"_s, ownValue(otherStructureID, inlineOffset - 1), ownValue(structureID, inlineOffset), jsNumber(1), true, 28 },
        { "an own value in the cell, and the site one out of line"_s, ownValue(otherStructureID, outOfLineOffset), ownValue(structureID, inlineOffset), jsNumber(1), true, 28 },
        { "an own value out of line"_s, ownValue(otherStructureID, outOfLineOffset), ownValue(structureID, outOfLineOffset), jsNumber(2), true, 39 },
        { "no property"_s, ownValue(otherStructureID, inlineOffset), Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain), jsUndefined(), true, 26 },
    };
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    reads.runner.setCountsInstructions(true);
#endif
    bool isWithinBudget = true;
    for (const Read& read : readsByFrontCell) {
        Slot site = read.site;
        reads.clear();
        reads.fileInFrontBesideOthers(read.inFront);
        states.fill(SiteCacheState { UINT8_MAX });
        StubRun run = reads.runner.run(getByIdAtSite(), { receiver.address(), std::bit_cast<uint64_t>(&site) }, PropertyReads::name);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected));
        AOT_TEST_CHECK(std::ranges::count_if(states, [](const SiteCacheState& state) { return state.missesLeft != UINT8_MAX; }) == read.countsMiss);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
        AOT_TEST_CHECK(run.instructionsInStubs);
        if (*run.instructionsInStubs != read.budget)
            dataLogLn("a read at a site of another Structure, the front table holds ", read.what, ": ", *run.instructionsInStubs, " instructions, the budget is ", read.budget);
        isWithinBudget &= *run.instructionsInStubs <= read.budget || AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE;
#endif
    }
    AOT_TEST_CHECK(isWithinBudget);

    // A name without a column: the site answers alike, and behind it the hashed table.
    for (StructureID idAtSite : { structureID, otherStructureID }) {
        Slot site = ownValue(idAtSite, inlineOffset);
        reads.file(ownValue(structureID, outOfLineOffset));
        StubRun run = reads.runner.run(getByIdAtSiteHashed(), { receiver.address(), std::bit_cast<uint64_t>(&site) }, PropertyReads::name);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(idAtSite == structureID ? 1 : 2)));
    }
}

static void testCallsOfClosuresNotCreated()
{
    constexpr uint64_t maxDistanceInFrame = 128;
    using Call = CalleeThatReadsItsClosure::Call;
    PropertyReads reads;
    FieldTable table(reads);
    LayoutWithDeferredClosure layout(reads);
    StubRunner& runner = reads.runner;
    Instance& instance = runner.instance();
    CalleeThatReadsItsClosure callee(instance);
    const uint64_t argument = bitsOf(jsNumber(71));
    const uint64_t& numberOfCallsWithoutClosure = instance.runtime.counters.values[static_cast<size_t>(RuntimeCounterID::CalledClosuresNotCreated)];

    FakeObject<16> environment;
    FakeObject<offsetOfSlimClosureCaptures + sizeof(uint64_t)> closureMadeByRead;
    closureMadeByRead.at<uint64_t>(0) = instance.slimClosureHeaders[0];
    closureMadeByRead.at<uint64_t>(offsetOfSlimClosureWord) = layout.closureWord();
    closureMadeByRead.at<uint64_t>(offsetOfSlimClosureCaptures) = untouched;

    FakeObjectWithShape object(FieldTable::shapeWithNarrowCell);
    uint32_t& slot = object.at<uint32_t>(FakeTypedObject::offsetOfField);
    uint32_t& slotOfEnvironment = object.at<uint32_t>(LayoutWithDeferredClosure::offsetOfEnvironment);
    const uint32_t encodedEnvironment = MarkedBlockCage::encode(instance.cellCageBase, std::bit_cast<JSCell*>(environment.address()));
    slot = uncreatedDeferredClosure;
    slotOfEnvironment = encodedEnvironment;

    // run() passes the name, and leaves this and the arguments as the last runCall() set them.
    auto callMethod = [&] {
        runner.runCall(callValueCell(1), closureMadeByRead.address(), object.address(), { argument });
        callee.takeCall();
        return runner.run(callMethodById(1), { }, PropertyReads::name);
    };
    auto checkCallWithoutClosure = [&](uint64_t capture) {
        const uint64_t numberOfCallsBefore = numberOfCallsWithoutClosure;
        StubRun run = callMethod();
        Call call = callee.takeCall();
        AOT_TEST_CHECK(!run.calledOperation && run.result == CalleeThatReadsItsClosure::result && call.numberOfCalls == 1);
        AOT_TEST_CHECK(call.callee > call.stackPointer && call.callee - call.stackPointer < maxDistanceInFrame);
        AOT_TEST_CHECK(call.header == instance.slimClosureHeaders[0] && call.word == layout.closureWord() && call.capture == capture);
        AOT_TEST_CHECK(call.thisValue == object.address() && static_cast<uint32_t>(call.count) == 1 && call.firstArgument == argument);
        AOT_TEST_CHECK(slot == uncreatedDeferredClosure && numberOfCallsWithoutClosure == numberOfCallsBefore + 1);
    };
    // The read of a closure that is not made yet asks for nothing but the closure.
    auto checkCallAfterRead = [&](Entry operationOfRead) {
        const uint64_t numberOfCallsBefore = numberOfCallsWithoutClosure;
        const uint32_t slotBefore = slot;
        runner.recordCallsOf(operationOfRead);
        runner.setResultOfRecordedOperation(closureMadeByRead.address());
        StubRun run = callMethod();
        runner.countCallsOf(operationOfRead);
        Call call = callee.takeCall();
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name);
        AOT_TEST_CHECK(run.result == CalleeThatReadsItsClosure::result && call.numberOfCalls == 1 && call.callee == closureMadeByRead.address() && call.capture == untouched);
        AOT_TEST_CHECK(call.thisValue == object.address() && static_cast<uint32_t>(call.count) == 1 && call.firstArgument == argument);
        AOT_TEST_CHECK(slot == slotBefore && numberOfCallsWithoutClosure == numberOfCallsBefore);
    };

    // A function without captures reads none, so it may get the object as well.
    for (uint16_t flags : { static_cast<uint16_t>(0), fieldDeferredClosureCapturesObject }) {
        layout.field().flags = flags;
        checkCallWithoutClosure(object.address());
    }
    layout.field().flags = fieldDeferredClosureCapturesEnvironment;
    checkCallWithoutClosure(environment.address());

    slotOfEnvironment = absentCell32;
    checkCallAfterRead(Entry::operationAOTMaterializeDeferredClosure);
    slotOfEnvironment = encodedEnvironment;

    slot = absentDeferredClosure;
    checkCallAfterRead(Entry::operationAOTGetByIdAndCache);
    slot = uncreatedDeferredClosure;

    layout.kind().isSlim = 0;
    checkCallAfterRead(Entry::operationAOTMaterializeDeferredClosure);
    layout.kind().isSlim = 1;

    // Zero in the slot of any other reference says that the slot is empty.
    layout.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::Cell32);
    checkCallAfterRead(Entry::operationAOTGetByIdAndCache);
    layout.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure);

    const uint64_t closureWord = std::exchange(layout.closureWord(), 0);
    checkCallAfterRead(Entry::operationAOTMaterializeDeferredClosure);
    layout.closureWord() = closureWord;

    checkCallWithoutClosure(environment.address());
}

static void testPresenceFromLeafLookup()
{
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    FakeTypedObject object;
    FakeObject<16> keyWithoutName;
    keyWithoutName.setHeader(0, FinalObjectType);
    auto ask = [&](PresenceQuestion question, LeafPresence answer) {
        runner.recordCallsOf(Entry::operationAOTInByValGeneric);
        runner.setResultOfRecordedOperation(static_cast<uint64_t>(answer));
        StubRun run = runner.run(inByValCached(question), { object.address(), keyWithoutName.address() });
        runner.countCallsOf(Entry::operationAOTInByValGeneric);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == keyWithoutName.address());
        return run.result;
    };

    AOT_TEST_CHECK(ask(PresenceQuestion::In, LeafPresence::Own) == 1 && ask(PresenceQuestion::HasOwn, LeafPresence::Own) == 1);
    AOT_TEST_CHECK(ask(PresenceQuestion::In, LeafPresence::Inherited) == 1 && !ask(PresenceQuestion::HasOwn, LeafPresence::Inherited));
    AOT_TEST_CHECK(!ask(PresenceQuestion::In, LeafPresence::Absent) && !ask(PresenceQuestion::HasOwn, LeafPresence::Absent));

    constexpr std::pair<PresenceQuestion, Entry> cachingOperations[] = {
        { PresenceQuestion::In, Entry::operationAOTInByValAndCache },
        { PresenceQuestion::HasOwn, Entry::operationAOTHasOwnByValAndCache },
    };
    for (auto [question, cachingOperation] : cachingOperations) {
        runner.recordCallsOf(cachingOperation);
        runner.setResultOfRecordedOperation(1);
        StubRun run = runner.run(inByValCached(question), { object.address(), keyWithoutName.address() });
        runner.countCallsOf(cachingOperation);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 1);
        AOT_TEST_CHECK(run.recordedArguments[0] == object.address() && run.recordedArguments[1] == keyWithoutName.address());
    }
}

std::span<const NativeTest> stubPropertyReadTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/get-by-id/typed-fields"_s, ImageTests::whenExecutable<testTypedFields> },
        { "stubs/get-by-id/fields-or-inherited-values"_s, ImageTests::whenExecutable<testFieldsOrInheritedValues> },
        { "stubs/get-by-id/empty-slots-that-may-be-undefined"_s, ImageTests::whenExecutable<testEmptySlotsThatMayBeUndefined> },
        { "stubs/get-by-id/fields-in-extension-or-inherited-values"_s, ImageTests::whenExecutable<testFieldsInExtensionOrInheritedValues> },
        { "stubs/get-by-id/fields-or-inherited-getters"_s, ImageTests::whenExecutable<testFieldsOrInheritedGetters> },
        { "stubs/get-by-id/inherited-fields"_s, ImageTests::whenExecutable<testInheritedFields> },
        { "stubs/get-by-id/inherited-fields-of-holder-that-is-not-filed"_s, ImageTests::whenExecutable<testInheritedFieldsOfHolderThatIsNotFiled> },
        { "stubs/get-by-val/places-of-keyed-values"_s, ImageTests::whenExecutable<testPlacesOfKeyedValues> },
        { "stubs/get-by-id/fields-or-inherited-fields"_s, ImageTests::whenExecutable<testFieldsOrInheritedFields> },
        { "stubs/get-by-id/fields-in-extension-or-inherited-fields"_s, ImageTests::whenExecutable<testFieldsInExtensionOrInheritedFields> },
        { "stubs/get-by-id/names-made-at-run-time"_s, ImageTests::whenExecutable<testNamesMadeAtRunTime> },
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        { "stubs/get-by-id/names-first-used-by-a-stub"_s, ImageTests::whenExecutable<testNamesFirstUsedByStub> },
#endif
        { "stubs/get-by-id/deferred-closures"_s, ImageTests::whenExecutable<testDeferredClosures> },
        { "stubs/get-by-id/typed-array-lengths"_s, ImageTests::whenExecutable<testTypedArrayLengths> },
        { "stubs/get-by-id/fields-found-in-field-table"_s, ImageTests::whenExecutable<testFieldsFoundInFieldTable> },
        { "stubs/get-by-id/booleans-and-doubles-found-in-field-table"_s, ImageTests::whenExecutable<testBooleansAndDoublesFoundInFieldTable> },
        { "stubs/get-by-id/missing-fields-answered-by-field-table"_s, ImageTests::whenExecutable<testMissingFieldsAnsweredByFieldTable> },
        { "stubs/get-by-id/instructions-of-reads-by-kind-of-field"_s, ImageTests::whenExecutable<testInstructionsOfReadsByKindOfField> },
        { "stubs/get-by-id/instructions-of-reads-of-filed-fields"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields> },
        { "stubs/get-by-id/instructions-of-reads-of-missing-fields-below-classes"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfMissingFieldsBelowClasses> },
        { "stubs/get-by-id/lookups-left-to-the-caches"_s, ImageTests::whenExecutable<testLookupsLeftToTheCaches> },
        { "stubs/get-by-id/names-outside-layout-are-absent"_s, ImageTests::whenExecutable<testNamesOutsideLayoutAreAbsent> },
        { "stubs/get-by-id/reads-at-sites"_s, ImageTests::whenExecutable<testReadsAtSites> },
        { "stubs/get-by-id/hits-at-sites"_s, ImageTests::whenExecutable<testHitsAtSites> },
        { "stubs/get-by-id/sites-of-other-structures"_s, ImageTests::whenExecutable<testSitesOfOtherStructures> },
        { "stubs/call-method-by-id/closures-not-created"_s, ImageTests::whenExecutable<testCallsOfClosuresNotCreated> },
        { "stubs/in-by-val/presence-from-leaf-lookup"_s, ImageTests::whenExecutable<testPresenceFromLeafLookup> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
