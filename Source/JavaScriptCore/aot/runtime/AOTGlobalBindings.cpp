/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGlobalBindings.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTProgramIdentifiers.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"
#include <bit>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(GlobalBindings);

GlobalBindings::GlobalBindings(State& state)
    : m_state(state)
    , m_records(unsafeMakeSpan(state.instance.runtime.counters.globalBindings, state.tables.globalBindings().size()))
    , m_names(state.tables.globalBindings())
    , m_chainDependentOwner(state.chainDependents->addOwner(*this))
{
    RELEASE_ASSERT(m_records.data());
    state.caches->add(CacheKind::WeakByKey, "GlobalBindings"_s, *this);
}

GlobalBindings::~GlobalBindings() = default;

GlobalBinding& GlobalBindings::at(uint32_t number)
{
    RELEASE_ASSERT(number < m_records.size());
    return m_records[number];
}

Identifier GlobalBindings::nameOf(uint32_t number) const
{
    RELEASE_ASSERT(number < m_names.size());
    return m_state.instance.vm->aotState().identifiers->identifier(identifierNumberOf(number));
}

bool GlobalBindings::tryWatch(VM& vm, uint32_t number, JSObject* holder, JSValue expected)
{
    // A free-name site with a cached store compares no Structure and fires no watchpoint, and each later Structure of the holder starts with a valid replacement set.
    if (m_names[number] & isStoredByNameFlag || m_identifiersStoredThrough.contains(identifierNumberOf(number)) || !holder->structure()->propertyAccessesAreCacheable())
        return false;
    ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, nameOf(number).impl());
    if (!condition || condition.requiredValue() != expected || !condition.isWatchable(PropertyCondition::EnsureWatchability))
        return false;

    auto watch = makeUnique<Watch>(*this, number);
    watch->watchpoint.watch(vm, condition, didFire, watch.get());
    m_watches.set(number, WTF::move(watch));
    return true;
}

void GlobalBindings::didFire(VM&, void* context)
{
    // A watchpoint is not destroyed while it fires. The next watch of the record or the next sweep frees it.
    Watch& watch = *static_cast<Watch*>(context);
    watch.hasFired = true;
    watch.owner.at(watch.number).constant = 0;
    watch.owner.m_writtenSinceCollection.recordAll();
}

bool GlobalBindings::fileConstant(VM& vm, uint32_t number, JSObject* holderToWatch, JSValue value)
{
    RELEASE_ASSERT(value);
    RELEASE_ASSERT_WITH_MESSAGE(!m_isStoredThrough.get(number), "A store through a global binding has a record of its own.");
    if (!holderToWatch)
        m_watches.remove(number);
    else if (!tryWatch(vm, number, holderToWatch, value))
        return false;

    // The global object or its lexical environment holds the value for as long as it stands here, so the record is no root.
    m_isFiledAsAbsent.clear(number);
    setConstant(number, value);
    return true;
}

void GlobalBindings::setConstant(uint32_t number, JSValue value)
{
    at(number).constant = JSValue::encode(value);
    if (!m_isListedAsConstant.set(number))
        m_filedConstants.append(number);
}

void GlobalBindings::fileAbsence(VM& vm, uint32_t number, std::span<JSObject* const> objectsAboveGlobalObject)
{
    RELEASE_ASSERT(answersUndefinedWhereAbsent(number));
    ChainObjects objects;
    objects.append(m_state.instance.globalObject);
    objects.append(objectsAboveGlobalObject);
    // Only an object that may be a prototype reports its changes. The global object becomes one here, which costs it a Structure.
    for (JSObject* object : objects) {
        if (object->mayBePrototype()) [[likely]]
            continue;
        DeferGCForAWhile deferGC(vm);
        object->didBecomePrototype(vm);
    }
    m_state.chainDependents->add(objects.span(), { nameOf(number).impl(), number, m_chainDependentOwner });

    m_watches.remove(number);
    m_isFiledAsAbsent.set(number);
    setConstant(number, jsUndefined());
}

void GlobalBindings::dropChainDependent(VM&, uint32_t number, UniquedStringImpl*)
{
    if (!m_isFiledAsAbsent.get(number))
        return;
    m_isFiledAsAbsent.clear(number);
    at(number).constant = 0;
}

bool GlobalBindings::hasChainDependent(VM&, uint32_t number, UniquedStringImpl*) const
{
    return m_isFiledAsAbsent.get(number);
}

void GlobalBindings::fileLocation(uint32_t number, Structure* structure, int64_t location)
{
    RELEASE_ASSERT(location);
    GlobalBinding& record = at(number);
    if (!record.structureID)
        m_filedLocations.append(number);
    record.structureID = structure->id().bits();
    record.lexicalBindingEpoch = m_state.instance.globalObject->globalLexicalBindingEpoch();
    record.location = location;
    m_writtenSinceCollection.recordAll();
}

void GlobalBindings::didFileStore(uint32_t number)
{
    RELEASE_ASSERT(!at(number).constant);
    m_isStoredThrough.set(number);
    m_identifiersStoredThrough.add(identifierNumberOf(number));
}

void GlobalBindings::dropConstants()
{
    for (uint32_t number : m_filedConstants) {
        at(number).constant = 0;
        m_isListedAsConstant.quickClear(number);
        m_isFiledAsAbsent.clear(number);
    }
    m_filedConstants.shrink(0);
}

static void forgetLocation(GlobalBinding& record)
{
    record.structureID = 0;
    record.lexicalBindingEpoch = 0;
    record.location = 0;
}

void GlobalBindings::dropLocations()
{
    for (uint32_t number : m_filedLocations)
        forgetLocation(at(number));
    m_filedLocations.shrink(0);
}

// The number of a dead Structure is given to a later one, which the global object may get.
size_t GlobalBindings::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfRemovedEntries = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        numberOfRemovedEntries += m_filedLocations.removeAllMatching([&](uint32_t number) {
            GlobalBinding& record = at(number);
            if (vm.heap.isMarked(std::bit_cast<StructureID>(record.structureID).decode()))
                return false;
            forgetLocation(record);
            return true;
        });
        m_watches.removeIf([&](auto& entry) {
            if (!entry.value->hasFired)
                return false;
            ++numberOfRemovedEntries;
            return true;
        });
    });
    return numberOfRemovedEntries;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
