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

#include "YarrPattern.h"
#include <array>
#include <expected>
#include <map>
#include <wtf/Hasher.h>
#include <wtf/Vector.h>

namespace JSC { namespace Yarr {

struct ClassTableLayout {
    static constexpr unsigned wordsInBlock = 8;
    static constexpr unsigned bytesInBlock = wordsInBlock * sizeof(uint32_t);
    static constexpr unsigned firstSurrogatePage = 0xd800 / (wordsInBlock * 32);
    static constexpr unsigned limitOffset = firstSurrogatePage * sizeof(int16_t);
    static constexpr unsigned trailingRangesOffset = limitOffset + sizeof(uint32_t);
    static constexpr unsigned firstBlockOffset = trailingRangesOffset + sizeof(uint32_t);
    static constexpr unsigned upperIndexOffset = firstBlockOffset + bytesInBlock;
    static constexpr char32_t charactersInBlock = wordsInBlock * 32;
};

enum class ClassTableForm : uint8_t { IndexThenBlocks, ForRoutine };

struct ClassTable {
    using Block = std::array<uint32_t, ClassTableLayout::wordsInBlock>;
    static constexpr size_t maxTrailingRanges = 5;

    ClassTableForm form { ClassTableForm::IndexThenBlocks };
    Vector<uint16_t> pageBlockIndexes;
    Vector<Block> blocks;
    Vector<CharacterRange, maxTrailingRanges> trailingRanges;

    char32_t limit() const { return pageBlockIndexes.size() * ClassTableLayout::charactersInBlock; }

    friend bool operator==(const ClassTable&, const ClassTable&) = default;
};

void add(Hasher&, const ClassTable&);

inline constexpr size_t maxWideEntriesWithoutClassTable = 7;

enum class ClassTableDecline : uint8_t { OnlyTrailingRanges, TooManyPages, TooLarge };

std::expected<ClassTable, ClassTableDecline> buildClassTable(const CharacterClass&, ClassTableForm, char32_t greatestCharacter);

class ClassTablePacker {
public:
    static constexpr size_t defaultMaxClusterSize = 96 * KB;
    static constexpr size_t maxPages = 4096;

    explicit ClassTablePacker(size_t maxClusterSize = defaultMaxClusterSize);

    size_t add(const ClassTable&);
    void pack();

    const Vector<uint8_t>& bytes() const { return m_bytes; }
    uint32_t offsetOf(size_t table) const { return m_tables[table].offset; }

private:
    using Block = ClassTable::Block;

    struct Entry {
        uint32_t offsetInFrame;
        uint32_t page;
        uint32_t block;
    };
    struct Table {
        Vector<uint8_t> frame;
        Vector<Entry> entries;
        Vector<uint32_t> pooledBlocks;
        uint32_t offset { 0 };
    };
    struct Cluster;

    uint32_t numberOf(const Block&);
    Vector<uint8_t> frameOf(const ClassTable&, Vector<Entry>&);
    void layOutCluster(const Cluster&, const Vector<const Block*>& blockContents);

    size_t m_maxClusterSize;
    std::map<Block, uint32_t> m_blockNumbers;
    Vector<Table> m_tables;
    Vector<uint8_t> m_bytes;
};

} } // namespace JSC::Yarr
