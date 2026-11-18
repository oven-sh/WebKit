/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class JSString;

namespace AOT {

struct State;

struct DoubleStringCache {
    static constexpr unsigned logOfNumberOfEntries = 6;
    static constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;

    struct Entry {
        EncodedJSValue number;
        JSString* string;
    };
    static constexpr unsigned logOfEntrySize = 4;
    static_assert(sizeof(Entry) == 1u << logOfEntrySize);

    static DoubleStringCache& ensure(State&);

    static constexpr size_t indexOf(EncodedJSValue number) { return static_cast<uint64_t>(number) * multiplier >> (64 - logOfNumberOfEntries); }
    void remember(EncodedJSValue number, JSString* string) { entries[indexOf(number)] = { number, string }; }

    Entry entries[size_t { 1 } << logOfNumberOfEntries];
};
static_assert(std::is_trivially_default_constructible_v<DoubleStringCache>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
