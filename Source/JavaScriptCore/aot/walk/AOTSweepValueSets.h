/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

    bool quickGet(size_t bit) const { return m_words[bit / bitsInWord] >> (bit % bitsInWord) & 1; }
    bool quickSet(size_t bit)
    {
        uint64_t& word = m_words[bit / bitsInWord];
        uint64_t mask = uint64_t { 1 } << (bit % bitsInWord);
        bool wasSet = word & mask;
        word |= mask;
        return wasSet;
    }
    void quickClear(size_t bit) { m_words[bit / bitsInWord] &= ~(uint64_t { 1 } << (bit % bitsInWord)); }
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

// Sets of numbers, each made out of another one and sharing with it whatever the two have in common. So a set for every block of a unit costs what the block adds and takes away, not what the set holds.
// A set is a tree over the bits of its members, highest bit first, whose leaves hold 64 neighbors each: Okasaki and Gill, "Fast Mergeable Integer Maps".
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

    // Whoever hands a set to with() or without() gives it up, unless keepSetsMadeSoFar() has been called since the set was made: only the answer counts from then on.
    Set with(Set set, uint32_t member) { return withLeaf(set, member & ~lowBits, bitOf(member), true); }
    Set without(Set set, uint32_t member) { return withoutBit(set, member & ~lowBits, bitOf(member)); }
    // Changes neither.
    Set unite(Set, Set);
    void keepSetsMadeSoFar() { m_firstNodeThatMayChange = numberOfNodes(); }

    bool contains(Set, uint32_t member) const;
    // Ascending. The functor makes no set.
    void forEachMember(Set, NOESCAPE const Invocable<void(uint32_t)> auto&) const;

    uint32_t numberOfNodes() const { return static_cast<uint32_t>(m_nodes.size()); }
    // No set that was made since is read again.
    void forgetSetsMadeSince(uint32_t numberOfNodes)
    {
        m_nodes.shrink(numberOfNodes);
        m_firstNodeThatMayChange = std::min(m_firstNodeThatMayChange, numberOfNodes);
    }

private:
    static constexpr uint32_t lowBits = 63;

    // A leaf has no branching bit: its members are `prefix` plus each bit that is set in the 64 bits of `low` and `high`.
    // Below a fork all members agree with `prefix` above the branching bit. Those without the bit are in `low`, the others in `high`.
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
    // The two have no member in common, and their prefixes differ.
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
    Set withLeaf(Set, uint32_t prefix, uint64_t bits, bool mayChangeInPlace);
    Set withoutBit(Set, uint32_t prefix, uint64_t bit);

    Vector<Node> m_nodes;
    uint32_t m_firstNodeThatMayChange { 1 };
};

inline SweepValueSets::Set SweepValueSets::withLeaf(Set set, uint32_t prefix, uint64_t bits, bool mayChangeInPlace)
{
    if (!set)
        return makeLeaf(prefix, bits);

    Node node = m_nodes[set];
    if (!node.branchingBit) {
        if (node.prefix != prefix)
            return join(prefix, makeLeaf(prefix, bits), node.prefix, set);
        uint64_t united = node.bits() | bits;
        if (united == node.bits())
            return set;
        if (mayChangeInPlace && mayChange(set)) {
            m_nodes[set].low = static_cast<uint32_t>(united);
            m_nodes[set].high = static_cast<uint32_t>(united >> 32);
            return set;
        }
        return makeLeaf(prefix, united);
    }
    if (prefixAbove(prefix, node.branchingBit) != node.prefix)
        return join(prefix, makeLeaf(prefix, bits), node.prefix, set);

    bool isHigh = prefix & node.branchingBit;
    Set child = isHigh ? node.high : node.low;
    Set changed = withLeaf(child, prefix, bits, mayChangeInPlace);
    return changed == child ? set : withChild(set, node, isHigh, changed, mayChangeInPlace);
}

inline SweepValueSets::Set SweepValueSets::withoutBit(Set set, uint32_t prefix, uint64_t bit)
{
    if (!set)
        return empty;

    Node node = m_nodes[set];
    if (!node.branchingBit) {
        if (node.prefix != prefix || !(node.bits() & bit))
            return set;
        uint64_t left = node.bits() & ~bit;
        if (!left)
            return empty;
        if (mayChange(set)) {
            m_nodes[set].low = static_cast<uint32_t>(left);
            m_nodes[set].high = static_cast<uint32_t>(left >> 32);
            return set;
        }
        return makeLeaf(prefix, left);
    }
    if (prefixAbove(prefix, node.branchingBit) != node.prefix)
        return set;

    bool isHigh = prefix & node.branchingBit;
    Set child = isHigh ? node.high : node.low;
    Set changed = withoutBit(child, prefix, bit);
    if (changed == child)
        return set;
    if (!changed)
        return isHigh ? node.low : node.high;
    return withChild(set, node, isHigh, changed, true);
}

inline SweepValueSets::Set SweepValueSets::unite(Set first, Set second)
{
    if (first == second || !second)
        return first;
    if (!first)
        return second;

    Node nodeOfFirst = m_nodes[first];
    Node nodeOfSecond = m_nodes[second];
    if (!nodeOfSecond.branchingBit)
        return withLeaf(first, nodeOfSecond.prefix, nodeOfSecond.bits(), false);
    if (!nodeOfFirst.branchingBit)
        return withLeaf(second, nodeOfFirst.prefix, nodeOfFirst.bits(), false);

    // All of `inner` belongs below one child of `outer`, or beside `outer`.
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
    while (set) {
        const Node& node = m_nodes[set];
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
