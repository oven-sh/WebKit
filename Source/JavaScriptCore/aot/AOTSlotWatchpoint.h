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

    void initialize(CodeBlock*, const ObjectPropertyCondition&, unsigned slotIndex);
    void install(VM&);
    void fireInternal(VM&, const FireDetail&);

    const ObjectPropertyCondition& key() const LIFETIME_BOUND { return m_key; }

private:
    PackedCellPtr<CodeBlock> m_owner;
    unsigned m_slotIndex { 0 };
    ObjectPropertyCondition m_key;
};

// By the index of the slot.
using SlotWatchpointMap = UncheckedKeyHashMap<unsigned, FixedVector<SlotWatchpoint>, WTF::IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>>;

// Has the conditions watched on behalf of the slot, in place of whatever was. False, and nothing is watched, if that cannot be
// done, in which case the slot must not be filled.
bool watchConditions(VM&, CodeBlock*, Slot*, const ObjectPropertyConditionSet&);
void stopWatching(CodeBlock*, Slot*);

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
