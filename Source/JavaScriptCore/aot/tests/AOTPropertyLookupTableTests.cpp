/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTPropertyLookupTableTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <array>
#include <bit>
#include <memory>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

void evictionKeepsFilling()
{
    constexpr uint32_t numberOfWideRows = RowDisplacementTable::maxNumberOfBases / RowDisplacementTable::numberOfColumns;
    constexpr unsigned numberOfNarrowRows = RowDisplacementTable::numberOfColumns;
    TableFixture fixture;
    auto& front = fixture.front();
    Vector<Structure*> structures;
    for (uint32_t i = 0; i < numberOfWideRows; ++i) {
        structures.append(fixture.createStructure());
        Slot slot = ownValueSlot(structures.last(), 0);
        for (uint32_t column = 1; column <= RowDisplacementTable::maxColumn; ++column)
            AOT_TEST_CHECK(front.trySet(structures.last(), column, SlotAccess::Read, slot));
    }
    for (Structure* structure : structures)
        AOT_TEST_CHECK(front.find(structure->id(), RowDisplacementTable::maxColumn, SlotAccess::Read));

    for (unsigned i = 0; i < numberOfNarrowRows; ++i) {
        structures.append(fixture.createStructure());
        Slot slot = ownValueSlot(structures.last(), 1);
        AOT_TEST_CHECK(front.trySet(structures.last(), 1, SlotAccess::Read, slot));
        AOT_TEST_CHECK(isSameSlot(front.find(structures.last()->id(), 1, SlotAccess::Read), slot));
    }

    unsigned numberOfEvictedRows = 0;
    for (Structure* structure : structures) {
        AOT_TEST_CHECK(front.hasRow(structure->id()) == !!rowOf(structure));
        AOT_TEST_CHECK(front.hasRow(structure->id()) == !!front.find(structure->id(), 1, SlotAccess::Read));
        numberOfEvictedRows += !front.hasRow(structure->id());
    }
    AOT_TEST_CHECK(numberOfEvictedRows && !front.hasRow(structures.first()->id()));
}

static constexpr size_t bytesInFrontOfZeroedInstance = static_cast<size_t>(-InstanceLayout::offsetOfKeyedValuePlaces);

struct ZeroedInstanceDeleter {
    void operator()(Instance* instance) const
    {
        instance->~Instance();
        fastFree(reinterpret_cast<uint8_t*>(instance) - bytesInFrontOfZeroedInstance);
    }
};
using ZeroedInstance = std::unique_ptr<Instance, ZeroedInstanceDeleter>;

ZeroedInstance createZeroedInstance()
{
    constexpr size_t size = bytesInFrontOfZeroedInstance + sizeof(Instance);
    auto* memory = static_cast<uint8_t*>(fastAlignedMalloc(alignof(Instance), size));
    ZeroedInstance instance { new (NotNull, memory + bytesInFrontOfZeroedInstance) Instance };
    zeroSpan(unsafeMakeSpan(memory, size));
    return instance;
}

bool hasPublishedWords(TableFixture& fixture, const Instance& instance)
{
    const PropertyCacheWords& words = instance.propertyCaches;
    for (SlotAccess access : allSlotAccesses) {
        const FrontWords& front = words.front[static_cast<unsigned>(access)];
        if (front.plane != fixture.front().plane(access) || front.rowsForStructureIDZero != StructureRows::singleton().rowsForStructureIDZero())
            return false;
    }
    return words.hashedSlots == fixture.hashedTable().entries() && words.hashedSlotOffsetMask == fixture.hashedTable().offsetMask()
        && words.customAccessors == fixture.caches().customAccessors().records();
}

void growthKeepsEntriesAndPublishesWords()
{
    constexpr uint32_t initialNumberOfEntries = HashedSlotTable::initialNumberOfEntries;
    constexpr unsigned maxNumberOfFills = 16 * initialNumberOfEntries;
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries && hasPublishedWords(fixture, *instance));

    Structure* lastStructure = nullptr;
    for (unsigned i = 0; table.numberOfEntries() == initialNumberOfEntries; ++i) {
        AOT_TEST_CHECK(i < maxNumberOfFills);
        lastStructure = fixture.createStructure();
        fixture.caches().set(lastStructure, fixture.nameWithoutNumber(1), SlotAccess::Read, absentSlot(lastStructure, 0));
    }

    AOT_TEST_CHECK(table.numberOfEntries() == 2 * initialNumberOfEntries);
    AOT_TEST_CHECK(table.offsetMask() == HashedSlot::offsetMaskFor(2 * initialNumberOfEntries));
    AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));
    AOT_TEST_CHECK(isSameSlot(table.find(lastStructure->id(), fixture.uid(1), SlotAccess::Read), absentSlot(lastStructure, 0)));
    size_t numberOfKeptEntries = 0;
    table.forEachEntry([&](const HashedSlot& entry) {
        AOT_TEST_CHECK(table.find(entry.slot.structureID, entry.uid(), entry.access()) == &entry.slot);
        ++numberOfKeptEntries;
    });
    AOT_TEST_CHECK(numberOfKeptEntries > initialNumberOfEntries / 2);
    fixture.vm().aotState().instances.clear();
}

void frontGrowthKeepsRowsAndPublishesWords()
{
    constexpr unsigned numberOfStructures = 3 * RowDisplacementTable::initialNumberOfBases;
    constexpr uint32_t columns[] = { 1, 977, RowDisplacementTable::maxColumn };
    TableFixture fixture;
    auto& front = fixture.front();
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));

    auto offsetFor = [](unsigned structure, uint32_t column) {
        return static_cast<PropertyOffset>((structure + column) % 50);
    };
    Vector<Structure*> structures;
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        structures.append(fixture.createStructure());
        for (uint32_t column : columns)
            fixture.caches().set(structures[i], fixture.name(column), SlotAccess::Read, ownValueSlot(structures[i], offsetFor(i, column)));
        fixture.caches().set(structures[i], fixture.name(columns[0]), SlotAccess::Define, absentSlot(structures[i], 0));
        AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));
    }
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        for (uint32_t column : columns) {
            AOT_TEST_CHECK(fixture.cellAt(structures[i], column, SlotAccess::Read).name == column);
            AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), column, SlotAccess::Read), ownValueSlot(structures[i], offsetFor(i, column))));
            AOT_TEST_CHECK(!front.find(structures[i]->id(), column, SlotAccess::Store));
        }
        AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), columns[0], SlotAccess::Define), absentSlot(structures[i], 0)));
    }
    fixture.vm().aotState().instances.clear();
}

void secondInstanceClearsOnce()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    auto& instances = fixture.vm().aotState().instances;
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot slot = ownValueSlot(structure, 0);
    std::array<ZeroedInstance, 3> storage { createZeroedInstance(), createZeroedInstance(), createZeroedInstance() };
    auto fill = [&] {
        caches.set(structure, fixture.name(1), SlotAccess::Read, slot);
        caches.set(structure, fixture.nameWithoutNumber(2), SlotAccess::Read, slot);
    };
    auto isFilled = [&] {
        return !!caches.find(id, fixture.name(1), SlotAccess::Read) && !!caches.find(id, fixture.nameWithoutNumber(2), SlotAccess::Read);
    };
    auto createInstance = [&](unsigned index) {
        instances.append(storage[index].get());
        caches.didCreateInstance(*storage[index]);
        AOT_TEST_CHECK(hasPublishedWords(fixture, *storage[index]));
    };

    createInstance(0);
    fill();
    AOT_TEST_CHECK(!caches.isSharedByInstances() && isFilled());

    createInstance(1);
    AOT_TEST_CHECK(caches.isSharedByInstances());
    AOT_TEST_CHECK(!fixture.numberOfSlots() && !fixture.front().hasRow(id) && !rowOf(structure));

    fill();
    createInstance(2);
    AOT_TEST_CHECK(caches.isSharedByInstances() && isFilled());
    instances.clear();
}

void onlyInstanceCountsAsRun()
{
    TableFixture fixture;
    VMState& state = fixture.vm().aotState();
    std::array<ZeroedInstance, 2> storage { createZeroedInstance(), createZeroedInstance() };
    Instance* first = storage[0].get();
    uint8_t& hasRun = first->propertyCaches.hasRunSinceClear;

    state.instances.append(first);
    state.didChangeInstances();
    AOT_TEST_CHECK(state.onlyInstance == first && hasRun);

    state.instances.append(storage[1].get());
    state.didChangeInstances();
    AOT_TEST_CHECK(!state.onlyInstance);
    hasRun = false;

    state.instances.removeLast();
    state.didChangeInstances();
    AOT_TEST_CHECK(state.onlyInstance == first && hasRun);
    state.instances.clear();
    state.didChangeInstances();
}

void absenceBesidePairDoesNotOutliveItsCount()
{
    using KeyOfPair = StringKeyCache::KeyOfPair;
    constexpr uint32_t mask = KeyOfPair::maskBelowOtherRead;
    TableFixture fixture;
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    std::array<KeyOfPair, StringKeyCache::waysOfKeysOfPairs> pairs { };
    instance->realm.keysOfPairs = pairs.data();
    instance->realm.keyOfPairMask = StringKeyCache::keyOfPairMaskFor(pairs.size());
    PropertyCacheWords& words = instance->propertyCaches;
    auto fileAbsence = [&] {
        pairs[1].structureIDOfPlace = 64;
        pairs[1].slotOfPlace = KeyOfPair::markOf(KeyOfPair::OtherRead::Absence) | (words.timesAbsencesWereForgotten & mask);
        words.pairsHavePlaces = 1;
    };
    auto absenceHolds = [&] {
        return pairs[1].structureIDOfPlace && !((pairs[1].slotOfPlace ^ words.timesAbsencesWereForgotten) & mask);
    };
    auto forgetAbsences = [&] {
        words.placesSayAbsence = 1;
        fixture.caches().didChangeObjectOnSomeChain(nullptr, nullptr);
    };

    for (uint32_t countWhenFiled : { 0u, 5u, mask, mask + 1 }) {
        words.timesAbsencesWereForgotten = countWhenFiled;
        fileAbsence();
        AOT_TEST_CHECK(absenceHolds());
        forgetAbsences();
        AOT_TEST_CHECK(words.timesAbsencesWereForgotten == countWhenFiled + 1 && !absenceHolds());

        uint32_t countWhenLowBitsAreZero = (countWhenFiled + 1 + mask) & ~mask;
        if (words.timesAbsencesWereForgotten != countWhenLowBitsAreZero) {
            words.timesAbsencesWereForgotten = countWhenLowBitsAreZero - 1;
            forgetAbsences();
        }
        if (words.timesAbsencesWereForgotten != countWhenFiled + mask + 1) {
            words.timesAbsencesWereForgotten = countWhenFiled + mask;
            forgetAbsences();
        }
        AOT_TEST_CHECK(words.timesAbsencesWereForgotten == countWhenFiled + mask + 1);
        AOT_TEST_CHECK(!absenceHolds());
    }
    fixture.vm().aotState().instances.clear();
}

} // namespace

void shortStringsHashAsText()
{
    auto check = [](const StringKeyCache::FewCharacters& joined, std::span<const Latin1Character> text) {
        AOT_TEST_CHECK(joined.length == text.size());
        auto characters = joined.text();
        AOT_TEST_CHECK(equalSpans(std::span<const Latin1Character> { characters }.first(joined.length), text));
        AOT_TEST_CHECK(joined.hash() == RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(text));
        if (text.size() < 2)
            return;
        Ref<StringImpl> impl = StringImpl::create(text);
        AOT_TEST_CHECK(joined.isTextOf(impl.get()));
        for (size_t i = 0; i < text.size(); ++i) {
            std::array<Latin1Character, StringKeyCache::FewCharacters::mostCharacters> other { };
            memcpySpan(std::span { other }, text);
            other[i] = static_cast<Latin1Character>(static_cast<uint8_t>(other[i]) ^ 1);
            AOT_TEST_CHECK(!joined.isTextOf(StringImpl::create(std::span<const Latin1Character> { other }.first(text.size())).get()));
        }
        AOT_TEST_CHECK(!joined.isTextOf(StringImpl::create(text.first(text.size() - 1)).get()));
    };

    uint64_t state = 0x9e3779b97f4a7c15ull;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (unsigned round = 0; round < 20000; ++round) {
        std::array<Latin1Character, StringKeyCache::FewCharacters::mostCharacters> text;
        StringKeyCache::FewCharacters joined;
        size_t length = 0;
        while (true) {
            size_t size = next() % (text.size() + 1);
            std::array<Latin1Character, StringKeyCache::FewCharacters::mostCharacters> part;
            for (Latin1Character& character : part)
                character = static_cast<Latin1Character>(next());
            bool fits = length + size <= text.size();
            AOT_TEST_CHECK(joined.tryAppend(std::span<const Latin1Character> { part }.first(size)) == fits);
            if (!fits)
                break;
            memcpySpan(std::span { text }.subspan(length), std::span<const Latin1Character> { part }.first(size));
            length += size;
        }
        if (length)
            check(joined, std::span<const Latin1Character> { text }.first(length));
    }

    for (unsigned round = 0; round < 20000; ++round) {
        uint32_t integer = round < 10000 ? round : static_cast<uint32_t>(next() % StringKeyCache::FewCharacters::limitOfIntegers);
        StringKeyCache::FewCharacters joined;
        AOT_TEST_CHECK(joined.tryAppendInteger(integer));
        check(joined, String::number(integer).span8());
    }
    StringKeyCache::FewCharacters full;
    AOT_TEST_CHECK(!full.tryAppendInteger(StringKeyCache::FewCharacters::limitOfIntegers) && !full.tryAppendInteger(static_cast<uint32_t>(-1)));
    AOT_TEST_CHECK(full.tryAppendInteger(99999999) && full.tryAppendInteger(10000000) && !full.tryAppendInteger(0));
}

void neighboringPairKeysFindSlots()
{
    constexpr size_t tableSize = 1 << 14;
    constexpr size_t numberOfSets = tableSize / StringKeyCache::waysOfKeysOfPairs;
    constexpr EncodedJSValue firstCell = 0x7f0012340000;
    auto check = [&](size_t firsts, EncodedJSValue first, EncodedJSValue stepOfFirst, size_t seconds, EncodedJSValue second, EncodedJSValue stepOfSecond) {
        std::array<uint16_t, numberOfSets> pairsOfSet { };
        size_t withoutPlace = 0;
        for (size_t i = 0; i < firsts; ++i) {
            for (size_t j = 0; j < seconds; ++j) {
                uint64_t offset = StringKeyCache::hashOfPair(first + i * stepOfFirst, second + j * stepOfSecond) & StringKeyCache::keyOfPairMaskFor(tableSize);
                size_t set = offset >> StringKeyCache::keyOfPairSetShift;
                AOT_TEST_CHECK(set < numberOfSets);
                withoutPlace += ++pairsOfSet[set] > StringKeyCache::waysOfKeysOfPairs;
            }
        }
        AOT_TEST_CHECK(withoutPlace * 20 <= firsts * seconds);
    };
    EncodedJSValue zero = JSValue::encode(jsNumber(0));
    for (EncodedJSValue spacing : { 16, 32, 48, 64, 96 }) {
        check(40, firstCell, spacing, 18, zero, 1);
        check(12, firstCell, spacing, 400, zero, 1);
        check(400, zero, 1, 12, firstCell, spacing);
        check(40, firstCell, spacing, 40, firstCell + 0x20000, spacing);
    }
    check(64, zero, 1, 64, zero, 1);
    check(12, zero, 1, 400, zero, 1);
    check(400, zero, 1, 12, zero, 1);
}

std::span<const NativeTest> propertyLookupTableTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup-table/eviction-keeps-filling"_s, evictionKeepsFilling },
        { "property-lookup-table/growth-keeps-entries-and-publishes-words"_s, growthKeepsEntriesAndPublishesWords },
        { "property-lookup-table/front-growth-keeps-rows-and-publishes-words"_s, frontGrowthKeepsRowsAndPublishesWords },
        { "property-lookup-table/second-instance-clears-once"_s, secondInstanceClearsOnce },
        { "property-lookup-table/only-instance-counts-as-run"_s, onlyInstanceCountsAsRun },
        { "property-lookup-table/absence-beside-pair-does-not-outlive-its-count"_s, absenceBesidePairDoesNotOutliveItsCount },
        { "property-lookup-table/few-characters-hash-as-their-text"_s, shortStringsHashAsText },
        { "property-lookup-table/pair-keys-of-neighbors-find-places"_s, neighboringPairKeysFindSlots },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
