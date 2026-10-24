/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinUses.h"
#include "AOTTypeTestStretches.h"
#include "AOTVector.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/PrintStream.h>

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
    Vector<PlannedWayBackIntoLoop> waysBackIntoLoops;

    bool isGuarded(unsigned stretch) const { return stretches[stretch].outcome == StretchOutcome::Guarded; }
    bool hasGuardedStretch() const { return stretches.containsIf([](const Stretch& stretch) { return stretch.outcome == StretchOutcome::Guarded && stretch.numberOfUses; }); }
    void dump(PrintStream&, const Program&) const;
};

// A guard holds what one test or one case of a switch has proven, wherever that has not ended on any way to the guard.
enum class ProofsHeldByGuards : uint8_t { AreListed, AreLeftOut };
Vector<ExistingProof> findExistingProofs(FunctionPassContext&, const TestConditions&, ProofsHeldByGuards = ProofsHeldByGuards::AreListed);
ValueID ensureEdgeResult(IRBuilder&, FunctionAnalyses&, Edge, uint32_t proof);

bool expectedTypeTellsResultOf(FunctionPassContext&, const TestSite&);
Vector<ValueID> collectSubjectsKnownBeyondTheirTypes(FunctionPassContext&, const TestPlacementPlan&);

TestPlacementPlan planTestPlacement(FunctionPassContext&);
TestPlacementPlan planTestPlacement(FunctionPassContext&, TestSites&&);

// The blocks of the ranges that the functor picks, and every block that is reached from one of them.
BitVector findBlocksBehindRanges(const Function& function, std::span<const StretchRange> ranges, NOESCAPE const Invocable<bool(const StretchRange&)> auto& picks)
{
    BitVector isBehind(function.blocks.size());
    Vector<BlockID, 16> worklist;
    auto add = [&](BlockID block) {
        if (isBehind.add(block.index()))
            worklist.append(block);
    };
    for (const StretchRange& range : ranges) {
        if (picks(range))
            add(range.block);
    }
    while (!worklist.isEmpty())
        function.at(worklist.takeLast()).forEachTarget([&](const Target& target) { add(target.block); });
    return isBehind;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
