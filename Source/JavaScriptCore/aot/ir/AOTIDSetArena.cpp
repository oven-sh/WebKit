/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIDSets.h"
#include "AOTInliningAcrossFiles.h"

#if ENABLE(AOT)

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <utility>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t emptyIDSet = UntypedSetID().bits();
static constexpr size_t wordsOfIDSetHeader = sizeof(IDSetHeader) / sizeof(uint32_t);
static constexpr size_t wordsOfIDSetChunkEntry = sizeof(IDSetChunkEntry) / sizeof(uint32_t);
static constexpr size_t wordsOfEpochIndexSlot = 2;

static constexpr size_t entriesOfIDSetMemoUntilReset = 1 << 12;
static constexpr uint32_t bitsOfQuestionInIDSetMemo = 3;
static constexpr uint32_t largestEpochOfIDSetArena = (1u << (32 - bitsOfQuestionInIDSetMemo)) - 1;

static_assert(!offsetof(IDSetHeader, size) && offsetof(IDSetHeader, numberOfChunks) == sizeof(uint32_t) && wordsOfIDSetHeader == 2);
static_assert(std::has_single_bit(entriesOfIDSetMemoUntilReset));

static constexpr bool idSetHoldsOneMember(uint32_t set) { return holdsOneMember(UntypedSetID::fromBits(set)); }
static constexpr uint32_t positionOfIDSet(uint32_t set) { return positionOf(UntypedSetID::fromBits(set)); }
static constexpr uint32_t idSetOfOne(uint32_t member) { return idAt<UntypedSetID>(IDHome::Value, member).bits(); }
static constexpr uint32_t idSetInThread(uint32_t position) { return idAt<UntypedSetID>(IDHome::Thread, position).bits(); }

static uint32_t hashOfIDSetWords(std::span<const uint32_t> words)
{
    uint64_t hash = words.size();
    for (uint32_t word : words) {
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 32;
    }
    return static_cast<uint32_t>(mixBitsForIDSets(hash));
}

static uint32_t hashOfIDSetChunk(const IDSetChunk& chunk)
{
    uint64_t hash = 0;
    for (uint64_t bits : chunk.bits)
        hash = mixBitsForIDSets(hash ^ bits);
    return static_cast<uint32_t>(hash);
}

static constexpr uint64_t placeInIDSetMemo(uint32_t question, uint32_t first, uint32_t second)
{
    return mixBitsForIDSets((static_cast<uint64_t>(first) << 32 | second) + question * 0x9e3779b97f4a7c15ull);
}

static constexpr uint32_t stampInIDSetMemo(uint32_t question, uint32_t epoch)
{
    return epoch << bitsOfQuestionInIDSetMemo | question;
}

static constexpr size_t makerInIDSetAdditions(std::optional<IDSetOperation> operation)
{
    static_assert(static_cast<size_t>(IDSetOperation::Difference) + 2 == IDSetArena::Additions::numberOfMakers);
    return operation ? static_cast<size_t>(*operation) : IDSetArena::Additions::numberOfMakers - 1;
}

static bool idSetMembersAreStrictlyAscending(std::span<const uint32_t> members)
{
    return std::ranges::adjacent_find(members, std::ranges::greater_equal { }) == members.end();
}

static std::span<const uint32_t> wordsOfIDSetMembers(const IDSetView& members)
{
    if (members.isList())
        return members.list();
    std::span<const IDSetChunkEntry> entries = members.chunkEntries();
    return unsafeMakeSpan(reinterpret_cast<const uint32_t*>(entries.data()), entries.size() * wordsOfIDSetChunkEntry);
}

static std::optional<uint32_t> findInEpochIndex(const Vector<uint32_t>& index, uint32_t epoch, uint32_t hash, NOESCAPE const Invocable<bool(uint32_t)> auto& matches)
{
    if (index.isEmpty())
        return std::nullopt;
    size_t mask = index.size() / wordsOfEpochIndexSlot - 1;
    for (size_t slot = hash & mask; index[slot * wordsOfEpochIndexSlot + 1] == epoch; slot = (slot + 1) & mask) {
        uint32_t position = index[slot * wordsOfEpochIndexSlot];
        if (matches(position))
            return position;
    }
    return std::nullopt;
}

static void putIntoFreeSlotOfEpochIndex(Vector<uint32_t>& index, uint32_t epoch, uint32_t hash, uint32_t position)
{
    size_t mask = index.size() / wordsOfEpochIndexSlot - 1;
    size_t slot = hash & mask;
    while (index[slot * wordsOfEpochIndexSlot + 1] == epoch)
        slot = (slot + 1) & mask;
    index[slot * wordsOfEpochIndexSlot] = position;
    index[slot * wordsOfEpochIndexSlot + 1] = epoch;
}

static void addToEpochIndex(Vector<uint32_t>& index, uint32_t epoch, uint32_t hash, uint32_t position, NOESCAPE const Invocable<uint32_t(uint32_t)> auto& hashAtPosition)
{
    constexpr size_t fewestSlots = 64;
    size_t numberOfSlots = index.size() / wordsOfEpochIndexSlot;
    if ((static_cast<size_t>(position) + 1) * 2 > numberOfSlots) {
        index.fill(0u, std::max(fewestSlots, numberOfSlots * 2) * wordsOfEpochIndexSlot);
        for (uint32_t earlier = 0; earlier < position; ++earlier)
            putIntoFreeSlotOfEpochIndex(index, epoch, hashAtPosition(earlier), earlier);
    }
    putIntoFreeSlotOfEpochIndex(index, epoch, hash, position);
}

template<typename T>
static void makeRoomKeepingOutgrownBuffer(Vector<T>& vector, Vector<Vector<T>>& outgrown, size_t numberToAdd)
{
    constexpr size_t leastCapacity = 256;
    size_t sizeNeeded = vector.size() + numberToAdd;
    if (sizeNeeded <= vector.capacity())
        return;
    Vector<T> larger;
    larger.reserveInitialCapacity(std::max({ sizeNeeded, vector.capacity() * 2, leastCapacity }));
    larger.append(vector.span());
    if (vector.capacity())
        outgrown.append(WTF::move(vector));
    vector = WTF::move(larger);
}

static uint32_t numberOfMembersInIDSetChunk(const IDSetChunk& chunk)
{
    uint32_t number = 0;
    for (uint64_t bits : chunk.bits)
        number += static_cast<uint32_t>(std::popcount(bits));
    return number;
}

template<typename T>
static size_t gallopingLowerBound(std::span<const T> ascending, size_t from, uint32_t bound, auto projection)
{
    size_t probe = from;
    for (size_t step = 1; probe < ascending.size() && std::invoke(projection, ascending[probe]) < bound; step *= 2) {
        from = probe + 1;
        probe += step;
    }
    std::span<const T> unprobed = ascending.subspan(from, std::min(probe, ascending.size()) - from);
    return from + (std::ranges::lower_bound(unprobed, bound, { }, projection) - unprobed.begin());
}

static size_t firstIDSetChunkEntryNotBelow(std::span<const IDSetChunkEntry> entries, size_t from, uint32_t number)
{
    return gallopingLowerBound(entries, from, number, &IDSetChunkEntry::firstMemberDividedBySize);
}

static IDSetChunk combineIDSetChunks(IDSetOperation operation, const IDSetChunk& first, const IDSetChunk& second)
{
    ASSERT(operation != IDSetOperation::Union);
    IDSetChunk combined;
    for (size_t word = 0; word < combined.bits.size(); ++word)
        combined.bits[word] = first.bits[word] & (operation == IDSetOperation::Intersection ? second.bits[word] : ~second.bits[word]);
    return combined;
}

class IDSetChunkCursor {
    WTF_MAKE_NONCOPYABLE(IDSetChunkCursor);
public:
    explicit IDSetChunkCursor(const IDSetView& set LIFETIME_BOUND)
        : m_set(set)
    {
        loadCurrentChunk();
    }

    bool isAtEnd() const { return m_isAtEnd; }
    uint32_t number() const { return m_number; }
    bool isInterned() const { return !m_set.isList(); }
    uint32_t id() const
    {
        ASSERT(isInterned());
        return m_id;
    }
    bool holdsSameChunkAs(const IDSetChunkCursor& other) const { return isInterned() && other.isInterned() && m_id == other.m_id; }

    const IDSetChunk& chunk()
    {
        if (!isInterned())
            return m_gathered;
        if (!m_interned)
            m_interned = &m_set.chunkAt(m_id);
        return *m_interned;
    }

    void advance()
    {
        if (isInterned())
            ++m_index;
        loadCurrentChunk();
    }

    void advanceTo(uint32_t number)
    {
        ASSERT(!m_isAtEnd && m_number < number);
        if (isInterned())
            m_index = firstIDSetChunkEntryNotBelow(m_set.chunkEntries(), m_index + 1, number);
        else
            m_index = gallopingLowerBound(m_set.list(), m_index, number * membersPerSetChunk, std::identity { });
        loadCurrentChunk();
    }

private:
    void loadCurrentChunk()
    {
        m_interned = nullptr;
        if (isInterned()) {
            std::span<const IDSetChunkEntry> entries = m_set.chunkEntries();
            m_isAtEnd = m_index == entries.size();
            if (m_isAtEnd)
                return;
            m_number = entries[m_index].firstMemberDividedBySize;
            m_id = entries[m_index].chunk;
            return;
        }
        std::span<const uint32_t> list = m_set.list();
        m_isAtEnd = m_index == list.size();
        if (m_isAtEnd)
            return;
        m_number = list[m_index] / membersPerSetChunk;
        m_gathered = { };
        for (; m_index < list.size() && list[m_index] / membersPerSetChunk == m_number; ++m_index) {
            uint32_t place = list[m_index] % membersPerSetChunk;
            m_gathered.bits[place / 64] |= 1ull << place % 64;
        }
    }

    const IDSetView& m_set;
    size_t m_index { 0 };
    bool m_isAtEnd { false };
    uint32_t m_number { 0 };
    uint32_t m_id { 0 };
    const IDSetChunk* m_interned { nullptr };
    IDSetChunk m_gathered;
};

static void forEachChunkOfIDSets(IDSetOperation operation, const IDSetView& first, const IDSetView& second, NOESCAPE const Invocable<void(IDSetChunkCursor*, IDSetChunkCursor*)> auto& functor)
{
    RELEASE_ASSERT(operation != IDSetOperation::Union);
    bool visitsChunksOnlyInFirst = operation == IDSetOperation::Difference;
    IDSetChunkCursor a(first);
    IDSetChunkCursor b(second);
    while (!a.isAtEnd() && !b.isAtEnd()) {
        if (a.number() == b.number()) {
            functor(&a, &b);
            a.advance();
            b.advance();
        } else if (a.number() > b.number())
            b.advanceTo(a.number());
        else if (visitsChunksOnlyInFirst) {
            functor(&a, nullptr);
            a.advance();
        } else
            a.advanceTo(b.number());
    }
    if (visitsChunksOnlyInFirst) {
        for (; !a.isAtEnd(); a.advance())
            functor(&a, nullptr);
    }
}

static uint32_t countCommonMembersOfIDSets(const IDSetView& first, const IDSetView& second)
{
    uint32_t number = 0;
    forEachChunkOfIDSets(IDSetOperation::Intersection, first, second, [&](IDSetChunkCursor* a, IDSetChunkCursor* b) {
        number += numberOfMembersInIDSetChunk(a->holdsSameChunkAs(*b) ? a->chunk() : combineIDSetChunks(IDSetOperation::Intersection, a->chunk(), b->chunk()));
    });
    return number;
}

static uint32_t countMembersOnlyInFirstIDSet(const IDSetView& first, const IDSetView& second)
{
    uint32_t number = 0;
    forEachChunkOfIDSets(IDSetOperation::Difference, first, second, [&](IDSetChunkCursor* a, IDSetChunkCursor* b) {
        if (!b)
            number += numberOfMembersInIDSetChunk(a->chunk());
        else if (!a->holdsSameChunkAs(*b))
            number += numberOfMembersInIDSetChunk(combineIDSetChunks(IDSetOperation::Difference, a->chunk(), b->chunk()));
    });
    return number;
}

static uint32_t sizeOfCombinedIDSets(IDSetOperation operation, const IDSetView& first, const IDSetView& second)
{
    switch (operation) {
    case IDSetOperation::Union:
        break;
    case IDSetOperation::Intersection:
        return countCommonMembersOfIDSets(first, second);
    case IDSetOperation::Difference:
        if (first.size() >= second.size())
            return first.size() - second.size() + countMembersOnlyInFirstIDSet(second, first);
        return countMembersOnlyInFirstIDSet(first, second);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void combineChunksWithSameNumber(IDSetOperation operation, IDSetChunkCursor* first, IDSetChunkCursor* second, NOESCAPE const Invocable<void(IDSetChunkCursor&)> auto& reuseChunk, NOESCAPE const Invocable<void(uint32_t, const IDSetChunk&)> auto& addNewChunk)
{
    if (!second) {
        reuseChunk(*first);
        return;
    }
    if (first->holdsSameChunkAs(*second)) {
        if (operation == IDSetOperation::Intersection)
            reuseChunk(*first);
        return;
    }
    IDSetChunk combined = combineIDSetChunks(operation, first->chunk(), second->chunk());
    if (combined == IDSetChunk { })
        return;
    bool isFirst = combined == first->chunk();
    bool isSecond = combined == second->chunk();
    if (isFirst && (first->isInterned() || !isSecond))
        reuseChunk(*first);
    else if (isSecond)
        reuseChunk(*second);
    else
        addNewChunk(first->number(), combined);
}

static void listCombinedIDSets(IDSetOperation operation, const IDSetView& first, const IDSetView& second, std::span<uint32_t> members)
{
    size_t count = 0;
    auto listChunk = [&](uint32_t number, const IDSetChunk& chunk) {
        for (size_t word = 0; word < chunk.bits.size(); ++word) {
            for (uint64_t bits = chunk.bits[word]; bits; bits &= bits - 1) {
                RELEASE_ASSERT(count < members.size());
                members[count++] = number * membersPerSetChunk + static_cast<uint32_t>(word * 64 + std::countr_zero(bits));
            }
        }
    };
    forEachChunkOfIDSets(operation, first, second, [&](IDSetChunkCursor* a, IDSetChunkCursor* b) {
        combineChunksWithSameNumber(operation, a, b, [&](IDSetChunkCursor& cursor) {
            listChunk(cursor.number(), cursor.chunk());
        }, listChunk);
    });
    RELEASE_ASSERT(count == members.size());
}

#if ASSERT_ENABLED
static bool isWellFormedIDSet(const IDSetView& members)
{
    uint32_t size = 0;
    uint64_t leastAllowed = 0;
    bool ascends = true;
    members.forEach([&](uint32_t member) {
        ascends &= member >= leastAllowed;
        leastAllowed = static_cast<uint64_t>(member) + 1;
        ++size;
    });
    if (!ascends || size != members.size() || leastAllowed > static_cast<uint64_t>(largestIDPosition) + 1)
        return false;
    return members.isList() || std::ranges::none_of(members.chunkEntries(), [&](const IDSetChunkEntry& entry) {
        return members.chunkAt(entry.chunk) == IDSetChunk { };
    });
}
#endif

static constexpr size_t initialSlotsOfIDMemo = 1 << 10;
static constexpr size_t mostBytesOfIDMemoThatDoubles = UINT32_MAX / 2;

void IDPairMemo::Generation::add(const Entry& added)
{
    if ((size + 1) * 2 > slots.size())
        grow();
    size_t mask = slots.size() - 1;
    size_t slot = slotOfMemoHash(hashOfEntry(added), slots.size());
    for (; slots[slot].questionPlusOne; slot = (slot + 1) & mask) {
        Entry& entry = slots[slot];
        if (entry.first != added.first || entry.second != added.second || entry.questionPlusOne != added.questionPlusOne)
            continue;
#if ASSERT_ENABLED || ASAN_ENABLED
        RELEASE_ASSERT(entry.answer == added.answer);
#endif
        entry.answer = added.answer;
        return;
    }
    slots[slot] = added;
    ++size;
}

void IDPairMemo::Generation::grow()
{
    if (slots.size() * sizeof(Entry) > mostBytesOfIDMemoThatDoubles) {
        slots.fill(Entry { });
        size = 0;
        return;
    }
    Vector<Entry> former = std::exchange(slots, { });
    slots.fill(Entry { }, std::max(initialSlotsOfIDMemo, former.size() * 2));
    size_t mask = slots.size() - 1;
    for (const Entry& entry : former) {
        if (!entry.questionPlusOne)
            continue;
        size_t slot = slotOfMemoHash(hashOfEntry(entry), slots.size());
        while (slots[slot].questionPlusOne)
            slot = (slot + 1) & mask;
        slots[slot] = entry;
    }
}

uint32_t IDPairMemo::moveToRecentGeneration(const Entry& entry)
{
    m_recent.add(entry);
    ++m_numberMoved;
    return entry.answer;
}

void IDPairMemo::add(uint32_t question, uint32_t first, uint32_t second, uint32_t answer)
{
    if (!m_keepsEverything && m_recent.size >= std::max(leastAnswersInGeneration, 4 * m_numberMoved)) {
        m_older = std::exchange(m_recent, { });
        m_numberMoved = 0;
    }
    m_recent.add(Entry { first, second, answer, question + 1 });
}

const IDListMemo::Entry* IDListMemo::Generation::find(uint32_t hash, std::span<const uint32_t> wanted) const
{
    if (slots.isEmpty())
        return nullptr;
    size_t mask = slots.size() - 1;
    for (size_t slot = hash & mask; slots[slot].numberOfIDs; slot = (slot + 1) & mask) {
        const Entry& entry = slots[slot];
        if (entry.hash == hash && entry.numberOfIDs == wanted.size() && equalSpans(ids.subspan(entry.firstID, entry.numberOfIDs), wanted))
            return &entry;
    }
    return nullptr;
}

void IDListMemo::Generation::add(uint32_t hash, std::span<const uint32_t> added, uint32_t answer)
{
    if ((ids.size() + added.size()) * sizeof(uint32_t) > mostBytesOfIDMemoThatDoubles)
        *this = { };
    if ((size + 1) * 2 > slots.size())
        grow();
    size_t mask = slots.size() - 1;
    size_t slot = hash & mask;
    for (; slots[slot].numberOfIDs; slot = (slot + 1) & mask) {
        Entry& entry = slots[slot];
        if (entry.hash != hash || entry.numberOfIDs != added.size() || !equalSpans(ids.subspan(entry.firstID, entry.numberOfIDs), added))
            continue;
#if ASSERT_ENABLED || ASAN_ENABLED
        RELEASE_ASSERT(entry.answer == answer);
#endif
        entry.answer = answer;
        return;
    }
    slots[slot] = Entry { static_cast<uint32_t>(ids.size()), static_cast<uint32_t>(added.size()), answer, hash };
    ids.append(added);
    ++size;
}

void IDListMemo::Generation::grow()
{
    if (slots.size() * sizeof(Entry) > mostBytesOfIDMemoThatDoubles) {
        *this = { };
        slots.fill(Entry { }, initialSlotsOfIDMemo);
        return;
    }
    Vector<Entry> former = std::exchange(slots, { });
    slots.fill(Entry { }, std::max(initialSlotsOfIDMemo, former.size() * 2));
    size_t mask = slots.size() - 1;
    for (const Entry& entry : former) {
        if (!entry.numberOfIDs)
            continue;
        size_t slot = entry.hash & mask;
        while (slots[slot].numberOfIDs)
            slot = (slot + 1) & mask;
        slots[slot] = entry;
    }
}

std::optional<uint32_t> IDListMemo::find(std::span<const uint32_t> ids)
{
    ASSERT(!ids.empty());
    uint32_t hash = hashOfIDSetWords(ids);
    const Entry* entry = m_recent.find(hash, ids);
    if (!entry) {
        entry = m_older.find(hash, ids);
        if (!entry)
            return std::nullopt;
        m_recent.add(hash, ids, entry->answer);
        m_numberOfIDsMoved += ids.size();
    }
#if ASSERT_ENABLED || ASAN_ENABLED
    if (!(++m_numberOfHits % 16))
        return std::nullopt;
#endif
    return entry->answer;
}

void IDListMemo::add(std::span<const uint32_t> ids, uint32_t answer)
{
    ASSERT(!ids.empty());
    if (!m_keepsEverything && m_recent.ids.size() >= std::max(leastIDsInGeneration, 4 * m_numberOfIDsMoved)) {
        m_older = std::exchange(m_recent, { });
        m_numberOfIDsMoved = 0;
    }
    m_recent.add(hashOfIDSetWords(ids), ids, answer);
}

IDSetArena::IDSetArena(IDSetTable& table, ArenaKeeps keeps, IDSetAnnotator* annotator)
    : m_table(table)
    , m_keeps(keeps)
    , m_annotator(annotator)
{
}

IDSetArena::~IDSetArena() = default;

uint32_t IDSetArena::setOfProgramBehind(uint32_t set) const
{
    if (idSetLiesInThread(set)) {
        uint32_t position = positionOfIDSet(set);
        return position < m_setOfProgramBehindRecord.size() ? m_setOfProgramBehindRecord[position] : emptyIDSet;
    }
    bool isLongSetOfProgram = set != emptyIDSet && !idSetHoldsOneMember(set) && m_table.viewOf(set).size() > membersPerSetChunk;
    return isLongSetOfProgram ? set : emptyIDSet;
}

void IDSetArena::noteSetOfProgramBehind(uint32_t set, uint32_t madeFrom)
{
    if (!idSetLiesInThread(set) || viewOf(set).size() <= membersPerSetChunk || setOfProgramBehind(set) != emptyIDSet)
        return;
    uint32_t ofProgram = setOfProgramBehind(madeFrom);
    if (ofProgram == emptyIDSet)
        return;
    uint32_t position = positionOfIDSet(set);
    if (position >= m_setOfProgramBehindRecord.size())
        m_setOfProgramBehindRecord.insertFill(m_setOfProgramBehindRecord.size(), emptyIDSet, position + 1 - m_setOfProgramBehindRecord.size());
    m_setOfProgramBehindRecord[position] = ofProgram;
}

void IDSetArena::annotateChunkByChunk(const IDSetView& members, std::span<uint32_t> attachedWords)
{
    Vector<IDSetChunkEntry, 64> entries;
    entries.append(members.chunkEntries());
    Vector<uint32_t, 64> setsOfChunks;
    for (IDSetChunkEntry entry : entries)
        setsOfChunks.append(setOfMembersInChunk(entry));
    m_annotator->annotate(members, IDSetOperation::Union, setsOfChunks.span(), attachedWords);
}

uint32_t IDSetArena::setOfMembersInChunk(IDSetChunkEntry entry)
{
    IDSetView members = viewOfEntries(singleElementSpan(entry), numberOfMembersInIDSetChunk(viewOfList({ }).chunkAt(entry.chunk)));
    if (members.size() > IDSetTable::mostMembersOfList)
        return intern(members, std::nullopt, emptyIDSet, emptyIDSet);

    std::array<uint32_t, IDSetTable::mostMembersOfList> list;
    size_t size = 0;
    members.forEach([&](uint32_t member) {
        list[size++] = member;
    });
    return intern(viewOfList(std::span(list).first(size)), std::nullopt, emptyIDSet, emptyIDSet);
}

ALWAYS_INLINE_INTO_OTHER_FILES void IDSetArena::reset()
{
    RELEASE_ASSERT(!m_depthOfInterning);
    ASSERT(m_scratch.isEmpty() && m_scratchEntries.isEmpty());
    forgetEarlierStates();
    if (m_firstWordOfRecord.isEmpty() && m_chunks.isEmpty())
        return;

    if (m_epoch == largestEpochOfIDSetArena) {
        m_index.fill(0u);
        m_chunkIndex.fill(0u);
        m_memoUntilReset.fill(MemoEntry { });
        m_epoch = 0;
    }
    ++m_epoch;
    m_words.shrink(0);
    m_firstWordOfRecord.shrink(0);
    m_lazyListStart.shrink(0);
    m_setOfProgramBehindRecord.shrink(0);
    m_chunks.shrink(0);
    m_outgrownWords.shrink(0);
    m_outgrownChunks.shrink(0);
}

IDSetView IDSetArena::viewOfRecordInThread(uint32_t set) const
{
    std::span<const uint32_t> record = m_words.subspan(m_firstWordOfRecord[positionOfIDSet(set)]);
    uint32_t numberOfAttachedWords = m_table.numberOfAttachedWords();
    IDSetView view;
    view.m_size = record[0];
    view.m_numberOfChunks = record[1];
    view.m_attachedWords = record.subspan(wordsOfIDSetHeader).data();
    view.m_numberOfAttachedWords = numberOfAttachedWords;
    view.m_words = record.subspan(wordsOfIDSetHeader + numberOfAttachedWords).data();
    view.m_table = &m_table;
    view.m_arena = this;
    return view;
}

std::span<const uint32_t> IDSetArena::listOfRecordInThread(uint32_t set)
{
    IDSetView view = viewOfRecordInThread(set);
    if (view.isList())
        return unsafeMakeSpan(view.m_words, view.m_size);

    uint32_t position = positionOfIDSet(set);
    if (position >= m_lazyListStart.size())
        m_lazyListStart.insertFill(m_lazyListStart.size(), UINT32_MAX, position + 1 - m_lazyListStart.size());
    if (m_lazyListStart[position] == UINT32_MAX) {
        makeRoomKeepingOutgrownBuffer(m_words, m_outgrownWords, view.size());
        m_lazyListStart[position] = static_cast<uint32_t>(m_words.size());
        view.forEach([&](uint32_t member) {
            m_words.append(member);
        });
    }
    return m_words.subspan(m_lazyListStart[position], view.size());
}

IDSetView IDSetArena::viewOfList(std::span<const uint32_t> ascendingWithoutRepeats) const
{
    IDSetView view;
    view.m_words = ascendingWithoutRepeats.data();
    view.m_size = static_cast<uint32_t>(ascendingWithoutRepeats.size());
    view.m_table = &m_table;
    view.m_arena = this;
    return view;
}

IDSetView IDSetArena::viewOfEntries(std::span<const IDSetChunkEntry> entries, uint32_t size) const
{
    IDSetView view;
    view.m_words = reinterpret_cast<const uint32_t*>(entries.data());
    view.m_size = size;
    view.m_numberOfChunks = static_cast<uint32_t>(entries.size());
    view.m_table = &m_table;
    view.m_arena = this;
    return view;
}

uint32_t IDSetArena::makeFromSorted(std::span<const uint32_t> ascendingWithoutRepeats)
{
    ASSERT(idSetMembersAreStrictlyAscending(ascendingWithoutRepeats));
    RELEASE_ASSERT(ascendingWithoutRepeats.empty() || ascendingWithoutRepeats.back() <= largestIDPosition);
    return intern(viewOfList(ascendingWithoutRepeats), std::nullopt, emptyIDSet, emptyIDSet);
}

uint32_t IDSetArena::makeFromUnsorted(std::span<const uint32_t> members)
{
    if (idSetMembersAreStrictlyAscending(members))
        return makeFromSorted(members);
    RELEASE_ASSERT(!m_depthOfInterning);
    size_t first = m_scratch.size();
    m_scratch.append(members);
    return makeFromEndOfScratch(first);
}

uint32_t IDSetArena::makeFromEndOfScratch(size_t first)
{
    std::span<uint32_t> members = m_scratch.mutableSpan().subspan(first);
    std::ranges::sort(members);
    size_t size = std::ranges::unique(members).begin() - members.begin();
    uint32_t result = makeFromSorted(members.first(size));
    m_scratch.shrink(first);
    return result;
}

uint32_t IDSetArena::with(uint32_t set, uint32_t member)
{
    return unionOf(set, idSetOfOne(member));
}

uint32_t IDSetArena::without(uint32_t set, uint32_t member)
{
    return differenceOf(set, idSetOfOne(member));
}

uint32_t IDSetArena::unionOf(uint32_t first, uint32_t second)
{
    if (first == second || second == emptyIDSet)
        return first;
    if (first == emptyIDSet)
        return second;
    return compute(IDSetOperation::Union, std::min(first, second), std::max(first, second));
}

static constexpr bool idSetChunkLiesInScratch(uint32_t chunk) { return idSetHoldsOneMember(chunk); }
static constexpr uint32_t idSetChunkInScratch(size_t position) { return idSetOfOne(static_cast<uint32_t>(position)); }

static const IDSetChunkEntry* findIDSetChunkEntry(std::span<const IDSetChunkEntry> entries, size_t& from, uint32_t number)
{
    from = firstIDSetChunkEntryNotBelow(entries, from, number);
    return from < entries.size() && entries[from].firstMemberDividedBySize == number ? &entries[from] : nullptr;
}

static void addToIDSetChunk(IDSetChunk& chunk, const IDSetChunk& added)
{
    for (size_t word = 0; word < chunk.bits.size(); ++word)
        chunk.bits[word] |= added.bits[word];
}

static bool idSetChunkIsSubsetOf(const IDSetChunk& chunk, const IDSetChunk& other)
{
    uint64_t outside = 0;
    for (size_t word = 0; word < chunk.bits.size(); ++word)
        outside |= chunk.bits[word] & ~other.bits[word];
    return !outside;
}

static bool idSetContainsAnyChunkOf(std::span<const IDSetChunkEntry> entries, std::span<const IDSetChunkEntry> chunksSortedByNumberThenID)
{
    std::span<const IDSetChunkEntry> chunks = chunksSortedByNumberThenID;
    size_t entry = 0;
    size_t chunk = 0;
    while (entry < entries.size() && chunk < chunks.size()) {
        uint32_t number = entries[entry].firstMemberDividedBySize;
        chunk = firstIDSetChunkEntryNotBelow(chunks, chunk, number);
        if (chunk == chunks.size())
            return false;
        if (chunks[chunk].firstMemberDividedBySize != number) {
            entry = firstIDSetChunkEntryNotBelow(entries, entry, chunks[chunk].firstMemberDividedBySize);
            continue;
        }
        size_t endOfNumber = firstIDSetChunkEntryNotBelow(chunks, chunk, number + 1);
        if (std::ranges::binary_search(chunks.subspan(chunk, endOfNumber - chunk), entries[entry].chunk, { }, &IDSetChunkEntry::chunk))
            return true;
        chunk = endOfNumber;
        ++entry;
    }
    return false;
}

static void prefetchIDSetChunk(const IDSetChunk& chunk)
{
    constexpr size_t wordsInCacheLine = 64 / sizeof(uint64_t);
    for (size_t word = 0; word < chunk.bits.size(); word += wordsInCacheLine)
        __builtin_prefetch(&chunk.bits[word]);
}

static void sortIDSetChunksByNumberThenID(Vector<IDSetChunkEntry>& chunks, Vector<IDSetChunkEntry>& sortBuffer, Vector<uint32_t>& offsetsByChunkNumber)
{
    auto keyOf = [](const IDSetChunkEntry& entry) {
        return static_cast<uint64_t>(entry.firstMemberDividedBySize) << 32 | entry.chunk;
    };
    if (std::ranges::is_sorted(chunks, { }, keyOf))
        return;

    auto [least, greatest] = std::ranges::minmax(chunks, { }, &IDSetChunkEntry::firstMemberDividedBySize);
    uint32_t leastNumber = least.firstMemberDividedBySize;
    size_t rangeOfNumbers = static_cast<size_t>(greatest.firstMemberDividedBySize - leastNumber) + 1;
    if (rangeOfNumbers > chunks.size()) {
        std::ranges::sort(chunks, { }, keyOf);
        return;
    }

    offsetsByChunkNumber.fill(0u, rangeOfNumbers);
    for (const IDSetChunkEntry& entry : chunks)
        ++offsetsByChunkNumber[entry.firstMemberDividedBySize - leastNumber];
    uint32_t offset = 0;
    for (uint32_t& count : offsetsByChunkNumber)
        offset += std::exchange(count, offset);
    sortBuffer.grow(chunks.size());
    for (const IDSetChunkEntry& entry : chunks)
        sortBuffer[offsetsByChunkNumber[entry.firstMemberDividedBySize - leastNumber]++] = entry;
    std::swap(chunks, sortBuffer);
    sortBuffer.shrink(0);

    std::span<IDSetChunkEntry> sortedByNumber = chunks.mutableSpan();
    size_t begin = 0;
    for (uint32_t end : offsetsByChunkNumber) {
        if (end - begin > 1)
            std::ranges::sort(sortedByNumber.subspan(begin, end - begin), { }, &IDSetChunkEntry::chunk);
        begin = end;
    }
    offsetsByChunkNumber.shrink(0);
}

uint32_t IDSetArena::unionOf(std::span<const uint32_t> sets, std::span<const uint32_t> earlierStates)
{
    RELEASE_ASSERT(earlierStates.empty() || earlierStates.size() == sets.size());
    Vector<uint64_t, 16> pairs;
    for (size_t index = 0; index < sets.size(); ++index) {
        if (sets[index] != emptyIDSet)
            pairs.append(static_cast<uint64_t>(sets[index]) << 32 | (earlierStates.empty() ? emptyIDSet : earlierStates[index]));
    }
    static_assert(emptyIDSet == UINT32_MAX);
    std::ranges::sort(pairs, std::greater<>());
    Vector<uint32_t, 16> operands;
    Vector<uint32_t, 16> earlierStatesOfOperands;
    bool someOperandHasEarlierState = false;
    for (uint64_t pair : pairs) {
        uint32_t set = static_cast<uint32_t>(pair >> 32);
        if (!operands.isEmpty() && operands.last() == set)
            continue;
        operands.append(set);
        earlierStatesOfOperands.append(static_cast<uint32_t>(pair));
        someOperandHasEarlierState |= earlierStatesOfOperands.last() != emptyIDSet;
    }
    if (operands.isEmpty())
        return emptyIDSet;
    if (operands.size() == 1)
        return operands[0];
    if (operands.size() > 2)
        return unionOfDistinct(operands.span(), earlierStatesOfOperands.span());
    if (!someOperandHasEarlierState)
        return compute(IDSetOperation::Union, operands[1], operands[0]);
    if (std::optional<uint32_t> answer = remembered(Question::Union, operands[1], operands[0]))
        return *answer;
    uint32_t result = unionOfDistinct(operands.span(), earlierStatesOfOperands.span());
    remember(Question::Union, operands[1], operands[0], result);
    return result;
}

ALWAYS_INLINE_INTO_OTHER_FILES void IDSetArena::forgetEarlierStates()
{
    if (m_earlierStatesCompared.isEmpty())
        return;
    m_earlierStatesCompared.shrink(0);
    m_entriesSinceEarlierStates.shrink(0);
    if (m_epochOfEarlierStates == UINT32_MAX) {
        m_indexOfEarlierStatesCompared.fill(0u);
        m_epochOfEarlierStates = 0;
    }
    ++m_epochOfEarlierStates;
}

std::span<const IDSetChunkEntry> IDSetArena::entriesSince(uint32_t set, uint32_t earlierState)
{
    auto hashOf = [](uint32_t set, uint32_t earlierState) {
        return static_cast<uint32_t>(mixBitsForIDSets(static_cast<uint64_t>(set) << 32 | earlierState));
    };
    uint32_t hash = hashOf(set, earlierState);
    std::optional<uint32_t> position = findInEpochIndex(m_indexOfEarlierStatesCompared, m_epochOfEarlierStates, hash, [&](uint32_t candidate) {
        return m_earlierStatesCompared[candidate].set == set && m_earlierStatesCompared[candidate].earlierState == earlierState;
    });
    if (!position) {
        uint32_t first = static_cast<uint32_t>(m_entriesSinceEarlierStates.size());
        std::span<const IDSetChunkEntry> earlierEntries = viewOf(earlierState).chunkEntries();
        size_t from = 0;
        for (const IDSetChunkEntry& entry : viewOf(set).chunkEntries()) {
            const IDSetChunkEntry* earlierEntry = findIDSetChunkEntry(earlierEntries, from, entry.firstMemberDividedBySize);
            if (!earlierEntry || earlierEntry->chunk != entry.chunk)
                m_entriesSinceEarlierStates.append(entry);
        }
        position = static_cast<uint32_t>(m_earlierStatesCompared.size());
        m_earlierStatesCompared.append(EntriesSinceEarlierState { set, earlierState, first, static_cast<uint32_t>(m_entriesSinceEarlierStates.size()) - first });
        addToEpochIndex(m_indexOfEarlierStatesCompared, m_epochOfEarlierStates, hash, *position, [&](uint32_t earlier) {
            return hashOf(m_earlierStatesCompared[earlier].set, m_earlierStatesCompared[earlier].earlierState);
        });
    }
    const EntriesSinceEarlierState& compared = m_earlierStatesCompared[*position];
    return m_entriesSinceEarlierStates.subspan(compared.first, compared.count);
}

uint32_t IDSetArena::unionOfDistinct(std::span<const uint32_t> operands, std::span<const uint32_t> earlierStates)
{
    RELEASE_ASSERT(!m_depthOfInterning);
    uint32_t largest = emptyIDSet;
    IDSetView viewOfLargest;
    Vector<uint32_t, 16> wideOperands;
    Vector<uint32_t, 16> earlierStatesOfWideOperands;
    size_t firstInScratch = m_scratch.size();
    for (size_t index = 0; index < operands.size(); ++index) {
        uint32_t operand = operands[index];
        IDSetView view = viewOf(operand);
        if (view.size() > viewOfLargest.size()) {
            largest = operand;
            viewOfLargest = view;
        }
        if (view.isList())
            m_scratch.append(view.list());
        else {
            wideOperands.append(operand);
            earlierStatesOfWideOperands.append(earlierStates[index]);
        }
    }
    uint32_t sizeOfLargest = viewOfLargest.size();
    std::span<uint32_t> listed = m_scratch.mutableSpan().subspan(firstInScratch);
    std::ranges::sort(listed);
    listed = listed.first(std::ranges::unique(listed).begin() - listed.begin());
    IDSetView listedMembers = viewOfList(listed);

    if (wideOperands.isEmpty()) {
        uint32_t result = listed.size() == sizeOfLargest ? largest : intern(listedMembers, IDSetOperation::Union, operands);
        m_scratch.shrink(firstInScratch);
        return result;
    }

    ASSERT(sizeOfLargest > IDSetTable::mostMembersOfList);
    UnionScratch& scratch = m_unionScratch;
    std::span<const IDSetChunkEntry> entriesOfLargest = viewOfLargest.chunkEntries();
    bool memoizesSubsets = operands.size() > 2;

    collectCandidateChunks(scratch, largest, entriesOfLargest, wideOperands.span(), earlierStatesOfWideOperands.span(), listedMembers, memoizesSubsets);
    m_scratch.shrink(firstInScratch);
    uint32_t size = sizeOfLargest + uniteCandidateChunks(scratch, entriesOfLargest, memoizesSubsets);
    ASSERT(scratch.changedEntries.isEmpty() == (size == sizeOfLargest));

    for (uint32_t operand : scratch.operandsWithCandidates) {
        if (!idSetContainsAnyChunkOf(viewOf(operand).chunkEntries(), scratch.chunksNotInLargest.span()))
            rememberSubset(operand, largest);
    }

    uint32_t result = scratch.changedEntries.isEmpty() ? largest : internUnion(scratch, entriesOfLargest, size, operands);

    scratch.candidateChunks.shrink(0);
    scratch.lastCandidateByEntry.shrink(0);
    scratch.operandsWithCandidates.shrink(0);
    scratch.chunksNotInLargest.shrink(0);
    scratch.changedEntries.shrink(0);
    scratch.resultEntries.shrink(0);
    scratch.chunks.shrink(0);
    if (idSetLiesInThread(result)) {
        noteSetOfProgramBehind(result, largest);
        for (uint32_t operand : wideOperands)
            noteSetOfProgramBehind(result, operand);
    }
#if ASSERT_ENABLED
    for (uint32_t operand : operands)
        ASSERT(Typed::isSubset(viewOf(operand), viewOf(result)));
#endif
    return result;
}

void IDSetArena::collectCandidateChunks(UnionScratch& scratch, uint32_t largest, std::span<const IDSetChunkEntry> entriesOfLargest, std::span<const uint32_t> wideOperands, std::span<const uint32_t> earlierStatesOfWideOperands, const IDSetView& listedMembers, bool memoizesSubsets)
{
    bool hasSeveralWideOperands = wideOperands.size() > 2;
    if (hasSeveralWideOperands)
        scratch.lastCandidateByEntry.fill(emptyIDSet, entriesOfLargest.size());
    auto addCandidate = [&](size_t& from, IDSetChunkEntry entry) {
        const IDSetChunkEntry* entryOfLargest = findIDSetChunkEntry(entriesOfLargest, from, entry.firstMemberDividedBySize);
        if (entryOfLargest && entryOfLargest->chunk == entry.chunk)
            return false;
        if (!entryOfLargest || !hasSeveralWideOperands || std::exchange(scratch.lastCandidateByEntry[from], entry.chunk) != entry.chunk)
            scratch.candidateChunks.append(entry);
        return true;
    };

    for (size_t index = 0; index < wideOperands.size(); ++index) {
        uint32_t operand = wideOperands[index];
        if (operand == largest)
            continue;
        if (memoizesSubsets && isRememberedSubset(operand, largest))
            continue;
        std::span<const IDSetChunkEntry> entries = viewOf(operand).chunkEntries();
        size_t numberOfEntries = entries.size();
        if (uint32_t earlierState = earlierStatesOfWideOperands[index]; earlierState != emptyIDSet && !viewOf(earlierState).isList())
            entries = entriesSince(operand, earlierState);
        bool hasCandidates = false;
        size_t from = 0;
        for (const IDSetChunkEntry& entry : entries)
            hasCandidates |= addCandidate(from, entry);
        if (!memoizesSubsets || entries.size() != numberOfEntries)
            continue;
        if (hasCandidates)
            scratch.operandsWithCandidates.append(operand);
        else
            rememberSubset(operand, largest);
    }

    size_t from = 0;
    for (IDSetChunkCursor cursor(listedMembers); !cursor.isAtEnd(); cursor.advance()) {
        addCandidate(from, IDSetChunkEntry { cursor.number(), idSetChunkInScratch(scratch.chunks.size()) });
        scratch.chunks.append(cursor.chunk());
    }
    sortIDSetChunksByNumberThenID(scratch.candidateChunks, scratch.sortBuffer, scratch.offsetsByChunkNumber);
}

uint32_t IDSetArena::uniteCandidateChunks(UnionScratch& scratch, std::span<const IDSetChunkEntry> entriesOfLargest, bool memoizesSubsets) const
{
    std::span<const IDSetChunkEntry> candidates = scratch.candidateChunks.span();
    IDSetView viewOfTableAndArena = viewOfList({ });
    auto resolveChunk = [&](uint32_t chunk) -> const IDSetChunk& {
        return idSetChunkLiesInScratch(chunk) ? scratch.chunks[positionOfIDSet(chunk)] : viewOfTableAndArena.chunkAt(chunk);
    };

    constexpr size_t prefetchDistance = 6;
    size_t fromForPrefetch = 0;
    auto prefetchChunksAt = [&](size_t index) {
        if (index >= candidates.size())
            return;
        const IDSetChunkEntry& candidate = candidates[index];
        bool beginsNumber = !index || candidates[index - 1].firstMemberDividedBySize != candidate.firstMemberDividedBySize;
        if (beginsNumber || candidates[index - 1].chunk != candidate.chunk)
            prefetchIDSetChunk(resolveChunk(candidate.chunk));
        if (!beginsNumber)
            return;
        if (const IDSetChunkEntry* entryOfLargest = findIDSetChunkEntry(entriesOfLargest, fromForPrefetch, candidate.firstMemberDividedBySize))
            prefetchIDSetChunk(resolveChunk(entryOfLargest->chunk));
    };
    for (size_t index = 0; index < prefetchDistance; ++index)
        prefetchChunksAt(index);

    uint32_t numberOfMembersGained = 0;
    size_t from = 0;
    Vector<const IDSetChunk*, 8> chunksOfNumber;
    for (size_t index = 0; index < candidates.size();) {
        uint32_t number = candidates[index].firstMemberDividedBySize;
        const IDSetChunkEntry* entryOfLargest = findIDSetChunkEntry(entriesOfLargest, from, number);
        const IDSetChunk* chunkOfLargest = entryOfLargest ? &resolveChunk(entryOfLargest->chunk) : nullptr;
        IDSetChunk united = chunkOfLargest ? *chunkOfLargest : IDSetChunk { };
        size_t first = index;
        chunksOfNumber.shrink(0);
        for (; index < candidates.size() && candidates[index].firstMemberDividedBySize == number; ++index) {
            prefetchChunksAt(index + prefetchDistance);
            bool isRepeat = index > first && candidates[index].chunk == candidates[index - 1].chunk;
            chunksOfNumber.append(isRepeat ? nullptr : &resolveChunk(candidates[index].chunk));
            if (!isRepeat)
                addToIDSetChunk(united, *chunksOfNumber.last());
        }
        if (chunkOfLargest && united == *chunkOfLargest)
            continue;

        numberOfMembersGained += numberOfMembersInIDSetChunk(united) - (chunkOfLargest ? numberOfMembersInIDSetChunk(*chunkOfLargest) : 0);
        uint32_t chunkOfUnion = emptyIDSet;
        for (size_t place = chunksOfNumber.size(); place-- && chunkOfUnion == emptyIDSet;) {
            if (chunksOfNumber[place] && *chunksOfNumber[place] == united)
                chunkOfUnion = candidates[first + place].chunk;
        }
        if (chunkOfUnion == emptyIDSet) {
            chunkOfUnion = idSetChunkInScratch(scratch.chunks.size());
            scratch.chunks.append(united);
        }
        scratch.changedEntries.append(IDSetChunkEntry { number, chunkOfUnion });

        for (size_t place = 0; memoizesSubsets && place < chunksOfNumber.size(); ++place) {
            const IDSetChunkEntry& candidate = candidates[first + place];
            if (chunksOfNumber[place] && !idSetChunkLiesInScratch(candidate.chunk) && !(chunkOfLargest && idSetChunkIsSubsetOf(*chunksOfNumber[place], *chunkOfLargest)))
                scratch.chunksNotInLargest.append(candidate);
        }
    }
    return numberOfMembersGained;
}

uint32_t IDSetArena::internUnion(UnionScratch& scratch, std::span<const IDSetChunkEntry> entriesOfLargest, uint32_t size, std::span<const uint32_t> operands)
{
    size_t numberCopied = 0;
    for (IDSetChunkEntry entry : scratch.changedEntries) {
        if (idSetChunkLiesInScratch(entry.chunk))
            entry.chunk = internChunk(scratch.chunks[positionOfIDSet(entry.chunk)], IDSetOperation::Union);
        size_t place = numberCopied;
        bool replacesEntry = !!findIDSetChunkEntry(entriesOfLargest, place, entry.firstMemberDividedBySize);
        scratch.resultEntries.append(entriesOfLargest.subspan(numberCopied, place - numberCopied));
        scratch.resultEntries.append(entry);
        numberCopied = place + replacesEntry;
    }
    scratch.resultEntries.append(entriesOfLargest.subspan(numberCopied));
    return intern(viewOfEntries(scratch.resultEntries.span(), size), IDSetOperation::Union, operands);
}

bool IDSetArena::isRememberedSubset(uint32_t set, uint32_t of)
{
    return remembered(Question::Union, std::min(set, of), std::max(set, of)) == of;
}

void IDSetArena::rememberSubset(uint32_t set, uint32_t of)
{
    remember(Question::Union, std::min(set, of), std::max(set, of), of);
}

uint32_t IDSetArena::intersectionOf(uint32_t first, uint32_t second)
{
    if (first == second)
        return first;
    if (first == emptyIDSet || second == emptyIDSet)
        return emptyIDSet;
    if (idSetHoldsOneMember(second))
        std::swap(first, second);
    if (idSetHoldsOneMember(first))
        return viewOf(second).contains(positionOfIDSet(first)) ? first : emptyIDSet;
    return compute(IDSetOperation::Intersection, std::min(first, second), std::max(first, second));
}

uint32_t IDSetArena::differenceOf(uint32_t first, uint32_t second)
{
    if (first == second || first == emptyIDSet)
        return emptyIDSet;
    if (second == emptyIDSet)
        return first;
    if (idSetHoldsOneMember(first))
        return viewOf(second).contains(positionOfIDSet(first)) ? emptyIDSet : first;
    return compute(IDSetOperation::Difference, first, second);
}

bool IDSetArena::isSubset(uint32_t set, uint32_t of)
{
    if (set == emptyIDSet || set == of)
        return true;
    if (of == emptyIDSet || idSetHoldsOneMember(of))
        return false;
    if (idSetHoldsOneMember(set))
        return viewOf(of).contains(positionOfIDSet(set));
    if (std::optional<uint32_t> answer = remembered(Question::Subset, set, of))
        return *answer;
    bool answer = Typed::isSubset(viewOf(set), viewOf(of));
    remember(Question::Subset, set, of, answer);
    return answer;
}

bool IDSetArena::overlap(uint32_t first, uint32_t second)
{
    if (first == emptyIDSet || second == emptyIDSet)
        return false;
    if (first == second)
        return true;
    if (idSetHoldsOneMember(second))
        std::swap(first, second);
    if (idSetHoldsOneMember(first))
        return viewOf(second).contains(positionOfIDSet(first));
    if (first > second)
        std::swap(first, second);
    if (std::optional<uint32_t> answer = remembered(Question::Overlap, first, second))
        return *answer;
    bool answer = Typed::overlap(viewOf(first), viewOf(second));
    remember(Question::Overlap, first, second, answer);
    return answer;
}

uint32_t IDSetArena::filtered(uint32_t set, NOESCAPE const ScopedLambda<bool(uint32_t)>& keeps)
{
    RELEASE_ASSERT(!m_depthOfInterning);
    IDSetView view = viewOf(set);
    bool collectsKeptMembers = view.isList();
    size_t first = m_scratch.size();
    view.forEach([&](uint32_t member) {
        if (keeps(member) == collectsKeptMembers)
            m_scratch.append(member);
    });
    uint32_t numberCollected = static_cast<uint32_t>(m_scratch.size() - first);

    uint32_t result;
    if (numberCollected == (collectsKeptMembers ? view.size() : 0))
        result = set;
    else if (collectsKeptMembers)
        result = makeFromSorted(m_scratch.subspan(first));
    else {
        result = internCombination(IDSetOperation::Difference, view, viewOfList(m_scratch.subspan(first)), view.size() - numberCollected, std::nullopt, emptyIDSet, emptyIDSet);
        noteSetOfProgramBehind(result, set);
    }
    m_scratch.shrink(first);
    return result;
}

uint32_t IDSetArena::replaced(uint32_t set, NOESCAPE const ScopedLambda<uint32_t(uint32_t)>& replacement)
{
    RELEASE_ASSERT(!m_depthOfInterning);
    size_t first = m_scratch.size();
    bool isChanged = false;
    viewOf(set).forEach([&](uint32_t member) {
        uint32_t newMember = replacement(member);
        isChanged |= newMember != member;
        m_scratch.append(newMember);
    });
    if (!isChanged) {
        m_scratch.shrink(first);
        return set;
    }
    return makeFromEndOfScratch(first);
}

uint32_t IDSetArena::intern(const IDSetView& members, std::optional<IDSetOperation> operation, uint32_t firstOperand, uint32_t secondOperand)
{
    std::array<uint32_t, 2> operands { firstOperand, secondOperand };
    return intern(members, operation, operands);
}

uint32_t IDSetArena::intern(const IDSetView& members, std::optional<IDSetOperation> operation, std::span<const uint32_t> operands)
{
    ASSERT(isWellFormedIDSet(members));
    if (members.isEmpty())
        return emptyIDSet;
    if (members.size() == 1)
        return idSetOfOne(members.list()[0]);

    bool hasChunks = members.size() > IDSetTable::mostMembersOfList;
    if (hasChunks && members.isList()) {
        RELEASE_ASSERT(!m_depthOfInterning);
        size_t first = m_scratchEntries.size();
        for (IDSetChunkCursor cursor(members); !cursor.isAtEnd(); cursor.advance())
            m_scratchEntries.append(IDSetChunkEntry { cursor.number(), internChunk(cursor.chunk(), operation) });
        uint32_t result = intern(viewOfEntries(m_scratchEntries.subspan(first), members.size()), operation, operands);
        m_scratchEntries.shrink(first);
        return result;
    }
    RELEASE_ASSERT(hasChunks == !members.isList());

    SetForScope holdsMembers(m_depthOfInterning, m_depthOfInterning + 1);
    std::span<const uint32_t> words = wordsOfIDSetMembers(members);
    uint32_t hash = hashOfIDSetWords(words);
    uint32_t numberOfAttachedWords = m_table.numberOfAttachedWords();
    auto addToTable = [&](std::span<const uint32_t> attachedWords) {
        bool isNew;
        uint32_t set = m_table.findOrAdd(hash, members, attachedWords, isNew);
        if (isNew) {
            ++m_additions.sets[makerInIDSetAdditions(operation)];
            m_additions.wordsOfSets[makerInIDSetAdditions(operation)] += words.size();
        }
        return set;
    };

    switch (m_keeps) {
    case ArenaKeeps::InProgram:
        if (!numberOfAttachedWords && !m_table.isFrozen())
            return addToTable({ });
        if (std::optional<uint32_t> found = m_table.find(hash, members))
            return *found;
        break;
    case ArenaKeeps::UntilReset: {
        bool mayLieInTable = std::ranges::none_of(members.chunkEntries(), [](const IDSetChunkEntry& entry) {
            return idSetLiesInThread(entry.chunk);
        });
        if (mayLieInTable) {
            if (std::optional<uint32_t> found = m_table.find(hash, members))
                return *found;
        }
        std::optional<uint32_t> known = findInEpochIndex(m_index, m_epoch, hash, [&](uint32_t position) {
            IDSetView record = viewOf(idSetInThread(position));
            return record.isList() == members.isList() && equalSpans(wordsOfIDSetMembers(record), words);
        });
        if (known)
            return idSetInThread(*known);
        break;
    }
    }

    Vector<uint32_t, 8> attachedWords;
    attachedWords.fill(0u, numberOfAttachedWords);
    if (m_annotator && numberOfAttachedWords) {
        bool isLongAndDense = members.size() > membersPerSetChunk && members.size() / members.m_numberOfChunks > IDSetTable::mostMembersOfList;
        if (isLongAndDense && m_keeps == ArenaKeeps::UntilReset && operation != IDSetOperation::Union)
            annotateChunkByChunk(members, attachedWords.mutableSpan());
        else
            m_annotator->annotate(members, operation, operands, attachedWords.mutableSpan());
    }

    if (m_keeps == ArenaKeeps::InProgram)
        return addToTable(attachedWords.span());

    ASSERT(m_table.takesNoAdditions());
    uint32_t position = static_cast<uint32_t>(m_firstWordOfRecord.size());
    uint32_t set = idSetInThread(position);
    makeRoomKeepingOutgrownBuffer(m_words, m_outgrownWords, wordsOfIDSetHeader + numberOfAttachedWords + words.size());
    m_firstWordOfRecord.append(static_cast<uint32_t>(m_words.size()));
    m_words.append(members.size());
    m_words.append(members.m_numberOfChunks);
    m_words.append(attachedWords.span());
    m_words.append(words);
    addToEpochIndex(m_index, m_epoch, hash, position, [&](uint32_t earlier) {
        return hashOfIDSetWords(wordsOfIDSetMembers(viewOf(idSetInThread(earlier))));
    });
    return set;
}

uint32_t IDSetArena::internChunk(const IDSetChunk& chunk, std::optional<IDSetOperation> operation)
{
    uint32_t hash = hashOfIDSetChunk(chunk);
    switch (m_keeps) {
    case ArenaKeeps::InProgram:
        if (!m_table.isFrozen()) {
            bool isNew;
            uint32_t id = m_table.findOrAddChunk(hash, chunk, isNew);
            m_additions.chunks[makerInIDSetAdditions(operation)] += isNew;
            return id;
        }
        break;
    case ArenaKeeps::UntilReset:
        break;
    }

    if (std::optional<uint32_t> found = m_table.findChunk(hash, chunk))
        return *found;
    RELEASE_ASSERT(m_keeps == ArenaKeeps::UntilReset);
    std::optional<uint32_t> known = findInEpochIndex(m_chunkIndex, m_epoch, hash, [&](uint32_t position) {
        return m_chunks[position] == chunk;
    });
    if (known)
        return idSetInThread(*known);

    ASSERT(m_table.takesNoAdditions());
    uint32_t position = static_cast<uint32_t>(m_chunks.size());
    uint32_t encodedChunk = idSetInThread(position);
    makeRoomKeepingOutgrownBuffer(m_chunks, m_outgrownChunks, 1);
    m_chunks.append(chunk);
    addToEpochIndex(m_chunkIndex, m_epoch, hash, position, [&](uint32_t earlier) {
        return hashOfIDSetChunk(m_chunks[earlier]);
    });
    return encodedChunk;
}

uint32_t IDSetArena::compute(IDSetOperation operation, uint32_t first, uint32_t second)
{
    static_assert(static_cast<uint8_t>(Question::Union) == static_cast<uint8_t>(IDSetOperation::Union));
    static_assert(static_cast<uint8_t>(Question::Intersection) == static_cast<uint8_t>(IDSetOperation::Intersection));
    static_assert(static_cast<uint8_t>(Question::Difference) == static_cast<uint8_t>(IDSetOperation::Difference));
    ASSERT(first != emptyIDSet && second != emptyIDSet && first != second);

    Question question = static_cast<Question>(operation);
    if (std::optional<uint32_t> answer = remembered(question, first, second))
        return *answer;

    IDSetView firstView = viewOf(first);
    IDSetView secondView = viewOf(second);
    uint32_t result;
    if (firstView.isList() && secondView.isList())
        result = combineLists(operation, first, firstView, second, secondView);
    else if (operation == IDSetOperation::Union) {
        std::array<uint32_t, 2> operands { first, second };
        std::array<uint32_t, 2> earlierStates { emptyIDSet, emptyIDSet };
        result = unionOfDistinct(operands, earlierStates);
    } else {
        uint32_t size = sizeOfCombinedIDSets(operation, firstView, secondView);
        if (size == firstView.size())
            result = first;
        else if (operation != IDSetOperation::Difference && size == secondView.size())
            result = second;
        else
            result = internCombination(operation, firstView, secondView, size, operation, first, second);
    }
    remember(question, first, second, result);
    noteSetOfProgramBehind(result, first);
    if (operation != IDSetOperation::Difference)
        noteSetOfProgramBehind(result, second);
    return result;
}

uint32_t IDSetArena::combineLists(IDSetOperation operation, uint32_t first, const IDSetView& firstView, uint32_t second, const IDSetView& secondView)
{
    RELEASE_ASSERT(!m_depthOfInterning);
    constexpr size_t sizeRatioForBinarySearch = 8;
    std::span<const uint32_t> x = firstView.list();
    std::span<const uint32_t> y = secondView.list();

    if (operation == IDSetOperation::Union && x.size() != y.size()) {
        bool firstIsLarger = x.size() > y.size();
        if (Typed::isSubset(firstIsLarger ? secondView : firstView, firstIsLarger ? firstView : secondView))
            return firstIsLarger ? first : second;
    }

    size_t firstInScratch = m_scratch.size();
    m_scratch.grow(firstInScratch + x.size() + y.size());
    std::span<uint32_t> room = m_scratch.mutableSpan().subspan(firstInScratch);
    size_t size = 0;
    auto appendFromSmaller = [&](std::span<const uint32_t> smaller, const IDSetView& larger, bool keepsMembersOfLarger) {
        for (uint32_t member : smaller) {
            if (larger.contains(member) == keepsMembersOfLarger)
                room[size++] = member;
        }
    };
    switch (operation) {
    case IDSetOperation::Union:
        size = std::ranges::set_union(x, y, room.begin()).out - room.begin();
        break;
    case IDSetOperation::Intersection:
        if (x.size() * sizeRatioForBinarySearch <= y.size())
            appendFromSmaller(x, secondView, true);
        else if (y.size() * sizeRatioForBinarySearch <= x.size())
            appendFromSmaller(y, firstView, true);
        else
            size = std::ranges::set_intersection(x, y, room.begin()).out - room.begin();
        break;
    case IDSetOperation::Difference:
        if (x.size() * sizeRatioForBinarySearch <= y.size())
            appendFromSmaller(x, secondView, false);
        else
            size = std::ranges::set_difference(x, y, room.begin()).out - room.begin();
        break;
    }

    uint32_t result;
    if (size == x.size())
        result = first;
    else if (operation != IDSetOperation::Difference && size == y.size())
        result = second;
    else
        result = intern(viewOfList(room.first(size)), operation, first, second);
    m_scratch.shrink(firstInScratch);
    return result;
}

uint32_t IDSetArena::internCombination(IDSetOperation operation, const IDSetView& first, const IDSetView& second, uint32_t size, std::optional<IDSetOperation> operationForAnnotator, uint32_t firstOperand, uint32_t secondOperand)
{
    RELEASE_ASSERT(operation != IDSetOperation::Union && !m_depthOfInterning);
    if (size <= IDSetTable::mostMembersOfList) {
        std::array<uint32_t, IDSetTable::mostMembersOfList> members;
        listCombinedIDSets(operation, first, second, std::span(members).first(size));
        return intern(viewOfList(std::span(members).first(size)), operationForAnnotator, firstOperand, secondOperand);
    }

    size_t firstEntry = m_scratchEntries.size();
    forEachChunkOfIDSets(operation, first, second, [&](IDSetChunkCursor* a, IDSetChunkCursor* b) {
        combineChunksWithSameNumber(operation, a, b, [&](IDSetChunkCursor& cursor) {
            m_scratchEntries.append(IDSetChunkEntry { cursor.number(), cursor.isInterned() ? cursor.id() : internChunk(cursor.chunk(), operationForAnnotator) });
        }, [&](uint32_t number, const IDSetChunk& chunk) {
            m_scratchEntries.append(IDSetChunkEntry { number, internChunk(chunk, operationForAnnotator) });
        });
    });
    uint32_t result = intern(viewOfEntries(m_scratchEntries.subspan(firstEntry), size), operationForAnnotator, firstOperand, secondOperand);
    m_scratchEntries.shrink(firstEntry);
    return result;
}

std::optional<uint32_t> IDSetArena::remembered(Question question, uint32_t first, uint32_t second)
{
    auto answerIn = [&](const Vector<MemoEntry>& memo, uint32_t epoch) -> std::optional<uint32_t> {
        if (memo.isEmpty())
            return std::nullopt;
        const MemoEntry& entry = memo[placeInIDSetMemo(static_cast<uint32_t>(question), first, second) & (memo.size() - 1)];
        if (entry.first != first || entry.second != second || entry.operationAndEpoch != stampInIDSetMemo(static_cast<uint32_t>(question), epoch))
            return std::nullopt;
        return entry.result;
    };
    if (!idSetLiesInThread(first) && !idSetLiesInThread(second)) {
        if (std::optional<uint32_t> answer = m_lastingMemo.find(static_cast<uint32_t>(question), first, second))
            return answer;
    }
    return answerIn(m_memoUntilReset, m_epoch);
}

void IDSetArena::remember(Question question, uint32_t first, uint32_t second, uint32_t answer)
{
    static_assert(static_cast<uint32_t>(Question::Overlap) < 1u << bitsOfQuestionInIDSetMemo);
    if (!idSetLiesInThread(first) && !idSetLiesInThread(second) && !idSetLiesInThread(answer)) {
        m_lastingMemo.add(static_cast<uint32_t>(question), first, second, answer);
        return;
    }
    if (m_memoUntilReset.isEmpty())
        m_memoUntilReset.fill(MemoEntry { }, entriesOfIDSetMemoUntilReset);
    m_memoUntilReset[placeInIDSetMemo(static_cast<uint32_t>(question), first, second) & (m_memoUntilReset.size() - 1)] = MemoEntry { first, second, answer, stampInIDSetMemo(static_cast<uint32_t>(question), m_epoch) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
