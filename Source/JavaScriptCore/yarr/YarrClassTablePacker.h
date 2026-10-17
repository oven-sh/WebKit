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

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Yarr {

struct ClassTableLayout {
    static constexpr unsigned wordsInBlock = 8;
    static constexpr unsigned bytesInBlock = wordsInBlock * sizeof(uint32_t);
    static constexpr unsigned firstSurrogatePage = 0xd800 / (wordsInBlock * 32);
    static constexpr unsigned limitOffset = firstSurrogatePage * sizeof(int16_t);
    static constexpr unsigned trailingRangesOffset = limitOffset + sizeof(uint32_t);
    static constexpr unsigned firstBlockOffset = trailingRangesOffset + sizeof(uint32_t);
    static constexpr unsigned upperIndexOffset = firstBlockOffset + bytesInBlock;
};

class ClassTablePacker {
public:
    enum class Layout : uint8_t { IndexThenBlocks, ForRoutine };
    static constexpr size_t notFound = static_cast<size_t>(-1);

    size_t add(std::span<const uint8_t> bytes, Layout layout)
    {
        Table table;
        if (!(layout == Layout::IndexThenBlocks ? parseIndexThenBlocks(bytes, table) : parseForRoutine(bytes, table)))
            return notFound;
        if (table.frame.size() + table.entries.size() * ClassTableLayout::bytesInBlock > maxClusterSize)
            return notFound;
        m_tables.append(WTF::move(table));
        return m_tables.size() - 1;
    }

    void pack()
    {
        std::map<Block, uint32_t> blockNumber;
        Vector<Vector<uint32_t>> tableBlocks;
        for (auto& table : m_tables) {
            Vector<uint32_t> blocks;
            for (auto& entry : table.entries) {
                entry.block = blockNumber.emplace(entry.contents, static_cast<uint32_t>(blockNumber.size())).first->second;
                blocks.append(entry.block);
            }
            std::sort(blocks.begin(), blocks.end());
            blocks.shrink(std::unique(blocks.begin(), blocks.end()) - blocks.begin());
            tableBlocks.append(WTF::move(blocks));
        }
        Vector<const Block*> blockContents;
        blockContents.fill(nullptr, blockNumber.size());
        for (auto& [contents, number] : blockNumber)
            blockContents[number] = &contents;

        constexpr uint32_t notInPool = std::numeric_limits<uint32_t>::max();
        Vector<bool> hasPlacement;
        hasPlacement.fill(false, m_tables.size());
        for (size_t placed = 0; placed < m_tables.size();) {
            Vector<size_t> cluster;
            Vector<uint32_t> poolOffsets;
            poolOffsets.fill(notInPool, blockNumber.size());
            Vector<uint32_t> pool;
            size_t sizeOfFrames = 0;
            while (true) {
                size_t best = notFound;
                size_t bestNewBlockCount = 0;
                for (size_t i = 0; i < m_tables.size(); ++i) {
                    if (hasPlacement[i])
                        continue;
                    size_t newBlocks = 0;
                    for (uint32_t block : tableBlocks[i])
                        newBlocks += poolOffsets[block] == notInPool;
                    if (!cluster.isEmpty() && sizeOfFrames + m_tables[i].frame.size() + (pool.size() + newBlocks) * ClassTableLayout::bytesInBlock > maxClusterSize)
                        continue;
                    bool isBetter = cluster.isEmpty()
                        ? best == notFound || tableBlocks[i].size() > tableBlocks[best].size()
                        : best == notFound || newBlocks * tableBlocks[best].size() < bestNewBlockCount * tableBlocks[i].size();
                    if (isBetter) {
                        best = i;
                        bestNewBlockCount = newBlocks;
                    }
                }
                if (best == notFound)
                    break;
                hasPlacement[best] = true;
                ++placed;
                cluster.append(best);
                sizeOfFrames += m_tables[best].frame.size();
                for (uint32_t block : tableBlocks[best]) {
                    if (poolOffsets[block] == notInPool) {
                        poolOffsets[block] = pool.size();
                        pool.append(block);
                    }
                }
            }
            layOutCluster(cluster, pool, poolOffsets, blockContents);
        }
    }

    const Vector<uint8_t>& bytes() const { return m_bytes; }
    uint32_t offsetOf(size_t table) const { return m_tables[table].offset; }

private:
    using Block = std::array<uint32_t, ClassTableLayout::wordsInBlock>;
    static constexpr size_t maxClusterSize = 96 * 1024;
    static constexpr size_t maxPages = 4096;

    struct Entry {
        uint32_t offsetInFrame;
        uint32_t page;
        Block contents;
        uint32_t block { 0 };
    };
    struct Table {
        Vector<uint8_t> frame;
        Vector<Entry> entries;
        uint32_t offset { 0 };
    };

    template<typename T>
    static T read(std::span<const uint8_t> bytes, size_t offset)
    {
        T value;
        memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }

    static bool readBlock(std::span<const uint8_t> bytes, int64_t word, size_t firstByte, size_t endByte, Block& block)
    {
        int64_t offset = word * static_cast<int64_t>(sizeof(uint32_t));
        if (offset < static_cast<int64_t>(firstByte) || offset + ClassTableLayout::bytesInBlock > endByte || (offset - firstByte) % ClassTableLayout::bytesInBlock)
            return false;
        memcpy(block.data(), bytes.data() + offset, ClassTableLayout::bytesInBlock);
        return true;
    }

    static bool parseIndexThenBlocks(std::span<const uint8_t> bytes, Table& table)
    {
        if (bytes.size() < sizeof(int16_t) || bytes.size() % sizeof(uint32_t))
            return false;
        int16_t indexWordCount = read<int16_t>(bytes, 0);
        size_t pages = static_cast<size_t>(indexWordCount) * 2;
        size_t sizeOfIndex = pages * sizeof(int16_t);
        if (indexWordCount <= 0 || pages > maxPages || sizeOfIndex >= bytes.size())
            return false;
        for (size_t page = 0; page < pages; ++page) {
            int16_t value = read<int16_t>(bytes, page * sizeof(int16_t));
            Entry entry { static_cast<uint32_t>(page * sizeof(int16_t)), static_cast<uint32_t>(page), { } };
            if (!readBlock(bytes, static_cast<int64_t>(value) + page * ClassTableLayout::wordsInBlock, sizeOfIndex, bytes.size(), entry.contents)) {
                bool isPadding = !value && page + 1 == pages;
                if (isPadding)
                    continue;
                return false;
            }
            table.entries.append(entry);
        }
        table.frame.append(bytes.first(sizeOfIndex));
        return true;
    }

    static bool parseForRoutine(std::span<const uint8_t> bytes, Table& table)
    {
        if (bytes.size() < ClassTableLayout::upperIndexOffset || bytes.size() % sizeof(uint32_t))
            return false;
        size_t limit = read<uint32_t>(bytes, ClassTableLayout::limitOffset);
        size_t trailingRanges = read<uint32_t>(bytes, ClassTableLayout::trailingRangesOffset);
        size_t pages = limit + limit % 2;
        if (limit < ClassTableLayout::firstSurrogatePage || pages > maxPages)
            return false;
        size_t endOfIndex = ClassTableLayout::upperIndexOffset + (pages - ClassTableLayout::firstSurrogatePage) * sizeof(int16_t);
        size_t endOfBlocks = trailingRanges ? trailingRanges : bytes.size();
        if (endOfIndex > endOfBlocks || endOfBlocks > bytes.size())
            return false;
        for (size_t page = 0; page < pages; ++page) {
            size_t offsetInFrame = page < ClassTableLayout::firstSurrogatePage ? page * sizeof(int16_t) : ClassTableLayout::upperIndexOffset + (page - ClassTableLayout::firstSurrogatePage) * sizeof(int16_t);
            int64_t word = static_cast<int64_t>(read<int16_t>(bytes, offsetInFrame)) + page * ClassTableLayout::wordsInBlock;
            if (word * sizeof(uint32_t) == ClassTableLayout::firstBlockOffset)
                continue;
            Entry entry { static_cast<uint32_t>(offsetInFrame), static_cast<uint32_t>(page), { } };
            if (!readBlock(bytes, word, endOfIndex, endOfBlocks, entry.contents))
                return false;
            table.entries.append(entry);
        }
        table.frame.append(bytes.first(endOfIndex));
        if (trailingRanges) {
            uint32_t movedTo = endOfIndex;
            memcpy(table.frame.mutableSpan().data() + ClassTableLayout::trailingRangesOffset, &movedTo, sizeof(movedTo));
            table.frame.append(bytes.subspan(trailingRanges));
        }
        return true;
    }

    void layOutCluster(const Vector<size_t>& cluster, const Vector<uint32_t>& pool, const Vector<uint32_t>& poolOffsets, const Vector<const Block*>& blockContents)
    {
        while (m_bytes.size() % sizeof(uint64_t))
            m_bytes.append(0);
        for (size_t i : cluster) {
            m_tables[i].offset = m_bytes.size();
            m_bytes.appendVector(m_tables[i].frame);
        }
        size_t poolAt = m_bytes.size();
        m_bytes.grow(poolAt + pool.size() * ClassTableLayout::bytesInBlock);
        for (size_t place = 0; place < pool.size(); ++place)
            memcpy(m_bytes.mutableSpan().data() + poolAt + place * ClassTableLayout::bytesInBlock, blockContents[pool[place]]->data(), ClassTableLayout::bytesInBlock);
        for (size_t i : cluster) {
            auto& table = m_tables[i];
            for (auto& entry : table.entries) {
                int64_t word = static_cast<int64_t>(poolAt + poolOffsets[entry.block] * ClassTableLayout::bytesInBlock - table.offset) / static_cast<int64_t>(sizeof(uint32_t));
                int64_t value = word - static_cast<int64_t>(entry.page) * ClassTableLayout::wordsInBlock;
                RELEASE_ASSERT(value == static_cast<int16_t>(value));
                int16_t encoded = static_cast<int16_t>(value);
                memcpy(m_bytes.mutableSpan().data() + table.offset + entry.offsetInFrame, &encoded, sizeof(encoded));
            }
        }
    }

    Vector<Table> m_tables;
    Vector<uint8_t> m_bytes;
};

} } // namespace JSC::Yarr

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
