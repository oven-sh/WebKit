/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "Allocator.h"
#include "JSArray.h"
#include "PropertyOffset.h"

namespace JSC {

class Identifier;
class PropertySlot;
class PutPropertySlot;
class Structure;

namespace AOT {

std::optional<uint32_t> propertyLocation(PropertyOffset);
void setInlineSlotPropertyNameIDs(VM&, Structure*, std::span<UniquedStringImpl* const> names);

void cacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache, bool mayBePolymorphic = false);

void cacheCustomGetter(JSGlobalObject*, Instance&, JSObject* base, const Identifier&, const PropertySlot&);
void cacheKnownCustomGetter(VM&, Instance&, Data*, Slot*, Structure*, const Identifier&, const Instance::CustomGetter&);
std::optional<PropertyOffset> inheritedSetterOffset(JSGlobalObject*, JSObject* base, Structure* structureBeforeStore, const Identifier&, const PutPropertySlot&);
void cacheInheritedSetter(JSGlobalObject*, Instance&, JSObject* base, Structure* structureBeforeStore, const Identifier&, const PutPropertySlot&);
bool cacheNativeSetter(JSGlobalObject*, Instance&, JSObject* base, Structure* structureBeforeStore, const Identifier&, const PutPropertySlot&);
void cachePrivateNameTransition(VM&, Data*, Slot* cache, JSObject* base, Structure* oldStructure, JSValue name, std::optional<PropertyOffset>);
void cacheKeyedAccess(Slot& entry, Structure*, JSCell* key, std::optional<PropertyOffset>);
enum class StorageAfterAddition : uint8_t { Unsupported, Stays, IsFirst, Doubled };
bool hasOnlyNamedOutOfLineStorage(Structure* before, Structure* after);
StorageAfterAddition storageAfterAddition(Structure* before, Structure* after, PropertyOffset);
void cacheKeyedDefinition(Instance*, VM&, Structure* before, Structure* after, JSCell* key, UniquedStringImpl*, unsigned definedAttributes = 0, uintptr_t keyTagBits = 0);
void cacheKeyedExtraPropertyAddition(Instance*, Structure* before, Structure* after, JSCell* key, PropertyOffset, bool createsExtraProperties);
void recordKeyedExtraPropertyAddition(Instance::KeyedAddition&, Structure* before, JSCell* key);
void cacheKeyedCompiledFieldRead(JSGlobalObject*, Instance::KeyedRead&, JSObject*, const Identifier&);
void cacheCompiledField(VM&, Data*, Slot* cache, JSCell* object, const Identifier&, bool mayBePolymorphic);
void cacheNamedFieldRead(Instance*, JSCell* object, const Identifier&, uint32_t name);
void cacheNamedFieldWrite(Instance*, JSCell* object, const Identifier&, uint32_t name);
void cacheKeyedCompiledFieldStore(Instance*, JSCell* object, JSCell* key, const Identifier&);
void cacheKeyedDelete(VM&, Data*, Slot* cache, JSObject* base, Structure* oldStructure, JSValue key, std::optional<PropertyOffset>);
void cacheKeyedDeletion(Instance*, Structure* before, Structure* after, JSValue key, UniquedStringImpl*, const DeletePropertySlot&);
void cacheInstanceOf(JSGlobalObject*, Data*, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype);
void cacheHasInstance(JSGlobalObject*, Data*, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, bool isCalled);

void makePrototypeChainWatchable(VM&, JSCell* base);

JSValue getByIdAndFillMegamorphicCache(JSGlobalObject*, JSValue base, const Identifier&, PropertySlot&);
bool hasPropertyAndFillMegamorphicCache(JSGlobalObject*, JSObject* base, UniquedStringImpl*, bool& dependsOnlyOnStructure, bool* isPermanent = nullptr);
bool flattenOnce(VM&, JSObject*);
std::optional<UniquedStringImpl*> findNameEqualToLongString(JSValue base, JSValue key, bool checksPrototypes = true);
bool findOwnPropertyGeneric(JSValue base, UniquedStringImpl*, PropertyOffset&, unsigned& attributes);
bool findOwnPropertyByKeyGeneric(Instance*, JSValue base, JSValue key, UniquedStringImpl*&, PropertyOffset&, unsigned& attributes);
bool isHostObjectOverridingPut(Structure*);
void fillMegamorphicCacheAfterPut(JSGlobalObject*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&);

void cachePutById(Instance*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);
void cacheStoreTransition(Instance*, Data*, JSCell* object, Structure* oldStructure, uint64_t wordsBefore, const Identifier&, Slot* cache);
void cacheKnownStoreTransition(Instance*, JSCell* object, Structure* oldStructure, uint64_t wordsBefore, const Identifier&, uint32_t name);
bool cacheFieldCreation(Instance*, Data*, JSCell* object, uint64_t wordsBefore, const Identifier&, Slot* cache, bool checksPrototypeChain);
void recordFieldCreation(Instance*, JSCell* object, uint64_t wordsBefore, const Identifier&);

void fillAllocationCache(VM&, Data*, Slot* cache, Structure*, Allocator, uint32_t payload = 0, JSCell* extra = nullptr);
bool tryGetFromOrdinaryObject(JSGlobalObject*, JSValue base, UniquedStringImpl*, PropertySlot&, JSValue& result, bool fillsMegamorphicCache = true, bool* structureGuaranteesAbsent = nullptr);
ALWAYS_INLINE bool isExactlyJSArray(JSCell* cell) { return cell->type() == ArrayType || (cell->type() == DerivedArrayType && cell->classInfo() == JSArray::info()); }
void cacheGetterShortcut(GetterSetter*);
bool storeMayBeCachedByStructureAndName(VM&, Structure*, UniquedStringImpl*);
bool storesAreOrdinaryAlongPrototypeChain(Structure*);
const EncodedJSValue* undefinedValueHolder();
void cacheDefinedProperty(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, UniquedStringImpl*, std::span<UniquedStringImpl* const> absent);
bool watchStores(JSGlobalObject*, Data*, Slot*, JSObject*, Structure*, std::span<UniquedStringImpl* const> names);
void cacheAssignedThisStructure(JSGlobalObject*, Data*, Slot*, JSObject*, Structure* empty, Structure* whole, std::span<UniquedStringImpl* const> names);
bool tryStoreIntoThisStructure(JSGlobalObject*, Data*, Slot*, JSValue base, const Identifier&, JSValue);
void cachePropertyRun(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, std::span<UniquedStringImpl* const> names, std::span<JSCell* const> keys);
struct PropertyRunStorage {
    static constexpr uint32_t capacityMask = (1u << 10) - 1;
    static constexpr uint32_t isAllocated = 1u << 10;
    static constexpr unsigned firstShift = 11;
    static constexpr uint32_t maxFirst = (1u << 10) - 1;
};
bool cachePropertyRunWithStorage(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, std::span<UniquedStringImpl* const> names, bool isAllocated);
bool cacheReplacementPropertyRun(VM&, Data*, Slot* cache, Structure*, std::span<const PropertyOffset>);
void cacheContiguousReplacementPropertyRun(VM&, Data*, Slot* cache, Structure*, PropertyOffset first, unsigned count, std::span<JSCell* const> keys);
bool cachePropertyRunByOffsets(JSGlobalObject*, Data*, Slot* cache, JSObject*, Structure* before, std::span<const PropertyOffset>, std::span<UniquedStringImpl* const> added);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
