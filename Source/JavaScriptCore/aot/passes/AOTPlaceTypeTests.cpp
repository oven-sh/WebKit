/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinMembers.h"
#include "AOTCodeShapes.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTExpectedTypesByUse.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccess.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRepresentation.h"
#include "AOTResolveAccesses.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTSplitUnions.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

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
    auto leavesNormalCode = [&](const Target& failing) { return function.at(failing.block).frequency.predicted() != BlockFrequency::Normal; };
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
                if (target.block == block || function.at(target.block).frequency.predicted() == BlockFrequency::Normal)
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
    for (std::span<const TestSite> sitesOfBlock : sites.sites.span() | std::views::chunk_by([](const TestSite& a, const TestSite& b) { return a.point.block == b.point.block; })) {
        if (context.function.at(sitesOfBlock[0].point.block).frequency.predicted() == BlockFrequency::Normal)
            shareConditionsTestsIn(context, sites, sitesOfBlock);
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
        if (function.at(site.point.block).frequency.predicted() == BlockFrequency::Normal && objectAskedAbout(context, site)) {
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

// A test for a string, a number or an object of a built-in class says nothing of layouts.
static bool asksOnlyForLayouts(FunctionPassContext& context, const TestSites& sites, const TestSite& site)
{
    return std::ranges::none_of(sites.tests.span().subspan(site.firstTest, site.numberOfTests), [&](const Value& test) {
        return test.opcode == Opcode::TypeTest && !context.types.at(test.testedType()).layouts;
    });
}

// In a checked function of the program the type of a variable, a parameter or a field is the checker's, and so is that of an element read out of it. The library is handed what its callers have.
static Expected<bool> isDescribedByChecker(FunctionPassContext& context, ValueID value)
{
    if (!context.function.has(FunctionFlags::IsChecked) || isLibraryCode(context.program, context.function))
        return false;
    // It ends: a list is defined in front of the read of its element.
    for (;;) {
        ValueID identity = context.analyses.identityOf(value);
        if (context.program.expectedOrTypeOf(context.function, identity).predicted().anyObject)
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
    if (context.program.typeOf(context.function, receiver).layouts || isDescribedByChecker(context, receiver).predicted())
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

bool paysToTestMembersUnderSymbolsOfProgram(FunctionPassContext& context, BlockID block)
{
    Expected<HeatClass> heatClass = heatClassOf(context, block);
    return isHot(heatClass).predicted() || heatClass.predicted() == HeatClass::Unknown || context.analyses.blockFrequencies().frequencyOf(block).predicted() >= HeatModel::minWeightOfCallInLoop;
}

static bool holdsMemberUnderSymbolOfProgram(const Program& program, std::span<const Value> tests)
{
    return std::ranges::any_of(tests, [&](const Value& test) {
        return test.opcode == Opcode::AreMembersUnchanged && holdMemberUnderSymbolOfProgram(program, test.conditions());
    });
}

// One of the objects that may arrive finds such a member under the name that is read.
static Expected<bool> readsMemberUnderSymbolOfProgram(FunctionPassContext& context, const Value& use)
{
    if (use.opcode != Opcode::PropGet || !isSymbolOfProgram(context.program.at(use.key()).kind))
        return false;
    const Program& program = context.program;
    const ReceiverCandidates candidates = receiverCandidates(program, context.function, use.objectOperand(), use.receivers());
    for (const LayoutList* layouts : { &candidates.proven, &candidates.expected.predicted() }) {
        for (LayoutID layout : *layouts) {
            if (findMemberOfClass(program, layout, ClassSide::Prototype, use.key()))
                return true;
        }
    }
    return false;
}

// A test buys a fast path and costs a copy of what stands behind it.
static bool paysToTestIn(HeatClass heatClass, bool isCompactFunction, bool paysInColdCode)
{
    return heatClass != HeatClass::RunsOnce && (heatClass != HeatClass::Cold || (!isCompactFunction && paysInColdCode));
}

static bool getsNoTests(const Heat& heat, FunctionAnalyses& analyses)
{
    const AnalysisContext& context = analyses.context();
    const Function& function = analyses.function();
    // No block runs more often than the function is entered unless it is in a cycle.
    if (heat.of(context.program, context.id).predicted() == HeatClass::RunsOnce && !analyses.cfg().hasCycle())
        return true;
    // Where a test at the entrance has failed. A test in here would guess again what has just proved wrong, and cost a copy.
    if (function.runsOnlyBehindFailedTest())
        return true;
    // The method table calls a selector's lookup only for a receiver that does not match the class definitions, so a guess here would fail.
    // A guessed direct call would also be one more caller of the method, which keeps the table from naming the method itself.
    return function.role == FunctionRole::TableLookup;
}

bool mayPlaceTestsOfAccessesByName(const Heat& heat, FunctionAnalyses& analyses, BlockID block)
{
    const AnalysisContext& context = analyses.context();
    if (!context.build.recordedOptions().usesExpectedTypes() || getsNoTests(heat, analyses))
        return false;
    return paysToTestIn(heatClassOf(heat, analyses, block).predicted(), heat.isCompact(context.program, context.id).predicted(), true);
}

static TestSites collectTestSites(FunctionPassContext& context)
{
    if (!context.build.recordedOptions().usesExpectedTypes())
        return { };
    // Most compact functions have no loop at all, and they are nearly half of a very large program: no rule looks at them.
    const Heat& heat = context.inputs.heat;
    bool isCompactFunction = heat.isCompact(context.program, context.id).predicted();
    // Without looking at the blocks: none of them runs more often than the function is entered.
    if (isCompactFunction && isCold(heat.classOf(heat.activationsOf(context.program, context.id) * roundingOfFrequencySums)).predicted() && runsNoBlockMoreOftenThanEntry(context.analyses.cfg()))
        return { };

    TestSiteReporter reporter(context);
    for (const TestSiteRule& rule : testSiteRules()) {
        reporter.beginRule(rule.id);
        rule.report(reporter);
    }

    TestSites sites = reporter.takeSites();
    for (const TestSite& site : sites.sites)
        RELEASE_ASSERT(testSiteOpcodes.contains(context.function.at(site.use).opcode));
    BitVector receiversThatMayHaveNoLayout = findReceiversAskedOnlyForNamesDefinedWithoutLayout(context, sites);
    sites.sites.removeAllMatching([&](const TestSite& site) {
        if (ValueID receiver = receiversThatMayHaveNoLayout.isEmpty() ? ValueID() : findReceiverWithGuessedLayout(context, sites, site); receiver && receiversThatMayHaveNoLayout.quickGet(receiver.index()))
            return true;
        // What a plain function without a layout constructs arrives as the receiver. A test for a layout would turn it away, and what stands behind the test would run in the copy.
        const Value& use = context.function.at(site.use);
        if (PayloadWord::accessSite(use.opcode) && use.accessSite() && context.inputs.flow.lossOf(use.accessSite()).seesConstructedObject && asksOnlyForLayouts(context, sites, site))
            return true;
        // Tests that say nothing of the member pass for the objects that lack it alone, and whoever reads a name expects an object that has it.
        if (holdsMemberUnderSymbolOfProgram(context.program, sites.testsOf(site)) ? !paysToTestMembersUnderSymbolsOfProgram(context, site.point.block) : readsMemberUnderSymbolOfProgram(context, use).predicted())
            return true;
        // What a test buys in cold code, against the copy that it costs. The general form of these is one call.
        bool paysInColdCode = site.rule != TestSiteRuleID::ElementAccess && site.rule != TestSiteRuleID::Operator;
        return !paysToTestIn(heatClassOf(context, site.point.block).predicted(), isCompactFunction, paysInColdCode);
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
        double heat = context.heatOf(site.point.block).predicted();
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
    if (!origin)
        return std::nullopt;
    switch (origin->source) {
    case GuardSource::Test:
        return conditions.find(function.at(origin->test));
    case GuardSource::SwitchCase: {
        const Terminator& terminator = function.at(origin->switchBlock).terminator;
        return conditions.find(makeShapeTest(terminator.subjectOperand(), terminator.layoutSets()[origin->caseIndex]));
    }
    case GuardSource::Allocation:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
            entry->highestSourceFrequency = std::max(entry->highestSourceFrequency, frequencies.frequencyOf(edge.from).predicted());
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
            if (!outside || context.program.typeOf(context.function, outside).mayBeUnspecified() || !canReplaceOperand(context, test, operandIndex, outside, isNarrowedByEarlierTest))
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
            if (isClosedOnlyByWaysBack(analyses, *loop).predicted())
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
            if (entry.highestSourceFrequency > frequencies.frequencyOf(useBlock).predicted())
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
            ValueID inFront;
            for (ValueID candidate : CastChain(context.function, test.operand(operandIndex))) {
                if (context.analyses.useLists().blockOf(candidate) != wayBack->head && context.analyses.dominates(candidate, ProgramPoint::atHeadOf(wayBack->head))) {
                    inFront = candidate;
                    break;
                }
            }
            if (!inFront || context.program.typeOf(context.function, inFront).mayBeUnspecified() || !canReplaceOperand(context, test, operandIndex, inFront, isNarrowedByEarlierTest.get(operandIndex)))
                return std::nullopt;
            operands.append(inFront);
        }
        return operands;
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

// The copy runs again what stands between its entry and the test, so the first tests in code that can run twice keep their own entries. The entry of the last of them serves every later test.
AOT_THRESHOLD(uint32_t, maxEntriesInRepeatableCode, 8);

// A failing test hands over to the generic copy what its stretch has made so far and the copy still reads. The tested code holds a number in a register of its own kind and the copy holds it as any value, so each is converted on the way.
// Tests that follow each other while the numbers made between them stay alive convert values by the square of their number. In a function where that is much code, a stretch ends in front of a test once its conversions outweigh its own code and what it costs to let the copy come back. The tests behind hand over only what is made from there on.
static Vector<ProgramPoint> findPointsToSplitStretchesAt(FunctionPassContext& context, const TestPlacementPlan& plan, const TestCoverage& coverage, const TestStretches& stretches)
{
    AOT_THRESHOLD(uint64_t, minBytesOfConversionsInFunction, 16 * KB);
    AOT_THRESHOLD(uint64_t, bytesToConvertValue, 16);
    // Behind the join the value is tested again where it is used.
    AOT_THRESHOLD(uint64_t, bytesToJoinValue, 128);

    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    auto isConverted = [&](ValueID id) {
        const Value& value = function.at(id);
        return value.type && value.expected().predicted() && defaultRepresentationOf(context.types.at(value.type)) != defaultRepresentationOf(context.types.at(value.expected().predicted()));
    };
    uint64_t numberOfTests = 0;
    for (uint32_t index = 0; index < plan.requirements.size(); ++index)
        numberOfTests += coverage.of(index) == RequirementCoverage::NeedsTest;
    uint64_t numberOfConvertedValues = 0;
    for (const StretchRange& range : stretches.ranges()) {
        for (uint32_t index = range.begin; range.isCloned && index < range.end; ++index)
            numberOfConvertedValues += isConverted(function.at(range.block).values[index]);
    }
    if (numberOfTests * numberOfConvertedValues * bytesToConvertValue <= minBytesOfConversionsInFunction)
        return { };

    struct Piece {
        uint64_t bytesOfStretch { 0 };
        uint64_t bytesOfConversions { 0 };
        uint32_t alive { 0 };
        uint32_t entriesInRepeatableCode { 0 };
    };
    constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    size_t numberOfValues = function.values.size();
    Vector<uint32_t> stretchOf(FillWith { }, numberOfValues, none);
    unsigned numberOfStretches = 0;
    for (const StretchRange& range : stretches.ranges()) {
        numberOfStretches = std::max(numberOfStretches, range.stretch + 1);
        for (uint32_t index = range.begin; range.isCloned && index < range.end; ++index)
            stretchOf[function.at(range.block).values[index].index()] = range.stretch;
    }
    Vector<uint32_t> testsInFront(FillWith { }, numberOfValues, 0u);
    Vector<uint64_t> testsLeftIn(FillWith { }, numberOfStretches, 0u);
    BitVector hasOwnEntry(numberOfValues);
    Vector<const TestSite*> siteOf(FillWith { }, numberOfValues, nullptr);
    for (uint32_t index = 0; index < plan.requirements.size(); ++index) {
        const TestRequirement& requirement = plan.requirements[index];
        const TestSite& site = plan.sites.sites[requirement.site];
        if (coverage.of(index) != RequirementCoverage::Unknown)
            siteOf[site.use.index()] = &site;
        if (coverage.of(index) != RequirementCoverage::NeedsTest)
            continue;
        ++testsInFront[site.use.index()];
        if (uint32_t stretch = stretchOf[site.use.index()]; stretch != none)
            ++testsLeftIn[stretch];
        if (!plan.conditions->at(requirement.condition).lastsForStretch())
            hasOwnEntry.quickSet(site.use.index());
    }

    const CFG& cfg = analyses.cfg();
    const UseLists& useLists = analyses.useLists();
    Vector<uint32_t> positionOf(FillWith { }, numberOfValues, 0u);
    Vector<uint32_t> positionOfEnd(FillWith { }, function.blocks.size(), 0u);
    uint32_t position = 0;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID value : function.at(block).values)
            positionOf[value.index()] = position++;
        positionOfEnd[block.index()] = position++;
    }
    auto positionOfLastUse = [&](ValueID value) {
        uint32_t last = 0;
        for (const Use& use : useLists.usesOf(value))
            last = std::max(last, use.kind == Use::Kind::Operand ? positionOf[use.user.index()] : positionOfEnd[use.block.index()]);
        return last;
    };
    auto canRunTwice = [&](ValueID value) {
        auto isUnseen = [](const Effects& effects) { return effects.writes.containsOnly(heapsWhoseWritesNobodySees) && !effects.mayRunOtherCode(); };
        if (const TestSite* site = siteOf[value.index()])
            return isUnseen(site->effectsWhenPassed);
        const EffectsOnEdges& effects = analyses.effectsOf(value);
        return isUnseen(effects.normal) && isUnseen(effects.handler);
    };

    Vector<Piece> pieces;
    Vector<uint32_t> currentPieceOf(FillWith { }, numberOfStretches, none);
    Vector<uint32_t> pieceOf(FillWith { }, numberOfValues, none);
    Vector<uint32_t> lastUseOf(FillWith { }, numberOfValues, 0u);
    auto release = [&](ValueID value, uint32_t at) {
        if (value && pieceOf[value.index()] != none && lastUseOf[value.index()] == at)
            --pieces[std::exchange(pieceOf[value.index()], none)].alive;
    };
    uint64_t bytesOfConversionsInFunction = 0;

    Vector<ProgramPoint> points;
    for (BlockID block : cfg.reversePostOrder()) {
        const Block& contents = function.at(block);
        bool isReachedOnOneEdge = cfg.incomingEdges(block).size() == 1;
        for (uint32_t index = 0; index < contents.values.size(); ++index) {
            ValueID value = contents.values[index];
            uint32_t stretch = stretchOf[value.index()];
            if (stretch != none) {
                uint32_t& current = currentPieceOf[stretch];
                auto startPiece = [&] {
                    current = pieces.size();
                    pieces.append(Piece { });
                };
                if (current == none)
                    startPiece();
                if (!index && !isReachedOnOneEdge)
                    pieces[current].entriesInRepeatableCode = 0;
                if (uint32_t tests = testsInFront[value.index()]) {
                    const Piece& piece = pieces[current];
                    bool repaysJoin = testsLeftIn[stretch] * bytesToConvertValue > bytesToJoinValue;
                    if (index && repaysJoin && bytesOfConversionsInFunction > minBytesOfConversionsInFunction && piece.bytesOfConversions > std::max(piece.bytesOfStretch, bytesToJoinValue * piece.alive)) {
                        points.append({ block, index });
                        startPiece();
                    }
                    testsLeftIn[stretch] -= tests;
                    Piece& entered = pieces[current];
                    if (hasOwnEntry.quickGet(value.index()) || ++entered.entriesInRepeatableCode <= maxEntriesInRepeatableCode) {
                        entered.bytesOfConversions += tests * bytesToConvertValue * entered.alive;
                        bytesOfConversionsInFunction += tests * bytesToConvertValue * entered.alive;
                    }
                }
            }
            function.at(value).forEachOperand([&](ValueID operand, uint32_t) { release(operand, positionOf[value.index()]); });
            if (stretch == none)
                continue;
            Piece& piece = pieces[currentPieceOf[stretch]];
            piece.bytesOfStretch += estimatedCodeBytes(context.program, function, function.at(value));
            if (!canRunTwice(value))
                piece.entriesInRepeatableCode = 0;
            if (!isConverted(value))
                continue;
            if (uint32_t last = positionOfLastUse(value); last > positionOf[value.index()]) {
                lastUseOf[value.index()] = last;
                pieceOf[value.index()] = currentPieceOf[stretch];
                ++piece.alive;
            }
        }
        contents.forEachChildOutsideValues([&](ValueID child) { release(child, positionOfEnd[block.index()]); });
    }
    return points;
}

// The blocks of the ranges that the functor picks, and every block that is reached from one of them.
static BitVector findBlocksBehindRanges(const Function& function, std::span<const StretchRange> ranges, NOESCAPE const Invocable<bool(const StretchRange&)> auto& picks)
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

static bool expectedTypeTellsResultOf(FunctionPassContext& context, const TestSite& site)
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
    if (!use.type || use.expected().predicted())
        return true;
    TypeID expectedNow = expectedTypeOf(context.program, context.function, use, context.types, &context.typingMemo).predicted();
    return !expectedNow || expectedNow == use.type;
}

static Vector<ValueID> collectSubjectsKnownBeyondTheirTypes(FunctionPassContext& context, const TestPlacementPlan& plan)
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

// `isLive` is empty before and behind.
static BitVector findPointsWhereNothingIsKnownBetter(const Function& function, BlockID block, uint32_t firstIndex, const BitVector& isKnownBetter, NOESCAPE const Invocable<const Liveness&()> auto& livenessOfValuesKnownBetter, BitVector& isLive)
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
    size_t numberOfLiveValues = 0;
    Vector<ValueID, 8> valuesThatWereLive;
    auto use = [&](ValueID value) {
        if (!value || !isKnownBetter.get(value.index()) || isLive.set(value.index()))
            return;
        ++numberOfLiveValues;
        valuesThatWereLive.append(value);
    };
    contents.forEachChildOutsideValues(use);
    contents.forEachTarget([&](const Target& target) {
        for (ValueID value : liveness.liveAtHead(target.block))
            use(value);
    });
    for (uint32_t index = values.size(); index-- > std::max(firstIndex, 1u);) {
        if (isLive.clear(values[index].index()))
            --numberOfLiveValues;
        function.at(values[index]).forEachOperand([&](ValueID operand, uint32_t) { use(operand); });
        if (!numberOfLiveValues)
            points.quickSet(index);
    }
    for (ValueID value : valuesThatWereLive)
        isLive.clear(value.index());
    return points;
}

// Where the tested code wants none of the proofs of this plan, the generic copy may come back.
static void appendRejoinPoints(TestPlacementPlan& plan, FunctionPassContext& context, const TestStretches& stretches, WantedProofs& wanted, std::span<const ProgramPoint> splitPoints)
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
        Expected<BlockFrequency> frequency = function.at(block).frequency;
        bool isRarerThanPredecessor = std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) { return (function.at(edge.from).frequency < frequency).predicted(); });
        // An arm that goes straight back is part of the code it left: what the copy brought in there would meet the tested values at the next join.
        bool goesStraightBack = std::ranges::any_of(function.at(block).terminator.targets, [&](const Target& target) { return (function.at(target.block).frequency < frequency).predicted(); });
        if (isRarerThanPredecessor && !goesStraightBack && mayRejoinAt(ProgramPoint::atHeadOf(block)))
            plan.rejoinPoints.append(ProgramPoint::atHeadOf(block));
    }
    auto orderOf = [&](ProgramPoint point) { return std::pair { cfg.reversePostOrderIndex(point.block), point.index }; };
    AOT::sort(plan.rejoinPoints, { }, orderOf);
    removeRepeatedElements(plan.rejoinPoints);
    for (ProgramPoint point : splitPoints) {
        if (mayRejoinAt(point))
            plan.splitPoints.append(point);
    }
    auto isSplitPoint = [&](ProgramPoint point) { return std::ranges::binary_search(plan.splitPoints, orderOf(point), { }, orderOf); };
    plan.rejoinPoints.removeAllMatching(isSplitPoint);

    std::span<const StretchRange> ranges = stretches.ranges();
    BitVector isBehindCopiedRange = findBlocksBehindRanges(function, ranges, [](const StretchRange& range) { return range.isCloned; });
    if (isBehindCopiedRange.isEmpty())
        return;
    // How soon the copy comes back follows from expected types, and is shuffled with them.
    const RecordedOptions& options = context.build.recordedOptions();
    if (options.has(TestingSwitch::ShuffleExpectedTypes) && WTF::pairIntHash(options.layoutSeed, context.id.index()) % 2) [[unlikely]]
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

    BitVector isLive;
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
        BitVector points = findPointsWhereNothingIsKnownBetter(function, block, firstEnd, isKnownBetter, livenessOfValuesKnownBetter, isLive);
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
    plan.rejoinPointsWithoutTests.removeAllMatching([&](ProgramPoint point) { return isSplitPoint(point) || std::ranges::binary_search(plan.rejoinPoints, orderOf(point), { }, orderOf); });
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

static TestPlacementPlan planTestPlacement(FunctionPassContext&, TestSites&&, std::optional<Vector<ProgramPoint>>&& splitPoints);

TestPlacementPlan planTestPlacement(FunctionPassContext& context, TestSites&& sites)
{
    return planTestPlacement(context, WTF::move(sites), std::nullopt);
}

static TestPlacementPlan planTestPlacement(FunctionPassContext& context, TestSites&& sites, std::optional<Vector<ProgramPoint>>&& splitPoints)
{
    TestPlacementPlan plan;
    plan.sites = WTF::move(sites);
    if (plan.sites.isEmpty())
        return plan;

    plan.conditions = makeUnique<TestConditions>(context.analyses);
    RELEASE_ASSERT(!plan.conditions->size());
    dropSitesBeyondConditionLimit(context, plan.sites);
    for (uint32_t index = 0; index < plan.sites.sites.size(); ++index) {
        const TestSite& site = plan.sites.sites[index];
        for (uint32_t test = site.firstTest; test < site.firstTest + site.numberOfTests; ++test)
            plan.requirements.append(TestRequirement { index, test, plan.conditions->add(plan.sites.tests[test]), site.point, RequirementKind::PlacesTest });
    }
    if (plan.requirements.isEmpty())
        return plan;

    Vector<ExistingProof> proofs = findExistingProofs(context, *plan.conditions);
    Vector<HoistedTest> hoistedTests = findHoistedTests(context, plan.sites, *plan.conditions, plan.requirements.span());
    BitVector valuesThatStartNewStretch;
    if (splitPoints) {
        for (ProgramPoint point : *splitPoints)
            valuesThatStartNewStretch.set(context.function.at(point.block).values[point.index].index());
    }
    TestEvents events(TestEventInputs { context.analyses, *plan.conditions, plan.sites.sites.span(), plan.requirements.span(), proofs.span(), hoistedTests.span(), true, &valuesThatStartNewStretch });
    // The generic copy goes on behind a stretch, so it holds the code of a gap whether a proof bridges the gap or not.
    uint32_t widestGap = static_cast<uint32_t>(std::min<uint64_t>(context.limit(LimitID::MaxClonedBytesPerStretch), UINT32_MAX));
    TestCoverage coverage(TestCoverageInputs { events, GapBudget { widestGap, widestGap }, nullptr, SlowPaths::MayLeave });
    WantedProofs wanted(events, coverage);
    TestStretches stretches(events, coverage, wanted);

    BitVector repeatedInVain = findTestsRepeatedInVain(plan, coverage, stretches);
    if (!repeatedInVain.isEmpty()) {
        uint32_t index = 0;
        plan.sites.sites.removeAllMatching([&](const TestSite&) { return repeatedInVain.get(index++); });
        return planTestPlacement(context, WTF::move(plan.sites), WTF::move(splitPoints));
    }
    if (!splitPoints) {
        splitPoints = findPointsToSplitStretchesAt(context, plan, coverage, stretches);
        if (!splitPoints->isEmpty())
            return planTestPlacement(context, WTF::move(plan.sites), WTF::move(splitPoints));
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

    appendRejoinPoints(plan, context, stretches, wanted, splitPoints->span());
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

namespace {

// A store may create its property. What is read may be left out by the object. What is called or looked into has to be there, or the function throws.
enum class NameDemand : uint8_t { Stored, Read, Required };

struct NamedUse {
    ValueID subject;
    ValueID access;
    ValueID receiver;
    BlockID block;
    PropertyKeyID key;
    LayoutSetID stamp;
    NameDemand demand { NameDemand::Stored };
    bool isOnEveryPath { true };
    bool callsBuiltinMethod { false };
    bool isAnsweredByBuiltins { false };
};

struct LayoutsToTest {
    Expected<LayoutList> layouts;
    bool leavesOutDeclarations { false };
};

enum class CountedUses : uint8_t { OnEveryPath, All };

using ExpectedValues = Vector<std::pair<ValueID, ExpectedType>>;
using NameList = SortedSet<PropertyKeyID, 8>;
using BuiltinClassList = SortedSet<BuiltinClassID, 4>;

// The names asked on every path from the head of a block to a return. Without a value, no path from there counts.
using AskedNames = std::optional<BitVector>;

} // anonymous namespace

static bool isOrdinaryText(const Program& program, PropertyKeyID key)
{
    return program.at(key).kind == PropertyKeyKind::Text && program.nameClassOf(key) == NameClass::Ordinary;
}

// A private name is found on the objects of its class alone, and a symbol that the program makes on the objects that the program puts it on.
// A name that the program also defines on objects without a layout does not say that the object has one.
static bool tellsLayoutsApart(const Program& program, PropertyKeyID key)
{
    PropertyKeyKind kind = program.at(key).kind;
    if (kind == PropertyKeyKind::Text)
        return isOrdinaryText(program, key) && !program.builtinNameOf(key) && !program.isDefinedOnObjectsWithoutKnownLayout(key);
    return kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::CreatedSymbol || kind == PropertyKeyKind::ModulePrivateName;
}

static std::optional<std::pair<ValueID, PropertyKeyID>> propertyReadBy(const Program& program, const Value& value)
{
    Opcode opcode = value.opcode;
    if (accessOf(opcode) != Access::Read)
        return std::nullopt;
    if (isNamedAccess(opcode)) {
        if (FlagMask::isFreeName(opcode) && value.isFreeName())
            return std::nullopt;
        return std::pair { value.operand(*receiverOf(opcode)), keyOfNamedAccess(program, value) };
    }
    if (!PayloadWord::key(opcode) || !OperandPosition::object(opcode))
        return std::nullopt;
    return std::pair { value.objectOperand(), value.key() };
}

// The value that stands for every value the function uses as one: a value behind its casts and tests, and what is read from one object under one name.
static Vector<ValueID> findSubjects(FunctionPassContext& context)
{
    const Function& function = context.function;
    const ValueIdentities& identities = context.analyses.identities();
    Vector<ValueID> subjects(function.values.size(), [&](size_t index) { return identities.identityOf(ValueID(index)); });

    HashMap<std::pair<ValueID, PropertyKeyID>, ValueID> firstReads;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (subjects[id.index()] != id)
                continue;
            if (auto read = propertyReadBy(context.program, function.at(id)))
                subjects[id.index()] = firstReads.add({ subjects[subjects[read->first.index()].index()], read->second }, id).iterator->value;
        }
    }
    for (ValueID& subject : subjects)
        subject = subjects[subject.index()];
    return subjects;
}

// Whether an operation that takes the value as an operand, as it is or behind casts, matches.
static bool anyUserBehindCasts(FunctionAnalyses& analyses, ValueID value, NOESCAPE const Invocable<bool(const Value& user, uint32_t operandIndex)> auto& matches)
{
    Vector<ValueID, 4> worklist { value };
    while (!worklist.isEmpty()) {
        for (const Use& use : analyses.useLists().usesOf(worklist.takeLast())) {
            if (use.kind != Use::Kind::Operand)
                continue;
            const Value& user = analyses.function().at(use.user);
            if (isCast(user.opcode) && !use.operandIndex)
                worklist.append(use.user);
            else if (matches(user, use.operandIndex))
                return true;
        }
    }
    return false;
}

static bool isCalleeOf(const Value& user, uint32_t operandIndex)
{
    return (user.opcode == Opcode::Call && operandIndex == OperandIndex::Call::callee) || (user.opcode == Opcode::CallList && operandIndex == OperandIndex::CallList::callee);
}

static bool throwsOnUndefined(const Value& user, uint32_t operandIndex)
{
    if (isNamedAccess(user.opcode))
        return *receiverOf(user.opcode) == operandIndex;
    if (elementAccessOf(user.opcode))
        return OperandPosition::object(user.opcode) == operandIndex;
    return isCalleeOf(user, operandIndex);
}

static NameDemand demandOf(FunctionPassContext& context, ValueID id, PropertyKeyID key)
{
    Access access = accessOf(context.function.at(id).opcode);
    // An access under a private name throws where the object lacks it, unless it defines it.
    if (context.program.at(key).kind == PropertyKeyKind::PrivateName)
        return access == Access::Define ? NameDemand::Stored : NameDemand::Required;
    if (access != Access::Read)
        return NameDemand::Stored;
    return anyUserBehindCasts(context.analyses, id, throwsOnUndefined) ? NameDemand::Required : NameDemand::Read;
}

static BuiltinClassList builtinClassesWithMethod(const Program& program, PropertyKeyID key)
{
    BuiltinClassList classes;
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    if (!name || name.isPrivateAlias)
        return classes;
    for (const MemberOfClasses& method : methodsNamed(program.builtins(), name.name)) {
        for (BuiltinClassID builtinClass : method.classes)
            classes.add(builtinClass);
    }
    return classes;
}

static Expected<bool> hasCalleesListedByFlow(const Program& program, const Value& call)
{
    if (!Value::opcodesWithCallees.contains(call.opcode) || !call.callees())
        return false;
    const CalleeSet& callees = program.at(call.callees());
    return callees.listsEveryCallee() || !callees.seen().predicted().functions.empty();
}

static Expected<bool> isCalledByNameAlone(FunctionAnalyses& analyses, const Program& program, ValueID read)
{
    return isOnlyCalled(analyses, read) && !anyUserBehindCasts(analyses, read, [&](const Value& user, uint32_t) { return hasCalleesListedByFlow(program, user).predicted(); });
}

static bool callsBuiltinMethod(FunctionPassContext& context, ValueID read, PropertyKeyID key)
{
    return context.function.at(read).opcode == Opcode::PropGet && isCalledByNameAlone(context.analyses, context.program, read).predicted() && !builtinClassesWithMethod(context.program, key).isEmpty();
}

// What is called only behind a join counts as not called: this feeds a guess.
static bool isCalledInPlace(FunctionPassContext& context, ValueID read)
{
    return anyUserBehindCasts(context.analyses, read, isCalleeOf);
}

static bool isGetterAlone(const BuiltinTable& builtins, BuiltinID member)
{
    return builtins.at(member).kind == BuiltinKind::Accessor && !std::ranges::contains(builtins.overloadSummariesOf(member), BuiltinCallMode::Set, &BuiltinOverloadSummary::mode);
}

// By name such a store is a call by value out of a stub. Into a field it is a few instructions, and a test for the layout buys little.
static bool findsSetterOfProgram(const Program& program, PropertyKeyID key)
{
    auto isSetter = [&](const ClassMember& member) {
        return member.key == key && member.side == ClassSide::Prototype && member.kind == MethodKind::Setter;
    };
    return std::ranges::any_of(program.declarersOfName(key), [&](LayoutID declarer) {
        const ClassFlow& classFlow = program.at(declarer).classFlow;
        return std::ranges::any_of(classFlow.members, isSetter) || std::ranges::any_of(classFlow.namedMembers, isSetter);
    });
}

static bool isOfOtherKindInBuiltins(FunctionPassContext& context, ValueID access, PropertyKeyID key)
{
    const BuiltinTable& builtins = context.program.builtins();
    BuiltinMemberName name = builtinMemberNameOf(context.program, key);
    if (!isOrdinaryText(context.program, key) || !name || name.isPrivateAlias)
        return false;
    bool stores = accessOf(context.function.at(access).opcode) == Access::Store && findsSetterOfProgram(context.program, key);
    bool asksForMethod = accessOf(context.function.at(access).opcode) == Access::Read && isCalledInPlace(context, access);
    std::span<const BuiltinClassID> classes = builtins.classesWithInstanceMember(name.name);
    if (asksForMethod && classes.empty())
        return false;
    return std::ranges::all_of(classes, [&](BuiltinClassID builtinClass) {
        BuiltinMemberLookup found = builtins.findForInstance(builtinClass, name.name);
        if (found.outcome != BuiltinMemberLookup::Outcome::Found)
            return false;
        BuiltinID member = builtins.canonicalID(found.member);
        // `response.body = text` stores nothing: whoever stores under the name has an object in mind that takes the value.
        return (stores && isGetterAlone(builtins, member)) || (builtins.at(member).kind == BuiltinKind::Method) != asksForMethod;
    });
}

static Vector<NamedUse> collectNamedUsesOfUnknownObjects(FunctionPassContext& context)
{
    const Program& program = context.program;
    const Function& function = context.function;
    Vector<NamedUse> uses;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        // Where a test has failed, the copy that takes over is seldom entered, and the object there is none of those that were expected.
        if (function.at(block).frequency.predicted() != BlockFrequency::Normal)
            continue;
        std::optional<bool> isColdBlock;
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            Opcode opcode = access.opcode;
            if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
                continue;
            RELEASE_ASSERT(testSiteOpcodes.contains(opcode));
            ValueID receiver = access.operand(*receiverOf(opcode));
            const Type& type = program.typeOf(function, receiver);
            PropertyKeyID key = keyOfNamedAccess(program, access);
            if (!type.anyObject || type.layouts)
                continue;
            // A test for the layouts that have the name would turn such an object away, and what stands behind the test would run in the copy.
            if (PayloadWord::accessSite(opcode) && access.accessSite() && context.inputs.flow.lossOf(access.accessSite()).seesConstructedObject)
                continue;
            // A read by name is a short call, which cold code keeps. Under a private name it is an operation of the run time.
            if (!isColdBlock)
                isColdBlock = isCold(heatClassOf(context, block)).predicted();
            if (*isColdBlock && program.at(key).kind != PropertyKeyKind::PrivateName && (tellsLayoutsApart(program, key) || !callsBuiltinMethod(context, id, key)))
                continue;
            // `Promise.resolve` through a variable that is expected to hold the constructor: flow has followed the one object, and it answers.
            if (TypeID expected = function.at(receiver).expected().predicted(); expected && !context.types.at(expected).builtins.isEmpty())
                continue;
            bool tellsLayouts = tellsLayoutsApart(program, key);
            bool isBuiltinMethodCall = !tellsLayouts && callsBuiltinMethod(context, id, key);
            // Among the layouts such a name tells as much as any other. Whether the object has a layout at all is for the other names to say.
            bool isAnsweredByBuiltins = !tellsLayouts && !isBuiltinMethodCall && !isOfOtherKindInBuiltins(context, id, key);
            if (isAnsweredByBuiltins && !isOrdinaryText(program, key))
                continue;
            uses.append(NamedUse { { }, id, receiver, block, key, PayloadWord::receivers(opcode) ? access.receivers() : LayoutSetID(), demandOf(context, id, key), true, isBuiltinMethodCall, isAnsweredByBuiltins });
        }
    }
    return uses;
}

static AskedNames namesAskedBehind(const Terminator& terminator, const Vector<AskedNames>& askedFrom)
{
    if (returnsToCaller(terminator.kind))
        return BitVector();
    AskedNames asked;
    for (const Target& target : terminator.targets) {
        const AskedNames& askedFromTarget = askedFrom[target.block.index()];
        if (!askedFromTarget)
            continue;
        if (asked)
            asked->filter(*askedFromTarget);
        else
            asked = *askedFromTarget;
    }
    return asked;
}

// An object that takes the other path is never asked for a name that one path alone asks for, so such a name tells nothing about the objects that arrive.
static void findUsesOnEveryPath(FunctionPassContext& context, std::span<NamedUse> usesBySubject)
{
    struct BranchingSubject {
        std::span<NamedUse> uses;
        uint32_t firstName { 0 };
        Vector<PropertyKeyID, 8> keys;

        uint32_t nameOf(const NamedUse& use) const { return firstName + static_cast<uint32_t>(keys.find(use.key)); }
    };
    Vector<BranchingSubject, 4> subjects;
    uint32_t numberOfNames = 0;
    for (std::span<NamedUse> uses : usesBySubject | std::views::chunk_by([](const NamedUse& a, const NamedUse& b) { return a.subject == b.subject; })) {
        if (std::ranges::all_of(uses, [&](const NamedUse& use) { return use.block == uses[0].block; }))
            continue;
        BranchingSubject subject { uses, numberOfNames, { } };
        for (const NamedUse& use : uses)
            subject.keys.appendIfNotContains(use.key);
        if (subject.keys.size() < 2)
            continue;
        numberOfNames += subject.keys.size();
        subjects.append(WTF::move(subject));
    }
    if (subjects.isEmpty())
        return;

    Vector<BitVector> askedIn(context.function.blocks.size());
    for (const BranchingSubject& subject : subjects) {
        for (const NamedUse& use : subject.uses)
            askedIn[use.block.index()].set(subject.nameOf(use));
    }
    // A path through a block that is marked as seldom entered does not count, nor does one that ends in a throw or leaves for a handler.
    Vector<AskedNames> askedFrom(context.function.blocks.size());
    for (bool didChange = true; std::exchange(didChange, false);) {
        for (BlockID id : context.analyses.cfg().reversePostOrder() | std::views::reverse) {
            const Block& block = context.function.at(id);
            AskedNames asked = block.frequency.predicted() == BlockFrequency::Normal ? namesAskedBehind(block.terminator, askedFrom) : std::nullopt;
            if (!asked)
                continue;
            asked->merge(askedIn[id.index()]);
            if (asked == askedFrom[id.index()])
                continue;
            askedFrom[id.index()] = WTF::move(asked);
            didChange = true;
        }
    }

    const Dominators& dominators = context.analyses.dominators();
    for (const BranchingSubject& subject : subjects) {
        BlockID commonDominator = subject.uses[0].block;
        for (const NamedUse& use : subject.uses)
            commonDominator = dominators.nearestCommonDominator(commonDominator, use.block);
        const AskedNames& asked = askedFrom[commonDominator.index()];
        if (!asked)
            continue;
        for (NamedUse& use : subject.uses)
            use.isOnEveryPath = asked->get(subject.nameOf(use));
    }
}

// A stamp that is not complete lists what flow saw arrive beside what it did not see. It joins what the names say and does not stand for it.
static Expected<bool> isExpectedToFindMember(const Program& program, const Function& function, const NamedUse& use)
{
    LayoutSetID stamp = use.stamp && program.at(use.stamp).listsEveryLayout() ? use.stamp : LayoutSetID();
    if (!function.at(use.receiver).expected().predicted() && !stamp)
        return false;
    std::span<const LayoutID> holders = program.layoutsWithName(use.key);
    return std::ranges::any_of(receiverCandidates(program, function, use.receiver, stamp).expected.predicted(), [&](LayoutID layout) { return std::ranges::binary_search(holders, layout); });
}

static LayoutID declarerOf(const Program& program, LayoutID layout, PropertyKeyID name)
{
    std::span<const LayoutID> holders = program.layoutsWithName(name);
    auto holder = std::ranges::lower_bound(holders, layout);
    if (holder == holders.end() || *holder != layout)
        return { };
    return program.declarersOfName(name)[holder - holders.begin()];
}

// A class and the layouts below it have an inherited field in one slot, and a test for all of them is one test.
static LayoutsToTest keepLikeliestDeclarations(FunctionPassContext& context, const Expected<LayoutList>& guessed, std::span<const NamedUse> uses, size_t maxDeclarations, const Expected<LayoutList>& metElsewhere)
{
    const LayoutList& layouts = guessed.predicted();
    const LayoutList& layoutsMetElsewhere = metElsewhere.predicted();
    if (layouts.size() <= maxDeclarations)
        return { layouts, false };

    const Program& program = context.program;
    const Heat& heat = context.inputs.heat;
    NameList names;
    for (const NamedUse& use : uses)
        names.add(use.key);

    ModuleID module = context.function.module;
    PropertyKeyID nameOfSubject;
    if (auto read = propertyReadBy(program, context.function.at(context.analyses.identityOf(uses[0].receiver))))
        nameOfSubject = read->second;
    // What flow saw arrive at one of the uses does arrive there, whatever else does.
    LayoutList layoutsSeenToArrive;
    for (const NamedUse& use : uses) {
        if (use.stamp)
            layoutsSeenToArrive.unite(LayoutList(program.at(use.stamp).seen().predicted()));
    }
    using Likelihood = std::tuple<bool, bool, bool, bool, double>;
    using RankedLayout = std::pair<Likelihood, LayoutID>;
    Vector<RankedLayout, 16> likeliestFirst(layouts.size(), [&](size_t index) {
        LayoutID layout = layouts.span()[index];
        return RankedLayout { Likelihood { nameOfSubject && heat.isStoredUnder(layout, nameOfSubject).predicted(), layoutsSeenToArrive.contains(layout), layoutsMetElsewhere.contains(layout), module && (program.at(layout).module == module || heat.isAllocatedIn(layout, module).predicted()), heat.allocationsOf(layout).predicted() }, layout };
    });
    AOT::stableSort(likeliestFirst, std::ranges::greater { }, &RankedLayout::first);

    Vector<LayoutID, 16> kept;
    Vector<LayoutList, 8> declarersOfNames(names.size());
    Vector<LayoutID, 8> declarers(names.size());
    for (auto [likelihood, layout] : likeliestFirst) {
        bool hasRoom = true;
        for (size_t index = 0; index < names.size(); ++index) {
            declarers[index] = declarerOf(program, layout, names[index]);
            hasRoom &= !declarers[index] || declarersOfNames[index].size() < maxDeclarations || declarersOfNames[index].contains(declarers[index]);
        }
        if (!hasRoom)
            continue;
        for (size_t index = 0; index < names.size(); ++index) {
            if (declarers[index])
                declarersOfNames[index].add(declarers[index]);
        }
        kept.append(layout);
    }
    return { LayoutList(kept.span()), kept.size() < layouts.size() };
}

// Whoever gets hold of an object may store to it. Where flow knows every writer of a field, no object with that field has reached code that flow does not see, and none comes out of it.
static LayoutList withoutLayoutsThatStayInSight(const Program& program, std::span<const NamedUse> uses, const LayoutList& layouts)
{
    LayoutList layoutsSeenToArrive;
    for (const NamedUse& use : uses) {
        if (use.stamp)
            layoutsSeenToArrive.unite(LayoutList(program.at(use.stamp).seen().predicted()));
    }
    LayoutList kept;
    for (LayoutID layout : layouts) {
        bool staysInSight = std::ranges::any_of(uses, [&](const NamedUse& use) {
            std::optional<FieldReference> field = findField(program, layout, use.key, BaseChain::Predicted);
            return field && fieldAt(program, *field).flow.location.writers == WriterState::AllKnown;
        });
        if (!staysInSight || layoutsSeenToArrive.contains(layout))
            kept.add(layout);
    }
    return kept;
}

// The object is expected to have every name that the uses that count ask it for.
static Expected<LayoutList> layoutsExpectedFrom(const Program& program, std::span<const NamedUse> uses, CountedUses countedUses, size_t maxLayouts)
{
    // A name that this many layouts have tells none of them from the others.
    auto isCommon = [&](PropertyKeyID name) { return program.layoutsWithName(name).size() > maxLayouts; };
    NameList names;
    for (NameDemand weakestDemand : { NameDemand::Stored, NameDemand::Read, NameDemand::Required }) {
        names.clear();
        for (const NamedUse& use : uses) {
            if (countedUses == CountedUses::OnEveryPath && !use.isOnEveryPath)
                continue;
            // A name that no layout has says nothing about layouts.
            if (use.demand >= weakestDemand && !program.layoutsWithName(use.key).empty())
                names.add(use.key);
        }
        if (names.isEmpty())
            break;
        if (std::ranges::all_of(names, isCommon))
            return LayoutList();
        LayoutList layouts = allocatedLayoutsWith(program, names.span(), maxLayouts);
        if (layouts.size() <= maxLayouts)
            layouts = withoutLayoutsThatStayInSight(program, uses, layouts);
        if (!layouts.isEmpty())
            return layouts;
    }

    for (const NamedUse& use : uses)
        names.add(use.key);
    LayoutList layoutsWithAnyName;
    for (PropertyKeyID name : names) {
        if (isCommon(name))
            continue;
        for (LayoutID layout : program.layoutsWithName(name)) {
            if (hasMemberToFindUnder(program, layout, name))
                layoutsWithAnyName.add(layout);
        }
    }
    return withoutLayoutsThatStayInSight(program, uses, layoutsWithAnyName);
}

static Expected<bool> resolvesSomeUse(FunctionPassContext& context, std::span<const NamedUse> uses, const Expected<LayoutList>& layouts)
{
    return std::ranges::any_of(uses, [&](const NamedUse& use) {
        const Value& access = context.function.at(use.access);
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, access);
        if (!query)
            return false;
        query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
        query->isOnlyCalled = access.opcode == Opcode::PropGet && isOnlyCalled(context.analyses, use.access);
        query->receivers.level = KnowledgeLevel::Expected;
        query->receivers.expected = layouts;
        PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
        // `events.removeListener === undefined`, `if (value.then)`: whoever reads a name and does not call what he got asks whether it is there, as `"then" in value` and `Object.hasOwn(value, "then")` do. `target.get.apply(..)` does not ask.
        bool asksWhetherMethodIsThere = plan.form == AccessForm::Method && !query->isOnlyCalled && use.demand != NameDemand::Required;
        bool asksWhetherNameIsThere = query->access == Access::Has && !query->isPrivate;
        return plan.isResolved() && !asksWhetherMethodIsThere && !asksWhetherNameIsThere;
    });
}

static bool isAllocatedHere(FunctionPassContext& context, ValueID value)
{
    return !proofsOfAllocation(context.program, context.function, context.function.at(context.analyses.identityOf(value))).isEmpty();
}

static Expected<LayoutList> withoutLayoutsOfOtherKind(FunctionPassContext& context, std::span<const NamedUse> uses, const Expected<LayoutList>& layouts)
{
    const Program& program = context.program;
    auto holdsOnlyFunctions = [&](const FieldReference& field) {
        const Type& slotType = program.at(slotTypeOf(program, fieldAt(program, field)));
        return !slotType.isUndefinedOrNull() && !(slotType.primitives & ~Primitive::UndefinedOrNull) && slotType.withOnlyObjects().isSubsetOf(Type::function(), program);
    };
    LayoutList kept = layouts.predicted();
    for (const NamedUse& use : uses) {
        bool asksForMethod = use.demand == NameDemand::Required && isCalledInPlace(context, use.access);
        if (use.demand == NameDemand::Required && !asksForMethod)
            continue;
        LayoutList layoutsOfOtherKind;
        for (LayoutID layout : kept) {
            std::optional<FieldReference> field = findField(program, layout, use.key, BaseChain::Predicted);
            if (field && holdsOnlyFunctions(*field) != asksForMethod)
                layoutsOfOtherKind.add(layout);
        }
        if (layoutsOfOtherKind.size() < kept.size())
            kept.subtract(layoutsOfOtherKind);
    }
    return kept;
}

// The names asked on every path leave the layouts in that a name asked on one path would take out. Where they leave more layouts than a test is made for, every name counts.
static LayoutsToTest findLayoutsToTestFor(FunctionPassContext& context, std::span<const NamedUse> uses, size_t maxDeclarations, size_t maxLayouts, const Expected<LayoutList>& layoutsMetElsewhere)
{
    auto choose = [&](CountedUses countedUses) -> LayoutsToTest {
        Expected<LayoutList> layoutsWithNames = layoutsExpectedFrom(context.program, uses, countedUses, maxLayouts);
        if (layoutsWithNames.predicted().isEmpty() || layoutsWithNames.predicted().size() > maxLayouts)
            return { };
        LayoutsToTest chosen = keepLikeliestDeclarations(context, withoutLayoutsOfOtherKind(context, uses, layoutsWithNames), uses, maxDeclarations, layoutsMetElsewhere);
        if (chosen.layouts.predicted().isEmpty() || !resolvesSomeUse(context, uses, chosen.layouts).predicted())
            return { };
        return chosen;
    };
    auto takesEveryLayout = [](const LayoutsToTest& chosen) { return !chosen.layouts.predicted().isEmpty() && !chosen.leavesOutDeclarations; };

    LayoutsToTest fromEveryPath = choose(CountedUses::OnEveryPath);
    if (takesEveryLayout(fromEveryPath) || std::ranges::all_of(uses, &NamedUse::isOnEveryPath))
        return fromEveryPath;
    LayoutsToTest fromEveryName = choose(CountedUses::All);
    if (fromEveryPath.layouts.predicted().isEmpty() || takesEveryLayout(fromEveryName))
        return fromEveryName;
    return fromEveryPath;
}

static void expectOfReceivers(std::span<const NamedUse> uses, ExpectedType expected, ExpectedValues& expectedValues)
{
    for (const NamedUse& use : uses) {
        if (!expectedValues.containsIf([&](const auto& listed) { return listed.first == use.receiver; }))
            expectedValues.append({ use.receiver, expected });
    }
}

static std::optional<Type> receiversAmong(FunctionPassContext& context, const BuiltinClassList& classes, std::span<const PropertyKeyID> names)
{
    size_t maxReceivers = context.limit(LimitID::MaxDispatchCases);
    LayoutList layouts = allocatedLayoutsWith(context.program, names, maxReceivers);
    if (classes.size() + layouts.size() > maxReceivers)
        return std::nullopt;

    Type receivers = layouts.isEmpty() ? Type::never() : Type::forLayouts(layouts.span(), context.types);
    for (BuiltinClassID builtinClass : classes) {
        std::optional<Type> instances = testedTypeOfInstances(context.program, builtinClass, Type::anything());
        if (!instances)
            return std::nullopt;
        receivers = receivers.unionWith(*instances, context.types);
    }
    if (receivers.isNever())
        return std::nullopt;
    return receivers;
}

static std::optional<Type> receiversWithMethod(FunctionPassContext& context, PropertyKeyID key)
{
    return receiversAmong(context, builtinClassesWithMethod(context.program, key), singleElementSpan(key));
}

static std::optional<Type> receiversWithEveryMethod(FunctionPassContext& context, std::span<const NamedUse> uses, CountedUses countedUses)
{
    std::optional<BuiltinClassList> classes;
    NameList names;
    for (const NamedUse& use : uses) {
        if (countedUses == CountedUses::OnEveryPath && !use.isOnEveryPath)
            continue;
        if (use.callsBuiltinMethod) {
            BuiltinClassList classesWithMethod = builtinClassesWithMethod(context.program, use.key);
            if (classes)
                classes->intersect(classesWithMethod);
            else
                classes = WTF::move(classesWithMethod);
            names.add(use.key);
        } else if (use.demand == NameDemand::Required && !context.program.layoutsWithName(use.key).empty()) {
            classes = BuiltinClassList();
            names.add(use.key);
        }
    }
    return classes ? receiversAmong(context, *classes, names.span()) : std::nullopt;
}

static bool expectReceiversOfBuiltinMethods(FunctionPassContext& context, std::span<const NamedUse> uses, ExpectedValues& expectedValues)
{
    std::optional<Type> receivers = receiversWithEveryMethod(context, uses, CountedUses::OnEveryPath);
    if (!receivers)
        receivers = receiversWithEveryMethod(context, uses, CountedUses::All);
    for (size_t index = 0; !receivers && index < uses.size(); ++index) {
        if (uses[index].callsBuiltinMethod)
            receivers = receiversWithMethod(context, uses[index].key);
    }
    if (!receivers)
        return false;
    expectOfReceivers(uses, ExpectedType(context.types.intern(*receivers)), expectedValues);
    return true;
}

// `twin.index`, and nothing else is asked of the twin.
static Expected<LayoutList> layoutsMetThatAnswer(FunctionPassContext& context, std::span<const NamedUse> uses, const Expected<LayoutList>& layoutsMet)
{
    bool isExpectedToBeSomeObject = std::ranges::any_of(uses, [&](const NamedUse& use) {
        TypeID expected = context.function.at(use.receiver).expected().predicted();
        return expected && !context.types.at(expected).withOnlyObjects().isNever();
    });
    LayoutList layouts;
    if (isExpectedToBeSomeObject)
        return layouts;
    for (LayoutID layout : layoutsMet.predicted()) {
        if (std::ranges::all_of(uses, [&](const NamedUse& use) { return std::ranges::binary_search(context.program.layoutsWithName(use.key), layout); }))
            layouts.add(layout);
    }
    return layouts;
}

// `this.response.status`: the layouts that the program stores under `response` and that answer `status`.
static Expected<LayoutList> findLayoutsStoredUnderNameOfSubject(FunctionPassContext& context, std::span<const NamedUse> uses, size_t maxLayouts)
{
    const Program& program = context.program;
    auto read = propertyReadBy(program, context.function.at(context.analyses.identityOf(uses[0].receiver)));
    if (!read || !read->second || context.function.at(uses[0].receiver).expected().predicted())
        return LayoutList();
    LayoutList layouts;
    for (LayoutID layout : program.layoutsWithName(uses[0].key)) {
        if (!context.inputs.heat.isStoredUnder(layout, read->second).predicted())
            continue;
        if (!std::ranges::all_of(uses, [&](const NamedUse& use) { return hasMemberToFindUnder(program, layout, use.key); }))
            continue;
        layouts.add(layout);
        if (layouts.size() > maxLayouts)
            return LayoutList();
    }
    return layouts;
}

// Answers whether the subject is settled. One whose names have more declarations than a test is made for waits until the layouts of the other subjects are known.
static bool expectLayoutsOf(FunctionPassContext& context, std::span<NamedUse> usesOfSubject, Expected<LayoutList>& layoutsMet, bool waitsForOtherSubjects, ExpectedValues& expectedValues)
{
    Function& function = context.function;
    auto expectLayouts = [&](std::span<const NamedUse> uses, const Expected<LayoutList>& layouts) {
        expectOfReceivers(uses, ExpectedType(context.types.intern(Type::forLayouts(layouts.predicted().span(), context.types))), expectedValues);
    };
    auto noteMet = [&](const Expected<LayoutList>& layouts) {
        LayoutList met = layoutsMet.predicted();
        met.unite(layouts.predicted());
        layoutsMet = WTF::move(met);
    };
    bool isFollowed = std::ranges::any_of(usesOfSubject, [&](const NamedUse& use) { return isExpectedToFindMember(context.program, function, use).predicted(); });
    // In a checked function what is expected of a value is what the checker says of it. Where that lists layouts that something makes the names have nothing to add: a name that none of them holds is found above them.
    bool typeListsLayouts = function.has(FunctionFlags::IsChecked) && std::ranges::any_of(usesOfSubject, [&](const NamedUse& use) {
        TypeID expected = function.at(use.receiver).expected().predicted();
        return expected && std::ranges::any_of(membersOf(context.program, context.types.at(expected)), [&](LayoutID layout) {
            return !context.program.at(layout).flags.contains(LayoutFlags::IsNeverAllocated) && context.program.at(layout).flow.allocation != AllocationState::NeverAllocated;
        });
    });
    if (isFollowed || typeListsLayouts || isAllocatedHere(context, usesOfSubject[0].receiver))
        return true;

    auto builtinMethodCalls = std::ranges::stable_partition(usesOfSubject, std::logical_not { }, &NamedUse::callsBuiltinMethod);
    if (!builtinMethodCalls.empty() && expectReceiversOfBuiltinMethods(context, usesOfSubject, expectedValues))
        return true;
    std::span<const NamedUse> uses = usesOfSubject.first(usesOfSubject.size() - builtinMethodCalls.size());
    if (std::ranges::all_of(uses, &NamedUse::isAnsweredByBuiltins)) {
        if (uses.empty())
            return true;
        if (waitsForOtherSubjects)
            return false;
        Expected<LayoutList> layouts = findLayoutsStoredUnderNameOfSubject(context, uses, context.limit(LimitID::MaxDeclarationsPredictedFromNames));
        if (layouts.predicted().isEmpty())
            layouts = layoutsMetThatAnswer(context, uses, layoutsMet);
        if (!layouts.predicted().isEmpty())
            expectLayouts(uses, layouts);
        return true;
    }

    size_t maxDeclarations = context.limit(LimitID::MaxDeclarationsPredictedFromNames);
    Expected<LayoutList> layoutsStoredUnderName = findLayoutsStoredUnderNameOfSubject(context, uses, maxDeclarations);
    if (!layoutsStoredUnderName.predicted().isEmpty() && resolvesSomeUse(context, uses, layoutsStoredUnderName).predicted()) {
        noteMet(layoutsStoredUnderName);
        expectLayouts(uses, layoutsStoredUnderName);
        return true;
    }
    LayoutsToTest chosen = findLayoutsToTestFor(context, uses, maxDeclarations, context.limit(LimitID::MaxAccessPlanMembers), layoutsMet);
    if (chosen.layouts.predicted().isEmpty())
        return true;
    if (chosen.leavesOutDeclarations) {
        if (waitsForOtherSubjects)
            return false;
        context.gaveUp(LimitID::MaxDeclarationsPredictedFromNames, uses[0].access);
    } else if (chosen.layouts.predicted().size() <= maxDeclarations)
        noteMet(chosen.layouts);

    expectLayouts(uses, chosen.layouts);
    return true;
}

// The layouts of the receivers that the function knows or expects already. A receiver that may be one of many tells nothing.
static void addLayoutsOfKnownReceivers(FunctionPassContext& context, Expected<LayoutList>& layoutsMet)
{
    LayoutList met = layoutsMet.predicted();
    const Function& function = context.function;
    size_t maxLayoutsOfReceiver = context.limit(LimitID::MaxDeclarationsPredictedFromNames);
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& access = function.at(id);
            std::optional<uint32_t> receiver = receiverOf(access.opcode);
            if (!receiver || !access.operand(*receiver))
                continue;
            const ReceiverCandidates candidates = receiverCandidates(context.program, function, access.operand(*receiver));
            for (const LayoutList* layouts : { &candidates.proven, &candidates.expected.predicted() }) {
                if (layouts->size() <= maxLayoutsOfReceiver)
                    met.unite(*layouts);
            }
        }
    }
    layoutsMet = WTF::move(met);
}

// The type of a checked position is what every value that is passed to it was tested for, so it stays.
static DeclaredType* findUncheckedEntryParameter(Function& function, ValueID value)
{
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span().first(function.numberOfEntryParameters());
    auto parameter = std::ranges::find(parameters, value);
    if (parameter == parameters.end())
        return nullptr;
    DeclaredType* declared = function.declaredEntryParameter(static_cast<unsigned>(parameter - parameters.begin()));
    bool isUnchecked = declared && (declared->isExpected() || declared->written() == TypeTable::anything);
    return isUnchecked ? declared : nullptr;
}

// What is expected of an entry parameter is said by its position: it reaches every use and stays when the values are typed again.
static bool expectValues(FunctionPassContext& context, const ExpectedValues& expectedValues, ExpectedTypesByUse& expectations)
{
    Function& function = context.function;
    auto narrowedTypeOf = [&](ValueID id, ExpectedType expected) {
        const Value& value = function.at(id);
        TypeID narrowed = context.types.intersectionOf(value.type, expected.predicted());
        TypeID expectedAlready = value.expected().predicted() ? context.types.intersectionOf(narrowed, value.expected().predicted()) : TypeTable::never;
        if (expectedAlready != TypeTable::never)
            narrowed = expectedAlready;
        return narrowed == TypeTable::never || narrowed == value.type ? TypeID() : narrowed;
    };

    Vector<ValueID, 4> changedParameters;
    for (auto [value, expected] : expectedValues) {
        TypeID narrowed = narrowedTypeOf(value, expected);
        DeclaredType* declared = narrowed ? findUncheckedEntryParameter(function, value) : nullptr;
        if (!declared)
            continue;
        if (declared->written() != narrowed || !declared->isExpected()) {
            declared->expect(ExpectedType(narrowed));
            changedParameters.append(value);
        }
        if (context.types.at(narrowed).layouts)
            expectations.didGuessFromNames(value);
    }
    // A cut renumbers the values.
    if (!changedParameters.isEmpty() && narrowValueTypes(TypePropagationInputs { context.program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo }, changedParameters.span()).cutBlocks)
        return true;

    for (auto [value, expected] : expectedValues) {
        TypeID narrowed = narrowedTypeOf(value, expected);
        if (narrowed && narrowed != function.at(value).expected().predicted()) {
            expectations.expect(value, narrowed);
            if (context.types.at(narrowed).layouts)
                expectations.didGuessFromNames(value);
        }
    }
    return !changedParameters.isEmpty();
}

static ValueID receiverOfMethodCall(TestSiteReporter& reporter, const Value& use)
{
    if (use.opcode == Opcode::PropGet)
        return isCalledByNameAlone(reporter.analyses(), reporter.program(), reporter.use()).predicted() ? use.objectOperand() : ValueID();
    if (use.opcode == Opcode::Call && use.calleeKey() && !hasCalleesListedByFlow(reporter.program(), use).predicted())
        return use.thisValueOperand();
    return { };
}

// `handle.flags`, `handle.onabort = undefined`, where the handle is the native object or its stand-in written in JavaScript.
static ValueID receiverIfBuiltinPartHasLockedMember(TestSiteReporter& reporter, ValueID receiver, PropertyKeyID key, BuiltinKind kind)
{
    const Program& program = reporter.program();
    const BuiltinTable& builtins = program.builtins();
    std::optional<Type> expected = reporter.expectedPartOf(receiver);
    BuiltinMemberName name = expected ? builtinMemberNameOf(program, key) : BuiltinMemberName();
    if (!name)
        return { };
    bool reachesLockedMember = std::ranges::any_of(expected->classes, [&](const ClassPart& part) {
        BuiltinMemberLookup found = builtins.findForInstance(part.builtinClass, name.name);
        if (found.outcome != BuiltinMemberLookup::Outcome::Found || builtins.at(builtins.canonicalID(found.member)).kind != kind)
            return false;
        if (kind == BuiltinKind::Method && !hasDirectFunction(builtins, builtins.canonicalID(found.member)))
            return false;
        return isLockedLocation(program, found.holder, key);
    });
    return reachesLockedMember ? receiver : ValueID();
}

static ValueID receiverOfLockedMethodCall(TestSiteReporter& reporter, const Value& use)
{
    if (use.opcode == Opcode::PropGet && use.key() && isOnlyCalled(reporter.analyses(), reporter.use()) && use.objectOperand())
        return receiverIfBuiltinPartHasLockedMember(reporter, use.objectOperand(), use.key(), BuiltinKind::Method);
    if (use.opcode == Opcode::Call && use.calleeKey() && use.thisValueOperand())
        return receiverIfBuiltinPartHasLockedMember(reporter, use.thisValueOperand(), use.calleeKey(), BuiltinKind::Method);
    return { };
}

static ValueID receiverOfAccessorAccess(TestSiteReporter& reporter, const Value& use)
{
    if ((use.opcode != Opcode::PropGet && use.opcode != Opcode::PropSet) || !use.key())
        return { };
    return receiverIfBuiltinPartHasLockedMember(reporter, use.objectOperand(), use.key(), BuiltinKind::Accessor);
}

// `req.headers`, where the request is expected to be the one of the host's library, which makes its headers in a getter, or a stand-in that keeps them in a field.
static std::optional<Type> receiversThatAnswerWithFieldOrGetter(TestSiteReporter& reporter, const Value& use)
{
    if (use.opcode != Opcode::PropGet || !use.key())
        return std::nullopt;
    FunctionPassContext& context = reporter.context();
    // Nearly every read of a program is none of these, and the same few reads stand all over it.
    PropertyAccessCache* cache = context.program.propertyAccessCache();
    std::optional<SiteContent> content = cache ? contentOfNamedAccess(context, use, static_cast<uint8_t>(TestSitePlanner::FieldOrGetter)) : std::nullopt;
    bool isKnownToBeRefused = content && cache->refusedContents.find(*content);
    if (isKnownToBeRefused && !cache->checksAnswers) [[likely]]
        return std::nullopt;
    auto refuse = [&]() -> std::optional<Type> {
        if (content && !isKnownToBeRefused)
            cache->refusedContents.add(*content);
        return std::nullopt;
    };

    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(context.program, context.function, use);
    if (!query || query->receivers.isProven() || query->receivers.expected.predicted().size() < 2)
        return refuse();
    query->maxMembers = context.limit(LimitID::MaxAccessPlanMembers);
    PropertyAccessPlan plan = planPropertyAccess({ context.program, &context.inputs.flow }, *query);
    const MemberPartition& partition = plan.partition;
    if (!plan.isResolved() || partition.arms.size() < 2 || !partition.rest.isEmpty())
        return refuse();
    bool everyArmHoldsData = std::ranges::all_of(partition.arms, [](const MemberArm& arm) {
        return arm.answer.kind == MemberKind::Field || (arm.answer.kind == MemberKind::Accessor && arm.answer.getter);
    });
    bool isRefusedByArms = !everyArmHoldsData || !partition.findArm(MemberKind::Accessor);
    // A limit that is passed is noted for the read.
    if (context.exceeds(LimitID::MaxDispatchCases, partition.arms.size(), reporter.use())) {
        RELEASE_ASSERT(!isKnownToBeRefused);
        return std::nullopt;
    }
    if (isRefusedByArms)
        return refuse();
    RELEASE_ASSERT(!isKnownToBeRefused);
    return Type::forLayouts(query->receivers.expected.predicted().span(), context.types);
}

void reportUnionReceiverTests(TestSiteReporter& reporter)
{
    reporter.forEachCandidate([&](const Value& use) {
        auto reportTestFor = [&](ValueID receiver, const Type& tested) {
            reporter.report({ makeTypeTest(receiver, reporter.types().intern(tested)) }, reporter.analyses().effectsOf(reporter.use()).normal);
        };
        Expected<BlockFrequency> frequency = reporter.function().at(reporter.point().block).frequency;
        if (frequency.predicted() == BlockFrequency::Rare)
            return;
        if (std::optional<Type> receivers = frequency.predicted() == BlockFrequency::Normal ? receiversThatAnswerWithFieldOrGetter(reporter, use) : std::nullopt) {
            reportTestFor(use.objectOperand(), *receivers);
            return;
        }
        ValueID receiver = receiverOfLockedMethodCall(reporter, use);
        if (!receiver && frequency.predicted() != BlockFrequency::Normal)
            return;
        if (!receiver)
            receiver = receiverOfMethodCall(reporter, use);
        if (!receiver)
            receiver = receiverOfAccessorAccess(reporter, use);
        if (!receiver || !reporter.typeOf(receiver).anyObject)
            return;
        std::optional<Type> expected = reporter.expectedPartOf(receiver);
        // No test finds an instance of a class without an instance test.
        if (!expected || *expected != expected->withoutClassProofs())
            return;
        Type testable = expected->withoutClassProofs().withUntestableBuiltinsWidened();
        UnionParts parts = unionPartsOf(testable);
        if (parts.size() >= 2 && parts.size() <= static_cast<size_t>(reporter.context().limit(LimitID::MaxDispatchCases)) && isClosedUnion(parts))
            reportTestFor(receiver, testable);
    });
}

void rewriteTestedUnionReceiver(TestSiteRewriter&)
{
}

bool expectsReceiverOfCalledMethod(FunctionPassContext& context, BlockID block, const Value& call)
{
    const Function& function = context.function;
    if (!context.build.recordedOptions().usesExpectedTypes() || function.at(block).frequency.predicted() != BlockFrequency::Normal)
        return false;

    ValueID read = context.analyses.identityOf(call.calleeOperand());
    const Value& callee = function.at(read);
    PropertyKeyID key = call.calleeKey();
    if (callee.opcode != Opcode::PropGet || callee.key() != key || tellsLayoutsApart(context.program, key) || !callsBuiltinMethod(context, read, key))
        return false;
    ValueID receiver = callee.objectOperand();
    const Type& type = context.program.typeOf(function, receiver);
    if (!type.anyObject || type.layouts || function.at(receiver).expected().predicted() || isAllocatedHere(context, receiver))
        return false;
    return !!receiversWithMethod(context, key);
}

// Of an object that nobody could follow, the names it is asked for are all there is to go by. Which layouts have a name is known, and where they are few the object is expected to be one of them.
bool expectLayoutsByNames(FunctionPassContext& context, ExpectedTypesByUse& expectations)
{
    Vector<NamedUse> uses = collectNamedUsesOfUnknownObjects(context);
    if (uses.isEmpty())
        return false;

    Vector<ValueID> subjects = findSubjects(context);
    for (NamedUse& use : uses)
        use.subject = subjects[use.receiver.index()];
    AOT::stableSort(uses, { }, &NamedUse::subject);
    findUsesOnEveryPath(context, uses.mutableSpan());

    ExpectedValues expectedValues;
    Expected<LayoutList> layoutsMet;
    Vector<std::span<NamedUse>, 4> unsettledSubjects;
    for (std::span<NamedUse> usesOfSubject : uses.mutableSpan() | std::views::chunk_by([](const NamedUse& a, const NamedUse& b) { return a.subject == b.subject; })) {
        if (!expectLayoutsOf(context, usesOfSubject, layoutsMet, true, expectedValues))
            unsettledSubjects.constructAndAppend(usesOfSubject);
    }
    if (!unsettledSubjects.isEmpty())
        addLayoutsOfKnownReceivers(context, layoutsMet);
    for (std::span<NamedUse> usesOfSubject : unsettledSubjects)
        expectLayoutsOf(context, usesOfSubject, layoutsMet, false, expectedValues);
    return expectValues(context, expectedValues, expectations);
}

static Expected<BlockFrequency> frequencyOfGenericCopy(Expected<BlockFrequency> original)
{
    return original.predicted() == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
}

static bool acceptsNarrowedValue(const Function& function, const Use& use)
{
    auto accepts = [](OperandKind kind) {
        return !mustBeDefinition(kind) && kind != OperandKind::Guard;
    };
    switch (use.kind) {
    case Use::Kind::Operand:
        return accepts(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand:
        return accepts(function.at(use.block).terminator.operandKind(use.operandIndex));
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

// Puts the tests of a plan into the function, and beside the code that relies on them a generic copy, which takes over where one fails.
struct TestPlacer {
    using PointKey = std::pair<BlockID, uint32_t>;
    using Definitions = Vector<ValueID, 2>;

    struct Cut {
        PointKey key() const { return { point.block, point.index }; }

        ProgramPoint point;
        BlockID blockBefore;
        BlockID blockAfter;
        bool hasTest { false };
    };

    struct PreparedLoop {
        const PlannedWayBackIntoLoop* planned { nullptr };
        BlockID blockOnEnteringEdge;
        Vector<BlockID, 2> blocksOnBackEdges;
        bool copyComesBack { false };
    };

    struct SubjectOperand {
        uint32_t use { 0 };
        uint32_t operandIndex { 0 };
        uint32_t requirement { 0 };
    };

    struct EmittedTest {
        uint32_t plannedTest { 0 };
        BlockID block;
        BlockID passed;
        BlockID failed;
        ValueID provenWhenPassed;
        ValueID provenWhenFailed;
    };

    FunctionPassContext& context;
    const TestPlacementPlan& plan;
    const ExpectedTypesByUse& expectations;
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    TypeArena& types { context.types };
    IRBuilder builder { analyses.context(), function, types, &analyses };
    bool expectsCheckedTypes { function.has(FunctionFlags::IsChecked) && !isLibraryCode(context.program, function) };
    bool everyTestIsUnpredictable { false };
    std::optional<GenericCopy> copy { };
    GenericCopy::Extent extentOfCopy { GenericCopy::Extent::NotCopied };
    BitVector unclonableStretches { };
    // Of a cold function, as it was before the placer changed it.
    std::optional<BitVector> stretchesInFrontOfLoops { };
    BitVector stretchesThatGuessLayouts { };
    Vector<Cut, 8> cuts { };
    Vector<std::pair<Edge, BlockID>, 4> splitEdges { };
    Vector<PreparedLoop, 2> preparedLoops { };
    BitVector isOnWayBackIntoLoop { };
    Vector<ProgramPoint> rejoinPoints { };
    BitVector isRejoinBlock { };
    Vector<ProgramPoint> rejoinPointsWithoutTests { };
    BitVector isRejoinBlockWithoutTests { };
    Vector<ProgramPoint> splitPoints { };
    BitVector isSplitBlock { };
    Vector<SubjectOperand> subjectOperands { };
    Vector<Definitions> definitions = Vector<Definitions>(plan.conditions->size());
    BitVector isDefinition { };
    Vector<ValueID> adoptedProofs { };
    Vector<EmittedTest> emittedTests { };
    Vector<ValueID> changedValues { };
    Vector<std::pair<ValueID, TypeID>, 4> ownTypesOfRepresentatives { };

    PassOutcome run()
    {
        // In a very large program the copies of the loops of cold functions are megabytes, and no run shows the time that they save. In any other program a kernel that is taken for cold would lose half its speed.
        const Heat& heat = context.inputs.heat;
        if (isCold(heat.of(context.program, context.id)).predicted())
            stretchesInFrontOfLoops = heat.isOfVeryLargeProgram() ? BitVector(plan.stretches.size()) : findStretchesInFrontOfLoops();
        collectSubjectOperands();
        findStretchesThatGuessLayouts();

        auto blocksBeforeSplitting = function.blocks;
        size_t numberOfValuesBeforeSplitting = function.values.size();

        // A proof adopted on an edge that also carries tests has to become a parameter of the block the tests stand in.
        splitTestedEdges();
        adoptExistingProofs();
        collectCuts();
        splitBlocks();
        findRejoinBlocks(rejoinPoints.span(), isRejoinBlock);
        findRejoinBlocks(rejoinPointsWithoutTests.span(), isRejoinBlockWithoutTests);
        findRejoinBlocks(splitPoints.span(), isSplitBlock);
        checkStretches();
        if (!numberOfGuardedStretches()) {
            // Left split, the function would be simplified, planned, split and rejected again in every round of the fixpoint.
            function.replaceBlocks(WTF::move(blocksBeforeSplitting));
            function.editAllValues().shrink(numberOfValuesBeforeSplitting);
            analyses.didChangeControlFlow();
            return PassOutcome::Unchanged;
        }

        cloneGenericCopies();
        redirectSlowPaths();
        emitTests();
        rewriteUses();
        if (copy)
            copy->addWaysBack(changedValues.span(), GenericCopy::TypesTestedOnWayBack::Expected);
        enterCopyBeforeRepeatableCode();
        repairProofs();
        replaceDominatedUses();
        narrowValueTypes(TypePropagationInputs { context.program, context.id, function, analyses, types, context.build, &context.typingMemo }, changedValues.span());
        return PassOutcome::Changed;
    }

    bool isPlaced(unsigned stretch) const { return plan.isGuarded(stretch) && !unclonableStretches.get(stretch); }
    bool shouldClone(const StretchRange& range) const { return range.isCloned && isPlaced(range.stretch); }
    auto clonedRanges() const { return plan.ranges | std::views::filter([this](const StretchRange& range) { return shouldClone(range); }); }
    ValueID useOf(const PlannedTest& test) const { return plan.sites.sites[plan.requirements[test.requirement].site].use; }

    unsigned numberOfGuardedStretches() const
    {
        unsigned numberOfStretches = 0;
        for (unsigned stretch = 0; stretch < plan.stretches.size(); ++stretch)
            numberOfStretches += isPlaced(stretch) && plan.stretches[stretch].numberOfUses;
        return numberOfStretches;
    }

    BitVector findStretchesInFrontOfLoops()
    {
        const CFG& cfg = analyses.cfg();
        BitVector stretches(plan.stretches.size());
        const NaturalLoops* loops = cfg.hasCycle() ? &analyses.naturalLoops() : nullptr;
        if (!loops || !loops->numLoops())
            return stretches;

        BitVector reachesLoop(function.blocks.size());
        Vector<BlockID, 16> worklist;
        for (BlockID block : cfg.reversePostOrder()) {
            if (loops->headerOf(block) && reachesLoop.add(block.index()))
                worklist.append(block);
        }
        while (!worklist.isEmpty()) {
            for (Edge edge : cfg.incomingEdges(worklist.takeLast())) {
                if (reachesLoop.add(edge.from.index()))
                    worklist.append(edge.from);
            }
        }
        for (const StretchRange& range : plan.ranges) {
            if (reachesLoop.get(range.block.index()))
                stretches.quickSet(range.stretch);
        }
        return stretches;
    }

    void collectSubjectOperands()
    {
        const ValueIdentities& identities = analyses.identities();
        for (uint32_t useIndex = 0; useIndex < plan.uses.size(); ++useIndex) {
            const PlannedUse& planned = plan.uses[useIndex];
            const TestSite& site = plan.sites.sites[planned.site];
            const Value& use = function.at(site.use);
            use.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (use.operandKind(operandIndex) == OperandKind::Guard)
                    return;
                ValueID subject = identities.identityOf(operand);
                for (uint32_t testIndex = site.numberOfTests; testIndex--;) {
                    uint32_t requirement = planned.firstRequirement + testIndex;
                    const TestCondition& condition = plan.conditions->at(plan.requirements[requirement].condition);
                    if (!condition.lastsForever() || condition.subject != subject)
                        continue;
                    subjectOperands.append(SubjectOperand { useIndex, operandIndex, requirement });
                    break;
                }
            });
        }
    }

    // Every test of such a stretch looks at a value of which layouts are expected and nobody has checked that: nothing is known of it but the names it is asked for, or the function is not a checked function of the program.
    void findStretchesThatGuessLayouts()
    {
        stretchesThatGuessLayouts.ensureSize(plan.stretches.size());
        BitVector stretchesWithOtherTests(plan.stretches.size());
        for (const PlannedTest& test : plan.tests) {
            const Value& written = plan.sites.tests[plan.requirements[test.requirement].test];
            ValueID tested = !test.operands.isEmpty() ? test.operands[testedOperandIndex] : written.numberOfOperands() ? written.operand(testedOperandIndex) : ValueID();
            bool isGuess = tested && expectations.isGuessedFromNames(tested);
            if (tested && !expectsCheckedTypes) {
                TypeID expected = function.at(tested).expected().predicted();
                isGuess |= expected && types.at(expected).layouts;
            }
            (isGuess ? stretchesThatGuessLayouts : stretchesWithOtherTests).quickSet(test.stretch);
        }
        stretchesThatGuessLayouts.exclude(stretchesWithOtherTests);
    }

    // Behind the stretch the copy keeps apart what the code that passed the test has proven.
    bool goesOnBehindStretch(const StretchRange& range) const
    {
        bool isColdWithoutLoopBehind = stretchesInFrontOfLoops && !stretchesInFrontOfLoops->quickGet(range.stretch);
        return shouldClone(range) && !stretchesThatGuessLayouts.quickGet(range.stretch) && !isColdWithoutLoopBehind;
    }

    bool goesOnBehindSomeStretch() const
    {
        return std::ranges::any_of(plan.ranges, [&](const StretchRange& range) { return goesOnBehindStretch(range); });
    }

    BlockID ensureSplit(Edge edge)
    {
        for (auto [splitEdge, block] : splitEdges) {
            if (splitEdge == edge)
                return block;
        }
        builder.setOrigin(function.at(edge.from).terminator.origin());
        splitEdges.append({ edge, builder.splitEdge(edge) });
        return splitEdges.last().second;
    }

    void splitTestedEdges()
    {
        for (const PlannedTest& test : plan.tests) {
            if (test.position == PlannedTestPosition::OnEdge && plan.isGuarded(test.stretch))
                ensureSplit(test.edge);
        }
        // The generic copy of a turn of a loop leaves the copy on an edge that goes around the loop, where a block is put for it, and goes on from there to the block on an edge that enters the loop.
        for (const PlannedWayBackIntoLoop& planned : plan.waysBackIntoLoops) {
            if (!plan.isGuarded(planned.stretch))
                continue;
            PreparedLoop loop { &planned, ensureSplit(planned.enteringEdge), { }, false };
            passArgumentsThrough(planned.enteringEdge, loop.blockOnEnteringEdge);
            for (Edge edge : planned.backEdges)
                loop.blocksOnBackEdges.append(ensureSplit(edge));
            preparedLoops.append(WTF::move(loop));
        }
    }

    // What the block passes on becomes its parameters, so that another edge into it can pass other values.
    void passArgumentsThrough(Edge edge, BlockID blockOnEdge)
    {
        Target next = function.at(blockOnEdge).terminator.next();
        RELEASE_ASSERT(function.at(blockOnEdge).parameters.isEmpty());

        builder.setOrigin(function.at(blockOnEdge).terminator.origin());
        Arguments parameters;
        for (ValueID parameter : Vector<ValueID, 4>(function.at(next.block).parameters.span())) {
            ValueID added = builder.addParameter(blockOnEdge, function.at(parameter).type);
            function.edit(added, ValueEdit::Type).setExpected(function.at(parameter).expected());
            parameters.append(added);
        }
        builder.redirectEdge(edge, blockOnEdge, WTF::move(next.arguments));
        builder.redirectEdge(Edge { blockOnEdge, 0 }, next.block, WTF::move(parameters));
    }

    void adoptExistingProofs()
    {
        adoptedProofs.reserveInitialCapacity(plan.proofs.size());
        for (const ExistingProof& proof : plan.proofs) {
            switch (proof.source) {
            case ProofSource::TakenEdge:
            case ProofSource::SwitchCase: {
                BlockID targetBefore = targetOf(function, proof.edge).block;
                adoptedProofs.append(ensureEdgeResult(builder, analyses, proof.edge, testedOperandIndex));
                BlockID target = targetOf(function, proof.edge).block;
                if (target != targetBefore)
                    splitEdges.append({ proof.edge, target });
                break;
            }
            case ProofSource::GuardValue:
            case ProofSource::Allocation:
                adoptedProofs.append(proof.value);
                break;
            }
        }
    }

    void collectCuts()
    {
        auto standsBeforeUse = [&](const PlannedTest& test) { return test.position == PlannedTestPosition::BeforeUse && plan.isGuarded(test.stretch); };
        auto addCut = [&](ProgramPoint point) { cuts.append(Cut { point, BlockID(), BlockID() }); };
        for (const PlannedTest& test : plan.tests) {
            if (standsBeforeUse(test))
                addCut(test.point);
        }
        // A copy that holds its stretches alone gets to no place where it could come back. Where tests stand at such a place, they still get a block of their own, which the copy of the range in front of them leads to.
        bool goesOnBehindStretches = goesOnBehindSomeStretch();
        Vector<PointKey, 8> pointsOfTests;
        if (!goesOnBehindStretches) {
            for (const Cut& cut : cuts)
                pointsOfTests.append(cut.key());
            AOT::sort(pointsOfTests);
        }
        for (const StretchRange& range : clonedRanges()) {
            if (range.begin)
                addCut({ range.block, range.begin });
            if (range.end < function.at(range.block).values.size())
                addCut({ range.block, range.end });
        }

        // No copy gets to any other place where it could come back.
        BitVector isBehindClonedRange = findBlocksBehindRanges(function, plan.ranges.span(), [&](const StretchRange& range) { return shouldClone(range); });
        auto keepRejoinPoints = [&](const Vector<ProgramPoint>& planned, Vector<ProgramPoint>& kept) {
            for (ProgramPoint point : planned) {
                if (!isBehindClonedRange.get(point.block.index()) || (!goesOnBehindStretches && !std::ranges::binary_search(pointsOfTests, PointKey { point.block, point.index })))
                    continue;
                kept.append(point);
                if (point.index)
                    addCut(point);
            }
        };
        keepRejoinPoints(plan.rejoinPoints, rejoinPoints);
        keepRejoinPoints(plan.rejoinPointsWithoutTests, rejoinPointsWithoutTests);
        keepRejoinPoints(plan.splitPoints, splitPoints);

        AOT::sort(cuts, { }, &Cut::key);
        removeRepeatedElements(cuts, [](const Cut& a, const Cut& b) { return a.point == b.point; });
        for (const PlannedTest& test : plan.tests) {
            if (standsBeforeUse(test))
                findCut(test.point)->hasTest = true;
        }
    }

    void splitBlocks()
    {
        Vector<uint32_t, 8> indexes;
        for (size_t next = 0; next < cuts.size();) {
            BlockID block = cuts[next].point.block;
            indexes.shrink(0);
            for (size_t i = next; i < cuts.size() && cuts[i].point.block == block; ++i)
                indexes.append(cuts[i].point.index);

            builder.setOrigin(function.at(function.at(block).values[indexes.first()]).origin());
            BlockID previous = block;
            for (BlockID part : builder.splitBlock(block, indexes.span())) {
                Cut& cut = cuts[next++];
                cut.blockBefore = previous;
                cut.blockAfter = part;
                previous = part;
            }
        }
    }

    Cut* findCut(ProgramPoint point)
    {
        auto found = std::ranges::lower_bound(cuts, PointKey { point.block, point.index }, { }, &Cut::key);
        return found != cuts.end() && found->point == point ? &*found : nullptr;
    }

    const Cut* findLastCut(BlockID block, uint32_t index = std::numeric_limits<uint32_t>::max()) const
    {
        auto next = std::ranges::upper_bound(cuts, PointKey { block, index }, { }, &Cut::key);
        return next != cuts.begin() && std::prev(next)->point.block == block ? &*std::prev(next) : nullptr;
    }

    BlockID lastPartOf(BlockID block) const
    {
        const Cut* last = findLastCut(block);
        return last ? last->blockAfter : block;
    }

    Edge behindCuts(Edge edge) const { return { lastPartOf(edge.from), edge.targetIndex }; }

    // The copy comes back in front of the tests that stand at the point, which get a block of their own.
    void findRejoinBlocks(std::span<const ProgramPoint> points, BitVector& rejoinBlocks)
    {
        for (ProgramPoint point : points) {
            if (!point.index) {
                rejoinBlocks.set(point.block.index());
                continue;
            }
            Cut* cut = findCut(point);
            RELEASE_ASSERT(cut);
            if (cut->hasTest) {
                builder.setOrigin(function.at(cut->blockBefore).terminator.origin());
                cut->blockBefore = builder.splitEdge(Edge { cut->blockBefore, 0 });
            }
            rejoinBlocks.set((cut->hasTest ? cut->blockBefore : cut->blockAfter).index());
        }
    }

    BlockID blockOf(const StretchRange& range)
    {
        if (range.begin == range.end)
            return range.block;
        const Cut* cut = findCut({ range.block, range.begin });
        RELEASE_ASSERT(cut || !range.begin);
        return cut ? cut->blockAfter : range.block;
    }

    BlockID guardedBlockOf(const PlannedTest& test)
    {
        if (test.position == PlannedTestPosition::OnEdge)
            return function.at(ensureSplit(test.edge)).terminator.next().block;
        const Cut* cut = findCut(test.point);
        RELEASE_ASSERT(cut);
        return cut->blockAfter;
    }

    void forEachEntryOfCopy(NOESCAPE const Invocable<void(unsigned stretch, BlockID)> auto& functor)
    {
        BitVector isEntry(function.blocks.size());
        for (const PlannedTest& test : plan.tests) {
            if (BlockID entry = isPlaced(test.stretch) ? guardedBlockOf(test) : BlockID(); entry && isEntry.add(entry.index()))
                functor(test.stretch, entry);
        }
        for (const PlannedSlowPath& slowPath : plan.slowPaths) {
            if (BlockID entry = targetOf(function, behindCuts(slowPath.edge)).block; isPlaced(slowPath.stretch) && isEntry.add(entry.index()))
                functor(slowPath.stretch, entry);
        }
    }

    void checkStretches()
    {
        struct StretchRegion {
            Vector<BlockID, 8> blocks;
            Vector<BlockID, 2> entries;
        };
        Vector<StretchRegion> regions(plan.stretches.size());
        for (const StretchRange& range : clonedRanges())
            regions[range.stretch].blocks.append(blockOf(range));
        if (!splitEdges.isEmpty()) {
            using BlockAndStretch = std::pair<uint32_t, unsigned>;
            Vector<BlockAndStretch> stretchesByBlock;
            for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
                for (BlockID block : regions[stretch].blocks)
                    stretchesByBlock.append({ block.index(), stretch });
            }
            AOT::sort(stretchesByBlock);
            removeRepeatedElements(stretchesByBlock);
            auto stretchesOf = [&](BlockID block) { return std::ranges::equal_range(stretchesByBlock, block.index(), { }, &BlockAndStretch::first); };
            for (auto [edge, blockOnEdge] : splitEdges) {
                auto stretchesOfTarget = stretchesOf(function.at(blockOnEdge).terminator.next().block);
                if (stretchesOfTarget.empty())
                    continue;
                for (auto [block, stretch] : stretchesOf(lastPartOf(edge.from))) {
                    if (std::ranges::binary_search(stretchesOfTarget, stretch, { }, &BlockAndStretch::second))
                        regions[stretch].blocks.append(blockOnEdge);
                }
            }
        }
        forEachEntryOfCopy([&](unsigned stretch, BlockID entry) { regions[stretch].entries.append(entry); });

        // A copy that goes on behind a stretch asks what is live in the rest of the function. One liveness serves every stretch and the copy of them all, which may hold any block on a split edge.
        bool goesOnBehindStretches = goesOnBehindSomeStretch();
        std::unique_ptr<Liveness> livenessInsideStretches;
        auto liveness = [&]() -> const Liveness& {
            if (goesOnBehindStretches)
                return analyses.liveness();
            if (!livenessInsideStretches) {
                Vector<BlockID, 16> blocks;
                Vector<BlockID, 16> entries;
                for (const StretchRegion& region : regions) {
                    blocks.appendVector(region.blocks);
                    entries.appendVector(region.entries);
                }
                for (auto [edge, blockOnEdge] : splitEdges)
                    blocks.append(blockOnEdge);
                livenessInsideStretches = RegionCloner::computeLivenessInside(analyses, blocks.span(), entries.span());
            }
            return *livenessInsideStretches;
        };
        for (unsigned stretch = 0; stretch < regions.size(); ++stretch) {
            const StretchRegion& region = regions[stretch];
            if (region.blocks.isEmpty())
                continue;
            CloneCheck check = RegionCloner::checkCanDuplicate(analyses, region.blocks.span(), region.entries.span(), liveness);
            if (check != CloneCheck::CanClone && check != CloneCheck::FunctionMayNotBeCloned)
                unclonableStretches.set(stretch);
        }
        extendCopiesBehindStretches(liveness);
        if (unclonableStretches.isEmpty())
            return;

        BitVector isDeclined(plan.stretches.size());
        for (const PlannedUse& use : plan.uses) {
            if (unclonableStretches.get(use.stretch) && isDeclined.add(use.stretch))
                context.addDecline(Wire::DeclineKind::StretchNotGuarded, plan.sites.sites[use.site].use);
        }
    }

    // Where a test fails, the copy goes on behind the stretch, so nothing joins the code behind the test that passed until that code has no proof left to lose.
    void extendCopiesBehindStretches(const RegionCloner::LivenessSource& liveness)
    {
        Vector<BlockID, 8> blocksOfStretches;
        for (const StretchRange& range : clonedRanges())
            blocksOfStretches.append(blockOf(range));
        if (blocksOfStretches.isEmpty())
            return;

        for (PreparedLoop& loop : preparedLoops) {
            // A proof adopted on an edge is a parameter that the copy has nothing to pass for.
            loop.copyComesBack = isPlaced(loop.planned->stretch) && function.at(loop.blockOnEnteringEdge).parameters.size() == function.at(loop.planned->head).parameters.size()
                && std::ranges::all_of(loop.blocksOnBackEdges, [&](BlockID block) { return function.at(block).parameters.isEmpty(); });
            if (!loop.copyComesBack)
                continue;
            for (BlockID block : loop.blocksOnBackEdges)
                isOnWayBackIntoLoop.set(block.index());
        }
        extentOfCopy = planGenericCopy(blocksOfStretches.span(), liveness);

        // A copy that comes back nowhere holds the blocks on the edges that go around its loops.
        if (extentOfCopy != GenericCopy::Extent::ToWaysBack && !isOnWayBackIntoLoop.isEmpty()) {
            for (PreparedLoop& loop : preparedLoops)
                loop.copyComesBack = false;
            isOnWayBackIntoLoop.clearAll();
            extentOfCopy = planGenericCopy(blocksOfStretches.span(), liveness);
        }
        if (copy->wasCutShortByLimit())
            context.gaveUp(LimitID::MaxBlocksCopiedBehindStretches);
        if (extentOfCopy != GenericCopy::Extent::NotCopied)
            return;

        copy.reset();
        for (const StretchRange& range : plan.ranges) {
            if (shouldClone(range))
                unclonableStretches.set(range.stretch);
        }
    }

    GenericCopy::Extent planGenericCopy(std::span<const BlockID> blocksOfStretches, const RegionCloner::LivenessSource& liveness)
    {
        copy.emplace(context, builder);
        copy->leaveAtRarerRejoinBlocks();
        copy->testOnlyWaysBackIntoLoops();

        BitVector isInRegion(function.blocks.size());
        for (BlockID block : blocksOfStretches) {
            isInRegion.quickSet(block.index());
            copy->addCopiedBlock(block);
        }
        // Left out, the block on a split edge would lead the generic copy through the tests on the edge into code that relies on proofs the copy does not have.
        for (auto [edge, blockOnEdge] : splitEdges) {
            if (!isOnWayBackIntoLoop.get(blockOnEdge.index()) && isInRegion.quickGet(lastPartOf(edge.from).index()) && isInRegion.quickGet(function.at(blockOnEdge).terminator.next().block.index()))
                copy->addCopiedBlock(blockOnEdge);
        }
        bool goesOnBehindStretches = false;
        for (const StretchRange& range : plan.ranges) {
            if (!goesOnBehindStretch(range))
                continue;
            copy->addStart(blockOf(range));
            goesOnBehindStretches = true;
        }
        forEachEntryOfCopy([&](unsigned, BlockID entry) { copy->addEntry(entry); });
        for (const PlannedUse& use : plan.uses) {
            if (isPlaced(use.stretch))
                copy->addValueToBeNarrowed(plan.sites.sites[use.site].use);
        }
        for (size_t index : isSplitBlock)
            copy->addRequiredRejoinBlock(BlockID(index));
        // A copy that holds its stretches alone leaves where they end: it looks for no place to come back at.
        if (!goesOnBehindStretches)
            return copy->plan(context.limit(LimitID::MaxBlocksCopiedBehindStretches), liveness);

        for (size_t index : isRejoinBlock)
            copy->addRejoinBlock(BlockID(index));
        for (size_t index : isOnWayBackIntoLoop)
            copy->addRejoinBlock(BlockID(index));
        for (size_t index : isRejoinBlockWithoutTests)
            copy->addRejoinBlockWithoutTests(BlockID(index));

        for (const PlannedUse& use : plan.uses) {
            const TestSite& site = plan.sites.sites[use.site];
            if (isPlaced(use.stretch) && !expectedTypeTellsResultOf(context, site))
                copy->addValueNarrowedInUnknownWay(site.use);
        }
        for (ValueID subject : collectSubjectsKnownBeyondTheirTypes(context, plan))
            copy->addValueKnownBeyondItsType(subject);
        return copy->plan(context.limit(LimitID::MaxBlocksCopiedBehindStretches));
    }

    void cloneGenericCopies()
    {
        if (!copy)
            return;
        // In a checked function of the program the type that a test expects is what the checker says. Elsewhere, and where the checker names no layout, it is a guess, and the copy runs.
        bool expectsWhatCheckerSays = expectsCheckedTypes && std::ranges::none_of(clonedRanges(), [&](const StretchRange& range) { return stretchesThatGuessLayouts.quickGet(range.stretch); });
        everyTestIsUnpredictable = std::ranges::all_of(plan.uses, [&](const PlannedUse& use) {
            return !isPlaced(use.stretch) || plan.sites.sites[use.site].outcome == TestOutcome::IsUnpredictable;
        });
        if (everyTestIsUnpredictable)
            copy->clone(+[](Expected<BlockFrequency> original) { return original; });
        else
            copy->clone(expectsWhatCheckerSays ? +[](Expected<BlockFrequency>) -> Expected<BlockFrequency> { return BlockFrequency::Rare; } : frequencyOfGenericCopy);

        RELEASE_ASSERT(extentOfCopy != GenericCopy::Extent::NotCopied);
        if (extentOfCopy == GenericCopy::Extent::ToWaysBack)
            context.build.statistics().count(StatisticID::StretchCopiesToWaysBack);
        else if (extentOfCopy == GenericCopy::Extent::ToEndOfFunction)
            context.build.statistics().count(StatisticID::StretchCopiesToEndOfFunction);

        for (const PreparedLoop& loop : preparedLoops) {
            if (loop.copyComesBack)
                addWayBackIntoLoop(loop);
        }
    }

    // Every proof that the head of the loop relies on is made again on the way back in: here, or by the tests on the entering edge that the way back joins.
    void addWayBackIntoLoop(const PreparedLoop& loop)
    {
        const PlannedWayBackIntoLoop& planned = *loop.planned;
        BlockID headOfCopy = copy->copyOf(planned.head);
        RELEASE_ASSERT(headOfCopy);

        // A test of a value that has a copy would be about another value than the test it repeats.
        bool canRepeatTests = std::ranges::none_of(planned.tests, [&](const PlannedTest& test) {
            return std::ranges::any_of(test.operands, [&](ValueID operand) { return !!copy->copyOf(operand); });
        });

        builder.setHandler(std::nullopt);
        for (uint32_t index = 0; index < planned.backEdges.size(); ++index) {
            BlockID block = loop.blocksOnBackEdges[index];
            Target next = function.at(block).terminator.next();
            RELEASE_ASSERT(next.block == planned.head);

            // The tested code goes around the loop past the block, so whatever enters the block comes out of the copy.
            builder.redirectEdge(behindCuts(planned.backEdges[index]), planned.head, Arguments(next.arguments));

            BlockID copyOfBlock = copy->copyOf(block);
            Target nextInCopy = copyOfBlock ? Target { copyOfBlock, { }, { } } : Target { headOfCopy, next.arguments, { } };

            builder.setOrigin(function.at(block).terminator.origin());
            Block& onBackEdge = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
            onBackEdge.frequency = function.at(headOfCopy).frequency;
            onBackEdge.terminator = Terminator { };
            builder.moveToEndOf(block);
            if (!canRepeatTests) {
                builder.emitJump(nextInCopy.block, WTF::move(nextInCopy.arguments));
                continue;
            }
            auto goOnWherePassed = [&](ValueID test) {
                Expected<BlockFrequency> frequency = function.at(builder.currentBlock()).frequency;
                IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, frequency, frequency);
                builder.moveToEndOf(branch.notTaken);
                builder.emitJump(nextInCopy.block, Arguments(nextInCopy.arguments));
                builder.moveToEndOf(branch.taken);
                return branch.provenOnTaken;
            };
            for (const PlannedTest& test : planned.tests) {
                builder.setOrigin(function.at(useOf(test)).origin());
                addDefinition(test.condition, goOnWherePassed(builder.append(createTest(test))));
            }
            for (uint32_t parameterIndex = 0; parameterIndex < planned.typesOfCounters.size(); ++parameterIndex) {
                if (TypeID type = planned.typesOfCounters[parameterIndex])
                    next.arguments[parameterIndex] = goOnWherePassed(builder.appendTypeTest(next.arguments[parameterIndex], type));
            }
            builder.emitJump(loop.blockOnEnteringEdge, WTF::move(next.arguments));
        }
    }

    void redirectSlowPaths()
    {
        for (const PlannedSlowPath& slowPath : plan.slowPaths) {
            if (!isPlaced(slowPath.stretch))
                continue;
            Edge edge = behindCuts(slowPath.edge);
            Target target = targetOf(function, edge);
            target.block = copy->copyOf(target.block);
            RELEASE_ASSERT(target.block);
            builder.redirectEdge(edge, WTF::move(target));
        }
    }

    void defineAdoptedProofs()
    {
        if (adoptedProofs.isEmpty())
            return;
        // An insertion ends the life of the use lists.
        const UseLists& useLists = analyses.useLists();
        Vector<ProgramPoint> pointsAfter(adoptedProofs.size(), [&](size_t index) { return useLists.pointAfter(adoptedProofs[index]); });

        for (uint32_t index = 0; index < adoptedProofs.size(); ++index) {
            TestConditionID condition = plan.proofs[index].condition;
            ValueID definition = adoptedProofs[index];
            if (definitions[condition.index()].contains(definition))
                continue;
            if (isDefinition.get(definition.index())) {
                builder.setOrigin(function.at(definition).origin());
                definition = builder.insertBefore(pointsAfter[index], IRBuilder::make(Opcode::AllGuards, { definition }));
            }
            addDefinition(condition, definition);
        }
        builder.applyInsertions();
    }

    void emitTests()
    {
        defineAdoptedProofs();

        using TestsAtOnePosition = Vector<uint32_t, 4>;
        Vector<TestsAtOnePosition, 8> waiting;
        for (uint32_t index = 0; index < plan.tests.size(); ++index) {
            const PlannedTest& test = plan.tests[index];
            if (!isPlaced(test.stretch))
                continue;
            const PlannedTest* previous = waiting.isEmpty() ? nullptr : &plan.tests[waiting.last().last()];
            if (!previous || previous->position != test.position || previous->point != test.point || previous->edge != test.edge)
                waiting.append(TestsAtOnePosition { });
            waiting.last().append(index);
        }

        // Where a slow path comes into a loop behind its head, the order of the blocks has the head behind the body, which rests on the tests of the head.
        // Every way through the tested code makes a proof in front of the tests that name it, so the first of the waiting tests on such a way waits for none of the others: each turn emits some.
        while (!waiting.isEmpty()) {
            unsigned numberOfPositionsEmitted = waiting.removeAllMatching([&](const TestsAtOnePosition& tests) {
                if (!hasProofsNamedBy(tests.span()))
                    return false;
                emitTestsAtOnePosition(tests.span());
                return true;
            });
            RELEASE_ASSERT(numberOfPositionsEmitted);
        }
    }

    bool hasProofsNamedBy(std::span<const uint32_t> testsAtOnePosition) const
    {
        bool hasProofs = true;
        for (size_t index = 0; index < testsAtOnePosition.size(); ++index) {
            forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, plan.tests[testsAtOnePosition[index]].requirement, [&](const TestPrerequisite& prerequisite) {
                hasProofs = hasProofs && (!definitions[prerequisite.condition.index()].isEmpty() || std::ranges::any_of(testsAtOnePosition.first(index), [&](uint32_t earlier) {
                    return plan.tests[earlier].condition == prerequisite.condition;
                }));
            });
        }
        return hasProofs;
    }

    void emitTestsAtOnePosition(std::span<const uint32_t> plannedTests)
    {
        RELEASE_ASSERT(copy);
        const PlannedTest& first = plan.tests[plannedTests.front()];
        // Then that block takes the proof of the last test itself.
        bool isOnlyWayToPassedTarget = first.position == PlannedTestPosition::BeforeUse;
        BlockID block;
        Target passedTarget;
        if (isOnlyWayToPassedTarget) {
            const Cut* cut = findCut(first.point);
            RELEASE_ASSERT(cut);
            block = cut->blockBefore;
            passedTarget.block = cut->blockAfter;
        } else {
            block = ensureSplit(first.edge);
            passedTarget = function.at(block).terminator.next();
        }
        Target failedTarget { copy->copyOf(passedTarget.block), passedTarget.arguments, { } };
        Expected<BlockFrequency> frequency = function.at(passedTarget.block).frequency;
        RELEASE_ASSERT(failedTarget.block);

        Terminator& jump = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        RELEASE_ASSERT(jump.kind == TerminatorKind::Jump && jump.next().block == passedTarget.block);
        jump = Terminator { };

        builder.setHandler(std::nullopt);
        builder.moveToEndOf(block);
        builder.emitFailureOfPredictions(Target(failedTarget));
        for (const uint32_t& plannedTest : plannedTests) {
            const PlannedTest& planned = plan.tests[plannedTest];
            builder.setOrigin(function.at(useOf(planned)).origin());
            ValueID test = builder.append(createTest(planned));
            BlockID blockOfTest = builder.currentBlock();

            // No block is made that only jumps: SimplifyControlFlow would merge it away in the same round, and every pass in between would walk it.
            LayoutID functionWhoseGuardsBegin = constructorFunctionAskedAbout(context.program, function.at(test));
            bool provesNothingWhenFailed = provesForStretch(provesOf(function.at(test).opcode)) && !functionWhoseGuardsBegin;
            bool entersPassedTarget = &plannedTest == &plannedTests.back() && isOnlyWayToPassedTarget;
            IRBuilder::ProvenBranchTargets targets;
            if (entersPassedTarget)
                targets.enteredOnlyWhenTaken = passedTarget.block;
            if (provesNothingWhenFailed)
                targets.whenNotTaken = failedTarget;
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, frequency, everyTestIsUnpredictable ? frequency : frequencyOfGenericCopy(frequency), WTF::move(targets));
            if (!provesNothingWhenFailed) {
                builder.moveToEndOf(branch.notTaken);
                if (functionWhoseGuardsBegin)
                    appendStartOfMemberGuards(builder, context.program, context.types, functionWhoseGuardsBegin);
                builder.emitJump(failedTarget.block, Arguments(failedTarget.arguments));
            }
            if (!entersPassedTarget)
                builder.moveToEndOf(branch.taken);

            emittedTests.append(EmittedTest { plannedTest, blockOfTest, branch.taken, branch.notTaken, branch.provenOnTaken, branch.provenOnNotTaken });
            addDefinition(planned.condition, branch.provenOnTaken);
        }
        if (!isOnlyWayToPassedTarget)
            builder.emitJump(passedTarget.block, WTF::move(passedTarget.arguments));
    }

    // The test names the proofs of its site that it rests on: what has passed an earlier test in place of an operand. Answers those that it needs as guards.
    Vector<ValueID, 4> namePrerequisites(Value& test, uint32_t requirement, NOESCAPE const Invocable<ValueID(Value&&)> auto& emit)
    {
        Vector<ValueID, 4> guards;
        forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, requirement, [&](const TestPrerequisite& prerequisite) {
            ValueID proof = representativeOf(prerequisite.condition);
            if (!prerequisite.narrowedOperand) {
                guards.appendIfNotContains(proof);
                return;
            }
            if (std::optional<TypeID> castType = findTypeOfOperand(test, *prerequisite.narrowedOperand, proof))
                proof = emit(makeAssumeType(proof, emit(IRBuilder::make(Opcode::Specified)), Proven::behindFact(*castType)));
            test.setOperand(*prerequisite.narrowedOperand, proof);
        });

        // The site was planned with the types of its own operands. The value that stands for an operand here may be the one in front of a cast, of which less is known.
        if (Value::opcodesWithKeyIsIntegral.contains(test.opcode) && test.keyIsIntegral()) {
            ValueID key = OperandPosition::property(test.opcode) ? test.propertyOperand() : test.positionOperand();
            if (!context.program.typeOf(function, key).hasOnlyPrimitives(Primitive::NumberWithoutFraction))
                test.setKeyIsIntegral(false);
        }
        return guards;
    }

    Value createTest(const PlannedTest& planned)
    {
        RELEASE_ASSERT(planned.condition == plan.requirements[planned.requirement].condition);
        Value test = plan.sites.tests[plan.requirements[planned.requirement].test];
        std::optional<unsigned> guardIndex = OperandPosition::guard(test.opcode);

        if (planned.position == PlannedTestPosition::OnEdge) {
            uint32_t numberOfHoistedOperands = 0;
            for (uint32_t operandIndex = 0; operandIndex < test.numberOfOperands(); ++operandIndex) {
                if (guardIndex != operandIndex)
                    test.setOperand(operandIndex, planned.operands[numberOfHoistedOperands++]);
            }
        }
        Vector<ValueID, 4> guards = namePrerequisites(test, planned.requirement, [&](Value&& cast) { return builder.append(WTF::move(cast)); });
        if (!guards.isEmpty())
            test.setOperand(*guardIndex, builder.appendAllGuards(guards.span()));
        return test;
    }

    // An operand that has to be an integer was one at the site: by its own type and the test that it passed. The value that passed the test may be the one in front of a cast.
    std::optional<TypeID> findTypeOfOperand(const Value& test, uint32_t operandIndex, ValueID passed) const
    {
        const Type& passedType = types.at(function.at(passed).type);
        const Type& operandType = types.at(function.at(test.operand(operandIndex)).type);
        bool hasToBeInteger = test.operandKind(operandIndex) == OperandKind::Integer;
        if (!hasToBeInteger && operandIndex != testedOperandIndex)
            return std::nullopt;
        if (hasToBeInteger ? passedType.hasOnlyPrimitives(Primitive::Integer) : passedType.isSubsetOf(operandType, context.program))
            return std::nullopt;
        Type both = operandType.intersectWith(passedType, types);
        if (both.isNever() || both == passedType || (hasToBeInteger && !both.hasOnlyPrimitives(Primitive::Integer)))
            return std::nullopt;
        return types.intern(both);
    }

    void addDefinition(TestConditionID condition, ValueID proof)
    {
        Definitions& proofs = definitions[condition.index()];
        if (proofs.contains(proof))
            return;
        if (isDefinition.get(proof.index()))
            proof = builder.append(Opcode::AllGuards, { proof });
        isDefinition.set(proof.index());
        proofs.append(proof);
    }

    ValueID representativeOf(TestConditionID condition) const
    {
        RELEASE_ASSERT(!definitions[condition.index()].isEmpty());
        return definitions[condition.index()].first();
    }

    // SSARepair redirects every use of a definition, and an adopted proof has other uses: an allocation is also the object, and what had passed another test before is read as both tests say.
    void wrapAdoptedDefinitions()
    {
        struct AdoptedDefinition {
            ValueID* definition;
            ProgramPoint pointAfter;
            bool isNamedAsGuard;
        };

        if (adoptedProofs.isEmpty())
            return;
        const UseLists& useLists = analyses.useLists();
        BitVector isAdopted(useLists.numberOfValues());
        for (ValueID proof : adoptedProofs) {
            RELEASE_ASSERT(proof.index() < useLists.numberOfValues());
            isAdopted.quickSet(proof.index());
        }
        // Where the proofs of a condition meet, the value has the type of all of them.
        auto saysMoreThanWhereProofsMeet = [&](ValueID proof, const Definitions& proofs) {
            TypeID typeOfAll = function.at(proof).type;
            for (ValueID other : proofs)
                typeOfAll = types.unionOf(typeOfAll, function.at(other).type);
            return typeOfAll != function.at(proof).type;
        };
        Vector<AdoptedDefinition, 4> adoptedDefinitions;
        for (size_t condition = 0; condition < definitions.size(); ++condition) {
            Definitions& proofs = definitions[condition];
            if (proofs.size() < 2)
                continue;
            bool isNamedAsGuard = plan.conditions->stretchConditions().get(condition);
            for (ValueID& proof : proofs) {
                if (isAdopted.get(proof.index()) && (isNamedAsGuard || saysMoreThanWhereProofsMeet(proof, proofs)))
                    adoptedDefinitions.append({ &proof, useLists.pointAfter(proof), isNamedAsGuard });
            }
        }
        if (adoptedDefinitions.isEmpty())
            return;

        Vector<ValueID> wrappers(function.values.size());
        for (auto [definition, pointAfter, isNamedAsGuard] : adoptedDefinitions) {
            RELEASE_ASSERT(!wrappers[definition->index()]);
            builder.setOrigin(function.at(*definition).origin());
            Value wrapper = IRBuilder::make(isNamedAsGuard ? Opcode::AllGuards : Opcode::Upcast, { *definition });
            if (!isNamedAsGuard)
                wrapper.setCastType(function.at(*definition).type);
            wrappers[definition->index()] = builder.insertBefore(pointAfter, WTF::move(wrapper));
            *definition = wrappers[definition->index()];
            isDefinition.set(definition->index());
        }
        auto wrapperOf = [&](ValueID value) { return value.index() < wrappers.size() ? wrappers[value.index()] : ValueID(); };
        auto redirectGuardOperandsOf = [&](ValueID user) {
            Value& value = function.edit(user, ValueEdit::Definition);
            value.forEachOperand([&](ValueID& operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Guard && wrapperOf(operand))
                    operand = wrapperOf(operand);
            });
        };
        for (const EmittedTest& emitted : emittedTests) {
            ValueID test = function.at(emitted.block).terminator.conditionOperand();
            const Value& value = function.at(test);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Guard && !isDefinition.get(operand.index()))
                    redirectGuardOperandsOf(operand);
            });
            redirectGuardOperandsOf(test);
            // See namePrerequisites().
            forEachTestPrerequisite(plan.requirements.span(), *plan.conditions, plan.tests[emitted.plannedTest].requirement, [&](const TestPrerequisite& prerequisite) {
                if (!prerequisite.narrowedOperand)
                    return;
                ValueID named = function.at(test).operand(*prerequisite.narrowedOperand);
                if (ValueID wrapper = wrapperOf(named))
                    function.edit(test, ValueEdit::Definition).setOperand(*prerequisite.narrowedOperand, wrapper);
                else if (function.at(named).opcode == Opcode::AssumeType && wrapperOf(function.at(named).valueOperand()))
                    function.edit(named, ValueEdit::Definition).setValueOperand(wrapperOf(function.at(named).valueOperand()));
            });
        }
    }

    void rewriteUses()
    {
        // Where a use stands now follows from where it stood and from the cuts: the lists of all uses of the function are not made for that.
        Vector<ProgramPoint> usePoints(plan.uses.size(), [&](size_t index) {
            const TestSite& site = plan.sites.sites[plan.uses[index].site];
            const Cut* cut = findLastCut(site.point.block, site.point.index);
            ProgramPoint point = cut ? ProgramPoint { cut->blockAfter, site.point.index - cut->point.index } : site.point;
            const BlockValues& values = function.at(point.block).values;
            if (point.index < values.size() && values[point.index] == site.use)
                return point;
            ValueLocation location = analyses.useLists().locationOf(site.use);
            RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
            return ProgramPoint { location.block, location.index };
        });
        wrapAdoptedDefinitions();

        for (const Definitions& proofs : definitions) {
            if (proofs.size() < 2)
                continue;
            TypeID typeOfFirst = function.at(proofs.first()).type;
            if (std::ranges::all_of(proofs, [&](ValueID proof) { return function.at(proof).type == typeOfFirst; }))
                continue;
            Type typeOfAll = types.at(typeOfFirst);
            for (ValueID proof : proofs.subspan(1))
                typeOfAll = typeOfAll.unionWith(types.at(function.at(proof).type), types);
            ownTypesOfRepresentatives.append({ proofs.first(), typeOfFirst });
            function.edit(proofs.first(), ValueEdit::Type).type = types.intern(typeOfAll);
        }
        for (uint32_t useIndex = 0; useIndex < plan.uses.size(); ++useIndex) {
            if (isPlaced(plan.uses[useIndex].stretch))
                rewriteUse(plan.uses[useIndex], useIndex, usePoints[useIndex]);
        }
        for (auto [representative, type] : ownTypesOfRepresentatives)
            function.edit(representative, ValueEdit::Type).type = type;
        builder.applyInsertions();
        // The uses and the guard operands of the tests were written past the builder.
        analyses.didChangeValues();
    }

    // One value stands for everything that has the identity of the tested value. The site was planned with the type of its own operand, which a branch or a cast may have narrowed in another way than the test.
    ValueID narrowToOperand(ValueID passed, ValueID operand, ProgramPoint point)
    {
        Type passedType = types.at(function.at(passed).type);
        Type operandType = types.at(function.at(operand).type);
        if (passedType.isSubsetOf(operandType, context.program))
            return passed;
        // An intersection names an object as its second type does: as the test did.
        Type both = operandType.intersectWith(passedType, types);
        // Beside a class that no test tells from others that intersection keeps every class that the second type lists. The operand's own type says which of them are left.
        bool listsClassThatOperandRulesOut = !operandType.anyObject && !operandType.anyFunction && std::ranges::any_of(passedType.classes, [&](const ClassPart& part) {
            return !operandType.findClassPart(part.builtinClass);
        });
        if (both == passedType && listsClassThatOperandRulesOut)
            both = passedType.intersectWith(operandType, types);
        if (both.isNever() || both == passedType)
            return passed;

        return castBefore(point, passed, types.intern(both));
    }

    ValueID castBefore(ProgramPoint point, ValueID value, TypeID castType)
    {
        if (context.build.validates())
            builder.insertBefore(point, makeValidateType(value, castType));
        return builder.insertBefore(point, makeAssumeType(value, builder.insertBefore(point, IRBuilder::make(Opcode::Specified)), Proven::behindFact(castType)));
    }

    void rewriteUse(const PlannedUse& planned, uint32_t useIndex, ProgramPoint point)
    {
        const TestSite& site = plan.sites.sites[planned.site];
        auto insert = [&](Value&& value) { return builder.insertBefore(point, WTF::move(value)); };
        auto insertAllGuards = [&](std::span<const ValueID> guards) {
            Value allGuards = IRBuilder::make(Opcode::AllGuards);
            allGuards.setGuardsOperands(guards);
            return insert(WTF::move(allGuards));
        };

        builder.setOrigin(function.at(site.use).origin());
        for (const SubjectOperand& narrowed : std::ranges::equal_range(subjectOperands, useIndex, { }, &SubjectOperand::use)) {
            ValueID operand = function.at(site.use).operand(narrowed.operandIndex);
            ValueID replacement = narrowToOperand(representativeOf(plan.requirements[narrowed.requirement].condition), operand, point);
            if (context.build.validates() && replacement != operand)
                insert(IRBuilder::make(Opcode::ValidateSameValue, { replacement, operand }));
            function.edit(site.use, ValueEdit::Definition).setOperand(narrowed.operandIndex, replacement);
        }

        Vector<ValueID, 4> proofs;
        Vector<ValueID, 4> guards;
        for (const TestRequirement& requirement : plan.requirements.subspan(planned.firstRequirement, site.numberOfTests)) {
            proofs.append(representativeOf(requirement.condition));
            if (plan.conditions->at(requirement.condition).lastsForStretch())
                guards.appendIfNotContains(proofs.last());
        }
        ValueID guard;
        if (guards.size() == 1)
            guard = guards.first();
        else if (!guards.isEmpty())
            guard = insertAllGuards(guards.span());

        // One test stands for every use of its stretch. In a validating build each use makes the tests that last for a stretch again, and their answers are compared with the one it relies on.
        for (uint32_t test = 0; context.build.validates() && test < site.numberOfTests; ++test) {
            uint32_t requirement = planned.firstRequirement + test;
            if (!plan.conditions->at(plan.requirements[requirement].condition).lastsForStretch())
                continue;
            Value repeated = plan.sites.tests[plan.requirements[requirement].test];
            Vector<ValueID, 4> prerequisites = namePrerequisites(repeated, requirement, insert);
            if (!prerequisites.isEmpty())
                repeated.setGuardOperand(insertAllGuards(prerequisites.span()));
            Value expected = IRBuilder::make(Opcode::Boolean);
            expected.setBoolean(true);
            ValueID answer = insert(WTF::move(repeated));
            ValueID expectedAnswer = insert(WTF::move(expected));
            insert(IRBuilder::make(Opcode::ValidateSameValue, { answer, expectedAnswer }));
        }

        changedValues.append(site.use);
        TestSiteRewriter rewriter(TestSiteRewriter::Inputs { context, builder, site, plan.sites.testsOf(site), proofs.span(), guard, point, changedValues });

        // Until SSA is repaired a site behind the join names the value of the fast copy. Its rule has to see the type the site was planned with, which is the type of the join.
        TypeID type = function.at(site.use).type;
        TypeID expected = function.at(site.use).expected().predicted();
        ruleOf(site.rule).rewrite(rewriter);
        Value& use = function.edit(site.use, ValueEdit::Type);
        use.type = type;
        use.setExpected(ExpectedType(expected));
    }

    // Nobody can tell that it ran, so nobody can tell that the copy runs it again.
    bool canRunTwice(BlockID block)
    {
        if (context.build.isSabotaged(Sabotage::WhatRunsAgainLeavesNoTrace))
            return true;
        auto canRunTwice = [](const EffectsOnEdges& effects) {
            auto isUnseen = [](const Effects& effects) { return effects.writes.containsOnly(heapsWhoseWritesNobodySees) && !effects.mayRunOtherCode(); };
            return isUnseen(effects.normal) && isUnseen(effects.handler);
        };
        return canRunTwice(analyses.effectsOfTerminator(block)) && std::ranges::all_of(function.at(block).values, [&](ValueID value) { return canRunTwice(analyses.effectsOf(value)); });
    }

    std::optional<Target> findEntryOfCopyAt(BlockID block)
    {
        BlockID copyOfBlock = copy->copyOf(block);
        if (!copyOfBlock)
            return std::nullopt;
        const auto& parameters = function.at(block).parameters;
        const auto& parametersOfCopy = function.at(copyOfBlock).parameters;
        if (parameters.size() < parametersOfCopy.size())
            return std::nullopt;
        Target entry { copyOfBlock, { }, { } };
        for (uint32_t index = 0; index < parametersOfCopy.size(); ++index) {
            if (copy->copyOf(parameters[index]) != parametersOfCopy[index])
                return std::nullopt;
            entry.arguments.append(parameters[index]);
        }
        return entry;
    }

    // A failing test enters the copy behind the test, and hands over every value that the tested code has made and the copy still reads.
    // Where the code in front of the test can run twice, the copy can start in front of that code instead and make those values itself. Many tests that follow each other then share one entry.
    void enterCopyBeforeRepeatableCode()
    {
        if (!copy || emittedTests.isEmpty())
            return;

        size_t numberOfBlocks = function.blocks.size();
        Vector<uint32_t> numberOfIncomingEdges(FillWith { }, numberOfBlocks, 0u);
        Vector<BlockID> predecessors(numberOfBlocks);
        for (size_t index = 0; index < numberOfBlocks; ++index) {
            function.at(BlockID(index)).forEachTarget([&](const Target& target) {
                ++numberOfIncomingEdges[target.block.index()];
                predecessors[target.block.index()] = BlockID(index);
            });
        }

        BitVector isCopy(numberOfBlocks);
        Vector<BlockID> originals(numberOfBlocks);
        for (BlockID block : copy->plannedBlocks()) {
            isCopy.quickSet(copy->copyOf(block).index());
            originals[copy->copyOf(block).index()] = block;
        }
        // No block of the copy becomes an entry that is none. What fails a test of a type is known by that in the copy, so such a test keeps its entry to itself.
        BitVector hasEntry(numberOfBlocks);
        for (const EmittedTest& test : emittedTests) {
            if (isCopy.quickGet(test.failed.index()))
                hasEntry.quickSet(originals[test.failed.index()].index());
        }

        struct EntriesAbove {
            uint32_t count { 0 };
            BlockID shared;
        };
        BitVector isKnown(numberOfBlocks);
        Vector<EntriesAbove> entriesAbove(numberOfBlocks);
        Vector<BlockID, 16> path;
        auto sharedEntryAbove = [&](BlockID last) {
            path.shrink(0);
            BlockID block = last;
            for (; !isKnown.quickGet(block.index()); block = predecessors[block.index()]) {
                if (!canRunTwice(block) || function.at(block).isHandlerBlock()) {
                    isKnown.quickSet(block.index());
                    break;
                }
                path.append(block);
                if (numberOfIncomingEdges[block.index()] != 1)
                    break;
            }
            EntriesAbove entries = isKnown.quickGet(block.index()) ? entriesAbove[block.index()] : EntriesAbove { };
            while (!path.isEmpty()) {
                block = path.takeLast();
                if (hasEntry.quickGet(block.index()) && findEntryOfCopyAt(block) && ++entries.count == maxEntriesInRepeatableCode)
                    entries.shared = block;
                isKnown.quickSet(block.index());
                entriesAbove[block.index()] = entries;
            }
            return entriesAbove[last.index()].shared;
        };

        // What the entry takes is written once, however many tests lead there.
        Vector<BlockID> blocksInFrontOfEntries(numberOfBlocks);
        builder.setHandler(std::nullopt);
        for (const EmittedTest& test : emittedTests) {
            if (!isCopy.quickGet(test.failed.index()))
                continue;
            Edge failingEdge { test.block, 1 };
            BlockID first = sharedEntryAbove(test.block);
            if (!first)
                continue;
            BlockID& target = blocksInFrontOfEntries[first.index()];
            if (!target) {
                Target entry = *findEntryOfCopyAt(first);
                builder.setOrigin(function.at(test.block).terminator.origin());
                target = builder.addBlock(function.at(entry.block).frequency);
                builder.moveToEndOf(target);
                builder.emitJump(entry.block, WTF::move(entry.arguments));
            }
            builder.redirectEdge(failingEdge, Target { target, { }, { } });
        }
    }

    void repairProofs()
    {
        SSARepair repair(builder, analyses);
        for (const Definitions& proofs : definitions) {
            if (proofs.size() < 2)
                continue;
            SSARepair::Variable variable = repair.addVariable(proofs.first());
            for (ValueID proof : proofs.subspan(1))
                repair.addDefinition(variable, proof);
        }
        // An adopted proof in a copied range stands for its condition at uses it does not reach, so one repair has to know it, its copy and the other proofs as one variable.
        if (copy)
            copy->addVariablesTo(repair);
        repair.run();
    }

    void replaceDominatedUses()
    {
        struct NarrowableUse {
            ValueID identity;
            ValueID alias;
            Use use;
            bool getsCast { false };
        };
        struct PlannedCast {
            Edge edge;
            ValueID proven;
            TypeID castType;
            const Use* use { nullptr };
        };
        struct Narrowing {
            TypeID replacedType;
            bool holdsProvenType { false };
            std::optional<TypeID> castType;
        };

        Vector<const EmittedTest*, 8> narrowingTests;
        for (const EmittedTest& emitted : emittedTests) {
            if (plan.conditions->at(plan.tests[emitted.plannedTest].condition).lastsForever())
                narrowingTests.append(&emitted);
        }
        BitVector isInUse;
        if (narrowingTests.isEmpty()) {
            leaveOutBlocksOfUnusedProofs(isInUse);
            return;
        }

        const CFG& cfg = analyses.cfg();
        auto edgeInto = [&](BlockID block) {
            std::optional<Edge> edge = cfg.singleIncomingEdge(block);
            RELEASE_ASSERT(edge);
            return *edge;
        };
        // Where the copies meet, the repair of SSA has given the test the join of what each of them passes on.
        auto subjectOf = [&](const EmittedTest& emitted) {
            ValueID test = function.at(edgeInto(emitted.passed).from).terminator.conditionOperand();
            return function.at(test).operand(testedOperandIndex);
        };

        BitVector isSubject(function.values.size());
        for (const EmittedTest* emitted : narrowingTests)
            isSubject.set(analyses.identityOf(subjectOf(*emitted)).index());
        Vector<NarrowableUse> uses;
        const UseLists& useLists = analyses.useLists();
        const size_t numberOfListedValues = function.values.size();
        // A build that validates adds a value at each replacement, and the identities do not outlast that.
        Vector<ValueID> identityOfListedValue(numberOfListedValues);
        for (size_t index = 0; index < numberOfListedValues; ++index) {
            ValueID alias(index);
            ValueID identity = analyses.identityOf(alias);
            identityOfListedValue[index] = identity;
            if (!isSubject.get(identity.index()))
                continue;
            for (const Use& use : useLists.usesOf(alias)) {
                if (acceptsNarrowedValue(function, use))
                    uses.append(NarrowableUse { identity, alias, use });
            }
        }
        AOT::stableSort(uses, { }, &NarrowableUse::identity);

        bool hasComparisons = false;
        auto replace = [&](const Use& use, ValueID replacement) {
            bool isCompared = context.build.validates() && use.kind != Use::Kind::HandlerArgument && use.kind != Use::Kind::BlockSubject;
            if (ValueID replaced = isCompared ? usedValueOf(function, use) : ValueID(); replaced && context.program.at(function.at(replaced).type).isJavaScriptValue()) {
                ProgramPoint point = pointBeforeTerminatorOf(function, use.block);
                if (use.kind == Use::Kind::Operand)
                    point.index = analyses.useLists().locationOf(use.user).index;
                builder.insertBefore(point, IRBuilder::make(Opcode::ValidateSameValue, { replacement, replaced }));
                hasComparisons = true;
            }
            replaceUse(function, use, replacement);
            if (use.kind == Use::Kind::Operand)
                changedValues.append(use.user);
        };
        auto isReachedByCastIn = [&](BlockID target, const Use& use) {
            if (use.block != target)
                return analyses.dominators().terminatorDominates(target, use.block);
            switch (use.kind) {
            case Use::Kind::Operand:
            case Use::Kind::TerminatorOperand:
            case Use::Kind::EdgeArgument:
                return true;
            case Use::Kind::HandlerArgument:
            case Use::Kind::BlockSubject:
                return false;
            }
            RELEASE_ASSERT_NOT_REACHED();
        };

        Vector<PlannedCast> plannedCasts;
        Vector<Narrowing, 4> narrowings;
        auto replaceUsesDominatedBy = [&](Edge edge, ValueID subject, ValueID proven) {
            TypeID provenTypeID = function.at(proven).type;
            Type provenType = types.at(provenTypeID);
            ProgramPoint pointOfTest = pointBeforeTerminatorOf(function, edge.from);
            BlockID target = targetOf(function, edge).block;
            narrowings.shrink(0);

            for (NarrowableUse& narrowable : std::ranges::equal_range(uses, analyses.identityOf(subject), { }, &NarrowableUse::identity)) {
                // A use that gets a cast will name none of the values that were listed.
                if (narrowable.alias == proven || narrowable.getsCast)
                    continue;
                const Use& use = narrowable.use;
                if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateSameValue)
                    continue;
                ValueID current = usedValueOf(function, use);
                if (current == proven || !analyses.dominates(edge, use.block))
                    continue;
                TypeID currentTypeID = function.at(current).type;
                if (currentTypeID == provenTypeID)
                    continue;
                size_t found = narrowings.findIf([&](const Narrowing& known) { return known.replacedType == currentTypeID; });
                if (found == notFound) {
                    found = narrowings.size();
                    narrowings.append(Narrowing { currentTypeID, provenType.isSubsetOf(types.at(currentTypeID), context.program), std::nullopt });
                }
                Narrowing& narrowing = narrowings[found];
                if (narrowing.holdsProvenType) {
                    replace(use, proven);
                    isInUse.set(proven.index());
                    continue;
                }
                // What a value defined behind the edge says holds from its definition on.
                if (!analyses.dominates(current, pointOfTest) || !isReachedByCastIn(target, use))
                    continue;
                // The type of the value may say what no test finds out, such as what flow proved about a class.
                if (!narrowing.castType) {
                    Type replacedType = types.at(currentTypeID);
                    Type both = provenType.intersectWith(replacedType, types);
                    bool saysMore = !both.isNever() && both != replacedType && both.isSubsetOf(replacedType, context.program);
                    narrowing.castType = saysMore ? types.intern(both) : TypeID();
                }
                if (*narrowing.castType) {
                    narrowable.getsCast = true;
                    plannedCasts.append(PlannedCast { edge, proven, *narrowing.castType, &use });
                    isInUse.set(proven.index());
                }
            }
        };
        for (const EmittedTest* emitted : narrowingTests) {
            ValueID subject = subjectOf(*emitted);
            replaceUsesDominatedBy(edgeInto(emitted->passed), subject, emitted->provenWhenPassed);
            if (emitted->provenWhenFailed)
                replaceUsesDominatedBy(edgeInto(emitted->failed), subject, emitted->provenWhenFailed);
        }

        // Code that two tests of one value lead to is behind neither of them. What passed either comes there as a parameter.
        for (size_t index = 0; index < numberOfListedValues; ++index) {
            ValueID joined(index);
            ValueID identity = identityOfListedValue[index];
            if (identity == joined || !isSubject.get(identity.index()))
                continue;
            ValueLocation location = useLists.locationOf(joined);
            if (location.placement != ValuePlacement::Parameter || cfg.singleIncomingEdge(location.block))
                continue;
            TypeID joinedTypeID = function.at(joined).type;
            Type joinedType = types.at(joinedTypeID);
            for (NarrowableUse& narrowable : std::ranges::equal_range(uses, identity, { }, &NarrowableUse::identity)) {
                const Use& use = narrowable.use;
                if (narrowable.getsCast || (use.kind != Use::Kind::Operand && use.kind != Use::Kind::TerminatorOperand && use.kind != Use::Kind::EdgeArgument))
                    continue;
                if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateSameValue)
                    continue;
                ValueID current = usedValueOf(function, use);
                TypeID currentTypeID = function.at(current).type;
                if (current == joined || currentTypeID == joinedTypeID || !analyses.dominators().dominates(location.block, use.block))
                    continue;
                if (joinedType.isSubsetOf(types.at(currentTypeID), context.program))
                    replace(use, joined);
            }
        }

        Vector<std::pair<TypeID, ValueID>, 2> castsOfProvenValue;
        ValueID proven;
        for (const PlannedCast& planned : plannedCasts) {
            if (planned.proven != proven) {
                proven = planned.proven;
                castsOfProvenValue.shrink(0);
                builder.setOrigin(function.at(planned.edge.from).terminator.origin());
            }
            size_t found = castsOfProvenValue.findIf([&](const auto& known) { return known.first == planned.castType; });
            if (found == notFound) {
                found = castsOfProvenValue.size();
                BlockID target = targetOf(function, planned.edge).block;
                ProgramPoint head { target, indexAfterCaughtValues(function, function.at(target)) };
                if (context.build.validates())
                    builder.insertBefore(head, makeValidateType(proven, planned.castType));
                castsOfProvenValue.append({ planned.castType, builder.insertBefore(head, makeAssumeType(proven, Proven::byRulesWithoutChecks(planned.castType))) });
            }
            replace(*planned.use, castsOfProvenValue[found].second);
        }
        if (!plannedCasts.isEmpty() || hasComparisons)
            builder.applyInsertions();
        analyses.didChangeValues();
        leaveOutBlocksOfUnusedProofs(isInUse);
    }

    // What a test proves where it fails is a parameter of a block on the failing edge.
    void leaveOutBlocksOfUnusedProofs(const BitVector& isUsed)
    {
        bool changed = false;
        for (const EmittedTest& emitted : emittedTests) {
            if (!emitted.provenWhenFailed || isUsed.get(emitted.provenWhenFailed.index()))
                continue;
            // A rewritten use may have split the block of the test, so the branch is looked for from its target.
            std::optional<Edge> edge = analyses.cfg().singleIncomingEdge(emitted.failed);
            RELEASE_ASSERT(edge && function.at(emitted.failed).terminator.kind == TerminatorKind::Jump);
            // The repair of SSA may have led a value through the block, and a cast may stand in it.
            if (!function.at(emitted.failed).values.isEmpty() || function.at(emitted.failed).parameters.size() != 1)
                continue;
            Block& failed = function.edit(emitted.failed, { BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
            Target& notTaken = editTarget(function, *edge);
            notTaken = Target { failed.terminator.next().block, WTF::move(failed.terminator.next().arguments), { } };
            failed.parameters.clear();
            failed.terminator = Terminator { };
            changed = true;
        }
        if (changed)
            analyses.didChangeControlFlow();
    }
};

} // anonymous namespace

// What nobody could follow is in an expected type beside whatever else reached the place.
static TypeID narrowExpectedTypeByUses(FunctionPassContext& context, std::span<const ValueID> values, TypeID expected)
{
    const Type& expectedType = context.types.at(expected);
    if (maxNumberOfUnionPartsWithObjects(expectedType) < 2)
        return expected;
    UnionParts parts = unionPartsOf(expectedType);
    // Only a part that has objects can go, and never the last one. undefined | null stands beside every reference type.
    if (std::ranges::count_if(parts, [](const UnionPart& part) { return part.type.hasObjects(); }) < 2)
        return expected;

    // One access alone contradicts nothing.
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, parts);
    if (evidence.numberOfResolvableAccesses < 2)
        return expected;
    Type narrowed = Type::never();
    bool expectsObject = false;
    for (size_t part = 0; part < parts.size(); ++part) {
        bool servesEveryAccess = evidence.countOfPart[part] == evidence.numberOfResolvableAccesses;
        if (!parts[part].type.hasObjects() || servesEveryAccess)
            narrowed = narrowed.unionWith(parts[part].type, context.types);
        expectsObject |= parts[part].type.hasObjects() && servesEveryAccess;
    }
    return expectsObject ? context.types.intern(narrowed) : expected;
}

// Of a value that nobody could follow, its uses are all there is to go by. What is read or stored under a number, and asked for nothing that an array does not have, is expected to be an array.
static TypeID expectArrayByUses(FunctionPassContext& context, std::span<const ValueID> values)
{
    bool isAccessedByKey = std::ranges::any_of(values, [&](ValueID id) {
        return std::ranges::any_of(context.analyses.useLists().usesOf(id), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && elementAccessOf(context.function.at(use.user).opcode);
        });
    });
    if (!isAccessedByKey)
        return { };
    UnionParts arrayAlone = unionPartsOf(Type::builtinClass(BuiltinClasses::Array).withoutClassProofs());
    AccessesResolvedOnParts evidence = countAccessesResolvedOnParts(context, values, arrayAlone);
    if (!evidence.numberOfResolvableElementAccesses || evidence.numberOfResolvableAccesses < evidence.numberOfAccesses)
        return { };
    return context.types.intern(arrayAlone[0].type);
}

// Every rule then tests a value for the same parts. A program reads `row.tags` again wherever it uses it: what is read from one object under one name is used as one value.
static void settleExpectedTypesByUses(FunctionPassContext& context, ExpectedTypesByUse& expectations)
{
    struct PropertyRead {
        std::tuple<ValueID, PropertyKeyID, TypeID> place;
        ValueID read;
    };
    const Function& function = context.function;
    auto settle = [&](std::span<const ValueID> values) {
        TypeID expected = function.at(values[0]).expected().predicted();
        TypeID settled = expected ? narrowExpectedTypeByUses(context, values, expected) : expectArrayByUses(context, values);
        if (settled == expected)
            return;
        for (ValueID id : values)
            expectations.expect(id, settled);
    };

    Vector<PropertyRead> reads;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        ValueID id(index);
        const Value& value = function.at(id);
        if (value.expected().predicted() == value.type || (!value.expected().predicted() && !context.types.at(value.type).anyObject) || !context.analyses.useLists().useCount(id))
            continue;
        if (value.opcode == Opcode::PropGet)
            reads.append(PropertyRead { { value.objectOperand(), value.key(), value.expected().predicted() }, id });
        else
            settle(singleElementSpan(id));
    }

    AOT::sort(reads, { }, [](const PropertyRead& read) { return std::pair { read.place, read.read }; });
    Vector<ValueID, 4> readsOfOnePlace;
    for (size_t index = 0; index < reads.size(); ++index) {
        readsOfOnePlace.append(reads[index].read);
        if (index + 1 < reads.size() && reads[index + 1].place == reads[index].place)
            continue;
        settle(readsOfOnePlace.span());
        readsOfOnePlace.shrink(0);
    }
}

static PassOutcome placePlannedTests(FunctionPassContext& context, const ExpectedTypesByUse& expectations)
{
    TestPlacementPlan plan = planTestPlacement(context);
    if (!plan.hasGuardedStretch())
        return PassOutcome::Unchanged;

    TestPlacer placer { context, plan, expectations };
    PassOutcome outcome = placer.run();
    Statistics& statistics = context.build.statistics();
    statistics.count(StatisticID::StretchesGuarded, placer.numberOfGuardedStretches());
    statistics.count(StatisticID::StretchesNotClonable, placer.unclonableStretches.bitCount());
    statistics.count(StatisticID::WaysBackFromStretchCopies, placer.copy ? placer.copy->numberOfWaysBack() : 0);
    statistics.count(StatisticID::TypeTestsPlaced, placer.emittedTests.size());
    return outcome;
}

static bool asksForElementOfArray(const FunctionPassContext& context, const Value& test)
{
    if (test.opcode == Opcode::HasArrayElement)
        return true;
    if (test.opcode != Opcode::ElemHas || !test.keyIsIntegral())
        return false;
    const ClassPart* receiver = context.program.typeOf(context.function, test.objectOperand()).singleBuiltinClass();
    return receiver && receiver->builtinClass == BuiltinClasses::Array;
}

// Arrays seldom have holes. The arm that a hole takes, in the loop of map() for one, is a slow path: what holds in front of the question holds behind it on the typed path, and the arm comes back through the tests.
// A new array takes what is stored into it but where it has left the storage that it was made with: the arm behind a store that failed gets no tests.
static PassOutcome markArmsThatAreSeldomTaken(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    const CFG& cfg = context.analyses.cfg();
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = context.function.at(id);
        if (!block.terminator.isConditionalBranch() || cfg.incomingEdges(block.terminator.notTaken().block).size() != 1)
            continue;
        const Value& test = context.function.at(block.terminator.conditionOperand());
        const Block& arm = context.function.at(block.terminator.notTaken().block);
        Expected<BlockFrequency> frequency = arm.frequency;
        if (test.opcode == Opcode::TryPutByValDirect)
            frequency = BlockFrequency::Rare;
        // An arm with code of its own is code of the program, which gets its tests as any other.
        else if (block.frequency.predicted() == BlockFrequency::Normal && arm.frequency.predicted() == BlockFrequency::Normal && arm.values.isEmpty() && arm.terminator.kind == TerminatorKind::Jump && asksForElementOfArray(context, test))
            frequency = BlockFrequency::Unlikely;
        if (frequency.isSamePredictionAs(arm.frequency))
            continue;
        context.function.edit(block.terminator.notTaken().block, BlockEdit::Rest).frequency = frequency;
        outcome = PassOutcome::Changed;
    }
    if (outcome == PassOutcome::Changed)
        context.analyses.invalidate(FunctionAnalysis::Frequencies);
    return outcome;
}

// `x !== x`: only NaN differs from itself. So what is no number has its answer, and the test for a number is no guess that sends other values into a copy.
static PassOutcome askOnlyNumbersWhetherTheyEqualThemselves(FunctionPassContext& context)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    if (!analyses.opcodesOfValues().contains(Opcode::StrictEq))
        return PassOutcome::Unchanged;
    Vector<ValueID, 4> comparisons;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (function.at(block).frequency.predicted() == BlockFrequency::Rare)
            continue;
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::StrictEq || value.leftOperand() != value.rightOperand())
                continue;
            // What a branch hands on of the operands of its test, it hands on of the comparison itself.
            bool isTestOfBranchWithProofs = std::ranges::any_of(analyses.useLists().usesOf(id), [&](const Use& use) {
                return use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.targets.containsIf([](const Target& target) { return !target.proofs.isEmpty(); });
            });
            const Type& type = context.program.typeOf(function, value.leftOperand());
            if ((type.primitives & Primitive::Number) && !type.isNumber() && !isTestOfBranchWithProofs)
                comparisons.append(id);
        }
    }
    if (comparisons.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    for (ValueID id : comparisons) {
        ValueLocation location = analyses.useLists().locationOf(id);
        ValueID compared = function.at(id).leftOperand();
        Expected<BlockFrequency> frequency = function.at(location.block).frequency;
        builder.setOrigin(function.at(id).origin());
        IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { location.block, location.index });
        IRBuilder::ProvenBranch isNumber = builder.emitBranchWithProof(builder.appendTypeTest(compared, TypeTable::number), OperandIndex::TypeTest::value, frequency, frequency);
        builder.moveToEndOf(isNumber.taken);
        builder.endExpansion(expansion, builder.append(Opcode::StrictEq, { isNumber.provenOnTaken, isNumber.provenOnTaken }));
        builder.moveToEndOf(isNumber.notTaken);
        builder.endExpansion(expansion, builder.appendBoolean(true));
    }
    return PassOutcome::Changed;
}

PassOutcome placeTypeTests(FunctionPassContext& context)
{
    const Heat& heat = context.inputs.heat;
    bool hasUsesLeftByLastRound = std::exchange(context.hasUsesAwaitingExpansion, false);
    // An operator whose one operand has become an integer is worth a test of the other.
    hasUsesLeftByLastRound |= std::exchange(context.hasCountersConvertedSinceTestsWerePlaced, false);
    // What a test in a cold function buys, it buys at run time: the passes of the rounds gain little from it, and they would carry its arm and its join: a twentieth of the build of a very large program.
    auto placesTestsBehindRounds = [&] { return heat.isOfVeryLargeProgram() && isCold(heat.of(context.program, context.id)).predicted(); };
    if (context.isBehindRounds ? !hasUsesLeftByLastRound && !placesTestsBehindRounds() : context.isInRounds && placesTestsBehindRounds())
        return PassOutcome::Unchanged;
    if (getsNoTests(heat, context.analyses))
        return PassOutcome::Unchanged;

    PassOutcome marked = markArmsThatAreSeldomTaken(context);
    marked |= askOnlyNumbersWhetherTheyEqualThemselves(context);
    ExpectedTypesByUse expectations(context.function);
    PassOutcome outcome = PassOutcome::Unchanged;
    if (context.build.recordedOptions().usesExpectedTypes()) {
        if (expectLayoutsByNames(context, expectations))
            outcome = PassOutcome::Changed;
        settleExpectedTypesByUses(context, expectations);
        if (!expectations.isEmpty())
            context.analyses.didChangeTypes();
    }
    outcome |= placePlannedTests(context, expectations);
    outcome |= addEmptySlotCases(context);
    if (outcome == PassOutcome::Unchanged && !expectations.isEmpty()) {
        expectations.takeBack();
        context.analyses.didChangeTypes();
    }
    // Whoever edits blocks, operands or edges here drops what that makes stale.
    context.keptAnalyses = outcome == PassOutcome::Unchanged ? allAnalyses - FunctionAnalysis::Effects : controlFlowAnalyses | analysesThatReadNoTypes;
    return outcome | marked;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
