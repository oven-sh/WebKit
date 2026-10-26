/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinIDs.h"
#include "AOTValueFlowRules.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr ValueFlowName steppedValuesName = ValueFlowName::of(ValueFlowNameKind::SteppedValues);
inline constexpr ValueFlowName returnedValueName = ValueFlowName::of(ValueFlowNameKind::ReturnedValue);
inline constexpr ValueFlowName sentValuesName = ValueFlowName::of(ValueFlowNameKind::SentValues);

inline ValueFlowName nameOf(ValueFlowRuleContext& context, BuiltinName name) { return ValueFlowName::property(context.program().keyOf(name)); }
inline constexpr ValueFlowName nameOf(BuiltinSlotID slot) { return { ValueFlowNameKind::Slot, slot.index() }; }

inline ClimbingSet known(ValueFlowRuleContext& context, ClimbingSet set) { return context.restrict(set, context.universe().knownObjects()); }

inline ClimbingSet without(ValueFlowRuleContext& context, ClimbingSet set, ValueFlowObjectID object) { return context.restrict(set, ValueSets::complement(context.maker().of(object))); }
inline ClimbingSet withoutPlaceholders(ValueFlowRuleContext& context, ClimbingSet set) { return context.restrict(set, ValueSets::complement(context.maker().unite(context.universe().unknownObjectAlone(), context.universe().untrackedObjectAlone()))); }
inline bool mayBeUnknownObject(ValueFlowRuleContext& context, ClimbingSet set) { return context.contains(set, ValueFlowUniverse::unknownObject()); }
inline bool mayBeUntrackedObject(ValueFlowRuleContext& context, ClimbingSet set) { return context.contains(set, ValueFlowUniverse::untrackedObject()); }
inline bool holdsSomething(ValueFlowRuleContext& context, ClimbingSet set) { return context.meet(set, ValueSets::anything); }

void forEachPlaceholderAndOthers(ValueFlowRuleContext& context, ClimbingSet receivers, NOESCAPE const Invocable<void(ClimbingSet)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    if (mayBeUnknownObject(context, receivers))
        functor(ClimbingSet(universe.unknownObjectAlone()));
    if (mayBeUntrackedObject(context, receivers))
        functor(ClimbingSet(universe.untrackedObjectAlone()));
    functor(withoutPlaceholders(context, receivers));
}

void connectContentsOfSlot(ValueFlowRuleContext&, ClimbingSet objects, ValueFlowName, ValueFlowNodeID target);

void forEachNodeOfSlotWrittenThrough(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    functor(context.written(known(context, objects), name));
    if (mayBeUntrackedObject(context, objects))
        functor(context.written(ClimbingSet(universe.untrackedObjectAlone()), name));
    if (!mayBeUnknownObject(context, objects))
        return;
    functor(context.written(ClimbingSet(universe.unknownObjectAlone()), name));
    functor(ValueFlowProgramNodes::escaped());
}

void resolvePromises(ValueFlowRuleContext&, ClimbingSet promises, ClimbingSet values);
void rejectPromises(ValueFlowRuleContext&, ClimbingSet promises, const ValueFlowOperand& reasons);

void letNativeCodeStepIterators(ValueFlowRuleContext&, ClimbingSet escaped);
void letNativeCodeSettlePromises(ValueFlowRuleContext&, ClimbingSet escaped);

void connectOutcomeOfStep(ValueFlowRuleContext&, ClimbingSet iterators, ResumeMode, std::span<const ValueFlowOperand> sent, ValueFlowNodeID target);
ClimbingSet makeIteratorResult(ValueFlowRuleContext&, ValueFlowObjectID, ClimbingSet iterators, ResumeMode, std::span<const ValueFlowOperand> sent);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
