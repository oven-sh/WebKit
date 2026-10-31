/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinUses.h"
#include "AOTPropertyAccess.h"
#include "AOTTypeTestStretches.h"
#include "AOTVector.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class Heat;

// The operations that the rules report as sites: accesses by name and by key, element accesses whose position is not tested yet, calls and what they rest on, operators, tests of a type.
inline constexpr OpcodeSet testSiteOpcodes = builtinUseOpcodes | Value::opcodesWithForm | namedAccesses | OpcodeSet {
    Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemHas,
    Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement,
    Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::SuperConstructList, Opcode::ConstructSuperForwardVarargs, Opcode::SuperGet, Opcode::SuperSet, Opcode::CallBuiltin,
    Opcode::GetConstructPrototype, Opcode::InstanceOf, Opcode::NewObject, Opcode::TypeTest,
};

// PlaceTypeTests has nothing to do in a function without any of these. The two others are the question for an element, whose arm for a hole it marks, and the test of a slot, behind which it reads an option that was left out.
inline constexpr OpcodeSet opcodesPlaceTypeTestsLooksFor = testSiteOpcodes | OpcodeSet { Opcode::HasArrayElement, Opcode::IsSlotFilled };

bool mayPlaceTestsOfAccessesByName(const Heat&, FunctionAnalyses&, BlockID);
// A prediction: what arrives as the receiver in this function has none of the layouts of the program, whatever other callers hand over.
bool arrivesWithoutLayoutOfProgram(FunctionPassContext&, BlockID, ValueID receiver);
bool paysToLeaveDataFromOutsideUntested(FunctionPassContext&, BlockID);

enum class PlannedTestPosition : uint8_t { BeforeUse, OnEdge };

struct PlannedTest {
    PlannedTestPosition position { PlannedTestPosition::BeforeUse };
    unsigned stretch { 0 };
    uint32_t requirement { 0 };
    TestConditionID condition;
    ProgramPoint point;
    Edge edge;
    Vector<ValueID, 2> operands;
};

struct PlannedSlowPath {
    unsigned stretch { 0 };
    Edge edge;
};

struct PlannedUse {
    unsigned stretch { 0 };
    uint32_t site { 0 };
    uint32_t firstRequirement { 0 };
};

// The head of the loop wants proofs.
struct PlannedWayBackIntoLoop {
    unsigned stretch { 0 };
    BlockID head;
    Edge enteringEdge;
    Vector<Edge, 2> backEdges;
    Vector<PlannedTest, 4> tests;
    Vector<TypeID, 4> typesOfCounters;
};

struct TestPlacementPlan {
    TestSites sites;
    std::unique_ptr<TestConditions> conditions;
    Vector<TestRequirement> requirements;
    Vector<ExistingProof> proofs;
    Vector<PlannedTest> tests;
    Vector<PlannedUse> uses;
    Vector<PlannedSlowPath> slowPaths;
    Vector<Stretch> stretches;
    Vector<StretchRange> ranges;
    Vector<ProgramPoint> rejoinPoints;
    Vector<ProgramPoint> rejoinPointsWithoutTests;
    // A long stretch ends here and the next one begins. The generic copy comes back, whatever the tested code knows better.
    Vector<ProgramPoint> splitPoints;
    Vector<PlannedWayBackIntoLoop> waysBackIntoLoops;

    bool isGuarded(unsigned stretch) const { return stretches[stretch].outcome == StretchOutcome::Guarded; }
    bool hasGuardedStretch() const { return stretches.containsIf([](const Stretch& stretch) { return stretch.outcome == StretchOutcome::Guarded && stretch.numberOfUses; }); }
    void dump(PrintStream&, const Program&) const;
};

// A guard holds what one test or one case of a switch has proven, wherever that has not ended on any way to the guard.
enum class ProofsHeldByGuards : uint8_t { AreListed, AreLeftOut };
// A parameter that takes a new object on each edge. No guard holds what the objects prove together.
enum class ProofsOfAllocationsThatMeet : uint8_t { AreLeftOut, AreListed };
bool takesAllocationOnSomeEdge(FunctionAnalyses&, ValueID parameter);
Vector<ExistingProof> findExistingProofs(FunctionPassContext&, const TestConditions&, ProofsHeldByGuards = ProofsHeldByGuards::AreListed, ProofsOfAllocationsThatMeet = ProofsOfAllocationsThatMeet::AreLeftOut);
ValueID ensureEdgeResult(IRBuilder&, FunctionAnalyses&, Edge, uint32_t proof);

// The `k` of `k < list.length` in front of `list[k]`.
enum class ComparedPositions : uint8_t { HaveNoTests, HaveTests };
PassOutcome askWhetherComparedPositionsAreInBounds(FunctionPassContext&, ComparedPositions);

TestPlacementPlan planTestPlacement(FunctionPassContext&);
TestPlacementPlan planTestPlacement(FunctionPassContext&, TestSites&&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
