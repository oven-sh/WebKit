/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "PropertyRun.h"
#include <algorithm>
#include <optional>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(maxFieldsInOrder + Structure::s_maxTransitionLengthForNonEvalPutById <= maxKeyOrderEntry, "Structure::addPropertyRunTransition declines a run before the extra properties have more keys than a key order can count");

struct PropertyRunStore {
    JSObject* object;
    Structure* structure;
    const PropertyRun& run;
    std::span<const EncodedJSValue> values;
    SiteCache cache;
};

struct ChainAboveRun {
    uint8_t slotFlags() const { return std::ranges::any_of(walks, &ChainWalk::dependsOnChain) ? Slot::dependsOnChain : 0; }

    Vector<ChainWalk, 4> walks;
    bool everyStoreCreatesOwnProperty { true };
    bool didFlatten { false };
};

static bool canStorePropertyRun(Structure* structure)
{
    return structure->typeInfo().type() == FinalObjectType
        && !structure->isCompiledStructure()
        && !structure->isDictionary()
        && structure->isStructureExtensible()
        && !structure->hasPolyProto()
        && !structure->mayBePrototype()
        && !structure->hasImmutableProperties()
        && !structure->hasListedImmutableProperties()
        && structure->propertyAccessesAreCacheable()
        && structure->transitionCountEstimate() <= Structure::s_maxTransitionLength;
}

static void fillPropertyRunCache(Instance* instance, SiteCache cache, const Slot& slot)
{
    instance->state->propertyCaches->siteCaches().fill(cache, slot);
}

// Single stores are right for every object. What is remembered follows from the Structure alone, so it holds as long as the Structure is the object's.
static void rememberRefusal(Instance* instance, SiteCache cache, Structure* structure)
{
    fillPropertyRunCache(instance, cache, Slot::createWithoutPayload(structure->id(), Slot::Kind::Uncacheable, 0));
}

static bool tryReplaceRun(Instance* instance, VM& vm, const PropertyRunStore& store)
{
    Structure* structure = store.structure;
    std::span names = store.run.names();
    PropertyOffset count = static_cast<PropertyOffset>(names.size());
    unsigned attributes = 0;
    PropertyOffset first = structure->get(vm, names[0].get(), attributes);
    if (!isValidOffset(first) || !isInlineOffset(first + count - 1))
        return false;
    for (PropertyOffset i = 0; i < count; ++i) {
        if (structure->get(vm, names[i].get(), attributes) != first + i || attributes)
            return false;
    }
    for (PropertyOffset i = 0; i < count; ++i) {
        structure->didCachePropertyReplacement(vm, first + i);
        store.object->putDirectOffset(vm, first + i, JSValue::decode(store.values[i]));
    }
    fillPropertyRunCache(instance, store.cache, Slot::createInStorage(structure->id(), Slot::Kind::Value, Slot::isOwn, nullptr, first));
    return true;
}

static bool hasAnyName(VM& vm, Structure* structure, const PropertyRun& run)
{
    return std::ranges::any_of(run.names(), [&](const RefPtr<UniquedStringImpl>& name) {
        return isValidOffset(structure->get(vm, name.get()));
    });
}

static ChainAboveRun walkChainAbove(Instance* instance, const PropertyRunStore& store)
{
    ChainAboveRun chain;
    for (const RefPtr<UniquedStringImpl>& name : store.run.names()) {
        chain.walks.append(walkChain({ instance->globalObject, store.structure, store.object, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, name.get()));
        chain.everyStoreCreatesOwnProperty &= storeThroughChain(chain.walks.last()) == StoreThroughChain::CreatesOwnProperty;
        chain.didFlatten |= chain.walks.last().didFlatten;
    }
    return chain;
}

static void fillPropertyRunCache(Instance* instance, const PropertyRunStore& store, const Slot& slot, const ChainAboveRun& chain)
{
    std::span names = store.run.names();
    Vector<SiteCaches::ChainDependence, 4> dependences(names.size(), [&](size_t index) {
        return SiteCaches::ChainDependence { names[index].get(), chain.walks[index].changeableObjects.span() };
    });
    instance->state->propertyCaches->siteCaches().fill(store.cache, slot, dependences.span());
}

// A change of the chain drops what is remembered.
static void rememberRefusal(Instance* instance, const PropertyRunStore& store, const ChainAboveRun& chain)
{
    fillPropertyRunCache(instance, store, Slot::createWithoutPayload(store.structure->id(), Slot::Kind::Uncacheable, chain.slotFlags()), chain);
}

static bool tryAddRun(Instance* instance, VM& vm, const PropertyRunStore& store)
{
    // The emitted code stores into the cell alone.
    Structure* before = store.structure;
    unsigned firstPropertyNumber = numberOfSlotsForMaxOffset(before->maxOffset(), before->inlineCapacity());
    if (firstPropertyNumber + store.run.size() > before->inlineCapacity()) {
        rememberRefusal(instance, store.cache, before);
        return false;
    }

    ChainAboveRun chain = walkChainAbove(instance, store);
    if (!chain.everyStoreCreatesOwnProperty) {
        // The walk gives another answer once a prototype is flattened.
        if (chain.didFlatten)
            instance->state->propertyCaches->siteCaches().didFailToFill(store.cache);
        else
            rememberRefusal(instance, store, chain);
        return false;
    }
    if (!store.object->tryPutDirectPropertyRun(vm, store.run, store.values)) {
        rememberRefusal(instance, store.cache, before);
        return false;
    }

    Structure* after = store.object->structure();
    std::optional<Slot> transition;
    if (before->typeInfo().inlineTypeFlags() == after->typeInfo().inlineTypeFlags())
        transition = Slot::tryCreateTransition(before->id(), Slot::isOwn | chain.slotFlags(), after->id(), offsetForPropertyNumber(firstPropertyNumber, before->inlineCapacity()), 0);
    if (transition)
        fillPropertyRunCache(instance, store, *transition, chain);
    else
        rememberRefusal(instance, store.cache, before);
    return true;
}

static bool tryAddRunToExtraProperties(Instance* instance, VM& vm, Object& object, const PropertyRun& run, std::span<const EncodedJSValue> values)
{
    Structure* structure = object.structure();
    if (object.isNotExtensible() || structure->mayBePrototype())
        return false;
    bool createsOwnProperties = std::ranges::all_of(run.names(), [&](const RefPtr<UniquedStringImpl>& name) {
        return !object.fieldNamed(*instance->state, name.get()) && storeCreatesOwnProperty(structure->realm(), structure, &object, name.get());
    });
    if (!createsOwnProperties)
        return false;
    JSObject* extraProperties = object.ensureExtraProperties(vm);
    if (!extraProperties)
        return false;
    for (auto& name : run.names())
        object.willAddExtraProperty(vm, name.get());
    if (!extraProperties->tryPutDirectPropertyRun(vm, run, values))
        return false;
    object.didChangeExtraProperties(vm);
    return true;
}

// The emitted code has counted the miss, and the site is due for a refill.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryPutProperties, size_t, (Instance* instance, SiteCache cache, EncodedJSValue encodedObject, uint32_t request, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryPutProperties);
    JSValue base = JSValue::decode(encodedObject);
    Object* compiledObject = compiledObjectOrNull(base);
    if (!compiledObject && !base.isCell())
        OPERATION_RETURN(scope, 0);
    if (!compiledObject && !canStorePropertyRun(base.asCell()->structure())) {
        rememberRefusal(instance, cache, base.asCell()->structure());
        OPERATION_RETURN(scope, 0);
    }
    const PropertyRun* run = propertyRunAt(instance, request);
    if (!run)
        OPERATION_RETURN(scope, 0);
    std::span<const EncodedJSValue> valuesOfRun { values, run->size() };
    if (compiledObject)
        OPERATION_RETURN(scope, static_cast<size_t>(tryAddRunToExtraProperties(instance, vm, *compiledObject, *run, valuesOfRun)));

    JSObject* object = asObject(base);
    PropertyRunStore store { object, object->structure(), *run, valuesOfRun, cache };
    if (tryReplaceRun(instance, vm, store))
        OPERATION_RETURN(scope, 1);
    if (hasAnyName(vm, store.structure, *run)) {
        rememberRefusal(instance, cache, store.structure);
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, static_cast<size_t>(tryAddRun(instance, vm, store)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
