/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTPropertyCachesWords.h"
#include "AOTFieldTable.h"
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
        : PropertyReads({ }, { })
    {
    }

    // For the stubs that are bound to names.
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

    // The entries stand from the first place of the number on.
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
    // With what earlier reads have filed in the access table.
    StubRun readAgain(uint64_t receiver, uint16_t otherName = name) { return runner.run(getById(), { receiver }, otherName); }

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

// A number may fall. What differs from its budget is printed.
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

    // The operation makes the closure. It gets every operand unchanged.
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
        inStructure<const void*>(CompiledStructure::offsetOfLayout()) = m_layout.data();
        instance.prefix().structureIDBase = m_structures.address();
        instance.propertyCaches.namesOfObjectPrototypes = &m_namesOfObjectPrototypes;
    }

    void setFlagsOfStructure(OptionSet<CompiledStructure::Flag> flags) { inStructure<uint8_t>(CompiledStructure::offsetOfFlags()) = flags.toRaw(); }
    void setRealmOfStructure(uint64_t realm) { inStructure<uint64_t>(Structure::realmOffset()) = realm; }
    void setBitFieldOfStructure(uint32_t bits) { inStructure<uint32_t>(Structure::bitFieldOffset()) = bits; }
    // FakeHolder has the StructureID zero.
    void setBitFieldOfStructureOfHolders(uint32_t bits) { m_structures.at<uint32_t>(Structure::bitFieldOffset()) = bits; }
    void setNamesOfObjectPrototypes(uint64_t bits) { m_namesOfObjectPrototypes = bits; }
    FieldData& field() { return *std::bit_cast<FieldData*>(m_layout.data() + sizeof(LayoutData)); }

private:
    static_assert(PropertyReads::numberOfNames <= 64);
    std::array<uint32_t, highestShape + highestShape + PropertyReads::numberOfNames> m_table { };
    std::array<uint16_t, highestShape + PropertyReads::numberOfNames> m_recordIndices { };
    template<typename T> T& inStructure(ptrdiff_t offset) { return m_structures.at<T>(PropertyReads::structureID + offset); }

    alignas(LayoutData) std::array<uint8_t, sizeof(LayoutData) + sizeof(FieldData)> m_layout { };
    // The Structure of the fake compiled objects lies as far in as its StructureID says. A smaller one leads to zeros: a Structure that says nothing.
    FakeObject<PropertyReads::structureID + sizeof(CompiledStructure)> m_structures;
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
        // Of the read that finds nothing in the access table and files there what it can, and of the read behind it.
        uint32_t budget;
        uint32_t budgetOfNextRead;
    };
    auto absent = [](SlotRepresentation representation) { return traitsOf(representation).absentBits; };
    const Read readsByKind[] = {
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, bitsOf(jsNumber(7)), jsNumber(7), 40, 19 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 7, jsNumber(7), 45, 32 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address())), JSValue(std::bit_cast<JSCell*>(cell.address())), 48, 35 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, JSValue::ValueTrue, jsBoolean(true), 55, 42 },
        { "a boolean"_s, FieldTable::shapeWithBoolean, SlotRepresentation::Boolean, 1, jsBoolean(true), 54, 41 },
        { "a double"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, std::bit_cast<uint64_t>(0.5), jsDoubleNumber(0.5), 50, 37 },
        { "a 64-bit integer"_s, FieldTable::shapeWithInt64, SlotRepresentation::OptionalInt64, 7, jsNumber(7), 59, 46 },
        { "a boxed value that is missing"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, absent(SlotRepresentation::JSValue), jsUndefined(), 44, 44 },
        { "an optional boolean that is missing"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, absent(SlotRepresentation::OptionalBoolean), jsUndefined(), 70, 57 },
        { "an integer that is missing"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, absent(SlotRepresentation::OptionalInt32), jsUndefined(), 90, 77 },
        { "a narrow reference that is missing"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, absent(SlotRepresentation::OptionalCell32), jsUndefined(), 98, 85 },
        { "a double that is missing"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, absent(SlotRepresentation::OptionalDouble), jsUndefined(), 104, 91 },
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
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected));
        isWithinBudget &= isWithinBudgetOfRead(run, read.budget, firstReadByName, read.what);
        run = reads.readAgain(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.expected));
        isWithinBudget &= isWithinBudgetOfRead(run, read.budgetOfNextRead, nextReadByName, read.what);
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
        { SlotRepresentation::JSValue, jsNumber(7), 61 },
        { SlotRepresentation::Cell, cellValue, 61 },
        { SlotRepresentation::Cell32, cellValue, 63 },
        { SlotRepresentation::OptionalCell32, cellValue, 63 },
        { SlotRepresentation::OptionalInt32, jsNumber(7), 66 },
        { SlotRepresentation::Int32, jsNumber(7), 66 },
        { SlotRepresentation::Double, jsDoubleNumber(0.5), 79 },
        { SlotRepresentation::OptionalDouble, jsDoubleNumber(0.5), 76 },
        { SlotRepresentation::OptionalDouble, jsNumber(7), 76 },
        { SlotRepresentation::Boolean, jsBoolean(true), 71 },
        { SlotRepresentation::OptionalBoolean, jsBoolean(true), 73 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByRepresentation) {
        reads.file(createFieldSlot(read.representation));
        FakeTypedObject object;
        AOT_TEST_CHECK(tryWrite(read.representation, &object.field(), read.value.isInt32() && traitsOf(read.representation).valueKind == SlotValueKind::Double ? jsDoubleNumber(read.value.asInt32()) : read.value, cageBase));
        // The access table holds nothing that a handler answers, so every read costs this.
        for (StubRun run : { reads.read(object.address()), reads.readAgain(object.address()) }) {
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.value));
            isWithinBudget &= isWithinBudgetOfRead(run, read.budget, "a read by name of a filed field, "_s, nameOf(read.representation));
        }
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
        uint32_t budgetOfNextRead;
    };
    const Read readsByKind[] = {
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, 49, 49 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, 75, 62 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 95, 82 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, 103, 90 },
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
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        isWithinBudget &= isWithinBudgetOfRead(run, read.budget, firstReadByName, read.what, " that is missing below a class");
        run = reads.readAgain(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
        isWithinBudget &= isWithinBudgetOfRead(run, read.budgetOfNextRead, nextReadByName, read.what, " that is missing below a class");

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

// Each realm has its own symbol under the number. An object of another realm has the field of that number under another symbol than the one that is asked for.
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
    // What is filed under the symbol of the realm that runs answers for any receiver.
    reads.file(absent);
    run = read(object.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    // The realm that runs has not made its symbol yet. The number names no atom of the image.
    reads.setSymbols({ });
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    reads.setSymbols({ { PropertyReads::name, reads.uid() } }, PropertyReads::name);

    // A cell in front goes by the number, for every realm at once. It is not asked for an object of another realm.
    reads.clear();
    reads.fileInFrontBesideOthers(createFieldSlot(SlotRepresentation::JSValue));
    AOT_TEST_CHECK(read(object.address()).calledOperation);
    reads.clear();

    // An empty slot of an object of the realm that runs is explained as for getById().
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

    // It says nothing about the chain, and the hashed table does.
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
        // The question stands behind the hashed table, which a name that has no uid yet does not get to.
        if (otherName == inheritedName || otherName == PropertyReads::nameMadeAtRunTime)
            AOT_TEST_CHECK(run.calledOperation);
        else
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsUndefined()));
    });

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

// A variant takes the receiver in another register and answers what the stub itself answers, whoever has the answer.
static void testReceiversInOtherRegisters()
{
    PropertyReads reads;
    FieldTable table(reads);
    table.setFlagsOfStructure(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    AOT_TEST_CHECK(alternateReceiverGPRs().empty() == !isX86_64());
    for (GPRReg reg : alternateReceiverGPRs()) {
        const StubKey variant = getById().withOperandRegister(reg);
        auto answerFor = [&](uint64_t receiver) {
            reads.runner.forgetAccessTable();
            StubRun run = reads.runner.run(variant, { receiver }, PropertyReads::name);
            AOT_TEST_CHECK(!run.calledOperation);
            return run.result;
        };

        reads.clear();
        FakeObjectWithShape withField(FieldTable::shapeWithValue);
        withField.field() = bitsOf(jsNumber(7));
        AOT_TEST_CHECK(answerFor(withField.address()) == bitsOf(jsNumber(7)));
        // Whoever explains an empty slot takes the receiver where the stub itself does.
        withField.field() = traitsOf(SlotRepresentation::JSValue).absentBits;
        AOT_TEST_CHECK(answerFor(withField.address()) == bitsOf(jsUndefined()));

        FakeTypedObject withoutShape;
        withoutShape.field() = bitsOf(jsNumber(8));
        reads.fileInFrontBesideOthers(createFieldSlot(SlotRepresentation::JSValue));
        AOT_TEST_CHECK(answerFor(withoutShape.address()) == bitsOf(jsNumber(8)));
        reads.file(createFieldSlot(SlotRepresentation::JSValue));
        AOT_TEST_CHECK(answerFor(withoutShape.address()) == bitsOf(jsNumber(8)));

        reads.clear();
        reads.runner.recordCallsOf(Entry::operationAOTGetByIdAndCache);
        for (uint64_t receiver : { withoutShape.address(), bitsOf(jsNumber(9)) }) {
            reads.runner.forgetAccessTable();
            StubRun run = reads.runner.run(variant, { receiver }, PropertyReads::name);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == receiver && static_cast<uint32_t>(run.recordedArguments[1]) == PropertyReads::name);
        }
        reads.runner.countCallsOf(Entry::operationAOTGetByIdAndCache);
    }
}

namespace {

// No row of FieldTable has an entry for it.
constexpr uint16_t nameOutsideLayouts = 2;
static_assert(nameOutsideLayouts != PropertyReads::name && nameOutsideLayouts != PropertyReads::nameMadeAtRunTime && nameOutsideLayouts < PropertyReads::numberOfNames);
constexpr uint32_t offsetOfExtraPropertiesWord = FakeTypedObject::offsetOfField + sizeof(uint64_t);

struct AnswerBehindFieldTable {
    ASCIILiteral what;
    Slot slot;
    uint64_t expected;
    // Of getById() for a compiled object, on x86-64: the read that finds nothing in the access table and files there what it can, and the read behind it.
    uint32_t budget;
    uint32_t budgetOfNextRead;
};

} // anonymous namespace

// `other` stands for the extra properties of the receiver, or for a holder above it.
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
        { "an extra property in the cell"_s, extraProperty(FakeHolder::inlineOffset), other.property(FakeHolder::inlineOffset), 38, 38 },
        { "an extra property out of line"_s, extraProperty(FakeHolder::outOfLineOffset), other.property(FakeHolder::outOfLineOffset), 47, 47 },
        { "a value in the cell of a holder"_s, valueOfHolder(FakeHolder::inlineOffset), other.property(FakeHolder::inlineOffset), 69, 20 },
        { "a value out of line in a holder"_s, valueOfHolder(FakeHolder::outOfLineOffset), other.property(FakeHolder::outOfLineOffset), 82, 30 },
        { "a name that nothing has"_s, Slot::createWithoutPayload(id, Slot::Kind::Absent, Slot::dependsOnChain), bitsOf(jsUndefined()), 63, 20 },
    } };
}

// What most cells of the front table say is answered where the cell is found, wherever the receiver lies and whether or not it is a compiled object.
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

    Vector<StubKey> stubs { getById() };
    for (GPRReg reg : alternateReceiverGPRs())
        stubs.append(getById().withOperandRegister(reg));
    for (StubKey stub : stubs) {
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

                // An object without extra properties is left to the run time, and nothing else looks at the word.
                extraPropertiesWord = 0;
                run = read();
                if (answer.slot.kind == Slot::Kind::ExtraProperty)
                    AOT_TEST_CHECK(run.calledOperation);
                else
                    AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
            }

            // A cell of any other kind goes on to the handler of its kind.
            reads.clear();
            reads.fileInFrontBesideOthers(createFieldSlot(SlotRepresentation::JSValue), nameOutsideLayouts);
            object->field() = bitsOf(jsNumber(7));
            StubRun run = read();
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));

            // A cell of another name says nothing.
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

// A number here may fall.
static void testInstructionsOfReadsBehindFieldTable()
{
    // A build with assertions puts more instructions into the stubs, so the budget is held against the other builds only.
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
        isWithinBudget &= isWithinBudgetOfRead(run, answer.budget, firstReadByName, answer.what);
        run = reads.readAgain(object.address(), nameOutsideLayouts);
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
        isWithinBudget &= isWithinBudgetOfRead(run, answer.budgetOfNextRead, nextReadByName, answer.what);
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

// What a read has found it files in the access table, under the StructureID of the receiver and the name, and the next read of an object of the Structure is answered there.
// A fake object keeps its StructureID whatever its shape is, so who has answered shows where the tables behind say something else, or nothing.
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

    // A value in the cell of a compiled object.
    FakeObjectWithShape withValue(FieldTable::shapeWithValue);
    withValue.field() = bitsOf(jsNumber(7));
    StubRun run = reads.read(withValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    AOT_TEST_CHECK(placeOf(PropertyReads::name).keyOfOwnValue == keyOf(PropertyReads::name) && placeOf(PropertyReads::name).offsetInCell == FakeTypedObject::offsetOfField && !placeOf(PropertyReads::name).keyOfOtherAnswer);
    AOT_TEST_CHECK(!words.numberOfKeysThatDependOnChains);
    FakeObjectWithShape withRowOfOtherForm(FieldTable::shapeWithOtherField);
    withRowOfOtherForm.field() = bitsOf(jsNumber(8));
    run = reads.readAgain(withRowOfOtherForm.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(8)));
    // An empty slot says that this object lacks the property. Another name finds another key.
    withRowOfOtherForm.field() = 0;
    AOT_TEST_CHECK(reads.readAgain(withRowOfOtherForm.address()).calledOperation);
    AOT_TEST_CHECK(reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);
    // A change on a chain does not end it, and a collection does.
    words.numberOfKeysThatDependOnChains = AccessTable::maxKeysListed + 1;
    words.didChangeObjectOnSomeChain();
    AOT_TEST_CHECK(placeOf(PropertyReads::name).keyOfOwnValue == keyOf(PropertyReads::name));
    words.forgetEverything();
    AOT_TEST_CHECK(!placeOf(PropertyReads::name).keyOfOwnValue);

    // A field of any other form: the place has the form and the operand, and the read goes on where the row would have led it.
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
        run = reads.read(object.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(field.expected));
        const AccessTable::Read& place = placeOf(PropertyReads::name);
        AOT_TEST_CHECK(!place.keyOfOwnValue && place.keyOfOtherAnswer == keyOf(PropertyReads::name));
        AOT_TEST_CHECK((place.otherAnswer & ((1u << AccessTable::answerShift) - 1)) == AccessTable::answerHasForm && !words.numberOfKeysThatDependOnChains);
        FakeObjectWithShape withRowOfValue(FieldTable::shapeWithValue);
        withRowOfValue.field() = field.slot;
        run = reads.readAgain(withRowOfValue.address());
        AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(field.expected));
    }

    // What a cell of the front table says of the objects above. Whatever a handler answers is not filed.
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
            // The value of a holder is replaced without a report.
            uint64_t& property = holder.property(answer.slot.has(Slot::isOutOfLine) ? FakeHolder::outOfLineOffset : FakeHolder::inlineOffset);
            uint64_t before = std::exchange(property, bitsOf(jsNumber(13)));
            AOT_TEST_CHECK(reads.readAgain(withValue.address(), nameOutsideLayouts).result == bitsOf(jsNumber(13)));
            property = before;
        }
        AOT_TEST_CHECK(words.keysThatDependOnChains[0] == &placeOf(nameOutsideLayouts).keyOfOtherAnswer);
        words.didChangeObjectOnSomeChain();
        AOT_TEST_CHECK(!words.numberOfKeysThatDependOnChains && reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);

        // Where more were filed than are listed, the count says so, and a change ends them all the same.
        reads.runner.forgetAccessTable();
        words.numberOfKeysThatDependOnChains = AccessTable::maxKeysListed;
        reads.fileInFrontBesideOthers(answer.slot, nameOutsideLayouts);
        run = reads.readAgain(withValue.address(), nameOutsideLayouts);
        reads.clear();
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected && words.numberOfKeysThatDependOnChains == AccessTable::maxKeysListed + 1);
        AOT_TEST_CHECK(!reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation && words.numberOfKeysThatDependOnChains == AccessTable::maxKeysListed + 1);
        words.didChangeObjectOnSomeChain();
        AOT_TEST_CHECK(!words.numberOfKeysThatDependOnChains && reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);

        // A dictionary changes and stays the Structure that it is, and making one flat moves its values. So nothing is filed for one, nor about a holder that is one.
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

    // A value of an ordinary object's own.
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
    }
}

namespace {

// The record of the layout behind FieldTable::shapeWithNarrowCell. Its second field is PropertyReads::name, which defers a closure.
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
            .operandIndex = 0,
            .slot = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::DeferredClosure),
            .type = 0,
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

    // The read of a closure that is not made yet asks for nothing but the closure, whatever the closure captures.
    slot = uncreatedDeferredClosure;
    for (uint16_t flags : { static_cast<uint16_t>(0), fieldDeferredClosureCapturesObject, fieldDeferredClosureCapturesEnvironment }) {
        layout.field().flags = flags;
        checkRead(Entry::operationAOTMaterializeDeferredClosure);
    }

    slot = absentDeferredClosure;
    checkRead(Entry::operationAOTGetByIdAndCache);

    // Zero in the slot of any other reference says that the slot is empty.
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

// A read that finds the slot of the field empty goes on above the receiver: the receiver itself has no such property.
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

    // Only the key order says whether such a field exists where its slot is empty.
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
    // No two bytes of a granule are alike, no two granules are alike, and none has a byte of zero.
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

        // An Extension that holds nothing also stands for the page of zeros that the narrow word of an object without an Extension names: that page belongs to a real cage.
        constexpr std::array<uint64_t, 6> setsOfGranules { 0, UINT64_MAX, 1, lastGranule, 1 | 1ull << 3 | lastGranule, 1ull << 1 | 1ull << 31 | 1ull << 32 | 1ull << 62 };
        for (uint64_t granuleBits : setsOfGranules) {
            hold(granuleBits);
            for (unsigned granule = 0; granule < maxExtensionGranules; ++granule) {
                for (SlotInGranule slot : slots)
                    AOT_TEST_CHECK(read(readFieldInExtension(granule, slot)) == (granuleBits >> granule & 1 ? bytesOf(granule, slot) : 0));
            }
        }

        hold(1 | 1ull << 3 | lastGranule);
        for (GPRReg reg : alternateReceiverGPRs()) {
            for (unsigned granule : { 0u, 2u, 3u, maxExtensionGranules - 1 }) {
                for (SlotInGranule slot : { SlotInGranule::Word64, SlotInGranule::Word32At4, SlotInGranule::ByteAt5 })
                    AOT_TEST_CHECK(read(readFieldInExtension(granule, slot).withOperandRegister(reg)) == (granule == 2 ? 0 : bytesOf(granule, slot)));
            }
        }

        if (usesWideReferences) {
            object.at<uint64_t>(offsetOfFirstField) = 0;
            for (unsigned granule : { 0u, 1u, maxExtensionGranules - 1 }) {
                for (SlotInGranule slot : slots)
                    AOT_TEST_CHECK(!read(readFieldInExtension(granule, slot)));
            }
        }

        // The caller keeps every register but that of the result, calls it without a frame where the call stores the return address, and none of its loads ends.
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
        { "stubs/get-by-id/symbols-of-a-realm"_s, ImageTests::whenExecutable<testSymbolsOfRealm> },
        { "stubs/get-by-id/names-outside-layout-are-absent"_s, ImageTests::whenExecutable<testNamesOutsideLayoutAreAbsent> },
        { "stubs/get-by-id/receivers-in-other-registers"_s, ImageTests::whenExecutable<testReceiversInOtherRegisters> },
        { "stubs/get-by-id/answers-behind-field-table"_s, ImageTests::whenExecutable<testAnswersBehindFieldTable> },
        { "stubs/get-by-id/instructions-of-reads-behind-field-table"_s, ImageTests::whenExecutable<testInstructionsOfReadsBehindFieldTable> },
        { "stubs/get-by-id/reads-answered-by-access-table"_s, ImageTests::whenExecutable<testReadsAnsweredByAccessTable> },
        { "stubs/get-by-id/closures-not-created"_s, ImageTests::whenExecutable<testReadsOfClosuresNotCreated> },
        { "stubs/in-by-val/presence-from-leaf-lookup"_s, ImageTests::whenExecutable<testPresenceFromLeafLookup> },
        { "stubs/in-by-val/own-presence-of-fields-that-objects-above-answer-for"_s, ImageTests::whenExecutable<testOwnPresenceOfFieldsThatObjectsAboveAnswerFor> },
        { "stubs/read-field-in-extension/granules-widths-and-registers"_s, ImageTests::whenExecutable<testFieldsReadOutOfExtension> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
