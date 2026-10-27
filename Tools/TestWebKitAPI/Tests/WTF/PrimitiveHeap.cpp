/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <cstring>
#include <wtf/FastMalloc.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace TestWebKitAPI {

static constexpr size_t sizes[] = { 1, 16, 48, 96, 208, 512, 1024, 4096, 16 * KB, 64 * KB, 100 * KB, MB, 5 * MB, 40 * MB };

static bool isPrimitive(const void* pointer)
{
    return Gigacage::isInDedicatedRange(Gigacage::Primitive, pointer);
}

// Whether Primitive allocations have ranges of their own in this build and process.
static bool hasDedicatedRange()
{
    void* block = Gigacage::malloc(Gigacage::Primitive, 1);
    bool result = isPrimitive(block);
    Gigacage::free(Gigacage::Primitive, block);
    return result;
}

TEST(WTF_PrimitiveHeap, EveryWayToAllocateUsesTheRange)
{
    bool expected = hasDedicatedRange();
#if USE(MIMALLOC) && CPU(ADDRESS64)
    EXPECT_TRUE(expected);
#endif
    for (size_t size : sizes) {
        void* blocks[] = {
            Gigacage::tryMalloc(Gigacage::Primitive, size),
            Gigacage::malloc(Gigacage::Primitive, size),
            Gigacage::tryZeroedMalloc(Gigacage::Primitive, size),
            Gigacage::zeroedMalloc(Gigacage::Primitive, size),
            Gigacage::tryAlignedMalloc(Gigacage::Primitive, 16 * KB, roundUpToMultipleOf<16 * KB>(size)),
            Gigacage::tryMallocArray(Gigacage::Primitive, size, 2),
            Gigacage::tryAllocateZeroedVirtualPages(Gigacage::Primitive, roundUpToMultipleOf(pageSize(), size)),
        };
        for (void* block : blocks) {
            ASSERT_TRUE(block);
            EXPECT_EQ(expected, isPrimitive(block));
            EXPECT_EQ(expected, isPrimitive(static_cast<uint8_t*>(block) + size - 1));
            memset(block, 0xab, size);
        }
        EXPECT_FALSE(std::bit_cast<uintptr_t>(blocks[4]) % (16 * KB));
        Gigacage::freeVirtualPages(Gigacage::Primitive, blocks[6], roundUpToMultipleOf(pageSize(), size));
        for (size_t index = 0; index < 6; ++index)
            Gigacage::free(Gigacage::Primitive, blocks[index]);
    }
}

TEST(WTF_PrimitiveHeap, NothingElseUsesTheRange)
{
    for (size_t size : sizes) {
        void* blocks[] = { fastMalloc(size), fastZeroedMalloc(size), fastCompactMalloc(size), fastAlignedMalloc(16 * KB, roundUpToMultipleOf<16 * KB>(size)) };
        for (void* block : blocks) {
            EXPECT_FALSE(isPrimitive(block));
            fastFree(block);
        }
    }
    int onStack = 0;
    EXPECT_FALSE(isPrimitive(&onStack));
    EXPECT_FALSE(isPrimitive(nullptr));
}

TEST(WTF_PrimitiveHeap, FreedBlocksStayOnTheirSide)
{
    if (!hasDedicatedRange())
        return;
    for (size_t size : { 48, 208, 1024, 4096 }) {
        void* freed = fastMalloc(size);
        fastFree(freed);
        Vector<void*> blocks;
        for (unsigned i = 0; i < 10000; ++i) {
            blocks.append(Gigacage::malloc(Gigacage::Primitive, size));
            EXPECT_NE(freed, blocks.last());
        }
        for (void* block : blocks)
            Gigacage::free(Gigacage::Primitive, block);

        freed = Gigacage::malloc(Gigacage::Primitive, size);
        Gigacage::free(Gigacage::Primitive, freed);
        blocks.shrink(0);
        for (unsigned i = 0; i < 10000; ++i) {
            blocks.append(fastMalloc(size));
            EXPECT_NE(freed, blocks.last());
        }
        for (void* block : blocks)
            fastFree(block);
    }
}

TEST(WTF_PrimitiveHeap, ZeroedMemoryIsZeroAfterReuse)
{
    for (size_t size : sizes) {
        for (unsigned round = 0; round < 3; ++round) {
            auto* block = static_cast<uint8_t*>(Gigacage::zeroedMalloc(Gigacage::Primitive, size));
            bool isZero = true;
            for (size_t i = 0; i < size; ++i)
                isZero &= !block[i];
            EXPECT_TRUE(isZero);
            memset(block, 0xff, size);
            Gigacage::free(Gigacage::Primitive, block);
        }
    }
}

TEST(WTF_PrimitiveHeap, ReallocKeepsContentAndRange)
{
    bool expected = hasDedicatedRange();
    auto* block = static_cast<uint8_t*>(Gigacage::malloc(Gigacage::Primitive, 1));
    block[0] = 7;
    size_t oldSize = 1;
    for (size_t size : { 3 * KB, 70 * KB, 3 * MB, 50 * MB, 2 * MB, 100 * KB, static_cast<size_t>(10) }) {
        block = static_cast<uint8_t*>(Gigacage::tryRealloc(Gigacage::Primitive, block, size));
        ASSERT_TRUE(block);
        EXPECT_EQ(expected, isPrimitive(block));
        bool isKept = true;
        for (size_t i = 0; i < std::min(oldSize, size); ++i)
            isKept &= block[i] == 7;
        EXPECT_TRUE(isKept);
        memset(block, 7, size);
        oldSize = size;
    }
    Gigacage::free(Gigacage::Primitive, block);
}

TEST(WTF_PrimitiveHeap, WhatARangeCannotHoldStillSucceeds)
{
    bool hasRange = hasDedicatedRange();
    size_t outsideBefore = Gigacage::allocationsOutsideDedicatedRange(Gigacage::Primitive);

    // Never touched, so they cost address space only.
    size_t tooLarge = 3 * GB;
    void* block = Gigacage::tryMalloc(Gigacage::Primitive, tooLarge);
    ASSERT_TRUE(block);
    EXPECT_FALSE(isPrimitive(block));
    Gigacage::free(Gigacage::Primitive, block);

    block = Gigacage::tryAlignedMalloc(Gigacage::Primitive, 4 * MB, 4 * MB);
    ASSERT_TRUE(block);
    EXPECT_FALSE(std::bit_cast<uintptr_t>(block) % (4 * MB));
    Gigacage::free(Gigacage::Primitive, block);

    EXPECT_EQ(hasRange ? 2u : 0u, Gigacage::allocationsOutsideDedicatedRange(Gigacage::Primitive) - outsideBefore);

    block = Gigacage::malloc(Gigacage::Primitive, 100);
    memset(block, 1, 100);
    block = Gigacage::tryRealloc(Gigacage::Primitive, block, tooLarge);
    ASSERT_TRUE(block);
    EXPECT_EQ(1, static_cast<uint8_t*>(block)[99]);
    block = Gigacage::tryRealloc(Gigacage::Primitive, block, 100);
    ASSERT_TRUE(block);
    EXPECT_EQ(1, static_cast<uint8_t*>(block)[99]);
    Gigacage::free(Gigacage::Primitive, block);
}

// A debug build of mimalloc fills what it hands out.
#ifdef NDEBUG
TEST(WTF_PrimitiveHeap, BlocksThatAnArenaCanHoldUseTheRange)
{
    bool expected = hasDedicatedRange();
    size_t outsideBefore = Gigacage::allocationsOutsideDedicatedRange(Gigacage::Primitive);
    // Never touched.
    for (size_t size : { 512 * MB, GB, GB + 1, GB + 512 * MB, 2 * GB - 64 * MB }) {
        void* block = Gigacage::tryMalloc(Gigacage::Primitive, size);
        ASSERT_TRUE(block);
        EXPECT_EQ(expected, isPrimitive(block));
        EXPECT_EQ(expected, isPrimitive(static_cast<uint8_t*>(block) + size - 1));
        Gigacage::free(Gigacage::Primitive, block);
    }
    EXPECT_EQ(outsideBefore, Gigacage::allocationsOutsideDedicatedRange(Gigacage::Primitive));
}

TEST(WTF_PrimitiveHeap, MoreThanOneRange)
{
    if (!hasDedicatedRange())
        return;
    // Never touched. 40 of them do not fit in one range of 16 GB.
    Vector<void*> blocks;
    for (unsigned i = 0; i < 40; ++i) {
        void* block = Gigacage::tryMalloc(Gigacage::Primitive, GB);
        ASSERT_TRUE(block);
        EXPECT_TRUE(isPrimitive(block));
        blocks.append(block);
    }
    void* small = Gigacage::malloc(Gigacage::Primitive, 64);
    EXPECT_TRUE(isPrimitive(small));
    Gigacage::free(Gigacage::Primitive, small);
    for (void* block : blocks)
        Gigacage::free(Gigacage::Primitive, block);
}

TEST(WTF_PrimitiveHeap, WhenAllRangesAreFull)
{
    if (!hasDedicatedRange())
        return;
    size_t outsideBefore = Gigacage::allocationsOutsideDedicatedRange(Gigacage::Primitive);
    // Never touched.
    Vector<void*> blocks;
    size_t inside = 0;
    while (blocks.size() < 100) {
        void* block = Gigacage::tryMalloc(Gigacage::Primitive, GB);
        ASSERT_TRUE(block);
        blocks.append(block);
        if (!isPrimitive(block))
            break;
        ++inside;
    }
    EXPECT_GT(inside, 40u);
    EXPECT_LT(inside, 64u);
    EXPECT_EQ(1u, Gigacage::allocationsOutsideDedicatedRange(Gigacage::Primitive) - outsideBefore);

    for (void* block : blocks)
        Gigacage::free(Gigacage::Primitive, block);
    void* block = Gigacage::tryMalloc(Gigacage::Primitive, GB);
    EXPECT_TRUE(isPrimitive(block));
    Gigacage::free(Gigacage::Primitive, block);
}
#endif

TEST(WTF_PrimitiveHeap, ThreadsThatEnd)
{
    bool expected = hasDedicatedRange();
    for (unsigned round = 0; round < 8; ++round) {
        Vector<void*> blocks;
        Thread::create("PrimitiveHeap test"_s, [&] {
            for (size_t size : sizes) {
                void* block = Gigacage::malloc(Gigacage::Primitive, size);
                EXPECT_EQ(expected, isPrimitive(block));
                memset(block, round, size);
                blocks.append(block);
            }
        })->waitForCompletion();
        for (size_t index = 0; index < blocks.size(); ++index) {
            EXPECT_EQ(round, static_cast<uint8_t*>(blocks[index])[sizes[index] - 1]);
            Gigacage::free(Gigacage::Primitive, blocks[index]);
        }
        WTF::releaseFastMallocFreeMemory();
    }
}

} // namespace TestWebKitAPI

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
