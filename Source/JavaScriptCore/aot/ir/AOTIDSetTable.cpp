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

// The index of a record counts headers, and a shard has 24 bits of them. So the members or entries of a long set lie outside its record: positions run out with the number of sets and not with their length.
static constexpr uint32_t fewestWordsOfIDSetThatLieOutside = 32;

static constexpr bool wordsOfIDSetLieOutside(IDSetHeader header)
{
    uint32_t numberOfWords = header.numberOfChunks ? header.numberOfChunks * static_cast<uint32_t>(sizeof(IDSetChunkEntry) / sizeof(uint32_t)) : header.size;
    return numberOfWords >= fewestWordsOfIDSetThatLieOutside;
}
static uint32_t*& wordsOutsideIDSetRecord(IDSetHeader* header) { return *reinterpret_cast<uint32_t**>(header + 1); }
static_assert(sizeof(uint32_t*) == sizeof(IDSetHeader));

// IDSetForm::ChunksWithLazyList: a set with chunks has room for a pointer to its list, behind the pointer to its entries where it has one. Null until IDSetTable::listOf() is asked.
static constexpr bool idSetRecordHasLazyList(IDSetForm form, IDSetHeader header) { return form == IDSetForm::ChunksWithLazyList && header.numberOfChunks; }
static std::atomic<uint32_t*>& lazyListOfIDSetRecord(IDSetHeader* header) { return *reinterpret_cast<std::atomic<uint32_t*>*>(header + 1 + wordsOfIDSetLieOutside(*header)); }
static_assert(sizeof(std::atomic<uint32_t*>) == sizeof(IDSetHeader) && alignof(std::atomic<uint32_t*>) <= 2 * alignof(IDSetHeader));

static constexpr uint32_t bitsOfShardInIDSetTable = 6;
static constexpr uint32_t bitsOfIndexInIDSetShard = idHomeShift - bitsOfShardInIDSetTable;
static constexpr size_t alignmentInIDSetTable = 64;

static_assert(IDSetTable::numberOfShards == 1u << bitsOfShardInIDSetTable);
static_assert(sizeof(IDSetHeader) == 2 * sizeof(uint32_t) && sizeof(IDSetChunkEntry) == 2 * sizeof(uint32_t));

// The id of a record or of a chunk: IDHome::Program, the shard, the index within the shard.
static constexpr uint32_t idInIDSetTable(uint32_t shard, uint32_t index)
{
    return static_cast<uint32_t>(IDHome::Program) << idHomeShift | shard << bitsOfIndexInIDSetShard | index;
}

static constexpr uint32_t shardOfIDInIDSetTable(uint32_t id) { return (id & idPositionMask) >> bitsOfIndexInIDSetShard; }
static constexpr uint32_t indexOfIDInIDSetTable(uint32_t id) { return id & ((1u << bitsOfIndexInIDSetShard) - 1); }

// The upper bits pick the shard and the lower bits the slot, so each bit has to depend on the whole hash. The mix is a bijection, so it adds no collisions.
static constexpr uint32_t spreadHashForIDSetTable(uint32_t hash)
{
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ hash >> 16;
}

static constexpr uint32_t shardOfHashInIDSetTable(uint32_t spreadHash) { return spreadHash >> (32 - bitsOfShardInIDSetTable); }

// Elements that never move. Each segment is twice as large as the one before it, so an index names its segment without a search.
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

    // A segment is stored before any index into it is published, so a thread that read the index from a published slot, or was handed it with synchronization, sees the segment.
    Element* at(uint32_t index) const
    {
        uint32_t segment = segmentOf(index);
        return m_segments[segment].load(std::memory_order_relaxed) + (index - firstIndexOf(segment));
    }

    // Under the shard's lock. The elements are contiguous.
    uint32_t allocate(size_t count)
    {
        uint32_t segment = segmentOf(m_next);
        while (segment < numberOfSegments && count > firstIndexOf(segment + 1) - m_next)
            m_next = firstIndexOf(++segment);
        // The thirty bits of a position are used up.
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
    // Every index that was handed out is below it.
    uint32_t endOfIndexes() const { return m_next; }

private:
    static constexpr uint32_t segmentOf(uint32_t index) { return static_cast<uint32_t>(std::bit_width(index + sizeOfFirstSegment)) - 1 - log2OfFirstSegment; }
    static constexpr uint32_t firstIndexOf(uint32_t segment) { return (sizeOfFirstSegment << segment) - sizeOfFirstSegment; }

    std::array<std::atomic<Element*>, numberOfSegments> m_segments { };
    uint32_t m_next { 0 };
    uint32_t m_numberInUse { 0 };
};

// From a hash to the ids of one shard. A slot holds the hash in its upper half and the id in its lower half, and no id is 0.
// Searching takes no lock, so slots that were outgrown stay as they are until the table is frozen or dies: somebody may still be reading them.
class IDSetTableIndex {
    WTF_MAKE_NONCOPYABLE(IDSetTableIndex);
public:
    IDSetTableIndex() = default;

    ~IDSetTableIndex()
    {
        for (Slots* slots = m_slots.load(std::memory_order_relaxed); slots;)
            fastFree(std::exchange(slots, slots->outgrown));
    }

    // 0: it was not there when the search began.
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

    // Under the shard's lock, after a find() under that lock found nothing. The record or chunk the id names is fully written.
    void add(uint32_t hash, uint32_t id)
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        if (!slots || (static_cast<size_t>(m_size) + 1) * 3 > (slots->mask + 1) * 2)
            slots = grow(slots);
        put(*slots, static_cast<uint64_t>(hash) << 32 | id);
        ++m_size;
    }

    // By one thread, while nobody searches or adds: nobody is left who reads them.
    void freeOutgrownSlots()
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        for (Slots* outgrown = slots ? std::exchange(slots->outgrown, nullptr) : nullptr; outgrown;)
            fastFree(std::exchange(outgrown, outgrown->outgrown));
    }

    // Under the shard's lock.
    size_t sizeInBytes() const
    {
        size_t size = 0;
        for (Slots* slots = m_slots.load(std::memory_order_relaxed); slots; slots = slots->outgrown)
            size += sizeof(Slots) + (slots->mask + 1) * sizeof(std::atomic<uint64_t>);
        return size;
    }

    // By one thread, while nobody searches or adds. The functor is called once for each id.
    void removeIf(NOESCAPE const Invocable<bool(uint32_t)> auto& shouldRemove)
    {
        Slots* former = m_slots.load(std::memory_order_relaxed);
        if (!former)
            return;
        Vector<uint64_t> kept;
        for (size_t place = 0; place <= former->mask; ++place) {
            uint64_t slot = former->at(place).load(std::memory_order_relaxed);
            if (slot && !shouldRemove(static_cast<uint32_t>(slot)))
                kept.append(slot);
        }
        m_slots.store(nullptr, std::memory_order_relaxed);
        m_size = 0;
        while (former)
            fastFree(std::exchange(former, former->outgrown));
        for (uint64_t slot : kept)
            add(static_cast<uint32_t>(slot >> 32), static_cast<uint32_t>(slot));
        freeOutgrownSlots();
    }

    // Both under the shard's lock.
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
    // The slots follow it in memory.
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

// The words of one shard that lie outside its records. They never move, and they go block by block with the table: nobody goes over the sets to find them.
class IDSetTableWordBlocks {
    WTF_MAKE_NONCOPYABLE(IDSetTableWordBlocks);
public:
    IDSetTableWordBlocks() = default;

    ~IDSetTableWordBlocks()
    {
        for (Block* block = m_newest; block;)
            fastFree(std::exchange(block, block->older));
    }

    // Under the shard's lock.
    uint32_t* allocate(size_t numberOfWords)
    {
        if (numberOfWords > m_free.size())
            return allocateInNewBlock(numberOfWords);
        uint32_t* words = m_free.data();
        m_free = m_free.subspan(numberOfWords);
        return words;
    }

    // Under the shard's lock. The blocks whole, with the room that nothing has taken.
    size_t sizeInBytes() const { return m_sizeInBytes; }

    // Under the shard's lock, while nobody reads a word of the blocks that go.
    void replaceWith(IDSetTableWordBlocks& other)
    {
        for (Block* block = m_newest; block;)
            fastFree(std::exchange(block, block->older));
        m_newest = std::exchange(other.m_newest, nullptr);
        m_free = std::exchange(other.m_free, { });
        m_wordsOfNextBlock = other.m_wordsOfNextBlock;
        m_sizeInBytes = std::exchange(other.m_sizeInBytes, 0);
    }

    // Under the shard's lock. No word moves. The room that nothing has taken in the blocks of the other is left as it is.
    void takeBlocksOf(IDSetTableWordBlocks& other)
    {
        Block* oldestOfOther = other.m_newest;
        if (!oldestOfOther)
            return;
        while (oldestOfOther->older)
            oldestOfOther = oldestOfOther->older;
        oldestOfOther->older = m_newest;
        m_newest = std::exchange(other.m_newest, nullptr);
        m_sizeInBytes += std::exchange(other.m_sizeInBytes, 0);
        other.m_free = { };
    }

private:
    // The words follow it in memory.
    struct Block {
        Block* older;
    };
    static_assert(!(sizeof(Block) % alignof(IDSetChunkEntry)));

    uint32_t* allocateInNewBlock(size_t numberOfWords)
    {
        constexpr size_t maxWordsPerBlock = 1 << 16;
        // What would take much of a block gets one of its own size, and the block that has room left stays the one that is filled.
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
    // Under the shard's lock. Room is handed out in units of one header.
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

    // A list of `member` alone that lasts as long as the table. It lies in a record that no id is handed out for: the set stays its own id.
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
    // The members or entries of long sets, and the lazy lists.
    IDSetTableWordBlocks wordsOutsideRecords;
    // A record at a lower index lasts as long as the table, and what lies outside it lies here and never moves: IDSetTable::keepAllSetsMadeSoFar().
    uint32_t firstRecordThatMayGo { UINT32_MAX };
    IDSetTableWordBlocks wordsOutsideRecordsThatLast;
    // From beginCollection() to collectShard(): a byte for each index of `records`.
    Vector<uint8_t> isNamed;
    Lock lock;
};

// By content: where the ids of two chunks differ, the chunks are compared.
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
    // Lists that were handed out point into it, so it is filled once and never moves. viewOf() handles a member past its end another way.
    if (!m_allMembers.isEmpty())
        return;
    m_allMembers.grow(size);
    for (uint32_t member = 0; member < size; ++member)
        m_allMembers[member] = member;
}

IDSetView IDSetTable::viewOf(uint32_t set) const
{
    IDSetView view;
    view.m_table = this;
    if (set == UINT32_MAX)
        return view;

    switch (static_cast<IDHome>(set >> idHomeShift)) {
    case IDHome::Value:
        view.m_size = 1;
        view.m_onlyMember = set & idPositionMask;
        // membersOf() hands out the list of a view that is gone by then, and the reader makes layout sets before reserveMembers() runs.
        if (view.m_onlyMember < m_allMembers.size())
            view.m_words = &m_allMembers[view.m_onlyMember];
        else if (m_form == IDSetForm::ChunksWithLazyList)
            view.m_words = Shard::lastingListOfOne(*this, view.m_onlyMember);
        return view;
    case IDHome::Program: {
        IDSetHeader* header = m_shards[shardOfIDInIDSetTable(set)].records.at(indexOfIDInIDSetTable(set));
        // collectShard() took the set for one that nothing names.
        RELEASE_ASSERT(header->size);
        view.m_size = header->size;
        view.m_numberOfChunks = header->numberOfChunks;
        view.m_numberOfAttachedWords = m_numberOfAttachedWords;
        bool wordsLieOutside = wordsOfIDSetLieOutside(*header);
        view.m_attachedWords = reinterpret_cast<const uint32_t*>(header + 1 + wordsLieOutside + idSetRecordHasLazyList(m_form, *header));
        view.m_words = wordsLieOutside ? wordsOutsideIDSetRecord(header) : view.m_attachedWords + m_numberOfAttachedWords;
        return view;
    }
    case IDHome::SmallRange:
    case IDHome::Thread:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::span<const uint32_t> IDSetTable::listOf(uint32_t set) const
{
    RELEASE_ASSERT(m_form == IDSetForm::ChunksWithLazyList);
    IDSetView view = viewOf(set);
    if (view.isEmpty())
        return { };
    if (view.isList()) {
        // Of one member too: viewOf() points at a list that lasts.
        return unsafeMakeSpan(view.m_words, view.m_size);
    }

    Shard& shard = m_shards[shardOfIDInIDSetTable(set)];
    std::atomic<uint32_t*>& place = lazyListOfIDSetRecord(shard.records.at(indexOfIDInIDSetTable(set)));
    uint32_t* list = place.load(std::memory_order_acquire);
    if (!list) {
        // Whoever asks for the same list meanwhile waits for this one.
        Locker locker { shard.lock };
        list = place.load(std::memory_order_relaxed);
        if (!list) {
            list = shard.wordsOutsideRecords.allocate(view.size());
            uint32_t* next = list;
            view.forEach([&](uint32_t member) {
                *next++ = member;
            });
            place.store(list, std::memory_order_release);
        }
    }
    return unsafeMakeSpan(list, view.size());
}

size_t IDSetTable::sizeInBytes() const
{
    size_t size = m_allMembers.capacity() * sizeof(uint32_t) + numberOfShards * sizeof(Shard);
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex) {
        Shard& shard = m_shards[shardIndex];
        Locker locker { shard.lock };
        size += shard.indexOfRecords.sizeInBytes() + shard.indexOfChunks.sizeInBytes() + shard.records.bytesInUse() + shard.wordsOutsideRecords.sizeInBytes() + shard.wordsOutsideRecordsThatLast.sizeInBytes() + shard.chunks.bytesInUse();
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
        contents.bytesOutsideRecords += shard.wordsOutsideRecords.sizeInBytes() + shard.wordsOutsideRecordsThatLast.sizeInBytes();
        contents.bytesOfIndexes += shard.indexOfRecords.sizeInBytes() + shard.indexOfChunks.sizeInBytes();
    }
    return contents;
}

void IDSetTable::forEachSet(NOESCAPE const ScopedLambda<void(uint32_t set)>& functor) const
{
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex) {
        Shard& shard = m_shards[shardIndex];
        Locker locker { shard.lock };
        shard.indexOfRecords.forEachID([&](uint32_t id) {
            // A record of one member is a list that lastingListOfOne() keeps. No id is handed out for it.
            if (shard.records.at(indexOfIDInIDSetTable(id))->size > 1)
                functor(id);
        });
    }
}

void IDSetTable::keepAllSetsMadeSoFar()
{
    RELEASE_ASSERT(m_form == IDSetForm::ListsAndChunks);
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex) {
        Shard& shard = m_shards[shardIndex];
        Locker locker { shard.lock };
        shard.firstRecordThatMayGo = shard.records.endOfIndexes();
        shard.wordsOutsideRecordsThatLast.takeBlocksOf(shard.wordsOutsideRecords);
    }
}

void IDSetTable::beginCollection()
{
    for (uint32_t shardIndex = 0; shardIndex < numberOfShards; ++shardIndex)
        m_shards[shardIndex].isNamed.fill(0, m_shards[shardIndex].records.endOfIndexes());
}

void IDSetTable::markAsNamed(uint32_t set)
{
    if (static_cast<IDHome>(set >> idHomeShift) != IDHome::Program)
        return;
    // Every thread stores the same.
    uint8_t* isNamed = &m_shards[shardOfIDInIDSetTable(set)].isNamed[indexOfIDInIDSetTable(set)];
    if (!WTF::atomicLoad(isNamed, std::memory_order_relaxed))
        WTF::atomicStore(isNamed, uint8_t { 1 }, std::memory_order_relaxed);
}

bool IDSetTable::holds(uint32_t set) const
{
    if (set == UINT32_MAX || static_cast<IDHome>(set >> idHomeShift) != IDHome::Program)
        return true;
    return m_shards[shardOfIDInIDSetTable(set)].records.at(indexOfIDInIDSetTable(set))->size;
}

void IDSetTable::collectShard(uint32_t shardIndex)
{
    RELEASE_ASSERT(m_form == IDSetForm::ListsAndChunks && !m_isFrozen);
    Shard& shard = m_shards[shardIndex];
    Locker locker { shard.lock };
    // What stays moves together, so the blocks go whole.
    IDSetTableWordBlocks wordsThatStay;
    shard.indexOfRecords.removeIf([&](uint32_t id) {
        uint32_t index = indexOfIDInIDSetTable(id);
        if (index < shard.firstRecordThatMayGo)
            return false;
        IDSetHeader* record = shard.records.at(index);
        bool wordsLieOutside = wordsOfIDSetLieOutside(*record);
        if (!shard.isNamed[index]) {
            if (wordsLieOutside)
                wordsOutsideIDSetRecord(record) = nullptr;
            record->size = 0;
            return true;
        }
        if (wordsLieOutside) {
            std::span<const uint32_t> words = unsafeMakeSpan(wordsOutsideIDSetRecord(record), record->numberOfChunks * sizeof(IDSetChunkEntry) / sizeof(uint32_t));
            uint32_t* moved = wordsThatStay.allocate(words.size());
            std::ranges::copy(words, moved);
            wordsOutsideIDSetRecord(record) = moved;
        }
        return false;
    });
    shard.wordsOutsideRecords.replaceWith(wordsThatStay);
    shard.isNamed = { };
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
    // Equal sets have equal ids only while each set has one form.
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
    // A chunk of a thread dies with its arena.
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
    // overlap(), compareByContent() and first() assume that no chunk of a set is empty.
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

// For the empty set, the bits of the null id.
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

// Goes over a set by stretches of membersPerSetChunk numbers, whichever form it has. The number of a stretch is its first number divided by that size.
// A stretch of a list is gathered into a chunk here.
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

    // For two cursors at the same number, over sets of one table and one arena. False does not say that the chunks differ: then they have to be read.
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

    // To the first stretch from here on whose number is not below `number`.
    void advanceTo(uint32_t number)
    {
        if (isAtEnd() || m_number >= number)
            return;
        auto numberAt = [&](size_t index) {
            return m_set.isList() ? m_members[index] / membersPerSetChunk : m_entries[index].firstMemberDividedBySize;
        };
        // numberAt(below) is below `number`. notBelow is the end, or numberAt(notBelow) is not below it.
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
    // The stretch is the entry at m_index, or the members from m_index up to m_next.
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

// As two ascending lists of members compare: at the first place where they differ the smaller member comes first, and a list sorts before every longer list that it is a prefix of.
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
            // The least member that one set alone holds. Up to it the lists are equal, so its holder comes first unless the other list ends here.
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
