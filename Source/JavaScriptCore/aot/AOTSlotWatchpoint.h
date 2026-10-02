/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
    Slot* m_slot { nullptr };
    ObjectPropertyCondition m_key;
};

class AssumptionWatchpoint final : public Watchpoint {
public:
    AssumptionWatchpoint();

    void install(InlineWatchpointSet&, uint32_t& flag);
    void fireInternal(VM&, const FireDetail&);

private:
    uint32_t* m_flag { nullptr };
};

using SlotWatchpointMap = UncheckedKeyHashMap<Slot*, FixedVector<SlotWatchpoint>>;

bool watchConditions(VM&, Data*, Slot*, const ObjectPropertyConditionSet&);
void stopWatching(Data*, Slot*);
void moveWatching(Data*, Slot* from, Slot* to);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
