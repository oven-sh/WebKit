/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "ObjectPropertyCondition.h"
#include "PackedCellPtr.h"
#include "Watchpoint.h"
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>

namespace JSC {

class CodeBlock;
class ObjectPropertyConditionSet;

namespace AOT {

struct Data;
struct Slot;

// An inline cache that rests on more than the structure the code checks: that a property is still where it was found on the
// prototype chain, or that there still is none. One of these watches each object involved, and empties the cache when what it
// watches for stops being so. The code never hears of them.
class SlotWatchpoint final : public Watchpoint {
public:
    SlotWatchpoint();

    void initialize(Data*, const ObjectPropertyCondition&, Slot*);
    void setSlot(Slot* slot) { m_slot = slot; }
    void install(VM&);
    void fireInternal(VM&, const FireDetail&);

    const ObjectPropertyCondition& key() const LIFETIME_BOUND { return m_key; }

private:
    Data* m_owner;
    Slot* m_slot { nullptr }; // One of the owner's own, or of one of its PolymorphicSlots.
    ObjectPropertyCondition m_key;
};

using SlotWatchpointMap = UncheckedKeyHashMap<Slot*, FixedVector<SlotWatchpoint>>;

// Has the conditions watched on behalf of the slot, in place of whatever was. False, and nothing is watched, if that cannot be
// done, in which case the slot must not be filled.
bool watchConditions(VM&, Data*, Slot*, const ObjectPropertyConditionSet&);
void stopWatching(Data*, Slot*);
void moveWatching(Data*, Slot* from, Slot* to); // What the one had is in the other now.

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
