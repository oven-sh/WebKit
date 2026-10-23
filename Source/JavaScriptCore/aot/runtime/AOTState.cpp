/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTState.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTArrivalCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinModules.h"
#include "AOTChainDependents.h"
#include "AOTCompileOptions.h"
#include "AOTCoroutineTable.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImmortalCells.h"
#include "AOTModules.h"
#include "AOTObjectModel.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeValidation.h"
#include "AbstractSlotVisitorInlines.h"
#include "JSCJSValueInlines.h"
#include "MarkedBlockCage.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include <wtf/OSAllocator.h>
#include <wtf/StdLibExtras.h>

#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

#if OS(LINUX)
#if defined(MADV_POPULATE_WRITE)
static constexpr int populateStateForWriting = MADV_POPULATE_WRITE;
#else
static constexpr int populateStateForWriting = 23;
#endif
#endif

void* ZeroedPages::allocate(size_t bytes, FirstUse firstUse)
{
    size_t size = roundUpToMultipleOf(pageSize(), std::max<size_t>(bytes, 1));
    void* pages = OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(pages, size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
    // A kernel older than 5.14 refuses, and the pages fault in one by one.
    if (firstUse == FirstUse::AllAtOnce)
        madvise(pages, size, populateStateForWriting);
#else
    UNUSED_PARAM(firstUse);
#endif
    m_ranges.append({ pages, size });
    return pages;
}

void ZeroedPages::release(void* pages)
{
    size_t index = m_ranges.findIf([&](const auto& range) {
        return range.first == pages;
    });
    RELEASE_ASSERT(index != notFound);
    OSAllocator::decommitAndRelease(pages, m_ranges[index].second);
    m_ranges.removeAt(index);
}

ZeroedPages::~ZeroedPages()
{
    for (auto& [pages, size] : m_ranges)
        OSAllocator::decommitAndRelease(pages, size);
}

static size_t sizeOfZeroedTable(size_t count, size_t size)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(count, 1) * size);
}

static size_t arenaCapacity(const ProgramTables& tables)
{
    constexpr size_t sizeOfFixedSizeTables = 2 * MB;
    bool usesWideReferences = Image::current()->header().options.useWideReferences();
    size_t sizeOfCellReference = usesWideReferences ? sizeof(JSCell*) : sizeof(uint32_t);
    std::span propertyRuns = tables.propertyRuns();
    size_t numberOfPropertyRuns = propertyRuns.empty() ? 0 : propertyRuns[0] - 1;
    size_t capacity = sizeOfFixedSizeTables
        + sizeOfZeroedTable(tables.firstEnvironmentShape, sizeof(uint32_t))
        + sizeOfZeroedTable(tables.allocations().size() + 1, sizeof(AllocationSiteRecord) + sizeof(int32_t))
        + sizeOfZeroedTable(tables.numberOfClasses(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.numberOfClosureCells, sizeOfCellReference)
        + sizeOfZeroedTable(tables.literalStrings().size(), sizeOfCellReference)
        + sizeOfZeroedTable(tables.keyCellIdentifiers().size(), sizeOfCellReference)
        + sizeOfZeroedTable(tables.bigIntConstants().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(InstanceLayout::holdsBuiltinCells(tables.builtinCells().size()) ? 0 : tables.builtinCells().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(numberOfPropertyRuns, sizeof(const PropertyRun*))
        + sizeOfZeroedTable(SiteCacheState::numberOfStates, sizeof(SiteCacheState))
        + sizeOfZeroedTable(SiteCaches::numberOfMissWords(tables.instanceSizes.numberOfSiteCaches), sizeof(uint64_t))
        + sizeOfZeroedTable(RememberedSlotSet::numberOfCardsFor(tables.instanceSizes.numberOfSiteCaches * sizeof(Slot), SiteCaches::regionShift), sizeof(uint8_t))
        + sizeOfZeroedTable(InstanceLayout::holdsGlobalBindings(tables.globalBindings().size() * sizeof(GlobalBinding)) ? 0 : tables.globalBindings().size(), sizeof(GlobalBinding))
        + sizeOfZeroedTable(tables.modules().size(), sizeof(uint8_t));
    if (usesWideReferences)
        capacity += sizeOfZeroedTable(tables.instanceSizes.numberOfStringCells, sizeof(JSCell*));
    return capacity;
}

ZeroedTableArena::ZeroedTableArena(ZeroedPages& pages, const ProgramTables& tables)
    : m_pages(pages)
{
    m_capacity = arenaCapacity(tables);
    m_start = static_cast<char*>(pages.allocate(m_capacity));
}

State::State(Instance& instance)
    : instance(instance)
    , tables(instance.tables())
    , tableArena(pages, tables)
    , propertyCaches(*this)
{
    instance.state = this;
    caches = makeUnique<CacheRegistry>();
    immortalCells = makeUnique<ImmortalCells>(*this);
    strings = makeUnique<ProgramStrings>(*this);
    statics = makeUnique<ProgramStatics>(*this);
    builtinCells = makeUnique<BuiltinCells>(*this);
    chainDependents = makeUnique<ChainDependents>(*this);
    siteCaches = makeUniqueWithoutFastMallocCheck<SiteCaches>(*this);
    initializePropertyCacheWords(instance);
    modules = makeUnique<Modules>(*this);
    builtinModules = makeUnique<BuiltinModuleTable>(*this);
    objects = makeUnique<ObjectModel>(*this);
    coroutines = makeUnique<CoroutineTable>(*this);
    builtinFamilies = makeUnique<BuiltinFamilyState>(*this);
    caches->add(CacheKind::StrongRoots, "realm cells"_s, instance);
}

State::~State() = default;

State* State::create(Instance& instance)
{
    RELEASE_ASSERT(instance.vm->heap.isDeferred());
    RELEASE_ASSERT(instance.options().useWideReferences() || MarkedBlockCage::isRequired());
    return new State(instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
