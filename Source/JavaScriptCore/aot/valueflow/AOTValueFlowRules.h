/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"

namespace JSC { namespace AOT { namespace Typed {

// The rule of every statement. The solver calls nothing else.
void runValueFlowRule(const ValueFlowStatement&, ValueFlowRuleContext&);

// Every primitive and the unknown object. Whoever brings the unknown object into being says why: the source goes with every set that the run adds from here on, until the run names another one.
ValueSetID unknownValue(ValueFlowRuleContext&, ValueFlowUnknownSource);
// Whoever fills a group of kind Call or Construct says from which position on it passes nothing: a parameter that every caller of the group fills is not undefined.
void passNoFurtherArguments(ValueFlowRuleContext&, const ValueFlowCallGroup&, unsigned numberOfArguments);

// The known objects of a value, for a rule that asks each of them something. They come one by one while they are no more than mostMembersToldApart(). Where they are more, the objects of one layout come
// together, as do those of one built-in class, and the first that many of these groups are told apart, in an order that no run changes: by layout, then by class, then by object.
// Only what is left comes as `rest`, which the rule takes together. A run with more objects covers what a run with fewer has done: a group only grows, and a group among the first was never in the rest.
struct ValueFlowObjectsToldApart {
    Vector<ValueSetID, 16> groups;
    ValueSetID rest { ValueSets::nothing };
};
ValueFlowObjectsToldApart tellObjectsApart(ValueFlowRuleContext&, ClimbingSet knownObjects);

// The key of a group never mixes a placeholder with other members: the rule of the group of a placeholder goes over what the placeholder covers, and only the unknown object lets the operands escape.
// So whoever asks for a group splits the callees first: those that the analysis follows, the untracked object, the unknown object. The functor runs for a part that holds something.
// In the library's code behind a described built-in the unknown object is an operand of a call whose records say what becomes of it: see calleeOfDescribedCall().
ValueSetID unknownCalleeOfStatement(ValueFlowRuleContext&);
void forEachPartOfCallees(ValueFlowRuleContext& context, ClimbingSet callees, NOESCAPE const Invocable<void(ClimbingSet)> auto& functor)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet followed = context.restrict(callees, ValueSets::complement(universe.placeholdersAlone()));
    if (context.meet(followed, ValueSets::anything))
        functor(followed);
    if (context.contains(callees, ValueFlowUniverse::untrackedObject()))
        functor(ClimbingSet(universe.untrackedObjectAlone()));
    if (context.contains(callees, ValueFlowUniverse::unknownObject()))
        functor(ClimbingSet(unknownCalleeOfStatement(context)));
}

// The definition of a value, through the operations that hand a value on unchanged and through tests. A test hands the value that it has tested on to the block behind it, narrowed. Such a block has one way in.
ValueID definitionThroughTests(const Function&, ValueID);
// `fetch`, `Bun.spawn`, `globalThis.process.binding`: the entry that the operations of the function lead to by themselves, through reads under constant keys that start at an entry of the table.
BuiltinID entryNamedByOperations(const Program&, const Function&, ValueID);

// What holds for an operation without one is read from the schema and depends on no set: the bodies turn it into edges and seeds.
bool hasRuleOfItsOwn(Opcode);

// An operator whose result has the kinds that the kinds of its operands can give.
bool answersByKindsOfOperands(Opcode);
// Of numbers without a fraction the operator makes a number without a fraction.
bool makesNoFraction(Opcode);
void runArithmetic(const ValueFlowStatement&, ValueFlowRuleContext&);
void runToPropertyKey(const ValueFlowStatement&, ValueFlowRuleContext&);

// Whether the set holds an instance of a class without a heritage that some operand passes as new.target: the run time may have made an ordinary object in its place.
bool mayHaveOtherLayoutThanItsClass(ValueFlowRuleContext&, ClimbingSet);
void runCall(const ValueFlowStatement&, ValueFlowRuleContext&);
void runCallGroup(const ValueFlowStatement&, ValueFlowRuleContext&);
void runEscape(const ValueFlowStatement&, ValueFlowRuleContext&);

// A conversion that the families cover is a copy. Behind any other the type says nothing about an object that the analysis has stopped following: the untracked object goes in and the unknown object comes out.
void runConvert(const ValueFlowStatement&, ValueFlowRuleContext&);
void runNewDictionary(const ValueFlowStatement&, ValueFlowRuleContext&);

void runNewClass(const ValueFlowStatement&, ValueFlowRuleContext&);
void runDefineMethod(const ValueFlowStatement&, ValueFlowRuleContext&);
void runReadOfPrototypeProperty(const ValueFlowStatement&, ValueFlowRuleContext&);

// A call that no operation spells out: native code runs a protocol of the language. A function of the program is entered, and the entry of a built-in is applied with the operands of this call alone.
// A placeholder among the callees is called through its own group. What the callees throw escapes.
struct ValueFlowProtocolCall {
    ClimbingSet callees;
    ClimbingSet thisValue;
    // What the protocol passes, position by position.
    std::span<const ValueFlowNodeID> arguments { };
    // What native code makes up and passes at any position.
    ValueSetID otherArguments { ValueSets::nothing };
    // None where native code drops the result.
    ValueFlowNodeID result { };
    // A protocol finds what it calls under a name of its own. The executor of a promise is a value that the program hands over.
    ValueFlowCalleeOrigin origin { ValueFlowCalleeOrigin::MemberFoundUnderName };
};
void callInProtocol(ValueFlowRuleContext&, const ValueFlowProtocolCall&);
// The objects of the language that the table has as members under the key and that have escaped: beside the unknown object no set lists them.
// The code behind a built-in of the host is in the image for a use that names the built-in, and that it has come into unknown hands is no such use.
ClimbingSet escapedMembersUnder(ValueFlowRuleContext&, PropertyKeyID);

// IteratorOpen, IteratorResume, IteratorClose, AppendSpread.
void runIterationOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
// StepArrayIterator, MapNext, SetNext.
void runStepOfLibrary(const ValueFlowStatement&, ValueFlowRuleContext&);
// ResumeGenerator, StepGenerator.
void runGeneratorOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runOpenedIterators(const ValueFlowStatement&, ValueFlowRuleContext&);
void runIteratedValues(const ValueFlowStatement&, ValueFlowRuleContext&);
void runIteratorResults(const ValueFlowStatement&, ValueFlowRuleContext&);
void runAwait(const ValueFlowStatement&, ValueFlowRuleContext&);
void runAwaitedValue(const ValueFlowStatement&, ValueFlowRuleContext&);
// CreatePromise, ResolvePromise, RejectPromise.
void runPromiseOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runCoroutine(const ValueFlowStatement&, ValueFlowRuleContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
