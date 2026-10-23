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

// What the rules of iteration, of promises and of coroutines share.

inline constexpr ValueFlowName steppedValuesName = ValueFlowName::of(ValueFlowNameKind::SteppedValues);
inline constexpr ValueFlowName returnedValueName = ValueFlowName::of(ValueFlowNameKind::ReturnedValue);
inline constexpr ValueFlowName sentValuesName = ValueFlowName::of(ValueFlowNameKind::SentValues);

inline constexpr ValueFlowName iteratedValuesName(IteratorKind kind)
{
    return { ValueFlowNameKind::IteratedValues, static_cast<uint32_t>(kind) };
}

inline ValueFlowName nameOf(ValueFlowRuleContext& context, BuiltinName name)
{
    return ValueFlowName::property(context.program().keyOf(name));
}

inline constexpr ValueFlowName nameOf(BuiltinSlotID slot)
{
    return { ValueFlowNameKind::Slot, slot.index() };
}

// Without either placeholder.
inline ClimbingSet known(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.restrict(set, context.universe().knownObjects());
}

inline ClimbingSet without(ValueFlowRuleContext& context, ClimbingSet set, ValueFlowObjectID object)
{
    return context.restrict(set, ValueSets::complement(context.maker().of(object)));
}

inline ClimbingSet withoutPlaceholders(ValueFlowRuleContext& context, ClimbingSet set)
{
    const ValueFlowUniverse& universe = context.universe();
    return context.restrict(set, ValueSets::complement(context.maker().unite(universe.unknownObjectAlone(), universe.untrackedObjectAlone())));
}

inline bool mayBeUnknownObject(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.contains(set, ValueFlowUniverse::unknownObject());
}

inline bool mayBeUntrackedObject(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.contains(set, ValueFlowUniverse::untrackedObject());
}

inline bool holdsSomething(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.meet(set, ValueSets::anything);
}

// A placeholder among the receivers of a protocol is looked at by itself: a method that is found through it would get every other receiver as `this`.
void forEachPlaceholderAndOthers(ValueFlowRuleContext& context, ClimbingSet receivers, NOESCAPE const Invocable<void(ClimbingSet)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    if (mayBeUnknownObject(context, receivers))
        functor(ClimbingSet(universe.unknownObjectAlone()));
    if (mayBeUntrackedObject(context, receivers))
        functor(ClimbingSet(universe.untrackedObjectAlone()));
    functor(withoutPlaceholders(context, receivers));
}

// The nodes that hold what the objects among these hold under the name of a slot.
void forEachNodeOfSlotReadThrough(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet unknownObject(universe.unknownObjectAlone());
    ClimbingSet untrackedObject(universe.untrackedObjectAlone());
    if (mayBeUntrackedObject(context, objects)) {
        functor(context.read(ClimbingSet(universe.knownObjects()), name));
        functor(context.read(untrackedObject, name));
        functor(context.read(unknownObject, name));
        return;
    }

    ClimbingSet followed = known(context, objects);
    functor(context.read(followed, name));
    if (!holdsSomething(context, followed))
        return;
    if (context.holdsAnyOf(followed, ValueFlowProgramNodes::escaped()))
        functor(context.read(unknownObject, name));
    if (context.holdsAnyOf(followed, ValueFlowProgramNodes::lost()))
        functor(context.read(untrackedObject, name));
}

// What the objects hold under the name goes to the target. Nobody knows what the unknown object holds.
void connectContentsOfSlot(ValueFlowRuleContext&, ClimbingSet objects, ValueFlowName, ValueFlowNodeID target);

// The nodes that take what is put into the slot. What is stored through a placeholder lies in one node for each name. Through the unknown object it reaches code that nobody compiles as well, so it escapes.
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

// The promises are resolved with the values: a thenable among them is asked for what it settles with, anything else is what the promises hold from then on.
void resolvePromises(ValueFlowRuleContext&, ClimbingSet promises, ClimbingSet values);
void rejectPromises(ValueFlowRuleContext&, ClimbingSet promises, const ValueFlowOperand& reasons);

void letNativeCodeStepIterators(ValueFlowRuleContext&, ClimbingSet escaped);
void letNativeCodeSettlePromises(ValueFlowRuleContext&, ClimbingSet escaped);

// What a step of a built-in iterator or a generator gives, whether or not it is the last one. `sent` is what the step hands in.
void connectOutcomeOfStep(ValueFlowRuleContext&, ClimbingSet iterators, ResumeMode, std::span<const ValueFlowOperand> sent, ValueFlowNodeID target);
// The same as an iterator result, for which the object stands.
ClimbingSet makeIteratorResult(ValueFlowRuleContext&, ValueFlowObjectID, ClimbingSet iterators, ResumeMode, std::span<const ValueFlowOperand> sent);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
