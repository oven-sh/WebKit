/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DeadLoop {
    BlockID header;
    BlockID exit;
    BitVector body;
};

struct LoopEntry {
    Edge edge;
    BlockID exit;
};

} // anonymous namespace

static bool hasNoEffectInLoop(FunctionPassContext& context, ValueID id)
{
    if (context.function.at(id).opcode == Opcode::LoopHint)
        return true;
    return context.analyses.isRemovableWhenUnused(id);
}

static bool hasNoEffectInLoop(FunctionPassContext& context, const Block& block)
{
    return std::ranges::all_of(block.values, [&](ValueID id) {
        return hasNoEffectInLoop(context, id);
    });
}

static bool mayHaveDeadLoop(FunctionPassContext& context)
{
    const Function& function = context.function;
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        bool hasLoopHint = std::ranges::any_of(block.values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::LoopHint;
        });
        return hasLoopHint && hasNoEffectInLoop(context, block);
    });
}

static bool isRemovableBody(FunctionPassContext& context, const BitVector& body)
{
    for (size_t index : body) {
        const Block& block = context.function.at(BlockID(index));
        TerminatorKind kind = block.terminator.kind;
        if (kind != TerminatorKind::Jump && kind != TerminatorKind::Branch)
            return false;
        if (block.handler || block.isHandlerBlock())
            return false;
        if (!hasNoEffectInLoop(context, block))
            return false;
    }
    return true;
}

static std::optional<BlockID> findSingleExit(const Function& function, const BitVector& body)
{
    BlockID exit;
    for (size_t index : body) {
        for (const Target& target : function.at(BlockID(index)).terminator.targets) {
            if (body.get(target.block.index()))
                continue;
            if (!target.arguments.isEmpty() || !target.proofs.isEmpty() || (exit && exit != target.block))
                return std::nullopt;
            exit = target.block;
        }
    }
    if (!exit)
        return std::nullopt;
    return exit;
}

static bool definesValueUsedOutside(FunctionPassContext& context, const BitVector& body)
{
    const UseLists& useLists = context.analyses.useLists();
    auto isUsedOutside = [&](ValueID id) {
        return std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) {
            return !body.get(use.block.index());
        });
    };
    for (size_t index : body) {
        const Block& block = context.function.at(BlockID(index));
        if (std::ranges::any_of(block.parameters, isUsedOutside) || std::ranges::any_of(block.values, isUsedOutside))
            return true;
    }
    return false;
}

static bool hasOnlyNaturalCycles(FunctionPassContext& context, const BitVector& body)
{
    CFG& cfg = context.analyses.cfg();
    Dominators& dominators = context.analyses.dominators();
    for (size_t index : body) {
        BlockID from(index);
        for (BlockID to : cfg.successors(from)) {
            if (!body.get(to.index()) || cfg.reversePostOrderIndex(to) > cfg.reversePostOrderIndex(from))
                continue;
            if (!dominators.dominates(to, from))
                return false;
        }
    }
    return true;
}

static bool isCountedAndEnds(FunctionPassContext& context, const NaturalLoop& loop)
{
    const InductionVariable* counter = context.analyses.inductionVariables().of(loop).counter();
    if (!counter || counter->step.kind != InductionStepKind::Constant || !counter->step.constant)
        return false;
    const InductionTest* test = counter->controllingTest();
    if (!test || test->boundKind != LoopBoundKind::Invariant)
        return false;
    if (!isBoundingRelation(test->relation, counter->direction))
        return false;

    auto isInteger = [&](ValueID id) {
        return context.program.typeOf(context.function, id).isInteger();
    };
    return isInteger(counter->parameter) && isInteger(test->bound);
}

static bool alwaysTerminates(FunctionPassContext& context, const BitVector& body)
{
    if (!hasOnlyNaturalCycles(context, body))
        return false;
    NaturalLoops& loops = context.analyses.naturalLoops();
    for (unsigned i = 0; i < loops.numLoops(); ++i) {
        const NaturalLoop& loop = loops.loop(i);
        if (body.get(loop.header().index()) && !isCountedAndEnds(context, loop))
            return false;
    }
    return true;
}

static std::optional<DeadLoop> describeDeadLoop(FunctionPassContext& context, const NaturalLoop& loop)
{
    BitVector body(context.function.blocks.size());
    for (unsigned i = 0; i < loop.size(); ++i)
        body.quickSet(loop.at(i).index());

    if (!isRemovableBody(context, body))
        return std::nullopt;
    std::optional<BlockID> exit = findSingleExit(context.function, body);
    if (!exit || definesValueUsedOutside(context, body) || !alwaysTerminates(context, body))
        return std::nullopt;
    return DeadLoop { loop.header(), *exit, WTF::move(body) };
}

static Vector<DeadLoop> findOutermostDeadLoops(FunctionPassContext& context)
{
    Vector<DeadLoop> found;
    NaturalLoops& loops = context.analyses.naturalLoops();
    for (unsigned i = 0; i < loops.numLoops(); ++i) {
        if (std::optional<DeadLoop> loop = describeDeadLoop(context, loops.loop(i)))
            found.append(WTF::move(*loop));
    }
    AOT::sort(found, [](const DeadLoop& a, const DeadLoop& b) {
        size_t aSize = a.body.bitCount();
        size_t bSize = b.body.bitCount();
        return aSize != bSize ? aSize > bSize : a.header.index() < b.header.index();
    });

    Vector<DeadLoop> outermost;
    BitVector covered(context.function.blocks.size());
    for (DeadLoop& loop : found) {
        if (covered.get(loop.header.index()))
            continue;
        covered.merge(loop.body);
        outermost.append(WTF::move(loop));
    }
    return outermost;
}

static Vector<LoopEntry> findLoopEntries(FunctionPassContext& context, const Vector<DeadLoop>& loops)
{
    CFG& cfg = context.analyses.cfg();
    Vector<LoopEntry> entries;
    for (const DeadLoop& loop : loops) {
        for (Edge edge : cfg.incomingEdges(loop.header)) {
            if (!loop.body.get(edge.from.index()))
                entries.append(LoopEntry { edge, loop.exit });
        }
    }
    return entries;
}

static bool removeOutermostDeadLoops(FunctionPassContext& context)
{
    if (!mayHaveDeadLoop(context))
        return false;
    Vector<DeadLoop> loops = findOutermostDeadLoops(context);
    if (loops.isEmpty())
        return false;
    Vector<LoopEntry> entries = findLoopEntries(context, loops);

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    for (const LoopEntry& entry : entries)
        builder.redirectEdge(entry.edge, Target { entry.exit, { }, { } });
    builder.emptyUnreachableBlocks();
    return true;
}

PassOutcome removeDeadCountedLoops(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    while (removeOutermostDeadLoops(context))
        outcome = PassOutcome::Changed;
    // The builder makes every edit and tells the analyses, and the search that found no more loops came behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
