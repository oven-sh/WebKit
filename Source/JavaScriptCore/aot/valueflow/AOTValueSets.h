/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlow.h"
#include <array>
#include <atomic>
#include <bit>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Interned, immutable sets of members of the universe. Equal sets of one polarity have equal ids, so "is it the same set" is one comparison.
//
// An id is twice the number of a stored set, plus one if it stands for everything but that set. Stored set 0 is empty: id 0 is nothing, id 1 is anything.
//
// A stored set has two levels, both interned. A block covers 4,096 members; a set is a list of (place of the block, number of the block). Two sets that differ in a few members
// share every other block: the second one costs its list and the blocks that differ, and an operation on the two compares numbers of blocks.
//
// Every thread may read any set it has an id of, and make sets through a ValueSetMaker of its own, at the same time as the others.
class ValueSets {
    WTF_MAKE_NONCOPYABLE(ValueSets);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueSets);
public:
    static constexpr ValueSetID nothing { 0u };
    static constexpr ValueSetID anything { 1u };
    static constexpr uint32_t membersPerBlock = 4096;

    explicit ValueSets(uint32_t sizeOfUniverse);
    ~ValueSets();

    uint32_t sizeOfUniverse() const { return m_sizeOfUniverse; }

    static constexpr ValueSetID complement(ValueSetID set) { return ValueSetID(set.index() ^ 1u); }
    static constexpr bool isComplement(ValueSetID set) { return set.index() & 1u; }

    bool isEmpty(ValueSetID set) const { return !size(set); }
    uint32_t size(ValueSetID) const;
    bool contains(ValueSetID, ValueFlowObjectID) const;
    // Whether the two share a member. Makes no set.
    bool meet(ValueSetID, ValueSetID) const;
    bool isSubset(ValueSetID, ValueSetID of) const;
    // None unless the set has exactly one member.
    ValueFlowObjectID onlyMember(ValueSetID) const;

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

    size_t numberOfStoredSets() const { return m_lists.size(); }
    size_t numberOfStoredBlocks() const { return m_blocks.size(); }
    size_t memoryInBytes() const { return m_lists.memoryInBytes() + m_blocks.memoryInBytes(); }

private:
    friend class ValueSetMaker;

    static constexpr unsigned wordsPerBlock = membersPerBlock / 64;

    // A run of 64-bit words that never changes. Of a block: which of its 64 words are not zero, then those words. Of a list: (place << 32 | number of the block), ascending.
    struct Run {
        const uint64_t* words { nullptr };
        uint32_t numberOfWords { 0 };
        uint32_t numberOfMembers { 0 };
        uint64_t hashOfMembers { 0 };
    };

    // Runs found again by their words. A number, once handed out, is good for the life of the table, and its run can be read without a lock.
    class InternedRuns {
        WTF_MAKE_NONCOPYABLE(InternedRuns);
    public:
        InternedRuns();
        ~InternedRuns();

        const Run& at(uint32_t number) const LIFETIME_BOUND
        {
            const Shard& shard = m_shards[number % numberOfShards];
            uint32_t index = number / numberOfShards + firstSegmentSize;
            unsigned segment = std::bit_width(index) - std::bit_width(firstSegmentSize);
            return shard.segments[segment].load(std::memory_order_acquire)[index - (firstSegmentSize << segment)];
        }

        // Number 0 is the run without words.
        uint32_t intern(std::span<const uint64_t> words, uint32_t numberOfMembers, uint64_t hashOfMembers);

        size_t size() const;
        size_t memoryInBytes() const;

    private:
        static constexpr unsigned numberOfShards = 64;
        static constexpr uint32_t firstSegmentSize = 64;
        static constexpr unsigned numberOfSegments = 21;
        static constexpr size_t wordsPerArenaChunk = 8192;

        struct Slot {
            uint32_t hash { 0 };
            uint32_t indexPlusOne { 0 };
        };

        struct alignas(64) Shard {
            Lock lock;
            Vector<Slot> slots;
            uint32_t size { 0 };
            std::array<std::atomic<Run*>, numberOfSegments> segments { };
            Vector<uint64_t*> arenaChunks;
            std::span<uint64_t> freeWords;
            size_t arenaBytes { 0 };
        };

        Run& runAt(Shard&, uint32_t index);
        std::span<uint64_t> allocate(Shard&, size_t numberOfWords);
        void grow(Shard&);

        std::array<Shard, numberOfShards> m_shards;
    };

    static constexpr uint32_t storedSetOf(ValueSetID set) { return set.index() >> 1; }
    static constexpr uint32_t placeOf(uint64_t entry) { return static_cast<uint32_t>(entry >> 32); }
    static constexpr uint32_t blockOf(uint64_t entry) { return static_cast<uint32_t>(entry); }

    std::span<const uint64_t> listOf(ValueSetID set) const LIFETIME_BOUND
    {
        const Run& run = m_lists.at(storedSetOf(set));
        return unsafeMakeSpan(run.words, run.numberOfWords);
    }

    uint32_t blockAt(ValueSetID, uint32_t place) const;
    bool storedSetsMeet(ValueSetID, ValueSetID) const;
    bool storedSetIsSubset(ValueSetID, ValueSetID of) const;

    void forEachStoredMember(ValueSetID set, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        for (uint64_t entry : listOf(set)) {
            const Run& block = m_blocks.at(blockOf(entry));
            uint32_t firstOfBlock = placeOf(entry) * membersPerBlock;
            const uint64_t* word = block.words + 1;
            for (uint64_t present = block.words[0]; present; present &= present - 1, ++word) {
                uint32_t firstOfWord = firstOfBlock + static_cast<uint32_t>(std::countr_zero(present)) * 64;
                for (uint64_t bits = *word; bits; bits &= bits - 1)
                    functor(firstOfWord + static_cast<uint32_t>(std::countr_zero(bits)));
            }
        }
    }

    const uint32_t m_sizeOfUniverse;
    InternedRuns m_blocks;
    InternedRuns m_lists;
};

// One thread's way of making sets: its buffers and what it remembers of the operations it has done.
class ValueSetMaker {
    WTF_MAKE_NONCOPYABLE(ValueSetMaker);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueSetMaker);
public:
    explicit ValueSetMaker(ValueSets&);

    const ValueSets& sets() const { return m_sets; }

    ValueSetID of(ValueFlowObjectID);
    // In any order, with or without repeats.
    ValueSetID of(std::span<const ValueFlowObjectID>);
    // Every member from the first up to the one before `end`.
    ValueSetID ofRange(ValueFlowObjectID first, ValueFlowObjectID end);
    ValueSetID unite(ValueSetID, ValueSetID);
    ValueSetID intersect(ValueSetID, ValueSetID);
    // What a place holds of the value whose mask was made from a type. Where the value holds the stand-in and the mask does not admit it, what the mask admits of the members it stands for takes its place.
    // A mask that a rule asks for leaves the stand-in out on purpose: that is intersect().
    ValueSetID pass(ValueSetID value, ValueSetID mask, const ValueFlowStandIn&);
    // No set is interned on the way.
    ValueSetID uniteAll(std::span<const ValueSetID>);
    // The set as the union of smaller sets, in ascending order: its blocks, or the words of its one block. A set that grows by a member keeps all its parts but one at each of the two steps.
    // Nothing for a set of few members, for one that lies within one word and for a complement.
    void appendParts(ValueSetID, Vector<ValueSetID>&);

    struct Counters {
        uint64_t answeredAtOnce { 0 };
        uint64_t remembered { 0 };
        uint64_t computed { 0 };
        uint64_t blocksRemembered { 0 };
        uint64_t blocksComputed { 0 };
    };
    const Counters& counters() const { return m_counters; }

private:
    enum class Operation : uint8_t { Unite, Intersect, Subtract };

    struct Remembered {
        uint32_t first { 0 };
        // The operation lies in the two highest bits.
        uint32_t second { 0 };
        uint32_t result { 0 };
    };

    static constexpr unsigned bitsOfRememberedSets = 17;
    static constexpr unsigned bitsOfRememberedBlocks = 16;
    static constexpr uint32_t mostMembersOfSetWithoutParts = 16;

    uint32_t combine(Operation, uint32_t first, uint32_t second);
    uint32_t combineBlocks(Operation, uint32_t first, uint32_t second);
    uint32_t internList(std::span<const uint64_t>);

    ValueSets& m_sets;
    Vector<Remembered> m_rememberedSets;
    Vector<Remembered> m_rememberedBlocks;
    // The set of one member, by the member: nothing where it has not been made by this maker.
    Vector<ValueSetID> m_setOfMember;
    HashMap<uint64_t, ValueSetID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_setOfRange;
    uint32_t m_fullBlock { 0 };
    Vector<uint64_t> m_list;
    Vector<uint32_t> m_blockAtPlace;
    Vector<uint32_t> m_members;
    Counters m_counters;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
