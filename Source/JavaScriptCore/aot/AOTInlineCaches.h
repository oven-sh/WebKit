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
void tellNamesOfRow(VM&, Structure*, std::span<UniquedStringImpl* const> names);

void cacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache, bool mayBePolymorphic = false);

void noteCustomGetter(JSGlobalObject*, Instance&, JSObject* base, const Identifier&, const PropertySlot&);
void cacheKnownCustomGetter(VM&, Instance&, Data*, Slot*, Structure*, const Identifier&, const Instance::CustomGetter&);
void noteInheritedSetter(JSGlobalObject*, Instance&, JSObject* base, const Identifier&, const PutPropertySlot&);
void cachePrivateName(VM&, Data*, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset>);
void cachePrivateNameTransition(VM&, Data*, Slot* cache, JSObject* base, Structure* oldStructure, JSValue name, std::optional<PropertyOffset>);
// After a miss of GetByValCached or PutByValCached: an entry of Instance::keyedReads or keyedStores. No offset: the pair is not for the table.
void cacheKeyedAccess(Slot& entry, Structure*, JSCell* key, std::optional<PropertyOffset>);
// Behind that, where it was told that the pair is not for the table and the object is of a compiled layout: if the name is that of a field, the entry says where it is. See Instance::keyIsCompiledField.
void cacheKeyedReadOfCompiledField(JSGlobalObject*, Instance::KeyedRead&, JSObject*, const Identifier&);
// After a read by name that Typed::Object::tryGetOwnData() has answered. See Slot::compiledFieldFlags.
void cacheCompiledField(VM&, Data*, Slot* cache, JSCell* object, const Identifier&, bool mayBePolymorphic);
// See Instance::NamedField. `name`: the number of the identifier in the program.
void noteNamedFieldRead(Instance*, JSCell* object, const Identifier&, uint32_t name);
void noteNamedFieldWrite(Instance*, JSCell* object, const Identifier&, uint32_t name);
// After a miss of DeleteByValCached, the property being gone. Two slots. No offset: what has happened is not for them.
void cacheKeyedDelete(VM&, Data*, Slot* cache, JSObject* base, Structure* oldStructure, JSValue key, std::optional<PropertyOffset>);
void cacheInstanceOf(JSGlobalObject*, Data*, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype);

void makePrototypeChainWatchable(VM&, JSCell* base);

JSValue getByIdAndFillMegamorphicCache(JSGlobalObject*, JSValue base, const Identifier&, PropertySlot&);
bool hasPropertyAndFillMegamorphicCache(JSGlobalObject*, JSObject* base, UniquedStringImpl*, bool& goesByStructure);
bool flattenOnce(VM&, JSObject*);
bool isHostObjectThatOverridesPut(Structure*);
void fillMegamorphicCacheAfterPut(JSGlobalObject*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&);

void cachePutById(Instance*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);
// After Typed::State::storeByKnownTransition() has taken a store at the site. `wordsBefore`: Typed::Object::orderAndShapeWords() as it was.
void cacheStoreTransition(Instance*, Data*, JSCell* object, Structure* oldStructure, uint64_t wordsBefore, const Identifier&, Slot* cache);

void fillConstructionCache(VM&, Data*, Slot* cache, JSFunction* callee, Structure* first, Structure* last, Allocator);
void fillAllocationCache(VM&, Data*, Slot* cache, Structure*, Allocator, uint32_t payload = 0, JSCell* extra = nullptr);
void fillFunctionAllocationCache(VM&, Data*, Slot* cache, Structure*, Allocator, uintptr_t functionWord);
void cacheSiteObject(VM&, Data*, Slot* cache, JSObject*);
bool tryGetFromOrdinaryObject(JSGlobalObject*, JSValue base, UniquedStringImpl*, PropertySlot&, JSValue& result, const Slot* site);
void readCacheFirstSwitch();
const EncodedJSValue* undefinedValueHolder();
void readFirstTrySwitch();
// After Object.defineProperty(object, name, { value: .., flags that are constants }) has been carried out on an object that was of the structure. `absent`: those of the six names of a descriptor that the literal has not.
// The slot: as of a store that adds the property; or, without a structure to go to, the place of a property that can be neither changed nor deleted. See Stub::DefineOwnProperty.
void cacheDefinedProperty(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, UniquedStringImpl*, std::span<UniquedStringImpl* const> absent);
bool watchStores(JSGlobalObject*, Data*, Slot*, JSObject*, Structure*, std::span<UniquedStringImpl* const> names);
void cacheRowGivenToThis(JSGlobalObject*, Data*, Slot*, JSObject*, Structure* empty, Structure* whole, std::span<UniquedStringImpl* const> names);
bool tryStoreIntoRowOfThis(JSGlobalObject*, Data*, Slot*, JSValue base, const Identifier&, JSValue);
// After operationAOTTypedTryPutProperties has given the object, which was of the structure, properties of the names, all of them in the object itself, one behind the other.
void cachePropertyRun(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, std::span<UniquedStringImpl* const> names, std::span<JSCell* const> keys);
// The same for properties of which some lie in the storage out of line. Slot::offset: the place of the first in the object, and in the bits of a name's number how many go there. Slot::fieldType: see below.
// Whether the slot is filled. See Stub::PutPropertiesWithStorage.
struct PropertyRunStorage {
    static constexpr uint32_t capacityMask = (1u << 10) - 1;
    static constexpr uint32_t isAllocated = 1u << 10;
    static constexpr unsigned firstShift = 11;
    static constexpr uint32_t maxFirst = (1u << 10) - 1;
};
bool cachePropertyRunWithStorage(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, std::span<UniquedStringImpl* const> names, bool isAllocated);
bool cachePropertyRunOfReplacements(VM&, Data*, Slot* cache, Structure*, std::span<const PropertyOffset>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
