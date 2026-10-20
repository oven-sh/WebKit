/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"

namespace JSC { namespace AOT { namespace Typed {

void connectContentsOfSlot(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, ValueFlowNodeID target)
{
    forEachNodeOfSlotReadThrough(context, objects, name, [&](ValueFlowNodeID held) {
        context.connect(held, target);
    });
    if (mayBeUnknownObject(context, objects))
        context.add(target, unknownValue(context));
}

void callInProtocol(ValueFlowRuleContext& context, const ValueFlowProtocolCall& call)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!context.meet(call.callees, universe.callables()))
        return;
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();

    Vector<ValueFlowOperand, 2> arguments;
    for (ValueFlowNodeID argument : call.arguments)
        arguments.append({ argument, ValueSets::nothing, ValueSets::anything });
    ValueFlowBuiltinCall application;
    application.kind = ValueFlowCallKind::CallByNativeCode;
    application.thisValue = { context.holding(call.thisValue), ValueSets::nothing, ValueSets::anything };
    application.arguments = arguments.span();
    application.otherArguments = { { }, call.otherArguments, ValueSets::anything };
    application.result = call.result;
    application.thrown = escaped;
    // Where the unknown object may be called the operands escape, everything may happen to them and the result is unknown: no entry adds to that.
    bool mayCallAnything = mayBeUnknownObject(context, call.callees);
    context.forEachMember(context.restrict(call.callees, universe.builtinFunctions()), [&](ValueFlowObjectID callee) {
        BuiltinID builtin = universe.builtinOf(callee);
        if (mayCallAnything) {
            noteCallOfBuiltin(builtin, application.kind, context);
            return;
        }
        application.resultObject = universe.resultOf(builtin, 0);
        application.contentsObject = universe.resultOf(builtin, 1);
        applyBuiltin(builtin, application, context);
    });

    ClimbingSet others = context.restrict(call.callees, context.maker().intersect(universe.callables(), ValueSets::complement(universe.builtinFunctions())));
    if (!holdsSomething(context, others))
        return;
    ValueFlowCallGroup group = context.group(others, ValueFlowCallKind::CallByNativeCode);
    context.add(group.thisValue, call.thisValue);
    RELEASE_ASSERT(call.arguments.size() <= numberOfGroupedArguments);
    for (unsigned index = 0; index < call.arguments.size(); ++index)
        context.connect(call.arguments[index], group.argument(index));
    context.add(group.otherArguments, call.otherArguments);
    if (call.result)
        context.connect(group.result, call.result);
    context.connect(group.thrown, escaped);
}

void letNativeCodeRunProtocols(ValueFlowRuleContext& context, ClimbingSet escaped)
{
    letNativeCodeStepIterators(context, escaped);
    letNativeCodeSettlePromises(context, escaped);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
