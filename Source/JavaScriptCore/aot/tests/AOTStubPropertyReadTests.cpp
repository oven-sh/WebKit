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
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, bitsOf(jsNumber(7)), jsNumber(7), 40, 19 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 7, jsNumber(7), 49, 36 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(cell.address())), JSValue(std::bit_cast<JSCell*>(cell.address())), 51, 38 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, JSValue::ValueTrue, jsBoolean(true), 56, 43 },
        { "a boolean"_s, FieldTable::shapeWithBoolean, SlotRepresentation::Boolean, 1, jsBoolean(true), 57, 44 },
        { "a double"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, std::bit_cast<uint64_t>(0.5), jsDoubleNumber(0.5), 62, 49 },
        { "a 64-bit integer"_s, FieldTable::shapeWithInt64, SlotRepresentation::OptionalInt64, 7, jsNumber(7), 60, 47 },
        { "a boxed value that is missing"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, absent(SlotRepresentation::JSValue), jsUndefined(), 44, 44 },
        { "an optional boolean that is missing"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, absent(SlotRepresentation::OptionalBoolean), jsUndefined(), 71, 58 },
        { "an integer that is missing"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, absent(SlotRepresentation::OptionalInt32), jsUndefined(), 94, 81 },
        { "a narrow reference that is missing"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, absent(SlotRepresentation::OptionalCell32), jsUndefined(), 101, 88 },
        { "a double that is missing"_s, FieldTable::shapeWithDouble, SlotRepresentation::OptionalDouble, absent(SlotRepresentation::OptionalDouble), jsUndefined(), 116, 103 },
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

    FakeExtension extension(FakeExtension::bitOfSlot);
    extension.granule() = bitsOf(jsNumber(7));
    FakeObjectWithShape withOutOfLineValue(FieldTable::shapeWithOutOfLineValue);
    withOutOfLineValue.at<uint32_t>(offsetOfOrderWord) = 0;
    withOutOfLineValue.field() = extension.encode(cageBase);
    StubRun run = reads.read(withOutOfLineValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    isWithinBudget &= isWithinBudgetOfRead(run, 56, firstReadByName, "a boxed value out of line"_s);
    run = reads.readAgain(withOutOfLineValue.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(7)));
    isWithinBudget &= isWithinBudgetOfRead(run, 43, nextReadByName, "a boxed value out of line"_s);
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

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
        { SlotRepresentation::OptionalBoolean, jsBoolean(true), 75 },
    };
    bool isWithinBudget = true;
    for (const Read& read : readsByRepresentation) {
        reads.file(createFieldSlot(read.representation));
        FakeTypedObject object;
        AOT_TEST_CHECK(tryWrite(read.representation, &object.field(), read.value.isInt32() && traitsOf(read.representation).valueKind == SlotValueKind::Double ? jsDoubleNumber(read.value.asInt32()) : read.value, cageBase));
        for (StubRun run : { reads.read(object.address()), reads.readAgain(object.address()) }) {
            AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(read.value));
            isWithinBudget &= isWithinBudgetOfRead(run, read.budget, "a read by name of a filed field, "_s, nameOf(read.representation));
        }
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

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
        { "a boxed value"_s, FieldTable::shapeWithValue, SlotRepresentation::JSValue, 49, 49 },
        { "an optional boolean"_s, FieldTable::shapeWithEncodedByte, SlotRepresentation::OptionalBoolean, 76, 63 },
        { "an integer"_s, FieldTable::shapeWithInt32, SlotRepresentation::OptionalInt32, 99, 86 },
        { "a narrow reference"_s, FieldTable::shapeWithNarrowCell, SlotRepresentation::OptionalCell32, 106, 93 },
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
        { "an extra property in the cell"_s, extraProperty(FakeHolder::inlineOffset), other.property(FakeHolder::inlineOffset), 38, 38 },
        { "an extra property out of line"_s, extraProperty(FakeHolder::outOfLineOffset), other.property(FakeHolder::outOfLineOffset), 47, 47 },
        { "a value in the cell of a holder"_s, valueOfHolder(FakeHolder::inlineOffset), other.property(FakeHolder::inlineOffset), 69, 20 },
        { "a value out of line in a holder"_s, valueOfHolder(FakeHolder::outOfLineOffset), other.property(FakeHolder::outOfLineOffset), 82, 30 },
        { "a name that nothing has"_s, Slot::createWithoutPayload(id, Slot::Kind::Absent, Slot::dependsOnChain), bitsOf(jsUndefined()), 63, 20 },
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
        isWithinBudget &= isWithinBudgetOfRead(run, answer.budget, firstReadByName, answer.what);
        run = reads.readAgain(object.address(), nameOutsideLayouts);
        AOT_TEST_CHECK(!run.calledOperation && run.result == answer.expected);
        isWithinBudget &= isWithinBudgetOfRead(run, answer.budgetOfNextRead, nextReadByName, answer.what);
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
    AOT_TEST_CHECK(placeOf(PropertyReads::name).keyOfOwnValue == keyOf(PropertyReads::name) && placeOf(PropertyReads::name).offsetInCell == FakeTypedObject::offsetOfField && !placeOf(PropertyReads::name).keyOfOtherAnswer);
    AOT_TEST_CHECK(!words.numberOfKeysThatDependOnChains);
    FakeObjectWithShape withRowOfOtherForm(FieldTable::shapeWithOtherField);
    withRowOfOtherForm.field() = bitsOf(jsNumber(8));
    run = reads.readAgain(withRowOfOtherForm.address());
    AOT_TEST_CHECK(!run.calledOperation && run.result == bitsOf(jsNumber(8)));
    withRowOfOtherForm.field() = 0;
    AOT_TEST_CHECK(reads.readAgain(withRowOfOtherForm.address()).calledOperation);
    AOT_TEST_CHECK(reads.readAgain(withValue.address(), nameOutsideLayouts).calledOperation);
    words.numberOfKeysThatDependOnChains = AccessTable::maxKeysListed + 1;
    words.didChangeObjectOnSomeChain();
    AOT_TEST_CHECK(placeOf(PropertyReads::name).keyOfOwnValue == keyOf(PropertyReads::name));
    words.forgetEverything();
    AOT_TEST_CHECK(!placeOf(PropertyReads::name).keyOfOwnValue);

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
    cellsOfBuiltins[numberOfCell] = cellAt(prototypeObject.address());

#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS
    runner.setCountsInstructions(true);
    AOT_TEST_CHECK(isWithinBudgetOfRead(askBuiltin(value.address()), 21, "instanceof of a built-in whose prototype object the stub reads"_s));
    AOT_TEST_CHECK(isWithinBudgetOfRead(ask(value.address()), 32, "instanceof of a remembered constructor, found at the first link"_s));
    structureOfValues.at<uint64_t>(Structure::prototypeOffset()) = objectPrototype.address();
    AOT_TEST_CHECK(isWithinBudgetOfRead(ask(value.address()), 34, "instanceof of a remembered constructor, not found below Object.prototype"_s));
    bindings[numberOfBinding].constant = static_cast<EncodedJSValue>(constructor.address());
    AOT_TEST_CHECK(isWithinBudgetOfRead(runner.run(instanceOf(InstanceOfOperand::NameSeenByScripts), { value.address() }, numberOfBinding), 41, "instanceof of a remembered constructor that a global name holds"_s));
    bindings[numberOfBinding] = { 0, structureIDOfValues, epochOfGlobalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(&wordOfName)) };
    AOT_TEST_CHECK(isWithinBudgetOfRead(runner.run(instanceOf(InstanceOfOperand::NameSeenByScripts), { value.address() }, numberOfBinding), 56, "instanceof of a remembered constructor at the location of a global name"_s));
#endif
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
