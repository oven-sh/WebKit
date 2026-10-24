/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCaches.h"
#include "Identifier.h"
#include <optional>

namespace JSC { namespace AOT {

struct NamedAccess {
    Instance* instance;
    JSGlobalObject* globalObject;
    VM& vm;
    PropertyCaches& caches;
    Identifier ident;
    std::optional<uint32_t> number;
    SiteCache site;
};

// The base is neither undefined nor null.
JSValue getAndCache(const NamedAccess&, JSValue base);
void putAndCache(const NamedAccess&, JSCell* base, JSValue, PutMode);
bool hasPropertyAndCache(Instance*, JSObject*, const CachedName&);
// Class objects that nobody has looked into share a Structure although they declare different names, so no slot answers for one, be it the receiver or an object above it.
void createClassObjectsOnChain(VM&, JSCell* receiver, UniquedStringImpl* nameAskedFor);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
