/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConstantPool.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSectionWriter.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <array>
#include <compare>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

struct PooledConstant {
    bool isShared() const { return !positionPlusOne; }
    size_t alignment() const { return imageConstantsAlignment >> (alignmentRankAndSize >> 32); }
    uint32_t size() const { return static_cast<uint32_t>(alignmentRankAndSize); }

    // The upper half ranks the alignment, largest first: in this order a constant whose size is a multiple of its alignment needs no padding.
    uint64_t alignmentRankAndSize;
    uint64_t firstEightBytes;
    uint32_t positionPlusOne;
    uint32_t index;
};

} // anonymous namespace

static PooledConstant createPooledConstant(std::span<const uint8_t> bytes, uint32_t alignment, uint32_t positionPlusOne, uint32_t index)
{
    RELEASE_ASSERT(isPowerOfTwo(alignment) && alignment <= imageConstantsAlignment);
    uint64_t alignmentRank = getLSBSet(imageConstantsAlignment) - getLSBSet(alignment);
    uint64_t firstEightBytes = 0;
    for (size_t at = 0; at < sizeof(firstEightBytes); ++at)
        firstEightBytes = firstEightBytes << 8 | (at < bytes.size() ? bytes[at] : 0);
    return { alignmentRank << 32 | safeCast<uint32_t>(bytes.size()), firstEightBytes, positionPlusOne, index };
}

static StatisticID byteStatisticFor(ConstantKind kind)
{
    switch (kind) {
    case ConstantKind::Unlabeled:
        return StatisticID::UnlabeledConstantBytes;
    case ConstantKind::Number:
        return StatisticID::ConstantBytesOfNumbers;
    case ConstantKind::ShapeSet:
        return StatisticID::ConstantBytesOfShapeSets;
    case ConstantKind::ShapeBitmap:
        return StatisticID::ConstantBytesOfShapeBitmaps;
    case ConstantKind::OffsetTable:
        return StatisticID::ConstantBytesOfOffsetTables;
    case ConstantKind::CellTemplate:
        return StatisticID::ConstantBytesOfCellTemplates;
    case ConstantKind::ByteLiteral:
        return StatisticID::ConstantBytesOfByteLiterals;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ConstantPool buildConstantPool(const LinkedFunctions& functions, BuildContext& context, const SharedConstants& shared)
{
    const ParallelLoops& loops = context.parallelLoops();
    Statistics& statistics = context.statistics();
    ConstantPool pool;
    auto bytesOfConstant = [&](const PooledConstant& constant) -> std::span<const uint8_t> {
        if (constant.isShared())
            return shared.bytes.subspan(shared.parts[constant.index].offset, shared.parts[constant.index].size);
        return functions[constant.positionPlusOne - 1].body.rareLists->constants[constant.index].bytes.span();
    };
    auto offsetOf = [&](const PooledConstant& constant) -> uint32_t& {
        return pool.constantOffsetsByBody[constant.positionPlusOne - 1][constant.index];
    };
    auto compareContents = [&](const PooledConstant& a, const PooledConstant& b) -> std::strong_ordering {
        if (a.alignmentRankAndSize != b.alignmentRankAndSize)
            return a.alignmentRankAndSize <=> b.alignmentRankAndSize;
        if (a.firstEightBytes != b.firstEightBytes || a.size() <= sizeof(a.firstEightBytes))
            return a.firstEightBytes <=> b.firstEightBytes;
        return compareSpans(bytesOfConstant(a), bytesOfConstant(b));
    };

    pool.constantOffsetsByBody.grow(functions.size());
    pool.jumpTableOffsetsByBody.grow(functions.size());
    Vector<PooledConstant> constants = collectInParallel<PooledConstant>(loops, functions.size(), [&](unsigned position, Vector<PooledConstant>& constantsOfChunk) {
        const RareListsOfBody& lists = *functions[position].body.rareLists;
        pool.constantOffsetsByBody[position].grow(lists.constants.size());
        pool.jumpTableOffsetsByBody[position].grow(lists.jumpTables.size());
        for (uint32_t index = 0; index < lists.constants.size(); ++index)
            constantsOfChunk.append(createPooledConstant(lists.constants[index].bytes.span(), lists.constants[index].alignment, position + 1, index));
    });
    for (uint32_t index = 0; index < shared.parts.size(); ++index) {
        ConstantRange part = shared.parts[index];
        RELEASE_ASSERT(part.size && !(part.offset % alignmentOfConstant(part.size)));
        constants.append(createPooledConstant(shared.bytes.subspan(part.offset, part.size), alignmentOfConstant(part.size), 0, index));
    }
    parallelSort(loops, constants.mutableSpan(), [&](const PooledConstant& a, const PooledConstant& b) {
        std::strong_ordering order = compareContents(a, b);
        if (order != std::strong_ordering::equal)
            return order == std::strong_ordering::less;
        return std::tie(a.positionPlusOne, a.index) < std::tie(b.positionPlusOne, b.index);
    });

    Vector<uint8_t> hasBytesOfConstantInFront;
    hasBytesOfConstantInFront.grow(constants.size());
    forEachRangeOfIndicesInParallel(loops, constants.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            hasBytesOfConstantInFront[index] = index && compareContents(constants[index - 1], constants[index]) == std::strong_ordering::equal;
    });
    uint64_t size = roundUpToMultipleOf<imageConstantsAlignment>(shared.bytes.size());
    uint64_t padding = size - shared.bytes.size();
    Vector<uint32_t> offsetsOfConstants;
    offsetsOfConstants.grow(constants.size());
    uint32_t offsetOfEqualConstants = 0;
    for (size_t index = 0; index < constants.size(); ++index) {
        const PooledConstant& constant = constants[index];
        if (constant.isShared())
            offsetOfEqualConstants = shared.parts[constant.index].offset;
        else if (!hasBytesOfConstantInFront[index]) {
            uint64_t start = roundUpToMultipleOf(constant.alignment(), size);
            padding += start - size;
            offsetOfEqualConstants = static_cast<uint32_t>(start);
            size = start + constant.size();
        }
        offsetsOfConstants[index] = offsetOfEqualConstants;
    }
    forEachRangeOfIndicesInParallel(loops, constants.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            if (!constants[index].isShared())
                offsetOf(constants[index]) = offsetsOfConstants[index];
        }
    });

    uint64_t sizeOfJumpTables = 0;
    for (size_t position = 0; position < functions.size(); ++position) {
        Vector<uint32_t, 0>& offsets = pool.jumpTableOffsetsByBody[position];
        for (size_t index = 0; index < offsets.size(); ++index) {
            uint64_t start = roundUpToMultipleOf<sizeof(int32_t)>(size);
            uint64_t sizeOfTable = sizeof(int32_t) * functions[position].body.rareLists->jumpTables[index].targets.size();
            padding += start - size;
            offsets[index] = static_cast<uint32_t>(start);
            sizeOfJumpTables += sizeOfTable;
            size = start + sizeOfTable;
        }
    }
    if (!checkSectionSize(size, context))
        return { };

    pool.bytes = Vector<uint8_t>(FillWith { }, size, 0);
    pool.sizeOfSharedConstants = static_cast<uint32_t>(shared.bytes.size());
    std::span<uint8_t> bytes = pool.bytes.mutableSpan();
    memcpySpan(bytes, shared.bytes);
    forEachRangeOfIndicesInParallel(loops, constants.size(), [&](size_t first, size_t end) {
        std::array<uint64_t, numberOfConstantKinds> bytesByKind { };
        for (size_t index = first; index < end; ++index) {
            const PooledConstant& constant = constants[index];
            if (constant.isShared() || hasBytesOfConstantInFront[index])
                continue;
            const ConstantData& data = functions[constant.positionPlusOne - 1].body.rareLists->constants[constant.index];
            memcpySpan(bytes.subspan(offsetsOfConstants[index]), data.bytes.span());
            bytesByKind[static_cast<unsigned>(data.kind)] += data.bytes.size();
        }
        for (unsigned kind = 0; kind < numberOfConstantKinds; ++kind) {
            if (bytesByKind[kind])
                statistics.count(byteStatisticFor(static_cast<ConstantKind>(kind)), bytesByKind[kind]);
        }
    });
    statistics.count(StatisticID::ConstantBytesEmitted, size);
    statistics.count(StatisticID::ConstantBytesOfSharedConstants, shared.bytes.size());
    statistics.count(StatisticID::ConstantBytesOfJumpTables, sizeOfJumpTables);
    statistics.count(StatisticID::PaddingBytesBetweenConstants, padding);
    return pool;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
