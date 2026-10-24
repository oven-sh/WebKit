/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeShapes.h"
#include "AOTFlowResults.h"
#include "AOTHeat.h"
#include "AOTPropertyAccess.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct WeightedCondition {
    TestConditionID condition;
    double weight { 0 };
};

// The tests of member conditions that sites of one block make for one object, with nothing between the sites that ends what such a test proves.
struct ConditionsTestsOfObject {
    ValueID object;
    Vector<uint32_t, 8> tests;
};

} // anonymous namespace

static std::optional<uint32_t> findConditionsTest(const TestSites& sites, const TestSite& site)
{
    for (uint32_t test = site.firstTest; test < site.firstTest + site.numberOfTests; ++test) {
        if (sites.tests[test].opcode == Opcode::AreMembersUnchanged)
            return test;
    }
    return std::nullopt;
}

// Where the test of its own conditions has failed, the rule for calls goes on to tests of each object. It knows that test by its conditions, so a member call and an access that calls keep it.
static ValueID objectAskedAbout(FunctionPassContext& context, const TestSite& site)
{
    const Function& function = context.function;
    const Value& use = function.at(site.use);
    switch (site.rule) {
    case TestSiteRuleID::PropertyAccess:
        return context.analyses.identityOf(use.operand(*receiverOf(use.opcode)));
    case TestSiteRuleID::Call:
        break;
    case TestSiteRuleID::EnumeratedKeyUse:
    case TestSiteRuleID::UnionReceiver:
    case TestSiteRuleID::BuiltinUse:
    case TestSiteRuleID::ElementAccess:
    case TestSiteRuleID::Operator:
    case TestSiteRuleID::RepeatedTest:
        return { };
    }

    if (use.opcode == Opcode::NewObject)
        return context.analyses.identityOf(site.use);
    if (use.opcode != Opcode::GetConstructPrototype)
        return { };
    std::optional<Use> allocation = context.analyses.useLists().singleUse(site.use);
    if (!allocation || allocation->kind != Use::Kind::Operand || function.at(allocation->user).opcode != Opcode::NewObject)
        return { };
    return context.analyses.identityOf(allocation->user);
}

static bool mayEndMemberConditions(const Effects& effects)
{
    return effects.maySuspend || effects.writes.contains(AbstractHeap::MemberGuards);
}

static auto conditionOrderOf(const MemberConditionRecord& condition)
{
    return std::tuple { condition.kind, condition.layout, condition.side, condition.key };
}

// Placement gives each set of conditions one bit, so a test costs the same however many conditions it lists.
static void shareConditionsTest(FunctionPassContext& context, TestSites& sites, BlockID block, const ConditionsTestsOfObject& group)
{
    if (group.tests.size() < 2)
        return;

    Vector<MemberConditionRecord, 8> conditions;
    for (uint32_t test : group.tests) {
        for (MemberConditionRecord condition : sites.tests[test].conditions())
            conditions.append(condition);
    }
    AOT::sort(conditions, { }, conditionOrderOf);
    conditions.shrink(conditions.size() - std::ranges::unique(conditions).size());

    Value shared = IRBuilder::make(Opcode::AreMembersUnchanged);
    shared.setConditions(conditions.span());
    if (context.analyses.findEarlierTest(shared, block) == EarlierTest::Failed)
        return;
    for (uint32_t test : group.tests)
        sites.tests[test] = shared;
}

static void shareConditionsTestsIn(FunctionPassContext& context, TestSites& sites, std::span<const TestSite> sitesOfBlock)
{
    auto numberOfConditionsTests = std::ranges::count_if(sitesOfBlock, [&](const TestSite& site) {
        return findConditionsTest(sites, site).has_value();
    });
    if (numberOfConditionsTests < 2)
        return;

    BlockID block = sitesOfBlock.front().point.block;
    const BlockValues& values = context.function.at(block).values;
    Vector<ConditionsTestsOfObject, 2> groups;
    auto shareTestsOfGroups = [&] {
        for (const ConditionsTestsOfObject& group : groups)
            shareConditionsTest(context, sites, block, group);
        groups.shrink(0);
    };

    uint32_t index = sitesOfBlock.front().point.index;
    for (const TestSite& site : sitesOfBlock) {
        for (; index < site.point.index; ++index) {
            if (mayEndMemberConditions(context.analyses.effectsOf(values[index]).normal))
                shareTestsOfGroups();
        }

        std::optional<uint32_t> test = findConditionsTest(sites, site);
        if (ValueID object = test ? objectAskedAbout(context, site) : ValueID()) {
            size_t position = groups.findIf([&](const ConditionsTestsOfObject& group) {
                return group.object == object;
            });
            if (position == notFound) {
                position = groups.size();
                groups.append(ConditionsTestsOfObject { object, { } });
            }
            groups[position].tests.append(*test);
        }

        if (mayEndMemberConditions(site.effectsWhenPassed))
            shareTestsOfGroups();
        ++index;
    }
    shareTestsOfGroups();
}

// In the copy behind a failed test each site asks for itself, so one member that has changed costs the other sites of its object one test and not their fast paths.
static void shareConditionsTests(FunctionPassContext& context, TestSites& sites)
{
    std::span<const TestSite> remaining = sites.sites.span();
    while (!remaining.empty()) {
        BlockID block = remaining.front().point.block;
        auto end = std::ranges::find_if_not(remaining, [&](const TestSite& site) {
            return site.point.block == block;
        });
        size_t count = end - remaining.begin();
        if (context.function.at(block).frequency == BlockFrequency::Normal)
            shareConditionsTestsIn(context, sites, remaining.first(count));
        remaining = remaining.subspan(count);
    }
}

namespace {

// A test of member conditions, and the point that is reached only where it has passed.
struct ConditionsTestInFront {
    ProgramPoint passedAt;
    Value test;
};

} // anonymous namespace

static bool listsConditionsOf(const Value& test, const Value& other)
{
    return std::ranges::all_of(other.conditions(), [&](const MemberConditionRecord& condition) {
        return std::ranges::contains(test.conditions(), condition);
    });
}

static Value makeTestOfConditionsOf(const Value& test)
{
    Vector<MemberConditionRecord, 8> conditions;
    for (MemberConditionRecord condition : test.conditions())
        conditions.append(condition);
    Value copy = IRBuilder::make(Opcode::AreMembersUnchanged);
    copy.setConditions(conditions.span());
    return copy;
}

// A test that has passed answers for each condition that it lists, and placement knows a test by all that it lists.
static void askForConditionsTestsInFront(FunctionPassContext& context, TestSites& sites)
{
    auto hasConditionsTest = [&](const TestSite& site) {
        return findConditionsTest(sites, site).has_value();
    };
    if (std::ranges::none_of(sites.sites, hasConditionsTest))
        return;

    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    Vector<ConditionsTestInFront, 8> testsInFront;
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        BlockID passed = terminator.taken().block;
        if (test.opcode == Opcode::AreMembersUnchanged && passed != terminator.notTaken().block && cfg.singleIncomingEdge(passed))
            testsInFront.append({ ProgramPoint::atHeadOf(passed), makeTestOfConditionsOf(test) });
    }

    auto isInFrontOf = [&](ProgramPoint passedAt, ProgramPoint point) {
        if (passedAt.block == point.block)
            return passedAt.index <= point.index;
        return context.analyses.dominators().dominates(passedAt.block, point.block);
    };
    for (const TestSite& site : sites.sites) {
        std::optional<uint32_t> test = findConditionsTest(sites, site);
        if (!test)
            continue;
        if (function.at(site.point.block).frequency == BlockFrequency::Normal && objectAskedAbout(context, site)) {
            for (const ConditionsTestInFront& candidate : testsInFront | std::views::reverse) {
                if (candidate.test.conditions().size() <= sites.tests[*test].conditions().size() || !listsConditionsOf(candidate.test, sites.tests[*test]) || !isInFrontOf(candidate.passedAt, site.point))
                    continue;
                if (context.analyses.findEarlierTest(candidate.test, site.point.block) != EarlierTest::Failed)
                    sites.tests[*test] = candidate.test;
                break;
            }
        }
        testsInFront.append({ site.point, sites.tests[*test] });
    }
}

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
    AOT::sort(conditions, [](const WeightedCondition& a, const WeightedCondition& b) {
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

// What a test buys in the cold code of a cold function, against the copy that it costs. The general form of these is one call.
static bool paysInColdCode(TestSiteRuleID rule)
{
    switch (rule) {
    case TestSiteRuleID::ElementAccess:
    case TestSiteRuleID::Operator:
        return false;
    default:
        return true;
    }
}

// Without looking at the blocks: none of them runs more often than the function is entered.
static bool isColdEverywhere(FunctionPassContext& context)
{
    const Heat& heat = context.inputs.heat;
    return isCold(heat.classOf(heat.activationsOf(context.program, context.id) * roundingOfFrequencySums)) && runsNoBlockMoreOftenThanEntry(context.analyses.cfg());
}

// A test for a string, a number or an object of a built-in class says nothing of layouts.
static bool asksOnlyForLayouts(FunctionPassContext& context, const TestSites& sites, const TestSite& site)
{
    return std::ranges::none_of(sites.tests.span().subspan(site.firstTest, site.numberOfTests), [&](const Value& test) {
        return test.opcode == Opcode::TypeTest && !context.types.at(test.testedType()).layouts;
    });
}

// In a checked function of the program the type of a variable, a parameter or a field is the checker's, and so is that of an element read out of it. The library is handed what its callers have.
static bool isDescribedByChecker(FunctionPassContext& context, ValueID value)
{
    if (!context.function.has(FunctionFlags::IsChecked) || isLibraryCode(context.program, context.function))
        return false;
    // It ends: a list is defined in front of the read of its element.
    for (;;) {
        ValueID identity = context.analyses.identityOf(value);
        if (context.program.expectedOrTypeOf(context.function, identity).anyObject)
            return false;
        const Value& definition = context.function.at(identity);
        if (definition.opcode != Opcode::GetArrayElement && definition.opcode != Opcode::ElemGet)
            return true;
        value = definition.operand(0);
    }
}

// The object that a site asks for a name, where nothing but a guess says that it has a layout.
static ValueID findReceiverWithGuessedLayout(FunctionPassContext& context, const TestSites& sites, const TestSite& site)
{
    const Value& use = context.function.at(site.use);
    if (!isNamedAccess(use.opcode) || !asksOnlyForLayouts(context, sites, site))
        return { };
    // A site without a test of a type guesses nothing: `view.length` of a typed array asks whether the array is as it was made.
    bool testsType = std::ranges::contains(sites.tests.span().subspan(site.firstTest, site.numberOfTests), Opcode::TypeTest, &Value::opcode);
    if (!testsType)
        return { };
    ValueID receiver = use.operand(*receiverOf(use.opcode));
    if (context.program.typeOf(context.function, receiver).layouts || isDescribedByChecker(context, receiver))
        return { };
    return context.analyses.identityOf(receiver);
}

// Object.defineProperty(this, "inner", { value }) in a plain function: the program defines some names on objects without a layout too. An object that is asked for such names alone may well be one of those.
// A test for the layouts that have the names would turn it away, and what stands behind the test would run in the copy.
static BitVector findReceiversAskedOnlyForNamesDefinedWithoutLayout(FunctionPassContext& context, const TestSites& sites)
{
    BitVector receivers(context.function.values.size());
    BitVector receiversAskedForOtherNames(context.function.values.size());
    for (const TestSite& site : sites.sites) {
        ValueID receiver = findReceiverWithGuessedLayout(context, sites, site);
        if (!receiver)
            continue;
        const Value& use = context.function.at(site.use);
        // What the analysis has seen arrive does arrive, whatever else does.
        bool isSeenWithLayout = PayloadWord::receivers(use.opcode) && use.receivers();
        bool isFoundWithoutLayout = !isSeenWithLayout && context.program.isDefinedOnObjectsWithoutKnownLayout(keyOfNamedAccess(context.program, use));
        (isFoundWithoutLayout ? receivers : receiversAskedForOtherNames).quickSet(receiver.index());
    }
    receivers.exclude(receiversAskedForOtherNames);
    return receivers;
}

TestSites collectTestSites(FunctionPassContext& context)
{
    if (!context.build.recordedOptions().usesExpectedTypes())
        return { };
    // Most compact functions have no loop at all, and they are nearly half of a very large program: no rule looks at them.
    bool isCompactFunction = context.inputs.heat.isCompact(context.program, context.id);
    if (isCompactFunction && isColdEverywhere(context))
        return { };

    TestSiteReporter reporter(context);
    for (const TestSiteRule& rule : testSiteRules()) {
        reporter.beginRule(rule.id);
        rule.report(reporter);
    }

    TestSites sites = reporter.takeSites();
    for (const TestSite& site : sites.sites)
        RELEASE_ASSERT(testSiteOpcodes.contains(context.function.at(site.use).opcode));
    // A test buys a fast path and costs a copy of what stands behind it.
    bool isColdFunction = isCold(context.inputs.heat.of(context.program, context.id));
    BitVector receiversThatMayHaveNoLayout = findReceiversAskedOnlyForNamesDefinedWithoutLayout(context, sites);
    sites.sites.removeAllMatching([&](const TestSite& site) {
        if (ValueID receiver = receiversThatMayHaveNoLayout.isEmpty() ? ValueID() : findReceiverWithGuessedLayout(context, sites, site); receiver && receiversThatMayHaveNoLayout.quickGet(receiver.index()))
            return true;
        // What a plain function without a layout constructs arrives as the receiver. A test for a layout would turn it away, and what stands behind the test would run in the copy.
        const Value& use = context.function.at(site.use);
        if (PayloadWord::accessSite(use.opcode) && use.accessSite() && context.inputs.flow.lossOf(use.accessSite()).seesConstructedObject && asksOnlyForLayouts(context, sites, site))
            return true;
        HeatClass heatClass = heatClassOf(context, site.point.block);
        return heatClass == HeatClass::RunsOnce || (isColdFunction && heatClass == HeatClass::Cold && (isCompactFunction || !paysInColdCode(site.rule)));
    });
    const CFG& cfg = context.analyses.cfg();
    AOT::sort(sites.sites, { }, [&](const TestSite& site) {
        return std::pair { cfg.reversePostOrderIndex(site.point.block), site.point.index };
    });
    shareConditionsTests(context, sites);
    askForConditionsTestsInFront(context, sites);
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
