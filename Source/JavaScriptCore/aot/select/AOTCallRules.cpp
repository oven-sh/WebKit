/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTEffectsOfRules.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTGroupRewriter.h"
#include "AOTLoopEntry.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTValueIdentity.h"
#include <optional>
#include <tuple>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct MethodCall {
    SelectionGroup group;
    ValueID read;
};

struct ResolvedCalls {
    SelectionGroup group;
    BlockID header;
    ValueID callee;
    uint32_t argumentCount;
    Vector<ValueID, 4> calls;
};

} // anonymous namespace

static uint32_t numberOfArgumentsOf(const Value& call)
{
    return static_cast<uint32_t>(call.argumentsOperands().size());
}

static bool isOwnedByReturnCall(SelectionContext& context, ValueID id)
{
    BlockID block = context.analyses().useLists().blockOf(id);
    if (!block)
        return false;
    const Terminator& terminator = context.function().at(block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id;
}

static bool isUntypedMethodRead(SelectionContext& context, ValueID id, PropertyKeyID key)
{
    const Value& read = context.function().at(id);
    if (read.opcode != Opcode::PropGet || read.key() != key || read.isFreeName() || read.isForTypeOf())
        return false;
    return context.typeOf(id).isAnything() && hasEffectsOfUnknownCode(context, id);
}

static std::optional<MethodCall> matchMethodCall(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    const Value& call = function.at(id);
    PropertyKeyID key = call.calleeKey();
    ValueID thisValue = call.thisValueOperand();
    if (!key || !thisValue || call.expectedResult() || numberOfArgumentsOf(call) > numberOfArgumentGPRs)
        return std::nullopt;

    ValueID read = call.calleeOperand();
    if (!isUntypedMethodRead(context, read, key))
        return std::nullopt;

    FunctionAnalyses& analyses = context.analyses();
    if (analyses.identityOf(thisValue) != analyses.identityOf(function.at(read).objectOperand()))
        return std::nullopt;
    if (!isOrdinaryKey(context.program(), key) || !context.slots().hasColumn(key))
        return std::nullopt;
    if (isOwnedByReturnCall(context, id))
        return std::nullopt;
    return MethodCall { groupOf({ read, id }), read };
}

static void rewriteMethodCall(GroupRewriter& rewriter, const MethodCall& match)
{
    const Value& read = rewriter.original(match.read);
    const Value& call = rewriter.original(match.group.root);

    Value& root = rewriter.replaceRoot(Opcode::CallMethod);
    root.setReceiverOperand(read.objectOperand());
    root.setArgumentsOperands(call.argumentsOperands());
    root.setKey(read.key());
    root.setCalleeName(call.calleeName());
    root.setReadOrigin(read.origin());
    root.setAccessSite(read.accessSite());
    root.setReceivers(read.receivers());
    root.setCallees(call.callees());
    root.setCallAccessSite(call.accessSite());
    root.setIsDemotedTailCall(call.isDemotedTailCall());
}

// CallMethod retains its operands. A call whose callees are all known may retain none of them, and then one may lie in the frame.
static bool passesFramePointer(const FramePointers& pointers, const Value& call)
{
    return pointers.contains(call.thisValueOperand()) || std::ranges::any_of(call.argumentsOperands(), [&](ValueID argument) {
        return pointers.contains(argument);
    });
}

void selectMethodCall(SelectionContext& context)
{
    std::optional<FramePointers> framePointers;
    Vector<MethodCall> matches;
    for (ValueID id : context.candidates(Opcode::Call)) {
        std::optional<MethodCall> match = matchMethodCall(context, id);
        if (!match)
            continue;
        if (!framePointers)
            framePointers.emplace(context.program(), context.function());
        if (passesFramePointer(*framePointers, context.function().at(id)))
            continue;
        if (context.tryClaim(SelectionRuleID::MethodCall, match->group) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    context.rewriteGroups(SelectionRuleID::MethodCall, matches, [](GroupRewriter& rewriter, const MethodCall& match) {
        rewriteMethodCall(rewriter, match);
    });
}

static bool canCallResolvedTarget(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    const Value& call = function.at(id);
    if (context.isClaimed(id) || context.isOnSlowPath(id) || call.expectedResult() || numberOfArgumentsOf(call) > numberOfArgumentGPRs)
        return false;

    BlockID block = context.analyses().useLists().blockOf(id);
    if (!block || isOwnedByReturnCall(context, id))
        return false;
    return function.at(block).frequency != BlockFrequency::Rare && context.shapeOf(block) != CodeShape::Compact;
}

static Vector<ResolvedCalls> findCallsOfInvariantCallees(SelectionContext& context)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();

    Vector<ResolvedCalls> matches;
    HashMap<std::tuple<BlockID, ValueID, uint32_t>, size_t> matchIndexes;
    for (ValueID id : context.candidates(Opcode::Call)) {
        if (!canCallResolvedTarget(context, id))
            continue;
        const Value& call = function.at(id);
        ValueID callee = call.calleeOperand();
        std::optional<InvariantOperand> invariant = findOutermostInvariantLoop(context, analyses.useLists().blockOf(id), callee);
        if (!invariant)
            continue;

        ValueID object = analyses.sameObjectThroughCasts(callee);
        uint32_t argumentCount = numberOfArgumentsOf(call);
        auto entry = matchIndexes.add(std::tuple { invariant->header, object, argumentCount }, matches.size());
        if (entry.isNewEntry)
            matches.append(ResolvedCalls { groupOfOne(id), invariant->header, invariant->value, argumentCount, { } });
        matches[entry.iterator->value].calls.append(id);
    }
    return matches;
}

static Vector<SelectionGroup, 8> groupsOf(const ResolvedCalls& match)
{
    Vector<SelectionGroup, 8> groups;
    for (ValueID call : match.calls)
        groups.append(groupOfOne(call));
    return groups;
}

static Value makeResolveCallTarget(const ResolvedCalls& match)
{
    Value target = IRBuilder::make(Opcode::ResolveCallTarget, { match.callee });
    target.setArgumentCount(match.argumentCount);
    return target;
}

static void convertToCallResolved(Value& value, ValueID target)
{
    Value call = value;
    value.become(Opcode::CallResolved);
    value.setCalleeOperand(call.calleeOperand());
    value.setTargetOperand(target);
    value.setThisValueOperand(call.thisValueOperand());
    value.setArgumentsOperands(call.argumentsOperands());
    value.setCalleeName(call.calleeName());
    value.setAccessSite(call.accessSite());
    value.setCallees(call.callees());
    value.setIsDemotedTailCall(call.isDemotedTailCall());
}

void selectResolvedCallTarget(SelectionContext& context)
{
    Vector<ResolvedCalls> matches = findCallsOfInvariantCallees(context);
    matches.removeAllMatching([&](const ResolvedCalls& match) {
        return !canEmitOnLoopEntry(context, match.header) || context.tryClaimAll(SelectionRuleID::ResolvedCallTarget, groupsOf(match).span()) != ClaimResult::Claimed;
    });

    context.rewriteGroups(SelectionRuleID::ResolvedCallTarget, matches, [&](GroupRewriter& rewriter, const ResolvedCalls& match) {
        ValueID target;
        LoopEntryResult result = emitOnLoopEntry(context, rewriter.builder(), match.header, [&](IRBuilder& builder) {
            target = builder.append(makeResolveCallTarget(match));
        });
        if (result != LoopEntryResult::Emitted)
            return;
        for (ValueID call : match.calls)
            convertToCallResolved(rewriter.function().at(call), target);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
