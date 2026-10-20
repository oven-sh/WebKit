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
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t smallestPlanSize = 1;

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
    // ResolveBuiltins has seen the call and does not know its callee. Whatever is called, the result is validated.
    if (value.opcode == Opcode::Call)
        return isValidated(program, value.expectedResult());
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    return matchedFormOf(program, value) != TypedResultForm::None || isValidated(program, value.expectedResult());
}

static TypedResultTarget findTarget(FunctionPassContext& context, ValueID call)
{
    return findTypedResultTarget(context.program, context.function, context.analyses.useLists(), call);
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

static ValueID findImmediateAwait(FunctionPassContext& context, const TypedResultSite& site)
{
    const UseLists& uses = context.analyses.useLists();
    auto isOperandInBlockOfCall = [&](const std::optional<Use>& use) {
        return use && use->kind == Use::Kind::Operand && use->block == site.block;
    };

    ValueID promise = site.call;
    std::optional<Use> use = uses.singleUse(promise);
    while (isOperandInBlockOfCall(use) && isCast(context.function.at(use->user).opcode)) {
        promise = use->user;
        use = uses.singleUse(promise);
    }
    if (!isOperandInBlockOfCall(use) || !isAwaitOf(context.function.at(use->user), promise))
        return { };

    const BlockValues& values = context.function.at(site.block).values;
    for (uint32_t index = site.index + 1, end = uses.locationOf(use->user).index; index < end; ++index) {
        if (!context.analyses.effectsOf(values[index]).onAnyEdge().isRemovableWhenUnused())
            return { };
    }
    return use->user;
}

static TypedResultChoice chooseParsedBody(FunctionPassContext& context, TypedResultSite site)
{
    site.await = findImmediateAwait(context, site);
    if (!site.await)
        return { .declineReason = TypedResultDeclineReason::PromiseIsNotAwaitedAtOnce };

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

// A frame has one source: opening a second one gives the buffer of the first away, and its cursors dangle.
static bool isSourceOpenAcross(FunctionPassContext& context, const TypedResultSite& site)
{
    const Function& function = context.function;
    Liveness::LocalCalc liveAfterCall(context.analyses.liveness(), site.block);
    for (uint32_t index = function.at(site.block).values.size(); index > site.index + 1;)
        liveAfterCall.execute(--index);

    bool isOpen = false;
    liveAfterCall.forEachLive([&](ValueID id) {
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
    return { .site = site };
}

// Flow filled the objects it allocated at the call from the field types as read. Its own predictions have replaced those since, and they can name a layout that flow never put there.
static bool reachesOnlyModeledLayouts(const NodeDecisionInputs& inputs, AccessSiteID call, TypeID type)
{
    std::span<const LayoutID> modeledLayouts = inputs.flow.typedResultLayouts(call);
    return std::ranges::all_of(layoutsReachedBy(inputs.program, type), [&](LayoutID layout) {
        return !allocatesLayout(inputs, layout) || std::ranges::binary_search(modeledLayouts, layout);
    });
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
        if (!type && findTarget(context, site.call))
            return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };
        if (type && !reachesOnlyModeledLayouts({ program, flow, context.build }, call.accessSite(), type))
            return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };
    } else
        type = expected ? program.at(expected).type : findTarget(context, site.call).type;
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
