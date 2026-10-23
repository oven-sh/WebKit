/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTCoroutines.h"
#include "AOTEffects.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTSelectionContext.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
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

struct PromiseMatch {
    SelectionGroup group;
    PromiseOperand promise;
    bool testsThenable;
};

struct AwaitOfPrimitive {
    SelectionGroup group;
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
    auto suspendsOnSomeEdge = [](const EffectsOnEdges& effects) {
        return effects.normal.maySuspend || effects.handler.maySuspend;
    };
    for (BlockID block : cfg.reversePostOrder()) {
        bool hasSuspension = declaredBoundOf(function.at(block).terminator.kind).maySuspend && suspendsOnSomeEdge(analyses.effectsOfTerminator(block));
        for (ValueID id : function.at(block).values) {
            if (!maySuspend(function.at(id).opcode) || !suspendsOnSomeEdge(analyses.effectsOf(id)))
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
    if (!analyses.opcodesOfValues().contains(Opcode::NewPromise))
        return false;
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

static void emitArms(GroupRewriter& rewriter, const PromiseMatch& match, ValueID promise, ValueID value)
{
    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();

    ValueID guard;
    if (match.testsThenable) {
        BlockID failedUnasked = builder.beginPrediction();
        ValueID test = builder.append(Opcode::IsNotThenable, { value });
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Rare);
        builder.endPrediction(failedUnasked, branch.notTaken);
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

static void rewriteResolution(GroupRewriter& rewriter, const PromiseMatch& match, ValueID promise, ValueID value)
{
    if (!match.testsThenable) {
        Value& root = rewriter.replaceRoot(Opcode::FulfillFreshPromise);
        root.setPromiseOperand(promise);
        root.setValueOperand(value);
        return;
    }

    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();
    BlockID failedUnasked = builder.beginPrediction();
    ValueID test = builder.append(Opcode::IsNotThenable, { value });
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, 0, BlockFrequency::Normal, BlockFrequency::Rare);
    builder.endPrediction(failedUnasked, branch.notTaken);
    builder.moveToEndOf(branch.notTaken);
    rewriter.appendOriginals();
    rewriter.endArm(std::nullopt);

    builder.moveToEndOf(branch.taken);
    appendFulfillment(builder, promise, value, branch.provenOnTaken);
    rewriter.endArm(std::nullopt);
}

static void rewrite(GroupRewriter& rewriter, const PromiseMatch& match)
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

static void matchResultsOfAsyncFunction(SelectionContext& context, Vector<PromiseMatch>& matches)
{
    const Function& function = context.function();
    if (function.role != FunctionRole::CoroutineBody || coroutineOfBody(function).kind != CoroutineKind::AsyncFunction)
        return;
    if (context.candidates(Opcode::ReturnAsyncResult).empty())
        return;

    ValueID driver = driverOf(function);
    std::optional<PointsBeforeSuspension> points;
    for (ValueID id : context.candidates(Opcode::ReturnAsyncResult)) {
        const Value& value = function.at(id);
        if (context.analyses().identityOf(value.promiseOperand()) != driver)
            continue;
        if (!points)
            points.emplace(context);
        if (!points->contains(id))
            continue;

        PromiseOperand promise = classifyPromiseOperand(context.typeOf(value.promiseOperand()));
        if (promise == PromiseOperand::Unknown)
            continue;

        bool testsThenable = mayBeThenable(context, value.valueOperand());
        bool needsArms = testsThenable || promise == PromiseOperand::PromiseOrUndefined;
        if (needsArms && context.shapeAt(id) != CodeShape::Fast)
            continue;

        // Each copy of a ReturnAsyncResult must be returned by its own block.
        if (needsArms && !context.isOnlyReturned(id))
            continue;

        PromiseMatch match { groupOfOne(id), promise, testsThenable };
        match.group.hasSlowPath = needsArms;
        if (context.tryClaim(SelectionRuleID::FreshPromiseFulfillment, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }
}

static void matchResolutionsOfNewPromises(SelectionContext& context, Vector<PromiseMatch>& matches)
{
    const Function& function = context.function();
    for (ValueID id : context.candidates(Opcode::ResolvePromise)) {
        const Value& value = function.at(id);
        bool testsThenable = mayBeThenable(context, value.valueOperand());
        if (testsThenable && context.shapeAt(id) != CodeShape::Fast)
            continue;
        if (!isFirstUseOfNewPromise(context, id, value.promiseOperand()))
            continue;

        PromiseMatch match { groupOfOne(id), PromiseOperand::Promise, testsThenable };
        match.group.hasSlowPath = testsThenable;
        if (context.tryClaim(SelectionRuleID::FreshPromiseFulfillment, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }
}

void selectFreshPromiseFulfillment(SelectionContext& context)
{
    Vector<PromiseMatch> matches;
    matchResultsOfAsyncFunction(context, matches);
    matchResolutionsOfNewPromises(context, matches);
    context.rewriteGroups(SelectionRuleID::FreshPromiseFulfillment, matches, [](GroupRewriter& rewriter, const PromiseMatch& match) {
        rewrite(rewriter, match);
    });
}

static bool awaitsPrimitive(SelectionContext& context, ValueID await)
{
    constexpr uint16_t presentPrimitives = Primitive::Defined & ~Primitive::Absent;
    const Type& awaited = context.typeOf(context.function().at(await).awaitedOperand());
    return !awaited.isNever() && awaited.hasOnlyPrimitives(presentPrimitives) && awaited.isSubsetOf(context.typeOf(await), context.program());
}

static void rewriteAwaitOfPrimitive(GroupRewriter& rewriter, const AwaitOfPrimitive& match)
{
    const Value& original = rewriter.original(match.group.root);
    ValueID awaited = original.awaitedOperand();
    TypeID type = original.type;

    rewriter.beginArms();
    IRBuilder& builder = rewriter.builder();
    BlockID failedUnasked = builder.beginPrediction();
    IRBuilder::BranchBlocks arms = builder.emitBranch(builder.append(Opcode::AwaitMayContinue, { }));
    builder.endPrediction(failedUnasked, arms.notTaken);
    rewriter.endArm(builder.appendUpcast(awaited, type));

    builder.moveToEndOf(arms.notTaken);
    rewriter.endArm(rewriter.appendOriginals());
}

// An await of a primitive queues the job that resumes the function.
void selectAwaitInPlace(SelectionContext& context)
{
    const Function& function = context.function();
    if (function.role != FunctionRole::CoroutineBody || coroutineOfBody(function).kind != CoroutineKind::AsyncFunction)
        return;

    Vector<AwaitOfPrimitive> matches;
    for (ValueID id : context.candidates(Opcode::AwaitBySavingFrame)) {
        if (context.shapeAt(id) != CodeShape::Fast || !awaitsPrimitive(context, id))
            continue;

        AwaitOfPrimitive match { groupOfOne(id) };
        match.group.hasSlowPath = true;
        if (context.tryClaim(SelectionRuleID::AwaitInPlace, match.group) == ClaimResult::Claimed)
            matches.append(WTF::move(match));
    }
    context.rewriteGroups(SelectionRuleID::AwaitInPlace, matches, [](GroupRewriter& rewriter, const AwaitOfPrimitive& match) {
        rewriteAwaitOfPrimitive(rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
