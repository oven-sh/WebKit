/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionContext.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeShapes.h"
#include "AOTEffects.h"
#include "AOTGuardAvailability.h"
#include "AOTResultRules.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct GroupSpan {
    BlockID block;
    uint32_t firstIndex;
    uint32_t rootIndex;
};

} // anonymous namespace

static std::span<const GroupMember> membersWithoutRoot(const SelectionGroup& group)
{
    return group.members.span().first(group.members.size() - 1);
}

static bool hasRepeatedMember(const SelectionGroup& group)
{
    std::span<const GroupMember> members = group.members.span();
    for (size_t index = 1; index < members.size(); ++index) {
        if (std::ranges::contains(members.first(index), members[index].value, &GroupMember::value))
            return true;
    }
    return false;
}

static std::optional<GroupSpan> findSpan(const UseLists& useLists, const SelectionGroup& group)
{
    std::optional<ValueLocation> previous;
    for (const GroupMember& member : group.members) {
        ValueLocation location = useLists.locationOf(member.value);
        if (location.placement != ValuePlacement::InBlock)
            return std::nullopt;
        if (previous && (location.block != previous->block || location.index <= previous->index))
            return std::nullopt;
        previous = location;
    }
    return GroupSpan { previous->block, useLists.locationOf(group.members.first().value).index, previous->index };
}

static bool spansInlineFrames(const Function& function, const SelectionGroup& group)
{
    uint32_t inlineFrame = function.at(group.root).inlineFrame;
    return std::ranges::any_of(group.members, [&](const GroupMember& member) {
        return function.at(member.value).inlineFrame != inlineFrame;
    });
}

static bool isFrameAllocation(const Value& value)
{
    return Value::opcodesWithStorage.contains(value.opcode) && value.storage() != AllocationStorage::Heap;
}

static bool isBoundToFrame(const Function& function, const SelectionGroup& group)
{
    bool movesMembers = group.members.size() > 1 || group.hasSlowPath;
    return std::ranges::any_of(group.members, [&](const GroupMember& member) {
        const Value& value = function.at(member.value);
        return (movesMembers && isFrameAllocation(value)) || (group.hasSlowPath && boundToOf(value.opcode) == BoundTo::Site);
    });
}

// An allocation that is named as a guard proves what it proves by being the allocation. A block parameter that takes its place proves nothing.
static bool requiresDefinition(const Function& function, const Use& use, OperandKind kind)
{
    return mustBeDefinition(kind) || (kind == OperandKind::Guard && !isGuardValue(function, usedValueOf(function, use)));
}

static bool requiresDefinition(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return requiresDefinition(function, use, operandKindOf(function.at(use.user).opcode, use.operandIndex));
    case Use::Kind::TerminatorOperand: {
        TerminatorKind kind = function.at(use.block).terminator.kind;
        return kind == TerminatorKind::ReturnCall || requiresDefinition(function, use, operandKindOf(kind, use.operandIndex));
    }
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasSingleResult(const FunctionPassContext& pass, ValueID id)
{
    const Value& value = pass.function.at(id);
    return resultRuleOf(value.opcode) != ResultRule::None && !numberOfResultsOf(pass.program, pass.function, value);
}

static bool isUsedOutsideGroup(const FunctionPassContext& pass, const UseLists& useLists, const SelectionGroup& group, const GroupSpan& span)
{
    const Function& function = pass.function;
    for (const GroupMember& member : membersWithoutRoot(group)) {
        if (member.isUsedOutside && !hasSingleResult(pass, member.value))
            return true;
        for (const Use& use : useLists.usesOf(member.value)) {
            if (use.kind == Use::Kind::Operand && group.contains(use.user))
                continue;
            if (!member.isUsedOutside || requiresDefinition(function, use))
                return true;
            ProgramPoint point = useLists.pointOf(use);
            if (point.block == span.block && point.index <= span.rootIndex)
                return true;
        }
    }

    if (!group.hasSlowPath)
        return false;
    if (numberOfResultsOf(pass.program, function, function.at(group.root)))
        return true;
    return std::ranges::any_of(useLists.usesOf(group.root), [&](const Use& use) {
        return requiresDefinition(function, use);
    });
}

static bool crossesEffects(FunctionAnalyses& analyses, const Function& function, const SelectionGroup& group, const GroupSpan& span)
{
    const BlockValues& values = function.at(span.block).values;
    Effects pending = Effects::none();
    unsigned nextMember = 0;
    for (uint32_t index = span.firstIndex; index < span.rootIndex; ++index) {
        if (values[index] == group.members[nextMember].value) {
            pending.merge(analyses.effectsOf(values[index]).onAnyEdge());
            ++nextMember;
            continue;
        }
        if (!pending.isPure() && pending.interferesWith(analyses.effectsOf(values[index]).onAnyEdge()))
            return true;
    }
    return false;
}

static bool losesGuard(FunctionAnalyses& analyses, const Function& function, const SelectionGroup& group, const GroupSpan& span)
{
    struct MovedGuard {
        ValueID guard;
        ValueID user;
    };

    const BlockValues& values = function.at(span.block).values;
    Vector<MovedGuard, 4> movedGuards;
    unsigned nextMember = 0;
    for (uint32_t index = span.firstIndex; index < span.rootIndex; ++index) {
        ValueID id = values[index];
        if (id == group.members[nextMember].value) {
            const Value& member = function.at(id);
            member.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (member.operandKind(operandIndex) == OperandKind::Guard)
                    movedGuards.append(MovedGuard { operand, id });
            });
            ++nextMember;
            continue;
        }
        for (const MovedGuard& moved : movedGuards) {
            if (analyses.guards().mayBeEndedAt(moved.guard, function.at(moved.user), ProgramPoint { span.block, index }))
                return true;
        }
    }
    return false;
}

SelectionContext::SelectionContext(FunctionPassContext& pass)
    : m_pass(pass)
    , m_functionAsRead(pass.subject().function)
{
    refresh();
}

const SlotPlan& SelectionContext::slots() const
{
    RELEASE_ASSERT(m_pass.inputs.slots);
    return *m_pass.inputs.slots;
}

const CallingConventions& SelectionContext::callingConventions() const
{
    RELEASE_ASSERT(m_pass.inputs.callingConventions);
    return *m_pass.inputs.callingConventions;
}

bool SelectionContext::isEnabled(SelectionRuleID rule) const
{
    return ruleOf(rule).kind == SelectionRuleKind::Required || m_pass.build.isEnabled(rule, m_functionAsRead);
}

void SelectionContext::refresh()
{
    chooseCodeShapes(m_pass, ChosenShapes::KeepChosen);

    size_t numberOfValues = m_pass.function.values.size();
    m_isClaimed.ensureSize(numberOfValues);
    m_isOnSlowPath.ensureSize(numberOfValues);

    collectValues();
}

void SelectionContext::collectValues()
{
    const Function& function = m_pass.function;
    auto& allRanges = m_allValues.ranges;
    auto& unclaimedRanges = m_unclaimedValues.ranges;

    for (Opcode opcode : m_collectedOpcodes) {
        allRanges[static_cast<unsigned>(opcode)] = { };
        unclaimedRanges[static_cast<unsigned>(opcode)] = { };
    }
    m_collectedOpcodes.shrink(0);

    m_listedValues.shrink(0);
    m_listedValues.reserveCapacity(function.values.size());
    for (BlockID block : m_pass.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            ListedValue listed { id, function.at(id).opcode, isClaimed(id) };
            m_listedValues.append(listed);
            if (!allRanges[static_cast<unsigned>(listed.opcode)].end++)
                m_collectedOpcodes.append(listed.opcode);
            if (!listed.isClaimed)
                ++unclaimedRanges[static_cast<unsigned>(listed.opcode)].end;
        }
    }

    auto placeRange = [](ValuesByOpcode::Range& range, uint32_t& numberOfValues) {
        uint32_t begin = numberOfValues;
        numberOfValues += range.end;
        range = { begin, begin };
    };
    uint32_t numberOfValues = 0;
    uint32_t numberOfUnclaimedValues = 0;
    for (Opcode opcode : m_collectedOpcodes) {
        placeRange(allRanges[static_cast<unsigned>(opcode)], numberOfValues);
        placeRange(unclaimedRanges[static_cast<unsigned>(opcode)], numberOfUnclaimedValues);
    }
    m_allValues.values.resize(numberOfValues);
    m_unclaimedValues.values.resize(numberOfUnclaimedValues);

    for (const ListedValue& listed : m_listedValues) {
        unsigned opcode = static_cast<unsigned>(listed.opcode);
        m_allValues.values[allRanges[opcode].end++] = listed.id;
        if (!listed.isClaimed)
            m_unclaimedValues.values[unclaimedRanges[opcode].end++] = listed.id;
    }
}

std::span<const ValueID> SelectionContext::ValuesByOpcode::of(Opcode opcode) const
{
    Range range = ranges[static_cast<unsigned>(opcode)];
    return values.span().subspan(range.begin, range.end - range.begin);
}

std::span<const ValueID> SelectionContext::candidates(Opcode opcode) const
{
    return m_unclaimedValues.of(opcode);
}

std::span<const ValueID> SelectionContext::valuesWith(Opcode opcode) const
{
    return m_allValues.of(opcode);
}

CodeShape SelectionContext::shapeOf(BlockID block) const
{
    CodeShape shape = m_pass.function.at(block).shape;
    RELEASE_ASSERT(shape != CodeShape::Unchosen);
    return shape;
}

CodeShape SelectionContext::shapeAt(ValueID id)
{
    BlockID block = m_pass.analyses.useLists().blockOf(id);
    RELEASE_ASSERT(block);
    return shapeOf(block);
}

std::optional<ValueID> SelectionContext::singleUser(ValueID id)
{
    std::optional<Use> use = m_pass.analyses.useLists().singleUse(id);
    if (!use || use->kind != Use::Kind::Operand)
        return std::nullopt;
    return use->user;
}

bool SelectionContext::isOnlyBranchedOn(ValueID id)
{
    FunctionAnalyses& analyses = m_pass.analyses;
    const UseLists& useLists = analyses.useLists();
    ValueLocation location = useLists.locationOf(id);
    if (location.placement != ValuePlacement::InBlock)
        return false;

    const Block& block = m_pass.function.at(location.block);
    const Terminator& terminator = block.terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != id || useLists.useCount(id) != 1)
        return false;

    return std::ranges::all_of(block.values.span().subspan(location.index + 1), [&](ValueID later) {
        return analyses.hasNoEffects(later);
    });
}

bool SelectionContext::isOnlyReturned(ValueID id)
{
    const UseLists& useLists = m_pass.analyses.useLists();
    ValueLocation location = useLists.locationOf(id);
    if (location.placement != ValuePlacement::InBlock)
        return false;

    const Block& block = m_pass.function.at(location.block);
    const Terminator& terminator = block.terminator;
    return terminator.kind == TerminatorKind::Return && terminator.valueOperand() == id && location.index + 1 == block.values.size() && useLists.useCount(id) == 1;
}

bool SelectionContext::passesProofs(BlockID block) const
{
    return m_pass.function.at(block).terminator.targets.containsIf([](const Target& target) {
        return !target.proofs.isEmpty();
    });
}

bool SelectionContext::isConditionWithProofs(ValueID test)
{
    return std::ranges::any_of(m_pass.analyses.useLists().usesOf(test), [&](const Use& use) {
        return use.kind == Use::Kind::TerminatorOperand && m_pass.function.at(use.block).terminator.isConditionalBranch() && passesProofs(use.block);
    });
}

ClaimResult SelectionContext::checkGroup(SelectionRuleID rule, const SelectionGroup& group)
{
    const Vector<GroupMember, 4>& members = group.members;
    RELEASE_ASSERT(!members.isEmpty() && members.last().value == group.root && !members.last().isUsedOutside);

    if (!isEnabled(rule))
        return ClaimResult::RuleIsDisabled;
    if (m_pass.exceeds(LimitID::MaxSelectionGroupMembers, members.size(), group.root))
        return ClaimResult::MemberLimit;

    bool hasClaimedMember = std::ranges::any_of(members, [&](const GroupMember& member) {
        return isClaimed(member.value);
    });
    if (hasClaimedMember || hasRepeatedMember(group))
        return ClaimResult::AlreadyClaimed;

    const Function& function = m_pass.function;
    const UseLists& useLists = m_pass.analyses.useLists();
    std::optional<GroupSpan> span = findSpan(useLists, group);
    if (!span)
        return ClaimResult::SpansBlocks;
    if (spansInlineFrames(function, group))
        return ClaimResult::SpansInlineFrames;
    if (m_pass.exceeds(LimitID::MaxSelectionGroupSpan, span->rootIndex - span->firstIndex, group.root))
        return ClaimResult::SpanLimit;
    if (isBoundToFrame(function, group))
        return ClaimResult::BoundToFrame;

    bool hasMemberOnSlowPath = std::ranges::any_of(members, [&](const GroupMember& member) {
        return isOnSlowPath(member.value);
    });
    if (ruleOf(rule).kind == SelectionRuleKind::Optional && hasMemberOnSlowPath)
        return ClaimResult::OnSlowPath;

    if (isUsedOutsideGroup(m_pass, useLists, group, *span))
        return ClaimResult::UsedOutsideGroup;
    if (crossesEffects(m_pass.analyses, function, group, *span) || losesGuard(m_pass.analyses, function, group, *span))
        return ClaimResult::CrossesEffects;
    return ClaimResult::Claimed;
}

ClaimResult SelectionContext::count(ClaimResult result)
{
    ++m_claimCounts[static_cast<unsigned>(result)];
    return result;
}

void SelectionContext::markClaimed(const SelectionGroup& group)
{
    for (const GroupMember& member : group.members)
        m_isClaimed.set(member.value.index());
}

ClaimResult SelectionContext::tryClaim(SelectionRuleID rule, const SelectionGroup& group)
{
    ClaimResult result = checkGroup(rule, group);
    if (result == ClaimResult::Claimed)
        markClaimed(group);
    return count(result);
}

ClaimResult SelectionContext::tryClaimAll(SelectionRuleID rule, std::span<const SelectionGroup> groups)
{
    BitVector isInCheckedGroup(m_pass.function.values.size());
    for (const SelectionGroup& group : groups) {
        ClaimResult result = checkGroup(rule, group);
        if (result != ClaimResult::Claimed)
            return count(result);
        for (const GroupMember& member : group.members) {
            if (!isInCheckedGroup.add(member.value.index()))
                return count(ClaimResult::AlreadyClaimed);
        }
    }

    for (const SelectionGroup& group : groups)
        markClaimed(group);
    return count(ClaimResult::Claimed);
}

void SelectionContext::didFinishRewrites(SelectionRuleID rule, uint32_t numberOfGroups, bool needsCompaction)
{
    m_didRewrite = true;
    m_pass.build.statistics().count(Wire::groupsOf(rule), numberOfGroups);
    refresh();

    bool hasUnreachableBlocks = m_pass.analyses.cfg().reversePostOrder().size() != m_pass.function.blocks.size();
    m_needsCompaction |= needsCompaction || hasUnreachableBlocks;
}

void SelectionContext::didSelectForms(SelectionRuleID rule, uint32_t numberOfValues)
{
    if (!numberOfValues)
        return;

    m_didRewrite = true;
    m_pass.build.statistics().count(Wire::groupsOf(rule), numberOfValues);
    // A form is a payload: every value has the operation, the operands and the place it had.
    m_pass.analyses.didChangeTypes();
}

// A test keeps its operation, its operands and its type, and nothing is made: there is nothing here for a session to check, to type or to list.
void SelectionContext::fuseTestsWithBranches(std::span<const BlockID> blocks)
{
    // No rule comes behind this one, so nobody asks for the values by opcode again.
    static_assert(static_cast<unsigned>(SelectionRuleID::TestAndBranch) == numberOfSelectionRules);
    if (blocks.empty())
        return;

    Function& function = m_pass.function;
    bool hasUnreachableBlocks = m_pass.analyses.cfg().reversePostOrder().size() != function.blocks.size();
    for (BlockID id : blocks) {
        Block& block = function.edit(id);
        ValueID test = block.terminator.conditionOperand();
        if (block.values.last() != test) {
            size_t index = block.values.reverseFind(test);
            RELEASE_ASSERT(index != notFound);
            block.values.removeAt(index);
            block.values.append(test);
        }
        changeBranchKind(block.terminator, TerminatorKind::TestAndBranch);
    }
    m_pass.analyses.didChangeControlFlow();

    m_didRewrite = true;
    m_needsCompaction |= hasUnreachableBlocks;
    m_pass.build.statistics().count(Wire::groupsOf(SelectionRuleID::TestAndBranch), blocks.size());
    chooseCodeShapes(m_pass, ChosenShapes::KeepChosen);
}

void SelectionContext::didRetype(ValueID id, const Type& previous, const Type& computed)
{
    m_narrowedValues.append(id);
    if (!computed.isSubsetOf(previous, m_pass.program))
        m_hasWidenedValue = true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
