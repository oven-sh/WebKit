/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class SweepBits {
    WTF_MAKE_NONCOPYABLE(SweepBits);
public:
    SweepBits() = default;

    void clearAll() { m_words.shrink(0); }
    void ensureSize(size_t numberOfBits)
    {
        size_t numberOfWords = (numberOfBits + bitsInWord - 1) / bitsInWord;
        size_t numberOfWordsSoFar = m_words.size();
        if (numberOfWords <= numberOfWordsSoFar)
            return;
        m_words.grow(numberOfWords);
        std::ranges::fill(m_words.mutableSpan().subspan(numberOfWordsSoFar), uint64_t { 0 });
    }

    bool quickGet(size_t bit) const
    {
        ASSERT(bit / bitsInWord < m_words.size());
        return m_words.span().data()[bit / bitsInWord] >> (bit % bitsInWord) & 1;
    }
    bool quickSet(size_t bit)
    {
        ASSERT(bit / bitsInWord < m_words.size());
        uint64_t& word = m_words.mutableSpan().data()[bit / bitsInWord];
        uint64_t mask = uint64_t { 1 } << (bit % bitsInWord);
        bool wasSet = word & mask;
        word |= mask;
        return wasSet;
    }
    void quickClear(size_t bit)
    {
        ASSERT(bit / bitsInWord < m_words.size());
        m_words.mutableSpan().data()[bit / bitsInWord] &= ~(uint64_t { 1 } << (bit % bitsInWord));
    }
    bool get(size_t bit) const { return bit / bitsInWord < m_words.size() && quickGet(bit); }
    void set(size_t bit)
    {
        ensureSize(bit + 1);
        quickSet(bit);
    }

    bool isEmpty() const
    {
        for (uint64_t word : m_words) {
            if (word)
                return false;
        }
        return true;
    }
    void merge(const SweepBits& other)
    {
        ensureSize(other.m_words.size() * bitsInWord);
        for (size_t index = 0; index < other.m_words.size(); ++index)
            m_words[index] |= other.m_words[index];
    }
    void forEachSetBit(NOESCAPE const Invocable<void(size_t)> auto& functor) const
    {
        for (size_t index = 0; index < m_words.size(); ++index) {
            for (uint64_t word = m_words[index]; word; word &= word - 1)
                functor(index * bitsInWord + static_cast<size_t>(std::countr_zero(word)));
        }
    }

private:
    static constexpr size_t bitsInWord = 64;

    Vector<uint64_t> m_words;
};

class SweepValueSets {
    WTF_MAKE_NONCOPYABLE(SweepValueSets);
public:
    using Set = uint32_t;
    static constexpr Set empty = 0;

    SweepValueSets() { clear(); }

    void clear()
    {
        m_nodes.shrink(0);
        m_nodes.append(Node { });
        m_firstNodeThatMayChange = 1;
    }

    Set with(Set set, uint32_t member) { return withLeaf(set, member & ~lowBits, bitOf(member), true); }
    Set without(Set set, uint32_t member) { return withoutBit(set, member & ~lowBits, bitOf(member)); }
    Set unite(Set first, Set second)
    {
        if (first == second || !second)
            return first;
        if (!first)
            return second;
        return uniteSetsThatDiffer(first, second);
    }
    void keepSetsMadeSoFar() { m_firstNodeThatMayChange = numberOfNodes(); }

    bool contains(Set, uint32_t member) const;
    void forEachMember(Set, NOESCAPE const Invocable<void(uint32_t)> auto&) const;

    uint32_t numberOfNodes() const { return static_cast<uint32_t>(m_nodes.size()); }
    void forgetSetsMadeSince(uint32_t numberOfNodes)
    {
        m_nodes.shrink(numberOfNodes);
        m_firstNodeThatMayChange = std::min(m_firstNodeThatMayChange, numberOfNodes);
    }

private:
    static constexpr uint32_t lowBits = 63;
    static constexpr unsigned mostForksOnWayDown = 26;
    static_assert(mostForksOnWayDown == 8 * sizeof(uint32_t) - std::popcount(lowBits));

    struct Node {
        uint32_t prefix { 0 };
        uint32_t branchingBit { 0 };
        uint32_t low { 0 };
        uint32_t high { 0 };

        uint64_t bits() const { return static_cast<uint64_t>(high) << 32 | low; }
    };

    static uint64_t bitOf(uint32_t member) { return 1ull << (member & lowBits); }
    static uint32_t prefixAbove(uint32_t number, uint32_t bit) { return number & ~(bit | (bit - 1)); }
    bool mayChange(Set set) const { return set >= m_firstNodeThatMayChange; }

    Set append(Node node)
    {
        m_nodes.append(node);
        return numberOfNodes() - 1;
    }
    Set makeLeaf(uint32_t prefix, uint64_t bits) { return append({ prefix, 0, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits >> 32) }); }
    Set join(uint32_t prefixOfFirst, Set first, uint32_t prefixOfSecond, Set second)
    {
        uint32_t bit = std::bit_floor(prefixOfFirst ^ prefixOfSecond);
        if (prefixOfFirst & bit)
            std::swap(first, second);
        return append({ prefixAbove(prefixOfFirst, bit), bit, first, second });
    }
    Set withChild(Set fork, const Node& node, bool isHigh, Set child, bool mayChangeInPlace)
    {
        if (mayChangeInPlace && mayChange(fork)) {
            (isHigh ? m_nodes[fork].high : m_nodes[fork].low) = child;
            return fork;
        }
        return append({ node.prefix, node.branchingBit, isHigh ? node.low : child, isHigh ? child : node.high });
    }
    Set withChangeBelow(Set set, const Set* forks, unsigned numberOfForks, uint32_t prefix, Set changed, bool mayChangeInPlace)
    {
        while (numberOfForks--) {
            Set fork = forks[numberOfForks];
            Node node = m_nodes[fork];
            Set replaced = withChild(fork, node, prefix & node.branchingBit, changed, mayChangeInPlace);
            if (replaced == fork)
                return set;
            changed = replaced;
        }
        return changed;
    }
    Set withLeaf(Set, uint32_t prefix, uint64_t bits, bool mayChangeInPlace);
    Set withoutBit(Set, uint32_t prefix, uint64_t bit);
    Set uniteSetsThatDiffer(Set, Set);

    Vector<Node> m_nodes;
    uint32_t m_firstNodeThatMayChange { 1 };
};

inline SweepValueSets::Set SweepValueSets::withLeaf(Set set, uint32_t prefix, uint64_t bits, bool mayChangeInPlace)
{
    if (!set)
        return makeLeaf(prefix, bits);

    Set forks[mostForksOnWayDown];
    unsigned numberOfForks = 0;
    Set place = set;
    Set changed = empty;
    Node* nodes = m_nodes.mutableSpan().data();
    for (;;) {
        Node node = nodes[place];
        if (node.branchingBit ? prefixAbove(prefix, node.branchingBit) != node.prefix : node.prefix != prefix) {
            changed = join(prefix, makeLeaf(prefix, bits), node.prefix, place);
            break;
        }
        if (!node.branchingBit) {
            uint64_t united = node.bits() | bits;
            if (united == node.bits())
                return set;
            if (mayChangeInPlace && mayChange(place)) {
                nodes[place].low = static_cast<uint32_t>(united);
                nodes[place].high = static_cast<uint32_t>(united >> 32);
                return set;
            }
            changed = makeLeaf(prefix, united);
            break;
        }
        ASSERT(numberOfForks < mostForksOnWayDown);
        forks[numberOfForks++] = place;
        place = prefix & node.branchingBit ? node.high : node.low;
    }
    return withChangeBelow(set, forks, numberOfForks, prefix, changed, mayChangeInPlace);
}

ALWAYS_INLINE SweepValueSets::Set SweepValueSets::withoutBit(Set set, uint32_t prefix, uint64_t bit)
{
    if (!set)
        return empty;

    Set forks[mostForksOnWayDown];
    unsigned numberOfForks = 0;
    Set place = set;
    Set changed = empty;
    Node* nodes = m_nodes.mutableSpan().data();
    for (;;) {
        Node node = nodes[place];
        if (!node.branchingBit) {
            if (node.prefix != prefix || !(node.bits() & bit))
                return set;
            uint64_t left = node.bits() & ~bit;
            if (!left)
                break;
            if (mayChange(place)) {
                nodes[place].low = static_cast<uint32_t>(left);
                nodes[place].high = static_cast<uint32_t>(left >> 32);
                return set;
            }
            changed = makeLeaf(prefix, left);
            break;
        }
        if (prefixAbove(prefix, node.branchingBit) != node.prefix)
            return set;
        ASSERT(numberOfForks < mostForksOnWayDown);
        forks[numberOfForks++] = place;
        place = prefix & node.branchingBit ? node.high : node.low;
    }
    if (!changed) {
        if (!numberOfForks)
            return empty;
        Node node = m_nodes[forks[--numberOfForks]];
        changed = prefix & node.branchingBit ? node.low : node.high;
    }
    return withChangeBelow(set, forks, numberOfForks, prefix, changed, true);
}

inline SweepValueSets::Set SweepValueSets::uniteSetsThatDiffer(Set first, Set second)
{
    Node nodeOfFirst = m_nodes[first];
    Node nodeOfSecond = m_nodes[second];
    if (!nodeOfSecond.branchingBit)
        return withLeaf(first, nodeOfSecond.prefix, nodeOfSecond.bits(), false);
    if (!nodeOfFirst.branchingBit)
        return withLeaf(second, nodeOfFirst.prefix, nodeOfFirst.bits(), false);

    auto uniteBelow = [&](Set outer, const Node& nodeOfOuter, Set inner, const Node& nodeOfInner) {
        if (prefixAbove(nodeOfInner.prefix, nodeOfOuter.branchingBit) != nodeOfOuter.prefix)
            return join(nodeOfOuter.prefix, outer, nodeOfInner.prefix, inner);
        bool isHigh = nodeOfInner.prefix & nodeOfOuter.branchingBit;
        Set child = isHigh ? nodeOfOuter.high : nodeOfOuter.low;
        Set changed = unite(child, inner);
        return changed == child ? outer : withChild(outer, nodeOfOuter, isHigh, changed, false);
    };
    if (nodeOfFirst.branchingBit > nodeOfSecond.branchingBit)
        return uniteBelow(first, nodeOfFirst, second, nodeOfSecond);
    if (nodeOfSecond.branchingBit > nodeOfFirst.branchingBit)
        return uniteBelow(second, nodeOfSecond, first, nodeOfFirst);
    if (nodeOfFirst.prefix != nodeOfSecond.prefix)
        return join(nodeOfFirst.prefix, first, nodeOfSecond.prefix, second);

    Set low = unite(nodeOfFirst.low, nodeOfSecond.low);
    Set high = unite(nodeOfFirst.high, nodeOfSecond.high);
    if (low == nodeOfFirst.low && high == nodeOfFirst.high)
        return first;
    if (low == nodeOfSecond.low && high == nodeOfSecond.high)
        return second;
    return append({ nodeOfFirst.prefix, nodeOfFirst.branchingBit, low, high });
}

inline bool SweepValueSets::contains(Set set, uint32_t member) const
{
    const Node* nodes = m_nodes.span().data();
    while (set) {
        const Node& node = nodes[set];
        if (!node.branchingBit)
            return node.prefix == (member & ~lowBits) && (node.bits() & bitOf(member));
        set = member & node.branchingBit ? node.high : node.low;
    }
    return false;
}

inline void SweepValueSets::forEachMember(Set set, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
{
    if (!set)
        return;

    Vector<Set, 32> waiting;
    waiting.append(set);
    while (!waiting.isEmpty()) {
        Node node = m_nodes[waiting.takeLast()];
        if (node.branchingBit) {
            waiting.append(node.high);
            waiting.append(node.low);
            continue;
        }
        for (uint64_t bits = node.bits(); bits; bits &= bits - 1)
            functor(node.prefix + static_cast<uint32_t>(std::countr_zero(bits)));
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
