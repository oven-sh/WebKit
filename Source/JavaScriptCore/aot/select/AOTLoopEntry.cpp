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
        if (!loops.belongsTo(uses.blockOf(id), loop) && analyses.dominates(id, loopHead) && isUsable(id) && !mayBeUnspecified(analyses, id))
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

bool canEmitOnLoopEntry(SelectionContext& context, BlockID header)
{
    const Function& function = context.function();
    const NaturalLoops& loops = context.analyses().naturalLoops();
    const NaturalLoop* loop = loops.headerOf(header);
    if (!loop)
        return false;
    std::optional<Edge> entry = loops.findSingleEntry(*loop);
    return entry && function.at(entry->from).handler == function.at(header).handler;
}

static BlockID ensurePreheader(IRBuilder& builder, Edge entry, BlockID header)
{
    Function& function = builder.function();
    if (function.at(entry.from).terminator.kind == TerminatorKind::Jump)
        return entry.from;

    BlockID preheader = builder.splitEdge(entry);
    if (function.at(header).handler)
        builder.coverWithHandler(singleElementSpan(preheader), *function.at(header).handler);
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
    if (function.at(entry->from).handler != function.at(header).handler)
        return LoopEntryResult::HandlerDiffers;

    BlockID preheader = ensurePreheader(builder, *entry, header);
    Terminator jump = std::exchange(function.edit(preheader).terminator, Terminator { });
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
