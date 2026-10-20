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
#include <compare>
#include <span>
#include <tuple>
#include <wtf/Hasher.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

struct HashedConstant {
    uint64_t hash;
    uint32_t position;
    uint32_t index;
};

} // anonymous namespace

// The upper half ranks the alignment, largest first: equal hashes have equal alignments, and the order by hash needs no padding.
static uint64_t hashOf(const ConstantData& data)
{
    RELEASE_ASSERT(isPowerOfTwo(data.alignment) && data.alignment <= imageConstantsAlignment);
    uint64_t alignmentRank = getLSBSet(imageConstantsAlignment) - getLSBSet(data.alignment);
    return alignmentRank << 32 | computeHash(data.bytes);
}

static const ConstantData& dataOf(const LinkedFunctions& functions, const HashedConstant& constant)
{
    return functions[constant.position].body.constants[constant.index];
}

static Vector<HashedConstant> collectConstants(const LinkedFunctions& functions, ConstantPool& pool, const ParallelLoops& loops)
{
    pool.constantOffsetsByBody.grow(functions.size());
    pool.jumpTableOffsetsByBody.grow(functions.size());
    return collectInParallel<HashedConstant>(loops, functions.size(), [&](unsigned position, Vector<HashedConstant>& constants) {
        const CodeBody& body = functions[position].body;
        pool.constantOffsetsByBody[position].grow(body.constants.size());
        pool.jumpTableOffsetsByBody[position].grow(body.jumpTables.size());
        for (uint32_t index = 0; index < body.constants.size(); ++index)
            constants.append({ hashOf(body.constants[index]), position, index });
    });
}

static void sortConstants(const LinkedFunctions& functions, Vector<HashedConstant>& constants, const ParallelLoops& loops)
{
    parallelSort(loops, constants.mutableSpan(), [&](const HashedConstant& a, const HashedConstant& b) {
        if (a.hash != b.hash)
            return a.hash < b.hash;
        std::strong_ordering order = compareSpans(dataOf(functions, a).bytes.span(), dataOf(functions, b).bytes.span());
        if (order != std::strong_ordering::equal)
            return order == std::strong_ordering::less;
        return std::tie(a.position, a.index) < std::tie(b.position, b.index);
    });
}

// Leaves one element in the list for each range of bytes that the pool stores.
static uint64_t placeConstants(const LinkedFunctions& functions, Vector<HashedConstant>& constants, ConstantPool& pool)
{
    auto offsetOf = [&](const HashedConstant& constant) -> uint32_t& {
        return pool.constantOffsetsByBody[constant.position][constant.index];
    };
    uint64_t size = 0;
    size_t numberOfStoredConstants = 0;
    for (size_t index = 0; index < constants.size(); ++index) {
        HashedConstant constant = constants[index];
        const ConstantData& data = dataOf(functions, constant);
        if (numberOfStoredConstants) {
            const HashedConstant& stored = constants[numberOfStoredConstants - 1];
            if (stored.hash == constant.hash && equalSpans(dataOf(functions, stored).bytes.span(), data.bytes.span())) {
                offsetOf(constant) = offsetOf(stored);
                continue;
            }
        }
        size = roundUpToMultipleOf(data.alignment, size);
        offsetOf(constant) = static_cast<uint32_t>(size);
        size += data.bytes.size();
        constants[numberOfStoredConstants++] = constant;
    }
    constants.shrink(numberOfStoredConstants);
    return size;
}

static uint64_t placeJumpTables(const LinkedFunctions& functions, uint64_t size, ConstantPool& pool)
{
    for (size_t position = 0; position < functions.size(); ++position) {
        Vector<uint32_t, 0>& offsets = pool.jumpTableOffsetsByBody[position];
        for (size_t index = 0; index < offsets.size(); ++index) {
            size = roundUpToMultipleOf<sizeof(int32_t)>(size);
            offsets[index] = static_cast<uint32_t>(size);
            size += sizeof(int32_t) * functions[position].body.jumpTables[index].targets.size();
        }
    }
    return size;
}

ConstantPool buildConstantPool(const LinkedFunctions& functions, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    ConstantPool pool;
    Vector<HashedConstant> constants = collectConstants(functions, pool, loops);
    sortConstants(functions, constants, loops);
    uint64_t size = placeConstants(functions, constants, pool);
    size = placeJumpTables(functions, size, pool);
    if (!checkSectionSize(size, context))
        return { };

    pool.bytes = Vector<uint8_t>(FillWith { }, size, 0);
    std::span<uint8_t> bytes = pool.bytes.mutableSpan();
    forEachIndexInParallel(loops, constants.size(), [&](unsigned index) {
        const HashedConstant& constant = constants[index];
        memcpySpan(bytes.subspan(pool.constantOffsetsByBody[constant.position][constant.index]), dataOf(functions, constant).bytes.span());
    });
    context.statistics().count(StatisticID::ConstantBytesEmitted, size);
    return pool;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
