/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/PageBlock.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class BuiltinCells;
class BuiltinModuleTable;
class CacheRegistry;
class ChainDependents;
class CoroutineTable;
class ImmortalCells;
class Modules;
class ProgramStatics;
class ProgramStrings;
class PropertyCaches;
struct BuiltinFamilyState;
struct ObjectModel;

class ZeroedPages {
    WTF_MAKE_NONCOPYABLE(ZeroedPages);
public:
    ZeroedPages() = default;
    ~ZeroedPages();

    void* allocate(size_t bytes);

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
    std::unique_ptr<PropertyCaches> propertyCaches;
    std::unique_ptr<Modules> modules;
    std::unique_ptr<BuiltinModuleTable> builtinModules;
    std::unique_ptr<ObjectModel> objects;
    std::unique_ptr<CoroutineTable> coroutines;
    std::unique_ptr<BuiltinFamilyState> builtinFamilies;

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
