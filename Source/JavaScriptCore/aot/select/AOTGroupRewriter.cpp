/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGroupRewriter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTSelectionContext.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static void unfuseBranch(Terminator& terminator, ValueID condition)
{
    if (terminator.kind != TerminatorKind::TestAndBranch || terminator.conditionOperand() != condition)
        return;

    Terminator branch(TerminatorKind::Branch);
    branch.setConditionOperand(condition);
    branch.setOrigin(terminator.origin());
    branch.targets = WTF::move(terminator.targets);
    terminator = WTF::move(branch);
}

GroupRewriter::GroupRewriter(RewriteSession& session, const SelectionGroup& group, uint32_t groupIndex)
    : m_session(session)
    , m_group(group)
    , m_groupIndex(groupIndex)
    , m_firstCreatedValue(static_cast<uint32_t>(session.m_function.values.size()))
    , m_firstSlowPathValue(static_cast<uint32_t>(session.m_slowPathValues.size()))
    , m_firstCreatedBlock(static_cast<uint32_t>(session.m_function.blocks.size()))
{
    RELEASE_ASSERT(!group.members.isEmpty() && group.members.last().value == group.root);
    m_originals.reserveInitialCapacity(group.members.size());
    for (const GroupMember& member : group.members)
        m_originals.append(function().at(member.value));

    builder().setOrigin(m_originals.last().origin());
    builder().setHandler(function().at(state().block).handler);
}

FunctionAnalyses& GroupRewriter::analyses()
{
    return m_session.m_context.analyses();
}

const Value& GroupRewriter::original(ValueID member) const
{
    std::optional<size_t> index = findMember(m_group, member);
    RELEASE_ASSERT(index);
    return m_originals[*index];
}

void GroupRewriter::updateRootIndex()
{
    const BlockValues& values = function().at(state().block).values;
    uint32_t index = state().index;
    while (index < values.size() && values[index] != m_group.root)
        ++index;
    RELEASE_ASSERT(index < values.size());
    state().index = index;
}

void GroupRewriter::flushInsertions()
{
    m_session.applyInsertions();
    updateRootIndex();
}

void GroupRewriter::reopenBlock(BlockID block)
{
    function().edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    analyses().didChangeControlFlow(singleElementSpan(block));
    builder().moveToEndOf(block);
}

ValueID GroupRewriter::insert(Value&& value)
{
    RELEASE_ASSERT(!m_expansion);
    updateRootIndex();
    return builder().insertBefore(ProgramPoint { state().block, state().index }, WTF::move(value));
}

Value& GroupRewriter::replaceRoot(Opcode opcode)
{
    RELEASE_ASSERT(!m_expansion);
    Value& value = function().edit(m_group.root);
    value.become(opcode);
    m_didReplaceRoot = true;
    if (branchesOf(opcode) == Branches::No)
        unfuseBranch(function().edit(state().block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, m_group.root);
    analyses().didChangeBlock(state().block);
    return value;
}

void GroupRewriter::eraseRoot()
{
    RELEASE_ASSERT(!m_expansion && !m_didReplaceRoot);
    m_session.m_isErasedFromBlock.set(m_group.root.index());
    m_session.m_needsCompaction = true;
}

void GroupRewriter::replaceWithResult(ValueID member, ValueID producer, uint32_t index)
{
    RELEASE_ASSERT(findMember(m_group, member));
    if (m_group.hasSlowPath) {
        m_fastResults.append(FastResult { member, producer, index });
        return;
    }

    Value& value = function().edit(member);
    value.become(Opcode::ResultAt);
    value.setOperand(OperandIndex::ResultAt::results, producer);
    value.setIndex(index);
    analyses().didChangeBlock(state().block);
    if (member == m_group.root) {
        m_didReplaceRoot = true;
        unfuseBranch(function().edit(state().block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, m_group.root);
        return;
    }

    m_session.m_isErasedFromBlock.set(member.index());
    m_session.m_resultsAfter.add(m_group.root, Vector<ValueID, 2> { }).iterator->value.append(member);
}

void GroupRewriter::replaceWithValue(ValueID member, ValueID replacement)
{
    RELEASE_ASSERT(findMember(m_group, member) && !m_group.hasSlowPath);
    m_session.substitution().replace(member, replacement);
    m_session.m_isErasedFromBlock.set(member.index());
    m_session.m_needsCompaction = true;
    if (member == m_group.root) {
        m_didReplaceRoot = false;
        unfuseBranch(function().edit(state().block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, m_group.root);
    }

    TypeID previous = function().at(member).type;
    TypeID narrowed = function().at(replacement).type;
    if (previous == narrowed)
        return;
    SelectionContext& context = m_session.m_context;
    const TypeArena& types = context.pass().types;
    context.didRetype(replacement, types.at(previous), types.at(narrowed));
}

void GroupRewriter::leaveUnclaimed(ValueID rootOrCreated)
{
    RELEASE_ASSERT(rootOrCreated.index() >= m_firstCreatedValue || (rootOrCreated == m_group.root && isRootListed()));
    m_session.m_unclaimedValues.append(rootOrCreated);
}

bool GroupRewriter::isRootListed() const
{
    return !m_expansion && !m_session.m_isErasedFromBlock.get(m_group.root.index());
}

void GroupRewriter::retype(ValueID id)
{
    SelectionContext& context = m_session.m_context;
    TypeArena& types = context.pass().types;
    Value& value = function().edit(id);
    TypeID computed = resultTypeOf(context.program(), function(), value, types);
    if (computed == value.type)
        return;
    context.didRetype(id, types.at(value.type), types.at(computed));
    value.type = computed;
}

void GroupRewriter::finishMembers()
{
    auto resultsAfterRoot = m_session.m_resultsAfter.find(m_group.root);
    for (const GroupMember& member : m_group.members.span().first(m_group.members.size() - 1)) {
        m_session.m_isErasedFromBlock.set(member.value.index());
        bool isListedAgain = resultsAfterRoot != m_session.m_resultsAfter.end() && resultsAfterRoot->value.contains(member.value);
        bool hasSuccessor = m_session.m_substitution && m_session.m_substitution->resolve(member.value) != member.value;
        RELEASE_ASSERT(!member.isUsedOutside || isListedAgain || hasSuccessor);
        m_session.m_needsCompaction |= !isListedAgain;
        if (isListedAgain)
            retype(member.value);
    }
}

void GroupRewriter::finishCreatedValues()
{
    Vector<ValueID>& narrowedValues = m_session.m_context.m_narrowedValues;
    if (m_expansion && function().at(m_group.root).opcode == Opcode::Parameter)
        narrowedValues.append(m_group.root);

    for (uint32_t index = m_firstCreatedValue; index < function().values.size(); ++index) {
        ValueID id(index);
        Value& value = function().edit(id);
        value.setExpected({ });
        if (value.opcode == Opcode::Parameter)
            narrowedValues.append(id);
    }
}

void GroupRewriter::checkCreatedEntries()
{
    SelectionContext& context = m_session.m_context;
    const EffectContext& effects = analyses().effectContext();
    Diagnostics& diagnostics = context.build().diagnostics();
    const ReplacedEffects& replaced = state().replaced;
    FunctionID functionID = context.pass().id;

    if (m_didReplaceRoot)
        checkReplacement(effects, function(), ReplacementSite { functionID, rootBlock(), m_group.root }, function().at(m_group.root), replaced, diagnostics);

    uint32_t numberOfValues = static_cast<uint32_t>(function().values.size());
    BitVector isOnSlowPath(numberOfValues - m_firstCreatedValue);
    for (ValueID id : m_session.m_slowPathValues.span().subspan(m_firstSlowPathValue))
        isOnSlowPath.quickSet(id.index() - m_firstCreatedValue);

    for (uint32_t index = m_firstCreatedValue; index < numberOfValues; ++index) {
        ValueID id(index);
        const Value& value = function().at(id);
        if (value.opcode == Opcode::Parameter || isOnSlowPath.quickGet(index - m_firstCreatedValue))
            continue;
        checkReplacement(effects, function(), ReplacementSite { functionID, rootBlock(), id }, value, replaced, diagnostics);
    }

    for (BlockID block : m_blocksWithCreatedTerminators)
        checkReplacement(effects, function(), ReplacementSite { functionID, block, { } }, function().at(block).terminator, replaced, diagnostics);
}

// A rule writes the operands of the root behind replaceRoot(), and those of what it has appended behind the builder's call.
void GroupRewriter::didChangeOwnBlocks()
{
    UseLists* useLists = analyses().updatingUseLists();
    if (!useLists)
        return;
    useLists->didChangeBlock(state().block);
    for (size_t index = m_firstCreatedBlock; index < function().blocks.size(); ++index)
        useLists->didChangeBlock(BlockID(index));
}

void GroupRewriter::finish()
{
    RELEASE_ASSERT(!std::exchange(m_didFinish, true));
    finishMembers();
    didChangeOwnBlocks();

    if (m_duplicatedTerminator) {
        function().edit(m_expansion->continuation, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        analyses().didChangeControlFlow(singleElementSpan(m_expansion->continuation));
        m_session.m_needsCompaction = true;
    }

    if (isRootListed())
        retype(m_group.root);
    finishCreatedValues();
    if (m_session.m_checksEffects)
        checkCreatedEntries();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
