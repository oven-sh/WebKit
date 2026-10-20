/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGroupRewriter.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTSelectionContext.h"
#include <algorithm>
#include <functional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

RewriteSession::RewriteSession(SelectionContext& context, SelectionRuleID rule)
    : m_context(context)
    , m_rule(rule)
    , m_function(context.pass().function)
    , m_builder(context.analyses().context(), m_function, context.pass().types, &context.analyses())
    , m_substitution(m_function)
    , m_isErasedFromBlock(m_function.values.size())
    , m_firstNewValue(static_cast<uint32_t>(m_function.values.size()))
{
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
            RELEASE_ASSERT(analyses.effectsOf(member.value).onAnyEdge().isRemovableWhenUnused());
            continue;
        }
        if (analyses.effectsOf(member.value) != EffectsOnEdges::none())
            state.replaced.add(analyses.effectContext(), m_function, m_function.at(member.value));
    }
    m_groups.append(WTF::move(state));
}

Vector<uint32_t> RewriteSession::takeOrder()
{
    Vector<uint32_t> order(m_groups.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::stable_sort(order, std::ranges::greater { }, [&](uint32_t index) {
        return std::pair { m_groups[index].blockOrder, m_groups[index].index };
    });
    return order;
}

void RewriteSession::rebuildBlockValues()
{
    BlockValues values;
    for (Block& block : m_function.blocks) {
        bool isChanged = false;
        values.shrink(0);
        for (ValueID id : block.values) {
            if (m_isErasedFromBlock.get(id.index())) {
                isChanged = true;
                continue;
            }
            values.append(id);
            if (auto results = m_resultsAfter.find(id); results != m_resultsAfter.end()) {
                values.appendVector(results->value);
                isChanged = true;
            }
        }
        if (isChanged)
            block.values = values;
    }
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
    m_builder.applyInsertions();
    if (!m_isErasedFromBlock.isEmpty() || !m_resultsAfter.isEmpty())
        rebuildBlockValues();
    if (!m_substitution.isEmpty())
        m_substitution.apply(m_function, &analyses);
    analyses.didChangeControlFlow();

    updateClaims();
    m_context.didFinishRewrites(m_rule, static_cast<uint32_t>(m_groups.size()), m_needsCompaction);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
