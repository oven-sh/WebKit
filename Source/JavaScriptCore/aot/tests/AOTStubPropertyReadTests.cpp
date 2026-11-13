/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTFunctionData.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCachesWords.h"
#include "AOTFieldTable.h"
#include "AOTImageText.h"
#include "AOTLayoutData.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRuntimeRecords.h"
#include "AOTSlotAccess.h"
#include "AOTSlotRepresentation.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeWords.h"
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
#include <memory>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static constexpr uint64_t untouched = 0xa5a5a5a5a5a5a5a5ULL;

static constinit StubKey stubOfPropertyReads = getById();

template<void (*test)()>
[[maybe_unused]] static void withStubThatKeepsRegisters()
{
    stubOfPropertyReads = getByIdCalledByCode();
    test();
    stubOfPropertyReads = getById();
}

static constexpr uint32_t instructionsOfProbeThatFindsNoField = 8;

static constinit bool readAtPlaceAsksStructureFirst = false;

template<void (*test)()>
[[maybe_unused]] static void withStubThatAsksStructureFirst()
{
    readAtPlaceAsksStructureFirst = true;
    test();
    readAtPlaceAsksStructureFirst = false;
}

namespace {

class PropertyReads {
    WTF_MAKE_NONCOPYABLE(PropertyReads);
public:
    static constexpr uint16_t name = 4;
    static constexpr uint16_t nameMadeAtRunTime = 6;
    static constexpr unsigned numberOfNames = 8;
    static constexpr uint32_t symbolMask = 3;
    static constexpr uint32_t structureID = 0x1000;
    static constexpr uint32_t structureIDOfHolder = 0x0800;

    PropertyReads()
        : PropertyReads({ }, { })
    {
    }

    PropertyReads(StubPolicy&& policy, std::span<const StubKey> stubsOfNames)
        : runner(WTF::move(policy), stubsOfNames)
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
        instance.image.programSymbols = &m_symbols;
        instance.cellCageBase = MarkedBlockCage::baseOf(this);
    }

    UniquedStringImpl* uid() { return reinterpret_cast<UniquedStringImpl*>(&m_identifiers[name]); }

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

    void setSymbols(std::initializer_list<ProgramSymbol> entries, uint16_t number = nameMadeAtRunTime)
    {
        m_symbolEntries.fill(ProgramSymbol { });
        std::ranges::copy(entries, m_symbolEntries.begin() + (number & symbolMask));
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

    void fileInFrontBesideOthers(const Slot& slot, uint16_t column = name)
    {
        uint32_t row = m_rows[slot.structureID.bits() >> StructureRowWords::granuleShift];
        m_planes[static_cast<unsigned>(SlotAccess::Read)][row + column] = *PackedSlot::tryPack(slot, column, runner.instance().cellCageBase);
    }

    StubRun read(uint64_t receiver) { return read(receiver, name); }
    StubRun read(uint64_t receiver, uint16_t otherName)
    {
        runner.forgetAccessTable();
        return readAgain(receiver, otherName);
    }
    StubRun readAgain(uint64_t receiver, uint16_t otherName = name) { return runner.run(stubOfPropertyReads, { receiver }, otherName); }

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

[[maybe_unused]] static bool isWithinBudgetOfRead(const StubRun& run, uint32_t budget, ASCIILiteral which, const auto&... what)
{
    AOT_TEST_CHECK(run.instructionsInStubs);
    if (*run.instructionsInStubs != budget)
        dataLogLn(which, what..., ": ", *run.instructionsInStubs, " instructions, the budget is ", budget);
    return *run.instructionsInStubs <= budget;
}

static constexpr ASCIILiteral firstReadByName = "the first read by name of "_s;
static constexpr ASCIILiteral nextReadByName = "the next read by name of "_s;

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
    std::optional<PackedSlot> packed = PackedSlot::tryPack(field, PropertyReads::name, cageBase);
    AOT_TEST_CHECK(packed && packed->unpack(field.structureID, cageBase, true).headerBits() == field.headerBits());
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
    accessors.setHeader(0, GetterSetterType);
    accessors.at<uint64_t>(GetterSetter::offsetOfGetter()) = getter.address();
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = accessors.address();
    reads.file(createFieldOrInheritedSlot(SlotRepresentation::JSValue, holder, FakeHolder::inlineOffset, Slot::inheritsGetter, cageBase));

    FakeTypedObject objectWithoutField;
    objectWithoutField.field() = JSValue::ValueEmpty;
    StubRun run = reads.read(objectWithoutField.address());
    AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::CompiledFunction && run.result == StubRunner::resultOfCompiledFunction);
    AOT_TEST_CHECK(run.landedCall.values.callee == getter.address() && run.landedCall.values.thisValue == objectWithoutField.address() && !static_cast<uint32_t>(run.landedCall.count));

    FakeObject<sizeof(GetterSetter)> cellOfOtherKind;
    cellOfOtherKind.setHeader(0, FinalObjectType);
    cellOfOtherKind.at<uint64_t>(GetterSetter::offsetOfGetter()) = getter.address();
    for (uint64_t word : std::initializer_list<uint64_t> { cellOfOtherKind.address(), JSValue::ValueEmpty, bitsOf(jsNumber(5)) }) {
        holder.property(FakeHolder::inlineOffset) = word;
        run = reads.read(objectWithoutField.address());
        AOT_TEST_CHECK(run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere);
    }
    holder.property(FakeHolder::inlineOffset) = accessors.address();

    FakeTypedObject object;
    object.field() = bitsOf(jsNumber(7));
    run = reads.read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere && run.result == bitsOf(jsNumber(7)));
}

static void testGettersThatReturnCaptures()
{
    constexpr uint32_t slimClosureStructureID = 0x1080;
    constexpr uint32_t closureNumber = 1;
    PropertyReads reads;
    Instance& instance = reads.runner.instance();
    instance.slimClosureHeaders[0] = slimClosureStructureID;
    FakeObject<2 * sizeof(uint64_t)> environment;
    FakeObject<offsetOfSlimClosureCaptures + sizeof(uint64_t)> getter;
    getter.setHeader(slimClosureStructureID, SlimClosureType);
    getter.at<uint64_t>(offsetOfSlimClosureWord) = reads.runner.closureWord(0, { .returnsCapture = true });
    FakeObject<sizeof(GetterSetter)> accessors;
    accessors.setHeader(0, GetterSetterType);
    accessors.at<uint64_t>(GetterSetter::offsetOfGetter()) = getter.address();
    uint64_t& returnedCapture = accessors.at<uint64_t>(GetterSetter::offsetOfReturnedCapture());
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = accessors.address();
    reads.file(createFieldOrInheritedSlot(SlotRepresentation::JSValue, holder, FakeHolder::inlineOffset, Slot::inheritsGetter, instance.cellCageBase));
    FakeTypedObject object;
    object.field() = JSValue::ValueEmpty;

    constexpr uint32_t capacity = 3;
    std::array<ReturnedCaptureData, capacity + 1> rows { };
    ReturnedCaptureData& row = rows[ReturnedCaptureData::placeOf(closureNumber + 1, capacity)];
    instance.image.returnedCaptures = rows.data();
    instance.image.capacityOfReturnedCaptures = capacity;
    auto answers = [&](uint64_t value) {
        StubRun run = reads.read(object.address());
        return !run.calledOperation && run.landedCall.place == LandedCall::Place::Nowhere && run.result == value;
    };
    auto callsGetter = [&] {
        StubRun run = reads.read(object.address());
        return !run.calledOperation && run.landedCall.place == LandedCall::Place::CompiledFunction && run.landedCall.values.callee == getter.address();
    };

    row = ReturnedCaptureData::create(closureNumber, ReturnedCaptureData::baseOfClosure, offsetOfSlimClosureCaptures, 0);
    getter.at<uint64_t>(offsetOfSlimClosureCaptures) = bitsOf(jsNumber(41));
    AOT_TEST_CHECK(answers(bitsOf(jsNumber(41))) && returnedCapture == getter.address() + offsetOfSlimClosureCaptures);
    getter.at<uint64_t>(offsetOfSlimClosureCaptures) = bitsOf(jsNumber(42));
    AOT_TEST_CHECK(answers(bitsOf(jsNumber(42))));
    getter.at<uint64_t>(offsetOfSlimClosureCaptures) = JSValue::ValueEmpty;
    AOT_TEST_CHECK(callsGetter());

    returnedCapture = 0;
    row = ReturnedCaptureData::create(closureNumber, offsetOfSlimClosureCaptures >> ReturnedCaptureData::baseShift, sizeof(uint64_t), static_cast<uint8_t>(SlotRepresentation::OptionalInt32));
    getter.at<uint64_t>(offsetOfSlimClosureCaptures) = environment.address();
    environment.at<uint32_t>(sizeof(uint64_t)) = 7;
    AOT_TEST_CHECK(answers(bitsOf(jsNumber(7))) && answers(bitsOf(jsNumber(7))));
    environment.at<uint32_t>(sizeof(uint64_t)) = static_cast<uint32_t>(absentInt32);
    AOT_TEST_CHECK(callsGetter());

    returnedCapture = 0;
    row = ReturnedCaptureData::create(closureNumber, ReturnedCaptureData::baseOfStatics, sizeof(uint64_t), ReturnedCaptureData::undefinedIsNotMade);
    instance.image.statics = std::bit_cast<uint8_t*>(static_cast<uintptr_t>(environment.address()));
    environment.at<uint64_t>(sizeof(uint64_t)) = bitsOf(jsUndefined());
    AOT_TEST_CHECK(callsGetter());
    environment.at<uint64_t>(sizeof(uint64_t)) = bitsOf(jsNull());
    AOT_TEST_CHECK(answers(bitsOf(jsNull())));

    returnedCapture = 0;
    row = { };
    AOT_TEST_CHECK(callsGetter() && static_cast<int64_t>(returnedCapture) == ReturnedCaptureData::returnsNoCapture && callsGetter());
    returnedCapture = 0;
    getter.at<uint64_t>(offsetOfSlimClosureWord) = reads.runner.closureWord(0);
    AOT_TEST_CHECK(callsGetter() && static_cast<int64_t>(returnedCapture) == ReturnedCaptureData::returnsNoCapture);
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

static KeyedValuePlace& placeOfReadByKey(StubRunner& runner, uint64_t key, uint32_t structureID)
{
    char* places = std::bit_cast<char*>(&runner.instance()) + InstanceLayout::offsetOfKeyedValuePlaces + KeyedValuePlace::offsetOfFirst(SlotAccess::Read);
    return *std::bit_cast<KeyedValuePlace*>(places + KeyedValuePlace::offsetFor(std::bit_cast<JSCell*>(key), std::bit_cast<StructureID>(structureID)));
}

static void testReadByKeyHandsBackOwnAccessor()
{
    constexpr uint32_t structureID = PropertyReads::structureID;
    constexpr int32_t wordInCell = 3;
    constexpr int32_t wordOutOfLine = -2;
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    FakeObject<8 * sizeof(uint64_t)> object;
    object.setHeader(structureID, FinalObjectType);
    FakeObject<8 * sizeof(uint64_t)> storage;
    object.at<uint64_t>(JSObject::butterflyOffset()) = storage.address() + 4 * sizeof(uint64_t);
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = std::bit_cast<uintptr_t>(reads.uid());
    FakeObject<sizeof(GetterSetter)> accessorInCell;
    accessorInCell.setHeader(0, GetterSetterType);
    FakeObject<sizeof(GetterSetter)> accessorOutOfLine;
    accessorOutOfLine.setHeader(0, GetterSetterType);
    object.at<uint64_t>(wordInCell * sizeof(uint64_t)) = accessorInCell.address();
    storage.at<uint64_t>((4 + wordOutOfLine) * sizeof(uint64_t)) = accessorOutOfLine.address();
    KeyedValuePlace& place = placeOfReadByKey(runner, key.address(), structureID);
    auto read = [&](uint64_t byKey) {
        return runner.run(getByValCachedOrOwnAccessor(), { object.address(), byKey });
    };
    auto lay = [&](uint64_t keyOfPlace, uint32_t structureIDOfPlace, int32_t wordIndex) {
        place.key = std::bit_cast<JSCell*>(keyOfPlace);
        place.structureID = structureIDOfPlace;
        place.wordIndex = wordIndex;
    };

    lay(key.address(), structureID + KeyedValuePlace::getterBit, wordInCell);
    StubRun inCell = read(key.address());
    AOT_TEST_CHECK(!inCell.calledOperation && inCell.result == accessorInCell.address());
    lay(key.address(), structureID + KeyedValuePlace::getterBit, wordOutOfLine);
    StubRun outOfLine = read(key.address());
    AOT_TEST_CHECK(!outOfLine.calledOperation && outOfLine.result == accessorOutOfLine.address());

    object.at<uint64_t>(wordInCell * sizeof(uint64_t)) = bitsOf(jsNumber(7));
    lay(key.address(), structureID, wordInCell);
    StubRun value = read(key.address());
    AOT_TEST_CHECK(!value.calledOperation && value.result == bitsOf(jsNumber(7)));
    lay(key.address(), structureID + KeyedValuePlace::absenceBit, 0);
    StubRun absent = read(key.address());
    AOT_TEST_CHECK(!absent.calledOperation && absent.result == bitsOf(jsUndefined()));

    object.at<uint64_t>(wordInCell * sizeof(uint64_t)) = accessorInCell.address();
    constexpr int32_t wordOfField = 5;
    object.at<uint64_t>(wordOfField * sizeof(uint64_t)) = bitsOf(jsNumber(11));
    reads.file(Slot::createField(std::bit_cast<StructureID>(structureID), Slot::Kind::Field, Slot::isOwn, SlotData::create(wordOfField * sizeof(uint64_t), SlotRepresentation::JSValue), 0, 0));
    lay(key.address(), PropertyReads::structureIDOfHolder + KeyedValuePlace::getterBit, wordInCell);
    StubRun atPlaceOfOtherStructure = read(key.address());
    AOT_TEST_CHECK(!atPlaceOfOtherStructure.calledOperation && atPlaceOfOtherStructure.result == bitsOf(jsNumber(11)));
    lay(accessorOutOfLine.address(), structureID + KeyedValuePlace::getterBit, wordInCell);
    StubRun atPlaceOfOtherKey = read(key.address());
    AOT_TEST_CHECK(!atPlaceOfOtherKey.calledOperation && atPlaceOfOtherKey.result == bitsOf(jsNumber(11)));
    lay(key.address(), structureID + KeyedValuePlace::getterBit, wordInCell);
    AOT_TEST_CHECK(read(key.address()).result == accessorInCell.address());
}

static void testReadByKeyFindsFieldInExtensionByPlace()
{
    constexpr uint32_t structureID = PropertyReads::structureID;
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = 8;
    PropertyReads reads;
    uintptr_t cageBase = reads.runner.instance().cellCageBase;
    FakeTypedObject object;
    FakeObject<offsetInExtension + sizeof(uint64_t)> extension;
    extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBitOf(offsetInExtension);
    object.at<uint32_t>(offsetOfExtensionWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
    extension.at<uint64_t>(offsetInExtension) = bitsOf(jsNumber(7));
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = std::bit_cast<uintptr_t>(reads.uid());
    KeyedValuePlace& place = placeOfReadByKey(reads.runner, key.address(), structureID);
    for (StubKey stub : { getByValCached(), getByValCached(RegistersOfCaller::AreKept) }) {
        place = { };
        reads.file(Slot::createField(std::bit_cast<StructureID>(structureID), Slot::Kind::Field, Slot::isOwn | Slot::isOutOfLine, SlotData::create(offsetInExtension, SlotRepresentation::JSValue), 0, offsetOfExtensionWord));
        StubRun first = reads.runner.run(stub, { object.address(), key.address() });
        AOT_TEST_CHECK(!first.calledOperation && first.result == bitsOf(jsNumber(7)));
        AOT_TEST_CHECK(place.key == std::bit_cast<JSCell*>(key.address()) && place.structureID == structureID + KeyedValuePlace::fieldInExtensionBits);
        AOT_TEST_CHECK(place.fieldInExtension.extensionWordOffset == offsetOfExtensionWord && place.fieldInExtension.slotOffset == offsetInExtension);
        reads.clear();
        StubRun again = reads.runner.run(stub, { object.address(), key.address() });
        AOT_TEST_CHECK(!again.calledOperation && again.result == bitsOf(jsNumber(7)));
    }
}

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

    FakeTypedObject objectBeforeFirstRead;
    objectBeforeFirstRead.at<uint32_t>(FakeTypedObject::offsetOfField) = uncreatedDeferredClosure;
    reads.runner.recordCallsOf(Entry::operationAOTReadField);
    reads.runner.setResultOfRecordedOperation(closure.address());
    run = reads.read(objectBeforeFirstRead.address());
    reads.runner.countCallsOf(Entry::operationAOTReadField);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == closure.address());
    AOT_TEST_CHECK(run.recordedArguments[0] == objectBeforeFirstRead.address() && run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits());
    AOT_TEST_CHECK(objectBeforeFirstRead.at<uint32_t>(FakeTypedObject::offsetOfField) == uncreatedDeferredClosure);

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

static void testWhetherLengthIsWrittenInteger()
{
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    auto says = [&](unsigned integer, uint64_t receiver) {
        StubRun run = runner.run(lengthIs(integer), { receiver }, PropertyReads::name);
        AOT_TEST_CHECK(!run.calledOperation);
        return !!run.result;
    };
    auto holdsForEveryInteger = [&](uint64_t receiver, uint32_t length) {
        for (unsigned integer = 0; integer <= greatestIntegerThatLengthIsAskedFor; ++integer) {
            if (says(integer, receiver) != (length == integer))
                return false;
        }
        return true;
    };
    constexpr std::array<uint32_t, 5> lengths { 0, 1, 2, 7, 8 };

    std::array<uint32_t, 4> storage { };
    FakeObject<16> list;
    list.setHeader(PropertyReads::structureID, ArrayType);
    list.at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(storage.data() + 2);
    static_assert(Butterfly::offsetOfPublicLength() == -static_cast<ptrdiff_t>(2 * sizeof(uint32_t)));
    for (IndexingType kind : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithContiguous }) {
        list.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) = kind;
        for (uint32_t length : lengths) {
            storage[0] = length;
            AOT_TEST_CHECK(holdsForEveryInteger(list.address(), length));
        }
    }

    std::array<uint32_t, 8> characters { };
    FakeObject<32> text;
    text.setHeader(PropertyReads::structureID, StringType);
    text.at<uint64_t>(JSString::offsetOfValue()) = std::bit_cast<uintptr_t>(characters.data());
    for (uint32_t length : lengths) {
        characters[StringImpl::lengthMemoryOffset() / sizeof(uint32_t)] = length;
        AOT_TEST_CHECK(holdsForEveryInteger(text.address(), length));
    }
    text.at<uint64_t>(JSString::offsetOfValue()) = JSString::isRopeInPointer;
    for (uint32_t length : lengths) {
        text.at<uint32_t>(JSRopeString::offsetOfLength()) = length;
        AOT_TEST_CHECK(holdsForEveryInteger(text.address(), length));
    }

    auto isLeftToRoadByName = [&](uint64_t receiver) {
        runner.forgetAccessTable();
        return runner.run(lengthIs(1), { receiver }, PropertyReads::name).calledOperation;
    };

    Instance& instance = runner.instance();
    FakeObject<sizeof(JSArrayBufferView)> buffer;
    buffer.setHeader(PropertyReads::structureID, Uint8ArrayType);
    buffer.at<uint8_t>(JSArrayBufferView::offsetOfMode()) = WastefulTypedArray;
    buffer.at<uint64_t>(JSArrayBufferView::offsetOfLength()) = 1;
    AOT_TEST_CHECK(isLeftToRoadByName(buffer.address()));
    instance.receiverStructureID(Receiver::Buffer) = PropertyReads::structureID;
    for (uint32_t length : lengths) {
        buffer.at<uint64_t>(JSArrayBufferView::offsetOfLength()) = length;
        AOT_TEST_CHECK(holdsForEveryInteger(buffer.address(), length));
    }
    instance.builtins.someClassOfViewsWasWrittenAbove = 1;
    AOT_TEST_CHECK(isLeftToRoadByName(buffer.address()));
    instance.builtins.someClassOfViewsWasWrittenAbove = 0;
    buffer.at<uint8_t>(JSArrayBufferView::offsetOfMode()) = ResizableNonSharedWastefulTypedArray;
    AOT_TEST_CHECK(isLeftToRoadByName(buffer.address()));
    buffer.at<uint8_t>(JSArrayBufferView::offsetOfMode()) = WastefulTypedArray;
    instance.receiverStructureID(Receiver::Buffer) = PropertyReads::structureID + 64;
    AOT_TEST_CHECK(isLeftToRoadByName(buffer.address()));
    instance.receiverStructureID(Receiver::Buffer) = 0;

    constexpr uint32_t numberOfClosure = 5;
    constexpr uint16_t numberOfFacts = 3;
    std::array<ClosureFacts, numberOfFacts + 1> facts { };
    std::array<uint16_t, numberOfClosure + 1> factNumbers { };
    factNumbers[numberOfClosure] = numberOfFacts;
    uint8_t& lengthOfFunction = facts[numberOfFacts].length;
    const uint16_t* factNumbersBefore = std::exchange(instance.image.closureFactNumbers, factNumbers.data());
    const ClosureFacts* factsBefore = std::exchange(instance.image.closureFacts, facts.data());
    FakeObject<16> closure;
    closure.setHeader(PropertyReads::structureID, SlimClosureType);
    closure.at<uint64_t>(offsetOfSlimClosureWord) = static_cast<uint64_t>(numberOfClosure) << ClosureWord::functionShift | 0x1234;
    for (uint32_t length : lengths) {
        lengthOfFunction = length;
        AOT_TEST_CHECK(holdsForEveryInteger(closure.address(), length));
    }
    lengthOfFunction = countsAreListed;
    AOT_TEST_CHECK(isLeftToRoadByName(closure.address()));
    lengthOfFunction = 1;
    closure.setHeader(PropertyReads::structureID, SlimClosureWithPropertiesType);
    AOT_TEST_CHECK(isLeftToRoadByName(closure.address()));
    instance.image.closureFactNumbers = factNumbersBefore;
    instance.image.closureFacts = factsBefore;

    reads.file(Slot::createWithoutPayload(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::TypedArrayLength, 0));
    FakeObject<sizeof(JSArrayBufferView)> other;
    other.setHeader(PropertyReads::structureID, Uint8ArrayType);
    other.at<uint8_t>(JSArrayBufferView::offsetOfMode()) = ResizableNonSharedWastefulTypedArray;
    auto saysOfWhatOperationFinds = [&](unsigned integer, JSValue found) {
        runner.forgetAccessTable();
        runner.recordCallsOf(Entry::operationAOTGetById);
        runner.setResultOfRecordedOperation(bitsOf(found));
        StubRun run = runner.run(lengthIs(integer), { other.address() }, PropertyReads::name);
        runner.countCallsOf(Entry::operationAOTGetById);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == other.address() && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name);
        return !!run.result;
    };
    for (unsigned integer = 0; integer <= greatestIntegerThatLengthIsAskedFor; ++integer) {
        for (int32_t found = -1; found <= 8; ++found) {
            AOT_TEST_CHECK(saysOfWhatOperationFinds(integer, jsNumber(found)) == (found == static_cast<int32_t>(integer)));
            AOT_TEST_CHECK(saysOfWhatOperationFinds(integer, JSValue(JSValue::EncodeAsDouble, static_cast<double>(found))) == (found == static_cast<int32_t>(integer)));
        }
        AOT_TEST_CHECK(saysOfWhatOperationFinds(integer, JSValue(JSValue::EncodeAsDouble, -0.0)) == !integer);
        for (JSValue found : { jsUndefined(), jsNull(), jsBoolean(true), jsBoolean(false), JSValue(JSValue::EncodeAsDouble, 0.5), JSValue(JSValue::EncodeAsDouble, PNaN), JSValue(std::bit_cast<JSCell*>(other.address())) })
            AOT_TEST_CHECK(!saysOfWhatOperationFinds(integer, found));
    }
}

namespace {

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
        instance.image.fieldRecordIndices = m_recordIndices.data();
        inStructure<const void*>(CompiledStructure::offsetOfLayout()) = m_layout.data();
        instance.prefix().structureIDBase = m_structures.address();
        instance.propertyCaches.namesOfObjectPrototypes = &m_namesOfObjectPrototypes;
    }

    void setFlagsOfStructure(OptionSet<CompiledStructure::Flag> flags) { inStructure<uint8_t>(CompiledStructure::offsetOfFlags()) = flags.toRaw(); }
    void setRealmOfStructure(uint64_t realm) { inStructure<uint64_t>(Structure::realmOffset()) = realm; }
    void setBitFieldOfStructure(uint32_t bits) { inStructure<uint32_t>(Structure::bitFieldOffset()) = bits; }
    void setBitFieldOfStructureOfHolders(uint32_t bits) { m_structures.at<uint32_t>(Structure::bitFieldOffset()) = bits; }
    void setNamesOfObjectPrototypes(uint64_t bits) { m_namesOfObjectPrototypes = bits; }
    FieldData& field() { return *std::bit_cast<FieldData*>(m_layout.data() + sizeof(LayoutData)); }

private:
    static_assert(PropertyReads::numberOfNames <= 64);
    std::array<uint32_t, highestShape + highestShape + PropertyReads::numberOfNames> m_table { };
    std::array<uint16_t, highestShape + PropertyReads::numberOfNames> m_recordIndices { };
    template<typename T> T& inStructure(ptrdiff_t offset) { return m_structures.at<T>(PropertyReads::structureID + offset); }

    alignas(LayoutData) std::array<uint8_t, sizeof(LayoutData) + sizeof(FieldData)> m_layout { };
    FakeObject<PropertyReads::structureID + sizeof(CompiledStructure)> m_structures;
    uint64_t m_namesOfObjectPrototypes { 0 };
};

struct FakeObjectWithShape : FakeTypedObject {
    explicit FakeObjectWithShape(uint32_t shape) { at<uint32_t>(offsetOfShapeWord) = shapeWordOf(shape); }
};

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
    table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::OptionalInt64);
    for (int64_t value : { static_cast<int64_t>(INT32_MAX) + 1, static_cast<int64_t>(INT32_MIN) - 1, maxIntegerAddedTo, minIntegerAddedTo, absentInteger, undefinedInteger, nullInteger }) {
        withInt64.field() = static_cast<uint64_t>(value);
        AOT_TEST_CHECK(reads.read(withInt64.address()).calledOperation);
    }
}

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

    table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure);
    FakeObjectWithShape withClosure(FieldTable::shapeWithNarrowCell);
    withClosure.at<uint32_t>(offsetOfOrderWord) = 0;
    withClosure.field() = untouched << 32 | uncreatedDeferredClosure;
    AOT_TEST_CHECK(reads.read(withClosure.address()).calledOperation);
}

template<int onlyRow = -1, uint32_t instructionsThatWereEnough = 0>
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
        uint32_t budgetOfNextRead;
    };
    auto absent = [](SlotRepresentation representation) { return traitsOf(representation).absentBits; };
    const Read readsByKind[] = {
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, bitsOf(jsNumber(7)), jsNumber(7), 17, 17 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 7, jsNumber(7), 21, 21 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address())), JSValue(std::bit_cast<JSCell*>(cell.address())), 24, 24 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, JSValue::ValueTrue, jsBoolean(true), 31, 31 },
        { "a boolean"_s, FieldTable::shapeWithBoolean, SlotRepresentation::Boolean, 1, jsBoolean(true), 32, 32 },
        { "a double"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, std::bit_cast<uint64_t>(0.5), jsDoubleNumber(0.5), 37, 37 },
        { "a 64-bit integer"_s, FieldTable::shapeWithInt64, SlotRepresentation::OptionalInt64, 7, jsNumber(7), 35, 35 },
        { "a boxed value that is missing"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, absent(SlotRepresentation::JSValue), jsUndefined(), 32, 32 },
        { "an optional boolean that is missing"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, absent(SlotRepresentation::OptionalBoolean), jsUndefined(), 46, 46 },
        { "an integer that is missing"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, absent(SlotRepresentation::OptionalInt32), jsUndefined(), 66, 66 },
        { "a narrow reference that is missing"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, absent(SlotRepresentation::OptionalCell32), jsUndefined(), 74, 74 },
        { "a double that is missing"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, absent(SlotRepresentation::OptionalDouble), jsUndefined(), 91, 91 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByKind) {
        if (onlyRow >= 0 && &read != &readsByKind[std::max(onlyRow, 0)])
            continue;
        table.field() = { };
        table.field().name = PropertyReads::name;
        table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, read.representation);
        FakeObjectWithShape object(read.shape);
        object.at<uint32_t>(offsetOfOrderWord) = 0;
        object.field() = read.slot;
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected));
        isWithinBudget &= isWithinBudgetOfRead(run, read.budget, firstReadByName, read.what);
        run = reads.readAgain(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected));
        isWithinBudget &= isWithinBudgetOfRead(run, onlyRow >= 0 ? instructionsThatWereEnough : read.budgetOfNextRead, nextReadByName, read.what);
    }
    if (onlyRow >= 0) {
        AOT_TEST_CHECK(isWithinBudget);
        return;
    }

    FakeExtension extension(FakeExtension::bitOfSlot);
    extension.granule() = bitsOf(jsNumber(7));
    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.at<uint32_t>(offsetOfOrderWord) = 0;
    withOutOfLineValue.field() = extension.encode(cageBase);
    StubRun run = reads.read(withOutOfLineValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    isWithinBudget &= isWithinBudgetOfRead(run, 38, firstReadByName, "a boxed value out of line"_s);
    run = reads.readAgain(withOutOfLineValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    isWithinBudget &= isWithinBudgetOfRead(run, 38, nextReadByName, "a boxed value out of line"_s);
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

template<int onlyRow = -1, uint32_t instructionsThatWereEnough = 0>
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
        { SlotRepresentation::JSValue, jsNumber(7), 62 },
        { SlotRepresentation::Cell, cellValue, 62 },
        { SlotRepresentation::Cell32, cellValue, 64 },
        { SlotRepresentation::OptionalCell32, cellValue, 64 },
        { SlotRepresentation::OptionalInt32, jsNumber(7), 67 },
        { SlotRepresentation::Int32, jsNumber(7), 67 },
        { SlotRepresentation::Double, jsDoubleNumber(0.5), 80 },
        { SlotRepresentation::OptionalDouble, jsDoubleNumber(0.5), 77 },
        { SlotRepresentation::OptionalDouble, jsNumber(7), 77 },
        { SlotRepresentation::Boolean, jsBoolean(true), 72 },
        { SlotRepresentation::OptionalBoolean, jsBoolean(true), 72 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByRepresentation) {
        if (onlyRow >= 0 && &read != &readsByRepresentation[std::max(onlyRow, 0)])
            continue;
        reads.file(createFieldSlot(read.representation));
        FakeTypedObject object;
        AOT_TEST_CHECK(tryWrite(read.representation, &object.field(), read.value.isInt32() && traitsOf(read.representation).valueKind == SlotValueKind::Double ? jsDoubleNumber(read.value.asInt32()) : read.value, cageBase));
        for (StubRun run : { reads.read(object.address()), reads.readAgain(object.address()) }) {
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.value));
            isWithinBudget &= isWithinBudgetOfRead(run, onlyRow >= 0 ? instructionsThatWereEnough : read.budget, "a read by name of a filed field, "_s, nameOf(read.representation));
        }
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

template<int onlyRow = -1, uint32_t instructionsThatWereEnough = 0>
static void testInstructionsOfReadsOfMissingFieldsBelowClasses()
{
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
        uint32_t budgetOfNextRead;
    };
    const Read readsByKind[] = {
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, 37, 37 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, 51, 51 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 71, 71 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, 79, 79 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByKind) {
        if (onlyRow >= 0 && &read != &readsByKind[std::max(onlyRow, 0)])
            continue;
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
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        isWithinBudget &= isWithinBudgetOfRead(run, read.budget, firstReadByName, read.what, " that is missing below a class");
        run = reads.readAgain(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        isWithinBudget &= isWithinBudgetOfRead(run, onlyRow >= 0 ? instructionsThatWereEnough : read.budgetOfNextRead, nextReadByName, read.what, " that is missing below a class");

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

    for (auto [shape, representation] : { std::pair { FieldTable::shapeWithValue, SlotRepresentation::JSValue }, std::pair { FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32 } }) {
        FakeObjectWithShape object(shape);
        object.field() = 0;
        reads.file(fieldSlot(representation, Slot::absentIsUndefined));
        StubRun run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        reads.file(Slot { });
        AOT_TEST_CHECK(reads.read(object.address()).calledOperation);
    }

    FakeObjectWithShape withoutClosure(FieldTable::shapeWithNarrowCell);
    withoutClosure.field() = absentDeferredClosure;
    AOT_TEST_CHECK(reads.read(withoutClosure.address()).calledOperation);

    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.field() = 0;
    AOT_TEST_CHECK(reads.read(withOutOfLineValue.address()).calledOperation);
    for (uint64_t granuleBit : { FakeExtension::bitOfSlot >> 1, FakeExtension::bitOfSlot << 1, FakeExtension::bitOfSlot }) {
        FakeExtension extension(granuleBit);
        extension.granule() = granuleBit == FakeExtension::bitOfSlot ? 0 : bitsOf(jsNumber(7));
        withOutOfLineValue.field() = extension.encode(cageBase);
        AOT_TEST_CHECK(reads.read(withOutOfLineValue.address()).calledOperation);
    }

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

static void testSymbolsOfRealm()
{
    PropertyReads reads;
    FieldTable table(reads);
    FakeObject<16> realm;
    FakeObject<16> otherRealm;
    reads.runner.instance().globalObject = std::bit_cast<JSGlobalObject*>(static_cast<uintptr_t>(realm.address()));
    reads.setSymbols({ { PropertyReads::name, reads.uid() } }, PropertyReads::name);
    auto read = [&](uint64_t receiver) {
        return reads.runner.run(getByIdOfSymbolOfRealm(), { receiver }, PropertyReads::name);
    };
    const Slot absent = Slot::createWithoutPayload(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Absent, 0);

    FakeObjectWithShape object(FieldTable::shapeWithValue);
    object.field() = bitsOf(jsNumber(7));
    table.setRealmOfStructure(realm.address());
    StubRun run = read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));

    table.setRealmOfStructure(otherRealm.address());
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    reads.file(absent);
    run = read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    reads.setSymbols({ });
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    reads.setSymbols({ { PropertyReads::name, reads.uid() } }, PropertyReads::name);

    reads.clear();
    reads.fileInFrontBesideOthers(createFieldSlot(SlotRepresentation::JSValue));
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    reads.clear();

    table.setRealmOfStructure(realm.address());
    table.field().name = PropertyReads::name;
    table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::JSValue);
    object.at<uint32_t>(offsetOfOrderWord) = 0;
    object.field() = 0;
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    run = read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    table.setNamesOfObjectPrototypes(uint64_t(1) << PropertyReads::name);
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    table.setNamesOfObjectPrototypes(0);
    table.setFlagsOfStructure({ });

    reads.file(Slot::createField(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Field, Slot::isOwn | Slot::absentIsUndefined, SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::JSValue), 0, 0));
    run = read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));

    reads.clear();
    AOT_TEST_CHECK(read(bitsOf(jsUndefined())).calledOperation);
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
        if (otherName == inheritedName || otherName == PropertyReads::nameMadeAtRunTime)
            AOT_TEST_CHECK(run.calledOperation);
        else
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    });

    for (OptionSet<Flag> flags : { OptionSet<Flag> { }, OptionSet<Flag> { Flag::HasExtraProperties }, OptionSet<Flag> { Flag::HasOtherPrototype, Flag::HasOnlyPlainSlots } }) {
        table.setFlagsOfStructure(flags);
        forEachOtherName([&](uint16_t, const StubRun& run) {
            AOT_TEST_CHECK(run.calledOperation);
        });
    }

    table.setFlagsOfStructure(Flag::HasOnlyFieldsBelowObjectPrototype);
    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    withValue.field() = bitsOf(jsNumber(7));
    StubRun run = reads.read(withValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
}

namespace {

constexpr uint16_t nameOutsideLayouts = 2;
static_assert(nameOutsideLayouts != PropertyReads::name && nameOutsideLayouts != PropertyReads::nameMadeAtRunTime && nameOutsideLayouts < PropertyReads::numberOfNames);
constexpr uint32_t offsetOfExtraPropertiesWord = FakeTypedObject::offsetOfField + sizeof(uint64_t);

struct AnswerBehindFieldTable {
    ASCIILiteral what;
    Slot slot;
    uint64_t expected;
    uint32_t budget;
    uint32_t budgetOfNextRead;
};

} // anonymous namespace

static std::array<AnswerBehindFieldTable, 5> answersBehindFieldTable(FakeHolder& other)
{
    const StructureID id = std::bit_cast<StructureID>(PropertyReads::structureID);
    auto extraProperty = [&](PropertyOffset offset) {
        return Slot::createExtraProperty(id, Slot::Kind::ExtraProperty, Slot::isOwn, StructureID(), offset, offsetOfExtraPropertiesWord);
    };
    auto valueOfHolder = [&](PropertyOffset offset) {
        return Slot::createInStorage(id, Slot::Kind::Value, Slot::dependsOnChain, std::bit_cast<JSCell*>(other.address()), offset);
    };
    return { {
        { "an extra property in the cell"_s, extraProperty(FakeHolder::inlineOffset), other.property(FakeHolder::inlineOffset), 32, 32 },
        { "an extra property out of line"_s, extraProperty(FakeHolder::outOfLineOffset), other.property(FakeHolder::outOfLineOffset), 35, 35 },
        { "a value in the cell of a holder"_s, valueOfHolder(FakeHolder::inlineOffset), other.property(FakeHolder::inlineOffset), 67, 20 },
        { "a value out of line in a holder"_s, valueOfHolder(FakeHolder::outOfLineOffset), other.property(FakeHolder::outOfLineOffset), 80, 28 },
        { "a name that nothing has"_s, Slot::createWithoutPayload(id, Slot::Kind::Absent, Slot::dependsOnChain), bitsOf(jsUndefined()), 61, 20 },
    } };
}

static void testAnswersBehindFieldTable()
{
    PropertyReads reads;
    FieldTable table(reads);
    const uintptr_t cageBase = reads.runner.instance().cellCageBase;
    const StructureID id = std::bit_cast<StructureID>(PropertyReads::structureID);
    FakeHolder other;
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(other.bytes.data()) == cageBase);
    other.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    other.property(FakeHolder::outOfLineOffset) = bitsOf(jsNumber(12));
    FakeHolder ordinary;
    ordinary.setHeader(PropertyReads::structureID, FinalObjectType);
    ordinary.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(21));
    ordinary.property(FakeHolder::outOfLineOffset) = bitsOf(jsNumber(22));

    for (StubKey stub : { stubOfPropertyReads }) {
        FakeObjectWithShape withShape(FieldTable::shapeWithValue);
        FakeTypedObject withoutShape;
        for (FakeTypedObject* object : { static_cast<FakeTypedObject*>(&withShape), &withoutShape }) {
            uint32_t& extraPropertiesWord = object->at<uint32_t>(offsetOfExtraPropertiesWord);
            auto read = [&] {
                reads.runner.forgetAccessTable();
                return reads.runner.run(stub, { object->address() }, nameOutsideLayouts);
            };
            for (const AnswerBehindFieldTable& answer : answersBehindFieldTable(other)) {
                reads.clear();
                reads.fileInFrontBesideOthers(answer.slot, nameOutsideLayouts);
                extraPropertiesWord = MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(other.address()));
                StubRun run = read();
                AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected && object->field() == untouched);

                extraPropertiesWord = 0;
                run = read();
                if (answer.slot.kind == Slot::Kind::ExtraProperty)
                    AOT_TEST_CHECK(run.calledOperation);
                else
                    AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
            }

            reads.clear();
            reads.fileInFrontBesideOthers(createFieldSlot(SlotRepresentation::JSValue), nameOutsideLayouts);
            object->field() = bitsOf(jsNumber(7));
            StubRun run = read();
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));

            reads.clear();
            reads.fileInFrontBesideOthers(Slot::createWithoutPayload(id, Slot::Kind::Absent, Slot::dependsOnChain), PropertyReads::name);
            AOT_TEST_CHECK(read().calledOperation);
        }

        for (PropertyOffset offset : { FakeHolder::inlineOffset, FakeHolder::outOfLineOffset }) {
            reads.clear();
            reads.fileInFrontBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, offset), nameOutsideLayouts);
            reads.runner.forgetAccessTable();
            StubRun run = reads.runner.run(stub, { ordinary.address() }, nameOutsideLayouts);
            AOT_TEST_CHECK(!run.calledOperation && run.result == ordinary.property(offset));
        }
    }
}

static void testInstructionsOfReadsBehindFieldTable()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    PropertyReads reads;
    FieldTable table(reads);
    FakeHolder other;
    other.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    other.property(FakeHolder::outOfLineOffset) = bitsOf(jsNumber(12));
    FakeObjectWithShape object(FieldTable::shapeWithValue);
    object.at<uint32_t>(offsetOfExtraPropertiesWord) = MarkedBlockCage::encode(reads.runner.instance().cellCageBase, std::bit_cast<JSCell*>(other.address()));
    reads.runner.setCountsInstructions(true);

    bool isWithinBudget = true;
    for (const AnswerBehindFieldTable& answer : answersBehindFieldTable(other)) {
        reads.clear();
        reads.fileInFrontBesideOthers(answer.slot, nameOutsideLayouts);
        StubRun run = reads.read(object.address(), nameOutsideLayouts);
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
        isWithinBudget &= isWithinBudgetOfRead(run, answer.budget + instructionsOfProbeThatFindsNoField, firstReadByName, answer.what);
        run = reads.readAgain(object.address(), nameOutsideLayouts);
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
        isWithinBudget &= isWithinBudgetOfRead(run, answer.budgetOfNextRead + instructionsOfProbeThatFindsNoField, nextReadByName, answer.what);
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

static void testReadsThatHandlersAnswerLeapOverNoAnswer()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    PropertyReads reads;
    FieldTable table(reads);
    reads.runner.setCountsInstructions(true);
    FakeObjectWithShape object(FieldTable::shapeWithValue);
    object.field() = bitsOf(jsNumber(7));
    reads.clear();
    reads.fileInFrontBesideOthers(createFieldSlot(SlotRepresentation::JSValue), nameOutsideLayouts);
    reads.runner.forgetAccessTable();
    StubRun run = reads.runner.run(getById(), { object.address() }, nameOutsideLayouts);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)) && run.leapsInStubs);
    constexpr uint32_t leapsToHandlerAndInIt = 4;
    if (*run.leapsInStubs != leapsToHandlerAndInIt)
        dataLogLn("a read that a handler answers: ", *run.leapsInStubs, " leaps, the budget is ", leapsToHandlerAndInIt);
    AOT_TEST_CHECK(*run.leapsInStubs <= leapsToHandlerAndInIt);
#endif
}

static void testReadsAnsweredByAccessTable()
{
    PropertyReads reads;
    FieldTable table(reads);
    Instance& instance = reads.runner.instance();
    AccessTableWords& words = instance.propertyCaches.accessTable;
    const uintptr_t cageBase = instance.cellCageBase;
    const StructureID id = std::bit_cast<StructureID>(PropertyReads::structureID);
    auto placeOf = [&](uint16_t name) -> AccessTable::Read& { return words.reads[AccessTable::indexFor(PropertyReads::structureID, name)]; };
    auto keyOf = [](uint16_t name) { return AccessTable::keyFor(PropertyReads::structureID, name); };
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);

    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    withValue.field() = bitsOf(jsNumber(7));
    StubRun run = reads.read(withValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    AOT_TEST_CHECK(!placeOf(PropertyReads::name).keyOfOwnValue && !placeOf(PropertyReads::name).keyOfOtherAnswer && !words.numberOfKeysThatDependOnChains);
    AOT_TEST_CHECK(reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);

    FakeExtension extension(FakeExtension::bitOfSlot);
    extension.granule() = bitsOf(jsNumber(5));
    struct Field {
        uint32_t shape;
        uint64_t slot;
        JSValue expected;
    };
    const Field fields[] = {
        { FieldTable::shapeWithInt32, untouched << 32 | 9, jsNumber(9) },
        { FieldTable::shapeWithNarrowCell, untouched << 32 | MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address())), JSValue(std::bit_cast<JSCell*>(cell.address())) },
        { FieldTable::shapeWithEncodedByte, untouched << 8 | JSValue::ValueTrue, jsBoolean(true) },
        { FieldTable::shapeWithBoolean, untouched << 8 | 1, jsBoolean(true) },
        { FieldTable::shapeWithDouble, std::bit_cast<uint64_t>(0.5), jsDoubleNumber(0.5) },
        { FieldTable::shapeWithInt64, 7, jsNumber(7) },
        { FieldTable::shapeWithOutOfLineValue, untouched << 32 | extension.encode(cageBase), jsNumber(5) },
    };
    for (const Field& field : fields) {
        FakeObjectWithShape object(field.shape);
        object.field() = field.slot;
        for (StubRun read : { reads.read(object.address()), reads.readAgain(object.address()) })
            AOT_TEST_CHECK(!read.calledOperation && read.result == bitsOf(field.expected));
        AOT_TEST_CHECK(!placeOf(PropertyReads::name).keyOfOwnValue && !placeOf(PropertyReads::name).keyOfOtherAnswer && !words.numberOfKeysThatDependOnChains);
    }

    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    holder.property(FakeHolder::outOfLineOffset) = bitsOf(jsNumber(12));
    withValue.at<uint32_t>(offsetOfExtraPropertiesWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(holder.address()));
    auto readFiledInFront = [&](const Slot& slot, uint64_t receiver) {
        reads.clear();
        reads.fileInFrontBesideOthers(slot, nameOutsideLayouts);
        StubRun first = reads.read(receiver, nameOutsideLayouts);
        reads.clear();
        return first;
    };
    for (const AnswerBehindFieldTable& answer : answersBehindFieldTable(holder)) {
        const bool isFiled = answer.slot.kind != Slot::Kind::ExtraProperty;
        run = readFiledInFront(answer.slot, withValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
        AOT_TEST_CHECK((placeOf(nameOutsideLayouts).keyOfOtherAnswer == keyOf(nameOutsideLayouts)) == isFiled && words.numberOfKeysThatDependOnChains == isFiled && !placeOf(nameOutsideLayouts).keyOfOwnValue);
        run = reads.readAgain(withValue.address(), nameOutsideLayouts);
        if (!isFiled) {
            AOT_TEST_CHECK(run.calledOperation);
            continue;
        }
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
        if (answer.slot.kind == Slot::Kind::Absent)
            AOT_TEST_CHECK(placeOf(nameOutsideLayouts).otherAnswer == std::bit_cast<uintptr_t>(&words.undefinedValue));
        else {
            uint64_t& property = holder.property(answer.slot.has(Slot::isOutOfLine) ? FakeHolder::outOfLineOffset : FakeHolder::inlineOffset);
            uint64_t before = std::exchange(property, bitsOf(jsNumber(13)));
            AOT_TEST_CHECK(reads.readAgain(withValue.address(), nameOutsideLayouts).result == bitsOf(jsNumber(13)));
            property = before;
        }
        AOT_TEST_CHECK(words.keysThatDependOnChains[0] == &placeOf(nameOutsideLayouts).keyOfOtherAnswer);
        words.didChangeObjectOnSomeChain();
        AOT_TEST_CHECK(!words.numberOfKeysThatDependOnChains && reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);

        reads.runner.forgetAccessTable();
        words.numberOfKeysThatDependOnChains = AccessTable::maxKeysListed;
        reads.fileInFrontBesideOthers(answer.slot, nameOutsideLayouts);
        run = reads.readAgain(withValue.address(), nameOutsideLayouts);
        reads.clear();
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected && words.numberOfKeysThatDependOnChains == AccessTable::maxKeysListed + 1);
        AOT_TEST_CHECK(!reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation && words.numberOfKeysThatDependOnChains == AccessTable::maxKeysListed + 1);
        words.didChangeObjectOnSomeChain();
        AOT_TEST_CHECK(!words.numberOfKeysThatDependOnChains && reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);

        constexpr uint32_t isDictionary = Structure::s_dictionaryKindMask << Structure::s_dictionaryKindShift;
        table.setBitFieldOfStructure(isDictionary);
        run = readFiledInFront(answer.slot, withValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected && !placeOf(nameOutsideLayouts).keyOfOtherAnswer && !words.numberOfKeysThatDependOnChains);
        table.setBitFieldOfStructure(0);
        if (answer.slot.kind == Slot::Kind::Absent)
            continue;
        table.setBitFieldOfStructureOfHolders(isDictionary);
        run = readFiledInFront(answer.slot, withValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected && !placeOf(nameOutsideLayouts).keyOfOtherAnswer && !words.numberOfKeysThatDependOnChains);
        table.setBitFieldOfStructureOfHolders(0);
    }

    FakeHolder ordinary;
    ordinary.setHeader(PropertyReads::structureID, FinalObjectType);
    for (PropertyOffset offset : { FakeHolder::inlineOffset, FakeHolder::outOfLineOffset }) {
        ordinary.property(offset) = bitsOf(jsNumber(21));
        run = readFiledInFront(Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, offset), ordinary.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(21)) && !words.numberOfKeysThatDependOnChains);
        AOT_TEST_CHECK((isInlineOffset(offset) ? placeOf(nameOutsideLayouts).keyOfOwnValue : placeOf(nameOutsideLayouts).keyOfOtherAnswer) == keyOf(nameOutsideLayouts));
        ordinary.property(offset) = bitsOf(jsNumber(22));
        run = reads.readAgain(ordinary.address(), nameOutsideLayouts);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(22)));
        if (!isInlineOffset(offset))
            continue;
        words.numberOfKeysThatDependOnChains = AccessTable::maxKeysListed + 1;
        words.didChangeObjectOnSomeChain();
        AOT_TEST_CHECK(placeOf(nameOutsideLayouts).keyOfOwnValue == keyOf(nameOutsideLayouts));
        words.forgetEverything();
        AOT_TEST_CHECK(!placeOf(nameOutsideLayouts).keyOfOwnValue);
    }
}

namespace {

class LayoutWithDeferredClosure {
    WTF_MAKE_NONCOPYABLE(LayoutWithDeferredClosure);
public:
    explicit LayoutWithDeferredClosure(PropertyReads& reads)
    {
        Instance& instance = reads.runner.instance();
        m_record.layout.numberOfFields = static_cast<uint16_t>(m_record.fields.size());
        field() = FieldData {
            .name = PropertyReads::name,
            .flags = 0,
            .typeNumber = 0,
            .slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure),
        };
        m_structure.at<const LayoutData*>(CompiledStructure::offsetOfLayout()) = &m_record.layout;
        instance.prefix().structureIDBase = m_structure.address() - PropertyReads::structureID;
        m_recordIndices[FieldTable::shapeWithNarrowCell + PropertyReads::name] = 1;
        instance.image.fieldRecordIndices = m_recordIndices.data();
    }

    FieldData& field() { return m_record.fields[1]; }

private:
    struct Record {
        LayoutData layout;
        std::array<FieldData, 2> fields;
    };
    static_assert(OBJECT_OFFSETOF(Record, fields) == sizeof(LayoutData));

    Record m_record { };
    FakeObject<sizeof(CompiledStructure)> m_structure;
    std::array<uint16_t, FieldTable::highestShape + PropertyReads::numberOfNames> m_recordIndices { };
};

} // anonymous namespace

static void testReadsOfClosuresNotCreated()
{
    PropertyReads reads;
    FieldTable table(reads);
    LayoutWithDeferredClosure layout(reads);
    StubRunner& runner = reads.runner;

    FakeObject<16> closureMadeByRead;
    closureMadeByRead.setHeader(0, JSFunctionType);

    FakeObjectWithShape object(FieldTable::shapeWithNarrowCell);
    object.at<uint32_t>(offsetOfOrderWord) = 0;
    uint32_t& slot = object.at<uint32_t>(FakeTypedObject::offsetOfField);

    auto checkRead = [&](Entry operationOfRead) {
        const uint32_t slotBefore = slot;
        runner.recordCallsOf(operationOfRead);
        runner.setResultOfRecordedOperation(closureMadeByRead.address());
        StubRun run = reads.read(object.address());
        runner.countCallsOf(operationOfRead);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name);
        AOT_TEST_CHECK(run.result == closureMadeByRead.address() && slot == slotBefore);
    };

    slot = uncreatedDeferredClosure;
    for (uint16_t flags : { static_cast<uint16_t>(0), fieldDeferredClosureCapturesObject, fieldDeferredClosureCapturesEnvironment }) {
        layout.field().flags = flags;
        checkRead(Entry::operationAOTMaterializeDeferredClosure);
    }

    slot = absentDeferredClosure;
    checkRead(Entry::operationAOTGetByIdAndCache);

    slot = absentCell32;
    layout.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::Cell32);
    checkRead(Entry::operationAOTGetByIdAndCache);
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

static void testOwnPresenceOfFieldsThatObjectsAboveAnswerFor()
{
    constexpr uint32_t offsetOfExtensionWord = offsetOfFirstField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = 8;
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    Instance& instance = runner.instance();
    uintptr_t cageBase = instance.cellCageBase;
    const StructureID id = std::bit_cast<StructureID>(PropertyReads::structureID);
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = std::bit_cast<uintptr_t>(reads.uid());
    FakeHolder holder;
    holder.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(11));
    FakeTypedHolder typedHolder;
    auto hasOwn = [&](uint64_t object) {
        StubRun run = runner.run(inByValCached(PresenceQuestion::HasOwn), { object, key.address() });
        AOT_TEST_CHECK(!run.calledOperation && run.result <= 1);
        return run.result == 1;
    };

    unsigned representationsAsked = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        SlotTraits traits = traitsOf(representation);
        if (!traits.hasAbsent || traits.valueKind == SlotValueKind::DeferredClosure)
            continue;
        const Slot slots[] = {
            createFieldOrInheritedSlot(representation, holder, FakeHolder::inlineOffset, 0, cageBase),
            createFieldOrInheritedSlot(representation, holder, FakeHolder::inlineOffset, Slot::inheritsGetter, cageBase),
            *Slot::tryCreateFieldOrInheritedField(id, Slot::dependsOnChain, SlotData::create(FakeTypedObject::offsetOfField, representation), std::bit_cast<JSCell*>(typedHolder.address()), 0, cageBase),
        };
        for (const Slot& slot : slots) {
            reads.file(slot);
            FakeTypedObject objectWithoutField;
            writeSlotBits(representation, &objectWithoutField.field(), traits.absentBits);
            AOT_TEST_CHECK(!hasOwn(objectWithoutField.address()));
            for (JSValue value : { jsNumber(7), jsBoolean(true) }) {
                FakeTypedObject object;
                if (!tryWrite(representation, &object.field(), value, cageBase))
                    continue;
                AOT_TEST_CHECK(hasOwn(object.address()));
                ++representationsAsked;
            }
        }
    }
    AOT_TEST_CHECK(representationsAsked);

    {
        reads.file(createFieldOrInheritedSlot(SlotRepresentation::JSValue, holder, FakeHolder::inlineOffset, Slot::emptySlotMayBeUndefined, cageBase));
        FakeTypedObject object;
        object.field() = bitsOf(jsNumber(7));
        AOT_TEST_CHECK(hasOwn(object.address()));
        object.field() = JSValue::ValueEmpty;
        runner.recordCallsOf(Entry::operationAOTHasOwnByValAndCache);
        runner.setResultOfRecordedOperation(1);
        StubRun run = runner.run(inByValCached(PresenceQuestion::HasOwn), { object.address(), key.address() });
        runner.countCallsOf(Entry::operationAOTHasOwnByValAndCache);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.result == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == key.address());
    }

    FakeObject<sizeof(CompiledStructure)> structure;
    instance.prefix().structureIDBase = structure.address() - PropertyReads::structureID;
    structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfExtensionWord, SlotRepresentation::Cell32).encoded();
    SlotData fieldInExtension = SlotData::create(offsetInExtension, SlotRepresentation::JSValue);
    const Slot slotsOfFieldInExtension[] = {
        *Slot::tryCreateFieldAndHolder(id, Slot::Kind::FieldOrInherited, Slot::isOutOfLine | Slot::dependsOnChain, fieldInExtension, std::bit_cast<JSCell*>(holder.address()), FakeHolder::inlineOffset, cageBase),
        *Slot::tryCreateFieldOrInheritedField(id, Slot::isOutOfLine | Slot::dependsOnChain, fieldInExtension, std::bit_cast<JSCell*>(typedHolder.address()), offsetOfExtensionWord, cageBase),
    };
    for (const Slot& slot : slotsOfFieldInExtension) {
        reads.file(slot);
        FakeTypedObject object;
        FakeObject<offsetInExtension + sizeof(uint64_t)> extension;
        extension.at<uint64_t>(offsetOfExtensionGranuleBits) = 0;
        object.at<uint32_t>(offsetOfExtensionWord) = MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
        extension.at<uint64_t>(offsetInExtension) = bitsOf(jsNumber(7));
        AOT_TEST_CHECK(!hasOwn(object.address()));
        extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBitOf(offsetInExtension);
        AOT_TEST_CHECK(hasOwn(object.address()));
        extension.at<uint64_t>(offsetInExtension) = JSValue::ValueEmpty;
        AOT_TEST_CHECK(!hasOwn(object.address()));
    }
}

static void testFieldsReadOutOfExtension()
{
    constexpr std::array slots { SlotInGranule::Word64, SlotInGranule::Word32At0, SlotInGranule::Word32At4, SlotInGranule::ByteAt0, SlotInGranule::ByteAt1, SlotInGranule::ByteAt2, SlotInGranule::ByteAt3, SlotInGranule::ByteAt4, SlotInGranule::ByteAt5, SlotInGranule::ByteAt6, SlotInGranule::ByteAt7 };
    static_assert(slots.size() == descriptorOf(StubFamily::ReadFieldInExtension).numberOfForms && descriptorOf(StubFamily::ReadFieldInExtension).maxCount + 1 == maxExtensionGranules);
    constexpr uint64_t lastGranule = 1ull << (maxExtensionGranules - 1);
    auto wordOf = [](unsigned granule) {
        return 0x1011121314151617ULL + granule * 0x0101010101010101ULL;
    };
    auto bytesOf = [&](unsigned granule, SlotInGranule slot) {
        uint64_t shifted = wordOf(granule) >> (8 * offsetInGranuleOf(slot));
        return widthOf(slot) == sizeof(uint64_t) ? shifted : shifted & ((1ull << (8 * widthOf(slot))) - 1);
    };

    for (bool usesWideReferences : { false, true }) {
        StubOptions options { .targetFeatures = CPUFeatureSet::buildTarget() };
        options.useWideReferences = usesWideReferences;
        StubRunner runner(options);
        StubCatalog catalog(options);
        FakeObject<offsetOfFirstField + sizeof(uint64_t)> object;
        FakeObject<maxExtensionSize> extension;
        uintptr_t cageBase = MarkedBlockCage::baseOf(&extension);
        runner.instance().cellCageBase = cageBase;
        if (usesWideReferences)
            object.at<uint64_t>(offsetOfFirstField) = extension.address();
        else
            object.at<uint64_t>(offsetOfFirstField) = untouched << 32 | MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));

        auto hold = [&](uint64_t granuleBits) {
            extension.bytes.fill(0xee);
            extension.at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBits;
            unsigned place = 0;
            for (unsigned granule = 0; granule < maxExtensionGranules; ++granule) {
                if (granuleBits >> granule & 1)
                    extension.at<uint64_t>(offsetOfFirstExtensionSlot + extensionGranuleSize * place++) = wordOf(granule);
            }
        };
        auto read = [&](StubKey key) {
            StubRun run = runner.run(key, { object.address() });
            AOT_TEST_CHECK(!run.calledOperation);
            return run.result;
        };

        constexpr std::array<uint64_t, 6> setsOfGranules { 0, UINT64_MAX, 1, lastGranule, 1 | 1ull << 3 | lastGranule, 1ull << 1 | 1ull << 31 | 1ull << 32 | 1ull << 62 };
        for (uint64_t granuleBits : setsOfGranules) {
            hold(granuleBits);
            for (unsigned granule = 0; granule < maxExtensionGranules; ++granule) {
                for (SlotInGranule slot : slots)
                    AOT_TEST_CHECK(read(readFieldInExtension(granule, slot)) == (granuleBits >> granule & 1 ? bytesOf(granule, slot) : 0));
            }
        }

        hold(1 | 1ull << 3 | lastGranule);
        if (usesWideReferences) {
            object.at<uint64_t>(offsetOfFirstField) = 0;
            for (unsigned granule : { 0u, 1u, maxExtensionGranules - 1 }) {
                for (SlotInGranule slot : slots)
                    AOT_TEST_CHECK(!read(readFieldInExtension(granule, slot)));
            }
        }

        RegisterSet written = RegisterSet::macroClobberedGPRs();
        written.merge(RegisterSet::macroClobberedFPRs());
        written.add(GPRInfo::returnValueGPR, IgnoreVectors);
        forEachRegisterVariant(readFieldInExtension(maxExtensionGranules - 1, SlotInGranule::Word32At4), [&](StubKey key) {
            Typed::Effects effects = catalog.effectsOf(key);
            AOT_TEST_CHECK(written.subsumes(catalog.summaryOf(key).clobbered) && catalog.isLeaf(key));
            AOT_TEST_CHECK(effects.writes.isEmpty() && effects.reads.containsOnly(Typed::AbstractHeapSet { Typed::AbstractHeap::ObjectFields }) && !effects.mayCollect && !effects.mayExitSideways());
        });
    }
}

static void testInstanceOfRemembersOrdinaryConstructors()
{
    constexpr uint32_t structureIDOfValues = 0x2000;
    PropertyReads reads;
    StubRunner& runner = reads.runner;
    Instance& instance = runner.instance();
    PropertyCacheWords& words = instance.propertyCaches;
    const StructureID id = std::bit_cast<StructureID>(PropertyReads::structureID);
    auto cellAt = [](uint64_t address) { return std::bit_cast<JSCell*>(address); };

    FakeObject<16> nameOfHasInstance;
    FakeObject<16> nameOfPrototype;
    words.hasInstanceUID = std::bit_cast<UniquedStringImpl*>(nameOfHasInstance.address());
    words.prototypeUID = std::bit_cast<UniquedStringImpl*>(nameOfPrototype.address());

    FakeObject<16> ordinaryMethod;
    ordinaryMethod.setHeader(0, JSFunctionType);
    FakeHolder functionPrototype;
    FakeHolder otherHolder;
    functionPrototype.property(FakeHolder::inlineOffset) = ordinaryMethod.address();
    otherHolder.property(FakeHolder::inlineOffset) = ordinaryMethod.address();
    words.defaultHasInstanceFunction = cellAt(ordinaryMethod.address());
    words.holderOfDefaultHasInstanceFunction = cellAt(functionPrototype.address());

    FakeObject<16> prototypeObject;
    FakeObject<16> otherPrototypeObject;
    FakeObject<16> objectPrototype;
    for (auto* object : { &prototypeObject, &otherPrototypeObject, &objectPrototype })
        object->setHeader(0, FinalObjectType);
    instance.realm.objectPrototype = std::bit_cast<JSObject*>(objectPrototype.address());

    FakeObject<sizeof(Structure)> structureOfValues;
    instance.prefix().structureIDBase = structureOfValues.address() - structureIDOfValues;
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = prototypeObject.address();
    FakeObject<16> value;
    value.setHeader(structureIDOfValues, FinalObjectType);

    FakeHolder constructor;
    constructor.setHeader(PropertyReads::structureID, JSFunctionType);
    constructor.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = ImplementsDefaultHasInstance;

    auto forgetPlaces = [&] { zeroSpan(std::span { words.headersOfPrototypeSlotsOfOrdinaryConstructors }); };
    auto fileMethodIn = [&](FakeHolder& holder) {
        reads.fileBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::dependsOnChain, cellAt(holder.address()), FakeHolder::inlineOffset), words.hasInstanceUID);
    };
    auto ask = [&](uint64_t asked) { return runner.run(instanceOf(InstanceOfOperand::Constructor), { asked, constructor.address() }); };
    auto answersWithoutTables = [&](bool expected) {
        reads.clear();
        StubRun run = ask(value.address());
        return !run.calledOperation && run.result == expected;
    };

    for (PropertyOffset offset : { FakeHolder::inlineOffset, FakeHolder::outOfLineOffset }) {
        forgetPlaces();
        reads.clear();
        fileMethodIn(functionPrototype);
        reads.fileBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, offset), words.prototypeUID);
        constructor.property(offset) = prototypeObject.address();
        StubRun first = ask(value.address());
        AOT_TEST_CHECK(!first.calledOperation && first.result == 1);
        AOT_TEST_CHECK(answersWithoutTables(true));

        constructor.property(offset) = otherPrototypeObject.address();
        structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = objectPrototype.address();
        AOT_TEST_CHECK(answersWithoutTables(false));
        structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = otherPrototypeObject.address();
        AOT_TEST_CHECK(answersWithoutTables(true));
        for (uint64_t primitive : { bitsOf(jsNumber(7)), bitsOf(jsNull()), bitsOf(jsUndefined()) }) {
            StubRun run = ask(primitive);
            AOT_TEST_CHECK(!run.calledOperation && !run.result);
        }

        constructor.property(offset) = bitsOf(jsNumber(3));
        runner.recordCallsOf(Entry::operationAOTDefaultHasInstance);
        StubRun withoutPrototypeObject = ask(value.address());
        runner.countCallsOf(Entry::operationAOTDefaultHasInstance);
        AOT_TEST_CHECK(withoutPrototypeObject.numberOfRecordedCalls == 1 && withoutPrototypeObject.recordedArguments[0] == value.address() && withoutPrototypeObject.recordedArguments[1] == bitsOf(jsNumber(3)));

        constructor.property(offset) = prototypeObject.address();
        structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = prototypeObject.address();
        constructor.setHeader(PropertyReads::structureIDOfHolder, JSFunctionType);
        AOT_TEST_CHECK(ask(value.address()).calledOperation);
        constructor.setHeader(PropertyReads::structureID, JSFunctionType);
        AOT_TEST_CHECK(answersWithoutTables(true));
        forgetPlaces();
        AOT_TEST_CHECK(ask(value.address()).calledOperation);
    }

    {
        std::array<PrototypeChainFilter, PrototypeChainFilter::numberOfFilters> filters { };
        struct {
            FakeObject<sizeof(Structure)> ofValues;
            FakeObject<sizeof(Structure)> ofPrototypes;
            FakeObject<sizeof(Structure)> ofEnd;
            FakeObject<sizeof(Structure)> ofPolyProto;
        } structures;
        FakeObject<16> end;
        FakeObject<64> polyProto;
        std::array<FakeObject<16>, 16> others;
        uint64_t base = std::exchange(instance.prefix().structureIDBase, structures.ofValues.address() - structureIDOfValues);
        auto idOf = [&](auto& structure) { return static_cast<uint32_t>(structure.address() - instance.prefix().structureIDBase); };
        auto setPrototypeIn = [](auto& structure, uint64_t prototype) { structure.template at<uint64_t>(Structure::prototypeOffset()) = prototype; };
        setPrototypeIn(structures.ofValues, otherPrototypeObject.address());
        setPrototypeIn(structures.ofPrototypes, end.address());
        setPrototypeIn(structures.ofEnd, bitsOf(jsNull()));
        setPrototypeIn(structures.ofPolyProto, 0);
        otherPrototypeObject.setHeader(idOf(structures.ofPrototypes), FinalObjectType);
        end.setHeader(idOf(structures.ofEnd), FinalObjectType);
        polyProto.setHeader(idOf(structures.ofPolyProto), FinalObjectType);
        polyProto.at<uint64_t>(offsetRelativeToBase(knownPolyProtoOffset)) = end.address();
        for (auto& other : others)
            other.setHeader(idOf(structures.ofEnd), FinalObjectType);

        auto askBelow = [&](uint64_t asked, uint64_t prototype) {
            constructor.property(FakeHolder::inlineOffset) = prototype;
            StubRun run = ask(asked);
            AOT_TEST_CHECK(!run.calledOperation);
            return run.result;
        };
        uint64_t& header = words.headersOfPrototypeSlotsOfOrdinaryConstructors[PlaceOfOrdinaryConstructor::indexFor(PropertyReads::structureID)];
        auto asksFilter = [&] { return static_cast<uint32_t>(header) == (PropertyReads::structureID ^ PlaceOfOrdinaryConstructor::asksFilter); };
        auto numberOfFilters = [&] { return std::ranges::count_if(filters, [](const PrototypeChainFilter& filter) { return filter.structureID; }); };
        auto bitOf = [](uint64_t object) { return static_cast<uint32_t>(object) * HashedSlot::structureMultiplier >> (32 - PrototypeChainFilter::numberOfBitsLog2); };

        forgetPlaces();
        reads.clear();
        fileMethodIn(functionPrototype);
        reads.fileBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, FakeHolder::inlineOffset), words.prototypeUID);
        AOT_TEST_CHECK(askBelow(value.address(), otherPrototypeObject.address()) == 1 && askBelow(value.address(), end.address()) == 1 && !asksFilter());
        AOT_TEST_CHECK(!askBelow(value.address(), others[0].address()) && asksFilter());
        reads.clear();
        AOT_TEST_CHECK(askBelow(value.address(), end.address()) == 1 && !askBelow(value.address(), others[0].address()) && !words.prototypeChainFiltersMayBeFilled);

        words.prototypeChainFilters = filters.data();
        AOT_TEST_CHECK(askBelow(value.address(), end.address()) == 1 && numberOfFilters() == 1 && words.prototypeChainFiltersMayBeFilled);
        uint64_t objects = std::ranges::find_if(filters, [](const PrototypeChainFilter& filter) { return filter.structureID; })->objects;
        AOT_TEST_CHECK(objects == (1ull << bitOf(end.address()) | 1ull << bitOf(otherPrototypeObject.address())));
        AOT_TEST_CHECK(askBelow(value.address(), otherPrototypeObject.address()) == 1);
        for (auto& other : others) {
            AOT_TEST_CHECK(!askBelow(value.address(), other.address()));
            if (objects >> bitOf(other.address()) & 1)
                continue;
            setPrototypeIn(structures.ofValues, other.address());
            AOT_TEST_CHECK(!askBelow(value.address(), other.address()));
            words.forgetPrototypeChainFilters();
            AOT_TEST_CHECK(!numberOfFilters() && askBelow(value.address(), other.address()) == 1 && numberOfFilters() == 1);
            setPrototypeIn(structures.ofValues, otherPrototypeObject.address());
            words.forgetPrototypeChainFilters();
        }
        for (uint64_t primitive : { bitsOf(jsNumber(7)), bitsOf(jsNull()) })
            AOT_TEST_CHECK(!askBelow(primitive, end.address()));
        words.forgetPrototypeChainFilters();
        AOT_TEST_CHECK(askBelow(polyProto.address(), end.address()) == 1 && !askBelow(polyProto.address(), others[0].address()) && !numberOfFilters());
        polyProto.at<uint8_t>(JSObject::typeInfoFlagsOffset()) = OverridesGetPrototype;
        constructor.property(FakeHolder::inlineOffset) = end.address();
        runner.recordCallsOf(Entry::operationAOTDefaultHasInstance);
        StubRun trapped = ask(polyProto.address());
        constructor.property(FakeHolder::inlineOffset) = bitsOf(jsNumber(3));
        StubRun withoutPrototypeObject = ask(value.address());
        runner.countCallsOf(Entry::operationAOTDefaultHasInstance);
        AOT_TEST_CHECK(trapped.numberOfRecordedCalls == 1 && withoutPrototypeObject.numberOfRecordedCalls == 1 && !numberOfFilters() && asksFilter());

        words.prototypeChainFilters = nullptr;
        words.prototypeChainFiltersMayBeFilled = 0;
        instance.prefix().structureIDBase = base;
        otherPrototypeObject.setHeader(0, FinalObjectType);
        constructor.property(FakeHolder::inlineOffset) = prototypeObject.address();
    }

    forgetPlaces();
    reads.clear();
    fileMethodIn(otherHolder);
    reads.fileBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, FakeHolder::inlineOffset), words.prototypeUID);
    StubRun methodLiesElsewhere = ask(value.address());
    AOT_TEST_CHECK(!methodLiesElsewhere.calledOperation && methodLiesElsewhere.result == 1);
    reads.clear();
    AOT_TEST_CHECK(ask(value.address()).calledOperation);

    forgetPlaces();
    reads.clear();
    fileMethodIn(functionPrototype);
    otherHolder.property(FakeHolder::outOfLineOffset) = prototypeObject.address();
    constructor.property(FakeHolder::outOfLineOffset) = otherPrototypeObject.address();
    reads.fileBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::dependsOnChain, cellAt(otherHolder.address()), FakeHolder::outOfLineOffset), words.prototypeUID);
    StubRun prototypeIsInherited = ask(value.address());
    AOT_TEST_CHECK(!prototypeIsInherited.calledOperation && prototypeIsInherited.result == 1);
    reads.clear();
    AOT_TEST_CHECK(ask(value.address()).calledOperation);

    forgetPlaces();
    reads.clear();
    fileMethodIn(functionPrototype);
    reads.fileBesideOthers(Slot::createInStorage(id, Slot::Kind::Value, Slot::isOwn, nullptr, FakeHolder::outOfLineOffset), words.prototypeUID);
    constructor.property(FakeHolder::outOfLineOffset) = prototypeObject.address();
    ask(value.address());

    constexpr uint32_t numberOfBinding = 2;
    std::array<GlobalBinding, numberOfBinding + 1> bindings { };
    instance.runtime.bindings.globalBindings = bindings.data();
    constexpr uint32_t epochOfGlobalObject = 7;
    FakeObject<sizeof(JSGlobalObject)> globalObject;
    globalObject.setHeader(structureIDOfValues, GlobalObjectType);
    globalObject.at<uint32_t>(JSGlobalObject::globalLexicalBindingEpochOffset()) = epochOfGlobalObject;
    instance.globalObject = std::bit_cast<JSGlobalObject*>(globalObject.address());
    uint64_t wordOfName = constructor.address();
    for (InstanceOfOperand name : { InstanceOfOperand::NameSeenByScripts, InstanceOfOperand::NameOfGlobalObject }) {
        bindings[numberOfBinding].constant = static_cast<EncodedJSValue>(constructor.address());
        StubRun byName = runner.run(instanceOf(name), { value.address() }, numberOfBinding);
        AOT_TEST_CHECK(!byName.calledOperation && byName.result == 1);
        StubRun ofPrimitive = runner.run(instanceOf(name), { bitsOf(jsNumber(7)) }, numberOfBinding);
        AOT_TEST_CHECK(!ofPrimitive.calledOperation && !ofPrimitive.result);

        bindings[numberOfBinding] = { 0, structureIDOfValues, epochOfGlobalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(&wordOfName)) };
        StubRun atLocation = runner.run(instanceOf(name), { value.address() }, numberOfBinding);
        AOT_TEST_CHECK(!atLocation.calledOperation && atLocation.result == 1);

        bindings[numberOfBinding] = { };
        Entry readOfBinding = name == InstanceOfOperand::NameSeenByScripts ? Entry::operationAOTGetGlobalBinding : Entry::operationAOTGetGlobalObjectBinding;
        runner.recordCallsOf(readOfBinding);
        runner.setResultOfRecordedOperation(constructor.address());
        StubRun withoutConstant = runner.run(instanceOf(name), { value.address() }, numberOfBinding);
        runner.countCallsOf(readOfBinding);
        AOT_TEST_CHECK(withoutConstant.numberOfRecordedCalls == 1 && withoutConstant.recordedArguments[0] == numberOfBinding && withoutConstant.result == 1);
    }

    constexpr uint32_t numberOfCell = 3;
    std::array<JSCell*, numberOfCell + 1> cellsOfBuiltins { };
    instance.prefix().builtinCells = cellsOfBuiltins.data();
    auto askBuiltin = [&](uint64_t asked) { return runner.run(instanceOf(InstanceOfOperand::PrototypeThatIsBuiltin), { asked }, numberOfCell); };
    cellsOfBuiltins[numberOfCell] = cellAt(prototypeObject.address());
    StubRun belowBuiltin = askBuiltin(value.address());
    AOT_TEST_CHECK(!belowBuiltin.calledOperation && belowBuiltin.result == 1);
    cellsOfBuiltins[numberOfCell] = cellAt(otherPrototypeObject.address());
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = objectPrototype.address();
    StubRun notBelowBuiltin = askBuiltin(value.address());
    AOT_TEST_CHECK(!notBelowBuiltin.calledOperation && !notBelowBuiltin.result);
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = prototypeObject.address();

    cellsOfBuiltins[numberOfCell] = nullptr;
    runner.recordCallsOf(Entry::operationAOTBuiltinCell);
    runner.setResultOfRecordedOperation(prototypeObject.address());
    StubRun withCellNotMade = askBuiltin(value.address());
    runner.countCallsOf(Entry::operationAOTBuiltinCell);
    AOT_TEST_CHECK(withCellNotMade.numberOfRecordedCalls == 1 && withCellNotMade.recordedArguments[0] == numberOfCell && withCellNotMade.result == 1);
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = objectPrototype.address();
    runner.recordCallsOf(Entry::operationAOTBuiltinCell);
    StubRun rightBelowObjectPrototype = askBuiltin(value.address());
    runner.countCallsOf(Entry::operationAOTBuiltinCell);
    AOT_TEST_CHECK(!rightBelowObjectPrototype.numberOfRecordedCalls && !rightBelowObjectPrototype.result);
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = prototypeObject.address();
    cellsOfBuiltins[numberOfCell] = cellAt(prototypeObject.address());

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    runner.setCountsInstructions(true);
    AOT_TEST_CHECK(isWithinBudgetOfRead(askBuiltin(value.address()), 22, "instanceof of a built-in whose prototype object the stub reads"_s));
    AOT_TEST_CHECK(isWithinBudgetOfRead(ask(value.address()), 32, "instanceof of a remembered constructor, found at the first link"_s));
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = objectPrototype.address();
    AOT_TEST_CHECK(isWithinBudgetOfRead(ask(value.address()), 34, "instanceof of a remembered constructor, not found below Object.prototype"_s));
    AOT_TEST_CHECK(isWithinBudgetOfRead(askBuiltin(value.address()), 15, "instanceof of a built-in, asked of an object right below Object.prototype"_s));
    bindings[numberOfBinding].constant = static_cast<EncodedJSValue>(constructor.address());
    AOT_TEST_CHECK(isWithinBudgetOfRead(runner.run(instanceOf(InstanceOfOperand::NameSeenByScripts), { value.address() }, numberOfBinding), 41, "instanceof of a remembered constructor that a global name holds"_s));
    bindings[numberOfBinding] = { 0, structureIDOfValues, epochOfGlobalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(&wordOfName)) };
    AOT_TEST_CHECK(isWithinBudgetOfRead(runner.run(instanceOf(InstanceOfOperand::NameSeenByScripts), { value.address() }, numberOfBinding), 56, "instanceof of a remembered constructor at the location of a global name"_s));
#endif
}

namespace {

class AnswersAtOnePlace {
    WTF_MAKE_NONCOPYABLE(AnswersAtOnePlace);
public:
    static constexpr uint16_t index = 60000;
    static constexpr uint32_t distance = WTF::roundUpToMultipleOf<64>(sizeof(Instance)) + 64;
    static constexpr ptrdiff_t offsetOfAnswers = InstanceLayout::offsetOfSiteAnswers + static_cast<ptrdiff_t>(index * sizeof(uint64_t));
    static_assert(-offsetOfAnswers <= static_cast<ptrdiff_t>(StubEnvironment::bytesBelowInstance) && -offsetOfAnswers >= static_cast<ptrdiff_t>(sizeof(InstancePrefix) + sizeof(SiteAnswers)));
    static_assert(distance + sizeof(uint16_t) <= InstanceLayout::offsetOfStringCells && !(offsetOfAnswers % static_cast<ptrdiff_t>(alignof(SiteAnswers))));

    explicit AnswersAtOnePlace(StubRunner& runner)
        : m_instance(reinterpret_cast<uint8_t*>(&runner.instance()))
    {
        *reinterpret_cast<uint16_t*>(m_instance + distance) = index;
        runner.instance().propertyCaches.firstWordOfWideAnswers = UINT32_MAX;
        misses() = 0;
        misses(1) = 0;
        answers() = SiteAnswers::forPlacesWithout();
    }

    SiteAnswers& answers() { return *reinterpret_cast<SiteAnswers*>(m_instance + offsetOfAnswers); }
    WideSiteAnswers& wideAnswers() { return *reinterpret_cast<WideSiteAnswers*>(m_instance + offsetOfAnswers); }
    static_assert(-offsetOfAnswers >= static_cast<ptrdiff_t>(sizeof(InstancePrefix) + sizeof(WideSiteAnswers)) && !(offsetOfAnswers % static_cast<ptrdiff_t>(alignof(WideSiteAnswers))));

    uint8_t& misses(unsigned byte = 0) { return m_instance[InstanceLayout::offsetOfMissesOfPlaces + static_cast<ptrdiff_t>(distance % InstanceLayout::bytesReservedForMissesOfPlaces + byte)]; }

private:
    uint8_t* m_instance;
};

struct OrdinaryObjectWithTwoValues : FakeObject<32> {
    static constexpr uint32_t offsetInCell = 16;
    static constexpr int32_t indexInStorage = -2;

    OrdinaryObjectWithTwoValues(uint32_t structureID, int32_t inCell, int32_t inStorage)
    {
        setHeader(structureID, FinalObjectType);
        at<uint64_t>(offsetInCell) = bitsOf(jsNumber(inCell));
        storage[0] = bitsOf(jsNumber(inStorage));
        at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(storage.data() - indexInStorage);
    }

    std::array<uint64_t, 2> storage { };
};

enum class HitAtPlace : uint8_t {
    BoxedField, Int32Field, NarrowReferenceField, DoubleField, OptionalBooleanField, Int64Field, FieldInExtension,
    OwnValueInCell, OwnValueInStorage, ValueInCellOfHolder, ValueInStorageOfHolder, NoProperty,
};

} // anonymous namespace

[[maybe_unused]] static uint32_t instructionsOfHitAtPlace(HitAtPlace hit, ReceiverAtSite receiverIs, std::optional<PresenceQuestion> presence = std::nullopt, bool runsNoCode = false)
{
    using Form = SiteAnswers::Form;
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    const uintptr_t cageBase = reads.runner.instance().cellCageBase;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    FakeObjectWithShape withField(1);
    withField.at<uint32_t>(offsetOfOrderWord) = 0;
    FakeExtension extension(FakeExtension::bitOfSlot);
    extension.granule() = bitsOf(jsNumber(7));
    OrdinaryObjectWithTwoValues receiver(PropertyReads::structureID, 1, 2);
    OrdinaryObjectWithTwoValues holder(PropertyReads::structureIDOfHolder, 3, 4);
    uint64_t wordThatHoldsUndefined = bitsOf(jsUndefined());
    auto wordsFromCage = [&](uint64_t address) { return static_cast<uint32_t>((address - cageBase) / sizeof(EncodedJSValue)); };
    auto inTypedSlot = [&](SlotRepresentation representation, uint64_t bits) {
        place.answers().first = *SiteAnswers::tryByShapeInTypedSlot(shapeWordOf(1), representation, FakeTypedObject::offsetOfField);
        withField.field() = bits;
    };
    auto byStructure = [&](Form form, uint32_t operand) { place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, structureID, form, operand); };

    uint64_t object = withField.address();
    JSValue expected = jsNumber(7);
    switch (hit) {
    case HitAtPlace::BoxedField:
        place.answers().first = SiteAnswers::byShape(shapeWordOf(1), FakeTypedObject::offsetOfField);
        withField.field() = bitsOf(jsNumber(7));
        break;
    case HitAtPlace::Int32Field:
        inTypedSlot(SlotRepresentation::OptionalInt32, 7);
        break;
    case HitAtPlace::NarrowReferenceField:
        inTypedSlot(SlotRepresentation::OptionalCell32, MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address())));
        expected = JSValue(std::bit_cast<JSCell*>(cell.address()));
        break;
    case HitAtPlace::DoubleField:
        inTypedSlot(SlotRepresentation::OptionalDouble, std::bit_cast<uint64_t>(0.5));
        expected = jsDoubleNumber(0.5);
        break;
    case HitAtPlace::OptionalBooleanField:
        inTypedSlot(SlotRepresentation::OptionalBoolean, JSValue::ValueTrue);
        expected = jsBoolean(true);
        break;
    case HitAtPlace::Int64Field:
        inTypedSlot(SlotRepresentation::OptionalInt64, 7);
        break;
    case HitAtPlace::FieldInExtension:
        place.answers().first = *SiteAnswers::tryByShapeInExtension(shapeWordOf(1), FieldTable::offsetInExtension);
        withField.field() = extension.encode(cageBase);
        break;
    case HitAtPlace::OwnValueInCell:
        byStructure(Form::OwnValueInStorage, OrdinaryObjectWithTwoValues::offsetInCell / sizeof(EncodedJSValue));
        object = receiver.address();
        expected = jsNumber(1);
        break;
    case HitAtPlace::OwnValueInStorage:
        byStructure(Form::OwnValueInStorage, static_cast<uint32_t>(OrdinaryObjectWithTwoValues::indexInStorage));
        object = receiver.address();
        expected = jsNumber(2);
        break;
    case HitAtPlace::ValueInCellOfHolder:
        byStructure(Form::ValueInWordOfCage, wordsFromCage(holder.address() + OrdinaryObjectWithTwoValues::offsetInCell));
        object = receiver.address();
        expected = jsNumber(3);
        break;
    case HitAtPlace::ValueInStorageOfHolder:
        byStructure(Form::ValueInStorageOfHolder, wordsFromCage(holder.address()));
        place.answers().first = SiteAnswers::withoutKey(SiteUse::Read, true, static_cast<uint32_t>(OrdinaryObjectWithTwoValues::indexInStorage));
        object = receiver.address();
        expected = jsNumber(4);
        break;
    case HitAtPlace::NoProperty:
        byStructure(Form::ValueInWordOfCage, wordsFromCage(std::bit_cast<uintptr_t>(&wordThatHoldsUndefined)));
        object = receiver.address();
        expected = jsUndefined();
        break;
    }
    reads.runner.setCountsInstructions(true);
    if (presence) {
        reads.runner.instance().propertyCaches.wordOfCageThatHoldsUndefined = wordsFromCage(std::bit_cast<uintptr_t>(&wordThatHoldsUndefined));
        bool standsAbove = hit == HitAtPlace::ValueInCellOfHolder || hit == HitAtPlace::ValueInStorageOfHolder;
        bool isThere = hit != HitAtPlace::NoProperty && (*presence == PresenceQuestion::In || !standsAbove);
        StubRun run = reads.runner.run(findsNameAtSite(*presence, !readAtPlaceAsksStructureFirst), { object }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!run.calledOperation && run.result == static_cast<uint64_t>(isThere));
        return run.instructionsInStubs.value_or(0);
    }
    if (runsNoCode) {
        StubRun run = reads.runner.run(readAtSiteRunningNoCode(receiverIs, !readAtPlaceAsksStructureFirst), { object }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(expected));
        return run.instructionsInStubs.value_or(0);
    }
    StubRun run = reads.runner.run(readAtPlaceAsksStructureFirst ? readAtSiteAskingStructureFirst(receiverIs, hit <= HitAtPlace::FieldInExtension ? SiteUse::ReadUnderNameOfField : SiteUse::Read) : readAtSite(receiverIs), { object }, AnswersAtOnePlace::distance);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(expected) && run.instructionsInStubs);
    return *run.instructionsInStubs;
}

static void testArmsOfAnswersAtPlace()
{
    using Form = SiteAnswers::Form;
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    Instance& instance = reads.runner.instance();
    const uintptr_t cageBase = instance.cellCageBase;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    const StructureID structureIDOfOtherVisitor = std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder);
    auto wordsFromCage = [&](uint64_t address) { return static_cast<uint32_t>((address - cageBase) / sizeof(EncodedJSValue)); };
    constexpr uint32_t offsetOfExtraProperties = 24;
    constexpr int32_t wordInCell = OrdinaryObjectWithTwoValues::offsetInCell / sizeof(EncodedJSValue);
    constexpr int32_t indexInStorage = OrdinaryObjectWithTwoValues::indexInStorage;
    auto operandOfExtraValue = [](int32_t index) { return static_cast<uint32_t>(index) << SiteAnswers::indexShiftInOperandOfExtraValue | offsetOfExtraProperties / sizeof(uint32_t); };
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    FakeObject<64> view;
    view.setHeader(PropertyReads::structureID, Uint8ArrayType);
    view.at<uint64_t>(JSArrayBufferView::offsetOfLength()) = 7;

    struct Arm {
        ASCIILiteral what;
        Form form;
        uint32_t budget;
        uint32_t budgetAsFurtherAnswer;
        uint32_t budgetUnderNameOfField;
    };
    constexpr Arm arms[] = {
        { "a value in the storage of a holder"_s, Form::ValueInStorageOfHolder, 14, 28, 14 },
        { "a value in a word of the cage"_s, Form::ValueInWordOfCage, 12, 22, 12 },
        { "an own field in the cell"_s, Form::OwnFieldInCell, 14, 24, 11 },
        { "an own value in storage"_s, Form::OwnValueInStorage, 0, 0, 14 },
        { "an extra value in the cell"_s, Form::ExtraValueInCell, 18, 28, 18 },
        { "an own value in the cell"_s, Form::OwnValueInCell, 12, 22, 12 },
        { "an extra value in storage"_s, Form::ExtraValueInStorage, 19, 29, 19 },
        { "the length of a typed array"_s, Form::LengthOfTypedArray, 18, 0, 18 },
        { "an own narrow reference"_s, Form::OwnNarrowReferenceInCell, 15, 0, 15 },
        { "an own integer"_s, Form::OwnInt32InCell, 16, 0, 16 },
        { "an own field in the extension"_s, Form::OwnFieldInExtension, 21, 0, 21 },
    };
    bool isWithinBudget = true;
    for (const Arm& arm : arms) {
        for (auto [use, isFurtherAnswer] : { std::pair(SiteUse::Read, false), std::pair(SiteUse::Read, true), std::pair(SiteUse::ReadUnderNameOfField, false), std::pair(SiteUse::ReadUnderNameOfField, true) }) {
            const uint32_t budgetOfSecondAnswer = use == SiteUse::Read ? arm.budget : arm.budgetUnderNameOfField;
            if (!(isFurtherAnswer ? arm.budgetAsFurtherAnswer : budgetOfSecondAnswer))
                continue;
            auto keyIn = [use = use](StubKey key, StubFamily family) {
                return key.withFamily(family).withForm(static_cast<uint8_t>(key.form() + (use == SiteUse::Read ? 0 : readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, use).form())));
            };
            for (bool isTruthy : { true, false }) {
                const int32_t number = isTruthy ? 7 : 0;
                OrdinaryObjectWithTwoValues receiver(PropertyReads::structureID, number, number);
                OrdinaryObjectWithTwoValues holder(PropertyReads::structureIDOfHolder, number, number);
                receiver.at<uint32_t>(offsetOfExtraProperties) = wordsFromCage(holder.address());
                FakeObjectWithShape withField(1);
                withField.at<uint32_t>(offsetOfOrderWord) = 0;
                FakeExtension extension(FakeExtension::bitOfSlot);
                uint64_t object = receiver.address();
                JSValue expected = jsNumber(number);
                uint32_t operand = 0;
                int32_t index = 0;
                switch (arm.form) {
                case Form::ValueInStorageOfHolder:
                    operand = wordsFromCage(holder.address());
                    index = indexInStorage;
                    break;
                case Form::ValueInWordOfCage:
                    operand = wordsFromCage(holder.address() + OrdinaryObjectWithTwoValues::offsetInCell);
                    break;
                case Form::OwnFieldInCell:
                    object = withField.address();
                    withField.field() = bitsOf(expected);
                    operand = FakeTypedObject::offsetOfField;
                    break;
                case Form::OwnValueInStorage:
                    operand = static_cast<uint32_t>(indexInStorage);
                    break;
                case Form::ExtraValueInCell:
                    operand = operandOfExtraValue(wordInCell);
                    break;
                case Form::OwnValueInCell:
                    operand = OrdinaryObjectWithTwoValues::offsetInCell;
                    break;
                case Form::ExtraValueInStorage:
                    operand = operandOfExtraValue(indexInStorage);
                    break;
                case Form::LengthOfTypedArray:
                    object = view.address();
                    view.at<uint64_t>(JSArrayBufferView::offsetOfLength()) = number;
                    break;
                case Form::OwnNarrowReferenceInCell:
                    if (!isTruthy)
                        continue;
                    object = withField.address();
                    withField.field() = MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address()));
                    expected = JSValue(std::bit_cast<JSCell*>(cell.address()));
                    operand = FakeTypedObject::offsetOfField;
                    break;
                case Form::OwnFieldInExtension:
                    object = withField.address();
                    extension.granule() = bitsOf(expected);
                    withField.field() = static_cast<uint64_t>(UINT32_MAX) << 32 | extension.encode(cageBase);
                    operand = *SiteAnswers::operandOfFieldInExtension(FieldTable::offsetInExtension);
                    break;
                case Form::OwnInt32InCell:
                    if (!isTruthy)
                        continue;
                    object = withField.address();
                    withField.field() = number;
                    operand = FakeTypedObject::offsetOfField;
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }
                place.wideAnswers() = { };
                place.answers() = SiteAnswers::forPlacesWithout();
                instance.propertyCaches.hitsOfFurtherAnswers = 0;
                if (isFurtherAnswer) {
                    instance.propertyCaches.firstWordOfWideAnswers = AnswersAtOnePlace::index;
                    WideSiteAnswers& wide = place.wideAnswers();
                    wide.answers.second = *SiteAnswers::tryByStructure(use, structureIDOfOtherVisitor, Form::OwnValueInStorage, static_cast<uint32_t>(indexInStorage));
                    wide.structuresOfFurtherAnswers[1] = structureID.bits();
                    wide.operandsOfFurtherAnswers[1] = operand;
                    wide.formsOfFurtherAnswersHeld[1] = { static_cast<uint8_t>(SiteAnswers::bitsOf(use, arm.form)), static_cast<int8_t>(index) };
                } else {
                    instance.propertyCaches.firstWordOfWideAnswers = UINT32_MAX;
                    place.answers().second = *SiteAnswers::tryByStructure(use, structureID, arm.form, operand);
                    if (index)
                        place.answers().first = SiteAnswers::withoutKey(use, true, static_cast<uint32_t>(index));
                }
                reads.runner.setCountsInstructions(true);
                for (ReceiverAtSite receiverIs : { ReceiverAtSite::IsAnyValue, ReceiverAtSite::IsCell }) {
                    for (NameAtSite name : { NameAtSite::IsAnyName, NameAtSite::HasColumn }) {
                        for (StubFamily family : { StubFamily::ReadAtSite, StubFamily::ReadAtSiteAskingStructureFirst }) {
                            if (use != SiteUse::Read && family == StubFamily::ReadAtSite)
                                continue;
                            StubRun run = reads.runner.run(keyIn(readAtSite(receiverIs, name), family), { object }, AnswersAtOnePlace::distance);
                            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(expected));
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
                            if (family != StubFamily::ReadAtSiteAskingStructureFirst || receiverIs != ReceiverAtSite::IsAnyValue)
                                continue;
#if defined(BUN_HARDENED)
                            constexpr uint32_t instructionsThatBoundIndexOfHandler = 1;
#else
                            constexpr uint32_t instructionsThatBoundIndexOfHandler = 0;
#endif
                            const uint32_t budget = isFurtherAnswer ? arm.budgetAsFurtherAnswer + instructionsThatBoundIndexOfHandler : budgetOfSecondAnswer;
                            if (*run.instructionsInStubs != budget)
                                dataLogLn("a read at a place that holds ", arm.what, use == SiteUse::Read ? "" : " under the name of a field", isFurtherAnswer ? " as a further answer: " : ": ", *run.instructionsInStubs, " instructions, the budget is ", budget);
                            isWithinBudget &= *run.instructionsInStubs <= budget;
#endif
                        }
                        for (StubFamily family : { StubFamily::ReadTruthAtSite, StubFamily::ReadTruthAtSiteAskingStructureFirst }) {
                            if (use != SiteUse::Read && family == StubFamily::ReadTruthAtSite)
                                continue;
                            StubRun run = reads.runner.run(keyIn(readAtSite(receiverIs, name), family), { object }, AnswersAtOnePlace::distance);
                            AOT_TEST_CHECK(!run.calledOperation && run.result == static_cast<uint64_t>(isTruthy));
                        }
                    }
                }
            }
        }
    }
    AOT_TEST_CHECK(isWithinBudget);
}

static void testEmptyWordInCellOfPlainObjectIsAskedBehindPlace()
{
    PropertyReads reads;
    FieldTable table(reads);
    table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    AnswersAtOnePlace place(reads.runner);
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    reads.runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    FakeObject<JSObject::offsetOfInlineStorage() + 2 * sizeof(EncodedJSValue)> receiver;
    receiver.setHeader(PropertyReads::structureID, FinalObjectType);
    receiver.at<uint64_t>(JSObject::butterflyOffset()) = 0;
    receiver.at<uint64_t>(JSObject::offsetOfInlineStorage()) = 0;
    receiver.at<uint64_t>(JSObject::offsetOfInlineStorage() + sizeof(EncodedJSValue)) = bitsOf(jsNumber(9));
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, 1));
    place.answers().second = *SiteAnswers::tryByStructure(SiteUse::ReadUnderNameOfField, structureID, SiteAnswers::Form::OwnFieldInCell, JSObject::offsetOfInlineStorage());
    reads.runner.forgetAccessTable();
    StubRun run = reads.runner.run(readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, SiteUse::ReadUnderNameOfField), { receiver.address() }, AnswersAtOnePlace::distance);
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(9)));
}

static void testStructureFiledWithoutFormIsSentToItsRow()
{
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    reads.runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    OrdinaryObjectWithTwoValues receiver(PropertyReads::structureID, 7, 9);
    reads.runner.setCountsInstructions(true);
    bool isWithinBudget = true;
    for (SiteUse use : { SiteUse::Read, SiteUse::ReadUnderNameOfField }) {
        for (bool rowHoldsSlot : { true, false }) {
            reads.clear();
            if (rowHoldsSlot)
                reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset));
            else
                reads.fileBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset));
            reads.runner.forgetAccessTable();
            place.answers() = SiteAnswers::forPlacesWithout();
            place.answers().second = *SiteAnswers::tryByStructure(use, structureID, SiteAnswers::Form::Nothing, 0);
            StubRun run = reads.runner.run(readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, use), { receiver.address() }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(9)));
            StubRun truth = reads.runner.run(readTruthAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, use), { receiver.address() }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!truth.calledOperation && truth.result == 1);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
            if (!rowHoldsSlot)
                continue;
            if (*run.instructionsInStubs != 42)
                dataLogLn("a read at a place that sends the structure to its row: ", *run.instructionsInStubs, " instructions, the budget is 42");
            isWithinBudget &= *run.instructionsInStubs <= 42;
#endif
        }
    }
    AOT_TEST_CHECK(isWithinBudget);
}

static void testWidePlaceAsksItsStructuresInFrontOfShape()
{
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    reads.runner.instance().propertyCaches.firstWordOfWideAnswers = AnswersAtOnePlace::index;
    WideSiteAnswers& wide = place.wideAnswers();
    wide = { };
    wide.answers.first = SiteAnswers::byShape(shapeWordOf(1), FakeTypedObject::offsetOfField);
    wide.structuresThatLackName[0] = PropertyReads::structureIDOfHolder;
    OrdinaryObjectWithTwoValues lacksName(PropertyReads::structureIDOfHolder, 1, 2);
    FakeObjectWithShape withField(1);
    withField.at<uint32_t>(offsetOfOrderWord) = 0;
    withField.field() = bitsOf(jsNumber(7));
    reads.runner.setCountsInstructions(true);
    for (StubFamily family : { StubFamily::ReadAtSite, StubFamily::ReadAtSiteAskingStructureFirst }) {
        const StubKey stub = family == StubFamily::ReadAtSite ? readAtSite(ReceiverAtSite::IsAnyValue, NameAtSite::IsAnyName) : readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, SiteUse::ReadUnderNameOfField);
        StubRun absent = reads.runner.run(stub, { lacksName.address() }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!absent.calledOperation && absent.result == bitsOf(jsUndefined()));
        StubRun field = reads.runner.run(stub, { withField.address() }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!field.calledOperation && field.result == bitsOf(jsNumber(7)));
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
        if (family != StubFamily::ReadAtSiteAskingStructureFirst)
            continue;
        if (*absent.instructionsInStubs != 19 || *field.instructionsInStubs != 31)
            dataLogLn("a wide place where the structure is asked first: ", *absent.instructionsInStubs, " instructions to a structure that lacks the name, the budget is 19; ", *field.instructionsInStubs, " to the answer by shape, the budget is 31");
        AOT_TEST_CHECK(*absent.instructionsInStubs <= 19 && *field.instructionsInStubs <= 31);
#endif
    }
    StubRun truth = reads.runner.run(readTruthAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, SiteUse::ReadUnderNameOfField), { lacksName.address() }, AnswersAtOnePlace::distance);
    AOT_TEST_CHECK(!truth.calledOperation && !truth.result);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    AOT_TEST_CHECK(isWithinBudgetOfRead(truth, 16, "the truth of a name that the structure lacks, at a wide place"_s));
#endif
}

static void testWhetherNameIsThereIsAnsweredByFurtherAnswers()
{
    using Form = SiteAnswers::Form;
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    Instance& instance = reads.runner.instance();
    instance.propertyCaches.firstWordOfWideAnswers = AnswersAtOnePlace::index;
    instance.propertyCaches.wordOfCageThatHoldsUndefined = 12345;
    OrdinaryObjectWithTwoValues visitor(PropertyReads::structureIDOfHolder, 1, 2);
    struct Held {
        Form form;
        uint32_t operand;
        bool isIn;
        bool isOwn;
    };
    for (Held held : { Held { Form::OwnValueInCell, OrdinaryObjectWithTwoValues::offsetInCell, true, true }, Held { Form::ValueInWordOfCage, 12345, false, false }, Held { Form::ValueInWordOfCage, 54321, true, false } }) {
        for (unsigned index = 0; index < WideSiteAnswers::numberOfFurtherAnswers; ++index) {
            WideSiteAnswers& wide = place.wideAnswers();
            wide = { };
            wide.structuresOfFurtherAnswers[index] = PropertyReads::structureIDOfHolder;
            wide.operandsOfFurtherAnswers[index] = held.operand;
            wide.formsOfFurtherAnswersHeld[index] = { static_cast<uint8_t>(SiteAnswers::bitsOf(SiteUse::Read, held.form)), 0 };
            for (bool asksShapeFirst : { false, true }) {
                StubRun in = reads.runner.run(findsNameAtSite(PresenceQuestion::In, asksShapeFirst), { visitor.address() }, AnswersAtOnePlace::distance);
                AOT_TEST_CHECK(!in.calledOperation && in.result == held.isIn);
                StubRun own = reads.runner.run(findsNameAtSite(PresenceQuestion::HasOwn, asksShapeFirst), { visitor.address() }, AnswersAtOnePlace::distance);
                AOT_TEST_CHECK(!own.calledOperation && own.result == held.isOwn);
            }
        }
    }
    instance.propertyCaches.firstWordOfWideAnswers = UINT32_MAX;
}

static void testStubThatCanMeetNoFieldAsksNoShape()
{
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    Instance& instance = reads.runner.instance();
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    instance.image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    place.misses() = 0;
    OrdinaryObjectWithTwoValues visitor(PropertyReads::structureID, 1, 2);
    reads.fileInFrontBesideOthers(Slot::createInStorage(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Value, Slot::isOwn, nullptr, 0));
    reads.runner.setCountsInstructions(true);
    auto instructionsOfMiss = [&](StubKey stub, uint64_t expected) {
        uint32_t instructions = 0;
        for (unsigned turn = 0; turn < 2; ++turn) {
            uint8_t counted = place.misses();
            uint8_t countedForEmptySiteCaches = instance.propertyCaches.missesOfEmptySites;
            StubRun run = reads.runner.run(stub, { visitor.address() }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!run.calledOperation && run.result == expected && place.misses() != counted && instance.propertyCaches.missesOfEmptySites == countedForEmptySiteCaches);
            instructions = run.instructionsInStubs.value_or(0);
        }
        return instructions;
    };
    for (ReceiverAtSite receiverIs : { ReceiverAtSite::IsAnyValue, ReceiverAtSite::IsCell }) {
        [[maybe_unused]] const uint32_t askingShape = instructionsOfMiss(readAtSiteAskingStructureFirst(receiverIs, SiteUse::ReadUnderNameOfField), bitsOf(jsNumber(1)));
        [[maybe_unused]] const uint32_t underNameThatNoLayoutLists = instructionsOfMiss(readAtSiteAskingStructureFirst(receiverIs), bitsOf(jsNumber(1)));
        [[maybe_unused]] const uint32_t ofNoObjectOfCompiledCode = instructionsOfMiss(readAtSiteAskingStructureFirst(receiverIs, SiteUse::ReadUnderNameOfField, true), bitsOf(jsNumber(1)));
        instructionsOfMiss(readTruthAtSiteAskingStructureFirst(receiverIs), 1);
        instructionsOfMiss(readTruthAtSiteAskingStructureFirst(receiverIs, SiteUse::ReadUnderNameOfField, true), 1);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
        if (underNameThatNoLayoutLists + 5 != askingShape || ofNoObjectOfCompiledCode + 5 != askingShape)
            dataLogLn("a miss where the structure is asked first: ", askingShape, " instructions where the shape is asked, ", underNameThatNoLayoutLists, " under a name that no layout lists, ", ofNoObjectOfCompiledCode, " of what is no object of compiled code: the budget is 5 fewer");
        AOT_TEST_CHECK(underNameThatNoLayoutLists + 5 <= askingShape && ofNoObjectOfCompiledCode + 5 <= askingShape);
#endif
    }
}

static void testPlacesWhoseDistancesAgreeInSixteenBitsShareOneCount()
{
    constexpr uint32_t bytesOfCounts = InstanceLayout::bytesReservedForMissesOfPlaces;
    constexpr uint32_t distanceOfSameCount = AnswersAtOnePlace::distance + bytesOfCounts;
    constexpr uint32_t distanceOfLastCount = 2 * bytesOfCounts - sizeof(uint16_t);
    static_assert(distanceOfSameCount < distanceOfLastCount && distanceOfLastCount + sizeof(uint16_t) <= InstanceLayout::offsetOfStringCells);
    constexpr uint8_t step = SiteCacheState::sampleStep;
    constexpr uint8_t oneMissBeforeSample = static_cast<uint8_t>(UINT8_MAX + 1 - step);
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    Instance& instance = reads.runner.instance();
    uint8_t* bytesOfInstance = reinterpret_cast<uint8_t*>(&instance);
    Vector<uint16_t> namesOfSites(FillWith { }, (distanceOfLastCount - AnswersAtOnePlace::distance) / sizeof(uint16_t) + 1, PropertyReads::name);
    instance.image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(namesOfSites.span().data()) - AnswersAtOnePlace::distance;
    for (uint32_t distance : { distanceOfSameCount, distanceOfLastCount })
        *reinterpret_cast<uint16_t*>(bytesOfInstance + distance) = AnswersAtOnePlace::index;
    uint8_t* lastCount = bytesOfInstance + InstanceLayout::offsetOfSiteAnswers - sizeof(uint16_t);
    lastCount[0] = 0;
    lastCount[1] = 0;
    const uint8_t firstByteOfAnswers = lastCount[2];
    OrdinaryObjectWithTwoValues visitor(PropertyReads::structureID, 1, 2);
    reads.fileInFrontBesideOthers(Slot::createInStorage(std::bit_cast<StructureID>(PropertyReads::structureID), Slot::Kind::Value, Slot::isOwn, nullptr, 0));
    auto missAt = [&](uint32_t distance) {
        StubRun run = reads.runner.run(readAtSiteAskingStructureFirst(ReceiverAtSite::IsCell, SiteUse::ReadUnderNameOfField), { visitor.address() }, distance);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(1)) && lastCount[2] == firstByteOfAnswers);
    };

    missAt(AnswersAtOnePlace::distance);
    AOT_TEST_CHECK(place.misses() == step && !place.misses(1) && !lastCount[0] && !lastCount[1]);
    missAt(distanceOfSameCount);
    AOT_TEST_CHECK(place.misses() == 2 * step && !place.misses(1) && !lastCount[0] && !lastCount[1]);
    missAt(distanceOfLastCount);
    AOT_TEST_CHECK(place.misses() == 2 * step && !place.misses(1) && lastCount[0] == step && !lastCount[1]);

    place.misses(1) = 2;
    for (uint32_t distance : { distanceOfSameCount, AnswersAtOnePlace::distance }) {
        const uint8_t leftOut = place.misses(1);
        place.misses() = oneMissBeforeSample;
        missAt(distance);
        AOT_TEST_CHECK(!place.misses() && place.misses(1) == leftOut - 1 && lastCount[0] == step && !lastCount[1]);
    }
    lastCount[0] = oneMissBeforeSample;
    lastCount[1] = 1;
    missAt(distanceOfLastCount);
    AOT_TEST_CHECK(!lastCount[0] && !lastCount[1] && !place.misses() && !place.misses(1));
}

template<HitAtPlace hit, uint32_t budget>
static void testInstructionsOfHitAtPlace()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    constexpr uint32_t instructionsOfTestForCell = 2;
    uint32_t ofAnyValue = instructionsOfHitAtPlace(hit, ReceiverAtSite::IsAnyValue);
    uint32_t ofCell = instructionsOfHitAtPlace(hit, ReceiverAtSite::IsCell);
    if (ofAnyValue != budget || ofCell + instructionsOfTestForCell != budget)
        dataLogLn("a read at a place: ", ofAnyValue, " instructions, of a cell ", ofCell, ", the budget is ", budget);
    AOT_TEST_CHECK(ofAnyValue <= budget && ofCell + instructionsOfTestForCell <= budget);
#endif
}

template<HitAtPlace hit, PresenceQuestion question, uint32_t budget>
static void testInstructionsOfPresenceAtPlace()
{
    [[maybe_unused]] uint32_t instructions = instructionsOfHitAtPlace(hit, ReceiverAtSite::IsAnyValue, question);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    if (instructions != budget)
        dataLogLn("whether a name is there, asked at a place: ", instructions, " instructions, the budget is ", budget);
    AOT_TEST_CHECK(instructions <= budget);
#endif
}

template<HitAtPlace hit, uint32_t budget>
static void testInstructionsOfHitAtPlaceRunningNoCode()
{
    constexpr uint32_t instructionsOfTestForCell = 2;
    [[maybe_unused]] uint32_t ofAnyValue = instructionsOfHitAtPlace(hit, ReceiverAtSite::IsAnyValue, std::nullopt, true);
    [[maybe_unused]] uint32_t ofCell = instructionsOfHitAtPlace(hit, ReceiverAtSite::IsCell, std::nullopt, true);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    if (ofAnyValue != budget || ofCell + instructionsOfTestForCell != budget)
        dataLogLn("a read at a place that runs no code: ", ofAnyValue, " instructions, of a cell ", ofCell, ", the budget is ", budget);
    AOT_TEST_CHECK(ofAnyValue <= budget && ofCell + instructionsOfTestForCell <= budget);
#endif
}

static void testLengthReadAtSite()
{
    PropertyReads reads;
    FieldTable table(reads);
    StubRunner& runner = reads.runner;
    AnswersAtOnePlace place(runner);
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    const StructureID otherStructureID = std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    place.misses() = 0;
    place.answers().first = SiteAnswers::byShape(shapeWordOf(FieldTable::shapeWithOtherField), FakeTypedObject::offsetOfField);
    place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, otherStructureID, SiteAnswers::Form::OwnValueInCell, OrdinaryObjectWithTwoValues::offsetInCell);
    reads.fileInFrontBesideOthers(Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain));
    runner.forgetAccessTable();
    constexpr std::array<uint32_t, 5> lengths { 0, 1, 2, 7, 8 };
    for (bool asksStructureFirst : { false, true }) {
        auto says = [&](uint64_t receiver, JSValue expected) {
            StubRun run = runner.run(readLengthAtSite(asksStructureFirst), { receiver }, AnswersAtOnePlace::distance);
            return !run.calledOperation && run.result == bitsOf(expected);
        };
        auto isAnsweredInFrontOfRecord = [&](uint64_t receiver, uint32_t length) { return says(receiver, jsNumber(static_cast<int32_t>(length))); };
        auto isLeftToRoadOfRecord = [&](uint64_t receiver) { return says(receiver, jsUndefined()); };

        std::array<uint32_t, 4> storage { };
        FakeObject<16> list;
        list.setHeader(PropertyReads::structureID, ArrayType);
        list.at<uint64_t>(JSObject::butterflyOffset()) = std::bit_cast<uintptr_t>(storage.data() + 2);
        for (IndexingType kind : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, CopyOnWriteArrayWithContiguous }) {
            list.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) = kind;
            for (uint32_t length : lengths) {
                storage[0] = length;
                AOT_TEST_CHECK(isAnsweredInFrontOfRecord(list.address(), length));
            }
        }
        storage[0] = 7;
        list.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) = ArrayWithArrayStorage;
        AOT_TEST_CHECK(isLeftToRoadOfRecord(list.address()));
        list.at<uint8_t>(JSCell::indexingTypeAndMiscOffset()) = ContiguousShape;
        AOT_TEST_CHECK(isLeftToRoadOfRecord(list.address()));

        std::array<uint32_t, 8> characters { };
        FakeObject<32> text;
        text.setHeader(PropertyReads::structureID, StringType);
        text.at<uint64_t>(JSString::offsetOfValue()) = std::bit_cast<uintptr_t>(characters.data());
        for (uint32_t length : lengths) {
            characters[StringImpl::lengthMemoryOffset() / sizeof(uint32_t)] = length;
            AOT_TEST_CHECK(isAnsweredInFrontOfRecord(text.address(), length));
        }
        text.at<uint64_t>(JSString::offsetOfValue()) = JSString::isRopeInPointer;
        for (uint32_t length : lengths) {
            text.at<uint32_t>(JSRopeString::offsetOfLength()) = length;
            AOT_TEST_CHECK(isAnsweredInFrontOfRecord(text.address(), length));
        }

        OrdinaryObjectWithTwoValues ordinary(PropertyReads::structureID, 1, 2);
        AOT_TEST_CHECK(isLeftToRoadOfRecord(ordinary.address()));
    }
}

static void testMissAtPlaceRunningNoCode()
{
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    reads.runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    place.misses() = 0;
    OrdinaryObjectWithTwoValues ordinary(PropertyReads::structureID, 1, 2);
    FakeObject<16> text;
    text.setHeader(PropertyReads::structureID, StringType);
    for (bool asksShapeFirst : { true, false }) {
        uint8_t counted = place.misses();
        StubRun run = reads.runner.run(readAtSiteRunningNoCode(ReceiverAtSite::IsAnyValue, asksShapeFirst), { ordinary.address() }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!run.calledOperation && !run.result && place.misses() != counted);
        counted = place.misses();
        run = reads.runner.run(readAtSiteRunningNoCode(ReceiverAtSite::IsAnyValue, asksShapeFirst), { text.address() }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!run.calledOperation && !run.result && place.misses() == counted);
        run = reads.runner.run(readAtSiteRunningNoCode(ReceiverAtSite::IsAnyValue, asksShapeFirst), { bitsOf(jsNumber(7)) }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!run.calledOperation && !run.result && place.misses() == counted);
    }
}

static void testFrontOfTablesAnswersWherePlaceHoldsAnotherStructureRunningNoCode()
{
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    const StructureID otherStructureID = std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    reads.runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    place.misses() = 0;
    place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, otherStructureID, SiteAnswers::Form::OwnValueInCell, OrdinaryObjectWithTwoValues::offsetInCell);
    OrdinaryObjectWithTwoValues ordinary(PropertyReads::structureID, 1, 2);
    auto readIs = [&](uint64_t expected) {
        for (bool asksShapeFirst : { true, false }) {
            for (ReceiverAtSite receiverIs : { ReceiverAtSite::IsAnyValue, ReceiverAtSite::IsCell }) {
                uint8_t counted = place.misses();
                StubRun run = reads.runner.run(readAtSiteRunningNoCode(receiverIs, asksShapeFirst), { ordinary.address() }, AnswersAtOnePlace::distance);
                if (run.calledOperation || run.result != expected || (place.misses() == counted) != !!expected)
                    return false;
            }
        }
        return true;
    };
    AOT_TEST_CHECK(readIs(0));
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, 0));
    AOT_TEST_CHECK(readIs(bitsOf(jsNumber(1))));
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset));
    AOT_TEST_CHECK(readIs(bitsOf(jsNumber(2))));
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Getter, Slot::isOwn, nullptr, firstOutOfLineOffset));
    AOT_TEST_CHECK(readIs(0));
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Getter, Slot::isOwn, nullptr, 0));
    AOT_TEST_CHECK(readIs(0));
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, 0), PropertyReads::name + 1);
    AOT_TEST_CHECK(readIs(0));
    nameOfSite[0] = InstanceLayout::nameOfSiteStandsBehind;
    reads.clear();
    reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, 0));
    AOT_TEST_CHECK(readIs(0));
}

static void testFieldInExtensionByStructure()
{
    using Form = SiteAnswers::Form;
    PropertyReads reads;
    FieldTable table(reads);
    table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    AnswersAtOnePlace place(reads.runner);
    const uintptr_t cageBase = reads.runner.instance().cellCageBase;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    reads.runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    const uint32_t operand = *SiteAnswers::operandOfFieldInExtension(FieldTable::offsetInExtension);
    struct Case {
        uint64_t granuleBit;
        uint64_t word;
        uint64_t read;
        uint64_t readRunningNoCode;
    };
    const Case cases[] = {
        { FakeExtension::bitOfSlot, bitsOf(jsNumber(7)), bitsOf(jsNumber(7)), bitsOf(jsNumber(7)) },
        { FakeExtension::bitOfSlot, 0, bitsOf(jsUndefined()), 0 },
        { FakeExtension::bitOfSlot >> 1, bitsOf(jsNumber(7)), bitsOf(jsUndefined()), 0 },
        { FakeExtension::bitOfSlot << 1, bitsOf(jsNumber(7)), bitsOf(jsUndefined()), 0 },
    };
    for (const Case& one : cases) {
        FakeExtension extension(one.granuleBit);
        extension.granule() = one.word;
        FakeObjectWithShape withField(1);
        withField.at<uint32_t>(offsetOfOrderWord) = 0;
        withField.field() = static_cast<uint64_t>(UINT32_MAX) << 32 | extension.encode(cageBase);
        for (SiteUse use : { SiteUse::Read, SiteUse::ReadUnderNameOfField }) {
            place.answers() = SiteAnswers::forPlacesWithout();
            place.answers().second = *SiteAnswers::tryByStructure(use, structureID, Form::OwnFieldInExtension, operand);
            StubRun run = reads.runner.run(readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue, use), { withField.address() }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!run.calledOperation && run.result == one.read);
        }
        place.answers() = SiteAnswers::forPlacesWithout();
        place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, structureID, Form::OwnFieldInExtension, operand);
        for (bool asksShapeFirst : { true, false }) {
            StubRun run = reads.runner.run(readAtSiteRunningNoCode(ReceiverAtSite::IsAnyValue, asksShapeFirst), { withField.address() }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!run.calledOperation && run.result == one.readRunningNoCode);
        }
    }
}

static void testFormsThatAreNoHitsRunningNoCode()
{
    using Form = SiteAnswers::Form;
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    const uintptr_t cageBase = reads.runner.instance().cellCageBase;
    OrdinaryObjectWithTwoValues receiver(PropertyReads::structureID, 1, 2);
    OrdinaryObjectWithTwoValues holder(PropertyReads::structureIDOfHolder, 3, 4);
    const uint32_t wordsToHolder = static_cast<uint32_t>((holder.address() - cageBase) / sizeof(EncodedJSValue));
    for (Form form : { Form::GetterInStorageOfHolder, Form::GetterInWordOfCage, Form::OwnGetterInStorage, Form::OwnGetterInCell, Form::LengthOfTypedArray, Form::Nothing }) {
        place.answers() = SiteAnswers::forPlacesWithout();
        place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, std::bit_cast<StructureID>(PropertyReads::structureID), form, wordsToHolder);
        for (bool asksShapeFirst : { true, false }) {
            for (ReceiverAtSite receiverIs : { ReceiverAtSite::IsAnyValue, ReceiverAtSite::IsCell }) {
                uint8_t counted = place.misses();
                StubRun run = reads.runner.run(readAtSiteRunningNoCode(receiverIs, asksShapeFirst), { receiver.address() }, AnswersAtOnePlace::distance);
                AOT_TEST_CHECK(!run.calledOperation && !run.result && place.misses() == counted);
            }
        }
    }
}

static void testGettersAnswerCallsByNameAtSite()
{
    using Form = SiteAnswers::Form;
    constexpr uint32_t slimClosureStructureID = 0x1080;
    constexpr uint32_t offsetOfExtraProperties = 24;
    constexpr uint32_t wordInCell = OrdinaryObjectWithTwoValues::offsetInCell / sizeof(EncodedJSValue);
    constexpr int32_t indexInStorage = OrdinaryObjectWithTwoValues::indexInStorage;
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    Instance& instance = reads.runner.instance();
    const uintptr_t cageBase = instance.cellCageBase;
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    instance.slimClosureHeaders[0] = slimClosureStructureID;
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    instance.image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> getter;
    getter.setHeader(slimClosureStructureID, SlimClosureType);
    getter.at<uint64_t>(offsetOfSlimClosureWord) = reads.runner.closureWord(0);
    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> method;
    method.setHeader(slimClosureStructureID, SlimClosureType);
    method.at<uint64_t>(offsetOfSlimClosureWord) = reads.runner.closureWord(2);
    FakeObject<sizeof(GetterSetter)> accessors;
    accessors.setHeader(0, GetterSetterType);
    accessors.at<uint64_t>(GetterSetter::offsetOfGetter()) = getter.address();
    const uint64_t first = bitsOf(jsNumber(21));
    const uint64_t second = bitsOf(jsNumber(22));
    const uint32_t hiddenWord = offsetOfExtraProperties / sizeof(uint32_t);

    enum class Lies : uint8_t { InCellOfReceiver, InStorageOfReceiver, InCellOfOther, InStorageOfOther };
    struct Arm {
        Form form;
        Lies lies;
        bool goesThroughHiddenWord;
        bool borrowsIndex;
    };
    const Arm arms[] = {
        { Form::OwnGetterInCell, Lies::InCellOfReceiver, false, false },
        { Form::ExtraGetterInCellAtCall, Lies::InCellOfOther, true, false },
        { Form::OwnGetterInStorage, Lies::InStorageOfReceiver, false, false },
        { Form::ExtraGetterInStorageAtCall, Lies::InStorageOfOther, true, false },
        { Form::GetterInWordOfCage, Lies::InCellOfOther, false, false },
        { Form::GetterInStorageOfHolder, Lies::InStorageOfOther, false, true },
    };
    for (const Arm& arm : arms) {
        for (bool liesInFirstAnswer : { true, false }) {
            if (arm.borrowsIndex && !liesInFirstAnswer)
                continue;
            OrdinaryObjectWithTwoValues receiver(PropertyReads::structureID, 1, 2);
            OrdinaryObjectWithTwoValues other(PropertyReads::structureIDOfHolder, 3, 4);
            const uint32_t wordsToOther = static_cast<uint32_t>((other.address() - cageBase) / sizeof(EncodedJSValue));
            receiver.at<uint32_t>(offsetOfExtraProperties) = arm.goesThroughHiddenWord ? wordsToOther : 0;
            switch (arm.lies) {
            case Lies::InCellOfReceiver:
                receiver.at<uint64_t>(OrdinaryObjectWithTwoValues::offsetInCell) = accessors.address();
                break;
            case Lies::InStorageOfReceiver:
                receiver.storage[0] = accessors.address();
                break;
            case Lies::InCellOfOther:
                other.at<uint64_t>(OrdinaryObjectWithTwoValues::offsetInCell) = accessors.address();
                break;
            case Lies::InStorageOfOther:
                other.storage[0] = accessors.address();
                break;
            }
            uint32_t operand = 0;
            switch (arm.form) {
            case Form::OwnGetterInCell:
                operand = OrdinaryObjectWithTwoValues::offsetInCell;
                break;
            case Form::ExtraGetterInCellAtCall:
                operand = wordInCell << SiteAnswers::indexShiftInOperandOfExtraValue | hiddenWord;
                break;
            case Form::OwnGetterInStorage:
                operand = static_cast<uint32_t>(indexInStorage);
                break;
            case Form::ExtraGetterInStorageAtCall:
                operand = static_cast<uint32_t>(indexInStorage) << SiteAnswers::indexShiftInOperandOfExtraValue | hiddenWord;
                break;
            case Form::GetterInWordOfCage:
                operand = wordsToOther + wordInCell;
                break;
            default:
                operand = wordsToOther;
                break;
            }
            place.answers() = SiteAnswers::forPlacesWithout();
            (liesInFirstAnswer ? place.answers().first : place.answers().second) = *SiteAnswers::tryByStructure(SiteUse::Call, structureID, arm.form, operand);
            if (arm.borrowsIndex)
                place.answers().second.operand = static_cast<uint32_t>(indexInStorage);

            reads.runner.setResultOfNextLanding(method.address());
            StubRun run = reads.runner.runCall(callMethodAtSite(2), 0, receiver.address(), { first, second }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!run.calledOperation && run.result == StubRunner::resultOfCompiledFunction && run.landedCall.place == LandedCall::Place::CompiledFunction);
            AOT_TEST_CHECK(reads.runner.calleeOfEarlierLanding() == getter.address() && reads.runner.thisValueOfEarlierLanding() == receiver.address());
            AOT_TEST_CHECK(run.landedCall.values.callee == method.address() && run.landedCall.values.thisValue == receiver.address());
            AOT_TEST_CHECK(run.landedCall.values.arguments[0] == first && run.landedCall.values.arguments[1] == second);

            bool hasWayWithoutGetter = arm.form == Form::OwnGetterInCell || arm.goesThroughHiddenWord;
            if (!hasWayWithoutGetter)
                continue;
            if (arm.goesThroughHiddenWord)
                receiver.at<uint32_t>(offsetOfExtraProperties) = 0;
            else
                receiver.at<uint64_t>(OrdinaryObjectWithTwoValues::offsetInCell) = 0;
            receiver.storage[0] = method.address();
            reads.clear();
            reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset));
            run = reads.runner.runCall(callMethodAtSite(2), 0, receiver.address(), { first, second }, AnswersAtOnePlace::distance);
            AOT_TEST_CHECK(!run.calledOperation && run.result == StubRunner::resultOfCompiledFunction && run.landedCall.place == LandedCall::Place::CompiledFunction);
            AOT_TEST_CHECK(!reads.runner.calleeOfEarlierLanding() && run.landedCall.values.callee == method.address() && run.landedCall.values.thisValue == receiver.address());
            AOT_TEST_CHECK(run.landedCall.values.arguments[0] == first && run.landedCall.values.arguments[1] == second);
            reads.clear();
        }
    }
}

static void testExtraValuesOfNarrowRecordRunningNoCode()
{
    using Form = SiteAnswers::Form;
    PropertyReads reads;
    AnswersAtOnePlace place(reads.runner);
    Instance& instance = reads.runner.instance();
    constexpr uint32_t offsetOfExtraProperties = 24;
    OrdinaryObjectWithTwoValues receiver(PropertyReads::structureID, 1, 2);
    OrdinaryObjectWithTwoValues extraProperties(PropertyReads::structureIDOfHolder, 3, 4);
    const uint32_t wordsToExtraProperties = static_cast<uint32_t>((extraProperties.address() - instance.cellCageBase) / sizeof(EncodedJSValue));
    instance.propertyCaches.firstWordOfWideAnswers = UINT32_MAX;
    struct Arm {
        Form form;
        int32_t index;
        int32_t expected;
    };
    for (Arm arm : { Arm { Form::ExtraValueInCell, OrdinaryObjectWithTwoValues::offsetInCell / sizeof(EncodedJSValue), 3 }, Arm { Form::ExtraValueInStorage, OrdinaryObjectWithTwoValues::indexInStorage, 4 } }) {
        const uint32_t operand = static_cast<uint32_t>(arm.index) << SiteAnswers::indexShiftInOperandOfExtraValue | offsetOfExtraProperties / sizeof(uint32_t);
        place.answers() = SiteAnswers::forPlacesWithout();
        place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, std::bit_cast<StructureID>(PropertyReads::structureID), arm.form, operand);
        for (bool hasExtraProperties : { true, false }) {
            receiver.at<uint32_t>(offsetOfExtraProperties) = hasExtraProperties ? wordsToExtraProperties : 0;
            for (bool asksShapeFirst : { true, false }) {
                for (ReceiverAtSite receiverIs : { ReceiverAtSite::IsAnyValue, ReceiverAtSite::IsCell }) {
                    StubRun run = reads.runner.run(readAtSiteRunningNoCode(receiverIs, asksShapeFirst), { receiver.address() }, AnswersAtOnePlace::distance);
                    AOT_TEST_CHECK(!run.calledOperation && run.result == (hasExtraProperties ? bitsOf(jsNumber(arm.expected)) : 0));
                }
            }
        }
    }
}

enum class MissAtPlace : uint8_t { OwnValueInCell, OwnValueInStorage, NoProperty, BoxedField, Int32Field };

[[maybe_unused]] static uint32_t instructionsOfNextMissAtPlace(MissAtPlace miss, NameAtSite name)
{
    PropertyReads reads;
    FieldTable table(reads);
    AnswersAtOnePlace place(reads.runner);
    const StructureID structureID = std::bit_cast<StructureID>(PropertyReads::structureID);
    const StructureID otherStructureID = std::bit_cast<StructureID>(PropertyReads::structureIDOfHolder);
    std::array<uint16_t, 1> nameOfSite { PropertyReads::name };
    reads.runner.instance().image.namesOfSitesByDistance = reinterpret_cast<const uint8_t*>(nameOfSite.data()) - AnswersAtOnePlace::distance;
    place.misses() = 0;
    place.answers().first = SiteAnswers::byShape(shapeWordOf(FieldTable::shapeWithOtherField), FakeTypedObject::offsetOfField);
    place.answers().second = *SiteAnswers::tryByStructure(SiteUse::Read, otherStructureID, SiteAnswers::Form::OwnValueInCell, OrdinaryObjectWithTwoValues::offsetInCell);

    OrdinaryObjectWithTwoValues ordinary(PropertyReads::structureID, 1, 2);
    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    withValue.at<uint32_t>(offsetOfOrderWord) = 0;
    withValue.field() = bitsOf(jsNumber(7));
    FakeObjectWithShape withInt32(FieldTable::shapeWithInt32);
    withInt32.at<uint32_t>(offsetOfOrderWord) = 0;
    withInt32.field() = 7;
    uint64_t object = ordinary.address();
    JSValue expected;
    switch (miss) {
    case MissAtPlace::OwnValueInCell:
        reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, 0));
        expected = jsNumber(1);
        break;
    case MissAtPlace::OwnValueInStorage:
        reads.fileInFrontBesideOthers(Slot::createInStorage(structureID, Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset));
        expected = jsNumber(2);
        break;
    case MissAtPlace::NoProperty:
        reads.fileInFrontBesideOthers(Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain));
        expected = jsUndefined();
        break;
    case MissAtPlace::BoxedField:
        object = withValue.address();
        expected = jsNumber(7);
        break;
    case MissAtPlace::Int32Field:
        table.field() = { };
        table.field().name = PropertyReads::name;
        table.field().slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::OptionalInt32);
        object = withInt32.address();
        expected = jsNumber(7);
        break;
    }
    reads.runner.setCountsInstructions(true);
    reads.runner.forgetAccessTable();
    StubRun run { };
    for (unsigned read = 0; read < 2; ++read) {
        run = reads.runner.run(readAtSite(ReceiverAtSite::IsAnyValue, name), { object }, AnswersAtOnePlace::distance);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(expected) && run.instructionsInStubs);
    }
    return *run.instructionsInStubs;
}

template<MissAtPlace miss, uint32_t budget, NameAtSite name = NameAtSite::IsAnyName>
static void testInstructionsOfNextMissAtPlace()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    uint32_t instructions = instructionsOfNextMissAtPlace(miss, name);
    if (instructions != budget)
        dataLogLn("a read at a place that holds the answers of other objects: ", instructions, " instructions, the budget is ", budget);
    AOT_TEST_CHECK(instructions <= budget);
#endif
}

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
            isWithinBudget &= *run.instructionsInStubs <= budget;
#endif
        }
    }
    AOT_TEST_CHECK(isWithinBudget);
}

template<int onlyRow = -1, uint32_t instructionsThatWereEnough = 0>
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
        { "an own value at the place that the site names"_s, ownValue(otherStructureID, inlineOffset), ownValue(structureID, inlineOffset), jsNumber(1), false, 27 },
        { "an own value in the cell at another place"_s, ownValue(otherStructureID, inlineOffset - 1), ownValue(structureID, inlineOffset), jsNumber(1), true, 33 },
        { "an own value in the cell, and the site one out of line"_s, ownValue(otherStructureID, outOfLineOffset), ownValue(structureID, inlineOffset), jsNumber(1), true, 33 },
        { "an own value out of line"_s, ownValue(otherStructureID, outOfLineOffset), ownValue(structureID, outOfLineOffset), jsNumber(2), true, 50 },
        { "no property"_s, ownValue(otherStructureID, inlineOffset), Slot::createWithoutPayload(structureID, Slot::Kind::Absent, Slot::dependsOnChain), jsUndefined(), true, 43 },
    };
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    reads.runner.setCountsInstructions(true);
#endif
    bool isWithinBudget = true;
    for (const Read& read : readsByFrontCell) {
        if (onlyRow >= 0 && &read != &readsByFrontCell[std::max(onlyRow, 0)])
            continue;
        [[maybe_unused]] const uint32_t budget = onlyRow >= 0 ? instructionsThatWereEnough : read.budget;
        Slot site = read.site;
        reads.clear();
        reads.fileInFrontBesideOthers(read.inFront);
        states.fill(SiteCacheState { UINT8_MAX });
        StubRun run = reads.runner.run(getByIdAtSite(), { receiver.address(), std::bit_cast<uint64_t>(&site) }, PropertyReads::name);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected));
        AOT_TEST_CHECK(std::ranges::count_if(states, [](const SiteCacheState& state) { return state.missesLeft != UINT8_MAX; }) == read.countsMiss);
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
        AOT_TEST_CHECK(run.instructionsInStubs);
        if (*run.instructionsInStubs != budget)
            dataLogLn("a read at a site of another Structure, the front table holds ", read.what, ": ", *run.instructionsInStubs, " instructions, the budget is ", budget);
        isWithinBudget &= *run.instructionsInStubs <= budget;
#endif
    }
    AOT_TEST_CHECK(isWithinBudget);
    if (onlyRow >= 0)
        return;

    for (StructureID idAtSite : { structureID, otherStructureID }) {
        Slot site = ownValue(idAtSite, inlineOffset);
        reads.file(ownValue(structureID, outOfLineOffset));
        StubRun run = reads.runner.run(getByIdAtSite(), { receiver.address(), std::bit_cast<uint64_t>(&site) }, PropertyReads::name);
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(idAtSite == structureID ? 1 : 2)));
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
        { "stubs/get-by-id/getters-that-return-captures"_s, ImageTests::whenExecutable<testGettersThatReturnCaptures> },
        { "stubs/get-by-id/inherited-fields"_s, ImageTests::whenExecutable<testInheritedFields> },
        { "stubs/get-by-id/inherited-fields-of-holder-that-is-not-filed"_s, ImageTests::whenExecutable<testInheritedFieldsOfHolderThatIsNotFiled> },
        { "stubs/get-by-val/an-own-getter-at-the-place-hands-back-the-accessor-and-all-else-goes-on-in-the-chain"_s, ImageTests::whenExecutable<testReadByKeyHandsBackOwnAccessor> },
        { "stubs/get-by-val/fields-in-extension-by-place"_s, ImageTests::whenExecutable<testReadByKeyFindsFieldInExtensionByPlace> },
        { "stubs/get-by-id/fields-or-inherited-fields"_s, ImageTests::whenExecutable<testFieldsOrInheritedFields> },
        { "stubs/get-by-id/fields-in-extension-or-inherited-fields"_s, ImageTests::whenExecutable<testFieldsInExtensionOrInheritedFields> },
        { "stubs/get-by-id/names-made-at-run-time"_s, ImageTests::whenExecutable<testNamesMadeAtRunTime> },
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        { "stubs/get-by-id/names-first-used-by-a-stub"_s, ImageTests::whenExecutable<testNamesFirstUsedByStub> },
#endif
        { "stubs/get-by-id/deferred-closures"_s, ImageTests::whenExecutable<testDeferredClosures> },
        { "stubs/get-by-id/typed-array-lengths"_s, ImageTests::whenExecutable<testTypedArrayLengths> },
        { "stubs/length-is/every-kind-of-receiver-against-every-written-integer"_s, ImageTests::whenExecutable<testWhetherLengthIsWrittenInteger> },
        { "stubs/read-length-at-site/a-list-and-a-text-are-answered-in-front-of-the-record"_s, ImageTests::whenExecutable<testLengthReadAtSite> },
        { "stubs/get-by-id/fields-found-in-field-table"_s, ImageTests::whenExecutable<testFieldsFoundInFieldTable> },
        { "stubs/get-by-id/booleans-and-doubles-found-in-field-table"_s, ImageTests::whenExecutable<testBooleansAndDoublesFoundInFieldTable> },
        { "stubs/get-by-id/missing-fields-answered-by-field-table"_s, ImageTests::whenExecutable<testMissingFieldsAnsweredByFieldTable> },
        { "stubs/get-by-id/instructions-of-reads-by-kind-of-field"_s, ImageTests::whenExecutable<testInstructionsOfReadsByKindOfField<>> },
        { "stubs/get-by-id/instructions-of-reads-of-filed-fields"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<>> },
        { "stubs/get-by-id/instructions-of-reads-of-missing-fields-below-classes"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfMissingFieldsBelowClasses<>> },
        { "stubs/get-by-id/lookups-left-to-the-caches"_s, ImageTests::whenExecutable<testLookupsLeftToTheCaches> },
        { "stubs/get-by-id/symbols-of-a-realm"_s, ImageTests::whenExecutable<testSymbolsOfRealm> },
        { "stubs/get-by-id/names-outside-layout-are-absent"_s, ImageTests::whenExecutable<testNamesOutsideLayoutAreAbsent> },
        { "stubs/get-by-id/answers-behind-field-table"_s, ImageTests::whenExecutable<testAnswersBehindFieldTable> },
        { "stubs/get-by-id/reads-that-handlers-answer-leap-over-no-answer"_s, ImageTests::whenExecutable<testReadsThatHandlersAnswerLeapOverNoAnswer> },
        { "stubs/get-by-id/instructions-of-reads-behind-field-table"_s, ImageTests::whenExecutable<testInstructionsOfReadsBehindFieldTable> },
        { "stubs/get-by-id/reads-answered-by-access-table"_s, ImageTests::whenExecutable<testReadsAnsweredByAccessTable> },
        { "stubs/get-by-id/closures-not-created"_s, ImageTests::whenExecutable<testReadsOfClosuresNotCreated> },
#if CPU(X86_64)
        { "stubs/get-by-id-keeping-registers/typed-fields"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testTypedFields>> },
        { "stubs/get-by-id-keeping-registers/fields-or-inherited-values"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testFieldsOrInheritedValues>> },
        { "stubs/get-by-id-keeping-registers/empty-slots-that-may-be-undefined"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testEmptySlotsThatMayBeUndefined>> },
        { "stubs/get-by-id-keeping-registers/fields-in-extension-or-inherited-values"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testFieldsInExtensionOrInheritedValues>> },
        { "stubs/get-by-id-keeping-registers/fields-or-inherited-getters"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testFieldsOrInheritedGetters>> },
        { "stubs/get-by-id-keeping-registers/getters-that-return-captures"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testGettersThatReturnCaptures>> },
        { "stubs/get-by-id-keeping-registers/inherited-fields"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testInheritedFields>> },
        { "stubs/get-by-id-keeping-registers/inherited-fields-of-holder-that-is-not-filed"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testInheritedFieldsOfHolderThatIsNotFiled>> },
        { "stubs/get-by-id-keeping-registers/fields-or-inherited-fields"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testFieldsOrInheritedFields>> },
        { "stubs/get-by-id-keeping-registers/fields-in-extension-or-inherited-fields"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testFieldsInExtensionOrInheritedFields>> },
        { "stubs/get-by-id-keeping-registers/names-made-at-run-time"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testNamesMadeAtRunTime>> },
        { "stubs/get-by-id-keeping-registers/deferred-closures"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testDeferredClosures>> },
        { "stubs/get-by-id-keeping-registers/typed-array-lengths"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testTypedArrayLengths>> },
        { "stubs/get-by-id-keeping-registers/fields-found-in-field-table"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testFieldsFoundInFieldTable>> },
        { "stubs/get-by-id-keeping-registers/booleans-and-doubles-found-in-field-table"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testBooleansAndDoublesFoundInFieldTable>> },
        { "stubs/get-by-id-keeping-registers/missing-fields-answered-by-field-table"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testMissingFieldsAnsweredByFieldTable>> },
        { "stubs/get-by-id-keeping-registers/lookups-left-to-the-caches"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testLookupsLeftToTheCaches>> },
        { "stubs/get-by-id-keeping-registers/symbols-of-a-realm"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testSymbolsOfRealm>> },
        { "stubs/get-by-id-keeping-registers/names-outside-layout-are-absent"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testNamesOutsideLayoutAreAbsent>> },
        { "stubs/get-by-id-keeping-registers/answers-behind-field-table"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testAnswersBehindFieldTable>> },
        { "stubs/get-by-id-keeping-registers/reads-that-handlers-answer-leap-over-no-answer"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testReadsThatHandlersAnswerLeapOverNoAnswer>> },
        { "stubs/get-by-id-keeping-registers/reads-answered-by-access-table"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testReadsAnsweredByAccessTable>> },
        { "stubs/get-by-id-keeping-registers/closures-not-created"_s, ImageTests::whenExecutable<withStubThatKeepsRegisters<testReadsOfClosuresNotCreated>> },
#endif
        { "stubs/get-by-id/reads-at-sites"_s, ImageTests::whenExecutable<testReadsAtSites> },
        { "stubs/get-by-id/hits-at-sites"_s, ImageTests::whenExecutable<testHitsAtSites> },
        { "stubs/get-by-id/sites-of-other-structures"_s, ImageTests::whenExecutable<testSitesOfOtherStructures<>> },
        { "stubs/read-at-site/every-arm-of-an-answer-that-runs-no-code"_s, ImageTests::whenExecutable<testArmsOfAnswersAtPlace> },
        { "stubs/read-at-site/an-empty-word-in-the-cell-of-a-plain-object-is-asked-behind-the-place"_s, ImageTests::whenExecutable<testEmptyWordInCellOfPlainObjectIsAskedBehindPlace> },
        { "stubs/read-at-site/a-structure-filed-without-a-form-is-sent-to-its-row"_s, ImageTests::whenExecutable<testStructureFiledWithoutFormIsSentToItsRow> },
        { "stubs/read-at-site/a-wide-place-asks-its-structures-in-front-of-the-shape"_s, ImageTests::whenExecutable<testWidePlaceAsksItsStructuresInFrontOfShape> },
        { "stubs/read-at-site/whether-a-name-is-there-is-answered-by-the-further-answers"_s, ImageTests::whenExecutable<testWhetherNameIsThereIsAnsweredByFurtherAnswers> },
        { "stubs/read-at-site/a-stub-that-can-meet-no-field-asks-no-shape"_s, ImageTests::whenExecutable<testStubThatCanMeetNoFieldAsksNoShape> },
        { "stubs/read-at-site/places-whose-distances-agree-in-sixteen-bits-share-one-count"_s, ImageTests::whenExecutable<testPlacesWhoseDistancesAgreeInSixteenBitsShareOneCount> },
        { "stubs/read-at-site/instructions/a-boxed-field"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::BoxedField, 11>> },
        { "stubs/read-at-site/instructions/an-integer-field"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::Int32Field, 22>> },
        { "stubs/read-at-site/instructions/a-narrow-reference"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::NarrowReferenceField, 23>> },
        { "stubs/read-at-site/instructions/a-double-field"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::DoubleField, 35>> },
        { "stubs/read-at-site/instructions/an-optional-boolean"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::OptionalBooleanField, 30>> },
        { "stubs/read-at-site/instructions/a-64-bit-integer"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::Int64Field, 34>> },
        { "stubs/read-at-site/instructions/a-field-in-the-extension"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::FieldInExtension, 31>> },
        { "stubs/read-at-site/instructions/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::OwnValueInCell, 14>> },
        { "stubs/read-at-site/instructions/an-own-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::OwnValueInStorage, 15>> },
        { "stubs/read-at-site/instructions/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::ValueInCellOfHolder, 15>> },
        { "stubs/read-at-site/instructions/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::ValueInStorageOfHolder, 17>> },
        { "stubs/read-at-site/instructions/no-property"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::NoProperty, 15>> },
        { "stubs/read-at-site/instructions-where-the-structure-is-asked-first/a-boxed-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlace<HitAtPlace::BoxedField, 18>>> },
        { "stubs/read-at-site/instructions-where-the-structure-is-asked-first/an-integer-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlace<HitAtPlace::Int32Field, 24>>> },
        { "stubs/read-at-site/instructions-where-the-structure-is-asked-first/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlace<HitAtPlace::OwnValueInCell, 11>>> },
        { "stubs/read-at-site/instructions-where-the-structure-is-asked-first/an-own-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlace<HitAtPlace::OwnValueInStorage, 12>>> },
        { "stubs/read-at-site/instructions-where-the-structure-is-asked-first/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlace<HitAtPlace::ValueInCellOfHolder, 12>>> },
        { "stubs/read-at-site/instructions-where-the-structure-is-asked-first/no-property"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlace<HitAtPlace::NoProperty, 12>>> },
        { "stubs/finds-name-at-site/in/instructions/a-boxed-field"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::BoxedField, PresenceQuestion::In, 11>> },
        { "stubs/finds-name-at-site/in/instructions/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInCell, PresenceQuestion::In, 11>> },
        { "stubs/finds-name-at-site/in/instructions/an-own-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInStorage, PresenceQuestion::In, 11>> },
        { "stubs/finds-name-at-site/in/instructions/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInCellOfHolder, PresenceQuestion::In, 19>> },
        { "stubs/finds-name-at-site/in/instructions/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInStorageOfHolder, PresenceQuestion::In, 16>> },
        { "stubs/finds-name-at-site/in/instructions/no-property"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::NoProperty, PresenceQuestion::In, 19>> },
        { "stubs/finds-name-at-site/in/instructions-where-the-structure-is-asked-first/a-boxed-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::BoxedField, PresenceQuestion::In, 16>>> },
        { "stubs/finds-name-at-site/in/instructions-where-the-structure-is-asked-first/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInCell, PresenceQuestion::In, 8>>> },
        { "stubs/finds-name-at-site/in/instructions-where-the-structure-is-asked-first/an-own-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInStorage, PresenceQuestion::In, 8>>> },
        { "stubs/finds-name-at-site/in/instructions-where-the-structure-is-asked-first/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInCellOfHolder, PresenceQuestion::In, 16>>> },
        { "stubs/finds-name-at-site/in/instructions-where-the-structure-is-asked-first/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInStorageOfHolder, PresenceQuestion::In, 13>>> },
        { "stubs/finds-name-at-site/in/instructions-where-the-structure-is-asked-first/no-property"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::NoProperty, PresenceQuestion::In, 16>>> },
        { "stubs/finds-name-at-site/has-own/instructions/a-boxed-field"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::BoxedField, PresenceQuestion::HasOwn, 11>> },
        { "stubs/finds-name-at-site/has-own/instructions/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInCell, PresenceQuestion::HasOwn, 11>> },
        { "stubs/finds-name-at-site/has-own/instructions/an-own-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInStorage, PresenceQuestion::HasOwn, 11>> },
        { "stubs/finds-name-at-site/has-own/instructions/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInCellOfHolder, PresenceQuestion::HasOwn, 17>> },
        { "stubs/finds-name-at-site/has-own/instructions/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInStorageOfHolder, PresenceQuestion::HasOwn, 16>> },
        { "stubs/finds-name-at-site/has-own/instructions/no-property"_s, ImageTests::whenExecutable<testInstructionsOfPresenceAtPlace<HitAtPlace::NoProperty, PresenceQuestion::HasOwn, 17>> },
        { "stubs/finds-name-at-site/has-own/instructions-where-the-structure-is-asked-first/a-boxed-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::BoxedField, PresenceQuestion::HasOwn, 16>>> },
        { "stubs/finds-name-at-site/has-own/instructions-where-the-structure-is-asked-first/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInCell, PresenceQuestion::HasOwn, 8>>> },
        { "stubs/finds-name-at-site/has-own/instructions-where-the-structure-is-asked-first/an-own-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::OwnValueInStorage, PresenceQuestion::HasOwn, 8>>> },
        { "stubs/finds-name-at-site/has-own/instructions-where-the-structure-is-asked-first/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInCellOfHolder, PresenceQuestion::HasOwn, 14>>> },
        { "stubs/finds-name-at-site/has-own/instructions-where-the-structure-is-asked-first/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::ValueInStorageOfHolder, PresenceQuestion::HasOwn, 13>>> },
        { "stubs/finds-name-at-site/has-own/instructions-where-the-structure-is-asked-first/no-property"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfPresenceAtPlace<HitAtPlace::NoProperty, PresenceQuestion::HasOwn, 14>>> },
        { "stubs/read-at-site-running-no-code/instructions/a-boxed-field"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::BoxedField, 11>> },
        { "stubs/read-at-site-running-no-code/instructions/an-integer-field"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::Int32Field, 22>> },
        { "stubs/read-at-site-running-no-code/instructions/a-narrow-reference"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::NarrowReferenceField, 23>> },
        { "stubs/read-at-site-running-no-code/instructions/a-double-field"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::DoubleField, 35>> },
        { "stubs/read-at-site-running-no-code/instructions/an-optional-boolean"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::OptionalBooleanField, 30>> },
        { "stubs/read-at-site-running-no-code/instructions/a-64-bit-integer"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::Int64Field, 34>> },
        { "stubs/read-at-site-running-no-code/instructions/a-field-in-the-extension"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::FieldInExtension, 31>> },
        { "stubs/read-at-site-running-no-code/instructions/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::OwnValueInCell, 14>> },
        { "stubs/read-at-site-running-no-code/instructions/an-own-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::OwnValueInStorage, 15>> },
        { "stubs/read-at-site-running-no-code/instructions/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::ValueInCellOfHolder, 17>> },
        { "stubs/read-at-site-running-no-code/instructions/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::ValueInStorageOfHolder, 18>> },
        { "stubs/read-at-site-running-no-code/instructions/no-property"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::NoProperty, 17>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/a-boxed-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::BoxedField, 16>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/an-integer-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::Int32Field, 22>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/a-narrow-reference"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::NarrowReferenceField, 23>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/a-double-field"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::DoubleField, 35>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/an-optional-boolean"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::OptionalBooleanField, 30>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/a-64-bit-integer"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::Int64Field, 34>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/a-field-in-the-extension"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::FieldInExtension, 31>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::OwnValueInCell, 11>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/an-own-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::OwnValueInStorage, 12>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/an-inherited-value-in-the-cell"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::ValueInCellOfHolder, 14>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/an-inherited-value-out-of-line"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::ValueInStorageOfHolder, 15>>> },
        { "stubs/read-at-site-running-no-code/instructions-where-the-structure-is-asked-first/no-property"_s, ImageTests::whenExecutable<withStubThatAsksStructureFirst<testInstructionsOfHitAtPlaceRunningNoCode<HitAtPlace::NoProperty, 14>>> },
        { "stubs/read-at-site-running-no-code/a-miss-is-counted-and-is-no-hit"_s, ImageTests::whenExecutable<testMissAtPlaceRunningNoCode> },
        { "stubs/read-at-site-running-no-code/the-front-of-the-tables-answers-where-the-place-holds-another-structure"_s, ImageTests::whenExecutable<testFrontOfTablesAnswersWherePlaceHoldsAnotherStructureRunningNoCode> },
        { "stubs/read-at-site-running-no-code/the-forms-that-run-code-or-ask-more-are-no-hits"_s, ImageTests::whenExecutable<testFormsThatAreNoHitsRunningNoCode> },
        { "stubs/read-at-site/a-field-in-the-extension-by-structure-and-both-ways-out"_s, ImageTests::whenExecutable<testFieldInExtensionByStructure> },
        { "stubs/read-at-site-running-no-code/an-extra-value-of-a-narrow-record-is-read"_s, ImageTests::whenExecutable<testExtraValuesOfNarrowRecordRunningNoCode> },
        { "stubs/call-method-at-site/a-getter-in-each-of-six-places-is-called-and-what-it-gives-is-called-on-the-receiver"_s, ImageTests::whenExecutable<testGettersAnswerCallsByNameAtSite> },
        { "stubs/read-at-site/instructions-of-misses/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::OwnValueInCell, 44>> },
        { "stubs/read-at-site/instructions-of-misses/an-own-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::OwnValueInStorage, 51>> },
        { "stubs/read-at-site/instructions-of-misses/no-property"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::NoProperty, 45>> },
        { "stubs/read-at-site/instructions-of-misses/a-boxed-field"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::BoxedField, 40>> },
        { "stubs/read-at-site/instructions-of-misses/an-integer-field"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::Int32Field, 44>> },
        { "stubs/read-at-site/instructions-of-misses-under-a-name-with-a-column/an-own-value-in-the-cell"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::OwnValueInCell, 42, NameAtSite::HasColumn>> },
        { "stubs/read-at-site/instructions-of-misses-under-a-name-with-a-column/an-own-value-out-of-line"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::OwnValueInStorage, 49, NameAtSite::HasColumn>> },
        { "stubs/read-at-site/instructions-of-misses-under-a-name-with-a-column/no-property"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::NoProperty, 43, NameAtSite::HasColumn>> },
        { "stubs/read-at-site/instructions-of-misses-under-a-name-with-a-column/a-boxed-field"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::BoxedField, 38, NameAtSite::HasColumn>> },
        { "stubs/read-at-site/instructions-of-misses-under-a-name-with-a-column/an-integer-field"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::Int32Field, 42, NameAtSite::HasColumn>> },
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
        { "stubs/read-at-site/instructions-that-were-enough/an-integer-field-takes-22-not-21"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::Int32Field, 21>>, "reads by name: 22 instructions where the compiler before this one took 21"_s },
        { "stubs/read-at-site/instructions-that-were-enough/a-double-field-takes-35-not-26"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::DoubleField, 26>>, "reads by name: 35 instructions where the compiler before this one took 26"_s },
        { "stubs/read-at-site/instructions-that-were-enough/an-own-value-in-the-cell-takes-14-not-10"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::OwnValueInCell, 10>>, "reads by name: 14 instructions where the compiler before this one took 10"_s },
        { "stubs/read-at-site/instructions-that-were-enough/an-inherited-value-in-the-cell-takes-15-not-14"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::ValueInCellOfHolder, 14>>, "reads by name: 15 instructions where the compiler before this one took 14"_s },
        { "stubs/read-at-site/instructions-that-were-enough/an-inherited-value-out-of-line-takes-17-not-14"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::ValueInStorageOfHolder, 14>>, "reads by name: 17 instructions where the compiler before this one took 14"_s },
        { "stubs/read-at-site/instructions-that-were-enough/no-property-takes-15-not-12"_s, ImageTests::whenExecutable<testInstructionsOfHitAtPlace<HitAtPlace::NoProperty, 12>>, "reads by name: 15 instructions where the compiler before this one took 12"_s },
        { "stubs/read-at-site/instructions-that-were-enough/a-miss-that-finds-an-own-value-in-the-cell-takes-43-not-28"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::OwnValueInCell, 28>>, "reads by name: 43 instructions where the compiler before this one took 28"_s },
        { "stubs/read-at-site/instructions-that-were-enough/a-miss-that-finds-an-own-value-out-of-line-takes-50-not-39"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::OwnValueInStorage, 39>>, "reads by name: 50 instructions where the compiler before this one took 39"_s },
        { "stubs/read-at-site/instructions-that-were-enough/a-miss-that-finds-no-property-takes-44-not-26"_s, ImageTests::whenExecutable<testInstructionsOfNextMissAtPlace<MissAtPlace::NoProperty, 26>>, "reads by name: 44 instructions where the compiler before this one took 26"_s },
        { "stubs/get-by-id/instructions-that-were-enough/the-next-read-of-a-boolean-takes-32-not-30"_s, ImageTests::whenExecutable<testInstructionsOfReadsByKindOfField<4, 30>>, "reads by name: 32 instructions where the compiler before this one took 30"_s },
        { "stubs/get-by-id/instructions-that-were-enough/the-next-read-of-a-double-takes-37-not-26"_s, ImageTests::whenExecutable<testInstructionsOfReadsByKindOfField<5, 26>>, "reads by name: 37 instructions where the compiler before this one took 26"_s },
        { "stubs/get-by-id/instructions-that-were-enough/the-next-read-of-a-double-that-is-missing-takes-91-not-80"_s, ImageTests::whenExecutable<testInstructionsOfReadsByKindOfField<11, 80>>, "reads by name: 91 instructions where the compiler before this one took 80"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-a-boxed-value-takes-62-not-50"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<0, 50>>, "reads by name: 62 instructions where the compiler before this one took 50"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-a-reference-takes-62-not-50"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<1, 50>>, "reads by name: 62 instructions where the compiler before this one took 50"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-a-narrow-reference-takes-64-not-52"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<2, 52>>, "reads by name: 64 instructions where the compiler before this one took 52"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-an-optional-narrow-reference-takes-64-not-52"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<3, 52>>, "reads by name: 64 instructions where the compiler before this one took 52"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-an-optional-integer-takes-67-not-55"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<4, 55>>, "reads by name: 67 instructions where the compiler before this one took 55"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-an-integer-takes-67-not-61"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<5, 61>>, "reads by name: 67 instructions where the compiler before this one took 61"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-a-double-takes-80-not-69"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<6, 69>>, "reads by name: 80 instructions where the compiler before this one took 69"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-an-optional-double-takes-77-not-69"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<7, 69>>, "reads by name: 77 instructions where the compiler before this one took 69"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-an-optional-double-that-is-whole-takes-77-not-69"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<8, 69>>, "reads by name: 77 instructions where the compiler before this one took 69"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-a-boolean-takes-72-not-61"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<9, 61>>, "reads by name: 72 instructions where the compiler before this one took 61"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-read-of-a-filed-field-an-optional-boolean-takes-72-not-62"_s, ImageTests::whenExecutable<testInstructionsOfReadsOfFiledFields<10, 62>>, "reads by name: 72 instructions where the compiler before this one took 62"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-site-of-another-structure-where-the-front-table-holds-an-own-value-at-the-place-that-the-site-names-takes-27-not-22"_s, ImageTests::whenExecutable<testSitesOfOtherStructures<0, 22>>, "reads by name: 27 instructions where the compiler before this one took 22"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-site-of-another-structure-where-the-front-table-holds-an-own-value-in-the-cell-at-another-place-takes-33-not-28"_s, ImageTests::whenExecutable<testSitesOfOtherStructures<1, 28>>, "reads by name: 33 instructions where the compiler before this one took 28"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-site-of-another-structure-where-the-front-table-holds-an-own-value-in-the-cell-and-the-site-one-out-of-line-takes-33-not-28"_s, ImageTests::whenExecutable<testSitesOfOtherStructures<2, 28>>, "reads by name: 33 instructions where the compiler before this one took 28"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-site-of-another-structure-where-the-front-table-holds-an-own-value-out-of-line-takes-50-not-39"_s, ImageTests::whenExecutable<testSitesOfOtherStructures<3, 39>>, "reads by name: 50 instructions where the compiler before this one took 39"_s },
        { "stubs/get-by-id/instructions-that-were-enough/a-site-of-another-structure-where-the-front-table-holds-no-property-takes-43-not-26"_s, ImageTests::whenExecutable<testSitesOfOtherStructures<4, 26>>, "reads by name: 43 instructions where the compiler before this one took 26"_s },
#endif
        { "stubs/in-by-val/presence-from-leaf-lookup"_s, ImageTests::whenExecutable<testPresenceFromLeafLookup> },
        { "stubs/in-by-val/own-presence-of-fields-that-objects-above-answer-for"_s, ImageTests::whenExecutable<testOwnPresenceOfFieldsThatObjectsAboveAnswerFor> },
        { "stubs/instance-of/ordinary-constructors-are-remembered"_s, ImageTests::whenExecutable<testInstanceOfRemembersOrdinaryConstructors> },
        { "stubs/read-field-in-extension/granules-widths-and-registers"_s, ImageTests::whenExecutable<testFieldsReadOutOfExtension> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
