/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTJSONEmission.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTSelectTypedResultsNodes.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTStringArena.h"
#include "AOTThreshold.h"
#include "AOTValueFlowTypedResults.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(uint64_t, smallestPlanSize, 1);

namespace {

struct TypedResultSite {
    BlockID block;
    uint32_t index { 0 };
    ValueID call;
    ValueID source;
    ValueID await;
    // Of a body.
    BlockID awaitBlock;
    ValueID mergedPromise;
    BuiltinID bodyBytesForm;
    TypedResultForm form { TypedResultForm::None };
    ResultNodeRef root;
    LayoutID rowLayout;
};

struct TypedResultChoice {
    std::optional<TypedResultSite> site;
    TypedResultDeclineReason declineReason { TypedResultDeclineReason::None };
};

} // anonymous namespace

// Which calls are expanded.

static TypedResultForm matchedFormOf(const Program& program, const Value& call)
{
    bool isWholeCall = call.opcode == Opcode::CallBuiltin && call.builtinCallForm() == BuiltinCallForm::Whole;
    return isWholeCall ? typedResultFormOf(program, call) : TypedResultForm::None;
}

static std::optional<Value> tryMakeBuiltinCall(const TypingContext& typing, BuiltinID callee, ValueID argument)
{
    BuiltinCallOperands operands { { }, { argument } };
    BuiltinOperandTypes types = operandTypesOf(typing, operands.receiver, operands.arguments.span());
    OverloadMatch match = matchOverload(OverloadQuery { typing, callee, BuiltinCallMode::Call, types, types });
    if (!match.overload)
        return std::nullopt;

    Value call = makeBuiltinCall(match.overload, BuiltinCallForm::Whole, operands, { });
    bool isMatched = match.state == BuiltinMatchState::Proven && match.requirements.isEmpty() && overloadTakesOperands(typing.program, typing.function, call);
    if (!isMatched)
        call.setBuiltinCallForm(BuiltinCallForm::AnyReceiver);
    return call;
}

static bool isCompiledContainer(const NodeDecisionInputs& inputs, ResultNodeRef ref, const ResultNode& node)
{
    return node.isContainer() && decideNodeCode(inputs, ref).code == NodeCode::Compiled;
}

static bool hasTypedPart(const NodeDecisionInputs& inputs, ResultNodeRef root)
{
    ResultNode node = describeNode(inputs.program, root);
    if (!isCompiledContainer(inputs, root, node))
        return false;
    return node.shape != ResultShape::Union || node.children.containsIf([&](ResultNodeRef arm) {
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
    if (!use || use->kind != Use::Kind::Operand || use->block != block)
        return { };
    const Value& user = context.function.at(use->user);
    std::optional<uint32_t> awaited = indexOfAwaitedOperand(user);
    if (!awaited || user.operand(*awaited) != promise)
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
    if (!forms || !forms->bodyBytesForm || !tryMakeBuiltinCall({ context.program, context.function, context.types }, forms->bodyBytesForm, call.receiver))
        return { .declineReason = TypedResultDeclineReason::HostHasNoBytesForm };

    site.bodyBytesForm = forms->bodyBytesForm;
    site.source = call.receiver;
    return { .site = site };
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

static TypedResultChoice chooseResult(FunctionPassContext& context, TypedResultSite site, ExpectedResultID expected)
{
    const Program& program = context.program;
    const FlowResults& flow = context.inputs.flow;
    const Value& call = context.function.at(site.call);
    auto findTypeOfTarget = [&] {
        return findTypeOfTypedResultTarget(program, context.function, context.analyses.useLists(), site.call);
    };
    auto hasExpectedRows = [&] {
        return std::ranges::any_of(context.analyses.useLists().usesOf(site.call), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && context.function.at(use.user).opcode == Opcode::ExpectRows;
        });
    };

    // The generic side of an expansion is a rare block, which ends the rounds. How often the call runs says nothing about how often its result is read.
    if (context.function.at(site.block).frequency == BlockFrequency::Rare)
        return { };
    if (site.form == TypedResultForm::Rows && hasExpectedRows())
        return { };

    TypeID type;
    if (flow.hasResults()) {
        type = flow.typedResultType(call.accessSite());
        if (!type && findTypeOfTarget())
            return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };
        if (type && !reachesOnlyModeledLayouts({ program, flow, context.build }, call.accessSite(), type))
            return { .declineReason = TypedResultDeclineReason::TargetIsNotModeledByFlow };
    } else
        type = expected ? program.at(expected).type : findTypeOfTarget();
    if (!type)
        return { .declineReason = TypedResultDeclineReason::NoTarget };

    site.root = ResultNodeRef { type };
    if (!hasTypedPart({ program, flow, context.build }, site.root))
        return { .declineReason = TypedResultDeclineReason::TargetHasNoTypedPart };
    if (context.exceeds(LimitID::MaxParserPlanNodes, smallestPlanSize, site.call))
        return { };

    switch (site.form) {
    case TypedResultForm::None:
        return { };
    case TypedResultForm::ParsedText:
        site.source = typedResultSourceOf(program, call);
        if (!site.source)
            return { };
        if (isSourceOpenAcross(context, site.block, site.index))
            return { .declineReason = TypedResultDeclineReason::AnotherSourceIsOpen };
        return { .site = site };
    case TypedResultForm::ParsedBody:
        if (isSourceOpenAcross(context, site.block, site.index))
            return { .declineReason = TypedResultDeclineReason::AnotherSourceIsOpen };
        return chooseParsedBody(context, site);
    case TypedResultForm::Rows:
        return chooseRows(context, site);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What a call becomes.

// What the casts between the call and its await say holds for the promise of json(), not for the promise of bytes().
static void removeCasts(Function& function, BlockID block, ValueID lastCast, ValueID promise)
{
    BitVector casts;
    for (ValueID cast = lastCast; cast != promise; cast = function.at(cast).operand(0)) {
        RELEASE_ASSERT(isCast(function.at(cast).opcode));
        casts.set(cast.index());
    }
    function.edit(block, BlockEdit::Values).values.removeAllMatching([&](ValueID id) {
        return casts.get(id.index());
    });
}

static void removeCastsOfAwaitedPromise(Function& function, const TypedResultSite& site)
{
    Value& await = function.edit(site.await);
    std::optional<uint32_t> awaitedIndex = indexOfAwaitedOperand(await);
    RELEASE_ASSERT(awaitedIndex);
    ValueID promiseInBlockOfAwait = site.mergedPromise ? site.mergedPromise : site.call;
    removeCasts(function, site.awaitBlock, await.operand(*awaitedIndex), promiseInBlockOfAwait);
    await.setOperand(*awaitedIndex, promiseInBlockOfAwait);
    if (!site.mergedPromise)
        return;

    size_t parameterIndex = function.at(site.awaitBlock).parameters.find(site.mergedPromise);
    RELEASE_ASSERT(parameterIndex != notFound);
    Target& target = function.edit(site.block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets[0];
    removeCasts(function, site.block, target.arguments[parameterIndex], site.call);
    target.arguments[parameterIndex] = site.call;
}

namespace {

struct TypedResultExpander final : Emission {
    TypedResultExpander(IRBuilder& builder, const EmissionInputs& inputs, FunctionPassContext& pass)
        : Emission(builder, inputs)
        , pass(pass)
    {
    }

    void expand(const TypedResultSite& site)
    {
        originalCall = function().at(site.call);
        switch (site.form) {
        case TypedResultForm::ParsedText:
            return expandParsedText(site);
        case TypedResultForm::ParsedBody:
            return expandParsedBody(site);
        case TypedResultForm::Rows:
            return expandRows(site);
        case TypedResultForm::None:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void endExpansion(const IRBuilder::Expansion& expansion, ValueID result)
    {
        DeclaredType replacedType(Proven::byPosition(function().at(expansion.results[0]).type));
        builder().endExpansion(expansion, convertTo(builder(), result, replacedType));
    }

    ValueID appendOriginalCall()
    {
        Value call = originalCall;
        call.setExpectedResult({ });
        return builder().append(WTF::move(call));
    }

    ValueID emitParseOf(const TypedResultSite& site, ValueID textOrBytes, BlockID generic)
    {
        builder().emitFailureOfPredictions(Target { generic, { }, { } });
        ValueID source = emitProvenBranch(builder().append(Opcode::JSONSourceOpen, { textOrBytes }), generic);
        ValueID start = builder().append(Opcode::JSONSourceStart, { source });
        ValueID first = builder().append(Opcode::JSONSkipWhitespace, { start });
        ValueID results = appendCallOf(inputs().functions.ensure(parserKeyOf(site.root)), { first });
        ValueID outcome = appendResultAt(results, 0);
        ValueID value = appendResultAt(results, 1);
        ValueID next = appendResultAt(results, 2);

        BlockID parsed = builder().addBlock();
        BlockID abandon = builder().addBlock(BlockFrequency::Rare);
        emitBranchOnOutcome(outcome, parsed, abandon, abandon);

        builder().moveToEndOf(abandon);
        builder().append(Opcode::JSONSourceAbandon, { source });
        builder().emitJump(generic);

        builder().moveToEndOf(parsed);
        emitJumpUnless(builder().append(Opcode::JSONSourceClose, { source, next }), generic);
        return value;
    }

    void expandParsedText(const TypedResultSite& site)
    {
        IRBuilder::Expansion expansion = builder().beginExpansion({ site.block, site.index });
        BlockID generic = builder().addBlock(BlockFrequency::Rare);
        endExpansion(expansion, emitParseOf(site, site.source, generic));
        builder().moveToEndOf(generic);
        endExpansion(expansion, appendOriginalCall());
    }

    // Each jump into the block of the await says whether it brings the promise of bytes().
    ValueID addParameterForOriginOfPromise(const TypedResultSite& site)
    {
        ValueID holdsBytes = builder().addParameter(site.awaitBlock, TypeTable::boolean);
        for (Edge edge : pass.analyses.cfg().incomingEdges(site.awaitBlock)) {
            Value constant = IRBuilder::make(Opcode::Boolean);
            constant.setBoolean(edge.from == site.block);
            builder().appendArgument(edge, builder().insertBefore(pointBeforeTerminatorOf(function(), edge.from), WTF::move(constant)));
        }
        builder().applyInsertions();
        return holdsBytes;
    }

    void expandParsedBody(const TypedResultSite& site)
    {
        Function& function = builder().function();
        TypingContext typing { program(), function, inputs().types };
        std::optional<Value> bytesCall = tryMakeBuiltinCall(typing, site.bodyBytesForm, site.source);
        RELEASE_ASSERT(bytesCall);

        bytesCall->setOrigin(originalCall.origin());
        function.edit(site.call) = WTF::move(*bytesCall);
        Value& call = function.edit(site.call);
        call.type = resultTypeOf(program(), function, call, inputs().types);
        call.setExpected(ExpectedType(expectedTypeOf(program(), function, call, inputs().types)));
        removeCastsOfAwaitedPromise(function, site);
        pass.analyses.didChangeValues();
        builder().setOrigin(originalCall.origin());
        ValueID holdsBytes = site.mergedPromise ? addParameterForOriginOfPromise(site) : ValueID();

        size_t awaitIndex = function.at(site.awaitBlock).values.find(site.await);
        RELEASE_ASSERT(awaitIndex != notFound);
        Value await = function.at(site.await);
        await.setExpected({ });

        IRBuilder::Expansion expansion = builder().beginExpansion({ site.awaitBlock, static_cast<uint32_t>(awaitIndex) });
        BlockID generic = builder().addBlock(BlockFrequency::Rare);
        ValueID bytes = builder().append(WTF::move(await));
        if (holdsBytes) {
            // On the other arms the callee has parsed the body itself.
            IRBuilder::BranchBlocks blocks = builder().emitBranch(holdsBytes, BlockFrequency::Normal, BlockFrequency::Unlikely);
            builder().moveToEndOf(blocks.notTaken);
            endExpansion(expansion, bytes);
            builder().moveToEndOf(blocks.taken);
        }
        endExpansion(expansion, emitParseOf(site, bytes, generic));

        builder().moveToEndOf(generic);
        std::optional<Value> parseCall = tryMakeBuiltinCall(typing, Builtins::JSONParse, builder().append(Opcode::JSONDecodeBytes, { bytes }));
        RELEASE_ASSERT(parseCall);
        endExpansion(expansion, builder().append(WTF::move(*parseCall)));
    }

    void expandRows(const TypedResultSite& site)
    {
        Value& call = builder().function().edit(site.call);
        call.setExpectedResult({ });
        builder().setOrigin(call.origin());

        Value expectation = IRBuilder::make(Opcode::ExpectRows, { site.call });
        expectation.setLayout(site.rowLayout);
        builder().insertBefore({ site.block, site.index + 1 }, WTF::move(expectation));
        builder().applyInsertions();
    }

    FunctionPassContext& pass;
    Value originalCall;
};

} // anonymous namespace

// The expansion of a body whose await stands in another block than the call splits that block, and the places of the other sites in it no longer hold.
static bool splitsBlockOf(const TypedResultSite& site, const TypedResultSite& other)
{
    return site.await && site.awaitBlock != site.block && (other.block == site.awaitBlock || other.awaitBlock == site.awaitBlock);
}

PassOutcome selectTypedResults(FunctionPassContext& context)
{
    const Program& program = context.program;
    Vector<TypedResultSite, 2> sites;
    context.forEachSiteIn(SiteList::TypedResultCalls, [&](const ValueSite& position) {
        const Value& call = context.function.at(position.value);
        TypedResultSite site { .block = position.block, .index = position.index, .call = position.value, .form = matchedFormOf(program, call) };
        ExpectedResultID expected = typedResultCallOpcodes.contains(call.opcode) ? call.expectedResult() : ExpectedResultID();
        if (site.form == TypedResultForm::None)
            return;

        TypedResultChoice choice = chooseResult(context, site, expected);
        // The next round finds a site that is left out here.
        bool waitsForNextRound = choice.site && sites.containsIf([&](const TypedResultSite& earlier) {
            return splitsBlockOf(earlier, *choice.site) || splitsBlockOf(*choice.site, earlier);
        });
        if (choice.site && !waitsForNextRound)
            sites.append(*choice.site);
        if (choice.declineReason != TypedResultDeclineReason::None)
            context.addDecline(Wire::DeclineKind::TypedResultNotSelected, position.value, choice.declineReason);
    });
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    EmissionInputs inputs(context);
    IRBuilder builder(inputs.analysisContext(), context.function, context.types, &context.analyses);
    TypedResultExpander expander(builder, inputs, context);
    for (const TypedResultSite& site : sites | std::views::reverse)
        expander.expand(site);

    context.build.statistics().count(StatisticID::TypedResultsSelected, sites.size());
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
