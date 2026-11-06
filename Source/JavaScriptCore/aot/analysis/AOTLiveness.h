/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTUseLists.h"
#include "AOTVector.h"
#include <array>
#include <bit>
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct LivenessOptions {
    enum class Solver : uint8_t { ChosenBySize, PerValue, SharedSets };

    const BitVector* trackedValues { nullptr };
    const BitVector* trackedBlocks { nullptr };
    std::span<const std::pair<BlockID, BlockID>> additionalEdges;
    const OpcodeSet* opcodesOfValues { nullptr };
    Solver solver { Solver::ChosenBySize };
};

class Liveness {
    WTF_MAKE_NONCOPYABLE(Liveness);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Liveness);
public:
    Liveness(const Function&, const CFG&, const UseLists&, const LivenessOptions& = { });

    // In ascending order.
    class LiveValues {
        WTF_MAKE_NONCOPYABLE(LiveValues);
    public:
        explicit LiveValues(std::span<const ValueID> values)
            : m_values(values)
        {
        }

        explicit LiveValues(Vector<ValueID>&& values)
            : m_storage(WTF::move(values))
            , m_values(m_storage.span())
        {
        }

        auto begin() const LIFETIME_BOUND { return m_values.begin(); }
        auto end() const LIFETIME_BOUND { return m_values.end(); }
        bool empty() const { return m_values.empty(); }
        size_t size() const { return m_values.size(); }
        operator std::span<const ValueID>() const LIFETIME_BOUND { return m_values; }

    private:
        Vector<ValueID> m_storage;
        std::span<const ValueID> m_values;
    };

    // What has been found out about one set of values. Blocks share most of what is live at their heads, and a part that several share is looked through once.
    class AnswersAboutValues {
    private:
        friend class Liveness;

        const Liveness* m_liveness { nullptr };
        Vector<uint8_t> m_answersOfSharedSets;
    };

    LiveValues liveAtHead(BlockID) const LIFETIME_BOUND;
    bool isAnyLiveAtHead(BlockID) const;
    bool isAnyLiveAtHead(BlockID, const BitVector& values, AnswersAboutValues&) const;
    bool isLiveAtHead(ValueID, BlockID) const;

    // Calls the functor for each value that `isPoint` accepts, in each block from the last to the first, with those of the values live after it, in ascending order,
    // that are live after no other point that follows. The functor makes them held operands of the point, which keeps them live after every point before it.
    // The work goes by the uses of the function, not by how much is live.
    using IsLive = ScopedLambda<bool(ValueID)>;
    using LastCrossing = void(ValueID point, std::span<const ValueID> values, const IsLive& isLiveAfterPoint);
    void forEachLastCrossing(NOESCAPE const ScopedLambda<bool(ValueID)>& isPoint, NOESCAPE const ScopedLambda<LastCrossing>&) const;

    // What it costs goes by the block and by what is live in it, not by the values of the function: a function of many blocks has one of these for each.
    class LocalCalc {
        WTF_MAKE_NONCOPYABLE(LocalCalc);
    public:
        LocalCalc(const Liveness&, BlockID);
        ~LocalCalc();

        void execute(uint32_t index);

        bool isLive(ValueID id) const
        {
            RELEASE_ASSERT(id.index() < m_liveness.m_useLists.numberOfValues());
            RELEASE_ASSERT(m_liveness.isTrackedValue(id));
            return m_words[id.index() / 64] >> (id.index() % 64) & 1;
        }

        size_t numberOfLiveValues() const
        {
            size_t count = 0;
            for (uint32_t word : wordsInUse())
                count += std::popcount(m_words[word]);
            return count;
        }

        // In ascending order.
        void forEachLive(NOESCAPE const Invocable<void(ValueID)> auto& functor) const
        {
            for (uint32_t word : wordsInUse()) {
                for (uint64_t bits = m_words[word]; bits; bits &= bits - 1)
                    functor(ValueID(64 * word + std::countr_zero(bits)));
            }
        }

    private:
        void use(ValueID);
        // Whether it was live before.
        bool add(ValueID);
        std::span<const uint32_t> wordsInUse() const LIFETIME_BOUND;

        const Liveness& m_liveness;
        BlockID m_block;
        uint32_t m_index { 0 };
        // A bit for each value of the function. They are borrowed from the Liveness, all clear, and given back so.
        Vector<uint64_t> m_words;
        // Every word that has held a bit. Sorted, and each once, up to m_numberOfSortedWords.
        mutable Vector<uint32_t, 16> m_wordsInUse;
        mutable size_t m_numberOfSortedWords { 0 };
    };

private:
    struct InteriorPointer {
        ValueID base;
        ValueID pointer;

        friend auto operator<=>(const InteriorPointer&, const InteriorPointer&) = default;
    };

    struct CrossBlockValue {
        ValueID value;
        BlockID definingBlock;
        uint32_t firstUseBlockIndex;
    };

    struct CrossBlockValues {
        std::span<const BlockID> useBlocksOf(size_t index) const LIFETIME_BOUND
        {
            size_t begin = values[index].firstUseBlockIndex;
            size_t end = index + 1 < values.size() ? values[index + 1].firstUseBlockIndex : useBlocks.size();
            return useBlocks.subspan(begin, end - begin);
        }

        Vector<CrossBlockValue, 32> values;
        Vector<BlockID, 64> useBlocks;
    };

    using AdditionalPredecessors = Vector<Vector<BlockID, 1>>;

    // Sets of small numbers that share the parts they have in common, so a set that differs from another in a few members costs a few nodes.
    class SharedSets {
    public:
        using Set = uint32_t;
        static constexpr Set emptySet = 0;

        void setNumberOfKeys(size_t);
        Set add(Set set, uint32_t key) { return add(set, key, m_numberOfInnerLevels); }
        Set remove(Set set, uint32_t key) { return remove(set, key, m_numberOfInnerLevels); }
        Set unite(Set first, Set second) { return unite(first, second, m_numberOfInnerLevels); }
        bool contains(Set, uint32_t key) const;
        void forEach(Set set, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const { forEach(set, 0, m_numberOfInnerLevels, functor); }
        size_t numberOfSets() const { return m_nodes.size(); }
        // `answers` has an entry for each set, zero at first, and goes with one functor.
        bool containsAny(Set set, Vector<uint8_t>& answers, NOESCAPE const Invocable<bool(uint32_t)> auto& functor) const { return containsAny(set, 0, m_numberOfInnerLevels, answers, functor); }

    private:
        static constexpr unsigned slotsPerNode = 8;
        static constexpr unsigned bitsPerSlotIndex = 3;
        static constexpr unsigned bitsPerLeafWord = 32;
        static constexpr unsigned bitsPerLeafKey = 8;
        using Node = std::array<uint32_t, slotsPerNode>;

        static unsigned slotOf(uint32_t key, unsigned level) { return (key >> (bitsPerLeafKey + bitsPerSlotIndex * (level - 1))) % slotsPerNode; }
        static unsigned wordOf(uint32_t key) { return key / bitsPerLeafWord % slotsPerNode; }
        static uint32_t maskOf(uint32_t key) { return 1u << (key % bitsPerLeafWord); }

        Set append(const Node&);
        Set add(Set, uint32_t key, unsigned level);
        Set remove(Set, uint32_t key, unsigned level);
        Set unite(Set, Set, unsigned level);

        void forEach(Set set, uint32_t firstKey, unsigned level, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
        {
            if (!set)
                return;
            const Node& node = m_nodes[set];
            for (unsigned slot = 0; slot < slotsPerNode; ++slot) {
                if (level) {
                    forEach(node[slot], firstKey + (slot << (bitsPerLeafKey + bitsPerSlotIndex * (level - 1))), level - 1, functor);
                    continue;
                }
                for (uint32_t bits = node[slot]; bits; bits &= bits - 1)
                    functor(firstKey + slot * bitsPerLeafWord + std::countr_zero(bits));
            }
        }

        bool containsAny(Set set, uint32_t firstKey, unsigned level, Vector<uint8_t>& answers, NOESCAPE const Invocable<bool(uint32_t)> auto& functor) const
        {
            constexpr uint8_t notAsked = 0;
            constexpr uint8_t no = 1;
            constexpr uint8_t yes = 2;
            if (!set)
                return false;
            if (answers[set] != notAsked)
                return answers[set] == yes;
            const Node& node = m_nodes[set];
            bool found = false;
            for (unsigned slot = 0; slot < slotsPerNode && !found; ++slot) {
                if (level) {
                    found = containsAny(node[slot], firstKey + (slot << (bitsPerLeafKey + bitsPerSlotIndex * (level - 1))), level - 1, answers, functor);
                    continue;
                }
                for (uint32_t bits = node[slot]; bits && !found; bits &= bits - 1)
                    found = functor(firstKey + slot * bitsPerLeafWord + std::countr_zero(bits));
            }
            answers[set] = found ? yes : no;
            return found;
        }

        Vector<Node> m_nodes;
        unsigned m_numberOfInnerLevels { 0 };
    };

    bool isTrackedValue(ValueID id) const { return !m_trackedValues || m_trackedValues->get(id.index()); }
    bool isTrackedBlock(BlockID block) const { return m_cfg.isReachable(block) && (!m_trackedBlocks || m_trackedBlocks->get(block.index())); }
    ValueID baseOf(ValueID) const;
    template<typename Functor> void forEachBase(ValueID, NOESCAPE const Functor&) const;
    CrossBlockValues collectCrossBlockValues(std::span<const InteriorPointer>) const;
    bool solvePerValue(const CrossBlockValues&, const AdditionalPredecessors&, size_t remainingVisits);
    void solvePerBlock(const CrossBlockValues&, const AdditionalPredecessors&);
    void solveWithSharedSets(const CrossBlockValues&, const AdditionalPredecessors&);
    void forEachLiveAtHead(BlockID, NOESCAPE const Invocable<void(ValueID)> auto&) const;

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    std::optional<BitVector> m_trackedValues;
    std::optional<BitVector> m_trackedBlocks;
    Vector<std::pair<BlockID, BlockID>> m_additionalEdges;
    Vector<ValueID> m_liveValues;
    Vector<uint32_t> m_firstLiveValue;
    SharedSets m_sharedSets;
    Vector<SharedSets::Set> m_sharedSetAtHead;
    Vector<ValueID> m_valuesInSharedSets;
    bool m_hasTrackedBases { false };
    mutable Lock m_spareWordsLock;
    mutable Vector<Vector<uint64_t>, 1> m_spareWords WTF_GUARDED_BY_LOCK(m_spareWordsLock);
};

class LivenessOfAskedValues {
    WTF_MAKE_NONCOPYABLE(LivenessOfAskedValues);
public:
    LivenessOfAskedValues(const Function&, const CFG&, const UseLists&);

    static bool answersFor(const OpcodeSet& opcodesOfValues);

    bool isLiveAtHead(ValueID, BlockID);
    const BitVector& blocksEnteredWith(ValueID);

private:
    uint32_t findBlocksEnteredWith(ValueID);

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    Vector<uint32_t> m_setOfValue;
    Vector<BitVector> m_blocksEnteredWithValue;
    std::optional<Vector<std::pair<ValueID, BlockID>>> m_narrowedOperandUses;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
