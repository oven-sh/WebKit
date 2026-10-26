/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "B3HeapSet.h"
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <span>
#include <type_traits>
#include <wtf/RangeSet.h>
#include <wtf/WeakRandom.h>

static HeapSet createSet(HeapSetTable& table, std::initializer_list<HeapRange> ranges)
{
    return table.create(std::span<const HeapRange>(ranges));
}

static void checkRanges(const HeapSet& set, std::initializer_list<HeapRange> expected)
{
    if (std::ranges::equal(set.ranges(), expected))
        return;
    crashLock.lock();
    dataLogLn("Expected the ranges ", listDump(expected), " but found ", set);
    CRASH();
}

static void checkIsNormalized(const HeapSet& set)
{
    std::span<const HeapRange> ranges = set.ranges();
    CHECK_EQ(set.isSingleRange(), ranges.size() <= 1);
    if (ranges.empty()) {
        CHECK_EQ(set.bounds(), HeapRange());
        return;
    }
    CHECK_EQ(set.bounds(), HeapRange(ranges.front().begin(), ranges.back().end()));
    for (size_t i = 0; i < ranges.size(); ++i) {
        CHECK(ranges[i]);
        if (i)
            CHECK(ranges[i - 1].end() < ranges[i].begin());
    }
}

static void testHeapSetOfOneRange()
{
    static_assert(sizeof(HeapSet) == sizeof(HeapRange) + sizeof(void*));
    static_assert(std::is_trivially_copyable_v<HeapSet>);
    static_assert(!HeapSet());
    static_assert(HeapSet(firstHeap));

    HeapSet empty;
    CHECK(!empty);
    CHECK(empty.isSingleRange());
    CHECK(empty.ranges().empty());
    CHECK_EQ(empty.bounds(), HeapRange());
    CHECK_EQ(empty, HeapRange());
    CHECK_EQ(empty, HeapSet(HeapRange()));
    CHECK(!empty.overlaps(HeapRange::top()));
    CHECK(!empty.overlaps(HeapSet::top()));
    CHECK(!empty.overlaps(empty));
    CHECK(!HeapSet::top().overlaps(empty));

    HeapSet set(firstHeap);
    CHECK(set);
    CHECK(set.isSingleRange());
    checkRanges(set, { firstHeap });
    CHECK_EQ(set.bounds(), firstHeap);
    CHECK_EQ(set, firstHeap);
    CHECK(set != secondHeap);
    CHECK(set != bothHeaps);
    CHECK(set.overlaps(firstHeap));
    CHECK(set.overlaps(bothHeaps));
    CHECK(!set.overlaps(secondHeap));
    CHECK(!set.overlaps(HeapRange()));

    constexpr uint64_t lastHeap = std::numeric_limits<uint64_t>::max() - 1;
    CHECK_EQ(HeapSet::top(), HeapRange::top());
    for (HeapRange range : { HeapRange(0), firstHeap, allHeaps, HeapRange(lastHeap), HeapRange::top() }) {
        CHECK(HeapSet::top().overlaps(range));
        CHECK(HeapSet::top().overlaps(HeapSet(range)));
        CHECK(HeapSet(range).overlaps(HeapSet::top()));
    }
    CHECK(!HeapSet::top().overlaps(HeapRange()));
}

static void testHeapSetTableNormalizes()
{
    HeapSetTable table;
    HeapSet set = createSet(table, { { 5, 6 }, { 1, 2 }, { 2, 3 }, { 5, 7 }, { 9, 9 } });
    checkRanges(set, { { 1, 3 }, { 5, 7 } });
    CHECK_EQ(set.bounds(), HeapRange(1, 7));
    CHECK(!set.isSingleRange());
    CHECK_EQ(table.numberOfLists(), 1u);

    HeapSet withContainedRange = createSet(table, { { 11, 12 }, { 3, 4 }, { 1, 9 } });
    checkRanges(withContainedRange, { { 1, 9 }, { 11, 12 } });
    CHECK_EQ(table.numberOfLists(), 2u);

    HeapSet merged = createSet(table, { { 4, 6 }, { 1, 2 }, { 2, 4 }, { 3, 5 } });
    CHECK(merged.isSingleRange());
    CHECK_EQ(merged, HeapRange(1, 6));
    CHECK_EQ(createSet(table, { firstHeap }), firstHeap);
    CHECK_EQ(createSet(table, { firstHeap, secondHeap, thirdHeap }), allHeaps);
    CHECK_EQ(table.numberOfLists(), 2u);

    CHECK(!createSet(table, { }));
    CHECK(!createSet(table, { { 9, 9 }, HeapRange() }));
    CHECK_EQ(table.numberOfLists(), 2u);
}

static void testHeapSetsAreInterned()
{
    HeapSetTable table;
    HeapSet set = createSet(table, { firstHeap, thirdHeap, pollHeap });
    CHECK_EQ(table.numberOfLists(), 1u);
    CHECK_EQ(createSet(table, { pollHeap, firstHeap, thirdHeap }), set);
    CHECK_EQ(createSet(table, { thirdHeap, pollHeap, HeapRange(), thirdHeap, firstHeap }), set);
    CHECK_EQ(table.unite(set, set), set);
    CHECK_EQ(table.unite(set, thirdHeap), set);
    CHECK_EQ(table.numberOfLists(), 1u);

    HeapSet withoutThirdHeap = createSet(table, { firstHeap, pollHeap });
    CHECK_EQ(table.numberOfLists(), 2u);
    CHECK_EQ(withoutThirdHeap.bounds(), set.bounds());
    CHECK(withoutThirdHeap != set);
    CHECK(HeapSet(set.bounds()) != set);
    CHECK(set != set.bounds());

    CHECK_EQ(table.unite(withoutThirdHeap, thirdHeap), set);
    CHECK_EQ(table.unite(thirdHeap, withoutThirdHeap), set);
    CHECK_EQ(table.numberOfLists(), 2u);
}

namespace {

struct ModeledSet {
    HeapSet set;
    uint64_t bits;
};

} // anonymous namespace

static constexpr unsigned numberOfModeledHeaps = 64;

static uint64_t bitsOf(const HeapRange& range)
{
    uint64_t result = 0;
    for (uint64_t heap = range.begin(); heap < range.end(); ++heap)
        result |= 1ull << heap;
    return result;
}

static uint64_t bitsOf(const HeapSet& set)
{
    uint64_t result = 0;
    for (const HeapRange& range : set.ranges())
        result |= bitsOf(range);
    return result;
}

static HeapRange randomRange(WeakRandom& random)
{
    uint64_t begin = random.getUint32(numberOfModeledHeaps);
    uint64_t end = std::min<uint64_t>(begin + random.getUint32(9), numberOfModeledHeaps);
    return HeapRange(begin, end);
}

static ModeledSet randomSet(HeapSetTable& table, WeakRandom& random)
{
    Vector<HeapRange> ranges;
    uint64_t bits = 0;
    for (unsigned remaining = random.getUint32(6); remaining--;) {
        ranges.append(randomRange(random));
        bits |= bitsOf(ranges.last());
    }
    return { table.create(ranges.span()), bits };
}

static void checkAgainstModel(HeapSetTable& table, const ModeledSet& first, const ModeledSet& second, const HeapRange& range)
{
    bool setsOverlap = first.bits & second.bits;
    checkIsNormalized(first.set);
    CHECK_EQ(bitsOf(first.set), first.bits);
    CHECK_EQ(first.set.overlaps(range), !!(first.bits & bitsOf(range)));
    CHECK_EQ(first.set.overlaps(second.set), setsOverlap);
    CHECK_EQ(second.set.overlaps(first.set), setsOverlap);
    CHECK_EQ(first.set.isSubsetOf(second.set), !(first.bits & ~second.bits));

    RangeSet<HeapRange> rangeSet;
    second.set.addTo(rangeSet);
    CHECK_EQ(first.set.overlaps(rangeSet), setsOverlap);

    HeapSet united = table.unite(first.set, second.set);
    checkIsNormalized(united);
    CHECK_EQ(bitsOf(united), first.bits | second.bits);
    CHECK_EQ(united, table.unite(second.set, first.set));
    CHECK(first.set.isSubsetOf(united));
    CHECK(second.set.isSubsetOf(united));
    CHECK_EQ(united.isSubsetOf(first.set), !(second.bits & ~first.bits));
}

static void testHeapSetOverlapsExactly()
{
    HeapSetTable table;
    HeapSet set = createSet(table, { { 1, 2 }, { 5, 6 } });
    CHECK(set.bounds().overlaps(HeapRange(2, 5)));
    CHECK(!set.overlaps(HeapRange(2, 5)));
    CHECK(!set.overlaps(HeapSet(HeapRange(2, 5))));
    CHECK(!HeapSet(HeapRange(2, 5)).overlaps(set));
    CHECK(!set.overlaps(HeapRange(0, 1)));
    CHECK(!set.overlaps(HeapRange(6, 9)));
    CHECK(!set.overlaps(HeapRange()));
    for (HeapRange range : { HeapRange(1, 2), HeapRange(0, 2), HeapRange(1, 5), HeapRange(4, 6), HeapRange(5, 9), HeapRange(0, 9) })
        CHECK(set.overlaps(range));

    HeapSet interleaved = createSet(table, { { 0, 1 }, { 3, 4 }, { 7, 8 } });
    CHECK(set.bounds().overlaps(interleaved.bounds()));
    CHECK(!set.overlaps(interleaved));
    CHECK(!interleaved.overlaps(set));

    HeapSet adjacent = createSet(table, { { 2, 5 }, { 6, 7 } });
    CHECK(!set.overlaps(adjacent));
    CHECK(!adjacent.overlaps(set));

    HeapSet sharingLastRange = createSet(table, { { 3, 4 }, { 5, 6 } });
    CHECK(set.overlaps(sharingLastRange));
    CHECK(sharingLastRange.overlaps(set));

    RangeSet<HeapRange> rangeSet;
    CHECK(!set.overlaps(rangeSet));
    interleaved.addTo(rangeSet);
    CHECK(!set.overlaps(rangeSet));
    rangeSet.add(HeapRange(5, 6));
    CHECK(set.overlaps(rangeSet));

    WeakRandom random(1);
    for (unsigned round = 0; round < 1000; ++round) {
        ModeledSet first = randomSet(table, random);
        ModeledSet second = randomSet(table, random);
        checkAgainstModel(table, first, second, randomRange(random));
    }
}

static void testHeapSetHasNoCap()
{
    constexpr uint64_t numberOfRanges = 1000;
    HeapSetTable table;
    Vector<HeapRange> ranges;
    Vector<HeapRange> gaps;
    for (uint64_t i = numberOfRanges; i--;) {
        ranges.append(HeapRange(2 * i));
        gaps.append(HeapRange(2 * i + 1));
    }

    HeapSet set = table.create(ranges.span());
    checkIsNormalized(set);
    CHECK_EQ(set.ranges().size(), numberOfRanges);
    CHECK_EQ(set.bounds(), HeapRange(0, 2 * numberOfRanges - 1));
    for (uint64_t i = 0; i < numberOfRanges; ++i) {
        CHECK(set.overlaps(HeapRange(2 * i)));
        CHECK(!set.overlaps(HeapRange(2 * i + 1)));
    }

    HeapSet gapSet = table.create(gaps.span());
    CHECK_EQ(gapSet.ranges().size(), numberOfRanges);
    CHECK(!set.overlaps(gapSet));
    CHECK(!gapSet.overlaps(set));
    CHECK(!set.isSubsetOf(gapSet));
    CHECK_EQ(table.unite(set, gapSet), HeapRange(0, 2 * numberOfRanges));
}

static void testHeapSetConvertsToBounds()
{
    HeapSetTable table;
    HeapSet set = createSet(table, { firstHeap, thirdHeap });
    HeapRange range = set;
    CHECK_EQ(range, allHeaps);
    CHECK_EQ(range, set.bounds());

    RangeSet<HeapRange> bounds;
    bounds.add(set);
    CHECK(bounds.contains(allHeaps));
    CHECK(bounds.overlaps(secondHeap));

    RangeSet<HeapRange> exactRanges;
    set.addTo(exactRanges);
    CHECK(exactRanges.contains(firstHeap));
    CHECK(exactRanges.contains(thirdHeap));
    CHECK(!exactRanges.overlaps(secondHeap));
    CHECK(!exactRanges.contains(allHeaps));
}

static void checkInterference(const Effects& first, const Effects& second, bool expected)
{
    CHECK_EQ(first.interferes(second), expected);
    CHECK_EQ(second.interferes(first), expected);
}

static void testEffectsInterfereBySets()
{
    HeapSetTable table;
    Effects writer;
    writer.writes = createSet(table, { firstHeap, thirdHeap });

    Effects reader;
    reader.reads = secondHeap;
    checkInterference(writer, reader, false);

    Effects writerOfBounds;
    writerOfBounds.writes = writer.writes.bounds();
    checkInterference(writerOfBounds, reader, true);

    reader.reads = thirdHeap;
    checkInterference(writer, reader, true);
    reader.reads = createSet(table, { secondHeap, pollHeap });
    checkInterference(writer, reader, false);
    reader.reads = createSet(table, { firstHeap, pollHeap });
    checkInterference(writer, reader, true);

    Effects otherWriter;
    otherWriter.writes = secondHeap;
    checkInterference(writer, otherWriter, false);
    otherWriter.writes = createSet(table, { thirdHeap, pollHeap });
    checkInterference(writer, otherWriter, true);

    Effects otherReader;
    otherReader.reads = reader.reads;
    checkInterference(reader, otherReader, false);
}

void addBunHeapSetTests(const TestConfig* config, BunTestTasks& tasks)
{
    RUN(testHeapSetOfOneRange());
    RUN(testHeapSetTableNormalizes());
    RUN(testHeapSetsAreInterned());
    RUN(testHeapSetOverlapsExactly());
    RUN(testHeapSetHasNoCap());
    RUN(testHeapSetConvertsToBounds());
    RUN(testEffectsInterfereBySets());
}

#endif // ENABLE(B3_JIT)
