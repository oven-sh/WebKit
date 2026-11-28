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
#include "AOTVMState.h"
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

#if OS(LINUX)
#endif

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

namespace {

struct DroppedClosure {
    Weak<JSObject> closure;
    Weak<JSString> text;
    Weak<Structure> structure;
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
static void changeObjectsUnseen(LoadedProgram& program)
{
    program.vm().aotState().nativeTestChangesObjectsUnseen = true;
    body(program);
}

template<LoadedProgramTest body>
static constexpr void (*withClosuresToDrop)() = runNativeTestWithLoadedProgram<cacheSweepTests, programWithClosuresToDrop, changeObjectsUnseen<body>>;

std::span<const NativeTest> cacheSweepTests()
{
    static constexpr NativeTest tests[] = {
        { "cache-sweep/closures/site-lets-go-of-structure-of-one-closure"_s, withClosuresToDrop<testSiteLetsGoOfStructureOfOneClosure> },
        { "cache-sweep/closures/what-is-remembered-of-a-structure-spells-no-address"_s, withClosuresToDrop<testWhatIsRememberedOfStructureSpellsNoAddress> },
        { "cache-sweep/empty-pages/regions-larger-than-a-page-stay"_s, testRegionsLargerThanPageStay },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
