/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTGroupRewriter.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using ArmsOfAgreements = HashMap<const SlotAgreement*, Vector<LayoutSetID, 4>>;

struct Match {
    SelectionGroup group;
    Vector<LayoutSetID, 4> arms;
    bool typeListsReceivers { false };
};

struct Arm {
    LayoutSetID receivers;
    ValueID object;
    ValueID guard;
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

static bool findsOffsetByShape(Opcode opcode, const SlotAgreement& agreement)
{
    return operationsWithOffsetByShape.contains(opcode) && agreement.hasOffsetByShape(storedOf(opcode).has_value());
}

// The arms are tested in this order: first the groups with a layout of the module that the function is written in, for a library handles the objects that it makes;
// then those whose layouts are allocated most often, then those with the most layouts. The first arm takes the members without a slot too.
static Vector<LayoutSetID, 4> createArms(SelectionContext& context, const SlotAgreement& agreement)
{
    const Program& program = context.program();
    const Heat& heat = context.pass().inputs.heat;
    TypeArena& types = context.pass().types;
    ModuleID module = context.function().module;

    Vector<std::tuple<bool, double, size_t>, 4> likelihoods(agreement.groups.size(), [&](size_t index) {
        std::span<const LayoutID> members = agreement.groups[index].members.span();
        bool hasLayoutOfModule = module && std::ranges::any_of(members, [&](LayoutID layout) {
            return program.at(layout).module == module || heat.isAllocatedIn(layout, module);
        });
        double allocations = 0;
        for (LayoutID layout : members)
            allocations += heat.allocationsOf(layout);
        return std::tuple { hasLayoutOfModule, allocations, members.size() };
    });
    Vector<size_t, 4> likeliestFirst(agreement.groups.size(), [](size_t index) {
        return index;
    });
    std::ranges::stable_sort(likeliestFirst, std::ranges::greater { }, [&](size_t index) {
        return likelihoods[index];
    });

    Vector<LayoutSetID, 4> arms;
    arms.reserveInitialCapacity(agreement.groups.size());
    for (size_t index : likeliestFirst) {
        Vector<LayoutID, 8> members(agreement.groups[index].members.span());
        if (arms.isEmpty())
            members.append(agreement.membersWithoutSlot.span());
        arms.append(types.completeLayoutSet(members.span()));
    }
    return arms;
}

// The type of such a receiver holds for good, and so does what is left of it behind a failed test: the last arm needs no test.
static bool typeListsReceivers(SelectionContext& context, const Value& access)
{
    return reliesOf(access.opcode).contains(Proof::Shape) && !proofsNeededFromGuard(context.program(), context.function(), access).contains(Proof::Shape);
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
        const SlotAgreement& agreement = context.slots().agreementOf(program, test.key(), test.receivers());
        if (canCopyIntoArms(context, id) || agreement.groups.size() < 2 || findsOffsetByShape(opcode, agreement))
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
        function.edit(branch).terminator.setConditionOperand(rewriter.copyRootToEndOf(branch));
    if (test.hasOtherUsers)
        rewriter.leaveUnclaimed(test.group.root);
    else
        rewriter.eraseRoot();
}

static void collectMatches(SelectionContext& context, Opcode opcode, ArmsOfAgreements& armsOfAgreements, Vector<Match>& matches)
{
    const Program& program = context.program();
    const Function& function = context.function();
    for (ValueID id : context.candidates(opcode)) {
        const Value& access = function.at(id);
        if (program.at(access.receivers()).members.size() < 2)
            continue;

        const SlotAgreement& agreement = context.slots().agreementOf(program, access.key(), access.receivers());
        if (agreement.groups.size() < 2 || findsOffsetByShape(opcode, agreement) || !canCopyIntoArms(context, id))
            continue;

        SelectionGroup group = groupOfOne(id);
        group.hasSlowPath = true;
        group.endsEveryArmInCopyOfRoot = true;
        if (context.tryClaim(SelectionRuleID::PlannedSlots, group) != ClaimResult::Claimed)
            continue;
        const Vector<LayoutSetID, 4>& arms = armsOfAgreements.ensure(&agreement, [&] {
            return createArms(context, agreement);
        }).iterator->value;
        matches.append(Match { WTF::move(group), arms, typeListsReceivers(context, access) });
    }
}

static ValueID appendArmGuard(IRBuilder& builder, ValueID caseGuard, ValueID previousGuard)
{
    if (!previousGuard)
        return caseGuard;
    const ValueID guards[] { caseGuard, previousGuard };
    return builder.appendAllGuards(guards);
}

static void endArmInCopy(GroupRewriter& rewriter, const Arm& arm)
{
    IRBuilder& builder = rewriter.builder();
    Function& function = builder.function();
    ValueID copy = rewriter.appendOriginals();
    Value& access = function.edit(copy);
    access.setObjectOperand(arm.object);
    access.setGuardOperand(arm.guard);
    access.setReceivers(arm.receivers);
    access.type = resultTypeOf(builder.context().program, function, access, builder.types());
    rewriter.endArm(resultRuleOf(access.opcode) != ResultRule::None ? std::optional(copy) : std::nullopt);
}

// A failed test takes the layouts of its arm out of the type of the object. What has failed every test has the layouts of the last arm, so no arm is left for other objects.
static void dispatchOnType(GroupRewriter& rewriter, const Match& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID object = original.objectOperand();
    ValueID guard = original.guardOperand();
    BlockFrequency frequency = rewriter.function().at(rewriter.rootBlock()).frequency;

    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();
    for (LayoutSetID arm : match.arms.span().first(match.arms.size() - 1)) {
        ValueID test = builder.appendTypeTest(object, Type::forLayoutSet(arm));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, frequency);
        endArmInCopy(rewriter, { arm, branch.provenOnTaken, guard });
        builder.moveToEndOf(branch.notTaken);
        object = branch.provenOnNotTaken;
    }
    endArmInCopy(rewriter, { match.arms.last(), object, guard });
}

// A guard proves the layouts of the receiver, and only the case of a switch gives a guard for fewer of them: every arm is a case.
static void dispatchOnShape(GroupRewriter& rewriter, const Match& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID object = original.objectOperand();
    ValueID previousGuard = original.guardOperand();
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
        endArmInCopy(rewriter, { match.arms[index], object, appendArmGuard(builder, caseGuards[index], previousGuard) });
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
    ArmsOfAgreements armsOfAgreements;
    for (Opcode opcode : slotOperations)
        collectMatches(context, opcode, armsOfAgreements, matches);

    context.rewriteGroups(SelectionRuleID::PlannedSlots, matches, [](GroupRewriter& rewriter, const Match& match) {
        if (match.typeListsReceivers)
            dispatchOnType(rewriter, match);
        else
            dispatchOnShape(rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
