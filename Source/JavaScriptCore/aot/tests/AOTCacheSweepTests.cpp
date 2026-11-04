/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTDroppedClosureTestSupport.h"
#include "AOTEmptyPages.h"
#include "AOTPropertyCaches.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "WeakInlines.h"
#include <array>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

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

namespace {

struct DroppedClosure {
    Weak<JSObject> closure;
    Weak<JSString> text;
    Weak<Structure> structure;
    // The number of a Structure is the low half of its address, and the collector takes every word of a frame for a reference. Beside `true` and three bytes that nobody had written since the address lay there,
    // the number spelled the whole address wherever the heap of Structures began at ..01'00000000: in one run of 256 the record in the frame of the test kept the Structure that the test had dropped.
    uint32_t complementOfStructureID { 0 };
    bool hasStructureOfItsOwn { false };

    StructureID structureID() const { return std::bit_cast<StructureID>(~complementOfStructureID); }
};

enum class StructureOfClosure : bool { IsShared, IsItsOwn };

} // anonymous namespace

static constexpr unsigned numberOfClosuresDropped = 64;

static DroppedClosure nameAndReadClosure(LoadedProgram& program, FunctionID function, unsigned number, StructureOfClosure structure)
{
    VM& vm = program.vm();
    JSString* text = createTextToDrop(program, number);
    JSObject* source = createSourceOfDisplayName(program, text);
    bool hasStructureOfItsOwn = structure == StructureOfClosure::IsItsOwn;
    JSObject* closure = hasStructureOfItsOwn ? createClosureWithStructureOfItsOwn(program, function, source) : createClosureWithSharedStructure(program, function, source);
    AOT_TEST_CHECK(closure->get(program.realm(), vm.propertyNames->displayName) == JSValue(text));
    fileReadOfDisplayName(program, closure);
    return { Weak<JSObject>(closure), Weak<JSString>(text), Weak<Structure>(closure->structure()), ~closure->structureID().bits(), hasStructureOfItsOwn };
}

static NEVER_INLINE void nameReadAndDropClosures(LoadedProgram& program, FunctionID function, Vector<DroppedClosure>& dropped)
{
    for (unsigned number = 0; number < numberOfClosuresDropped; ++number)
        dropped.append(nameAndReadClosure(program, function, number, number % 2 ? StructureOfClosure::IsShared : StructureOfClosure::IsItsOwn));
}

static NEVER_INLINE void readThroughSiteAndDropClosure(LoadedProgram& program, FunctionID function, StructureOfClosure structure, DroppedClosure& dropped)
{
    dropped = nameAndReadClosure(program, function, numberOfClosuresDropped, structure);
    fillSiteWithReadOfDisplayName(program, dropped.closure.get());
}

static void collectWhatWasDropped(LoadedProgram& program)
{
    program.vm().heap.collectSync(CollectionScope::Full);
    program.vm().heap.collectSync(CollectionScope::Full);
}

static bool tablesAnswerFor(LoadedProgram& program, StructureID id)
{
    return !!program.state().propertyCaches->tables().find(id, displayNameInCaches(program), SlotAccess::Read);
}

static void testTextsGivenAsDisplayNamesEndWithTheirClosures(LoadedProgram& program)
{
    if (framesAreThoseOfAddressSanitizer)
        return;
    bool hasSeenSlimClosures = false;
    bool hasSeenFullClosures = false;
    for (ASCIILiteral name : namesOfFunctionsWithClosuresToDrop) {
        FunctionID function = findFunctionWithClosuresToDrop(program, name);
        (functionKindData(function).isSlim ? hasSeenSlimClosures : hasSeenFullClosures) = true;
        Vector<DroppedClosure> dropped;
        nameReadAndDropClosures(program, function, dropped);
        collectWhatWasDropped(program);

        unsigned numberOfTextsLeft = 0;
        unsigned numberOfOwnStructuresLeft = 0;
        for (const DroppedClosure& one : dropped) {
            numberOfTextsLeft += !!one.text;
            numberOfOwnStructuresLeft += one.hasStructureOfItsOwn && one.structure;
            AOT_TEST_CHECK(one.structure || !tablesAnswerFor(program, one.structureID()));
        }
        AOT_TEST_CHECK(numberOfTextsLeft < numberOfClosuresDropped / 4);
        AOT_TEST_CHECK(numberOfOwnStructuresLeft < numberOfClosuresDropped / 4);
    }
    AOT_TEST_CHECK(hasSeenSlimClosures && hasSeenFullClosures);
}

// The closures that follow go the same way through the same frames, so no word of the stack names the first one any more.
static void testSiteKeepsStructureThatNextClosureGetsAndNotWhatClosureHeld(LoadedProgram& program)
{
    if (framesAreThoseOfAddressSanitizer)
        return;
    SiteCache site = siteOfTestsOfDroppedClosures(program);
    for (ASCIILiteral name : namesOfFunctionsWithClosuresToDrop) {
        FunctionID function = findFunctionWithClosuresToDrop(program, name);
        DroppedClosure read;
        readThroughSiteAndDropClosure(program, function, StructureOfClosure::IsShared, read);
        Vector<DroppedClosure> others;
        nameReadAndDropClosures(program, function, others);
        collectWhatWasDropped(program);

        AOT_TEST_CHECK(!read.closure && !read.text);
        AOT_TEST_CHECK(read.structure && site->get().structureID == read.structureID() && tablesAnswerFor(program, read.structureID()));
    }
}

static void testSiteLetsGoOfStructureOfOneClosure(LoadedProgram& program)
{
    if (framesAreThoseOfAddressSanitizer)
        return;
    SiteCache site = siteOfTestsOfDroppedClosures(program);
    for (ASCIILiteral name : namesOfFunctionsWithClosuresToDrop) {
        FunctionID function = findFunctionWithClosuresToDrop(program, name);
        DroppedClosure read;
        readThroughSiteAndDropClosure(program, function, StructureOfClosure::IsItsOwn, read);
        Vector<DroppedClosure> others;
        nameReadAndDropClosures(program, function, others);
        collectWhatWasDropped(program);

        AOT_TEST_CHECK(!read.closure && !read.text && !read.structure);
        AOT_TEST_CHECK(site->get().isEmpty() && !tablesAnswerFor(program, read.structureID()));
    }
}

// No half of a word of the record is the number of the Structure: whatever lies beside it, no word of a frame that holds the record is the address.
static void testWhatIsRememberedOfStructureSpellsNoAddress(LoadedProgram& program)
{
    DroppedClosure read;
    readThroughSiteAndDropClosure(program, findFunctionWithClosuresToDrop(program, namesOfFunctionsWithClosuresToDrop[0]), StructureOfClosure::IsItsOwn, read);
    uint32_t number = read.structureID().bits();
    AOT_TEST_CHECK(number && read.structure && read.structure->id().bits() == number);
    std::array<uint32_t, sizeof(DroppedClosure) / sizeof(uint32_t)> halves;
    memcpy(halves.data(), static_cast<const void*>(&read), sizeof(halves));
    for (uint32_t half : halves)
        AOT_TEST_CHECK(half != number);
}

template<LoadedProgramTest body>
static constexpr void (*withClosuresToDrop)() = runNativeTestWithLoadedProgram<cacheSweepTests, programWithClosuresToDrop, body>;

std::span<const NativeTest> cacheSweepTests()
{
    static constexpr NativeTest tests[] = {
        { "cache-sweep/closures/texts-given-as-display-names-end-with-their-closures"_s, withClosuresToDrop<testTextsGivenAsDisplayNamesEndWithTheirClosures> },
        { "cache-sweep/closures/site-keeps-structure-that-next-closure-gets-and-not-what-closure-held"_s, withClosuresToDrop<testSiteKeepsStructureThatNextClosureGetsAndNotWhatClosureHeld> },
        { "cache-sweep/closures/site-lets-go-of-structure-of-one-closure"_s, withClosuresToDrop<testSiteLetsGoOfStructureOfOneClosure> },
        { "cache-sweep/closures/what-is-remembered-of-a-structure-spells-no-address"_s, withClosuresToDrop<testWhatIsRememberedOfStructureSpellsNoAddress> },
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
