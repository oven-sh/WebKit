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
    // Of a call that stands for a group of callees: the arguments at the positions behind `arguments`, also where some of `arguments` may be absent.
    ValueFlowOperand laterArguments { { }, ValueSets::nothing, ValueSets::anything };
    ValueFlowNodeID result;
    ValueFlowNodeID thrown;
    // The object that the built-in allocates and returns, and the one that stands for all it allocates to put into that one. Without them the unknown object stands for both.
    ValueFlowObjectID resultObject;
    ValueFlowObjectID contentsObject;
    // The operation that spells the call out. None for what all calls of one set of callees share, and for a call by native code.
    const ValueFlowStatement* operation { nullptr };
    // The built-in is one of many that the callee may be, and whoever applies it lets the operands escape and answers the unknown value, which is all that a row could say of them.
    // Left to say: that the built-in runs, and whether it calls a value nobody knows.
    bool saysOnlyWhatIsCalled { false };
};

// A cell of native code on the built-in's own object, under no property's name. Something lies there once a call may take an overload that runs code which no operand names.
inline ValueFlowName cellOfCallsThatRunOtherCode(const BuiltinTable& builtins)
{
    return { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() };
}

// Another such cell: what native code has got from code of the library and put where the entry stands (BuiltinHostReadUse::IsValueOf).
inline ValueFlowName cellOfValueFromLibrary(const BuiltinTable& builtins)
{
    return { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() + 1 };
}

// Another such cell: the results of the calls whose result the program drops, where native code hands the result on by itself.
inline ValueFlowName cellOfDroppedResults(const BuiltinTable& builtins)
{
    return { ValueFlowNameKind::Slot, builtins.slotCount() + builtins.nameCount() + 2 };
}

// Memory of native code that the program reaches only through the entries and records that name the cell. It has no holder, so the global object stands in.
ValueFlowNodeID nodeWrittenToNativeCell(ValueFlowRuleContext&, BuiltinName cell);
ValueFlowNodeID nodeReadFromNativeCell(ValueFlowRuleContext&, BuiltinName cell);

// What native code does where no call of a built-in is under way. What it allocates for that is the second object of the built-in on whose behalf it acts.
// With a value that it got hold of by itself: an export of a module of the library, the object of a function of the library, what a call of its own answered.
void applyHostUses(BuiltinID onBehalfOf, const ValueFlowOperand& value, const BuiltinHostUses&, ValueFlowRuleContext&);
void callFromHost(BuiltinID onBehalfOf, const ValueFlowOperand& callees, const BuiltinHostCall&, ValueFlowRuleContext&);

// The records of the overload say what a call does with each operand and what it answers.
bool doRecordsDescribe(const BuiltinTable&, const BuiltinOverload&);
// A function of the library that runs only for calls which the records of a built-in or a model describe: the code behind such built-ins, what is written inside it,
// and the traversal that FuseArrayChains puts in the place of such calls. What it does with the operands of a call is applied where the call is.
// A call that no overload declares (`Array.from.call(List, items)`) enters the code with its own operands, and says itself what an operand nobody knows among them means.
bool standsBehindDescribedBuiltins(const Program&, FunctionID);
// What stands in the key of a group for a callee nobody knows that such code calls, by itself or through a built-in that it applies. It is an operand of a call that records describe:
// they say where that call is what is called with what, also that code runs which nobody compiles. So the group lets its operands escape, answers a value nobody knows, and raises nothing.
// No value holds the object: nothing applies the entry whose second object it is before the passes run.
ValueFlowObjectID calleeOfDescribedCall(const ValueFlowUniverse&);
// Such a function, or a function that only such functions can name: one that its file does not export, in a file of the library where every function that the table names is such a function.
bool isCodeThatRecordsDescribe(const ValueFlowUniverse&, FunctionID);

// The entry is applied with the operands of one call: what is pushed here goes into the arrays that are pushed to here.
void applyBuiltin(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// A call of such a bound function applies a target of the table with its own operands, on the bound `this`: no group of callees stands between. Whoever lists the applications of built-ins lists it with the operation.
bool bindsNoArgument(const Program&, const ValueFlowUniverse&, ValueFlowObjectID boundFunction);
// Such a target goes through a group of callees all the same: see applyEventsOfCall().
bool raisesEventWhenCalled(const BuiltinTable&, BuiltinID);

// `new Object(value)` answers the value or wraps it. Where new.target is another function than Object itself, the language ignores the value and makes an ordinary object below what new.target has as
// `prototype`: `class Below extends Object { constructor(value) { super(value) } }`, Reflect.construct(Object, [value], Other). The rows of the table tell the operands apart and have no word for new.target.
// Answers the overload that holds for another new.target whatever is passed, and none for any call but a construction of Object.
BuiltinOverloadID overloadForAnotherNewTarget(const BuiltinTable&, BuiltinID, ValueFlowCallKind);
// What the entries do not say about iteration. next, return and throw of a built-in iterator or a generator answer what that object hands out: true where the call is one of these, and the entry is then not applied.
bool applyStepOfBuiltinIterator(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// The iterator that a call makes over its receiver hands out what the receiver holds.
void holdContentsIteratedByResult(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// `new Promise(executor)`: the promise and the function that resolves it are objects of the call, where the constructor of the library would make one promise for the whole program.
// True where the call is one, and the entry is then not applied.
bool applyConstructionOfPromise(BuiltinID, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// A call of what may be a function that was made for the executor of a promise.
void settlePromisesThrough(ClimbingSet functions, const ValueFlowBuiltinCall&, ValueFlowRuleContext&);
// A promise that is resolved with the values takes over the rejection of a promise or a thenable among them. `reason` is where it holds what it is rejected with.
void adoptRejectionOf(ValueFlowRuleContext&, ClimbingSet values, ValueFlowNodeID reason);
// `call` is a Call, CallList, Construct, ConstructList, SuperConstruct or SuperConstructList. `receivers` is what of its second operand the built-in may be called on.
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&, ValueSetID receivers = ValueSets::anything);
// `x.key(..)` where the built-in is what the read of the key from the unknown object may have answered. So the unknown object is its receiver: an object that the sets list among the receivers
// has answered what its own lookup says, and the row of this built-in says nothing about it. The caller lets the operands escape: see ValueFlowBuiltinCall::saysOnlyWhatIsCalled.
void applyBuiltinFoundThroughUnknownObject(BuiltinID, const ValueFlowStatement& call, ValueFlowRuleContext&);
// For a built-in that is called together with others, or by native code: with what all calls of the group pass. What it allocates is the same two objects for all of them.
void applyBuiltin(BuiltinID, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
// A call of a function of the library that an entry describes (ValueFlowBuiltinTables::closuresThatEntriesDescribe): the overloads of the entry are applied with the operands of the call,
// and none of the operands goes into the function.
void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void applyEntryDescribing(ValueFlowObjectID closure, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);
// The function runs for callers that nobody follows into it. Every operand is any primitive or one object of which nothing is known and to which nothing happens: it stands for what the callers pass.
void enterCodeThatEntryDescribes(ValueFlowObjectID closure, ValueFlowRuleContext&);
// For a call that may call anything: its operands escape and its result is unknown, so of the entry only the use is left.
void noteCallOfBuiltin(BuiltinID, ValueFlowCallKind, ValueFlowRuleContext&);
// The function stands behind entries of the table that are all declared, or it is a helper beside such functions. The overloads list what such code calls of its operands.
bool isCodeBehindDeclaredEntries(const Program&, FunctionID);
// What can be called and is neither a function of the program nor a built-in function: a bound function.
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowStatement& call, ValueFlowRuleContext&);
void callOtherCallables(ClimbingSet callees, ValueFlowCallKind, const ValueFlowCallGroup&, ValueFlowRuleContext&);

// The method under @@hasInstance gets the left operand itself.
void runInstanceOf(const ValueFlowStatement&, ValueFlowRuleContext&);
// `value instanceof constructor` by code nobody follows, of a value nobody knows. What is found on one constructor is called on that one alone.
void letNativeCodeTestAgainst(ValueFlowRuleContext&, const ValueFlowOperand& constructors);

// The rule of a statement of kind Behavior.
void runBehavior(const ValueFlowStatement&, ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
