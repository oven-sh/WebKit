/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

void runValueFlowRule(const ValueFlowStatement&, ValueFlowRuleContext&);

ValueSetID unknownValue(ValueFlowRuleContext&, ValueFlowUnknownSource);
ValueSetID undescribedValueThatIsNoBuffer(ValueFlowRuleContext&);
void passNoFurtherArguments(ValueFlowRuleContext&, const ValueFlowCallGroup&, unsigned numberOfArguments, unsigned numberOfPositionsTaken = numberOfGroupedArguments);

ValueFlowObjectsToldApart tellObjectsApart(ValueFlowRuleContext&, ClimbingSet knownObjects);

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

ValueID definitionThroughTests(const Function&, ValueID);
// TryReExportFields passes on the names of its source, or leaves that to the loop that is written behind it. This is a step of that loop.
bool walksNamesThatAreReExported(const Function&, const Value& stepOfEnumeration);
BuiltinID entryNamedByOperations(const Program&, const Function&, ValueID);

bool hasRuleOfItsOwn(Opcode);
bool throwsInsteadOfRunning(const Function& callee, bool isConstruction);

bool answersByKindsOfOperands(Opcode);
bool makesNoFraction(Opcode);
void runArithmetic(const ValueFlowStatement&, ValueFlowRuleContext&);
void runToPropertyKey(const ValueFlowStatement&, ValueFlowRuleContext&);

void runCall(const ValueFlowStatement&, ValueFlowRuleContext&);
void runCallGroup(const ValueFlowStatement&, ValueFlowRuleContext&);

void runNewClass(const ValueFlowStatement&, ValueFlowRuleContext&);
void runDefineMethod(const ValueFlowStatement&, ValueFlowRuleContext&);
void runReadOfPrototypeProperty(const ValueFlowStatement&, ValueFlowRuleContext&);

struct ValueFlowProtocolCall {
    ClimbingSet callees;
    ClimbingSet thisValue;
    std::span<const ValueFlowNodeID> arguments { };
    ValueSetID otherArguments { ValueSets::nothing };
    ValueFlowNodeID result { };
    ValueFlowCalleeOrigin origin { ValueFlowCalleeOrigin::MemberFoundUnderName };
};
void callInProtocol(ValueFlowRuleContext&, const ValueFlowProtocolCall&);
ClimbingSet escapedMembersUnder(ValueFlowRuleContext&, PropertyKeyID);
ValueFlowNodeID heldPrototypeObjectsOfLibraryClasses(ValueFlowRuleContext&);

void runIterationOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runStepOfLibrary(const ValueFlowStatement&, ValueFlowRuleContext&);
void runGeneratorOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runOpenedIterators(const ValueFlowStatement&, ValueFlowRuleContext&);
void runIteratedValues(const ValueFlowStatement&, ValueFlowRuleContext&);
void runIteratorResults(const ValueFlowStatement&, ValueFlowRuleContext&);
void runAwait(const ValueFlowStatement&, ValueFlowRuleContext&);
void runAwaitedValue(const ValueFlowStatement&, ValueFlowRuleContext&);
void runPromiseOperation(const ValueFlowStatement&, ValueFlowRuleContext&);
void runCoroutine(const ValueFlowStatement&, ValueFlowRuleContext&);

inline bool passesList(Opcode opcode) { return opcode == Opcode::CallList || opcode == Opcode::ConstructList || opcode == Opcode::SuperConstructList; }
inline ClimbingSet only(ValueFlowRuleContext& context, ValueFlowObjectID object) { return ClimbingSet(context.maker().of(object)); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
