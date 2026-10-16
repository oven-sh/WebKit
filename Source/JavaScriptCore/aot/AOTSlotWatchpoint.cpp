/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotWatchpoint.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "CodeBlockInlines.h"
#include "JSCellInlines.h"
#include "StructureInlines.h"

namespace JSC { namespace AOT {

AssumptionWatchpoint::AssumptionWatchpoint()
    : Watchpoint(Watchpoint::Type::AOTAssumption)
{
}

void AssumptionWatchpoint::install(InlineWatchpointSet& set, uint32_t& flag)
{
    flag = set.isStillValid();
    if (!flag)
        return;
    m_flag = &flag;
    set.add(this);
}

void AssumptionWatchpoint::fireInternal(VM&, const FireDetail&)
{
    *m_flag = 0;
}

TransitionWatchpoint::TransitionWatchpoint()
    : Watchpoint(Watchpoint::Type::AOTTransition)
{
}

bool TransitionWatchpoint::install(JSObject* object, Callback callback, void* context)
{
    Structure* structure = object->structure();
    if (structure->isDictionary() || structure->transitionWatchpointSetHasBeenInvalidated())
        return false;
    m_callback = callback;
    m_context = context;
    structure->addTransitionWatchpoint(this);
    return true;
}

void TransitionWatchpoint::fireInternal(VM& vm, const FireDetail&)
{
    m_callback(vm, m_context);
}

void BufferAccessorWatchpoint::watch(VM& vm, const ObjectPropertyCondition& condition, Instance& instance, unsigned row)
{
    RELEASE_ASSERT(!m_instance);
    m_instance = &instance;
    m_row = row;
    initialize(condition);
    install(vm);
}

void BufferAccessorWatchpoint::handleFire(VM&, const FireDetail&)
{
    m_instance->bufferAccessorStructureIDs[m_row] = Instance::bufferAccessorIsNotLookedFor;
    m_instance->bufferAccessors[m_row] = nullptr;
}

void AsyncLocalStorageWatchpoint::watch(VM& vm, const ObjectPropertyCondition& condition, Instance& instance)
{
    RELEASE_ASSERT(!m_instance);
    m_instance = &instance;
    initialize(condition);
    install(vm);
}

void AsyncLocalStorageWatchpoint::handleFire(VM&, const FireDetail&)
{
    m_instance->asyncLocalStorageStructureIDs[0] = 0;
    m_instance->asyncLocalStorageStructureIDs[1] = 0;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(GlobalPropertyWatchpoint);

void GlobalPropertyWatchpoint::watch(VM& vm, const ObjectPropertyCondition& condition, Typed::GlobalPropertyCache& cache, std::unique_ptr<GlobalPropertyWatchpoint>* sibling)
{
    RELEASE_ASSERT(!m_cache);
    m_cache = &cache;
    m_sibling = sibling;
    initialize(condition);
    install(vm);
}

void GlobalPropertyWatchpoint::handleFire(VM&, const FireDetail&)
{
    *std::exchange(m_cache, nullptr) = { 0, 0, !!m_sibling, 0 };
    if (m_sibling)
        *m_sibling = nullptr;
}

SlotWatchpoint::SlotWatchpoint()
    : Watchpoint(Watchpoint::Type::AOTSlot)
    , m_owner(nullptr)
{
}

void SlotWatchpoint::initialize(Data* owner, const ObjectPropertyCondition& key, Slot* slot)
{
    RELEASE_ASSERT(key.watchingRequiresStructureTransitionWatchpoint());
    RELEASE_ASSERT(!key.watchingRequiresReplacementWatchpoint());
    m_owner = owner;
    m_key = key;
    m_slot = slot;
}

static bool interceptsNoStore(const ObjectPropertyCondition& key)
{
    Structure* structure = key.object()->structure();
    return !structure->isDictionary() && !structure->hasPolyProto() && structure->transitionWatchpointSetIsStillValid() && !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto()
        && !(structure->transitionPropertyAttributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue) && structure->storedPrototypeObject() == key.prototype();
}

static NEVER_INLINE bool isAbsenceDeterminedByStructure(const ObjectPropertyCondition& key)
{
    return key.kind() == PropertyCondition::Absence && Typed::lacksWhatItsStructureLacks(key.object()->vm(), key.object(), key.uid(), false);
}

static ALWAYS_INLINE bool isWatchable(const ObjectPropertyCondition& key, PropertyCondition::WatchabilityEffort effort)
{
    return key.isWatchable(effort) || (isAbsenceDeterminedByStructure(key) && key.isWatchableAssumingImpurePropertyWatchpoint(effort));
}

void SlotWatchpoint::install(VM& vm)
{
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation("SlotWatchpoint", "installed");
    RELEASE_ASSERT(m_key.uid() ? isWatchable(m_key, PropertyCondition::MakeNoChanges) : interceptsNoStore(m_key));
    m_key.object()->structure()->addTransitionWatchpoint(this);
}

void SlotWatchpoint::fireInternal(VM& vm, const FireDetail&)
{
    if (m_key.uid() ? isWatchable(m_key, PropertyCondition::EnsureWatchability) : interceptsNoStore(m_key)) {
        install(vm);
        return;
    }

    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation("SlotWatchpoint", "cleared-slot");
    Data* data = m_owner;
    m_slot->clear();
    data->slotEpoch++;
}

static bool hasPermanentOffset(VM& vm, Structure* structure, PropertyOffset offset)
{
    unsigned propertyNumber = 0;
    bool result = false;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        if (entry.offset() == offset) {
            result = offset == offsetForPropertyNumber(propertyNumber, structure->inlineCapacity());
            return false;
        }
        propertyNumber++;
        return entry.attributes() & PropertyAttribute::DontDelete || PropertyName(entry.key()).isPrivateName();
    });
    return result;
}

static bool isImmutablePrototype(Structure* structure)
{
    return structure->inheritorsMayOverrideReadOnlyProperties() && !structure->isStructureExtensible() && structure->typeInfo().isImmutablePrototypeExoticObject();
}

static bool isPermanentlyValid(VM& vm, const ObjectPropertyCondition& condition)
{
    Structure* structure = condition.object()->structure();
    if (!structure->inheritorsMayOverrideReadOnlyProperties())
        return false;
    switch (condition.kind()) {
    case PropertyCondition::Presence:
        return condition.attributes() & PropertyAttribute::DontDelete && condition.attributes() & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue) && hasPermanentOffset(vm, structure, condition.offset());
    case PropertyCondition::Absence:
    case PropertyCondition::AbsenceOfSetEffect:
        return isImmutablePrototype(structure);
    default:
        return false;
    }
}

bool watchConditions(VM& vm, Data* data, Slot* slot, std::span<const ObjectPropertyCondition> conditions)
{
    if (SharedData::contains(slot))
        return false;
    Vector<ObjectPropertyCondition, 16> watchedConditions;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (isPermanentlyValid(vm, condition))
            continue;
        if (!condition.isWatchable(PropertyCondition::MakeNoChanges)) {
            if (auto* closure = dynamicDowncast<Typed::Closure>(condition.object()))
                closure->ensureClassObject(vm);
            if (!isAbsenceDeterminedByStructure(condition) || !condition.isWatchableAssumingImpurePropertyWatchpoint(PropertyCondition::MakeNoChanges))
                return false;
        }
        watchedConditions.append(condition);
    }

    if (watchedConditions.isEmpty()) {
        stopWatching(data, slot);
        return true;
    }

    if (!data->watchpoints)
        data->watchpoints = new SlotWatchpointMap;
    FixedVector<SlotWatchpoint> watchpoints(watchedConditions.size());
    unsigned i = 0;
    for (const ObjectPropertyCondition& condition : watchedConditions) {
        auto& watchpoint = watchpoints[i++];
        watchpoint.initialize(data, condition, slot);
        watchpoint.install(vm);
    }
    data->watchpoints->set(slot, WTF::move(watchpoints));
    return true;
}

bool watchForStores(VM& vm, Data* data, Slot* slot, std::span<JSObject* const> prototypes)
{
    if (SharedData::contains(slot))
        return false;
    if (prototypes.empty()) {
        stopWatching(data, slot);
        return true;
    }
    auto keyAt = [&](size_t i) {
        return ObjectPropertyCondition::absenceOfSetEffectWithoutBarrier(prototypes[i], nullptr, i + 1 < prototypes.size() ? prototypes[i + 1] : nullptr);
    };
    Vector<size_t, 4> watched;
    for (size_t i = 0; i < prototypes.size(); ++i) {
        if (!interceptsNoStore(keyAt(i)))
            return false;
        if (!isImmutablePrototype(prototypes[i]->structure()))
            watched.append(i);
    }
    if (watched.isEmpty()) {
        stopWatching(data, slot);
        return true;
    }
    if (!data->watchpoints)
        data->watchpoints = new SlotWatchpointMap;
    FixedVector<SlotWatchpoint> watchpoints(watched.size());
    for (size_t i = 0; i < watched.size(); ++i) {
        watchpoints[i].initialize(data, keyAt(watched[i]), slot);
        watchpoints[i].install(vm);
    }
    data->watchpoints->set(slot, WTF::move(watchpoints));
    return true;
}

void stopWatching(Data* data, Slot* slot)
{
    bool wasWatched = data->watchpoints && data->watchpoints->remove(slot);
    if (Options::validateAOTTypedFacts() && data->instance) [[unlikely]]
        data->instance->didStopWatching(data, slot, wasWatched);
}

void moveWatching(Data* data, Slot* from, Slot* to)
{
    if (!data->watchpoints)
        return;
    auto watchpoints = data->watchpoints->take(from);
    if (watchpoints.isEmpty())
        return;
    for (auto& watchpoint : watchpoints)
        watchpoint.setSlot(to);
    data->watchpoints->set(to, WTF::move(watchpoints));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
