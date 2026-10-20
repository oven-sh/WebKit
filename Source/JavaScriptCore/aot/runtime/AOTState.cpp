/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTState.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinModules.h"
#include "AOTChainDependents.h"
#include "AOTCompileOptions.h"
#include "AOTCoroutineTable.h"
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

void* ZeroedPages::allocate(size_t bytes)
{
    size_t size = roundUpToMultipleOf(pageSize(), std::max<size_t>(bytes, 1));
    void* pages = OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(pages, size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    m_ranges.append({ pages, size });
    return pages;
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
    const Image& image = *Image::current();
    std::span propertyRuns = tables.propertyRuns();
    size_t numberOfPropertyRuns = propertyRuns.empty() ? 0 : propertyRuns[0] - 1;
    size_t capacity = sizeOfFixedSizeTables
        + sizeOfZeroedTable(tables.firstEnvironmentShape, sizeof(uint32_t))
        + sizeOfZeroedTable(tables.allocations().size() + 1, sizeof(AllocationSiteRecord) + sizeof(int32_t))
        + sizeOfZeroedTable(tables.numberOfClasses(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.functionKinds().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.literalStrings().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(image.text().numberOfIdentifiers(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.bigIntConstants().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(tables.builtinCells().size(), sizeof(JSCell*))
        + sizeOfZeroedTable(numberOfPropertyRuns, sizeof(const PropertyRun*))
        + sizeOfZeroedTable(tables.instanceSizes.numberOfSiteCaches, sizeof(SiteCacheState))
        + sizeOfZeroedTable(tables.modules().size(), sizeof(uint8_t))
        + sizeOfZeroedTable(tables.requireEdges().size(), sizeof(uint8_t));
    if (image.header().options.useWideReferences())
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
{
    instance.state = this;
    caches = makeUnique<CacheRegistry>();
    immortalCells = makeUnique<ImmortalCells>(*this);
    strings = makeUnique<ProgramStrings>(*this);
    statics = makeUnique<ProgramStatics>(*this);
    builtinCells = makeUnique<BuiltinCells>(*this);
    chainDependents = makeUnique<ChainDependents>(*this);
    propertyCaches = makeUnique<PropertyCaches>(*this);
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
