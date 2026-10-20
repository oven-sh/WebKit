/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesAnswers.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTClosure.h"
#include "AOTObject.h"
#include "AOTRuntimeCounters.h"
#include "AOTStaticFields.h"
#include "ErrorInstance.h"
#include "InlineCacheCompiler.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "PutPropertySlot.h"

namespace JSC { namespace AOT {

bool repeatsPropertyAddition(Structure* structure, UniquedStringImpl* uid)
{
    if (structure->isDictionary())
        return true;
    PropertyOffset offset = invalidOffset;
    return Structure::addPropertyTransitionToExistingStructure(structure, uid, 0, offset);
}

static PropertyAnswer uncacheable(const StoreQuery& query, bool didFlatten = false)
{
    return PropertyAnswer::uncacheable(accessFor(query.mode), didFlatten);
}

static PropertyAnswer uncacheableOnce(const StoreQuery& query)
{
    return uncacheable(query, true);
}

static PropertyAnswer uncacheableAfter(const StoreQuery& query, ChainWalk&& walk)
{
    PropertyAnswer answer = uncacheable(query, walk.didFlatten);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer ownAnswer(const StoreQuery& query, PropertyAnswerKind kind, PropertyOffset offset = invalidOffset)
{
    PropertyAnswer answer;
    answer.kind = kind;
    answer.access = accessFor(query.mode);
    answer.isOwn = true;
    answer.offset = offset;
    return answer;
}

// A setter may have given the receiver another prototype, and the walk is about the chain of structureBefore.
static ChainWalk walkChainAbove(const StoreQuery& query)
{
    Structure* before = query.structureBefore;
    bool hasSamePrototype = before->hasMonoProto() && query.base->getPrototypeDirect() == before->storedPrototype();
    return walkChain({ query.globalObject, before, hasSamePrototype ? query.base : nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, query.uid);
}

OutOfLineStorageChange outOfLineStorageChange(Structure* before, Structure* after, PropertyOffset offset)
{
    if (before->isDictionary() || after->isDictionary() || after->previousID() != before || before->typeInfoBlob() != after->typeInfoBlob())
        return OutOfLineStorageChange::Unsupported;
    unsigned capacityBefore = before->outOfLineCapacity();
    unsigned capacityAfter = after->outOfLineCapacity();
    if (capacityAfter == capacityBefore)
        return OutOfLineStorageChange::Stays;
    if (offset != firstOutOfLineOffset + static_cast<PropertyOffset>(capacityBefore))
        return OutOfLineStorageChange::Unsupported;
    if (before->couldHaveIndexingHeader())
        return OutOfLineStorageChange::GrowsBesideIndexingHeader;
    if (!capacityBefore)
        return capacityAfter == initialOutOfLineCapacity ? OutOfLineStorageChange::IsFirst : OutOfLineStorageChange::Unsupported;
    bool doublesCapacity = capacityBefore >= initialOutOfLineCapacity && capacityAfter == 2 * capacityBefore && capacityAfter * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff;
    return doublesCapacity ? OutOfLineStorageChange::Doubled : OutOfLineStorageChange::Unsupported;
}

bool isHostObjectOverridingPut(Structure* structure)
{
    return structure->typeInfo().overridesPut() && structure->typeInfo().type() == ObjectType;
}

static bool storesAreOrdinaryAlongPrototypeChain(Structure* structure)
{
    return canUseMegamorphicPutFastPath(structure);
}

bool hasOrdinaryDefineOwnProperty(Structure* structure)
{
    return structure->classInfoForCells()->methodTable.defineOwnProperty == JSObject::defineOwnProperty;
}

static bool isFunctionWithOrdinaryPut(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid)
{
    if (isCompiledClosure(base)) {
        auto* closure = uncheckedDowncast<Closure>(base);
        FunctionKindData kind = closure->kind();
        if (closure->classData() || kind.callable() == CallableKind::Bound || kind.isConstructor)
            return false;
    } else if (base->type() != JSFunctionType || base->classInfo() != JSBoundFunction::info())
        return false;
    if (isFunctionOwnName(vm, uid))
        return false;
    if (structure->hasPolyProto() || structure->typeInfo().overridesGetPrototype())
        return false;
    JSValue prototype = structure->storedPrototype();
    return prototype.isObject() && storesAreOrdinaryAlongPrototypeChain(asObject(prototype)->structure());
}

bool hasOrdinaryDefineForName(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid)
{
    if (!base->isObject() || parseIndex(*uid) || !structure->isStructureExtensible() || structure->isCompiledStructure())
        return false;
    if (base->type() == FinalObjectType)
        return true;
    if (isJSArray(base))
        return uid != vm.propertyNames->length.impl();
    if (structure->classInfoForCells() == ErrorInstance::info())
        return !ErrorInstance::isLazilyMaterialized(vm, uid);
    TypeInfo typeInfo = structure->typeInfo();
    return hasOrdinaryDefineOwnProperty(structure) && !typeInfo.overridesGetOwnPropertySlot() && !typeInfo.hasStaticPropertyTable();
}

static bool hasOrdinarySetForName(VM& vm, const StoreQuery& query)
{
    if (isJSArray(query.base))
        return hasOrdinaryDefineForName(vm, query.base, query.structureBefore, query.uid);
    return !query.structureBefore->typeInfo().overridesPut();
}

static OutOfLineStorageChange storageChangeOfCachedAddition(VM& vm, Structure* before, Structure* after, PropertyOffset offset, bool isFunction)
{
    OutOfLineStorageChange change = outOfLineStorageChange(before, after, offset);
    if (change == OutOfLineStorageChange::Stays)
        return change;
    if (change == OutOfLineStorageChange::Unsupported || (isJSFunctionType(before->typeInfo().type()) && !isFunction) || before->isCompiledStructure())
        return OutOfLineStorageChange::Unsupported;
    if (change == OutOfLineStorageChange::IsFirst || change == OutOfLineStorageChange::GrowsBesideIndexingHeader)
        return change;
    if (before->outOfLineCapacity() <= runtimeLimitValue(vm, LimitID::MaxCapacityGrownByStub))
        return change;
    countLimitHit(vm, LimitID::MaxCapacityGrownByStub);
    return OutOfLineStorageChange::Unsupported;
}

static bool isNeverCachedStore(VM& vm, const StoreQuery& query)
{
    Structure* structureBefore = query.structureBefore;
    if (isNeverCachedName(vm, query.uid))
        return true;
    if (isProxyType(structureBefore->typeInfo().type()) || structureBefore->needImpurePropertyWatchpoint())
        return true;
    if (structureBefore->isCompiledStructure())
        return false;
    if (!structureBefore->propertyAccessesAreCacheable() || structureBefore->typeInfo().prohibitsPropertyCaching())
        return true;
    return query.mode != PutMode::Define && isHostObjectOverridingPut(structureBefore);
}

static std::optional<PropertyAnswer> classifyStaticFieldStore(const StoreQuery& query)
{
    if (query.mode == PutMode::Define || !isCompiledClosure(query.base))
        return std::nullopt;
    const FieldData* field = StaticFields::findFieldForCache(query.globalObject->vm(), query.base, query.uid);
    if (!field)
        return std::nullopt;
    if (query.base->structure() != query.structureBefore)
        return uncacheableOnce(query);
    PropertyAnswer answer = ownAnswer(query, PropertyAnswerKind::Variable);
    answer.field = field;
    answer.variable = field->slot;
    return answer;
}

static std::optional<PropertyAnswer> classifyArrayLengthStore(const StoreQuery& query)
{
    if (query.mode == PutMode::Define || !hasCacheableArrayLength(query.globalObject->vm(), query.base, query.structureBefore, query.uid))
        return std::nullopt;
    // Storage that is shared with other arrays is copied before its length changes.
    if (isCopyOnWrite(query.structureBefore->indexingMode()))
        return std::nullopt;
    return ownAnswer(query, PropertyAnswerKind::ArrayLength);
}

static std::optional<PropertyAnswer> classifyReplacement(const StoreQuery& query)
{
    const PutPropertySlot& slot = query.slot;
    Structure* structure = query.structureBefore;
    if (!slot.isCacheablePut() || slot.type() != PutPropertySlot::ExistingProperty || slot.base() != query.base || query.base->structure() != structure)
        return std::nullopt;
    bool isDefinition = query.mode == PutMode::Define;
    if ((!isDefinition && isHostObjectOverridingPut(structure)) || structure->isImmutableProperty(query.uid))
        return std::nullopt;
    VM& vm = query.globalObject->vm();
    unsigned attributes = 0;
    PropertyOffset offset = slot.cachedOffset();
    if (structure->get(vm, query.uid, attributes) != offset)
        return std::nullopt;
    if (isDefinition ? attributes : attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return std::nullopt;
    structure->didCachePropertyReplacement(vm, offset);
    return ownAnswer(query, PropertyAnswerKind::Value, offset);
}

static std::optional<PropertyAnswer> classifyAddition(const StoreQuery& query)
{
    const PutPropertySlot& slot = query.slot;
    if (!slot.isCacheablePut() || slot.type() != PutPropertySlot::NewProperty || slot.base() != query.base)
        return std::nullopt;
    VM& vm = query.globalObject->vm();
    Structure* before = query.structureBefore;
    Structure* after = query.base->structure();
    if (before->isDictionary() || after->isDictionary() || before->mayBePrototype() || before->mayHaveImmutableProperties() || !after->propertyAccessesAreCacheable())
        return std::nullopt;
    // A bound closure leaves the Structure it shares with other closures at its first store.
    if (after->previousID() != before)
        return uncacheableOnce(query);
    unsigned attributes = 0;
    PropertyOffset offset = slot.cachedOffset();
    if (after->get(vm, query.uid, attributes) != offset || attributes)
        return std::nullopt;

    PropertyAnswer answer = ownAnswer(query, PropertyAnswerKind::Addition, offset);
    answer.newStructure = after;
    bool isStore = query.mode != PutMode::Define;
    bool isFunction = isStore && isFunctionWithOrdinaryPut(vm, query.base, before, query.uid);
    if (isStore) {
        if (!isFunction && !hasOrdinarySetForName(vm, query))
            return std::nullopt;
        ChainWalk walk = walkChainAbove(query);
        if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
            return uncacheableAfter(query, WTF::move(walk));
        answer.changeableObjects = WTF::move(walk.changeableObjects);
    }

    switch (storageChangeOfCachedAddition(vm, before, after, offset, isFunction)) {
    case OutOfLineStorageChange::Unsupported:
        return std::nullopt;
    case OutOfLineStorageChange::Stays:
        break;
    case OutOfLineStorageChange::GrowsBesideIndexingHeader:
        answer.storageHasIndexingHeader = true;
        [[fallthrough]];
    case OutOfLineStorageChange::IsFirst:
    case OutOfLineStorageChange::Doubled:
        answer.newStorageCapacity = Slot::storageCapacityAfterAdding(Slot::wordIndexFor(offset));
        break;
    }
    return answer;
}

static std::optional<PropertyAnswer> classifyOwnSetterCall(const StoreQuery& query)
{
    if (query.structureBefore->isCompiledStructure())
        return std::nullopt;
    unsigned attributes = 0;
    PropertyOffset offset = query.slot.cachedOffset();
    if (query.structureBefore->get(query.globalObject->vm(), query.uid, attributes) != offset || !(attributes & PropertyAttribute::Accessor))
        return std::nullopt;
    return ownAnswer(query, PropertyAnswerKind::Accessor, offset);
}

static std::optional<PropertyAnswer> classifySetterCall(const StoreQuery& query)
{
    if (query.mode == PutMode::Define || !query.slot.isCacheableSetter())
        return std::nullopt;
    JSObject* holder = query.slot.base();
    if (holder == query.base)
        return classifyOwnSetterCall(query);

    Structure* structureBefore = query.structureBefore;
    if (!structureBefore->isCompiledStructure()) {
        if (isCopyOnWrite(structureBefore->indexingMode()) || !structureProvesOwnAbsence(query.globalObject->vm(), query.base, structureBefore, query.uid, ChainWalkMode::MayAllocate))
            return std::nullopt;
    }
    ChainWalk walk = walkChainAbove(query);
    if (storeThroughChain(walk) != StoreThroughChain::CallsSetter || walk.holder != holder)
        return uncacheableAfter(query, WTF::move(walk));
    PropertyAnswer answer;
    answer.kind = PropertyAnswerKind::Accessor;
    answer.access = accessFor(query.mode);
    answer.offset = walk.offset;
    answer.holder = holder;
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static std::optional<PropertyAnswer> classifyCustomSetterCall(const StoreQuery& query)
{
    if (query.mode == PutMode::Define || !query.slot.isCustomAccessor() || !query.slot.isCacheableCustom())
        return std::nullopt;
    return classifyCustomSetter(query);
}

static std::optional<PropertyAnswer> classifyStoreDecidedByChain(const StoreQuery& query)
{
    if (query.mode == PutMode::Define || query.slot.isCacheablePut())
        return std::nullopt;
    if (!structureProvesOwnAbsence(query.globalObject->vm(), query.base, query.structureBefore, query.uid, ChainWalkMode::MayAllocate))
        return std::nullopt;
    return uncacheableAfter(query, walkChainAbove(query));
}

static PropertyAnswer classifyStoreToCompiledObject(const StoreQuery& query)
{
    PropertyAnswer answer = uncacheableOnce(query);
    if (query.base->structure()->isCompiledStructure()) {
        answer = classifyCompiledObjectStore(query, *uncheckedDowncast<Object>(query.base));
        if (answer.isCacheable())
            return answer;
    }
    if (Object::structureGuaranteesAbsence(query.structureBefore, query.uid)) {
        if (auto setterCall = classifySetterCall(query))
            return WTF::move(*setterCall);
    }
    return answer;
}

PropertyAnswer classifyStore(const StoreQuery& query)
{
    using StoreRule = std::optional<PropertyAnswer> (*)(const StoreQuery&);
    static constexpr StoreRule rulesForOrdinaryObject[] = {
        classifyArrayLengthStore,
        classifyReplacement,
        classifyAddition,
        classifySetterCall,
        classifyCustomSetterCall,
        classifyStoreDecidedByChain,
    };

    VM& vm = query.globalObject->vm();
    Structure* structureBefore = query.structureBefore;
    bool isCompiled = structureBefore->isCompiledStructure();
    if (structureBefore->isDictionary() && !isCompiled) {
        if (!isNeverCachedStore(vm, query)) {
            if (auto answer = classifyReplacement(query))
                return WTF::move(*answer);
        }
        return uncacheable(query, flattenOnce(vm, query.base));
    }
    if (isNeverCachedStore(vm, query))
        return uncacheable(query);
    if (auto answer = classifyStaticFieldStore(query))
        return WTF::move(*answer);
    if (isCompiled)
        return classifyStoreToCompiledObject(query);
    if (query.mode == PutMode::Define && !hasOrdinaryDefineForName(vm, query.base, structureBefore, query.uid))
        return uncacheable(query);

    for (StoreRule rule : rulesForOrdinaryObject) {
        if (auto answer = rule(query)) {
            ASSERT(answer->access == accessFor(query.mode));
            return WTF::move(*answer);
        }
    }
    return uncacheable(query);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
