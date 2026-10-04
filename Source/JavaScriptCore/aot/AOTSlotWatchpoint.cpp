/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotWatchpoint.h"

#if ENABLE(AOT)

#include "AOTRuntime.h"
#include "CodeBlockInlines.h"
#include "JSCellInlines.h"
#include "ObjectPropertyConditionSet.h"
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

void SlotWatchpoint::install(VM&)
{
    RELEASE_ASSERT(m_key.isWatchable(PropertyCondition::MakeNoChanges));
    m_key.object()->structure()->addTransitionWatchpoint(this);
}

void SlotWatchpoint::fireInternal(VM& vm, const FireDetail&)
{
    if (m_key.isWatchable(PropertyCondition::EnsureWatchability)) {
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
        return !structure->isStructureExtensible() && structure->typeInfo().isImmutablePrototypeExoticObject();
    default:
        return false;
    }
}

bool watchConditions(VM& vm, Data* data, Slot* slot, const ObjectPropertyConditionSet& conditions)
{
    if (!conditions.isValid() || SharedData::contains(slot))
        return false;
    Vector<ObjectPropertyCondition, 4> watchedConditions;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (isPermanentlyValid(vm, condition))
            continue;
        if (!condition.isWatchable(PropertyCondition::MakeNoChanges))
            return false;
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

void stopWatching(Data* data, Slot* slot)
{
    if (data->watchpoints)
        data->watchpoints->remove(slot);
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
