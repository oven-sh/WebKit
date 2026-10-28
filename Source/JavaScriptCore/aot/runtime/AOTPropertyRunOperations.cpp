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
#include "AOTObjectModel.h"
#include "AOTOperationScope.h"
#include "AOTProgramTables.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "AOTVector.h"
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
    // Null at a site that probes no cache and calls the operation every time. Nothing is remembered for it.
    SiteCache cache;
    bool isDefinition;
};

struct ChainAboveRun {
    uint8_t slotFlags() const { return std::ranges::any_of(walks, &ChainWalk::dependsOnChain) ? Slot::dependsOnChain : 0; }

    Vector<ChainWalk, 4> walks;
    bool everyStoreCreatesOwnProperty { true };
    bool didFlatten { false };
};

const PropertyRun* propertyRunAt(Instance* instance, uint32_t number)
{
    const PropertyRun*& run = instance->runtime.literals.propertyRuns[number];
    if (run) [[likely]]
        return run;
    std::span<const uint32_t> row = instance->tables().propertyRunRow(number);
    RELEASE_ASSERT(row.size() == PropertyRunHeader::decode(row[0]).count + 1u);
    std::span<const uint32_t> numbers = row.subspan(1);
    Vector<UniquedStringImpl*, 16> names(numbers.size(), [&](size_t index) {
        return identifierAt(instance, numbers[index]).impl();
    });
    run = instance->vm->propertyRuns.intern(names.span());
    return run;
}

static bool canPutPropertiesDirectly(Structure* structure)
{
    return structure->typeInfo().type() == FinalObjectType
        && !structure->isCompiledStructure()
        && structure->isStructureExtensible()
        && !structure->hasPolyProto()
        && !structure->mayBePrototype()
        && !structure->hasImmutableProperties()
        && !structure->hasListedImmutableProperties();
}

// A definition replaces a property that is configurable, whatever it was, and is refused by one that is not.
static bool canDefinePropertiesOneByOne(Structure* structure)
{
    return canPutPropertiesDirectly(structure) && !structure->hasNonConfigurableProperties();
}

static bool canStorePropertyRun(Structure* structure)
{
    return canPutPropertiesDirectly(structure)
        && !structure->isDictionary()
        && structure->propertyAccessesAreCacheable()
        && structure->transitionCountEstimate() <= Structure::s_maxTransitionLength;
}

// A change of the chain drops what is remembered. Without a chain it follows from the Structure alone, so it holds as long as the Structure is the object's.
static void fillPropertyRunCache(Instance* instance, SiteCache cache, const Slot& slot, const PropertyRun* run = nullptr, const ChainAboveRun& chain = { })
{
    if (!cache)
        return;
    Vector<SiteCaches::ChainDependence, 4> dependences(chain.walks.size(), [&](size_t index) {
        return SiteCaches::ChainDependence { run->names()[index].get(), chain.walks[index].changeableObjects.span() };
    });
    instance->state->propertyCaches->siteCaches().fill(cache, slot, dependences.span());
}

// Single stores are right for every object.
static void rememberRefusal(Instance* instance, SiteCache cache, Structure* structure, const PropertyRun* run = nullptr, const ChainAboveRun& chain = { })
{
    fillPropertyRunCache(instance, cache, Slot::createWithoutPayload(structure->id(), Slot::Kind::Uncacheable, chain.slotFlags()), run, chain);
}

// A store and a definition do the same to a data property that is writable, enumerable and configurable.
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

static bool tryAddRun(Instance* instance, VM& vm, const PropertyRunStore& store)
{
    // The code that probes a site cache stores into the cell alone.
    Structure* before = store.structure;
    unsigned firstPropertyNumber = numberOfSlotsForMaxOffset(before->maxOffset(), before->inlineCapacity());
    if (store.cache && firstPropertyNumber + store.run.size() > before->inlineCapacity()) {
        rememberRefusal(instance, store.cache, before);
        return false;
    }

    // CreateDataProperty does not look at the chain, so what is remembered for a definition depends on nothing above the object.
    ChainAboveRun chain = store.isDefinition ? ChainAboveRun { } : walkChainAbove(instance, store);
    if (!chain.everyStoreCreatesOwnProperty) {
        // The walk gives another answer once a prototype is flattened.
        if (!chain.didFlatten)
            rememberRefusal(instance, store.cache, before, &store.run, chain);
        else if (store.cache)
            instance->state->propertyCaches->siteCaches().didFailToFillWithRun(store.cache);
        return false;
    }
    if (!store.object->tryPutDirectPropertyRun(vm, store.run, store.values)) {
        rememberRefusal(instance, store.cache, before);
        return false;
    }
    if (!store.cache)
        return true;

    Structure* after = store.object->structure();
    std::optional<Slot> transition;
    if (before->typeInfo().inlineTypeFlags() == after->typeInfo().inlineTypeFlags())
        transition = Slot::tryCreateTransition(before->id(), Slot::isOwn | chain.slotFlags(), after->id(), offsetForPropertyNumber(firstPropertyNumber, before->inlineCapacity()), 0);
    if (transition)
        fillPropertyRunCache(instance, store.cache, *transition, &store.run, chain);
    else
        rememberRefusal(instance, store.cache, before);
    return true;
}

static bool tryAddRunToExtraProperties(Instance* instance, VM& vm, Object& object, const PropertyRunStore& store)
{
    // The layout says which names are fields, and the Structure says which layout it is.
    bool namesField = std::ranges::any_of(store.run.names(), [&](const RefPtr<UniquedStringImpl>& name) {
        return !!object.fieldNamed(*instance->state, name.get());
    });
    if (namesField) {
        rememberRefusal(instance, store.cache, store.structure);
        return false;
    }
    if (object.isNotExtensible() || store.structure->mayBePrototype())
        return false;
    bool createsOwnProperties = store.isDefinition || std::ranges::all_of(store.run.names(), [&](const RefPtr<UniquedStringImpl>& name) {
        return storeCreatesOwnProperty(store.structure->realm(), store.structure, &object, name.get());
    });
    if (!createsOwnProperties)
        return false;
    JSObject* extraProperties = object.ensureExtraProperties(vm);
    if (!extraProperties)
        return false;
    for (auto& name : store.run.names())
        object.willAddExtraProperty(vm, name.get());
    if (!extraProperties->tryPutDirectPropertyRun(vm, store.run, store.values))
        return false;
    object.didChangeExtraProperties(vm);
    return true;
}

// At a site with a cache the emitted code has counted the miss, and the site is due for a refill.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryPutProperties, size_t, (Instance* instance, SiteCache cache, EncodedJSValue encodedObject, uint32_t request, const EncodedJSValue* values, uint32_t isDefinition))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryPutProperties);
    JSValue base = JSValue::decode(encodedObject);
    Object* compiledObject = compiledObjectOrNull(base);
    if (!compiledObject && !base.isCell())
        OPERATION_RETURN(scope, 0);
    bool canStoreRun = compiledObject || canStorePropertyRun(base.asCell()->structure());
    if (!canStoreRun && !(isDefinition && canDefinePropertiesOneByOne(base.asCell()->structure()))) {
        rememberRefusal(instance, cache, base.asCell()->structure());
        OPERATION_RETURN(scope, 0);
    }
    const PropertyRun* run = propertyRunAt(instance, request);
    if (!run)
        OPERATION_RETURN(scope, 0);
    JSObject* object = asObject(base);
    if (!canStoreRun) {
        for (size_t index = 0; index < run->size(); ++index)
            object->putDirect(vm, PropertyName(run->names()[index].get()), JSValue::decode(values[index]));
        OPERATION_RETURN(scope, 1);
    }
    PropertyRunStore store { object, object->structure(), *run, { values, run->size() }, cache, !!isDefinition };
    if (compiledObject)
        OPERATION_RETURN(scope, static_cast<size_t>(tryAddRunToExtraProperties(instance, vm, *compiledObject, store)));

    if (tryReplaceRun(instance, vm, store))
        OPERATION_RETURN(scope, 1);
    bool hasAnyName = std::ranges::any_of(run->names(), [&](const RefPtr<UniquedStringImpl>& name) {
        return isValidOffset(store.structure->get(vm, name.get()));
    });
    if (hasAnyName) {
        rememberRefusal(instance, cache, store.structure);
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, static_cast<size_t>(tryAddRun(instance, vm, store)));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
