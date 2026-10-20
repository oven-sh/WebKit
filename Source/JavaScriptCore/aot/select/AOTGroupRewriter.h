/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsCheck.h"
#include "AOTIRBuilder.h"
#include "AOTSchemaEnums.h"
#include "AOTSelectionGroup.h"
#include "AOTValueSubstitution.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;
class SelectionContext;

class RewriteSession {
    WTF_MAKE_NONCOPYABLE(RewriteSession);
public:
    RewriteSession(SelectionContext&, SelectionRuleID);

    void add(const SelectionGroup&);
    Vector<uint32_t> takeOrder();
    void finish();

private:
    friend class GroupRewriter;

    struct GroupState {
        BlockID block;
        uint32_t index { 0 };
        uint32_t blockOrder { 0 };
        bool isOnlyBranchedOn { false };
        bool canDuplicateTerminator { false };
        ReplacedEffects replaced;
    };

    using ResultsByAnchor = UncheckedKeyHashMap<ValueID, Vector<ValueID, 2>>;

    void rebuildBlockValues();
    void updateClaims();

    SelectionContext& m_context;
    SelectionRuleID m_rule;
    Function& m_function;
    IRBuilder m_builder;
    ValueSubstitution m_substitution;
    Vector<GroupState> m_groups;
    BitVector m_isErasedFromBlock;
    ResultsByAnchor m_resultsAfter;
    Vector<ValueID> m_unclaimedValues;
    Vector<ValueID> m_slowPathValues;
    uint32_t m_firstNewValue { 0 };
    bool m_needsCompaction { false };
};

class GroupRewriter {
    WTF_MAKE_NONCOPYABLE(GroupRewriter);
public:
    GroupRewriter(RewriteSession&, const SelectionGroup&, uint32_t groupIndex);
    void finish();

    IRBuilder& builder() { return m_session.m_builder; }
    Function& function() { return m_session.m_function; }
    const SelectionGroup& group() const { return m_group; }
    const Value& original(ValueID member) const;
    BlockID rootBlock() const { return m_session.m_groups[m_groupIndex].block; }

    ValueID insert(Value&&);
    Value& replaceRoot(Opcode);
    void eraseRoot();
    void replaceWithResult(ValueID member, ValueID producer, uint32_t index);
    void replaceWithValue(ValueID member, ValueID replacement);
    void leaveUnclaimed(ValueID rootOrCreated);

    void emitTestAndBranch();
    void emitTestAndBranchAsValue();
    void endBlockAsUnreachable();

    void emitSlowPath(ValueID succeeded);
    void emitTwoWay(Terminator&&);

    void beginArms();
    BlockID addArm(BlockFrequency);
    ValueID appendOriginals();
    void endArm(std::optional<ValueID> result);

    ValueID copyRootToEndOf(BlockID);

private:
    struct FastResult {
        ValueID member;
        ValueID producer;
        uint32_t index { 0 };
    };

    RewriteSession::GroupState& state() { return m_session.m_groups[m_groupIndex]; }
    FunctionAnalyses& analyses();

    void updateRootIndex();
    void flushInsertions();
    void reopenBlock(BlockID);
    void moveRootToBlockEnd();
    void expandRoot();
    BlockFrequency slowPathFrequency() const;
    Vector<uint32_t, 4> carriedMembers() const;
    void addCarriedParameters(const Vector<uint32_t, 4>& carried);
    ValueID appendFastResult(ValueID member);
    Vector<ValueID, 4> appendOriginalMembers();
    bool isRootListed() const;
    void retype(ValueID);
    void finishMembers();
    void finishCreatedValues();
    void checkCreatedEntries();

    RewriteSession& m_session;
    const SelectionGroup& m_group;
    uint32_t m_groupIndex;
    uint32_t m_firstCreatedValue { 0 };
    uint32_t m_firstSlowPathValue { 0 };
    Vector<Value, 4> m_originals;
    Vector<FastResult, 2> m_fastResults;
    Vector<BlockID, 2> m_blocksWithCreatedTerminators;
    bool m_didReplaceRoot { false };
    bool m_didFinish { false };
    std::optional<IRBuilder::Expansion> m_expansion;
    std::optional<Terminator> m_duplicatedTerminator;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
