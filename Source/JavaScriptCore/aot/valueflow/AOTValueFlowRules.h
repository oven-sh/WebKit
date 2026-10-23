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
// What an entry of the engine answers without a description. The engine makes no object of the host.
ValueSetID unknownValueOutOfEngine(ValueFlowRuleContext&);
// Whoever fills a group of kind Call or Construct says from which position on it passes nothing: a parameter that every caller of the group fills is not undefined.
void passNoFurtherArguments(ValueFlowRuleContext&, const ValueFlowCallGroup&, unsigned numberOfArguments);

// The known objects of a value, for a rule that asks each of them something.
struct ValueFlowObjectsToldApart {
    Vector<ValueSetID, 16> groups;
    ValueSetID rest { ValueSets::nothing };
};
ValueFlowObjectsToldApart tellObjectsApart(ValueFlowRuleContext&, ClimbingSet knownObjects);

// The key of a group never mixes a placeholder with other members: the rule of the group of a placeholder goes over what the placeholder covers, and only the unknown object lets the operands escape.
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

// A call that no operation spells out: native code runs a protocol of the language.
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
