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

using UntypedSetID = ID<struct UntypedSetIDTag>;

constexpr bool idSetLiesInThread(uint32_t set) { return liesInThread(UntypedSetID::fromBits(set)); }

constexpr uint64_t mixBitsForIDSets(uint64_t bits)
{
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdull;
    bits ^= bits >> 33;
    bits *= 0xc4ceb9fe1a85ec53ull;
    return bits ^ bits >> 33;
}

constexpr uint32_t hashOfMemoQuestion(uint32_t question, uint32_t first, uint32_t second)
{
    return first * 0x9e3779b1u + second * 0x85ebca77u + question * 0xc2b2ae3du;
}

constexpr size_t slotOfMemoHash(uint32_t hash, size_t numberOfSlots)
{
    return static_cast<size_t>(static_cast<uint64_t>(hash) * numberOfSlots >> 32);
}

enum class IDSetForm : uint8_t {
    ListsAndChunks,
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
    uint32_t chunk;
};

struct IDSetHeader {
    uint32_t size;
    uint32_t numberOfChunks;
};

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
            uint32_t firstMemberOfChunk = entry.firstMemberDividedBySize * membersPerSetChunk;
            if (firstMemberOfChunk >= end)
                return false;
            const IDSetChunk& chunk = chunkAt(entry.chunk);
            for (uint32_t word = begin > firstMemberOfChunk ? (begin - firstMemberOfChunk) / 64 : 0; word < chunk.bits.size(); ++word) {
                for (uint64_t bits = chunk.bits[word]; bits; bits &= bits - 1) {
                    uint32_t member = firstMemberOfChunk + word * 64 + std::countr_zero(bits);
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

JS_EXPORT_PRIVATE void forEachAddedMember(const IDSetView& larger, const IDSetView& smaller, NOESCAPE const ScopedLambda<void(uint32_t)>&);
JS_EXPORT_PRIVATE bool isSubset(const IDSetView&, const IDSetView& of);
JS_EXPORT_PRIVATE bool overlap(const IDSetView&, const IDSetView&);
JS_EXPORT_PRIVATE std::strong_ordering compareByContent(const IDSetView&, const IDSetView&);

class IDSetAnnotator {
public:
    virtual ~IDSetAnnotator() = default;
    virtual void annotate(const IDSetView& membersWithoutAttachedWords, std::optional<IDSetOperation>, std::span<const uint32_t> operands, std::span<uint32_t> attachedWords) = 0;
};

class IDPairMemo {
public:
    static constexpr size_t leastAnswersInGeneration = 1 << 14;

    std::optional<uint32_t> find(uint32_t question, uint32_t first, uint32_t second);
    std::optional<uint32_t> findInRecentGeneration(uint32_t question, uint32_t first, uint32_t second);
    std::optional<uint32_t> findInOlderGeneration(uint32_t question, uint32_t first, uint32_t second);
    JS_EXPORT_PRIVATE void add(uint32_t question, uint32_t first, uint32_t second, uint32_t answer);
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
        uint32_t questionPlusOne { 0 };
    };

    struct Generation {
        const Entry* find(uint32_t hash, uint32_t questionPlusOne, uint32_t first, uint32_t second) const;
        void add(const Entry&);
        void grow();

        Vector<Entry> slots;
        size_t size { 0 };
    };

    static constexpr uint32_t hashOfEntry(const Entry& entry) { return hashOfMemoQuestion(entry.questionPlusOne - 1, entry.first, entry.second); }
    JS_EXPORT_PRIVATE uint32_t moveToRecentGeneration(const Entry&);

    Generation m_recent;
    Generation m_older;
    size_t m_numberMoved { 0 };
    bool m_keepsEverything { false };
#if ASSERT_ENABLED || ASAN_ENABLED
    unsigned m_numberOfHits { 0 };
#endif
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

ALWAYS_INLINE const IDPairMemo::Entry* IDPairMemo::Generation::find(uint32_t hash, uint32_t questionPlusOne, uint32_t first, uint32_t second) const
{
    if (slots.isEmpty())
        return nullptr;
    const Entry* entries = slots.span().data();
    size_t mask = slots.size() - 1;
    for (size_t slot = slotOfMemoHash(hash, slots.size());; slot = (slot + 1) & mask) {
        const Entry& entry = entries[slot];
        if (entry.questionPlusOne == questionPlusOne && entry.first == first && entry.second == second)
            return &entry;
        if (!entry.questionPlusOne)
            return nullptr;
    }
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

ALWAYS_INLINE std::optional<uint32_t> IDPairMemo::find(uint32_t question, uint32_t first, uint32_t second)
{
    uint32_t hash = hashOfMemoQuestion(question, first, second);
    uint32_t answer;
    if (const Entry* recent = m_recent.find(hash, question + 1, first, second))
        answer = recent->answer;
    else if (const Entry* older = m_older.find(hash, question + 1, first, second))
        answer = moveToRecentGeneration(*older);
    else
        return std::nullopt;
#if ASSERT_ENABLED || ASAN_ENABLED
    if (!(++m_numberOfHits % 16))
        return std::nullopt;
#endif
    return answer;
}

ALWAYS_INLINE std::optional<uint32_t> IDPairMemo::findInRecentGeneration(uint32_t question, uint32_t first, uint32_t second)
{
    const Entry* recent = m_recent.find(hashOfMemoQuestion(question, first, second), question + 1, first, second);
    if (!recent)
        return std::nullopt;
#if ASSERT_ENABLED || ASAN_ENABLED
    if (!(++m_numberOfHits % 16))
        return std::nullopt;
#endif
    return recent->answer;
}

ALWAYS_INLINE std::optional<uint32_t> IDPairMemo::findInOlderGeneration(uint32_t question, uint32_t first, uint32_t second)
{
    const Entry* older = m_older.find(hashOfMemoQuestion(question, first, second), question + 1, first, second);
    if (!older)
        return std::nullopt;
    uint32_t answer = moveToRecentGeneration(*older);
#if ASSERT_ENABLED || ASAN_ENABLED
    if (!(++m_numberOfHits % 16))
        return std::nullopt;
#endif
    return answer;
}

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

class IDSetTable {
    WTF_MAKE_NONCOPYABLE(IDSetTable);
public:
    static constexpr uint32_t mostMembersOfList = 16;
    static constexpr uint32_t numberOfShards = 64;

    JS_EXPORT_PRIVATE IDSetTable(IDSetForm, uint32_t numberOfAttachedWords);
    JS_EXPORT_PRIVATE ~IDSetTable();

    uint32_t numberOfAttachedWords() const { return m_numberOfAttachedWords; }

    JS_EXPORT_PRIVATE void reserveMembers(uint32_t size);

    IDSetView viewOf(uint32_t set) const;
    std::span<const uint32_t> listOf(uint32_t set) const;
    uint32_t sizeOf(uint32_t set) const;

    void freeze()
    {
        m_isFrozen = true;
        freeOutgrownSlots();
    }
    bool isFrozen() const { return m_isFrozen; }
    void beginRoundWithoutAdditions() { m_numberOfRoundsWithoutAdditions.fetch_add(1, std::memory_order_relaxed); }
    void endRoundWithoutAdditions() { m_numberOfRoundsWithoutAdditions.fetch_sub(1, std::memory_order_relaxed); }
    bool takesNoAdditions() const { return m_isFrozen || m_numberOfRoundsWithoutAdditions.load(std::memory_order_relaxed); }
    JS_EXPORT_PRIVATE size_t sizeInBytes() const;

    struct Contents {
        uint64_t numberOfSets { 0 };
        uint64_t numberOfChunks { 0 };
        uint64_t bytesOfRecords { 0 };
        uint64_t bytesOutsideRecords { 0 };
        uint64_t bytesOfIndexes { 0 };
    };
    JS_EXPORT_PRIVATE Contents contents() const;

private:
    friend class IDSetArena;
    friend class IDSetView;

    struct Shard;

    std::optional<uint32_t> find(uint32_t hash, const IDSetView& members) const;
    uint32_t findOrAdd(uint32_t hash, const IDSetView& members, std::span<const uint32_t> attachedWords, bool& isNew);
    std::optional<uint32_t> findChunk(uint32_t hash, const IDSetChunk&) const;
    uint32_t findOrAddChunk(uint32_t hash, const IDSetChunk&, bool& isNew);
    JS_EXPORT_PRIVATE void freeOutgrownSlots();

    JS_EXPORT_PRIVATE IDSetView viewOfRecord(uint32_t set) const;
    JS_EXPORT_PRIVATE std::span<const uint32_t> listOfRecord(uint32_t set) const;
    JS_EXPORT_PRIVATE uint32_t sizeOfRecord(uint32_t set) const;
    JS_EXPORT_PRIVATE const uint32_t* lastingListOfOne(uint32_t member) const;

    const IDSetForm m_form;
    const uint32_t m_numberOfAttachedWords;
    bool m_isFrozen { false };
    std::atomic<unsigned> m_numberOfRoundsWithoutAdditions { 0 };
    Vector<uint32_t> m_allMembers;
    std::unique_ptr<Shard[]> m_shards;
};

inline IDSetView IDSetTable::viewOf(uint32_t set) const
{
    if (static_cast<IDHome>(set >> idHomeShift) != IDHome::Value)
        return viewOfRecord(set);

    IDSetView view;
    view.m_table = this;
    view.m_size = 1;
    view.m_onlyMember = set;
    if (set < m_allMembers.size())
        view.m_words = &m_allMembers[set];
    else if (m_form == IDSetForm::ChunksWithLazyList)
        view.m_words = lastingListOfOne(set);
    return view;
}

inline std::span<const uint32_t> IDSetTable::listOf(uint32_t set) const
{
    RELEASE_ASSERT(m_form == IDSetForm::ChunksWithLazyList);
    if (set < m_allMembers.size())
        return unsafeMakeSpan(&m_allMembers[set], 1);
    return listOfRecord(set);
}

inline uint32_t IDSetTable::sizeOf(uint32_t set) const
{
    if (static_cast<IDHome>(set >> idHomeShift) == IDHome::Value)
        return 1;
    return sizeOfRecord(set);
}

enum class ArenaKeeps : uint8_t {
    InProgram,
    UntilReset,
};

class IDSetArena {
    WTF_MAKE_NONCOPYABLE(IDSetArena);
public:
    JS_EXPORT_PRIVATE IDSetArena(IDSetTable&, ArenaKeeps, IDSetAnnotator* = nullptr);
    JS_EXPORT_PRIVATE ~IDSetArena();

    JS_EXPORT_PRIVATE void reset();

    IDSetTable& table() { return m_table; }
    const IDSetTable& table() const { return m_table; }
    struct Additions {
        static constexpr size_t numberOfMakers = 4;

        std::array<uint64_t, numberOfMakers> sets { };
        std::array<uint64_t, numberOfMakers> wordsOfSets { };
        std::array<uint64_t, numberOfMakers> chunks { };
    };
    const Additions& additions() const { return m_additions; }
    size_t numberMadeAndKeptToItself() const { return m_firstWordOfRecord.size() + m_chunks.size(); }

    IDSetView viewOf(uint32_t set) const { return idSetLiesInThread(set) ? viewOfRecordInThread(set) : m_table.viewOf(set); }
    std::span<const uint32_t> listOf(uint32_t set) { return idSetLiesInThread(set) ? listOfRecordInThread(set) : m_table.listOf(set); }
    uint32_t sizeOf(uint32_t set) const { return idSetLiesInThread(set) ? m_words[m_firstWordOfRecord[set & idPositionMask]] : m_table.sizeOf(set); }

    IDPairMemo takeLastingMemo() { return std::exchange(m_lastingMemo, { }); }
    void setLastingMemo(IDPairMemo&& memo) { m_lastingMemo = WTF::move(memo); }
    IDPairMemo& lastingMemo() { return m_lastingMemo; }
    const IDPairMemo& lastingMemo() const { return m_lastingMemo; }
    size_t sizeOfMemosInBytes() const
    {
        return m_lastingMemo.sizeInBytes() + m_memoUntilReset.capacity() * sizeof(MemoEntry) + m_earlierStatesCompared.capacity() * sizeof(EntriesSinceEarlierState)
            + m_entriesSinceEarlierStates.capacity() * sizeof(IDSetChunkEntry) + m_indexOfEarlierStatesCompared.capacity() * sizeof(uint32_t);
    }
    size_t sizeOfScratchInBytes() const { return m_scratch.capacity() * sizeof(uint32_t) + m_scratchEntries.capacity() * sizeof(IDSetChunkEntry) + m_unionScratch.sizeInBytes(); }

    JS_EXPORT_PRIVATE uint32_t makeFromSorted(std::span<const uint32_t> ascendingWithoutRepeats);
    JS_EXPORT_PRIVATE uint32_t makeFromUnsorted(std::span<const uint32_t>);
    JS_EXPORT_PRIVATE uint32_t with(uint32_t set, uint32_t member);
    JS_EXPORT_PRIVATE uint32_t without(uint32_t set, uint32_t member);
    JS_EXPORT_PRIVATE uint32_t unionOf(uint32_t, uint32_t);
    uint32_t unionOf(std::span<const uint32_t> sets) { return unionOf(sets, { }); }
    JS_EXPORT_PRIVATE uint32_t unionOf(std::span<const uint32_t> sets, std::span<const uint32_t> earlierStates);
    JS_EXPORT_PRIVATE void forgetEarlierStates();
    JS_EXPORT_PRIVATE uint32_t intersectionOf(uint32_t, uint32_t);
    JS_EXPORT_PRIVATE uint32_t differenceOf(uint32_t, uint32_t);
    JS_EXPORT_PRIVATE bool isSubset(uint32_t set, uint32_t of);
    JS_EXPORT_PRIVATE bool overlap(uint32_t, uint32_t);
    JS_EXPORT_PRIVATE uint32_t filtered(uint32_t set, NOESCAPE const ScopedLambda<bool(uint32_t)>& keeps);
    JS_EXPORT_PRIVATE uint32_t replaced(uint32_t set, NOESCAPE const ScopedLambda<uint32_t(uint32_t)>& replacement);

    JS_EXPORT_PRIVATE uint32_t setOfProgramBehind(uint32_t set) const;
    JS_EXPORT_PRIVATE void noteSetOfProgramBehind(uint32_t set, uint32_t madeFrom);

private:
    friend class IDSetView;

    Vector<uint32_t> m_setOfProgramBehindRecord;

    JS_EXPORT_PRIVATE IDSetView viewOfRecordInThread(uint32_t set) const;
    JS_EXPORT_PRIVATE std::span<const uint32_t> listOfRecordInThread(uint32_t set);
    void annotateChunkByChunk(const IDSetView& members, std::span<uint32_t> attachedWords);
    uint32_t setOfMembersInChunk(IDSetChunkEntry);

    struct MemoEntry {
        uint32_t first { UINT32_MAX };
        uint32_t second { UINT32_MAX };
        uint32_t result { UINT32_MAX };
        uint32_t operationAndEpoch { 0 };
    };

    enum class Question : uint8_t { Union, Intersection, Difference, Subset, Overlap };

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

    struct EntriesSinceEarlierState {
        uint32_t set;
        uint32_t earlierState;
        uint32_t first;
        uint32_t count;
    };

    uint32_t unionOfDistinct(std::span<const uint32_t> operands, std::span<const uint32_t> earlierStates);
    void collectCandidateChunks(UnionScratch&, uint32_t largest, std::span<const IDSetChunkEntry> entriesOfLargest, std::span<const uint32_t> wideOperands, std::span<const uint32_t> earlierStatesOfWideOperands, const IDSetView& listedMembers, bool memoizesSubsets);
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
    Vector<uint32_t> m_lazyListStart;
    Vector<IDSetChunk> m_chunks;
    Vector<uint32_t> m_index;
    Vector<uint32_t> m_chunkIndex;
    IDPairMemo m_lastingMemo;
    Vector<MemoEntry> m_memoUntilReset;
    Vector<uint32_t> m_scratch;
    Vector<IDSetChunkEntry> m_scratchEntries;
    UnionScratch m_unionScratch;
    unsigned m_depthOfInterning { 0 };
    Vector<EntriesSinceEarlierState> m_earlierStatesCompared;
    Vector<IDSetChunkEntry> m_entriesSinceEarlierStates;
    Vector<uint32_t> m_indexOfEarlierStatesCompared;
    uint32_t m_epochOfEarlierStates { 1 };
    Vector<Vector<uint32_t>> m_outgrownWords;
    Vector<Vector<IDSetChunk>> m_outgrownChunks;
};

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
    uint32_t sizeOf(SetID set) const { return !set ? 0 : holdsOneMember(set) ? 1 : m_arena.sizeOf(set.bits()); }
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
