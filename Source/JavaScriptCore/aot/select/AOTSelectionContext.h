/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTGroupRewriter.h"
#include "AOTPass.h"
#include "AOTResultRules.h"
#include "AOTSelectionGroup.h"
#include "AOTSelectionRules.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class SelectionContext {
    WTF_MAKE_NONCOPYABLE(SelectionContext);
public:
    explicit SelectionContext(FunctionPassContext&);

    FunctionPassContext& pass() { return m_pass; }
    const Program& program() const { return m_pass.program; }
    const Function& function() const { return m_pass.function; }
    FunctionAnalyses& analyses() { return m_pass.analyses; }
    BuildContext& build() { return m_pass.build; }
    const FlowResults& flow() const { return m_pass.inputs.flow; }
    const SlotPlan& slots() const;
    const CallingConventions& callingConventions() const;
    TypingContext typing() const { return { m_pass.program, m_pass.function, m_pass.types }; }
    const Type& typeOf(ValueID id) const { return m_pass.program.typeOf(m_pass.function, id); }

    bool isEnabled(SelectionRuleID) const;

    CodeShape shapeOf(BlockID) const;
    CodeShape shapeAt(ValueID);

    std::span<const ValueID> candidates(Opcode) const LIFETIME_BOUND;
    std::span<const ValueID> valuesWith(Opcode) const LIFETIME_BOUND;
    bool isClaimed(ValueID id) const { return m_isClaimed.get(id.index()); }
    bool isOnSlowPath(ValueID id) const { return m_isOnSlowPath.get(id.index()); }

    std::optional<ValueID> singleUser(ValueID);
    bool isOnlyBranchedOn(ValueID);
    bool isOnlyReturned(ValueID);
    bool passesProofs(BlockID) const;
    bool isConditionWithProofs(ValueID);

    ClaimResult tryClaim(SelectionRuleID, const SelectionGroup&);
    ClaimResult tryClaimAll(SelectionRuleID, std::span<const SelectionGroup>);

    template<typename Match>
    void rewriteGroups(SelectionRuleID rule, Vector<Match>& matches, NOESCAPE const Invocable<void(GroupRewriter&, const Match&)> auto& rewrite)
    {
        if (matches.isEmpty())
            return;
        RewriteSession session(*this, rule, matches.size());
        for (const Match& match : matches)
            session.add(match.group);
        for (uint32_t index : session.takeOrder()) {
            GroupRewriter rewriter(session, matches[index].group, index);
            rewrite(rewriter, matches[index]);
            rewriter.finish();
        }
        session.finish();
    }

    void didSelectForms(SelectionRuleID, uint32_t numberOfValues);
    // Each block ends in a branch on a test that it lists and that isOnlyBranchedOn().
    void fuseTestsWithBranches(std::span<const BlockID>);

    bool didRewrite() const { return m_didRewrite; }
    bool needsCompaction() const { return m_needsCompaction; }
    // An allocation under a handler was hoisted out of its loop or removed, so the loop may no longer need the handler.
    bool removedAllocationUnderHandler() const { return m_removedAllocationUnderHandler; }
    void noteRemovedAllocationUnderHandler() { m_removedAllocationUnderHandler = true; }
    std::span<const ValueID> narrowedValues() const LIFETIME_BOUND { return m_narrowedValues.span(); }
    bool hasWidenedValue() const { return m_hasWidenedValue; }
    bool takeDidKeepType() { return std::exchange(m_didKeepType, false); }
    void didRestoreTypes();
    void didRenumberValues();
    std::span<const uint32_t> claimCounts() const LIFETIME_BOUND { return m_claimCounts; }

private:
    friend class GroupRewriter;
    friend class RewriteSession;

    struct ValuesByOpcode {
        struct Range {
            uint32_t begin { 0 };
            uint32_t end { 0 };
        };

        std::span<const ValueID> of(Opcode) const LIFETIME_BOUND;

        Vector<ValueID> values;
        std::array<Range, numberOfEnumerators<Opcode>> ranges { };
    };

    struct ListedValue {
        ValueID id;
        Opcode opcode;
        bool isClaimed;
    };

    ClaimResult checkGroup(SelectionRuleID, const SelectionGroup&);
    ClaimResult count(ClaimResult);
    void markClaimed(const SelectionGroup&);
    void refresh();
    void collectValues();
    void didFinishRewrites(SelectionRuleID, uint32_t numberOfGroups, bool needsCompaction);
    void didRetype(ValueID, const Type& previous, const Type& computed);

    FunctionPassContext& m_pass;
    FunctionID m_functionAsRead;
    BitVector m_isClaimed;
    BitVector m_isOnSlowPath;
    ValuesByOpcode m_allValues;
    ValuesByOpcode m_unclaimedValues;
    Vector<Opcode, 64> m_collectedOpcodes;
    Vector<ListedValue> m_listedValues;
    Vector<ValueID> m_narrowedValues;
    bool m_hasWidenedValue { false };
    bool m_didKeepType { false };
    bool m_didRewrite { false };
    bool m_needsCompaction { false };
    bool m_removedAllocationUnderHandler { false };
    std::array<uint32_t, numberOfClaimResults> m_claimCounts { };
};

inline void SelectionContext::didRestoreTypes()
{
    m_narrowedValues.shrink(0);
    m_hasWidenedValue = false;
}

inline void SelectionContext::didRenumberValues()
{
    RELEASE_ASSERT(m_narrowedValues.isEmpty());
    m_isClaimed.clearAll();
    m_isOnSlowPath.clearAll();
    refresh();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
