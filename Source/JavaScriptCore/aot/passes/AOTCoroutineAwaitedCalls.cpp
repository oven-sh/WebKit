/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinTable.h"
#include "AOTCoroutines.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// One of the two is set: the body of a function that may suspend, which is entered without a promise, or the worker of one that never does.
struct AwaitableCallee {
    FunctionID entryWithoutPromise;
    FunctionID worker;

    explicit operator bool() const { return entryWithoutPromise || worker; }
};

// Every use of an iterator result that nobody but the reads of its two members gets to see.
struct IteratorResultReads {
    Vector<ValueID, 2> valueReads;
    Vector<ValueID, 2> doneReads;
    Vector<ValueID, 2> testsForPrimitives;
    Vector<ValueID, 2> casts;
};

// The call stands in the block of the await or in a block from which lone jumps lead there. callIndex is the first index in the block of the await that may hold the call or a cast.
struct AwaitedCall {
    ValueID call;
    ValueID await;
    BlockID block;
    uint32_t callIndex { 0 };
    uint32_t awaitIndex { 0 };
    AwaitableCallee callee;
    Vector<ValueID, 1> casts;
    Vector<BlockID, 1> blocksInFront;
    // Nothing where somebody gets to see the iterator result: then it is made.
    std::optional<IteratorResultReads> resultReads;
};

struct SuccessorParts {
    FunctionID entry;
    ValueID promise;
    Vector<ValueID, 8> arguments;
    Origin callOrigin;
    AccessSiteID accessSite;
    TypeID expected;
    bool isDemotedTailCall;
};

struct AwaitOfMergedValues {
    ValueID await;
    BlockID block;
    uint32_t awaitIndex { 0 };
    unsigned parameterIndex { 0 };
    Vector<BlockID, 4> predecessors;
};

// The tests at the head of a typed async function are a function of their own, which calls a ramp in each of its two arms. Only who asks what will stand here once the tests are taken in looks behind them.
enum class TestsAtHeadOfCallee : uint8_t { AreNoCallee, StandForTypedBody };

struct DirectCallElement {
    ValueID promiseAll;
    uint32_t elementIndex { 0 };
    ValueID call;
    ProgramPoint callPoint;
    FunctionID entry;
    Vector<ValueID, 1> casts;
};

} // anonymous namespace

// directCalls has a bit for each of the first elements.
static constexpr uint32_t maxDirectCallElements = 32;
static constexpr size_t maxJumpsBetweenCallAndAwait = 4;

static AwaitableCallee findAwaitableCallee(const Program& program, const Value& call, TestsAtHeadOfCallee tests = TestsAtHeadOfCallee::AreNoCallee)
{
    if (call.opcode != Opcode::CallDirect)
        return { };
    const Function* callee = &program.at(call.function());
    if (const auto* entryChecks = callee->creator.tryAs<CreatorRecord::EntryChecks>(); entryChecks && tests == TestsAtHeadOfCallee::StandForTypedBody)
        callee = &program.at(entryChecks->body);
    const CreatorRecord::Coroutine* record = coroutineOf(*callee);
    if (!record || callee->role != FunctionRole::CoroutineRamp)
        return { };
    return { record->entryWithoutPromise, record->worker };
}

static bool isNextOfAsyncGenerator(FunctionPassContext& context, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || call.needsPristineReceiver())
        return false;
    if (context.program.builtins().at(call.overload()).builtin != Builtins::AsyncGeneratorPrototypeNext || call.argumentsOperands().size() != 1)
        return false;
    ValueID receiver = call.receiverOperand();
    if (!receiver)
        return false;
    const Type& generators = context.types.at(context.types.intern(Type::builtinClass(BuiltinClasses::AsyncGenerator)));
    return context.program.typeOf(context.function, receiver).isSubsetOf(generators, context.program);
}

// The engine makes the result afresh for each step, with `value` and `done` as its own data properties. While only those are read, nobody can tell whether it was made.
static bool collectIteratorResultReads(FunctionPassContext& context, ValueID result, IteratorResultReads& reads)
{
    const Function& function = context.function;
    const Program& program = context.program;
    for (const Use& use : context.analyses.useLists().usesOf(result)) {
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = function.at(use.user);
        if (isCastWithoutTest(user.opcode)) {
            reads.casts.append(use.user);
            if (!collectIteratorResultReads(context, use.user, reads))
                return false;
            continue;
        }
        if (user.opcode == Opcode::TypeTest && !context.types.at(user.testedType()).hasObjects()) {
            reads.testsForPrimitives.append(use.user);
            continue;
        }
        if (user.opcode != Opcode::PropGet || user.type != TypeTable::anything)
            return false;
        if (user.key() == program.keyOf(BuiltinNames::value))
            reads.valueReads.append(use.user);
        else if (user.key() == program.keyOf(BuiltinNames::done))
            reads.doneReads.append(use.user);
        else
            return false;
    }
    return true;
}

static bool hasOnlyPureValues(FunctionPassContext& context, std::span<const ValueID> values)
{
    return std::ranges::all_of(values, [&](ValueID value) {
        return context.analyses.hasNoEffects(value);
    });
}

// Nothing if a cast on the way has another use.
static ValueID definitionBehindOwnCasts(FunctionPassContext& context, ValueID value, Vector<ValueID, 1>& casts)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    for (; isCastWithoutTest(function.at(value).opcode); value = function.at(value).operand(0)) {
        if (!useLists.singleUse(value))
            return { };
        casts.append(value);
    }
    return value;
}

static void eraseValuesIn(IRBuilder& builder, BlockID block, std::span<const ValueID> erasedValues)
{
    if (erasedValues.empty())
        return;
    const BlockValues& values = builder.function().at(block).values;
    BitVector erasedIndexes(values.size());
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (std::ranges::find(erasedValues, values[index]) != erasedValues.end())
            erasedIndexes.quickSet(index);
    }
    builder.eraseValues(block, erasedIndexes);
}

static bool isAwaitableCallAtEndOf(FunctionPassContext& context, BlockID block, ValueID incoming)
{
    const Function& function = context.function;
    Vector<ValueID, 1> casts;
    ValueID call = definitionBehindOwnCasts(context, incoming, casts);
    if (!call || (!findAwaitableCallee(context.program, function.at(call)) && !isNextOfAsyncGenerator(context, function.at(call))))
        return false;

    const UseLists& useLists = context.analyses.useLists();
    ValueLocation location = useLists.locationOf(call);
    if (!useLists.singleUse(call) || location.placement != ValuePlacement::InBlock || location.block != block)
        return false;
    return hasOnlyPureValues(context, function.at(block).values.span().subspan(location.index + 1));
}

// An expected type is set only where it says more than the type.
static void setExpectedIfNarrower(FunctionPassContext& context, Value& value, TypeID expected)
{
    if (!expected)
        return;
    TypeID type = resultTypeOf(context.program, context.function, value, context.types);
    if (expected != type && context.types.at(expected).isSubsetOf(context.types.at(type), context.program))
        value.setExpected(expected);
}

static bool hasTypeOfAwait(FunctionPassContext& context, const Value& await, ValueID awaited)
{
    Value moved = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, await.promiseOperand() });
    const Type& type = context.types.at(resultTypeOf(context.program, context.function, moved, context.types));
    return type.isSubsetOf(context.types.at(await.type), context.program);
}

// The function that tests the arguments of a typed async function is inlined into the caller: the typed and the untyped version are called in two arms, and the await stands behind their merge.
static std::optional<AwaitOfMergedValues> findAwaitOfMergedValues(FunctionPassContext& context, ValueID await)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    if (function.at(await).takesExtraTurn())
        return std::nullopt;

    Vector<ValueID, 1> casts;
    ValueID parameter = definitionBehindOwnCasts(context, function.at(await).awaitedOperand(), casts);
    if (!parameter || !useLists.singleUse(parameter))
        return std::nullopt;

    ValueLocation awaitLocation = useLists.locationOf(await);
    ValueLocation parameterLocation = useLists.locationOf(parameter);
    RELEASE_ASSERT(awaitLocation.placement == ValuePlacement::InBlock);
    if (parameterLocation.placement != ValuePlacement::Parameter || parameterLocation.block != awaitLocation.block)
        return std::nullopt;

    BlockID block = awaitLocation.block;
    const Block& merge = function.at(block);
    if (merge.isHandlerBlock() || !hasOnlyPureValues(context, merge.values.span().first(awaitLocation.index)))
        return std::nullopt;
    for (Edge edge : context.analyses.edgesFromUnreachableBlocks()) {
        if (targetOf(function, edge).block == block)
            return std::nullopt;
    }

    AwaitOfMergedValues found { await, block, awaitLocation.index, parameterLocation.index, { } };
    bool awaitsCall = false;
    for (Edge edge : context.analyses.cfg().incomingEdges(block)) {
        const Block& predecessor = function.at(edge.from);
        if (edge.isHandlerEdge() || predecessor.terminator.kind != TerminatorKind::Jump || predecessor.handler != merge.handler)
            return std::nullopt;
        const Arguments& arguments = predecessor.terminator.next().arguments;
        if (arguments.size() != merge.parameters.size())
            return std::nullopt;
        ValueID incoming = arguments[found.parameterIndex];
        if (!hasTypeOfAwait(context, function.at(await), incoming))
            return std::nullopt;
        awaitsCall |= isAwaitableCallAtEndOf(context, edge.from, incoming);
        found.predecessors.append(edge.from);
    }
    if (!awaitsCall)
        return std::nullopt;
    return found;
}

static void sinkAwaitIntoPredecessors(FunctionPassContext& context, IRBuilder& builder, const AwaitOfMergedValues& site)
{
    Function& function = context.function;
    ValueID driver = function.at(site.await).promiseOperand();
    TypeID type = function.at(site.await).type;
    TypeID expected = function.at(site.await).expected();
    ValueID parameter = function.at(site.block).parameters[site.parameterIndex];

    Vector<ValueID, 1> casts;
    for (ValueID cast = function.at(site.await).awaitedOperand(); cast != parameter; cast = function.at(cast).operand(0))
        casts.append(cast);

    builder.setOrigin(function.at(site.await).origin());
    for (BlockID predecessor : site.predecessors) {
        ValueID incoming = function.at(predecessor).terminator.next().arguments[site.parameterIndex];
        Value moved = IRBuilder::make(Opcode::AwaitBySavingFrame, { incoming, driver });
        setExpectedIfNarrower(context, moved, expected);
        ValueID result = builder.insertBefore(pointBeforeTerminatorOf(function, predecessor), WTF::move(moved));
        function.edit(predecessor, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.next().arguments[site.parameterIndex] = result;
    }
    builder.applyInsertions();

    function.edit(parameter, ValueEdit::Type).type = type;
    function.edit(parameter, ValueEdit::Type).setExpected(expected);
    becomeUpcast(function, site.await, parameter);
    eraseValuesIn(builder, site.block, casts.span());
}

static unsigned sinkAwaitsOfMergedValues(FunctionPassContext& context, IRBuilder& builder)
{
    const Function& function = context.function;
    Vector<AwaitOfMergedValues> sites;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (function.at(value).opcode != Opcode::AwaitBySavingFrame)
                continue;
            if (std::optional<AwaitOfMergedValues> site = findAwaitOfMergedValues(context, value))
                sites.append(WTF::move(*site));
        }
    }
    for (const AwaitOfMergedValues& site : sites)
        sinkAwaitIntoPredecessors(context, builder, site);
    if (!sites.isEmpty())
        context.analyses.didChangeValues();
    return sites.size();
}

namespace {

// `next` of an async generator is called in one arm of several, and the arms meet, perhaps more than once, in front of the await.
struct AwaitBehindJoins {
    ValueID await;
    BlockID block;
    uint32_t awaitIndex { 0 };
    Vector<BlockID, 2> blocksThatCall;
};

} // anonymous namespace

static constexpr unsigned maxJoinsBetweenCallAndAwait = 3;

// The block has one parameter, and the values in front of `end` are casts of it, each with one use. Answers the last of them. Nothing of the block is missed by whoever goes round it with the value.
static ValueID valueHandedThrough(FunctionPassContext& context, BlockID block, uint32_t end)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    const Block& contents = function.at(block);
    if (contents.isHandlerBlock() || contents.parameters.size() != 1 || context.analyses.isEnteredFromUnreachableBlock(block))
        return { };
    ValueID handed = contents.parameters[0];
    for (uint32_t index = 0; index < end; ++index) {
        const Value& cast = function.at(contents.values[index]);
        if (!isCastWithoutTest(cast.opcode) || cast.operand(0) != handed || !useLists.singleUse(handed))
            return { };
        handed = contents.values[index];
    }
    return useLists.singleUse(handed) ? handed : ValueID();
}

static bool isNextOfAsyncGeneratorAtEndOf(FunctionPassContext& context, BlockID block, ValueID incoming)
{
    Vector<ValueID, 1> casts;
    ValueID call = definitionBehindOwnCasts(context, incoming, casts);
    return call && isNextOfAsyncGenerator(context, context.function.at(call)) && isAwaitableCallAtEndOf(context, block, incoming);
}

static void collectBlocksThatCallNext(FunctionPassContext& context, const Value& await, BlockID join, const std::optional<Target>& handlerOfAwait, unsigned joinsLeft, Vector<BlockID, 2>& found)
{
    const Function& function = context.function;
    for (Edge edge : context.analyses.cfg().incomingEdges(join)) {
        const Block& predecessor = function.at(edge.from);
        if (edge.isHandlerEdge() || predecessor.terminator.kind != TerminatorKind::Jump || predecessor.terminator.next().arguments.size() != 1)
            continue;
        ValueID incoming = predecessor.terminator.next().arguments[0];
        if (isNextOfAsyncGeneratorAtEndOf(context, edge.from, incoming)) {
            if (predecessor.handler == handlerOfAwait && hasTypeOfAwait(context, await, incoming))
                found.append(edge.from);
            continue;
        }
        if (joinsLeft && valueHandedThrough(context, edge.from, predecessor.values.size()) == incoming)
            collectBlocksThatCallNext(context, await, edge.from, handlerOfAwait, joinsLeft - 1, found);
    }
}

static std::optional<AwaitBehindJoins> findAwaitBehindJoins(FunctionPassContext& context, ValueID await)
{
    const Function& function = context.function;
    if (function.at(await).takesExtraTurn())
        return std::nullopt;
    ValueLocation location = context.analyses.useLists().locationOf(await);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
    if (valueHandedThrough(context, location.block, location.index) != function.at(await).awaitedOperand())
        return std::nullopt;
    AwaitBehindJoins found { await, location.block, location.index, { } };
    collectBlocksThatCallNext(context, function.at(await), location.block, function.at(location.block).handler, maxJoinsBetweenCallAndAwait - 1, found.blocksThatCall);
    if (found.blocksThatCall.isEmpty())
        return std::nullopt;
    return found;
}

// The arm that calls gets an await of its own and goes on behind the await of the others.
static void giveCallersTheirOwnAwait(FunctionPassContext& context, IRBuilder& builder, const AwaitBehindJoins& site)
{
    Function& function = context.function;
    ValueID awaited = function.at(site.await).awaitedOperand();
    ValueID driver = function.at(site.await).promiseOperand();
    TypeID expected = function.at(site.await).expected();
    Origin origin = function.at(site.await).origin();

    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { site.block, site.awaitIndex });
    Value again = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, driver });
    setExpectedIfNarrower(context, again, expected);
    builder.endExpansion(expansion, builder.append(WTF::move(again)));

    builder.setOrigin(origin);
    Vector<ValueID, 2> ownAwaits;
    for (BlockID block : site.blocksThatCall) {
        Value own = IRBuilder::make(Opcode::AwaitBySavingFrame, { function.at(block).terminator.next().arguments[0], driver });
        setExpectedIfNarrower(context, own, expected);
        ownAwaits.append(builder.insertBefore(pointBeforeTerminatorOf(function, block), WTF::move(own)));
    }
    builder.applyInsertions();
    for (size_t index = 0; index < ownAwaits.size(); ++index)
        builder.redirectEdge(Edge { site.blocksThatCall[index], 0 }, expansion.continuation, { ownAwaits[index] });
}

static unsigned giveAwaitsToCallersOfNext(FunctionPassContext& context, IRBuilder& builder)
{
    const Function& function = context.function;
    unsigned count = 0;
    // Each one changes the blocks, so the next one is looked for anew.
    for (bool didChange = true; didChange;) {
        didChange = false;
        for (BlockID block : context.analyses.cfg().reversePostOrder()) {
            for (ValueID value : function.at(block).values) {
                if (function.at(value).opcode != Opcode::AwaitBySavingFrame)
                    continue;
                std::optional<AwaitBehindJoins> site = findAwaitBehindJoins(context, value);
                if (!site)
                    continue;
                giveCallersTheirOwnAwait(context, builder, *site);
                context.analyses.didChangeValues();
                ++count;
                didChange = true;
                break;
            }
            if (didChange)
                break;
        }
    }
    return count;
}

static std::optional<BlockID> findBlockThatAloneJumpsTo(FunctionPassContext& context, BlockID block)
{
    const Function& function = context.function;
    const Block& contents = function.at(block);
    std::optional<Edge> edge = context.analyses.cfg().singleIncomingEdge(block);
    if (!edge || edge->isHandlerEdge() || contents.isHandlerBlock() || !contents.parameters.isEmpty() || context.analyses.isEnteredFromUnreachableBlock(block))
        return std::nullopt;
    const Block& predecessor = function.at(edge->from);
    if (predecessor.terminator.kind != TerminatorKind::Jump || predecessor.handler != contents.handler)
        return std::nullopt;
    return edge->from;
}

static std::optional<AwaitedCall> findAwaitedCall(FunctionPassContext& context, ValueID await, TestsAtHeadOfCallee tests = TestsAtHeadOfCallee::AreNoCallee)
{
    const Function& function = context.function;
    if (function.at(await).takesExtraTurn())
        return std::nullopt;

    Vector<ValueID, 1> casts;
    ValueID awaited = definitionBehindOwnCasts(context, function.at(await).awaitedOperand(), casts);
    if (!awaited)
        return std::nullopt;
    AwaitableCallee callee = findAwaitableCallee(context.program, function.at(awaited), tests);
    if (!callee && !isNextOfAsyncGenerator(context, function.at(awaited)))
        return std::nullopt;
    std::optional<IteratorResultReads> resultReads;
    if (!callee && !collectIteratorResultReads(context, await, resultReads.emplace()))
        resultReads = std::nullopt;

    const UseLists& useLists = context.analyses.useLists();
    if (!useLists.singleUse(awaited))
        return std::nullopt;

    ValueLocation callLocation = useLists.locationOf(awaited);
    ValueLocation awaitLocation = useLists.locationOf(await);
    RELEASE_ASSERT(awaitLocation.placement == ValuePlacement::InBlock);
    if (callLocation.placement != ValuePlacement::InBlock)
        return std::nullopt;

    // A pass that cuts a block behind a call leaves a jump there. Nothing else enters the block behind it, so what stood in one block still runs in one go.
    Vector<BlockID, 1> blocksInFront;
    BlockID block = awaitLocation.block;
    uint32_t end = awaitLocation.index;
    while (block != callLocation.block) {
        std::optional<BlockID> predecessor = findBlockThatAloneJumpsTo(context, block);
        if (!predecessor || blocksInFront.size() == maxJumpsBetweenCallAndAwait || !hasOnlyPureValues(context, function.at(block).values.span().first(end)))
            return std::nullopt;
        block = *predecessor;
        end = function.at(block).values.size();
        blocksInFront.append(block);
    }
    if (callLocation.index >= end || !hasOnlyPureValues(context, function.at(block).values.subspan(callLocation.index + 1, end - callLocation.index - 1)))
        return std::nullopt;
    uint32_t firstIndex = blocksInFront.isEmpty() ? callLocation.index : 0;
    return AwaitedCall { awaited, await, awaitLocation.block, firstIndex, awaitLocation.index, callee, WTF::move(casts), WTF::move(blocksInFront), WTF::move(resultReads) };
}

static Vector<AwaitedCall> findAwaitedCalls(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<AwaitedCall> sites;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (function.at(value).opcode != Opcode::AwaitBySavingFrame)
                continue;
            if (std::optional<AwaitedCall> site = findAwaitedCall(context, value))
                sites.append(WTF::move(*site));
        }
    }
    return sites;
}

static SuccessorParts copySuccessorParts(const Function& function, const AwaitedCall& site)
{
    const Value& call = function.at(site.call);
    const Value& await = function.at(site.await);
    return {
        .entry = site.callee.entryWithoutPromise,
        .promise = await.promiseOperand(),
        .arguments = Vector<ValueID, 8>(call.argumentsOperands()),
        .callOrigin = call.origin(),
        .accessSite = call.accessSite(),
        .expected = await.expected(),
        .isDemotedTailCall = call.isDemotedTailCall(),
    };
}

static void becomeAwaitOfCall(Value& value, const SuccessorParts& parts)
{
    value.become(Opcode::AwaitCallBySavingFrame);
    value.setFunction(parts.entry);
    value.setPromiseOperand(parts.promise);
    value.setArgumentsOperands(parts.arguments.span());
    value.setOrigin(parts.callOrigin);
    value.setAccessSite(parts.accessSite);
    value.setExpected(parts.expected);
    value.setIsDemotedTailCall(parts.isDemotedTailCall);
}

// Behind the call the blocks in front hold pure values only, so no other site has a position there that this moves.
static void markCallAndCasts(IRBuilder& builder, const AwaitedCall& site, BitVector& erasedIndexes)
{
    const BlockValues& values = builder.function().at(site.block).values;
    for (uint32_t index = site.callIndex; index < site.awaitIndex; ++index) {
        if (values[index] == site.call || site.casts.contains(values[index]))
            erasedIndexes.set(index);
    }
    if (site.blocksInFront.isEmpty())
        return;
    Vector<ValueID, 2> callAndCasts(site.casts.span());
    callAndCasts.append(site.call);
    for (BlockID block : site.blocksInFront)
        eraseValuesIn(builder, block, callAndCasts.span());
}

static void eraseMarkedValues(IRBuilder& builder, BlockID block, BitVector& erasedIndexes)
{
    if (erasedIndexes.isEmpty())
        return;
    builder.eraseValues(block, erasedIndexes);
    erasedIndexes.clearAll();
}

static void expandTypedAwait(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site, Value&& successor, const ReplacedEffects& replaced)
{
    Function& function = context.function;
    TypeID type = function.at(site.await).type;
    Origin callOrigin = successor.origin();

    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { site.block, site.awaitIndex });
    Origin awaitOrigin = builder.origin();
    builder.setOrigin(callOrigin);
    ValueID result = builder.append(WTF::move(successor));
    checkReplacement(context.analyses.effectContext(), function, { context.id, site.block, result }, function.at(result), replaced, context.build.diagnostics());

    builder.setOrigin(awaitOrigin);
    builder.endExpansion(expansion, convertTo(builder, result, DeclaredType { .type = type }));
}

// The promise of a function that never suspends is settled when the function returns, so the caller awaits what it would have been settled with.
static void expandAwaitOfWorker(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site)
{
    Function& function = context.function;
    TypeArena& types = context.types;
    ValueID driver = function.at(site.await).promiseOperand();
    TypeID type = function.at(site.await).type;
    TypeID expected = function.at(site.await).expected();
    Value call = function.at(site.call);
    call.setFunction(site.callee.worker);
    call.setExpected({ });
    Origin callOrigin = call.origin();

    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { site.block, site.awaitIndex });
    Origin awaitOrigin = builder.origin();
    BlockFrequency frequency = function.at(site.block).frequency;

    BlockID rejection = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    BlockID awaiting = builder.addBlock(frequency);
    builder.setHandler(Target { rejection, { }, { } });
    BlockID calling = builder.addBlock(frequency);
    builder.setHandler(expansion.handler);

    builder.setOrigin(callOrigin);
    builder.emitJump(calling);
    builder.moveToEndOf(calling);
    ValueID result = builder.append(WTF::move(call));
    TypeID resultType = function.at(result).type;
    ValueID awaited = builder.addParameter(awaiting, types.unionOf(resultType, types.intern(Type::builtinClass(BuiltinClasses::Promise))));

    if (resultType == TypeTable::never)
        builder.emitUnreachable();
    else if (!protocolMayFindMember(context.program, types.at(resultType), Protocol::Thenable))
        builder.emitJump(awaiting, { result });
    else {
        BlockID resolving = builder.addBlock(rarerOf(frequency, BlockFrequency::Unlikely));
        builder.emitBranch(builder.append(Opcode::IsNotThenable, { result }), Target { awaiting, { result }, { } }, Target { resolving, { }, { } });

        builder.moveToEndOf(resolving);
        ValueID promise = builder.append(Opcode::NewPromise, { });
        builder.append(Opcode::ResolvePromise, { promise, result });
        builder.emitJump(awaiting, { promise });
    }

    builder.moveToEndOf(rejection);
    ValueID error = builder.append(Opcode::CatchValue, { });
    ValueID rejected = builder.append(Opcode::NewPromise, { });
    builder.append(Opcode::RejectPromise, { rejected, error });
    builder.emitJump(awaiting, { rejected });

    builder.setOrigin(awaitOrigin);
    builder.moveToEndOf(awaiting);
    Value await = IRBuilder::make(Opcode::AwaitBySavingFrame, { awaited, driver });
    setExpectedIfNarrower(context, await, expected);
    builder.endExpansion(expansion, convertTo(builder, builder.append(WTF::move(await)), DeclaredType { .type = type }));
}

static ValueID appendResultAt(IRBuilder& builder, ValueID producer, StepGeneratorResult result)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { producer });
    read.setIndex(static_cast<uint32_t>(result));
    return builder.append(WTF::move(read));
}

static ValueID appendStatusTest(IRBuilder& builder, ValueID status, AsyncGeneratorStepStatus expected)
{
    return builder.append(Opcode::StrictEq, { status, builder.appendInteger(static_cast<int64_t>(expected)) });
}

static ValueID appendPropGet(IRBuilder& builder, ValueID object, PropertyKeyID key)
{
    Value read = IRBuilder::make(Opcode::PropGet, { object });
    read.setKey(key);
    return builder.append(WTF::move(read));
}

static void appendPropDefine(IRBuilder& builder, ValueID object, PropertyKeyID key, ValueID value)
{
    Value definition = IRBuilder::make(Opcode::PropDefine, { object, value });
    definition.setKey(key);
    builder.append(WTF::move(definition));
}

static void eraseValuesEverywhere(IRBuilder& builder, std::span<const ValueID> erasedValues)
{
    if (erasedValues.empty())
        return;
    for (uint32_t index = 0; index < builder.function().blocks.size(); ++index)
        eraseValuesIn(builder, BlockID(index), erasedValues);
}

// `for await` is Await(Call(next)) as well, so the step that serves it takes as many turns of the job queue as the call and the await.
static void expandAwaitOfGeneratorStep(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site)
{
    Function& function = context.function;
    const Program& program = context.program;
    ValueID generator = function.at(site.call).receiverOperand();
    ValueID sent = function.at(site.call).argumentsOperands()[0];
    Origin callOrigin = function.at(site.call).origin();
    BlockFrequency frequency = function.at(site.block).frequency;

    TypeID type = function.at(site.await).type;

    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { site.block, site.awaitIndex });
    builder.setOrigin(callOrigin);
    ValueID driver = builder.append(Opcode::Absent, { });
    if (!site.resultReads) {
        ValueID step = builder.append(Opcode::StepAsyncGenerator, { generator, sent, driver });
        ValueID stepped = appendResultAt(builder, step, StepGeneratorResult::Value);
        ValueID status = appendResultAt(builder, step, StepGeneratorResult::Status);
        IRBuilder::BranchBlocks blocks = builder.emitBranch(appendStatusTest(builder, status, AsyncGeneratorStepStatus::Result), BlockFrequency::Rare, frequency);
        builder.endExpansion(expansion, convertTo(builder, stepped, DeclaredType { .type = type }));

        // What CreateIterResultObject makes: an ordinary object with the two as its own data properties, in this order.
        builder.moveToEndOf(blocks.notTaken);
        ValueID object = builder.append(Opcode::NewDictionary, { });
        appendPropDefine(builder, object, program.keyOf(BuiltinNames::value), stepped);
        appendPropDefine(builder, object, program.keyOf(BuiltinNames::done), appendStatusTest(builder, status, AsyncGeneratorStepStatus::Completed));
        builder.endExpansion(expansion, convertTo(builder, object, DeclaredType { .type = type }));
        return;
    }

    ValueID value = builder.addParameter(expansion.continuation, TypeTable::anything);
    ValueID done = builder.addParameter(expansion.continuation, TypeTable::anything);
    auto continueWith = [&](ValueID stepValue, ValueID stepDone) {
        Value unread = IRBuilder::make(Opcode::Unspecified);
        unread.setValueType(type);
        builder.emitJump(expansion.continuation, { builder.append(WTF::move(unread)), stepValue, stepDone });
    };

    ValueID step = builder.append(Opcode::StepAsyncGenerator, { generator, sent, driver });
    ValueID stepped = appendResultAt(builder, step, StepGeneratorResult::Value);
    ValueID status = appendResultAt(builder, step, StepGeneratorResult::Status);

    IRBuilder::BranchBlocks blocks = builder.emitBranch(appendStatusTest(builder, status, AsyncGeneratorStepStatus::Result), BlockFrequency::Rare, frequency);
    Value check = IRBuilder::make(Opcode::RequireObject, { stepped });
    check.setRuntimeError(RuntimeErrorKind::IteratorResultNotObject);
    ValueID object = builder.append(WTF::move(check));
    ValueID valueOfObject = appendPropGet(builder, object, program.keyOf(BuiltinNames::value));
    continueWith(valueOfObject, appendPropGet(builder, object, program.keyOf(BuiltinNames::done)));

    builder.moveToEndOf(blocks.notTaken);
    continueWith(stepped, appendStatusTest(builder, status, AsyncGeneratorStepStatus::Completed));

    for (ValueID read : site.resultReads->valueReads)
        becomeUpcast(function, read, value);
    for (ValueID read : site.resultReads->doneReads)
        becomeUpcast(function, read, done);
    // The result is an object, and so is the generator: a test for primitives answers the same for both.
    for (ValueID test : site.resultReads->testsForPrimitives)
        function.edit(test, ValueEdit::Definition).setValueOperand(generator);
    eraseValuesEverywhere(builder, site.resultReads->casts.span());
}

// Sites of one block are rewritten last position first: what is marked lies after this site, so this site's positions still hold.
static void rewrite(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site, BitVector& erasedIndexes)
{
    Function& function = context.function;
    if (!site.callee) {
        eraseMarkedValues(builder, site.block, erasedIndexes);
        expandAwaitOfGeneratorStep(context, builder, site);
        markCallAndCasts(builder, site, erasedIndexes);
        return;
    }
    if (site.callee.worker) {
        eraseMarkedValues(builder, site.block, erasedIndexes);
        expandAwaitOfWorker(context, builder, site);
        markCallAndCasts(builder, site, erasedIndexes);
        return;
    }

    const EffectContext& effects = context.analyses.effectContext();

    ReplacedEffects replaced;
    replaced.add(effects, function, function.at(site.call));
    replaced.add(effects, function, function.at(site.await));

    SuccessorParts parts = copySuccessorParts(function, site);
    Value successor;
    becomeAwaitOfCall(successor, parts);
    const Type& successorType = context.types.at(resultTypeOf(context.program, function, successor, context.types));
    if (successorType.isSubsetOf(context.types.at(function.at(site.await).type), context.program)) {
        becomeAwaitOfCall(function.edit(site.await), parts);
        checkReplacement(effects, function, { context.id, site.block, site.await }, function.at(site.await), replaced, context.build.diagnostics());
    } else {
        eraseMarkedValues(builder, site.block, erasedIndexes);
        expandTypedAwait(context, builder, site, WTF::move(successor), replaced);
    }
    markCallAndCasts(builder, site, erasedIndexes);
}

static void rewriteAll(FunctionPassContext& context, IRBuilder& builder, std::span<const AwaitedCall> sites)
{
    BitVector erasedIndexes;
    for (size_t index = sites.size(); index--;) {
        const AwaitedCall& site = sites[index];
        rewrite(context, builder, site, erasedIndexes);
        if (!index || sites[index - 1].block != site.block)
            eraseMarkedValues(builder, site.block, erasedIndexes);
    }
    context.analyses.didChangeValues();
    context.build.statistics().count(StatisticID::AwaitedCallsResolved, sites.size());
}

static std::optional<DirectCallElement> findDirectCallElement(FunctionPassContext& context, ValueID promiseAll, uint32_t elementIndex)
{
    const Function& function = context.function;
    DirectCallElement element { .promiseAll = promiseAll, .elementIndex = elementIndex };
    element.call = definitionBehindOwnCasts(context, function.at(promiseAll).elementsOperands()[elementIndex], element.casts);
    if (!element.call)
        return std::nullopt;
    element.entry = findAwaitableCallee(context.program, function.at(element.call)).entryWithoutPromise;
    if (!element.entry || !context.analyses.useLists().singleUse(element.call))
        return std::nullopt;
    return element;
}

// What a call without a promise returns is for the operation alone. Until the operation has it, nothing runs but the calls for the other elements.
static void findDirectCallElementsOf(FunctionPassContext& context, ProgramPoint promiseAllPoint, Vector<DirectCallElement>& found)
{
    const Function& function = context.function;
    const BlockValues& values = function.at(promiseAllPoint.block).values;
    ValueID promiseAll = values[promiseAllPoint.index];

    Vector<DirectCallElement, 4> candidates;
    uint32_t numberOfElements = std::min<uint32_t>(function.at(promiseAll).elementsOperands().size(), maxDirectCallElements);
    for (uint32_t elementIndex = 0; elementIndex < numberOfElements; ++elementIndex) {
        if (std::optional<DirectCallElement> element = findDirectCallElement(context, promiseAll, elementIndex))
            candidates.append(WTF::move(*element));
    }

    for (uint32_t index = promiseAllPoint.index; index-- && !candidates.isEmpty();) {
        size_t position = candidates.findIf([&](const DirectCallElement& candidate) {
            return candidate.call == values[index];
        });
        if (position != notFound) {
            candidates[position].callPoint = ProgramPoint { promiseAllPoint.block, index };
            found.append(WTF::move(candidates[position]));
            candidates.removeAt(position);
            continue;
        }
        if (!context.analyses.hasNoEffects(values[index]))
            return;
    }
}

static void retargetElement(FunctionPassContext& context, IRBuilder& builder, const DirectCallElement& element)
{
    Function& function = context.function;
    builder.setOrigin(function.at(element.call).origin());
    ValueID driver = builder.insertBefore(element.callPoint, IRBuilder::make(Opcode::Undefined));

    Value& call = function.edit(element.call);
    call.setFunction(element.entry);
    call.appendVariadicOperand(driver);
    call.type = resultTypeOf(context.program, function, call, context.types);
    call.setExpected({ });

    Value& promiseAll = function.edit(element.promiseAll);
    promiseAll.setOperand(OperandIndex::PromiseAllOfElements::elements + element.elementIndex, element.call);
    promiseAll.setDirectCalls(promiseAll.directCalls() | 1u << element.elementIndex);
}

static unsigned retargetElementsOfPromiseAll(FunctionPassContext& context, IRBuilder& builder)
{
    const Function& function = context.function;
    Vector<DirectCallElement> elements;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (function.at(values[index]).opcode == Opcode::PromiseAllOfElements)
                findDirectCallElementsOf(context, ProgramPoint { block, index }, elements);
        }
    }
    if (elements.isEmpty())
        return 0;

    for (const DirectCallElement& element : elements)
        retargetElement(context, builder, element);
    builder.applyInsertions();
    for (const DirectCallElement& element : elements)
        eraseValuesIn(builder, element.callPoint.block, element.casts.span());
    context.analyses.didChangeValues();
    return elements.size();
}

static bool hasPromiseAllOfElements(const Function& function)
{
    return function.values.containsIf([](const Value& value) {
        return value.opcode == Opcode::PromiseAllOfElements;
    });
}

static bool isBodyOfAsyncFunction(const Function& function)
{
    return function.role == FunctionRole::CoroutineBody && coroutineOfBody(function).kind == CoroutineKind::AsyncFunction;
}

bool willEnterAwaitedCalleeWithoutPromise(FunctionPassContext& context, ValueID call)
{
    const Function& function = context.function;
    if (!isBodyOfAsyncFunction(function) || !findAwaitableCallee(context.program, function.at(call), TestsAtHeadOfCallee::StandForTypedBody))
        return false;

    const UseLists& useLists = context.analyses.useLists();
    std::optional<Use> mergingUse;
    for (ValueID promise = call;;) {
        std::optional<Use> use = useLists.singleUse(promise);
        if (!use)
            return false;
        if (use->kind == Use::Kind::EdgeArgument && !mergingUse) {
            const CompactList<ValueID>& parameters = function.at(targetOf(function, use->edge()).block).parameters;
            if (use->operandIndex >= parameters.size() || !isAwaitableCallAtEndOf(context, use->block, promise))
                return false;
            mergingUse = use;
            promise = parameters[use->operandIndex];
            continue;
        }
        if (use->kind != Use::Kind::Operand)
            return false;
        Opcode user = function.at(use->user).opcode;
        if (isCastWithoutTest(user)) {
            promise = use->user;
            continue;
        }
        if (user != Opcode::AwaitBySavingFrame || use->operandIndex != OperandIndex::AwaitBySavingFrame::awaited)
            return false;
        if (mergingUse)
            return findAwaitOfMergedValues(context, use->user).has_value();
        std::optional<AwaitedCall> site = findAwaitedCall(context, use->user, TestsAtHeadOfCallee::StandForTypedBody);
        return site && site->call == call;
    }
}

static PassOutcome resolveAwaitedAndCollectedCalls(FunctionPassContext& context, bool awaitsCalls, bool collectsCalls)
{
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    PassOutcome outcome = PassOutcome::Unchanged;
    if (collectsCalls && retargetElementsOfPromiseAll(context, builder))
        outcome = PassOutcome::Changed;
    if (!awaitsCalls)
        return outcome;

    if (unsigned numberOfSunkAwaits = sinkAwaitsOfMergedValues(context, builder)) {
        context.build.statistics().count(StatisticID::AwaitsSunkIntoPredecessors, numberOfSunkAwaits);
        outcome = PassOutcome::Changed;
    }

    Vector<AwaitedCall> sites = findAwaitedCalls(context);
    if (sites.isEmpty())
        return outcome;
    rewriteAll(context, builder, sites.span());
    return PassOutcome::Changed;
}

unsigned stepAwaitedAsyncGenerators(FunctionPassContext& context)
{
    const Function& function = context.function;
    if (function.role != FunctionRole::CoroutineBody || coroutineOfBody(function).kind == CoroutineKind::Generator)
        return 0;
    if (!context.analyses.opcodesOfValues().contains(Opcode::AwaitBySavingFrame))
        return 0;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    unsigned numberOfOwnAwaits = context.analyses.opcodesOfValues().contains(Opcode::CallBuiltin) ? giveAwaitsToCallersOfNext(context, builder) : 0;
    context.build.statistics().count(StatisticID::AwaitsSunkIntoPredecessors, numberOfOwnAwaits);

    Vector<AwaitedCall> sites;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (function.at(value).opcode != Opcode::AwaitBySavingFrame || function.at(sameObjectThroughCasts(function, function.at(value).awaitedOperand())).opcode != Opcode::CallBuiltin)
                continue;
            if (std::optional<AwaitedCall> site = findAwaitedCall(context, value))
                sites.append(WTF::move(*site));
        }
    }
    if (sites.isEmpty())
        return numberOfOwnAwaits;
    rewriteAll(context, builder, sites.span());
    return sites.size() + numberOfOwnAwaits;
}

PassOutcome resolveAwaitedCalls(FunctionPassContext& context)
{
    bool awaitsCalls = isBodyOfAsyncFunction(context.function);
    bool collectsCalls = hasPromiseAllOfElements(context.function);
    if (!awaitsCalls && !collectsCalls)
        return PassOutcome::Unchanged;

    size_t numberOfBlocksBefore = context.function.blocks.size();
    PassOutcome outcome = resolveAwaitedAndCollectedCalls(context, awaitsCalls, collectsCalls);
    // Only an await that is expanded changes an edge, and it adds blocks.
    if (outcome == PassOutcome::Changed && context.function.blocks.size() == numberOfBlocksBefore)
        context.keptAnalyses = controlFlowAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
