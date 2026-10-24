/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
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

struct BuiltinDirectFunctionBinding;
struct BuiltinNativeFunctionBinding;

struct BuiltinWords {
    // By RealmStructureID. An own slot whose Structure has a higher number is read by name.
    static constexpr unsigned numberOfOwnSlotStructures = 16;

    ObjectBuiltinWords objects;
    // One WatchedLocationState for each entry of the table of built-ins, zero until compiled code asks about the entry (AOTBuiltinLocationWatch.h).
    uint8_t* locationStates;
    // What the host made at the first read of a member, by the number of the member's value cell. Empty until then.
    EncodedJSValue* hostValues;
    // The number of the Structure with which an instance holds an own slot at its place. Zero: nobody has looked for it.
    uint32_t ownSlotStructureIDs[numberOfOwnSlotStructures];
};

struct BuiltinBindingWords {
    // BuiltinBindings::directFunctions() of the host.
    const BuiltinDirectFunctionBinding* directFunctions;
    // BuiltinBindings::nativeFunctionsOfHost().
    const BuiltinNativeFunctionBinding* nativeFunctionsOfHost;
};

static_assert(sizeof(ObjectCreateCacheEntry) == 16);
static_assert(!OBJECT_OFFSETOF(ObjectCreateCacheEntry, prototype));
static_assert(std::has_single_bit(ObjectBuiltinWords::objectCreateSize));
static_assert(sizeof(ObjectBuiltinWords) <= 1 * KB);
static_assert(std::is_trivially_default_constructible_v<BuiltinWords> && std::is_standard_layout_v<BuiltinWords>);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
