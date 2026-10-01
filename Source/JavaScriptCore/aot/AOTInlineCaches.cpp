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
#include "JSTypedArrayViewPrototype.h"

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
    return (static_cast<uint32_t>(location) & Slot::offsetMask) | (location < 0 ? Slot::isIndirect : 0);
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
        offsetAndFlags |= Slot::isIndirect;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    cache->offset = offsetAndFlags | attempts;
    cache->pointer = pointer;
    WTF::storeStoreFence();
    cache->structureID = structure->id();
    didFillSlot(vm, data);
    if (Options::verboseAOTCompilation()) [[unlikely]]
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
    // (Given over to the name: see generateGetById().)
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return !!cache->structureID && cache->structureID == structure->id();
    if (!cache->structureID || cache->structureID == structure->id())
        return true;
    cache->offset += 1u << Slot::attemptsShift;
    return true;
}

// Whatever the reason a slot could not be filled, a site where that goes on happening is one for the megamorphic cache.
static void countFailure(Slot* cache)
{
    if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
        cache->offset += 1u << Slot::attemptsShift;
}

static bool tryCacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache);

// Which slot is to have what has been found out about objects of that structure: the place's own, until there is a second structure.
static uint16_t propertyNameID(VM& vm, UniquedStringImpl* uid)
{
    auto& table = vm.aotPropertyNameIDs;
    if (!table.next) {
        Image* image = Image::withShapes();
        table.next = (image ? image->header().largestFieldID : 0) + 1;
    }
    auto result = table.ids.add(uid, 0);
    if (!result.isNewEntry)
        return result.iterator->value;
    if (table.next >= Structure::ambiguousFieldID) {
        table.ids.remove(result.iterator);
        return 0;
    }
    result.iterator->value = static_cast<uint16_t>(table.next++);
    return result.iterator->value;
}

// If the access found a plain data property of the base's own in one of the first inline slots: records the name's id in the Structure and returns it. Otherwise zero.
static uint16_t recordPropertyNameInStructure(VM& vm, JSCell* base, Structure* structure, const Identifier& ident, const PropertySlot& slot)
{
    if (!slot.isCacheableValue() || slot.slotBase() != base || slot.attributes())
        return 0;
    PropertyOffset offset = slot.cachedOffset();
    if (!isInlineOffset(offset) || static_cast<unsigned>(offset) >= Structure::numberOfSlotsWithFieldIDs)
        return 0;
    // (A cell of the static heap that another VM reads has a Structure of the first VM's.)
    if (structure->isDictionary() || structure->typedLayoutID() || structure->cannotConvertToTypedLayout() || &structure->vm() != &vm)
        return 0;
    // As tryCacheGetById().
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return 0;
    uint16_t id = propertyNameID(vm, ident.impl());
    if (!id)
        return 0;
    RELEASE_ASSERT(!structure->fieldIDInSlot(offset) || structure->fieldIDInSlot(offset) == id);
    structure->setPropertyNameIDInInlineSlot(offset, id);
    return id;
}

static Slot* slotToFill(VM& vm, Data* data, Slot* cache, Structure* structure, const Identifier& ident)
{
    if (!cache->isPolymorphic()) {
        if (!cache->structureID || cache->structureID == structure->id())
            return cache;
        PolymorphicSlots* several = data->instance->makeSlotsOfSite(data, ident.impl());
        // What it had is as good as it was. (The collector looks at any time: it never sees a structure with a second word that is not its own.)
        Slot& first = several->slots[0];
        first.offset = cache->offset & ~Slot::attemptsMask;
        first.pointer = cache->pointer;
        moveWatching(data, cache, &first);
        WTF::storeStoreFence();
        first.structureID = cache->structureID;
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = Slot::polymorphicFlags;
        cache->pointer = several;
        didFillSlot(vm, data);
    }
    auto* several = static_cast<PolymorphicSlots*>(cache->pointer);
    if (several->timesLeftToLearnAtOnce)
        several->timesLeftToLearnAtOnce--;
    for (Slot& slot : several->slots) {
        if (slot.structureID == structure->id())
            return &slot;
    }
    for (Slot& slot : several->slots) {
        if (!slot.structureID && (slot.offset & Slot::attemptsMask) != Slot::attemptsMask)
            return &slot;
    }
    Slot& leaving = several->slots[several->next++ % PolymorphicSlots::numberOfSlots];
    stopWatching(data, &leaving);
    leaving.clear();
    leaving.offset = 0;
    data->slotEpoch++;
    return &leaving;
}

void cacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache, bool mayBePolymorphic)
{
    if (SharedData::contains(cache))
        return;
    if (mayBePolymorphic && usesStubs && base.isCell() && base.asCell()->structure() == structureBefore) {
        // The slot is monomorphic for another Structure with the same name at the same offset. Now that this Structure says so too, the stub hits on both: the site stays monomorphic.
        if (uint16_t id = recordPropertyNameInStructure(globalObject->vm(), base.asCell(), structureBefore, ident, slot)) {
            uint32_t sameAccess = *locationOfProperty(slot.cachedOffset()) | static_cast<uint32_t>(id) << Slot::nameIDShift;
            if (cache->structureID && (cache->offset & ~Slot::attemptsMask) == sameAccess)
                return;
        }
    }
    if (mayBePolymorphic && usesStubs && base.isCell())
        cache = slotToFill(globalObject->vm(), data, cache, base.asCell()->structure(), ident);
    if (!tryCacheGetById(globalObject, data, base, structureBefore, ident, slot, cache))
        countFailure(cache);
}

static bool tryCacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache)
{
    // Only the stubs know what to do about a getter.
    uint32_t getterFlag = usesStubs && slot.isCacheableGetter() ? Slot::isGetter : 0;
    if (!base.isCell() || (!slot.isCacheableValue() && !slot.isUnset() && !getterFlag))
        return false;

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* structure = cell->structure();
    // What the slot says, it says of the object as it was.
    if (structure != structureBefore)
        return false;
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return false;
    if (getterFlag) {
        // And of whatever it was found in.
        unsigned attributes;
        if (slot.slotBase()->structure()->get(vm, ident.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
            return false;
        // Where the stub looks for the getter's code is filled in when a function is first called from code. This one may only ever
        // have been called from here.
        if (auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter())) {
            // (By where it is: without the JIT a host function does not say which intrinsic it is.)
            if (ident == vm.propertyNames->length && slot.slotBase()->inherits<JSTypedArrayViewPrototype>() && slot.slotBase()->globalObject() == globalObject)
                data->instance->getterOfLengthOfTypedArrays = function;
            if (auto* executable = dynamicDowncast<FunctionExecutable>(function->executable()); executable && executable->isGeneratedForCall())
                executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
        }
    }

    if (!slot.isUnset() && slot.slotBase() == cell) {
        if (structure->isDictionary()) {
            // Next time, if it has settled down. Once: an object that goes on to be one again is being used as one.
            if (!structure->hasBeenFlattenedBefore() && cell->isObject())
                structure->flattenDictionaryStructure(vm, asObject(cell));
            return false;
        }
        auto location = locationOfProperty(slot.cachedOffset());
        if (!location)
            return false;
        if (!mayReplace(cache, structure))
            return false;
        if (cache->hasPointer())
            stopWatching(data, cache);
        // (Where there is no more to it than a place in the object, the second word is for the name: Slot::name.)
        uint32_t nameID = usesStubs ? recordPropertyNameInStructure(vm, cell, structure, ident, slot) : 0;
        fill(vm, data, cache, structure, *location | getterFlag | nameID << Slot::nameIDShift, (*location | getterFlag) & (Slot::isIndirect | Slot::isGetter) ? nullptr : ident.impl());
        return true;
    }

    // What follows takes some doing, and undoing. It is for the sites that see few structures, and not for code that runs once.
    uint32_t attempts = (cache->offset & Slot::attemptsMask) >> Slot::attemptsShift;
    if (attempts == Slot::maxAttempts)
        return false;
    // A site that sees one structure needs two. One that is still at it after a few sees several, and each time costs a good deal
    // more than looking in the megamorphic cache ever will.
    constexpr uint32_t maxAttemptsAtThis = 4;
    if (attempts >= maxAttemptsAtThis) {
        cache->offset |= Slot::attemptsMask;
        return false;
    }
    cache->offset += 1u << Slot::attemptsShift;
    if (!attempts)
        return false;

    if (structure->typeInfo().prohibitsPropertyCaching())
        return false;
    if (structure->isDictionary()) {
        // Next time.
        if (!structure->hasBeenFlattenedBefore())
            structure->flattenDictionaryStructure(vm, asObject(cell));
        return false;
    }
    if (slot.isUnset() && structure->typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence())
        return false;
    auto status = prepareChainForCaching(globalObject, cell, ident.impl(), slot);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return false;
    makePrototypeChainWatchable(vm, cell);

    if (slot.isUnset()) {
        if (!watchConditions(vm, data, cache, generateConditionsForPropertyMiss(vm, globalObject, globalObject, structure, ident.impl())))
            return false;
        fill(vm, data, cache, structure, *locationOfProperty(0) | Slot::pointerIsNotCell, const_cast<EncodedJSValue*>(holderOfUndefined));
        return true;
    }

    auto location = locationOfProperty(slot.cachedOffset());
    if (!location)
        return false;
    if (!watchConditions(vm, data, cache, generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, slot.slotBase(), ident.impl())))
        return false;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell | getterFlag, slot.slotBase());
    return true;
}

// As tryCacheGetBy() has it, for what it calls CustomAccessorGetter and CustomValueGetter.
void noteCustomGetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, const Identifier& ident, const PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    if (!vm.megamorphicCache())
        return;
    Structure* structure = base->structure();
    JSObject* holder = slot.slotBase();
    if (base->type() == GlobalProxyType || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching())
        return;
    if (holder->structure()->isDictionary())
        return;
    if (holder == base) {
        if (!prepareChainForCaching(globalObject, holder, ident.impl(), holder))
            return;
    } else {
        auto status = prepareChainForCaching(globalObject, base, ident.impl(), slot);
        if (!status || status->flattenedDictionary || status->usesPolyProto)
            return;
        if (!generateConditionsForPrototypePropertyHitCustom(vm, globalObject, globalObject, structure, holder, ident.impl(), slot.attributes()).isValid())
            return;
        // It stays so for as long as nothing is done to what the object inherits from, up to where the property is.
        if (!MegamorphicCache::noteDependenceOnPrototypes(structure->id(), holder))
            return;
    }
    // (Which is all that the check that the function is given the right kind of object comes to, of an object of a Structure that is known.)
    bool passesHolder = !(slot.attributes() & PropertyAttribute::CustomAccessor);
    if (auto domAttribute = slot.domAttribute(); domAttribute && !(passesHolder ? holder : base)->inherits(domAttribute->classInfo))
        return;
    instance.customGetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), vm.megamorphicCache()->epoch(), passesHolder, ident.impl(), std::bit_cast<void*>(slot.customGetter().taggedPtr()), holder };
}

void cachePrivateName(VM& vm, Data* data, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset> offset)
{
    if (!name.isCell() || SharedData::contains(cache))
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
    // A struct has been given a field. The next of its kind to be given that one need not come here, wherever it is done (Instance::addsOfFields).
    if (slot.type() == PutPropertySlot::NewTypedField && base.isCell() && slot.base() == base.asCell() && isInlineOffset(slot.cachedOffset())) {
        Structure* newStructure = base.asCell()->structure();
        if (newStructure->previousID() == oldStructure && !newStructure->isDictionary() && !oldStructure->mayBePrototype() && oldStructure->outOfLineCapacity() == newStructure->outOfLineCapacity())
            globalObject->aotInstance()->noteAddOfField(oldStructure, slot.cachedOffset(), newStructure);
    }
    if (SharedData::contains(cache))
        return;
    if (!tryCachePutById(globalObject, data, base, oldStructure, ident, slot, isDirect, cache))
        countFailure(cache);
}

static bool tryCachePutById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    if (!base.isCell() || (!slot.isCacheablePut() && !slot.isCacheablePutOfTypedField()) || slot.base() != base.asCell())
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
        uint32_t fieldType = slot.type() == PutPropertySlot::ExistingTypedField || slot.type() == PutPropertySlot::NewTypedField ? slot.fieldType() : 0;
        cache->offset = *location | attempts | (structureAfterwards ? Slot::isIndirect : 0) | (fieldType ? Slot::hasFieldType : 0);
        cache->pointer = nullptr;
        cache->newStructureID = structureAfterwards ? structureAfterwards->id() : StructureID();
        cache->fieldType = fieldType;
        WTF::storeStoreFence();
        cache->structureID = oldStructure->id();
        // For a transition the collector has to see it too, even if it has been by already.
        if (structureAfterwards)
            data->instance->noteTransitionCached(cache);
        didFillSlot(vm, data);
    };

    if (slot.type() == PutPropertySlot::ExistingProperty || slot.type() == PutPropertySlot::ExistingTypedField) {
        if (newStructure != oldStructure)
            return false;
        // Code that has folded the property to a constant has to hear about writes that go around the runtime.
        oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
        stopWatching(data, cache);
        fillPut(nullptr);
        return true;
    }

    // A new property. Only the case that takes no more than storing the value and the new structure.
    if (newStructure->isDictionary() || newStructure->previousID() != oldStructure || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity())
        return false;

    if (isDirect)
        stopWatching(data, cache);
    else {
        // Nothing on the prototype chain has a say now (a setter, a read-only property), and that has to stay so.
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
    return !parseIndex(*uid) && uid != vm.propertyNames->underscoreProto;
}

static bool canUseMegamorphicCacheForPut(VM& vm, UniquedStringImpl* uid)
{
    return canUseMegamorphicPutById(vm, uid);
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
        // Like the JIT's inline caches (tryCacheGetBy()): flatten a cacheable dictionary the first time an access tries to cache it.
        if (structure->isDictionary() && !structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore()) [[unlikely]] {
            structure->flattenDictionaryStructure(vm, object);
            structure = object->structure();
        }
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        structure = object->structure(); // Reifying a static property changes it.
        cacheable &= structure->propertyAccessesAreCacheable();
        if (hasProperty) {
            if (cacheable && slot.cachedOffset() <= MegamorphicCache::maxOffset && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary())) {
                if (slot.isCacheableValue())
                    cache.initAsHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
                else if (usesStubs && slot.isCacheableGetter())
                    cache.initAsGetterHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
            }
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
        }

        cacheable &= structure->propertyAccessesAreCacheableForAbsence();
        cacheable &= structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
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
    if (!allocator || SharedData::contains(cache))
        return;
    cache[0].clear();
    fill(vm, data, &cache[2], first, 0, nullptr);
    fillAllocationCache(vm, data, cache, last, allocator, last->inlineCapacity(), callee);
}

void cacheObjectOfSite(VM& vm, Data* data, Slot* cache, JSObject* object)
{
    if (SharedData::contains(cache))
        return;
    fill(vm, data, cache, object->structure(), Slot::pointerIsCell, object);
}

void fillAllocationCache(VM& vm, Data* data, Slot* cache, Structure* structure, Allocator allocator, uint32_t payload, JSCell* extra)
{
    if (!allocator || SharedData::contains(cache))
        return;
    ASSERT(payload <= Slot::offsetMask);
    cache[1].offset = structure->typeInfoBlob();
    cache[1].pointer = allocator.localAllocator();
    fill(vm, data, &cache[0], structure, payload | (extra ? Slot::pointerIsCell : 0), extra);
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
