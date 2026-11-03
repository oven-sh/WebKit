/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

void runValueFlowRule(const ValueFlowStatement&, ValueFlowRuleContext&);

ValueSetID unknownValue(ValueFlowRuleContext&, ValueFlowUnknownSource);
ValueSetID undescribedValueThatIsNoBuffer(ValueFlowRuleContext&);
void passNoFurtherArguments(ValueFlowRuleContext&, const ValueFlowCallGroup&, unsigned numberOfArguments, unsigned numberOfPositionsTaken = numberOfGroupedArguments);

ValueFlowObjectsToldApart tellObjectsApart(ValueFlowRuleContext&, ClimbingSet knownObjects);
void passKnownReceiversToWhatTheyFind(ValueFlowRuleContext&, ClimbingSet receivers, ValueFlowName, ValueFlowNodeID receiversAnswered);

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

class DefinitionsThroughTests {
public:
    explicit DefinitionsThroughTests(const Function& function)
        : m_function(function)
    {
    }

    const Function& function() const { return m_function; }
    ValueID of(ValueID);
    uint64_t numberOfBlocksRead() const { return m_numberOfBlocksRead; }

private:
    struct WaysIn {
        uint32_t count { 0 };
        uint32_t from { 0 };
        uint32_t targetIndex { 0 };
    };

    void makeTables();

    const Function& m_function;
    bool m_hasTables { false };
    uint64_t m_numberOfBlocksRead { 0 };
    Vector<BlockID, 128> m_ownerOfParameter;
    Vector<WaysIn, 32> m_waysIn;
};

BuiltinID entryNamedByOperations(const Program&, DefinitionsThroughTests&, ValueID);
PropertyKeyID keyOfMemberCalledOnItsHolderByText(const Program&, DefinitionsThroughTests&, const Value& call);
bool readsUnderNameOfEnumerationOfSameObject(DefinitionsThroughTests&, const Value& read);
std::optional<uint8_t> positionInPairWrittenAsKey(const Function&, const Value& read);
bool areUnchangedPairsOfKeyAndValue(ValueFlowRuleContext&, ClimbingSet containers);
// `"$" + name`, `"on" + kind + suffix`: the text on the left, where it is written out, is not empty and is ASCII.
StringID textThatSumBeginsWith(const Program&, const Function&, ValueID);
// `target[key] = source[key]`, `target[key] = test ? first[key] : second[key]`: the objects that what is stored was read from, under the very key of the store. Nothing where anything else may be stored.
// They stand behind the operands of the statement of the store.
Vector<ValueID, 2> objectsReadUnderKeyOfStore(const Function&, const Value& store);

bool hasRuleOfItsOwn(Opcode);
inline constexpr OperandUses usesThatMayCreateHoles = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };
void noteArraysThatMayGetHoles(ValueFlowRuleContext&, ClimbingSet objects);
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
void notePrototypeObjectsAboveUnknownObject(ValueFlowRuleContext&);

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
