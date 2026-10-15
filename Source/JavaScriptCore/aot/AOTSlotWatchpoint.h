/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include "ObjectPropertyCondition.h"
#include "PackedCellPtr.h"
#include "Watchpoint.h"
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>

namespace JSC {

class CodeBlock;

namespace AOT {

struct Data;
struct Instance;
struct Slot;

namespace Typed {
struct GlobalPropertyCache;
}

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

// On the Structure of an object that no other object has: it fires when the object makes a transition, by whatever path. It is on no list by then.
class TransitionWatchpoint final : public Watchpoint {
public:
    using Callback = void (*)(VM&, void* context);

    TransitionWatchpoint();

    // False: a transition has been made from the Structure before, so the next one would not be told.
    bool install(JSObject*, Callback, void* context);
    void fireInternal(VM&, const FireDetail&);

private:
    Callback m_callback { nullptr };
    void* m_context { nullptr };
};

// The holder has the function under the name: see Instance::bufferAccessors.
class BufferAccessorWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
public:
    void watch(VM&, const ObjectPropertyCondition&, Instance&, unsigned row);

private:
    void handleFire(VM&, const FireDetail&) final;

    Instance* m_instance { nullptr };
    unsigned m_row { 0 };
};

class GlobalPropertyWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
    WTF_MAKE_TZONE_ALLOCATED(GlobalPropertyWatchpoint);
public:
    bool isWatching() const { return m_cache; }
    void watch(VM&, const ObjectPropertyCondition&, Typed::GlobalPropertyCache&, std::unique_ptr<GlobalPropertyWatchpoint>* sibling = nullptr);

private:
    void handleFire(VM&, const FireDetail&) final;

    Typed::GlobalPropertyCache* m_cache { nullptr };
    std::unique_ptr<GlobalPropertyWatchpoint>* m_sibling { nullptr };
};

using SlotWatchpointMap = UncheckedKeyHashMap<Slot*, FixedVector<SlotWatchpoint>>;

bool watchConditions(VM&, Data*, Slot*, std::span<const ObjectPropertyCondition>);
bool watchForStores(VM&, Data*, Slot*, std::span<JSObject* const> prototypes);
void stopWatching(Data*, Slot*);
void moveWatching(Data*, Slot* from, Slot* to);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
