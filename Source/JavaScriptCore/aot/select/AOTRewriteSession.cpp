/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGroupRewriter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTSelectionContext.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

// The rewrites of these rules write the root, what they have made themselves and the branch that takes a copy of the root, and ask for no analysis meanwhile.
static constexpr bool writesOnlyNamedBlocks(SelectionRuleID rule)
{
    switch (rule) {
    case SelectionRuleID::PlannedSlots:
    case SelectionRuleID::MethodCall:
    case SelectionRuleID::PropertyReadAndTest:
    case SelectionRuleID::PropertyChain:
    case SelectionRuleID::PropertyPair:
    case SelectionRuleID::StringLiteralForms:
    case SelectionRuleID::InlineBuiltin:
    case SelectionRuleID::OperatorForm:
        return true;
    default:
        return false;
    }
}

RewriteSession::RewriteSession(SelectionContext& context, SelectionRuleID rule, size_t numberOfGroups)
    : m_context(context)
    , m_rule(rule)
    , m_checksEffects(context.build().options().verifiesProgram)
    , m_function(context.pass().function)
    , m_builder(context.analyses().context(), m_function, context.pass().types, &context.analyses())
    , m_isErasedFromBlock(m_function.values.size())
    , m_firstNewValue(static_cast<uint32_t>(m_function.values.size()))
    , m_firstNewBlock(static_cast<uint32_t>(m_function.blocks.size()))
{
    m_groups.reserveInitialCapacity(numberOfGroups);
    context.analyses().setEveryChangeIsReported(writesOnlyNamedBlocks(rule));
}

void RewriteSession::add(const SelectionGroup& group)
{
    FunctionAnalyses& analyses = m_context.analyses();
    ValueLocation location = analyses.useLists().locationOf(group.root);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);

    bool isOnlyBranchedOn = m_context.isOnlyBranchedOn(group.root);
    // A branch passes the proofs of the test it names, so a copy of it needs a copy of the test.
    bool canDuplicateBranch = isOnlyBranchedOn && (group.endsEveryArmInCopyOfRoot || !m_context.passesProofs(location.block));
    GroupState state {
        .block = location.block,
        .index = location.index,
        .blockOrder = analyses.cfg().reversePostOrderIndex(location.block),
        .isOnlyBranchedOn = isOnlyBranchedOn,
        .canDuplicateTerminator = canDuplicateBranch || m_context.isOnlyReturned(group.root),
        .replaced = { },
    };
    for (const GroupMember& member : group.members) {
        if (member.isOnlyErased) {
            RELEASE_ASSERT(analyses.isRemovableWhenUnused(member.value));
            continue;
        }
        if (!m_checksEffects)
            continue;
        const EffectsOnEdges& effects = analyses.effectsOf(member.value);
        if (effects != EffectsOnEdges::none())
            state.replaced.add(effects, m_function.at(member.value).opcode);
    }
    m_groups.append(WTF::move(state));
}

ValueSubstitution& RewriteSession::substitution()
{
    if (!m_substitution)
        m_substitution.emplace(m_function);
    return *m_substitution;
}

Vector<uint32_t> RewriteSession::takeOrder()
{
    Vector<uint32_t> order(m_groups.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::stableSort(order, std::ranges::greater { }, [&](uint32_t index) {
        return std::pair { m_groups[index].blockOrder, m_groups[index].index };
    });
    return order;
}

// A member stands in the block of its group, or in a block that the session has cut off that block.
void RewriteSession::rebuildBlockValues()
{
    BlockValues values;
    bool hasResults = !m_resultsAfter.isEmpty();
    auto rebuild = [&](Block& block) {
        bool isChanged = false;
        values.shrink(0);
        for (ValueID id : block.values) {
            if (m_isErasedFromBlock.get(id.index())) {
                isChanged = true;
                continue;
            }
            values.append(id);
            if (!hasResults)
                continue;
            if (auto results = m_resultsAfter.find(id); results != m_resultsAfter.end()) {
                values.appendVector(results->value);
                isChanged = true;
            }
        }
        if (isChanged)
            block.values = values;
    };

    BitVector isRebuilt(m_firstNewBlock);
    for (const GroupState& group : m_groups) {
        if (isRebuilt.add(group.block.index()))
            rebuild(m_function.edit(group.block));
    }
    for (size_t index = m_firstNewBlock; index < m_function.blocks.size(); ++index)
        rebuild(m_function.edit(BlockID(index)));
}

// With nothing queued the builder drops every analysis of values.
void RewriteSession::applyInsertions()
{
    if (m_builder.hasInsertions()) {
        m_builder.applyInsertions();
        return;
    }
    m_context.analyses().didChangeValuesButNotUses();
}

void RewriteSession::updateClaims()
{
    size_t numberOfValues = m_function.values.size();
    BitVector& isClaimed = m_context.m_isClaimed;
    BitVector& isOnSlowPath = m_context.m_isOnSlowPath;
    isClaimed.ensureSize(numberOfValues);
    isOnSlowPath.ensureSize(numberOfValues);

    for (size_t index = m_firstNewValue; index < numberOfValues; ++index)
        isClaimed.quickSet(index);
    for (ValueID id : m_unclaimedValues)
        isClaimed.quickClear(id.index());
    for (ValueID id : m_slowPathValues) {
        isClaimed.quickClear(id.index());
        isOnSlowPath.quickSet(id.index());
    }
}

void RewriteSession::finish()
{
    FunctionAnalyses& analyses = m_context.analyses();
    applyInsertions();
    if (!m_isErasedFromBlock.isEmpty() || !m_resultsAfter.isEmpty())
        rebuildBlockValues();
    if (m_substitution && !m_substitution->isEmpty())
        m_substitution->apply(m_function, &analyses);
    // Whoever has added a block, or written a target or a handler, has said so where it did.
    if (UseLists* useLists = analyses.updatingUseLists()) {
        for (const GroupState& group : m_groups)
            useLists->didChangeBlock(group.block);
        for (size_t index = m_firstNewBlock; index < m_function.blocks.size(); ++index)
            useLists->didChangeBlock(BlockID(index));
    }
    analyses.didChangeValuesButNotUses();
    analyses.setEveryChangeIsReported(false);

    updateClaims();
    m_context.didFinishRewrites(m_rule, static_cast<uint32_t>(m_groups.size()), m_needsCompaction);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
