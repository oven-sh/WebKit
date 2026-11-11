/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSectionWriter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include <algorithm>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static constexpr size_t maxWrittenVectorSize = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());
static_assert(WTF::isValidCapacityForVector<uint8_t>(maxWrittenVectorSize) && !WTF::isValidCapacityForVector<uint8_t>(maxWrittenVectorSize + 1));

bool checkSectionSize(uint64_t size, BuildContext& context)
{
    for (LimitID limit : { LimitID::VectorBytes, LimitID::TableBytes }) {
        if (size > context.limit(limit)) {
            context.reportLimitExceeded(limit, size);
            return false;
        }
    }
    return true;
}

bool checkListSize(uint64_t count, size_t sizeOfElement, BuildContext& context)
{
    if (count <= context.limit(LimitID::VectorBytes) / sizeOfElement)
        return true;
    context.reportLimitExceeded(LimitID::VectorBytes, count * sizeOfElement);
    return false;
}

void growSectionTo(Vector<uint8_t>& bytes, size_t newSize)
{
    ASSERT(newSize <= maxWrittenVectorSize);
    if (newSize > bytes.capacity())
        bytes.reserveCapacity(std::min(maxWrittenVectorSize, std::max(newSize, bytes.capacity() + bytes.capacity() / 2)));
    bytes.grow(newSize);
}

SectionWriter::SectionWriter(BuildContext& context)
    : m_context(context)
{
}

bool SectionWriter::canGrowTo(size_t newSize)
{
    if (!m_didOverflow)
        m_didOverflow = !checkSectionSize(newSize, m_context);
    return !m_didOverflow;
}

uint32_t SectionWriter::append(std::span<const uint8_t> bytes, size_t alignment)
{
    size_t size = m_bytes.size();
    size_t offset = roundUpToMultipleOf(alignment, size);
    if (!canGrowTo(offset + bytes.size()))
        return 0;
    growSectionTo(m_bytes, offset + bytes.size());
    zeroSpan(m_bytes.mutableSpan().subspan(size, offset - size));
    memcpySpan(m_bytes.mutableSpan().subspan(offset), bytes);
    return static_cast<uint32_t>(offset);
}

Vector<uint8_t> SectionWriter::takeBytes()
{
    if (m_didOverflow)
        return { };
    return std::exchange(m_bytes, { });
}

static uint32_t hashOfStream(std::span<const uint8_t> bytes)
{
    constexpr uint64_t multiplier = 1099511628211ull;
    uint64_t hash = 1469598103934665603ull;
    for (; bytes.size() >= sizeof(uint64_t); bytes = bytes.subspan(sizeof(uint64_t))) {
        uint64_t word;
        memcpySpan(asMutableByteSpan(word), bytes.first(sizeof(uint64_t)));
        hash = (hash ^ word) * multiplier;
        hash ^= hash >> 32;
    }
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * multiplier;
    return static_cast<uint32_t>(hash ^ hash >> 32);
}

namespace {

struct HashedStream {
    uint32_t hash;
    uint32_t index;
};

} // anonymous namespace

static Vector<HashedStream> sortStreamsByHash(const ParallelLoops& loops, std::span<const Vector<uint16_t>> streams)
{
    Vector<HashedStream> entries = collectInParallel<HashedStream>(loops, streams.size(), [&](unsigned index, Vector<HashedStream>& hashed) {
        if (!streams[index].isEmpty())
            hashed.append({ hashOfStream(asBytes(streams[index].span())), index });
    });
    radixSortKeepingEqualKeysInOrder<32>(entries, [](const HashedStream& entry) {
        return entry.hash;
    });
    return entries;
}

Vector<uint32_t> findFirstEqualStreams(std::span<const Vector<uint16_t>> streams, const ParallelLoops& loops)
{
    Vector<HashedStream> entries = sortStreamsByHash(loops, streams);
    Vector<uint32_t> firstEqualStream(streams.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    forEachRangeOfIndicesInParallel(loops, entries.size(), [&](size_t at, size_t end) {
        while (at && at < end && entries[at].hash == entries[at - 1].hash)
            ++at;
        Vector<uint32_t, 4> newStreams;
        while (at < end) {
            uint32_t hash = entries[at].hash;
            newStreams.shrink(0);
            for (; at < entries.size() && entries[at].hash == hash; ++at) {
                uint32_t index = entries[at].index;
                auto bytes = streams[index].span();
                size_t equal = newStreams.findIf([&](uint32_t earlier) {
                    return equalSpans(streams[earlier].span(), bytes);
                });
                if (equal != notFound)
                    firstEqualStream[index] = newStreams[equal];
                else
                    newStreams.append(index);
            }
        }
    });
    return firstEqualStream;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
