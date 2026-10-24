/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTGroupRewriter.h"
#include "AOTGuardFactDetails.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
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

struct PlannedSlotMatch {
    SelectionGroup group;
    Vector<LayoutSetID, 4> arms;
    bool typeListsReceivers { false };
    ValueID follower;
    bool followerIsOnlyReturned { false };
};

struct Arm {
    LayoutSetID receivers;
    ValueID object;
    ValueID caseGuard;
    ValueID previousGuard;
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

// The objects of a class that extends a class of the module are the module's as much as those that it makes itself: the methods of a base class mostly meet its heirs.
static bool belongsTo(const Program& program, const Heat& heat, LayoutID layout, ModuleID module)
{
    for (LayoutID current = declaredLayoutOf(program, layout); current; current = expectedBaseOf(program.at(current))) {
        if (program.at(current).module == module || heat.isAllocatedIn(current, module))
            return true;
    }
    return heat.isAllocatedIn(layout, module);
}

// The arms are tested in this order: first the groups with a layout of the module that the function is written in, for a library handles the objects that it makes.
static Vector<LayoutSetID, 4> createArms(SelectionContext& context, const SlotAgreement& agreement)
{
    const Program& program = context.program();
    const Heat& heat = context.pass().inputs.heat;
    TypeArena& types = context.pass().types;
    ModuleID module = context.function().module;

    Vector<std::tuple<bool, double, size_t>, 4> likelihoods(agreement.groups.size(), [&](size_t index) {
        std::span<const LayoutID> members = agreement.groups[index].members.span();
        bool hasLayoutOfModule = module && std::ranges::any_of(members, [&](LayoutID layout) {
            return belongsTo(program, heat, layout, module);
        });
        double allocations = 0;
        for (LayoutID layout : members)
            allocations += heat.allocationsOf(layout);
        return std::tuple { hasLayoutOfModule, allocations, members.size() };
    });
    Vector<size_t, 4> likeliestFirst(agreement.groups.size(), [](size_t index) {
        return index;
    });
    AOT::stableSort(likeliestFirst, std::ranges::greater { }, [&](size_t index) {
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

static std::optional<Vector<GuardDependencies, 2>> dependenciesOfTest(FunctionAnalyses& analyses, const Value& test)
{
    Vector<GuardDependencies, 2> dependencies;
    if (std::optional<GuardDependencies> read = dependenciesOfGuardOf(analyses, test)) {
        // A store that fills the slot leaves the proof as it is and changes the answer.
        read->slotState = TestedSlotState::None;
        dependencies.append(WTF::move(*read));
    }
    if (!test.guardOperand())
        return dependencies;

    const GuardFacts& facts = analyses.guards().factsOf(test.guardOperand());
    if (!facts.isKnown())
        return std::nullopt;
    ProofSet needed = proofsNeededFromGuard(analyses.program(), analyses.function(), test);
    ValueID subject = analyses.identityOf(guardedOperandOf(test));
    for (const GuardFact& fact : facts.facts) {
        bool isNeeded = fact.subject == subject && std::ranges::any_of(allMembersOf<Proof>, [&](Proof proof) {
            return needed.contains(proof) && provesForUser(analyses, fact, proof, test);
        });
        if (!isNeeded)
            continue;
        GuardDependencies relied(analyses.program(), Vector<MemoryLocation, 2>(fact.dependsOn));
        relied.slotState = slotStateProvenBy(analyses.function(), fact);
        relied.subject = exclusiveSubjectOf(analyses, fact);
        dependencies.append(WTF::move(relied));
    }
    return dependencies;
}

// Each writer between the test and the branch is asked by itself: GuardAvailability leaves guards out of large functions, and placement refuses a test that stays over several places.
static bool canRepeatInFrontOf(SelectionContext& context, ValueID test, BlockID branch)
{
    FunctionAnalyses& analyses = context.analyses();
    const Function& function = context.function();
    std::optional<Vector<GuardDependencies, 2>> dependencies = dependenciesOfTest(analyses, function.at(test));
    if (!dependencies)
        return false;

    auto mayWrite = [&](BlockID block, uint32_t firstIndex) {
        return std::ranges::any_of(function.at(block).values.span().subspan(firstIndex), [&](ValueID value) {
            GuardWriter writer(analyses, value);
            return std::ranges::any_of(*dependencies, [&](const GuardDependencies& relied) {
                return writer.mayEnd(relied);
            });
        });
    };
    ValueLocation location = analyses.useLists().locationOf(test);
    for (BlockID block = branch; block != location.block;) {
        std::optional<Edge> onlyWayIn = analyses.cfg().singleIncomingEdge(block);
        if (!onlyWayIn || onlyWayIn->isHandlerEdge() || mayWrite(block, 0))
            return false;
        block = onlyWayIn->from;
        const Effects& effects = analyses.effectsOfTerminator(block).normal;
        bool terminatorMayWrite = effects.maySuspend || std::ranges::any_of(*dependencies, [&](const GuardDependencies& relied) {
            return effects.writes.overlaps(relied.masks);
        });
        if (terminatorMayWrite)
            return false;
    }
    return !mayWrite(location.block, location.index + 1);
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
        function.edit(branch, BlockEdit::Terminator).terminator.setConditionOperand(rewriter.copyRootToEndOf(branch));
    if (test.hasOtherUsers)
        rewriter.leaveUnclaimed(test.group.root);
    else
        rewriter.eraseRoot();
}

// A store asks whether the slot is filled, then whether the value fits, and both go the same way when the answer is no. Asked the other way round, the test of the slot stands right in front of the store.
static void testStoredValuesFirst(SelectionContext& context)
{
    const Program& program = context.program();
    Function& function = context.pass().function;
    FunctionAnalyses& analyses = context.analyses();
    bool didChange = false;
    for (ValueID id : context.candidates(Opcode::IsSlotFilled)) {
        const Value& test = function.at(id);
        if (context.slots().agreementOf(program, test.key(), test.receivers()).groups.size() < 2 || !context.isOnlyBranchedOn(id))
            continue;

        BlockID first = analyses.useLists().blockOf(id);
        const Block& slotIsTested = function.at(first);
        BlockID second = slotIsTested.terminator.taken().block;
        const Block& valueIsTested = function.at(second);
        if (slotIsTested.values.last() != id || valueIsTested.values.size() != 1 || valueIsTested.terminator.kind != TerminatorKind::Branch || valueIsTested.handler != slotIsTested.handler)
            continue;
        ValueID fits = valueIsTested.values.first();
        BlockID third = valueIsTested.terminator.taken().block;
        if (function.at(fits).opcode != Opcode::TypeTest || !context.isOnlyBranchedOn(fits) || slotIsTested.terminator.notTaken() != valueIsTested.terminator.notTaken())
            continue;
        if (analyses.cfg().incomingEdges(second).size() != 1 || analyses.cfg().incomingEdges(third).size() != 1)
            continue;
        if (!slotIsTested.terminator.taken().arguments.isEmpty() || !valueIsTested.terminator.taken().arguments.isEmpty() || !slotIsTested.terminator.notTaken().proofs.isEmpty())
            continue;

        Block& front = function.edit(first);
        Block& middle = function.edit(second);
        Block& behind = function.edit(third);
        std::swap(front.values.last(), middle.values.last());
        front.terminator.setConditionOperand(fits);
        middle.terminator.setConditionOperand(id);
        std::swap(front.terminator.taken().proofs, middle.terminator.taken().proofs);
        std::swap(middle.parameters, behind.parameters);
        didChange = true;
        analyses.didChangeControlFlow();
    }
    if (didChange)
        analyses.didChangeValues();
}

// An access to the slot that a test has just found filled. The arm that knows where the slot lies for the test knows it for the access.
static ValueID findFollower(SelectionContext& context, ValueID id, const SlotAgreement& agreement)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    const Value& test = function.at(id);
    if (provesOf(test.opcode) == Proof::None || !context.isOnlyBranchedOn(id))
        return { };

    const Block& from = function.at(analyses.useLists().blockOf(id));
    const Target& taken = from.terminator.taken();
    const Block& entered = function.at(taken.block);
    if (!taken.arguments.isEmpty() || taken.proofs.size() != 1 || entered.values.isEmpty() || entered.handler != from.handler)
        return { };
    if (analyses.cfg().incomingEdges(taken.block).size() != 1)
        return { };

    size_t index = entered.values.findIf([&](ValueID earlier) {
        Opcode opcode = function.at(earlier).opcode;
        return opcode != Opcode::AllGuards && resultRuleOf(opcode) != ResultRule::Constant;
    });
    if (index == notFound)
        return { };
    ValueID follower = entered.values[index];
    const Value& access = function.at(follower);
    if (!isSlotOperation(access.opcode) || provesOf(access.opcode) != Proof::None || numberOfResultsOf(context.program(), function, access))
        return { };
    if (access.objectOperand() != test.objectOperand() || access.key() != test.key() || access.receivers() != test.receivers())
        return { };

    auto isUsedOnlyUpToFollower = [&](ValueID id) {
        return std::ranges::all_of(analyses.useLists().usesOf(id), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && use.block == taken.block && entered.values.find(use.user) <= index;
        });
    };
    if (!isUsedOnlyUpToFollower(entered.parameters[0]) || !std::ranges::all_of(entered.values.span().first(index), isUsedOnlyUpToFollower))
        return { };
    if (findsOffsetByShape(access.opcode, agreement) || !typeListsReceivers(context, access) || !typeListsReceivers(context, test))
        return { };
    return follower;
}

static void collectMatches(SelectionContext& context, Opcode opcode, ArmsOfAgreements& armsOfAgreements, Vector<PlannedSlotMatch>& matches)
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
        ValueID follower = findFollower(context, id, agreement);
        if (follower && context.tryClaimAll(SelectionRuleID::PlannedSlots, std::array { group, groupOfOne(follower) }) != ClaimResult::Claimed)
            follower = { };
        if (!follower && context.tryClaim(SelectionRuleID::PlannedSlots, group) != ClaimResult::Claimed)
            continue;
        const Vector<LayoutSetID, 4>& arms = armsOfAgreements.ensure(&agreement, [&] {
            return createArms(context, agreement);
        }).iterator->value;
        matches.append(PlannedSlotMatch { WTF::move(group), arms, typeListsReceivers(context, access), follower, follower && context.isOnlyReturned(follower) });
    }
}

static ValueID appendArmGuard(IRBuilder& builder, ValueID caseGuard, ValueID previousGuard)
{
    if (!caseGuard || !previousGuard)
        return caseGuard ? caseGuard : previousGuard;
    const ValueID guards[] { caseGuard, previousGuard };
    return builder.appendAllGuards(guards);
}

static std::optional<ValueID> narrowToArm(IRBuilder& builder, ValueID copy, const Arm& arm, ValueID guard)
{
    Function& function = builder.function();
    Value& access = function.edit(copy);
    access.setObjectOperand(arm.object);
    access.setGuardOperand(guard);
    access.setReceivers(arm.receivers);
    access.type = resultTypeOf(builder.context().program, function, access, builder.types());
    return resultRuleOf(access.opcode) != ResultRule::None ? std::optional(copy) : std::nullopt;
}

static void endArmInCopy(GroupRewriter& rewriter, const PlannedSlotMatch& match, const Arm& arm)
{
    IRBuilder& builder = rewriter.builder();
    ValueID guard = appendArmGuard(builder, arm.caseGuard, arm.previousGuard);
    std::optional<ValueID> result = narrowToArm(builder, rewriter.appendOriginals(), arm, guard);
    if (!match.follower) {
        rewriter.endArm(result);
        return;
    }

    rewriter.endArmInFrontOfFollower(*result);
    ValueID follower = rewriter.appendFollower();
    guard = builder.function().at(follower).guardOperand();
    RELEASE_ASSERT(!arm.caseGuard);
    rewriter.endFollower(narrowToArm(builder, follower, arm, guard));
}

static void beginArms(GroupRewriter& rewriter, const PlannedSlotMatch& match)
{
    if (match.follower)
        rewriter.beginArms(match.follower, match.followerIsOnlyReturned);
    else
        rewriter.beginArms();
}

// A failed test takes the layouts of its arm out of the type of the object. What has failed every test has the layouts of the last arm, so no arm is left for other objects.
static void dispatchOnType(GroupRewriter& rewriter, const PlannedSlotMatch& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID object = original.objectOperand();
    ValueID guard = original.guardOperand();
    BlockFrequency frequency = rewriter.function().at(rewriter.rootBlock()).frequency;

    beginArms(rewriter, match);
    IRBuilder& builder = rewriter.builder();
    for (LayoutSetID arm : match.arms.span().first(match.arms.size() - 1)) {
        ValueID test = builder.appendTypeTest(object, Type::forLayoutSet(arm));
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, frequency);
        endArmInCopy(rewriter, match, { arm, branch.provenOnTaken, { }, guard });
        builder.moveToEndOf(branch.notTaken);
        object = branch.provenOnNotTaken;
    }
    endArmInCopy(rewriter, match, { match.arms.last(), object, { }, guard });
}

// A guard proves the layouts of the receiver, and only the case of a switch gives a guard for fewer of them: every arm is a case.
static void dispatchOnShape(GroupRewriter& rewriter, const PlannedSlotMatch& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID object = original.objectOperand();
    ValueID previousGuard = original.guardOperand();
    BlockFrequency frequency = rewriter.function().at(rewriter.rootBlock()).frequency;

    beginArms(rewriter, match);
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
        endArmInCopy(rewriter, match, { match.arms[index], object, caseGuards[index], previousGuard });
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

    testStoredValuesFirst(context);

    Vector<PlannedSlotMatch> matches;
    ArmsOfAgreements armsOfAgreements;
    for (bool isTest : { true, false }) {
        for (Opcode opcode : slotOperations) {
            if ((provesOf(opcode) != Proof::None) == isTest)
                collectMatches(context, opcode, armsOfAgreements, matches);
        }
    }

    context.rewriteGroups(SelectionRuleID::PlannedSlots, matches, [](GroupRewriter& rewriter, const PlannedSlotMatch& match) {
        if (match.typeListsReceivers)
            dispatchOnType(rewriter, match);
        else
            dispatchOnShape(rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
