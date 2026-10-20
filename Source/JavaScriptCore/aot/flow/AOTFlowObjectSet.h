/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowIDs.h"
#include <algorithm>
#include <bit>
#include <span>
#include <utility>
#include <wtf/Forward.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FlowObjectBitmap;

class FlowObjectSet {
public:
    static constexpr uint32_t objectsPerChunk = 32;
    static constexpr uint32_t maxObjectIndex = (1u << 31) - 1;

    FlowObjectSet() = default;

    explicit FlowObjectSet(FlowObjectID object)
        : m_word(inlineTag | makeChunk(object.index() / objectsPerChunk, 1u << (object.index() % objectsPerChunk)))
    {
        RELEASE_ASSERT(object && object.index() <= maxObjectIndex);
    }

    FlowObjectSet(const FlowObjectSet& other)
    {
        if (other.isInline())
            m_word = other.m_word;
        else
            copyOutOfLine(other);
    }

    FlowObjectSet(FlowObjectSet&& other)
        : m_word(std::exchange(other.m_word, inlineTag))
    {
    }

    ~FlowObjectSet()
    {
        if (!isInline())
            freeStorage();
    }

    FlowObjectSet& operator=(const FlowObjectSet& other)
    {
        if (this != &other) {
            FlowObjectSet copy(other);
            std::swap(m_word, copy.m_word);
        }
        return *this;
    }

    FlowObjectSet& operator=(FlowObjectSet&& other)
    {
        std::swap(m_word, other.m_word);
        return *this;
    }

    bool isEmpty() const { return m_word == inlineTag; }
    size_t size() const;
    bool hasMoreObjectsThan(size_t) const;
    bool contains(FlowObjectID) const;

    bool containsUnknownObject() const
    {
        static_assert(!unknownFlowObject.index());
        std::span<const uint64_t> ascendingChunks = chunks();
        return !ascendingChunks.empty() && !chunkIndexOf(ascendingChunks[0]) && (chunkBitsOf(ascendingChunks[0]) & 1u);
    }

    bool isSubsetOf(const FlowObjectSet& other) const
    {
        if (isInline() && other.isInline()) {
            uint32_t bits = chunkBitsOf(m_word);
            return !bits || (chunkIndexOf(m_word) == chunkIndexOf(other.m_word) && !(bits & ~chunkBitsOf(other.m_word)));
        }
        return isSubsetOfChunksOf(other);
    }

    bool overlaps(const FlowObjectSet&) const;
    bool hasMoreCommonObjectsThan(const FlowObjectBitmap&, size_t) const;
    FlowObjectID onlyObject() const;

    bool add(FlowObjectID);
    void add(std::span<const FlowObjectID>);
    bool remove(FlowObjectID);
    bool unite(const FlowObjectSet&);
    bool uniteAndCollect(const FlowObjectSet& source, FlowObjectSet& added)
    {
        if (isInline() && source.isInline() && added.isEmpty() && (isEmpty() || chunkIndexOf(m_word) == chunkIndexOf(source.m_word))) {
            uint32_t freshBits = chunkBitsOf(source.m_word) & ~chunkBitsOf(m_word);
            if (!freshBits)
                return false;
            added.m_word = inlineTag | makeChunk(chunkIndexOf(source.m_word), freshBits);
            m_word = inlineTag | makeChunk(chunkIndexOf(source.m_word), chunkBitsOf(m_word) | freshBits);
            return true;
        }
        return uniteAndCollectChunks(source, nullptr, added);
    }

    // Leaves out the objects of the source that are excluded. The unknown object is never left out.
    bool uniteAndCollectExcluding(const FlowObjectSet& source, const FlowObjectBitmap& excluded, FlowObjectSet& added) { return uniteAndCollectChunks(source, &excluded, added); }
    void intersect(const FlowObjectSet&);
    void intersect(const FlowObjectBitmap&);
    void subtract(const FlowObjectSet&);
    void subtract(const FlowObjectBitmap&);
    // Removes what is in both.
    void subtractCommonObjectsOf(const FlowObjectSet&, const FlowObjectBitmap&);
    void clear() { *this = FlowObjectSet(); }

    void forEach(NOESCAPE const Invocable<void(FlowObjectID)> auto& functor) const
    {
        for (uint64_t chunk : chunks())
            forEachInChunk(chunkIndexOf(chunk), chunkBitsOf(chunk), functor);
    }

    void forEachNotIn(const FlowObjectSet& excluded, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor) const
    {
        std::span<const uint64_t> excludedChunks = excluded.chunks();
        size_t position = 0;
        for (uint64_t chunk : chunks()) {
            uint32_t index = chunkIndexOf(chunk);
            position = findLowerBound(excludedChunks, position, index);
            uint32_t bits = chunkBitsOf(chunk);
            if (position < excludedChunks.size() && chunkIndexOf(excludedChunks[position]) == index)
                bits &= ~chunkBitsOf(excludedChunks[position]);
            forEachInChunk(index, bits, functor);
        }
    }

    bool anyOf(NOESCAPE const Invocable<bool(FlowObjectID)> auto& predicate) const
    {
        for (uint64_t chunk : chunks()) {
            uint32_t first = chunkIndexOf(chunk) * objectsPerChunk;
            for (uint32_t bits = chunkBitsOf(chunk); bits; bits &= bits - 1) {
                if (predicate(FlowObjectID(first + static_cast<uint32_t>(std::countr_zero(bits)))))
                    return true;
            }
        }
        return false;
    }

    bool allOf(NOESCAPE const Invocable<bool(FlowObjectID)> auto& predicate) const
    {
        return !anyOf([&](FlowObjectID object) {
            return !predicate(object);
        });
    }

    size_t memoryInBytes() const;
    void dump(PrintStream&) const;

    friend bool operator==(const FlowObjectSet&, const FlowObjectSet&);

private:
    friend class FlowObjectBitmap;

    using ChunkBuffer = Vector<uint64_t, 16>;

    struct Storage {
        std::span<uint64_t> allocatedChunks() LIFETIME_BOUND { return unsafeMakeSpan(words, capacity); }
        std::span<uint64_t> chunks() LIFETIME_BOUND { return unsafeMakeSpan(words, size); }

        uint32_t size;
        uint32_t capacity;
        uint64_t words[0];
    };
    static_assert(sizeof(Storage) == sizeof(uint64_t));

    static constexpr uint64_t inlineTag = 1ull << 63;

    static constexpr uint64_t makeChunk(uint32_t index, uint32_t bits) { return static_cast<uint64_t>(index) << 32 | bits; }
    static constexpr uint32_t chunkIndexOf(uint64_t chunk) { return static_cast<uint32_t>((chunk & ~inlineTag) >> 32); }
    static constexpr uint32_t chunkBitsOf(uint64_t chunk) { return static_cast<uint32_t>(chunk); }
    static constexpr uint64_t withoutInlineTag(uint64_t chunk) { return chunk & ~inlineTag; }

    static void forEachInChunk(uint32_t index, uint32_t bits, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
    {
        uint32_t first = index * objectsPerChunk;
        for (; bits; bits &= bits - 1)
            functor(FlowObjectID(first + static_cast<uint32_t>(std::countr_zero(bits))));
    }

    static size_t findLowerBound(std::span<const uint64_t> ascendingChunks, size_t start, uint32_t index)
    {
        size_t low = start;
        size_t high = start;
        for (size_t step = 1; high < ascendingChunks.size() && chunkIndexOf(ascendingChunks[high]) < index; step *= 2) {
            low = high + 1;
            high += step;
        }
        std::span<const uint64_t> candidates = ascendingChunks.subspan(low, std::min(high, ascendingChunks.size()) - low);
        return low + static_cast<size_t>(std::ranges::lower_bound(candidates, index, { }, chunkIndexOf) - candidates.begin());
    }

    bool isInline() const { return m_word & inlineTag; }
    Storage* storage() const { return std::bit_cast<Storage*>(static_cast<uintptr_t>(m_word << 1)); }
    void setStorage(Storage* storage) { m_word = static_cast<uint64_t>(std::bit_cast<uintptr_t>(storage)) >> 1; }

    std::span<const uint64_t> chunks() const LIFETIME_BOUND
    {
        if (isInline())
            return singleElementSpan(m_word).first(chunkBitsOf(m_word) ? 1 : 0);
        return storage()->chunks();
    }

    std::span<uint64_t> mutableChunks() LIFETIME_BOUND
    {
        if (isInline())
            return singleElementSpan(m_word).first(chunkBitsOf(m_word) ? 1 : 0);
        return storage()->chunks();
    }

    static Storage* allocateStorage(uint32_t capacity);
    void copyOutOfLine(const FlowObjectSet&);
    void freeStorage();
    void adopt(std::span<const uint64_t> ascendingChunks);
    void reserveCapacity(uint32_t capacity);
    void insertChunks(std::span<const uint64_t> ascendingAbsentChunks);
    bool isSubsetOfChunksOf(const FlowObjectSet&) const;
    bool uniteAndCollectChunks(const FlowObjectSet& source, const FlowObjectBitmap* excluded, FlowObjectSet& added);
    bool uniteChunks(std::span<const uint64_t> ascendingChunks, ChunkBuffer* fresh, const FlowObjectBitmap* excluded = nullptr);
    void keepBits(NOESCAPE const Invocable<uint32_t(uint32_t index, uint32_t bits)> auto& keptBitsOf);

    uint64_t m_word { inlineTag };
};

static_assert(sizeof(FlowObjectSet) == sizeof(uint64_t));
static_assert(sizeof(uintptr_t) == sizeof(uint64_t));

// The objects of a set that only grows, with a word for each run of object indexes whether it holds any or not: a test is one load.
// For a set that others are tested against all the time.
class FlowObjectBitmap {
public:
    bool contains(FlowObjectID object) const
    {
        return object && (bitsOfChunk(object.index() / FlowObjectSet::objectsPerChunk) >> (object.index() % FlowObjectSet::objectsPerChunk) & 1u);
    }

    uint32_t bitsOfChunk(uint32_t index) const { return index < m_words.size() ? m_words[index] : 0u; }
    void add(const FlowObjectSet&);

private:
    Vector<uint32_t> m_words;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
