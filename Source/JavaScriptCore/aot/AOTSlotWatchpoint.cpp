/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotWatchpoint.h"

#if ENABLE(FTL_JIT)

#include "AOTRuntime.h"
#include "CodeBlockInlines.h"
#include "JSCellInlines.h"
#include "ObjectPropertyConditionSet.h"
#include "StructureInlinesLight.h"

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
    // The object has a new structure, which need not make a difference to the property.
    if (m_key.isWatchable(PropertyCondition::EnsureWatchability)) {
        install(vm);
        return;
    }

    // The others stay until the slot is filled again or a collection comes by: this one is being walked over by its set.
    Data* data = m_owner;
    m_slot->clear();
    data->slotEpoch++;
}

// Of an object whose properties were fixed (JSObject::makePropertiesImmutable()): what it had then, it has, where it had it. If it cannot be
// given more either, what it does not have it never will. There is nothing to watch for.
static bool isPermanentlyValid(const ObjectPropertyCondition& condition)
{
    Structure* structure = condition.object()->structure();
    if (!structure->inheritorsMayOverrideReadOnlyProperties())
        return false;
    switch (condition.kind()) {
    case PropertyCondition::Presence:
        return condition.attributes() & PropertyAttribute::DontDelete && condition.attributes() & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue);
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
    unsigned numberToWatch = 0;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (isPermanentlyValid(condition))
            continue;
        if (!condition.isWatchable(PropertyCondition::MakeNoChanges))
            return false;
        numberToWatch++;
    }

    if (!numberToWatch) {
        stopWatching(data, slot);
        return true;
    }

    if (!data->watchpoints)
        data->watchpoints = new SlotWatchpointMap;
    FixedVector<SlotWatchpoint> watchpoints(numberToWatch);
    unsigned i = 0;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (isPermanentlyValid(condition))
            continue;
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
    // (They stay where they are: what they watch has hold of them.)
    auto watchpoints = data->watchpoints->take(from);
    if (watchpoints.isEmpty())
        return;
    for (auto& watchpoint : watchpoints)
        watchpoint.setSlot(to);
    data->watchpoints->set(to, WTF::move(watchpoints));
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
