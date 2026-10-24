/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "Identifier.h"
#include "JSCJSValue.h"
#include <optional>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class VM;

namespace AOT {

enum class KeyAtomization : bool { OnlyExisting, Atomize };
enum class KeyNameKind : uint8_t { NotAName, HasNoAtom, Index, Name };

struct KeyName {
    KeyNameKind kind { KeyNameKind::NotAName };
    UniquedStringImpl* uid { nullptr };
    uint32_t index { 0 };
};

KeyName nameOfKeyWithoutAllocating(JSGlobalObject*, JSCell* key, KeyAtomization);
std::optional<uint32_t> indexOfKey(JSValue key);
std::optional<Identifier> nameForKeyedCache(JSGlobalObject*, JSValue key);
UniquedStringImpl* atomOfKey(JSValue key);
UniquedStringImpl* uidOfKey(JSCell* key);
std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes = true);
bool hasOnlyOrdinaryLookupsOnChain(JSObject* base);

// A leaf operation cannot collect: a store by key makes a Structure only out of the block that the allocator holds.
bool hasStructureAtHand(VM&);

struct NamedAccess {
    CachedName name() const { return { ident.impl(), number }; }
    PropertyCaches& caches() const { return *instance->state->propertyCaches; }

    Instance* instance;
    Identifier ident;
    std::optional<uint32_t> number;
    SiteCache site;
};

// The base is neither undefined nor null.
JSValue getAndCache(const NamedAccess&, JSValue base);
void putAndCache(const NamedAccess&, JSCell* base, JSValue, PutMode);
bool hasPropertyAndCache(Instance*, JSObject*, const CachedName&);
bool instanceOfAndCache(Instance*, JSValue, JSValue constructor);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
