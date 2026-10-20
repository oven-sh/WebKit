/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct WeightedCondition {
    TestConditionID condition;
    double weight { 0 };
};

} // anonymous namespace

static Vector<TestConditionID> addConditionsOfTests(const TestSites& sites, TestConditions& conditions)
{
    Vector<TestConditionID> conditionOfTest(FillWith { }, sites.tests.size(), TestConditionID());
    for (const TestSite& site : sites.sites) {
        for (uint32_t test = site.firstTest; test < site.firstTest + site.numberOfTests; ++test)
            conditionOfTest[test] = conditions.add(sites.tests[test]);
    }
    return conditionOfTest;
}

static Vector<WeightedCondition> computeConditionWeights(FunctionPassContext& context, const TestSites& sites, std::span<const TestConditionID> conditionOfTest, unsigned numberOfConditions, const BitVector& dropped)
{
    Vector<double> weights(FillWith { }, numberOfConditions, 0.0);
    BitVector isNeeded(numberOfConditions);
    for (size_t index = 0; index < sites.sites.size(); ++index) {
        if (dropped.get(index))
            continue;
        const TestSite& site = sites.sites[index];
        double heat = context.heatOf(site.point.block);
        for (TestConditionID condition : conditionOfTest.subspan(site.firstTest, site.numberOfTests)) {
            isNeeded.set(condition.index());
            weights[condition.index()] += heat;
        }
    }

    Vector<WeightedCondition> neededConditions;
    for (size_t condition : isNeeded)
        neededConditions.append(WeightedCondition { TestConditionID(condition), weights[condition] });
    return neededConditions;
}

static BitVector findHeaviestConditions(Vector<WeightedCondition>&& conditions, size_t count)
{
    std::ranges::sort(conditions, [](const WeightedCondition& a, const WeightedCondition& b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return a.condition < b.condition;
    });

    BitVector heaviest;
    for (const WeightedCondition& weighted : conditions.span().first(count))
        heaviest.set(weighted.condition.index());
    return heaviest;
}

static void applyConditionLimit(FunctionPassContext& context, const TestSites& sites, std::span<const TestConditionID> conditionOfTest, unsigned numberOfConditions, BitVector& dropped)
{
    uint64_t limit = context.limit(LimitID::MaxTestedTypesPerFunction);
    if (numberOfConditions <= limit)
        return;

    Vector<WeightedCondition> neededConditions = computeConditionWeights(context, sites, conditionOfTest, numberOfConditions, dropped);
    if (neededConditions.size() <= limit)
        return;

    BitVector isChosen = findHeaviestConditions(WTF::move(neededConditions), static_cast<size_t>(limit));
    for (size_t index = 0; index < sites.sites.size(); ++index) {
        if (dropped.get(index))
            continue;
        const TestSite& site = sites.sites[index];
        bool needsOnlyChosenConditions = std::ranges::all_of(conditionOfTest.subspan(site.firstTest, site.numberOfTests), [&](TestConditionID condition) {
            return isChosen.get(condition.index());
        });
        if (needsOnlyChosenConditions)
            continue;
        dropped.set(index);
        context.gaveUp(LimitID::MaxTestedTypesPerFunction, site.use);
    }
}

static void removeDroppedSites(TestSites& sites, const BitVector& dropped)
{
    if (dropped.isEmpty())
        return;

    size_t numberOfRemainingSites = 0;
    for (size_t index = 0; index < sites.sites.size(); ++index) {
        if (dropped.get(index))
            continue;
        if (numberOfRemainingSites != index)
            sites.sites[numberOfRemainingSites] = WTF::move(sites.sites[index]);
        ++numberOfRemainingSites;
    }
    sites.sites.shrink(numberOfRemainingSites);
}

TestSites collectTestSites(FunctionPassContext& context)
{
    if (!context.build.recordedOptions().usesPredictions())
        return { };

    TestSiteReporter reporter(context);
    for (const TestSiteRule& rule : testSiteRules()) {
        reporter.beginRule(rule.id);
        rule.report(reporter);
    }

    TestSites sites = reporter.takeSites();
    const CFG& cfg = context.analyses.cfg();
    std::ranges::sort(sites.sites, { }, [&](const TestSite& site) {
        return std::pair { cfg.reversePostOrderIndex(site.point.block), site.point.index };
    });
    return sites;
}

Vector<TestRequirement> chooseRequirements(FunctionPassContext& context, TestSites& sites, TestConditions& conditions)
{
    RELEASE_ASSERT(!conditions.size());

    TestConditions allConditions(context.analyses);
    Vector<TestConditionID> conditionOfTest = addConditionsOfTests(sites, allConditions);

    BitVector dropped;
    applyConditionLimit(context, sites, conditionOfTest.span(), allConditions.size(), dropped);
    removeDroppedSites(sites, dropped);

    Vector<TestRequirement> requirements;
    for (uint32_t index = 0; index < sites.sites.size(); ++index) {
        const TestSite& site = sites.sites[index];
        for (uint32_t test = site.firstTest; test < site.firstTest + site.numberOfTests; ++test)
            requirements.append(TestRequirement { index, test, conditions.add(sites.tests[test]), site.point, RequirementKind::PlacesTest });
    }
    return requirements;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
