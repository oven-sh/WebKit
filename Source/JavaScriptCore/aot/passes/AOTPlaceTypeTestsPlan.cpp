/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTGenericCopy.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
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

// Plans tests that establish again what the tested code wants where a loop is entered, about values that are defined in front of the loop.
class WayBackPlanner {
    WTF_MAKE_NONCOPYABLE(WayBackPlanner);
public:
    WayBackPlanner(FunctionPassContext&, const TestPlacementPlan&, const TestCoverage&);

    std::optional<Vector<PlannedTest, 4>> planTests(const PlannedWayBackIntoLoop&, const BitVector& wanted);

private:
    enum class State : uint8_t { NotPlanned, BeingPlanned, Planned };
    using Operands = Vector<ValueID, 2>;

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_context.types.at(m_context.function.at(value).type); }

    bool planTest(TestConditionID);
    std::optional<Operands> findOperandsInFrontOfLoop(uint32_t requirement);
    ValueID findDefinitionInFrontOfLoop(ValueID);

    FunctionPassContext& m_context;
    const TestPlacementPlan& m_plan;
    Vector<Vector<uint32_t, 2>> m_requirementsOfCondition;
    Vector<State> m_states;
    const PlannedWayBackIntoLoop* m_wayBack { nullptr };
    Vector<PlannedTest, 4> m_tests;
};

WayBackPlanner::WayBackPlanner(FunctionPassContext& context, const TestPlacementPlan& plan, const TestCoverage& coverage)
    : m_context(context)
    , m_plan(plan)
    , m_requirementsOfCondition(plan.conditions->size())
    , m_states(FillWith { }, plan.conditions->size(), State::NotPlanned)
{
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        if (coverage.of(index) != RequirementCoverage::Unknown)
            m_requirementsOfCondition[plan.requirements[index].condition.index()].append(index);
    }
}

std::optional<Vector<PlannedTest, 4>> WayBackPlanner::planTests(const PlannedWayBackIntoLoop& wayBack, const BitVector& wanted)
{
    m_tests.shrink(0);
    if (wanted.isEmpty())
        return std::exchange(m_tests, { });

    m_wayBack = &wayBack;
    m_states.fill(State::NotPlanned);
    for (const TestRequirement& requirement : m_plan.requirements) {
        if (wanted.get(requirement.condition.index()) && !planTest(requirement.condition))
            return std::nullopt;
    }
    return std::exchange(m_tests, { });
}

bool WayBackPlanner::planTest(TestConditionID condition)
{
    switch (m_states[condition.index()]) {
    case State::Planned:
        return true;
    case State::BeingPlanned:
        return false;
    case State::NotPlanned:
        break;
    }
    m_states[condition.index()] = State::BeingPlanned;

    for (uint32_t requirement : m_requirementsOfCondition[condition.index()]) {
        std::optional<Operands> operands = findOperandsInFrontOfLoop(requirement);
        if (!operands)
            continue;

        bool hasPrerequisites = true;
        forEachTestPrerequisite(m_plan.requirements.span(), *m_plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
            hasPrerequisites = hasPrerequisites && planTest(prerequisite.condition);
        });
        if (!hasPrerequisites)
            return false;

        m_tests.append(PlannedTest {
            .position = PlannedTestPosition::OnEdge,
            .stretch = m_wayBack->stretch,
            .requirement = requirement,
            .condition = condition,
            .point = { },
            .edge = m_wayBack->backEdges.first(),
            .operands = WTF::move(*operands),
        });
        m_states[condition.index()] = State::Planned;
        return true;
    }
    return false;
}

auto WayBackPlanner::findOperandsInFrontOfLoop(uint32_t requirement) -> std::optional<Operands>
{
    const Value& test = m_plan.sites.tests[m_plan.requirements[requirement].test];
    std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);

    BitVector isNarrowedByEarlierTest;
    forEachTestPrerequisite(m_plan.requirements.span(), *m_plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
        if (prerequisite.narrowedOperand)
            isNarrowedByEarlierTest.set(*prerequisite.narrowedOperand);
    });

    Operands operands;
    for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
        if (guardIndex == operandIndex)
            continue;

        ValueID operand = test.operand(operandIndex);
        ValueID inFront = findDefinitionInFrontOfLoop(operand);
        if (!inFront)
            return std::nullopt;

        bool canReplaceOperand = inFront == operand
            || isNarrowedByEarlierTest.get(operandIndex)
            || test.operandKind(operandIndex) == OperandKind::Any
            || typeOf(inFront).isSubsetOf(typeOf(operand), m_context.program);
        if (!canReplaceOperand)
            return std::nullopt;
        operands.append(inFront);
    }
    return operands;
}

ValueID WayBackPlanner::findDefinitionInFrontOfLoop(ValueID value)
{
    FunctionAnalyses& analyses = m_context.analyses;
    ProgramPoint head = ProgramPoint::atHeadOf(m_wayBack->head);
    for (ValueID candidate : CastChain(m_context.function, value)) {
        if (analyses.useLists().blockOf(candidate) != head.block && analyses.dominates(candidate, head))
            return candidate;
    }
    return { };
}

} // anonymous namespace

static void appendTestsOnEdges(TestPlacementPlan& plan, const Function& function, std::span<const HoistedTest> hoistedTests, const PlacementResults& results)
{
    for (uint32_t index = 0; index < hoistedTests.size(); ++index) {
        if (!results.wanted.isHoistedTestWanted(index))
            continue;
        const HoistedTest& hoisted = hoistedTests[index];
        plan.tests.append(PlannedTest {
            .position = PlannedTestPosition::OnEdge,
            .stretch = results.stretches.stretchAtHeadOf(targetOf(function, hoisted.edge).block),
            .requirement = hoisted.requirement,
            .condition = hoisted.condition,
            .point = { },
            .edge = hoisted.edge,
            .operands = hoisted.operands,
        });
    }
}

static void appendTestsBeforeUses(TestPlacementPlan& plan, const PlacementResults& results)
{
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        if (results.coverage.of(index) != RequirementCoverage::NeedsTest)
            continue;
        const TestRequirement& requirement = plan.requirements[index];
        plan.tests.append(PlannedTest {
            .position = PlannedTestPosition::BeforeUse,
            .stretch = results.stretches.stretchAt(requirement.point),
            .requirement = index,
            .condition = requirement.condition,
            .point = requirement.point,
            .edge = { },
            .operands = { },
        });
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

// Where the tested code wants none of the proofs of this plan, the generic copy may come back.
static void appendRejoinPoints(TestPlacementPlan& plan, FunctionPassContext& context, const TestStretches& stretches, WantedProofs& wanted)
{
    FunctionAnalyses& analyses = context.analyses;
    Vector<const TestSite*> siteOfValue(FillWith { }, context.function.values.size(), nullptr);
    for (const PlannedUse& use : plan.uses) {
        const TestSite& site = plan.sites.sites[use.site];
        siteOfValue[site.use.index()] = &site;
    }

    auto mayCallUnknownCode = [&](ValueID value) {
        const TestSite* site = siteOfValue[value.index()];
        return site ? site->effectsWhenPassed.mayCallUnknownCode : analyses.mayCallUnknownCode(value);
    };
    // A range is copied as one block.
    auto mayRejoinAt = [&](ProgramPoint point) {
        return !wanted.isAnyProofWantedAt(point) && !stretches.isInsideRange(point);
    };
    plan.rejoinPoints = findRejoinPoints(analyses, mayCallUnknownCode, mayRejoinAt);

    // Code that is rarer than what leads to it is a slow path already: the generic version of the function, or the copy of an earlier run. The copy of this run goes on there and copies none of it.
    const Function& function = context.function;
    const CFG& cfg = analyses.cfg();
    for (BlockID block : cfg.reversePostOrder()) {
        if (function.at(block).isHandlerBlock())
            continue;
        bool isRarerThanPredecessor = std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
            return function.at(edge.from).frequency < function.at(block).frequency;
        });
        // An arm that goes straight back is part of the code it left: what the copy brought in there would meet the tested values at the next join.
        bool goesStraightBack = std::ranges::any_of(function.at(block).terminator.targets, [&](const Target& target) {
            return function.at(target.block).frequency < function.at(block).frequency;
        });
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        if (isRarerThanPredecessor && !goesStraightBack && mayRejoinAt(head))
            plan.rejoinPoints.append(head);
    }
    std::ranges::sort(plan.rejoinPoints, { }, [&](ProgramPoint point) {
        return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
    });
    removeRepeatedElements(plan.rejoinPoints);
}

bool expectedTypeTellsResultOf(FunctionPassContext& context, const TestSite& site)
{
    switch (site.rule) {
    case TestSiteRuleID::BuiltinUse:
    case TestSiteRuleID::Call:
        return false;
    case TestSiteRuleID::EnumeratedKeyUse:
    case TestSiteRuleID::UnionReceiver:
    case TestSiteRuleID::PropertyAccess:
    case TestSiteRuleID::ElementAccess:
    case TestSiteRuleID::Operator:
    case TestSiteRuleID::RepeatedTest:
        break;
    }
    const Value& use = context.function.at(site.use);
    if (!use.type || use.expected())
        return true;
    TypeID expectedNow = expectedTypeOf(context.program, context.function, use, context.types, &context.typingMemo);
    return !expectedNow || expectedNow == use.type;
}

Vector<ValueID> collectSubjectsKnownBeyondTheirTypes(FunctionPassContext& context, const TestPlacementPlan& plan)
{
    const Function& function = context.function;
    BitVector isSubject(function.values.size());
    for (const PlannedTest& test : plan.tests) {
        const TestCondition& condition = plan.conditions->at(test.condition);
        bool isAboutCollection = plan.sites.sites[plan.requirements[test.requirement].site].rule == TestSiteRuleID::ElementAccess;
        if (condition.subject && (isAboutCollection || condition.lastsForever()))
            isSubject.set(condition.subject.index());
    }

    Vector<ValueID> subjects;
    if (isSubject.isEmpty())
        return subjects;
    const ValueIdentities& identities = context.analyses.identities();
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        if (isSubject.get(identities.identityOf(ValueID(index)).index()))
            subjects.append(ValueID(index));
    }
    return subjects;
}

static BitVector findPointsWhereNothingIsKnownBetter(FunctionPassContext& context, BlockID block, uint32_t firstIndex, const BitVector& isKnownBetter, NOESCAPE const ScopedLambda<const Liveness&()>& livenessOfValuesKnownBetter)
{
    const Function& function = context.function;
    const Block& contents = function.at(block);
    const BlockValues& values = contents.values;
    BitVector points(values.size());
    if (isKnownBetter.isEmpty()) {
        for (uint32_t index = std::max(firstIndex, 1u); index < values.size(); ++index)
            points.quickSet(index);
        return points;
    }

    const Liveness& liveness = livenessOfValuesKnownBetter();
    Vector<ValueID, 8> live;
    auto use = [&](ValueID value) {
        if (value && isKnownBetter.get(value.index()))
            live.appendIfNotContains(value);
    };
    contents.forEachChildOutsideValues(use);
    contents.forEachTarget([&](const Target& target) {
        for (ValueID value : liveness.liveAtHead(target.block))
            use(value);
    });
    for (uint32_t index = values.size(); index-- > std::max(firstIndex, 1u);) {
        live.removeFirst(values[index]);
        function.at(values[index]).forEachOperand([&](ValueID operand, uint32_t) {
            use(operand);
        });
        if (live.isEmpty())
            points.quickSet(index);
    }
    return points;
}

static void appendRejoinPointsWithoutTests(TestPlacementPlan& plan, FunctionPassContext& context, const TestStretches& stretches, WantedProofs& wanted)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    auto mayRejoinAt = [&](ProgramPoint point) {
        return !wanted.isAnyProofWantedAt(point) && !stretches.isInsideRange(point);
    };

    std::span<const StretchRange> ranges = stretches.ranges();
    BitVector isBehindCopiedRange(function.blocks.size());
    Vector<BlockID, 16> worklist;
    for (const StretchRange& range : ranges) {
        if (range.isCloned && isBehindCopiedRange.add(range.block.index()))
            worklist.append(range.block);
    }
    if (worklist.isEmpty())
        return;
    while (!worklist.isEmpty()) {
        function.at(worklist.takeLast()).forEachTarget([&](const Target& target) {
            if (isBehindCopiedRange.add(target.block.index()))
                worklist.append(target.block);
        });
    }

    for (BlockID block : cfg.reversePostOrder()) {
        if (!isBehindCopiedRange.get(block.index()))
            continue;
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        if (block != cfg.root() && !function.at(block).isHandlerBlock() && mayRejoinAt(head))
            plan.rejoinPointsWithoutTests.append(head);
    }

    Vector<ValueID> narrowedAsExpected;
    Vector<ValueID> narrowedInUnknownWay;
    for (const PlannedUse& use : plan.uses) {
        const TestSite& site = plan.sites.sites[use.site];
        (expectedTypeTellsResultOf(context, site) ? narrowedAsExpected : narrowedInUnknownWay).append(site.use);
    }
    Vector<ValueID> subjects = collectSubjectsKnownBeyondTheirTypes(context, plan);
    BitVector isKnownBetter = findValuesTypedCodeKnowsMoreAbout(context.analyses, context.types, { narrowedAsExpected.span(), narrowedInUnknownWay.span(), subjects.span() });

    // Nothing is asked about the other values, and they are most of the function.
    std::optional<Liveness> ownLiveness;
    auto livenessOfValuesKnownBetter = [&]() -> const Liveness& {
        if (context.analyses.computed().contains(FunctionAnalysis::Liveness))
            return context.analyses.liveness();
        if (!ownLiveness) {
            LivenessOptions options;
            options.trackedValues = &isKnownBetter;
            ownLiveness.emplace(function, cfg, context.analyses.useLists(), options);
        }
        return *ownLiveness;
    };

    for (size_t first = 0; first < ranges.size();) {
        BlockID block = ranges[first].block;
        uint32_t numberOfValues = function.at(block).values.size();
        uint32_t firstEnd = numberOfValues;
        size_t next = first;
        for (; next < ranges.size() && ranges[next].block == block; ++next) {
            if (ranges[next].isCloned)
                firstEnd = std::min(firstEnd, ranges[next].end);
        }
        if (firstEnd < numberOfValues) {
            BitVector points = findPointsWhereNothingIsKnownBetter(context, block, firstEnd, isKnownBetter, livenessOfValuesKnownBetter);
            for (const StretchRange& range : ranges.subspan(first, next - first)) {
                if (!range.isCloned)
                    continue;
                for (uint32_t index = range.end; index < numberOfValues; ++index) {
                    if (!points.quickGet(index) || !mayRejoinAt({ block, index }))
                        continue;
                    plan.rejoinPointsWithoutTests.append({ block, index });
                    break;
                }
            }
        }
        first = next;
    }

    auto orderOf = [&](ProgramPoint point) {
        return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
    };
    std::ranges::sort(plan.rejoinPointsWithoutTests, { }, orderOf);
    removeRepeatedElements(plan.rejoinPointsWithoutTests);
    plan.rejoinPointsWithoutTests.removeAllMatching([&](ProgramPoint point) {
        return std::ranges::binary_search(plan.rejoinPoints, orderOf(point), { }, orderOf);
    });
}

// A test whose proof lasts forever, about values made in front of the loop, fails at every turn once it has failed. Where only such tests lead into the copy, it would come back in vain.
static bool mayPassAtLaterTurn(const TestPlacementPlan& plan, FunctionPassContext& context, unsigned stretch, const NaturalLoop& loop)
{
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const UseLists& useLists = context.analyses.useLists();
    bool hasSlowPath = plan.slowPaths.containsIf([&](const PlannedSlowPath& slowPath) {
        return slowPath.stretch == stretch;
    });
    return hasSlowPath || plan.tests.containsIf([&](const PlannedTest& test) {
        if (test.stretch != stretch)
            return false;
        const TestCondition& condition = plan.conditions->at(test.condition);
        if (!condition.lastsForever())
            return true;
        auto isMadeInLoop = [&](ValueID operand) {
            BlockID block = useLists.blockOf(operand);
            return block && loops.belongsTo(block, loop);
        };
        if (test.position == PlannedTestPosition::OnEdge)
            return std::ranges::any_of(test.operands, isMadeInLoop);
        bool namesValueMadeInLoop = false;
        condition.test.forEachOperand([&](ValueID operand, uint32_t) {
            namesValueMadeInLoop |= isMadeInLoop(operand);
        });
        return namesValueMadeInLoop;
    });
}

// A failed test costs one turn of a loop: at the end of the turn the generic copy makes the tests again that the head of the loop relies on.
static void appendWaysBackIntoLoops(TestPlacementPlan& plan, FunctionPassContext& context, const PlacementResults& results, WantedProofs& wanted)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    if (!cfg.hasCycle())
        return;
    const NaturalLoops& loops = context.analyses.naturalLoops();
    if (!loops.numLoops())
        return;

    std::span<const StretchRange> ranges = results.stretches.ranges();
    std::span<const Edge> edgesIntoGenericCopy = wanted.edgesIntoGenericCopy();
    std::optional<WayBackPlanner> planner;
    for (BlockID head : cfg.reversePostOrder()) {
        const NaturalLoop* loop = loops.headerOf(head);
        if (!loop || !wanted.isInStretchAtHead(head))
            continue;
        auto firstRange = std::ranges::find(ranges, head, &StretchRange::block);
        RELEASE_ASSERT(firstRange != ranges.end());
        if (!firstRange->isCloned)
            continue;

        PlannedWayBackIntoLoop wayBack { firstRange->stretch, head, { }, { }, { } };
        std::optional<Edge> enteringEdge;
        bool hasTestsOnEnteringEdge = false;
        bool isEnteredWithArgumentsOnly = true;
        for (Edge edge : cfg.incomingEdges(head)) {
            if (edge.isHandlerEdge() || numberOfEdgeResults(function, edge.from, edge.targetIndex)) {
                isEnteredWithArgumentsOnly = false;
                break;
            }
            if (std::ranges::contains(edgesIntoGenericCopy, edge))
                continue;
            if (loops.belongsTo(edge.from, *loop)) {
                wayBack.backEdges.append(edge);
                continue;
            }
            bool hasTests = plan.tests.containsIf([&](const PlannedTest& test) {
                return test.position == PlannedTestPosition::OnEdge && test.edge == edge;
            });
            if (!enteringEdge || (hasTests && !hasTestsOnEnteringEdge)) {
                enteringEdge = edge;
                hasTestsOnEnteringEdge = hasTests;
            }
        }
        if (!isEnteredWithArgumentsOnly || !enteringEdge || wayBack.backEdges.isEmpty())
            continue;
        if (!mayPassAtLaterTurn(plan, context, wayBack.stretch, *loop))
            continue;
        wayBack.enteringEdge = *enteringEdge;

        if (!planner)
            planner.emplace(context, plan, results.coverage);
        std::optional<Vector<PlannedTest, 4>> tests = planner->planTests(wayBack, wanted.wantedInFrontOfTestsOn(*enteringEdge));
        if (!tests)
            continue;
        wayBack.tests = WTF::move(*tests);
        plan.waysBackIntoLoops.append(WTF::move(wayBack));
    }
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
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        if (coverage.exceededBudget(index))
            context.gaveUp(LimitID::MaxClonedBytesPerStretch, plan.sites.sites[plan.requirements[index].site].use);
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
    });
    // The generic copy goes on behind a stretch, so it holds the code of a gap whether a proof bridges the gap or not.
    uint32_t widestGap = static_cast<uint32_t>(std::min<uint64_t>(context.limit(LimitID::MaxClonedBytesPerStretch), UINT32_MAX));
    TestCoverage coverage(TestCoverageInputs { events, GapBudget { widestGap, widestGap }, nullptr, SlowPaths::MayLeave });
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
    appendRejoinPoints(plan, context, stretches, wanted);
    appendRejoinPointsWithoutTests(plan, context, stretches, wanted);
    appendWaysBackIntoLoops(plan, context, results, wanted);
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

static void dumpPlannedTest(PrintStream& out, const Program& program, const TestPlacementPlan& plan, const PlannedTest& test)
{
    out.print("test ");
    plan.conditions->dump(out, program, test.condition);

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
        out.print(separator, "cloned ", stretches[stretch].clonedBytes);
        for (const PlannedWayBackIntoLoop& wayBack : waysBackIntoLoops) {
            if (wayBack.stretch != stretch)
                continue;
            out.print(separator, "way back into ");
            dumpID(out, wayBack.head);
            out.print(" joins ");
            dumpEdge(out, wayBack.enteringEdge);
            for (const PlannedTest& test : wayBack.tests) {
                out.print(", test ");
                conditions->dump(out, program, test.condition);
            }
        }
        out.print("\n");
    }

    if (!numberOfPrintedStretches || rejoinPoints.isEmpty())
        return;
    out.print("copy comes back at");
    for (ProgramPoint point : rejoinPoints) {
        out.print(" ");
        dumpPoint(out, point);
    }
    out.print("\n");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
