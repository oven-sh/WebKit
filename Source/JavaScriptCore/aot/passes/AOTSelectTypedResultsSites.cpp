/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTThreshold.h"
#include "AOTValueFlowTypedResults.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(uint64_t, smallestPlanSize, 1);

static bool isValidated(const Program& program, ExpectedResultID expected)
{
    return expected && !program.at(expected).validation.isEmpty();
}

static TypedResultForm matchedFormOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin)
        return TypedResultForm::None;
    switch (call.builtinCallForm()) {
    case BuiltinCallForm::Whole:
        return typedResultFormOf(program, call);
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::AnyReceiver:
    case BuiltinCallForm::List:
        return TypedResultForm::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool mayHaveTypedResult(const Program& program, const Value& value)
{
    if (!typedResultCallOpcodes.contains(value.opcode))
        return false;
    // ResolveBuiltins has seen the call and does not know its callee. Whatever is called, the result is validated.
    if (value.opcode == Opcode::Call)
        return isValidated(program, value.expectedResult());
    return matchedFormOf(program, value) != TypedResultForm::None || isValidated(program, value.expectedResult());
}

static TypeID findTypeOfTarget(FunctionPassContext& context, ValueID call)
{
    return findTypeOfTypedResultTarget(context.program, context.function, context.analyses.useLists(), call);
}

static bool isCompiledContainer(const NodeDecisionInputs& inputs, ResultNodeRef ref, const ResultNode& node)
{
    return node.isContainer() && decideNodeCode(inputs, ref, NodeUse::ParsedFromText).code == NodeCode::Compiled;
}

static bool hasTypedPart(const NodeDecisionInputs& inputs, ResultNodeRef root)
{
    ResultNode node = describeNode(inputs.program, root);
    if (!isCompiledContainer(inputs, root, node))
        return false;
    if (node.shape != ResultShape::Union)
        return true;
    return node.children.containsIf([&](ResultNodeRef arm) {
        return isCompiledContainer(inputs, arm, describeNode(inputs.program, arm));
    });
}

static std::optional<ResultNode> findOnlyContainer(const Program& program, ResultNodeRef ref)
{
    ResultNode node = describeNode(program, ref);
    if (!node.isContainer())
        return std::nullopt;
    if (node.shape != ResultShape::Union)
        return node;

    std::optional<ResultNode> container;
    for (ResultNodeRef arm : node.children) {
        ResultNode alternative = describeNode(program, arm);
        if (!alternative.isContainer())
            continue;
        if (container)
            return std::nullopt;
        container = WTF::move(alternative);
    }
    return container;
}

static bool isAwaitOf(const Value& user, ValueID promise)
{
    if (user.opcode == Opcode::Await)
        return user.valueOperand() == promise;
    return user.opcode == Opcode::AwaitBySavingFrame && user.awaitedOperand() == promise;
}

static bool hasOnlyRemovableValues(FunctionPassContext& context, BlockID block, uint32_t first, uint32_t end)
{
    const BlockValues& values = context.function.at(block).values;
    for (uint32_t index = first; index < end; ++index) {
        if (!context.analyses.isUnobservable(values[index]))
            return false;
    }
    return true;
}

// The one use of the promise behind the casts of it that stand in the block. `promise` becomes the last of those casts.
static std::optional<Use> findUseBehindCasts(FunctionPassContext& context, BlockID block, ValueID& promise)
{
    const UseLists& uses = context.analyses.useLists();
    std::optional<Use> use = uses.singleUse(promise);
    while (use && use->kind == Use::Kind::Operand && use->block == block && isCast(context.function.at(use->user).opcode)) {
        promise = use->user;
        use = uses.singleUse(promise);
    }
    return use;
}

// `firstIndex` is where the values start that run between the promise and its await.
static ValueID findAwaitInBlock(FunctionPassContext& context, BlockID block, uint32_t firstIndex, ValueID promise)
{
    std::optional<Use> use = findUseBehindCasts(context, block, promise);
    if (!use || use->kind != Use::Kind::Operand || use->block != block || !isAwaitOf(context.function.at(use->user), promise))
        return { };
    if (!hasOnlyRemovableValues(context, block, firstIndex, context.analyses.useLists().locationOf(use->user).index))
        return { };
    return use->user;
}

// Every way into the block is a jump, which can say whether it brings the promise of the call.
static bool isEnteredByJumpsAlone(FunctionPassContext& context, BlockID block)
{
    const Function& function = context.function;
    if (function.at(block).isHandlerBlock() || context.analyses.isEnteredFromUnreachableBlock(block))
        return false;
    return std::ranges::all_of(context.analyses.cfg().incomingEdges(block), [&](Edge edge) {
        return !edge.isHandlerEdge() && edge.from != block && function.at(edge.from).terminator.kind == TerminatorKind::Jump;
    });
}

static void findImmediateAwait(FunctionPassContext& context, TypedResultSite& site)
{
    const Function& function = context.function;
    site.awaitBlock = site.block;
    site.await = findAwaitInBlock(context, site.block, site.index + 1, site.call);
    if (site.await)
        return;

    ValueID promise = site.call;
    std::optional<Use> use = findUseBehindCasts(context, site.block, promise);
    if (!use || function.at(site.block).terminator.kind != TerminatorKind::Jump)
        return;

    // A pass that cuts a block behind a call leaves a jump there, into a block that nothing else enters.
    BlockID next = function.at(site.block).terminator.next().block;
    if (use->kind == Use::Kind::Operand && use->block == next && promise == site.call) {
        if (!context.analyses.cfg().singleIncomingEdge(next) || !isEnteredByJumpsAlone(context, next) || function.at(next).handler != function.at(site.block).handler)
            return;
        if (!hasOnlyRemovableValues(context, site.block, site.index + 1, function.at(site.block).values.size()))
            return;
        site.awaitBlock = next;
        site.await = findAwaitInBlock(context, next, 0, site.call);
        return;
    }

    if (use->kind != Use::Kind::EdgeArgument || use->block != site.block)
        return;
    BlockID merge = targetOf(function, use->edge()).block;
    if (!isEnteredByJumpsAlone(context, merge) || function.at(merge).handler != function.at(site.block).handler || use->operandIndex >= function.at(merge).parameters.size())
        return;
    if (!hasOnlyRemovableValues(context, site.block, site.index + 1, function.at(site.block).values.size()))
        return;

    site.awaitBlock = merge;
    site.mergedPromise = function.at(merge).parameters[use->operandIndex];
    site.await = findAwaitInBlock(context, merge, 0, site.mergedPromise);
}

// A frame has one source: opening a second one gives the buffer of the first away, and its cursors dangle.
static bool isSourceOpenAcross(FunctionPassContext& context, BlockID block, uint32_t indexInBlock)
{
    const Function& function = context.function;
    Liveness::LocalCalc liveAfterValue(context.analyses.liveness(), block);
    for (uint32_t index = function.at(block).values.size(); index > indexInBlock + 1;)
        liveAfterValue.execute(--index);

    bool isOpen = false;
    liveAfterValue.forEachLive([&](ValueID id) {
        if (context.program.typeOf(function, id).isOpaque(OpaqueKind::Cursor)) {
            isOpen = true;
            return;
        }
        if (!isGuardValue(function, id))
            return;
        const GuardFacts& facts = context.analyses.guardFacts(id);
        isOpen |= !facts.isKnown() || facts.find(Proof::SourceOpen);
    });
    return isOpen;
}

static bool isSourceOpenAcross(FunctionPassContext& context, const TypedResultSite& site)
{
    return isSourceOpenAcross(context, site.block, site.index);
}

static TypedResultChoice chooseParsedBody(FunctionPassContext& context, TypedResultSite site)
{
    findImmediateAwait(context, site);
    if (!site.await)
        return { .declineReason = TypedResultDeclineReason::PromiseIsNotAwaitedAtOnce };
    // Another arm may bring an open source along.
    if (site.mergedPromise && isSourceOpenAcross(context, site.awaitBlock, context.analyses.useLists().locationOf(site.await).index))
        return { .declineReason = TypedResultDeclineReason::AnotherSourceIsOpen };

    const BuiltinTable& builtins = context.program.builtins();
    BuiltinCall call = builtinCallOf(context.program, context.function.at(site.call));
    const BuiltinForms* forms = builtins.formsOf(builtins.canonicalID(builtins.at(call.overload).builtin));
    if (!forms || !forms->bodyBytesForm)
        return { .declineReason = TypedResultDeclineReason::HostHasNoBytesForm };
    if (!canCallBodyBytesForm({ context.program, context.function, context.types }, forms->bodyBytesForm, call.receiver))
        return { .declineReason = TypedResultDeclineReason::HostHasNoBytesForm };

    site.source = call.receiver;
    return { .site = site };
}

static bool hasExpectedRows(FunctionPassContext& context, ValueID call)
{
    for (const Use& use : context.analyses.useLists().usesOf(call)) {
        if (use.kind == Use::Kind::Operand && context.function.at(use.user).opcode == Opcode::ExpectRows)
            return true;
    }
    return false;
}

static TypedResultChoice chooseRows(FunctionPassContext& context, TypedResultSite site)
{
    const Program& program = context.program;
    std::optional<ResultNode> row;
    if (std::optional<ResultNode> rows = findOnlyContainer(program, site.root); rows && rows->shape == ResultShape::Array)
        row = findOnlyContainer(program, rows->children[0]);
    if (!row || row->shape != ResultShape::Object || !row->layout)
        return { .declineReason = TypedResultDeclineReason::RowsHaveSeveralLayouts };
    if (!allocatesLayout({ program, context.inputs.flow, context.build }, *row))
        return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };

    site.rowLayout = row->layout;
    return { .site = site };
}

static TypedResultChoice chooseValidatedResult(FunctionPassContext& context, TypedResultSite site, ExpectedResultID expected)
{
    const Program& program = context.program;
    site.root = ResultNodeRef::forValidation(canonicalValidationOf(program, expected), 0);

    // Flow allocated nothing at the call, so no object with a layout comes out of it, not even one with a layout that flow allocated at another call.
    const FlowResults& flow = context.inputs.flow;
    if (flow.hasResults() && !flow.typedResultType(context.function.at(site.call).accessSite())) {
        site.isValidatedByPlanAlone = true;
        context.exceeds(LimitID::MaxObjectsOfTypedResult, typedResultOf(program, expected, context.limit(LimitID::MaxObjectsOfTypedResult)).numberOfObjects(), site.call);
        if (site.form == TypedResultForm::ParsedText)
            site.form = TypedResultForm::None;
    }

    switch (site.form) {
    case TypedResultForm::ParsedText:
    case TypedResultForm::ValidatedValue:
        site.source = typedResultSourceOf(program, context.function.at(site.call));
        if (!site.source)
            site.form = TypedResultForm::None;
        break;
    case TypedResultForm::None:
    case TypedResultForm::ParsedBody:
    case TypedResultForm::Rows:
        break;
    }
    if (site.form == TypedResultForm::ParsedText && isSourceOpenAcross(context, site))
        site.form = TypedResultForm::None;

    context.exceeds(LimitID::MaxParserPlanNodes, program.at(expected).validation.size(), site.call);
    return { .site = site, .declineReason = site.isValidatedByPlanAlone ? TypedResultDeclineReason::TargetIsNotModeledByFlow : TypedResultDeclineReason::None };
}

// Flow filled the objects it allocated at the call from the field types as read. Its own predictions have replaced those since, and they can name a layout that flow never put there.
static bool reachesOnlyModeledLayouts(const NodeDecisionInputs& inputs, AccessSiteID call, TypeID type)
{
    std::span<const LayoutID> modeledLayouts = inputs.flow.typedResultLayouts(call);
    bool reachesOnlyModeled = true;
    auto check = [&](LayoutID layout) {
        reachesOnlyModeled = !allocatesLayout(inputs, layout) || std::ranges::binary_search(modeledLayouts, layout);
        return reachesOnlyModeled ? IterationStatus::Continue : IterationStatus::Done;
    };
    forEachLayoutReachedBy(inputs.program, type, nullptr, ScopedLambda<IterationStatus(LayoutID)>(check));
    return reachesOnlyModeled;
}

static TypedResultChoice chooseUnvalidatedResult(FunctionPassContext& context, TypedResultSite site, ExpectedResultID expected)
{
    const Program& program = context.program;
    const FlowResults& flow = context.inputs.flow;
    const Value& call = context.function.at(site.call);

    // The generic side of an expansion is a rare block, which ends the rounds. How often the call runs says nothing about how often its result is read.
    if (context.function.at(site.block).frequency == BlockFrequency::Rare)
        return { };
    if (site.form == TypedResultForm::Rows && hasExpectedRows(context, site.call))
        return { };

    TypeID type;
    if (flow.hasResults()) {
        type = flow.typedResultType(call.accessSite());
        if (!type && findTypeOfTarget(context, site.call))
            return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };
        if (type && !reachesOnlyModeledLayouts({ program, flow, context.build }, call.accessSite(), type))
            return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };
    } else
        type = expected ? program.at(expected).type : findTypeOfTarget(context, site.call);
    if (!type)
        return { .declineReason = TypedResultDeclineReason::NoTarget };

    site.root = ResultNodeRef::forType(type);
    if (!hasTypedPart({ program, flow, context.build }, site.root))
        return { .declineReason = TypedResultDeclineReason::TargetHasNoTypedPart };
    if (context.exceeds(LimitID::MaxParserPlanNodes, smallestPlanSize, site.call))
        return { };

    switch (site.form) {
    case TypedResultForm::None:
    case TypedResultForm::ValidatedValue:
        return { };
    case TypedResultForm::ParsedText:
        site.source = typedResultSourceOf(program, call);
        if (!site.source)
            return { };
        if (isSourceOpenAcross(context, site))
            return { .declineReason = TypedResultDeclineReason::AnotherSourceIsOpen };
        return { .site = site };
    case TypedResultForm::ParsedBody:
        if (isSourceOpenAcross(context, site))
            return { .declineReason = TypedResultDeclineReason::AnotherSourceIsOpen };
        return chooseParsedBody(context, site);
    case TypedResultForm::Rows:
        return chooseRows(context, site);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypedResultChoice chooseTypedResult(FunctionPassContext& context, BlockID block, uint32_t index)
{
    ValueID id = context.function.at(block).values[index];
    const Value& call = context.function.at(id);
    ASSERT(mayHaveTypedResult(context.program, call));

    TypedResultSite site { .block = block, .index = index, .call = id, .form = matchedFormOf(context.program, call) };
    ExpectedResultID expected = call.expectedResult();
    if (isValidated(context.program, expected))
        return chooseValidatedResult(context, site, expected);
    return chooseUnvalidatedResult(context, site, expected);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
