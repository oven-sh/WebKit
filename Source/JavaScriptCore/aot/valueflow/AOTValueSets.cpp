/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueSets.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/Locker.h>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t mix(uint64_t first, uint64_t second)
{
    uint64_t hash = (first + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull ^ second * 0x94d049bb133111ebull;
    hash ^= hash >> 32;
    hash *= 0xd6e8feb86659fd93ull;
    return hash ^ hash >> 29;
}

struct ValueSetMaker::BlockBeingMade {
    ALWAYS_INLINE void add(unsigned word, uint64_t bits)
    {
        present |= 1ull << word;
        words[size++] = bits;
        numberOfMembers += static_cast<uint32_t>(std::popcount(bits));
        hash += mix(word, bits);
    }

    std::array<uint64_t, ValueSets::wordsPerBlock + 1> words;
    unsigned size { 1 };
    uint64_t present { 0 };
    uint32_t numberOfMembers { 0 };
    uint64_t hash { 0 };
};

ValueSets::InternedRuns::InternedRuns()
{
    for (Shard& shard : m_shards)
        shard.table.store(createTable(6), std::memory_order_relaxed);
    uint32_t number = takeUnits(unitsPerLine);
    RELEASE_ASSERT(!number);
    mutableRunAt(number) = Run { };
    m_shards[0].size = 1;
}

ValueSets::InternedRuns::~InternedRuns()
{
    freeOutgrownTables();
    for (Shard& shard : m_shards)
        fastFree(shard.table.load(std::memory_order_relaxed).slots());
    for (auto& segment : m_segments)
        fastFree(segment.load(std::memory_order_relaxed));
}

uint32_t ValueSets::InternedRuns::takeUnits(uint32_t numberOfUnits)
{
    RELEASE_ASSERT(numberOfUnits <= unitsPerSegment);
    numberOfUnits = static_cast<uint32_t>(roundUpToMultipleOf<unitsPerLine>(numberOfUnits));
    Locker locker { m_lockOfUnits };
    if (m_nextUnit % unitsPerSegment + numberOfUnits > unitsPerSegment)
        m_nextUnit = static_cast<uint32_t>(roundUpToMultipleOf<unitsPerSegment>(m_nextUnit));
    uint32_t segment = m_nextUnit / unitsPerSegment;
    RELEASE_ASSERT(segment < numberOfSegments);
    if (!m_segments[segment].load(std::memory_order_relaxed))
        m_segments[segment].store(static_cast<char*>(fastAlignedMalloc(64, unitsPerSegment * bytesPerUnit)), std::memory_order_release);
    uint32_t first = m_nextUnit;
    m_nextUnit += numberOfUnits;
    m_endOfNumbers.store(m_nextUnit, std::memory_order_relaxed);
    return first;
}

uint32_t ValueSets::InternedRuns::allocate(Room& room, size_t numberOfWords)
{
    size_t bytes = sizeof(Run) + numberOfWords * sizeof(uint64_t);
    uint32_t numberOfUnits = static_cast<uint32_t>((bytes + bytesPerUnit - 1) / bytesPerUnit);
    if (numberOfUnits > unitsPerChunk / 4)
        return takeUnits(numberOfUnits);
    uint32_t first = room.nextUnit;
    if (first % unitsPerLine + std::min(std::max(numberOfUnits, unitsOfHeadAndFirstWord), unitsPerLine) > unitsPerLine)
        first = static_cast<uint32_t>(roundUpToMultipleOf<unitsPerLine>(first));
    if (first + numberOfUnits > room.endOfChunk) {
        first = takeUnits(unitsPerChunk);
        room.endOfChunk = first + unitsPerChunk;
    }
    room.nextUnit = first + numberOfUnits;
    return first;
}

auto ValueSets::InternedRuns::createTable(unsigned bits) -> Table
{
    size_t bytes = sizeof(Slot) << bits;
    Slot* slots = static_cast<Slot*>(fastAlignedMalloc(64, bytes));
    memset(static_cast<void*>(slots), 0, bytes);
    return Table { slots, bits };
}

void ValueSets::InternedRuns::freeOutgrownTables()
{
    for (Shard& shard : m_shards) {
        for (Table table : shard.outgrownTables)
            fastFree(table.slots());
        shard.outgrownTables.clear();
    }
}

void ValueSets::InternedRuns::grow(Shard& shard)
{
    Table previous = shard.table.load(std::memory_order_relaxed);
    Table next = createTable(std::bit_width(previous.mask()) + 1);
    Slot* slots = next.slots();
    size_t mask = next.mask();
    for (size_t index = 0; index <= previous.mask(); ++index) {
        uint64_t slot = previous.slots()[index].load(std::memory_order_relaxed);
        if (!slot)
            continue;
        size_t position = (slot >> 32) & mask;
        while (slots[position].load(std::memory_order_relaxed))
            position = (position + 1) & mask;
        slots[position].store(slot, std::memory_order_relaxed);
    }
    shard.table.store(next, std::memory_order_release);
    shard.outgrownTables.append(previous);
}

ALWAYS_INLINE uint32_t ValueSets::InternedRuns::find(Table table, size_t& position, uint32_t hash, const Run& head, std::span<const uint64_t> words) const
{
    Slot* slots = table.slots();
    size_t mask = table.mask();
    for (;; position = (position + 1) & mask) {
        uint64_t slot = slots[position].load(std::memory_order_acquire);
        if (!slot)
            return 0;
        if (slot >> 32 != hash)
            continue;
        const Run& candidate = at(static_cast<uint32_t>(slot));
        if (candidate.hashOfMembers == head.hashOfMembers && candidate.numberOfWords == head.numberOfWords && candidate.numberOfMembers == head.numberOfMembers && candidate.lowestMembers == head.lowestMembers && std::ranges::equal(candidate.span(), words))
            return static_cast<uint32_t>(slot);
    }
}

uint32_t ValueSets::InternedRuns::intern(Room& room, std::span<const uint64_t> words, uint32_t numberOfMembers, uint64_t hashOfMembers, uint64_t lowestMembers)
{
    if (!numberOfMembers)
        return 0;

    Run head { static_cast<uint32_t>(words.size()), numberOfMembers, hashOfMembers, lowestMembers };
    uint32_t hash = static_cast<uint32_t>(hashOfMembers >> 32);
    Shard& shard = m_shards[hashOfMembers % numberOfShards];
    Table table = shard.table.load(std::memory_order_acquire);
    size_t position = hash & table.mask();
    if (uint32_t number = find(table, position, hash, head, words))
        return number;

    Room roomBefore = room;
    uint32_t number = allocate(room, words.size());
    Run& run = mutableRunAt(number);
    run = head;
    std::ranges::copy(words, const_cast<uint64_t*>(run.words()));

    Locker locker { shard.lock };
    if (Table current = shard.table.load(std::memory_order_relaxed); current != table) {
        table = current;
        position = hash & table.mask();
    }
    if (uint32_t found = find(table, position, hash, head, words)) {
        if (roomBefore.endOfChunk == room.endOfChunk)
            room = roomBefore;
        return found;
    }
    table.slots()[position].store(static_cast<uint64_t>(hash) << 32 | number, std::memory_order_release);
    if (static_cast<size_t>(++shard.size) * 2 > table.numberOfSlots())
        grow(shard);
    return number;
}

size_t ValueSets::InternedRuns::size() const
{
    size_t size = 0;
    for (const Shard& shard : m_shards)
        size += shard.size;
    return size;
}

void ValueFlowBytesByTable::add(ASCIILiteral table, size_t bytesInUse, size_t bytesHeld)
{
    for (Row& row : m_rows) {
        if (row.table.characters() == table.characters()) {
            row.bytesInUse += bytesInUse;
            row.bytesHeld += bytesHeld;
            return;
        }
    }
    m_rows.append(Row { table, bytesInUse, bytesHeld });
}

void ValueFlowBytesByTable::dump() const
{
    Row all { "all tables"_s };
    auto dumpRow = [](const Row& row) {
        dataLogLn("AOT flow bytes | ", row.table, " | in use ", row.bytesInUse / KB, " KB | held ", row.bytesHeld / KB, " KB");
    };
    for (const Row& row : m_rows) {
        dumpRow(row);
        all.bytesInUse += row.bytesInUse;
        all.bytesHeld += row.bytesHeld;
    }
    dumpRow(all);
}

void ValueSets::InternedRuns::addBytesTo(ValueFlowBytesByTable& bytes, ASCIILiteral runs, ASCIILiteral index) const
{
    bytes.add(runs, static_cast<size_t>(endOfNumbers()) * bytesPerUnit);
    for (const Shard& shard : m_shards) {
        bytes.add(index, shard.size * sizeof(Slot), shard.table.load(std::memory_order_relaxed).numberOfSlots() * sizeof(Slot));
        for (Table table : shard.outgrownTables)
            bytes.add(index, 0, table.numberOfSlots() * sizeof(Slot));
    }
}

void ValueSetMaker::addBytesTo(ValueFlowBytesByTable& bytes) const
{
    constexpr ASCIILiteral remembered = "set makers: results remembered"_s;
    bytes.add(remembered, sizeof(m_rememberedSets));
    bytes.addList(remembered, m_rememberedEntries);
    bytes.addList(remembered, m_rememberedBlocks);
    bytes.addMap(remembered, m_setOfRange);
    auto addScratch = [&](const auto&... lists) {
        (bytes.addList("set makers: scratch lists"_s, lists), ...);
    };
    addScratch(m_list, m_listOfBlocks, m_entryAtGroupOfPlaces, m_blockAtPlace, m_gatheredBlocks, m_members, m_highMembers, m_bitsOfWords, m_scatteredMembers, m_wordsTouchedAtPlace, m_placesTouched);
}

void ValueSets::addBytesTo(ValueFlowBytesByTable& bytes) const
{
    m_lists.addBytesTo(bytes, "sets: lists"_s, "sets: index of lists"_s);
    m_groups.addBytesTo(bytes, "sets: groups"_s, "sets: index of groups"_s);
    m_blocks.addBytesTo(bytes, "sets: blocks"_s, "sets: index of blocks"_s);
    bytes.add("sets: results remembered by all threads"_s, sizeof(RememberedByAll) << m_bitsOfRememberedByAll);
}

ValueSets::ValueSets(uint32_t sizeOfUniverse)
    : m_sizeOfUniverse(sizeOfUniverse)
    , m_bitsOfRememberedByAll(std::clamp<unsigned>(std::bit_width(sizeOfUniverse), 12, 22) - 2)
    , m_rememberedByAll(std::make_unique<RememberedByAll[]>(static_cast<size_t>(1) << m_bitsOfRememberedByAll))
{
    RELEASE_ASSERT(sizeOfUniverse < namesOneMember);
}

static void forEachBlockOfMembers(uint64_t lowestMembers, std::span<const uint32_t> highMembers, NOESCAPE const Invocable<void(uint32_t place, uint64_t hashOfBlock)> auto& functor)
{
    constexpr uint32_t membersPerBlock = ValueSets::membersPerBlock;
    size_t index = 0;
    bool startsWithLowestMembers = lowestMembers;
    while (index < highMembers.size() || startsWithLowestMembers) {
        uint32_t place = startsWithLowestMembers ? 0 : highMembers[index] / membersPerBlock;
        uint64_t hash = startsWithLowestMembers ? mix(0, lowestMembers) : 0;
        startsWithLowestMembers = false;
        while (index < highMembers.size() && highMembers[index] / membersPerBlock == place) {
            uint32_t word = highMembers[index] / 64;
            uint64_t bits = 0;
            for (; index < highMembers.size() && highMembers[index] / 64 == word; ++index)
                bits |= 1ull << highMembers[index] % 64;
            hash += mix(word % (membersPerBlock / 64), bits);
        }
        functor(place, hash);
    }
}

uint64_t ValueSets::hashOfMembers(ValueSetID set) const
{
    RunOfOneMember room;
    const Run& run = runOf(storedSetOf(set), room);
    uint64_t stored = isShort(run) ? 0 : run.hashOfMembers;
    if (isShort(run)) {
        forEachBlockOfMembers(run.lowestMembers, highMembersOf(run), [&](uint32_t place, uint64_t hashOfBlock) {
            stored += mix(place, hashOfBlock);
        });
    }
    return isComplement(set) ? ~stored : stored;
}

static size_t numberBelow(std::span<const uint32_t> members, uint32_t member)
{
    const uint32_t* first = members.data();
    size_t size = members.size();
    while (size > 8) {
        size_t half = size / 2;
        first += first[half - 1] < member ? half : 0;
        size -= half;
    }
    size_t number = first - members.data();
    for (size_t index = 0; index < size; ++index)
        number += first[index] < member;
    return number;
}

static bool isAmong(std::span<const uint32_t> members, uint32_t member)
{
    size_t position = numberBelow(members, member);
    return position < members.size() && members[position] == member;
}

static bool hasFewBeside(std::span<const uint32_t> few, std::span<const uint32_t> many)
{
    return many.size() >= 8 && few.size() * 8 <= many.size();
}

static uint64_t hashOfHighMember(uint32_t member)
{
    uint64_t hash = (member + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull;
    return (hash ^ hash >> 32) * 0x94d049bb133111ebull;
}

std::array<uint64_t, 4> ValueSets::sampleOfBlocks(ValueSetID set) const
{
    RELEASE_ASSERT(!isComplement(set));
    std::array<uint64_t, 4> sample { noBlock, noBlock, noBlock, noBlock };
    auto take = [&](uint32_t place, uint64_t hashOfBlock) {
        uint64_t hash = (hashOfBlock ^ (place + 1) * 0xc2b2ae3d27d4eb4full) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 32;
        if (hash >= sample[3])
            return;
        size_t position = 3;
        for (; position && hash < sample[position - 1]; --position)
            sample[position] = sample[position - 1];
        sample[position] = hash;
    };
    RunOfOneMember room;
    const Run& run = runOf(storedSetOf(set), room);
    if (isShort(run)) {
        forEachBlockOfMembers(run.lowestMembers, highMembersOf(run), take);
        return sample;
    }
    forEachStoredBlock(run.span(), [&](uint32_t place, uint32_t block) {
        take(place, m_blocks.at(block).hashOfMembers);
    });
    return sample;
}

uint32_t ValueSets::blockAt(std::span<const uint64_t> list, uint32_t place) const
{
    uint32_t groupOfPlaces = place / placesPerGroup;
    size_t position = std::min<size_t>(groupOfPlaces, list.size() - 1);
    while (position && groupOfPlacesOf(list[position]) > groupOfPlaces)
        --position;
    if (groupOfPlacesOf(list[position]) != groupOfPlaces)
        return 0;
    uint64_t entry = list[position];
    if (!isGroup(entry))
        return placeOf(entry) == place ? blockOf(entry) : 0;
    const uint64_t* words = m_groups.at(groupOf(entry)).words();
    uint64_t bit = 1ull << place % placesPerGroup;
    return words[0] & bit ? blocksOfGroup(words)[std::popcount(words[0] & (bit - 1))] : 0;
}

bool ValueSets::hasHighMember(const Run& run, uint32_t member) const
{
    if (isShort(run))
        return isAmong(highMembersOf(run), member);
    uint32_t number = blockAt(run.span(), member / membersPerBlock);
    if (!number)
        return false;
    const uint64_t* words = m_blocks.at(number).words();
    unsigned word = member % membersPerBlock / 64;
    uint64_t present = words[0];
    return (present >> word & 1) && (words[1 + std::popcount(present & ((1ull << word) - 1))] >> member % 64 & 1);
}

ValueFlowObjectID ValueSets::onlyMemberOfComplement(ValueSetID set) const
{
    if (size(set) != 1)
        return { };
    ValueFlowObjectID only;
    forEachMember(set, [&](ValueFlowObjectID member) {
        only = member;
    });
    return only;
}

static void forEachCommonWord(const uint64_t* first, const uint64_t* second, NOESCAPE const Invocable<bool(uint64_t, uint64_t)> auto& stopsAt)
{
    uint64_t presentInFirst = first[0];
    uint64_t presentInSecond = second[0];
    for (uint64_t common = presentInFirst & presentInSecond; common; common &= common - 1) {
        uint64_t below = (common & -common) - 1;
        if (stopsAt(first[1 + std::popcount(presentInFirst & below)], second[1 + std::popcount(presentInSecond & below)]))
            return;
    }
}

size_t ValueSets::positionOfGroupOfPlaces(std::span<const uint64_t> list, size_t from, uint32_t groupOfPlaces)
{
    for (size_t end = std::min(from + 4, list.size()); from < end; ++from) {
        if (groupOfPlacesOf(list[from]) >= groupOfPlaces)
            return from;
    }
    return std::ranges::lower_bound(list.subspan(from), groupOfPlaces, { }, groupOfPlacesOf) - list.begin();
}

ALWAYS_INLINE bool ValueSets::anyEntriesOfSameGroupOfPlaces(std::span<const uint64_t> first, std::span<const uint64_t> second, NOESCAPE const Invocable<bool(uint64_t, uint64_t)> auto& functor)
{
    size_t i = 0;
    size_t j = 0;
    while (i < first.size() && j < second.size()) {
        uint32_t firstGroup = groupOfPlacesOf(first[i]);
        uint32_t secondGroup = groupOfPlacesOf(second[j]);
        if (firstGroup < secondGroup)
            i = positionOfGroupOfPlaces(first, i + 1, secondGroup);
        else if (secondGroup < firstGroup)
            j = positionOfGroupOfPlaces(second, j + 1, firstGroup);
        else if (functor(first[i++], second[j++]))
            return true;
    }
    return false;
}

ALWAYS_INLINE bool ValueSets::blocksMeet(uint32_t first, uint32_t second) const
{
    if (first == second)
        return true;
    bool meet = false;
    forEachCommonWord(m_blocks.at(first).words(), m_blocks.at(second).words(), [&](uint64_t a, uint64_t b) {
        meet = a & b;
        return meet;
    });
    return meet;
}

static uint32_t numberOfCommonElements(std::span<const uint32_t> first, std::span<const uint32_t> second)
{
    uint32_t number = 0;
    if (first.size() > second.size())
        std::swap(first, second);
    if (hasFewBeside(first, second)) {
        for (uint32_t member : first)
            number += isAmong(second, member);
        return number;
    }
    size_t i = 0;
    size_t j = 0;
    while (i < first.size() && j < second.size()) {
        uint32_t a = first[i];
        uint32_t b = second[j];
        number += a == b;
        i += a <= b;
        j += b <= a;
    }
    return number;
}

bool ValueSets::storedSetsMeet(ValueSetID first, ValueSetID second) const
{
    RunOfOneMember firstRoom;
    RunOfOneMember secondRoom;
    const Run* firstRun = &runOf(storedSetOf(first), firstRoom);
    const Run* secondRun = &runOf(storedSetOf(second), secondRoom);
    if (firstRun->lowestMembers & secondRun->lowestMembers)
        return true;
    if (isShort(*secondRun))
        std::swap(firstRun, secondRun);
    if (isShort(*firstRun)) {
        if (isShort(*secondRun))
            return numberOfCommonElements(highMembersOf(*firstRun), highMembersOf(*secondRun));
        MembersOfList others(*this, secondRun->span());
        for (uint32_t member : highMembersOf(*firstRun)) {
            if (others.contains(member))
                return true;
        }
        return false;
    }
    return anyEntriesOfSameGroupOfPlaces(firstRun->span(), secondRun->span(), [&](uint64_t firstEntry, uint64_t secondEntry) {
        if (firstEntry == secondEntry)
            return true;
        BlocksOfEntry a(*this, firstEntry);
        BlocksOfEntry b(*this, secondEntry);
        for (uint64_t common = a.present & b.present; common; common &= common - 1) {
            uint64_t bit = common & -common;
            if (blocksMeet(a.blockAt(bit), b.blockAt(bit)))
                return true;
        }
        return false;
    });
}

uint32_t ValueSets::numberOfCommonMembers(ValueSetID first, ValueSetID second) const
{
    RELEASE_ASSERT(!isComplement(first) && !isComplement(second));
    RunOfOneMember firstRoom;
    RunOfOneMember secondRoom;
    const Run* firstRun = &runOf(storedSetOf(first), firstRoom);
    const Run* secondRun = &runOf(storedSetOf(second), secondRoom);
    if (isShort(*secondRun))
        std::swap(firstRun, secondRun);
    if (isShort(*firstRun)) {
        uint32_t common = static_cast<uint32_t>(std::popcount(firstRun->lowestMembers & secondRun->lowestMembers));
        if (isShort(*secondRun))
            return common + numberOfCommonElements(highMembersOf(*firstRun), highMembersOf(*secondRun));
        MembersOfList others(*this, secondRun->span());
        for (uint32_t member : highMembersOf(*firstRun))
            common += others.contains(member);
        return common;
    }
    uint32_t number = 0;
    anyEntriesOfSameGroupOfPlaces(firstRun->span(), secondRun->span(), [&](uint64_t firstEntry, uint64_t secondEntry) {
        if (firstEntry == secondEntry && isGroup(firstEntry)) {
            number += m_groups.at(groupOf(firstEntry)).numberOfMembers;
            return false;
        }
        BlocksOfEntry a(*this, firstEntry);
        BlocksOfEntry b(*this, secondEntry);
        for (uint64_t common = a.present & b.present; common; common &= common - 1) {
            uint64_t bit = common & -common;
            uint32_t firstBlock = a.blockAt(bit);
            uint32_t secondBlock = b.blockAt(bit);
            if (firstBlock == secondBlock) {
                number += m_blocks.at(firstBlock).numberOfMembers;
                continue;
            }
            forEachCommonWord(m_blocks.at(firstBlock).words(), m_blocks.at(secondBlock).words(), [&](uint64_t x, uint64_t y) {
                number += std::popcount(x & y);
                return false;
            });
        }
        return false;
    });
    return number;
}

bool ValueSets::storedSetIsSubset(ValueSetID set, ValueSetID of) const
{
    RunOfOneMember room;
    RunOfOneMember otherRoom;
    const Run& run = runOf(storedSetOf(set), room);
    const Run& otherRun = runOf(storedSetOf(of), otherRoom);
    if ((run.lowestMembers & ~otherRun.lowestMembers) || numberOfHighMembers(run) > numberOfHighMembers(otherRun))
        return false;
    if (isShort(run)) {
        std::span<const uint32_t> members = highMembersOf(run);
        if (isShort(otherRun))
            return numberOfCommonElements(members, highMembersOf(otherRun)) == members.size();
        MembersOfList others(*this, otherRun.span());
        for (uint32_t member : members) {
            if (!others.contains(member))
                return false;
        }
        return true;
    }
    std::span<const uint64_t> list = run.span();
    std::span<const uint64_t> otherList = otherRun.span();
    size_t j = 0;
    if (list.size() > otherList.size())
        return false;
    for (uint64_t entry : list) {
        j = positionOfGroupOfPlaces(otherList, j, groupOfPlacesOf(entry));
        if (j == otherList.size() || groupOfPlacesOf(otherList[j]) != groupOfPlacesOf(entry))
            return false;
        if (otherList[j] == entry)
            continue;
        BlocksOfEntry a(*this, entry);
        BlocksOfEntry b(*this, otherList[j]);
        if (a.present & ~b.present)
            return false;
        const uint32_t* next = a.blocks;
        for (uint64_t present = a.present; present; present &= present - 1) {
            uint32_t number = *next++;
            uint32_t otherNumber = b.blockAt(present & -present);
            if (number == otherNumber)
                continue;
            const uint64_t* block = m_blocks.at(number).words();
            const uint64_t* otherBlock = m_blocks.at(otherNumber).words();
            if (block[0] & ~otherBlock[0])
                return false;
            bool sticksOut = false;
            forEachCommonWord(block, otherBlock, [&](uint64_t x, uint64_t y) {
                sticksOut = x & ~y;
                return sticksOut;
            });
            if (sticksOut)
                return false;
        }
    }
    return true;
}

bool ValueSets::setsOfSeveralMembersMeet(ValueSetID first, ValueSetID second) const
{
    if (!isComplement(first) && !isComplement(second))
        return storedSetsMeet(first, second);
    if (isComplement(first) && isComplement(second)) {
        uint32_t most = size(complement(first)) + size(complement(second));
        if (most < m_sizeOfUniverse)
            return true;
        bool meet = false;
        forEachMember(first, [&](ValueFlowObjectID member) {
            meet |= contains(second, member);
        });
        return meet;
    }
    if (isComplement(first))
        std::swap(first, second);
    return !storedSetIsSubset(first, complement(second));
}

ValueSetMaker::ValueSetMaker(ValueSets& sets)
    : m_sets(sets)
    , m_rememberedEntries(static_cast<size_t>(1) << bitsOfRememberedEntries)
    , m_rememberedBlocks(static_cast<size_t>(1) << bitsOfRememberedBlocks)
{
    m_blockAtPlace.fill(0u, roundUpToMultipleOf<ValueSets::placesPerGroup>(sets.sizeOfUniverse() / ValueSets::membersPerBlock + 1));
    m_entryAtGroupOfPlaces.fill(0u, m_blockAtPlace.size() / ValueSets::placesPerGroup);
}

static constexpr uint32_t placeInTable(uint32_t first, uint32_t second, unsigned bits)
{
    return (first * 0x9e3779b1u ^ second * 0x85ebca77u) >> (32 - bits);
}

uint32_t ValueSetMaker::combineBlocks(Operation operation, uint32_t first, uint32_t second)
{
    if (operation != Operation::Subtract && first > second)
        std::swap(first, second);
    uint32_t key = second | static_cast<uint32_t>(operation) << 30;
    Remembered& remembered = m_rememberedBlocks[placeInTable(first, key, bitsOfRememberedBlocks)];
    if (remembered.first == first && remembered.second == key)
        return remembered.result;

    const ValueSets::Run& firstRun = m_sets.m_blocks.at(first);
    const ValueSets::Run& secondRun = m_sets.m_blocks.at(second);
    const uint64_t* a = firstRun.words() + 1;
    const uint64_t* b = secondRun.words() + 1;
    uint64_t presentInFirst = firstRun.words()[0];
    uint64_t presentInSecond = secondRun.words()[0];

    std::array<uint64_t, ValueSets::wordsPerBlock + 1> result;
    unsigned size = 1;
    uint64_t present = 0;
    uint32_t numberOfMembers = 0;
    bool startsFromSecond = operation == Operation::Unite ? secondRun.numberOfMembers > firstRun.numberOfMembers : operation == Operation::Intersect && secondRun.numberOfMembers < firstRun.numberOfMembers;
    uint64_t hash = startsFromSecond ? secondRun.hashOfMembers : firstRun.hashOfMembers;
    for (uint64_t either = presentInFirst | presentInSecond; either; either &= either - 1) {
        uint64_t bit = either & -either;
        uint64_t x = presentInFirst & bit ? *a++ : 0;
        uint64_t y = presentInSecond & bit ? *b++ : 0;
        uint64_t word = operation == Operation::Unite ? x | y : operation == Operation::Intersect ? x & y : x & ~y;
        uint64_t counted = startsFromSecond ? y : x;
        if (word != counted) {
            uint64_t index = static_cast<uint64_t>(std::countr_zero(bit));
            if (counted)
                hash -= mix(index, counted);
            if (word)
                hash += mix(index, word);
        }
        if (!word)
            continue;
        present |= bit;
        result[size++] = word;
        numberOfMembers += static_cast<uint32_t>(std::popcount(word));
    }
    result[0] = present;

    uint32_t number;
    if (numberOfMembers == firstRun.numberOfMembers)
        number = first;
    else if (operation != Operation::Subtract && numberOfMembers == secondRun.numberOfMembers)
        number = second;
    else
        number = numberOfMembers ? m_sets.m_blocks.intern(m_roomForBlocks, std::span<const uint64_t>(result).first(size), numberOfMembers, hash) : 0;
    remembered = Remembered { first, key, number };
    return number;
}

ALWAYS_INLINE uint32_t ValueSetMaker::intern(BlockBeingMade& block)
{
    block.words[0] = block.present;
    return m_sets.m_blocks.intern(m_roomForBlocks, std::span<const uint64_t>(block.words).first(block.size), block.numberOfMembers, block.hash);
}

ALWAYS_INLINE auto ValueSetMaker::totalsOfBlock(uint32_t place, uint32_t block) const -> Totals
{
    const ValueSets::Run& run = m_sets.m_blocks.at(block);
    return { run.numberOfMembers, mix(place, run.hashOfMembers) };
}

ALWAYS_INLINE auto ValueSetMaker::totalsOf(uint64_t entry) const -> Totals
{
    if (!ValueSets::isGroup(entry))
        return totalsOfBlock(ValueSets::placeOf(entry), ValueSets::blockOf(entry));
    const ValueSets::Run& run = m_sets.m_groups.at(ValueSets::groupOf(entry));
    return { run.numberOfMembers, run.hashOfMembers };
}

uint64_t ValueSetMaker::entryFor(uint32_t firstPlace, uint64_t present, std::span<const uint32_t> blocks, Totals totals)
{
    if (blocks.empty())
        return 0;
    if (blocks.size() == 1)
        return ValueSets::entryOfBlock(firstPlace + static_cast<uint32_t>(std::countr_zero(present)), blocks[0]);
    std::array<uint64_t, 2 + ValueSets::placesPerGroup / 2> words;
    words[0] = present;
    size_t numberOfWords = 2 + blocks.size() / 2;
    words[numberOfWords - 1] = 0;
    uint32_t* halves = reinterpret_cast<uint32_t*>(words.data() + 1);
    halves[0] = firstPlace / ValueSets::placesPerGroup;
    std::ranges::copy(blocks, halves + 1);
    return ValueSets::entryOfGroup(firstPlace, m_sets.m_groups.intern(m_roomForGroups, std::span<const uint64_t>(words).first(numberOfWords), totals.numberOfMembers, totals.hashOfMembers));
}

uint64_t ValueSetMaker::entryFor(uint32_t firstPlace, uint64_t present, std::span<const uint32_t> blocks)
{
    Totals totals;
    if (blocks.size() > 1) {
        const uint32_t* block = blocks.data();
        for (uint64_t rest = present; rest; rest &= rest - 1)
            totals += totalsOfBlock(firstPlace + static_cast<uint32_t>(std::countr_zero(rest)), *block++);
    }
    return entryFor(firstPlace, present, blocks, totals);
}

uint64_t ValueSetMaker::combineEntries(Operation operation, uint64_t first, uint64_t second)
{
    if (first == second)
        return operation == Operation::Subtract ? 0 : first;
    if (!ValueSets::isGroup(first) && !ValueSets::isGroup(second) && ValueSets::placeOf(first) == ValueSets::placeOf(second)) {
        uint32_t block = combineBlocks(operation, ValueSets::blockOf(first), ValueSets::blockOf(second));
        return block ? ValueSets::entryOfBlock(ValueSets::placeOf(first), block) : 0;
    }
    if (operation != Operation::Subtract && first > second)
        std::swap(first, second);

    uint32_t firstPlace = ValueSets::placeOf(first) & ~(ValueSets::placesPerGroup - 1);
    bool isRemembered = ValueSets::isGroup(first) || ValueSets::isGroup(second);
    uint32_t detail = 1 + static_cast<uint32_t>(operation) + (ValueSets::placeOf(first) % ValueSets::placesPerGroup << 2) + (ValueSets::placeOf(second) % ValueSets::placesPerGroup << 8);
    RememberedEntries& remembered = m_rememberedEntries[placeInTable(static_cast<uint32_t>(first) + detail * 0xc2b2ae35u, static_cast<uint32_t>(second), bitsOfRememberedEntries)];
    if (isRemembered && remembered.first == static_cast<uint32_t>(first) && remembered.second == static_cast<uint32_t>(second) && remembered.detail == detail)
        return static_cast<uint64_t>(remembered.placeOfResult) << 32 | remembered.result;

    ValueSets::BlocksOfEntry a(m_sets, first);
    ValueSets::BlocksOfEntry b(m_sets, second);
    std::array<uint32_t, ValueSets::placesPerGroup> blocks;
    unsigned size = 0;
    uint64_t present = 0;
    bool isFirst = true;
    bool isSecond = true;
    bool startsFromSecond = operation == Operation::Unite && std::popcount(b.present) > std::popcount(a.present);
    Totals totals = operation == Operation::Intersect ? Totals { } : totalsOf(startsFromSecond ? second : first);
    const uint32_t* nextOfFirst = a.blocks;
    const uint32_t* nextOfSecond = b.blocks;
    for (uint64_t either = a.present | b.present; either; either &= either - 1) {
        uint64_t bit = either & -either;
        uint32_t place = firstPlace + static_cast<uint32_t>(std::countr_zero(either));
        uint32_t x = a.present & bit ? *nextOfFirst++ : 0;
        uint32_t y = b.present & bit ? *nextOfSecond++ : 0;
        uint32_t block;
        if (x && y)
            block = x == y ? (operation == Operation::Subtract ? 0 : x) : combineBlocks(operation, x, y);
        else
            block = operation == Operation::Unite ? x | y : operation == Operation::Subtract ? x : 0;
        isFirst &= block == x;
        isSecond &= block == y;
        uint32_t counted = operation == Operation::Intersect ? 0 : startsFromSecond ? y : x;
        if (block != counted) {
            if (counted)
                totals -= totalsOfBlock(place, counted);
            if (block)
                totals += totalsOfBlock(place, block);
        }
        if (!block)
            continue;
        present |= bit;
        blocks[size++] = block;
    }
    uint64_t result = isFirst ? first : isSecond ? second : entryFor(firstPlace, present, std::span<const uint32_t>(blocks).first(size), totals);
    if (isRemembered)
        remembered = RememberedEntries { static_cast<uint32_t>(first), static_cast<uint32_t>(second), detail, ValueSets::placeOf(result), static_cast<uint32_t>(result) };
    return result;
}

uint32_t ValueSetMaker::internList(std::span<const uint64_t> list)
{
    Totals totals;
    for (uint64_t entry : list)
        totals += totalsOf(entry);
    uint64_t lowestMembers = 0;
    if (!list.empty() && !ValueSets::placeOf(list[0])) {
        ValueSets::BlocksOfEntry blocks(m_sets, list[0]);
        if (blocks.present & 1) {
            const uint64_t* words = m_sets.m_blocks.at(blocks.blocks[0]).words();
            if (words[0] & 1)
                lowestMembers = words[1];
        }
    }
    return internList(list, totals, lowestMembers);
}

uint32_t ValueSetMaker::internList(std::span<const uint64_t> list, Totals totals, uint64_t lowestMembers)
{
    if (totals.numberOfMembers - static_cast<uint32_t>(std::popcount(lowestMembers)) > ValueSets::mostHighMembersOfShortSet)
        return m_sets.m_lists.intern(m_roomForLists, list, totals.numberOfMembers, totals.hashOfMembers, lowestMembers);

    std::array<uint32_t, ValueSets::mostHighMembersOfShortSet> highMembers;
    size_t size = 0;
    m_sets.forEachStoredBlock(list, [&](uint32_t place, uint32_t block) {
        const uint64_t* words = m_sets.m_blocks.at(block).words();
        const uint64_t* word = words + 1;
        for (uint64_t present = words[0]; present; present &= present - 1, ++word) {
            uint32_t firstOfWord = place * ValueSets::membersPerBlock + static_cast<uint32_t>(std::countr_zero(present)) * 64;
            if (!firstOfWord)
                continue;
            for (uint64_t bits = *word; bits; bits &= bits - 1)
                highMembers[size++] = firstOfWord + static_cast<uint32_t>(std::countr_zero(bits));
        }
    });
    return internShortSet(lowestMembers, std::span<const uint32_t>(highMembers).first(size));
}

uint32_t ValueSetMaker::internShortSet(uint64_t lowestMembers, std::span<const uint32_t> highMembers)
{
    if (highMembers.empty() && std::has_single_bit(lowestMembers))
        return ValueSets::storedSetOfOneMember(static_cast<uint32_t>(std::countr_zero(lowestMembers)));
    if (highMembers.size() == 1 && !lowestMembers)
        return ValueSets::storedSetOfOneMember(highMembers[0]);

    uint64_t hashOfHighMembers = 0;
    for (uint32_t member : highMembers)
        hashOfHighMembers += hashOfHighMember(member);
    return internShortSet(lowestMembers, highMembers, hashOfHighMembers);
}

uint32_t ValueSetMaker::internShortSet(uint64_t lowestMembers, std::span<const uint32_t> highMembers, uint64_t hashOfHighMembers)
{
    uint64_t hashOfMembers = hashOfHighMembers + mix(highMembers.size(), lowestMembers);
    std::array<uint64_t, (ValueSets::mostHighMembersOfShortSet + 1) / 2> words;
    size_t numberOfWords = (highMembers.size() + 1) / 2;
    if (numberOfWords)
        words[numberOfWords - 1] = 0;
    memcpy(words.data(), highMembers.data(), highMembers.size_bytes());
    return m_sets.m_lists.intern(m_roomForLists, std::span<const uint64_t>(words).first(numberOfWords), static_cast<uint32_t>(std::popcount(lowestMembers) + highMembers.size()), hashOfMembers, lowestMembers);
}

uint32_t ValueSetMaker::internMembers(uint64_t lowestMembers, std::span<const uint32_t> highMembers)
{
    if (highMembers.size() <= ValueSets::mostHighMembersOfShortSet)
        return internShortSet(lowestMembers, highMembers);

    m_listOfBlocks.shrink(0);
    size_t index = 0;
    bool startsWithLowestMembers = lowestMembers;
    while (index < highMembers.size() || startsWithLowestMembers) {
        uint32_t place = startsWithLowestMembers ? 0 : highMembers[index] / ValueSets::membersPerBlock;
        BlockBeingMade block;
        if (std::exchange(startsWithLowestMembers, false))
            block.add(0, lowestMembers);
        while (index < highMembers.size() && highMembers[index] / ValueSets::membersPerBlock == place) {
            unsigned word = highMembers[index] % ValueSets::membersPerBlock / 64;
            uint64_t bits = 0;
            for (; index < highMembers.size() && highMembers[index] / 64 == place * ValueSets::wordsPerBlock + word; ++index)
                bits |= 1ull << highMembers[index] % 64;
            block.add(word, bits);
        }
        m_listOfBlocks.append(static_cast<uint64_t>(place) << 32 | intern(block));
    }
    return internListOfBlocks(m_listOfBlocks.span());
}

uint32_t ValueSetMaker::combineShortSets(Operation operation, uint32_t first, uint32_t second)
{
    ValueSets::RunOfOneMember firstRoom;
    ValueSets::RunOfOneMember secondRoom;
    const ValueSets::Run& a = m_sets.runOf(first, firstRoom);
    const ValueSets::Run& b = m_sets.runOf(second, secondRoom);
    std::span<const uint32_t> x = ValueSets::highMembersOf(a);
    std::span<const uint32_t> y = ValueSets::highMembersOf(b);
    if (hasFewBeside(x, y) || hasFewBeside(y, x))
        return combineShortSetsOfUnlikeSizes(operation, first, a, second, b);
    std::array<uint32_t, 2 * ValueSets::mostHighMembersOfShortSet> members;
    size_t size = 0;
    size_t i = 0;
    size_t j = 0;
    uint64_t lowestMembers;
    switch (operation) {
    case Operation::Unite:
        lowestMembers = a.lowestMembers | b.lowestMembers;
        while (i < x.size() && j < y.size()) {
            uint32_t p = x[i];
            uint32_t q = y[j];
            members[size++] = std::min(p, q);
            i += p <= q;
            j += q <= p;
        }
        for (; i < x.size(); ++i)
            members[size++] = x[i];
        for (; j < y.size(); ++j)
            members[size++] = y[j];
        break;
    case Operation::Intersect:
        lowestMembers = a.lowestMembers & b.lowestMembers;
        while (i < x.size() && j < y.size()) {
            uint32_t p = x[i];
            uint32_t q = y[j];
            members[size] = p;
            size += p == q;
            i += p <= q;
            j += q <= p;
        }
        break;
    case Operation::Subtract:
        lowestMembers = a.lowestMembers & ~b.lowestMembers;
        while (i < x.size() && j < y.size()) {
            uint32_t p = x[i];
            uint32_t q = y[j];
            members[size] = p;
            size += p < q;
            i += p <= q;
            j += q <= p;
        }
        for (; i < x.size(); ++i)
            members[size++] = x[i];
        break;
    }
    if (lowestMembers == a.lowestMembers && size == x.size())
        return first;
    if (operation != Operation::Subtract && lowestMembers == b.lowestMembers && size == y.size())
        return second;
    return internMembers(lowestMembers, std::span<const uint32_t>(members).first(size));
}

uint32_t ValueSetMaker::combineShortSetsOfUnlikeSizes(Operation operation, uint32_t first, const ValueSets::Run& a, uint32_t second, const ValueSets::Run& b)
{
    bool firstHasFew = ValueSets::numberOfHighMembers(a) <= ValueSets::numberOfHighMembers(b);
    std::span<const uint32_t> few = ValueSets::highMembersOf(firstHasFew ? a : b);
    std::span<const uint32_t> many = ValueSets::highMembersOf(firstHasFew ? b : a);
    const ValueSets::Run& runOfMany = firstHasFew ? b : a;
    uint64_t lowestMembers = operation == Operation::Unite ? a.lowestMembers | b.lowestMembers : operation == Operation::Intersect ? a.lowestMembers & b.lowestMembers : a.lowestMembers & ~b.lowestMembers;

    bool keepsCommonMembers = operation == Operation::Intersect || (operation == Operation::Subtract && !firstHasFew);
    std::array<uint32_t, ValueSets::mostHighMembersOfShortSet / 8 + 1> kept;
    std::array<uint32_t, ValueSets::mostHighMembersOfShortSet / 8 + 1> positions;
    size_t size = 0;
    for (uint32_t member : few) {
        size_t position = numberBelow(many, member);
        bool isCommon = position < many.size() && many[position] == member;
        kept[size] = member;
        positions[size] = static_cast<uint32_t>(position);
        size += isCommon == keepsCommonMembers;
    }
    if (operation == Operation::Intersect || (operation == Operation::Subtract && firstHasFew)) {
        if (size == few.size() && lowestMembers == (firstHasFew ? a : b).lowestMembers)
            return firstHasFew ? first : second;
        return internShortSet(lowestMembers, std::span<const uint32_t>(kept).first(size));
    }
    if (!size && lowestMembers == runOfMany.lowestMembers)
        return firstHasFew ? second : first;

    bool adds = operation == Operation::Unite;
    std::array<uint32_t, ValueSets::mostHighMembersOfShortSet + ValueSets::mostHighMembersOfShortSet / 8 + 1> members;
    uint64_t hashOfHighMembers = runOfMany.hashOfMembers - mix(many.size(), runOfMany.lowestMembers);
    size_t numberOfMembers = 0;
    size_t from = 0;
    for (size_t index = 0; index < size; ++index) {
        size_t position = positions[index];
        memcpy(members.data() + numberOfMembers, many.data() + from, (position - from) * sizeof(uint32_t));
        numberOfMembers += position - from;
        from = position;
        if (adds) {
            members[numberOfMembers++] = kept[index];
            hashOfHighMembers += hashOfHighMember(kept[index]);
        } else {
            ++from;
            hashOfHighMembers -= hashOfHighMember(kept[index]);
        }
    }
    memcpy(members.data() + numberOfMembers, many.data() + from, (many.size() - from) * sizeof(uint32_t));
    numberOfMembers += many.size() - from;
    std::span<const uint32_t> result = std::span<const uint32_t>(members).first(numberOfMembers);
    if (numberOfMembers > ValueSets::mostHighMembersOfShortSet || numberOfMembers + static_cast<size_t>(std::popcount(lowestMembers)) < 2)
        return internMembers(lowestMembers, result);
    return internShortSet(lowestMembers, result, hashOfHighMembers);
}

uint32_t ValueSetMaker::combineShortSetWithList(Operation operation, uint32_t first, uint32_t second)
{
    bool firstIsShort = ValueSets::holdsOneMember(first) || ValueSets::isShort(m_sets.m_lists.at(first));
    uint32_t shortSet = firstIsShort ? first : second;
    uint32_t list = firstIsShort ? second : first;
    ValueSets::RunOfOneMember room;
    const ValueSets::Run& a = m_sets.runOf(shortSet, room);
    const ValueSets::Run& b = m_sets.m_lists.at(list);
    std::span<const uint32_t> x = ValueSets::highMembersOf(a);
    ValueSets::MembersOfList membersOfList(m_sets, b.span());
    bool keepsCommonMembers = operation == Operation::Intersect || (operation == Operation::Subtract && !firstIsShort);
    std::array<uint32_t, ValueSets::mostHighMembersOfShortSet> members;
    size_t size = 0;
    for (uint32_t member : x) {
        members[size] = member;
        size += membersOfList.contains(member) == keepsCommonMembers;
    }
    uint64_t lowestMembers = keepsCommonMembers ? a.lowestMembers & b.lowestMembers : a.lowestMembers & ~b.lowestMembers;
    std::span<const uint32_t> kept = std::span<const uint32_t>(members).first(size);

    if (operation == Operation::Intersect || (operation == Operation::Subtract && firstIsShort)) {
        if (lowestMembers == a.lowestMembers && size == x.size())
            return shortSet;
        return internShortSet(lowestMembers, kept);
    }
    if (!lowestMembers && !size)
        return list;
    if (operation == Operation::Unite)
        return changeList(list, true, lowestMembers, kept);

    uint32_t numberLeft = ValueSets::numberOfHighMembers(b) - static_cast<uint32_t>(size);
    if (numberLeft > ValueSets::mostHighMembersOfShortSet)
        return changeList(list, false, lowestMembers, kept);
    std::array<uint32_t, ValueSets::mostHighMembersOfShortSet> left;
    size_t numberTaken = 0;
    size_t next = 0;
    m_sets.forEachStoredMember(ValueSetID(list * 2), [&](uint32_t member) {
        if (member < 64)
            return;
        if (next < kept.size() && kept[next] == member) {
            ++next;
            return;
        }
        left[numberTaken++] = member;
    });
    return internShortSet(b.lowestMembers & ~lowestMembers, std::span<const uint32_t>(left).first(numberTaken));
}

uint32_t ValueSetMaker::changeBlock(uint32_t block, bool adds, std::span<const BitsOfWord> changes)
{
    const ValueSets::Run& run = m_sets.m_blocks.at(block);
    uint64_t presentBefore = block ? run.words()[0] : 0;
    const uint64_t* before = run.words() + 1;
    uint64_t changed = 0;
    for (const BitsOfWord& change : changes)
        changed |= 1ull << change.word % ValueSets::wordsPerBlock;

    std::array<uint64_t, ValueSets::wordsPerBlock + 1> result;
    unsigned size = 1;
    uint64_t present = 0;
    uint32_t numberOfMembers = run.numberOfMembers;
    uint64_t hash = run.hashOfMembers;
    const BitsOfWord* change = changes.data();
    for (uint64_t either = presentBefore | changed; either; either &= either - 1) {
        uint64_t bit = either & -either;
        uint64_t word = presentBefore & bit ? *before++ : 0;
        if (changed & bit) {
            uint64_t index = static_cast<uint64_t>(std::countr_zero(bit));
            uint64_t after = adds ? word | change->bits : word & ~change->bits;
            ++change;
            if (word)
                hash -= mix(index, word);
            if (after)
                hash += mix(index, after);
            numberOfMembers += static_cast<uint32_t>(std::popcount(after)) - static_cast<uint32_t>(std::popcount(word));
            word = after;
        }
        if (!word)
            continue;
        present |= bit;
        result[size++] = word;
    }
    result[0] = present;
    return numberOfMembers ? m_sets.m_blocks.intern(m_roomForBlocks, std::span<const uint64_t>(result).first(size), numberOfMembers, hash) : 0;
}

uint32_t ValueSetMaker::changeList(uint32_t number, bool adds, uint64_t lowestMembers, std::span<const uint32_t> highMembers)
{
    m_bitsOfWords.shrink(0);
    if (lowestMembers)
        m_bitsOfWords.append(BitsOfWord { 0, lowestMembers });
    for (size_t index = 0; index < highMembers.size();) {
        uint32_t word = highMembers[index] / 64;
        uint64_t bits = 0;
        for (; index < highMembers.size() && highMembers[index] / 64 == word; ++index)
            bits |= 1ull << highMembers[index] % 64;
        m_bitsOfWords.append(BitsOfWord { word, bits });
    }
    std::span<const BitsOfWord> changes = m_bitsOfWords.span();

    const ValueSets::Run& run = m_sets.m_lists.at(number);
    std::span<const uint64_t> list = run.span();
    Totals totals { run.numberOfMembers, run.hashOfMembers };
    constexpr uint32_t wordsPerGroup = ValueSets::wordsPerBlock * ValueSets::placesPerGroup;
    m_list.shrink(0);
    size_t position = 0;
    for (size_t next = 0; next < changes.size();) {
        uint32_t groupOfPlaces = changes[next].word / wordsPerGroup;
        size_t positionOfGroup = ValueSets::positionOfGroupOfPlaces(list, position, groupOfPlaces);
        m_list.append(list.subspan(position, positionOfGroup - position));
        position = positionOfGroup;

        std::array<uint32_t, ValueSets::placesPerGroup> blocks;
        unsigned size = 0;
        uint64_t present = 0;
        Totals ofGroup;
        if (position < list.size() && ValueSets::groupOfPlacesOf(list[position]) == groupOfPlaces) {
            uint64_t entry = list[position++];
            ValueSets::BlocksOfEntry before(m_sets, entry);
            present = before.present;
            size = static_cast<unsigned>(std::popcount(present));
            std::copy_n(before.blocks, size, blocks.data());
            ofGroup = totalsOf(entry);
            totals -= ofGroup;
        }
        while (next < changes.size() && changes[next].word / wordsPerGroup == groupOfPlaces) {
            uint32_t place = changes[next].word / ValueSets::wordsPerBlock;
            size_t end = next + 1;
            while (end < changes.size() && changes[end].word / ValueSets::wordsPerBlock == place)
                ++end;
            uint64_t bit = 1ull << place % ValueSets::placesPerGroup;
            unsigned index = static_cast<unsigned>(std::popcount(present & (bit - 1)));
            uint32_t before = present & bit ? blocks[index] : 0;
            uint32_t after = changeBlock(before, adds, changes.subspan(next, end - next));
            next = end;
            if (before)
                ofGroup -= totalsOfBlock(place, before);
            if (after)
                ofGroup += totalsOfBlock(place, after);
            if (before && after)
                blocks[index] = after;
            else if (after) {
                std::copy_backward(blocks.data() + index, blocks.data() + size, blocks.data() + size + 1);
                blocks[index] = after;
                present |= bit;
                ++size;
            } else {
                std::copy(blocks.data() + index + 1, blocks.data() + size, blocks.data() + index);
                present &= ~bit;
                --size;
            }
        }
        if (uint64_t entry = entryFor(groupOfPlaces * ValueSets::placesPerGroup, present, std::span<const uint32_t>(blocks).first(size), ofGroup))
            m_list.append(entry);
        totals += ofGroup;
    }
    m_list.append(list.subspan(position));
    return m_sets.m_lists.intern(m_roomForLists, m_list.span(), totals.numberOfMembers, totals.hashOfMembers, adds ? run.lowestMembers | lowestMembers : run.lowestMembers & ~lowestMembers);
}

bool ValueSetMaker::gatherShortResult(Operation operation, std::span<const uint64_t> a, std::span<const uint64_t> b)
{
    bool intersects = operation == Operation::Intersect;
    m_highMembers.shrink(0);
    auto takeBits = [&](uint32_t firstOfWord, uint64_t bits) {
        if (!firstOfWord)
            return true;
        if (m_highMembers.size() + static_cast<size_t>(std::popcount(bits)) > ValueSets::mostHighMembersOfShortSet)
            return false;
        for (; bits; bits &= bits - 1)
            m_highMembers.append(firstOfWord + static_cast<uint32_t>(std::countr_zero(bits)));
        return true;
    };
    auto takeBlock = [&](uint32_t place, uint32_t block) {
        const ValueSets::Run& run = m_sets.m_blocks.at(block);
        if (run.numberOfMembers > ValueSets::mostHighMembersOfShortSet + 64)
            return false;
        const uint64_t* word = run.words() + 1;
        for (uint64_t present = run.words()[0]; present; present &= present - 1, ++word) {
            if (!takeBits(place * ValueSets::membersPerBlock + static_cast<uint32_t>(std::countr_zero(present)) * 64, *word))
                return false;
        }
        return true;
    };
    auto takeEntry = [&](uint64_t entry) {
        return !m_sets.anyStoredBlock(singleElementSpan(entry), [&](uint32_t place, uint32_t block) {
            return !takeBlock(place, block);
        });
    };
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        uint32_t firstGroup = ValueSets::groupOfPlacesOf(a[i]);
        uint32_t secondGroup = ValueSets::groupOfPlacesOf(b[j]);
        if (firstGroup < secondGroup) {
            if (!intersects && !takeEntry(a[i]))
                return false;
            ++i;
            continue;
        }
        if (secondGroup < firstGroup) {
            j = ValueSets::positionOfGroupOfPlaces(b, j + 1, firstGroup);
            continue;
        }
        uint64_t first = a[i++];
        uint64_t second = b[j++];
        if (first == second) {
            if (intersects && !takeEntry(first))
                return false;
            continue;
        }
        ValueSets::BlocksOfEntry x(m_sets, first);
        ValueSets::BlocksOfEntry y(m_sets, second);
        uint32_t firstPlace = ValueSets::placeOf(first) & ~(ValueSets::placesPerGroup - 1);
        const uint32_t* next = x.blocks;
        for (uint64_t present = x.present; present; present &= present - 1) {
            uint64_t bit = present & -present;
            uint32_t place = firstPlace + static_cast<uint32_t>(std::countr_zero(present));
            uint32_t block = *next++;
            if (!(y.present & bit)) {
                if (!intersects && !takeBlock(place, block))
                    return false;
                continue;
            }
            uint32_t other = y.blockAt(bit);
            if (block == other) {
                if (intersects && !takeBlock(place, block))
                    return false;
                continue;
            }
            const uint64_t* words = m_sets.m_blocks.at(block).words();
            const uint64_t* otherWords = m_sets.m_blocks.at(other).words();
            const uint64_t* word = words + 1;
            for (uint64_t wordsPresent = words[0]; wordsPresent; wordsPresent &= wordsPresent - 1, ++word) {
                uint64_t bitOfWord = wordsPresent & -wordsPresent;
                uint64_t otherWord = otherWords[0] & bitOfWord ? otherWords[1 + std::popcount(otherWords[0] & (bitOfWord - 1))] : 0;
                uint64_t bits = intersects ? *word & otherWord : *word & ~otherWord;
                if (bits && !takeBits(place * ValueSets::membersPerBlock + static_cast<uint32_t>(std::countr_zero(wordsPresent)) * 64, bits))
                    return false;
            }
        }
    }
    if (!intersects) {
        for (; i < a.size(); ++i) {
            if (!takeEntry(a[i]))
                return false;
        }
    }
    return true;
}

uint32_t ValueSetMaker::internListOfBlocks(std::span<const uint64_t> listOfBlocks)
{
    m_list.shrink(0);
    std::array<uint32_t, ValueSets::placesPerGroup> blocks;
    for (size_t i = 0; i < listOfBlocks.size();) {
        uint32_t groupOfPlaces = ValueSets::groupOfPlacesOf(listOfBlocks[i]);
        unsigned size = 0;
        uint64_t present = 0;
        for (; i < listOfBlocks.size() && ValueSets::groupOfPlacesOf(listOfBlocks[i]) == groupOfPlaces; ++i) {
            present |= 1ull << ValueSets::placeOf(listOfBlocks[i]) % ValueSets::placesPerGroup;
            blocks[size++] = ValueSets::blockOf(listOfBlocks[i]);
        }
        m_list.append(entryFor(groupOfPlaces * ValueSets::placesPerGroup, present, std::span<const uint32_t>(blocks).first(size)));
    }
    return internList(m_list.span());
}

uint32_t ValueSetMaker::combine(Operation operation, uint32_t first, uint32_t second)
{
    switch (operation) {
    case Operation::Unite:
        if (first == second || !second)
            return first;
        if (!first)
            return second;
        break;
    case Operation::Intersect:
        if (first == second || !first || !second)
            return first == second ? first : 0;
        break;
    case Operation::Subtract:
        if (!first || first == second || !second)
            return second ? 0 : first;
        break;
    }
    if (operation != Operation::Subtract && first > second)
        std::swap(first, second);

    bool firstIsOne = ValueSets::holdsOneMember(first);
    bool secondIsOne = ValueSets::holdsOneMember(second);
    if (firstIsOne && secondIsOne && operation != Operation::Unite)
        return operation == Operation::Subtract ? first : 0;

    uint32_t firstKey = first | (static_cast<uint32_t>(operation) & 1) << 31;
    uint32_t secondKey = second | (static_cast<uint32_t>(operation) >> 1) << 31;
    bool isCheap = operation == Operation::Intersect ? firstIsOne || secondIsOne : operation == Operation::Subtract && firstIsOne;
    uint64_t key = static_cast<uint64_t>(firstKey) << 32 | secondKey;
    if (!isCheap)
        __builtin_prefetch(&m_sets.rememberedByAllAt(key));
    if (!firstIsOne)
        __builtin_prefetch(&m_sets.m_lists.at(first));
    if (!secondIsOne)
        __builtin_prefetch(&m_sets.m_lists.at(second));
    if (!isCheap) {
        if (std::optional<uint32_t> result = m_sets.findRememberedByAll(key))
            return *result;
    }

    uint32_t result = combineByForm(operation, first, second);
    if (!isCheap)
        m_sets.rememberForAll(key, result);
    return result;
}

uint32_t ValueSetMaker::combineByForm(Operation operation, uint32_t first, uint32_t second)
{
    bool firstIsOne = ValueSets::holdsOneMember(first);
    if (firstIsOne || ValueSets::holdsOneMember(second)) {
        uint32_t one = firstIsOne ? first : second;
        uint32_t other = firstIsOne ? second : first;
        bool isInOther = m_sets.contains(ValueSetID(other * 2), ValueFlowObjectID(ValueSets::onlyMemberOf(one)));
        if (operation == Operation::Intersect || (operation == Operation::Subtract && firstIsOne))
            return isInOther == (operation == Operation::Intersect) ? one : 0;
        if (isInOther == (operation == Operation::Unite))
            return other;
    }
    bool firstIsShort = firstIsOne || ValueSets::isShort(m_sets.m_lists.at(first));
    bool secondIsShort = ValueSets::holdsOneMember(second) || ValueSets::isShort(m_sets.m_lists.at(second));
    return firstIsShort && secondIsShort ? combineShortSets(operation, first, second) : firstIsShort || secondIsShort ? combineShortSetWithList(operation, first, second) : combineLists(operation, first, second);
}

uint32_t ValueSetMaker::combineLists(Operation operation, uint32_t first, uint32_t second)
{
    const ValueSets::Run& firstRun = m_sets.m_lists.at(first);
    const ValueSets::Run& secondRun = m_sets.m_lists.at(second);
    std::span<const uint64_t> a = firstRun.span();
    std::span<const uint64_t> b = secondRun.span();
    uint64_t lowestMembers = operation == Operation::Unite ? firstRun.lowestMembers | secondRun.lowestMembers : operation == Operation::Intersect ? firstRun.lowestMembers & secondRun.lowestMembers : firstRun.lowestMembers & ~secondRun.lowestMembers;
    if (operation != Operation::Unite) {
        bool walksSecond = operation == Operation::Intersect && secondRun.numberOfMembers < firstRun.numberOfMembers;
        if ((walksSecond ? secondRun : firstRun).numberOfMembers <= mostMembersOfListGatheredFrom && gatherShortResult(operation, walksSecond ? b : a, walksSecond ? a : b))
            return internShortSet(lowestMembers, m_highMembers.span());
    }
    bool startsFromSecond = operation == Operation::Unite && secondRun.numberOfMembers > firstRun.numberOfMembers;
    Totals totals;
    if (operation != Operation::Intersect)
        totals = startsFromSecond ? Totals { secondRun.numberOfMembers, secondRun.hashOfMembers } : Totals { firstRun.numberOfMembers, firstRun.hashOfMembers };
    auto takeFromFirst = [&](std::span<const uint64_t> entries) {
        if (operation == Operation::Intersect)
            return;
        m_list.append(entries);
        if (startsFromSecond) {
            for (uint64_t entry : entries)
                totals += totalsOf(entry);
        }
    };
    auto takeFromSecond = [&](std::span<const uint64_t> entries) {
        if (operation != Operation::Unite)
            return;
        m_list.append(entries);
        if (!startsFromSecond) {
            for (uint64_t entry : entries)
                totals += totalsOf(entry);
        }
    };
    m_list.shrink(0);
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        uint32_t firstGroup = ValueSets::groupOfPlacesOf(a[i]);
        uint32_t secondGroup = ValueSets::groupOfPlacesOf(b[j]);
        if (firstGroup < secondGroup) {
            size_t next = ValueSets::positionOfGroupOfPlaces(a, i + 1, secondGroup);
            takeFromFirst(a.subspan(i, next - i));
            i = next;
            continue;
        }
        if (secondGroup < firstGroup) {
            size_t next = ValueSets::positionOfGroupOfPlaces(b, j + 1, firstGroup);
            takeFromSecond(b.subspan(j, next - j));
            j = next;
            continue;
        }
        uint64_t entry = combineEntries(operation, a[i], b[j]);
        uint64_t counted = operation == Operation::Intersect ? 0 : startsFromSecond ? b[j] : a[i];
        if (entry != counted) {
            if (counted)
                totals -= totalsOf(counted);
            if (entry)
                totals += totalsOf(entry);
        }
        if (entry)
            m_list.append(entry);
        ++i;
        ++j;
    }
    takeFromFirst(a.subspan(i));
    takeFromSecond(b.subspan(j));

    if (totals.numberOfMembers == firstRun.numberOfMembers)
        return first;
    if (operation != Operation::Subtract && totals.numberOfMembers == secondRun.numberOfMembers)
        return second;
    return internList(m_list.span(), totals, lowestMembers);
}

NEVER_INLINE ValueSetID ValueSetMaker::intersectDifferentSets(ValueSetID first, ValueSetID second)
{
    if (first.index() > second.index())
        std::swap(first, second);
    Remembered& remembered = m_rememberedSets[placeInTable(first.index(), second.index(), bitsOfRememberedSets)];
    if (remembered.first == first.index() && remembered.second == second.index() && remembered.result != intersectionIsNotEmpty) [[likely]]
        return ValueSetID(remembered.result);
    return intersectUncached(first, second, remembered);
}

NEVER_INLINE ValueSetID ValueSetMaker::intersectUncached(ValueSetID first, ValueSetID second, Remembered& remembered)
{
    uint32_t a = ValueSets::storedSetOf(first);
    uint32_t b = ValueSets::storedSetOf(second);
    if (ValueSets::holdsOneMember(b)) {
        bool isHeld = m_sets.contains(first, ValueFlowObjectID(ValueSets::onlyMemberOf(b)));
        if (!ValueSets::isComplement(second) || !isHeld) {
            ValueSetID answer = ValueSets::isComplement(second) ? first : isHeld ? second : ValueSets::nothing;
            remembered = Remembered { first.index(), second.index(), answer.index() };
            return answer;
        }
    }
    ValueSetID result;
    if (!ValueSets::isComplement(first))
        result = ValueSetID(combine(ValueSets::isComplement(second) ? Operation::Subtract : Operation::Intersect, a, b) * 2);
    else
        result = ValueSets::isComplement(second) ? ValueSetID(combine(Operation::Unite, a, b) * 2 + 1) : ValueSetID(combine(Operation::Subtract, b, a) * 2);
    remembered = Remembered { first.index(), second.index(), result.index() };
    return result;
}

NEVER_INLINE bool ValueSetMaker::nonEmptySetsMeet(ValueSetID first, ValueSetID second)
{
    if (first.index() > second.index())
        std::swap(first, second);
    Remembered& remembered = m_rememberedSets[placeInTable(first.index(), second.index(), bitsOfRememberedSets)];
    if (remembered.first == first.index() && remembered.second == second.index() && !ValueSets::isComplement(ValueSetID(remembered.result))) [[likely]]
        return remembered.result;
    return meetUncached(first, second, remembered);
}

NEVER_INLINE bool ValueSetMaker::meetUncached(ValueSetID first, ValueSetID second, Remembered& remembered)
{
    bool result = m_sets.meet(first, second);
    bool isCached = remembered.first == first.index() && remembered.second == second.index();
    if (!isCached && !(ValueSets::isComplement(first) && ValueSets::isComplement(second)))
        remembered = Remembered { first.index(), second.index(), result ? intersectionIsNotEmpty : 0 };
    return result;
}

ValueSetID ValueSetMaker::pass(ValueSetID value, ValueSetID mask, ValueSetID familyObjects, ValueFlowMaskKind kind, const ValueFlowPlaceholders& placeholders, const ValueFlowCoveredObjects& covered)
{
    if (mask == ValueSets::anything || value == ValueSets::nothing)
        return value;

    switch (kind) {
    case ValueFlowMaskKind::Plain:
        return intersect(value, mask);
    case ValueFlowMaskKind::Checked: {
        ValueSetID passed = intersect(value, mask);
        if (!familyObjects)
            return passed;
        ValueSetID stoodFor = ValueSets::nothing;
        for (auto [placeholder, coveredObjects] : { std::pair { placeholders.untracked, covered.byUntracked }, std::pair { placeholders.unknown, covered.byUnknown } }) {
            if (!placeholder || !m_sets.contains(value, placeholder))
                continue;
            stoodFor = unite(stoodFor, coveredObjects);
            passed = intersect(passed, ValueSets::complement(of(placeholder)));
        }
        return stoodFor == ValueSets::nothing ? passed : unite(passed, intersect(stoodFor, intersect(familyObjects, mask)));
    }
    case ValueFlowMaskKind::Declared:
    case ValueFlowMaskKind::Expected:
        if (!familyObjects || !placeholders.untracked || !m_sets.contains(value, placeholders.untracked))
            return value;
        return unite(intersect(value, ValueSets::complement(of(placeholders.untracked))), intersect(covered.byUntracked, intersect(familyObjects, mask)));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE void ValueSetMaker::scatter(uint32_t index)
{
    uint32_t place = index / ValueSets::membersPerBlock;
    m_scatteredMembers[index / 64] |= 1ull << index % 64;
    m_wordsTouchedAtPlace[place] |= 1ull << index / 64 % ValueSets::wordsPerBlock;
    m_placesTouched[place / 64] |= 1ull << place % 64;
}

void ValueSetMaker::makeRoomToScatter()
{
    static_assert(ValueSets::wordsPerBlock == 64);
    if (!m_placesTouched.isEmpty())
        return;
    size_t numberOfPlaces = (static_cast<size_t>(m_sets.sizeOfUniverse()) + ValueSets::membersPerBlock - 1) / ValueSets::membersPerBlock;
    m_scatteredMembers.fill(0, numberOfPlaces * ValueSets::wordsPerBlock);
    m_wordsTouchedAtPlace.fill(0, numberOfPlaces);
    m_placesTouched.fill(0, (numberOfPlaces + 63) / 64);
}

void ValueSetMaker::sortMembersAndRemoveRepeats()
{
    AOT_THRESHOLD(size_t, mostMembersSorted, 48);
    if (m_members.size() <= mostMembersSorted) {
        AOT::sort(m_members);
        removeRepeatedElements(m_members);
        return;
    }
    makeRoomToScatter();
    for (uint32_t member : m_members)
        scatter(member);
    m_members.shrink(0);
    for (size_t wordOfPlaces = 0; wordOfPlaces < m_placesTouched.size(); ++wordOfPlaces) {
        for (uint64_t places = std::exchange(m_placesTouched[wordOfPlaces], 0); places; places &= places - 1) {
            size_t place = wordOfPlaces * 64 + std::countr_zero(places);
            for (uint64_t words = std::exchange(m_wordsTouchedAtPlace[place], 0); words; words &= words - 1) {
                size_t word = place * ValueSets::wordsPerBlock + std::countr_zero(words);
                for (uint64_t bits = std::exchange(m_scatteredMembers[word], 0); bits; bits &= bits - 1)
                    m_members.append(static_cast<uint32_t>(word * 64 + std::countr_zero(bits)));
            }
        }
    }
}

ValueSetID ValueSetMaker::ofManyMembers(std::span<const ValueFlowObjectID> members)
{
    makeRoomToScatter();
    for (ValueFlowObjectID member : members) {
        RELEASE_ASSERT(member.index() < m_sets.sizeOfUniverse());
        scatter(member.index());
    }

    m_listOfBlocks.shrink(0);
    for (size_t wordOfPlaces = 0; wordOfPlaces < m_placesTouched.size(); ++wordOfPlaces) {
        for (uint64_t places = std::exchange(m_placesTouched[wordOfPlaces], 0); places; places &= places - 1) {
            uint64_t place = wordOfPlaces * 64 + std::countr_zero(places);
            BlockBeingMade block;
            for (uint64_t words = std::exchange(m_wordsTouchedAtPlace[place], 0); words; words &= words - 1) {
                unsigned word = std::countr_zero(words);
                block.add(word, std::exchange(m_scatteredMembers[place * ValueSets::wordsPerBlock + word], 0));
            }
            m_listOfBlocks.append(place << 32 | intern(block));
        }
    }
    return ValueSetID(internListOfBlocks(m_listOfBlocks.span()) * 2);
}

ValueSetID ValueSetMaker::of(std::span<const ValueFlowObjectID> members)
{
    AOT_THRESHOLD(size_t, mostMembersSorted, 24);
    if (members.size() > mostMembersSorted)
        return ofManyMembers(members);
    if (members.empty())
        return ValueSets::nothing;
    if (members.size() == 1) {
        RELEASE_ASSERT(members[0].index() < m_sets.sizeOfUniverse());
        return of(members[0]);
    }

    m_members.shrink(0);
    for (ValueFlowObjectID member : members) {
        RELEASE_ASSERT(member.index() < m_sets.sizeOfUniverse());
        m_members.append(member.index());
    }
    AOT::sort(m_members);
    removeRepeatedElements(m_members);
    uint64_t lowestMembers = 0;
    size_t numberOfLowestMembers = 0;
    for (; numberOfLowestMembers < m_members.size() && m_members[numberOfLowestMembers] < 64; ++numberOfLowestMembers)
        lowestMembers |= 1ull << m_members[numberOfLowestMembers];
    return ValueSetID(internMembers(lowestMembers, m_members.span().subspan(numberOfLowestMembers)) * 2);
}

ValueSetID ValueSetMaker::ofRange(ValueFlowObjectID first, ValueFlowObjectID end)
{
    uint64_t begin = first.index();
    uint64_t limit = end.index();
    RELEASE_ASSERT(begin <= limit && limit <= m_sets.sizeOfUniverse());
    if (begin == limit)
        return ValueSets::nothing;
    auto remembered = m_setOfRange.add(begin << 32 | limit, ValueSetID());
    if (!remembered.isNewEntry)
        return remembered.iterator->value;

    m_listOfBlocks.shrink(0);
    for (uint64_t place = begin / ValueSets::membersPerBlock; place * ValueSets::membersPerBlock < limit; ++place) {
        bool isFull = begin <= place * ValueSets::membersPerBlock && (place + 1) * ValueSets::membersPerBlock <= limit;
        if (isFull && m_fullBlock) {
            m_listOfBlocks.append(place << 32 | m_fullBlock);
            continue;
        }
        BlockBeingMade block;
        for (unsigned word = 0; word < ValueSets::wordsPerBlock; ++word) {
            uint64_t firstOfWord = place * ValueSets::membersPerBlock + word * 64;
            uint64_t low = std::max(begin, firstOfWord);
            uint64_t high = std::min(limit, firstOfWord + 64);
            if (low < high)
                block.add(word, (high - low == 64 ? ~0ull : (1ull << (high - low)) - 1) << (low - firstOfWord));
        }
        uint32_t number = intern(block);
        if (isFull)
            m_fullBlock = number;
        m_listOfBlocks.append(place << 32 | number);
    }
    ValueSetID set(internListOfBlocks(m_listOfBlocks.span()) * 2);
    remembered.iterator->value = set;
    return set;
}

void ValueSetMaker::appendParts(ValueSetID set, Vector<ValueSetID>& parts)
{
    if (ValueSets::isComplement(set) || m_sets.size(set) <= mostMembersOfSetWithoutParts)
        return;
    const ValueSets::Run& run = m_sets.m_lists.at(ValueSets::storedSetOf(set));
    if (ValueSets::isShort(run)) {
        std::span<const uint32_t> highMembers = ValueSets::highMembersOf(run);
        uint64_t lowestMembers = run.lowestMembers;
        if (highMembers.empty() || (!lowestMembers && highMembers.front() / 64 == highMembers.back() / 64))
            return;
        uint32_t divisor = (lowestMembers ? 0 : highMembers.front() / ValueSets::membersPerBlock) == highMembers.back() / ValueSets::membersPerBlock ? 64 : ValueSets::membersPerBlock;
        if (lowestMembers && highMembers.front() / divisor)
            parts.append(ValueSetID(internShortSet(lowestMembers, { }) * 2));
        for (size_t first = 0; first < highMembers.size();) {
            size_t end = first + 1;
            while (end < highMembers.size() && highMembers[end] / divisor == highMembers[first] / divisor)
                ++end;
            parts.append(ValueSetID(internShortSet(highMembers[first] / divisor ? 0 : lowestMembers, highMembers.subspan(first, end - first)) * 2));
            first = end;
        }
        return;
    }
    std::span<const uint64_t> list = run.span();
    if (list.size() > 1 || ValueSets::isGroup(list[0])) {
        m_sets.forEachStoredBlock(list, [&](uint32_t place, uint32_t block) {
            uint64_t entry = ValueSets::entryOfBlock(place, block);
            parts.append(ValueSetID(internList(singleElementSpan(entry)) * 2));
        });
        return;
    }

    const uint64_t* words = m_sets.m_blocks.at(ValueSets::blockOf(list[0])).words();
    if (std::has_single_bit(words[0]))
        return;
    const uint64_t* word = words + 1;
    for (uint64_t present = words[0]; present; present &= present - 1, ++word) {
        std::array<uint64_t, 2> block { present & -present, *word };
        uint32_t number = m_sets.m_blocks.intern(m_roomForBlocks, block, static_cast<uint32_t>(std::popcount(*word)), mix(static_cast<uint64_t>(std::countr_zero(present)), *word));
        uint64_t entry = static_cast<uint64_t>(ValueSets::placeOf(list[0])) << 32 | number;
        parts.append(ValueSetID(internList(singleElementSpan(entry)) * 2));
    }
}

ValueSetID ValueSetMaker::uniteAll(std::span<const ValueSetID> sets)
{
    if (sets.size() < 3 || std::ranges::any_of(sets, ValueSets::isComplement)) {
        ValueSetID result = ValueSets::nothing;
        for (ValueSetID set : sets)
            result = unite(result, set);
        return result;
    }

    uint64_t lowestMembers = 0;
    m_members.shrink(0);
    uint32_t numberOfLists = 0;
    uint32_t united = 0;
    for (ValueSetID set : sets) {
        ValueSets::RunOfOneMember room;
        const ValueSets::Run& run = m_sets.runOf(ValueSets::storedSetOf(set), room);
        if (!ValueSets::isShort(run)) {
            ++numberOfLists;
            united = ValueSets::storedSetOf(set);
            continue;
        }
        lowestMembers |= run.lowestMembers;
        m_members.append(ValueSets::highMembersOf(run));
    }
    sortMembersAndRemoveRepeats();
    if (!numberOfLists)
        return ValueSetID(internMembers(lowestMembers, m_members.span()) * 2);
    if (numberOfLists > 1)
        united = uniteAllLists(sets);
    const ValueSets::Run& run = m_sets.m_lists.at(united);
    ValueSets::MembersOfList membersOfList(m_sets, run.span());
    m_members.removeAllMatching([&](uint32_t member) {
        return membersOfList.contains(member);
    });
    lowestMembers &= ~run.lowestMembers;
    if (!lowestMembers && m_members.isEmpty())
        return ValueSetID(united * 2);
    return ValueSetID(changeList(united, true, lowestMembers, m_members.span()) * 2);
}

uint32_t ValueSetMaker::uniteAllLists(std::span<const ValueSetID> sets)
{
    constexpr uint32_t isGathered = 1u << 31;
    uint32_t numberOfGatheredBlocks = 0;
    auto addTo = [&](GatheredBlock& gathered, uint32_t block) {
        const uint64_t* words = m_sets.m_blocks.at(block).words();
        const uint64_t* next = words + 1;
        for (uint64_t present = words[0]; present; present &= present - 1) {
            unsigned index = std::countr_zero(present);
            gathered.words[index] = gathered.present >> index & 1 ? gathered.words[index] | *next : *next;
            ++next;
        }
        gathered.present |= words[0];
    };
    auto spread = [&](uint64_t entry) {
        m_sets.forEachStoredBlock(singleElementSpan(entry), [&](uint32_t place, uint32_t block) {
            uint32_t& held = m_blockAtPlace[place];
            if (!held)
                held = block;
            else if (held & isGathered)
                addTo(m_gatheredBlocks[held & ~isGathered], block);
            else if (held != block) {
                if (numberOfGatheredBlocks == m_gatheredBlocks.size())
                    m_gatheredBlocks.append(GatheredBlock { });
                GatheredBlock& gathered = m_gatheredBlocks[numberOfGatheredBlocks];
                gathered.present = 0;
                addTo(gathered, held);
                addTo(gathered, block);
                held = numberOfGatheredBlocks++ | isGathered;
            }
        });
    };
    constexpr uint64_t isSpread = 1;
    static_assert(!ValueSets::isGroup(isSpread));
    uint32_t lowest = UINT32_MAX;
    uint32_t highest = 0;
    for (ValueSetID set : sets) {
        if (ValueSets::holdsOneMember(ValueSets::storedSetOf(set)))
            continue;
        const ValueSets::Run& run = m_sets.m_lists.at(ValueSets::storedSetOf(set));
        if (ValueSets::isShort(run))
            continue;
        for (uint64_t entry : run.span()) {
            uint32_t groupOfPlaces = ValueSets::groupOfPlacesOf(entry);
            uint64_t& held = m_entryAtGroupOfPlaces[groupOfPlaces];
            if (!held) {
                held = entry;
                lowest = std::min(lowest, groupOfPlaces);
                highest = std::max(highest, groupOfPlaces);
            } else if (held != entry) {
                if (held != isSpread)
                    spread(std::exchange(held, isSpread));
                spread(entry);
            }
        }
    }
    m_list.shrink(0);
    std::array<uint32_t, ValueSets::placesPerGroup> blocks;
    for (uint32_t groupOfPlaces = lowest; groupOfPlaces <= highest && lowest != UINT32_MAX; ++groupOfPlaces) {
        uint64_t entry = std::exchange(m_entryAtGroupOfPlaces[groupOfPlaces], 0u);
        if (!entry)
            continue;
        if (entry != isSpread) {
            m_list.append(entry);
            continue;
        }
        unsigned numberOfBlocks = 0;
        uint64_t placesPresent = 0;
        for (uint32_t index = 0; index < ValueSets::placesPerGroup; ++index) {
            uint32_t block = std::exchange(m_blockAtPlace[groupOfPlaces * ValueSets::placesPerGroup + index], 0u);
            if (!block)
                continue;
            if (block & isGathered) {
                const GatheredBlock& gathered = m_gatheredBlocks[block & ~isGathered];
                BlockBeingMade result;
                for (uint64_t present = gathered.present; present; present &= present - 1)
                    result.add(std::countr_zero(present), gathered.words[std::countr_zero(present)]);
                block = intern(result);
            }
            placesPresent |= 1ull << index;
            blocks[numberOfBlocks++] = block;
        }
        m_list.append(entryFor(groupOfPlaces * ValueSets::placesPerGroup, placesPresent, std::span<const uint32_t>(blocks).first(numberOfBlocks)));
    }
    return internList(m_list.span());
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
