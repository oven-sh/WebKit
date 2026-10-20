/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlacementResults {
    const TestCoverage& coverage;
    const WantedProofs& wanted;
    const TestStretches& stretches;
};

} // anonymous namespace

static void appendTestsOnEdges(TestPlacementPlan& plan, const Function& function, std::span<const HoistedTest> hoistedTests, const PlacementResults& results)
{
    for (uint32_t index = 0; index < hoistedTests.size(); ++index) {
        if (!results.wanted.isHoistedTestWanted(index))
            continue;
        const HoistedTest& hoisted = hoistedTests[index];
        plan.tests.append(PlannedTest {
            .kind = PlannedTestKind::Test,
            .position = PlannedTestPosition::OnEdge,
            .stretch = results.stretches.stretchAtHeadOf(targetOf(function, hoisted.edge).block),
            .requirement = hoisted.requirement,
            .condition = hoisted.condition,
            .point = { },
            .edge = hoisted.edge,
            .operands = hoisted.operands,
            .renewed = { },
        });
    }
}

static Vector<TestConditionID, 2> findRenewedConditions(const TestPlacementPlan& plan, uint32_t requirement, const PlacementResults& results)
{
    BitVector renewed = results.coverage.staleBefore(requirement);
    renewed.filter(results.wanted.beforeRequirement(requirement));
    RELEASE_ASSERT(renewed.get(plan.requirements[requirement].condition.index()));

    Vector<TestConditionID, 2> conditions;
    for (size_t index : renewed)
        conditions.append(TestConditionID(index));
    return conditions;
}

static void appendTestsBeforeUses(TestPlacementPlan& plan, const PlacementResults& results)
{
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        switch (results.coverage.of(index)) {
        case RequirementCoverage::Unknown:
        case RequirementCoverage::Covered:
            break;
        case RequirementCoverage::NeedsTest:
            plan.tests.append(PlannedTest {
                .kind = PlannedTestKind::Test,
                .position = PlannedTestPosition::BeforeUse,
                .stretch = results.stretches.stretchAt(requirement.point),
                .requirement = index,
                .condition = requirement.condition,
                .point = requirement.point,
                .edge = { },
                .operands = { },
                .renewed = { },
            });
            break;
        case RequirementCoverage::NeedsRenewal:
            plan.tests.append(PlannedTest {
                .kind = PlannedTestKind::Renewal,
                .position = PlannedTestPosition::BeforeUse,
                .stretch = results.stretches.stretchAt(requirement.point),
                .requirement = index,
                .condition = { },
                .point = requirement.point,
                .edge = { },
                .operands = { },
                .renewed = findRenewedConditions(plan, index, results),
            });
            break;
        }
    }
}

// A test in front of an equal test gains nothing. It is worth its copy where its proof reaches another use.
static BitVector findTestsRepeatedInVain(const TestPlacementPlan& plan, const PlacementResults& results)
{
    using CoveredCondition = std::pair<unsigned, TestConditionID>;
    HashSet<CoveredCondition> coveredConditions;
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        if (results.coverage.of(index) == RequirementCoverage::Covered)
            coveredConditions.add({ results.stretches.stretchAt(requirement.point), requirement.condition });
    }

    BitVector sites;
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        if (plan.sites.sites[requirement.site].rule != TestSiteRuleID::RepeatedTest || results.coverage.of(index) != RequirementCoverage::NeedsTest)
            continue;
        if (!coveredConditions.contains({ results.stretches.stretchAt(requirement.point), requirement.condition }))
            sites.set(requirement.site);
    }
    return sites;
}

static void appendUses(TestPlacementPlan& plan, const PlacementResults& results)
{
    for (uint32_t first = 0; first < plan.requirements.size();) {
        uint32_t siteIndex = plan.requirements[first].site;
        const TestSite& site = plan.sites.sites[siteIndex];
        uint32_t end = first + site.numberOfTests;
        RELEASE_ASSERT(site.numberOfTests && end <= plan.requirements.size());

        bool isKnown = true;
        for (uint32_t index = first; index < end; ++index) {
            RELEASE_ASSERT(plan.requirements[index].site == siteIndex);
            isKnown &= results.coverage.of(index) != RequirementCoverage::Unknown;
        }
        if (isKnown)
            plan.uses.append(PlannedUse { results.stretches.stretchAt(site.point), siteIndex, first });
        first = end;
    }
}

static void appendSlowPaths(TestPlacementPlan& plan, const Function& function, const CFG& cfg, const PlacementResults& results)
{
    for (Edge edge : results.wanted.edgesIntoGenericCopy()) {
        BlockID target = targetOf(function, edge).block;
        if (results.wanted.isInStretchAtHead(target))
            plan.slowPaths.append(PlannedSlowPath { results.stretches.stretchAtHeadOf(target), edge });
    }
    std::ranges::sort(plan.slowPaths, { }, [&](const PlannedSlowPath& slowPath) {
        return std::pair { cfg.reversePostOrderIndex(slowPath.edge.from), slowPath.edge.targetIndex };
    });
}

static void appendWantedProofs(TestPlacementPlan& plan, std::span<const ExistingProof> proofs, const WantedProofs& wanted)
{
    for (uint32_t index = 0; index < proofs.size(); ++index) {
        if (wanted.isProofWanted(index))
            plan.proofs.append(proofs[index]);
    }
}

static void recordExceededBudgets(FunctionPassContext& context, const TestPlacementPlan& plan, const TestCoverage& coverage)
{
    const BlockCycles& cycles = context.analyses.blockCycles();
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        if (coverage.exceededBudget(index) && cycles.isInCycle(requirement.point.block))
            context.gaveUp(LimitID::MaxClonedBytesPerStretch, plan.sites.sites[requirement.site].use);
    }
}

static std::tuple<uint32_t, uint32_t, uint32_t> placementOrderOf(const CFG& cfg, const PlannedTest& test)
{
    switch (test.position) {
    case PlannedTestPosition::BeforeUse:
        return { cfg.reversePostOrderIndex(test.point.block), test.point.index, 0u };
    case PlannedTestPosition::OnEdge:
        return { cfg.reversePostOrderIndex(test.edge.from), UINT32_MAX, test.edge.targetIndex };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TestPlacementPlan planTestPlacement(FunctionPassContext& context)
{
    return planTestPlacement(context, collectTestSites(context));
}

TestPlacementPlan planTestPlacement(FunctionPassContext& context, TestSites&& sites)
{
    TestPlacementPlan plan;
    plan.sites = WTF::move(sites);
    if (plan.sites.isEmpty())
        return plan;

    plan.conditions = makeUnique<TestConditions>(context.analyses);
    plan.requirements = chooseRequirements(context, plan.sites, *plan.conditions);
    if (plan.requirements.isEmpty())
        return plan;

    Vector<ExistingProof> proofs = findExistingProofs(context, *plan.conditions);
    Vector<HoistedTest> hoistedTests = findHoistedTests(context, plan.sites, *plan.conditions, plan.requirements.span());
    TestEvents events(TestEventInputs {
        context.analyses,
        *plan.conditions,
        plan.sites.sites.span(),
        plan.requirements.span(),
        proofs.span(),
        hoistedTests.span(),
        RenewalPolicy::ByShapeEpoch,
    });
    GapBudget budget {
        TestPlacementModel::bytesSavedPerCoveredUse,
        static_cast<uint32_t>(std::min<uint64_t>(context.limit(LimitID::MaxClonedBytesPerStretch), UINT32_MAX)),
    };
    TestCoverage coverage(TestCoverageInputs { events, budget, nullptr, SlowPaths::MayLeave });
    WantedProofs wanted(events, coverage);
    TestStretches stretches(events, coverage, wanted);
    PlacementResults results { coverage, wanted, stretches };

    BitVector repeatedInVain = findTestsRepeatedInVain(plan, results);
    if (!repeatedInVain.isEmpty()) {
        uint32_t index = 0;
        plan.sites.sites.removeAllMatching([&](const TestSite&) {
            return repeatedInVain.get(index++);
        });
        return planTestPlacement(context, WTF::move(plan.sites));
    }

    appendTestsOnEdges(plan, context.function, hoistedTests.span(), results);
    appendTestsBeforeUses(plan, results);
    appendUses(plan, results);
    appendSlowPaths(plan, context.function, context.analyses.cfg(), results);
    appendWantedProofs(plan, proofs.span(), wanted);
    recordExceededBudgets(context, plan, coverage);

    const CFG& cfg = context.analyses.cfg();
    std::ranges::stable_sort(plan.tests, { }, [&](const PlannedTest& test) {
        return placementOrderOf(cfg, test);
    });

    plan.stretches = stretches.takeStretches();
    plan.ranges = stretches.takeRanges();
    return plan;
}

bool TestPlacementPlan::hasGuardedStretch() const
{
    return stretches.containsIf([](const Stretch& stretch) {
        return stretch.outcome == StretchOutcome::Guarded && stretch.numberOfUses;
    });
}

bool TestPlacementPlan::hasRenewal() const
{
    return tests.containsIf([](const PlannedTest& test) {
        return test.kind == PlannedTestKind::Renewal;
    });
}

static void dumpPlannedTest(PrintStream& out, const Program& program, const TestPlacementPlan& plan, const PlannedTest& test)
{
    switch (test.kind) {
    case PlannedTestKind::Test:
        out.print("test ");
        plan.conditions->dump(out, program, test.condition);
        break;
    case PlannedTestKind::Renewal:
        out.print("renew");
        for (TestConditionID condition : test.renewed)
            out.print(" ", condition.index());
        break;
    }

    switch (test.position) {
    case PlannedTestPosition::BeforeUse:
        out.print(" before ");
        dumpID(out, plan.sites.sites[plan.requirements[test.requirement].site].use);
        return;
    case PlannedTestPosition::OnEdge:
        out.print(" on ");
        dumpEdge(out, test.edge);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpExistingProof(PrintStream& out, const ExistingProof& proof)
{
    switch (proof.source) {
    case ProofSource::TakenEdge:
        out.print("proof taken-edge ");
        dumpEdge(out, proof.edge);
        return;
    case ProofSource::SwitchCase:
        out.print("proof switch-case ");
        dumpEdge(out, proof.edge);
        return;
    case ProofSource::GuardValue:
        out.print("proof guard ");
        dumpID(out, proof.value);
        return;
    case ProofSource::Allocation:
        out.print("proof allocation ");
        dumpID(out, proof.value);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestPlacementPlan::dump(PrintStream& out, const Program& program) const
{
    unsigned numberOfPrintedStretches = 0;
    for (unsigned stretch = 0; stretch < stretches.size(); ++stretch) {
        Vector<ValueID> usedValues;
        BitVector requiredConditions;
        for (const PlannedUse& use : uses) {
            if (use.stretch != stretch)
                continue;
            const TestSite& site = sites.sites[use.site];
            usedValues.append(site.use);
            for (const TestRequirement& requirement : requirements.subspan(use.firstRequirement, site.numberOfTests))
                requiredConditions.set(requirement.condition.index());
        }
        if (usedValues.isEmpty())
            continue;

        out.print("stretch ", numberOfPrintedStretches++, ": ");
        CommaPrinter separator("; "_s);
        for (const PlannedTest& test : tests) {
            if (test.stretch != stretch)
                continue;
            out.print(separator);
            dumpPlannedTest(out, program, *this, test);
        }
        for (const ExistingProof& proof : proofs) {
            if (!requiredConditions.get(proof.condition.index()))
                continue;
            out.print(separator);
            dumpExistingProof(out, proof);
        }
        for (const PlannedSlowPath& slowPath : slowPaths) {
            if (slowPath.stretch != stretch)
                continue;
            out.print(separator, "slow path leaves on ");
            dumpEdge(out, slowPath.edge);
        }

        std::ranges::sort(usedValues);
        out.print(separator, "uses");
        for (ValueID value : usedValues) {
            out.print(" ");
            dumpID(out, value);
        }
        out.print(separator, "cloned ", stretches[stretch].clonedBytes, "\n");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
