/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPrefetchStep.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

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
class StringKeyCache;
struct BuiltinFamilyState;
struct ObjectModel;

void* reserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void* tryReserveGuardedPages(size_t bytes, OSAllocator::Usage = OSAllocator::FastMallocPages);
void releaseGuardedPages(void* pages, size_t bytes);

class PagesOfVM {
    WTF_MAKE_TZONE_ALLOCATED(PagesOfVM);
    WTF_MAKE_NONCOPYABLE(PagesOfVM);
public:
    explicit PagesOfVM(size_t sizeOfMapping);
    ~PagesOfVM();

    void* allocate(size_t bytes);
    void* tryAllocate(size_t bytes);
    void release(void* pages, size_t bytes);
    void didMoveAway(void* pages, size_t bytes);
    void willReleaseEverything() { m_releasesEverything = true; }

private:
    bool contains(const void* pages) const { return std::bit_cast<uintptr_t>(pages) - std::bit_cast<uintptr_t>(m_start) < m_size; }

    uint8_t* m_start { nullptr };
    size_t m_size { 0 };
    size_t m_used { 0 };
    bool m_releasesEverything { false };
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

private:
    ZeroedPages& m_pages;
    char* m_start { nullptr };
    size_t m_capacity { 0 };
    size_t m_used { 0 };
};

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

    T& makeFromZeros()
    {
        ASSERT(!m_value);
        m_value = std::launder(reinterpret_cast<T*>(m_storage));
        return *m_value;
    }

private:
    Storage* m_storage;
    T* m_value { nullptr };
};

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

struct FieldUnderDeclaredName {
    const LayoutData* layoutOfObject;
    const FieldData* declared;
    const FieldData* field;
};

namespace Typed {
enum class CountInEffectsNet : uint8_t;
}

struct GrowingFlatText {
    JSString* buffer;
    const StringImpl* lastResolved;
    uint32_t consecutiveResolves;
};

struct WideCharacterStrings {
    static constexpr unsigned size = 256;

    std::array<JSString*, size> strings;
    std::array<char16_t, size> characters;
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
    GrowingFlatText growingFlatText { };
    static constexpr unsigned maxPrefetchingLoops = 4;
    struct PrefetchingLoop {
        const JSCell* array;
        uint32_t request;
        uint64_t nextPosition;
    };
    using PrefetchedRows = std::array<std::array<const JSCell*, PrefetchStep::maxSlots>, PrefetchStep::maxRows>;
    std::array<PrefetchingLoop, maxPrefetchingLoops> prefetchingLoops { };
    std::unique_ptr<std::array<PrefetchedRows, maxPrefetchingLoops>> prefetchedRows;
    WideCharacterStrings wideCharacterStrings { };
    std::array<JSString*, 64> textsOfFewStrings { };
    std::unique_ptr<ImmortalCells> immortalCells;
    JSObject* objectThatHoldsUndefined { nullptr };
    std::unique_ptr<ProgramStrings> strings;
    std::unique_ptr<ProgramStatics> statics;
    std::unique_ptr<BuiltinCells> builtinCells;
    std::unique_ptr<ChainDependents> chainDependents;
    CreatedAtFirstUse<PropertyCaches> propertyCaches;
    CreatedAtFirstUse<StringKeyCache> stringKeys;
    std::unique_ptr<FirstAdditions> firstAdditions;
    std::unique_ptr<Modules> modules;
    std::unique_ptr<BuiltinModuleTable> builtinModules;
    std::unique_ptr<ObjectModel> objects;
    std::unique_ptr<CoroutineTable> coroutines;
    std::unique_ptr<BuiltinFamilyState> builtinFamilies;
    std::unique_ptr<GlobalBindings> globalBindings;
    uint64_t registerBeforeFault { 0 };
    const void* returnAddressOfThrowAfterCleanup { nullptr };
    uint64_t keptByCleanup { 0 };
    static constexpr size_t numberOfFieldsUnderDeclaredNames = 64;
    FieldUnderDeclaredName* fieldsUnderDeclaredNames { nullptr };
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsEnteredThatRunOnce;
    std::array<uint64_t, 8> writesCountedForEffectsNet { };
    EncodedJSValue receiverOfPendingRead { 0 };
    uint32_t keyOfPendingRead { 0 };
    uint32_t slowPathReadCountAtPendingRead { 0 };
    bool acceptsForeignBinaryData { false };
    bool disallowsEvaluation { false };
    void countWriteForEffectsNet(Typed::CountInEffectsNet count) { ++writesCountedForEffectsNet[static_cast<size_t>(count)]; }

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
