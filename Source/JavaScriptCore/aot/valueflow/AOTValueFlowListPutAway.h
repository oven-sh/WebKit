/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

// A list of plain records that nobody reads for a while. Neighbors in such a list are alike, so each word is kept as its distance from the same word of the record before, in as few bytes as that takes.
template<typename T>
class ValueFlowListPutAway {
    static_assert(std::is_trivially_copyable_v<T> && !(sizeof(T) % sizeof(uint32_t)));
public:
    void putAway(const ParallelLoops& loops, Vector<T>&& list)
    {
        RELEASE_ASSERT(!m_size);
        m_size = list.size();
        m_chunks = Vector<Vector<uint8_t>>((m_size + recordsPerChunk - 1) / recordsPerChunk);
        forEachIndexInParallel(loops, m_chunks.size(), [&](unsigned chunk) {
            Vector<uint8_t> bytes;
            Words before { };
            for (const T& record : list.subspan(chunk * recordsPerChunk, std::min(recordsPerChunk, m_size - chunk * recordsPerChunk))) {
                Words words;
                memcpy(words.data(), &record, sizeof(T));
                for (size_t index = 0; index < wordsPerRecord; ++index) {
                    uint32_t distance = words[index] - before[index];
                    for (distance = distance << 1 ^ (distance >> 31 ? UINT32_MAX : 0); distance >= 0x80; distance >>= 7)
                        bytes.append(static_cast<uint8_t>(distance | 0x80));
                    bytes.append(static_cast<uint8_t>(distance));
                }
                before = words;
            }
            bytes.shrinkToFit();
            m_chunks[chunk] = WTF::move(bytes);
        });
        list = { };
    }

    Vector<T> takeOut(const ParallelLoops& loops)
    {
        Vector<T> list(m_size);
        forEachIndexInParallel(loops, m_chunks.size(), [&](unsigned chunk) {
            std::span<const uint8_t> bytes = m_chunks[chunk].span();
            size_t position = 0;
            Words words { };
            for (T& record : list.mutableSpan().subspan(chunk * recordsPerChunk, std::min(recordsPerChunk, m_size - chunk * recordsPerChunk))) {
                for (size_t index = 0; index < wordsPerRecord; ++index) {
                    uint32_t distance = 0;
                    for (unsigned shift = 0;; shift += 7) {
                        uint8_t byte = bytes[position++];
                        distance |= static_cast<uint32_t>(byte & 0x7f) << shift;
                        if (!(byte & 0x80))
                            break;
                    }
                    words[index] += distance >> 1 ^ (distance & 1 ? UINT32_MAX : 0);
                }
                memcpy(static_cast<void*>(&record), words.data(), sizeof(T));
            }
            m_chunks[chunk] = { };
        });
        m_chunks = { };
        m_size = 0;
        return list;
    }

    size_t sizeInBytes() const
    {
        size_t size = 0;
        for (const Vector<uint8_t>& chunk : m_chunks)
            size += chunk.size();
        return size;
    }

private:
    static constexpr size_t recordsPerChunk = 1 << 14;
    static constexpr size_t wordsPerRecord = sizeof(T) / sizeof(uint32_t);
    using Words = std::array<uint32_t, wordsPerRecord>;

    size_t m_size { 0 };
    Vector<Vector<uint8_t>> m_chunks;
};

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
