/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCaches.h"

#if ENABLE(FTL_JIT)

#include "AOTOperationHelpers.h"
#include "AOTSlotWatchpoint.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "InlineCacheCompiler.h"
#include "JSCInlines.h"
#include "MegamorphicCache.h"
#include "ObjectPropertyConditionSet.h"
#include "StaticHeap.h"

namespace JSC { namespace AOT {

std::optional<uint32_t> locationOfProperty(PropertyOffset offset)
{
    int32_t location;
    if (isInlineOffset(offset))
        location = JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset;
    else
        location = -(offset - firstOutOfLineOffset) - 2;
    constexpr int32_t limit = 1 << (Slot::offsetBits - 1);
    if (location < -limit || location >= limit)
        return std::nullopt;
    return (static_cast<uint32_t>(location) & Slot::offsetMask) | (location < 0 ? Slot::isIntricate : 0);
}

// Read as an object whose first inline property is undefined.
static const EncodedJSValue holderOfUndefined[JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + 1] = { 0, 0, JSValue::ValueUndefined };
static_assert(JSObject::offsetOfInlineStorage() == 2 * sizeof(EncodedJSValue));

static void fill(VM& vm, Data* data, Slot* cache, Structure* structure, uint32_t offsetAndFlags, void* pointer)
{
    // The code reads the first word and then the second. In between it does nothing that lets this run, so all that matters is
    // that the collector, which reads them at any time, never sees a structure with a second word that is not its own.
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    if (offsetAndFlags & (Slot::isGetter | Slot::pointerIsCell | Slot::pointerIsNotCell))
        offsetAndFlags |= Slot::isIntricate;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    cache->offset = offsetAndFlags | attempts;
    cache->pointer = pointer;
    WTF::storeStoreFence();
    cache->structureID = structure->id();
    didFillSlot(vm, data);
    if (Options::aotVerbose()) [[unlikely]]
        dataLogLn("AOT: slot ", RawPointer(cache), " filled: structure ", RawPointer(structure), " id ", structure->id().bits(), " offset and flags ", RawHex(cache->offset), " pointer ", RawPointer(pointer));
}

void makePrototypeChainWatchable(VM& vm, JSCell* base)
{
    if (!base->isObject())
        return;
    for (JSValue next = asObject(base)->getPrototypeDirect(); next.isObject(); next = asObject(next)->getPrototypeDirect()) {
        JSObject* prototype = asObject(next);
        Structure* structure = prototype->structure();
        if (!structure->hasMonoProto())
            return;
        if (!structure->transitionWatchpointSetHasBeenInvalidated() || structure->isDictionary() || !structure->propertyAccessesAreCacheable() || StaticHeap::contains(prototype))
            continue;
        prototype->convertToDictionary(vm);
        prototype->flattenDictionaryObject(vm);
    }
}

// A slot is good for one structure. At a site that sees several, filling it again every time costs more than it saves (the collector
// has to be told, for one thing), and the megamorphic cache is there for those. So it is only done so often.
static bool mayReplace(Slot* cache, Structure* structure)
{
    if (!cache->structureID || cache->structureID == structure->id())
        return true;
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return false;
    cache->offset += 1u << Slot::attemptsShift;
    return true;
}

// Whatever the reason a slot could not be filled, a site where that goes on happening is one for the megamorphic cache.
static void countFailure(Slot* cache)
{
    if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
        cache->offset += 1u << Slot::attemptsShift;
}

static ASCIILiteral tryCacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache);

ASCIILiteral cacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache)
{
    if (SharedData::contains(cache))
        return "the function has no slots yet"_s;
    ASCIILiteral whyNot = tryCacheGetById(globalObject, data, base, structureBefore, ident, slot, cache);
    if (!whyNot.isEmpty())
        countFailure(cache);
    return whyNot;
}

static ASCIILiteral tryCacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache)
{
    if (Options::aotDisableFastPaths() & 1) [[unlikely]]
        return "disabled"_s;
    // Only the stubs know what to do about a getter.
    uint32_t getterFlag = usesStubs && slot.isCacheableGetter() ? Slot::isGetter : 0;
    if (!base.isCell() || (!slot.isCacheableValue() && !slot.isUnset() && !getterFlag))
        return "not a cell, or not a kind of property there is a cache for"_s;

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* structure = cell->structure();
    // What the slot says, it says of the object as it was.
    if (structure != structureBefore)
        return "the structure changed"_s;
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return "the structure is not cacheable"_s;
    if (getterFlag) {
        // And of whatever it was found in.
        unsigned attributes;
        if (slot.slotBase()->structure()->get(vm, ident.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
            return "the getter moved"_s;
        // Where the stub looks for the getter's code is filled in when a function is first called from code. This one may only ever
        // have been called from here.
        if (auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter())) {
            if (auto* executable = dynamicDowncast<FunctionExecutable>(function->executable()); executable && executable->isGeneratedForCall())
                executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
        }
    }

    if (!slot.isUnset() && slot.slotBase() == cell) {
        if (structure->isDictionary()) {
            // Next time, if it has settled down. Once: an object that goes on to be one again is being used as one.
            if (!structure->hasBeenFlattenedBefore() && cell->isObject())
                structure->flattenDictionaryStructure(vm, asObject(cell));
            return "own, in a dictionary"_s;
        }
        auto location = locationOfProperty(slot.cachedOffset());
        if (!location)
            return "own, too far"_s;
        if (!mayReplace(cache, structure))
            return "sees too many structures"_s;
        if (cache->hasPointer())
            stopWatching(data, cache);
        fill(vm, data, cache, structure, *location | getterFlag, nullptr);
        return ""_s;
    }

    // What follows takes some doing, and undoing. It is for the sites that see few structures, and not for code that runs once.
    if (Options::aotDisableFastPaths() & 256) [[unlikely]]
        return "disabled"_s;
    uint32_t attempts = (cache->offset & Slot::attemptsMask) >> Slot::attemptsShift;
    if (attempts == Slot::maxAttempts)
        return "gave up"_s;
    // A site that sees one structure needs two. One that is still at it after a few sees several, and each time costs a good deal
    // more than looking in the megamorphic cache ever will.
    constexpr uint32_t maxAttemptsAtThis = 4;
    if (attempts >= maxAttemptsAtThis) {
        cache->offset |= Slot::attemptsMask;
        return "gave up"_s;
    }
    cache->offset += 1u << Slot::attemptsShift;
    if (!attempts)
        return "first time"_s;

    if (structure->typeInfo().prohibitsPropertyCaching())
        return "prohibits caching"_s;
    if (structure->isDictionary()) {
        // Next time.
        if (!structure->hasBeenFlattenedBefore())
            structure->flattenDictionaryStructure(vm, asObject(cell));
        return "inherited, base is a dictionary"_s;
    }
    if (slot.isUnset() && structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence())
        return "impure for absence"_s;
    auto status = prepareChainForCaching(globalObject, cell, ident.impl(), slot);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return "the chain cannot be prepared"_s;
    makePrototypeChainWatchable(vm, cell);

    if (slot.isUnset()) {
        if (!watchConditions(vm, data, cache, generateConditionsForPropertyMiss(vm, globalObject, globalObject, structure, ident.impl())))
            return "cannot watch for a miss"_s;
        fill(vm, data, cache, structure, *locationOfProperty(0) | Slot::pointerIsNotCell, const_cast<EncodedJSValue*>(holderOfUndefined));
        return ""_s;
    }

    auto location = locationOfProperty(slot.cachedOffset());
    if (!location)
        return "inherited, too far"_s;
    if (!watchConditions(vm, data, cache, generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, slot.slotBase(), ident.impl())))
        return "cannot watch for a hit"_s;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell | getterFlag, slot.slotBase());
    return ""_s;
}

void cachePrivateName(VM& vm, Data* data, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset> offset)
{
    if ((Options::aotDisableFastPaths() & 1) || !name.isCell() || SharedData::contains(cache))
        return;
    Structure* structure = base->structure();
    if (!structure->propertyAccessesAreCacheable() || structure->isDictionary())
        return;
    auto location = locationOfProperty(offset.value_or(0));
    if (!location || !mayReplace(cache, structure))
        return;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell, name.asCell());
}

static bool tryCachePutById(JSGlobalObject*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);

void cachePutById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    if (SharedData::contains(cache))
        return;
    if (!tryCachePutById(globalObject, data, base, oldStructure, ident, slot, isDirect, cache))
        countFailure(cache);
}

static bool tryCachePutById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    if (!base.isCell() || !slot.isCacheablePut() || slot.base() != base.asCell())
        return false;
    // Objects that others inherit from have more depending on them than a store lets on.
    if (!oldStructure->propertyAccessesAreCacheable() || oldStructure->isDictionary() || oldStructure->mayBePrototype())
        return false;

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* newStructure = cell->structure();
    auto location = locationOfProperty(slot.cachedOffset());
    if (!newStructure->propertyAccessesAreCacheable() || !location)
        return false;

    if (!mayReplace(cache, oldStructure))
        return false;

    auto fillPut = [&](Structure* structureAfterwards) {
        uint32_t attempts = cache->offset & Slot::attemptsMask;
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = *location | attempts | (structureAfterwards ? Slot::isIntricate : 0);
        cache->pointer = nullptr;
        cache->newStructureID = structureAfterwards ? structureAfterwards->id() : StructureID();
        WTF::storeStoreFence();
        cache->structureID = oldStructure->id();
        // For a transition CodeBlock::propagateTransitions() has to see it too, even if the collector has been by already.
        didFillSlot(vm, data);
    };

    if (slot.type() == PutPropertySlot::ExistingProperty) {
        if (newStructure != oldStructure || (Options::aotDisableFastPaths() & 2))
            return false;
        // Code that has folded the property to a constant has to hear about writes that go around the runtime.
        oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
        stopWatching(data, cache);
        fillPut(nullptr);
        return true;
    }

    // A new property. Only the case that takes no more than storing the value and the new structure.
    if ((Options::aotDisableFastPaths() & 4) || newStructure->isDictionary() || newStructure->previousID() != oldStructure
        || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity())
        return false;

    if (isDirect)
        stopWatching(data, cache);
    else {
        // Nothing on the prototype chain has a say now (a setter, a read-only property), and that has to stay so.
        if (Options::aotDisableFastPaths() & 512) [[unlikely]]
            return false;
        auto status = prepareChainForCaching(globalObject, cell, ident.impl(), nullptr);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return false;
        makePrototypeChainWatchable(vm, cell);
        if (!watchConditions(vm, data, cache, generateConditionsForPropertySetterMiss(vm, globalObject, globalObject, newStructure, ident.impl())))
            return false;
    }
    fillPut(newStructure);
    return true;
}

// ---- The megamorphic cache

static bool canUseMegamorphicCacheForGet(VM& vm, UniquedStringImpl* uid)
{
    // (The other tiers leave out three more names, which getByIdAndFillMegamorphicCache() has another way of being careful about.)
    return !(Options::aotDisableFastPaths() & 1024) && !parseIndex(*uid) && uid != vm.propertyNames->underscoreProto;
}

static bool canUseMegamorphicCacheForPut(VM& vm, UniquedStringImpl* uid)
{
    return !(Options::aotDisableFastPaths() & 1024) && canUseMegamorphicPutById(vm, uid);
}

// What the other tiers do for a get_by_id that has gone megamorphic (getByIdMegamorphic() in JITOperations.cpp): the lookup, one
// object at a time, keeping track of whether the answer is one that structures and the cache's epoch vouch for.
JSValue getByIdAndFillMegamorphicCache(JSGlobalObject* globalObject, JSValue base, const Identifier& ident, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    UniquedStringImpl* uid = ident.impl();
    if (!base.isObject() || !canUseMegamorphicCacheForGet(vm, uid))
        RELEASE_AND_RETURN(scope, base.get(globalObject, ident, slot));

    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    JSObject* baseObject = asObject(base);
    JSObject* object = baseObject;
    bool cacheable = true;
    bool isNameThoseHaveASayAbout = uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype;
    noteSlowPath("mega"_s, JSValue(), nullptr, cache.whyLoadIsNotFound(baseObject->structureID(), uid)); // TEMPORARY-SLOT-STATS
    while (true) {
        // Some kinds of object only have a say of their own about a few names.
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && (isNameThoseHaveASayAbout || (object->type() != ArrayType && object->type() != JSFunctionType && object->type() != DerivedStringObjectType && object != globalObject->arrayPrototype()))) [[unlikely]] {
            bool hasProperty = object->getNonIndexPropertySlot(globalObject, uid, slot);
            RETURN_IF_EXCEPTION(scope, { });
            if (hasProperty)
                RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
            return jsUndefined();
        }

        Structure* structure = object->structure();
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        structure = object->structure(); // Reifying a static property changes it.
        if (cacheable && !structure->propertyAccessesAreCacheable())
            noteSlowPath("mega-broken-by"_s, object, nullptr, object == baseObject ? "the base"_s : "an object on the chain"_s); // TEMPORARY-SLOT-STATS
        cacheable &= structure->propertyAccessesAreCacheable();
        if (hasProperty) {
            noteSlowPath("mega-present"_s, baseObject, nullptr, !cacheable ? "not cacheable"_s : !slot.isCacheableValue() ? "not a plain value"_s : slot.slotBase() == baseObject ? "own"_s : baseObject->structure()->isDictionary() ? "inherited, base is a dictionary"_s : "inherited"_s); // TEMPORARY-SLOT-STATS
            if (cacheable && slot.cachedOffset() <= MegamorphicCache::maxOffset && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary())) {
                if (slot.isCacheableValue())
                    cache.initAsHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
                else if (usesStubs && slot.isCacheableGetter())
                    cache.initAsGetterHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
            }
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
        }

        if (cacheable && (!structure->propertyAccessesAreCacheableForAbsence() || !structure->hasMonoProto()))
            noteSlowPath("mega-broken-by"_s, object, nullptr, !structure->hasMonoProto() ? "poly proto"_s : "not cacheable for absence"_s); // TEMPORARY-SLOT-STATS
        cacheable &= structure->propertyAccessesAreCacheableForAbsence();
        cacheable &= structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            noteSlowPath("mega-absent"_s, baseObject, nullptr, !cacheable ? "not cacheable"_s : baseObject->structure()->isDictionary() ? "base is a dictionary"_s : "cached as a miss"_s); // TEMPORARY-SLOT-STATS
            if (cacheable && !baseObject->structure()->isDictionary())
                cache.initAsMiss(baseObject->structureID(), uid);
            return jsUndefined();
        }
        object = asObject(prototype);
    }
}

// putMegamorphic() in JITOperations.cpp. Only for a put that went by the rules of [[Set]]: that it ended up as a store to the
// base is what says that nothing on the prototype chain objects.
void fillMegamorphicCacheAfterPut(JSGlobalObject* globalObject, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    UniquedStringImpl* uid = ident.impl();
    if (!base.isObject() || !slot.isCacheablePut() || slot.base() != base.asCell() || slot.cachedOffset() > MegamorphicCache::maxOffset)
        return;
    if (!canUseMegamorphicCacheForPut(vm, uid) || !oldStructure->propertyAccessesAreCacheable() || !canUseMegamorphicPutFastPath(oldStructure))
        return;

    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    Structure* newStructure = base.asCell()->structure();
    if (slot.type() == PutPropertySlot::ExistingProperty) {
        if (oldStructure == newStructure) {
            oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
            cache.initAsReplace(oldStructure->id(), uid, slot.cachedOffset());
        }
        return;
    }

    if (oldStructure->isDictionary() || newStructure->isDictionary() || oldStructure->mayBePrototype() || newStructure->previousID() != oldStructure || !newStructure->propertyAccessesAreCacheable())
        return;
    cache.initAsTransition(oldStructure->id(), newStructure->id(), uid, slot.cachedOffset(), newStructure->outOfLineCapacity() != oldStructure->outOfLineCapacity());
}

// ---- Calls

// ---- Allocation

void fillConstructionCache(VM& vm, Data* data, Slot* cache, JSFunction* callee, Structure* first, Structure* last, Allocator allocator)
{
    if (!allocator || (Options::aotDisableFastPaths() & 2048) || SharedData::contains(cache))
        return;
    cache[0].clear();
    fill(vm, data, &cache[2], first, 0, nullptr);
    fillAllocationCache(vm, data, cache, last, allocator, last->inlineCapacity(), callee);
}

void fillAllocationCache(VM& vm, Data* data, Slot* cache, Structure* structure, Allocator allocator, uint32_t payload, JSCell* extra)
{
    if (!allocator || (Options::aotDisableFastPaths() & 2048) || SharedData::contains(cache))
        return;
    ASSERT(payload <= Slot::offsetMask);
    cache[1].offset = structure->typeInfoBlob();
    cache[1].pointer = allocator.localAllocator();
    fill(vm, data, &cache[0], structure, payload | (extra ? Slot::pointerIsCell : 0), extra);
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
