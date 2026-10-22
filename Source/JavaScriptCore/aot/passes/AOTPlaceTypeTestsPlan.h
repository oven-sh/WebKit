/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinUses.h"
#include "AOTTypeTestStretches.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The operations that the rules report as sites: accesses by name and by key, element accesses whose position is not tested yet, calls and what they rest on, operators, tests of a type.
inline constexpr OpcodeSet testSiteOpcodes = builtinUseOpcodes | Value::opcodesWithForm | OpcodeSet {
    Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine, Opcode::PropHas, Opcode::PropHasOwn,
    Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas,
    Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemHas,
    Opcode::GetArrayElement, Opcode::PutArrayElement, Opcode::GetTypedArrayElement, Opcode::PutTypedArrayElement,
    Opcode::Call, Opcode::Construct, Opcode::SuperConstruct, Opcode::SuperGet, Opcode::SuperSet, Opcode::CallBuiltin,
    Opcode::GetConstructPrototype, Opcode::InstanceOf, Opcode::NewObject, Opcode::TypeTest,
};

// PlaceTypeTests has nothing to do in a function without any of these. The two others are the question for an element, whose arm for a hole it marks, and the test of a slot, behind which it reads an option that was left out.
inline constexpr OpcodeSet opcodesPlaceTypeTestsLooksFor = testSiteOpcodes | OpcodeSet { Opcode::HasArrayElement, Opcode::IsSlotFilled };

namespace TestPlacementModel {

constexpr BlockFrequency frequencyOfGenericCopy(BlockFrequency original)
{
    switch (original) {
    case BlockFrequency::Normal:
        return BlockFrequency::Unlikely;
    case BlockFrequency::Unlikely:
    case BlockFrequency::Rare:
        return BlockFrequency::Rare;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr BlockFrequency frequencyOfCopyAgainstCheckedTypes(BlockFrequency)
{
    return BlockFrequency::Rare;
}

} // namespace TestPlacementModel

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

// The head of the loop wants proofs. At the end of a turn the generic copy joins an edge that enters the loop, in front of the tests that stand on that edge.
// On its way there it makes the tests for whatever else the head wants, each behind the tests whose proofs it names.
struct PlannedWayBackIntoLoop {
    unsigned stretch { 0 };
    BlockID head;
    Edge enteringEdge;
    Vector<Edge, 2> backEdges;
    Vector<PlannedTest, 4> tests;
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
    Vector<PlannedWayBackIntoLoop> waysBackIntoLoops;

    bool isGuarded(unsigned stretch) const { return stretches[stretch].outcome == StretchOutcome::Guarded; }
    bool hasGuardedStretch() const;
    void dump(PrintStream&, const Program&) const;
};

TestSites collectTestSites(FunctionPassContext&);
Vector<TestRequirement> chooseRequirements(FunctionPassContext&, TestSites&, TestConditions&);
// A guard holds what one test or one case of a switch has proven, wherever that has not ended on any way to the guard. The edge of that test or case is listed too, so a guard adds no place
// at which a condition holds: it adds a value that names the proof. Whoever only asks where conditions hold leaves the guards out, and with them the question where each of them holds.
enum class ProofsHeldByGuards : uint8_t { AreListed, AreLeftOut };
Vector<ExistingProof> findExistingProofs(FunctionPassContext&, const TestConditions&, ProofsHeldByGuards = ProofsHeldByGuards::AreListed);
ValueID ensureEdgeResult(IRBuilder&, FunctionAnalyses&, Edge, uint32_t proof);
Vector<HoistedTest> findHoistedTests(FunctionPassContext&, const TestSites&, const TestConditions&, std::span<const TestRequirement>);

bool expectedTypeTellsResultOf(FunctionPassContext&, const TestSite&);
Vector<ValueID> collectSubjectsKnownBeyondTheirTypes(FunctionPassContext&, const TestPlacementPlan&);

TestPlacementPlan planTestPlacement(FunctionPassContext&);
TestPlacementPlan planTestPlacement(FunctionPassContext&, TestSites&&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
