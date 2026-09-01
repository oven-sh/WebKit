/*
 * Copyright (C) 2018-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "MetadataTable.h"
#include "UnlinkedMetadataTable.h"
#include <wtf/FastMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

ALWAYS_INLINE UnlinkedMetadataTable::UnlinkedMetadataTable()
    : m_hasMetadata(false)
    , m_isFinalized(false)
    , m_isLinked(false)
    , m_is32Bit(false)
    , m_rawBuffer(static_cast<uint8_t*>(MetadataTableMalloc::zeroedMalloc(s_offset32TableSize)))
{
}

// A finalized table as it comes out of the bytecode cache: the caller fills in the offset table; the value-profile /
// metadata buffer is allocated at link().
ALWAYS_INLINE UnlinkedMetadataTable::UnlinkedMetadataTable(bool is32Bit, unsigned numValueProfiles)
    : m_hasMetadata(true)
    , m_isFinalized(false)
    , m_isLinked(false)
    , m_is32Bit(is32Bit)
    , m_numValueProfiles(numValueProfiles)
    , m_rawBuffer(static_cast<uint8_t*>(MetadataTableMalloc::zeroedMalloc(is32Bit ? s_offset16TableSize + s_offset32TableSize : s_offset16TableSize)))
{
}

ALWAYS_INLINE UnlinkedMetadataTable::UnlinkedMetadataTable(unsigned numValueProfiles, std::span<const uint32_t> persistentSteps)
    : m_hasMetadata(true)
    , m_isFinalized(true)
    , m_isLinked(false)
    , m_is32Bit(stepsNeed32BitOffsets(persistentSteps))
    , m_isBackedBySteps(true)
    , m_numValueProfiles(numValueProfiles)
    , m_stepsCount(persistentSteps.size())
    , m_steps(persistentSteps.data())
    , m_rawBuffer(nullptr)
{
}

template<typename OffsetType>
unsigned UnlinkedMetadataTable::expandSteps(std::span<const uint32_t> steps, OffsetType* table)
{
    // finalize()'s layout, from (opcode, count) pairs instead of the preprocess buffer. 64-bit so that counts out of a
    // payload cannot wrap the offset; a table too big for 32-bit offsets is refused rather than truncated.
    uint64_t offset = s_offset16TableSize;
    unsigned i = 0;
    for (uint32_t step : steps) {
        unsigned opcode = step >> stepIndexShift;
        RELEASE_ASSERT(opcode >= i && opcode < s_offsetTableEntries - 1); // ascending, as the encoder wrote them
        for (; i <= opcode; ++i) {
            if (table)
                table[i] = offset; // aligned when accessed, as in finalize()
        }
        offset = roundUpToMultipleOf(metadataAlignment(static_cast<OpcodeID>(opcode)), offset);
        offset += static_cast<uint64_t>(step & stepCountMask) * metadataSize(static_cast<OpcodeID>(opcode));
        RELEASE_ASSERT(offset + s_offset32TableSize <= std::numeric_limits<Offset32>::max());
    }
    for (; i < s_offsetTableEntries; ++i) {
        if (table)
            table[i] = offset;
    }
    if (offset > UINT16_MAX) {
        ASSERT(!table || sizeof(OffsetType) == sizeof(Offset32));
        if (table) {
            for (i = 0; i < s_offsetTableEntries; ++i)
                table[i] += s_offset32TableSize;
        }
        return offset + s_offset32TableSize;
    }
    return offset;
}

ALWAYS_INLINE UnlinkedMetadataTable::UnlinkedMetadataTable(EmptyTag)
    : m_hasMetadata(false)
    , m_isFinalized(true)
    , m_isLinked(false)
    , m_is32Bit(false)
    , m_rawBuffer(nullptr)
{
}

ALWAYS_INLINE unsigned UnlinkedMetadataTable::addEntry(OpcodeID opcodeID)
{
    ASSERT(!m_isFinalized && opcodeID < s_offsetTableEntries - 1);
    m_hasMetadata = true;
    return preprocessBuffer()[opcodeID]++;
}

ALWAYS_INLINE unsigned UnlinkedMetadataTable::addValueProfile()
{
    ASSERT(!m_isFinalized);
    m_hasMetadata = true;
    // Preinecrement because we want the first value profile's offset to be 1, since it's negative indexed.
    return ++m_numValueProfiles;
}

template <typename Bytecode>
ALWAYS_INLINE unsigned UnlinkedMetadataTable::numEntries()
{
    constexpr auto opcodeID = Bytecode::opcodeID;
    ASSERT(!m_isFinalized && opcodeID < s_offsetTableEntries - 1);
    return preprocessBuffer()[opcodeID];
}

ALWAYS_INLINE size_t UnlinkedMetadataTable::sizeInBytesForGC()
{
    if (m_isFinalized && !m_hasMetadata)
        return 0;

    if (m_is32Bit)
        return s_offset16TableSize + s_offset32TableSize;
    return s_offset16TableSize;
}

ALWAYS_INLINE size_t UnlinkedMetadataTable::sizeInBytesForGC(MetadataTable& metadataTable)
{
    ASSERT(m_isFinalized);

    // In this case, we return the size of the table minus the offset table,
    // which was already accounted for in the UnlinkedCodeBlock.

    // Be careful not to touch m_rawBuffer if this metadataTable is not owning it.
    // It is possible that, m_rawBuffer is realloced in the other thread while we are accessing here.
    size_t result = metadataTable.totalSize();
    if (metadataTable.buffer() == buffer()) {
        ASSERT(m_isLinked);
        if (m_is32Bit)
            return result - (s_offset16TableSize + s_offset32TableSize);
        return result - s_offset16TableSize;
    }
    return result;
}

ALWAYS_INLINE RefPtr<MetadataTable> UnlinkedMetadataTable::link()
{
    ASSERT(m_isFinalized);

    if (!m_hasMetadata)
        return nullptr;

    unsigned totalSize = this->totalSize();
    unsigned offsetTableSize = this->offsetTableSize();
    unsigned valueProfileSize = m_numValueProfiles * sizeof(ValueProfile);
    uint8_t* buffer = static_cast<uint8_t*>(MetadataTableMalloc::zeroedMalloc(sizeof(LinkingData) + totalSize));
    uint8_t* table = buffer + valueProfileSize + sizeof(LinkingData);
    if (m_isBackedBySteps && !m_isLinked) {
        if (m_is32Bit)
            expandSteps(std::span { m_steps, m_stepsCount }, std::bit_cast<Offset32*>(table + s_offset16TableSize));
        else
            expandSteps(std::span { m_steps, m_stepsCount }, std::bit_cast<Offset16*>(table));
    } else
        memcpy(table, this->buffer(), offsetTableSize);
    if (!m_isLinked) {
        if (m_rawBuffer)
            MetadataTableMalloc::free(m_rawBuffer);
        m_rawBuffer = buffer;
        m_isLinked = true;
    } else {
#if ENABLE(METADATA_STATISTICS)
        MetadataStatistics::numberOfCopiesFromLinking++;
        MetadataStatistics::linkingCopyMemory += sizeof(LinkingData) + totalSize;
#endif
    }
    // FIXME: Is this needed since we'll clear the data in the CodeBlock Constructor... Plus I could see caching value profiles being profitable.
#if TSAN_ENABLED
    // THREADS/TSAN-gated (TSAN-TRIAGE family codeblock-init): the value-profile
    // lanes live on MetadataTableMalloc memory that compiler threads probe with
    // relaxed atomics under the racy-profiling tolerance; on a recycled buffer
    // a plain memset is the undefined side of that pair. Word-wise relaxed
    // stores keep it defined; production keeps memset.
    {
        auto zeroRacy = [](uint8_t* base, size_t size) {
            // Byte-wise (alignment-agnostic); TSAN-only path, perf irrelevant.
            for (size_t i = 0; i < size; ++i)
                WTF::atomicStore(base + i, static_cast<uint8_t>(0), std::memory_order_relaxed);
        };
        zeroRacy(buffer, valueProfileSize);
        zeroRacy(buffer + valueProfileSize + sizeof(LinkingData) + offsetTableSize, totalSize - offsetTableSize - valueProfileSize);
    }
#else
    memset(buffer, 0, valueProfileSize);
    memset(buffer + valueProfileSize + sizeof(LinkingData) + offsetTableSize, 0, totalSize - offsetTableSize - valueProfileSize);
#endif
    return adoptRef(*new (buffer + valueProfileSize + sizeof(LinkingData)) MetadataTable(*this));
}

ALWAYS_INLINE void UnlinkedMetadataTable::unlink(MetadataTable& metadataTable)
{
    ASSERT(m_isFinalized);
    if (!m_hasMetadata)
        return;

    if (metadataTable.buffer() == buffer()) {
        ASSERT(m_isLinked);
        if (m_isBackedBySteps) {
            MetadataTableMalloc::free(m_rawBuffer);
            m_rawBuffer = nullptr;
            m_isLinked = false;
            return;
        }
        unsigned offsetTableSize = this->offsetTableSize();
        uint8_t* compact = static_cast<uint8_t*>(MetadataTableMalloc::malloc(offsetTableSize));
        memcpy(compact, buffer(), offsetTableSize);
        MetadataTableMalloc::free(m_rawBuffer);
        m_rawBuffer = compact;
        m_isLinked = false;
        return;
    }
    MetadataTableMalloc::free(metadataTable.valueProfilesEnd() + -static_cast<ptrdiff_t>(numValueProfiles()));
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
