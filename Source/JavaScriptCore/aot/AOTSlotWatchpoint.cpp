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

SlotWatchpoint::SlotWatchpoint()
    : Watchpoint(Watchpoint::Type::AOTSlot)
    , m_owner(nullptr)
{
}

void SlotWatchpoint::initialize(Data* owner, const ObjectPropertyCondition& key, unsigned slotIndex)
{
    RELEASE_ASSERT(key.watchingRequiresStructureTransitionWatchpoint());
    RELEASE_ASSERT(!key.watchingRequiresReplacementWatchpoint());
    m_owner = owner;
    m_key = key;
    m_slotIndex = slotIndex;
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
    data->slots[m_slotIndex].clear();
    data->slotEpoch++;
}

static unsigned indexOf(Data* data, Slot* slot)
{
    ASSERT(slot >= data->slots && slot < data->slots + data->numSlots);
    return slot - data->slots;
}

// Of an object whose properties were fixed (JSObject::fixProperties()): what it had then, it has, where it had it. If it cannot be
// given more either, what it does not have it never will. There is nothing to watch for.
static bool holdsForGood(const ObjectPropertyCondition& condition)
{
    Structure* structure = condition.object()->structure();
    if (!structure->heirsMayOverrideReadOnlyProperties())
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
    if (!conditions.isValid())
        return false;
    unsigned numberToWatch = 0;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (holdsForGood(condition))
            continue;
        if (!condition.isWatchable(PropertyCondition::MakeNoChanges))
            return false;
        numberToWatch++;
    }

    unsigned index = indexOf(data, slot);
    if (!numberToWatch) {
        stopWatching(data, slot);
        return true;
    }

    if (!data->watchpoints)
        data->watchpoints = new SlotWatchpointMap;
    FixedVector<SlotWatchpoint> watchpoints(numberToWatch);
    unsigned i = 0;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (holdsForGood(condition))
            continue;
        auto& watchpoint = watchpoints[i++];
        watchpoint.initialize(data, condition, index);
        watchpoint.install(vm);
    }
    data->watchpoints->set(index, WTF::move(watchpoints));
    return true;
}

void stopWatching(Data* data, Slot* slot)
{
    if (data->watchpoints)
        data->watchpoints->remove(indexOf(data, slot));
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
