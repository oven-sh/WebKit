/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowRuleContext.h"

namespace JSC { namespace AOT { namespace Typed {

// A call of a built-in as its entry in the table sees it.
struct ValueFlowBuiltinCall {
    ValueFlowCallKind kind { ValueFlowCallKind::Call };
    // Of a construction: new.target.
    ValueFlowOperand thisValue { { }, ValueSets::nothing, ValueSets::anything };
    std::span<const ValueFlowOperand> arguments;
    // The call stands for several, and some of them may pass fewer arguments.
    bool argumentsMayBeAbsent { false };
    // What may stand at any position behind `arguments`: the elements of a list that is spread.
    ValueFlowOperand otherArguments { { }, ValueSets::nothing, ValueSets::anything };
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;
    // The object that the built-in allocates and returns, and the one that stands for all it allocates to put into that one. Without them the unknown object stands for both.
    ValueFlowObjectID resultObject;
    ValueFlowObjectID contentsObject;
};

// The entry is applied with the operands of one call: what is pushed here goes into the arrays that are pushed to here.
void applyBuiltin(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// What the entries do not say about iteration. next, return and throw of a built-in iterator or a generator answer what that object hands out: true where the call is one of these, and the entry is then not applied.
bool applyStepOfBuiltinIterator(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// The iterator that a call makes over its receiver hands out what the receiver holds.
void holdContentsIteratedByResult(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// `new Promise(executor)`: the promise and the function that resolves it are objects of the call, where the constructor of the library would make one promise for the whole program.
// True where the call is one, and the entry is then not applied.
bool applyConstructionOfPromise(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// A call of what may be a function that was made for the executor of a promise.
void settlePromisesThrough(ClimbingSet functions, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// `call` is a Call, CallList, Construct, ConstructList, SuperConstruct or SuperConstructList.
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
// For a built-in that is called together with others, or by native code: with what all calls of the group pass. What it allocates is the same two objects for all of them.
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
// For a call that may call anything: its operands escape and its result is unknown, so of the entry only the use is left.
void noteCallOfBuiltin(BuiltinID, ValueFlowCallKind, ValueFlowRuleContext&);
// What can be called and is neither a function of the program nor a built-in function: a bound function.
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);

// The method under @@hasInstance gets the left operand itself.
void runInstanceOf(const ValueFlowStatement&, ValueFlowRuleContext&);

// The rule of a statement of kind Behavior.
void runBehavior(const ValueFlowStatement&, ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
