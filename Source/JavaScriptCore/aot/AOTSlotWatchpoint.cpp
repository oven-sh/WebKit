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

void SlotWatchpoint::initialize(CodeBlock* owner, const ObjectPropertyCondition& key, unsigned slotIndex)
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
    if (m_owner->isPendingDestruction())
        return;

    // The object has a new structure, which need not make a difference to the property.
    if (m_key.isWatchable(PropertyCondition::EnsureWatchability)) {
        install(vm);
        return;
    }

    // The others stay until the slot is filled again or a collection comes by: this one is being walked over by its set.
    if (Data* data = m_owner->aotData()) {
        data->slots[m_slotIndex].clear();
        data->slotEpoch++;
    }
}

static unsigned indexOf(Data* data, Slot* slot)
{
    ASSERT(slot >= data->slots && slot < data->slots + data->numSlots);
    return slot - data->slots;
}

bool watchConditions(VM& vm, CodeBlock* codeBlock, Slot* slot, const ObjectPropertyConditionSet& conditions)
{
    if (!conditions.isValid())
        return false;
    for (const ObjectPropertyCondition& condition : conditions) {
        if (!condition.isWatchable(PropertyCondition::MakeNoChanges))
            return false;
    }

    Data* data = codeBlock->aotData();
    unsigned index = indexOf(data, slot);
    if (!conditions.size()) {
        stopWatching(codeBlock, slot);
        return true;
    }

    if (!data->watchpoints)
        data->watchpoints = new SlotWatchpointMap;
    FixedVector<SlotWatchpoint> watchpoints(conditions.size());
    unsigned i = 0;
    for (const ObjectPropertyCondition& condition : conditions) {
        auto& watchpoint = watchpoints[i++];
        watchpoint.initialize(codeBlock, condition, index);
        watchpoint.install(vm);
    }
    data->watchpoints->set(index, WTF::move(watchpoints));
    return true;
}

void stopWatching(CodeBlock* codeBlock, Slot* slot)
{
    Data* data = codeBlock->aotData();
    if (data->watchpoints)
        data->watchpoints->remove(indexOf(data, slot));
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
