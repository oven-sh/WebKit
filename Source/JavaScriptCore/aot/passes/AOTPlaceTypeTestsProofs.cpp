/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Edge takenEdgeOf(const Function& function, BlockID block)
{
    const Terminator& terminator = function.at(block).terminator;
    RELEASE_ASSERT(terminator.isConditionalBranch());
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        if (&terminator.targets[targetIndex] == &terminator.taken())
            return Edge { block, targetIndex };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofEnding endingBeforeBranch(FunctionAnalyses& analyses, const TestCondition& condition, ProgramPoint test)
{
    if (condition.lastsForever())
        return ProofEnding::DoesNotEnd;

    const BlockValues& values = analyses.function().at(test.block).values;
    for (uint32_t index = test.index + 1; index < values.size(); ++index) {
        const EffectsOnEdges& effects = analyses.effectsOf(values[index]);
        if (effects.normal.maySuspend || endingBetweenTestAndBranch(analyses, condition, values[index], effects) == ProofEnding::MayEnd)
            return ProofEnding::MayEnd;
    }
    return ProofEnding::DoesNotEnd;
}

static void appendTakenEdgeProof(FunctionAnalyses& analyses, const TestConditions& conditions, BlockID block, Vector<ExistingProof>& proofs)
{
    const Function& function = analyses.function();
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.taken().block == terminator.notTaken().block)
        return;

    ValueID test = terminator.conditionOperand();
    if (provesOf(function.at(test).opcode) == Proof::None)
        return;
    std::optional<TestConditionID> condition = conditions.find(function.at(test));
    if (!condition)
        return;

    ValueLocation location = analyses.useLists().locationOf(test);
    if (location.placement != ValuePlacement::InBlock || location.block != block)
        return;
    if (endingBeforeBranch(analyses, conditions.at(*condition), { block, location.index }) == ProofEnding::MayEnd)
        return;
    proofs.append(ExistingProof { ProofSource::TakenEdge, *condition, test, takenEdgeOf(function, block) });
}

static void appendSwitchCaseProofs(const Function& function, const TestConditions& conditions, BlockID block, Vector<ExistingProof>& proofs)
{
    const Terminator& terminator = function.at(block).terminator;
    std::span<const LayoutSetID> layoutSets = terminator.layoutSets();
    for (uint32_t caseIndex = 0; caseIndex < layoutSets.size(); ++caseIndex) {
        std::optional<TestConditionID> condition = conditions.find(makeShapeTest(terminator.subjectOperand(), layoutSets[caseIndex]));
        if (condition)
            proofs.append(ExistingProof { ProofSource::SwitchCase, *condition, { }, Edge { block, caseIndex } });
    }
}

static void appendEdgeProofs(FunctionAnalyses& analyses, const TestConditions& conditions, Vector<ExistingProof>& proofs)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.isConditionalBranch())
            appendTakenEdgeProof(analyses, conditions, block, proofs);
        else if (terminator.kind == TerminatorKind::SwitchOnShape)
            appendSwitchCaseProofs(function, conditions, block, proofs);
    }
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

static void appendProofsOfGuard(FunctionAnalyses& analyses, const TestConditions& conditions, NOESCAPE const Invocable<const GuardAvailability::Cursor&()> auto& cursorAfterDefinition, ProgramPoint afterDefinition, ValueID guard, Vector<ExistingProof>& proofs)
{
    const GuardFacts& facts = analyses.guardFacts(guard);
    if (!facts.isKnown())
        return;

    size_t firstProofOfGuard = proofs.size();
    for (const GuardFact& fact : facts.facts) {
        std::optional<TestConditionID> condition = findConditionOf(analyses.function(), conditions, fact);
        if (!condition)
            continue;
        bool isRepeated = std::ranges::any_of(proofs.span().subspan(firstProofOfGuard), [&](const ExistingProof& proof) {
            return proof.condition == *condition;
        });
        if (isRepeated)
            continue;

        // A parameter or a list may hold a guard that has ended.
        const TestCondition& tested = conditions.at(*condition);
        std::optional<GuardState> state = analyses.guards().stateSeenWithoutSolving(guard, ProofSet { tested.proof }, tested.subject, afterDefinition);
        if (!state)
            state = cursorAfterDefinition().stateOf(guard, ProofSet { tested.proof }, tested.subject);
        if (*state != GuardState::Available)
            continue;
        proofs.append(ExistingProof { ProofSource::GuardValue, *condition, guard, { } });
    }
}

static void appendGuardValueProofs(FunctionAnalyses& analyses, const TestConditions& conditions, Vector<ExistingProof>& proofs)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Block& contents = function.at(block);
        std::optional<GuardAvailability::Cursor> cursor;
        auto appendProofsOf = [&](ValueID value, uint32_t indexAfterDefinition) {
            if (!isGuardValue(function, value))
                return;
            // Few guards prove a condition of this plan, and the state of the guards at the head of a block is solved for.
            auto cursorAfterDefinition = [&]() -> const GuardAvailability::Cursor& {
                if (!cursor)
                    cursor.emplace(analyses.guards(), block);
                cursor->advanceTo(indexAfterDefinition);
                return *cursor;
            };
            appendProofsOfGuard(analyses, conditions, cursorAfterDefinition, { block, indexAfterDefinition }, value, proofs);
        };

        for (ValueID parameter : contents.parameters)
            appendProofsOf(parameter, 0);
        for (uint32_t index = 0; index < contents.values.size(); ++index)
            appendProofsOf(contents.values[index], index + 1);
    }
}

static void appendAllocationProofs(FunctionPassContext& context, const TestConditions& conditions, Vector<ExistingProof>& proofs)
{
    for (size_t index : conditions.stretchConditions()) {
        TestConditionID id(index);
        const TestCondition& condition = conditions.at(id);
        if (!condition.subject)
            continue;
        if (allocationProofOf(context.program, context.function, context.function.at(condition.subject), condition) == AllocationProof::Proves)
            proofs.append(ExistingProof { ProofSource::Allocation, id, condition.subject, { } });
    }
}

Vector<ExistingProof> findExistingProofs(FunctionPassContext& context, const TestConditions& conditions, ProofsHeldByGuards heldByGuards)
{
    Vector<ExistingProof> proofs;
    appendEdgeProofs(context.analyses, conditions, proofs);
    if (conditions.stretchConditions().isEmpty())
        return proofs;

    if (heldByGuards == ProofsHeldByGuards::AreListed)
        appendGuardValueProofs(context.analyses, conditions, proofs);
    appendAllocationProofs(context, conditions, proofs);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
