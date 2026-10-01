/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTRuntime.h"
#include "Allocator.h"
#include "PropertyOffset.h"

namespace JSC {

class Identifier;
class PropertySlot;
class PutPropertySlot;
class Structure;

namespace AOT {

// Where a property is, the way get_by_id and put_by_id caches have it, which spares the code a branch: in words, as a signed number
// of Slot::offsetBits bits, from the start of the object if it is not negative and from the object's butterfly if it is.
std::optional<uint32_t> locationOfProperty(PropertyOffset);

// What the operations behind property accesses do once they have their answer: leave it in the slot for the code to find next
// time, if it is an answer that holds for every object of the base's structure.

//     cache->structureID: the structure of the base. cache->offset: the location of the property, and what pointer is (see Slot).
//     cache->pointer: null if that is the base itself. Else the object on the prototype chain that has the property; or, if none
//     has, something that looks enough like an object that has undefined there.
// structureBefore: the base's, if it is a cell, before the property was looked up: getting it may have run anything.
// Says why not, if it did not.
// mayBePolymorphic: whoever reads the slot knows what to make of Slot::isPolymorphic().
void cacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache, bool mayBePolymorphic = false);

//     cache->structureID: the structure of the base. cache->pointer: the private name, or the brand.
//     cache->offset: the location of the field, if this is about one.
void noteCustomGetter(JSGlobalObject*, Instance&, JSObject* base, const Identifier&, const PropertySlot&); // Instance::customGetters
void cachePrivateName(VM&, Data*, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset>);

// A slot that rests on what the objects on the prototype chain have, or do not have, is told when that changes by the structures
// they have now. A structure can only tell if nothing has ever moved on from it, and an object that has next to nothing in it, as
// many a prototype does, shares its structure with others that have. There is only one of a prototype: it gets a structure of its own.
void makePrototypeChainWatchable(VM&, JSCell* base);

// A site's slot is good for one structure. Behind it, for the sites that see many, is the VM's megamorphic cache, which the
// thunks in front of the operations consult (AOTThunks.cpp) and which these fill.
JSValue getByIdAndFillMegamorphicCache(JSGlobalObject*, JSValue base, const Identifier&, PropertySlot&); // base.get(), in effect.
void fillMegamorphicCacheAfterPut(JSGlobalObject*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&);

//     cache->structureID: the structure of the base. cache->offset: the location of the property.
//     cache->newStructureID: the structure the base has afterwards, if the property is new.
void cachePutById(JSGlobalObject*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);

// For a site that allocates cells of one structure and size. Two slots.
//     cache[0].structureID: the structure. cache[0].offset, pointer: whatever else the site wants to have at hand.
//     cache[1].offset: the other half of a new cell's header. cache[1].pointer: the allocator.
// cache[0]: the callee. cache[1]: its CodeBlock. See Lowering::lowerCallToKnownFunction().
// cache[0] and cache[1]: as for fillAllocationCache(), for the structure the object ends up with, and for that callee only.
// cache[2]: the structure that the callee has to be making its instances from.
void fillConstructionCache(VM&, Data*, Slot* cache, JSFunction* callee, Structure* first, Structure* last, Allocator);
void fillAllocationCache(VM&, Data*, Slot* cache, Structure*, Allocator, uint32_t payload = 0, JSCell* extra = nullptr);
// cache->pointer: the object, for as long as the collector finds some other reason to keep it.
void cacheObjectOfSite(VM&, Data*, Slot* cache, JSObject*);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
