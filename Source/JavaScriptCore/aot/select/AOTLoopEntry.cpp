/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLoopEntry.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static std::optional<ValueID> findValueOutsideLoop(SelectionContext& context, const NaturalLoop& loop, ValueID operand, const ScopedLambda<bool(ValueID)>& isUsable)
{
    FunctionAnalyses& analyses = context.analyses();
    const NaturalLoops& loops = analyses.naturalLoops();
    const UseLists& uses = analyses.useLists();
    ProgramPoint loopHead = ProgramPoint::atHeadOf(loop.header());
    for (ValueID id : CastChain(context.function(), operand)) {
        // Code on the loop entry also runs on the paths that never reach the use inside the loop.
        if (!loops.belongsTo(uses.blockOf(id), loop) && analyses.dominates(id, loopHead) && isUsable(id) && !context.typeOf(id).mayBeUnspecified())
            return id;
    }
    return std::nullopt;
}

std::optional<InvariantOperand> findOutermostInvariantLoop(SelectionContext& context, BlockID user, ValueID operand)
{
    return findOutermostInvariantLoop(context, user, operand, [](ValueID) {
        return true;
    });
}

std::optional<InvariantOperand> findOutermostInvariantLoop(SelectionContext& context, BlockID user, ValueID operand, NOESCAPE const ScopedLambda<bool(ValueID)>& isUsable)
{
    if (!context.analyses().cfg().hasCycle())
        return std::nullopt;
    const NaturalLoops& loops = context.analyses().naturalLoops();
    std::optional<InvariantOperand> outermost;
    for (const NaturalLoop* loop = loops.innerMostLoopOf(user); loop; loop = loops.innerMostOuterLoop(*loop)) {
        std::optional<ValueID> valueOutsideLoop = findValueOutsideLoop(context, *loop, operand, isUsable);
        if (!valueOutsideLoop)
            break;
        outermost = InvariantOperand { loop->header(), *valueOutsideLoop };
    }
    return outermost;
}

using LoopParameters = Vector<ValueID, 4>;

// A handler edge from the loop entry reaches the handler without passing the header. After that, a value defined below the header no longer dominates its uses on the paths from the handler that avoid the header.
// A header parameter has a value on the loop entry, and the handler can take it as an argument where the handler dominates all of those uses. Returns those parameters, or std::nullopt if there is any other such use.
static std::optional<LoopParameters> findParametersReadAfterHandler(SelectionContext& context, BlockID header, BlockID handler)
{
    FunctionAnalyses& analyses = context.analyses();
    const Function& function = context.function();
    const Dominators& dominators = analyses.dominators();
    LoopParameters parameters;
    if (!dominators.dominates(header, handler))
        return parameters;

    BitVector reachedFromHandler;
    auto isReachedFromHandler = [&](BlockID block) {
        if (reachedFromHandler.isEmpty()) {
            Vector<BlockID, 16> worklist { handler };
            reachedFromHandler.set(handler.index());
            while (!worklist.isEmpty()) {
                for (BlockID successor : analyses.cfg().successors(worklist.takeLast())) {
                    if (successor != header && !reachedFromHandler.set(successor.index()))
                        worklist.append(successor);
                }
            }
        }
        return reachedFromHandler.get(block.index());
    };
    enum class UsesAfterHandler : uint8_t { None, BelowHandler, Elsewhere };
    auto usesAfterHandlerOf = [&](ValueID id) {
        UsesAfterHandler result = UsesAfterHandler::None;
        for (const Use& use : analyses.useLists().usesOf(id)) {
            if (!isReachedFromHandler(use.block))
                continue;
            if (!dominators.dominates(handler, use.block))
                return UsesAfterHandler::Elsewhere;
            result = UsesAfterHandler::BelowHandler;
        }
        return result;
    };
    auto isUsedAfterHandler = [&](ValueID id) {
        return usesAfterHandlerOf(id) != UsesAfterHandler::None;
    };
    for (BlockID block = dominators.idom(handler); block != header; block = dominators.idom(block)) {
        if (std::ranges::any_of(function.at(block).parameters, isUsedAfterHandler) || std::ranges::any_of(function.at(block).values, isUsedAfterHandler))
            return std::nullopt;
    }
    if (std::ranges::any_of(function.at(header).values, isUsedAfterHandler))
        return std::nullopt;
    for (ValueID parameter : function.at(header).parameters) {
        switch (usesAfterHandlerOf(parameter)) {
        case UsesAfterHandler::None:
            break;
        case UsesAfterHandler::BelowHandler:
            parameters.append(parameter);
            break;
        case UsesAfterHandler::Elsewhere:
            return std::nullopt;
        }
    }
    bool isEnteredByHandlerEdgesAlone = std::ranges::all_of(analyses.cfg().predecessors(handler), [&](BlockID predecessor) {
        return std::ranges::none_of(function.at(predecessor).terminator.targets, [&](const Target& target) {
            return target.block == handler;
        });
    });
    if (!parameters.isEmpty() && !isEnteredByHandlerEdgesAlone)
        return std::nullopt;
    return parameters;
}

static void passParametersToHandler(SelectionContext& context, IRBuilder& builder, BlockID handler, const LoopParameters& parameters)
{
    FunctionAnalyses& analyses = context.analyses();
    Function& function = builder.function();
    Vector<BlockID, 8> coveredBlocks;
    for (BlockID predecessor : analyses.cfg().predecessors(handler)) {
        if (!coveredBlocks.contains(predecessor))
            coveredBlocks.append(predecessor);
    }
    Vector<Vector<Use, 4>, 4> usesBelowHandler;
    for (ValueID parameter : parameters) {
        usesBelowHandler.append({ });
        for (const Use& use : analyses.useLists().usesOf(parameter)) {
            if (analyses.dominators().dominates(handler, use.block))
                usesBelowHandler.last().append(use);
        }
    }
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID received = builder.addParameter(handler, function.at(parameters[index]).type);
        for (BlockID block : coveredBlocks)
            builder.appendArgument(Edge::toHandlerOf(block), parameters[index]);
        for (const Use& use : usesBelowHandler[index])
            replaceUse(function, use, received);
    }
    analyses.didChangeControlFlow();
}

struct HandlerOnEntry {
    std::optional<Target> handler;
    LoopParameters parametersToPass;
};

// Code emitted on the loop entry stands in for work of the first iteration, so it gets the header's handler, which receives what it would receive from the start of the first iteration.
static std::optional<HandlerOnEntry> findHandlerOnEntry(SelectionContext& context, Edge entry, BlockID header)
{
    const Function& function = context.function();
    const std::optional<Target>& headerHandler = function.at(header).handler;
    if (!headerHandler || function.at(entry.from).handler == headerHandler)
        return HandlerOnEntry { headerHandler, { } };
    Target handler = *headerHandler;
    const Target& target = targetOf(function, entry);
    if (target.arguments.size() != function.at(header).parameters.size())
        return std::nullopt;
    ProgramPoint endOfEntry = pointBeforeTerminatorOf(function, entry.from);
    for (ValueID& argument : handler.arguments) {
        size_t parameter = function.at(header).parameters.find(argument);
        if (parameter != notFound)
            argument = target.arguments[parameter];
        else if (!context.analyses().dominates(argument, endOfEntry))
            return std::nullopt;
    }
    std::optional<LoopParameters> parametersToPass = findParametersReadAfterHandler(context, header, handler.block);
    if (!parametersToPass)
        return std::nullopt;
    return HandlerOnEntry { WTF::move(handler), WTF::move(*parametersToPass) };
}

bool canEmitOnLoopEntry(SelectionContext& context, BlockID header)
{
    const NaturalLoops& loops = context.analyses().naturalLoops();
    const NaturalLoop* loop = loops.headerOf(header);
    if (!loop)
        return false;
    std::optional<Edge> entry = loops.findSingleEntry(*loop);
    return entry && findHandlerOnEntry(context, *entry, header);
}

bool loopEntryGetsNewHandlerEdge(SelectionContext& context, BlockID header)
{
    const Function& function = context.function();
    const NaturalLoops& loops = context.analyses().naturalLoops();
    const NaturalLoop* loop = loops.headerOf(header);
    std::optional<Edge> entry = loop ? loops.findSingleEntry(*loop) : std::nullopt;
    return entry && function.at(header).handler && function.at(entry->from).handler != function.at(header).handler;
}

static BlockID ensurePreheader(IRBuilder& builder, Edge entry, const std::optional<Target>& handler)
{
    Function& function = builder.function();
    if (function.at(entry.from).terminator.kind == TerminatorKind::Jump && function.at(entry.from).handler == handler)
        return entry.from;

    BlockID preheader = builder.splitEdge(entry);
    if (handler)
        builder.coverWithHandler(singleElementSpan(preheader), *handler);
    return preheader;
}

LoopEntryResult emitOnLoopEntry(SelectionContext& context, IRBuilder& builder, BlockID header, NOESCAPE const LoopEntryEmitter& emit)
{
    builder.applyInsertions();

    FunctionAnalyses& analyses = context.analyses();
    const NaturalLoop* loop = analyses.naturalLoops().headerOf(header);
    if (!loop)
        return LoopEntryResult::Unknown;

    std::optional<Edge> entry = analyses.naturalLoops().findSingleEntry(*loop);
    if (!entry)
        return LoopEntryResult::NoSingleEntry;

    Function& function = builder.function();
    std::optional<HandlerOnEntry> found = findHandlerOnEntry(context, *entry, header);
    if (!found)
        return LoopEntryResult::HandlerDiffers;
    if (!found->parametersToPass.isEmpty()) {
        passParametersToHandler(context, builder, found->handler->block, found->parametersToPass);
        found = findHandlerOnEntry(context, *entry, header);
        RELEASE_ASSERT(found && found->parametersToPass.isEmpty());
    }

    BlockID preheader = ensurePreheader(builder, *entry, found->handler);
    if (found->handler)
        context.noteRemovedAllocationUnderHandler();
    Terminator jump = std::exchange(function.edit(preheader, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator { });
    analyses.didChangeControlFlow();

    Origin savedOrigin = builder.origin();
    std::optional<Target> savedHandler = builder.handler();
    builder.setOrigin(jump.origin());
    builder.setHandler(function.at(preheader).handler);
    builder.moveToEndOf(preheader);
    emit(builder);
    RELEASE_ASSERT(builder.currentBlock());
    builder.emitTerminator(WTF::move(jump));

    builder.setOrigin(savedOrigin);
    builder.setHandler(WTF::move(savedHandler));
    return LoopEntryResult::Emitted;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
