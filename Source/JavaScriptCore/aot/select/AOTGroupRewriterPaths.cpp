/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGroupRewriter.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static bool hasResult(const Value& value)
{
    return resultRuleOf(value.opcode) != ResultRule::None;
}

static bool hasMemberUsedOutside(const SelectionGroup& group)
{
    return group.members.containsIf([](const GroupMember& member) {
        return member.isUsedOutside;
    });
}

void changeBranchKind(Terminator& terminator, TerminatorKind kind)
{
    Terminator changed(kind);
    changed.setConditionOperand(terminator.conditionOperand());
    changed.setOrigin(terminator.origin());
    changed.targets = WTF::move(terminator.targets);
    terminator = WTF::move(changed);
}

static void unfuseBranchOnPath(Terminator& terminator, ValueID condition)
{
    if (terminator.kind == TerminatorKind::TestAndBranch && terminator.conditionOperand() == condition)
        changeBranchKind(terminator, TerminatorKind::Branch);
}

Expected<BlockFrequency> GroupRewriter::slowPathFrequency() const
{
    return BlockFrequency::Rare;
}

void GroupRewriter::moveRootToBlockEnd()
{
    flushInsertions();
    BlockValues& values = function().edit(state().block, BlockEdit::Values).values;
    if (values.last() == m_group.root)
        return;

    values.removeAt(state().index);
    values.append(m_group.root);
    state().index = static_cast<uint32_t>(values.size() - 1);
    analyses().didChangeBlock(state().block);
}

void GroupRewriter::emitTestAndBranch()
{
    RELEASE_ASSERT(state().isOnlyBranchedOn && branchesOf(function().at(m_group.root).opcode) != Branches::No);
    moveRootToBlockEnd();

    Terminator& branch = function().edit(state().block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    RELEASE_ASSERT(branch.kind == TerminatorKind::Branch && branch.conditionOperand() == m_group.root);
    changeBranchKind(branch, TerminatorKind::TestAndBranch);
    analyses().didChangeBlock(state().block);
}

void GroupRewriter::emitTestAndBranchAsValue()
{
    Value test = function().at(m_group.root);
    RELEASE_ASSERT(branchesOf(test.opcode) != Branches::No);
    Expected<BlockFrequency> frequency = function().at(state().block).frequency;

    beginArms();
    ValueID condition = builder().append(WTF::move(test));
    BlockID taken = addArm(frequency);
    BlockID notTaken = addArm(frequency);
    Terminator branch = IRBuilder::makeTerminator(TerminatorKind::TestAndBranch, { condition });
    branch.targets.append(Target { taken, { }, { } });
    branch.targets.append(Target { notTaken, { }, { } });
    builder().emitTerminator(WTF::move(branch));

    builder().moveToEndOf(taken);
    endArm(builder().appendBoolean(true));
    builder().moveToEndOf(notTaken);
    endArm(builder().appendBoolean(false));
}

void GroupRewriter::endBlockAsUnreachable()
{
    flushInsertions();
    m_session.m_needsCompaction = true;

    BlockID block = state().block;
    for (ValueID id : function().at(block).values.subspan(state().index + 1))
        m_session.m_isErasedFromBlock.set(id.index());
    reopenBlock(block);
    builder().emitUnreachable();
}

void GroupRewriter::expandRoot()
{
    RELEASE_ASSERT(m_group.hasSlowPath && !m_expansion);
    analyses().didChangeBlock(state().block);
    flushInsertions();
    m_expansion = builder().beginExpansion(ProgramPoint { state().block, state().index });
    builder().setHandler(m_expansion->handler);
    unfuseBranchOnPath(function().edit(m_expansion->continuation, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, m_group.root);
    m_session.m_needsCompaction |= m_expansion->results.isEmpty();
    m_didReplaceRoot = false;
}

Vector<uint32_t, 4> GroupRewriter::carriedMembers() const
{
    Vector<uint32_t, 4> carried;
    uint32_t rootIndex = static_cast<uint32_t>(m_group.members.size() - 1);
    if (hasResult(m_originals[rootIndex]))
        carried.append(rootIndex);
    for (uint32_t index = 0; index < rootIndex; ++index) {
        if (m_group.members[index].isUsedOutside)
            carried.append(index);
    }
    return carried;
}

void GroupRewriter::addCarriedParameters(const Vector<uint32_t, 4>& carried)
{
    for (uint32_t index : carried) {
        ValueID member = m_group.members[index].value;
        if (member == m_group.root)
            continue;
        ValueID parameter = builder().addParameter(m_expansion->continuation, m_originals[index].type);
        m_session.substitution().replace(member, parameter);
        m_expansion->results.append(parameter);
    }
}

ValueID GroupRewriter::appendFastResult(ValueID member)
{
    auto result = std::ranges::find(m_fastResults, member, &FastResult::member);
    RELEASE_ASSERT(result != m_fastResults.end());
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_session.m_context.program(), function(), function().at(result->producer));
    RELEASE_ASSERT(numberOfResults && result->index < *numberOfResults);
    return builder().append(makeResultAt(result->producer, result->index));
}

Vector<ValueID, 4> GroupRewriter::appendOriginalMembers()
{
    Vector<ValueID, 4> copies;
    copies.reserveInitialCapacity(m_originals.size());
    for (const Value& saved : m_originals) {
        Value copy = saved;
        copy.forEachOperand([&](ValueID& operand, uint32_t) {
            if (std::optional<size_t> member = findMember(m_group, operand))
                operand = copies[*member];
        });
        builder().setOrigin(copy.origin());
        ValueID id = builder().append(WTF::move(copy));
        m_session.m_slowPathValues.append(id);
        copies.append(id);
    }
    builder().setOrigin(m_originals.last().origin());
    return copies;
}

ValueID GroupRewriter::appendOriginals()
{
    RELEASE_ASSERT(m_expansion);
    return appendOriginalMembers().last();
}

ValueID GroupRewriter::copyRootToEndOf(BlockID block)
{
    RELEASE_ASSERT(!m_expansion && m_originals.size() == 1);
    ValueID copy = builder().insertBefore(pointBeforeTerminatorOf(function(), block), Value(m_originals.last()));
    m_session.m_slowPathValues.append(copy);
    analyses().didChangeBlock(block);
    return copy;
}

void GroupRewriter::emitSlowPath(ValueID succeeded)
{
    if (succeeded == m_group.root) {
        RELEASE_ASSERT(!hasResult(m_originals.last()));
        succeeded = insert(std::exchange(function().edit(m_group.root), m_originals.last()));
        m_didReplaceRoot = false;
    }

    Vector<uint32_t, 4> carried = carriedMembers();
    expandRoot();
    addCarriedParameters(carried);

    IRBuilder::BranchBlocks arms = builder().emitBranch(succeeded, function().at(state().block).frequency, slowPathFrequency());
    builder().moveToEndOf(arms.taken);
    Vector<ValueID, 4> results;
    for (uint32_t index : carried)
        results.append(appendFastResult(m_group.members[index].value));
    builder().endExpansion(*m_expansion, results.span());

    builder().moveToEndOf(arms.notTaken);
    Vector<ValueID, 4> copies = appendOriginalMembers();
    results.shrink(0);
    for (uint32_t index : carried)
        results.append(copies[index]);
    builder().endExpansion(*m_expansion, results.span());
}

void GroupRewriter::emitTwoWay(Terminator&& fast, Vector<Value, 1>&& inFront, Vector<Value, 1>&& behindFastPath)
{
    RELEASE_ASSERT(m_group.hasSlowPath && fast.kind == TerminatorKind::TryCallInline && fast.targets.isEmpty());
    RELEASE_ASSERT(!hasMemberUsedOutside(m_group));
    RELEASE_ASSERT(hasResult(m_originals.last()) && !numberOfResultsOf(m_session.m_context.program(), function(), m_originals.last()));

    expandRoot();
    for (Value& value : inFront)
        builder().append(WTF::move(value));
    BlockID head = builder().currentBlock();
    BlockID miss = addArm(slowPathFrequency());
    BlockID done = m_expansion->continuation;
    ValueID answer;
    if (!behindFastPath.isEmpty()) {
        done = addArm(function().at(head).frequency);
        answer = builder().addParameter(done, m_originals.last().type);
    }
    fast.targets.append(Target { done, { }, { } });
    fast.targets.append(Target { miss, { }, { } });
    builder().emitTerminator(WTF::move(fast));
    m_blocksWithCreatedTerminators.append(head);

    if (answer) {
        builder().moveToEndOf(done);
        for (Value& value : behindFastPath)
            builder().append(WTF::move(value));
        endArm(answer);
    }

    builder().moveToEndOf(miss);
    endArm(appendOriginals());
}

void GroupRewriter::beginArms()
{
    RELEASE_ASSERT(!hasMemberUsedOutside(m_group));
    if (state().canDuplicateTerminator) {
        moveRootToBlockEnd();
        m_duplicatedTerminator = function().at(state().block).terminator;
        RELEASE_ASSERT(m_duplicatedTerminator->operand(0) == m_group.root);
    }
    expandRoot();
}

void GroupRewriter::beginArms(ValueID follower, bool followerIsOnlyReturned)
{
    beginArms();
    RELEASE_ASSERT(m_duplicatedTerminator && m_duplicatedTerminator->isConditionalBranch());
    BlockID head = builder().currentBlock();
    BlockID block = m_duplicatedTerminator->taken().block;
    const Block& entered = function().at(block);
    uint32_t index = static_cast<uint32_t>(entered.values.find(follower));
    RELEASE_ASSERT(index < entered.values.size() && entered.parameters.size() == 1 && entered.handler == m_expansion->handler);

    std::optional<Terminator> duplicatedTerminator;
    if (followerIsOnlyReturned)
        duplicatedTerminator = entered.terminator;
    Expected<BlockFrequency> frequency = entered.frequency;
    ValueID handedOver = entered.parameters[0];
    Vector<ValueID, 2> earlierValues(entered.values.span().first(index));
    Vector<Value, 3> originals;
    for (ValueID id : entered.values.span().first(index + 1))
        originals.append(function().at(id));
    analyses().didChangeBlock(block);
    m_follower = Follower { follower, block, frequency, handedOver, WTF::move(earlierValues), WTF::move(originals), builder().beginExpansion(ProgramPoint { block, index }), WTF::move(duplicatedTerminator) };
    function().edit(block, BlockEdit::Values).values.shrink(0);

    builder().setOrigin(m_originals.last().origin());
    builder().moveToEndOf(head);
}

BlockID GroupRewriter::addArm(Expected<BlockFrequency> frequency)
{
    RELEASE_ASSERT(m_expansion);
    return builder().addBlock(frequency);
}

void GroupRewriter::endArm(std::optional<ValueID> result)
{
    RELEASE_ASSERT(m_expansion);
    if (!m_duplicatedTerminator) {
        if (result)
            builder().endExpansion(*m_expansion, *result);
        else
            builder().endExpansion(*m_expansion, std::span<const ValueID> { });
        return;
    }

    RELEASE_ASSERT(result);
    Terminator terminator = *m_duplicatedTerminator;
    terminator.setOperand(0, *result);
    Origin armOrigin = builder().origin();
    builder().setOrigin(terminator.origin());
    builder().emitTerminator(WTF::move(terminator));
    builder().setOrigin(armOrigin);
}

void GroupRewriter::endArmInFrontOfFollower(ValueID result)
{
    RELEASE_ASSERT(m_follower);
    Terminator branch = *m_duplicatedTerminator;
    branch.setOperand(0, result);
    RELEASE_ASSERT(branch.taken().arguments.isEmpty() && branch.taken().proofs.size() == 1);
    uint32_t proof = branch.taken().proofs[0];

    BlockID from = builder().currentBlock();
    BlockID entered = addArm(m_follower->frequency);
    branch.taken() = Target { entered, { }, { } };
    builder().setOrigin(branch.origin());
    builder().emitTerminator(WTF::move(branch));

    builder().setOrigin(m_follower->originals.last().origin());
    m_handedOverInArm = builder().addEdgeResult(Edge { from, 0 }, proof);
    builder().moveToEndOf(entered);
}

ValueID GroupRewriter::appendFollower()
{
    RELEASE_ASSERT(m_follower && m_handedOverInArm);
    Vector<ValueID, 3> copies;
    for (const Value& saved : m_follower->originals) {
        Value copy = saved;
        copy.forEachOperand([&](ValueID& operand, uint32_t) {
            if (operand == m_follower->handedOver)
                operand = m_handedOverInArm;
            else if (size_t earlier = m_follower->earlierValues.find(operand); earlier != notFound)
                operand = copies[earlier];
        });
        builder().setOrigin(copy.origin());
        copies.append(builder().append(WTF::move(copy)));
    }
    m_session.m_unclaimedValues.append(copies.span().first(copies.size() - 1));
    m_copiesOfEarlierValues.append(copies.span().first(copies.size() - 1));
    m_session.m_slowPathValues.append(copies.last());
    return copies.last();
}

void GroupRewriter::endFollower(std::optional<ValueID> result)
{
    RELEASE_ASSERT(m_follower);
    if (m_follower->duplicatedTerminator) {
        Terminator terminator = *m_follower->duplicatedTerminator;
        terminator.setOperand(0, *result);
        builder().setOrigin(terminator.origin());
        builder().emitTerminator(WTF::move(terminator));
    } else if (result)
        builder().endExpansion(m_follower->expansion, *result);
    else
        builder().endExpansion(m_follower->expansion, std::span<const ValueID> { });
    builder().setOrigin(m_originals.last().origin());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
