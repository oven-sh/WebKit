/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
#include "YarrClassTable.h"

#include <algorithm>
#include <limits>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace Yarr {

void add(Hasher& hasher, const ClassTable& table)
{
    add(hasher, table.form, table.pageBlockIndexes, table.blocks);
    for (auto& range : table.trailingRanges)
        add(hasher, range.begin, range.end);
}

static Vector<CharacterRange> collectClassTableRuns(const CharacterClass& charClass, char32_t greatest)
{
    Vector<CharacterRange> runs;
    for (auto* matches : { &charClass.m_matches8, &charClass.m_matches32 }) {
        for (char32_t match : *matches) {
            if (match <= greatest)
                runs.append(CharacterRange(match, match));
        }
    }
    for (auto* ranges : { &charClass.m_ranges8, &charClass.m_ranges32 }) {
        for (auto& range : *ranges) {
            if (range.begin <= greatest)
                runs.append(CharacterRange(range.begin, std::min(range.end, greatest)));
        }
    }
    std::ranges::sort(runs, { }, &CharacterRange::begin);
    size_t numberOfRuns = 0;
    for (auto run : runs) {
        if (numberOfRuns && run.begin <= runs[numberOfRuns - 1].end + 1)
            runs[numberOfRuns - 1].end = std::max(runs[numberOfRuns - 1].end, run.end);
        else
            runs[numberOfRuns++] = run;
    }
    runs.shrink(numberOfRuns);
    return runs;
}

static size_t computeClassTableFrameSize(ClassTableForm form, size_t pageCount, size_t trailingRangeCount)
{
    size_t slotCount = roundUpToMultipleOf<2>(pageCount);
    switch (form) {
    case ClassTableForm::IndexThenBlocks:
        return slotCount * sizeof(int16_t);
    case ClassTableForm::ForRoutine: {
        RELEASE_ASSERT(pageCount >= ClassTableLayout::firstSurrogatePage);
        size_t trailingWordCount = trailingRangeCount ? 1 + 2 * trailingRangeCount : 0;
        return ClassTableLayout::upperIndexOffset + (slotCount - ClassTableLayout::firstSurrogatePage) * sizeof(int16_t) + trailingWordCount * sizeof(uint32_t);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::expected<ClassTable, ClassTableDecline> buildClassTable(const CharacterClass& charClass, ClassTableForm form, char32_t greatestCharacter)
{
    constexpr char32_t charactersInBlock = ClassTableLayout::charactersInBlock;
    using Block = ClassTable::Block;
    Vector<CharacterRange> runs = collectClassTableRuns(charClass, greatestCharacter);

    constexpr char32_t minGapForTrailingRange = 128 * charactersInBlock;
    auto limitOfFirstRuns = [&](size_t number) -> char32_t {
        return number ? (runs[number - 1].end / charactersInBlock + 1) * charactersInBlock : 0;
    };
    size_t numberInTable = runs.size();
    for (size_t trailingRunCount = 1; trailingRunCount <= ClassTable::maxTrailingRanges && trailingRunCount <= runs.size(); ++trailingRunCount) {
        if (limitOfFirstRuns(runs.size() - trailingRunCount + 1) - limitOfFirstRuns(runs.size() - trailingRunCount) >= minGapForTrailingRange)
            numberInTable = runs.size() - trailingRunCount;
    }
    char32_t limit = limitOfFirstRuns(numberInTable);
    if (!limit)
        return std::unexpected(ClassTableDecline::OnlyTrailingRanges);
    if (form == ClassTableForm::ForRoutine)
        limit = std::max<char32_t>(limit, ClassTableLayout::firstSurrogatePage * charactersInBlock);
    if (limit / charactersInBlock > ClassTablePacker::maxPages)
        return std::unexpected(ClassTableDecline::TooManyPages);

    ClassTable table;
    table.form = form;
    for (size_t i = numberInTable; i < runs.size(); ++i) {
        if (runs[i].end >= limit)
            table.trailingRanges.append(CharacterRange(std::max(runs[i].begin, limit), runs[i].end));
    }
    Vector<Block> pages(FillWith { }, limit / charactersInBlock, Block { });
    for (auto& run : runs) {
        char32_t end = std::min<char32_t>(run.end, limit - 1);
        for (char32_t character = run.begin; character <= end;) {
            auto& word = pages[character / charactersInBlock][character % charactersInBlock / 32];
            if (!(character % 32) && end - character >= 31) {
                word = std::numeric_limits<uint32_t>::max();
                character += 32;
                continue;
            }
            word |= 1u << character % 32;
            ++character;
        }
    }

    std::map<Block, uint16_t> blockIndexes;
    table.pageBlockIndexes.reserveInitialCapacity(pages.size());
    for (auto& page : pages) {
        auto [entry, isNew] = blockIndexes.emplace(page, static_cast<uint16_t>(table.blocks.size()));
        if (isNew)
            table.blocks.append(page);
        table.pageBlockIndexes.append(entry->second);
    }
    size_t frameSize = computeClassTableFrameSize(form, table.pageBlockIndexes.size(), table.trailingRanges.size());
    if (frameSize + table.blocks.size() * ClassTableLayout::bytesInBlock > ClassTablePacker::defaultMaxClusterSize)
        return std::unexpected(ClassTableDecline::TooLarge);
    return table;
}

template<typename T>
static void storeInClassTable(Vector<uint8_t>& bytes, size_t offset, const T& value)
{
    memcpySpan(bytes.mutableSpan().subspan(offset), asByteSpan(value));
}

static size_t computeClassTableSlotOffset(ClassTableForm form, size_t page)
{
    if (form == ClassTableForm::ForRoutine && page >= ClassTableLayout::firstSurrogatePage)
        return ClassTableLayout::upperIndexOffset + (page - ClassTableLayout::firstSurrogatePage) * sizeof(int16_t);
    return page * sizeof(int16_t);
}

static int16_t encodeClassTableSlot(size_t blockOffset, size_t page)
{
    ASSERT(!(blockOffset % sizeof(uint32_t)));
    int64_t word = static_cast<int64_t>(blockOffset / sizeof(uint32_t));
    int64_t value = word - static_cast<int64_t>(page) * ClassTableLayout::wordsInBlock;
    RELEASE_ASSERT(value == static_cast<int16_t>(value));
    return static_cast<int16_t>(value);
}

struct ClassTablePacker::Cluster {
    Vector<size_t> tables;
    Vector<uint32_t> pool;
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> poolSlots;
    size_t frameSize { 0 };
};

ClassTablePacker::ClassTablePacker(size_t maxClusterSize)
    : m_maxClusterSize(maxClusterSize)
{
    RELEASE_ASSERT(maxClusterSize <= defaultMaxClusterSize);
}

uint32_t ClassTablePacker::numberOf(const Block& block)
{
    return m_blockNumbers.emplace(block, static_cast<uint32_t>(m_blockNumbers.size())).first->second;
}

Vector<uint8_t> ClassTablePacker::frameOf(const ClassTable& table, Vector<Entry>& entries)
{
    const bool isForRoutine = table.form == ClassTableForm::ForRoutine;
    const size_t pageCount = table.pageBlockIndexes.size();
    Vector<uint8_t> frame(FillWith { }, computeClassTableFrameSize(table.form, pageCount, 0), static_cast<uint8_t>(0));

    Vector<uint32_t> blockNumbers = table.blocks.map([&](auto& block) {
        return numberOf(block);
    });
    for (size_t page = 0; page < pageCount; ++page) {
        uint32_t block = blockNumbers[table.pageBlockIndexes[page]];
        size_t offsetInFrame = computeClassTableSlotOffset(table.form, page);
        if (isForRoutine && block == blockNumbers[0])
            storeInClassTable(frame, offsetInFrame, encodeClassTableSlot(ClassTableLayout::firstBlockOffset, page));
        else
            entries.append(Entry { static_cast<uint32_t>(offsetInFrame), static_cast<uint32_t>(page), block });
    }
    if (!isForRoutine)
        return frame;

    storeInClassTable(frame, ClassTableLayout::limitOffset, static_cast<uint32_t>(pageCount));
    storeInClassTable(frame, ClassTableLayout::firstBlockOffset, table.blocks[0]);
    if (!table.trailingRanges.isEmpty()) {
        auto appendWord = [&](uint32_t word) {
            frame.append(asByteSpan(word));
        };
        storeInClassTable(frame, ClassTableLayout::trailingRangesOffset, static_cast<uint32_t>(frame.size()));
        appendWord(table.trailingRanges.size());
        for (auto& range : table.trailingRanges) {
            appendWord(range.begin);
            appendWord(range.end - range.begin);
        }
    }
    ASSERT(frame.size() == computeClassTableFrameSize(table.form, pageCount, table.trailingRanges.size()));
    return frame;
}

size_t ClassTablePacker::add(const ClassTable& table)
{
    Table serialized;
    serialized.frame = frameOf(table, serialized.entries);
    auto& blocks = serialized.pooledBlocks;
    for (auto& entry : serialized.entries)
        blocks.append(entry.block);
    std::ranges::sort(blocks);
    blocks.shrink(blocks.size() - std::ranges::unique(blocks).size());
    m_tables.append(WTF::move(serialized));
    return m_tables.size() - 1;
}

void ClassTablePacker::pack()
{
    ASSERT(m_bytes.isEmpty());
    Vector<const Block*> blockContents;
    blockContents.fill(nullptr, m_blockNumbers.size());
    for (auto& [contents, number] : m_blockNumbers)
        blockContents[number] = &contents;

    Vector<size_t> order(m_tables.size(), [](size_t index) {
        return index;
    });
    std::ranges::stable_sort(order, [&](size_t a, size_t b) {
        return std::ranges::lexicographical_compare(m_tables[a].pooledBlocks, m_tables[b].pooledBlocks);
    });

    Cluster cluster;
    for (size_t index : order) {
        auto& table = m_tables[index];
        size_t newBlockCount = std::ranges::count_if(table.pooledBlocks, [&](uint32_t block) {
            return !cluster.poolSlots.contains(block);
        });
        size_t sizeWithTable = cluster.frameSize + table.frame.size() + (cluster.pool.size() + newBlockCount) * ClassTableLayout::bytesInBlock;
        if (!cluster.tables.isEmpty() && sizeWithTable > m_maxClusterSize) {
            layOutCluster(cluster, blockContents);
            cluster = { };
        }
        cluster.tables.append(index);
        cluster.frameSize += table.frame.size();
        for (uint32_t block : table.pooledBlocks) {
            if (cluster.poolSlots.add(block, static_cast<uint32_t>(cluster.pool.size())).isNewEntry)
                cluster.pool.append(block);
        }
    }
    layOutCluster(cluster, blockContents);
}

void ClassTablePacker::layOutCluster(const Cluster& cluster, const Vector<const Block*>& blockContents)
{
    while (m_bytes.size() % sizeof(uint64_t))
        m_bytes.append(0);
    for (size_t i : cluster.tables) {
        m_tables[i].offset = m_bytes.size();
        m_bytes.appendVector(m_tables[i].frame);
    }
    size_t poolAt = m_bytes.size();
    for (uint32_t block : cluster.pool)
        m_bytes.append(asByteSpan(*blockContents[block]));
    for (size_t i : cluster.tables) {
        auto& table = m_tables[i];
        for (auto& entry : table.entries) {
            size_t blockOffset = poolAt + cluster.poolSlots.get(entry.block) * ClassTableLayout::bytesInBlock - table.offset;
            storeInClassTable(m_bytes, table.offset + entry.offsetInFrame, encodeClassTableSlot(blockOffset, entry.page));
        }
    }
}

} } // namespace JSC::Yarr
