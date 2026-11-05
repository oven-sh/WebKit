/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class IDSetArena;
class IDSetTable;

// Interned sets of 32-bit numbers. Equal sets have equal ids within one table and one arena over it.
// A set of at most IDSetTable::mostMembersOfList members is a sorted list. A larger set is a sorted list of chunks of membersPerSetChunk numbers.
// Chunks are interned too, so two large sets that differ in a few members share the rest, and a union skips every chunk that both hold and reads a chunk that many of its sets hold once.
// Every set repeats its list of chunks and shares the chunks themselves, so the lists are what large sets cost: a chunk is large enough to keep them short.
enum class IDSetForm : uint8_t {
    ListsAndChunks,
    // The same, and listOf() hands out the members of any set as one sorted list that lasts as long as the set. The list of a set with chunks is made when it is first asked for.
    ChunksWithLazyList,
};
enum class IDSetOperation : uint8_t { Union, Intersection, Difference };

inline constexpr uint32_t membersPerSetChunk = 2048;

struct IDSetChunk {
    std::array<uint64_t, membersPerSetChunk / 64> bits { };

    friend constexpr bool operator==(const IDSetChunk&, const IDSetChunk&) = default;
};

struct IDSetChunkEntry {
    uint32_t firstMemberDividedBySize;
    // Encoded like a set id: IDHome::Program or IDHome::Thread and a position.
    uint32_t chunk;
};

// In memory: the header, the attached words, then `size` members or `numberOfChunks` entries.
// The members or entries of a long set lie outside the table: the header, a pointer to them, then the attached words.
struct IDSetHeader {
    uint32_t size;
    uint32_t numberOfChunks;
};

// A set to read. It stays valid as long as its record: for ever for a record of the table, until reset() for one of an arena.
class IDSetView {
public:
    IDSetView() = default;

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    bool isList() const { return !m_numberOfChunks; }
    std::span<const uint32_t> list() const LIFETIME_BOUND
    {
        RELEASE_ASSERT(isList());
        return m_size == 1 && !m_words ? std::span<const uint32_t>(&m_onlyMember, 1) : unsafeMakeSpan(m_words, m_size);
    }
    std::span<const uint32_t> attachedWords() const { return unsafeMakeSpan(m_attachedWords, m_numberOfAttachedWords); }

    JS_EXPORT_PRIVATE bool contains(uint32_t member) const;
    JS_EXPORT_PRIVATE uint32_t first() const;
    JS_EXPORT_PRIVATE uint32_t last() const;

    // In ascending order.
    void forEach(NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        if (isList()) {
            for (uint32_t member : list())
                functor(member);
            return;
        }
        for (const IDSetChunkEntry& entry : chunkEntries()) {
            const IDSetChunk& chunk = chunkAt(entry.chunk);
            for (uint32_t word = 0; word < chunk.bits.size(); ++word) {
                for (uint64_t bits = chunk.bits[word]; bits; bits &= bits - 1)
                    functor(entry.firstMemberDividedBySize * membersPerSetChunk + word * 64 + std::countr_zero(bits));
            }
        }
    }

    // In ascending order, up to the first match.
    bool anyOf(NOESCAPE const Invocable<bool(uint32_t)> auto& matches) const
    {
        if (isList())
            return std::ranges::any_of(list(), matches);
        for (const IDSetChunkEntry& entry : chunkEntries()) {
            const IDSetChunk& chunk = chunkAt(entry.chunk);
            for (uint32_t word = 0; word < chunk.bits.size(); ++word) {
                for (uint64_t bits = chunk.bits[word]; bits; bits &= bits - 1) {
                    if (matches(entry.firstMemberDividedBySize * membersPerSetChunk + word * 64 + std::countr_zero(bits)))
                        return true;
                }
            }
        }
        return false;
    }

    // The same over the members from `begin` up to and not including `end`. Chunks outside the stretch are not read.
    bool anyOfWithin(uint32_t begin, uint32_t end, NOESCAPE const Invocable<bool(uint32_t)> auto& matches) const
    {
        if (isList()) {
            std::span<const uint32_t> members = list();
            for (uint32_t member : members.subspan(std::ranges::lower_bound(members, begin) - members.begin())) {
                if (member >= end)
                    return false;
                if (matches(member))
                    return true;
            }
            return false;
        }
        std::span<const IDSetChunkEntry> entries = chunkEntries();
        size_t firstEntry = std::ranges::lower_bound(entries, begin / membersPerSetChunk, { }, &IDSetChunkEntry::firstMemberDividedBySize) - entries.begin();
        for (const IDSetChunkEntry& entry : entries.subspan(firstEntry)) {
            if (entry.firstMemberDividedBySize * membersPerSetChunk >= end)
                return false;
            const IDSetChunk& chunk = chunkAt(entry.chunk);
            for (uint32_t word = 0; word < chunk.bits.size(); ++word) {
                for (uint64_t bits = chunk.bits[word]; bits; bits &= bits - 1) {
                    uint32_t member = entry.firstMemberDividedBySize * membersPerSetChunk + word * 64 + std::countr_zero(bits);
                    if (member >= end)
                        return false;
                    if (member >= begin && matches(member))
                        return true;
                }
            }
        }
        return false;
    }

    bool allOf(NOESCAPE const Invocable<bool(uint32_t)> auto& matches) const
    {
        return !anyOf([&](uint32_t member) {
            return !matches(member);
        });
    }

    // 64 members at a time, in ascending order, up to the first match: their numbers divided by 64, and a bit for each of them that the set holds, which is one at least.
    // For a question whose answers for all numbers are laid out 64 a word: a large set then costs a step for 64 members.
    bool anyWordOf(NOESCAPE const Invocable<bool(uint32_t word, uint64_t members)> auto& matches) const
    {
        if (isList()) {
            std::span<const uint32_t> members = list();
            for (size_t index = 0; index < members.size();) {
                uint32_t word = members[index] / 64;
                uint64_t membersInWord = 0;
                for (; index < members.size() && members[index] / 64 == word; ++index)
                    membersInWord |= 1ull << members[index] % 64;
                if (matches(word, membersInWord))
                    return true;
            }
            return false;
        }
        for (const IDSetChunkEntry& entry : chunkEntries()) {
            const IDSetChunk& chunk = chunkAt(entry.chunk);
            for (uint32_t word = 0; word < chunk.bits.size(); ++word) {
                if (chunk.bits[word] && matches(entry.firstMemberDividedBySize * (membersPerSetChunk / 64) + word, chunk.bits[word]))
                    return true;
            }
        }
        return false;
    }

    std::span<const IDSetChunkEntry> chunkEntries() const { return unsafeMakeSpan(reinterpret_cast<const IDSetChunkEntry*>(m_words), m_numberOfChunks); }
    JS_EXPORT_PRIVATE const IDSetChunk& chunkAt(uint32_t encodedChunk) const;

private:
    friend class IDSetArena;
    friend class IDSetTable;

    const uint32_t* m_words { nullptr };
    const uint32_t* m_attachedWords { nullptr };
    uint32_t m_size { 0 };
    uint32_t m_numberOfChunks { 0 };
    uint32_t m_numberOfAttachedWords { 0 };
    uint32_t m_onlyMember { 0 };
    const IDSetTable* m_table { nullptr };
    const IDSetArena* m_arena { nullptr };
};

// The members of `larger` that `smaller` lacks, in ascending order. Chunks that both hold are skipped unread.
// This is how a rule of the solver sees each new member once.
JS_EXPORT_PRIVATE void forEachAddedMember(const IDSetView& larger, const IDSetView& smaller, NOESCAPE const ScopedLambda<void(uint32_t)>&);
JS_EXPORT_PRIVATE bool isSubset(const IDSetView&, const IDSetView& of);
JS_EXPORT_PRIVATE bool overlap(const IDSetView&, const IDSetView&);
// By content, so the same at any number of threads.
JS_EXPORT_PRIVATE std::strong_ordering compareByContent(const IDSetView&, const IDSetView&);

// Whoever keeps words beside each set of several members says here what they are. Called before the set is interned, with the operands where the set
// is the result of an operation on sets, so that the words of a union come from the words of its operands.
// The members may lie in the scratch of the arena that calls, which moves when it grows. So annotate() at most reads that arena: an operation that lays anything out in scratch meanwhile stops the build.
class IDSetAnnotator {
public:
    virtual ~IDSetAnnotator() = default;
    virtual void annotate(const IDSetView& membersWithoutAttachedWords, std::optional<IDSetOperation>, std::span<const uint32_t> operands, std::span<uint32_t> attachedWords) = 0;
};

// Answers by question and pair of ids, for ids that last as long as the table. One thread at a time.
// Two generations. An answer found in the older one moves to the recent one. Once the recent one holds four times what has moved into it, the older one is forgotten and the recent one takes its place.
class IDPairMemo {
public:
    static constexpr size_t leastAnswersInGeneration = 1 << 14;

    JS_EXPORT_PRIVATE std::optional<uint32_t> find(uint32_t question, uint32_t first, uint32_t second);
    JS_EXPORT_PRIVATE void add(uint32_t question, uint32_t first, uint32_t second, uint32_t answer);
    // Where the values asked about no longer change: nothing is forgotten from here on.
    void keepEverything() { m_keepsEverything = true; }
    void clear()
    {
        m_recent = { };
        m_older = { };
        m_numberMoved = 0;
    }
    size_t sizeInBytes() const { return (m_recent.slots.capacity() + m_older.slots.capacity()) * sizeof(Entry); }

private:
    struct Entry {
        uint32_t first { 0 };
        uint32_t second { 0 };
        uint32_t answer { 0 };
        // 0 in a free slot.
        uint32_t questionPlusOne { 0 };
    };

    struct Generation {
        const Entry* find(uint32_t questionPlusOne, uint32_t first, uint32_t second) const;
        void add(const Entry&);
        void grow();

        // Open addressing. Empty, or a power of two that is at most half full.
        Vector<Entry> slots;
        size_t size { 0 };
    };

    Generation m_recent;
    Generation m_older;
    size_t m_numberMoved { 0 };
    bool m_keepsEverything { false };
#if ASSERT_ENABLED || ASAN_ENABLED
    unsigned m_numberOfHits { 0 };
#endif
};

// The same by a list of ids. Its generations are measured in ids.
class IDListMemo {
public:
    static constexpr size_t leastIDsInGeneration = 1 << 16;

    JS_EXPORT_PRIVATE std::optional<uint32_t> find(std::span<const uint32_t>);
    JS_EXPORT_PRIVATE void add(std::span<const uint32_t>, uint32_t answer);
    void keepEverything() { m_keepsEverything = true; }
    void clear()
    {
        m_recent = { };
        m_older = { };
        m_numberOfIDsMoved = 0;
    }
    size_t sizeInBytes() const { return (m_recent.slots.capacity() + m_older.slots.capacity()) * sizeof(Entry) + (m_recent.ids.capacity() + m_older.ids.capacity()) * sizeof(uint32_t); }

private:
    struct Entry {
        uint32_t firstID { 0 };
        // 0 in a free slot.
        uint32_t numberOfIDs { 0 };
        uint32_t answer { 0 };
        uint32_t hash { 0 };
    };

    struct Generation {
        const Entry* find(uint32_t hash, std::span<const uint32_t>) const;
        void add(uint32_t hash, std::span<const uint32_t>, uint32_t answer);
        void grow();

        Vector<Entry> slots;
        Vector<uint32_t> ids;
        size_t size { 0 };
    };

    Generation m_recent;
    Generation m_older;
    size_t m_numberOfIDsMoved { 0 };
    bool m_keepsEverything { false };
#if ASSERT_ENABLED || ASAN_ENABLED
    unsigned m_numberOfHits { 0 };
#endif
};

// The sets of the program. While it grows any thread may add through its own arena. Once frozen nobody adds and reads take no lock.
// A record never moves, so a thread that made an id or a view, or was handed one with synchronization, reads it without a lock at any time.
class IDSetTable {
    WTF_MAKE_NONCOPYABLE(IDSetTable);
public:
    static constexpr uint32_t mostMembersOfList = 16;
    static constexpr uint32_t numberOfShards = 64;

    JS_EXPORT_PRIVATE IDSetTable(IDSetForm, uint32_t numberOfAttachedWords);
    JS_EXPORT_PRIVATE ~IDSetTable();

    uint32_t numberOfAttachedWords() const { return m_numberOfAttachedWords; }

    // IDSetForm::ChunksWithLazyList: the list of a set of one member outlives the view, whenever it is asked for.
    // For a number below `size` it points into a list of all those numbers, which costs nothing. One thread, once: a second call changes nothing.
    JS_EXPORT_PRIVATE void reserveMembers(uint32_t size);

    // For the null id, a set of one member and a record of the table.
    JS_EXPORT_PRIVATE IDSetView viewOf(uint32_t set) const;
    // IDSetForm::ChunksWithLazyList. For the same ids. Ascending. Any thread, while the table grows and once it is frozen: a list is no set, so making one adds no id.
    JS_EXPORT_PRIVATE std::span<const uint32_t> listOf(uint32_t set) const;

    // By one thread, while no other thread is at the table.
    void freeze()
    {
        m_isFrozen = true;
        freeOutgrownSlots();
    }
    bool isFrozen() const { return m_isFrozen; }
    // A ROUND IN WHICH NOBODY ADDS, on a table that goes on growing behind it: TypeTable::RoundWithoutAdditions. To an arena that keeps what it makes to itself the table is as good as frozen. Whoever adds meanwhile stops the build.
    void beginRoundWithoutAdditions() { m_numberOfRoundsWithoutAdditions.fetch_add(1, std::memory_order_relaxed); }
    void endRoundWithoutAdditions() { m_numberOfRoundsWithoutAdditions.fetch_sub(1, std::memory_order_relaxed); }
    bool takesNoAdditions() const { return m_isFrozen || m_numberOfRoundsWithoutAdditions.load(std::memory_order_relaxed); }
    JS_EXPORT_PRIVATE size_t sizeInBytes() const;

    // For a log of what a stage holds.
    struct Contents {
        uint64_t numberOfSets { 0 };
        uint64_t numberOfChunks { 0 };
        uint64_t bytesOfRecords { 0 };
        // The members or entries of long sets, and the lazy lists.
        uint64_t bytesOutsideRecords { 0 };
        uint64_t bytesOfIndexes { 0 };
    };
    JS_EXPORT_PRIVATE Contents contents() const;

private:
    friend class IDSetArena;
    friend class IDSetView;

    struct Shard;

    // All four answer ids: IDHome::Program and a position. The hash is the arena's and is kept beside the id. Neither find takes a lock, also while the table grows.
    // findOrAdd() stops the build on a frozen table, a set of fewer than two members, a set in the wrong form for its size, and an entry that names a chunk of a thread.
    // findOrAddChunk() stops it on a frozen table and a chunk without members.
    std::optional<uint32_t> find(uint32_t hash, const IDSetView& members) const;
    uint32_t findOrAdd(uint32_t hash, const IDSetView& members, std::span<const uint32_t> attachedWords, bool& isNew);
    std::optional<uint32_t> findChunk(uint32_t hash, const IDSetChunk&) const;
    uint32_t findOrAddChunk(uint32_t hash, const IDSetChunk&, bool& isNew);
    JS_EXPORT_PRIVATE void freeOutgrownSlots();

    const IDSetForm m_form;
    const uint32_t m_numberOfAttachedWords;
    bool m_isFrozen { false };
    std::atomic<unsigned> m_numberOfRoundsWithoutAdditions { 0 };
    Vector<uint32_t> m_allMembers;
    std::unique_ptr<Shard[]> m_shards;
};

enum class ArenaKeeps : uint8_t {
    // What is made goes into the program's table: the reader, Facts, Plan.
    InProgram,
    // What the frozen table lacks lies in the arena until reset(): Compile.
    UntilReset,
};

// One thread's door to a table: it makes sets and remembers results by pair of ids.
// What it remembers of the program's records outlives reset(): IDPairMemo. What it remembers of its own lies in a table of fixed size that forgets by overwriting.
class IDSetArena {
    WTF_MAKE_NONCOPYABLE(IDSetArena);
public:
    JS_EXPORT_PRIVATE IDSetArena(IDSetTable&, ArenaKeeps, IDSetAnnotator* = nullptr);
    JS_EXPORT_PRIVATE ~IDSetArena();

    JS_EXPORT_PRIVATE void reset();

    IDSetTable& table() { return m_table; }
    const IDSetTable& table() const { return m_table; }
    // What this arena was the first to add to the program's table, by the operation that made it. The last place is for what no operation on two sets made.
    struct Additions {
        static constexpr size_t numberOfMakers = 4;

        std::array<uint64_t, numberOfMakers> sets { };
        std::array<uint64_t, numberOfMakers> wordsOfSets { };
        std::array<uint64_t, numberOfMakers> chunks { };
    };
    const Additions& additions() const { return m_additions; }
    // The sets and chunks that lie in the thread: what the program's table lacks. It grows until reset().
    size_t numberMadeAndKeptToItself() const { return m_firstWordOfRecord.size() + m_chunks.size(); }

    JS_EXPORT_PRIVATE IDSetView viewOf(uint32_t set) const;
    // IDSetTable::listOf(), and the same for a set of the arena, whose list lasts until reset().
    JS_EXPORT_PRIVATE std::span<const uint32_t> listOf(uint32_t set);

    // What is remembered of the program's records holds for every arena over the table, so an arena that ends may hand it to the next.
    IDPairMemo takeLastingMemo() { return std::exchange(m_lastingMemo, { }); }
    void setLastingMemo(IDPairMemo&& memo) { m_lastingMemo = WTF::move(memo); }
    IDPairMemo& lastingMemo() { return m_lastingMemo; }
    const IDPairMemo& lastingMemo() const { return m_lastingMemo; }
    size_t sizeOfMemosInBytes() const
    {
        return m_lastingMemo.sizeInBytes() + m_memoUntilReset.capacity() * sizeof(MemoEntry) + m_earlierStatesCompared.capacity() * sizeof(EntriesSinceEarlierState)
            + m_entriesSinceEarlierStates.capacity() * sizeof(IDSetChunkEntry) + m_indexOfEarlierStatesCompared.capacity() * sizeof(uint32_t);
    }
    // It is empty between two operations and keeps its capacity as long as the arena lives.
    size_t sizeOfScratchInBytes() const { return m_scratch.capacity() * sizeof(uint32_t) + m_scratchEntries.capacity() * sizeof(IDSetChunkEntry) + m_unionScratch.sizeInBytes(); }

    JS_EXPORT_PRIVATE uint32_t makeFromSorted(std::span<const uint32_t> ascendingWithoutRepeats);
    JS_EXPORT_PRIVATE uint32_t makeFromUnsorted(std::span<const uint32_t>);
    JS_EXPORT_PRIVATE uint32_t with(uint32_t set, uint32_t member);
    JS_EXPORT_PRIVATE uint32_t without(uint32_t set, uint32_t member);
    JS_EXPORT_PRIVATE uint32_t unionOf(uint32_t, uint32_t);
    // Of any number of sets. The result alone is interned: what the sets make on the way to it lies in scratch.
    uint32_t unionOf(std::span<const uint32_t> sets) { return unionOf(sets, { }); }
    // The same for sets that have grown. earlierStates is empty or has one entry a set: a subset of that set which the union holds without it, since others of the sets hold it. The empty set says nothing.
    // A chunk that a set shares with its earlier state is passed over unread, so what a set has gained need not be made into a set of its own.
    JS_EXPORT_PRIVATE uint32_t unionOf(std::span<const uint32_t> sets, std::span<const uint32_t> earlierStates);
    // In which chunks a set differs from its earlier state is worked out once and kept until here.
    JS_EXPORT_PRIVATE void forgetEarlierStates();
    JS_EXPORT_PRIVATE uint32_t intersectionOf(uint32_t, uint32_t);
    JS_EXPORT_PRIVATE uint32_t differenceOf(uint32_t, uint32_t);
    JS_EXPORT_PRIVATE bool isSubset(uint32_t set, uint32_t of);
    JS_EXPORT_PRIVATE bool overlap(uint32_t, uint32_t);
    JS_EXPORT_PRIVATE uint32_t filtered(uint32_t set, NOESCAPE const ScopedLambda<bool(uint32_t)>& keeps);
    // The functor answers the member to put in place of the one it is given. The result is sorted anew.
    JS_EXPORT_PRIVATE uint32_t replaced(uint32_t set, NOESCAPE const ScopedLambda<uint32_t(uint32_t)>& replacement);

    // A set of the thread that is longer than a chunk is mostly what a difference, an intersection, a filter or a union has left of a set of the program that is as long.
    // The arena notes that set beside the new one until reset(), and what is made of a noted set inherits the note. Of such a set of the program the answer is the set itself. Empty: nothing is noted.
    // The note says where most members come from and proves nothing: the set may hold members that the noted set lacks.
    JS_EXPORT_PRIVATE uint32_t setOfProgramBehind(uint32_t set) const;
    // For whoever has made `set` out of `madeFrom` in a way of its own.
    JS_EXPORT_PRIVATE void noteSetOfProgramBehind(uint32_t set, uint32_t madeFrom);

private:
    friend class IDSetView;

    // By record, as far as anybody has noted.
    Vector<uint32_t> m_setOfProgramBehindRecord;

    void annotateChunkByChunk(const IDSetView& members, std::span<uint32_t> attachedWords);
    uint32_t setOfMembersInChunk(IDSetChunkEntry);

    struct MemoEntry {
        uint32_t first { UINT32_MAX };
        uint32_t second { UINT32_MAX };
        uint32_t result { UINT32_MAX };
        uint32_t operationAndEpoch { 0 };
    };

    enum class Question : uint8_t { Union, Intersection, Difference, Subset, Overlap };

    // What a union of sets with chunks lays out on its way. All empty between two unions.
    struct UnionScratch {
        size_t sizeInBytes() const
        {
            return chunks.capacity() * sizeof(IDSetChunk)
                + (candidateChunks.capacity() + sortBuffer.capacity() + chunksNotInLargest.capacity() + changedEntries.capacity() + resultEntries.capacity()) * sizeof(IDSetChunkEntry)
                + (lastCandidateByEntry.capacity() + offsetsByChunkNumber.capacity() + operandsWithCandidates.capacity()) * sizeof(uint32_t);
        }

        Vector<IDSetChunk> chunks;
        Vector<IDSetChunkEntry> candidateChunks;
        Vector<uint32_t> lastCandidateByEntry;
        Vector<IDSetChunkEntry> sortBuffer;
        Vector<uint32_t> offsetsByChunkNumber;
        Vector<uint32_t> operandsWithCandidates;
        Vector<IDSetChunkEntry> chunksNotInLargest;
        Vector<IDSetChunkEntry> changedEntries;
        Vector<IDSetChunkEntry> resultEntries;
    };

    // Of a set with chunks and an earlier state with chunks.
    struct EntriesSinceEarlierState {
        uint32_t set;
        uint32_t earlierState;
        uint32_t first;
        uint32_t count;
    };

    uint32_t unionOfDistinct(std::span<const uint32_t> operands, std::span<const uint32_t> earlierStates);
    void collectCandidateChunks(UnionScratch&, uint32_t largest, std::span<const IDSetChunkEntry> entriesOfLargest, std::span<const uint32_t> wideOperands, std::span<const uint32_t> earlierStatesOfWideOperands, const IDSetView& listedMembers, bool memoizesSubsets);
    // The entries of the set that its earlier state lacks. Until the next call.
    std::span<const IDSetChunkEntry> entriesSince(uint32_t set, uint32_t earlierState);
    uint32_t uniteCandidateChunks(UnionScratch&, std::span<const IDSetChunkEntry> entriesOfLargest, bool memoizesSubsets) const;
    uint32_t internUnion(UnionScratch&, std::span<const IDSetChunkEntry> entriesOfLargest, uint32_t size, std::span<const uint32_t> operands);
    bool isRememberedSubset(uint32_t set, uint32_t of);
    void rememberSubset(uint32_t set, uint32_t of);
    uint32_t intern(const IDSetView& members, std::optional<IDSetOperation>, std::span<const uint32_t> operands);
    uint32_t intern(const IDSetView& members, std::optional<IDSetOperation>, uint32_t firstOperand, uint32_t secondOperand);
    uint32_t internChunk(const IDSetChunk&, std::optional<IDSetOperation>);
    uint32_t compute(IDSetOperation, uint32_t, uint32_t);
    IDSetView viewOfList(std::span<const uint32_t> ascendingWithoutRepeats) const;
    IDSetView viewOfEntries(std::span<const IDSetChunkEntry>, uint32_t size) const;
    uint32_t makeFromEndOfScratch(size_t first);
    uint32_t combineLists(IDSetOperation, uint32_t first, const IDSetView&, uint32_t second, const IDSetView&);
    uint32_t internCombination(IDSetOperation, const IDSetView&, const IDSetView&, uint32_t size, std::optional<IDSetOperation> operationForAnnotator, uint32_t firstOperand, uint32_t secondOperand);
    std::optional<uint32_t> remembered(Question, uint32_t first, uint32_t second);
    void remember(Question, uint32_t first, uint32_t second, uint32_t answer);

    IDSetTable& m_table;
    const ArenaKeeps m_keeps;
    IDSetAnnotator* m_annotator;
    Additions m_additions;
    uint32_t m_epoch { 1 };
    Vector<uint32_t> m_words;
    Vector<uint32_t> m_firstWordOfRecord;
    // By record, as far as anybody has asked: where in m_words the list that listOf() made begins. UINT32_MAX: none was made.
    Vector<uint32_t> m_lazyListStart;
    Vector<IDSetChunk> m_chunks;
    Vector<uint32_t> m_index;
    Vector<uint32_t> m_chunkIndex;
    IDPairMemo m_lastingMemo;
    Vector<MemoEntry> m_memoUntilReset;
    Vector<uint32_t> m_scratch;
    Vector<IDSetChunkEntry> m_scratchEntries;
    UnionScratch m_unionScratch;
    // How many calls of intern() hold members that may lie in one of the three above. Whatever lays anything out there stops the build unless it is 0.
    unsigned m_depthOfInterning { 0 };
    // entriesSince(). The index is an epoch index over the first list.
    Vector<EntriesSinceEarlierState> m_earlierStatesCompared;
    Vector<IDSetChunkEntry> m_entriesSinceEarlierStates;
    Vector<uint32_t> m_indexOfEarlierStatesCompared;
    uint32_t m_epochOfEarlierStates { 1 };
    // The buffers that m_words and m_chunks have outgrown. Views, lists and chunks that were handed out point into them until reset().
    Vector<Vector<uint32_t>> m_outgrownWords;
    Vector<Vector<IDSetChunk>> m_outgrownChunks;
};

// The same with the types of its members and its ids.
template<typename MemberID, typename SetID>
class SetView {
public:
    SetView() = default;
    explicit SetView(IDSetView view)
        : m_view(view)
    {
    }

    uint32_t size() const { return m_view.size(); }
    bool isEmpty() const { return m_view.isEmpty(); }
    bool contains(MemberID member) const { return member && m_view.contains(member.index()); }
    MemberID first() const { return MemberID(m_view.first()); }
    MemberID last() const { return MemberID(m_view.last()); }
    MemberID onlyMember() const { return size() == 1 ? first() : MemberID(); }
    bool isList() const { return m_view.isList(); }
    std::span<const MemberID> list() const LIFETIME_BOUND
    {
        static_assert(sizeof(MemberID) == sizeof(uint32_t));
        std::span<const uint32_t> numbers = m_view.list();
        return unsafeMakeSpan(reinterpret_cast<const MemberID*>(numbers.data()), numbers.size());
    }
    void forEach(NOESCAPE const Invocable<void(MemberID)> auto& functor) const
    {
        m_view.forEach([&](uint32_t member) {
            functor(MemberID(member));
        });
    }
    bool anyOf(NOESCAPE const Invocable<bool(MemberID)> auto& matches) const
    {
        return m_view.anyOf([&](uint32_t member) {
            return matches(MemberID(member));
        });
    }
    bool anyOfWithin(uint32_t begin, uint32_t end, NOESCAPE const Invocable<bool(MemberID)> auto& matches) const
    {
        return m_view.anyOfWithin(begin, end, [&](uint32_t member) {
            return matches(MemberID(member));
        });
    }
    bool allOf(NOESCAPE const Invocable<bool(MemberID)> auto& matches) const
    {
        return m_view.allOf([&](uint32_t member) {
            return matches(MemberID(member));
        });
    }
    const IDSetView& untyped() const LIFETIME_BOUND { return m_view; }

private:
    IDSetView m_view;
};

template<typename MemberID, typename SetID>
class SetsOf {
public:
    using View = SetView<MemberID, SetID>;

    SetsOf(IDSetTable& table, ArenaKeeps keeps, IDSetAnnotator* annotator = nullptr)
        : m_arena(table, keeps, annotator)
    {
    }

    void reset() { m_arena.reset(); }

    View at(SetID set) const { return View(m_arena.viewOf(set.bits())); }
    std::span<const MemberID> listOf(SetID set)
    {
        static_assert(sizeof(MemberID) == sizeof(uint32_t));
        std::span<const uint32_t> numbers = m_arena.listOf(set.bits());
        return unsafeMakeSpan(reinterpret_cast<const MemberID*>(numbers.data()), numbers.size());
    }
    uint32_t sizeOf(SetID set) const { return !set ? 0 : holdsOneMember(set) ? 1 : at(set).size(); }
    bool contains(SetID set, MemberID member) const { return holdsOneMember(set) ? positionOf(set) == member.index() : at(set).contains(member); }
    MemberID onlyMemberOf(SetID set) const { return holdsOneMember(set) ? MemberID(positionOf(set)) : MemberID(); }

    static constexpr SetID of(MemberID member) { return setOfOne<SetID>(member); }
    SetID ofSorted(std::span<const MemberID> members) { return SetID::fromBits(m_arena.makeFromSorted(numbersOf(members))); }
    SetID ofUnsorted(std::span<const MemberID> members) { return SetID::fromBits(m_arena.makeFromUnsorted(numbersOf(members))); }
    SetID with(SetID set, MemberID member) { return SetID::fromBits(m_arena.with(set.bits(), member.index())); }
    SetID without(SetID set, MemberID member) { return SetID::fromBits(m_arena.without(set.bits(), member.index())); }
    SetID unionOf(SetID a, SetID b) { return a == b || !b ? a : !a ? b : SetID::fromBits(m_arena.unionOf(a.bits(), b.bits())); }
    SetID unionOf(std::span<const SetID> sets, std::span<const SetID> earlierStates = { })
    {
        static_assert(sizeof(SetID) == sizeof(uint32_t));
        return SetID::fromBits(m_arena.unionOf(unsafeMakeSpan(reinterpret_cast<const uint32_t*>(sets.data()), sets.size()), unsafeMakeSpan(reinterpret_cast<const uint32_t*>(earlierStates.data()), earlierStates.size())));
    }
    SetID intersectionOf(SetID a, SetID b) { return a == b ? a : !a || !b ? SetID() : SetID::fromBits(m_arena.intersectionOf(a.bits(), b.bits())); }
    SetID differenceOf(SetID a, SetID b) { return a == b || !a ? SetID() : !b ? a : SetID::fromBits(m_arena.differenceOf(a.bits(), b.bits())); }
    bool isSubset(SetID set, SetID of) { return !set || set == of || (of && m_arena.isSubset(set.bits(), of.bits())); }
    bool overlap(SetID a, SetID b) { return a && b && (a == b || m_arena.overlap(a.bits(), b.bits())); }

    SetID filtered(SetID set, NOESCAPE const Invocable<bool(MemberID)> auto& keeps)
    {
        return SetID::fromBits(m_arena.filtered(set.bits(), [&](uint32_t member) {
            return keeps(MemberID(member));
        }));
    }
    SetID replaced(SetID set, NOESCAPE const Invocable<MemberID(MemberID)> auto& replacement)
    {
        return SetID::fromBits(m_arena.replaced(set.bits(), [&](uint32_t member) {
            return replacement(MemberID(member)).index();
        }));
    }
    void forEachAdded(SetID larger, SetID smaller, NOESCAPE const Invocable<void(MemberID)> auto& functor) const
    {
        forEachAddedMember(m_arena.viewOf(larger.bits()), m_arena.viewOf(smaller.bits()), [&](uint32_t member) {
            functor(MemberID(member));
        });
    }
    SetID setOfProgramBehind(SetID set) const { return SetID::fromBits(m_arena.setOfProgramBehind(set.bits())); }
    void noteSetOfProgramBehind(SetID set, SetID madeFrom) { m_arena.noteSetOfProgramBehind(set.bits(), madeFrom.bits()); }

    IDSetArena& untyped() { return m_arena; }
    const IDSetArena& untyped() const { return m_arena; }

private:
    static std::span<const uint32_t> numbersOf(std::span<const MemberID> members)
    {
        static_assert(sizeof(MemberID) == sizeof(uint32_t));
        return unsafeMakeSpan(reinterpret_cast<const uint32_t*>(members.data()), members.size());
    }

    IDSetArena m_arena;
};

using LayoutSets = SetsOf<LayoutID, LayoutSetID>;
using ObjectSets = SetsOf<ObjectID, ObjectSetID>;
using StringSets = SetsOf<StringID, StringSetID>;
using KeySets = SetsOf<PropertyKeyID, KeySetID>;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
