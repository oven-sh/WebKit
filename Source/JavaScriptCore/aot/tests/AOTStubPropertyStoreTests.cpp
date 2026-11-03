/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTLimits.h"
#include "AOTPrimitives.h"
#include "AOTPropertyCachesWords.h"
#include "AOTFieldTable.h"
#include "AOTHeapHooks.h"
#include "AOTImageText.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeType.h"
#include "AOTSlotAccess.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRunner.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsPropertyKeys.h"
#include "CompleteSubspace.h"
#include "GetterSetter.h"
#include "IndexingHeader.h"
#include "JSCInlines.h"
#include "JSObject.h"
#include "MarkStack.h"
#include "MarkedBlockCage.h"
#include "PropertyOffset.h"
#include "StructureID.h"
#include "Symbol.h"
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static uint64_t bitsOf(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static constexpr uint64_t untouched = 0xa5a5a5a5a5a5a5a5ULL;
static constexpr std::array allPutModes { PutMode::Strict, PutMode::Sloppy, PutMode::Define };

namespace {

enum class FiledIn : bool { Front, HashedTable };

struct StoreCase {
    PutMode mode;
    FiledIn filedIn;
    bool nameIsBound;
};

struct FakePlainObject : FakeObject<JSObject::offsetOfInlineStorage() + 6 * sizeof(uint64_t)> {
    static constexpr unsigned firstInlineWord = JSObject::offsetOfInlineStorage() / sizeof(uint64_t);
    static constexpr unsigned numberOfWords = firstInlineWord + 6;

    FakePlainObject(uint32_t structureID, uint64_t butterfly = 0)
    {
        for (unsigned word = 0; word < numberOfWords; ++word)
            this->word(word) = untouched;
        at<uint64_t>(0) = 0;
        setHeader(structureID, FinalObjectType);
        setCellState(CellState::DefinitelyWhite);
        at<uint64_t>(JSObject::butterflyOffset()) = butterfly;
    }

    uint64_t& word(int32_t index) { return at<uint64_t>(index * static_cast<ptrdiff_t>(sizeof(uint64_t))); }
    uint32_t structureID() { return at<uint32_t>(JSCell::structureIDOffset()); }
    uint64_t butterfly() { return at<uint64_t>(JSObject::butterflyOffset()); }
    void setCellState(CellState state) { at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(state); }
};

template<unsigned capacity>
struct FakeStorage : FakeObject<capacity * sizeof(uint64_t)> {
    FakeStorage()
    {
        for (unsigned i = 0; i < capacity; ++i)
            this->template at<uint64_t>(i * sizeof(uint64_t)) = untouched;
    }

    uint64_t butterfly() const { return this->address() + capacity * sizeof(uint64_t) + sizeof(IndexingHeader); }
    uint64_t& property(unsigned index) { return this->template at<uint64_t>((capacity - 1 - index) * sizeof(uint64_t)); }
};

class PropertyStores {
    WTF_MAKE_NONCOPYABLE(PropertyStores);
public:
    static constexpr uint16_t boundName = 3;
    static constexpr uint16_t unboundName = 4;
    static constexpr unsigned numberOfNames = 8;
    static constexpr uint32_t structureID = 0x1000;
    static constexpr uint32_t structureIDAfterAddition = 0x1040;
    static constexpr uint32_t slimClosureStructureID = 0x1080;
    static constexpr uint32_t typeOfAnything = 64;
    static constexpr uint32_t typeOfNumbers = 96;
    static constexpr uint32_t typeOfBooleans = 128;
    static constexpr uint32_t offsetOfOrders = 160;
    static constexpr uint32_t highestShapeWithoutFields = 16;
    static constexpr unsigned numberOfFieldRecords = 10;

    PropertyStores()
        : runner(createPolicy(), boundKeys)
    {
        Instance& instance = runner.instance();
        m_rows[structureID >> StructureRowWords::granuleShift] = 0;
        m_rows[structureIDAfterAddition >> StructureRowWords::granuleShift] = numberOfNames;
        m_rows[slimClosureStructureID >> StructureRowWords::granuleShift] = 2 * numberOfNames;
        for (SlotAccess access : allSlotAccesses)
            instance.propertyCaches.front[static_cast<unsigned>(access)] = { m_rows.data(), m_planes[static_cast<unsigned>(access)].data() };
        instance.propertyCaches.hashedSlots = m_hashedSlots.data();
        instance.propertyCaches.hashedSlotOffsetMask = HashedSlot::offsetMaskFor(m_hashedSlots.size());
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        for (unsigned name = 1; name < numberOfNames; ++name) {
            m_identifiers[name].header.length = 1;
            m_identifiers[name].number = name;
            m_identifierOffsets[name] = name * sizeof(LazyStringHeader);
        }
        instance.image.lazyStringHeaderOffsets = m_identifierOffsets.data();
        instance.lazyStrings.headers = reinterpret_cast<uint8_t*>(m_identifiers.data());
        instance.lazyStrings.capacity = sizeof(m_identifiers);
#else
        for (unsigned name = 1; name < numberOfNames; ++name)
            m_identifiers[name].length = 1;
        instance.image.programIdentifiers = reinterpret_cast<UniquedStringImpl*>(m_identifiers.data());
#endif
        instance.image.fieldTableEntries = m_emptyFieldTable.data() + highestShapeWithoutFields;
        instance.image.fieldRecordIndices = m_emptyFieldRecordIndices.data();
        instance.cellCageBase = MarkedBlockCage::baseOf(this);

        m_auxiliarySpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + initialOutOfLineCapacity * sizeof(uint32_t)) = &allocatorOfFirstStorage;
        m_auxiliarySpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + 2 * initialOutOfLineCapacity * sizeof(uint32_t)) = &allocatorOfDoubledStorage;
        m_auxiliarySpace.at<void*>(CompleteSubspace::offsetOfAllocatorForSizeStep() + 4 * initialOutOfLineCapacity * sizeof(uint32_t)) = &allocatorOfStorageDoubledTwice;
        instance.allocation.auxiliarySpace = m_auxiliarySpace.bytes.data();

        instance.image.tables = reinterpret_cast<const ProgramTables*>(m_types.bytes.data());
        m_types.at<RuntimeType>(typeOfAnything) = { Typed::Primitive::All, 0, 0, 1, 0 };
        m_types.at<RuntimeCellTest>(typeOfAnything + RuntimeType::offsetOfCellTests()) = RuntimeCellTest::isObject();
        m_types.at<RuntimeType>(typeOfNumbers) = { Typed::Primitive::Number, 0, 0, 0, 0 };
        m_types.at<RuntimeType>(typeOfBooleans) = { Typed::Primitive::Boolean, 0, 0, 0, 0 };
        instance.slimClosureHeaders[0] = slimClosureStructureID;

        // The Structure of every fake compiled object. Its layout lists no order.
        instance.prefix().structureIDBase = structure.address() - structureID;
        structure.at<uint64_t>(CompiledStructure::offsetOfLayout()) = m_layout.address();
    }

    LayoutData& layout() { return m_layout.at<LayoutData>(0); }
    FieldData& fieldRecord(unsigned index) { return m_layout.at<FieldData>(sizeof(LayoutData) + index * sizeof(FieldData)); }
    template<typename T> T& inTables(uint32_t offset) { return m_types.at<T>(offset); }

    static uint16_t nameOf(const StoreCase& storeCase) { return storeCase.nameIsBound ? boundName : unboundName; }
    uint64_t uidOf(uint16_t name) const { return std::bit_cast<uintptr_t>(m_identifiers.data() + name); }

    void file(const StoreCase& storeCase, const Slot& slot)
    {
        clearCaches();
        SlotAccess access = accessFor(storeCase.mode);
        uint16_t name = nameOf(storeCase);
        if (storeCase.filedIn == FiledIn::Front) {
            std::optional<PackedSlot> packed = PackedSlot::tryPack(slot, name, runner.instance().cellCageBase);
            AOT_TEST_CHECK(packed.has_value());
            m_planes[static_cast<unsigned>(access)][m_rows[slot.structureID.bits() >> StructureRowWords::granuleShift] + name] = *packed;
            return;
        }
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        auto* uid = reinterpret_cast<UniquedStringImpl*>(&m_identifiers[name]);
#else
        UniquedStringImpl* uid = runner.instance().image.programIdentifiers + name;
#endif
        uint32_t offset = HashedSlot::offsetOfFirstWay(slot.structureID, uid, runner.instance().propertyCaches.hashedSlotOffsetMask);
        HashedSlot& entry = m_hashedSlots[offset >> HashedSlot::sizeShift];
        entry.slot = slot;
        entry.key = HashedSlot::keyFor(uid, access);
    }

    void clearCaches()
    {
        for (auto& plane : m_planes)
            plane.fill(PackedSlot { });
        m_hashedSlots.fill(HashedSlot { });
    }

    StubRun store(const StoreCase& storeCase, uint64_t receiver, uint64_t value)
    {
        if (storeCase.nameIsBound)
            return runner.run(putById(storeCase.mode).withImmediate(boundName), { receiver, value });
        return runner.run(putById(storeCase.mode), { receiver, value }, unboundName);
    }

    StubRunner runner;
    FakeObject<sizeof(CompiledStructure)> structure;
    FakeAllocator allocatorOfFirstStorage { initialOutOfLineCapacity * sizeof(uint64_t) };
    FakeAllocator allocatorOfDoubledStorage { 2 * initialOutOfLineCapacity * sizeof(uint64_t) };
    FakeAllocator allocatorOfStorageDoubledTwice { 4 * initialOutOfLineCapacity * sizeof(uint64_t) };

private:
    static constexpr std::array boundKeys { putById(PutMode::Strict).withImmediate(boundName), putById(PutMode::Sloppy).withImmediate(boundName), putById(PutMode::Define).withImmediate(boundName) };

    static StubPolicy createPolicy()
    {
        StubPolicy policy;
        policy.namesWithOwnStubByAccess[static_cast<size_t>(Typed::Access::Store)].set(boundName);
        policy.namesWithOwnStubByAccess[static_cast<size_t>(Typed::Access::Define)].set(boundName);
        return policy;
    }

    std::array<uint32_t, (slimClosureStructureID >> StructureRowWords::granuleShift) + 1> m_rows { };
    std::array<std::array<PackedSlot, 3 * numberOfNames>, numberOfSlotAccesses> m_planes { };
    std::array<HashedSlot, 64> m_hashedSlots { };
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    std::array<LazyStringHeader, numberOfNames> m_identifiers { };
    std::array<uint32_t, numberOfNames> m_identifierOffsets { };
#else
    std::array<ImageStringHeader, numberOfNames> m_identifiers { };
#endif
    std::array<uint32_t, highestShapeWithoutFields + numberOfNames> m_emptyFieldTable { };
    std::array<uint16_t, numberOfNames> m_emptyFieldRecordIndices { };
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + 16 * sizeof(void*)> m_auxiliarySpace;
    FakeObject<192> m_types;
    FakeObject<sizeof(LayoutData) + numberOfFieldRecords * sizeof(FieldData)> m_layout;
};

} // anonymous namespace

static StructureID toStructureID(uint32_t bits) { return std::bit_cast<StructureID>(bits); }

template<typename Functor>
static void forEachStoreCase(std::span<const PutMode> modes, const Functor& functor)
{
    for (PutMode mode : modes) {
        for (FiledIn filedIn : { FiledIn::Front, FiledIn::HashedTable }) {
            for (bool nameIsBound : { false, true })
                functor(StoreCase { mode, filedIn, nameIsBound });
        }
    }
}

static void checkOnlyWordChanged(FakePlainObject& object, int32_t changedWord, uint64_t value)
{
    for (int32_t word = FakePlainObject::firstInlineWord; word < static_cast<int32_t>(FakePlainObject::numberOfWords); ++word)
        AOT_TEST_CHECK(object.word(word) == (word == changedWord ? value : untouched));
}

static void testReplacedValues()
{
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        for (PropertyOffset offset : { 0, 5 }) {
            FakePlainObject object(PropertyStores::structureID);
            stores.file(storeCase, Slot::createInStorage(toStructureID(PropertyStores::structureID), Slot::Kind::Value, Slot::isOwn, nullptr, offset));
            StubRun run = stores.store(storeCase, object.address(), value);
            AOT_TEST_CHECK(!run.calledOperation && object.structureID() == PropertyStores::structureID && !object.butterfly());
            checkOnlyWordChanged(object, Slot::wordIndexFor(offset), value);
        }
        for (unsigned index : { 0u, 3u }) {
            FakeStorage<initialOutOfLineCapacity> storage;
            FakePlainObject object(PropertyStores::structureID, storage.butterfly());
            stores.file(storeCase, Slot::createInStorage(toStructureID(PropertyStores::structureID), Slot::Kind::Value, Slot::isOwn, nullptr, firstOutOfLineOffset + index));
            StubRun run = stores.store(storeCase, object.address(), value);
            AOT_TEST_CHECK(!run.calledOperation && object.structureID() == PropertyStores::structureID && object.butterfly() == storage.butterfly());
            checkOnlyWordChanged(object, -1, value);
            for (unsigned other = 0; other < initialOutOfLineCapacity; ++other)
                AOT_TEST_CHECK(storage.property(other) == (other == index ? value : untouched));
        }
    });
}

static Slot createAddition(PropertyOffset offset, uint32_t grownCapacityOrZero, uint8_t flags = 0)
{
    std::optional<Slot> slot = Slot::tryCreateTransition(toStructureID(PropertyStores::structureID), flags, toStructureID(PropertyStores::structureIDAfterAddition), offset, grownCapacityOrZero);
    AOT_TEST_CHECK(slot.has_value());
    return *slot;
}

static void testAdditionsInExistingStorage()
{
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        FakePlainObject object(PropertyStores::structureID);
        stores.file(storeCase, createAddition(2, 0));
        StubRun run = stores.store(storeCase, object.address(), value);
        AOT_TEST_CHECK(!run.calledOperation && object.structureID() == PropertyStores::structureIDAfterAddition);
        checkOnlyWordChanged(object, Slot::wordIndexFor(2), value);

        FakeStorage<initialOutOfLineCapacity> storage;
        FakePlainObject objectWithStorage(PropertyStores::structureID, storage.butterfly());
        stores.file(storeCase, createAddition(firstOutOfLineOffset + 1, 0));
        run = stores.store(storeCase, objectWithStorage.address(), value);
        AOT_TEST_CHECK(!run.calledOperation && objectWithStorage.structureID() == PropertyStores::structureIDAfterAddition && objectWithStorage.butterfly() == storage.butterfly());
        AOT_TEST_CHECK(storage.property(1) == value && storage.property(0) == untouched && storage.property(2) == untouched);
    });
}

static void checkLeftToAdditionBySlot(PropertyStores& stores, const StoreCase& storeCase, const Slot& slot, FakePlainObject& object, uint64_t value)
{
    uint64_t butterflyBefore = object.butterfly();
    stores.runner.recordCallsOf(Entry::operationAOTAddPropertyBySlot);
    StubRun run = stores.store(storeCase, object.address(), value);
    stores.runner.countCallsOf(Entry::operationAOTAddPropertyBySlot);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address());
    AOT_TEST_CHECK(run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits());
    AOT_TEST_CHECK(run.recordedArguments[3] == value && static_cast<uint32_t>(run.recordedArguments[4]) == static_cast<uint32_t>(RuntimeCounterID::StubCannotGrowStorage));
    AOT_TEST_CHECK(object.structureID() == PropertyStores::structureID && object.butterfly() == butterflyBefore);
}

static void testAdditionsWithFirstStorage()
{
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    const Slot slot = createAddition(firstOutOfLineOffset, initialOutOfLineCapacity);
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        stores.file(storeCase, slot);
        FakeStorage<initialOutOfLineCapacity> storage;
        FakePlainObject object(PropertyStores::structureID);
        stores.allocatorOfFirstStorage.makeRoomAt(storage.address(), sizeof(storage.bytes));
        StubRun run = stores.store(storeCase, object.address(), value);
        AOT_TEST_CHECK(!run.calledOperation && !stores.allocatorOfFirstStorage.hasRoom());
        AOT_TEST_CHECK(object.structureID() == PropertyStores::structureIDAfterAddition && object.butterfly() == storage.butterfly());
        AOT_TEST_CHECK(storage.property(0) == value);
        for (unsigned index = 1; index < initialOutOfLineCapacity; ++index)
            AOT_TEST_CHECK(!storage.property(index));
        checkOnlyWordChanged(object, -1, value);

        FakePlainObject objectWithoutRoom(PropertyStores::structureID);
        checkLeftToAdditionBySlot(stores, storeCase, slot, objectWithoutRoom, value);

        FakePlainObject objectThatHasStorage(PropertyStores::structureID, storage.butterfly());
        stores.allocatorOfFirstStorage.makeRoomAt(storage.address(), sizeof(storage.bytes));
        checkLeftToAdditionBySlot(stores, storeCase, slot, objectThatHasStorage, value);
        stores.allocatorOfFirstStorage.makeRoomAt(0, 0);
    });
}

template<unsigned fullCapacity>
static void checkAdditionsWithDoubledStorage(PropertyStores& stores, FakeAllocator& allocator)
{
    constexpr unsigned doubledCapacity = 2 * fullCapacity;
    const uint64_t value = bitsOf(jsNumber(42));
    const Slot slot = createAddition(firstOutOfLineOffset + fullCapacity, doubledCapacity);
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        stores.file(storeCase, slot);
        FakeStorage<fullCapacity> fullStorage;
        for (unsigned index = 0; index < fullCapacity; ++index)
            fullStorage.property(index) = bitsOf(jsNumber(static_cast<int32_t>(100 + index)));
        FakeStorage<doubledCapacity> storage;
        FakePlainObject object(PropertyStores::structureID, fullStorage.butterfly());
        allocator.makeRoomAt(storage.address(), sizeof(storage.bytes));
        StubRun run = stores.store(storeCase, object.address(), value);
        AOT_TEST_CHECK(!run.calledOperation && !allocator.hasRoom());
        AOT_TEST_CHECK(object.structureID() == PropertyStores::structureIDAfterAddition && object.butterfly() == storage.butterfly());
        for (unsigned index = 0; index < doubledCapacity; ++index) {
            uint64_t expected = index < fullCapacity ? bitsOf(jsNumber(static_cast<int32_t>(100 + index))) : index == fullCapacity ? value : 0;
            AOT_TEST_CHECK(storage.property(index) == expected);
        }
        for (unsigned index = 0; index < fullCapacity; ++index)
            AOT_TEST_CHECK(fullStorage.property(index) == bitsOf(jsNumber(static_cast<int32_t>(100 + index))));

        FakePlainObject objectWithoutRoom(PropertyStores::structureID, fullStorage.butterfly());
        checkLeftToAdditionBySlot(stores, storeCase, slot, objectWithoutRoom, value);

        FakePlainObject objectWithoutStorage(PropertyStores::structureID);
        allocator.makeRoomAt(storage.address(), sizeof(storage.bytes));
        checkLeftToAdditionBySlot(stores, storeCase, slot, objectWithoutStorage, value);
        allocator.makeRoomAt(0, 0);
    });
}

static void testAdditionsWithDoubledStorage()
{
    PropertyStores stores;
    checkAdditionsWithDoubledStorage<initialOutOfLineCapacity>(stores, stores.allocatorOfDoubledStorage);
    checkAdditionsWithDoubledStorage<2 * initialOutOfLineCapacity>(stores, stores.allocatorOfStorageDoubledTwice);
}

static void testAdditionsBesideIndexingHeader()
{
    constexpr unsigned doubledCapacity = 2 * initialOutOfLineCapacity;
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    const Slot first = createAddition(firstOutOfLineOffset, initialOutOfLineCapacity, Slot::storageHasIndexingHeader);
    const Slot doubled = createAddition(firstOutOfLineOffset + initialOutOfLineCapacity, doubledCapacity, Slot::storageHasIndexingHeader);
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        if (storeCase.filedIn == FiledIn::Front)
            return;
        FakeStorage<initialOutOfLineCapacity> storageWithElements;
        FakeStorage<doubledCapacity> room;
        FakePlainObject object(PropertyStores::structureID, storageWithElements.butterfly());

        stores.file(storeCase, first);
        stores.allocatorOfFirstStorage.makeRoomAt(room.address(), initialOutOfLineCapacity * sizeof(uint64_t));
        checkLeftToAdditionBySlot(stores, storeCase, first, object, value);
        AOT_TEST_CHECK(stores.allocatorOfFirstStorage.hasRoom());
        stores.allocatorOfFirstStorage.makeRoomAt(0, 0);

        stores.file(storeCase, doubled);
        stores.allocatorOfDoubledStorage.makeRoomAt(room.address(), sizeof(room.bytes));
        checkLeftToAdditionBySlot(stores, storeCase, doubled, object, value);
        AOT_TEST_CHECK(stores.allocatorOfDoubledStorage.hasRoom());
        stores.allocatorOfDoubledStorage.makeRoomAt(0, 0);

        for (unsigned index = 0; index < initialOutOfLineCapacity; ++index)
            AOT_TEST_CHECK(storageWithElements.property(index) == untouched);
        for (unsigned index = 0; index < doubledCapacity; ++index)
            AOT_TEST_CHECK(room.property(index) == untouched);
    });
}

static void testSetters()
{
    constexpr std::array modesWithSetters { PutMode::Strict, PutMode::Sloppy };
    constexpr PropertyOffset offset = 1;
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    const Slot slot = Slot::createInStorage(toStructureID(PropertyStores::structureID), Slot::Kind::Setter, Slot::isOwn, nullptr, offset);

    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> setter;
    setter.setHeader(PropertyStores::slimClosureStructureID, SlimClosureType);
    setter.at<uint64_t>(offsetOfSlimClosureWord) = stores.runner.closureWord(1);
    FakeObject<16> absentSetter;
    absentSetter.setHeader(0, NullSetterFunctionType);
    FakeObject<sizeof(GetterSetter)> accessors;
    FakePlainObject object(PropertyStores::structureID);
    object.word(Slot::wordIndexFor(offset)) = accessors.address();

    forEachStoreCase(modesWithSetters, [&](const StoreCase& storeCase) {
        stores.file(storeCase, slot);
        accessors.at<uint64_t>(GetterSetter::offsetOfSetter()) = setter.address();
        StubRun run = stores.store(storeCase, object.address(), value);
        AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::CompiledFunction);
        AOT_TEST_CHECK(run.landedCall.values.callee == setter.address() && run.landedCall.values.thisValue == object.address());
        AOT_TEST_CHECK(run.landedCall.values.arguments[0] == value && static_cast<uint32_t>(run.landedCall.count) == 1);

        accessors.at<uint64_t>(GetterSetter::offsetOfSetter()) = absentSetter.address();
        stores.runner.recordCallsOf(Entry::operationAOTCallSetterOfSlot);
        run = stores.store(storeCase, object.address(), value);
        stores.runner.countCallsOf(Entry::operationAOTCallSetterOfSlot);
        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.landedCall.place == LandedCall::Place::Nowhere);
        AOT_TEST_CHECK(run.recordedArguments[0] == object.address() && run.recordedArguments[1] == accessors.address() && run.recordedArguments[2] == value);
        AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[3]) == static_cast<uint32_t>(storeCase.mode));
        AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[4]) == static_cast<uint32_t>(RuntimeCounterID::StubCannotCallSetter));
        AOT_TEST_CHECK(object.word(Slot::wordIndexFor(offset)) == accessors.address());
    });
}

static void checkMiss(PropertyStores& stores, const StoreCase& storeCase, uint64_t receiver, uint64_t value)
{
    stores.runner.recordCallsOf(Entry::operationAOTPutByIdAndCache);
    StubRun run = stores.store(storeCase, receiver, value);
    stores.runner.countCallsOf(Entry::operationAOTPutByIdAndCache);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == receiver && run.recordedArguments[1] == value);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[2]) == PropertyStores::nameOf(storeCase) && !run.recordedArguments[3]);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[4]) == static_cast<uint32_t>(storeCase.mode));
}

static Entry uncachedOperationOf(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return Entry::operationAOTPutById;
    case PutMode::Sloppy:
        return Entry::operationAOTPutByIdSloppy;
    case PutMode::Define:
        return Entry::operationAOTDefineById;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkUncached(PropertyStores& stores, const StoreCase& storeCase, uint64_t receiver, uint64_t value)
{
    Entry operation = uncachedOperationOf(storeCase.mode);
    stores.runner.recordCallsOf(operation);
    StubRun run = stores.store(storeCase, receiver, value);
    stores.runner.countCallsOf(operation);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == receiver);
    AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[1]) == PropertyStores::nameOf(storeCase) && run.recordedArguments[2] == value);
}

static void testExits()
{
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        FakePlainObject object(PropertyStores::structureID);
        stores.clearCaches();
        checkMiss(stores, storeCase, object.address(), value);
        checkMiss(stores, storeCase, bitsOf(jsNumber(7)), value);

        stores.file(storeCase, Slot::createInStorage(toStructureID(PropertyStores::structureIDAfterAddition), Slot::Kind::Value, Slot::isOwn, nullptr, 0));
        checkMiss(stores, storeCase, object.address(), value);

        stores.file(storeCase, Slot::createWithoutPayload(toStructureID(PropertyStores::structureID), Slot::Kind::Uncacheable, 0));
        checkUncached(stores, storeCase, object.address(), value);
        checkOnlyWordChanged(object, -1, value);
    });

    for (FiledIn filedIn : { FiledIn::Front, FiledIn::HashedTable }) {
        StoreCase definition { PutMode::Define, filedIn, false };
        FakePlainObject object(PropertyStores::structureID);
        stores.file(definition, Slot::createInStorage(toStructureID(PropertyStores::structureID), Slot::Kind::Setter, Slot::isOwn, nullptr, 1));
        checkMiss(stores, definition, object.address(), value);
    }
}

static void testBarriers()
{
    PropertyStores stores;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    stores.runner.recordCallsOf(Entry::operationAOTBarrierSlow);
    forEachStoreCase(allPutModes, [&](const StoreCase& storeCase) {
        for (bool addsProperty : { false, true }) {
            stores.file(storeCase, addsProperty ? createAddition(0, 0) : Slot::createInStorage(toStructureID(PropertyStores::structureID), Slot::Kind::Value, Slot::isOwn, nullptr, 0));
            FakePlainObject whiteOwner(PropertyStores::structureID);
            AOT_TEST_CHECK(!stores.store(storeCase, whiteOwner.address(), cell.address()).calledOperation);
            AOT_TEST_CHECK(whiteOwner.word(Slot::wordIndexFor(0)) == cell.address());

            FakePlainObject blackOwner(PropertyStores::structureID);
            blackOwner.setCellState(CellState::PossiblyBlack);
            StubRun run = stores.store(storeCase, blackOwner.address(), cell.address());
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == blackOwner.address());
            AOT_TEST_CHECK(blackOwner.word(Slot::wordIndexFor(0)) == cell.address());
        }

        stores.file(storeCase, Slot::createInStorage(toStructureID(PropertyStores::structureID), Slot::Kind::Value, Slot::isOwn, nullptr, 0));
        FakePlainObject blackOwner(PropertyStores::structureID);
        blackOwner.setCellState(CellState::PossiblyBlack);
        AOT_TEST_CHECK(!stores.store(storeCase, blackOwner.address(), bitsOf(jsNumber(42))).calledOperation);
    });
}

// While no collector runs beside the program the stub itself remembers a black owner. Only a full segment is left to the operation.
static void testBarrierRemembersOwnerWithoutOperation()
{
#if !ASSERT_ENABLED
    PropertyStores stores;
    Instance& instance = stores.runner.instance();
    MarkStackArray stack;
    uintptr_t barriersExecuted = 0;
    instance.heap.mutatorMarkStack = &stack;
    instance.heap.barriersExecuted = &barriersExecuted;
    stores.runner.recordCallsOf(Entry::operationAOTBarrierSlow);

    std::array<FakePlainObject, 3> owners { FakePlainObject(PropertyStores::structureID), FakePlainObject(PropertyStores::structureID), FakePlainObject(PropertyStores::structureID) };
    for (FakePlainObject& owner : owners) {
        owner.setCellState(CellState::PossiblyBlack);
        StubRun run = stores.runner.run(writeBarrier(), { owner.address() });
        AOT_TEST_CHECK(!run.calledOperation && !run.numberOfRecordedCalls);
        AOT_TEST_CHECK(owner.at<uint8_t>(JSCell::cellStateOffset()) == static_cast<uint8_t>(CellState::PossiblyGrey));
    }
    AOT_TEST_CHECK(barriersExecuted == owners.size() && stack.size() == owners.size());
    for (size_t i = owners.size(); i--;)
        AOT_TEST_CHECK(std::bit_cast<uint64_t>(stack.removeLast()) == owners[i].address());

    // An owner that is remembered already is not remembered again.
    AOT_TEST_CHECK(!stores.runner.run(writeBarrier(), { owners[0].address() }).calledOperation && stack.isEmpty() && barriersExecuted == owners.size());

    while (stack.size() < MutatorMarkStackLayout::segmentCapacity)
        stack.append(std::bit_cast<const JSCell*>(owners[0].address()));
    owners[1].setCellState(CellState::PossiblyBlack);
    StubRun atFullSegment = stores.runner.run(writeBarrier(), { owners[1].address() });
    AOT_TEST_CHECK(atFullSegment.numberOfRecordedCalls == 1 && atFullSegment.recordedArguments[0] == owners[1].address() && stack.size() == MutatorMarkStackLayout::segmentCapacity);
    instance.heap.mutatorMarkStack = nullptr;
    instance.heap.barriersExecuted = nullptr;
#endif
}

namespace {

struct FakeTypedObject : FakeObject<offsetOfFirstField + 2 * sizeof(uint64_t)> {
    static constexpr uint32_t offsetOfField = offsetOfFirstField;

    explicit FakeTypedObject(uint64_t bitsOfField, uint32_t orderWord = 0)
    {
        setHeader(PropertyStores::structureID, CompiledObjectType);
        at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::DefinitelyWhite);
        at<uint32_t>(offsetOfOrderWord) = orderWord;
        field() = bitsOfField;
        at<uint64_t>(offsetOfField + sizeof(uint64_t)) = untouched;
    }

    uint64_t& field() { return at<uint64_t>(offsetOfField); }
    bool wordAfterFieldIsUntouched() { return at<uint64_t>(offsetOfField + sizeof(uint64_t)) == untouched; }
};

struct ExpectedStore {
    bool isWritten;
    bool isWrittenByStub;
    uint64_t word;
};

} // anonymous namespace

static Slot createFieldSlot(Slot::Kind kind, SlotRepresentation representation, uint32_t type)
{
    return Slot::createField(toStructureID(PropertyStores::structureID), kind, Slot::isOwn, SlotData::create(FakeTypedObject::offsetOfField, representation), type, 0);
}

static uint64_t replaceLowBytes(uint64_t word, unsigned width, uint64_t bits)
{
    if (width == sizeof(uint64_t))
        return bits;
    uint64_t mask = (static_cast<uint64_t>(1) << (8 * width)) - 1;
    return (word & ~mask) | (bits & mask);
}

static uint64_t presentBitsOf(SlotRepresentation representation, uint64_t cell, uintptr_t cageBase)
{
    SlotTraits traits = traitsOf(representation);
    uint64_t bits = 0;
    if (traits.hasAbsent && !traits.absentBits)
        bits = traits.hasUndefined ? traits.undefinedBits : traits.isNarrowReference ? MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(static_cast<uintptr_t>(cell))) : cell;
    return replaceLowBytes(untouched, traits.width, bits);
}

static ExpectedStore expectedStoreOf(SlotRepresentation representation, JSValue value, uint64_t wordBefore, uintptr_t cageBase)
{
    SlotTraits traits = traitsOf(representation);
    uint64_t word = wordBefore;
    auto written = [&](bool isWritten, bool isWrittenByStub) {
        return ExpectedStore { isWritten, isWritten && isWrittenByStub, word };
    };
    auto writtenBits = [&](uint64_t bits) {
        word = replaceLowBytes(wordBefore, traits.width, bits);
        return ExpectedStore { true, true, word };
    };
    if (traits.valueKind == SlotValueKind::DeferredClosure)
        return { false, false, wordBefore };
    if (traits.valueKind == SlotValueKind::Boxed)
        return writtenBits(bitsOf(value));
    if (value.isUndefined())
        return traits.hasUndefined ? writtenBits(traits.undefinedBits) : ExpectedStore { false, false, wordBefore };
    if (value.isNull())
        return traits.hasNull ? writtenBits(traits.nullBits) : ExpectedStore { false, false, wordBefore };
    if (value.isCell()) {
        if (traits.valueKind != SlotValueKind::Cell)
            return { false, false, wordBefore };
        return writtenBits(traits.isNarrowReference ? MarkedBlockCage::encode(cageBase, value.asCell()) : bitsOf(value));
    }
    if (value.isInt32())
        return written(tryWriteInt32(representation, &word, value.asInt32()), true);
    if (value.isDouble())
        return written(tryWriteDouble(representation, &word, value.asDouble()), traits.valueKind == SlotValueKind::Double || traits.valueKind == SlotValueKind::Float);
    return written(tryWriteBoolean(representation, &word, value.asBoolean()), true);
}

static void checkFieldStore(PropertyStores& stores, const StoreCase& storeCase, SlotRepresentation representation, JSValue value, uint64_t cell)
{
    uintptr_t cageBase = stores.runner.instance().cellCageBase;
    const Slot slot = createFieldSlot(Slot::Kind::Field, representation, PropertyStores::typeOfAnything);
    FakeTypedObject object(presentBitsOf(representation, cell, cageBase));
    uint64_t wordBefore = object.field();
    ExpectedStore expected = expectedStoreOf(representation, value, wordBefore, cageBase);

    stores.file(storeCase, slot);
    stores.runner.recordCallsOf(Entry::operationAOTStoreField);
    StubRun run = stores.store(storeCase, object.address(), bitsOf(value));
    stores.runner.countCallsOf(Entry::operationAOTStoreField);
    AOT_TEST_CHECK(object.wordAfterFieldIsUntouched());
    if (!run.calledOperation) {
        AOT_TEST_CHECK(expected.isWritten && object.field() == expected.word);
        return;
    }
    AOT_TEST_CHECK(!expected.isWrittenByStub && object.field() == wordBefore);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address());
    AOT_TEST_CHECK(run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits() && run.recordedArguments[3] == bitsOf(value));
    AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[4]) == static_cast<uint32_t>(RuntimeCounterID::StubCannotStoreField));
}

static void testTypedFields()
{
    PropertyStores stores;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(cell.bytes.data()) == stores.runner.instance().cellCageBase);
    const JSValue values[] = {
        jsNumber(0), jsNumber(7), jsNumber(-1), jsNumber(200), jsNumber(-200), jsNumber(70000), jsNumber(INT32_MAX), jsNumber(INT32_MIN), jsNumber(absentInt32), jsNumber(minInlineInt32),
        jsDoubleNumber(0.5), jsDoubleNumber(7), jsDoubleNumber(1e300), jsDoubleNumber(-0.0),
        jsBoolean(true), jsBoolean(false), jsUndefined(), jsNull(), JSValue(std::bit_cast<JSCell*>(cell.address())),
    };
    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
            for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
                for (JSValue value : values)
                    checkFieldStore(stores, storeCase, representation, value, cell.address());
            }
        }
    }
}

static void testFieldsThatLeaveTheStub()
{
    PropertyStores stores;
    const uint64_t number = bitsOf(jsNumber(42));
    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
            stores.file(storeCase, createFieldSlot(Slot::Kind::Field, SlotRepresentation::JSValue, PropertyStores::typeOfAnything));
            FakeTypedObject objectWithoutField(JSValue::ValueEmpty);
            checkUncached(stores, storeCase, objectWithoutField.address(), number);
            AOT_TEST_CHECK(!objectWithoutField.field());

            const Slot slotOfNumbers = createFieldSlot(Slot::Kind::Field, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers);
            stores.file(storeCase, slotOfNumbers);
            FakeTypedObject object(JSValue::ValueUndefined);
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), number).calledOperation && object.field() == number);
            stores.runner.recordCallsOf(Entry::operationAOTStoreField);
            StubRun run = stores.store(storeCase, object.address(), bitsOf(jsBoolean(true)));
            stores.runner.countCallsOf(Entry::operationAOTStoreField);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[3] == bitsOf(jsBoolean(true)) && object.field() == number);
        }
    }
}

static void testInheritedSettersOfAbsentFields()
{
    constexpr PropertyOffset offset = 1;
    PropertyStores stores;
    const uint64_t value = bitsOf(jsNumber(42));
    FakeObject<offsetOfSlimClosureWord + sizeof(uint64_t)> setter;
    setter.setHeader(PropertyStores::slimClosureStructureID, SlimClosureType);
    setter.at<uint64_t>(offsetOfSlimClosureWord) = stores.runner.closureWord(1);
    FakeObject<sizeof(GetterSetter)> accessors;
    accessors.at<uint64_t>(GetterSetter::offsetOfSetter()) = setter.address();
    FakePlainObject holder(0);
    holder.word(Slot::wordIndexFor(offset)) = accessors.address();
    const SlotData field = SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::JSValue);
    const Slot slot = *Slot::tryCreateFieldAndHolder(toStructureID(PropertyStores::structureID), Slot::Kind::FieldOrInheritedSetter, Slot::dependsOnChain, field, std::bit_cast<JSCell*>(holder.address()), offset, stores.runner.instance().cellCageBase);

    for (PutMode mode : { PutMode::Strict, PutMode::Sloppy }) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
            stores.file(storeCase, slot);
            // Neither a frozen object nor one that takes no more properties stops a setter.
            for (uint32_t orderWord : { 0u, orderWordIntegrityBits }) {
                FakeTypedObject objectWithoutField(JSValue::ValueEmpty, orderWord);
                StubRun run = stores.store(storeCase, objectWithoutField.address(), value);
                AOT_TEST_CHECK(!run.calledOperation && run.landedCall.place == LandedCall::Place::CompiledFunction && !objectWithoutField.field());
                AOT_TEST_CHECK(run.landedCall.values.callee == setter.address() && run.landedCall.values.thisValue == objectWithoutField.address());
                AOT_TEST_CHECK(run.landedCall.values.arguments[0] == value && static_cast<uint32_t>(run.landedCall.count) == 1);
            }

            FakeTypedObject object(JSValue::ValueUndefined);
            checkUncached(stores, storeCase, object.address(), value);
            AOT_TEST_CHECK(object.field() == JSValue::ValueUndefined);
        }
    }
}

static void testAdditionsToExtraProperties()
{
    constexpr uint32_t structureIDOfExtraProperties = 0x2000;
    constexpr uint32_t structureIDOfExtraPropertiesAfterAddition = 0x2040;
    PropertyStores stores;
    Instance& instance = stores.runner.instance();
    const uint64_t value = bitsOf(jsNumber(42));
    FakeObject<sizeof(CompiledStructure)> structureAfterAddition;
    instance.prefix().structureIDBase = structureAfterAddition.address() - PropertyStores::structureIDAfterAddition;
    structureAfterAddition.at<uint32_t>(CompiledStructure::offsetOfExtraPropertiesStructureID()) = structureIDOfExtraPropertiesAfterAddition;
    auto createSlot = [](PropertyOffset offset, uint8_t flags) {
        return Slot::createExtraPropertyAddition(toStructureID(PropertyStores::structureID), Slot::isOwn | flags, toStructureID(PropertyStores::structureIDAfterAddition), offset, FakeTypedObject::offsetOfField);
    };
    const Slot fitting = createSlot(firstOutOfLineOffset + 1, 0);
    const Slot growing = createSlot(firstOutOfLineOffset + initialOutOfLineCapacity, Slot::growsStorage);

    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
            FakeStorage<initialOutOfLineCapacity> storage;
            FakePlainObject extraProperties(structureIDOfExtraProperties, storage.butterfly());
            const uint64_t extraPropertiesWord = MarkedBlockCage::encode(instance.cellCageBase, std::bit_cast<void*>(extraProperties.address()));
            auto isUnchanged = [&](FakeTypedObject& object) {
                for (unsigned index = 0; index < initialOutOfLineCapacity; ++index) {
                    if (storage.property(index) != untouched)
                        return false;
                }
                return object.at<uint32_t>(JSCell::structureIDOffset()) == PropertyStores::structureID && extraProperties.structureID() == structureIDOfExtraProperties && extraProperties.butterfly() == storage.butterfly();
            };
            auto checkLeftToAdditionBySlot = [&](const Slot& slot, FakeTypedObject& object) {
                stores.runner.recordCallsOf(Entry::operationAOTAddPropertyBySlot);
                StubRun run = stores.store(storeCase, object.address(), value);
                stores.runner.countCallsOf(Entry::operationAOTAddPropertyBySlot);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address());
                AOT_TEST_CHECK(run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits() && run.recordedArguments[3] == value);
                AOT_TEST_CHECK(isUnchanged(object));
            };

            stores.file(storeCase, growing);
            FakeTypedObject objectWithFullStorage(extraPropertiesWord, orderWordHasExtraProperties);
            checkLeftToAdditionBySlot(growing, objectWithFullStorage);

            stores.file(storeCase, fitting);
            FakeTypedObject notExtensibleObject(extraPropertiesWord, orderWordHasExtraProperties | orderWordIsNotExtensible);
            checkUncached(stores, storeCase, notExtensibleObject.address(), value);
            AOT_TEST_CHECK(isUnchanged(notExtensibleObject));

            FakeTypedObject markedObject(extraPropertiesWord, orderWordHasExtraProperties);
            markedObject.at<uint8_t>(JSCell::cellStateOffset()) = static_cast<uint8_t>(CellState::PossiblyBlack);
            checkLeftToAdditionBySlot(fitting, markedObject);

            FakeTypedObject object(extraPropertiesWord, orderWordHasExtraProperties);
            StubRun run = stores.store(storeCase, object.address(), value);
            AOT_TEST_CHECK(!run.calledOperation && storage.property(1) == value && storage.property(0) == untouched && storage.property(2) == untouched);
            AOT_TEST_CHECK(object.at<uint32_t>(JSCell::structureIDOffset()) == PropertyStores::structureIDAfterAddition && extraProperties.structureID() == structureIDOfExtraPropertiesAfterAddition);
        }
    }
}

static void testAdditionsToExtraPropertiesInNewStorage()
{
    constexpr unsigned doubledCapacity = 2 * initialOutOfLineCapacity;
    constexpr unsigned capacityPastLimit = 2 * defaultLimitValue(LimitID::MaxCapacityGrownByStub, hostArchitecture());
    constexpr uint32_t structureIDOfExtraProperties = 0x2000;
    constexpr uint32_t structureIDOfExtraPropertiesAfterAddition = 0x2040;
    PropertyStores stores;
    Instance& instance = stores.runner.instance();
    const uint64_t value = bitsOf(jsNumber(42));
    FakeObject<sizeof(CompiledStructure)> structureAfterAddition;
    instance.prefix().structureIDBase = structureAfterAddition.address() - PropertyStores::structureIDAfterAddition;
    structureAfterAddition.at<uint32_t>(CompiledStructure::offsetOfExtraPropertiesStructureID()) = structureIDOfExtraPropertiesAfterAddition;
    auto createSlot = [](PropertyOffset offset, uint8_t flags) {
        return Slot::createExtraPropertyAddition(toStructureID(PropertyStores::structureID), Slot::isOwn | Slot::growsStorage | flags, toStructureID(PropertyStores::structureIDAfterAddition), offset, FakeTypedObject::offsetOfField);
    };
    auto hiddenWordFor = [&](FakePlainObject& extraProperties) -> uint64_t {
        return MarkedBlockCage::encode(instance.cellCageBase, std::bit_cast<void*>(extraProperties.address()));
    };
    auto valueAt = [](unsigned index) {
        return bitsOf(jsNumber(static_cast<int32_t>(100 + index)));
    };
    const Slot first = createSlot(firstOutOfLineOffset, 0);
    const Slot doubled = createSlot(firstOutOfLineOffset + initialOutOfLineCapacity, 0);
    const Slot besideIndexingHeader = createSlot(firstOutOfLineOffset + initialOutOfLineCapacity, Slot::storageHasIndexingHeader);
    const Slot pastLimit = createSlot(firstOutOfLineOffset + capacityPastLimit, 0);

    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };

            FakeStorage<initialOutOfLineCapacity> firstStorage;
            FakePlainObject extraPropertiesWithoutStorage(structureIDOfExtraProperties);
            FakeTypedObject objectWithoutStorage(hiddenWordFor(extraPropertiesWithoutStorage), orderWordHasExtraProperties);
            stores.file(storeCase, first);
            stores.allocatorOfFirstStorage.makeRoomAt(firstStorage.address(), sizeof(firstStorage.bytes));
            AOT_TEST_CHECK(!stores.store(storeCase, objectWithoutStorage.address(), value).calledOperation && !stores.allocatorOfFirstStorage.hasRoom());
            AOT_TEST_CHECK(objectWithoutStorage.at<uint32_t>(JSCell::structureIDOffset()) == PropertyStores::structureIDAfterAddition);
            AOT_TEST_CHECK(extraPropertiesWithoutStorage.structureID() == structureIDOfExtraPropertiesAfterAddition && extraPropertiesWithoutStorage.butterfly() == firstStorage.butterfly());
            for (unsigned index = 0; index < initialOutOfLineCapacity; ++index)
                AOT_TEST_CHECK(firstStorage.property(index) == (index ? 0 : value));

            FakeStorage<initialOutOfLineCapacity> fullStorage;
            for (unsigned index = 0; index < initialOutOfLineCapacity; ++index)
                fullStorage.property(index) = valueAt(index);
            FakeStorage<doubledCapacity> doubledStorage;
            FakePlainObject extraProperties(structureIDOfExtraProperties, fullStorage.butterfly());
            stores.allocatorOfDoubledStorage.makeRoomAt(doubledStorage.address(), sizeof(doubledStorage.bytes));
            for (const Slot& slot : { besideIndexingHeader, pastLimit }) {
                FakeTypedObject object(hiddenWordFor(extraProperties), orderWordHasExtraProperties);
                stores.file(storeCase, slot);
                stores.runner.recordCallsOf(Entry::operationAOTAddPropertyBySlot);
                StubRun run = stores.store(storeCase, object.address(), value);
                stores.runner.countCallsOf(Entry::operationAOTAddPropertyBySlot);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address());
                AOT_TEST_CHECK(run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits() && run.recordedArguments[3] == value);
                AOT_TEST_CHECK(object.at<uint32_t>(JSCell::structureIDOffset()) == PropertyStores::structureID && stores.allocatorOfDoubledStorage.hasRoom());
                AOT_TEST_CHECK(extraProperties.structureID() == structureIDOfExtraProperties && extraProperties.butterfly() == fullStorage.butterfly());
            }

            FakeTypedObject object(hiddenWordFor(extraProperties), orderWordHasExtraProperties);
            stores.file(storeCase, doubled);
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), value).calledOperation && !stores.allocatorOfDoubledStorage.hasRoom());
            AOT_TEST_CHECK(object.at<uint32_t>(JSCell::structureIDOffset()) == PropertyStores::structureIDAfterAddition);
            AOT_TEST_CHECK(extraProperties.structureID() == structureIDOfExtraPropertiesAfterAddition && extraProperties.butterfly() == doubledStorage.butterfly());
            for (unsigned index = 0; index < doubledCapacity; ++index) {
                uint64_t expected = index < initialOutOfLineCapacity ? valueAt(index) : index == initialOutOfLineCapacity ? value : 0;
                AOT_TEST_CHECK(doubledStorage.property(index) == expected);
            }
        }
    }
}

static void testFieldsOfObjectsWithIntegrityLevels()
{
    PropertyStores stores;
    const uint64_t number = bitsOf(jsNumber(42));
    constexpr uint32_t sealed = orderWordIsSealed | orderWordIsNotExtensible;
    constexpr uint32_t frozen = orderWordIsFrozen | sealed;
    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            for (Slot::Kind kind : { Slot::Kind::Field, Slot::Kind::FieldCreation }) {
                StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
                stores.file(storeCase, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfAnything));

                FakeTypedObject notExtensibleObject(JSValue::ValueUndefined, orderWordIsNotExtensible);
                AOT_TEST_CHECK(!stores.store(storeCase, notExtensibleObject.address(), number).calledOperation && notExtensibleObject.field() == number);

                FakeTypedObject sealedObject(JSValue::ValueUndefined, sealed);
                if (mode == PutMode::Define) {
                    checkUncached(stores, storeCase, sealedObject.address(), number);
                    AOT_TEST_CHECK(sealedObject.field() == JSValue::ValueUndefined);
                } else
                    AOT_TEST_CHECK(!stores.store(storeCase, sealedObject.address(), number).calledOperation && sealedObject.field() == number);

                FakeTypedObject frozenObject(JSValue::ValueUndefined, frozen);
                checkUncached(stores, storeCase, frozenObject.address(), number);
                AOT_TEST_CHECK(frozenObject.field() == JSValue::ValueUndefined);
            }
        }
    }
}

// Slot::remembersOrder: in place of the type stands the order word of the object for which the slot was made, and the location is the number of the order that it got.
static void testFieldsCreatedBySlotsThatRememberTheOrder()
{
    PropertyStores stores;
    Instance& instance = stores.runner.instance();
    const uint64_t before = bitsOf(jsNumber(7));
    const uint64_t number = bitsOf(jsNumber(42));
    constexpr uint32_t slotKey = slotKeyFor(FakeTypedObject::offsetOfField, false);
    constexpr uint32_t remembered = orderWordOf(3, 1);
    constexpr uint32_t other = orderWordOf(4, 2);
    std::array<KeyOrderTransition, 16> transitions { };
    instance.objects.keyOrderTransitions = transitions.data();
    instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(transitions.size() - 1);

    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            for (uint32_t flagOfObject : { 0u, orderWordHasExtraProperties }) {
                StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
                const Slot slot = Slot::createField(toStructureID(PropertyStores::structureID), Slot::Kind::FieldCreation, Slot::isOwn | Slot::remembersOrder,
                    SlotData::create(FakeTypedObject::offsetOfField, SlotRepresentation::JSValue), remembered | flagOfObject, 5);
                AOT_TEST_CHECK(!PackedSlot::tryPack(slot, PropertyStores::nameOf(storeCase), instance.cellCageBase));
                stores.file(storeCase, slot);
                auto orderWordOfObject = [](FakeTypedObject& object) { return object.at<uint32_t>(offsetOfOrderWord); };

                // The order word that the slot remembers: one key more, of the order that it remembers, and the other bits as they were.
                FakeTypedObject created(0, remembered | flagOfObject);
                AOT_TEST_CHECK(!stores.store(storeCase, created.address(), number).calledOperation);
                AOT_TEST_CHECK(created.field() == number && orderWordOfObject(created) == (orderWordOf(5, 2) | flagOfObject) && created.wordAfterFieldIsUntouched());

                // The field is there: the value takes the place of the other, whatever the order word.
                for (uint32_t orderWord : { remembered | flagOfObject, other, other | orderWordIsNotExtensible }) {
                    FakeTypedObject replaced(before, orderWord);
                    AOT_TEST_CHECK(!stores.store(storeCase, replaced.address(), number).calledOperation && replaced.field() == number && orderWordOfObject(replaced) == orderWord);
                }
                FakeTypedObject frozen(before, remembered | orderWordIsFrozen);
                checkUncached(stores, storeCase, frozen.address(), number);
                AOT_TEST_CHECK(frozen.field() == before);

                // Any bit more and the order word is another: an object that takes no new field is left alone.
                for (uint32_t bit : { orderWordIsNotExtensible, orderWordIsSealed | orderWordIsNotExtensible }) {
                    FakeTypedObject closed(0, remembered | flagOfObject | bit);
                    checkUncached(stores, storeCase, closed.address(), number);
                    AOT_TEST_CHECK(!closed.field() && orderWordOfObject(closed) == (remembered | flagOfObject | bit));
                }

                // Another order word: as by a slot that remembers nothing: the value is stored, and where no transition is known the run time lists the key.
                FakeTypedObject elsewhere(0, other);
                stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
                StubRun run = stores.store(storeCase, elsewhere.address(), number);
                stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == elsewhere.address() && static_cast<uint32_t>(run.recordedArguments[1]) == slotKey);
                AOT_TEST_CHECK(elsewhere.field() == number && orderWordOfObject(elsewhere) == other);
            }
        }
    }
}

static void checkKeyedStoreLeftTo(PropertyStores& stores, StubKey stub, Entry operation, FakeTypedObject& object, uint64_t key, uint64_t value)
{
    uint64_t wordBefore = object.field();
    stores.runner.recordCallsOf(operation);
    StubRun run = stores.runner.run(stub, { object.address(), key, value });
    stores.runner.countCallsOf(operation);
    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == key && run.recordedArguments[2] == value);
    AOT_TEST_CHECK(object.field() == wordBefore);
}

static void testKeyedStoresToObjectsWithIntegrityLevels()
{
    PropertyStores stores;
    const uint64_t number = bitsOf(jsNumber(42));
    constexpr uint32_t sealed = orderWordIsSealed | orderWordIsNotExtensible;
    constexpr uint32_t frozen = orderWordIsFrozen | sealed;
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = stores.uidOf(PropertyStores::unboundName);

    for (PutMode mode : allPutModes) {
        StubKey stub = mode == PutMode::Define ? defineByValCached() : putByValCached(mode);
        Entry uncachedOperation = mode == PutMode::Define ? Entry::operationAOTDefineField : Entry::operationAOTPutByVal;
        for (Slot::Kind kind : { Slot::Kind::Field, Slot::Kind::FieldCreation }) {
            stores.file({ mode, FiledIn::HashedTable, false }, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfAnything));

            FakeTypedObject object(JSValue::ValueUndefined);
            AOT_TEST_CHECK(!stores.runner.run(stub, { object.address(), key.address(), number }).calledOperation && object.field() == number);

            FakeTypedObject sealedObject(JSValue::ValueUndefined, sealed);
            if (mode == PutMode::Define)
                checkKeyedStoreLeftTo(stores, stub, uncachedOperation, sealedObject, key.address(), number);
            else
                AOT_TEST_CHECK(!stores.runner.run(stub, { sealedObject.address(), key.address(), number }).calledOperation && sealedObject.field() == number);

            FakeTypedObject frozenObject(JSValue::ValueUndefined, frozen);
            checkKeyedStoreLeftTo(stores, stub, uncachedOperation, frozenObject, key.address(), number);
        }
    }
}

static void testKeyedStoresUnderSymbolsWhoseHoldersArePrepared()
{
    PropertyStores stores;
    const uint64_t number = bitsOf(jsNumber(42));
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint8_t>(JSCell::typeInfoFlagsOffset()) = TypeInfoPerCellBit;
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = stores.uidOf(PropertyStores::unboundName);

    for (PutMode mode : allPutModes) {
        StubKey stub = mode == PutMode::Define ? defineByValCached() : putByValCached(mode);
        Entry cachingOperation = mode == PutMode::Define ? Entry::operationAOTDefineField : mode == PutMode::Strict ? Entry::operationAOTPutByValAndCache : Entry::operationAOTPutByValAndCacheSloppy;
        for (Slot::Kind kind : { Slot::Kind::Field, Slot::Kind::FieldCreation }) {
            stores.file({ mode, FiledIn::HashedTable, false }, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfAnything));
            FakeTypedObject object(JSValue::ValueUndefined);
            checkKeyedStoreLeftTo(stores, stub, cachingOperation, object, key.address(), number);
        }
    }
}

namespace {

struct CreatedFieldOrders {
    static constexpr uint32_t shapeWord = shapeWordOf(9);
    static constexpr uint32_t orderBefore = orderWordOf(3, 1);
    static constexpr uint32_t orderAfter = orderWordOf(3, 2);

    CreatedFieldOrders(Instance& instance, uint32_t slotKey)
    {
        constexpr uint64_t wordsBefore = static_cast<uint64_t>(shapeWord) << 32 | orderBefore;
        const uint32_t key = transitionKeyFor(slotKey, 0);
        instance.objects.keyOrderTransitions = m_transitions.data();
        instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(m_transitions.size() - 1);
        m_transitions[KeyOrderTransition::hashOf(wordsBefore, key) & instance.objects.keyOrderTransitionMask] = { wordsBefore, key, orderAfter };
    }

private:
    std::array<KeyOrderTransition, 16> m_transitions { };
};

struct FakeObjectWithOrder : FakeTypedObject {
    explicit FakeObjectWithOrder(uint64_t bitsOfField, uint32_t flags = 0)
        : FakeTypedObject(bitsOfField, CreatedFieldOrders::orderBefore | flags)
    {
        at<uint32_t>(offsetOfShapeWord) = CreatedFieldOrders::shapeWord;
    }

    uint32_t orderWord() { return at<uint32_t>(offsetOfOrderWord); }
};

struct StorageOnRequest {
    uint32_t* extensionWord;
    uint32_t encodedExtension;
};

} // anonymous namespace

static void testCreatedFields()
{
    PropertyStores stores;
    CreatedFieldOrders orders(stores.runner.instance(), slotKeyFor(FakeTypedObject::offsetOfField, false));
    const uint64_t number = bitsOf(jsNumber(42));
    const uint64_t otherNumber = bitsOf(jsNumber(43));
    const uint64_t boolean = bitsOf(jsBoolean(true));
    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
            const Slot slot = createFieldSlot(Slot::Kind::FieldCreation, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers);
            stores.file(storeCase, slot);

            FakeObjectWithOrder object(JSValue::ValueEmpty);
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), number).calledOperation && object.field() == number && object.orderWord() == CreatedFieldOrders::orderAfter);
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), otherNumber).calledOperation && object.field() == otherNumber && object.orderWord() == CreatedFieldOrders::orderAfter);
            AOT_TEST_CHECK(object.wordAfterFieldIsUntouched());

            for (uint64_t bitsBefore : { static_cast<uint64_t>(JSValue::ValueEmpty), number }) {
                FakeObjectWithOrder objectGivenOtherType(bitsBefore);
                stores.runner.recordCallsOf(Entry::operationAOTCreateField);
                StubRun run = stores.store(storeCase, objectGivenOtherType.address(), boolean);
                stores.runner.countCallsOf(Entry::operationAOTCreateField);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == objectGivenOtherType.address());
                AOT_TEST_CHECK(run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits() && run.recordedArguments[3] == boolean);
                AOT_TEST_CHECK(static_cast<uint32_t>(run.recordedArguments[4]) == static_cast<uint32_t>(RuntimeCounterID::StubCannotCreateField));
                AOT_TEST_CHECK(objectGivenOtherType.field() == bitsBefore && objectGivenOtherType.orderWord() == CreatedFieldOrders::orderBefore);
            }

            for (uint64_t value : { number, boolean }) {
                FakeObjectWithOrder notExtensibleObject(JSValue::ValueEmpty, orderWordIsNotExtensible);
                checkUncached(stores, storeCase, notExtensibleObject.address(), value);
                AOT_TEST_CHECK(!notExtensibleObject.field() && notExtensibleObject.orderWord() == (CreatedFieldOrders::orderBefore | orderWordIsNotExtensible));
            }
        }
    }
}

static StorageOnRequest* storageOnRequest;

static UGPRPair JIT_OPERATION_ATTRIBUTES giveStorageOnRequest(Instance* instance, uint64_t object, uint64_t granuleBits)
{
    StubEnvironment& environment = StubEnvironment::of(instance);
    ++environment.numberOfOperationCalls;
    ++environment.numberOfRecordedCalls;
    environment.recordedArguments = { object, granuleBits, 0, 0, 0 };
    *storageOnRequest->extensionWord = storageOnRequest->encodedExtension;
    return makeUGPRPair(0, 0);
}

static void testCreatedFieldsWithoutStorage()
{
    constexpr uint32_t offsetOfExtensionWord = FakeTypedObject::offsetOfField + sizeof(uint64_t);
    constexpr uint32_t offsetInExtension = offsetOfFirstExtensionSlot + 2 * extensionGranuleSize;
    constexpr uint64_t granuleBit = granuleBitOf(offsetInExtension);
    using FakeExtension = FakeObject<extensionCellSizeFor(2)>;
    PropertyStores stores;
    stores.structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfExtensionWord, SlotRepresentation::Cell32).encoded();
    // A space without allocators: the stub that grows an Extension leaves it to the operation.
    FakeObject<sizeof(CompleteSubspace)> spaceWithoutAllocators;
    stores.runner.instance().allocation.cagedAuxiliarySpace = std::bit_cast<void*>(spaceWithoutAllocators.address());
    CreatedFieldOrders orders(stores.runner.instance(), slotKeyFor(offsetInExtension, true));
    const uintptr_t cageBase = stores.runner.instance().cellCageBase;
    const uint64_t number = bitsOf(jsNumber(42));
    const uint64_t undefined = bitsOf(jsUndefined());
    auto createSlot = [](uint8_t flags) {
        return Slot::createField(toStructureID(PropertyStores::structureID), Slot::Kind::FieldCreation, Slot::isOwn | Slot::isOutOfLine | flags, SlotData::create(offsetInExtension, SlotRepresentation::JSValue), PropertyStores::typeOfAnything, offsetOfExtensionWord);
    };
    const Slot slot = createSlot(0);
    const Slot slotOfFieldThatNeedsNoStorage = createSlot(Slot::undefinedNeedsNoStorage);
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = stores.uidOf(PropertyStores::unboundName);

    struct ExtensionBefore {
        uint64_t granuleBits;
        bool hasRoom;
        uint32_t offsetInReplacement;
    };
    constexpr ExtensionBefore extensionsBefore[] = {
        { granuleBit >> 1 | granuleBit >> 2, true, 0 },
        { granuleBit << 1, false, offsetOfFirstExtensionSlot },
        { granuleBit >> 1, false, offsetOfFirstExtensionSlot + extensionGranuleSize },
    };
    auto encode = [&](FakeExtension& extension) {
        return MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
    };
    auto fill = [](FakeExtension& extension, uint64_t bits) {
        extension.at<uint64_t>(offsetOfExtensionGranuleBits) = bits;
        for (uint32_t offset = offsetOfFirstExtensionSlot; offset < extensionCellSizeFor(2); offset += extensionGranuleSize)
            extension.at<uint64_t>(offset) = JSValue::ValueEmpty;
    };
    auto holdsNothing = [](FakeExtension& extension, uint64_t bits) {
        for (uint32_t offset = offsetOfFirstExtensionSlot; offset < extensionCellSizeFor(2); offset += extensionGranuleSize) {
            if (extension.at<uint64_t>(offset) != JSValue::ValueEmpty)
                return false;
        }
        return extension.at<uint64_t>(offsetOfExtensionGranuleBits) == bits;
    };

    for (PutMode mode : allPutModes) {
        for (bool isKeyed : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, false };
            auto store = [&](FakeTypedObject& object, uint64_t value) {
                if (isKeyed)
                    return stores.runner.run(mode == PutMode::Define ? defineByValCached() : putByValCached(mode), { object.address(), key.address(), value });
                return stores.store(storeCase, object.address(), value);
            };
            for (const ExtensionBefore& before : extensionsBefore) {
                const uint64_t bitsBefore = before.granuleBits;
                for (auto [filedSlot, value] : { std::pair { &slot, number }, std::pair { &slot, undefined }, std::pair { &slotOfFieldThatNeedsNoStorage, number } }) {
                    stores.file(storeCase, *filedSlot);
                    FakeExtension extension;
                    fill(extension, bitsBefore);
                    FakeExtension replacement;
                    fill(replacement, bitsBefore | granuleBit);
                    const uint32_t wordBefore = encode(extension);

                    FakeObjectWithOrder object(untouched);
                    uint32_t& extensionWord = object.at<uint32_t>(offsetOfExtensionWord);
                    extensionWord = wordBefore;
                    StorageOnRequest storage { &extensionWord, encode(replacement) };
                    storageOnRequest = &storage;
                    stores.runner.install(Entry::operationAOTEnsureExtensionGranules, giveStorageOnRequest);
                    StubRun run = store(object, value);
                    if (before.hasRoom) {
                        AOT_TEST_CHECK(!run.numberOfRecordedCalls && extensionWord == wordBefore && extension.at<uint64_t>(offsetOfExtensionGranuleBits) == (bitsBefore | granuleBit));
                        AOT_TEST_CHECK(extension.at<uint64_t>(offsetInExtension) == value && !extension.at<uint64_t>(offsetOfFirstExtensionSlot));
                    } else {
                        AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == granuleBit);
                        AOT_TEST_CHECK(holdsNothing(extension, bitsBefore) && replacement.at<uint64_t>(before.offsetInReplacement) == value);
                    }
                    AOT_TEST_CHECK(object.orderWord() == CreatedFieldOrders::orderAfter && object.field() == untouched);
                    AOT_TEST_CHECK(!store(object, value).calledOperation && object.orderWord() == CreatedFieldOrders::orderAfter);

                    FakeExtension extensionOfNotExtensibleObject;
                    fill(extensionOfNotExtensibleObject, bitsBefore);
                    FakeObjectWithOrder notExtensibleObject(untouched, orderWordIsNotExtensible);
                    notExtensibleObject.at<uint32_t>(offsetOfExtensionWord) = encode(extensionOfNotExtensibleObject);
                    run = store(notExtensibleObject, value);
                    AOT_TEST_CHECK(run.calledOperation && !run.numberOfRecordedCalls && holdsNothing(extensionOfNotExtensibleObject, bitsBefore));
                    stores.runner.countCallsOf(Entry::operationAOTEnsureExtensionGranules);
                }

                // The key order alone says that the field exists, and without a granule it reads as undefined.
                stores.file(storeCase, slotOfFieldThatNeedsNoStorage);
                FakeExtension extension;
                fill(extension, bitsBefore);
                const uint32_t wordBefore = encode(extension);
                FakeObjectWithOrder object(untouched);
                uint32_t& extensionWord = object.at<uint32_t>(offsetOfExtensionWord);
                extensionWord = wordBefore;
                AOT_TEST_CHECK(!store(object, undefined).calledOperation && object.orderWord() == CreatedFieldOrders::orderAfter);
                AOT_TEST_CHECK(extensionWord == wordBefore && holdsNothing(extension, bitsBefore) && object.field() == untouched);

                // No transition leads on from the order that lists the field.
                stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
                StubRun run = store(object, undefined);
                stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == slotKeyFor(offsetInExtension, true));
                AOT_TEST_CHECK(extensionWord == wordBefore && holdsNothing(extension, bitsBefore) && object.orderWord() == CreatedFieldOrders::orderAfter);

                FakeObjectWithOrder notExtensibleObject(untouched, orderWordIsNotExtensible);
                notExtensibleObject.at<uint32_t>(offsetOfExtensionWord) = wordBefore;
                AOT_TEST_CHECK(store(notExtensibleObject, undefined).calledOperation && holdsNothing(extension, bitsBefore));
                AOT_TEST_CHECK(notExtensibleObject.orderWord() == (CreatedFieldOrders::orderBefore | orderWordIsNotExtensible));
            }
        }
    }
}

static void testUndefinedAndNullInFieldsOfOtherTypes()
{
    constexpr std::pair<Slot::Kind, Entry> kinds[] = {
        { Slot::Kind::Field, Entry::operationAOTStoreField },
        { Slot::Kind::FieldCreation, Entry::operationAOTCreateField },
    };
    PropertyStores stores;
    CreatedFieldOrders orders(stores.runner.instance(), slotKeyFor(FakeTypedObject::offsetOfField, false));
    const uintptr_t cageBase = stores.runner.instance().cellCageBase;
    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, nameIsBound };
            for (auto [kind, operationThatFinishes] : kinds) {
                for (SlotRepresentation representation : { SlotRepresentation::JSValue, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalDouble, SlotRepresentation::OptionalInt64, SlotRepresentation::Int32, SlotRepresentation::Double }) {
                    const Slot slot = createFieldSlot(kind, representation, PropertyStores::typeOfNumbers);
                    stores.file(storeCase, slot);
                    ExpectedStore number = expectedStoreOf(representation, jsNumber(7), untouched, cageBase);
                    AOT_TEST_CHECK(number.isWritten);
                    for (JSValue value : { jsUndefined(), jsNull() }) {
                        for (bool cellHasMark : { true, false }) {
                            ExpectedStore expected = expectedStoreOf(representation, value, number.word, cageBase);
                            AOT_TEST_CHECK(expected.isWritten == traitsOf(representation).hasUndefined);
                            FakeTypedObject object(number.word);
                            if (cellHasMark)
                                object.at<uint8_t>(JSCell::typeInfoFlagsOffset()) |= TypeInfoPerCellBit;
                            stores.runner.recordCallsOf(operationThatFinishes);
                            StubRun run = stores.store(storeCase, object.address(), bitsOf(value));
                            stores.runner.countCallsOf(operationThatFinishes);
                            bool isWrittenByStub = expected.isWritten && cellHasMark;
                            AOT_TEST_CHECK(object.field() == (isWrittenByStub ? expected.word : number.word) && object.wordAfterFieldIsUntouched());
                            if (isWrittenByStub) {
                                AOT_TEST_CHECK(!run.calledOperation);
                                continue;
                            }
                            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address());
                            AOT_TEST_CHECK(run.recordedArguments[1] == slot.headerBits() && run.recordedArguments[2] == slot.payloadBits() && run.recordedArguments[3] == bitsOf(value));
                        }
                    }
                }
            }

            stores.file(storeCase, createFieldSlot(Slot::Kind::FieldCreation, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers));
            for (JSValue value : { jsUndefined(), jsNull() }) {
                FakeObjectWithOrder object(JSValue::ValueEmpty);
                object.at<uint8_t>(JSCell::typeInfoFlagsOffset()) |= TypeInfoPerCellBit;
                AOT_TEST_CHECK(!stores.store(storeCase, object.address(), bitsOf(value)).calledOperation);
                AOT_TEST_CHECK(object.field() == bitsOf(value) && object.orderWord() == CreatedFieldOrders::orderAfter && object.wordAfterFieldIsUntouched());
            }
        }
    }
}

namespace {

// Both names are values in the cell of shapes 1 and 2, values in the Extension of shape 3 and integers in the cell of shapes 5 and 6. A store to shape 2 or 6 has to test a type: that of shape 2 takes numbers, that of shape 6 no integer. Shape 4 has neither name.
class FieldTable {
    WTF_MAKE_NONCOPYABLE(FieldTable);
public:
    static constexpr uint32_t shapeThatTakesAnyValue = 1;
    static constexpr uint32_t shapeWithTypedField = 2;
    static constexpr uint32_t shapeWithOutOfLineValue = 3;
    static constexpr uint32_t shapeWithoutTheNames = 4;
    static constexpr uint32_t shapeWithInt32 = 5;
    static constexpr uint32_t shapeWithInt32OfNarrowerType = 6;
    static constexpr uint32_t highestShape = 6;
    static constexpr uint32_t offsetInExtension = 2 * sizeof(uint64_t);

    explicit FieldTable(PropertyStores& stores)
    {
        static_assert(PropertyStores::unboundName == PropertyStores::boundName + 1);
        using Entry = FieldTableEntry;
        for (uint32_t shape = 1; shape <= highestShape; ++shape)
            m_table[highestShape - shape] = rowOf(shape);
        for (uint16_t name : { PropertyStores::boundName, PropertyStores::unboundName }) {
            for (uint32_t shape : { shapeThatTakesAnyValue, shapeWithTypedField })
                m_table[highestShape + rowOf(shape) + name] = Entry::create(name, Entry::Form::InlineValue, FakeTypedObject::offsetOfField / sizeof(uint64_t)).bits;
            m_table[highestShape + rowOf(shapeWithOutOfLineValue) + name] = Entry::create(name, Entry::Form::OutOfLineValue, offsetInExtension / sizeof(uint64_t)).bits;
            for (uint32_t shape : { shapeWithInt32, shapeWithInt32OfNarrowerType })
                m_table[highestShape + rowOf(shape) + name] = Entry::create(name, Entry::Form::InlineInt32, FakeTypedObject::offsetOfField / sizeof(uint32_t)).bits;
            constexpr std::tuple<uint32_t, SlotRepresentation, uint32_t, bool> records[] = {
                { shapeThatTakesAnyValue, SlotRepresentation::JSValue, PropertyStores::typeOfAnything, true },
                { shapeWithTypedField, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers, false },
                { shapeWithOutOfLineValue, SlotRepresentation::JSValue, PropertyStores::typeOfAnything, true },
                { shapeWithInt32, SlotRepresentation::OptionalInt32, PropertyStores::typeOfNumbers, true },
                { shapeWithInt32OfNarrowerType, SlotRepresentation::OptionalInt32, PropertyStores::typeOfBooleans, false },
            };
            // All shapes have one Structure here, and so one layout: each has a record for each name.
            uint16_t index = name - PropertyStores::boundName;
            for (auto [shape, representation, type, isStoredWithoutTypeTest] : records) {
                bool isOutOfLine = shape == shapeWithOutOfLineValue;
                FieldData& record = stores.fieldRecord(index);
                record.name = name;
                record.flags = isOutOfLine ? fieldIsOutOfLine : 0;
                record.slot = SlotData::create(isOutOfLine ? offsetInExtension : FakeTypedObject::offsetOfField, representation);
                record.type = type;
                m_fields[rowOf(shape) + name] = index | (isStoredWithoutTypeTest ? FieldRecordIndex::isStoredWithoutTypeTest : 0);
                index += 2;
            }
        }
        stores.layout().numberOfFields = PropertyStores::numberOfFieldRecords;
        stores.structure.at<uint32_t>(CompiledStructure::offsetOfExtensionSlot()) = SlotData::create(offsetOfFirstField, SlotRepresentation::Cell32).encoded();
        Instance& instance = stores.runner.instance();
        instance.image.fieldTableEntries = m_table.data() + highestShape;
        instance.image.fieldRecordIndices = m_fields.data();
    }

    static constexpr uint16_t recordOf(uint32_t shape, uint16_t name)
    {
        constexpr uint32_t shapesWithRecords[] = { shapeThatTakesAnyValue, shapeWithTypedField, shapeWithOutOfLineValue, shapeWithInt32, shapeWithInt32OfNarrowerType };
        uint16_t index = name - PropertyStores::boundName;
        for (uint32_t other : shapesWithRecords) {
            if (other == shape)
                return index;
            index += 2;
        }
        RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
    }

private:
    static constexpr uint32_t entriesBetweenRows = 2;
    static constexpr uint32_t numberOfEntries = entriesBetweenRows * highestShape + PropertyStores::numberOfNames;
    static uint32_t rowOf(uint32_t shape) { return entriesBetweenRows * shape; }

    std::array<uint32_t, highestShape + numberOfEntries> m_table { };
    std::array<uint16_t, numberOfEntries> m_fields { };
};

struct FakeObjectWithShape : FakeTypedObject {
    FakeObjectWithShape(uint32_t shape, uint64_t bitsOfField, uint32_t orderWord = 0)
        : FakeTypedObject(bitsOfField, orderWord)
    {
        at<uint32_t>(offsetOfShapeWord) = shapeWordOf(shape);
    }
};

// Holds one granule. The word behind it belongs to the next cell.
struct FakeExtension : FakeObject<extensionCellSizeFor(1) + minExtensionSize> {
    static constexpr uint64_t bitOfSlot = granuleBitOf(FieldTable::offsetInExtension);

    FakeExtension(uint64_t granuleBit, uint64_t bitsOfGranule)
    {
        at<uint64_t>(offsetOfExtensionGranuleBits) = granuleBit;
        granule() = bitsOfGranule;
        at<uint64_t>(offsetOfFirstExtensionSlot + extensionGranuleSize) = untouched;
    }

    uint64_t& granule() { return at<uint64_t>(offsetOfFirstExtensionSlot); }
    bool wordAfterGranuleIsUntouched() { return at<uint64_t>(offsetOfFirstExtensionSlot + extensionGranuleSize) == untouched; }
    uint32_t encode(uintptr_t cageBase) const { return MarkedBlockCage::encode(cageBase, std::bit_cast<JSCell*>(address() - offsetOfEmptyExtensionInCage)); }
};

} // anonymous namespace

template<typename Functor>
static void forEachStoreWithEmptyCaches(PropertyStores& stores, const Functor& functor)
{
    stores.clearCaches();
    for (PutMode mode : allPutModes) {
        for (bool nameIsBound : { false, true })
            functor(StoreCase { mode, FiledIn::Front, nameIsBound });
    }
}

static void testFieldsFoundInFieldTable()
{
    PropertyStores stores;
    FieldTable table(stores);
    uintptr_t cageBase = stores.runner.instance().cellCageBase;
    FakeObject<16> cell;
    cell.setHeader(0, FinalObjectType);
    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        for (uint64_t value : { bitsOf(jsNumber(42)), bitsOf(jsUndefined()), static_cast<uint64_t>(cell.address()) }) {
            FakeObjectWithShape object(FieldTable::shapeThatTakesAnyValue, bitsOf(jsNumber(1)));
            StubRun run = stores.store(storeCase, object.address(), value);
            AOT_TEST_CHECK(!run.calledOperation && object.field() == value && object.wordAfterFieldIsUntouched());

            FakeExtension extension(FakeExtension::bitOfSlot, bitsOf(jsNumber(1)));
            const uint64_t reference = extension.encode(cageBase);
            FakeObjectWithShape withExtension(FieldTable::shapeWithOutOfLineValue, reference);
            run = stores.store(storeCase, withExtension.address(), value);
            AOT_TEST_CHECK(!run.calledOperation && extension.granule() == value && extension.wordAfterGranuleIsUntouched() && withExtension.field() == reference);
        }
        for (int32_t value : { 0, 42, -1, INT32_MAX, minInlineInt32 }) {
            for (int32_t before : { 1, undefinedInt32, nullInt32 }) {
                FakeObjectWithShape object(FieldTable::shapeWithInt32, untouched << 32 | static_cast<uint32_t>(before));
                StubRun run = stores.store(storeCase, object.address(), bitsOf(jsNumber(value)));
                AOT_TEST_CHECK(!run.calledOperation && object.field() == (untouched << 32 | static_cast<uint32_t>(value)) && object.wordAfterFieldIsUntouched());
            }
        }
        for (uint32_t orderWord : { orderWordHasExtraProperties, orderWordIsNotExtensible }) {
            FakeObjectWithShape object(FieldTable::shapeThatTakesAnyValue, bitsOf(jsNumber(1)), orderWord);
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), bitsOf(jsNumber(42))).calledOperation && object.field() == bitsOf(jsNumber(42)));
        }
        FakeObjectWithShape sealed(FieldTable::shapeThatTakesAnyValue, bitsOf(jsNumber(1)), orderWordIsSealed | orderWordIsNotExtensible);
        bool isStored = !stores.store(storeCase, sealed.address(), bitsOf(jsNumber(42))).calledOperation;
        AOT_TEST_CHECK(isStored == (storeCase.mode != PutMode::Define) && sealed.field() == bitsOf(jsNumber(isStored ? 42 : 1)));
    });
}

namespace {

struct FakeObjectWithShapeAndOrder : FakeObjectWithShape {
    FakeObjectWithShapeAndOrder(uint32_t shape, uint64_t bitsOfField, uint32_t orderWord)
        : FakeObjectWithShape(shape, bitsOfField, orderWord)
    {
    }

    uint32_t orderWord() { return at<uint32_t>(offsetOfOrderWord); }
};

// What a store that creates a property asks about the objects above its receiver.
class Chains {
    WTF_MAKE_NONCOPYABLE(Chains);
public:
    static constexpr uint32_t generation = 7;
    static constexpr uint32_t changesUnderNames = 3;

    explicit Chains(PropertyStores& stores)
        : m_stores(stores)
    {
        PropertyCacheWords& words = stores.runner.instance().propertyCaches;
        words.absenceFilters = m_filters.data();
        words.absenceFilterGenerationForStores = generation;
        words.changesUnderNamesOnChains = changesUnderNames;
        words.namesOfObjectPrototypes = &namesOfObjectPrototypes;
        words.namesChangedOnChains = &namesChangedOnChains;
        letEveryStorePass();
    }

    ~Chains()
    {
        setFlags(0);
    }

    AbsenceFilter& filter() { return m_filters[AbsenceFilter::indexFor(PropertyStores::structureID)]; }
    void letEveryStorePass() { filter() = { PropertyStores::structureID, generation, { }, changesUnderNames, 0 }; }
    void setFlags(uint8_t flags) { m_stores.structure.at<uint8_t>(CompiledStructure::offsetOfFlags()) = flags; }

    uint64_t namesOfObjectPrototypes { 0 };
    uint64_t namesChangedOnChains { 0 };

private:
    PropertyStores& m_stores;
    std::array<AbsenceFilter, AbsenceFilter::numberOfFilters> m_filters { };
};

} // anonymous namespace

static void testStoresLeftToTheCaches()
{
    PropertyStores stores;
    FieldTable table(stores);
    CreatedFieldOrders orders(stores.runner.instance(), slotKeyFor(FakeTypedObject::offsetOfField, false));
    Chains chains(stores);
    uintptr_t cageBase = stores.runner.instance().cellCageBase;
    constexpr uint32_t frozen = orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible;
    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        auto isLeft = [&](uint32_t shape, uint64_t bitsOfField, uint32_t orderWord, JSValue value = jsNumber(42)) {
            FakeObjectWithShape object(shape, bitsOfField, orderWord);
            StubRun run = stores.store(storeCase, object.address(), bitsOf(value));
            return run.calledOperation && object.field() == bitsOfField && object.wordAfterFieldIsUntouched() && object.at<uint32_t>(offsetOfOrderWord) == orderWord;
        };
        AOT_TEST_CHECK(isLeft(FieldTable::shapeThatTakesAnyValue, bitsOf(jsNumber(1)), frozen));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), frozen));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), 0, jsBoolean(true)));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, 0, 0, jsBoolean(true)));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithoutTheNames, bitsOf(jsNumber(1)), 0));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithInt32, 1, frozen));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithInt32OfNarrowerType, 1, 0));
        // An object that takes no more properties, or that has a field outside its slot, gets no field from a stub that knows the field and nothing else.
        for (uint32_t orderWord : { orderWordIsNotExtensible, orderWordIsSealed | orderWordIsNotExtensible, frozen, orderWordHasFieldOutsideSlot | orderWordHasExtraProperties }) {
            AOT_TEST_CHECK(isLeft(FieldTable::shapeThatTakesAnyValue, 0, orderWord));
            AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, 0, orderWord));
            AOT_TEST_CHECK(isLeft(FieldTable::shapeWithInt32, static_cast<uint32_t>(absentInt32), orderWord));
        }
        for (JSValue value : { jsDoubleNumber(0.5), jsUndefined(), jsNull(), jsBoolean(true), jsNumber(minInlineInt32 - 1), jsNumber(INT32_MIN) }) {
            FakeObjectWithShape object(FieldTable::shapeWithInt32, 1);
            AOT_TEST_CHECK(stores.store(storeCase, object.address(), bitsOf(value)).calledOperation && object.field() == 1);
        }

        // The run time reports a field that a prototype gets.
        stores.structure.at<uint32_t>(Structure::bitFieldOffset()) = Structure::s_mayBePrototypeBits;
        AOT_TEST_CHECK(isLeft(FieldTable::shapeThatTakesAnyValue, 0, CreatedFieldOrders::orderBefore));
        stores.structure.at<uint32_t>(Structure::bitFieldOffset()) = 0;

        // What the layout gives another form than that of a plain property.
        for (uint16_t flags : { fieldIsReadOnly, fieldIsNotEnumerable, fieldIsNotConfigurable, fieldIsAccessor, fieldIsPrivate }) {
            FieldData& record = stores.fieldRecord(FieldTable::recordOf(FieldTable::shapeWithTypedField, PropertyStores::nameOf(storeCase)));
            record.flags = flags;
            AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), 0));
            AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, 0, 0));
            record.flags = 0;
        }

        // The one store of a field that is stored once creates it, here. A store that finds the slot filled is left to the run time, which takes the property out of its slot.
        {
            FieldData& record = stores.fieldRecord(FieldTable::recordOf(FieldTable::shapeWithTypedField, PropertyStores::nameOf(storeCase)));
            record.flags = fieldIsStoredOnce;
            AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), 0));
            FakeObjectWithShape created(FieldTable::shapeWithTypedField, 0, 0);
            stores.store(storeCase, created.address(), bitsOf(jsNumber(42)));
            AOT_TEST_CHECK(created.field() == bitsOf(jsNumber(42)) && created.wordAfterFieldIsUntouched());
            record.flags = 0;
        }

        auto isLeftWithExtension = [&](uint64_t granuleBit, uint64_t bitsOfGranule, uint32_t orderWord) {
            FakeExtension extension(granuleBit, bitsOfGranule);
            FakeObjectWithShape object(FieldTable::shapeWithOutOfLineValue, extension.encode(cageBase), orderWord);
            return stores.store(storeCase, object.address(), bitsOf(jsNumber(42))).calledOperation && extension.granule() == bitsOfGranule && extension.wordAfterGranuleIsUntouched();
        };
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot, 0, orderWordIsNotExtensible));
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot >> 1, bitsOf(jsNumber(1)), orderWordIsNotExtensible));
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot, bitsOf(jsNumber(1)), frozen));

        FakeTypedObject withoutShape(bitsOf(jsNumber(1)));
        AOT_TEST_CHECK(stores.store(storeCase, withoutShape.address(), bitsOf(jsNumber(42))).calledOperation && withoutShape.field() == bitsOf(jsNumber(1)));
    });
}

// The table of fields names the record of a field, and the record says what the slot admits: nothing is filed for such a store, and none is a first one.
static void testFieldsStoredByTheirRecords()
{
    PropertyStores stores;
    FieldTable table(stores);
    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        for (JSValue value : { jsNumber(42), jsDoubleNumber(0.5) }) {
            FakeObjectWithShape object(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)));
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), bitsOf(value)).calledOperation && object.field() == bitsOf(value) && object.wordAfterFieldIsUntouched());
        }
        for (uint32_t orderWord : { orderWordHasExtraProperties, orderWordIsNotExtensible, orderWordHasFieldOutsideSlot | orderWordHasExtraProperties }) {
            FakeObjectWithShape object(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), orderWord);
            AOT_TEST_CHECK(!stores.store(storeCase, object.address(), bitsOf(jsNumber(42))).calledOperation && object.field() == bitsOf(jsNumber(42)));
        }
        FakeObjectWithShape sealed(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), orderWordIsSealed | orderWordIsNotExtensible);
        bool isStored = !stores.store(storeCase, sealed.address(), bitsOf(jsNumber(42))).calledOperation;
        AOT_TEST_CHECK(isStored == (storeCase.mode != PutMode::Define) && sealed.field() == bitsOf(jsNumber(isStored ? 42 : 1)));
    });

    // At an empty site one such store in 256 goes the way of the caches. A store that the table answers by itself counts for nothing.
    uint8_t& storesAtEmptySites = stores.runner.instance().propertyCaches.storesAtEmptySites;
    for (PutMode mode : { PutMode::Strict, PutMode::Sloppy, PutMode::Define }) {
        std::array<uint64_t, 2> site { };
        auto leavesAtSite = [&](uint32_t shape) {
            FakeObjectWithShape object(shape, bitsOf(jsNumber(1)));
            bool leaves = stores.runner.run(putByIdAtSite(mode), { object.address(), bitsOf(jsNumber(42)), std::bit_cast<uint64_t>(site.data()) }, PropertyStores::unboundName).calledOperation;
            AOT_TEST_CHECK(object.field() == bitsOf(jsNumber(leaves ? 1 : 42)));
            return leaves;
        };
        storesAtEmptySites = UINT8_MAX;
        AOT_TEST_CHECK(!leavesAtSite(FieldTable::shapeThatTakesAnyValue) && storesAtEmptySites == UINT8_MAX);
        storesAtEmptySites = UINT8_MAX - 1;
        AOT_TEST_CHECK(!leavesAtSite(FieldTable::shapeWithTypedField) && storesAtEmptySites == UINT8_MAX);
        AOT_TEST_CHECK(leavesAtSite(FieldTable::shapeWithTypedField) && !storesAtEmptySites);
    }
}

static void testFieldsCreatedByTheirRecords()
{
    PropertyStores stores;
    FieldTable table(stores);
    Instance& instance = stores.runner.instance();
    const uint64_t number = bitsOf(jsNumber(42));
    constexpr uint32_t slotKey = slotKeyFor(FakeTypedObject::offsetOfField, false);
    std::array<KeyOrderTransition, 16> transitions { };
    instance.objects.keyOrderTransitions = transitions.data();
    instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(transitions.size() - 1);
    constexpr uint32_t orderBefore = orderWordOf(3, 1);
    constexpr uint32_t orderAfter = orderWordOf(5, 2);
    auto publish = [&](uint32_t shape) {
        uint64_t wordsBefore = static_cast<uint64_t>(shapeWordOf(shape)) << 32 | orderBefore;
        uint32_t index = KeyOrderTransition::hashOf(wordsBefore, slotKey) & instance.objects.keyOrderTransitionMask;
        while (!transitions[index].isEmpty())
            index = (index + 1) & instance.objects.keyOrderTransitionMask;
        transitions[index] = { wordsBefore, slotKey, orderAfter };
    };
    constexpr std::pair<uint32_t, uint64_t> shapesAndEmptySlots[] = {
        { FieldTable::shapeThatTakesAnyValue, 0 },
        { FieldTable::shapeWithTypedField, 0 },
        { FieldTable::shapeWithInt32, untouched << 32 | static_cast<uint32_t>(absentInt32) },
    };
    for (auto [shape, emptySlot] : shapesAndEmptySlots)
        publish(shape);

    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        Chains chains(stores);
        const uint16_t name = PropertyStores::nameOf(storeCase);
        const bool asksChain = storeCase.mode != PutMode::Define;
        auto isCreated = [&](uint32_t shape, uint64_t emptySlot, uint32_t orderWordBefore, uint32_t orderWordAfter) {
            FakeObjectWithShapeAndOrder object(shape, emptySlot, orderWordBefore);
            StubRun run = stores.store(storeCase, object.address(), number);
            if (run.calledOperation) {
                AOT_TEST_CHECK(object.field() == emptySlot && object.orderWord() == orderWordBefore);
                return false;
            }
            uint64_t expected = shape == FieldTable::shapeWithInt32 ? untouched << 32 | 42 : number;
            AOT_TEST_CHECK(object.field() == expected && object.orderWord() == orderWordAfter && object.wordAfterFieldIsUntouched());
            return true;
        };
        auto isCreatedInEveryShape = [&] {
            unsigned created = 0;
            for (auto [shape, emptySlot] : shapesAndEmptySlots)
                created += isCreated(shape, emptySlot, orderBefore, orderAfter);
            AOT_TEST_CHECK(!created || created == std::size(shapesAndEmptySlots));
            return !!created;
        };

        AOT_TEST_CHECK(isCreatedInEveryShape());

        // No transition: the value is stored, and the run time lists the key.
        {
            FakeObjectWithShapeAndOrder object(FieldTable::shapeWithTypedField, 0, orderWordOf(4, 1));
            stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
            StubRun run = stores.store(storeCase, object.address(), number);
            stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == slotKey);
            AOT_TEST_CHECK(object.field() == number && object.orderWord() == orderWordOf(4, 1));
        }

        // An order of the image that goes on with the field: one key more of the same order, without a transition.
        {
            const uint16_t record = FieldTable::recordOf(FieldTable::shapeWithTypedField, name);
            stores.layout().numberOfOrders = 2;
            stores.layout().ordersOffset = PropertyStores::offsetOfOrders;
            stores.inTables<uint32_t>(PropertyStores::offsetOfOrders) = PropertyStores::offsetOfOrders + 8;
            stores.inTables<uint32_t>(PropertyStores::offsetOfOrders + 4) = PropertyStores::offsetOfOrders + 8;
            const uint16_t order[] = { 3, PropertyStores::numberOfFieldRecords, record, FieldTable::recordOf(FieldTable::shapeWithOutOfLineValue, name) };
            for (unsigned index = 0; index < std::size(order); ++index)
                stores.inTables<uint16_t>(PropertyStores::offsetOfOrders + 8 + index * sizeof(uint16_t)) = order[index];
            AOT_TEST_CHECK(isCreated(FieldTable::shapeWithTypedField, 0, orderWordOf(1, 1), orderWordOf(1, 2)));
            // The order goes on with an extra key, with another field, not at all; it is none of the image's; the object has extra properties.
            for (uint32_t orderWord : { orderWordOf(1, 0), orderWordOf(1, 2), orderWordOf(1, 3), orderWordOf(2, 1), orderWordOf(1, 1) | orderWordHasExtraProperties }) {
                FakeObjectWithShapeAndOrder object(FieldTable::shapeWithTypedField, 0, orderWord);
                stores.structure.at<uint16_t>(CompiledStructure::offsetOfNumberOfExtraKeys()) = 0;
                stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
                StubRun run = stores.store(storeCase, object.address(), number);
                stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && object.field() == number && object.orderWord() == orderWord);
            }
            stores.layout().numberOfOrders = 0;
        }

        // Below the Object.prototype of the realm the names of that object decide.
        chains.filter() = { };
        chains.setFlags(static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype));
        AOT_TEST_CHECK(isCreatedInEveryShape());
        chains.namesOfObjectPrototypes = uint64_t { 1 } << name;
        AOT_TEST_CHECK(isCreatedInEveryShape() == !asksChain);
        chains.namesOfObjectPrototypes = ~(uint64_t { 1 } << name);
        AOT_TEST_CHECK(isCreatedInEveryShape());

        // Elsewhere the filter of the Structure does.
        chains.setFlags(0);
        AOT_TEST_CHECK(isCreatedInEveryShape() == !asksChain);
        chains.letEveryStorePass();
        AOT_TEST_CHECK(isCreatedInEveryShape());
        chains.filter().namesThatStopStores[0] = uint64_t { 1 } << name;
        AOT_TEST_CHECK(isCreatedInEveryShape() == !asksChain);
        chains.filter().namesThatStopStores[0] = ~(uint64_t { 1 } << name);
        chains.filter().namesThatStopStores[1] = UINT64_MAX;
        AOT_TEST_CHECK(isCreatedInEveryShape());
        chains.filter().generationForStores = Chains::generation - 1;
        AOT_TEST_CHECK(isCreatedInEveryShape() == !asksChain);
        chains.filter().generationForStores = Chains::generation;
        chains.filter().structureID = PropertyStores::structureID + (AbsenceFilter::numberOfFilters << StructureRowWords::granuleShift);
        AOT_TEST_CHECK(isCreatedInEveryShape() == !asksChain);
        chains.filter().structureID = PropertyStores::structureID;
        // A filter built after every change reflects the name's current state.
        chains.namesChangedOnChains = uint64_t { 1 } << name;
        AOT_TEST_CHECK(isCreatedInEveryShape());
        chains.filter().changesUnderNames = Chains::changesUnderNames - 1;
        AOT_TEST_CHECK(isCreatedInEveryShape() == !asksChain);
        chains.namesChangedOnChains = ~(uint64_t { 1 } << name);
        AOT_TEST_CHECK(isCreatedInEveryShape());
        chains.filter().changesUnderNames = Chains::changesUnderNames;

        // The guard that compiled code has for the stores under the name holds once the run time has looked: one store in 16 goes there.
        uint8_t& storesToGuardedFields = stores.runner.instance().propertyCaches.storesToGuardedFields;
        storesToGuardedFields = 15 * 16;
        AOT_TEST_CHECK(isCreatedInEveryShape() && storesToGuardedFields == 15 * 16);
        for (auto [shape, emptySlot] : shapesAndEmptySlots) {
            stores.fieldRecord(FieldTable::recordOf(shape, name)).hasGuardOfStores = 1;
            storesToGuardedFields = 14 * 16;
            AOT_TEST_CHECK(isCreated(shape, emptySlot, orderBefore, orderAfter));
            AOT_TEST_CHECK(isCreated(shape, emptySlot, orderBefore, orderAfter) == !asksChain);
            AOT_TEST_CHECK(storesToGuardedFields == (asksChain ? 0 : 14 * 16));
        }
        for (auto [shape, emptySlot] : shapesAndEmptySlots)
            stores.fieldRecord(FieldTable::recordOf(shape, name)).hasGuardOfStores = 0;
    });
}

// The stub that is told the record, and that the slot in the cell takes any value as it is.
static void testFieldsThatTakeAnyValueStoredByTheirRecords()
{
    PropertyStores stores;
    FieldTable table(stores);
    Instance& instance = stores.runner.instance();
    const uint64_t before = bitsOf(jsNumber(7));
    const uint64_t number = bitsOf(jsNumber(42));
    constexpr uint32_t shape = FieldTable::shapeThatTakesAnyValue;
    constexpr uint32_t slotKey = slotKeyFor(FakeTypedObject::offsetOfField, false);
    constexpr uint32_t orderBefore = orderWordOf(3, 1);
    constexpr uint32_t orderAfter = orderWordOf(5, 2);
    std::array<KeyOrderTransition, 16> transitions { };
    instance.objects.keyOrderTransitions = transitions.data();
    instance.objects.keyOrderTransitionMask = static_cast<uint32_t>(transitions.size() - 1);
    const uint64_t wordsBefore = static_cast<uint64_t>(shapeWordOf(shape)) << 32 | orderBefore;
    transitions[KeyOrderTransition::hashOf(wordsBefore, slotKey) & instance.objects.keyOrderTransitionMask] = { wordsBefore, slotKey, orderAfter };

    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        Chains chains(stores);
        const bool isDefinition = storeCase.mode == PutMode::Define;
        const uint16_t record = FieldTable::recordOf(shape, PropertyStores::nameOf(storeCase));
        const uint32_t offsetOfRecord = sizeof(LayoutData) + record * sizeof(FieldData);
        auto store = [&](FakeObjectWithShapeAndOrder& object) {
            return stores.runner.run(putKnownFieldThatTakesAnyValue(storeCase.mode), { object.address(), number }, offsetOfRecord);
        };
        auto isStoredByStub = [&](uint64_t bitsOfField, uint32_t orderWordBefore, uint32_t orderWordAfter) {
            FakeObjectWithShapeAndOrder object(shape, bitsOfField, orderWordBefore);
            if (store(object).calledOperation) {
                AOT_TEST_CHECK(object.field() == bitsOfField && object.orderWord() == orderWordBefore);
                return false;
            }
            AOT_TEST_CHECK(object.field() == number && object.orderWord() == orderWordAfter && object.wordAfterFieldIsUntouched());
            return true;
        };
        auto isReplaced = [&](uint32_t flags) { return isStoredByStub(before, orderBefore | flags, orderBefore | flags); };
        auto isCreated = [&](uint32_t flags = 0) { return isStoredByStub(0, orderBefore | flags, orderAfter | flags); };

        // The field is there.
        AOT_TEST_CHECK(isReplaced(0) && isReplaced(orderWordIsNotExtensible) && isReplaced(orderWordHasFieldOutsideSlot));
        AOT_TEST_CHECK(!isReplaced(orderWordIsFrozen));
        AOT_TEST_CHECK(isReplaced(orderWordIsSealed) == !isDefinition);

        // It is not: a transition says what the order becomes.
        AOT_TEST_CHECK(isCreated());
        for (uint32_t flag : { orderWordIsNotExtensible, orderWordIsSealed, orderWordIsFrozen, orderWordHasFieldOutsideSlot })
            AOT_TEST_CHECK(!isCreated(flag));

        // The one store of a field that is stored once creates it.
        stores.fieldRecord(record).flags |= fieldIsStoredOnce;
        AOT_TEST_CHECK(!isReplaced(0) && isCreated());
        stores.fieldRecord(record).flags &= ~fieldIsStoredOnce;

        // No transition: the value is stored, and the run time lists the key.
        {
            FakeObjectWithShapeAndOrder object(shape, 0, orderWordOf(4, 1));
            stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
            StubRun run = store(object);
            stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == slotKey);
            AOT_TEST_CHECK(object.field() == number && object.orderWord() == orderWordOf(4, 1));
        }

        // An order of the image that goes on with the field, which the order names by its index in the layout: one key more of the same order, without a transition.
        {
            stores.layout().numberOfOrders = 2;
            stores.layout().ordersOffset = PropertyStores::offsetOfOrders;
            stores.inTables<uint32_t>(PropertyStores::offsetOfOrders) = PropertyStores::offsetOfOrders + 8;
            stores.inTables<uint32_t>(PropertyStores::offsetOfOrders + 4) = PropertyStores::offsetOfOrders + 8;
            const uint16_t order[] = { 3, PropertyStores::numberOfFieldRecords, record, static_cast<uint16_t>(record + 1) };
            for (unsigned index = 0; index < std::size(order); ++index)
                stores.inTables<uint16_t>(PropertyStores::offsetOfOrders + 8 + index * sizeof(uint16_t)) = order[index];
            AOT_TEST_CHECK(isStoredByStub(0, orderWordOf(1, 1), orderWordOf(1, 2)));
            // The order goes on with an extra key, with another field, not at all; it is none of the image's.
            for (uint32_t orderWord : { orderWordOf(1, 0), orderWordOf(1, 2), orderWordOf(1, 3), orderWordOf(2, 1) }) {
                FakeObjectWithShapeAndOrder object(shape, 0, orderWord);
                stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
                StubRun run = store(object);
                stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
                AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && object.field() == number && object.orderWord() == orderWord);
            }
            stores.layout().numberOfOrders = 0;
        }

        // A store asks what is above the object. A definition does not.
        chains.filter() = { };
        AOT_TEST_CHECK(isCreated() == isDefinition && isReplaced(0));
        chains.letEveryStorePass();
        AOT_TEST_CHECK(isCreated());

        // A field of another kind is left to the stub for any field.
        for (uint16_t flag : { fieldIsReadOnly, fieldIsNotEnumerable, fieldIsNotConfigurable, fieldIsAccessor, fieldIsPrivate, fieldIsBrand }) {
            stores.fieldRecord(record).flags |= flag;
            AOT_TEST_CHECK(!isReplaced(0) && !isCreated());
            stores.fieldRecord(record).flags &= ~flag;
        }
    });
}

static void testFieldsCreatedOutsideTheCellByTheirRecords()
{
    PropertyStores stores;
    FieldTable table(stores);
    CreatedFieldOrders orders(stores.runner.instance(), slotKeyFor(FieldTable::offsetInExtension, true));
    const uintptr_t cageBase = stores.runner.instance().cellCageBase;
    const uint64_t number = bitsOf(jsNumber(42));
    using RoomyExtension = FakeObject<extensionCellSizeFor(2)>;
    Chains chains(stores);
    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        auto storeAndExpectKey = [&](FakeTypedObject& object, uint32_t slotKey) {
            stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
            StubRun run = stores.store(storeCase, object.address(), number);
            stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == slotKey);
        };
        // The granule is there and empty.
        {
            FakeExtension extension(FakeExtension::bitOfSlot, 0);
            FakeObjectWithShape object(FieldTable::shapeWithOutOfLineValue, extension.encode(cageBase), CreatedFieldOrders::orderBefore);
            storeAndExpectKey(object, slotKeyFor(FieldTable::offsetInExtension, true));
            AOT_TEST_CHECK(extension.granule() == number && extension.wordAfterGranuleIsUntouched());
        }
        // The Extension has room behind its two granules: the stub appends the third.
        {
            constexpr uint32_t offsetOfThirdGranule = offsetOfFirstExtensionSlot + 2 * extensionGranuleSize;
            constexpr uint64_t bitOfThirdGranule = granuleBitOf(offsetOfThirdGranule);
            FieldData& record = stores.fieldRecord(FieldTable::recordOf(FieldTable::shapeWithOutOfLineValue, PropertyStores::nameOf(storeCase)));
            const SlotData slotBefore = record.slot;
            record.slot = SlotData::create(offsetOfThirdGranule, SlotRepresentation::JSValue);
            RoomyExtension extension;
            extension.at<uint64_t>(offsetOfExtensionGranuleBits) = bitOfThirdGranule >> 1 | bitOfThirdGranule >> 2;
            for (uint32_t offset = offsetOfFirstExtensionSlot; offset < extensionCellSizeFor(2); offset += extensionGranuleSize)
                extension.at<uint64_t>(offset) = JSValue::ValueEmpty;
            FakeObjectWithShape object(FieldTable::shapeWithOutOfLineValue, MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage)), CreatedFieldOrders::orderBefore);
            storeAndExpectKey(object, slotKeyFor(offsetOfThirdGranule, true));
            AOT_TEST_CHECK(extension.at<uint64_t>(offsetOfExtensionGranuleBits) == (bitOfThirdGranule | bitOfThirdGranule >> 1 | bitOfThirdGranule >> 2));
            AOT_TEST_CHECK(extension.at<uint64_t>(offsetOfThirdGranule) == number && !extension.at<uint64_t>(offsetOfFirstExtensionSlot) && !extension.at<uint64_t>(offsetOfFirstExtensionSlot + extensionGranuleSize));
            record.slot = slotBefore;
        }
        // Where the key order alone says that the field exists, undefined asks for no granule: the key is listed and nothing is stored.
        {
            FieldData& record = stores.fieldRecord(FieldTable::recordOf(FieldTable::shapeWithOutOfLineValue, PropertyStores::nameOf(storeCase)));
            record.flags |= fieldMayExistWithoutStorage;
            FakeExtension extension(FakeExtension::bitOfSlot >> 1, bitsOf(jsNumber(1)));
            FakeObjectWithShape object(FieldTable::shapeWithOutOfLineValue, extension.encode(cageBase), CreatedFieldOrders::orderBefore);
            stores.runner.recordCallsOf(Entry::operationAOTDidCreateFieldAt);
            StubRun run = stores.store(storeCase, object.address(), bitsOf(jsUndefined()));
            stores.runner.countCallsOf(Entry::operationAOTDidCreateFieldAt);
            AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && static_cast<uint32_t>(run.recordedArguments[1]) == slotKeyFor(FieldTable::offsetInExtension, true));
            AOT_TEST_CHECK(extension.granule() == bitsOf(jsNumber(1)) && extension.at<uint64_t>(offsetOfExtensionGranuleBits) == FakeExtension::bitOfSlot >> 1);
            record.flags &= ~fieldMayExistWithoutStorage;
        }
    });
}

// Behind the first store the tables are emptied, so that a store is made without an operation only where the place of the key and the kinds that the type has accepted answer.
static void testKeyedStoresAtPlaces()
{
    PropertyStores stores;
    stores.runner.setKeepsKeyedValuePlacesAcrossRuns(true);
    Instance& instance = stores.runner.instance();
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = stores.uidOf(PropertyStores::unboundName);
    FakeObject<sizeof(Symbol)> otherKey;
    otherKey.setHeader(0, SymbolType);
    otherKey.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = stores.uidOf(PropertyStores::boundName);
    const uint64_t one = bitsOf(jsNumber(1));
    const uint64_t two = bitsOf(jsNumber(2));
    const uint64_t half = bitsOf(jsNumber(0.5));
    const uint64_t quarter = bitsOf(jsNumber(0.25));
    auto forgetPlaces = [&] {
        zeroSpan(instance.keyedValuePlaces());
        zeroSpan(instance.acceptedKindsOfValues());
        instance.propertyCaches.placesFiledFromSlots = 0;
        instance.propertyCaches.placesFiledFromSlotsAndCleared = 0;
    };

    for (PutMode mode : { PutMode::Strict, PutMode::Sloppy }) {
        StoreCase storeCase { mode, FiledIn::HashedTable, false };
        auto store = [&](auto& object, auto& underKey, uint64_t value) {
            return stores.runner.run(putByValCached(mode), { object.address(), underKey.address(), value });
        };

        for (Slot::Kind kind : { Slot::Kind::Field, Slot::Kind::FieldCreation }) {
            forgetPlaces();
            stores.file(storeCase, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers));
            FakeTypedObject object(one);
            AOT_TEST_CHECK(!store(object, key, two).calledOperation && object.field() == two && instance.propertyCaches.placesFiledFromSlots == 1);
            stores.clearCaches();
            AOT_TEST_CHECK(!store(object, key, one).calledOperation && object.field() == one && object.wordAfterFieldIsUntouched());
            FakeTypedObject twin(two);
            AOT_TEST_CHECK(!store(twin, key, one).calledOperation && twin.field() == one && object.field() == one);

            // Of another kind: the type has not been asked, or does not accept it.
            for (uint64_t value : { half, bitsOf(jsBoolean(true)), bitsOf(jsUndefined()), bitsOf(jsNull()), static_cast<uint64_t>(key.address()) })
                AOT_TEST_CHECK(store(object, key, value).calledOperation && object.field() == one);
            AOT_TEST_CHECK(store(object, otherKey, two).calledOperation && object.field() == one);

            // The slot is empty, or the object is frozen.
            FakeTypedObject withoutField(JSValue::ValueEmpty);
            AOT_TEST_CHECK(store(withoutField, key, two).calledOperation && !withoutField.field());
            FakeTypedObject frozen(one, orderWordIsFrozen);
            AOT_TEST_CHECK(store(frozen, key, two).calledOperation && frozen.field() == one);

            // Each kind is asked about once.
            stores.file(storeCase, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers));
            AOT_TEST_CHECK(!store(object, key, half).calledOperation && object.field() == half);
            stores.clearCaches();
            AOT_TEST_CHECK(!store(object, key, quarter).calledOperation && object.field() == quarter);
            AOT_TEST_CHECK(!store(object, key, two).calledOperation && object.field() == two);
            AOT_TEST_CHECK(store(object, key, bitsOf(jsBoolean(true))).calledOperation && object.field() == two);

            // What one type has accepted says nothing about another.
            forgetPlaces();
            stores.file(storeCase, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfBooleans));
            FakeTypedObject flag(bitsOf(jsBoolean(false)));
            AOT_TEST_CHECK(!store(flag, key, bitsOf(jsBoolean(true))).calledOperation && flag.field() == bitsOf(jsBoolean(true)));
            stores.clearCaches();
            AOT_TEST_CHECK(!store(flag, key, bitsOf(jsBoolean(true))).calledOperation);
            AOT_TEST_CHECK(store(flag, key, bitsOf(jsBoolean(false))).calledOperation && store(flag, key, one).calledOperation && flag.field() == bitsOf(jsBoolean(true)));

            // Once every place has been filled, no store fills one more.
            forgetPlaces();
            instance.propertyCaches.placesFiledFromSlots = KeyedValuePlace::numberOfPlacesPerAccess;
            stores.file(storeCase, createFieldSlot(kind, SlotRepresentation::JSValue, PropertyStores::typeOfNumbers));
            AOT_TEST_CHECK(!store(object, key, one).calledOperation && object.field() == one && instance.propertyCaches.placesFiledFromSlots == KeyedValuePlace::numberOfPlacesPerAccess);
            stores.clearCaches();
            AOT_TEST_CHECK(store(object, key, two).calledOperation && object.field() == one);
        }

        // A store that adds the name where the storage has room.
        forgetPlaces();
        constexpr PropertyOffset offset = 2;
        constexpr int32_t word = FakePlainObject::firstInlineWord + offset;
        std::optional<Slot> addition = Slot::tryCreateTransition(toStructureID(PropertyStores::structureID), 0, toStructureID(PropertyStores::structureIDAfterAddition), offset, 0);
        AOT_TEST_CHECK(addition.has_value());
        stores.file(storeCase, *addition);
        FakePlainObject first(PropertyStores::structureID);
        AOT_TEST_CHECK(!store(first, key, one).calledOperation && first.structureID() == PropertyStores::structureIDAfterAddition && instance.propertyCaches.placesFiledFromSlots == 1);
        checkOnlyWordChanged(first, word, one);
        stores.clearCaches();
        FakePlainObject second(PropertyStores::structureID);
        AOT_TEST_CHECK(!store(second, key, two).calledOperation && second.structureID() == PropertyStores::structureIDAfterAddition);
        checkOnlyWordChanged(second, word, two);
        FakePlainObject third(PropertyStores::structureID);
        AOT_TEST_CHECK(store(third, otherKey, two).calledOperation && third.structureID() == PropertyStores::structureID);
        AOT_TEST_CHECK(store(second, key, one).calledOperation && second.word(word) == two);
    }
}

std::span<const NativeTest> stubPropertyStoreTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/put-by-id/replaced-values"_s, ImageTests::whenExecutable<testReplacedValues> },
        { "stubs/put-by-id/additions-in-existing-storage"_s, ImageTests::whenExecutable<testAdditionsInExistingStorage> },
        { "stubs/put-by-id/additions-with-first-storage"_s, ImageTests::whenExecutable<testAdditionsWithFirstStorage> },
        { "stubs/put-by-id/additions-with-doubled-storage"_s, ImageTests::whenExecutable<testAdditionsWithDoubledStorage> },
        { "stubs/put-by-id/additions-beside-indexing-header"_s, ImageTests::whenExecutable<testAdditionsBesideIndexingHeader> },
        { "stubs/put-by-id/setters"_s, ImageTests::whenExecutable<testSetters> },
        { "stubs/put-by-id/exits"_s, ImageTests::whenExecutable<testExits> },
        { "stubs/put-by-id/barriers"_s, ImageTests::whenExecutable<testBarriers> },
        { "stubs/write-barrier/remembers-owner-without-operation"_s, ImageTests::whenExecutable<testBarrierRemembersOwnerWithoutOperation> },
        { "stubs/put-by-id/typed-fields"_s, ImageTests::whenExecutable<testTypedFields> },
        { "stubs/put-by-id/fields-that-leave-the-stub"_s, ImageTests::whenExecutable<testFieldsThatLeaveTheStub> },
        { "stubs/put-by-id/created-fields"_s, ImageTests::whenExecutable<testCreatedFields> },
        { "stubs/put-by-id/created-fields-without-storage"_s, ImageTests::whenExecutable<testCreatedFieldsWithoutStorage> },
        { "stubs/put-by-id/undefined-and-null-in-fields-of-other-types"_s, ImageTests::whenExecutable<testUndefinedAndNullInFieldsOfOtherTypes> },
        { "stubs/put-by-id/inherited-setters-of-absent-fields"_s, ImageTests::whenExecutable<testInheritedSettersOfAbsentFields> },
        { "stubs/put-by-id/additions-to-extra-properties"_s, ImageTests::whenExecutable<testAdditionsToExtraProperties> },
        { "stubs/put-by-id/additions-to-extra-properties-in-new-storage"_s, ImageTests::whenExecutable<testAdditionsToExtraPropertiesInNewStorage> },
        { "stubs/put-by-id/integrity-levels"_s, ImageTests::whenExecutable<testFieldsOfObjectsWithIntegrityLevels> },
        { "stubs/put-by-id/fields-found-in-field-table"_s, ImageTests::whenExecutable<testFieldsFoundInFieldTable> },
        { "stubs/put-by-id/stores-left-to-the-caches"_s, ImageTests::whenExecutable<testStoresLeftToTheCaches> },
        { "stubs/put-by-id/fields-stored-by-their-records"_s, ImageTests::whenExecutable<testFieldsStoredByTheirRecords> },
        { "stubs/put-by-id/fields-created-by-their-records"_s, ImageTests::whenExecutable<testFieldsCreatedByTheirRecords> },
        { "stubs/put-by-id/fields-created-by-slots-that-remember-the-order"_s, ImageTests::whenExecutable<testFieldsCreatedBySlotsThatRememberTheOrder> },
        { "stubs/put-by-id/fields-that-take-any-value-stored-by-their-records"_s, ImageTests::whenExecutable<testFieldsThatTakeAnyValueStoredByTheirRecords> },
        { "stubs/put-by-id/fields-created-outside-the-cell-by-their-records"_s, ImageTests::whenExecutable<testFieldsCreatedOutsideTheCellByTheirRecords> },
        { "stubs/put-by-val/integrity-levels"_s, ImageTests::whenExecutable<testKeyedStoresToObjectsWithIntegrityLevels> },
        { "stubs/put-by-val/symbols-whose-holders-are-prepared"_s, ImageTests::whenExecutable<testKeyedStoresUnderSymbolsWhoseHoldersArePrepared> },
        { "stubs/put-by-val/stores-at-places"_s, ImageTests::whenExecutable<testKeyedStoresAtPlaces> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
