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

inline constexpr ValueFlowName elementsName = ValueFlowName::of(ValueFlowNameKind::Elements);
inline constexpr ValueFlowName prototypeLinkName = ValueFlowName::of(ValueFlowNameKind::PrototypeLink);
inline constexpr ValueFlowName steppedValuesName = ValueFlowName::of(ValueFlowNameKind::SteppedValues);
inline constexpr ValueFlowName returnedValueName = ValueFlowName::of(ValueFlowNameKind::ReturnedValue);
inline constexpr ValueFlowName sentValuesName = ValueFlowName::of(ValueFlowNameKind::SentValues);
inline constexpr ValueFlowName awaitedValueName = ValueFlowName::of(ValueFlowNameKind::AwaitedValue);

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

inline ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object)
{
    return ClimbingSet(context.maker().of(object));
}

inline ClimbingSet known(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.restrict(set, context.universe().knownObjects());
}

inline ClimbingSet without(ValueFlowRuleContext& context, ClimbingSet set, ValueFlowObjectID object)
{
    return context.restrict(set, ValueSets::complement(context.maker().of(object)));
}

inline bool mayBeUnknownObject(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.contains(set, ValueFlowUniverse::unknownObject());
}

inline bool holdsSomething(ValueFlowRuleContext& context, ClimbingSet set)
{
    return context.meet(set, ValueSets::anything);
}

// The nodes that hold what the known objects among these hold under the name of a slot. An object that has escaped may hold what was stored through the unknown object.
void forEachNodeOfSlotReadThrough(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    functor(context.read(known(context, objects), name));
    if (context.meet(objects, context.valueOf(ValueFlowProgramNodes::escaped())))
        functor(context.read(ClimbingSet(context.universe().unknownObjectAlone()), name));
}

// What the objects hold under the name goes to the target. Nobody knows what the unknown object holds.
void connectContentsOfSlot(ValueFlowRuleContext&, ClimbingSet objects, ValueFlowName, ValueFlowNodeID target);

// The nodes that take what is put into the slot. What is stored through the unknown object lies in one node for each name, and it escapes.
void forEachNodeOfSlotWrittenThrough(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor)
{
    functor(context.written(known(context, objects), name));
    if (!mayBeUnknownObject(context, objects))
        return;
    functor(context.written(ClimbingSet(context.universe().unknownObjectAlone()), name));
    functor(ValueFlowProgramNodes::escaped());
}

// The promises are resolved with the values: a thenable among them is asked for what it settles with, anything else is what the promises hold from then on.
void resolvePromises(ValueFlowRuleContext&, ClimbingSet promises, ClimbingSet values);
void rejectPromises(ValueFlowRuleContext&, ClimbingSet promises, const ValueFlowOperand& reasons);

void letNativeCodeStepIterators(ValueFlowRuleContext&, ClimbingSet escaped);
void letNativeCodeSettlePromises(ValueFlowRuleContext&, ClimbingSet escaped);

// What a step of a built-in iterator or a generator gives, whether or not it is the last one. `sent` is what the step hands in.
void connectOutcomeOfStep(ValueFlowRuleContext&, ClimbingSet iterators, ResumeMode, std::span<const ValueFlowOperand> sent, ValueFlowNodeID target);
// The same as an iterator result, for which the object stands. Without an object the unknown object stands for it.
ClimbingSet makeIteratorResult(ValueFlowRuleContext&, ValueFlowObjectID, ClimbingSet iterators, ResumeMode, std::span<const ValueFlowOperand> sent);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
