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
#include <compare>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/Hasher.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

// positionPlusOne is zero for a part of the shared constants, so among equal constants that part comes first.
struct HashedConstant {
    bool isShared() const { return !positionPlusOne; }

    uint64_t hash;
    uint32_t positionPlusOne;
    uint32_t index;
};

struct PoolInputs {
    std::span<const uint8_t> bytesOf(const HashedConstant& constant) const
    {
        if (constant.isShared())
            return shared.bytes.subspan(shared.parts[constant.index].offset, shared.parts[constant.index].size);
        return functions[constant.positionPlusOne - 1].body.constants[constant.index].bytes.span();
    }

    const LinkedFunctions& functions;
    const SharedConstants& shared;
};

} // anonymous namespace

// The upper half ranks the alignment, largest first: equal hashes have equal alignments, and the order by hash needs no padding.
static uint64_t hashOf(std::span<const uint8_t> bytes, uint32_t alignment)
{
    RELEASE_ASSERT(isPowerOfTwo(alignment) && alignment <= imageConstantsAlignment);
    uint64_t alignmentRank = getLSBSet(imageConstantsAlignment) - getLSBSet(alignment);
    return alignmentRank << 32 | computeHash(bytes);
}

static Vector<HashedConstant> collectConstants(const PoolInputs& inputs, ConstantPool& pool, const ParallelLoops& loops)
{
    const LinkedFunctions& functions = inputs.functions;
    pool.constantOffsetsByBody.grow(functions.size());
    pool.jumpTableOffsetsByBody.grow(functions.size());
    Vector<HashedConstant> constants = collectInParallel<HashedConstant>(loops, functions.size(), [&](unsigned position, Vector<HashedConstant>& constants) {
        const CodeBody& body = functions[position].body;
        pool.constantOffsetsByBody[position].grow(body.constants.size());
        pool.jumpTableOffsetsByBody[position].grow(body.jumpTables.size());
        for (uint32_t index = 0; index < body.constants.size(); ++index)
            constants.append({ hashOf(body.constants[index].bytes.span(), body.constants[index].alignment), position + 1, index });
    });
    for (uint32_t index = 0; index < inputs.shared.parts.size(); ++index) {
        ConstantRange part = inputs.shared.parts[index];
        RELEASE_ASSERT(part.size && !(part.offset % alignmentOfConstant(part.size)));
        constants.append({ hashOf(inputs.shared.bytes.subspan(part.offset, part.size), alignmentOfConstant(part.size)), 0, index });
    }
    return constants;
}

static void sortConstants(const PoolInputs& inputs, Vector<HashedConstant>& constants, const ParallelLoops& loops)
{
    parallelSort(loops, constants.mutableSpan(), [&](const HashedConstant& a, const HashedConstant& b) {
        if (a.hash != b.hash)
            return a.hash < b.hash;
        std::strong_ordering order = compareSpans(inputs.bytesOf(a), inputs.bytesOf(b));
        if (order != std::strong_ordering::equal)
            return order == std::strong_ordering::less;
        return std::tie(a.positionPlusOne, a.index) < std::tie(b.positionPlusOne, b.index);
    });
}

// Leaves one element in the list for each range of bytes that the pool stores behind the shared constants.
static uint64_t placeConstants(const PoolInputs& inputs, Vector<HashedConstant>& constants, ConstantPool& pool)
{
    auto offsetOf = [&](const HashedConstant& constant) -> uint32_t {
        if (constant.isShared())
            return inputs.shared.parts[constant.index].offset;
        return pool.constantOffsetsByBody[constant.positionPlusOne - 1][constant.index];
    };
    auto setOffsetOf = [&](const HashedConstant& constant, uint32_t offset) {
        pool.constantOffsetsByBody[constant.positionPlusOne - 1][constant.index] = offset;
    };
    uint64_t size = roundUpToMultipleOf<imageConstantsAlignment>(inputs.shared.bytes.size());
    size_t numberOfStoredConstants = 0;
    std::optional<HashedConstant> last;
    for (size_t index = 0; index < constants.size(); ++index) {
        HashedConstant constant = constants[index];
        std::span<const uint8_t> bytes = inputs.bytesOf(constant);
        if (constant.isShared()) {
            last = constant;
            continue;
        }
        if (last && last->hash == constant.hash && equalSpans(inputs.bytesOf(*last), bytes)) {
            setOffsetOf(constant, offsetOf(*last));
            continue;
        }
        size = roundUpToMultipleOf(inputs.functions[constant.positionPlusOne - 1].body.constants[constant.index].alignment, size);
        setOffsetOf(constant, static_cast<uint32_t>(size));
        size += bytes.size();
        last = constant;
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

ConstantPool buildConstantPool(const LinkedFunctions& functions, BuildContext& context, const SharedConstants& shared)
{
    const ParallelLoops& loops = context.parallelLoops();
    ConstantPool pool;
    PoolInputs inputs { functions, shared };
    Vector<HashedConstant> constants = collectConstants(inputs, pool, loops);
    sortConstants(inputs, constants, loops);
    uint64_t size = placeConstants(inputs, constants, pool);
    size = placeJumpTables(functions, size, pool);
    if (!checkSectionSize(size, context))
        return { };

    pool.bytes = Vector<uint8_t>(FillWith { }, size, 0);
    pool.sizeOfSharedConstants = static_cast<uint32_t>(shared.bytes.size());
    std::span<uint8_t> bytes = pool.bytes.mutableSpan();
    memcpySpan(bytes, shared.bytes);
    forEachIndexInParallel(loops, constants.size(), [&](unsigned index) {
        const HashedConstant& constant = constants[index];
        memcpySpan(bytes.subspan(pool.constantOffsetsByBody[constant.positionPlusOne - 1][constant.index]), inputs.bytesOf(constant));
    });
    context.statistics().count(StatisticID::ConstantBytesEmitted, size);
    return pool;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
