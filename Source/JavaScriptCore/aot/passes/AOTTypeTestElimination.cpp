/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTGuardFactDetails.h"
#include "AOTSSARepair.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestedBranch {
    BlockID block;
    uint32_t index { 0 };
    ValueID test;
    uint32_t firstRequirement { 0 };
    uint32_t numberOfRequirements { 0 };
};

enum class ProofOrigin : uint8_t { Unknown, Test, SwitchCase, Allocation };

struct BranchVerdicts {
    BitVector redundant;
    BitVector unreachable;
};

enum class TestUse : uint8_t { FoldedBranch, Boolean, Other };

struct FoldedBranch {
    uint32_t branch { 0 };
    ValueID replacementGuard;
    Vector<Use, 2> booleanUses;
    bool hasOtherUse { false };
};

struct FoldingInputs {
    FunctionPassContext& context;
    const TestConditions& conditions;
    std::span<const TestedBranch> branches;
    std::span<const TestRequirement> requirements;
    std::span<const ExistingProof> proofs;
    const BitVector& redundant;
};

class BranchFolder {
    WTF_MAKE_NONCOPYABLE(BranchFolder);
public:
    explicit BranchFolder(const FoldingInputs&);

    void run();

private:
    const TestedBranch& branchOf(const FoldedBranch& folded) const LIFETIME_BOUND { return m_inputs.branches[folded.branch]; }
    std::span<const TestRequirement> requirementsOf(const TestedBranch& branch) const { return m_inputs.requirements.subspan(branch.firstRequirement, branch.numberOfRequirements); }

    void adoptProofs();
    void collectUsesOfTests();
    TestUse classify(const Use&, const TestedBranch&) const;
    void defineProofs();
    void createReplacementGuards();
    void replaceBooleanUses();
    void fold(const FoldedBranch&);
    void eraseUnusedTests();
    void repairProofs();

    FoldingInputs m_inputs;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder m_builder;
    Vector<FoldedBranch, 4> m_foldedBranches;
    BitVector m_foldedBlocks;
    BitVector m_conditionsInPlay;
    Vector<Vector<ValueID, 2>> m_provingValues;
    Vector<Vector<ValueID, 2>> m_definitions;
};

class GuardEliminator {
    WTF_MAKE_NONCOPYABLE(GuardEliminator);
public:
    explicit GuardEliminator(FunctionPassContext&);

    PassOutcome run();

private:
    std::span<const TestRequirement> requirementsOf(const TestedBranch& branch) const LIFETIME_BOUND { return m_requirements.subspan(branch.firstRequirement, branch.numberOfRequirements); }
    bool renewsGuards(const TestedBranch& branch) const { return m_function.at(branch.test).opcode == Opcode::IsShapeEpochCurrent; }

    void collectBranches();
    bool addBranch(BlockID, ValueID test);
    Vector<ValueID, 2> findProvingTests(ValueID test);
    bool mayFindRedundantBranch() const;

    void findProofs();
    ProofOrigin originOf(const ExistingProof&);
    Vector<ProofSet> findProofsReliedOn(const BitVector& conditions);
    bool canReplaceTests(const ExistingProof& allocationProof, ProofSet reliedOn) const;

    BitVector findRedundantBranches();
    BranchVerdicts findCoveredBranches(const TestEvents&);
    BranchVerdicts classifyBranches(const TestCoverage&, const BitVector& branches) const;
    BitVector findMixedConditions(const BranchVerdicts&) const;

    PassOutcome simplifyGuardLists();
    Vector<ValueID, 4> flattenedGuardsOf(ValueID list) const;
    bool eraseUnusedGuardLists();

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    TestConditions m_conditions;
    Vector<TestedBranch> m_branches;
    Vector<TestRequirement> m_requirements;
    BitVector m_conditionsWithOtherTests;
    bool m_hasSwitchOnShape { false };
    Vector<ExistingProof> m_proofsFromTests;
    Vector<ExistingProof> m_otherProofs;
    Vector<ExistingProof> m_proofs;
    uint64_t m_numberOfRounds { 0 };
    bool m_didReachRoundLimit { false };
};

BranchFolder::BranchFolder(const FoldingInputs& inputs)
    : m_inputs(inputs)
    , m_function(inputs.context.function)
    , m_analyses(inputs.context.analyses)
    , m_builder(inputs.context.analyses.context(), inputs.context.function, inputs.context.types, &inputs.context.analyses)
    , m_provingValues(inputs.conditions.size())
    , m_definitions(inputs.conditions.size())
{
    for (size_t index : inputs.redundant) {
        const TestedBranch& branch = inputs.branches[index];
        m_foldedBranches.append(FoldedBranch { static_cast<uint32_t>(index), { }, { }, false });
        m_foldedBlocks.set(branch.block.index());
        for (const TestRequirement& requirement : requirementsOf(branch))
            m_conditionsInPlay.set(requirement.condition.index());
    }
}

void BranchFolder::run()
{
    adoptProofs();
    collectUsesOfTests();
    defineProofs();
    createReplacementGuards();
    replaceBooleanUses();
    m_builder.applyInsertions();

    for (const FoldedBranch& folded : m_foldedBranches)
        fold(folded);
    eraseUnusedTests();
    m_builder.emptyUnreachableBlocks();
    repairProofs();
}

void BranchFolder::adoptProofs()
{
    for (const ExistingProof& proof : m_inputs.proofs) {
        if (!m_conditionsInPlay.get(proof.condition.index()))
            continue;

        ValueID value = proof.value;
        switch (proof.source) {
        case ProofSource::TakenEdge:
            if (m_foldedBlocks.get(proof.edge.from.index()))
                continue;
            [[fallthrough]];
        case ProofSource::SwitchCase:
            value = ensureEdgeResult(m_builder, m_analyses, proof.edge, testedOperandIndex);
            break;
        case ProofSource::GuardValue:
        case ProofSource::Allocation:
            break;
        }
        m_provingValues[proof.condition.index()].appendIfNotContains(value);
    }
}

TestUse BranchFolder::classify(const Use& use, const TestedBranch& branch) const
{
    switch (use.kind) {
    case Use::Kind::TerminatorOperand:
        if (use.block == branch.block)
            return TestUse::FoldedBranch;
        return m_function.at(use.block).terminator.isConditionalBranch() ? TestUse::Other : TestUse::Boolean;
    case Use::Kind::Operand:
        return m_function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard ? TestUse::Other : TestUse::Boolean;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return TestUse::Boolean;
    case Use::Kind::BlockSubject:
        return TestUse::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BranchFolder::collectUsesOfTests()
{
    const UseLists& useLists = m_analyses.useLists();
    for (FoldedBranch& folded : m_foldedBranches) {
        const TestedBranch& branch = branchOf(folded);
        for (const Use& use : useLists.usesOf(branch.test)) {
            switch (classify(use, branch)) {
            case TestUse::FoldedBranch:
                break;
            case TestUse::Boolean:
                folded.booleanUses.append(use);
                break;
            case TestUse::Other:
                folded.hasOtherUse = true;
                break;
            }
        }
    }
}

void BranchFolder::defineProofs()
{
    struct PendingDefinition {
        size_t condition { 0 };
        ValueID value;
        ProgramPoint pointAfter;
    };

    Vector<PendingDefinition, 8> pendingDefinitions;
    const UseLists& useLists = m_analyses.useLists();
    for (size_t condition : m_conditionsInPlay) {
        for (ValueID value : m_provingValues[condition])
            pendingDefinitions.append(PendingDefinition { condition, value, useLists.pointAfter(value) });
    }

    // SSARepair redirects every use of a definition, and a proving value has uses of its own: an allocation is also the object.
    for (const PendingDefinition& pending : pendingDefinitions) {
        m_builder.setOrigin(m_function.at(pending.value).origin());
        m_definitions[pending.condition].append(m_builder.insertBefore(pending.pointAfter, IRBuilder::make(Opcode::AllGuards, { pending.value })));
    }
}

void BranchFolder::createReplacementGuards()
{
    for (FoldedBranch& folded : m_foldedBranches) {
        const TestedBranch& branch = branchOf(folded);
        if (m_function.at(branch.block).terminator.taken().proofs.isEmpty())
            continue;

        Vector<ValueID, 2> guards;
        for (const TestRequirement& requirement : requirementsOf(branch)) {
            const Vector<ValueID, 2>& definitions = m_definitions[requirement.condition.index()];
            RELEASE_ASSERT(!definitions.isEmpty());
            guards.appendIfNotContains(definitions.first());
        }
        if (guards.size() == 1) {
            folded.replacementGuard = guards.first();
            continue;
        }

        Value list = IRBuilder::make(Opcode::AllGuards);
        list.setGuardsOperands(guards.span());
        m_builder.setOrigin(m_function.at(branch.test).origin());
        folded.replacementGuard = m_builder.insertBefore(pointBeforeTerminatorOf(m_function, branch.block), WTF::move(list));
    }
}

void BranchFolder::replaceBooleanUses()
{
    bool keepsTests = m_inputs.context.build.validates();
    for (FoldedBranch& folded : m_foldedBranches) {
        if (folded.booleanUses.isEmpty() && !keepsTests)
            continue;

        const TestedBranch& branch = branchOf(folded);
        Value boolean = IRBuilder::make(Opcode::Boolean);
        boolean.setBoolean(true);
        m_builder.setOrigin(m_function.at(branch.test).origin());
        ValueID constant = m_builder.insertBefore({ branch.block, branch.index }, WTF::move(boolean));
        for (const Use& use : folded.booleanUses)
            replaceUse(m_function, use, constant);
        if (!keepsTests)
            continue;
        // In a validating build the test is made all the same, and its answer is compared with the one that an earlier test stands for.
        m_builder.insertBefore(pointBeforeTerminatorOf(m_function, branch.block), IRBuilder::make(Opcode::ValidateSameValue, { branch.test, constant }));
        folded.hasOtherUse = true;
    }
}

void BranchFolder::fold(const FoldedBranch& folded)
{
    BlockID block = branchOf(folded).block;
    Terminator& terminator = m_function.at(block).terminator;
    Target taken = terminator.taken();
    Arguments arguments = WTF::move(taken.arguments);
    for (size_t result = 0; result < taken.proofs.size(); ++result)
        arguments.append(folded.replacementGuard);

    m_builder.setOrigin(terminator.origin());
    terminator = Terminator { };
    m_builder.moveToEndOf(block);
    m_builder.emitJump(taken.block, WTF::move(arguments));
}

void BranchFolder::eraseUnusedTests()
{
    for (const FoldedBranch& folded : m_foldedBranches) {
        if (folded.hasOtherUse)
            continue;

        const TestedBranch& branch = branchOf(folded);
        size_t index = m_function.at(branch.block).values.find(branch.test);
        RELEASE_ASSERT(index != notFound);
        BitVector indexes;
        indexes.set(index);
        m_builder.eraseValues(branch.block, indexes);
    }
}

void BranchFolder::repairProofs()
{
    SSARepair repair(m_builder, m_analyses);
    for (size_t condition : m_conditionsInPlay) {
        std::span<const ValueID> definitions = m_definitions[condition].span();
        if (definitions.size() < 2)
            continue;
        SSARepair::Variable variable = repair.addVariable(definitions.front());
        for (ValueID definition : definitions.subspan(1))
            repair.addDefinition(variable, definition);
    }
    repair.run();
}

GuardEliminator::GuardEliminator(FunctionPassContext& context)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
    , m_conditions(context.analyses)
{
}

PassOutcome GuardEliminator::run()
{
    PassOutcome outcome = PassOutcome::Unchanged;
    collectBranches();
    if (mayFindRedundantBranch()) {
        BitVector redundant = findRedundantBranches();
        if (!redundant.isEmpty()) {
            BranchFolder(FoldingInputs { m_context, m_conditions, m_branches.span(), m_requirements.span(), m_proofs.span(), redundant }).run();
            m_context.build.statistics().count(StatisticID::GuardsEliminated, redundant.bitCount());
            outcome = PassOutcome::Changed;
        }
    }
    outcome |= simplifyGuardLists();
    return outcome;
}

void GuardEliminator::collectBranches()
{
    Vector<ValueID, 4> otherTests;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        m_hasSwitchOnShape |= terminator.kind == TerminatorKind::SwitchOnShape;
        if (!terminator.isConditionalBranch())
            continue;

        ValueID test = terminator.conditionOperand();
        if (!provesForStretch(provesOf(m_function.at(test).opcode)))
            continue;
        if (!addBranch(block, test))
            otherTests.append(test);
    }

    for (ValueID test : otherTests) {
        if (std::optional<TestConditionID> condition = m_conditions.find(m_function.at(test)))
            m_conditionsWithOtherTests.set(condition->index());
    }
}

bool GuardEliminator::addBranch(BlockID block, ValueID test)
{
    const Terminator& terminator = m_function.at(block).terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.taken().block == terminator.notTaken().block)
        return false;

    ValueLocation location = m_analyses.useLists().locationOf(test);
    if (location.placement != ValuePlacement::InBlock || location.block != block)
        return false;

    Effects bound = declaredBoundOf(m_function.at(test).opcode);
    if (!bound.writes.isEmpty() || bound.mayExitSideways() || bound.mayCollect)
        return false;

    Vector<ValueID, 2> provingTests = findProvingTests(test);
    if (provingTests.isEmpty())
        return false;

    size_t numberOfNewConditions = std::ranges::count_if(provingTests, [&](ValueID provingTest) {
        return !m_conditions.find(m_function.at(provingTest));
    });
    if (m_conditions.size() + numberOfNewConditions > m_context.limit(LimitID::MaxTestedTypesPerFunction)) {
        m_context.gaveUp(LimitID::MaxTestedTypesPerFunction, test);
        return false;
    }

    uint32_t firstRequirement = static_cast<uint32_t>(m_requirements.size());
    for (ValueID provingTest : provingTests)
        m_requirements.append(TestRequirement { 0, 0, m_conditions.add(m_function.at(provingTest)), { block, location.index }, RequirementKind::AsksOnly });
    m_branches.append(TestedBranch { block, location.index, test, firstRequirement, static_cast<uint32_t>(provingTests.size()) });
    return true;
}

Vector<ValueID, 2> GuardEliminator::findProvingTests(ValueID test)
{
    const Value& value = m_function.at(test);
    if (value.opcode != Opcode::IsShapeEpochCurrent)
        return { test };

    // A fact that one of several tests proved, or one about a parameter that took the place of the tested value, has another condition than its test.
    const GuardFacts& renewed = m_analyses.guardFacts(value.guardOperand());
    bool hasConditionsOfTests = renewed.isKnown() && std::ranges::all_of(renewed.facts, [&](const GuardFact& fact) {
        const GuardOrigin* origin = fact.singleOrigin();
        return origin && origin->source == GuardSource::Test && !hasRenamedSubject(m_analyses, fact);
    });
    if (!hasConditionsOfTests)
        return { };

    Vector<ValueID, 2> provingTests;
    for (const GuardFact& fact : renewed.facts)
        provingTests.append(fact.origins.first().test);
    return provingTests;
}

bool GuardEliminator::mayFindRedundantBranch() const
{
    if (m_branches.isEmpty())
        return false;
    if (m_requirements.size() > m_conditions.size() || !m_conditionsWithOtherTests.isEmpty())
        return true;

    for (size_t index : m_conditions.stretchConditions()) {
        const TestCondition& condition = m_conditions.at(TestConditionID(index));
        if (m_hasSwitchOnShape && condition.proof == Proof::Shape)
            return true;
        if (condition.subject && !proofsOfAllocation(m_context.program, m_function, m_function.at(condition.subject)).isEmpty())
            return true;
    }
    return false;
}

ProofOrigin GuardEliminator::originOf(const ExistingProof& proof)
{
    switch (proof.source) {
    case ProofSource::TakenEdge:
        return ProofOrigin::Test;
    case ProofSource::SwitchCase:
        return ProofOrigin::SwitchCase;
    case ProofSource::Allocation:
        return ProofOrigin::Allocation;
    case ProofSource::GuardValue:
        break;
    }

    // A user needs everything its guard proves about what it relies on to hold, so a guard that proves more than the folded test did cannot replace it.
    const GuardFacts& proven = m_analyses.guardFacts(proof.value);
    const GuardOrigin* origin = proven.facts.size() == 1 ? proven.facts.first().singleOrigin() : nullptr;
    if (!origin)
        return ProofOrigin::Unknown;

    switch (origin->source) {
    case GuardSource::Test:
        return ProofOrigin::Test;
    case GuardSource::SwitchCase:
        return ProofOrigin::SwitchCase;
    case GuardSource::Allocation:
        return ProofOrigin::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ProofSet> GuardEliminator::findProofsReliedOn(const BitVector& conditions)
{
    Vector<ProofSet> reliedOn(m_conditions.size());
    auto addUser = [&](const auto& user, ProofSet proofs) {
        if (proofs.isEmpty())
            return;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (user.operandKind(operandIndex) != OperandKind::Guard)
                return;
            for (const GuardFact& fact : m_analyses.guardFacts(operand).facts) {
                for (const GuardOrigin& origin : fact.origins) {
                    if (origin.source != GuardSource::Test)
                        continue;
                    std::optional<TestConditionID> condition = m_conditions.find(m_function.at(origin.test));
                    if (condition && conditions.get(condition->index()))
                        reliedOn[condition->index()] |= proofs;
                }
            }
        });
    };

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Block& contents = m_function.at(block);
        for (ValueID id : contents.values) {
            const Value& value = m_function.at(id);
            addUser(value, reliesOf(value.opcode));
        }
        addUser(contents.terminator, reliesOf(contents.terminator.kind));
    }
    return reliedOn;
}

bool GuardEliminator::canReplaceTests(const ExistingProof& allocationProof, ProofSet reliedOn) const
{
    const TestCondition& condition = m_conditions.at(allocationProof.condition);
    const Value& allocation = m_function.at(allocationProof.value);

    AbstractHeapSet heapsDependedOn;
    for (const MemoryLocation& location : condition.dependsOn)
        heapsDependedOn.add(location.heap());

    // Whoever names the allocation gets all it proves, and each of those proofs has to last as long as a passed test does.
    reliedOn.add(condition.proof);
    ProofSet proven = proofsOfAllocation(m_context.program, m_function, allocation);
    return std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
        if (!reliedOn.contains(proof) || !proven.contains(proof))
            return true;
        return std::ranges::all_of(locationsBehindAllocationProof(proof), [&](const MemoryLocation& location) {
            return heapsDependedOn.contains(location.heap());
        });
    });
}

void GuardEliminator::findProofs()
{
    Vector<ExistingProof> proofs = findExistingProofs(m_context, m_conditions);

    BitVector conditionsOnAllocations;
    for (const ExistingProof& proof : proofs) {
        if (proof.source == ProofSource::Allocation)
            conditionsOnAllocations.set(proof.condition.index());
    }

    BitVector conditionsProvenByAllocation;
    if (!conditionsOnAllocations.isEmpty()) {
        Vector<ProofSet> reliedOn = findProofsReliedOn(conditionsOnAllocations);
        for (const ExistingProof& proof : proofs) {
            if (proof.source == ProofSource::Allocation && canReplaceTests(proof, reliedOn[proof.condition.index()]))
                conditionsProvenByAllocation.set(proof.condition.index());
        }
    }

    for (const ExistingProof& proof : proofs) {
        bool isProvenByAllocation = conditionsProvenByAllocation.get(proof.condition.index());
        switch (originOf(proof)) {
        case ProofOrigin::Unknown:
            break;
        case ProofOrigin::Test:
            m_proofsFromTests.append(proof);
            break;
        case ProofOrigin::SwitchCase:
            if (!isProvenByAllocation)
                m_otherProofs.append(proof);
            break;
        case ProofOrigin::Allocation:
            if (isProvenByAllocation)
                m_otherProofs.append(proof);
            break;
        }
    }
}

BitVector GuardEliminator::findRedundantBranches()
{
    findProofs();
    for (;;) {
        m_proofs = m_proofsFromTests;
        m_proofs.appendVector(m_otherProofs);

        TestEvents events(TestEventInputs { m_analyses, m_conditions, { }, m_requirements.span(), m_proofs.span(), { }, RenewalPolicy::Never });
        BranchVerdicts verdicts = findCoveredBranches(events);
        BitVector mixedConditions = findMixedConditions(verdicts);
        if (mixedConditions.isEmpty())
            return WTF::move(verdicts.redundant);

        m_otherProofs.removeAllMatching([&](const ExistingProof& proof) {
            return m_didReachRoundLimit || mixedConditions.get(proof.condition.index());
        });
    }
}

BranchVerdicts GuardEliminator::findCoveredBranches(const TestEvents& events)
{
    BitVector assumed(m_branches.size());
    for (size_t index = 0; index < m_branches.size(); ++index)
        assumed.quickSet(index);

    uint64_t maxNumberOfRounds = m_context.limit(LimitID::MaxGuardEliminationRounds);
    while (m_numberOfRounds < maxNumberOfRounds) {
        ++m_numberOfRounds;

        BitVector blocksWithIgnoredFailingEdge(m_function.blocks.size());
        for (size_t index : assumed)
            blocksWithIgnoredFailingEdge.quickSet(m_branches[index].block.index());

        TestCoverage coverage(TestCoverageInputs { events, GapBudget::unlimited(), &blocksWithIgnoredFailingEdge });
        BranchVerdicts verdicts = classifyBranches(coverage, assumed);
        BitVector stillAssumed = verdicts.redundant;
        stillAssumed.merge(verdicts.unreachable);
        if (stillAssumed == assumed)
            return verdicts;
        assumed = WTF::move(stillAssumed);
    }

    if (!std::exchange(m_didReachRoundLimit, true))
        m_context.gaveUp(LimitID::MaxGuardEliminationRounds);

    BitVector allBranches(m_branches.size());
    for (size_t index = 0; index < m_branches.size(); ++index)
        allBranches.quickSet(index);
    TestCoverage coverage(TestCoverageInputs { events, GapBudget::unlimited(), nullptr });
    return classifyBranches(coverage, allBranches);
}

BranchVerdicts GuardEliminator::classifyBranches(const TestCoverage& coverage, const BitVector& branches) const
{
    BranchVerdicts verdicts;
    for (size_t index : branches) {
        const TestedBranch& branch = m_branches[index];
        bool isReachable = true;
        bool isCovered = true;
        for (uint32_t offset = 0; offset < branch.numberOfRequirements; ++offset) {
            switch (coverage.of(branch.firstRequirement + offset)) {
            case RequirementCoverage::Unknown:
                isReachable = false;
                break;
            case RequirementCoverage::NeedsTest:
            case RequirementCoverage::NeedsRenewal:
                isCovered = false;
                break;
            case RequirementCoverage::Covered:
                break;
            }
        }
        if (!isReachable)
            verdicts.unreachable.set(index);
        else if (isCovered)
            verdicts.redundant.set(index);
    }
    return verdicts;
}

// GuardAvailability does not merge what a test proved with what a switch case or an allocation proved, so one of those stands for the tests of a condition only where no such test remains.
BitVector GuardEliminator::findMixedConditions(const BranchVerdicts& verdicts) const
{
    BitVector mixedConditions;
    if (m_otherProofs.isEmpty())
        return mixedConditions;

    BitVector conditionsInPlay;
    BitVector conditionsStillTested = m_conditionsWithOtherTests;
    for (size_t index = 0; index < m_branches.size(); ++index) {
        const TestedBranch& branch = m_branches[index];
        if (verdicts.redundant.get(index)) {
            for (const TestRequirement& requirement : requirementsOf(branch))
                conditionsInPlay.set(requirement.condition.index());
            continue;
        }
        if (!verdicts.unreachable.get(index) && !renewsGuards(branch))
            conditionsStillTested.set(requirementsOf(branch).front().condition.index());
    }

    for (const ExistingProof& proof : m_otherProofs)
        mixedConditions.set(proof.condition.index());
    mixedConditions.filter(conditionsInPlay);
    mixedConditions.filter(conditionsStillTested);
    return mixedConditions;
}

Vector<ValueID, 4> GuardEliminator::flattenedGuardsOf(ValueID list) const
{
    Vector<ValueID, 4> guards;
    for (ValueID operand : m_function.at(list).guardsOperands()) {
        const Value& guard = m_function.at(operand);
        if (guard.opcode != Opcode::AllGuards) {
            guards.appendIfNotContains(operand);
            continue;
        }
        for (ValueID nested : guard.guardsOperands())
            guards.appendIfNotContains(nested);
    }
    return guards;
}

PassOutcome GuardEliminator::simplifyGuardLists()
{
    Vector<ValueID, 8> lists;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID value : m_function.at(block).values) {
            if (m_function.at(value).opcode == Opcode::AllGuards)
                lists.append(value);
        }
    }
    if (lists.isEmpty())
        return PassOutcome::Unchanged;

    auto isOperandUse = [](const Use& use) {
        return use.kind == Use::Kind::Operand || use.kind == Use::Kind::TerminatorOperand;
    };

    const UseLists& useLists = m_analyses.useLists();
    ValueSubstitution substitution(m_function);
    bool didChange = false;
    for (ValueID list : lists) {
        Vector<ValueID, 4> guards = flattenedGuardsOf(list);
        Value& value = m_function.at(list);
        if (!std::ranges::equal(guards, value.guardsOperands())) {
            value.setGuardsOperands(guards.span());
            didChange = true;
        }
        if (guards.size() != 1)
            continue;

        // An allocation may stand where an operand of kind Guard is wanted, not as an argument for a parameter of type guard.
        if (isGuardValue(m_function, guards.first()) || std::ranges::all_of(useLists.usesOf(list), isOperandUse)) {
            substitution.replace(list, guards.first());
            didChange = true;
        }
    }

    if (didChange)
        m_analyses.didChangeValues();
    substitution.apply(m_function, &m_analyses);
    didChange |= eraseUnusedGuardLists();
    return didChange ? PassOutcome::Changed : PassOutcome::Unchanged;
}

bool GuardEliminator::eraseUnusedGuardLists()
{
    Vector<std::pair<BlockID, BitVector>, 4> unusedLists;
    const UseLists& useLists = m_analyses.useLists();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        BitVector indexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (m_function.at(values[index]).opcode == Opcode::AllGuards && !useLists.useCount(values[index]))
                indexes.set(index);
        }
        if (!indexes.isEmpty())
            unusedLists.append({ block, WTF::move(indexes) });
    }
    if (unusedLists.isEmpty())
        return false;

    IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
    for (const auto& [block, indexes] : unusedLists)
        builder.eraseValues(block, indexes);
    return true;
}

} // anonymous namespace

PassOutcome eliminateGuards(FunctionPassContext& context)
{
    return GuardEliminator(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
