/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTVector.h"
#include "StructureID.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMalloc.h>

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
class FirstAdditions;
class SiteCaches;
class StringKeyCache;
struct BuiltinFamilyState;
struct ObjectModel;

// Zeroed pages for data that lives as long as the process. With ENABLE(AOT_GUARDED_PAGES) they sit between two inaccessible pages, on Linux at their own random address.
void* reserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void* tryReserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void releaseGuardedPages(void* pages, size_t bytes);

// Zeroed pages for data that lives no longer than a VM. What the VM takes at startup are pieces of one mapping, which is unmapped in one call when the VM ends,
// so threads that start or end together make one system call each. What does not fit gets its own pages. With ENABLE(AOT_GUARDED_PAGES) everything does.
class PagesOfVM {
    WTF_MAKE_TZONE_ALLOCATED(PagesOfVM);
    WTF_MAKE_NONCOPYABLE(PagesOfVM);
public:
    explicit PagesOfVM(size_t sizeOfMapping);
    ~PagesOfVM();

    void* allocate(size_t bytes);
    void* tryAllocate(size_t bytes);
    void release(void* pages, size_t bytes);
    // The system moved the pages elsewhere: nothing is mapped here any more.
    void didMoveAway(void* pages, size_t bytes);
    // From here on, a released piece stays mapped until the whole mapping is released, if the mapping is still whole.
    void willReleaseEverything() { m_releasesEverything = true; }
    std::span<const uint8_t> handedOut() const { return unsafeMakeSpan(m_start, m_used); }

private:
    bool contains(const void* pages) const { return std::bit_cast<uintptr_t>(pages) - std::bit_cast<uintptr_t>(m_start) < m_size; }

    uint8_t* m_start { nullptr };
    size_t m_size { 0 };
    size_t m_used { 0 };
    bool m_releasesEverything { false };
    // The pieces that were released early, unordered. A neighboring piece may have grown into one since.
    Vector<std::pair<uint8_t*, size_t>> m_gaps;
};

class ZeroedPages {
    WTF_MAKE_NONCOPYABLE(ZeroedPages);
public:
    explicit ZeroedPages(PagesOfVM& pagesOfVM)
        : m_pagesOfVM(pagesOfVM)
    {
    }
    ~ZeroedPages();

    // AllAtOnce is for a table whose every page is written right away: one call into the kernel takes the place of a fault per page.
    enum class FirstUse : uint8_t { PageByPage, AllAtOnce };
    void* allocate(size_t bytes, FirstUse = FirstUse::PageByPage);
    void release(void* pages);
    std::span<const std::pair<void*, size_t>> ranges() const { return m_ranges.span(); }

private:
    PagesOfVM& m_pagesOfVM;
    Vector<std::pair<void*, size_t>> m_ranges;
};

class ZeroedTableArena {
    WTF_MAKE_NONCOPYABLE(ZeroedTableArena);
public:
    ZeroedTableArena(ZeroedPages&, const ProgramTables&);
    ZeroedTableArena(ZeroedPages&, size_t capacity);

    template<typename T> T* allocate(size_t count);
    std::span<const char> handedOut() const { return unsafeMakeSpan(m_start, m_used); }

private:
    ZeroedPages& m_pages;
    char* m_start { nullptr };
    size_t m_capacity { 0 };
    size_t m_used { 0 };
};

// A table that is made at its first use. Its pages are set aside with the tables of the program, and a program that never makes it never touches them.
// The first use may come where no more memory can be mapped, and what reads a property or copies an object has no way to report that.
template<typename T>
class MadeInArena {
    WTF_MAKE_NONCOPYABLE(MadeInArena);
public:
    struct alignas(64) Storage {
        static_assert(alignof(T) <= 64);
        uint8_t bytes[sizeof(T)];
    };

    explicit MadeInArena(ZeroedTableArena& arena)
        : m_storage(arena.allocate<Storage>(1))
    {
    }

    ~MadeInArena()
    {
        if (m_value)
            std::destroy_at(m_value);
    }

    explicit operator bool() const { return m_value; }
    T& operator*() const { return *m_value; }
    T* operator->() const { return m_value; }
    T* get() const { return m_value; }

    template<typename... Arguments>
    T& make(Arguments&&... arguments)
    {
        ASSERT(!m_value);
        m_value = new (NotNull, m_storage) T(std::forward<Arguments>(arguments)...);
        return *m_value;
    }

private:
    Storage* m_storage;
    T* m_value { nullptr };
};

// Created at its first use. A program without one never pays for it.
template<typename T>
class CreatedAtFirstUse {
    WTF_MAKE_NONCOPYABLE(CreatedAtFirstUse);
public:
    explicit CreatedAtFirstUse(State&);
    ~CreatedAtFirstUse();

    T& operator*()
    {
        if (m_value) [[likely]]
            return *m_value;
        return create();
    }
    T* operator->() { return &**this; }
    T* getIfCreated() const { return m_value.get(); }

private:
    T& create();

    State& m_state;
    MadeInArena<T> m_value;
};

extern template class CreatedAtFirstUse<PropertyCaches>;
extern template class CreatedAtFirstUse<StringKeyCache>;

// A compiled object right below a locked Object.prototype, as it was born, has one of the two traps only in the slot of its field. A field is null where the layout has none of the name.
struct HandlerKnownByItsSlots {
    StructureID structure;
    const JSObject* prototype { nullptr };
    const FieldData* fieldOfDefineProperty { nullptr };
    const FieldData* fieldOfGetOwnPropertyDescriptor { nullptr };
};

// The field of an object's layout that has the name of a field that a class declares. Layouts and their fields are data of the image, so what is remembered never goes stale.
struct FieldUnderDeclaredName {
    const LayoutData* layoutOfObject;
    const FieldData* declared;
    const FieldData* field;
};

struct State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);
    WTF_MAKE_NONCOPYABLE(State);

    static State* create(Instance&);
    ~State();

    static State& of(JSGlobalObject* realm) { return *Instance::of(realm)->state; }

    Instance& instance;
    const ProgramTables& tables;
    // The realm runs other images beside the one that these tables are of: a compiled object or a closure of the realm need not be theirs.
    const bool sharesRealm;
    ZeroedPages pages;
    ZeroedTableArena tableArena;
    std::unique_ptr<CacheRegistry> caches;
    std::unique_ptr<ImmortalCells> immortalCells;
    std::unique_ptr<ProgramStrings> strings;
    std::unique_ptr<ProgramStatics> statics;
    std::unique_ptr<BuiltinCells> builtinCells;
    std::unique_ptr<ChainDependents> chainDependents;
    std::unique_ptr<SiteCaches> siteCaches;
    CreatedAtFirstUse<PropertyCaches> propertyCaches;
    CreatedAtFirstUse<StringKeyCache> stringKeys;
    // Created when an object is first given a name that is new to its Structure.
    std::unique_ptr<FirstAdditions> firstAdditions;
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
    // Where the last Throw at the end of a cleanup returns to. No other throw returns there.
    const void* returnAddressOfThrowAfterCleanup { nullptr };
    // What that cleanup has kept of the throw that entered it: the Exception, or where a `throw` of the same function returns to, or zero. It is read only between that Throw and the search for its handler.
    uint64_t keptByCleanup { 0 };
    static constexpr unsigned numberOfHandlersKnownByTheirSlots = 8;
    std::array<HandlerKnownByItsSlots, numberOfHandlersKnownByTheirSlots> handlersKnownByTheirSlots { };
    static constexpr size_t numberOfFieldsUnderDeclaredNames = 64;
    // Allocated when a private name is first looked for in an object of another layout than the one that declares it.
    FieldUnderDeclaredName* fieldsUnderDeclaredNames { nullptr };

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
