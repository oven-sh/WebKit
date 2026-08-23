/*
 * Copyright (C) 2019-2023 Apple Inc. All rights reserved.
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

#include "config.h"
#include "UnlinkedMetadataTable.h"

#include "BytecodeStructs.h"
#include "UnlinkedMetadataTableInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MetadataTable);
DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(UnlinkedMetadataTable);

#if CPU(ADDRESS64)
static_assert((UnlinkedMetadataTable::s_maxMetadataAlignment >=
#define JSC_ALIGNMENT_CHECK(size) size) && (size >=
FOR_EACH_BYTECODE_METADATA_ALIGNMENT(JSC_ALIGNMENT_CHECK)
#undef JSC_ALIGNMENT_CHECK
1));
#else
#define JSC_ALIGNMENT_CHECK(size) static_assert(size <= UnlinkedMetadataTable::s_maxMetadataAlignment);
FOR_EACH_BYTECODE_METADATA_ALIGNMENT(JSC_ALIGNMENT_CHECK)
#undef JSC_ALIGNMENT_CHECK
#endif

#if ENABLE(METADATA_STATISTICS)
size_t MetadataStatistics::unlinkedMetadataCount = 0;
size_t MetadataStatistics::size32MetadataCount = 0;
size_t MetadataStatistics::totalMemory = 0;
size_t MetadataStatistics::perOpcodeCount[NUMBER_OF_BYTECODE_WITH_METADATA] { 0 };
size_t MetadataStatistics::numberOfCopiesFromLinking = 0;
size_t MetadataStatistics::linkingCopyMemory = 0;

void MetadataStatistics::reportMetadataStatistics()
{
    static constexpr bool verbose = true;

    dataLogLn("\nMetadata statistics\n");

    totalMemory += unlinkedMetadataCount * sizeof(UnlinkedMetadataTable::LinkingData);
    totalMemory += size32MetadataCount * (UnlinkedMetadataTable::s_offset32TableSize);
    totalMemory += linkingCopyMemory;
    dataLogLn("total memory: ", totalMemory);
    if (verbose)
        dataLogLn("\t of which due to multiple linked copies: ", linkingCopyMemory);

    dataLogLn("# of unlinked metadata tables created: ", unlinkedMetadataCount);
    dataLogLn("# of which were 32bit: ", size32MetadataCount);
    dataLogLn("# of copies from linking: ", numberOfCopiesFromLinking);
    dataLogLn();

    if (!verbose)
        return;

    dataLogLn("Per opcode statistics:");
    std::array<unsigned, NUMBER_OF_BYTECODE_WITH_METADATA> opcodeIds;
    std::array<size_t, NUMBER_OF_BYTECODE_WITH_METADATA> memoryUsagePerOpcode;
    for (unsigned i = 0; i < NUMBER_OF_BYTECODE_WITH_METADATA; ++i) {
        opcodeIds[i] = i;
        memoryUsagePerOpcode[i] = perOpcodeCount[i] * metadataSize(static_cast<OpcodeID>(i));
    }
    std::ranges::sort(opcodeIds, [&](auto a, auto b) {
        return memoryUsagePerOpcode[a] > memoryUsagePerOpcode[b];
    });
    for (unsigned i = 0; i < NUMBER_OF_BYTECODE_WITH_METADATA; ++i) {
        auto id = opcodeIds[i];
        auto numberOfEntries = perOpcodeCount[id];
        if (!numberOfEntries)
            continue;
        dataLogLn(opcodeNames[id], ":");
        dataLogLn("\tnumber of entries: ", numberOfEntries);
        dataLogLn("\tmemory usage: ", memoryUsagePerOpcode[id]);
    }
}
#endif

void UnlinkedMetadataTable::finalize()
{
    ASSERT(!m_isFinalized);
    m_isFinalized = true;
    if (!m_hasMetadata) {
        MetadataTableMalloc::free(m_rawBuffer);
        m_rawBuffer = nullptr;
        return;
    }

    std::array<Offset32, s_offsetTableEntries> offsets;
    Layout layout = layOut([&](unsigned opcode) { return preprocessBuffer()[opcode]; }, offsets.data());
    m_is32Bit = layout.is32Bit;

#if ENABLE(METADATA_STATISTICS)
    for (unsigned i = 0; i < s_offsetTableEntries - 1; i++)
        MetadataStatistics::perOpcodeCount[i] += preprocessBuffer()[i];
    MetadataStatistics::unlinkedMetadataCount++;
    if (m_is32Bit)
        MetadataStatistics::size32MetadataCount++;
    MetadataStatistics::totalMemory += layout.end;
    static std::once_flag once;
    std::call_once(once, [] {
        std::atexit(MetadataStatistics::reportMetadataStatistics);
    });
#endif

    ASSERT(!m_isLinked);
    MetadataTableMalloc::free(m_rawBuffer);
    if (m_is32Bit) {
        m_rawBuffer = static_cast<uint8_t*>(MetadataTableMalloc::zeroedMalloc(s_offset16TableSize + s_offset32TableSize));
        std::copy(offsets.begin(), offsets.end(), offsetTable32());
    } else {
        m_rawBuffer = static_cast<uint8_t*>(MetadataTableMalloc::zeroedMalloc(s_offset16TableSize));
        std::copy(offsets.begin(), offsets.end(), offsetTable16());
    }
}

Vector<uint32_t, 16> UnlinkedMetadataTable::entryCounts() const
{
    ASSERT(m_isFinalized && m_hasMetadata);
    Vector<uint32_t, 16> result;
    if (m_isBackedByEntryCounts && !m_isLinked) {
        result.append(entryCountsBacking());
        return result;
    }
    // The range computation MetadataTable::forEach() does, divided by the entry size.
    auto offset = [&](unsigned i) -> unsigned { return m_is32Bit ? offsetTable32()[i] : offsetTable16()[i]; };
    for (unsigned i = 0; i < s_offsetTableEntries - 1; i++) {
        unsigned start = roundUpToMultipleOf(metadataAlignment(static_cast<OpcodeID>(i)), offset(i));
        unsigned end = offset(i + 1);
        if (start >= end)
            continue;
        unsigned count = (end - start) / metadataSize(static_cast<OpcodeID>(i));
        ASSERT(count && start + count * metadataSize(static_cast<OpcodeID>(i)) == end);
        RELEASE_ASSERT(count < 1u << entryCountBits);
        result.append(i << entryCountBits | count);
    }
#if ASSERT_ENABLED
    std::array<Offset32, s_offsetTableEntries> roundTrip;
    ASSERT(layOutEntryCounts(result.span(), roundTrip.data()).is32Bit == m_is32Bit);
    for (unsigned i = 0; i < s_offsetTableEntries; i++)
        ASSERT(roundTrip[i] == offset(i));
#endif
    return result;
}

UnlinkedMetadataTable::~UnlinkedMetadataTable()
{
    ASSERT(!m_isLinked);
    if (m_hasMetadata && m_rawBuffer)
        MetadataTableMalloc::free(m_rawBuffer);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
