/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRulesProtocolsInternal.h"

#if ENABLE(AOT)

#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

void connectContentsOfSlot(ValueFlowRuleContext& context, ClimbingSet objects, ValueFlowName name, ValueFlowNodeID target)
{
    forEachNodeOfSlotReadThrough(context, objects, name, [&](ValueFlowNodeID held) {
        context.connect(held, target);
    });
    if (mayBeUnknownObject(context, objects))
        context.add(target, unknownValue(context, ValueFlowUnknownSource::ContentsOfEscapedObject));
}

void callInProtocol(ValueFlowRuleContext& context, const ValueFlowProtocolCall& call)
{
    const ValueFlowUniverse& universe = context.universe();
    if (!context.meet(call.callees, universe.callables()))
        return;
    ValueFlowNodeID escaped = ValueFlowProgramNodes::escaped();
    // Any other built-in function is there because the program has put it there. It is called through a group like a function of the program, so that the nodes of the group keep its operands
    // for whoever asks after the solve what the built-in was applied to.
    ClimbingSet builtinFunctions = context.restrict(call.callees, universe.builtinFunctionsOfProtocols());
    // Beside the unknown object the callees do not list what has escaped, and of that a protocol finds the functions that the table has under the names of protocols. Their code runs.
    // The operands of a call of the unknown object escape and its result is unknown, which is all that the entry of such a function could say.
    if (call.origin == ValueFlowCalleeOrigin::MemberFoundUnderName && context.contains(call.callees, ValueFlowUniverse::unknownObject())) {
        context.forEachMember(context.valueWithin(ClimbingSet(universe.builtinFunctionsOfProtocols()), escaped), [&](ValueFlowObjectID callee) {
            // That a built-in of the host has come into unknown hands is no use of it: see runUsedBuiltins().
            if (context.program().builtins().at(universe.builtinOf(callee)).origin != BuiltinOrigin::Host)
                noteCallOfBuiltin(universe.builtinOf(callee), ValueFlowCallKind::CallByNativeCode, context);
        });
    }

    // A built-in that lets in unseen code does so wherever it is applied. The protocol runs for an operation of a function, or for a statement that a rule has made, behind which seen code may stand.
    // The group of the other callees says the same of them.
    ValueFlowStatement protocol;
    protocol.function = context.functionOfStatement();
    protocol.result = call.result;
    applyEventsOfCall(protocol, context, builtinFunctions);

    // The entry of a built-in is applied with the operands of this call, whatever else may be called beside it.
    if (holdsSomething(context, builtinFunctions)) {
        Vector<ValueFlowOperand, 2> arguments;
        for (ValueFlowNodeID argument : call.arguments)
            arguments.append({ argument, ValueSets::nothing, ValueSets::anything });
        ValueFlowBuiltinCall application;
        application.kind = ValueFlowCallKind::CallByNativeCode;
        application.thisValue = context.operandHolding(call.thisValue);
        application.arguments = arguments.span();
        application.otherArguments = { { }, call.otherArguments, ValueSets::anything };
        application.result = call.result;
        application.thrown = escaped;
        context.forEachMember(builtinFunctions, [&](ValueFlowObjectID callee) {
            BuiltinID builtin = universe.builtinOf(callee);
            application.resultObject = universe.resultOf(builtin, 0);
            application.contentsObject = universe.resultOf(builtin, 1);
            applyBuiltin(builtin, application, context);
        });
    }

    // The operands go into the group of a placeholder as into any group: the rule of that group says what the placeholder stands for, and only where the unknown object is called do they escape.
    RELEASE_ASSERT(call.arguments.size() <= numberOfGroupedArguments);
    ClimbingSet others = context.restrict(call.callees, context.maker().intersect(universe.callables(), ValueSets::complement(universe.builtinFunctionsOfProtocols())));
    forEachPartOfCallees(context, others, [&](ClimbingSet callees) {
        ValueFlowCallGroup group = context.group(callees, ValueFlowCallKind::CallByNativeCode, call.origin);
        context.add(group.thisValue, call.thisValue);
        for (unsigned index = 0; index < call.arguments.size(); ++index)
            context.connect(call.arguments[index], group.argument(index));
        context.add(group.otherArguments, call.otherArguments);
        if (call.result)
            context.connect(group.result, call.result);
        context.connect(group.thrown, escaped);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
