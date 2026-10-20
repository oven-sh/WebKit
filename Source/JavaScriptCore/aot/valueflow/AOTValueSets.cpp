/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueSets.h"

#if ENABLE(AOT)

#include <algorithm>
#include <wtf/Locker.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t mix(uint64_t first, uint64_t second)
{
    uint64_t hash = (first + 0x9e3779b97f4a7c15ull) * 0xbf58476d1ce4e5b9ull ^ second * 0x94d049bb133111ebull;
    hash ^= hash >> 32;
    hash *= 0xd6e8feb86659fd93ull;
    return hash ^ hash >> 29;
}

ValueSets::InternedRuns::InternedRuns()
{
    for (Shard& shard : m_shards)
        shard.slots.grow(64);
    // The run without words.
    Shard& first = m_shards[0];
    runAt(first, 0) = Run { };
    first.size = 1;
}

ValueSets::InternedRuns::~InternedRuns()
{
    for (Shard& shard : m_shards) {
        for (auto& segment : shard.segments)
            fastFree(segment.load(std::memory_order_relaxed));
        for (uint64_t* chunk : shard.arenaChunks)
            fastFree(chunk);
    }
}

auto ValueSets::InternedRuns::runAt(Shard& shard, uint32_t index) -> Run&
{
    uint32_t shifted = index + firstSegmentSize;
    unsigned segment = std::bit_width(shifted) - std::bit_width(firstSegmentSize);
    RELEASE_ASSERT(segment < numberOfSegments);
    Run* runs = shard.segments[segment].load(std::memory_order_relaxed);
    if (!runs) {
        runs = static_cast<Run*>(fastZeroedMalloc(sizeof(Run) * (static_cast<size_t>(firstSegmentSize) << segment)));
        shard.segments[segment].store(runs, std::memory_order_release);
    }
    return runs[shifted - (firstSegmentSize << segment)];
}

std::span<uint64_t> ValueSets::InternedRuns::allocate(Shard& shard, size_t numberOfWords)
{
    if (numberOfWords > shard.freeWords.size()) {
        size_t size = std::max(numberOfWords, wordsPerArenaChunk);
        uint64_t* chunk = static_cast<uint64_t*>(fastMalloc(size * sizeof(uint64_t)));
        shard.arenaChunks.append(chunk);
        shard.arenaBytes += size * sizeof(uint64_t);
        // What is left of a chunk is given up only for a run that would not have fitted into a fresh one either.
        if (size > wordsPerArenaChunk)
            return unsafeMakeSpan(chunk, numberOfWords);
        shard.freeWords = unsafeMakeSpan(chunk, size);
    }
    std::span<uint64_t> result = shard.freeWords.first(numberOfWords);
    shard.freeWords = shard.freeWords.subspan(numberOfWords);
    return result;
}

void ValueSets::InternedRuns::grow(Shard& shard)
{
    Vector<Slot> previous = std::exchange(shard.slots, Vector<Slot>());
    shard.slots.grow(previous.size() * 2);
    size_t mask = shard.slots.size() - 1;
    for (Slot slot : previous) {
        if (!slot.indexPlusOne)
            continue;
        size_t position = slot.hash & mask;
        while (shard.slots[position].indexPlusOne)
            position = (position + 1) & mask;
        shard.slots[position] = slot;
    }
}

uint32_t ValueSets::InternedRuns::intern(std::span<const uint64_t> words, uint32_t numberOfMembers, uint64_t hashOfMembers)
{
    if (words.empty())
        return 0;

    unsigned shardNumber = static_cast<unsigned>(hashOfMembers % numberOfShards);
    uint32_t hash = static_cast<uint32_t>(hashOfMembers >> 32);
    Shard& shard = m_shards[shardNumber];
    Locker locker { shard.lock };

    size_t mask = shard.slots.size() - 1;
    size_t position = hash & mask;
    for (; shard.slots[position].indexPlusOne; position = (position + 1) & mask) {
        const Slot& slot = shard.slots[position];
        if (slot.hash != hash)
            continue;
        const Run& candidate = runAt(shard, slot.indexPlusOne - 1);
        if (candidate.hashOfMembers == hashOfMembers && candidate.numberOfWords == words.size() && std::ranges::equal(unsafeMakeSpan(candidate.words, candidate.numberOfWords), words))
            return (slot.indexPlusOne - 1) * numberOfShards + shardNumber;
    }

    uint32_t index = shard.size++;
    std::span<uint64_t> stored = allocate(shard, words.size());
    std::ranges::copy(words, stored.begin());
    runAt(shard, index) = Run { stored.data(), static_cast<uint32_t>(words.size()), numberOfMembers, hashOfMembers };
    shard.slots[position] = Slot { hash, index + 1 };
    if (static_cast<size_t>(shard.size) * 2 > shard.slots.size())
        grow(shard);
    return index * numberOfShards + shardNumber;
}

size_t ValueSets::InternedRuns::size() const
{
    size_t size = 0;
    for (const Shard& shard : m_shards)
        size += shard.size;
    return size;
}

size_t ValueSets::InternedRuns::memoryInBytes() const
{
    size_t bytes = 0;
    for (const Shard& shard : m_shards) {
        bytes += shard.arenaBytes + shard.slots.capacity() * sizeof(Slot);
        for (unsigned segment = 0; segment < numberOfSegments; ++segment) {
            if (shard.segments[segment].load(std::memory_order_relaxed))
                bytes += sizeof(Run) * (static_cast<size_t>(firstSegmentSize) << segment);
        }
    }
    return bytes;
}

ValueSets::ValueSets(uint32_t sizeOfUniverse)
    : m_sizeOfUniverse(sizeOfUniverse)
{
}

ValueSets::~ValueSets() = default;

uint32_t ValueSets::size(ValueSetID set) const
{
    uint32_t stored = m_lists.at(storedSetOf(set)).numberOfMembers;
    return isComplement(set) ? m_sizeOfUniverse - stored : stored;
}

uint64_t ValueSets::hashOfMembers(ValueSetID set) const
{
    uint64_t stored = m_lists.at(storedSetOf(set)).hashOfMembers;
    return isComplement(set) ? ~stored : stored;
}

uint32_t ValueSets::blockAt(ValueSetID set, uint32_t place) const
{
    std::span<const uint64_t> list = listOf(set);
    auto entry = std::ranges::lower_bound(list, place, { }, placeOf);
    return entry != list.end() && placeOf(*entry) == place ? blockOf(*entry) : 0;
}

bool ValueSets::contains(ValueSetID set, ValueFlowObjectID member) const
{
    bool isStored = false;
    if (uint32_t number = blockAt(set, member.index() / membersPerBlock)) {
        const Run& block = m_blocks.at(number);
        unsigned word = member.index() % membersPerBlock / 64;
        uint64_t present = block.words[0];
        if (present >> word & 1)
            isStored = block.words[1 + std::popcount(present & ((1ull << word) - 1))] >> (member.index() % 64) & 1;
    }
    return isStored != isComplement(set);
}

ValueFlowObjectID ValueSets::onlyMember(ValueSetID set) const
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

bool ValueSets::storedSetsMeet(ValueSetID first, ValueSetID second) const
{
    std::span<const uint64_t> firstList = listOf(first);
    std::span<const uint64_t> secondList = listOf(second);
    size_t i = 0;
    size_t j = 0;
    while (i < firstList.size() && j < secondList.size()) {
        uint32_t firstPlace = placeOf(firstList[i]);
        uint32_t secondPlace = placeOf(secondList[j]);
        if (firstPlace < secondPlace) {
            ++i;
            continue;
        }
        if (secondPlace < firstPlace) {
            ++j;
            continue;
        }
        if (firstList[i] == secondList[j])
            return true;
        bool meet = false;
        forEachCommonWord(m_blocks.at(blockOf(firstList[i])).words, m_blocks.at(blockOf(secondList[j])).words, [&](uint64_t a, uint64_t b) {
            meet = a & b;
            return meet;
        });
        if (meet)
            return true;
        ++i;
        ++j;
    }
    return false;
}

bool ValueSets::storedSetIsSubset(ValueSetID set, ValueSetID of) const
{
    std::span<const uint64_t> list = listOf(set);
    std::span<const uint64_t> otherList = listOf(of);
    size_t j = 0;
    for (uint64_t entry : list) {
        while (j < otherList.size() && placeOf(otherList[j]) < placeOf(entry))
            ++j;
        if (j == otherList.size() || placeOf(otherList[j]) != placeOf(entry))
            return false;
        if (otherList[j] == entry)
            continue;
        const uint64_t* block = m_blocks.at(blockOf(entry)).words;
        const uint64_t* otherBlock = m_blocks.at(blockOf(otherList[j])).words;
        if (block[0] & ~otherBlock[0])
            return false;
        bool sticksOut = false;
        forEachCommonWord(block, otherBlock, [&](uint64_t a, uint64_t b) {
            sticksOut = a & ~b;
            return sticksOut;
        });
        if (sticksOut)
            return false;
    }
    return true;
}

bool ValueSets::meet(ValueSetID first, ValueSetID second) const
{
    if (!isComplement(first) && !isComplement(second))
        return storedSetsMeet(first, second);
    if (isComplement(first) && isComplement(second)) {
        // Something lies outside both stored sets unless the two cover the universe.
        uint32_t most = m_lists.at(storedSetOf(first)).numberOfMembers + m_lists.at(storedSetOf(second)).numberOfMembers;
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

bool ValueSets::isSubset(ValueSetID set, ValueSetID of) const
{
    return !meet(set, complement(of));
}

ValueSetMaker::ValueSetMaker(ValueSets& sets)
    : m_sets(sets)
    , m_rememberedSets(static_cast<size_t>(1) << bitsOfRememberedSets)
    , m_rememberedBlocks(static_cast<size_t>(1) << bitsOfRememberedBlocks)
{
    m_blockAtPlace.fill(0u, sets.sizeOfUniverse() / ValueSets::membersPerBlock + 1);
    m_setOfMember.fill(ValueSets::nothing, sets.sizeOfUniverse());
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
    if (remembered.first == first && remembered.second == key) {
        ++m_counters.blocksRemembered;
        return remembered.result;
    }
    ++m_counters.blocksComputed;

    const ValueSets::Run& firstRun = m_sets.m_blocks.at(first);
    const ValueSets::Run& secondRun = m_sets.m_blocks.at(second);
    const uint64_t* a = firstRun.words + 1;
    const uint64_t* b = secondRun.words + 1;
    uint64_t presentInFirst = firstRun.words[0];
    uint64_t presentInSecond = secondRun.words[0];

    std::array<uint64_t, ValueSets::wordsPerBlock + 1> result;
    unsigned size = 1;
    uint64_t present = 0;
    uint32_t numberOfMembers = 0;
    uint64_t hash = 0;
    for (uint64_t either = presentInFirst | presentInSecond; either; either &= either - 1) {
        uint64_t bit = either & -either;
        uint64_t x = presentInFirst & bit ? *a++ : 0;
        uint64_t y = presentInSecond & bit ? *b++ : 0;
        uint64_t word = operation == Operation::Unite ? x | y : operation == Operation::Intersect ? x & y : x & ~y;
        if (!word)
            continue;
        present |= bit;
        result[size++] = word;
        numberOfMembers += static_cast<uint32_t>(std::popcount(word));
        hash += mix(static_cast<uint64_t>(std::countr_zero(bit)), word);
    }
    result[0] = present;

    uint32_t number;
    if (numberOfMembers == firstRun.numberOfMembers)
        number = first;
    else if (operation != Operation::Subtract && numberOfMembers == secondRun.numberOfMembers)
        number = second;
    else
        number = numberOfMembers ? m_sets.m_blocks.intern(std::span<const uint64_t>(result).first(size), numberOfMembers, hash) : 0;
    remembered = Remembered { first, key, number };
    return number;
}

uint32_t ValueSetMaker::internList(std::span<const uint64_t> list)
{
    uint32_t numberOfMembers = 0;
    uint64_t hash = 0;
    for (uint64_t entry : list) {
        const ValueSets::Run& block = m_sets.m_blocks.at(ValueSets::blockOf(entry));
        numberOfMembers += block.numberOfMembers;
        hash += mix(ValueSets::placeOf(entry), block.hashOfMembers);
    }
    return m_sets.m_lists.intern(list, numberOfMembers, hash);
}

uint32_t ValueSetMaker::combine(Operation operation, uint32_t first, uint32_t second)
{
    switch (operation) {
    case Operation::Unite:
        if (first == second || !second) {
            ++m_counters.answeredAtOnce;
            return first;
        }
        if (!first) {
            ++m_counters.answeredAtOnce;
            return second;
        }
        break;
    case Operation::Intersect:
        if (first == second || !first || !second) {
            ++m_counters.answeredAtOnce;
            return first == second ? first : 0;
        }
        break;
    case Operation::Subtract:
        if (!first || first == second || !second) {
            ++m_counters.answeredAtOnce;
            return second ? 0 : first;
        }
        break;
    }
    if (operation != Operation::Subtract && first > second)
        std::swap(first, second);

    RELEASE_ASSERT(second < 1u << 30);
    uint32_t key = second | static_cast<uint32_t>(operation) << 30;
    uint32_t place = placeInTable(first, key, bitsOfRememberedSets);
    if (m_rememberedSets[place].first == first && m_rememberedSets[place].second == key) {
        ++m_counters.remembered;
        return m_rememberedSets[place].result;
    }
    ++m_counters.computed;

    const ValueSets::Run& firstRun = m_sets.m_lists.at(first);
    const ValueSets::Run& secondRun = m_sets.m_lists.at(second);
    std::span<const uint64_t> a = unsafeMakeSpan(firstRun.words, firstRun.numberOfWords);
    std::span<const uint64_t> b = unsafeMakeSpan(secondRun.words, secondRun.numberOfWords);
    m_list.shrink(0);
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() || j < b.size()) {
        uint32_t firstPlace = i < a.size() ? ValueSets::placeOf(a[i]) : UINT32_MAX;
        uint32_t secondPlace = j < b.size() ? ValueSets::placeOf(b[j]) : UINT32_MAX;
        if (firstPlace < secondPlace) {
            if (operation != Operation::Intersect)
                m_list.append(a[i]);
            ++i;
            continue;
        }
        if (secondPlace < firstPlace) {
            if (operation == Operation::Unite)
                m_list.append(b[j]);
            ++j;
            continue;
        }
        uint32_t block;
        if (a[i] == b[j])
            block = operation == Operation::Subtract ? 0 : ValueSets::blockOf(a[i]);
        else
            block = combineBlocks(operation, ValueSets::blockOf(a[i]), ValueSets::blockOf(b[j]));
        if (block)
            m_list.append(static_cast<uint64_t>(firstPlace) << 32 | block);
        ++i;
        ++j;
    }

    uint32_t result;
    if (std::ranges::equal(m_list, a))
        result = first;
    else if (std::ranges::equal(m_list, b))
        result = second;
    else
        result = internList(m_list.span());
    m_rememberedSets[place] = Remembered { first, key, result };
    return result;
}

ValueSetID ValueSetMaker::unite(ValueSetID first, ValueSetID second)
{
    uint32_t a = ValueSets::storedSetOf(first);
    uint32_t b = ValueSets::storedSetOf(second);
    if (!ValueSets::isComplement(first))
        return ValueSets::isComplement(second) ? ValueSetID(combine(Operation::Subtract, b, a) * 2 + 1) : ValueSetID(combine(Operation::Unite, a, b) * 2);
    return ValueSetID(combine(ValueSets::isComplement(second) ? Operation::Intersect : Operation::Subtract, a, b) * 2 + 1);
}

ValueSetID ValueSetMaker::intersect(ValueSetID first, ValueSetID second)
{
    uint32_t a = ValueSets::storedSetOf(first);
    uint32_t b = ValueSets::storedSetOf(second);
    if (!ValueSets::isComplement(first))
        return ValueSetID(combine(ValueSets::isComplement(second) ? Operation::Subtract : Operation::Intersect, a, b) * 2);
    return ValueSets::isComplement(second) ? ValueSetID(combine(Operation::Unite, a, b) * 2 + 1) : ValueSetID(combine(Operation::Subtract, b, a) * 2);
}

ValueSetID ValueSetMaker::pass(ValueSetID value, ValueSetID mask, const ValueFlowStandIn& standIn)
{
    if (mask == ValueSets::anything)
        return value;
    ValueSetID passed = intersect(value, mask);
    if (!standIn.member || !m_sets.contains(value, standIn.member) || m_sets.contains(mask, standIn.member))
        return passed;
    return unite(passed, intersect(mask, standIn.covered));
}

ValueSetID ValueSetMaker::of(ValueFlowObjectID member)
{
    ValueSetID& set = m_setOfMember[member.index()];
    if (set != ValueSets::nothing)
        return set;
    unsigned word = member.index() % ValueSets::membersPerBlock / 64;
    std::array<uint64_t, 2> block { 1ull << word, 1ull << member.index() % 64 };
    uint64_t entry = static_cast<uint64_t>(member.index() / ValueSets::membersPerBlock) << 32 | m_sets.m_blocks.intern(block, 1, mix(word, block[1]));
    set = ValueSetID(internList(singleElementSpan(entry)) * 2);
    return set;
}

ValueSetID ValueSetMaker::of(std::span<const ValueFlowObjectID> members)
{
    m_members.shrink(0);
    for (ValueFlowObjectID member : members) {
        RELEASE_ASSERT(member.index() < m_sets.sizeOfUniverse());
        m_members.append(member.index());
    }
    std::ranges::sort(m_members);

    m_list.shrink(0);
    for (size_t i = 0; i < m_members.size();) {
        uint32_t place = m_members[i] / ValueSets::membersPerBlock;
        std::array<uint64_t, ValueSets::wordsPerBlock + 1> block;
        unsigned size = 1;
        uint64_t present = 0;
        uint32_t numberOfMembers = 0;
        uint64_t hash = 0;
        while (i < m_members.size() && m_members[i] / ValueSets::membersPerBlock == place) {
            unsigned word = m_members[i] % ValueSets::membersPerBlock / 64;
            uint64_t bits = 0;
            for (; i < m_members.size() && m_members[i] / 64 == place * ValueSets::wordsPerBlock + word; ++i)
                bits |= 1ull << m_members[i] % 64;
            present |= 1ull << word;
            block[size++] = bits;
            numberOfMembers += static_cast<uint32_t>(std::popcount(bits));
            hash += mix(word, bits);
        }
        block[0] = present;
        m_list.append(static_cast<uint64_t>(place) << 32 | m_sets.m_blocks.intern(std::span<const uint64_t>(block).first(size), numberOfMembers, hash));
    }
    return ValueSetID(internList(m_list.span()) * 2);
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

    m_list.shrink(0);
    for (uint64_t place = begin / ValueSets::membersPerBlock; place * ValueSets::membersPerBlock < limit; ++place) {
        bool isFull = begin <= place * ValueSets::membersPerBlock && (place + 1) * ValueSets::membersPerBlock <= limit;
        if (isFull && m_fullBlock) {
            m_list.append(place << 32 | m_fullBlock);
            continue;
        }
        std::array<uint64_t, ValueSets::wordsPerBlock + 1> block;
        unsigned size = 1;
        uint64_t present = 0;
        uint32_t numberOfMembers = 0;
        uint64_t hash = 0;
        for (unsigned word = 0; word < ValueSets::wordsPerBlock; ++word) {
            uint64_t firstOfWord = place * ValueSets::membersPerBlock + word * 64;
            uint64_t low = std::max(begin, firstOfWord);
            uint64_t high = std::min(limit, firstOfWord + 64);
            if (low >= high)
                continue;
            uint64_t bits = (high - low == 64 ? ~0ull : (1ull << (high - low)) - 1) << (low - firstOfWord);
            present |= 1ull << word;
            block[size++] = bits;
            numberOfMembers += static_cast<uint32_t>(high - low);
            hash += mix(word, bits);
        }
        block[0] = present;
        uint32_t number = m_sets.m_blocks.intern(std::span<const uint64_t>(block).first(size), numberOfMembers, hash);
        if (isFull)
            m_fullBlock = number;
        m_list.append(place << 32 | number);
    }
    ValueSetID set(internList(m_list.span()) * 2);
    remembered.iterator->value = set;
    return set;
}

void ValueSetMaker::appendParts(ValueSetID set, Vector<ValueSetID>& parts)
{
    if (ValueSets::isComplement(set) || m_sets.size(set) <= mostMembersOfSetWithoutParts)
        return;
    std::span<const uint64_t> list = m_sets.listOf(set);
    if (list.size() > 1) {
        for (uint64_t entry : list)
            parts.append(ValueSetID(internList(singleElementSpan(entry)) * 2));
        return;
    }

    const uint64_t* words = m_sets.m_blocks.at(ValueSets::blockOf(list[0])).words;
    if (std::has_single_bit(words[0]))
        return;
    const uint64_t* word = words + 1;
    for (uint64_t present = words[0]; present; present &= present - 1, ++word) {
        std::array<uint64_t, 2> block { present & -present, *word };
        uint32_t number = m_sets.m_blocks.intern(block, static_cast<uint32_t>(std::popcount(*word)), mix(static_cast<uint64_t>(std::countr_zero(present)), *word));
        uint64_t entry = static_cast<uint64_t>(ValueSets::placeOf(list[0])) << 32 | number;
        parts.append(ValueSetID(internList(singleElementSpan(entry)) * 2));
    }
}

ValueSetID ValueSetMaker::uniteAll(std::span<const ValueSetID> sets)
{
    // A complement among them is rare: the pairs do it.
    if (sets.size() < 3 || std::ranges::any_of(sets, ValueSets::isComplement)) {
        ValueSetID result = ValueSets::nothing;
        for (ValueSetID set : sets)
            result = unite(result, set);
        return result;
    }

    uint32_t lowest = UINT32_MAX;
    uint32_t highest = 0;
    for (ValueSetID set : sets) {
        for (uint64_t entry : m_sets.listOf(set)) {
            uint32_t place = ValueSets::placeOf(entry);
            uint32_t& held = m_blockAtPlace[place];
            if (!held) {
                held = ValueSets::blockOf(entry);
                lowest = std::min(lowest, place);
                highest = std::max(highest, place);
            } else if (held != ValueSets::blockOf(entry))
                held = combineBlocks(Operation::Unite, held, ValueSets::blockOf(entry));
        }
    }
    m_list.shrink(0);
    for (uint32_t place = lowest; place <= highest && lowest != UINT32_MAX; ++place) {
        if (uint32_t block = std::exchange(m_blockAtPlace[place], 0u))
            m_list.append(static_cast<uint64_t>(place) << 32 | block);
    }
    return ValueSetID(internList(m_list.span()) * 2);
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
