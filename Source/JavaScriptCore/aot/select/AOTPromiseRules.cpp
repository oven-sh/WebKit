/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTCoroutines.h"
#include "AOTEffects.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTSelectionContext.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class PromiseOperand : uint8_t {
    Unknown,
    Promise,
    NoPromise,
    PromiseOrUndefined,
};

struct Match {
    SelectionGroup group;
    PromiseOperand promise;
    bool testsThenable;
};

class PointsBeforeSuspension {
public:
    explicit PointsBeforeSuspension(SelectionContext&);

    bool contains(ValueID user) const;

private:
    SelectionContext& m_context;
    BitVector m_suspensions;
    BitVector m_blocksAfterSuspension;
};

} // anonymous namespace

PointsBeforeSuspension::PointsBeforeSuspension(SelectionContext& context)
    : m_context(context)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    const CFG& cfg = analyses.cfg();
    for (BlockID block : cfg.reversePostOrder()) {
        bool hasSuspension = analyses.effectsOfTerminator(block).onAnyEdge().maySuspend;
        for (ValueID id : function.at(block).values) {
            if (!analyses.effectsOf(id).onAnyEdge().maySuspend)
                continue;
            m_suspensions.set(id.index());
            hasSuspension = true;
        }
        if (!hasSuspension)
            continue;
        for (BlockID successor : cfg.successors(block)) {
            if (!m_blocksAfterSuspension.get(successor.index()))
                m_blocksAfterSuspension.merge(cfg.blocksReachableFrom(successor, m_blocksAfterSuspension));
        }
    }
}

bool PointsBeforeSuspension::contains(ValueID user) const
{
    ValueLocation location = m_context.analyses().useLists().locationOf(user);
    if (location.placement != ValuePlacement::InBlock || m_blocksAfterSuspension.get(location.block.index()))
        return false;
    return std::ranges::none_of(m_context.function().at(location.block).values.span().first(location.index), [&](ValueID earlier) {
        return m_suspensions.get(earlier.index());
    });
}

// Until an operation takes it, a promise is known to nobody: it is pending and has no reaction, whatever runs after its allocation.
static bool isFirstUseOfNewPromise(SelectionContext& context, ValueID user, ValueID promise)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();
    ValueID allocation = analyses.identityOf(promise);
    if (function.at(allocation).opcode != Opcode::NewPromise)
        return false;

    ValueLocation allocated = analyses.useLists().locationOf(allocation);
    ValueLocation used = analyses.useLists().locationOf(user);
    if (allocated.placement != ValuePlacement::InBlock || used.placement != ValuePlacement::InBlock || allocated.block != used.block)
        return false;

    std::span<const ValueID> valuesBetween = function.at(used.block).values.span().first(used.index).subspan(allocated.index + 1);
    return std::ranges::none_of(valuesBetween, [&](ValueID id) {
        const Value& value = function.at(id);
        if (isCast(value.opcode))
            return false;
        bool takesPromise = false;
        value.forEachChild([&](ValueID operand) {
            takesPromise |= analyses.identityOf(operand) == allocation;
        });
        return takesPromise;
    });
}

static bool mayBeThenable(SelectionContext& context, ValueID value)
{
    return protocolMayFindMember(context.program(), context.typeOf(value), Protocol::Thenable);
}

static PromiseOperand classifyPromiseOperand(const Type& type)
{
    if (type.isObject())
        return PromiseOperand::Promise;
    if (type.primitives != Primitive::Undefined || !type.isJavaScriptValue())
        return PromiseOperand::Unknown;
    return type.hasObjects() ? PromiseOperand::PromiseOrUndefined : PromiseOperand::NoPromise;
}

static void appendFulfillment(IRBuilder& builder, ValueID promise, ValueID value, ValueID guard)
{
    Value fulfillment = IRBuilder::make(Opcode::FulfillFreshPromise, { promise, value });
    if (guard)
        fulfillment.setGuardOperand(guard);
    builder.append(WTF::move(fulfillment));
}

static void replaceRootWithUpcast(GroupRewriter& rewriter, ValueID result)
{
    Value& root = rewriter.replaceRoot(Opcode::Upcast);
    root.setValueOperand(result);
    root.setCastType(TypeTable::anything);
}

static void emitArms(GroupRewriter& rewriter, const Match& match, ValueID promise, ValueID value)
{
    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();

    ValueID guard;
    if (match.testsThenable) {
        ValueID test = builder.append(Opcode::IsNotThenable, { value });
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Rare);
        guard = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        rewriter.endArm(rewriter.appendOriginals());
        builder.moveToEndOf(branch.taken);
    }

    switch (match.promise) {
    case PromiseOperand::Promise:
        appendFulfillment(builder, promise, value, guard);
        rewriter.endArm(builder.appendUpcast(promise, TypeTable::anything));
        return;
    case PromiseOperand::NoPromise:
        rewriter.endArm(builder.appendUpcast(value, TypeTable::anything));
        return;
    case PromiseOperand::PromiseOrUndefined: {
        ValueID isUndefined = builder.appendTypeTest(promise, TypeTable::undefined);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isUndefined);
        rewriter.endArm(builder.appendUpcast(value, TypeTable::anything));

        builder.moveToEndOf(branch.notTaken);
        appendFulfillment(builder, branch.provenOnNotTaken, value, guard);
        rewriter.endArm(builder.appendUpcast(branch.provenOnNotTaken, TypeTable::anything));
        return;
    }
    case PromiseOperand::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void rewriteResolution(GroupRewriter& rewriter, const Match& match, ValueID promise, ValueID value)
{
    if (!match.testsThenable) {
        Value& root = rewriter.replaceRoot(Opcode::FulfillFreshPromise);
        root.setPromiseOperand(promise);
        root.setValueOperand(value);
        return;
    }

    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();
    ValueID test = builder.append(Opcode::IsNotThenable, { value });
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Rare);
    builder.moveToEndOf(branch.notTaken);
    rewriter.appendOriginals();
    rewriter.endArm(std::nullopt);

    builder.moveToEndOf(branch.taken);
    appendFulfillment(builder, promise, value, branch.provenOnTaken);
    rewriter.endArm(std::nullopt);
}

static void rewrite(GroupRewriter& rewriter, const Match& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID promise = original.promiseOperand();
    ValueID value = original.valueOperand();
    if (original.opcode == Opcode::ResolvePromise) {
        rewriteResolution(rewriter, match, promise, value);
        return;
    }
    if (match.group.hasSlowPath) {
        emitArms(rewriter, match, promise, value);
        return;
    }

    switch (match.promise) {
    case PromiseOperand::Promise:
        rewriter.insert(IRBuilder::make(Opcode::FulfillFreshPromise, { promise, value }));
        replaceRootWithUpcast(rewriter, promise);
        return;
    case PromiseOperand::NoPromise:
        replaceRootWithUpcast(rewriter, value);
        return;
    case PromiseOperand::PromiseOrUndefined:
    case PromiseOperand::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void matchResultsOfAsyncFunction(SelectionContext& context, Vector<Match>& matches)
{
    const Function& function = context.function();
    if (function.role != FunctionRole::CoroutineBody || coroutineOfBody(function).kind != CoroutineKind::AsyncFunction)
        return;
    if (context.candidates(Opcode::ReturnAsyncResult).empty())
        return;

    ValueID driver = driverOf(function);
    PointsBeforeSuspension points(context);
    for (ValueID id : context.candidates(Opcode::ReturnAsyncResult)) {
        const Value& value = function.at(id);
        if (context.analyses().identityOf(value.promiseOperand()) != driver)
            continue;
        if (!points.contains(id))
            continue;

        PromiseOperand promise = classifyPromiseOperand(context.typeOf(value.promiseOperand()));
        if (promise == PromiseOperand::Unknown)
            continue;

        bool testsThenable = mayBeThenable(context, value.valueOperand());
        if (testsThenable && context.shapeAt(id) == CodeShape::Compact)
            continue;

        bool needsArms = testsThenable || promise == PromiseOperand::PromiseOrUndefined;
        // Each copy of a ReturnAsyncResult must be returned by its own block.
        if (needsArms && !context.isOnlyReturned(id))
            continue;

        Match match { groupOfOne(id), promise, testsThenable };
        match.group.hasSlowPath = needsArms;
        if (context.tryClaim(SelectionRuleID::FreshPromiseFulfillment, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }
}

static void matchResolutionsOfNewPromises(SelectionContext& context, Vector<Match>& matches)
{
    const Function& function = context.function();
    for (ValueID id : context.candidates(Opcode::ResolvePromise)) {
        const Value& value = function.at(id);
        bool testsThenable = mayBeThenable(context, value.valueOperand());
        if (testsThenable && context.shapeAt(id) == CodeShape::Compact)
            continue;
        if (!isFirstUseOfNewPromise(context, id, value.promiseOperand()))
            continue;

        Match match { groupOfOne(id), PromiseOperand::Promise, testsThenable };
        match.group.hasSlowPath = testsThenable;
        if (context.tryClaim(SelectionRuleID::FreshPromiseFulfillment, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }
}

void selectFreshPromiseFulfillment(SelectionContext& context)
{
    Vector<Match> matches;
    matchResultsOfAsyncFunction(context, matches);
    matchResolutionsOfNewPromises(context, matches);
    context.rewriteGroups(SelectionRuleID::FreshPromiseFulfillment, matches, [](GroupRewriter& rewriter, const Match& match) {
        rewrite(rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
