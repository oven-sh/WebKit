/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTVector.h"
#include <algorithm>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class AllocationCounts;
class ArrivalCounts;
class BuiltinCells;
class BuiltinModuleTable;
class CacheRegistry;
class ChainDependents;
class CoroutineTable;
class GlobalBindings;
class ImmortalCells;
class Modules;
class ProgramStatics;
class ProgramStrings;
class PropertyCaches;
class SiteCaches;
struct BuiltinFamilyState;
struct ObjectModel;

// Zeroed pages for what lives as long as a VM or the process. With ENABLE(AOT_GUARDED_PAGES) they lie between two pages that cannot be touched, on Linux at an address drawn for them alone.
void* reserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void* tryReserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void releaseGuardedPages(void* pages, size_t bytes);

class ZeroedPages {
    WTF_MAKE_NONCOPYABLE(ZeroedPages);
public:
    ZeroedPages() = default;
    ~ZeroedPages();

    // AllAtOnce is for a table whose every page is written right away: one call into the kernel takes the place of a fault per page.
    enum class FirstUse : uint8_t { PageByPage, AllAtOnce };
    void* allocate(size_t bytes, FirstUse = FirstUse::PageByPage);
    void release(void* pages);
    std::span<const std::pair<void*, size_t>> ranges() const { return m_ranges.span(); }

private:
    Vector<std::pair<void*, size_t>> m_ranges;
};

class ZeroedTableArena {
    WTF_MAKE_NONCOPYABLE(ZeroedTableArena);
public:
    ZeroedTableArena(ZeroedPages&, const ProgramTables&);

    template<typename T> T* allocate(size_t count);

private:
    ZeroedPages& m_pages;
    char* m_start { nullptr };
    size_t m_capacity { 0 };
    size_t m_used { 0 };
};

// Created at their first use, which in most programs is the first access that neither inline code nor a stub answers. A program without one never pays for them.
class LazyPropertyCaches {
    WTF_MAKE_NONCOPYABLE(LazyPropertyCaches);
public:
    explicit LazyPropertyCaches(State&);
    ~LazyPropertyCaches();

    PropertyCaches& operator*()
    {
        if (m_caches) [[likely]]
            return *m_caches;
        return create();
    }
    PropertyCaches* operator->() { return &**this; }
    PropertyCaches* getIfCreated() const { return m_caches.get(); }

private:
    PropertyCaches& create();

    State& m_state;
    std::unique_ptr<PropertyCaches> m_caches;
};

struct State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);
    WTF_MAKE_NONCOPYABLE(State);

    static State* create(Instance&);
    ~State();

    static State& of(JSGlobalObject* realm) { return *Instance::of(realm)->state; }

    Instance& instance;
    const ProgramTables& tables;
    ZeroedPages pages;
    ZeroedTableArena tableArena;
    std::unique_ptr<CacheRegistry> caches;
    std::unique_ptr<ImmortalCells> immortalCells;
    std::unique_ptr<ProgramStrings> strings;
    std::unique_ptr<ProgramStatics> statics;
    std::unique_ptr<BuiltinCells> builtinCells;
    std::unique_ptr<ChainDependents> chainDependents;
    std::unique_ptr<SiteCaches> siteCaches;
    LazyPropertyCaches propertyCaches;
    std::unique_ptr<Modules> modules;
    std::unique_ptr<BuiltinModuleTable> builtinModules;
    std::unique_ptr<ObjectModel> objects;
    std::unique_ptr<CoroutineTable> coroutines;
    std::unique_ptr<BuiltinFamilyState> builtinFamilies;
    std::unique_ptr<GlobalBindings> globalBindings;
    std::unique_ptr<ArrivalCounts> arrivalCounts;
    std::unique_ptr<AllocationCounts> allocationCounts;
    // What registerWrittenAtFaults held at the last null check that faulted. The handler may run on another thread than the one that faulted.
    uint64_t registerBeforeFault { 0 };

private:
    explicit State(Instance&);
};

template<typename T>
T* ZeroedTableArena::allocate(size_t count)
{
    constexpr size_t smallTableAlignment = 64;
    static_assert(alignof(T) <= smallTableAlignment);
    static_assert(std::is_trivially_default_constructible_v<T>);
    size_t size = std::max<size_t>(count, 1) * sizeof(T);
    size_t start = roundUpToMultipleOf(size < pageSize() ? smallTableAlignment : pageSize(), m_used);
    if (start + size > m_capacity)
        return static_cast<T*>(m_pages.allocate(size));
    m_used = start + size;
    return reinterpret_cast<T*>(m_start + start);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
