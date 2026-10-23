/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlow.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// Interned, immutable sets of members of the universe.
class ValueSets {
    WTF_MAKE_NONCOPYABLE(ValueSets);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueSets);
public:
    static constexpr ValueSetID nothing { 0u };
    static constexpr ValueSetID anything { 1u };
    static constexpr uint32_t membersPerBlock = 4096;
    static constexpr uint32_t mostHighMembersOfShortSet = 26;

    explicit ValueSets(uint32_t sizeOfUniverse);
    ~ValueSets();

    uint32_t sizeOfUniverse() const { return m_sizeOfUniverse; }

    static constexpr ValueSetID complement(ValueSetID set) { return ValueSetID(set.index() ^ 1u); }
    static constexpr bool isComplement(ValueSetID set) { return set.index() & 1u; }

    bool isEmpty(ValueSetID set) const { return !size(set); }
    uint32_t size(ValueSetID set) const
    {
        uint32_t stored = holdsOneMember(storedSetOf(set)) ? 1 : m_lists.at(storedSetOf(set)).numberOfMembers;
        return isComplement(set) ? m_sizeOfUniverse - stored : stored;
    }
    bool contains(ValueSetID set, ValueFlowObjectID member) const
    {
        uint32_t stored = storedSetOf(set);
        if (holdsOneMember(stored))
            return (onlyMemberOf(stored) == member.index()) != isComplement(set);
        if (!stored)
            return isComplement(set);
        const Run& run = m_lists.at(stored);
        if (member.index() < 64)
            return (run.lowestMembers >> member.index() & 1) != isComplement(set);
        return hasHighMember(run, member.index()) != isComplement(set);
    }
    uint64_t membersBelow64(ValueSetID set) const
    {
        uint32_t stored = storedSetOf(set);
        uint64_t members = !holdsOneMember(stored) ? m_lists.at(stored).lowestMembers : onlyMemberOf(stored) < 64 ? 1ull << onlyMemberOf(stored) : 0;
        if (!isComplement(set))
            return members;
        return ~members & (m_sizeOfUniverse < 64 ? (1ull << m_sizeOfUniverse) - 1 : ~0ull);
    }
    void prefetch(ValueSetID set) const
    {
        if (!holdsOneMember(storedSetOf(set)))
            __builtin_prefetch(&m_lists.at(storedSetOf(set)));
    }
    // Whether the two share a member. Makes no set.
    bool meet(ValueSetID first, ValueSetID second) const
    {
        if (first == nothing || second == nothing)
            return false;
        if (!isComplement(first) && holdsOneMember(storedSetOf(first)))
            return contains(second, ValueFlowObjectID(onlyMemberOf(storedSetOf(first))));
        if (!isComplement(second) && holdsOneMember(storedSetOf(second)))
            return contains(first, ValueFlowObjectID(onlyMemberOf(storedSetOf(second))));
        return setsOfSeveralMembersMeet(first, second);
    }
    bool isSubset(ValueSetID set, ValueSetID of) const
    {
        if (set == nothing || set == of)
            return true;
        return !meet(set, complement(of));
    }
    void forEachSetHolding(ValueFlowObjectID member, size_t numberOfSets, NOESCAPE const Invocable<ValueSetID(size_t)> auto& setAt, NOESCAPE const Invocable<void(size_t)> auto& functor) const
    {
        enum class Step : uint8_t { IsOut, IsIn, ReadsHead, ReadsGroup, ReadsBlock };
        constexpr size_t setsInBatch = 16;
        uint32_t index = member.index();
        uint32_t place = index / membersPerBlock;
        for (size_t first = 0; first < numberOfSets; first += setsInBatch) {
            size_t size = std::min(setsInBatch, numberOfSets - first);
            std::array<Step, setsInBatch> steps;
            std::array<bool, setsInBatch> isSkippedOrComplement;
            std::array<const Run*, setsInBatch> runs;
            for (size_t position = 0; position < size; ++position) {
                ValueSetID set = setAt(first + position);
                if (!set) {
                    steps[position] = Step::IsOut;
                    isSkippedOrComplement[position] = false;
                    continue;
                }
                uint32_t stored = storedSetOf(set);
                isSkippedOrComplement[position] = isComplement(set);
                if (holdsOneMember(stored) || !stored) {
                    steps[position] = stored && onlyMemberOf(stored) == index ? Step::IsIn : Step::IsOut;
                    continue;
                }
                runs[position] = &m_lists.at(stored);
                __builtin_prefetch(runs[position]);
                steps[position] = Step::ReadsHead;
            }
            for (size_t position = 0; position < size; ++position) {
                if (steps[position] != Step::ReadsHead)
                    continue;
                const Run& run = *runs[position];
                if (index < 64 || isShort(run)) {
                    steps[position] = (index < 64 ? run.lowestMembers >> index & 1 : hasHighMember(run, index)) ? Step::IsIn : Step::IsOut;
                    continue;
                }
                std::span<const uint64_t> list = run.span();
                uint32_t groupOfPlaces = place / placesPerGroup;
                size_t positionInList = std::min<size_t>(groupOfPlaces, list.size() - 1);
                while (positionInList && groupOfPlacesOf(list[positionInList]) > groupOfPlaces)
                    --positionInList;
                uint64_t entry = list[positionInList];
                if (groupOfPlacesOf(entry) != groupOfPlaces || (!isGroup(entry) && placeOf(entry) != place)) {
                    steps[position] = Step::IsOut;
                    continue;
                }
                runs[position] = isGroup(entry) ? &m_groups.at(groupOf(entry)) : &m_blocks.at(blockOf(entry));
                __builtin_prefetch(runs[position]);
                steps[position] = isGroup(entry) ? Step::ReadsGroup : Step::ReadsBlock;
            }
            for (size_t position = 0; position < size; ++position) {
                if (steps[position] != Step::ReadsGroup)
                    continue;
                const uint64_t* words = runs[position]->words();
                uint64_t bit = 1ull << place % placesPerGroup;
                if (!(words[0] & bit)) {
                    steps[position] = Step::IsOut;
                    continue;
                }
                runs[position] = &m_blocks.at(blocksOfGroup(words)[std::popcount(words[0] & (bit - 1))]);
                __builtin_prefetch(runs[position]);
                steps[position] = Step::ReadsBlock;
            }
            for (size_t position = 0; position < size; ++position) {
                if (steps[position] == Step::ReadsBlock) {
                    const uint64_t* words = runs[position]->words();
                    unsigned word = index % membersPerBlock / 64;
                    uint64_t present = words[0];
                    steps[position] = (present >> word & 1) && (words[1 + std::popcount(present & ((1ull << word) - 1))] >> index % 64 & 1) ? Step::IsIn : Step::IsOut;
                }
                if ((steps[position] == Step::IsIn) != isSkippedOrComplement[position])
                    functor(first + position);
            }
        }
    }

    void forEachSetMeeting(ValueSetID set, size_t numberOfSets, NOESCAPE const Invocable<ValueSetID(size_t)> auto& setAt, NOESCAPE const Invocable<void(size_t)> auto& functor) const
    {
        if (ValueFlowObjectID only = onlyMember(set)) {
            forEachSetHolding(only, numberOfSets, setAt, functor);
            return;
        }
        constexpr size_t setsInBatch = 16;
        for (size_t first = 0; first < numberOfSets; first += setsInBatch) {
            size_t size = std::min(setsInBatch, numberOfSets - first);
            std::array<ValueSetID, setsInBatch> sets;
            for (size_t position = 0; position < size; ++position) {
                sets[position] = setAt(first + position);
                if (sets[position])
                    prefetch(sets[position]);
            }
            for (size_t position = 0; position < size; ++position) {
                if (sets[position] && meet(set, sets[position]))
                    functor(first + position);
            }
        }
    }

    // How many members the two share. No set is made. Not for a complement.
    uint32_t numberOfCommonMembers(ValueSetID, ValueSetID) const;
    // None unless the set has exactly one member.
    ValueFlowObjectID onlyMember(ValueSetID set) const
    {
        if (isComplement(set)) [[unlikely]]
            return onlyMemberOfComplement(set);
        return holdsOneMember(storedSetOf(set)) ? ValueFlowObjectID(onlyMemberOf(storedSetOf(set))) : ValueFlowObjectID();
    }

    // Depends on the members alone: the same in every run and on any number of threads, which the numbers of sets are not. What has to come out in one order everywhere is ordered by this.
    uint64_t hashOfMembers(ValueSetID) const;

    // In ascending order.
    void forEachMember(ValueSetID set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const
    {
        if (isComplement(set)) {
            uint32_t next = 0;
            forEachStoredMember(set, [&](uint32_t member) {
                for (; next < member; ++next)
                    functor(ValueFlowObjectID(next));
                ++next;
            });
            for (; next < m_sizeOfUniverse; ++next)
                functor(ValueFlowObjectID(next));
            return;
        }
        forEachStoredMember(set, [&](uint32_t member) {
            functor(ValueFlowObjectID(member));
        });
    }

    // In ascending order, until the functor answers true. Not for a complement.
    bool anyMember(ValueSetID set, NOESCAPE const Invocable<bool(ValueFlowObjectID)> auto& functor) const
    {
        RELEASE_ASSERT(!isComplement(set));
        if (holdsOneMember(storedSetOf(set)))
            return functor(ValueFlowObjectID(onlyMemberOf(storedSetOf(set))));
        const Run& run = m_lists.at(storedSetOf(set));
        if (isShort(run)) {
            for (uint64_t bits = run.lowestMembers; bits; bits &= bits - 1) {
                if (functor(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(bits)))))
                    return true;
            }
            for (uint32_t member : highMembersOf(run)) {
                if (functor(ValueFlowObjectID(member)))
                    return true;
            }
            return false;
        }
        return anyStoredBlock(run.span(), [&](uint32_t place, uint32_t block) {
            const uint64_t* words = m_blocks.at(block).words();
            uint32_t firstOfBlock = place * membersPerBlock;
            const uint64_t* word = words + 1;
            for (uint64_t present = words[0]; present; present &= present - 1, ++word) {
                uint32_t firstOfWord = firstOfBlock + static_cast<uint32_t>(std::countr_zero(present)) * 64;
                for (uint64_t bits = *word; bits; bits &= bits - 1) {
                    if (functor(ValueFlowObjectID(firstOfWord + static_cast<uint32_t>(std::countr_zero(bits)))))
                        return true;
                }
            }
            return false;
        });
    }

    // The blocks of a set that is no complement, in ascending order of their places.
    void forEachBlock(ValueSetID set, NOESCAPE const Invocable<void(uint64_t nameOfBlock)> auto& functor) const
    {
        RELEASE_ASSERT(!isComplement(set));
        RunOfOneMember room;
        const Run& run = runOf(storedSetOf(set), room);
        if (isShort(run)) {
            uint32_t latest = UINT32_MAX;
            if (run.lowestMembers) {
                latest = 0;
                functor(entryOfGroup(0, storedSetOf(set)));
            }
            for (uint32_t member : highMembersOf(run)) {
                if (member / membersPerBlock == latest)
                    continue;
                latest = member / membersPerBlock;
                functor(entryOfGroup(latest, storedSetOf(set)));
            }
            return;
        }
        forEachStoredBlock(run.span(), [&](uint32_t place, uint32_t block) {
            functor(entryOfBlock(place, block));
        });
    }

    void forEachMemberOfBlock(uint64_t nameOfBlock, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const
    {
        if (isGroup(nameOfBlock)) {
            RunOfOneMember room;
            const Run& run = runOf(groupOf(nameOfBlock), room);
            if (!placeOf(nameOfBlock)) {
                for (uint64_t bits = run.lowestMembers; bits; bits &= bits - 1)
                    functor(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(bits))));
            }
            for (uint32_t member : highMembersOf(run)) {
                if (member / membersPerBlock == placeOf(nameOfBlock))
                    functor(ValueFlowObjectID(member));
            }
            return;
        }
        const uint64_t* words = m_blocks.at(blockOf(nameOfBlock)).words();
        uint32_t firstOfBlock = placeOf(nameOfBlock) * membersPerBlock;
        const uint64_t* word = words + 1;
        for (uint64_t present = words[0]; present; present &= present - 1, ++word) {
            uint32_t firstOfWord = firstOfBlock + static_cast<uint32_t>(std::countr_zero(present)) * 64;
            for (uint64_t bits = *word; bits; bits &= bits - 1)
                functor(ValueFlowObjectID(firstOfWord + static_cast<uint32_t>(std::countr_zero(bits))));
        }
    }

    // Up to four of the blocks of the set, and noBlock for the rest.
    static constexpr uint64_t noBlock = UINT64_MAX;
    std::array<uint64_t, 4> sampleOfBlocks(ValueSetID) const;

    size_t numberOfStoredSets() const { return m_lists.size(); }
    // A number for a table with an entry for each set of several members: no two of them have the same, and all lie below the end; that of a set of one member lies beyond it. They are not dense: see InternedRuns.
    static constexpr uint32_t indexInTableBySet(ValueSetID set) { return (set.index() >> 2) * 2 + (set.index() & 1); }
    uint32_t endOfTableBySet() const { return m_lists.endOfNumbers() + 2; }
    size_t numberOfStoredBlocks() const { return m_blocks.size(); }
    size_t memoryInBytes() const { return m_lists.memoryInBytes() + m_groups.memoryInBytes() + m_blocks.memoryInBytes() + (sizeof(RememberedByAll) << m_bitsOfRememberedByAll); }
    // While no thread makes a set.
    void freeOutgrownTables()
    {
        m_lists.freeOutgrownTables();
        m_groups.freeOutgrownTables();
        m_blocks.freeOutgrownTables();
    }

private:
    friend class ValueSetMaker;

    static constexpr unsigned wordsPerBlock = membersPerBlock / 64;

    // A run of 64-bit words that never changes: this head, and right behind it the words.
    struct Run {
        const uint64_t* words() const LIFETIME_BOUND { return reinterpret_cast<const uint64_t*>(this + 1); }
        std::span<const uint64_t> span() const LIFETIME_BOUND { return unsafeMakeSpan(words(), numberOfWords); }

        uint32_t numberOfWords { 0 };
        uint32_t numberOfMembers { 0 };
        uint64_t hashOfMembers { 0 };
        // Of a list and of a short set: the members below 64.
        uint64_t lowestMembers { 0 };
    };

    // Runs found again by their words.
    class InternedRuns {
        WTF_MAKE_NONCOPYABLE(InternedRuns);
    public:
        InternedRuns();
        ~InternedRuns();

        const Run& at(uint32_t number) const LIFETIME_BOUND
        {
            const char* segment = m_segments[number / unitsPerSegment].load(std::memory_order_acquire);
            return *reinterpret_cast<const Run*>(segment + static_cast<size_t>(number % unitsPerSegment) * bytesPerUnit);
        }

        struct Room {
            uint32_t nextUnit { 0 };
            uint32_t endOfChunk { 0 };
        };

        // Number 0 is the run without words.
        uint32_t intern(Room&, std::span<const uint64_t> words, uint32_t numberOfMembers, uint64_t hashOfMembers, uint64_t lowestMembers = 0);

        void freeOutgrownTables();

        size_t size() const;
        uint32_t endOfNumbers() const;
        size_t memoryInBytes() const;

    private:
        static constexpr unsigned numberOfShards = 64;
        static constexpr size_t bytesPerUnit = 16;
        static constexpr uint32_t unitsPerLine = 64 / bytesPerUnit;
        static constexpr uint32_t unitsOfHeadAndFirstWord = (sizeof(Run) + sizeof(uint64_t)) / bytesPerUnit;
        static constexpr uint32_t unitsPerSegment = 1u << 19;
        // A number has 30 bits.
        static constexpr uint32_t numberOfSegments = (1u << 30) / unitsPerSegment;
        static constexpr uint32_t unitsPerChunk = 4096;

        // A slot: the hash above, the number of the run below. A run that is there is found without the lock.
        using Slot = std::atomic<uint64_t>;

        // The slots lie at a cache line, so the low bits of their address are free for the binary logarithm of their number: one load gives both.
        class Table {
        public:
            Table()
                : m_bits(0)
            {
            }

            Table(Slot* slots, unsigned bits)
                : m_bits(reinterpret_cast<uintptr_t>(slots) | bits)
            {
            }

            Slot* slots() const { return reinterpret_cast<Slot*>(m_bits & ~static_cast<uintptr_t>(63)); }
            size_t numberOfSlots() const { return static_cast<size_t>(1) << (m_bits & 63); }
            size_t mask() const { return numberOfSlots() - 1; }

            friend bool operator==(Table, Table) = default;

        private:
            uintptr_t m_bits;
        };

        struct alignas(64) Shard {
            // Every lookup reads this line, and only a table that grows writes it.
            std::atomic<Table> table { };
            // Whoever adds a run writes this one.
            alignas(64) Lock lock;
            uint32_t size { 0 };
            Vector<Table> outgrownTables;
        };

        static Table createTable(unsigned bits);
        uint32_t find(Table, size_t& position, uint32_t hash, const Run& head, std::span<const uint64_t> words) const;
        uint32_t takeUnits(uint32_t numberOfUnits);
        uint32_t allocate(Room&, size_t numberOfWords);
        Run& mutableRunAt(uint32_t number) { return const_cast<Run&>(at(number)); }
        void grow(Shard&);

        std::array<Shard, numberOfShards> m_shards;
        Lock m_lockOfUnits;
        uint32_t m_nextUnit WTF_GUARDED_BY_LOCK(m_lockOfUnits) { 0 };
        std::atomic<uint32_t> m_endOfNumbers { 0 };
        std::array<std::atomic<char*>, numberOfSegments> m_segments { };
    };

    static constexpr uint32_t storedSetOf(ValueSetID set) { return set.index() >> 1; }
    // The number of a run has 30 bits.
    static constexpr uint32_t namesOneMember = 1u << 30;
    static constexpr bool holdsOneMember(uint32_t storedSet) { return storedSet & namesOneMember; }
    static constexpr uint32_t onlyMemberOf(uint32_t storedSet) { return storedSet & ~namesOneMember; }
    static constexpr uint32_t storedSetOfOneMember(uint32_t member) { return member | namesOneMember; }
    static constexpr uint32_t placesPerGroup = 64;
    // The number of a run has 30 bits.
    static constexpr uint32_t namesGroup = 1u << 31;

    static constexpr uint32_t placeOf(uint64_t entry) { return static_cast<uint32_t>(entry >> 32); }
    static constexpr uint32_t groupOfPlacesOf(uint64_t entry) { return placeOf(entry) / placesPerGroup; }
    static constexpr bool isGroup(uint64_t entry) { return entry & namesGroup; }
    static constexpr uint32_t blockOf(uint64_t entry) { return static_cast<uint32_t>(entry); }
    static constexpr uint32_t groupOf(uint64_t entry) { return static_cast<uint32_t>(entry) & ~namesGroup; }
    static constexpr uint64_t entryOfBlock(uint32_t place, uint32_t block) { return static_cast<uint64_t>(place) << 32 | block; }
    static constexpr uint64_t entryOfGroup(uint32_t firstPlace, uint32_t group) { return static_cast<uint64_t>(firstPlace) << 32 | namesGroup | group; }
    static const uint32_t* blocksOfGroup(const uint64_t* wordsOfGroup) { return reinterpret_cast<const uint32_t*>(wordsOfGroup + 1) + 1; }

    static uint32_t numberOfHighMembers(const Run& run) { return run.numberOfMembers - static_cast<uint32_t>(std::popcount(run.lowestMembers)); }
    static bool isShort(const Run& run) { return numberOfHighMembers(run) <= mostHighMembersOfShortSet; }
    static std::span<const uint32_t> highMembersOf(const Run& run LIFETIME_BOUND) { return unsafeMakeSpan(reinterpret_cast<const uint32_t*>(run.words()), numberOfHighMembers(run)); }

    struct RunOfOneMember {
        Run head;
        uint64_t word { 0 };
    };

    ALWAYS_INLINE const Run& runOf(uint32_t storedSet, RunOfOneMember& room LIFETIME_BOUND) const LIFETIME_BOUND
    {
        if (!holdsOneMember(storedSet))
            return m_lists.at(storedSet);
        uint32_t member = onlyMemberOf(storedSet);
        bool isLow = member < 64;
        room.head = Run { isLow ? 0u : 1u, 1, 0, isLow ? 1ull << member : 0 };
        memcpy(&room.word, &member, sizeof(member));
        return room.head;
    }

    bool hasHighMember(const Run&, uint32_t member) const;
    bool setsOfSeveralMembersMeet(ValueSetID, ValueSetID) const;
    ValueFlowObjectID onlyMemberOfComplement(ValueSetID) const;

    // What an entry stands for: the places of its group that have a block, and the numbers of those blocks in ascending order of their places.
    class BlocksOfEntry {
        WTF_MAKE_NONCOPYABLE(BlocksOfEntry);
    public:
        BlocksOfEntry(const ValueSets& sets, uint64_t entry)
        {
            if (isGroup(entry)) {
                const uint64_t* words = sets.m_groups.at(groupOf(entry)).words();
                present = words[0];
                blocks = blocksOfGroup(words);
                return;
            }
            m_block = blockOf(entry);
            present = 1ull << placeOf(entry) % placesPerGroup;
            blocks = &m_block;
        }

        uint32_t blockAt(uint64_t bitOfPlace) const { return blocks[std::popcount(present & (bitOfPlace - 1))]; }

        uint64_t present;
        const uint32_t* blocks;

    private:
        uint32_t m_block;
    };

    // In ascending order of the places, until the functor answers true.
    bool anyStoredBlock(std::span<const uint64_t> list, NOESCAPE const Invocable<bool(uint32_t place, uint32_t block)> auto& functor) const
    {
        for (uint64_t entry : list) {
            if (!isGroup(entry)) {
                if (functor(placeOf(entry), blockOf(entry)))
                    return true;
                continue;
            }
            const uint64_t* words = m_groups.at(groupOf(entry)).words();
            const uint32_t* block = blocksOfGroup(words);
            for (uint64_t present = words[0]; present; present &= present - 1, ++block) {
                if (functor(placeOf(entry) + static_cast<uint32_t>(std::countr_zero(present)), *block))
                    return true;
            }
        }
        return false;
    }

    void forEachStoredBlock(std::span<const uint64_t> list, NOESCAPE const Invocable<void(uint32_t place, uint32_t block)> auto& functor) const
    {
        anyStoredBlock(list, [&](uint32_t place, uint32_t block) {
            functor(place, block);
            return false;
        });
    }

    class MembersOfList {
        WTF_MAKE_NONCOPYABLE(MembersOfList);
    public:
        MembersOfList(const ValueSets& sets, std::span<const uint64_t> list)
            : m_sets(sets)
            , m_list(list)
        {
        }

        bool contains(uint32_t member)
        {
            uint32_t place = member / membersPerBlock;
            if (place != m_place) {
                m_place = place;
                uint32_t groupOfPlaces = place / placesPerGroup;
                if (groupOfPlaces != m_groupOfPlaces) {
                    m_groupOfPlaces = groupOfPlaces;
                    while (m_position < m_list.size() && groupOfPlacesOf(m_list[m_position]) < groupOfPlaces)
                        ++m_position;
                    m_present = 0;
                    if (m_position < m_list.size() && groupOfPlacesOf(m_list[m_position]) == groupOfPlaces) {
                        uint64_t entry = m_list[m_position];
                        if (isGroup(entry)) {
                            const uint64_t* words = m_sets.m_groups.at(groupOf(entry)).words();
                            m_present = words[0];
                            m_blocks = blocksOfGroup(words);
                        } else {
                            m_block = blockOf(entry);
                            m_present = 1ull << placeOf(entry) % placesPerGroup;
                            m_blocks = &m_block;
                        }
                    }
                }
                uint64_t bit = 1ull << place % placesPerGroup;
                m_words = m_present & bit ? m_sets.m_blocks.at(m_blocks[std::popcount(m_present & (bit - 1))]).words() : nullptr;
            }
            if (!m_words)
                return false;
            unsigned word = member % membersPerBlock / 64;
            uint64_t present = m_words[0];
            return (present >> word & 1) && (m_words[1 + std::popcount(present & ((1ull << word) - 1))] >> member % 64 & 1);
        }

    private:
        const ValueSets& m_sets;
        std::span<const uint64_t> m_list;
        size_t m_position { 0 };
        uint32_t m_groupOfPlaces { UINT32_MAX };
        uint32_t m_place { UINT32_MAX };
        uint32_t m_block { 0 };
        uint64_t m_present { 0 };
        const uint32_t* m_blocks { nullptr };
        const uint64_t* m_words { nullptr };
    };

    uint32_t blockAt(std::span<const uint64_t> list, uint32_t place) const;
    static size_t positionOfGroupOfPlaces(std::span<const uint64_t> list, size_t from, uint32_t groupOfPlaces);
    bool blocksMeet(uint32_t, uint32_t) const;
    bool storedSetsMeet(ValueSetID, ValueSetID) const;
    bool storedSetIsSubset(ValueSetID, ValueSetID of) const;

    void forEachStoredMember(ValueSetID set, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        if (holdsOneMember(storedSetOf(set))) {
            functor(onlyMemberOf(storedSetOf(set)));
            return;
        }
        const Run& run = m_lists.at(storedSetOf(set));
        if (isShort(run)) {
            for (uint64_t bits = run.lowestMembers; bits; bits &= bits - 1)
                functor(static_cast<uint32_t>(std::countr_zero(bits)));
            for (uint32_t member : highMembersOf(run))
                functor(member);
            return;
        }
        forEachStoredBlock(run.span(), [&](uint32_t place, uint32_t block) {
            const uint64_t* words = m_blocks.at(block).words();
            uint32_t firstOfBlock = place * membersPerBlock;
            const uint64_t* word = words + 1;
            for (uint64_t present = words[0]; present; present &= present - 1, ++word) {
                uint32_t firstOfWord = firstOfBlock + static_cast<uint32_t>(std::countr_zero(present)) * 64;
                for (uint64_t bits = *word; bits; bits &= bits - 1)
                    functor(firstOfWord + static_cast<uint32_t>(std::countr_zero(bits)));
            }
        });
    }

    struct RememberedByAll {
        std::atomic<uint64_t> resultAndCount { 0 };
        std::atomic<uint64_t> key { 0 };
    };

    RememberedByAll& rememberedByAllAt(uint64_t key) const LIFETIME_BOUND { return m_rememberedByAll[(key * 0x9e3779b97f4a7c15ull) >> (64 - m_bitsOfRememberedByAll)]; }

    ALWAYS_INLINE std::optional<uint32_t> findRememberedByAll(uint64_t key) const
    {
        const RememberedByAll& remembered = rememberedByAllAt(key);
        uint64_t resultAndCount = remembered.resultAndCount.load(std::memory_order_acquire);
        if ((resultAndCount & 1) || remembered.key.load(std::memory_order_relaxed) != key)
            return std::nullopt;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (remembered.resultAndCount.load(std::memory_order_relaxed) != resultAndCount)
            return std::nullopt;
        return static_cast<uint32_t>(resultAndCount >> 32);
    }

    ALWAYS_INLINE void rememberForAll(uint64_t key, uint32_t result) const
    {
        RememberedByAll& remembered = rememberedByAllAt(key);
        uint64_t before = remembered.resultAndCount.load(std::memory_order_relaxed);
        if ((before & 1) || !remembered.resultAndCount.compare_exchange_strong(before, before | 1, std::memory_order_acquire, std::memory_order_relaxed))
            return;
        std::atomic_thread_fence(std::memory_order_release);
        remembered.key.store(key, std::memory_order_relaxed);
        remembered.resultAndCount.store(static_cast<uint64_t>(result) << 32 | static_cast<uint32_t>(before + 2), std::memory_order_release);
    }

    const uint32_t m_sizeOfUniverse;
    const unsigned m_bitsOfRememberedByAll;
    const std::unique_ptr<RememberedByAll[]> m_rememberedByAll;
    InternedRuns m_blocks;
    InternedRuns m_groups;
    InternedRuns m_lists;
};

// What each placeholder stands for where a value passes a mask: the untracked object for what the analysis has stopped following, the unknown object for what has escaped.
struct ValueFlowCoveredObjects {
    ValueSetID byUntracked { ValueSets::anything };
    ValueSetID byUnknown { ValueSets::anything };
};

// One thread's way of making sets: its buffers and what it remembers of the operations it has done.
class ValueSetMaker {
    WTF_MAKE_NONCOPYABLE(ValueSetMaker);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueSetMaker);
public:
    explicit ValueSetMaker(ValueSets&);

    const ValueSets& sets() const { return m_sets; }

    ValueSetID of(ValueFlowObjectID member) { return ValueSetID(ValueSets::storedSetOfOneMember(member.index()) * 2); }
    // In any order, with or without repeats.
    ValueSetID of(std::span<const ValueFlowObjectID>);
    ValueSetID ofManyMembers(std::span<const ValueFlowObjectID>);
    // Every member from the first up to the one before `end`.
    ValueSetID ofRange(ValueFlowObjectID first, ValueFlowObjectID end);
    ValueSetID unite(ValueSetID first, ValueSetID second)
    {
        if (second == ValueSets::nothing || first == second)
            return first;
        if (first == ValueSets::nothing)
            return second;
        return ValueSets::complement(intersectDifferentSets(ValueSets::complement(first), ValueSets::complement(second)));
    }
    ValueSetID intersect(ValueSetID first, ValueSetID second)
    {
        if (second == ValueSets::anything || first == second)
            return first;
        if (first == ValueSets::anything)
            return second;
        if (first == ValueSets::nothing || second == ValueSets::nothing)
            return ValueSets::nothing;
        return intersectDifferentSets(first, second);
    }
    bool meet(ValueSetID first, ValueSetID second)
    {
        if (first == ValueSets::nothing || second == ValueSets::nothing)
            return false;
        return nonEmptySetsMeet(first, second);
    }
    // What a place with a mask takes of a value that arrives there.
    ValueSetID pass(ValueSetID value, ValueSetID mask, ValueSetID familyObjects, ValueFlowMaskKind, const ValueFlowPlaceholders&, const ValueFlowCoveredObjects& = { });
    ValueSetID uniteAll(std::span<const ValueSetID>);
    // The set as the union of smaller sets, in ascending order: its blocks, or the words of its one block.
    void appendParts(ValueSetID, Vector<ValueSetID>&);

private:
    enum class Operation : uint8_t { Unite, Intersect, Subtract };

    struct Remembered {
        // Of sets: the two operands of an intersection, in ascending order. Of blocks: the operation lies in the two highest bits of the second.
        uint32_t first { 0 };
        uint32_t second { 0 };
        uint32_t result { 0 };
    };

    struct RememberedEntries {
        uint32_t first { 0 };
        uint32_t second { 0 };
        // The operation and where in the group of places each of the two begins. Never zero.
        uint32_t detail { 0 };
        uint32_t placeOfResult { 0 };
        uint32_t result { 0 };
    };

    // What the members of some blocks add to the head of whatever holds the blocks.
    struct Totals {
        uint32_t numberOfMembers { 0 };
        uint64_t hashOfMembers { 0 };
    };

    static constexpr unsigned bitsOfRememberedSets = 13;
    static constexpr unsigned bitsOfRememberedEntries = 11;
    static constexpr unsigned bitsOfRememberedBlocks = 12;
    static constexpr uint32_t mostMembersOfSetWithoutParts = 16;
    static constexpr uint32_t mostMembersOfListGatheredFrom = 256;

    struct BitsOfWord {
        uint32_t word { 0 };
        uint64_t bits { 0 };
    };

    static constexpr uint32_t intersectionIsNotEmpty = UINT32_MAX - 1;
    ValueSetID intersectDifferentSets(ValueSetID, ValueSetID);
    ValueSetID intersectUncached(ValueSetID, ValueSetID, Remembered&);
    bool nonEmptySetsMeet(ValueSetID, ValueSetID);
    bool meetUncached(ValueSetID, ValueSetID, Remembered&);
    uint32_t combine(Operation, uint32_t first, uint32_t second);
    uint32_t combineByForm(Operation, uint32_t first, uint32_t second);
    uint32_t combineShortSets(Operation, uint32_t first, uint32_t second);
    uint32_t combineShortSetWithList(Operation, uint32_t first, uint32_t second);
    uint32_t combineLists(Operation, uint32_t first, uint32_t second);
    uint32_t uniteAllLists(std::span<const ValueSetID>);
    bool gatherShortResult(Operation, std::span<const uint64_t> first, std::span<const uint64_t> second);
    uint32_t changeList(uint32_t list, bool adds, uint64_t lowestMembers, std::span<const uint32_t> highMembers);
    uint32_t changeBlock(uint32_t block, bool adds, std::span<const BitsOfWord>);
    uint32_t internShortSet(uint64_t lowestMembers, std::span<const uint32_t> highMembers);
    uint32_t internShortSet(uint64_t lowestMembers, std::span<const uint32_t> highMembers, uint64_t hashOfHighMembers);
    uint32_t combineShortSetsOfUnlikeSizes(Operation, uint32_t first, const ValueSets::Run&, uint32_t second, const ValueSets::Run&);
    uint32_t internMembers(uint64_t lowestMembers, std::span<const uint32_t> highMembers);
    void makeRoomToScatter();
    void scatter(uint32_t member);
    void sortMembersAndRemoveRepeats();
    // Of two entries for the same group of places. Nothing is 0.
    uint64_t combineEntries(Operation, uint64_t first, uint64_t second);
    uint32_t combineBlocks(Operation, uint32_t first, uint32_t second);
    Totals totalsOfBlock(uint32_t place, uint32_t block) const;
    Totals totalsOf(uint64_t entry) const;
    // The entry for the blocks at some places of one group of places. Nothing is 0.
    uint64_t entryFor(uint32_t firstPlace, uint64_t present, std::span<const uint32_t> blocks, Totals);
    uint64_t entryFor(uint32_t firstPlace, uint64_t present, std::span<const uint32_t> blocks);
    uint32_t internList(std::span<const uint64_t>);
    uint32_t internList(std::span<const uint64_t>, Totals, uint64_t lowestMembers);
    // Of (place << 32 | number of the block), ascending.
    uint32_t internListOfBlocks(std::span<const uint64_t>);

    ValueSets& m_sets;
    ValueSets::InternedRuns::Room m_roomForBlocks;
    ValueSets::InternedRuns::Room m_roomForGroups;
    ValueSets::InternedRuns::Room m_roomForLists;
    std::array<Remembered, 1u << bitsOfRememberedSets> m_rememberedSets { };
    Vector<RememberedEntries> m_rememberedEntries;
    Vector<Remembered> m_rememberedBlocks;
    HashMap<uint64_t, ValueSetID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_setOfRange;
    uint32_t m_fullBlock { 0 };
    Vector<uint64_t> m_list;
    Vector<uint64_t> m_listOfBlocks;
    Vector<uint64_t> m_entryAtGroupOfPlaces;
    Vector<uint32_t> m_blockAtPlace;
    // The words that are present, one bit each, and the words. A word that is not present holds anything.
    struct GatheredBlock {
        uint64_t present { 0 };
        std::array<uint64_t, ValueSets::wordsPerBlock> words;
    };
    Vector<GatheredBlock> m_gatheredBlocks;
    Vector<uint32_t> m_members;
    Vector<uint32_t> m_highMembers;
    Vector<BitsOfWord> m_bitsOfWords;
    // For ofManyMembers(): a bit for each member of the universe, a bit for each word of a place that holds one, a bit for each place that has such a word. All zero between two calls.
    Vector<uint64_t> m_scatteredMembers;
    Vector<uint64_t> m_wordsTouchedAtPlace;
    Vector<uint64_t> m_placesTouched;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
