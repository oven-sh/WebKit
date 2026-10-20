/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeTestStretches.h"
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace TestPlacementModel {

inline constexpr uint32_t bytesSavedPerCoveredUse = 32;

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

} // namespace TestPlacementModel

enum class PlannedTestKind : uint8_t { Test, Renewal };
enum class PlannedTestPosition : uint8_t { BeforeUse, OnEdge };

struct PlannedTest {
    PlannedTestKind kind { PlannedTestKind::Test };
    PlannedTestPosition position { PlannedTestPosition::BeforeUse };
    unsigned stretch { 0 };
    uint32_t requirement { 0 };
    TestConditionID condition;
    ProgramPoint point;
    Edge edge;
    Vector<ValueID, 2> operands;
    Vector<TestConditionID, 2> renewed;
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

    bool isGuarded(unsigned stretch) const { return stretches[stretch].outcome == StretchOutcome::Guarded; }
    bool hasGuardedStretch() const;
    bool hasRenewal() const;
    void dump(PrintStream&, const Program&) const;
};

TestSites collectTestSites(FunctionPassContext&);
Vector<TestRequirement> chooseRequirements(FunctionPassContext&, TestSites&, TestConditions&);
Vector<ExistingProof> findExistingProofs(FunctionPassContext&, const TestConditions&);
ValueID ensureEdgeResult(IRBuilder&, FunctionAnalyses&, Edge, uint32_t proof);
Vector<HoistedTest> findHoistedTests(FunctionPassContext&, const TestSites&, const TestConditions&, std::span<const TestRequirement>);

TestPlacementPlan planTestPlacement(FunctionPassContext&);
TestPlacementPlan planTestPlacement(FunctionPassContext&, TestSites&&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
