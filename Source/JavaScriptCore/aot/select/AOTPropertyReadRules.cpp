/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTEffectsOfRules.h"
#include "AOTFusedAccessEmitter.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include <algorithm>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CheckedReadMatch {
    SelectionGroup group;
};

struct TestedReadMatch {
    SelectionGroup group;
    std::optional<StringID> comparedString;
};

struct ComparedRead {
    ValueID read;
    StringID literal;
};

struct ReadPairMatch {
    SelectionGroup group;
    bool readsFromFirstResult;
};

} // anonymous namespace

static bool isPlainRead(SelectionContext& context, ValueID id)
{
    const Value& value = context.function().at(id);
    if (value.opcode != Opcode::PropGet || context.isClaimed(id) || context.isOnSlowPath(id) || value.type != TypeTable::anything)
        return false;
    if (value.isFreeName() || value.isForTypeOf())
        return false;
    if (!isOrdinaryKey(context.program(), value.key()) || !context.slots().hasColumn(value.key()))
        return false;
    return hasEffectsOfUnknownCode(context, id);
}

static bool canFuseRead(SelectionContext& context, ValueID read, ValueID user)
{
    return context.singleUser(read) == user && isPlainRead(context, read);
}

static bool prefersSeparateReads(SelectionContext& context, BlockID block)
{
    return context.shapeOf(block) == CodeShape::Fast && !context.analyses().blockCycles().isInCycle(block);
}

static bool canFuseTest(SelectionContext& context, ValueID test)
{
    BlockID block = context.analyses().useLists().blockOf(test);
    return block && context.shapeOf(block) != CodeShape::Fast && !context.isConditionWithProofs(test);
}

static std::optional<StringID> shortLiteralOf(SelectionContext& context, ValueID id)
{
    const Program& program = context.program();
    std::optional<StringID> string = stringConstantOf(program, context.function(), id);
    if (!string || program.string(*string).empty() || !program.strings.isAllASCII(*string))
        return std::nullopt;
    if (context.pass().exceeds(LimitID::MaxRegisterStringLength, program.string(*string).size(), id))
        return std::nullopt;
    return string;
}

static SelectionGroup createFusedGroup(ValueID read, ValueID user, bool hasSlowPath = false)
{
    return { .root = user, .members = { GroupMember { read }, GroupMember { user } }, .hasSlowPath = hasSlowPath };
}

static void copyAccessStamp(Value& fused, const Value& read)
{
    fused.setAccessSite(read.accessSite());
    fused.setReceivers(read.receivers());
}

void selectCheckedPropertyRead(SelectionContext& context)
{
    const Program& program = context.program();
    const Function& function = context.function();
    Vector<CheckedReadMatch> matches;
    for (ValueID claim : context.candidates(Opcode::Claim)) {
        const Value& value = function.at(claim);
        if (!value.site())
            continue;
        ValueID read = value.valueOperand();
        if (!canFuseRead(context, read, claim) || !checkedReadKindOf(program, program.at(value.castType())))
            continue;
        CheckedReadMatch match { createFusedGroup(read, claim) };
        if (context.tryClaim(SelectionRuleID::CheckedPropertyRead, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }

    context.rewriteGroups(SelectionRuleID::CheckedPropertyRead, matches, [](GroupRewriter& rewriter, const CheckedReadMatch& match) {
        const Value& read = rewriter.original(match.group.members.first().value);
        const Value& claim = rewriter.original(match.group.root);
        Value& checked = rewriter.replaceRoot(Opcode::PropGetChecked);
        checked.setObjectOperand(read.objectOperand());
        checked.setKey(read.key());
        checked.setCheckedType(claim.castType());
        checked.setSite(claim.site());
        checked.setCause(claim.cause());
        checked.setReadOrigin(read.origin());
        copyAccessStamp(checked, read);
    });
}

static std::optional<ComparedRead> findComparedRead(SelectionContext& context, ValueID comparison)
{
    const Value& value = context.function().at(comparison);
    ValueID left = value.leftOperand();
    ValueID right = value.rightOperand();
    for (auto [read, other] : { std::pair { left, right }, std::pair { right, left } }) {
        if (!canFuseRead(context, read, comparison))
            continue;
        if (std::optional<StringID> literal = shortLiteralOf(context, other))
            return ComparedRead { read, *literal };
    }
    return std::nullopt;
}

void selectPropertyReadAndTest(SelectionContext& context)
{
    const Function& function = context.function();
    Vector<TestedReadMatch> matches;
    auto tryAddMatch = [&](TestedReadMatch&& match) {
        if (context.tryClaim(SelectionRuleID::PropertyReadAndTest, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    };

    for (ValueID test : context.candidates(Opcode::ToBoolean)) {
        if (!canFuseTest(context, test) || !context.isOnlyBranchedOn(test))
            continue;
        ValueID read = function.at(test).valueOperand();
        if (canFuseRead(context, read, test))
            tryAddMatch({ createFusedGroup(read, test), std::nullopt });
    }

    for (ValueID comparison : context.candidates(Opcode::StrictEq)) {
        if (!canFuseTest(context, comparison))
            continue;
        if (std::optional<ComparedRead> compared = findComparedRead(context, comparison))
            tryAddMatch({ createFusedGroup(compared->read, comparison, !context.isOnlyBranchedOn(comparison)), compared->literal });
    }

    context.rewriteGroups(SelectionRuleID::PropertyReadAndTest, matches, [](GroupRewriter& rewriter, const TestedReadMatch& match) {
        const Value& read = rewriter.original(match.group.members.first().value);
        Value& test = rewriter.replaceRoot(Opcode::PropGetAndTest);
        test.setObjectOperand(read.objectOperand());
        test.setKey(read.key());
        if (match.comparedString)
            test.setComparedString(*match.comparedString);
        copyAccessStamp(test, read);
        test.setOrigin(read.origin());
        if (!match.group.hasSlowPath) {
            rewriter.emitTestAndBranch();
            return;
        }
        rewriter.builder().setOrigin(read.origin());
        rewriter.emitTestAndBranchAsValue();
    });
}

static std::optional<ReadPairMatch> tryClaimReadPair(SelectionContext& context, SelectionRuleID rule, ValueID first, ValueID second)
{
    const UseLists& useLists = context.analyses().useLists();
    BlockID block = useLists.blockOf(second);
    if (!block || useLists.blockOf(first) != block || prefersSeparateReads(context, block))
        return std::nullopt;

    bool readsFromFirstResult = context.function().at(second).objectOperand() == first;
    bool isChain = readsFromFirstResult && useLists.useCount(first) == 1;
    if (isChain != (rule == SelectionRuleID::PropertyChain))
        return std::nullopt;

    ReadPairMatch match { { .root = second, .members = { GroupMember { first, !isChain }, GroupMember { second } } }, readsFromFirstResult };
    if (context.tryClaim(rule, match.group) != ClaimResult::Claimed)
        return std::nullopt;
    return match;
}

static void setReadPairPayload(Value& value, const Value& firstRead, const Value& secondRead)
{
    value.setKey(firstRead.key());
    value.setSecondKey(secondRead.key());
    value.setReadOrigin(firstRead.origin());
    copyAccessStamp(value, firstRead);
    value.setSecondAccessSite(secondRead.accessSite());
}

static void rewriteReadPair(GroupRewriter& rewriter, const ReadPairMatch& match)
{
    const GroupMember& first = match.group.members.first();
    const Value& firstRead = rewriter.original(first.value);
    const Value& secondRead = rewriter.original(match.group.root);
    ValueID receiver = firstRead.objectOperand();

    if (!first.isUsedOutside) {
        Value& chain = rewriter.replaceRoot(Opcode::PropGetChain);
        chain.setObjectOperand(receiver);
        setReadPairPayload(chain, firstRead, secondRead);
        return;
    }

    Value pair = IRBuilder::make(Opcode::PropGetPair, { receiver });
    if (!match.readsFromFirstResult && secondRead.objectOperand() != receiver)
        pair.setOtherOperand(secondRead.objectOperand());
    setReadPairPayload(pair, firstRead, secondRead);
    pair.setReadsFromFirstResult(match.readsFromFirstResult);
    ValueID pairID = rewriter.insert(WTF::move(pair));
    rewriter.replaceWithResult(first.value, pairID, 0);
    rewriter.replaceWithResult(match.group.root, pairID, 1);
}

static void selectReadPairs(SelectionContext& context, SelectionRuleID rule)
{
    Vector<ReadPairMatch> matches;
    std::optional<ValueID> unpairedRead;
    for (ValueID read : context.candidates(Opcode::PropGet)) {
        if (!isPlainRead(context, read)) {
            unpairedRead = std::nullopt;
            continue;
        }
        std::optional<ReadPairMatch> match = unpairedRead ? tryClaimReadPair(context, rule, *unpairedRead, read) : std::nullopt;
        if (!match) {
            unpairedRead = read;
            continue;
        }
        matches.append(WTF::move(*match));
        unpairedRead = std::nullopt;
    }

    context.rewriteGroups(rule, matches, rewriteReadPair);
}

void selectPropertyChain(SelectionContext& context)
{
    selectReadPairs(context, SelectionRuleID::PropertyChain);
}

void selectPropertyPair(SelectionContext& context)
{
    selectReadPairs(context, SelectionRuleID::PropertyPair);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
