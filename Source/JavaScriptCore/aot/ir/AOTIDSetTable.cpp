/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIDSets.h"

#if ENABLE(AOT)

#include <atomic>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t fewestWordsOfIDSetThatLieOutside = 32;

static constexpr bool wordsOfIDSetLieOutside(IDSetHeader header)
{
    uint32_t numberOfWords = header.numberOfChunks ? header.numberOfChunks * static_cast<uint32_t>(sizeof(IDSetChunkEntry) / sizeof(uint32_t)) : header.size;
    return numberOfWords >= fewestWordsOfIDSetThatLieOutside;
}
static uint32_t*& wordsOutsideIDSetRecord(IDSetHeader* header) { return *reinterpret_cast<uint32_t**>(header + 1); }
static_assert(sizeof(uint32_t*) == sizeof(IDSetHeader));

static constexpr bool idSetRecordHasLazyList(IDSetForm form, IDSetHeader header) { return form == IDSetForm::ChunksWithLazyList && header.numberOfChunks; }
static std::atomic<uint32_t*>& lazyListOfIDSetRecord(IDSetHeader* header) { return *reinterpret_cast<std::atomic<uint32_t*>*>(header + 1 + wordsOfIDSetLieOutside(*header)); }
static_assert(sizeof(std::atomic<uint32_t*>) == sizeof(IDSetHeader) && alignof(std::atomic<uint32_t*>) <= 2 * alignof(IDSetHeader));

static constexpr uint32_t bitsOfShardInIDSetTable = 6;
static constexpr uint32_t bitsOfIndexInIDSetShard = idHomeShift - bitsOfShardInIDSetTable;
static constexpr size_t alignmentInIDSetTable = 64;

static_assert(IDSetTable::numberOfShards == 1u << bitsOfShardInIDSetTable);
static_assert(sizeof(IDSetHeader) == 2 * sizeof(uint32_t) && sizeof(IDSetChunkEntry) == 2 * sizeof(uint32_t));

static constexpr uint32_t idInIDSetTable(uint32_t shard, uint32_t index)
{
    return static_cast<uint32_t>(IDHome::Program) << idHomeShift | shard << bitsOfIndexInIDSetShard | index;
}

static constexpr uint32_t shardOfIDInIDSetTable(uint32_t id) { return (id & idPositionMask) >> bitsOfIndexInIDSetShard; }
static constexpr uint32_t indexOfIDInIDSetTable(uint32_t id) { return id & ((1u << bitsOfIndexInIDSetShard) - 1); }

static constexpr uint32_t spreadHashForIDSetTable(uint32_t hash)
{
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ hash >> 16;
}

static constexpr uint32_t shardOfHashInIDSetTable(uint32_t spreadHash) { return spreadHash >> (32 - bitsOfShardInIDSetTable); }

template<typename Element, uint32_t log2OfFirstSegment>
class IDSetTableSegments {
    WTF_MAKE_NONCOPYABLE(IDSetTableSegments);
public:
    static constexpr uint32_t sizeOfFirstSegment = 1u << log2OfFirstSegment;
    static constexpr uint32_t numberOfSegments = bitsOfIndexInIDSetShard - log2OfFirstSegment;
    static constexpr uint32_t capacity = (1u << bitsOfIndexInIDSetShard) - sizeOfFirstSegment;

    static_assert((idInIDSetTable(IDSetTable::numberOfShards - 1, capacity - 1) & idPositionMask) <= largestIDPosition);
    static_assert(!(sizeOfFirstSegment * sizeof(Element) % alignmentInIDSetTable));

    IDSetTableSegments() = default;

    ~IDSetTableSegments()
    {
        for (auto& segment : m_segments) {
            if (Element* elements = segment.load(std::memory_order_relaxed))
                FastAlignedMalloc::free(elements);
        }
    }

    Element* at(uint32_t index) const
    {
        uint32_t segment = segmentOf(index);
        return m_segments[segment].load(std::memory_order_relaxed) + (index - firstIndexOf(segment));
    }

    uint32_t allocate(size_t count)
    {
        uint32_t segment = segmentOf(m_next);
        while (segment < numberOfSegments && count > firstIndexOf(segment + 1) - m_next)
            m_next = firstIndexOf(++segment);
        RELEASE_ASSERT(segment < numberOfSegments);
        if (!m_segments[segment].load(std::memory_order_relaxed)) {
            size_t bytes = (static_cast<size_t>(sizeOfFirstSegment) << segment) * sizeof(Element);
            m_segments[segment].store(static_cast<Element*>(FastAlignedMalloc::alignedMalloc(alignmentInIDSetTable, bytes)), std::memory_order_release);
        }
        uint32_t index = m_next;
        m_next += static_cast<uint32_t>(count);
        m_numberInUse += static_cast<uint32_t>(count);
        return index;
    }

    size_t bytesInUse() const { return static_cast<size_t>(m_numberInUse) * sizeof(Element); }

private:
    static constexpr uint32_t segmentOf(uint32_t index) { return static_cast<uint32_t>(std::bit_width(index + sizeOfFirstSegment)) - 1 - log2OfFirstSegment; }
    static constexpr uint32_t firstIndexOf(uint32_t segment) { return (sizeOfFirstSegment << segment) - sizeOfFirstSegment; }

    std::array<std::atomic<Element*>, numberOfSegments> m_segments { };
    uint32_t m_next { 0 };
    uint32_t m_numberInUse { 0 };
};

class IDSetTableIndex {
    WTF_MAKE_NONCOPYABLE(IDSetTableIndex);
public:
    IDSetTableIndex() = default;

    ~IDSetTableIndex()
    {
        for (Slots* slots = m_slots.load(std::memory_order_relaxed); slots;)
            fastFree(std::exchange(slots, slots->outgrown));
    }

    uint32_t find(uint32_t hash, NOESCAPE const Invocable<bool(uint32_t)> auto& isWanted) const
    {
        Slots* slots = m_slots.load(std::memory_order_acquire);
        if (!slots)
            return 0;
        for (size_t place = hash & slots->mask;; place = (place + 1) & slots->mask) {
            uint64_t slot = slots->at(place).load(std::memory_order_acquire);
            if (!slot)
                return 0;
            if (slot >> 32 == hash && isWanted(static_cast<uint32_t>(slot)))
                return static_cast<uint32_t>(slot);
        }
    }

    void add(uint32_t hash, uint32_t id)
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        if (!slots || (static_cast<size_t>(m_size) + 1) * 3 > (slots->mask + 1) * 2)
            slots = grow(slots);
        put(*slots, static_cast<uint64_t>(hash) << 32 | id);
        ++m_size;
    }

    void freeOutgrownSlots()
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        for (Slots* outgrown = slots ? std::exchange(slots->outgrown, nullptr) : nullptr; outgrown;)
            fastFree(std::exchange(outgrown, outgrown->outgrown));
    }

    size_t sizeInBytes() const
    {
        size_t size = 0;
        for (Slots* slots = m_slots.load(std::memory_order_relaxed); slots; slots = slots->outgrown)
            size += sizeof(Slots) + (slots->mask + 1) * sizeof(std::atomic<uint64_t>);
        return size;
    }

    uint32_t size() const { return m_size; }
    void forEachID(NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        for (size_t place = 0; slots && place <= slots->mask; ++place) {
            if (uint64_t slot = slots->at(place).load(std::memory_order_relaxed))
                functor(static_cast<uint32_t>(slot));
        }
    }

private:
    struct Slots {
        std::atomic<uint64_t>& at(size_t place) { return reinterpret_cast<std::atomic<uint64_t>*>(this + 1)[place]; }

        size_t mask;
        Slots* outgrown;
    };
    static_assert(!(sizeof(Slots) % alignof(std::atomic<uint64_t>)) && sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));

    static void put(Slots& slots, uint64_t slot)
    {
        size_t place = (slot >> 32) & slots.mask;
        while (slots.at(place).load(std::memory_order_relaxed))
            place = (place + 1) & slots.mask;
        slots.at(place).store(slot, std::memory_order_release);
    }

    Slots* grow(Slots* outgrown)
    {
        constexpr size_t fewestSlots = 16;
        size_t numberOfSlots = outgrown ? (outgrown->mask + 1) * 2 : fewestSlots;
        Slots* larger = static_cast<Slots*>(fastZeroedMalloc(sizeof(Slots) + numberOfSlots * sizeof(std::atomic<uint64_t>)));
        larger->mask = numberOfSlots - 1;
        larger->outgrown = outgrown;
        for (size_t place = 0; outgrown && place <= outgrown->mask; ++place) {
            if (uint64_t slot = outgrown->at(place).load(std::memory_order_relaxed))
                put(*larger, slot);
        }
        m_slots.store(larger, std::memory_order_release);
        return larger;
    }

    std::atomic<Slots*> m_slots { nullptr };
    uint32_t m_size { 0 };
};

class IDSetTableWordBlocks {
    WTF_MAKE_NONCOPYABLE(IDSetTableWordBlocks);
public:
    IDSetTableWordBlocks() = default;

    ~IDSetTableWordBlocks()
    {
        for (Block* block = m_newest; block;)
            fastFree(std::exchange(block, block->older));
    }

    uint32_t* allocate(size_t numberOfWords)
    {
        if (numberOfWords > m_free.size())
            return allocateInNewBlock(numberOfWords);
        uint32_t* words = m_free.data();
        m_free = m_free.subspan(numberOfWords);
        return words;
    }

    size_t sizeInBytes() const { return m_sizeInBytes; }

private:
    struct Block {
        Block* older;
    };
    static_assert(!(sizeof(Block) % alignof(IDSetChunkEntry)));

    uint32_t* allocateInNewBlock(size_t numberOfWords)
    {
        constexpr size_t maxWordsPerBlock = 1 << 16;
        bool sharesBlock = numberOfWords <= m_wordsOfNextBlock / 4;
        size_t wordsOfBlock = sharesBlock ? m_wordsOfNextBlock : numberOfWords;
        m_wordsOfNextBlock = std::min(m_wordsOfNextBlock * 2, maxWordsPerBlock);
        size_t bytes = sizeof(Block) + wordsOfBlock * sizeof(uint32_t);
        Block* block = static_cast<Block*>(fastMalloc(bytes));
        m_sizeInBytes += bytes;
        block->older = m_newest;
        m_newest = block;
        uint32_t* words = reinterpret_cast<uint32_t*>(block + 1);
        if (sharesBlock)
            m_free = unsafeMakeSpan(words + numberOfWords, wordsOfBlock - numberOfWords);
        return words;
    }

    Block* m_newest { nullptr };
    std::span<uint32_t> m_free;
    size_t m_wordsOfNextBlock { 64 };
    size_t m_sizeInBytes { 0 };
};

struct alignas(alignmentInIDSetTable) IDSetTable::Shard {
    uint32_t addRecord(IDSetForm form, uint32_t shardIndex, uint32_t hash, IDSetHeader header, std::span<const uint32_t> attachedWords, std::span<const uint32_t> membersOrEntries)
    {
        constexpr size_t wordsOfHeader = sizeof(IDSetHeader) / sizeof(uint32_t);
        bool wordsLieOutside = wordsOfIDSetLieOutside(header);
        bool hasLazyList = idSetRecordHasLazyList(form, header);
        size_t numberOfPointers = wordsLieOutside + hasLazyList;
        size_t wordsBehindPointers = attachedWords.size() + (wordsLieOutside ? 0 : membersOrEntries.size());
        uint32_t index = records.allocate(1 + numberOfPointers + (wordsBehindPointers + wordsOfHeader - 1) / wordsOfHeader);
        IDSetHeader* record = records.at(index);
        *record = header;
        if (hasLazyList)
            new (NotNull, &lazyListOfIDSetRecord(record)) std::atomic<uint32_t*>(nullptr);
        uint32_t* wordsInside = std::ranges::copy(attachedWords, reinterpret_cast<uint32_t*>(record + 1 + numberOfPointers)).out;
        if (wordsLieOutside) {
            uint32_t* words = wordsOutsideRecords.allocate(membersOrEntries.size());
            std::ranges::copy(membersOrEntries, words);
            wordsOutsideIDSetRecord(record) = words;
        } else
            std::ranges::copy(membersOrEntries, wordsInside);
        uint32_t id = idInIDSetTable(shardIndex, index);
        indexOfRecords.add(hash, id);
        return id;
    }

    static const uint32_t* lastingListOfOne(const IDSetTable& table, uint32_t member)
    {
        uint32_t hash = spreadHashForIDSetTable(member);
        uint32_t shardIndex = shardOfHashInIDSetTable(hash);
        Shard& shard = table.m_shards[shardIndex];
        auto listOf = [&](uint32_t id) {
            return reinterpret_cast<const uint32_t*>(shard.records.at(indexOfIDInIDSetTable(id)) + 1) + table.m_numberOfAttachedWords;
        };
        auto isWanted = [&](uint32_t id) {
            return shard.records.at(indexOfIDInIDSetTable(id))->size == 1 && *listOf(id) == member;
        };
        uint32_t id = shard.indexOfRecords.find(hash, isWanted);
        if (!id) {
            Locker locker { shard.lock };
            id = shard.indexOfRecords.find(hash, isWanted);
            if (!id) {
                Vector<uint32_t, 8> attachedWords;
                attachedWords.fill(0u, table.m_numberOfAttachedWords);
                id = shard.addRecord(table.m_form, shardIndex, hash, IDSetHeader { 1, 0 }, attachedWords.span(), std::span<const uint32_t>(&member, 1));
            }
        }
        return listOf(id);
    }

    IDSetTableIndex indexOfRecords;
    IDSetTableIndex indexOfChunks;
    IDSetTableSegments<IDSetHeader, 8> records;
    IDSetTableSegments<IDSetChunk, 6> chunks;
    IDSetTableWordBlocks wordsOutsideRecords;
    Lock lock;
};

static bool idSetRecordMatches(const IDSetView& record, const IDSetView& members)
{
    if (record.size() != members.size() || record.isList() != members.isList())
        return false;
    if (record.isList())
        return std::ranges::equal(record.list(), members.list());
    return std::ranges::equal(record.chunkEntries(), members.chunkEntries(), [&](const IDSetChunkEntry& held, const IDSetChunkEntry& wanted) {
        return held.firstMemberDividedBySize == wanted.firstMemberDividedBySize && (held.chunk == wanted.chunk || record.chunkAt(held.chunk) == members.chunkAt(wanted.chunk));
    });
}

IDSetTable::IDSetTable(IDSetForm form, uint32_t numberOfAttachedWords)
    : m_form(form)
    , m_numberOfAttachedWords(numberOfAttachedWords)
    , m_shards(std::make_unique<Shard[]>(numberOfShards))
{
}

IDSetTable::~IDSetTable() = default;

void IDSetTable::freeOutgrownSlots()
{
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex) {
        m_shards[shardIndex].indexOfRecords.freeOutgrownSlots();
        m_shards[shardIndex].indexOfChunks.freeOutgrownSlots();
    }
}

void IDSetTable::reserveMembers(uint32_t size)
{
    if (!m_allMembers.isEmpty())
        return;
    size = std::min(size, idPositionMask + 1);
    m_allMembers.grow(size);
    for (uint32_t member = 0; member < size; ++member)
        m_allMembers[member] = member;
}

const uint32_t* IDSetTable::lastingListOfOne(uint32_t member) const
{
    return Shard::lastingListOfOne(*this, member);
}

IDSetView IDSetTable::viewOfRecord(uint32_t set) const
{
    IDSetView view;
    view.m_table = this;
    if (set == UINT32_MAX)
        return view;

    RELEASE_ASSERT(static_cast<IDHome>(set >> idHomeShift) == IDHome::Program);
    IDSetHeader* header = m_shards[shardOfIDInIDSetTable(set)].records.at(indexOfIDInIDSetTable(set));
    RELEASE_ASSERT(header->size);
    view.m_size = header->size;
    view.m_numberOfChunks = header->numberOfChunks;
    view.m_numberOfAttachedWords = m_numberOfAttachedWords;
    bool wordsLieOutside = wordsOfIDSetLieOutside(*header);
    view.m_attachedWords = reinterpret_cast<const uint32_t*>(header + 1 + wordsLieOutside + idSetRecordHasLazyList(m_form, *header));
    view.m_words = wordsLieOutside ? wordsOutsideIDSetRecord(header) : view.m_attachedWords + m_numberOfAttachedWords;
    return view;
}

uint32_t IDSetTable::sizeOfRecord(uint32_t set) const
{
    if (set == UINT32_MAX)
        return 0;
    RELEASE_ASSERT(static_cast<IDHome>(set >> idHomeShift) == IDHome::Program);
    uint32_t size = m_shards[shardOfIDInIDSetTable(set)].records.at(indexOfIDInIDSetTable(set))->size;
    RELEASE_ASSERT(size);
    return size;
}

std::span<const uint32_t> IDSetTable::listOfRecord(uint32_t set) const
{
    if (set == UINT32_MAX)
        return { };
    if (static_cast<IDHome>(set >> idHomeShift) == IDHome::Value)
        return unsafeMakeSpan(lastingListOfOne(set), 1);

    RELEASE_ASSERT(static_cast<IDHome>(set >> idHomeShift) == IDHome::Program);
    Shard& shard = m_shards[shardOfIDInIDSetTable(set)];
    IDSetHeader* header = shard.records.at(indexOfIDInIDSetTable(set));
    RELEASE_ASSERT(header->size);
    if (!header->numberOfChunks) {
        const uint32_t* words = wordsOfIDSetLieOutside(*header) ? wordsOutsideIDSetRecord(header) : reinterpret_cast<const uint32_t*>(header + 1) + m_numberOfAttachedWords;
        return unsafeMakeSpan(words, header->size);
    }

    std::atomic<uint32_t*>& place = lazyListOfIDSetRecord(header);
    uint32_t* list = place.load(std::memory_order_acquire);
    if (!list) {
        Locker locker { shard.lock };
        list = place.load(std::memory_order_relaxed);
        if (!list) {
            list = shard.wordsOutsideRecords.allocate(header->size);
            uint32_t* next = list;
            viewOfRecord(set).forEach([&](uint32_t member) {
                *next++ = member;
            });
            place.store(list, std::memory_order_release);
        }
    }
    return unsafeMakeSpan(list, header->size);
}

size_t IDSetTable::sizeInBytes() const
{
    size_t size = m_allMembers.capacity() * sizeof(uint32_t) + numberOfShards * sizeof(Shard);
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex) {
        Shard& shard = m_shards[shardIndex];
        Locker locker { shard.lock };
        size += shard.indexOfRecords.sizeInBytes() + shard.indexOfChunks.sizeInBytes() + shard.records.bytesInUse() + shard.wordsOutsideRecords.sizeInBytes() + shard.chunks.bytesInUse();
    }
    return size;
}

IDSetTable::Contents IDSetTable::contents() const
{
    Contents contents;
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex) {
        Shard& shard = m_shards[shardIndex];
        Locker locker { shard.lock };
        contents.numberOfSets += shard.indexOfRecords.size();
        contents.numberOfChunks += shard.indexOfChunks.size();
        contents.bytesOfRecords += shard.records.bytesInUse();
        contents.bytesOutsideRecords += shard.wordsOutsideRecords.sizeInBytes();
        contents.bytesOfIndexes += shard.indexOfRecords.sizeInBytes() + shard.indexOfChunks.sizeInBytes();
    }
    return contents;
}

std::optional<uint32_t> IDSetTable::find(uint32_t hash, const IDSetView& members) const
{
    RELEASE_ASSERT(members.size() > 1);
    hash = spreadHashForIDSetTable(hash);
    uint32_t found = m_shards[shardOfHashInIDSetTable(hash)].indexOfRecords.find(hash, [&](uint32_t id) {
        return idSetRecordMatches(viewOf(id), members);
    });
    if (!found)
        return std::nullopt;
    return found;
}

uint32_t IDSetTable::findOrAdd(uint32_t hash, const IDSetView& members, std::span<const uint32_t> attachedWords, bool& isNew)
{
    isNew = false;
    RELEASE_ASSERT(!m_isFrozen);
    RELEASE_ASSERT(members.size() > 1 && attachedWords.size() == m_numberOfAttachedWords);
    RELEASE_ASSERT(members.isList() == (members.size() <= mostMembersOfList));

    hash = spreadHashForIDSetTable(hash);
    uint32_t shardIndex = shardOfHashInIDSetTable(hash);
    Shard& shard = m_shards[shardIndex];
    auto isWanted = [&](uint32_t id) {
        return idSetRecordMatches(viewOf(id), members);
    };
    if (uint32_t found = shard.indexOfRecords.find(hash, isWanted))
        return found;

    Locker locker { shard.lock };
    if (uint32_t found = shard.indexOfRecords.find(hash, isWanted))
        return found;
    RELEASE_ASSERT(!takesNoAdditions());
    for (const IDSetChunkEntry& entry : members.chunkEntries())
        RELEASE_ASSERT(static_cast<IDHome>(entry.chunk >> idHomeShift) == IDHome::Program);
    size_t numberOfWords = members.isList() ? members.size() : members.chunkEntries().size_bytes() / sizeof(uint32_t);
    isNew = true;
    return shard.addRecord(m_form, shardIndex, hash, IDSetHeader { members.m_size, members.m_numberOfChunks }, attachedWords, unsafeMakeSpan(members.m_words, numberOfWords));
}

std::optional<uint32_t> IDSetTable::findChunk(uint32_t hash, const IDSetChunk& chunk) const
{
    hash = spreadHashForIDSetTable(hash);
    const Shard& shard = m_shards[shardOfHashInIDSetTable(hash)];
    uint32_t found = shard.indexOfChunks.find(hash, [&](uint32_t id) {
        return *shard.chunks.at(indexOfIDInIDSetTable(id)) == chunk;
    });
    if (!found)
        return std::nullopt;
    return found;
}

uint32_t IDSetTable::findOrAddChunk(uint32_t hash, const IDSetChunk& chunk, bool& isNew)
{
    isNew = false;
    RELEASE_ASSERT(!m_isFrozen);
    RELEASE_ASSERT(chunk != IDSetChunk { });

    hash = spreadHashForIDSetTable(hash);
    uint32_t shardIndex = shardOfHashInIDSetTable(hash);
    Shard& shard = m_shards[shardIndex];
    auto isWanted = [&](uint32_t id) {
        return *shard.chunks.at(indexOfIDInIDSetTable(id)) == chunk;
    };
    if (uint32_t found = shard.indexOfChunks.find(hash, isWanted))
        return found;

    Locker locker { shard.lock };
    if (uint32_t found = shard.indexOfChunks.find(hash, isWanted))
        return found;
    RELEASE_ASSERT(!takesNoAdditions());
    isNew = true;
    uint32_t index = shard.chunks.allocate(1);
    *shard.chunks.at(index) = chunk;
    uint32_t id = idInIDSetTable(shardIndex, index);
    shard.indexOfChunks.add(hash, id);
    return id;
}

bool IDSetView::contains(uint32_t member) const
{
    if (isList())
        return std::ranges::binary_search(list(), member);
    std::span<const IDSetChunkEntry> entries = chunkEntries();
    uint32_t number = member / membersPerSetChunk;
    auto entry = std::ranges::lower_bound(entries, number, { }, &IDSetChunkEntry::firstMemberDividedBySize);
    if (entry == entries.end() || entry->firstMemberDividedBySize != number)
        return false;
    uint32_t bit = member % membersPerSetChunk;
    return chunkAt(entry->chunk).bits[bit / 64] >> bit % 64 & 1;
}

uint32_t IDSetView::first() const
{
    if (isEmpty())
        return UINT32_MAX;
    if (isList())
        return list().front();
    const IDSetChunkEntry& entry = chunkEntries().front();
    const IDSetChunk& chunk = chunkAt(entry.chunk);
    for (uint32_t word = 0; word < chunk.bits.size(); ++word) {
        if (chunk.bits[word])
            return entry.firstMemberDividedBySize * membersPerSetChunk + word * 64 + static_cast<uint32_t>(std::countr_zero(chunk.bits[word]));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint32_t IDSetView::last() const
{
    if (isEmpty())
        return UINT32_MAX;
    if (isList())
        return list().back();
    const IDSetChunkEntry& entry = chunkEntries().back();
    const IDSetChunk& chunk = chunkAt(entry.chunk);
    for (uint32_t word = static_cast<uint32_t>(chunk.bits.size()); word--;) {
        if (chunk.bits[word])
            return entry.firstMemberDividedBySize * membersPerSetChunk + word * 64 + 63 - static_cast<uint32_t>(std::countl_zero(chunk.bits[word]));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const IDSetChunk& IDSetView::chunkAt(uint32_t encodedChunk) const
{
    switch (static_cast<IDHome>(encodedChunk >> idHomeShift)) {
    case IDHome::Program:
        return *m_table->m_shards[shardOfIDInIDSetTable(encodedChunk)].chunks.at(indexOfIDInIDSetTable(encodedChunk));
    case IDHome::Thread:
        return m_arena->m_chunks[encodedChunk & idPositionMask];
    case IDHome::Value:
    case IDHome::SmallRange:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

class IDSetReadingCursor {
    WTF_MAKE_NONCOPYABLE(IDSetReadingCursor);
public:
    explicit IDSetReadingCursor(const IDSetView& set LIFETIME_BOUND)
        : m_set(set)
        , m_members(set.isList() ? set.list() : std::span<const uint32_t> { })
        , m_entries(set.chunkEntries())
        , m_end(set.isList() ? m_members.size() : m_entries.size())
    {
        settle();
    }

    bool isAtEnd() const { return m_index == m_end; }
    bool isAtLastStretch() const { return m_next == m_end; }
    uint32_t number() const { return m_number; }
    const IDSetChunk& chunk() const { return m_set.isList() ? m_gathered : m_set.chunkAt(m_entries[m_index].chunk); }

    bool holdsSameChunkAs(const IDSetReadingCursor& other) const
    {
        if (m_set.isList() || other.m_set.isList() || m_entries[m_index].chunk != other.m_entries[other.m_index].chunk)
            return false;
        ASSERT(&chunk() == &other.chunk());
        return true;
    }

    void advance()
    {
        m_index = m_next;
        settle();
    }

    void advanceTo(uint32_t number)
    {
        if (isAtEnd() || m_number >= number)
            return;
        auto numberAt = [&](size_t index) {
            return m_set.isList() ? m_members[index] / membersPerSetChunk : m_entries[index].firstMemberDividedBySize;
        };
        size_t below = m_next - 1;
        size_t step = 1;
        while (below + step < m_end && numberAt(below + step) < number) {
            below += step;
            step *= 2;
        }
        size_t notBelow = std::min(below + step, m_end);
        while (notBelow - below > 1) {
            size_t middle = below + (notBelow - below) / 2;
            if (numberAt(middle) < number)
                below = middle;
            else
                notBelow = middle;
        }
        m_index = notBelow;
        settle();
    }

private:
    void settle()
    {
        if (isAtEnd())
            return;
        if (!m_set.isList()) {
            m_number = m_entries[m_index].firstMemberDividedBySize;
            m_next = m_index + 1;
            return;
        }
        m_number = m_members[m_index] / membersPerSetChunk;
        m_gathered = { };
        for (m_next = m_index; m_next < m_end && m_members[m_next] / membersPerSetChunk == m_number; ++m_next) {
            uint32_t bit = m_members[m_next] % membersPerSetChunk;
            m_gathered.bits[bit / 64] |= 1ull << bit % 64;
        }
    }

    const IDSetView& m_set;
    std::span<const uint32_t> m_members;
    std::span<const IDSetChunkEntry> m_entries;
    size_t m_end;
    size_t m_index { 0 };
    size_t m_next { 0 };
    uint32_t m_number { 0 };
    IDSetChunk m_gathered;
};

void forEachAddedMember(const IDSetView& larger, const IDSetView& smaller, NOESCAPE const ScopedLambda<void(uint32_t)>& functor)
{
    if (larger.isList() && larger.size() <= IDSetTable::mostMembersOfList) {
        for (uint32_t member : larger.list()) {
            if (!smaller.contains(member))
                functor(member);
        }
        return;
    }

    IDSetReadingCursor known(smaller);
    for (IDSetReadingCursor cursor(larger); !cursor.isAtEnd(); cursor.advance()) {
        known.advanceTo(cursor.number());
        bool isKnownInPart = !known.isAtEnd() && known.number() == cursor.number();
        if (isKnownInPart && cursor.holdsSameChunkAs(known))
            continue;
        IDSetChunk added = cursor.chunk();
        if (isKnownInPart) {
            const IDSetChunk& chunkOfKnown = known.chunk();
            for (size_t word = 0; word < added.bits.size(); ++word)
                added.bits[word] &= ~chunkOfKnown.bits[word];
        }
        for (uint32_t word = 0; word < added.bits.size(); ++word) {
            for (uint64_t bits = added.bits[word]; bits; bits &= bits - 1)
                functor(cursor.number() * membersPerSetChunk + word * 64 + static_cast<uint32_t>(std::countr_zero(bits)));
        }
    }
}

bool isSubset(const IDSetView& set, const IDSetView& of)
{
    if (set.size() > of.size())
        return false;
    if (set.isList() && set.size() <= IDSetTable::mostMembersOfList) {
        return std::ranges::all_of(set.list(), [&](uint32_t member) {
            return of.contains(member);
        });
    }

    IDSetReadingCursor outer(of);
    for (IDSetReadingCursor inner(set); !inner.isAtEnd(); inner.advance()) {
        outer.advanceTo(inner.number());
        if (outer.isAtEnd() || outer.number() != inner.number())
            return false;
        if (inner.holdsSameChunkAs(outer))
            continue;
        const IDSetChunk& chunkOfInner = inner.chunk();
        const IDSetChunk& chunkOfOuter = outer.chunk();
        for (size_t word = 0; word < chunkOfInner.bits.size(); ++word) {
            if (chunkOfInner.bits[word] & ~chunkOfOuter.bits[word])
                return false;
        }
    }
    return true;
}

bool overlap(const IDSetView& first, const IDSetView& second)
{
    const IDSetView& smaller = first.size() <= second.size() ? first : second;
    const IDSetView& larger = first.size() <= second.size() ? second : first;
    if (smaller.isList() && smaller.size() <= IDSetTable::mostMembersOfList) {
        return std::ranges::any_of(smaller.list(), [&](uint32_t member) {
            return larger.contains(member);
        });
    }

    IDSetReadingCursor a(smaller);
    IDSetReadingCursor b(larger);
    while (!a.isAtEnd() && !b.isAtEnd()) {
        if (a.number() != b.number()) {
            a.advanceTo(b.number());
            if (!a.isAtEnd())
                b.advanceTo(a.number());
            continue;
        }
        if (a.holdsSameChunkAs(b))
            return true;
        const IDSetChunk& chunkOfA = a.chunk();
        const IDSetChunk& chunkOfB = b.chunk();
        for (size_t word = 0; word < chunkOfA.bits.size(); ++word) {
            if (chunkOfA.bits[word] & chunkOfB.bits[word])
                return true;
        }
        a.advance();
        b.advance();
    }
    return false;
}

std::strong_ordering compareByContent(const IDSetView& first, const IDSetView& second)
{
    if (first.isList() && second.isList()) {
        std::span<const uint32_t> a = first.list();
        std::span<const uint32_t> b = second.list();
        return std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
    }

    auto hasMemberAfter = [](const IDSetReadingCursor& cursor, const IDSetChunk& chunk, size_t word, uint64_t bitsUpToMember) {
        if ((chunk.bits[word] & ~bitsUpToMember) || !cursor.isAtLastStretch())
            return true;
        while (++word < chunk.bits.size()) {
            if (chunk.bits[word])
                return true;
        }
        return false;
    };

    IDSetReadingCursor a(first);
    IDSetReadingCursor b(second);
    for (; !a.isAtEnd() && !b.isAtEnd(); a.advance(), b.advance()) {
        if (a.number() != b.number())
            return a.number() <=> b.number();
        if (a.holdsSameChunkAs(b))
            continue;
        const IDSetChunk& chunkOfA = a.chunk();
        const IDSetChunk& chunkOfB = b.chunk();
        for (size_t word = 0; word < chunkOfA.bits.size(); ++word) {
            uint64_t inOneAlone = chunkOfA.bits[word] ^ chunkOfB.bits[word];
            if (!inOneAlone)
                continue;
            uint64_t bitsUpToMember = inOneAlone ^ (inOneAlone - 1);
            bool isInFirst = chunkOfA.bits[word] & inOneAlone & bitsUpToMember;
            bool otherGoesOn = isInFirst ? hasMemberAfter(b, chunkOfB, word, bitsUpToMember) : hasMemberAfter(a, chunkOfA, word, bitsUpToMember);
            return isInFirst == otherGoesOn ? std::strong_ordering::less : std::strong_ordering::greater;
        }
    }
    if (a.isAtEnd() && b.isAtEnd())
        return std::strong_ordering::equal;
    return a.isAtEnd() ? std::strong_ordering::less : std::strong_ordering::greater;
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
