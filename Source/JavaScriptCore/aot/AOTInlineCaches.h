/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "Allocator.h"
#include "PropertyOffset.h"

namespace JSC {

class Identifier;
class PropertySlot;
class PutPropertySlot;
class Structure;

namespace AOT {

std::optional<uint32_t> propertyLocation(PropertyOffset);

void cacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache, bool mayBePolymorphic = false);

void noteCustomGetter(JSGlobalObject*, Instance&, JSObject* base, const Identifier&, const PropertySlot&);
void noteInheritedSetter(JSGlobalObject*, Instance&, JSObject* base, const Identifier&, const PutPropertySlot&);
void cachePrivateName(VM&, Data*, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset>);
void cachePrivateNameTransition(VM&, Data*, Slot* cache, JSObject* base, Structure* oldStructure, JSValue name, std::optional<PropertyOffset>);
void cacheInstanceOf(JSGlobalObject*, Data*, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype);

void makePrototypeChainWatchable(VM&, JSCell* base);

JSValue getByIdAndFillMegamorphicCache(JSGlobalObject*, JSValue base, const Identifier&, PropertySlot&);
void fillMegamorphicCacheAfterPut(JSGlobalObject*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&);

void cachePutById(Instance*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);

void fillConstructionCache(VM&, Data*, Slot* cache, JSFunction* callee, Structure* first, Structure* last, Allocator);
void fillAllocationCache(VM&, Data*, Slot* cache, Structure*, Allocator, uint32_t payload = 0, JSCell* extra = nullptr);
void cacheSiteObject(VM&, Data*, Slot* cache, JSObject*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
