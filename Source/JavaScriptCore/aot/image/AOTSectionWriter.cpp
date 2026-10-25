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

void growSectionTo(Vector<uint8_t>& bytes, size_t newSize)
{
    ASSERT(newSize <= maxWrittenVectorSize);
    // Vector::grow() reserves more than it needs, and crashes when that is more than a Vector can hold.
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

static uint64_t hashBytes(std::span<const uint8_t> bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : bytes)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

namespace {

struct HashedStream {
    uint64_t hash;
    uint32_t index;
};

} // anonymous namespace

static Vector<HashedStream> sortStreamsByHash(const ParallelLoops& loops, std::span<const Vector<uint8_t>> streams)
{
    Vector<HashedStream> entries = collectInParallel<HashedStream>(loops, streams.size(), [&](unsigned index, Vector<HashedStream>& hashed) {
        if (!streams[index].isEmpty())
            hashed.append({ hashBytes(streams[index].span()), index });
    });
    parallelSort(loops, entries.mutableSpan(), [](const HashedStream& a, const HashedStream& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.index < b.index;
    });
    return entries;
}

Vector<uint32_t> SectionWriter::appendAll(std::span<const Vector<uint8_t>> streams)
{
    Vector<uint32_t> offsets(FillWith { }, streams.size(), 0);
    if (m_didOverflow)
        return offsets;

    const ParallelLoops& loops = m_context.parallelLoops();
    Vector<HashedStream> entries = sortStreamsByHash(loops, streams);
    // The first stream with the same bytes: the stream itself where its bytes are stored.
    Vector<uint32_t> firstEqualStream(streams.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    constexpr size_t entriesInChunk = 4096;
    forEachIndexInParallel(loops, (entries.size() + entriesInChunk - 1) / entriesInChunk, [&](unsigned chunk) {
        size_t end = std::min(entries.size(), (static_cast<size_t>(chunk) + 1) * entriesInChunk);
        size_t at = static_cast<size_t>(chunk) * entriesInChunk;
        while (at && at < end && entries[at].hash == entries[at - 1].hash)
            ++at;
        Vector<uint32_t, 4> newStreams;
        while (at < end) {
            uint64_t hash = entries[at].hash;
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

    size_t newSize = m_bytes.size();
    for (size_t index = 0; index < streams.size(); ++index) {
        if (streams[index].isEmpty() || firstEqualStream[index] != index)
            continue;
        offsets[index] = static_cast<uint32_t>(newSize);
        newSize += streams[index].size();
    }
    if (!canGrowTo(newSize)) {
        offsets.fill(0);
        return offsets;
    }
    growSectionTo(m_bytes, newSize);
    std::span<uint8_t> section = m_bytes.mutableSpan();
    forEachIndexInParallel(loops, streams.size(), [&](unsigned index) {
        if (firstEqualStream[index] != index)
            offsets[index] = offsets[firstEqualStream[index]];
        else if (!streams[index].isEmpty())
            memcpySpan(section.subspan(offsets[index]), streams[index].span());
    });
    return offsets;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
