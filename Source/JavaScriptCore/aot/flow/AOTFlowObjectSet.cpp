/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectSet.h"

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <functional>
#include <wtf/CommaPrinter.h>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

size_t FlowObjectSet::size() const
{
    size_t result = 0;
    for (uint64_t chunk : chunks())
        result += static_cast<size_t>(std::popcount(chunkBitsOf(chunk)));
    return result;
}

bool FlowObjectSet::hasMoreObjectsThan(size_t count) const
{
    size_t result = 0;
    for (uint64_t chunk : chunks()) {
        result += static_cast<size_t>(std::popcount(chunkBitsOf(chunk)));
        if (result > count)
            return true;
    }
    return false;
}

bool FlowObjectSet::contains(FlowObjectID object) const
{
    if (!object)
        return false;
    uint32_t index = object.index() / objectsPerChunk;
    uint32_t bit = 1u << (object.index() % objectsPerChunk);
    std::span<const uint64_t> own = chunks();
    auto found = std::ranges::lower_bound(own, index, { }, chunkIndexOf);
    return found != own.end() && chunkIndexOf(*found) == index && (chunkBitsOf(*found) & bit);
}

bool FlowObjectSet::isSubsetOfChunksOf(const FlowObjectSet& other) const
{
    std::span<const uint64_t> own = chunks();
    std::span<const uint64_t> otherChunks = other.chunks();
    if (own.size() > otherChunks.size())
        return false;
    size_t position = 0;
    for (uint64_t chunk : own) {
        uint32_t index = chunkIndexOf(chunk);
        position = findLowerBound(otherChunks, position, index);
        if (position == otherChunks.size() || chunkIndexOf(otherChunks[position]) != index)
            return false;
        if (chunkBitsOf(chunk) & ~chunkBitsOf(otherChunks[position]))
            return false;
    }
    return true;
}

bool FlowObjectSet::overlaps(const FlowObjectSet& other) const
{
    std::span<const uint64_t> fewer = chunks();
    std::span<const uint64_t> more = other.chunks();
    if (fewer.size() > more.size())
        std::swap(fewer, more);
    size_t position = 0;
    for (uint64_t chunk : fewer) {
        uint32_t index = chunkIndexOf(chunk);
        position = findLowerBound(more, position, index);
        if (position == more.size())
            return false;
        if (chunkIndexOf(more[position]) == index && (chunkBitsOf(chunk) & chunkBitsOf(more[position])))
            return true;
    }
    return false;
}

bool FlowObjectSet::hasMoreCommonObjectsThan(const FlowObjectBitmap& other, size_t count) const
{
    size_t result = 0;
    for (uint64_t chunk : chunks()) {
        result += static_cast<size_t>(std::popcount(chunkBitsOf(chunk) & other.bitsOfChunk(chunkIndexOf(chunk))));
        if (result > count)
            return true;
    }
    return false;
}

FlowObjectID FlowObjectSet::onlyObject() const
{
    std::span<const uint64_t> own = chunks();
    if (own.size() != 1 || !std::has_single_bit(chunkBitsOf(own[0])))
        return { };
    return FlowObjectID(chunkIndexOf(own[0]) * objectsPerChunk + static_cast<uint32_t>(std::countr_zero(chunkBitsOf(own[0]))));
}

bool FlowObjectSet::add(FlowObjectID object)
{
    RELEASE_ASSERT(object && object.index() <= maxObjectIndex);
    uint32_t index = object.index() / objectsPerChunk;
    uint32_t bit = 1u << (object.index() % objectsPerChunk);
    if (isEmpty()) {
        m_word = inlineTag | makeChunk(index, bit);
        return true;
    }

    std::span<uint64_t> own = mutableChunks();
    auto found = std::ranges::lower_bound(own, index, { }, chunkIndexOf);
    if (found != own.end() && chunkIndexOf(*found) == index) {
        bool wasClear = !(chunkBitsOf(*found) & bit);
        *found |= bit;
        return wasClear;
    }

    const uint64_t chunk = makeChunk(index, bit);
    insertChunks(singleElementSpan(chunk));
    return true;
}

void FlowObjectSet::add(std::span<const FlowObjectID> objects)
{
    constexpr size_t maxObjectsToAddOneByOne = 8;
    if (objects.size() <= maxObjectsToAddOneByOne) {
        for (FlowObjectID object : objects)
            add(object);
        return;
    }

    Vector<FlowObjectID, 64> sortedObjects;
    if (!std::ranges::is_sorted(objects)) {
        sortedObjects.append(objects);
        std::ranges::sort(sortedObjects);
        objects = sortedObjects.span();
    }

    ChunkBuffer ascendingChunks;
    for (FlowObjectID object : objects) {
        RELEASE_ASSERT(object && object.index() <= maxObjectIndex);
        uint32_t index = object.index() / objectsPerChunk;
        uint32_t bit = 1u << (object.index() % objectsPerChunk);
        if (!ascendingChunks.isEmpty() && chunkIndexOf(ascendingChunks.last()) == index)
            ascendingChunks.last() |= bit;
        else
            ascendingChunks.append(makeChunk(index, bit));
    }

    if (isEmpty())
        adopt(ascendingChunks.span());
    else
        uniteChunks(ascendingChunks.span(), nullptr);
}

bool FlowObjectSet::remove(FlowObjectID object)
{
    if (!object)
        return false;
    uint32_t index = object.index() / objectsPerChunk;
    uint32_t bit = 1u << (object.index() % objectsPerChunk);
    std::span<uint64_t> own = mutableChunks();
    auto found = std::ranges::lower_bound(own, index, { }, chunkIndexOf);
    if (found == own.end() || chunkIndexOf(*found) != index || !(chunkBitsOf(*found) & bit))
        return false;
    if (chunkBitsOf(*found) != bit) {
        *found &= ~static_cast<uint64_t>(bit);
        return true;
    }

    size_t position = static_cast<size_t>(found - own.begin());
    ChunkBuffer remaining;
    remaining.append(own.first(position));
    remaining.append(own.subspan(position + 1));
    adopt(remaining.span());
    return true;
}

bool FlowObjectSet::unite(const FlowObjectSet& source)
{
    if (&source == this || source.isEmpty())
        return false;
    if (isEmpty()) {
        *this = source;
        return true;
    }
    return uniteChunks(source.chunks(), nullptr);
}

bool FlowObjectSet::uniteAndCollectChunks(const FlowObjectSet& source, const FlowObjectBitmap* excluded, FlowObjectSet& added)
{
    RELEASE_ASSERT(&added != this && &added != &source);
    if (&source == this || source.isEmpty())
        return false;
    if (isEmpty() && !excluded) {
        *this = source;
        added.unite(source);
        return true;
    }

    ChunkBuffer fresh;
    if (!uniteChunks(source.chunks(), &fresh, excluded))
        return false;
    if (added.isEmpty())
        added.adopt(fresh.span());
    else
        added.uniteChunks(fresh.span(), nullptr);
    return true;
}

void FlowObjectSet::intersect(const FlowObjectSet& other)
{
    std::span<const uint64_t> fewer = chunks();
    std::span<const uint64_t> more = other.chunks();
    if (fewer.size() > more.size())
        std::swap(fewer, more);
    ChunkBuffer common;
    size_t position = 0;
    for (uint64_t chunk : fewer) {
        uint32_t index = chunkIndexOf(chunk);
        position = findLowerBound(more, position, index);
        if (position == more.size())
            break;
        if (chunkIndexOf(more[position]) != index)
            continue;
        if (uint32_t bits = chunkBitsOf(chunk) & chunkBitsOf(more[position]))
            common.append(makeChunk(index, bits));
    }
    adopt(common.span());
}

void FlowObjectSet::subtract(const FlowObjectSet& other)
{
    std::span<const uint64_t> otherChunks = other.chunks();
    ChunkBuffer remaining;
    size_t position = 0;
    for (uint64_t chunk : chunks()) {
        uint32_t index = chunkIndexOf(chunk);
        uint32_t bits = chunkBitsOf(chunk);
        position = findLowerBound(otherChunks, position, index);
        if (position < otherChunks.size() && chunkIndexOf(otherChunks[position]) == index)
            bits &= ~chunkBitsOf(otherChunks[position]);
        if (bits)
            remaining.append(makeChunk(index, bits));
    }
    adopt(remaining.span());
}

void FlowObjectSet::keepBits(NOESCAPE const Invocable<uint32_t(uint32_t index, uint32_t bits)> auto& keptBitsOf)
{
    if (isInline()) {
        uint32_t index = chunkIndexOf(m_word);
        uint32_t bits = chunkBitsOf(m_word) ? keptBitsOf(index, chunkBitsOf(m_word)) : 0u;
        m_word = bits ? inlineTag | makeChunk(index, bits) : inlineTag;
        return;
    }

    std::span<uint64_t> own = storage()->chunks();
    size_t numberOfKeptChunks = 0;
    for (uint64_t chunk : own) {
        uint32_t index = chunkIndexOf(chunk);
        if (uint32_t bits = keptBitsOf(index, chunkBitsOf(chunk)))
            own[numberOfKeptChunks++] = makeChunk(index, bits);
    }
    if (numberOfKeptChunks > 1) {
        storage()->size = static_cast<uint32_t>(numberOfKeptChunks);
        return;
    }

    uint64_t word = numberOfKeptChunks ? inlineTag | own[0] : inlineTag;
    freeStorage();
    m_word = word;
}

void FlowObjectSet::intersect(const FlowObjectBitmap& other)
{
    keepBits([&](uint32_t index, uint32_t bits) {
        return bits & other.bitsOfChunk(index);
    });
}

void FlowObjectSet::subtract(const FlowObjectBitmap& other)
{
    keepBits([&](uint32_t index, uint32_t bits) {
        return bits & ~other.bitsOfChunk(index);
    });
}

void FlowObjectSet::subtractCommonObjectsOf(const FlowObjectSet& first, const FlowObjectBitmap& second)
{
    RELEASE_ASSERT(&first != this);
    std::span<const uint64_t> firstChunks = first.chunks();
    size_t position = 0;
    keepBits([&](uint32_t index, uint32_t bits) {
        uint32_t candidates = bits & second.bitsOfChunk(index);
        if (!candidates)
            return bits;
        position = findLowerBound(firstChunks, position, index);
        if (position == firstChunks.size() || chunkIndexOf(firstChunks[position]) != index)
            return bits;
        return bits & ~(candidates & chunkBitsOf(firstChunks[position]));
    });
}

void FlowObjectBitmap::add(const FlowObjectSet& objects)
{
    std::span<const uint64_t> chunks = objects.chunks();
    if (chunks.empty())
        return;

    size_t numberOfWords = FlowObjectSet::chunkIndexOf(chunks.back()) + 1;
    if (numberOfWords > m_words.size())
        m_words.insertFill(m_words.size(), 0u, std::max(numberOfWords, m_words.size() * 2) - m_words.size());
    for (uint64_t chunk : chunks)
        m_words[FlowObjectSet::chunkIndexOf(chunk)] |= FlowObjectSet::chunkBitsOf(chunk);
}

size_t FlowObjectSet::memoryInBytes() const
{
    if (isInline())
        return 0;
    return sizeof(Storage) + storage()->capacity * sizeof(uint64_t);
}

void FlowObjectSet::dump(PrintStream& out) const
{
    CommaPrinter comma;
    out.print('{');
    forEach([&](FlowObjectID object) {
        out.print(comma);
        if (object == unknownFlowObject)
            out.print("unknown"_s);
        else
            out.print('o', object.index());
    });
    out.print('}');
}

bool operator==(const FlowObjectSet& a, const FlowObjectSet& b)
{
    return std::ranges::equal(a.chunks(), b.chunks(), { }, FlowObjectSet::withoutInlineTag, FlowObjectSet::withoutInlineTag);
}

auto FlowObjectSet::allocateStorage(uint32_t capacity) -> Storage*
{
    auto* allocated = static_cast<Storage*>(fastMalloc(sizeof(Storage) + capacity * sizeof(uint64_t)));
    allocated->size = 0;
    allocated->capacity = capacity;
    return allocated;
}

void FlowObjectSet::copyOutOfLine(const FlowObjectSet& other)
{
    adopt(other.chunks());
}

void FlowObjectSet::freeStorage()
{
    fastFree(storage());
}

void FlowObjectSet::adopt(std::span<const uint64_t> ascendingChunks)
{
    ASSERT(std::ranges::all_of(ascendingChunks, chunkBitsOf));
    ASSERT(std::ranges::adjacent_find(ascendingChunks, std::ranges::greater_equal { }, chunkIndexOf) == ascendingChunks.end());
    if (ascendingChunks.size() <= 1) {
        uint64_t word = ascendingChunks.empty() ? inlineTag : inlineTag | ascendingChunks[0];
        if (!isInline())
            freeStorage();
        m_word = word;
        return;
    }

    uint32_t chunkCount = static_cast<uint32_t>(ascendingChunks.size());
    if (isInline() || storage()->capacity < chunkCount) {
        if (!isInline())
            freeStorage();
        setStorage(allocateStorage(std::max<uint32_t>(4, roundUpToPowerOfTwo(chunkCount))));
    }
    std::ranges::transform(ascendingChunks, storage()->allocatedChunks().begin(), withoutInlineTag);
    storage()->size = chunkCount;
}

void FlowObjectSet::reserveCapacity(uint32_t capacity)
{
    if (!isInline() && storage()->capacity >= capacity)
        return;
    std::span<const uint64_t> own = chunks();
    Storage* grown = allocateStorage(std::max<uint32_t>(4, roundUpToPowerOfTwo(capacity)));
    std::ranges::transform(own, grown->allocatedChunks().begin(), withoutInlineTag);
    grown->size = static_cast<uint32_t>(own.size());
    if (!isInline())
        freeStorage();
    setStorage(grown);
}

void FlowObjectSet::insertChunks(std::span<const uint64_t> ascendingAbsentChunks)
{
    ASSERT(!isEmpty() && !ascendingAbsentChunks.empty());
    size_t readEnd = chunks().size();
    size_t writeEnd = readEnd + ascendingAbsentChunks.size();
    uint32_t chunkCount = static_cast<uint32_t>(writeEnd);
    reserveCapacity(chunkCount);

    std::span<uint64_t> allocated = storage()->allocatedChunks();
    for (size_t remaining = ascendingAbsentChunks.size(); remaining--;) {
        uint64_t chunk = withoutInlineTag(ascendingAbsentChunks[remaining]);
        std::span<uint64_t> unmoved = allocated.first(readEnd);
        size_t position = static_cast<size_t>(std::ranges::lower_bound(unmoved, chunkIndexOf(chunk), { }, chunkIndexOf) - unmoved.begin());
        std::span<uint64_t> tail = unmoved.subspan(position);
        writeEnd -= tail.size();
        memmoveSpan(allocated.subspan(writeEnd, tail.size()), tail);
        allocated[--writeEnd] = chunk;
        readEnd = position;
    }
    ASSERT(writeEnd == readEnd);
    storage()->size = chunkCount;
}

bool FlowObjectSet::uniteChunks(std::span<const uint64_t> ascendingChunks, ChunkBuffer* fresh, const FlowObjectBitmap* excluded)
{
    static_assert(!unknownFlowObject.index());
    std::span<uint64_t> own = mutableChunks();
    ChunkBuffer absent;
    bool didChange = false;
    size_t position = 0;
    for (uint64_t chunk : ascendingChunks) {
        uint32_t index = chunkIndexOf(chunk);
        position = findLowerBound(own, position, index);
        bool hasChunk = position != own.size() && chunkIndexOf(own[position]) == index;
        uint32_t freshBits = hasChunk ? chunkBitsOf(chunk) & ~chunkBitsOf(own[position]) : chunkBitsOf(chunk);
        if (!freshBits)
            continue;

        if (excluded) {
            uint32_t neverExcludedBits = index ? 0u : 1u;
            freshBits &= ~excluded->bitsOfChunk(index) | neverExcludedBits;
            if (!freshBits)
                continue;
        }

        if (hasChunk) {
            own[position] |= freshBits;
            didChange = true;
        } else
            absent.append(makeChunk(index, freshBits));
        if (fresh)
            fresh->append(makeChunk(index, freshBits));
    }
    if (absent.isEmpty())
        return didChange;
    if (isEmpty())
        adopt(absent.span());
    else
        insertChunks(absent.span());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
