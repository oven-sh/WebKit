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
#include <array>
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

template<typename CountForOpcode>
ALWAYS_INLINE auto UnlinkedMetadataTable::layOut(const CountForOpcode& countForOpcode, Offset32* offsets) -> Layout
{
    // 64-bit so that counts out of a payload cannot wrap it; a table anywhere near 4 GB is not one anybody generated.
    uint64_t offset = s_offset16TableSize;
    for (unsigned i = 0; i < s_offsetTableEntries - 1; i++) {
        if (offsets)
            offsets[i] = offset; // aligned by whoever indexes the table, so an opcode without entries costs nothing
        unsigned numberOfEntries = countForOpcode(i);
        if (!numberOfEntries)
            continue;
        unsigned alignment = metadataAlignment(static_cast<OpcodeID>(i));
        ASSERT(alignment <= s_maxMetadataAlignment);
#if CPU(ADDRESS64)
        // This is only necessary for the first metadata entry, if the buffer
        // is 4-byte aligned and the entry has an alignment requirement of 8
        ASSERT(offset == roundUpToMultipleOf(alignment, offset) || offset == s_offset16TableSize);
#endif
        offset = roundUpToMultipleOf(alignment, offset);
        offset += static_cast<uint64_t>(numberOfEntries) * metadataSize(static_cast<OpcodeID>(i));
        RELEASE_ASSERT(offset + s_offset32TableSize <= std::numeric_limits<Offset32>::max());
    }
    if (offsets)
        offsets[s_offsetTableEntries - 1] = offset;
    if (offset <= UINT16_MAX)
        return { false, static_cast<unsigned>(offset) };
    // The 32-bit table sits after the (then unused) 16-bit one; s_offset32TableSize is a multiple of s_maxMetadataAlignment
    // so displacing every offset by it keeps their alignment.
    if (offsets) {
        for (unsigned i = 0; i < s_offsetTableEntries; i++)
            offsets[i] += s_offset32TableSize;
    }
    return { true, static_cast<unsigned>(offset + s_offset32TableSize) };
}

ALWAYS_INLINE auto UnlinkedMetadataTable::layOutEntryCounts(std::span<const uint32_t> entryCounts, Offset32* offsets) -> Layout
{
    std::array<unsigned, s_offsetTableEntries - 1> counts { };
    for (uint32_t entry : entryCounts) {
        unsigned opcode = entry >> entryCountBits;
        RELEASE_ASSERT(opcode < s_offsetTableEntries - 1); // as the encoder wrote them; a malformed payload stops here rather than past the table
        counts[opcode] = entry & ((1u << entryCountBits) - 1);
    }
    return layOut([&](unsigned opcode) { return counts[opcode]; }, offsets);
}

template<typename OffsetType>
ALWAYS_INLINE void UnlinkedMetadataTable::writeOffsets(std::span<const uint32_t> entryCounts, OffsetType* table)
{
    std::array<Offset32, s_offsetTableEntries> offsets;
    Layout layout = layOutEntryCounts(entryCounts, offsets.data());
    ASSERT_UNUSED(layout, layout.is32Bit == (sizeof(OffsetType) == sizeof(Offset32)));
    for (unsigned i = 0; i < s_offsetTableEntries; i++)
        table[i] = offsets[i];
}

// A finalized table as it comes out of the bytecode cache; the value-profile / metadata buffer is allocated at link().
ALWAYS_INLINE UnlinkedMetadataTable::UnlinkedMetadataTable(unsigned numValueProfiles, std::span<const uint32_t> entryCounts)
    : m_hasMetadata(true)
    , m_isFinalized(true)
    , m_isLinked(false)
    , m_is32Bit(layOutEntryCounts(entryCounts, nullptr).is32Bit)
    , m_numValueProfiles(numValueProfiles)
    , m_rawBuffer(static_cast<uint8_t*>(MetadataTableMalloc::zeroedMalloc(m_is32Bit ? s_offset16TableSize + s_offset32TableSize : s_offset16TableSize)))
{
    if (m_is32Bit)
        writeOffsets(entryCounts, offsetTable32());
    else
        writeOffsets(entryCounts, offsetTable16());
}

ALWAYS_INLINE UnlinkedMetadataTable::UnlinkedMetadataTable(PersistentTag, unsigned numValueProfiles, std::span<const uint32_t> entryCounts)
    : m_hasMetadata(true)
    , m_isFinalized(true)
    , m_isLinked(false)
    , m_is32Bit(layOutEntryCounts(entryCounts, nullptr).is32Bit)
    , m_isBackedByEntryCounts(true)
    , m_numValueProfiles(numValueProfiles)
    , m_entryCountsSize(entryCounts.size())
    , m_entryCounts(entryCounts.data())
    , m_rawBuffer(nullptr)
{
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
    if (m_isBackedByEntryCounts && !m_isLinked)
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
    if (m_isBackedByEntryCounts && !m_isLinked) {
        if (m_is32Bit)
            writeOffsets(entryCountsBacking(), std::bit_cast<Offset32*>(table + s_offset16TableSize));
        else
            writeOffsets(entryCountsBacking(), std::bit_cast<Offset16*>(table));
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
    return adoptRef(*new (buffer + valueProfileSize + sizeof(LinkingData)) MetadataTable(*this));
}

ALWAYS_INLINE void UnlinkedMetadataTable::unlink(MetadataTable& metadataTable)
{
    ASSERT(m_isFinalized);
    if (!m_hasMetadata)
        return;

    if (metadataTable.buffer() == buffer()) {
        ASSERT(m_isLinked);
        if (m_isBackedByEntryCounts) {
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
