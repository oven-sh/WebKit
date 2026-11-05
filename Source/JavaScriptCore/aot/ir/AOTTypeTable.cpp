/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTDeclaredType.h"
#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t bitsOfShardInInternedTable = 6;
static constexpr uint32_t numberOfShardsOfInternedTable = 1u << bitsOfShardInInternedTable;
static constexpr size_t cacheLineOfInternedTable = 64;
static constexpr uint32_t wordsPerClassPartInTypeTable = 1 + mostContentsOfClass;

static_assert(InternedRecords<Type>::numberOfShards == numberOfShardsOfInternedTable);
static_assert(InternedRecords<PropertyKey>::numberOfShards == numberOfShardsOfInternedTable);

// TypeArena writes a list of parts as four words per part: the class, then the written contents. at(ClassPartsID) reinterprets those words as ClassParts.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(ClassPart) == wordsPerClassPartInTypeTable * sizeof(uint32_t) && alignof(ClassPart) == alignof(uint32_t));
static_assert(offsetof(ClassPart, writtenContents) == sizeof(uint32_t));

static constexpr uint64_t addToInternedHash(uint64_t hash, uint64_t bits)
{
    hash = (hash ^ bits) * 0x9e3779b97f4a7c15ull;
    return hash ^ hash >> 32;
}

static constexpr uint64_t addToInternedHash(uint64_t hash, uint32_t first, uint32_t second)
{
    return addToInternedHash(hash, static_cast<uint64_t>(first) << 32 | second);
}

// The upper bits pick the shard and the lower bits the slot. One more round of mixing makes both depend on every input bit.
static constexpr uint32_t finishInternedHash(uint64_t hash)
{
    return static_cast<uint32_t>(addToInternedHash(hash, 0));
}

static constexpr uint32_t shardOfInternedHash(uint32_t hash) { return hash >> (32 - bitsOfShardInInternedTable); }

uint32_t hashOf(const TypeParts& parts)
{
    uint64_t flags = parts.primitives
        | static_cast<uint64_t>(parts.seeds.toRaw()) << 16
        | static_cast<uint64_t>(parts.anyObject) << 32
        | static_cast<uint64_t>(parts.anyFunction) << 33
        | (parts.opaque ? static_cast<uint64_t>(*parts.opaque) + 1 : 0) << 34;
    uint64_t hash = addToInternedHash(0, flags);
    hash = addToInternedHash(hash, static_cast<uint64_t>(parts.integers.minimum));
    hash = addToInternedHash(hash, static_cast<uint64_t>(parts.integers.maximum));
    hash = addToInternedHash(hash, static_cast<uint64_t>(parts.boundsOfFractions.minimum));
    hash = addToInternedHash(hash, static_cast<uint64_t>(parts.boundsOfFractions.maximum));
    hash = addToInternedHash(hash, parts.environment.bits(), parts.stringLiterals.bits());
    hash = addToInternedHash(hash, parts.symbolLiterals.bits(), parts.objects.bits());
    hash = addToInternedHash(hash, parts.unnamedLayouts.bits(), parts.unnamedClasses.bits());
    return finishInternedHash(hash);
}

uint32_t hashOf(std::span<const uint32_t> words)
{
    uint64_t hash = words.size();
    for (uint32_t word : words)
        hash = addToInternedHash(hash, word);
    return finishInternedHash(hash);
}

uint32_t hashOf(const PropertyKey& key)
{
    uint64_t hash = addToInternedHash(0, static_cast<uint32_t>(key.kind), key.symbol ? static_cast<uint32_t>(*key.symbol) + 1 : 0);
    hash = addToInternedHash(hash, key.text.bits(), key.owner.bits());
    hash = addToInternedHash(hash, key.module.bits(), key.site.bits());
    hash = addToInternedHash(hash, key.callNumber ? static_cast<uint64_t>(*key.callNumber) + 1 : 0);
    return finishInternedHash(hash);
}

// The length goes in first, so a text that ends in zero bytes differs from a shorter one.
static uint64_t addBytesToInternedHash(uint64_t hash, std::span<const uint8_t> bytes)
{
    hash = addToInternedHash(hash, bytes.size());
    for (; bytes.size() >= sizeof(uint64_t); bytes = bytes.subspan(sizeof(uint64_t))) {
        uint64_t word;
        memcpy(&word, bytes.data(), sizeof(uint64_t));
        hash = addToInternedHash(hash, word);
    }
    uint64_t rest = 0;
    for (size_t index = 0; index < bytes.size(); ++index)
        rest |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    return addToInternedHash(hash, rest);
}

// As many as a mask of locations has bits.
static constexpr uint32_t numberOfLocationBitsOfKeys = 64;

// An id is a number in one of the program's lists, and those move when other files join the program. So a text goes in by its bytes, a module by the bytes of its path and a layout by the bit that the program gave it.
// The site of a created symbol is nothing but such a number and stays out. What stays out makes more keys share a bit and changes no answer.
static uint8_t locationBitOfKeyInTypeTable(const Program& program, const PropertyKey& key)
{
    uint64_t hash = addToInternedHash(0, static_cast<uint32_t>(key.kind), key.symbol ? static_cast<uint32_t>(*key.symbol) + 1 : 0);
    hash = addToInternedHash(hash, key.callNumber ? static_cast<uint64_t>(*key.callNumber) + 1 : 0);
    if (key.text)
        hash = addBytesToInternedHash(hash, program.string(key.text));
    if (std::optional<uint8_t> bitOfOwner = program.locationBitOf(key.owner))
        hash = addToInternedHash(hash, static_cast<uint64_t>(*bitOfOwner) + 1);
    if (key.module)
        hash = addBytesToInternedHash(hash, program.string(program.at(key.module).path));
    return static_cast<uint8_t>(finishInternedHash(hash) % numberOfLocationBitsOfKeys);
}

// Each segment is twice as large as the one before it, so a position names its segment without a search.
static constexpr uint32_t log2OfFirstInternedSegment = 8;
static constexpr uint32_t sizeOfFirstInternedSegment = 1u << log2OfFirstInternedSegment;

static constexpr uint32_t internedSegmentOf(uint32_t position)
{
    return static_cast<uint32_t>(std::bit_width(position + sizeOfFirstInternedSegment)) - 1 - log2OfFirstInternedSegment;
}

static constexpr uint32_t firstPositionOfInternedSegment(uint32_t segment)
{
    return (sizeOfFirstInternedSegment << segment) - sizeOfFirstInternedSegment;
}

static constexpr uint32_t numberOfInternedSegments = internedSegmentOf(largestIDPosition) + 1;

static_assert(!internedSegmentOf(0) && !internedSegmentOf(sizeOfFirstInternedSegment - 1) && internedSegmentOf(sizeOfFirstInternedSegment) == 1);
static_assert(firstPositionOfInternedSegment(1) == sizeOfFirstInternedSegment && internedSegmentOf(firstPositionOfInternedSegment(2) - 1) == 1);

// The elements of all shards of one table, by position. Positions are dense and follow the order of arrival. An element never moves.
template<typename Element>
class InternedElements {
    WTF_MAKE_NONCOPYABLE(InternedElements);
public:
    static_assert(std::is_trivially_destructible_v<Element>);

    InternedElements() = default;

    ~InternedElements()
    {
        for (auto& segment : m_segments) {
            if (Element* elements = segment.load(std::memory_order_relaxed))
                fastFree(elements);
        }
    }

    uint32_t size() const { return m_size.load(std::memory_order_relaxed); }
    size_t sizeInBytes() const { return static_cast<size_t>(size()) * sizeof(Element); }

    // A position is handed out only after its segment and element are written, so a thread that got the position through a lock or an acquire load sees both.
    const Element& at(uint32_t position) const
    {
        RELEASE_ASSERT(position <= largestIDPosition);
        ASSERT(position < size());
        uint32_t segment = internedSegmentOf(position);
        const Element* elements = m_segments[segment].load(std::memory_order_relaxed);
        RELEASE_ASSERT(elements);
        return elements[position - firstPositionOfInternedSegment(segment)];
    }

    // Call with a shard's lock held, after a search under that lock missed, so every position taken is filled. Threads holding different shards' locks may be here at once.
    uint32_t append(const Element& element)
    {
        uint32_t position = m_size.fetch_add(1, std::memory_order_relaxed);
        // Fails once the thirty bits of a position are used up.
        RELEASE_ASSERT(position <= largestIDPosition);
        uint32_t segment = internedSegmentOf(position);
        new (NotNull, elementsOf(segment) + (position - firstPositionOfInternedSegment(segment))) Element(element);
        return position;
    }

    // By one thread, while nobody appends. put() fills each of the positions from the answer on, once, before anybody is handed the position.
    uint32_t takePositions(uint32_t count)
    {
        uint32_t first = m_size.fetch_add(count, std::memory_order_relaxed);
        RELEASE_ASSERT(first <= largestIDPosition + 1 && count <= largestIDPosition + 1 - first);
        return first;
    }

    void put(uint32_t position, const Element& element)
    {
        ASSERT(position < size());
        uint32_t segment = internedSegmentOf(position);
        new (NotNull, elementsOf(segment) + (position - firstPositionOfInternedSegment(segment))) Element(element);
    }

private:
    Element* elementsOf(uint32_t segment)
    {
        if (Element* elements = m_segments[segment].load(std::memory_order_acquire))
            return elements;
        Locker locker { m_lockOfSegments };
        Element* elements = m_segments[segment].load(std::memory_order_relaxed);
        if (!elements) {
            elements = static_cast<Element*>(fastMalloc((static_cast<size_t>(sizeOfFirstInternedSegment) << segment) * sizeof(Element)));
            m_segments[segment].store(elements, std::memory_order_release);
        }
        return elements;
    }

    std::array<std::atomic<Element*>, numberOfInternedSegments> m_segments { };
    // Every append writes the size and every read loads a segment pointer, so the two must not share a cache line.
    alignas(cacheLineOfInternedTable) std::atomic<uint32_t> m_size { 0 };
    Lock m_lockOfSegments;
};

// Maps a hash to the positions of one shard. A slot holds the hash in its upper half and the position plus one in its lower half, and is zero while free.
// Searching takes no lock, so outgrown slot arrays are kept unchanged until the table is frozen or destroyed: another thread may still be reading them.
class InternedPositions {
    WTF_MAKE_NONCOPYABLE(InternedPositions);
public:
    InternedPositions() = default;

    ~InternedPositions()
    {
        for (Slots* slots = m_slots.load(std::memory_order_relaxed); slots;)
            fastFree(std::exchange(slots, slots->outgrown));
    }

    // std::nullopt: it was not there when the search began.
    std::optional<uint32_t> find(uint32_t hash, NOESCAPE const Invocable<bool(uint32_t)> auto& isWanted) const
    {
        Slots* slots = m_slots.load(std::memory_order_acquire);
        if (!slots)
            return std::nullopt;
        for (size_t place = hash & slots->mask;; place = (place + 1) & slots->mask) {
            uint64_t slot = slots->at(place).load(std::memory_order_acquire);
            if (!slot)
                return std::nullopt;
            uint32_t position = static_cast<uint32_t>(slot) - 1;
            if (slot >> 32 == hash && isWanted(position))
                return position;
        }
    }

    // Call with the shard's lock held. One search: what is new goes where the search for it has ended.
    // `append` writes the element fully and answers with its position.
    uint32_t findOrAdd(uint32_t hash, NOESCAPE const Invocable<bool(uint32_t)> auto& isWanted, NOESCAPE const Invocable<uint32_t()> auto& append)
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        if (!slots || (static_cast<size_t>(m_size) + 1) * 3 > (slots->mask + 1) * 2)
            slots = grow(slots);
        size_t place = hash & slots->mask;
        while (uint64_t slot = slots->at(place).load(std::memory_order_acquire)) {
            uint32_t position = static_cast<uint32_t>(slot) - 1;
            if (slot >> 32 == hash && isWanted(position))
                return position;
            place = (place + 1) & slots->mask;
        }
        uint32_t position = append();
        slots->at(place).store(static_cast<uint64_t>(hash) << 32 | (position + 1), std::memory_order_release);
        ++m_size;
        return position;
    }

    // Call with the shard's lock held. Behind it addAbsent() may be called that many times. The slots grow once, however many come.
    void makeRoomFor(uint32_t added)
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        size_t numberOfSlots = slots ? slots->mask + 1 : fewestSlots;
        while ((static_cast<size_t>(m_size) + added) * 3 > numberOfSlots * 2)
            numberOfSlots *= 2;
        if (!slots || numberOfSlots > slots->mask + 1)
            growTo(slots, numberOfSlots);
    }

    // Call with the shard's lock held, for what a search would miss. Nobody searches until the element at the position is written.
    void addAbsent(uint32_t hash, uint32_t position)
    {
        put(*m_slots.load(std::memory_order_relaxed), static_cast<uint64_t>(hash) << 32 | (position + 1));
        ++m_size;
    }

    // By one thread, while nobody searches or adds: nobody is left who reads them.
    void freeOutgrownSlots()
    {
        Slots* slots = m_slots.load(std::memory_order_relaxed);
        for (Slots* outgrown = slots ? std::exchange(slots->outgrown, nullptr) : nullptr; outgrown;)
            fastFree(std::exchange(outgrown, outgrown->outgrown));
    }

    // Call with the shard's lock held.
    size_t sizeInBytes() const
    {
        size_t size = 0;
        for (Slots* slots = m_slots.load(std::memory_order_relaxed); slots; slots = slots->outgrown)
            size += sizeof(Slots) + (slots->mask + 1) * sizeof(std::atomic<uint64_t>);
        return size;
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

    static constexpr size_t fewestSlots = 16;

    Slots* grow(Slots* outgrown)
    {
        return growTo(outgrown, outgrown ? (outgrown->mask + 1) * 2 : fewestSlots);
    }

    Slots* growTo(Slots* outgrown, size_t numberOfSlots)
    {
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

// Storage for the words of one shard's lists. A block never moves.
class InternedWordBlocks {
    WTF_MAKE_NONCOPYABLE(InternedWordBlocks);
public:
    InternedWordBlocks() = default;

    ~InternedWordBlocks()
    {
        for (Block* block = m_newest; block;)
            fastFree(std::exchange(block, block->older));
    }

    // Call with the shard's lock held.
    std::span<const uint32_t> keep(std::span<const uint32_t> words)
    {
        if (words.size() > m_free.size())
            addBlock(words.size());
        std::span<uint32_t> kept = m_free.first(words.size());
        m_free = m_free.subspan(words.size());
        std::ranges::copy(words, kept.begin());
        return kept;
    }

    // Call with the shard's lock held.
    size_t sizeInBytes() const { return m_sizeInBytes; }

private:
    // The words follow it in memory.
    struct Block {
        Block* older;
    };
    static_assert(!(sizeof(Block) % alignof(uint32_t)));

    void addBlock(size_t fewestWords)
    {
        constexpr size_t maxWordsPerBlock = 1 << 14;
        size_t numberOfWords = std::max(fewestWords, m_wordsOfNextBlock);
        m_wordsOfNextBlock = std::min(m_wordsOfNextBlock * 2, maxWordsPerBlock);
        size_t bytes = sizeof(Block) + numberOfWords * sizeof(uint32_t);
        Block* block = static_cast<Block*>(fastMalloc(bytes));
        block->older = m_newest;
        m_newest = block;
        m_free = unsafeMakeSpan(reinterpret_cast<uint32_t*>(block + 1), numberOfWords);
        m_sizeInBytes += bytes;
    }

    Block* m_newest { nullptr };
    std::span<uint32_t> m_free;
    size_t m_wordsOfNextBlock { 64 };
    size_t m_sizeInBytes { 0 };
};

template<typename Record>
struct InternedRecords<Record>::Shard {
    InternedPositions positions;
    Lock lock;
    // The table's only member is its array of shards, so the state that all shards share lives in the first one. In the others it stays empty.
    InternedElements<Record> recordsOfAllShards;
};

template<typename Record>
InternedRecords<Record>::InternedRecords()
    : m_shards(std::make_unique<Shard[]>(numberOfShards))
{
}

template<typename Record>
InternedRecords<Record>::~InternedRecords() = default;

template<typename Record>
const Record& InternedRecords<Record>::at(uint32_t position) const
{
    return m_shards[0].recordsOfAllShards.at(position);
}

template<typename Record>
std::optional<uint32_t> InternedRecords<Record>::find(uint32_t hash, const Record& record) const
{
    return m_shards[shardOfInternedHash(hash)].positions.find(hash, [&](uint32_t position) {
        return at(position) == record;
    });
}

template<typename Record>
uint32_t InternedRecords<Record>::findOrAdd(uint32_t hash, const Record& record)
{
    if (std::optional<uint32_t> found = find(hash, record))
        return *found;
    return addAfterMiss(hash, record);
}

template<typename Record>
uint32_t InternedRecords<Record>::addAfterMiss(uint32_t hash, const Record& record)
{
    Shard& shard = m_shards[shardOfInternedHash(hash)];
    Locker locker { shard.lock };
    return shard.positions.findOrAdd(hash, [&](uint32_t position) {
        return at(position) == record;
    }, [&] {
        return m_shards[0].recordsOfAllShards.append(record);
    });
}

template<typename Record>
void InternedRecords<Record>::addAt(uint32_t position, uint32_t hash, const Record& record)
{
    Shard& shard = m_shards[shardOfInternedHash(hash)];
    Locker locker { shard.lock };
    uint32_t positionInTable = shard.positions.findOrAdd(hash, [&](uint32_t candidate) {
        return at(candidate) == record;
    }, [&] {
        m_shards[0].recordsOfAllShards.put(position, record);
        return position;
    });
    RELEASE_ASSERT(positionInTable == position);
}

// How findOrAddInOrder() deals its candidates out to tasks: by their number alone. No answer depends on either.
// One candidate may take a thousand times as long to prepare as another, so a task has few. A place within a share fits a byte.
static constexpr uint32_t internedCandidatesPerShare = 64;
static_assert(internedCandidatesPerShare <= UINT8_MAX);
// One thread is through so few before the others have woken up.
static constexpr uint32_t fewestInternedCandidatesForEveryThread = 2048;

enum class InternedCandidate : uint8_t {
    WantsNoRecord,
    HasHash,
    IsInTable,
    IsNew,
    RepeatsEarlierCandidate,
};

// Equal records have one hash and so one shard. A shard's task goes through the candidates of its shard in ascending order and finds the first of each group of equals.
// Which candidates are new is then known of all, so their positions are counted off in ascending order before one of them is added.
template<typename Record>
void InternedRecords<Record>::findOrAddInOrder(const ParallelLoops& loops, std::span<Record> candidates, NOESCAPE const PrepareCandidate& prepare, NOESCAPE const FinishCandidate& finish)
{
    RELEASE_ASSERT(candidates.size() <= largestIDPosition);
    uint32_t count = static_cast<uint32_t>(candidates.size());
    InternedElements<Record>& records = m_shards[0].recordsOfAllShards;

    if (count < fewestInternedCandidatesForEveryThread || loops.width() == 1) {
        // A loop of one index tells the calling thread its number.
        forEachIndexInParallel(loops, 1, [&](unsigned, unsigned worker) {
            for (uint32_t candidate = 0; candidate < count; ++candidate) {
                std::optional<uint32_t> hash = prepare(candidate, worker);
                if (!hash)
                    continue;
                if (std::optional<uint32_t> found = find(*hash, candidates[candidate])) {
                    finish(candidate, *found, false, worker);
                    continue;
                }
                uint32_t position = records.takePositions(1);
                finish(candidate, position, true, worker);
                addAt(position, *hash, candidates[candidate]);
            }
        });
        return;
    }

    uint32_t sizeAtStart = records.size();
    constexpr uint32_t perShare = internedCandidatesPerShare;
    constexpr size_t placesPerShare = numberOfShards + 1;
    uint32_t numberOfShares = (count + perShare - 1) / perShare;
    // None of these is written by the calling thread: what takes the time is the first touch of each page.
    auto hashes = MallocSpan<uint32_t>::malloc(count * sizeof(uint32_t));
    auto states = MallocSpan<InternedCandidate>::malloc(count * sizeof(InternedCandidate));
    // IsInTable, and IsNew once the positions are counted off: the position. RepeatsEarlierCandidate: that candidate, which IsNew.
    auto answers = MallocSpan<uint32_t>::malloc(count * sizeof(uint32_t));
    // A share has the places of its candidates. There stand those that have a hash, the ones of a shard in a row, ascending. Each is counted from the first candidate of the share, and so is each place.
    auto candidatesByShard = MallocSpan<uint8_t>::malloc(count);
    // [share][shard], and one more for the end.
    auto firstPlaceOfShard = MallocSpan<uint8_t>::malloc(numberOfShares * placesPerShare);

    forEachIndexInParallel(loops, numberOfShares, [&](unsigned share, unsigned worker) {
        uint32_t first = share * perShare;
        uint32_t end = std::min(count, first + perShare);
        std::array<uint8_t, numberOfShards> nextPlaceOfShard { };
        for (uint32_t candidate = first; candidate < end; ++candidate) {
            std::optional<uint32_t> hash = prepare(candidate, worker);
            states[candidate] = hash ? InternedCandidate::HasHash : InternedCandidate::WantsNoRecord;
            if (!hash)
                continue;
            hashes[candidate] = *hash;
            ++nextPlaceOfShard[shardOfInternedHash(*hash)];
        }
        std::span<uint8_t> firstPlaces = firstPlaceOfShard.mutableSpan().subspan(share * placesPerShare, placesPerShare);
        uint8_t place = 0;
        for (uint32_t shard = 0; shard < numberOfShards; ++shard) {
            uint8_t numberInShard = nextPlaceOfShard[shard];
            firstPlaces[shard] = place;
            nextPlaceOfShard[shard] = place;
            place += numberInShard;
        }
        firstPlaces[numberOfShards] = place;
        for (uint32_t candidate = first; candidate < end; ++candidate) {
            if (states[candidate] == InternedCandidate::HasHash)
                candidatesByShard[first + nextPlaceOfShard[shardOfInternedHash(hashes[candidate])]++] = static_cast<uint8_t>(candidate - first);
        }
    });

    auto forEachCandidateOfShard = [&](uint32_t shard, NOESCAPE const Invocable<void(uint32_t)> auto& functor) {
        for (uint32_t share = 0; share < numberOfShares; ++share) {
            size_t places = share * placesPerShare + shard;
            uint32_t first = share * perShare;
            for (uint32_t place = firstPlaceOfShard[places], end = firstPlaceOfShard[places + 1]; place < end; ++place)
                functor(first + candidatesByShard[first + place]);
        }
    };

    std::array<uint32_t, numberOfShards> numberOfNewInShard { };
    forEachIndexInParallel(loops, numberOfShards, [&](unsigned shard) {
        size_t numberOfCandidates = 0;
        for (uint32_t share = 0; share < numberOfShares; ++share)
            numberOfCandidates += firstPlaceOfShard[share * placesPerShare + shard + 1] - firstPlaceOfShard[share * placesPerShare + shard];
        if (!numberOfCandidates)
            return;

        // The new ones of the shard, with open addressing. A slot holds a candidate plus one, and zero while it is free.
        Vector<uint32_t> newCandidates;
        newCandidates.fill(0u, std::bit_ceil(numberOfCandidates * 2));
        size_t mask = newCandidates.size() - 1;
        const InternedPositions& positions = m_shards[shard].positions;
        forEachCandidateOfShard(shard, [&](uint32_t candidate) {
            uint32_t hash = hashes[candidate];
            const Record& record = candidates[candidate];
            std::optional<uint32_t> found = positions.find(hash, [&](uint32_t position) {
                return records.at(position) == record;
            });
            if (found) {
                states[candidate] = InternedCandidate::IsInTable;
                answers[candidate] = *found;
                return;
            }
            size_t slot = hash & mask;
            for (; newCandidates[slot]; slot = (slot + 1) & mask) {
                uint32_t earlier = newCandidates[slot] - 1;
                if (hashes[earlier] == hash && candidates[earlier] == record) {
                    states[candidate] = InternedCandidate::RepeatsEarlierCandidate;
                    answers[candidate] = earlier;
                    return;
                }
            }
            newCandidates[slot] = candidate + 1;
            states[candidate] = InternedCandidate::IsNew;
            ++numberOfNewInShard[shard];
        });
    });

    uint32_t numberOfNew = 0;
    for (uint32_t number : numberOfNewInShard)
        numberOfNew += number;
    uint32_t nextPosition = records.takePositions(numberOfNew);
    // A record that somebody else has added since the start may be one of the candidates, and then the table would hold it twice.
    RELEASE_ASSERT(nextPosition == sizeAtStart);
    uint32_t endOfPositions = nextPosition + numberOfNew;
    for (uint32_t candidate = 0; candidate < count; ++candidate) {
        if (states[candidate] == InternedCandidate::IsNew)
            answers[candidate] = nextPosition++;
    }
    RELEASE_ASSERT(nextPosition == endOfPositions);

    // A shard's slots need the hash and the position alone, so they are filled while the records are written.
    forEachIndexInParallel(loops, numberOfShards + numberOfShares, [&](unsigned index, unsigned worker) {
        if (index < numberOfShards) {
            if (!numberOfNewInShard[index])
                return;
            Shard& shard = m_shards[index];
            Locker locker { shard.lock };
            shard.positions.makeRoomFor(numberOfNewInShard[index]);
            forEachCandidateOfShard(index, [&](uint32_t candidate) {
                if (states[candidate] == InternedCandidate::IsNew)
                    shard.positions.addAbsent(hashes[candidate], answers[candidate]);
            });
            return;
        }

        uint32_t first = (index - numberOfShards) * perShare;
        uint32_t end = std::min(count, first + perShare);
        for (uint32_t candidate = first; candidate < end; ++candidate) {
            switch (states[candidate]) {
            case InternedCandidate::WantsNoRecord:
                break;
            case InternedCandidate::IsInTable:
                finish(candidate, answers[candidate], false, worker);
                break;
            case InternedCandidate::RepeatsEarlierCandidate:
                finish(candidate, answers[answers[candidate]], false, worker);
                break;
            case InternedCandidate::IsNew:
                finish(candidate, answers[candidate], true, worker);
                records.put(answers[candidate], candidates[candidate]);
                break;
            case InternedCandidate::HasHash:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    });
    RELEASE_ASSERT(records.size() == endOfPositions);
}

template<typename Record>
void InternedRecords<Record>::freeOutgrownSlots()
{
    for (uint32_t index = 0; index < numberOfShards; ++index)
        m_shards[index].positions.freeOutgrownSlots();
}

template<typename Record>
uint32_t InternedRecords<Record>::size() const
{
    return m_shards[0].recordsOfAllShards.size();
}

template<typename Record>
size_t InternedRecords<Record>::sizeInBytes() const
{
    size_t size = numberOfShards * sizeof(Shard) + m_shards[0].recordsOfAllShards.sizeInBytes();
    for (uint32_t index = 0; index < numberOfShards; ++index) {
        Shard& shard = m_shards[index];
        Locker locker { shard.lock };
        size += shard.positions.sizeInBytes();
    }
    return size;
}

struct InternedWordLists::Shard {
    InternedPositions positions;
    InternedWordBlocks words;
    Lock lock;
    // As in InternedRecords: only the first shard's is used.
    InternedElements<std::span<const uint32_t>> listsOfAllShards;
};

InternedWordLists::InternedWordLists()
    : m_shards(std::make_unique<Shard[]>(numberOfShardsOfInternedTable))
{
}

InternedWordLists::~InternedWordLists() = default;

std::span<const uint32_t> InternedWordLists::at(uint32_t position) const
{
    return m_shards[0].listsOfAllShards.at(position);
}

std::optional<uint32_t> InternedWordLists::find(uint32_t hash, std::span<const uint32_t> words) const
{
    return m_shards[shardOfInternedHash(hash)].positions.find(hash, [&](uint32_t position) {
        return std::ranges::equal(at(position), words);
    });
}

uint32_t InternedWordLists::findOrAdd(uint32_t hash, std::span<const uint32_t> words)
{
    if (std::optional<uint32_t> found = find(hash, words))
        return *found;

    Shard& shard = m_shards[shardOfInternedHash(hash)];
    Locker locker { shard.lock };
    return shard.positions.findOrAdd(hash, [&](uint32_t position) {
        return std::ranges::equal(at(position), words);
    }, [&] {
        return m_shards[0].listsOfAllShards.append(shard.words.keep(words));
    });
}

void InternedWordLists::freeOutgrownSlots()
{
    for (uint32_t index = 0; index < numberOfShardsOfInternedTable; ++index)
        m_shards[index].positions.freeOutgrownSlots();
}

size_t InternedWordLists::sizeInBytes() const
{
    size_t size = numberOfShardsOfInternedTable * sizeof(Shard) + m_shards[0].listsOfAllShards.sizeInBytes();
    for (uint32_t index = 0; index < numberOfShardsOfInternedTable; ++index) {
        Shard& shard = m_shards[index];
        Locker locker { shard.lock };
        size += shard.positions.sizeInBytes() + shard.words.sizeInBytes();
    }
    return size;
}

TypeTable::TypeTable(const BuiltinTable& builtins, const Program* program)
    : m_builtins(builtins)
    , m_program(program)
{
    m_partOfClassAlone.reserveInitialCapacity(builtins.classCount());
    for (uint32_t index = 0; index < builtins.classCount(); ++index)
        m_partOfClassAlone.append(ClassPart { BuiltinClassID(index), { } });

    // These are in the canonical form that TypeArena::intern() produces, so the same parts interned there find these records.
    auto internAtStart = [&](TypeID expected, uint16_t primitives, bool anyObject) {
        Type type;
        type.primitives = primitives;
        type.integers = primitives & Primitive::Integer ? Range::full() : Range::empty();
        type.anyObject = anyObject;
        type.anyFunction = true;
        uint32_t position = m_types.findOrAdd(hashOf(type), type);
        RELEASE_ASSERT(idAt<TypeID>(IDHome::Program, position) == expected);
    };
    constexpr uint16_t primitivesThatAreCells = cellPrimitives & ~primitivesWithLists;
    internAtStart(function, 0, false);
    internAtStart(object, 0, true);
    internAtStart(anything, Primitive::All, true);
    internAtStart(anythingOrAbsent, Primitive::All | Primitive::Absent, true);
    internAtStart(cell, primitivesThatAreCells, true);
    internAtStart(cellOrMarker, primitivesThatAreCells | Primitive::UndefinedOrNull | Primitive::Absent, true);

    auto internRangeAtStart = [&](TypeID expected, Range integers) {
        Type type;
        type.primitives = Primitive::Integer;
        type.integers = integers;
        uint32_t position = m_types.findOrAdd(hashOf(type), type);
        RELEASE_ASSERT(idAt<TypeID>(IDHome::Program, position) == expected);
    };
    internRangeAtStart(int16, { INT16_MIN, INT16_MAX });
    internRangeAtStart(uint16, { 0, UINT16_MAX });
    RELEASE_ASSERT(m_types.size() == numberOfTypesInternedAtStart);
}

TypeTable::~TypeTable() = default;

void TypeTable::setObjects(std::unique_ptr<ObjectTable>&& objects)
{
    // The summaries attached to interned object sets were computed from the first table's records, so it is never replaced.
    RELEASE_ASSERT(objects && !m_objects);
    m_objects = WTF::move(objects);
}

std::span<const ClassPart> TypeTable::at(ClassPartsID id) const
{
    if (!id)
        return { };

    switch (homeOf(id)) {
    case IDHome::Value:
        return std::span<const ClassPart>(&m_partOfClassAlone[positionOf(id)], 1);
    case IDHome::Program: {
        std::span<const uint32_t> words = m_classParts.at(positionOf(id));
        return unsafeMakeSpan(reinterpret_cast<const ClassPart*>(words.data()), words.size() / wordsPerClassPartInTypeTable);
    }
    case IDHome::SmallRange:
    case IDHome::Thread:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const PropertyKey& TypeTable::at(PropertyKeyID key) const
{
    // The id of a key in the table is its bare position, so that it can be a member of a set.
    RELEASE_ASSERT(homeOf(key) == IDHome::Value);
    return m_propertyKeys.at(positionOf(key));
}

unsigned TypeTable::numberOfPropertyKeys() const
{
    return m_propertyKeys.size();
}

void TypeTable::endStableNumbersOfPropertyKeys()
{
    endStableNumbersOfPropertyKeys(ParallelLoops(1));
}

void TypeTable::endStableNumbersOfPropertyKeys(const ParallelLoops& loops)
{
    bool isFirstCall = m_numberOfKeysWithStableNumbers == UINT32_MAX;
    // It never rises: a key that a thread added after the first call never gets a stable number.
    m_numberOfKeysWithStableNumbers = std::min(m_numberOfKeysWithStableNumbers, m_propertyKeys.size());
    if (!isFirstCall || !m_program)
        return;

    m_locationBitsOfKeys.grow(m_numberOfKeysWithStableNumbers);
    forEachRangeOfIndicesInParallel(loops, m_numberOfKeysWithStableNumbers, [&](size_t first, size_t end) {
        for (size_t position = first; position < end; ++position)
            m_locationBitsOfKeys[position] = locationBitOfKeyInTypeTable(*m_program, m_propertyKeys.at(static_cast<uint32_t>(position)));
    });
}

// One thread alone would miss the cache at the slot and at the record of each of half a million texts, one after the other.
void TypeTable::addKeysOfTexts(uint32_t numberOfStrings, const ParallelLoops& loops)
{
    if (!numberOfStrings)
        return;
    uint32_t numberOfKeysInFront = m_propertyKeys.size();
    auto keys = MallocSpan<PropertyKey>::malloc(numberOfStrings * sizeof(PropertyKey));
    m_propertyKeys.findOrAddInOrder(loops, keys.mutableSpan(), [&](uint32_t text, unsigned) -> std::optional<uint32_t> {
        new (NotNull, &keys[text]) PropertyKey { .kind = PropertyKeyKind::Text, .text = StringID(text) };
        return hashOf(keys[text]);
    }, [](uint32_t, uint32_t, bool, unsigned) { });
    RELEASE_ASSERT(!m_isFrozen || m_propertyKeys.size() == numberOfKeysInFront);
}

std::optional<PropertyKeyID> TypeTable::find(const PropertyKey& key) const
{
    std::optional<uint32_t> position = m_propertyKeys.find(hashOf(key), key);
    if (!position)
        return std::nullopt;
    return idAt<PropertyKeyID>(IDHome::Value, *position);
}

ProvenInEveryVersion TypeTable::provenTypeOf(const DeclaredType& declared) const
{
    if (std::optional<ProvenInEveryVersion> established = declared.established())
        return *established;
    // Only the compiler stores something that is not a JavaScript value into a position, and only the compiler leaves a position empty.
    TypeID predicted = declared.expected().predicted();
    Type expected = at(predicted);
    if (!expected.isJavaScriptValue())
        return Proven::byPosition(predicted);
    return Proven::byPosition(expected.primitives & Primitive::Absent ? anythingOrAbsent : anything);
}

void TypeTable::beginKeepingMemosAtRest()
{
    Locker locker { m_memosAtRestLock };
    m_keepsMemosAtRest = true;
}

void TypeTable::invalidateMemosAtRest()
{
    Locker locker { m_memosAtRestLock };
    // What is remembered of sets follows from their members alone.
    for (MemosOfArena& memos : m_memosAtRest) {
        memos.types.clear();
        memos.unionsOfMany.clear();
    }
}

void TypeTable::freeze()
{
    {
        // From here on arenas keep what they make until reset(), and each has a memo for as long as it lives.
        Locker locker { m_memosAtRestLock };
        m_keepsMemosAtRest = false;
        m_memosAtRest.clear();
    }
    m_isFrozen = true;
    // Every String constant of a body asks for the type of its text.
    for (uint32_t position = 0, end = m_types.size(); position < end; ++position) {
        const Type& type = m_types.at(position);
        if (type.primitives != Primitive::LiteralString || !type.stringLiterals || !holdsOneMember(type.stringLiterals))
            continue;
        TypeParts ofOneText;
        ofOneText.primitives = Primitive::LiteralString;
        ofOneText.stringLiterals = type.stringLiterals;
        if (static_cast<const TypeParts&>(type) != ofOneText)
            continue;
        uint32_t string = positionOf(type.stringLiterals);
        if (string >= m_typesOfLiteralStrings.size())
            m_typesOfLiteralStrings.grow(string + 1);
        m_typesOfLiteralStrings[string] = idAt<TypeID>(IDHome::Program, position);
    }
    m_types.freeOutgrownSlots();
    m_typesOnceFrozen = Vector<const Type*>(m_types.size(), [&](size_t position) {
        return &m_types.at(static_cast<uint32_t>(position));
    });
    m_classParts.freeOutgrownSlots();
    m_propertyKeys.freeOutgrownSlots();
    m_layoutSets.freeze();
    m_objectSets.freeze();
    m_stringSets.freeze();
    m_keySets.freeze();
    m_sharedAnswers = makeUniqueWithoutFastMallocCheck<SharedAnswers>();
}

size_t TypeTable::sizeInBytes() const
{
    size_t size = sizeof(TypeTable) + m_partOfClassAlone.capacity() * sizeof(ClassPart) + (m_typeOfWireIndex.capacity() + m_typesOfLiteralStrings.capacity()) * sizeof(TypeID) + m_locationBitsOfKeys.capacity() * sizeof(uint8_t);
    if (m_objects)
        size += sizeof(ObjectTable) + static_cast<size_t>(m_objects->size()) * sizeof(ObjectRecord);
    size += m_types.sizeInBytes() + m_typesOnceFrozen.capacity() * sizeof(const Type*) + m_classParts.sizeInBytes() + m_propertyKeys.sizeInBytes();
    size += m_layoutSets.sizeInBytes() + m_objectSets.sizeInBytes() + m_stringSets.sizeInBytes() + m_keySets.sizeInBytes();
    if (m_sharedAnswers)
        size += m_sharedAnswers->sizeInBytes();
    return size;
}

template class InternedRecords<Type>;
template class InternedRecords<PropertyKey>;

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
