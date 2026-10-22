/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTEmptyPages.h"
#include "AOTSiteCaches.h"
#include "JSCInlines.h"
#include <array>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Vector.h>

#if OS(LINUX)
#include <sys/mman.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> cacheSweepTests();

namespace {

using CacheEntry = RememberedSlot<uint64_t>;

constexpr unsigned regionShift = 8;
constexpr size_t regionSize = 1 << regionShift;

class Pages {
    WTF_MAKE_NONCOPYABLE(Pages);
public:
    explicit Pages(size_t numberOfPages)
        : m_size(numberOfPages * pageSize())
        , m_start(static_cast<uint8_t*>(OSAllocator::reserveAndCommit(m_size, OSAllocator::FastMallocPages)))
    {
    }

    ~Pages() { OSAllocator::decommitAndRelease(m_start, m_size); }

    std::span<uint8_t> bytes() const { return { m_start, m_size }; }
    uint8_t* page(size_t index) const { return m_start + index * pageSize(); }
    CacheEntry& entryAt(size_t index, size_t offsetInPage) const { return *reinterpret_cast<CacheEntry*>(page(index) + offsetInPage); }

private:
    size_t m_size;
    uint8_t* m_start;
};

} // anonymous namespace

static Vector<size_t> startsOfRegionsEverWritten(RememberedSlotSet& set)
{
    Vector<size_t> starts;
    set.forEachRegionToSweep(CollectionScope::Full, [&](size_t begin, size_t) {
        starts.append(begin);
    });
    return starts;
}

static Vector<size_t> startsOfRegionsWrittenSinceLastSweep(RememberedSlotSet& set)
{
    Vector<size_t> starts;
    set.forEachRegionToSweep(CollectionScope::Eden, [&](size_t begin, size_t) {
        starts.append(begin);
    });
    return starts;
}

#if OS(LINUX)
// A read makes a page present, so this comes before any look at what the page holds.
static bool isPresent(void* page)
{
    unsigned char residency = 0;
    AOT_TEST_CHECK(!mincore(page, pageSize(), &residency));
    return residency & 1;
}
#endif

static void testPagesWhoseEntriesDiedGoBack()
{
    Pages table(6);
    RememberedSlotSet set(table.bytes(), regionShift);
    size_t page = pageSize();
    size_t lastOffset = page - sizeof(CacheEntry);

    table.entryAt(0, 0).set(set, 1);
    table.entryAt(2, 2 * regionSize).set(set, 2);
    table.entryAt(2, lastOffset).set(set, 3);
    table.entryAt(3, 0).set(set, 4);
    table.entryAt(5, regionSize).set(set, 5);
    table.entryAt(5, 4 * regionSize).set(set, 6);
    AOT_TEST_CHECK(!decommitEmptyPages(table.bytes(), set));
    AOT_TEST_CHECK(startsOfRegionsEverWritten(set).size() == 6);

    table.entryAt(2, 2 * regionSize).clear();
    table.entryAt(2, lastOffset).clear();
    table.entryAt(3, 0).clear();
    table.entryAt(5, regionSize).clear();
    AOT_TEST_CHECK(decommitEmptyPages(table.bytes(), set) == 2 * page);
#if OS(LINUX)
    AOT_TEST_CHECK(isPresent(table.page(0)) && !isPresent(table.page(2)) && !isPresent(table.page(3)) && isPresent(table.page(5)));
#endif
    AOT_TEST_CHECK((startsOfRegionsEverWritten(set) == Vector<size_t> { 0, 5 * page + regionSize, 5 * page + 4 * regionSize }));
    AOT_TEST_CHECK(table.entryAt(0, 0).get() == 1 && table.entryAt(5, 4 * regionSize).get() == 6);
    AOT_TEST_CHECK(holdsOnlyZeros(table.bytes().subspan(page, 4 * page)));
    AOT_TEST_CHECK(!decommitEmptyPages(table.bytes(), set));

    table.entryAt(2, lastOffset).set(set, 7);
    AOT_TEST_CHECK(table.entryAt(2, lastOffset).get() == 7);
    AOT_TEST_CHECK((startsOfRegionsWrittenSinceLastSweep(set) == Vector<size_t> { 3 * page - regionSize }));
    AOT_TEST_CHECK(!decommitEmptyPages(table.bytes(), set));
    AOT_TEST_CHECK(table.entryAt(2, lastOffset).get() == 7);
}

static void testPagesNeverWrittenAreNotTouched()
{
    Pages table(3);
    RememberedSlotSet set(table.bytes(), regionShift);
    table.entryAt(1, 0).set(set, 1);
    table.entryAt(1, 0).clear();
    AOT_TEST_CHECK(decommitEmptyPages(table.bytes(), set) == pageSize());
#if OS(LINUX)
    AOT_TEST_CHECK(!isPresent(table.page(0)) && !isPresent(table.page(1)) && !isPresent(table.page(2)));
#endif
    AOT_TEST_CHECK(startsOfRegionsEverWritten(set).isEmpty());
}

static void testPartOfPageStays()
{
    Pages pages(2);
    size_t page = pageSize();
    std::span<uint8_t> table = pages.bytes().first(page + page / 2);
    RememberedSlotSet set(table, regionShift);
    pages.entryAt(0, regionSize).set(set, 1);
    pages.entryAt(1, regionSize).set(set, 2);
    pages.entryAt(1, page / 2).setWithoutRecordingForTesting(3);
    pages.entryAt(0, regionSize).clear();
    pages.entryAt(1, regionSize).clear();

    AOT_TEST_CHECK(decommitEmptyPages(table, set) == page);
    AOT_TEST_CHECK((startsOfRegionsEverWritten(set) == Vector<size_t> { page + regionSize }));
    AOT_TEST_CHECK(pages.entryAt(1, page / 2).get() == 3);
}

static void testTableThatStartsInsidePageStays()
{
    Pages pages(3);
    std::span<uint8_t> table = pages.bytes().subspan(regionSize);
    RememberedSlotSet set(table, regionShift);
    pages.entryAt(0, 0).setWithoutRecordingForTesting(1);
    pages.entryAt(1, 0).set(set, 2);
    pages.entryAt(1, 0).clear();

    AOT_TEST_CHECK(!decommitEmptyPages(table, set));
    AOT_TEST_CHECK(startsOfRegionsEverWritten(set).size() == 1);
    AOT_TEST_CHECK(pages.entryAt(0, 0).get() == 1);
}

static void testRegionsLargerThanPageStay()
{
    constexpr unsigned shiftOfLargeRegion = 20;
    Pages table(2);
    RememberedSlotSet set(table.bytes(), shiftOfLargeRegion);
    table.entryAt(1, 0).set(set, 1);
    table.entryAt(1, 0).clear();
    AOT_TEST_CHECK((static_cast<size_t>(1) << shiftOfLargeRegion) > pageSize());
    AOT_TEST_CHECK(!decommitEmptyPages(table.bytes(), set));
    AOT_TEST_CHECK(startsOfRegionsEverWritten(set).size() == 1);
}

static void testHoldsOnlyZeros()
{
    alignas(uint64_t) std::array<uint8_t, 27> bytes { };
    std::span<const uint8_t> all { bytes };
    AOT_TEST_CHECK(holdsOnlyZeros(all) && holdsOnlyZeros(all.first(0)) && holdsOnlyZeros(all.subspan(3)));
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = 1;
        AOT_TEST_CHECK(!holdsOnlyZeros(all) && holdsOnlyZeros(all.first(index)) && holdsOnlyZeros(all.subspan(index + 1)));
        bytes[index] = 0;
    }
}

std::span<const NativeTest> cacheSweepTests()
{
    static constexpr NativeTest tests[] = {
        { "cache-sweep/empty-pages/pages-whose-entries-died-go-back"_s, testPagesWhoseEntriesDiedGoBack },
        { "cache-sweep/empty-pages/pages-never-written-are-not-touched"_s, testPagesNeverWrittenAreNotTouched },
        { "cache-sweep/empty-pages/part-of-a-page-stays"_s, testPartOfPageStays },
        { "cache-sweep/empty-pages/table-that-starts-inside-a-page-stays"_s, testTableThatStartsInsidePageStays },
        { "cache-sweep/empty-pages/regions-larger-than-a-page-stay"_s, testRegionsLargerThanPageStay },
        { "cache-sweep/empty-pages/holds-only-zeros"_s, testHoldsOnlyZeros },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
