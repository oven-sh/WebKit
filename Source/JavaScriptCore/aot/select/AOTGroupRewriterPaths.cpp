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
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<size_t> findMember(const SelectionGroup& group, ValueID id)
{
    size_t index = group.members.findIf([&](const GroupMember& member) {
        return member.value == id;
    });
    if (index == notFound)
        return std::nullopt;
    return index;
}

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

static void unfuseBranch(Terminator& terminator, ValueID condition)
{
    if (terminator.kind == TerminatorKind::TestAndBranch && terminator.conditionOperand() == condition)
        changeBranchKind(terminator, TerminatorKind::Branch);
}

BlockFrequency GroupRewriter::slowPathFrequency() const
{
    return BlockFrequency::Rare;
}

void GroupRewriter::moveRootToBlockEnd()
{
    flushInsertions();
    BlockValues& values = function().edit(state().block).values;
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

    Terminator& branch = function().edit(state().block).terminator;
    RELEASE_ASSERT(branch.kind == TerminatorKind::Branch && branch.conditionOperand() == m_group.root);
    changeBranchKind(branch, TerminatorKind::TestAndBranch);
    analyses().didChangeBlock(state().block);
}

void GroupRewriter::emitTestAndBranchAsValue()
{
    Value test = function().at(m_group.root);
    RELEASE_ASSERT(branchesOf(test.opcode) != Branches::No);
    BlockFrequency frequency = function().at(state().block).frequency;

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
    unfuseBranch(function().edit(m_expansion->continuation).terminator, m_group.root);
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

void GroupRewriter::emitTwoWay(Terminator&& fast)
{
    RELEASE_ASSERT(m_group.hasSlowPath && fast.kind == TerminatorKind::TryCallInline && fast.targets.isEmpty());
    RELEASE_ASSERT(!hasMemberUsedOutside(m_group));
    RELEASE_ASSERT(hasResult(m_originals.last()) && !numberOfResultsOf(m_session.m_context.program(), function(), m_originals.last()));

    expandRoot();
    BlockID head = builder().currentBlock();
    BlockID miss = addArm(slowPathFrequency());
    fast.targets.append(Target { m_expansion->continuation, { }, { } });
    fast.targets.append(Target { miss, { }, { } });
    builder().emitTerminator(WTF::move(fast));
    m_blocksWithCreatedTerminators.append(head);

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

BlockID GroupRewriter::addArm(BlockFrequency frequency)
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
