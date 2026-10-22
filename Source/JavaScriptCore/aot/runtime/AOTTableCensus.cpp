/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableCensus.h"

#if ENABLE(AOT)

#include "AOTAllocationSiteRecord.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTCompileOptions.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRuntimeCounters.h"
#include "AOTSiteCaches.h"
#include "AOTSlotRepresentation.h"
#include "AOTState.h"
#include "JSCConfig.h"
#include "VM.h"
#include <algorithm>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/PageBlock.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

#if OS(UNIX)
#include <sys/mman.h>
#endif
#if OS(LINUX)
#include <fcntl.h>
#include <unistd.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct PageCounts {
    size_t touchedPages { 0 };
    size_t privatePages { 0 };
    size_t pagesWithData { 0 };
    size_t words { 0 };
};

class TableCensus {
    WTF_MAKE_NONCOPYABLE(TableCensus);
public:
    explicit TableCensus(PrintStream& out)
        : m_out(out)
    {
#if OS(LINUX)
        m_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
#endif
    }

    ~TableCensus()
    {
#if OS(LINUX)
        if (m_pagemap >= 0)
            close(m_pagemap);
#endif
    }

    void count(ASCIILiteral name, const void* start, size_t bytes)
    {
        if (!start || !bytes)
            return;
        uintptr_t address = std::bit_cast<uintptr_t>(start);
        m_tables.append({ address, address + bytes });
        printLine(""_s, name, address, bytes, measure(address, bytes));
    }

    // The mapping as a whole, then every run of its pages that holds a touched page and no part of a table counted so far.
    void countMapping(ASCIILiteral name, const void* start, size_t bytes)
    {
        size_t page = pageSize();
        uintptr_t next = std::bit_cast<uintptr_t>(start);
        uintptr_t end = next + bytes;
        printLine("whole: "_s, name, next, bytes, measure(next, bytes));

        auto countPagesUpTo = [&](uintptr_t limit) {
            if (next >= limit)
                return;
            PageCounts counts = measure(next, limit - next);
            if (counts.touchedPages)
                printLine("no row: "_s, name, next, limit - next, counts);
        };
        std::ranges::sort(m_tables);
        for (auto [startOfTable, endOfTable] : m_tables) {
            if (endOfTable <= next)
                continue;
            if (startOfTable >= end)
                break;
            countPagesUpTo(startOfTable & ~(page - 1));
            next = std::max(next, roundUpToMultipleOf(page, endOfTable));
        }
        countPagesUpTo(end);
    }

private:
    PageCounts measure(uintptr_t start, size_t bytes)
    {
        size_t page = pageSize();
        uintptr_t first = start & ~(page - 1);
        uintptr_t end = roundUpToMultipleOf(page, start + bytes);
        size_t numberOfPages = (end - first) / page;

        // Only pages that are present are read: a read would make a page present.
        m_residency.fill(1, numberOfPages);
#if OS(UNIX)
        if (mincore(std::bit_cast<void*>(first), end - first, reinterpret_cast<decltype(residencyArgument())>(m_residency.mutableSpan().data())))
            m_residency.fill(0, numberOfPages);
#endif

        uintptr_t firstWord = roundUpToMultipleOf<sizeof(uint64_t)>(start);
        uintptr_t endOfWords = (start + bytes) & ~(sizeof(uint64_t) - 1);
        PageCounts counts;
        for (size_t i = 0; i < numberOfPages; ++i) {
            if (!(m_residency[i] & 1))
                continue;
            ++counts.touchedPages;
            size_t wordsInPage = 0;
            uintptr_t endInPage = std::min(endOfWords, first + (i + 1) * page);
            for (uintptr_t address = std::max(firstWord, first + i * page); address < endInPage; address += sizeof(uint64_t))
                wordsInPage += !!*std::bit_cast<const uint64_t*>(address);
            counts.pagesWithData += !!wordsInPage;
            counts.words += wordsInPage;
        }
#if OS(LINUX)
        counts.privatePages = countPrivatePages(first / page, numberOfPages);
#else
        // Elsewhere a page that has only been read is the process's own as well.
        counts.privatePages = counts.touchedPages;
#endif
        return counts;
    }

    void printLine(ASCIILiteral kind, ASCIILiteral name, uintptr_t start, size_t bytes, const PageCounts& counts)
    {
        size_t page = pageSize();
        m_out.println("AOT live: table ", kind, name, " bytes ", bytes, " touched ", counts.touchedPages * page, " with-data ", counts.pagesWithData * page, " words ", counts.words, " private ", counts.privatePages * page, " address ", start);
    }

#if OS(LINUX)
    // Where a page has only been read, Linux maps the one page of zeros that every process shares.
    size_t countPrivatePages(size_t firstPage, size_t numberOfPages)
    {
        constexpr uint64_t isPresent = 1ull << 63;
        constexpr uint64_t isMappedOnlyHere = 1ull << 56;
        m_pagemapEntries.fill(0, numberOfPages);
        size_t bytes = numberOfPages * sizeof(uint64_t);
        if (m_pagemap < 0 || pread(m_pagemap, m_pagemapEntries.mutableSpan().data(), bytes, firstPage * sizeof(uint64_t)) != static_cast<ssize_t>(bytes))
            return 0;
        size_t result = 0;
        for (uint64_t entry : m_pagemapEntries)
            result += (entry & (isPresent | isMappedOnlyHere)) == (isPresent | isMappedOnlyHere);
        return result;
    }
#endif

#if OS(DARWIN)
    static char* residencyArgument();
#else
    static unsigned char* residencyArgument();
#endif

    PrintStream& m_out;
    Vector<uint8_t> m_residency;
    Vector<std::pair<uintptr_t, uintptr_t>> m_tables;
#if OS(LINUX)
    int m_pagemap { -1 };
    Vector<uint64_t> m_pagemapEntries;
#endif
};

} // anonymous namespace

// Every instance of a VM has the same words for these.
static void countTablesSharedByInstances(TableCensus& census, const Instance& instance)
{
    const PropertyCacheWords& caches = instance.propertyCaches;
    census.count("hashed slots"_s, caches.hashedSlots, caches.hashedSlotOffsetMask + sizeof(HashedSlot));
    for (const FrontWords& front : caches.front)
        census.count("row displacement plane"_s, front.plane, RowDisplacementTable::cellsPerPlane * sizeof(PackedSlot));
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    const ImageText& text = instance.vm->aotState().image->text();
    census.count("lazy string headers"_s, text.lazyHeaders(), text.lazyHeadersSize());
    census.count("lazy string header offsets"_s, text.lazyHeaderOffsets(), text.stringRecords().size() * sizeof(uint32_t));
#endif
    census.count("rows of structures"_s, caches.front[0].rowsForStructureIDZero + (static_cast<uint32_t>(g_jscConfig.startOfStructureHeap) >> StructureRowWords::granuleShift), (g_jscConfig.sizeOfStructureHeap >> StructureRowWords::granuleShift) << StructureRowWords::wordShift);
}

static void countTablesOf(TableCensus& census, Instance& instance)
{
    const ProgramTables& tables = instance.tables();
    InstanceSizes sizes = tables.instanceSizes;
    bool usesWideReferences = instance.options().useWideReferences();
    size_t sizeOfReference = usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t);
    size_t numberOfAllocations = tables.allocations().size();
    std::span propertyRuns = tables.propertyRuns();
    size_t numberOfPropertyRuns = propertyRuns.empty() ? 0 : propertyRuns[0] - 1;
    std::span siteCaches = instance.siteCaches();
    uint8_t* base = reinterpret_cast<uint8_t*>(&instance);
    const ConstantCellWords& constants = instance.runtime.constants;
    const CounterWords& counters = instance.runtime.counters;
    const ObjectWords& objects = instance.objects;

    auto countCells = [&](ASCIILiteral name, const void* narrowCells, const void* wideCells, size_t numberOfCells) {
        census.count(name, usesWideReferences ? wideCells : narrowCells, numberOfCells * sizeOfReference);
    };

    census.count("cards of statics"_s, instance.cards(), static_cast<size_t>(InstanceLayout::offsetOfStatics(sizes) - InstanceLayout::offsetOfCards(sizes)));
    census.count("statics"_s, instance.statics(), sizes.sizeOfStatics);
    census.count("instance"_s, &instance.prefix(), sizeof(InstancePrefix) + sizeof(Instance));
    countCells("string cells"_s, base + InstanceLayout::offsetOfStringCells, constants.wideStringCells, sizes.numberOfStringCells);
    census.count("site caches"_s, siteCaches.data(), siteCaches.size_bytes());
    if (const SiteCacheState* states = instance.propertyCaches.siteCacheStates)
        census.count("site cache states"_s, states, SiteCacheState::numberOfStates * sizeof(SiteCacheState));
    std::span sitesThatMissed = instance.state->siteCaches->sitesThatMissed();
    census.count("sites that missed"_s, sitesThatMissed.data(), sitesThatMissed.size_bytes());
    std::span cardsOfSiteCaches = instance.state->siteCaches->cards();
    census.count("cards of site caches"_s, cardsOfSiteCaches.data(), cardsOfSiteCaches.size());
    census.count("closure cells"_s, instance.closureCells, tables.numberOfClosureCells * sizeOfReference);
    census.count("class prototypes"_s, instance.classes.classPrototypes, tables.numberOfClasses() * sizeof(JSCell*));
    census.count("allocation site records"_s, objects.emptyAllocationSite() - numberOfAllocations, (numberOfAllocations + 1) * sizeof(AllocationSiteRecord));
    census.count("distances to allocation site records"_s, objects.distancesToAllocationSites, numberOfAllocations * sizeof(int32_t));
    countCells("literal strings"_s, constants.literalStrings, constants.wideLiteralStrings, tables.literalStrings().size());
    census.count("literal string cache"_s, constants.literalStringCache, sizeof(uint64_t) << literalStringCacheIndexBits);
    if (constants.sizeOfStringHeaders)
        census.count("headers of string constants"_s, constants.startOfStringHeaders, constants.sizeOfStringHeaders);
    countCells("key cells"_s, constants.keyCells, constants.wideKeyCells, tables.keyCellIdentifiers().size());
    if (constants.functionNameCache)
        census.count("names of functions"_s, constants.functionNameCache, ConstantCellWords::numberOfCachedFunctionNames * sizeof(uint64_t));
    census.count("BigInt cells"_s, constants.bigIntCells, tables.bigIntConstants().size() * sizeof(JSCell*));
    census.count("cells of built-ins"_s, instance.prefix().builtinCells, tables.builtinCells().size() * sizeof(JSCell*));
    census.count("class infos of built-in classes"_s, instance.realm.instanceClassInfos, numberOfBuiltinClasses * sizeof(const ClassInfo*));
    census.count("property runs"_s, instance.runtime.literals.propertyRuns, numberOfPropertyRuns * sizeof(const PropertyRun*));
    census.count("global bindings"_s, counters.globalBindings, tables.globalBindings().size() * sizeof(GlobalBinding));
    census.count("branch counts"_s, counters.branchCounts, runtimeLimitValue(*instance.vm, LimitID::BranchCounters) * sizeof(uint64_t));
    census.count("module statuses"_s, instance.modules.status, tables.modules().size());
    census.count("key order transitions"_s, objects.keyOrderTransitions, (static_cast<size_t>(objects.keyOrderTransitionMask) + 1) * sizeof(KeyOrderTransition));
    census.count("key order runs"_s, objects.keyOrderRuns, numberOfKeyOrderRuns * sizeof(KeyOrderRun));
    census.count("key order steps"_s, base + InstanceLayout::offsetOfKeyOrderSteps, numberOfKeyOrderStepCaches * sizeof(KeyOrderStepCache));
    census.count("key lists"_s, objects.keyLists, numberOfKeyLists * sizeof(KeyListCacheEntry));
    census.count("enumerators"_s, objects.enumerators, numberOfCachedEnumerators * sizeof(EnumeratorCacheEntry));
    census.count("prototype transitions"_s, objects.prototypeTransitions, numberOfPrototypeTransitions * sizeof(PrototypeTransition));

    // The loop poll page can be inaccessible.
    uint8_t* startOfReservation = base - InstanceLayout::bytesReservedBelowInstance(sizes);
    uint8_t* endOfReservation = base + InstanceLayout::bytesReservedFromInstance(sizes);
    uint8_t* loopPollPage = base + InstanceLayout::offsetOfLoopPollPage();
    uint8_t* endOfLoopPollPage = loopPollPage + InstanceLayout::bytesReservedForLoopPollPage;
    census.countMapping("the instance's reservation"_s, startOfReservation, static_cast<size_t>(loopPollPage - startOfReservation));
    census.countMapping("the instance's reservation"_s, endOfLoopPollPage, static_cast<size_t>(endOfReservation - endOfLoopPollPage));
    for (auto [pages, size] : instance.state->pages.ranges())
        census.countMapping("zeroed pages of the state"_s, pages, size);
}

void dumpTableCensus(VM& vm, PrintStream& out)
{
    TableCensus census(out);
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    if (!state.instances.isEmpty())
        countTablesSharedByInstances(census, *state.instances.first());
    for (Instance* instance : state.instances)
        countTablesOf(census, *instance);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
