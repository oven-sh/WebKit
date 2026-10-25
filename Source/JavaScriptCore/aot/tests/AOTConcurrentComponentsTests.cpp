/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTConcurrentComponents.h"
#include "AOTNativeTest.h"
#include "AOTThreads.h"
#include <wtf/WeakRandom.h>

namespace JSC { namespace AOT { namespace Typed {

static void checkEveryParentIsNoHigher(const ConcurrentComponents& components, uint32_t numberOfMembers)
{
    for (uint32_t member = 0; member < numberOfMembers; ++member)
        AOT_TEST_CHECK(components.parentOf(member) <= member);
}

static void concurrentComponentsJoinUnderTheLowestMember()
{
    ConcurrentComponents components(8);
    for (uint32_t member = 0; member < 8; ++member)
        AOT_TEST_CHECK(components.componentOf(member) == member);
    components.join(6, 3);
    components.join(7, 6);
    components.join(1, 5);
    AOT_TEST_CHECK(components.componentOf(3) == 3 && components.componentOf(6) == 3 && components.componentOf(7) == 3);
    AOT_TEST_CHECK(components.componentOf(1) == 1 && components.componentOf(5) == 1);
    AOT_TEST_CHECK(components.componentOf(0) == 0 && components.componentOf(2) == 2 && components.componentOf(4) == 4);
    components.join(5, 7);
    for (uint32_t member : { 1u, 3u, 5u, 6u, 7u })
        AOT_TEST_CHECK(components.componentOf(member) == 1);
    checkEveryParentIsNoHigher(components, 8);
}

static void concurrentComponentsLeaveALowerRootAloneWhenAPathIsShortenedWithAnOlderRoot()
{
    ConcurrentComponents components(8);
    components.join(7, 5);
    uint32_t rootReadEarlier = components.rootAbove(7);
    AOT_TEST_CHECK(rootReadEarlier == 5);

    components.join(5, 2);
    AOT_TEST_CHECK(components.componentOf(7) == 2 && components.parentOf(7) == 2 && components.parentOf(5) == 2);

    components.shortenPath(7, rootReadEarlier);
    AOT_TEST_CHECK(components.parentOf(2) == 2);
    checkEveryParentIsNoHigher(components, 8);
    for (uint32_t member : { 2u, 5u, 7u })
        AOT_TEST_CHECK(components.componentOf(member) == 2);
}

static void concurrentComponentsStayWithoutCyclesUnderManyThreads()
{
    constexpr uint32_t numberOfMembers = 64;
    constexpr unsigned numberOfWorkers = 16;
    constexpr unsigned numberOfRounds = 400;
    constexpr unsigned joinsOfWorker = 256;
    ParallelLoops loops(numberOfWorkers);
    for (unsigned round = 0; round < numberOfRounds; ++round) {
        ConcurrentComponents components(numberOfMembers);
        forEachIndexInParallel(loops, numberOfWorkers, [&](unsigned worker) {
            WeakRandom random(round * numberOfWorkers + worker + 1);
            for (unsigned join = 0; join < joinsOfWorker; ++join) {
                components.join(random.getUint32(numberOfMembers), random.getUint32(numberOfMembers));
                AOT_TEST_CHECK(components.parentOf(random.getUint32(numberOfMembers)) < numberOfMembers);
            }
        });
        checkEveryParentIsNoHigher(components, numberOfMembers);
    }
}

#define FOR_EACH_AOT_CONCURRENT_COMPONENTS_TEST(v) \
    v(concurrentComponentsJoinUnderTheLowestMember) v(concurrentComponentsLeaveALowerRootAloneWhenAPathIsShortenedWithAnOlderRoot) \
    v(concurrentComponentsStayWithoutCyclesUnderManyThreads)

std::span<const NativeTest> concurrentComponentsTests()
{
#define AOT_CONCURRENT_COMPONENTS_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_CONCURRENT_COMPONENTS_TEST(AOT_CONCURRENT_COMPONENTS_TEST_ENTRY) };
#undef AOT_CONCURRENT_COMPONENTS_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
