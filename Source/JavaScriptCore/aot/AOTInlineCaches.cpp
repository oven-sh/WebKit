/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCaches.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTSlotWatchpoint.h"
#include "CodeBlock.h"
#include "GetterSetter.h"
#include "InlineCacheCompiler.h"
#include "JSBoundFunctionInlines.h"
#include "JSCInlines.h"
#include "JSModuleEnvironment.h"
#include "JSTypedArrayViewPrototype.h"
#include "MegamorphicCache.h"
#include "ObjectPropertyConditionSet.h"

namespace JSC { namespace AOT {

std::optional<uint32_t> propertyLocation(PropertyOffset offset)
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

static const EncodedJSValue undefinedHolder[JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + 1] = { 0, 0, JSValue::ValueUndefined };
static_assert(JSObject::offsetOfInlineStorage() == 2 * sizeof(EncodedJSValue));

static void fill(VM& vm, Data* data, Slot* cache, Structure* structure, uint32_t offsetAndFlags, void* pointer)
{
    uint32_t attempts = cache->offset & Slot::attemptsMask;
    if (offsetAndFlags & (Slot::isGetter | Slot::pointerIsCell | Slot::pointerIsNotCell))
        offsetAndFlags |= Slot::isIndirect;
    if (!(offsetAndFlags & Slot::flagsMask))
        attempts = 0;
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
        if (!structure->transitionWatchpointSetHasBeenInvalidated() || structure->isDictionary() || !structure->propertyAccessesAreCacheable())
            continue;
        prototype->convertToDictionary(vm);
        prototype->flattenDictionaryObject(vm);
    }
}

static bool mayReplace(Slot* cache, Structure* structure)
{
    if ((cache->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return !!cache->structureID && cache->structureID == structure->id();
    if (!cache->structureID || cache->structureID == structure->id())
        return true;
    cache->offset += 1u << Slot::attemptsShift;
    return true;
}

static void countFailure(Slot* cache)
{
    if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
        cache->offset += 1u << Slot::attemptsShift;
}

static bool abandon(Slot* cache)
{
    cache->offset |= Slot::attemptsMask;
    return false;
}

static bool tryCacheGetById(JSGlobalObject*, Data*, JSValue base, Structure* structureBefore, const Identifier&, const PropertySlot&, Slot* cache);

static auto& propertyNameIDs(VM& vm)
{
    auto& table = vm.aotPropertyNameIDs;
    if (!table.next) [[unlikely]] {
        RELEASE_ASSERT(table.ids.isEmpty());
        Image* image = Image::withCode();
        table.next = (image ? image->header().largestPropertyNameID : 0) + 1;
    }
    return table;
}

static uint16_t knownPropertyNameID(VM& vm, Image* imageWithPropertyNames, UniquedStringImpl* uid)
{
    auto& ids = propertyNameIDs(vm).ids;
    uint16_t id = ids.get(uid);
    if (!id && imageWithPropertyNames) {
        id = imageWithPropertyNames->propertyNameID(vm, uid);
        if (id)
            ids.add(uid, id);
    }
    return id;
}

bool hasListOfPropertyNames()
{
    return Image::withPropertyNames();
}

uint16_t propertyNameIDIfKnown(VM& vm, UniquedStringImpl* uid)
{
    Image* image = Image::withPropertyNames();
    if (!image)
        return 0;
    uint16_t id = knownPropertyNameID(vm, image, uid);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation(__func__, id ? "known" : "unknown");
    return id;
}

static uint16_t propertyNameID(VM& vm, UniquedStringImpl* uid)
{
    if (uint16_t id = knownPropertyNameID(vm, Image::withPropertyNames(), uid))
        return id;
    auto& table = propertyNameIDs(vm);
    if (table.next >= Structure::firstReservedPropertyNameID)
        return 0;
    uint16_t id = static_cast<uint16_t>(table.next++);
    table.ids.add(uid, id);
    return id;
}

void validatePropertyNameIDs(VM& vm)
{
    Image* image = Image::withPropertyNames();
    if (!image)
        return;
    for (auto& name : image->propertyNames()) {
        RELEASE_ASSERT(name.id > image->header().largestFieldID && name.id < propertyNameIDs(vm).next);
        RELEASE_ASSERT(propertyNameID(vm, VMProgram::of(vm)->identifier(static_cast<uint32_t>(name.identifier))) == name.id);
    }
}

static uint16_t recordPropertyNameInStructure(VM& vm, JSCell* base, Structure* structure, const Identifier& ident, const PropertySlot& slot)
{
    if (!slot.isCacheableValue() || slot.slotBase() != base || slot.attributes())
        return 0;
    PropertyOffset offset = slot.cachedOffset();
    if (!isInlineOffset(offset) || static_cast<unsigned>(offset) >= Structure::numberOfSlotsWithPropertyNameIDs)
        return 0;
    if (structure->isDictionary() || !structure->recordsPropertyNames() || structure->cannotConvertToTypedLayout())
        return 0;
    if (structure->mayBePrototype() || (structure->typedLayoutID() && TypedLayoutTable::hasTypedFields()))
        return 0;
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return 0;
    uint16_t id = propertyNameID(vm, ident.impl());
    if (!id)
        return 0;
    RELEASE_ASSERT(!structure->fieldIDInSlot(offset) || structure->fieldIDInSlot(offset) == Structure::noPropertyNameID || structure->fieldIDInSlot(offset) == id);
    if (structure->isWatchingReplacement())
        structure->firePropertyReplacementWatchpointSet(vm, offset, "Code stores to it without asking");
    structure->setPropertyNameIDInInlineSlot(offset, id);
    return id;
}

static bool fillPropertyNameTable(VM& vm, Structure* structure)
{
    if (!structure->recordsPropertyNames())
        return false;
    if (structure->cannotConvertToTypedLayout())
        return true;
    bool isFullyFilled = true;
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithPropertyNameIDs; ++slot)
        isFullyFilled &= !!structure->fieldIDInSlot(slot);
    if (isFullyFilled)
        return true;
    uint16_t ids[Structure::numberOfSlotsWithPropertyNameIDs];
    std::ranges::fill(ids, Structure::noPropertyNameID);
    bool hasTypedFields = structure->typedLayoutID() && TypedLayoutTable::hasTypedFields();
    if (!structure->isDictionary() && !structure->mayBePrototype() && !hasTypedFields && structure->propertyAccessesAreCacheable() && !structure->needImpurePropertyWatchpoint() && !structure->typeInfo().overridesGetOwnPropertySlot()) {
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (static_cast<unsigned>(entry.offset()) < Structure::numberOfSlotsWithPropertyNameIDs && !entry.attributes()) {
                if (uint16_t id = propertyNameID(vm, entry.key()))
                    ids[entry.offset()] = id;
            }
            return true;
        });
    }
    for (unsigned slot = 0; slot < Structure::numberOfSlotsWithPropertyNameIDs; ++slot) {
        if (structure->fieldIDInSlot(slot))
            continue;
        if (ids[slot] != Structure::noPropertyNameID && structure->isWatchingReplacement())
            structure->firePropertyReplacementWatchpointSet(vm, static_cast<PropertyOffset>(slot), "Code stores to it without asking");
        structure->setPropertyNameIDInInlineSlot(slot, ids[slot]);
    }
    return true;
}

static bool cacheByName(VM& vm, PolymorphicSlots* several, JSCell* base, Structure* structure, const PropertySlot& slot)
{
    if (!fillPropertyNameTable(vm, structure)) {
        countOperationNamed(several->owner->instance, __func__, several->fillsNameTables() ? "structure-records-no-names" : "site-no-longer-fills-name-tables");
        if (several->fillsNameTables())
            several->didFailToFillNameTable();
        return false;
    }
    if (!slot.isCacheableValue() || slot.slotBase() != base || static_cast<unsigned>(slot.cachedOffset()) >= Structure::numberOfSlotsWithPropertyNameIDs)
        return false;
    uint16_t id = structure->fieldIDInSlot(slot.cachedOffset());
    return id < Structure::firstReservedPropertyNameID && several->addInlineNameSlot(id, slot.cachedOffset());
}

static PolymorphicSlots* siteSlots(VM& vm, Data* data, Slot* cache, Structure* structure, const Identifier& ident)
{
    if (cache->isNameOnly()) {
        PolymorphicSlots* several = data->instance->makeSiteSlots(data, ident.impl());
        several->addInlineNameSlot(cache->offset >> Slot::nameIDShift, (cache->offset & Slot::directLocationMask) - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
        cache->offset = Slot::polymorphicFlags;
        cache->pointer = several;
        didFillSlot(vm, data);
    }
    if (!cache->isPolymorphic()) {
        if (!cache->structureID || cache->structureID == structure->id())
            return nullptr;
        PolymorphicSlots* several = data->instance->makeSiteSlots(data, ident.impl());
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
        if (uint16_t id = first.offset >> Slot::nameIDShift; id && !(first.offset & Slot::flagsMask))
            several->addInlineNameSlot(id, (first.offset & Slot::directLocationMask) - JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
    }
    return static_cast<PolymorphicSlots*>(cache->pointer);
}

static Slot* findFillableEntry(Data* data, PolymorphicSlots* several, Structure* structure)
{
    if (several->remainingBulkLearnAttempts)
        several->remainingBulkLearnAttempts--;
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
    if (auto namespaceSlot = slot.moduleNamespaceSlot()) {
        if (base != slot.slotBase())
            return;
        Structure* structure = base.asCell()->structure();
        uint32_t location = JSLexicalEnvironment::offsetOfVariable(ScopeOffset(namespaceSlot->scopeOffset)) / sizeof(EncodedJSValue);
        if (usesDataStubs() && structure == structureBefore && structure != globalObject->moduleNamespaceObjectStructure() && location < (1u << (Slot::offsetBits - 1)) && mayReplace(cache, structure)) {
            if (cache->hasPointer())
                stopWatching(data, cache);
            fill(globalObject->vm(), data, cache, structure, location | Slot::pointerIsCell, namespaceSlot->environment);
        }
        return;
    }
    if (mayBePolymorphic && usesDataStubs() && base.isCell() && base.asCell()->structure() == structureBefore) {
        if (uint16_t id = recordPropertyNameInStructure(globalObject->vm(), base.asCell(), structureBefore, ident, slot)) {
            uint32_t location = *propertyLocation(slot.cachedOffset());
            uint32_t sameAccess = location | static_cast<uint32_t>(id) << Slot::nameIDShift;
            if (cache->isNameOnly() && cache->offset == sameAccess)
                return;
            if (uint32_t cachedAccess = cache->offset & ~Slot::attemptsMask; cache->structureID && (cachedAccess == sameAccess || cachedAccess == location)) {
                Structure* cached = cache->structureID.decode();
                if (fillPropertyNameTable(globalObject->vm(), cached) && cached->fieldIDInSlot(slot.cachedOffset()) == id) {
                    cache->structureID = StructureID();
                    cache->offset = sameAccess;
                    data->slotEpoch++;
                    return;
                }
            }
        }
    }
    if (mayBePolymorphic && usesDataStubs() && base.isCell()) {
        VM& vm = globalObject->vm();
        Structure* structure = base.asCell()->structure();
        if (PolymorphicSlots* several = siteSlots(vm, data, cache, structure, ident)) {
            if (structure == structureBefore && cacheByName(vm, several, base.asCell(), structure, slot))
                return;
            cache = findFillableEntry(data, several, structure);
        }
    }
    if (!tryCacheGetById(globalObject, data, base, structureBefore, ident, slot, cache))
        countFailure(cache);
}

static void cacheGetterShortcut(VM& vm, GetterSetter* getterSetter, JSFunction* getter)
{
    if (getterSetter->getterShortcut() || getter->isHostFunction())
        return;
    constexpr auto kind = CodeSpecializationKind::CodeForCall;
    Instance* instance = Instance::of(getter);
    FunctionExecutable* executable = getter->jsExecutable();
    FunctionRef function = FunctionRef::of(vm, executable, kind, tokenOf(instance));
    if (!function || function.instance != instance)
        return;
    const ImageFunction* record = function.info().function();
    if (record->usesStaticImports && !moduleIsLinkedAsCompiled(getter->scope()))
        return;
    if (record->returnsScopeVariable) {
        JSScope* scope = getter->scope();
        for (unsigned hops = record->returnedVariable()[0]; hops && scope; --hops)
            scope = scope->next();
        ScopeOffset offset(record->returnedVariable()[1]);
        if (auto* environment = dynamicDowncast<JSLexicalEnvironment>(scope); environment && environment->isValidScopeOffset(offset))
            getterSetter->setVariableReturnedByGetter(&environment->variableAt(offset));
        return;
    }
    if (record->takesList || record->numberOfParameters || !executable->aotEntryFor(kind))
        return;
    if (!instance->isLinked(function.index)) {
        DeferGCForAWhile deferGC(vm);
        if (!linkStaticFunction(instance, executable, kind, getter->scopeUnchecked()))
            return;
    }
    getterSetter->setCodeOfGetter(instance->code + (executable->aotEntryFor(kind) & EntryWord::addressMask));
}

static void cachePropertyOfBoundThis(JSGlobalObject* globalObject, Instance& instance, JSFunction* getter)
{
    VM& vm = globalObject->vm();
    auto* bound = dynamicDowncast<JSBoundFunction>(getter);
    if (!bound || bound->boundArgsLength() != 1 || bound->structureID().bits() != instance.boundFunctionStructureID)
        return;
    bound->clearCachedPropertyOfBoundThis();
    auto* target = dynamicDowncast<JSFunction>(bound->targetFunction());
    if (!target || target->isHostFunction())
        return;
    FunctionRef function = FunctionRef::of(vm, target->jsExecutable(), CodeSpecializationKind::CodeForCall, tokenOf(Instance::of(target)));
    if (!function || !function.info().function()->isGetByValOnThis)
        return;
    JSValue key;
    bound->forEachBoundArg([&](JSValue argument) {
        key = argument;
        return IterationStatus::Done;
    });
    JSObject* object = bound->boundThis().getObject();
    if (!object || !key.isString())
        return;
    String name = asString(key)->tryGetValue();
    RefPtr uid = name.isNull() ? nullptr : AtomStringImpl::lookUp(name.impl());
    if (!uid || parseIndex(*uid))
        return;
    if (Structure* structure = object->structure(); structure->isDictionary() && !structure->hasBeenFlattenedBefore())
        structure->flattenDictionaryStructure(vm, object);
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (!object->methodTable()->getOwnPropertySlot(object, globalObject, uid.get(), slot))
        return;
    if (slot.slotBase() != object || slot.isTaintedByOpaqueObject())
        return;
    bool isAccessor = slot.isCacheableGetter();
    if (isAccessor) {
        auto* inner = dynamicDowncast<JSFunction>(slot.getterSetter()->getter());
        if (!inner)
            return;
        cacheGetterShortcut(vm, slot.getterSetter(), inner);
        if (!slot.getterSetter()->getterReturnsVariable())
            return;
    } else if (!slot.isCacheableValue())
        return;
    Structure* structure = object->structure();
    if (!structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint())
        return;
    unsigned attributes;
    if (structure->get(vm, uid.get(), attributes) != slot.cachedOffset())
        return;
    PropertyOffset offset = slot.cachedOffset();
    int32_t location = isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offset : offsetInButterfly(offset);
    bound->cachePropertyOfBoundThis(vm, structure, location * 2 + isAccessor);
}

static void noteLengthOfTypedArray(JSGlobalObject* globalObject, Instance& instance, JSCell* cell, const PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Structure* structure = cell->structure();
    JSType type = cell->type();
    auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter());
    JSObject* viewPrototype = slot.slotBase();
    unsigned attributes;
    if (!isTypedArrayType(type) || !function || !isTypedArrayViewPrototypeLengthGetter(function) || !viewPrototype->inherits<JSTypedArrayViewPrototype>() || viewPrototype->globalObject() != globalObject)
        return;
    if (!structure->propertyAccessesAreCacheable() || viewPrototype->structure()->isDictionary() || viewPrototype->structure()->get(vm, vm.propertyNames->length.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
        return;
    instance.typedArrayLengthGetter = function;
    auto prototypeOf = [](Structure* structure) -> JSObject* { return structure->hasMonoProto() && !structure->isDictionary() ? structure->storedPrototypeObject() : nullptr; };
    JSObject* prototype = prototypeOf(structure);
    JSObject* secondPrototype = prototype ? prototypeOf(prototype->structure()) : nullptr;
    if (secondPrototype == viewPrototype)
        secondPrototype = prototype;
    if (!secondPrototype || prototypeOf(secondPrototype->structure()) != viewPrototype || isInlineOffset(slot.cachedOffset()))
        return;
    instance.typedArrayViewPrototype = viewPrototype;
    instance.typedArrayViewPrototypeStructureID = viewPrototype->structureID().bits();
    instance.typedArrayLengthAccessor = slot.getterSetter();
    instance.typedArrayLengthOffsetInButterfly = offsetInButterfly(slot.cachedOffset()) * static_cast<intptr_t>(sizeof(EncodedJSValue));
    auto& entry = instance.typedArraysWithBuiltinLength[type - FirstTypedArrayType][structure != globalObject->typedArrayStructure(typedArrayType(type), false)];
    entry.structureID = 0;
    entry.prototype = prototype;
    entry.prototypeStructureID = prototype->structureID().bits();
    entry.secondPrototype = secondPrototype;
    entry.secondPrototypeStructureID = secondPrototype->structureID().bits();
    entry.structureID = structure->id().bits();
    vm.writeBarrier(instance.globalObject);
}

static bool tryCacheGetById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* structureBefore, const Identifier& ident, const PropertySlot& slot, Slot* cache)
{
    if (base.isCell() && slot.isCacheableGetter() && ident == globalObject->vm().propertyNames->length && base.asCell()->structure() == structureBefore)
        noteLengthOfTypedArray(globalObject, *data->instance, base.asCell(), slot);
    uint32_t getterFlag = usesDataStubs() && slot.isCacheableGetter() ? Slot::isGetter : 0;
    if (!base.isCell() || (!slot.isCacheableValue() && !slot.isUnset() && !getterFlag))
        return false;

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* structure = cell->structure();
    if (structure != structureBefore)
        return false;
    if (structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore() && !structure->typeInfo().prohibitsPropertyCaching() && cell->isObject()) {
        asObject(cell)->flattenDictionaryObject(vm);
        return false;
    }
    if (!structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return false;
    if (getterFlag) {
        unsigned attributes;
        if (slot.slotBase()->structure()->get(vm, ident.impl(), attributes) != slot.cachedOffset() || !(attributes & PropertyAttribute::Accessor))
            return false;
        if (auto* function = dynamicDowncast<JSFunction>(slot.getterSetter()->getter())) {
            if (auto* executable = dynamicDowncast<FunctionExecutable>(function->executable()); executable && executable->isGeneratedForCall())
                executable->entrypointFor(CodeSpecializationKind::CodeForCall, ArityCheckMode::MustCheckArity);
            cachePropertyOfBoundThis(globalObject, *data->instance, function);
            cacheGetterShortcut(vm, slot.getterSetter(), function);
        }
    }

    if (!slot.isUnset() && slot.slotBase() == cell) {
        if (structure->isDictionary()) {
            if (!structure->hasBeenFlattenedBefore() && cell->isObject())
                structure->flattenDictionaryStructure(vm, asObject(cell));
            return false;
        }
        auto location = propertyLocation(slot.cachedOffset());
        if (!location)
            return false;
        if (!mayReplace(cache, structure))
            return false;
        if (cache->hasPointer())
            stopWatching(data, cache);
        uint32_t nameID = usesDataStubs() ? recordPropertyNameInStructure(vm, cell, structure, ident, slot) : 0;
        fill(vm, data, cache, structure, *location | getterFlag | nameID << Slot::nameIDShift, (*location | getterFlag) & (Slot::isIndirect | Slot::isGetter) ? nullptr : ident.impl());
        return true;
    }

    uint32_t attempts = (cache->offset & Slot::attemptsMask) >> Slot::attemptsShift;
    if (attempts == Slot::maxAttempts)
        return false;
    if (attempts >= Slot::maxAttemptsForInheritedProperty) {
        cache->offset |= Slot::attemptsMask;
        return false;
    }
    cache->offset += 1u << Slot::attemptsShift;
    if (!attempts)
        return false;

    if (structure->typeInfo().prohibitsPropertyCaching())
        return false;
    if (structure->isDictionary()) {
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
        fill(vm, data, cache, structure, *propertyLocation(0) | Slot::pointerIsNotCell, const_cast<EncodedJSValue*>(undefinedHolder));
        return true;
    }

    auto location = propertyLocation(slot.cachedOffset());
    if (!location)
        return false;
    if (!watchConditions(vm, data, cache, generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, slot.slotBase(), ident.impl())))
        return false;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell | getterFlag, slot.slotBase());
    return true;
}

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
        if (!MegamorphicCache::noteDependenceOnPrototypes(structure->id(), holder))
            return;
    }
    bool passesHolder = !(slot.attributes() & PropertyAttribute::CustomAccessor);
    if (auto domAttribute = slot.domAttribute(); domAttribute && !(passesHolder ? holder : base)->inherits(domAttribute->classInfo))
        return;
    instance.customGetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), vm.megamorphicCache()->epoch(), passesHolder, ident.impl(), std::bit_cast<void*>(slot.customGetter().taggedPtr()), holder };
}

void noteInheritedSetter(JSGlobalObject* globalObject, Instance& instance, JSObject* base, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    Structure* structure = base->structure();
    JSObject* holder = slot.base();
    if (!usesDataStubs() || !vm.megamorphicCache() || holder == base || parseIndex(*ident.impl()))
        return;
    if (base->type() == GlobalProxyType || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching() || isCopyOnWrite(structure->indexingMode()))
        return;
    if (holder->structure()->isDictionary())
        return;
    auto status = prepareChainForCaching(globalObject, base, ident.impl(), holder);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return;
    auto conditions = generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, holder, ident.impl());
    if (!conditions.isValid() || !(conditions.slotBaseCondition().attributes() & PropertyAttribute::Accessor))
        return;
    PropertyOffset offset = conditions.slotBaseCondition().offset();
    if (offset > MegamorphicCache::maxOffset || !MegamorphicCache::noteDependenceOnPrototypes(structure->id(), holder))
        return;
    instance.inheritedSetterFor(structure->id().bits(), ident.impl()) = { structure->id().bits(), vm.megamorphicCache()->epoch(), static_cast<uint16_t>(offset), ident.impl(), holder, nullptr };
}

void cacheInstanceOf(JSGlobalObject* globalObject, Data* data, Slot* cache, JSObject* constructor, Structure* structureBefore, const PropertySlot& hasInstance, const PropertySlot& prototype)
{
    if (SharedData::contains(cache))
        return;
    VM& vm = globalObject->vm();
    Structure* structure = constructor->structure();
    if (structure != structureBefore || !structure->propertyAccessesAreCacheable() || structure->isDictionary() || structure->needImpurePropertyWatchpoint() || structure->typeInfo().prohibitsPropertyCaching())
        return;
    if (!prototype.isCacheableValue() || prototype.slotBase() != constructor)
        return;
    if (!hasInstance.isCacheableValue() || hasInstance.slotBase() == constructor)
        return;
    if (!(hasInstance.attributes() & PropertyAttribute::ReadOnly) || !(hasInstance.attributes() & PropertyAttribute::DontDelete))
        return;
    auto location = propertyLocation(prototype.cachedOffset());
    if (!location || !mayReplace(cache, structure))
        return;
    UniquedStringImpl* uid = vm.propertyNames->hasInstanceSymbol.impl();
    auto status = prepareChainForCaching(globalObject, constructor, uid, hasInstance);
    if (!status || status->flattenedDictionary || status->usesPolyProto)
        return;
    makePrototypeChainWatchable(vm, constructor);
    if (cache->hasPointer())
        stopWatching(data, cache);
    if (!watchConditions(vm, data, cache, generateConditionsForPrototypePropertyHit(vm, globalObject, globalObject, structure, hasInstance.slotBase(), uid)))
        return;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell, hasInstance.slotBase());
}

void cachePrivateName(VM& vm, Data* data, Slot* cache, JSObject* base, JSValue name, std::optional<PropertyOffset> offset)
{
    if (!name.isCell() || SharedData::contains(cache))
        return;
    Structure* structure = base->structure();
    if (!structure->propertyAccessesAreCacheable() || structure->isDictionary())
        return;
    auto location = propertyLocation(offset.value_or(0));
    if (!location || !mayReplace(cache, structure))
        return;
    fill(vm, data, cache, structure, *location | Slot::pointerIsCell, name.asCell());
}

void cachePrivateNameTransition(VM& vm, Data* data, Slot* cache, JSObject* base, Structure* oldStructure, JSValue name, std::optional<PropertyOffset> offset)
{
    if (!name.isCell() || SharedData::contains(cache))
        return;
    Structure* newStructure = base->structure();
    if (!oldStructure->propertyAccessesAreCacheable() || oldStructure->isDictionary() || oldStructure->mayBePrototype() || !newStructure->propertyAccessesAreCacheable() || newStructure->isDictionary())
        return;
    if (newStructure->previousID() != oldStructure || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity() || oldStructure->typeInfoBlob() != newStructure->typeInfoBlob())
        return;
    auto location = propertyLocation(offset.value_or(0));
    if (!location || !mayReplace(cache, oldStructure))
        return;
    Slot& transition = cache[1];
    transition.structureID = StructureID();
    WTF::storeStoreFence();
    transition.offset = Slot::isIndirect;
    transition.pointer = nullptr;
    oldStructure->assertHandsFamilyOnTo(*newStructure);
    transition.newStructureID = newStructure->id();
    WTF::storeStoreFence();
    transition.structureID = oldStructure->id();
    data->instance->noteTransitionCached(&transition);
    fill(vm, data, cache, oldStructure, *location | Slot::pointerIsCell, name.asCell());
}

static bool tryCachePutById(JSGlobalObject*, Data*, JSValue base, Structure* oldStructure, const Identifier&, const PutPropertySlot&, bool isDirect, Slot* cache);

void cachePutById(Instance* instance, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    JSGlobalObject* globalObject = instance->globalObject;
    if (slot.type() == PutPropertySlot::NewProperty && slot.isCacheablePut() && base.isCell() && slot.base() == base.asCell() && !isInlineOffset(slot.cachedOffset()))
        instance->noteOutOfLineProperty(base.asCell()->structure());
    if (slot.type() == PutPropertySlot::NewTypedField && base.isCell() && slot.base() == base.asCell() && isInlineOffset(slot.cachedOffset())) {
        Structure* newStructure = base.asCell()->structure();
        if (newStructure->previousID() == oldStructure && !newStructure->isDictionary() && !oldStructure->mayBePrototype() && oldStructure->outOfLineCapacity() == newStructure->outOfLineCapacity())
            instance->noteFieldAddition(oldStructure, slot.cachedOffset(), newStructure);
    }
    if (SharedData::contains(cache))
        return;
    if (!tryCachePutById(globalObject, data, base, oldStructure, ident, slot, isDirect, cache))
        countFailure(cache);
}

static bool tryCachePutById(JSGlobalObject* globalObject, Data* data, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot, bool isDirect, Slot* cache)
{
    if (!base.isCell() || (!slot.isCacheablePut() && !slot.isTypedFieldCacheablePut()) || slot.base() != base.asCell())
        return false;
    if (!oldStructure->propertyAccessesAreCacheable() || oldStructure->isDictionary() || oldStructure->mayBePrototype())
        return abandon(cache);

    VM& vm = globalObject->vm();
    JSCell* cell = base.asCell();
    Structure* newStructure = cell->structure();
    auto location = propertyLocation(slot.cachedOffset());
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
        if (structureAfterwards)
            oldStructure->assertHandsFamilyOnTo(*structureAfterwards);
        cache->newStructureID = structureAfterwards ? structureAfterwards->id() : StructureID();
        cache->fieldType = fieldType;
        WTF::storeStoreFence();
        cache->structureID = oldStructure->id();
        if (structureAfterwards)
            data->instance->noteTransitionCached(cache);
        didFillSlot(vm, data);
    };

    if (slot.type() == PutPropertySlot::ExistingProperty || slot.type() == PutPropertySlot::ExistingTypedField) {
        if (newStructure != oldStructure)
            return false;
        oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
        stopWatching(data, cache);
        fillPut(nullptr);
        return true;
    }

    if (newStructure->isDictionary() || newStructure->previousID() != oldStructure || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity())
        return abandon(cache);

    if (isDirect)
        stopWatching(data, cache);
    else {
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

static bool canUseMegamorphicCacheForGet(VM& vm, UniquedStringImpl* uid)
{
    return !parseIndex(*uid) && uid != vm.propertyNames->underscoreProto;
}

static bool canUseMegamorphicCacheForPut(VM& vm, UniquedStringImpl* uid)
{
    return canUseMegamorphicPutById(vm, uid);
}

static ALWAYS_INLINE void countFill(VM& vm, const char* kind, MegamorphicCache& cache, StructureID structureID, UniquedStringImpl* uid, const char* outcome = nullptr, StructureID newStructureID = { })
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countFillOfMegamorphicCache(kind, cache.epoch(), static_cast<uint64_t>(newStructureID.bits()) << 32 | structureID.bits(), uid, outcome);
}

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
    bool isLazyFunctionPropertyName = uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype;
    while (true) {
        if (TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags()) && (isLazyFunctionPropertyName || (object->type() != ArrayType && object->type() != JSFunctionType && object->type() != DerivedStringObjectType && object != globalObject->arrayPrototype()))) [[unlikely]] {
            bool hasProperty = object->getNonIndexPropertySlot(globalObject, uid, slot);
            RETURN_IF_EXCEPTION(scope, { });
            if (hasProperty)
                RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
            return jsUndefined();
        }

        Structure* structure = object->structure();
        if (structure->isDictionary() && !structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore()) [[unlikely]] {
            structure->flattenDictionaryStructure(vm, object);
            structure = object->structure();
        }
        bool hasProperty = object->getOwnNonIndexPropertySlot(vm, structure, uid, slot);
        structure = object->structure();
        cacheable &= structure->propertyAccessesAreCacheable();
        if (hasProperty) {
            if (cacheable && slot.cachedOffset() <= MegamorphicCache::maxOffset && (slot.slotBase() == baseObject || !baseObject->structure()->isDictionary())) {
                if (slot.isCacheableValue()) {
                    bool isPresent = !!cache.findLoad(baseObject->structureID(), uid);
                    countFill(vm, "MegamorphicCache::load", cache, baseObject->structureID(), uid, isPresent ? "present" : nullptr);
                    if (!isPresent)
                        cache.initAsHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
                } else if (usesDataStubs() && slot.isCacheableGetter())
                    cache.initAsGetterHit(baseObject->structureID(), uid, slot.slotBase(), slot.cachedOffset(), slot.slotBase() == baseObject);
            }
            RELEASE_AND_RETURN(scope, slot.getValue(globalObject, uid));
        }

        cacheable &= structure->propertyAccessesAreCacheableForAbsence();
        cacheable &= structure->hasMonoProto();
        JSValue prototype = object->getPrototypeDirect();
        if (!prototype.isObject()) {
            if (cacheable && !baseObject->structure()->isDictionary()) {
                bool isPresent = !!cache.findLoad(baseObject->structureID(), uid);
                countFill(vm, "MegamorphicCache::absent", cache, baseObject->structureID(), uid, isPresent ? "present" : nullptr);
                if (!isPresent)
                    cache.initAsMiss(baseObject->structureID(), uid);
            }
            return jsUndefined();
        }
        object = asObject(prototype);
    }
}

void fillMegamorphicCacheAfterPut(JSGlobalObject* globalObject, JSValue base, Structure* oldStructure, const Identifier& ident, const PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    UniquedStringImpl* uid = ident.impl();
    if (!base.isObject() || !slot.isCacheablePut() || slot.base() != base.asCell() || slot.cachedOffset() > MegamorphicCache::maxOffset)
        return;
    if (!canUseMegamorphicCacheForPut(vm, uid) || !oldStructure->propertyAccessesAreCacheable() || !canUseMegamorphicPutFastPath(oldStructure))
        return;

    MegamorphicCache& cache = vm.ensureMegamorphicCache();
    if (cache.findStore(oldStructure->id(), uid)) {
        countFill(vm, "MegamorphicCache::store", cache, oldStructure->id(), uid, "present");
        return;
    }
    Structure* newStructure = base.asCell()->structure();
    if (slot.type() == PutPropertySlot::ExistingProperty) {
        if (oldStructure == newStructure) {
            oldStructure->didCachePropertyReplacement(vm, slot.cachedOffset());
            countFill(vm, "MegamorphicCache::replace", cache, oldStructure->id(), uid);
            cache.initAsReplace(oldStructure->id(), uid, slot.cachedOffset());
        }
        return;
    }

    if (oldStructure->isDictionary() || newStructure->isDictionary() || oldStructure->mayBePrototype() || newStructure->previousID() != oldStructure || !newStructure->propertyAccessesAreCacheable())
        return;
    uint8_t reallocating = 0;
    if (newStructure->outOfLineCapacity() != oldStructure->outOfLineCapacity()) {
        bool allocatesInitialStorage = !oldStructure->outOfLineCapacity() && newStructure->outOfLineCapacity() == initialOutOfLineCapacity && slot.cachedOffset() == firstOutOfLineOffset
            && oldStructure->typeInfo().type() == FinalObjectType && !hasIndexedProperties(oldStructure->indexingType());
        reallocating = allocatesInitialStorage ? MegamorphicCache::StoreEntry::allocatesInitialOutOfLineStorage : MegamorphicCache::StoreEntry::reallocates;
    }
    oldStructure->assertHandsFamilyOnTo(*newStructure);
    cache.initAsTransition(oldStructure->id(), newStructure->id(), uid, slot.cachedOffset(), reallocating);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        countFill(vm, "MegamorphicCache::transition", cache, oldStructure->id(), uid, cache.findStore(oldStructure->id(), uid) ? nullptr : "refused", newStructure->id());
}

void fillConstructionCache(VM& vm, Data* data, Slot* cache, JSFunction* callee, Structure* first, Structure* last, Allocator allocator)
{
    if (!allocator || SharedData::contains(cache))
        return;
    cache[0].clear();
    Slot& transition = cache[2];
    transition.structureID = StructureID();
    WTF::storeStoreFence();
    transition.offset = 0;
    transition.pointer = nullptr;
    first->assertHandsFamilyOnTo(*last);
    transition.newStructureID = last->id();
    WTF::storeStoreFence();
    transition.structureID = first->id();
    data->instance->noteTransitionCached(&transition);
    fillAllocationCache(vm, data, cache, last, allocator, last->inlineCapacity(), callee);
}

void cacheSiteObject(VM& vm, Data* data, Slot* cache, JSObject* object)
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

void fillFunctionAllocationCache(VM& vm, Data* data, Slot* cache, Structure* structure, Allocator allocator, uintptr_t functionWord)
{
    if (!allocator || SharedData::contains(cache))
        return;
    cache[1].offset = structure->typeInfoBlob();
    cache[1].pointer = allocator.localAllocator();
    fill(vm, data, &cache[0], structure, Slot::pointerIsNotCell, std::bit_cast<void*>(functionWord));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
