/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableCensus.h"

#if ENABLE(AOT)

#include "AOTAllocationSiteRecord.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTPropertyCachesFront.h"
#include "AOTState.h"
#include "JSCConfig.h"
#include "VM.h"
#include <wtf/PageBlock.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

#if OS(UNIX)
#include <sys/mman.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

class TableCensus {
public:
    explicit TableCensus(PrintStream& out)
        : m_out(out)
    {
    }

    void count(ASCIILiteral name, const void* start, size_t bytes)
    {
        if (!start || !bytes)
            return;
        size_t page = pageSize();
        uintptr_t first = std::bit_cast<uintptr_t>(start) & ~(page - 1);
        uintptr_t end = roundUpToMultipleOf(page, std::bit_cast<uintptr_t>(start) + bytes);
        size_t numberOfPages = (end - first) / page;

        // Only pages that are present are read: a read would make a page present.
        m_residency.fill(1, numberOfPages);
#if OS(UNIX)
        if (mincore(std::bit_cast<void*>(first), end - first, reinterpret_cast<decltype(residencyArgument())>(m_residency.mutableSpan().data())))
            m_residency.fill(0, numberOfPages);
#endif

        uintptr_t firstWord = roundUpToMultipleOf<sizeof(uint64_t)>(std::bit_cast<uintptr_t>(start));
        uintptr_t endOfWords = (std::bit_cast<uintptr_t>(start) + bytes) & ~(sizeof(uint64_t) - 1);
        size_t touchedPages = 0;
        size_t pagesWithData = 0;
        size_t words = 0;
        for (size_t i = 0; i < numberOfPages; ++i) {
            if (!(m_residency[i] & 1))
                continue;
            ++touchedPages;
            size_t wordsInPage = 0;
            uintptr_t endInPage = std::min(endOfWords, first + (i + 1) * page);
            for (uintptr_t address = std::max(firstWord, first + i * page); address < endInPage; address += sizeof(uint64_t))
                wordsInPage += !!*std::bit_cast<const uint64_t*>(address);
            pagesWithData += !!wordsInPage;
            words += wordsInPage;
        }
        m_out.println("AOT live: table ", name, " bytes ", bytes, " touched ", touchedPages * page, " with-data ", pagesWithData * page, " words ", words, " address ", std::bit_cast<uintptr_t>(start));
    }

private:
#if OS(DARWIN)
    static char* residencyArgument();
#else
    static unsigned char* residencyArgument();
#endif

    PrintStream& m_out;
    Vector<uint8_t> m_residency;
};

} // anonymous namespace

static void countTablesOf(TableCensus& census, Instance& instance)
{
    const ProgramTables& tables = instance.tables();
    InstanceSizes sizes = tables.instanceSizes;
    bool usesWideReferences = instance.options().useWideReferences();
    size_t numberOfIdentifiers = Image::current()->text().numberOfIdentifiers();
    size_t numberOfAllocations = tables.allocations().size();
    std::span siteCaches = instance.siteCaches();

    census.count("cards of statics"_s, instance.cards(), static_cast<size_t>(InstanceLayout::offsetOfStatics(sizes) - InstanceLayout::offsetOfCards(sizes)));
    census.count("statics"_s, instance.statics(), sizes.sizeOfStatics);
    census.count("instance"_s, &instance.prefix(), sizeof(InstancePrefix) + sizeof(Instance));
    census.count("string cells"_s, reinterpret_cast<uint8_t*>(&instance) + InstanceLayout::offsetOfStringCells, sizes.numberOfStringCells * InstanceLayout::sizeOfStringCell);
    census.count("site caches"_s, siteCaches.data(), siteCaches.size_bytes());
    census.count("site cache states"_s, std::bit_cast<const void*>(instance.propertyCaches.siteCacheStateBias + (std::bit_cast<uintptr_t>(siteCaches.data()) >> SiteCacheState::slotAddressShift)), siteCaches.size() * sizeof(SiteCacheState));
    census.count("closure cells"_s, instance.closureCells, tables.functionKinds().size() * (usesWideReferences ? sizeof(uint64_t) : sizeof(uint32_t)));
    census.count("class prototypes"_s, instance.classes.classPrototypes, tables.numberOfClasses() * sizeof(JSCell*));
    census.count("allocation site records"_s, instance.objects.emptyAllocationSite() - numberOfAllocations, (numberOfAllocations + 1) * sizeof(AllocationSiteRecord));
    census.count("distances to allocation site records"_s, instance.objects.distancesToAllocationSites, numberOfAllocations * sizeof(int32_t));
    census.count("literal strings"_s, instance.runtime.constants.literalStrings, tables.literalStrings().size() * sizeof(JSCell*));
    census.count("key cells"_s, instance.runtime.constants.keyCells, numberOfIdentifiers * sizeof(JSCell*));
    census.count("program identifiers"_s, instance.image.programIdentifiers, numberOfIdentifiers * sizeof(UniquedStringImpl*));
    census.count("key order transitions"_s, instance.objects.keyOrderTransitions, (static_cast<size_t>(instance.objects.keyOrderTransitionMask) + 1) * sizeof(KeyOrderTransition));
    census.count("hashed slots"_s, instance.propertyCaches.hashedSlots, instance.propertyCaches.hashedSlotOffsetMask + sizeof(HashedSlot));
    for (const FrontWords& front : instance.propertyCaches.front)
        census.count("row displacement plane"_s, front.plane, RowDisplacementTable::cellsPerPlane * sizeof(PackedSlot));
    census.count("rows of structures"_s, instance.propertyCaches.front[0].rowsForStructureIDZero + (static_cast<uint32_t>(g_jscConfig.startOfStructureHeap) >> StructureRowWords::granuleShift), (g_jscConfig.sizeOfStructureHeap >> StructureRowWords::granuleShift) << StructureRowWords::wordShift);
}

void dumpTableCensus(VM& vm, PrintStream& out)
{
    TableCensus census(out);
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances)
        countTablesOf(census, *instance);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
