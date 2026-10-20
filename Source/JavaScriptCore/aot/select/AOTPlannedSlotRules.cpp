/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTGroupRewriter.h"
#include "AOTIRBuilder.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Match {
    SelectionGroup group;
    Vector<LayoutSetID, 4> arms;
};

struct RepeatedTest {
    SelectionGroup group;
    Vector<BlockID, 2> branches;
    bool hasOtherUsers { false };
};

} // anonymous namespace

static constexpr uint32_t guardProofEntry = 0;

static constexpr bool isSlotOperation(Opcode opcode)
{
    return Value::opcodesWithFieldAccess.contains(opcode);
}

static constexpr size_t numberOfSlotOperations = std::ranges::count_if(allMembersOf<Opcode>, isSlotOperation);

static constexpr std::array<Opcode, numberOfSlotOperations> slotOperations = [] {
    std::array<Opcode, numberOfSlotOperations> operations { };
    std::ranges::copy_if(allMembersOf<Opcode>, operations.begin(), isSlotOperation);
    return operations;
}();

static size_t findLargestGroup(const SlotAgreement& agreement)
{
    size_t largest = 0;
    for (size_t index = 1; index < agreement.groups.size(); ++index) {
        if (agreement.groups[index].members.size() > agreement.groups[largest].members.size())
            largest = index;
    }
    return largest;
}

static Vector<LayoutSetID, 4> createArms(TypeArena& types, const SlotAgreement& agreement)
{
    size_t largest = findLargestGroup(agreement);
    Vector<LayoutSetID, 4> arms;
    arms.reserveInitialCapacity(agreement.groups.size());
    for (size_t index = 0; index < agreement.groups.size(); ++index) {
        Vector<LayoutID, 8> members(agreement.groups[index].members.span());
        if (index == largest)
            members.append(agreement.membersWithoutSlot.span());
        arms.append(types.completeLayoutSet(members.span()));
    }
    return arms;
}

static bool canCopyIntoArms(SelectionContext& context, ValueID id)
{
    if (provesOf(context.function().at(id).opcode) == Proof::None)
        return true;
    // A branch passes the proofs of the test it names, so every copy of the test needs a copy of the branch.
    return context.isOnlyBranchedOn(id) || !context.isConditionWithProofs(id);
}

static bool passesProofsOfCondition(SelectionContext& context, const Use& use)
{
    return use.kind == Use::Kind::TerminatorOperand && context.function().at(use.block).terminator.isConditionalBranch() && context.passesProofs(use.block);
}

// A proof is available behind the branch only where nothing between the test and the branch may end it. There the test finds in front of the branch what it found where it stands.
static bool canRepeatInFrontOf(SelectionContext& context, ValueID test, BlockID branch)
{
    FunctionAnalyses& analyses = context.analyses();
    const Function& function = context.function();
    const Value& value = function.at(test);
    ValueID guard = value.guardOperand();
    if (guard && analyses.guards().stateAt(guard, value, pointBeforeTerminatorOf(function, branch)) != GuardState::Available)
        return false;

    bool everyProofIsAvailable = true;
    forEachEdgeResult(function, branch, [&](const EdgeResult& result, ValueID parameter) {
        BlockID target = analyses.useLists().locationOf(parameter).block;
        everyProofIsAvailable &= result.kind == EdgeResultKind::Guard && analyses.isGuardAvailable(parameter, ProgramPoint::atHeadOf(target));
    });
    return everyProofIsAvailable;
}

static void collectRepeatedTests(SelectionContext& context, Opcode opcode, Vector<RepeatedTest>& tests)
{
    if (provesOf(opcode) == Proof::None)
        return;

    const Program& program = context.program();
    for (ValueID id : context.candidates(opcode)) {
        const Value& test = context.function().at(id);
        if (canCopyIntoArms(context, id) || context.slots().agreementOf(program, test.key(), test.receivers()).groups.size() < 2)
            continue;

        RepeatedTest repeated { groupOfOne(id), { }, false };
        for (const Use& use : context.analyses().useLists().usesOf(id)) {
            if (passesProofsOfCondition(context, use))
                repeated.branches.append(use.block);
            else
                repeated.hasOtherUsers = true;
        }
        bool canRepeat = std::ranges::all_of(repeated.branches, [&](BlockID branch) {
            return canRepeatInFrontOf(context, id, branch);
        });
        if (canRepeat && context.tryClaim(SelectionRuleID::PlannedSlots, repeated.group) == ClaimResult::Claimed)
            tests.append(WTF::move(repeated));
    }
}

static void repeatInFrontOfBranches(GroupRewriter& rewriter, const RepeatedTest& test)
{
    Function& function = rewriter.function();
    for (BlockID branch : test.branches)
        function.at(branch).terminator.setConditionOperand(rewriter.copyRootToEndOf(branch));
    if (test.hasOtherUsers)
        rewriter.leaveUnclaimed(test.group.root);
    else
        rewriter.eraseRoot();
}

static void collectMatches(SelectionContext& context, Opcode opcode, Vector<Match>& matches)
{
    const Program& program = context.program();
    const Function& function = context.function();
    for (ValueID id : context.candidates(opcode)) {
        const Value& access = function.at(id);
        std::span<const LayoutID> receivers = program.at(access.receivers()).members.span();
        if (receivers.size() < 2)
            continue;

        SlotAgreement agreement = context.slots().agreementOf(program, access.key(), receivers);
        if (agreement.groups.size() < 2 || !canCopyIntoArms(context, id))
            continue;

        SelectionGroup group = groupOfOne(id);
        group.hasSlowPath = true;
        group.endsEveryArmInCopyOfRoot = true;
        if (context.tryClaim(SelectionRuleID::PlannedSlots, group) != ClaimResult::Claimed)
            continue;
        matches.append(Match { WTF::move(group), createArms(context.pass().types, agreement) });
    }
}

static ValueID appendArmGuard(IRBuilder& builder, ValueID caseGuard, ValueID previousGuard)
{
    if (!previousGuard)
        return caseGuard;
    const ValueID guards[] { caseGuard, previousGuard };
    return builder.appendAllGuards(guards);
}

static void narrowToArm(IRBuilder& builder, ValueID copy, LayoutSetID receivers, ValueID guard)
{
    Function& function = builder.function();
    Value& access = function.at(copy);
    access.setReceivers(receivers);
    access.setGuardOperand(guard);
    access.type = resultTypeOf(builder.context().program, function, access, builder.types());
}

static void rewrite(GroupRewriter& rewriter, const Match& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID object = original.objectOperand();
    ValueID previousGuard = original.guardOperand();
    bool hasResult = resultRuleOf(original.opcode) != ResultRule::None;
    BlockFrequency frequency = rewriter.function().at(rewriter.rootBlock()).frequency;

    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();
    BlockID head = builder.currentBlock();

    Vector<BlockID, 4> blocks;
    Vector<std::pair<LayoutSetID, Target>> cases;
    cases.reserveInitialCapacity(match.arms.size());
    for (LayoutSetID arm : match.arms) {
        blocks.append(rewriter.addArm(frequency));
        cases.append(std::pair { arm, Target { blocks.last(), { }, { } } });
    }
    BlockID unreachable = rewriter.addArm(BlockFrequency::Rare);
    builder.emitSwitchOnShape(object, WTF::move(cases), Target { unreachable, { }, { } });

    // addEdgeResult reads the CFG, and closing a block invalidates it.
    Vector<ValueID, 4> caseGuards;
    for (uint32_t index = 0; index < blocks.size(); ++index)
        caseGuards.append(builder.addEdgeResult(Edge { head, index }, guardProofEntry));

    for (uint32_t index = 0; index < blocks.size(); ++index) {
        builder.moveToEndOf(blocks[index]);
        ValueID guard = appendArmGuard(builder, caseGuards[index], previousGuard);
        ValueID copy = rewriter.appendOriginals();
        narrowToArm(builder, copy, match.arms[index], guard);
        rewriter.endArm(hasResult ? std::optional(copy) : std::nullopt);
    }

    builder.moveToEndOf(unreachable);
    builder.emitUnreachable();
}

void selectPlannedSlots(SelectionContext& context)
{
    // Each branch that passes the proofs of a test gets a test of its own, which the arms can copy together with the branch.
    Vector<RepeatedTest> repeatedTests;
    for (Opcode opcode : slotOperations)
        collectRepeatedTests(context, opcode, repeatedTests);
    context.rewriteGroups(SelectionRuleID::PlannedSlots, repeatedTests, [](GroupRewriter& rewriter, const RepeatedTest& test) {
        repeatInFrontOfBranches(rewriter, test);
    });

    Vector<Match> matches;
    for (Opcode opcode : slotOperations)
        collectMatches(context, opcode, matches);

    context.rewriteGroups(SelectionRuleID::PlannedSlots, matches, [](GroupRewriter& rewriter, const Match& match) {
        rewrite(rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
