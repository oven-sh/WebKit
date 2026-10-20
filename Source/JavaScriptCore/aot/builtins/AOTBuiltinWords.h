/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <bit>
#include <cstdint>
#include <type_traits>
#include <wtf/StdLibExtras.h>

namespace JSC {

class JSCell;

namespace AOT {

struct ObjectCreateCacheEntry {
    JSCell* prototype;
    uint64_t header;
};

struct ObjectBuiltinWords {
    static constexpr unsigned objectCreateSize = 64;
    static constexpr unsigned objectCreateShift = 6;
    static constexpr unsigned objectCreateIndex(uintptr_t prototype) { return (prototype >> objectCreateShift) & (objectCreateSize - 1); }

    ObjectCreateCacheEntry objectCreate[objectCreateSize];
};

struct BuiltinWords {
    ObjectBuiltinWords objects;
};

static_assert(sizeof(ObjectCreateCacheEntry) == 16);
static_assert(!OBJECT_OFFSETOF(ObjectCreateCacheEntry, prototype));
static_assert(std::has_single_bit(ObjectBuiltinWords::objectCreateSize));
static_assert(sizeof(ObjectBuiltinWords) <= 1 * KB);
static_assert(std::is_trivially_default_constructible_v<BuiltinWords> && std::is_standard_layout_v<BuiltinWords>);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
