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
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeType.h"
#include "AOTSlotAccess.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubRunner.h"
#include "AOTStubsPropertyKeys.h"
#include "CompleteSubspace.h"
#include "GetterSetter.h"
#include "IndexingHeader.h"
#include "JSCInlines.h"
#include "JSObject.h"
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
    static constexpr uint32_t highestShapeWithoutFields = 16;

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
        for (unsigned name = 0; name < numberOfNames; ++name)
            m_identifiers[name] = std::bit_cast<UniquedStringImpl*>(m_uids[name].address());
        instance.image.programIdentifiers = m_identifiers.data();
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
        instance.slimClosureHeaders[0] = slimClosureStructureID;
    }

    static uint16_t nameOf(const StoreCase& storeCase) { return storeCase.nameIsBound ? boundName : unboundName; }
    uint64_t uidOf(uint16_t name) const { return m_uids[name].address(); }

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
        uint32_t offset = HashedSlot::offsetOfFirstWay(slot.structureID, m_identifiers[name], runner.instance().propertyCaches.hashedSlotOffsetMask);
        HashedSlot& entry = m_hashedSlots[offset >> HashedSlot::sizeShift];
        entry.slot = slot;
        entry.key = HashedSlot::keyFor(m_identifiers[name], access);
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
    std::array<FakeObject<16>, numberOfNames> m_uids;
    std::array<UniquedStringImpl*, numberOfNames> m_identifiers { };
    std::array<uint32_t, highestShapeWithoutFields + numberOfNames> m_emptyFieldTable { };
    std::array<uint16_t, numberOfNames> m_emptyFieldRecordIndices { };
    FakeObject<CompleteSubspace::offsetOfAllocatorForSizeStep() + 16 * sizeof(void*)> m_auxiliarySpace;
    FakeObject<128> m_types;
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
    constexpr uint32_t offsetInExtension = offsetOfFirstExtensionSlot + extensionGranuleSize;
    constexpr uint64_t granuleBit = granuleBitOf(offsetInExtension);
    PropertyStores stores;
    CreatedFieldOrders orders(stores.runner.instance(), slotKeyFor(offsetInExtension, true));
    const uintptr_t cageBase = stores.runner.instance().cellCageBase;
    const uint64_t number = bitsOf(jsNumber(42));
    const Slot slot = Slot::createField(toStructureID(PropertyStores::structureID), Slot::Kind::FieldCreation, Slot::isOwn | Slot::isOutOfLine, SlotData::create(offsetInExtension, SlotRepresentation::JSValue), PropertyStores::typeOfAnything, offsetOfExtensionWord);
    FakeObject<sizeof(Symbol)> key;
    key.setHeader(0, SymbolType);
    key.at<uint64_t>(Symbol::offsetOfSymbolImpl()) = stores.uidOf(PropertyStores::unboundName);

    for (PutMode mode : allPutModes) {
        for (bool isKeyed : { false, true }) {
            StoreCase storeCase { mode, FiledIn::HashedTable, false };
            stores.file(storeCase, slot);
            auto store = [&](FakeTypedObject& object) {
                if (isKeyed)
                    return stores.runner.run(mode == PutMode::Define ? defineByValCached() : putByValCached(mode), { object.address(), key.address(), number });
                return stores.store(storeCase, object.address(), number);
            };
            // The page of zeros that stands for "no Extension" belongs to a real cage, so every object here has an Extension: one with room behind its only granule, or one whose granule comes after the new one.
            auto encode = [&](auto& extension) {
                return MarkedBlockCage::encode(cageBase, std::bit_cast<void*>(extension.address() - offsetOfEmptyExtensionInCage));
            };
            using FakeExtension = FakeObject<extensionCellSizeFor(2)>;
            for (bool hasRoom : { false, true }) {
                const uint64_t bitsBefore = hasRoom ? granuleBit >> 1 : granuleBit << 1;
                auto fill = [&](FakeExtension& extension, uint64_t bits) {
                    extension.template at<uint64_t>(offsetOfExtensionGranuleBits) = bits;
                    for (uint32_t offset = offsetOfFirstExtensionSlot; offset < extensionCellSizeFor(2); offset += extensionGranuleSize)
                        extension.template at<uint64_t>(offset) = JSValue::ValueEmpty;
                };
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
                StubRun run = store(object);
                if (hasRoom) {
                    AOT_TEST_CHECK(!run.numberOfRecordedCalls && extensionWord == wordBefore && extension.at<uint64_t>(offsetOfExtensionGranuleBits) == (bitsBefore | granuleBit));
                    AOT_TEST_CHECK(extension.at<uint64_t>(offsetInExtension) == number && !extension.at<uint64_t>(offsetOfFirstExtensionSlot));
                } else {
                    AOT_TEST_CHECK(run.numberOfRecordedCalls == 1 && run.recordedArguments[0] == object.address() && run.recordedArguments[1] == granuleBit);
                    AOT_TEST_CHECK(extension.at<uint64_t>(offsetOfExtensionGranuleBits) == bitsBefore && replacement.at<uint64_t>(offsetOfFirstExtensionSlot) == number);
                }
                AOT_TEST_CHECK(object.orderWord() == CreatedFieldOrders::orderAfter && object.field() == untouched);
                AOT_TEST_CHECK(!store(object).calledOperation && object.orderWord() == CreatedFieldOrders::orderAfter);

                FakeExtension extensionOfNotExtensibleObject;
                fill(extensionOfNotExtensibleObject, bitsBefore);
                FakeObjectWithOrder notExtensibleObject(untouched, orderWordIsNotExtensible);
                notExtensibleObject.at<uint32_t>(offsetOfExtensionWord) = encode(extensionOfNotExtensibleObject);
                run = store(notExtensibleObject);
                AOT_TEST_CHECK(run.calledOperation && !run.numberOfRecordedCalls && extensionOfNotExtensibleObject.at<uint64_t>(offsetOfExtensionGranuleBits) == bitsBefore);
                stores.runner.countCallsOf(Entry::operationAOTEnsureExtensionGranules);
            }
        }
    }
}

namespace {

// Both names are values in the cell of shapes 1 and 2, values in the Extension of shape 3 and integers in the cell of shapes 5 and 6. A store to shape 2 or 6 has to test a type. Shape 4 has neither name.
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
            for (uint32_t shape : { shapeThatTakesAnyValue, shapeWithOutOfLineValue, shapeWithInt32 })
                m_fields[rowOf(shape) + name] = FieldRecordIndex::isStoredWithoutTypeTest;
        }
        Instance& instance = stores.runner.instance();
        instance.image.fieldTableEntries = m_table.data() + highestShape;
        instance.image.fieldRecordIndices = m_fields.data();
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

// Holds one granule.
struct FakeExtension : FakeObject<extensionCellSizeFor(1)> {
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
            for (int32_t before : { 1, undefinedInt32, nullInt32, doubleInExtensionInt32 }) {
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

static void testStoresLeftToTheCaches()
{
    PropertyStores stores;
    FieldTable table(stores);
    uintptr_t cageBase = stores.runner.instance().cellCageBase;
    constexpr uint32_t frozen = orderWordIsFrozen | orderWordIsSealed | orderWordIsNotExtensible;
    forEachStoreWithEmptyCaches(stores, [&](const StoreCase& storeCase) {
        auto isLeft = [&](uint32_t shape, uint64_t bitsOfField, uint32_t orderWord) {
            FakeObjectWithShape object(shape, bitsOfField, orderWord);
            StubRun run = stores.store(storeCase, object.address(), bitsOf(jsNumber(42)));
            return run.calledOperation && object.field() == bitsOfField && object.wordAfterFieldIsUntouched();
        };
        AOT_TEST_CHECK(isLeft(FieldTable::shapeThatTakesAnyValue, 0, 0));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeThatTakesAnyValue, bitsOf(jsNumber(1)), frozen));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithTypedField, bitsOf(jsNumber(1)), 0));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithoutTheNames, bitsOf(jsNumber(1)), 0));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithOutOfLineValue, 0, 0));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithInt32, static_cast<uint32_t>(absentInt32), 0));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithInt32, 1, frozen));
        AOT_TEST_CHECK(isLeft(FieldTable::shapeWithInt32OfNarrowerType, 1, 0));
        for (JSValue value : { jsDoubleNumber(0.5), jsUndefined(), jsNull(), jsBoolean(true), jsNumber(minInlineInt32 - 1), jsNumber(INT32_MIN) }) {
            FakeObjectWithShape object(FieldTable::shapeWithInt32, 1);
            AOT_TEST_CHECK(stores.store(storeCase, object.address(), bitsOf(value)).calledOperation && object.field() == 1);
        }

        auto isLeftWithExtension = [&](uint64_t granuleBit, uint64_t bitsOfGranule, uint32_t orderWord) {
            FakeExtension extension(granuleBit, bitsOfGranule);
            FakeObjectWithShape object(FieldTable::shapeWithOutOfLineValue, extension.encode(cageBase), orderWord);
            return stores.store(storeCase, object.address(), bitsOf(jsNumber(42))).calledOperation && extension.granule() == bitsOfGranule && extension.wordAfterGranuleIsUntouched();
        };
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot >> 1, bitsOf(jsNumber(1)), 0));
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot << 1, bitsOf(jsNumber(1)), 0));
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot, 0, 0));
        AOT_TEST_CHECK(isLeftWithExtension(FakeExtension::bitOfSlot, bitsOf(jsNumber(1)), frozen));

        FakeTypedObject withoutShape(bitsOf(jsNumber(1)));
        AOT_TEST_CHECK(stores.store(storeCase, withoutShape.address(), bitsOf(jsNumber(42))).calledOperation && withoutShape.field() == bitsOf(jsNumber(1)));
    });
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
        { "stubs/put-by-id/typed-fields"_s, ImageTests::whenExecutable<testTypedFields> },
        { "stubs/put-by-id/fields-that-leave-the-stub"_s, ImageTests::whenExecutable<testFieldsThatLeaveTheStub> },
        { "stubs/put-by-id/created-fields"_s, ImageTests::whenExecutable<testCreatedFields> },
        { "stubs/put-by-id/created-fields-without-storage"_s, ImageTests::whenExecutable<testCreatedFieldsWithoutStorage> },
        { "stubs/put-by-id/inherited-setters-of-absent-fields"_s, ImageTests::whenExecutable<testInheritedSettersOfAbsentFields> },
        { "stubs/put-by-id/additions-to-extra-properties"_s, ImageTests::whenExecutable<testAdditionsToExtraProperties> },
        { "stubs/put-by-id/additions-to-extra-properties-in-new-storage"_s, ImageTests::whenExecutable<testAdditionsToExtraPropertiesInNewStorage> },
        { "stubs/put-by-id/integrity-levels"_s, ImageTests::whenExecutable<testFieldsOfObjectsWithIntegrityLevels> },
        { "stubs/put-by-id/fields-found-in-field-table"_s, ImageTests::whenExecutable<testFieldsFoundInFieldTable> },
        { "stubs/put-by-id/stores-left-to-the-caches"_s, ImageTests::whenExecutable<testStoresLeftToTheCaches> },
        { "stubs/put-by-val/integrity-levels"_s, ImageTests::whenExecutable<testKeyedStoresToObjectsWithIntegrityLevels> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
