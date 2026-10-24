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
#include "AOTFrameAllocationPointers.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTPropertyAccess.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include <algorithm>
#include <ranges>
#include <tuple>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// The sites.

FailedConditions::FailedConditions(FunctionAnalyses& analyses)
    : m_conditions(analyses)
{
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();
    std::span<const BlockID> order = cfg.reversePostOrder();
    BitVector isPending;
    auto failedAtHeadOf = [&](BlockID block) -> BitVector& {
        std::optional<BitVector>& failed = m_failedAtHead[block.index()];
        if (!failed)
            failed.emplace();
        return *failed;
    };
    auto addFailingEdge = [&](const Target& failing, const Value& test) {
        if (m_failedAtHead.isEmpty()) {
            m_failedAtHead.grow(function.blocks.size());
            isPending.ensureSize(order.size());
        }
        if (failedAtHeadOf(failing.block).add(m_conditions.add(test).index()))
            isPending.quickSet(cfg.reversePostOrderIndex(failing.block));
    };
    auto leavesNormalCode = [&](const Target& failing) { return function.at(failing.block).frequency != BlockFrequency::Normal; };
    for (BlockID block : order) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.isConditionalBranch() && leavesNormalCode(terminator.notTaken())) {
            const Value& test = function.at(terminator.conditionOperand());
            if (provesOf(test.opcode) != Proof::None)
                addFailingEdge(terminator.notTaken(), test);
        } else if (terminator.kind == TerminatorKind::SwitchOnShape && leavesNormalCode(terminator.otherwise())) {
            for (LayoutSetID layouts : terminator.layoutSets())
                addFailingEdge(terminator.otherwise(), makeShapeTest(terminator.subjectOperand(), layouts));
        }
    }

    // What has failed goes on along every edge and is forgotten where normal code is entered: it is known in the marked blocks behind the failing edges and nowhere else.
    for (bool shouldRepeat = !m_failedAtHead.isEmpty(); shouldRepeat;) {
        shouldRepeat = false;
        for (size_t position : isPending) {
            isPending.quickClear(position);
            BlockID block = order[position];
            function.at(block).forEachTarget([&](const Target& target) {
                if (target.block == block || function.at(target.block).frequency == BlockFrequency::Normal)
                    return;
                BitVector& failed = failedAtHeadOf(target.block);
                size_t numberOfConditions = failed.bitCount();
                failed.merge(*m_failedAtHead[block.index()]);
                if (failed.bitCount() == numberOfConditions)
                    return;
                uint32_t positionOfTarget = cfg.reversePostOrderIndex(target.block);
                isPending.quickSet(positionOfTarget);
                shouldRepeat |= positionOfTarget <= position;
            });
        }
    }
}

bool FailedConditions::mayHaveFailed(const Value& test, BlockID at) const
{
    if (m_failedAtHead.isEmpty() || !m_failedAtHead[at.index()] || m_failedAtHead[at.index()]->isEmpty())
        return false;
    std::optional<TestConditionID> condition = m_conditions.find(test);
    return condition && m_failedAtHead[at.index()]->get(condition->index());
}

bool FailedConditions::mayHaveFailedTestOf(ValueID subject, BlockID at) const
{
    if (m_failedAtHead.isEmpty() || !m_failedAtHead[at.index()])
        return false;
    for (size_t condition : *m_failedAtHead[at.index()]) {
        if (m_conditions.at(TestConditionID(condition)).subject == subject)
            return true;
    }
    return false;
}

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
    const Value& use = context.function.at(site.use);
    if (site.rule == TestSiteRuleID::PropertyAccess)
        return context.analyses.identityOf(use.operand(*receiverOf(use.opcode)));
    if (site.rule != TestSiteRuleID::Call)
        return { };
    if (use.opcode == Opcode::NewObject)
        return context.analyses.identityOf(site.use);
    if (use.opcode != Opcode::GetConstructPrototype)
        return { };
    std::optional<Use> allocation = context.analyses.useLists().singleUse(site.use);
    if (!allocation || allocation->kind != Use::Kind::Operand || context.function.at(allocation->user).opcode != Opcode::NewObject)
        return { };
    return context.analyses.identityOf(allocation->user);
}

static bool mayEndMemberConditions(const Effects& effects)
{
    return effects.maySuspend || effects.writes.contains(AbstractHeap::MemberGuards);
}

using MemberConditionList = Vector<MemberConditionRecord, 8>;

static void appendConditionsOf(const Value& test, MemberConditionList& conditions)
{
    for (MemberConditionRecord condition : test.conditions())
        conditions.append(condition);
}

static Value makeTestOfConditions(const MemberConditionList& conditions)
{
    Value test = IRBuilder::make(Opcode::AreMembersUnchanged);
    test.setConditions(conditions.span());
    return test;
}

// Placement gives each set of conditions one bit, so a test costs the same however many conditions it lists.
static void shareConditionsTest(FunctionPassContext& context, TestSites& sites, BlockID block, std::span<const uint32_t> tests)
{
    if (tests.size() < 2)
        return;
    MemberConditionList conditions;
    for (uint32_t test : tests)
        appendConditionsOf(sites.tests[test], conditions);
    AOT::sort(conditions, { }, [](const MemberConditionRecord& condition) { return std::tuple { condition.kind, condition.layout, condition.side, condition.key }; });
    conditions.shrink(conditions.size() - std::ranges::unique(conditions).size());

    Value shared = makeTestOfConditions(conditions);
    if (context.analyses.findEarlierTest(shared, block) == EarlierTest::Failed)
        return;
    for (uint32_t test : tests)
        sites.tests[test] = shared;
}

static void shareConditionsTestsIn(FunctionPassContext& context, TestSites& sites, std::span<const TestSite> sitesOfBlock)
{
    if (std::ranges::count_if(sitesOfBlock, [&](const TestSite& site) { return findConditionsTest(sites, site).has_value(); }) < 2)
        return;

    // The tests of member conditions that sites of one block make for one object, with nothing between the sites that ends what such a test proves.
    Vector<std::pair<ValueID, Vector<uint32_t, 8>>, 2> testsOfObjects;
    BlockID block = sitesOfBlock.front().point.block;
    const BlockValues& values = context.function.at(block).values;
    auto shareTestsOfObjects = [&] {
        for (const auto& [object, tests] : testsOfObjects)
            shareConditionsTest(context, sites, block, tests.span());
        testsOfObjects.shrink(0);
    };

    uint32_t index = sitesOfBlock.front().point.index;
    for (const TestSite& site : sitesOfBlock) {
        for (; index < site.point.index; ++index) {
            if (mayEndMemberConditions(context.analyses.effectsOf(values[index]).normal))
                shareTestsOfObjects();
        }
        std::optional<uint32_t> test = findConditionsTest(sites, site);
        if (ValueID object = test ? objectAskedAbout(context, site) : ValueID()) {
            size_t position = testsOfObjects.findIf([&](const auto& known) { return known.first == object; });
            if (position == notFound) {
                position = testsOfObjects.size();
                testsOfObjects.append({ object, { } });
            }
            testsOfObjects[position].second.append(*test);
        }
        if (mayEndMemberConditions(site.effectsWhenPassed))
            shareTestsOfObjects();
        ++index;
    }
    shareTestsOfObjects();
}

// In the copy behind a failed test each site asks for itself, so one member that has changed costs the other sites of its object one test and not their fast paths.
static void shareConditionsTests(FunctionPassContext& context, TestSites& sites)
{
    for (std::span<const TestSite> remaining = sites.sites.span(); !remaining.empty();) {
        BlockID block = remaining.front().point.block;
        size_t count = std::ranges::find_if_not(remaining, [&](const TestSite& site) { return site.point.block == block; }) - remaining.begin();
        if (context.function.at(block).frequency == BlockFrequency::Normal)
            shareConditionsTestsIn(context, sites, remaining.first(count));
        remaining = remaining.subspan(count);
    }
}

// A test that has passed answers for each condition that it lists, and placement knows a test by all that it lists.
static void askForConditionsTestsInFront(FunctionPassContext& context, TestSites& sites)
{
    if (std::ranges::none_of(sites.sites, [&](const TestSite& site) { return findConditionsTest(sites, site).has_value(); }))
        return;

    // A test of member conditions, and the point that is reached only where it has passed.
    Vector<std::pair<ProgramPoint, Value>, 8> testsInFront;
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        BlockID passed = terminator.taken().block;
        if (test.opcode != Opcode::AreMembersUnchanged || passed == terminator.notTaken().block || !cfg.singleIncomingEdge(passed))
            continue;
        MemberConditionList conditions;
        appendConditionsOf(test, conditions);
        testsInFront.append({ ProgramPoint::atHeadOf(passed), makeTestOfConditions(conditions) });
    }

    auto isInFrontOf = [&](ProgramPoint passedAt, ProgramPoint point) {
        return passedAt.block == point.block ? passedAt.index <= point.index : context.analyses.dominators().dominates(passedAt.block, point.block);
    };
    auto listsConditionsOf = [](const Value& test, const Value& other) {
        return std::ranges::all_of(other.conditions(), [&](const MemberConditionRecord& condition) { return std::ranges::contains(test.conditions(), condition); });
    };
    for (const TestSite& site : sites.sites) {
        std::optional<uint32_t> test = findConditionsTest(sites, site);
        if (!test)
            continue;
        if (function.at(site.point.block).frequency == BlockFrequency::Normal && objectAskedAbout(context, site)) {
            for (const auto& [passedAt, candidate] : testsInFront | std::views::reverse) {
                if (candidate.conditions().size() <= sites.tests[*test].conditions().size() || !listsConditionsOf(candidate, sites.tests[*test]) || !isInFrontOf(passedAt, site.point))
                    continue;
                if (context.analyses.findEarlierTest(candidate, site.point.block) != EarlierTest::Failed)
                    sites.tests[*test] = candidate;
                break;
            }
        }
        testsInFront.append({ site.point, sites.tests[*test] });
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

static TestSites collectTestSites(FunctionPassContext& context)
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
        // What a test buys in the cold code of a cold function, against the copy that it costs. The general form of these is one call.
        bool paysInColdCode = site.rule != TestSiteRuleID::ElementAccess && site.rule != TestSiteRuleID::Operator;
        return heatClass == HeatClass::RunsOnce || (isColdFunction && heatClass == HeatClass::Cold && (isCompactFunction || !paysInColdCode));
    });
    const CFG& cfg = context.analyses.cfg();
    AOT::sort(sites.sites, { }, [&](const TestSite& site) { return std::pair { cfg.reversePostOrderIndex(site.point.block), site.point.index }; });
    shareConditionsTests(context, sites);
    askForConditionsTestsInFront(context, sites);
    return sites;
}

static void dropSitesBeyondConditionLimit(FunctionPassContext& context, TestSites& sites)
{
    uint64_t limit = context.limit(LimitID::MaxTestedTypesPerFunction);
    if (sites.tests.size() <= limit)
        return;
    TestConditions conditions(context.analyses);
    Vector<TestConditionID> conditionOfTest(FillWith { }, sites.tests.size(), TestConditionID());
    for (const TestSite& site : sites.sites) {
        for (uint32_t test = site.firstTest; test < site.firstTest + site.numberOfTests; ++test)
            conditionOfTest[test] = conditions.add(sites.tests[test]);
    }
    if (conditions.size() <= limit)
        return;

    Vector<std::pair<double, uint32_t>> heaviestFirst(conditions.size(), [](size_t index) { return std::pair { 0.0, static_cast<uint32_t>(index) }; });
    for (const TestSite& site : sites.sites) {
        double heat = context.heatOf(site.point.block);
        for (TestConditionID condition : conditionOfTest.subspan(site.firstTest, site.numberOfTests))
            heaviestFirst[condition.index()].first += heat;
    }
    AOT::sort(heaviestFirst, [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    BitVector isChosen;
    for (auto [weight, condition] : heaviestFirst.span().first(static_cast<size_t>(limit)))
        isChosen.set(condition);
    sites.sites.removeAllMatching([&](const TestSite& site) {
        if (std::ranges::all_of(conditionOfTest.subspan(site.firstTest, site.numberOfTests), [&](TestConditionID condition) { return isChosen.get(condition.index()); }))
            return false;
        context.gaveUp(LimitID::MaxTestedTypesPerFunction, site.use);
        return true;
    });
}

static Vector<TestRequirement> chooseRequirements(FunctionPassContext& context, TestSites& sites, TestConditions& conditions)
{
    RELEASE_ASSERT(!conditions.size());
    dropSitesBeyondConditionLimit(context, sites);

    Vector<TestRequirement> requirements;
    for (uint32_t index = 0; index < sites.sites.size(); ++index) {
        const TestSite& site = sites.sites[index];
        for (uint32_t test = site.firstTest; test < site.firstTest + site.numberOfTests; ++test)
            requirements.append(TestRequirement { index, test, conditions.add(sites.tests[test]), site.point, RequirementKind::PlacesTest });
    }
    return requirements;
}

// The proofs that the function holds already.

static void appendTakenEdgeProof(FunctionAnalyses& analyses, const TestConditions& conditions, BlockID block, Vector<ExistingProof>& proofs)
{
    const Function& function = analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    ValueID test = terminator.conditionOperand();
    if (terminator.taken().block == terminator.notTaken().block || provesOf(function.at(test).opcode) == Proof::None)
        return;
    std::optional<TestConditionID> condition = conditions.find(function.at(test));
    if (!condition)
        return;
    ValueLocation location = analyses.useLists().locationOf(test);
    if (location.placement != ValuePlacement::InBlock || location.block != block)
        return;

    const TestCondition& tested = conditions.at(*condition);
    const BlockValues& values = function.at(block).values;
    for (uint32_t index = location.index + 1; !tested.lastsForever() && index < values.size(); ++index) {
        const EffectsOnEdges& effects = analyses.effectsOf(values[index]);
        if (effects.normal.maySuspend || endingBetweenTestAndBranch(analyses, tested, values[index], effects) == ProofEnding::MayEnd)
            return;
    }
    proofs.append(ExistingProof { ProofSource::TakenEdge, *condition, test, Edge { block, 0 } });
}

// What one of several origins proved is weaker than the condition of any of them.
static std::optional<TestConditionID> findConditionOf(const Function& function, const TestConditions& conditions, const GuardFact& fact)
{
    const GuardOrigin* origin = fact.singleOrigin();
    if (!origin || origin->source == GuardSource::Allocation)
        return std::nullopt;
    if (origin->source == GuardSource::Test)
        return conditions.find(function.at(origin->test));
    const Terminator& terminator = function.at(origin->switchBlock).terminator;
    return conditions.find(makeShapeTest(terminator.subjectOperand(), terminator.layoutSets()[origin->caseIndex]));
}

static void appendGuardValueProofs(FunctionAnalyses& analyses, const TestConditions& conditions, Vector<ExistingProof>& proofs)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        // Few guards prove a condition of this plan, and the state of the guards at the head of a block is solved for.
        std::optional<GuardAvailability::Cursor> cursor;
        auto appendProofsOf = [&](ValueID guard, uint32_t indexAfterDefinition) {
            const GuardFacts* facts = isGuardValue(function, guard) ? &analyses.guardFacts(guard) : nullptr;
            if (!facts || !facts->isKnown())
                return;
            size_t firstProofOfGuard = proofs.size();
            for (const GuardFact& fact : facts->facts) {
                std::optional<TestConditionID> condition = findConditionOf(function, conditions, fact);
                if (!condition || std::ranges::contains(proofs.span().subspan(firstProofOfGuard), *condition, &ExistingProof::condition))
                    continue;

                // A parameter or a list may hold a guard that has ended.
                const TestCondition& tested = conditions.at(*condition);
                std::optional<GuardState> state = analyses.guards().stateSeenWithoutSolving(guard, ProofSet { tested.proof }, tested.subject, { block, indexAfterDefinition });
                if (!state) {
                    if (!cursor)
                        cursor.emplace(analyses.guards(), block);
                    cursor->advanceTo(indexAfterDefinition);
                    state = cursor->stateOf(guard, ProofSet { tested.proof }, tested.subject);
                }
                if (*state == GuardState::Available)
                    proofs.append(ExistingProof { ProofSource::GuardValue, *condition, guard, { } });
            }
        };
        const Block& contents = function.at(block);
        for (ValueID parameter : contents.parameters)
            appendProofsOf(parameter, 0);
        for (uint32_t index = 0; index < contents.values.size(); ++index)
            appendProofsOf(contents.values[index], index + 1);
    }
}

Vector<ExistingProof> findExistingProofs(FunctionPassContext& context, const TestConditions& conditions, ProofsHeldByGuards heldByGuards)
{
    const Function& function = context.function;
    Vector<ExistingProof> proofs;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.isConditionalBranch())
            appendTakenEdgeProof(context.analyses, conditions, block, proofs);
        std::span<const LayoutSetID> layoutSets = terminator.kind == TerminatorKind::SwitchOnShape ? terminator.layoutSets() : std::span<const LayoutSetID> { };
        for (uint32_t caseIndex = 0; caseIndex < layoutSets.size(); ++caseIndex) {
            if (std::optional<TestConditionID> condition = conditions.find(makeShapeTest(terminator.subjectOperand(), layoutSets[caseIndex])))
                proofs.append(ExistingProof { ProofSource::SwitchCase, *condition, { }, Edge { block, caseIndex } });
        }
    }
    if (conditions.stretchConditions().isEmpty())
        return proofs;

    if (heldByGuards == ProofsHeldByGuards::AreListed)
        appendGuardValueProofs(context.analyses, conditions, proofs);
    for (size_t index : conditions.stretchConditions()) {
        const TestCondition& condition = conditions.at(TestConditionID(index));
        if (condition.subject && allocationProofOf(context.program, function, function.at(condition.subject), condition) == AllocationProof::Proves)
            proofs.append(ExistingProof { ProofSource::Allocation, TestConditionID(index), condition.subject, { } });
    }
    return proofs;
}

ValueID ensureEdgeResult(IRBuilder& builder, FunctionAnalyses& analyses, Edge edge, uint32_t proof)
{
    Function& function = builder.function();
    // A parameter that other edges fill too is a merge of their arguments, not the proof of this edge.
    if (!analyses.hasSingleIncomingEdge(targetOf(function, edge).block))
        builder.splitEdge(edge);

    const Target& target = targetOf(function, edge);
    size_t existing = target.proofs.find(proof);
    if (existing != notFound)
        return function.at(target.block).parameters[firstEdgeResultParameter(function, edge.from, edge.targetIndex) + existing];

    builder.setOrigin(function.at(edge.from).terminator.origin());
    return builder.addEdgeResult(edge, proof);
}

// Tests in front of loops.

// Whether a test may look at another definition of what its operand holds.
static bool canReplaceOperand(FunctionPassContext& context, const Value& test, uint32_t operandIndex, ValueID replacement, bool isNarrowedByEarlierTest)
{
    auto typeOf = [&](ValueID value) -> const Type& { return context.types.at(context.function.at(value).type); };
    ValueID operand = test.operand(operandIndex);
    return replacement == operand || isNarrowedByEarlierTest || test.operandKind(operandIndex) == OperandKind::Any || typeOf(replacement).isSubsetOf(typeOf(operand), context.program);
}

static Vector<HoistedTest> findHoistedTests(FunctionPassContext& context, const TestSites& sites, const TestConditions& conditions, std::span<const TestRequirement> requirements)
{
    using Operands = Vector<ValueID, 2>;
    using SiteOperands = Vector<std::optional<Operands>, 4>;
    struct LoopEntry {
        Vector<Edge, 2> edges;
        uint32_t firstEdgeNumber { 0 };
        double highestSourceFrequency { 0 };
    };

    FunctionAnalyses& analyses = context.analyses;
    const CFG& cfg = analyses.cfg();
    if (!cfg.hasCycle() || !analyses.naturalLoops().numLoops())
        return { };
    const NaturalLoops& loops = analyses.naturalLoops();
    const UseLists& useLists = analyses.useLists();
    const BlockFrequencies& frequencies = analyses.blockFrequencies();

    Vector<std::optional<LoopEntry>> entries(loops.numLoops());
    uint32_t numberOfEnteringEdges = 0;
    auto entryOf = [&](const NaturalLoop& loop) -> const LoopEntry& {
        std::optional<LoopEntry>& entry = entries[loop.index()];
        if (entry)
            return *entry;
        entry.emplace();
        entry->firstEdgeNumber = numberOfEnteringEdges;
        for (Edge edge : cfg.incomingEdges(loop.header())) {
            if (edge.isHandlerEdge() || loops.belongsTo(edge.from, loop))
                continue;
            entry->edges.append(edge);
            entry->highestSourceFrequency = std::max(entry->highestSourceFrequency, frequencies.frequencyOf(edge.from));
        }
        numberOfEnteringEdges += entry->edges.size();
        return *entry;
    };
    auto findDefinitionOutside = [&](ValueID value, const NaturalLoop& loop) {
        for (ValueID candidate : CastChain(context.function, value)) {
            ValueLocation location = useLists.locationOf(candidate);
            if (location.placement != ValuePlacement::Detached && !loops.belongsTo(location.block, loop))
                return candidate;
        }
        return ValueID();
    };
    auto findOperandsOutside = [&](std::span<const TestRequirement> site, uint32_t index, const SiteOperands& earlierOperands, const NaturalLoop& loop) -> std::optional<Operands> {
        const Value& test = sites.tests[site[index].test];
        const Value& canonicalTest = conditions.at(site[index].condition).test;
        std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);
        Operands operands;
        for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
            if (guardIndex == operandIndex)
                continue;
            bool isNarrowedByEarlierTest = false;
            for (uint32_t earlier = 0; earlier < index; ++earlier) {
                const TestCondition& condition = conditions.at(site[earlier].condition);
                if (condition.subject != canonicalTest.operand(operandIndex))
                    continue;
                if (!earlierOperands[earlier])
                    return std::nullopt;
                isNarrowedByEarlierTest |= condition.lastsForever();
            }
            // Inside the loop the test stands behind whatever decides that the value is there, the switch on the kind of an iteration for one. In front of the loop it would read through whatever the register holds.
            ValueID outside = findDefinitionOutside(test.operand(operandIndex), loop);
            if (!outside || mayBeUnspecified(analyses, outside) || !canReplaceOperand(context, test, operandIndex, outside, isNarrowedByEarlierTest))
                return std::nullopt;
            operands.append(outside);
        }
        return operands;
    };

    BitVector isOffered;
    Vector<HoistedTest> hoistedTests;
    for (uint32_t first = 0, end = 0; first < requirements.size(); first = end) {
        for (end = first + 1; end < requirements.size() && requirements[end].site == requirements[first].site;)
            ++end;
        std::span<const TestRequirement> site = requirements.subspan(first, end - first);
        BlockID useBlock = site.front().point.block;
        for (const NaturalLoop* loop = loops.innerMostLoopOf(useBlock); loop; loop = loops.innerMostOuterLoop(*loop)) {
            if (isClosedOnlyByWaysBack(analyses, *loop))
                continue;
            SiteOperands operands;
            bool canLeaveLoop = false;
            for (uint32_t index = 0; index < site.size(); ++index) {
                operands.append(findOperandsOutside(site, index, operands, *loop));
                canLeaveLoop |= operands.last().has_value();
            }
            if (!canLeaveLoop)
                break;
            const LoopEntry& entry = entryOf(*loop);
            if (entry.highestSourceFrequency > frequencies.frequencyOf(useBlock))
                continue;
            for (uint32_t edgeIndex = 0; edgeIndex < entry.edges.size(); ++edgeIndex) {
                size_t firstBit = static_cast<size_t>(entry.firstEdgeNumber + edgeIndex) * conditions.size();
                for (uint32_t index = 0; index < site.size(); ++index) {
                    if (operands[index] && isOffered.add(firstBit + site[index].condition.index()))
                        hoistedTests.append(HoistedTest { entry.edges[edgeIndex], first + index, site[index].condition, *operands[index] });
                }
            }
        }
    }
    return hoistedTests;
}

// The plan.

namespace {

// Plans tests that establish again what the tested code wants where a loop is entered, about values that are defined in front of the loop.
struct WayBackPlanner {
    enum class State : uint8_t { NotPlanned, BeingPlanned, Planned };
    using Operands = Vector<ValueID, 2>;

    FunctionPassContext& context;
    const TestPlacementPlan& plan;
    Vector<Vector<uint32_t, 2>> requirementsOfCondition;
    Vector<State> states { };
    const PlannedWayBackIntoLoop* wayBack { nullptr };
    Vector<PlannedTest, 4> tests { };

    WayBackPlanner(FunctionPassContext& context, const TestPlacementPlan& plan, const TestCoverage& coverage)
        : context(context)
        , plan(plan)
        , requirementsOfCondition(plan.conditions->size())
    {
        for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
            if (coverage.of(index) != RequirementCoverage::Unknown)
                requirementsOfCondition[plan.requirements[index].condition.index()].append(index);
        }
    }

    std::optional<Vector<PlannedTest, 4>> planTests(const PlannedWayBackIntoLoop& newWayBack, const BitVector& wanted)
    {
        tests.shrink(0);
        if (wanted.isEmpty())
            return std::exchange(tests, { });

        wayBack = &newWayBack;
        states.fill(State::NotPlanned, plan.conditions->size());
        for (const TestRequirement& requirement : plan.requirements) {
            if (wanted.get(requirement.condition.index()) && !planTest(requirement.condition))
                return std::nullopt;
        }
        return std::exchange(tests, { });
    }

    bool planTest(TestConditionID condition)
    {
        if (states[condition.index()] != State::NotPlanned)
            return states[condition.index()] == State::Planned;
        states[condition.index()] = State::BeingPlanned;

        for (uint32_t requirement : requirementsOfCondition[condition.index()]) {
            std::optional<Operands> operands = findOperandsInFrontOfLoop(requirement);
            if (!operands)
                continue;
            bool hasPrerequisites = true;
            forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
                hasPrerequisites = hasPrerequisites && planTest(prerequisite.condition);
            });
            if (!hasPrerequisites)
                return false;
            tests.append(PlannedTest { PlannedTestPosition::OnEdge, wayBack->stretch, requirement, condition, { }, wayBack->backEdges.first(), WTF::move(*operands) });
            states[condition.index()] = State::Planned;
            return true;
        }
        return false;
    }

    std::optional<Operands> findOperandsInFrontOfLoop(uint32_t requirement)
    {
        const Value& test = plan.sites.tests[plan.requirements[requirement].test];
        std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);
        BitVector isNarrowedByEarlierTest;
        forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
            if (prerequisite.narrowedOperand)
                isNarrowedByEarlierTest.set(*prerequisite.narrowedOperand);
        });

        Operands operands;
        for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
            if (guardIndex == operandIndex)
                continue;
            ValueID inFront = findDefinitionInFrontOfLoop(test.operand(operandIndex));
            if (!inFront || !canReplaceOperand(context, test, operandIndex, inFront, isNarrowedByEarlierTest.get(operandIndex)))
                return std::nullopt;
            operands.append(inFront);
        }
        return operands;
    }

    ValueID findDefinitionInFrontOfLoop(ValueID value)
    {
        for (ValueID candidate : CastChain(context.function, value)) {
            if (context.analyses.useLists().blockOf(candidate) != wayBack->head && context.analyses.dominates(candidate, ProgramPoint::atHeadOf(wayBack->head)))
                return candidate;
        }
        return { };
    }
};

} // anonymous namespace

// A test in front of an equal test gains nothing. It is worth its copy where its proof reaches another use.
static BitVector findTestsRepeatedInVain(const TestPlacementPlan& plan, const TestCoverage& coverage, const TestStretches& stretches)
{
    HashSet<std::pair<unsigned, TestConditionID>> coveredConditions;
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        if (coverage.of(index) == RequirementCoverage::Covered)
            coveredConditions.add({ stretches.stretchAt(requirement.point), requirement.condition });
    }

    BitVector sites;
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        if (plan.sites.sites[requirement.site].rule != TestSiteRuleID::RepeatedTest || coverage.of(index) != RequirementCoverage::NeedsTest)
            continue;
        if (!coveredConditions.contains({ stretches.stretchAt(requirement.point), requirement.condition }))
            sites.set(requirement.site);
    }
    return sites;
}

bool expectedTypeTellsResultOf(FunctionPassContext& context, const TestSite& site)
{
    if (site.rule == TestSiteRuleID::BuiltinUse || site.rule == TestSiteRuleID::Call)
        return false;
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

static BitVector findPointsWhereNothingIsKnownBetter(const Function& function, BlockID block, uint32_t firstIndex, const BitVector& isKnownBetter, NOESCAPE const Invocable<const Liveness&()> auto& livenessOfValuesKnownBetter)
{
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
        function.at(values[index]).forEachOperand([&](ValueID operand, uint32_t) { use(operand); });
        if (live.isEmpty())
            points.quickSet(index);
    }
    return points;
}

// Where the tested code wants none of the proofs of this plan, the generic copy may come back.
static void appendRejoinPoints(TestPlacementPlan& plan, FunctionPassContext& context, const TestStretches& stretches, WantedProofs& wanted)
{
    FunctionAnalyses& analyses = context.analyses;
    const Function& function = context.function;
    Vector<const TestSite*> siteOfValue(FillWith { }, function.values.size(), nullptr);
    for (const PlannedUse& use : plan.uses)
        siteOfValue[plan.sites.sites[use.site].use.index()] = &plan.sites.sites[use.site];

    auto mayCallUnknownCode = [&](ValueID value) {
        const TestSite* site = siteOfValue[value.index()];
        return site ? site->effectsWhenPassed.mayCallUnknownCode : analyses.mayCallUnknownCode(value);
    };
    // A range is copied as one block.
    auto mayRejoinAt = [&](ProgramPoint point) { return !wanted.isAnyProofWantedAt(point) && !stretches.isInsideRange(point); };
    plan.rejoinPoints = findRejoinPoints(analyses, mayCallUnknownCode, mayRejoinAt);

    // Code that is rarer than what leads to it is a slow path already: the generic version of the function, or the copy of an earlier run. The copy of this run goes on there and copies none of it.
    const CFG& cfg = analyses.cfg();
    for (BlockID block : cfg.reversePostOrder()) {
        if (function.at(block).isHandlerBlock())
            continue;
        BlockFrequency frequency = function.at(block).frequency;
        bool isRarerThanPredecessor = std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) { return function.at(edge.from).frequency < frequency; });
        // An arm that goes straight back is part of the code it left: what the copy brought in there would meet the tested values at the next join.
        bool goesStraightBack = std::ranges::any_of(function.at(block).terminator.targets, [&](const Target& target) { return function.at(target.block).frequency < frequency; });
        if (isRarerThanPredecessor && !goesStraightBack && mayRejoinAt(ProgramPoint::atHeadOf(block)))
            plan.rejoinPoints.append(ProgramPoint::atHeadOf(block));
    }
    auto orderOf = [&](ProgramPoint point) { return std::pair { cfg.reversePostOrderIndex(point.block), point.index }; };
    AOT::sort(plan.rejoinPoints, { }, orderOf);
    removeRepeatedElements(plan.rejoinPoints);

    // Without tests.
    std::span<const StretchRange> ranges = stretches.ranges();
    BitVector isBehindCopiedRange = findBlocksBehindRanges(function, ranges, [](const StretchRange& range) { return range.isCloned; });
    if (isBehindCopiedRange.isEmpty())
        return;
    for (BlockID block : cfg.reversePostOrder()) {
        if (isBehindCopiedRange.get(block.index()) && block != cfg.root() && !function.at(block).isHandlerBlock() && mayRejoinAt(ProgramPoint::atHeadOf(block)))
            plan.rejoinPointsWithoutTests.append(ProgramPoint::atHeadOf(block));
    }

    Vector<ValueID> narrowedAsExpected;
    Vector<ValueID> narrowedInUnknownWay;
    for (const PlannedUse& use : plan.uses) {
        const TestSite& site = plan.sites.sites[use.site];
        (expectedTypeTellsResultOf(context, site) ? narrowedAsExpected : narrowedInUnknownWay).append(site.use);
    }
    Vector<ValueID> subjects = collectSubjectsKnownBeyondTheirTypes(context, plan);
    BitVector isKnownBetter = findValuesTypedCodeKnowsMoreAbout(analyses, context.types, { narrowedAsExpected.span(), narrowedInUnknownWay.span(), subjects.span() });

    // Nothing is asked about the other values, and they are most of the function.
    std::optional<Liveness> ownLiveness;
    auto livenessOfValuesKnownBetter = [&]() -> const Liveness& {
        if (analyses.computed().contains(FunctionAnalysis::Liveness))
            return analyses.liveness();
        if (!ownLiveness) {
            LivenessOptions options;
            options.trackedValues = &isKnownBetter;
            ownLiveness.emplace(function, cfg, analyses.useLists(), options);
        }
        return *ownLiveness;
    };

    for (size_t first = 0, next = 0; first < ranges.size(); first = next) {
        BlockID block = ranges[first].block;
        uint32_t numberOfValues = function.at(block).values.size();
        uint32_t firstEnd = numberOfValues;
        for (; next < ranges.size() && ranges[next].block == block; ++next) {
            if (ranges[next].isCloned)
                firstEnd = std::min(firstEnd, ranges[next].end);
        }
        if (firstEnd == numberOfValues)
            continue;
        BitVector points = findPointsWhereNothingIsKnownBetter(function, block, firstEnd, isKnownBetter, livenessOfValuesKnownBetter);
        for (const StretchRange& range : ranges.subspan(first, next - first)) {
            for (uint32_t index = range.end; range.isCloned && index < numberOfValues; ++index) {
                if (!points.quickGet(index) || !mayRejoinAt({ block, index }))
                    continue;
                plan.rejoinPointsWithoutTests.append({ block, index });
                break;
            }
        }
    }

    AOT::sort(plan.rejoinPointsWithoutTests, { }, orderOf);
    removeRepeatedElements(plan.rejoinPointsWithoutTests);
    plan.rejoinPointsWithoutTests.removeAllMatching([&](ProgramPoint point) { return std::ranges::binary_search(plan.rejoinPoints, orderOf(point), { }, orderOf); });
}

// A test whose proof lasts forever, about values made in front of the loop, fails at every turn once it has failed. Where only such tests lead into the copy, it would come back in vain.
static bool mayPassAtLaterTurn(const TestPlacementPlan& plan, FunctionPassContext& context, unsigned stretch, const NaturalLoop& loop)
{
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const UseLists& useLists = context.analyses.useLists();
    auto isMadeInLoop = [&](ValueID operand) {
        BlockID block = useLists.blockOf(operand);
        return block && loops.belongsTo(block, loop);
    };
    return std::ranges::contains(plan.slowPaths, stretch, &PlannedSlowPath::stretch) || plan.tests.containsIf([&](const PlannedTest& test) {
        if (test.stretch != stretch)
            return false;
        const TestCondition& condition = plan.conditions->at(test.condition);
        if (!condition.lastsForever())
            return true;
        if (test.position == PlannedTestPosition::OnEdge)
            return std::ranges::any_of(test.operands, isMadeInLoop);
        bool namesValueMadeInLoop = false;
        condition.test.forEachOperand([&](ValueID operand, uint32_t) { namesValueMadeInLoop |= isMadeInLoop(operand); });
        return namesValueMadeInLoop;
    });
}

// A failed test costs one turn of a loop: at the end of the turn the generic copy makes the tests again that the head of the loop relies on.
static void appendWaysBackIntoLoops(TestPlacementPlan& plan, FunctionPassContext& context, const TestCoverage& coverage, std::span<const StretchRange> ranges, WantedProofs& wanted)
{
    const CFG& cfg = context.analyses.cfg();
    if (!cfg.hasCycle() || !context.analyses.naturalLoops().numLoops())
        return;
    const NaturalLoops& loops = context.analyses.naturalLoops();

    std::optional<WayBackPlanner> planner;
    for (BlockID head : cfg.reversePostOrder()) {
        const NaturalLoop* loop = loops.headerOf(head);
        if (!loop || !wanted.isInStretchAtHead(head))
            continue;
        auto firstRange = std::ranges::find(ranges, head, &StretchRange::block);
        RELEASE_ASSERT(firstRange != ranges.end());
        if (!firstRange->isCloned)
            continue;

        PlannedWayBackIntoLoop wayBack { firstRange->stretch, head, { }, { }, { }, { } };
        std::optional<Edge> enteringEdge;
        bool hasTestsOnEnteringEdge = false;
        bool isEnteredWithArgumentsOnly = true;
        for (Edge edge : cfg.incomingEdges(head)) {
            if (edge.isHandlerEdge() || numberOfEdgeResults(context.function, edge.from, edge.targetIndex)) {
                isEnteredWithArgumentsOnly = false;
                break;
            }
            if (std::ranges::contains(wanted.edgesIntoGenericCopy(), edge))
                continue;
            if (loops.belongsTo(edge.from, *loop)) {
                wayBack.backEdges.append(edge);
                continue;
            }
            bool hasTests = plan.tests.containsIf([&](const PlannedTest& test) { return test.position == PlannedTestPosition::OnEdge && test.edge == edge; });
            if (!enteringEdge || (hasTests && !hasTestsOnEnteringEdge)) {
                enteringEdge = edge;
                hasTestsOnEnteringEdge = hasTests;
            }
        }
        if (!isEnteredWithArgumentsOnly || !enteringEdge || wayBack.backEdges.isEmpty() || !mayPassAtLaterTurn(plan, context, wayBack.stretch, *loop))
            continue;
        wayBack.enteringEdge = *enteringEdge;

        if (!planner)
            planner.emplace(context, plan, coverage);
        std::optional<Vector<PlannedTest, 4>> tests = planner->planTests(wayBack, wanted.wantedInFrontOfTestsOn(*enteringEdge));
        if (!tests)
            continue;
        wayBack.tests = WTF::move(*tests);
        wayBack.typesOfCounters = typesOfCountersOnWayBack(context.analyses, context.types, head);
        plan.waysBackIntoLoops.append(WTF::move(wayBack));
    }
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
    TestEvents events(TestEventInputs { context.analyses, *plan.conditions, plan.sites.sites.span(), plan.requirements.span(), proofs.span(), hoistedTests.span() });
    // The generic copy goes on behind a stretch, so it holds the code of a gap whether a proof bridges the gap or not.
    uint32_t widestGap = static_cast<uint32_t>(std::min<uint64_t>(context.limit(LimitID::MaxClonedBytesPerStretch), UINT32_MAX));
    TestCoverage coverage(TestCoverageInputs { events, GapBudget { widestGap, widestGap }, nullptr, SlowPaths::MayLeave });
    WantedProofs wanted(events, coverage);
    TestStretches stretches(events, coverage, wanted);

    BitVector repeatedInVain = findTestsRepeatedInVain(plan, coverage, stretches);
    if (!repeatedInVain.isEmpty()) {
        uint32_t index = 0;
        plan.sites.sites.removeAllMatching([&](const TestSite&) { return repeatedInVain.get(index++); });
        return planTestPlacement(context, WTF::move(plan.sites));
    }

    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    for (uint32_t index = 0; index < hoistedTests.size(); ++index) {
        const HoistedTest& hoisted = hoistedTests[index];
        if (wanted.isHoistedTestWanted(index))
            plan.tests.append(PlannedTest { PlannedTestPosition::OnEdge, stretches.stretchAtHeadOf(targetOf(function, hoisted.edge).block), hoisted.requirement, hoisted.condition, { }, hoisted.edge, hoisted.operands });
    }
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        if (coverage.of(index) == RequirementCoverage::NeedsTest)
            plan.tests.append(PlannedTest { PlannedTestPosition::BeforeUse, stretches.stretchAt(requirement.point), index, requirement.condition, requirement.point, { }, { } });
    }
    for (uint32_t first = 0; first < plan.requirements.size();) {
        uint32_t siteIndex = plan.requirements[first].site;
        const TestSite& site = plan.sites.sites[siteIndex];
        uint32_t end = first + site.numberOfTests;
        RELEASE_ASSERT(site.numberOfTests && end <= plan.requirements.size());

        bool isKnown = true;
        for (uint32_t index = first; index < end; ++index) {
            RELEASE_ASSERT(plan.requirements[index].site == siteIndex);
            isKnown &= coverage.of(index) != RequirementCoverage::Unknown;
        }
        if (isKnown)
            plan.uses.append(PlannedUse { stretches.stretchAt(site.point), siteIndex, first });
        first = end;
    }
    for (Edge edge : wanted.edgesIntoGenericCopy()) {
        BlockID target = targetOf(function, edge).block;
        if (wanted.isInStretchAtHead(target))
            plan.slowPaths.append(PlannedSlowPath { stretches.stretchAtHeadOf(target), edge });
    }
    AOT::sort(plan.slowPaths, { }, [&](const PlannedSlowPath& slowPath) { return std::pair { cfg.reversePostOrderIndex(slowPath.edge.from), slowPath.edge.targetIndex }; });

    appendRejoinPoints(plan, context, stretches, wanted);
    appendWaysBackIntoLoops(plan, context, coverage, stretches.ranges(), wanted);
    for (uint32_t index = 0; index < proofs.size(); ++index) {
        if (wanted.isProofWanted(index))
            plan.proofs.append(proofs[index]);
    }
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        if (coverage.exceededBudget(index))
            context.gaveUp(LimitID::MaxClonedBytesPerStretch, plan.sites.sites[plan.requirements[index].site].use);
    }

    AOT::stableSort(plan.tests, { }, [&](const PlannedTest& test) {
        if (test.position == PlannedTestPosition::BeforeUse)
            return std::tuple { cfg.reversePostOrderIndex(test.point.block), test.point.index, 0u };
        return std::tuple { cfg.reversePostOrderIndex(test.edge.from), UINT32_MAX, test.edge.targetIndex };
    });
    plan.stretches = stretches.takeStretches();
    plan.ranges = stretches.takeRanges();
    return plan;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
