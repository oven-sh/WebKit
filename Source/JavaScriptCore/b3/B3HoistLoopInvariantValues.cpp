/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "B3HoistLoopInvariantValues.h"

#if ENABLE(B3_JIT)

#include "B3BackwardsDominators.h"
#include "B3Dominators.h"
#include "B3EnsureLoopPreHeaders.h"
#include "B3LikelyPaths.h"
#include "B3Limits.h"
#include "B3LoopUtilities.h"
#include "B3MovedLoadCheck.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RegisterPressure.h"
#include "B3ValueInlines.h"
#include <array>
#include <wtf/HashSet.h>
#include <wtf/RangeSet.h>

namespace JSC { namespace B3 {

// A load that can trap reports exitsSideways, which normally disqualifies it from hoisting. The
// fault is its only escaping effect, so it can still move to a point where it is certain to run and
// where nothing observable precedes it. WasmGC field and length loads depend on this: their null
// check is a trap rather than a branch, which would otherwise be all that blocks hoisting them.
static bool isTrappingLoad(const Value* value)
{
    if (!value->traps())
        return false;
    switch (value->opcode()) {
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
        // A fenced load also orders memory, which is an effect that cannot move.
        return !value->as<MemoryValue>()->hasFence();
    case WasmStructGet:
    case WasmArrayLength:
        return true;
    default:
        return false;
    }
}

// Whether a poll leaves depends on nothing that the procedure computes, so it guards no value behind it.
static bool isBarrier(const Value* value, const Effects& effects)
{
    return effects.isTrapBarrier() && !isLoopPoll(value);
}

static bool isReachedWithoutTrapBarrier(const NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block, const IndexSet<BasicBlock*>& blockBarriers)
{
    if (block == loop.header())
        return true;

    // Tracing ancestors from the block. If any blocks belonging to this loop has a trap,
    // then we cannot hoist since that trap needs to be executed before the hoisted value causes
    // a trap at a pre-header block.
    Vector<BasicBlock*, 8> worklist;
    IndexSet<BasicBlock*> seen;
    auto push = [&](BasicBlock* predecessor) {
        if (loops.belongsTo(predecessor, loop)) {
            if (seen.add(predecessor)) {
                // Every value in these blocks runs before the one being hoisted.
                if (blockBarriers.contains(predecessor))
                    return false;

                // Stopping at the header is enough even though earlier iterations run other blocks:
                // callers only reach here for a value whose block backwards-dominates the pre-header,
                // and backwards dominance counts a back edge as a way for the procedure to end, so
                // such a value always runs before any iteration completes.
                if (predecessor == loop.header())
                    return true;

                worklist.append(predecessor);
            }
        }
        return true;
    };

    for (BasicBlock* predecessor : block->predecessors()) {
        if (!push(predecessor))
            return false;
    }

    while (!worklist.isEmpty()) {
        BasicBlock* current = worklist.takeLast();
        for (BasicBlock* predecessor : current->predecessors()) {
            if (!push(predecessor))
                return false;
        }
    }

    return true;
}

bool hoistLoopInvariantValues(Procedure& proc)
{
    PhaseScope phaseScope(proc, "hoistLoopInvariantValues"_s);
    
    ensureLoopPreHeaders(proc);
    
    NaturalLoops& loops = proc.naturalLoops();
    if (!loops.numLoops())
        return false;

    proc.resetValueOwners();
    Dominators& dominators = proc.dominators();
    const bool hoistsOnlyFromLikelyPaths = proc.policy().loops.hoistsOnlyFromLikelyPaths;
    const bool hoistsImmutableLoadsPastWrites = proc.policy().loops.hoistsImmutableLoadsPastWrites;
    
    // FIXME: We should have a reusable B3::EffectsSet data structure.
    // https://bugs.webkit.org/show_bug.cgi?id=174762
    struct LoopData {
        RangeSet<HeapRange> writes;
        bool writesLocalState { false };
        bool writesPinned { false };
        bool exitsSideways { false };
        BasicBlock* preHeader { nullptr };
        UncheckedKeyHashSet<BasicBlock*> blocksOnEveryLikelyPath;
        UncheckedKeyHashSet<BasicBlock*> likelyBlocks;
        RegisterBudget registerBudget;
        UncheckedKeyHashSet<Value*> loadsLiveThroughLoop;
        std::array<unsigned, numBanks> numberOfLoadsLiveThroughLoop { };
        unsigned numberOfHoistedLoads { 0 };
        bool didReachLoadLimit { false };
    };

    IndexMap<NaturalLoop, LoopData> data(loops.numLoops());
    auto bankOfLoad = [](const Value* load) {
        return load->type().isNumeric() ? load->resultBank() : GP;
    };
    auto addLoadLiveThroughLoop = [&](const NaturalLoop& loop, Value* load) {
        if (data[loop].loadsLiveThroughLoop.add(load).isNewEntry)
            ++data[loop].numberOfLoadsLiveThroughLoop[bankOfLoad(load)];
    };
    IndexSet<BasicBlock*> blockBarriers;
    std::optional<LikelyPathAnalysis> likelyPaths;
    if (hoistsOnlyFromLikelyPaths)
        likelyPaths.emplace(proc, loops);

    for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (loops.belongsTo(predecessor, loop))
                continue;
            RELEASE_ASSERT(!data[loop].preHeader);
            data[loop].preHeader = predecessor;
        }
        if (hoistsOnlyFromLikelyPaths) {
            auto keepBlocksOfLoop = [&](const IndexSet<BasicBlock*>& blocks, UncheckedKeyHashSet<BasicBlock*>& result) {
                for (unsigned index = loop.size(); index--;) {
                    if (blocks.contains(loop.at(index)))
                        result.add(loop.at(index));
                }
            };
            IndexSet<BasicBlock*> blocksOnEveryLikelyPath;
            likelyPaths->findBlocksOnEveryLikelyPath(loop, blocksOnEveryLikelyPath);
            keepBlocksOfLoop(blocksOnEveryLikelyPath, data[loop].blocksOnEveryLikelyPath);
            keepBlocksOfLoop(findLikelyBlocks(loops, loop), data[loop].likelyBlocks);
            data[loop].registerBudget = allocatableRegisters(proc);
        }
    }
    
    for (BasicBlock* block : proc) {
        const NaturalLoop* loop = loops.innerMostLoopOf(block);
        if (!loop)
            continue;
        for (Value* value : *block) {
            Effects effects = value->effects();
            effects.writes.addTo(data[*loop].writes);
            data[*loop].writesLocalState |= effects.writesLocalState;
            data[*loop].writesPinned |= effects.writesPinned;
            data[*loop].exitsSideways |= effects.exitsSideways && !isLoopPoll(value);
            if (hoistsOnlyFromLikelyPaths && data[*loop].likelyBlocks.contains(block))
                data[*loop].registerBudget.restrictTo(registersPreservedBy(proc, value));
            if (hoistsOnlyFromLikelyPaths) {
                for (Value* child : value->children()) {
                    if (loops.belongsTo(child->owner, *loop) || !child->effects().reads)
                        continue;
                    for (const NaturalLoop* current = loop; current && !loops.belongsTo(child->owner, *current); current = loops.innerMostOuterLoop(*current))
                        addLoadLiveThroughLoop(*current, child);
                }
            }
            if (isBarrier(value, effects))
                blockBarriers.add(block);
        }
    }
    
    for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        for (const NaturalLoop* current = loops.innerMostOuterLoop(loop); current; current = loops.innerMostOuterLoop(*current)) {
            data[*current].writes.addAll(data[loop].writes);
            data[*current].writesLocalState |= data[loop].writesLocalState;
            data[*current].writesPinned |= data[loop].writesPinned;
            data[*current].exitsSideways |= data[loop].exitsSideways;
            if (data[*current].likelyBlocks.contains(loop.header()))
                data[*current].registerBudget.restrictTo(data[loop].registerBudget);
        }
    }
    
    bool changed = false;
    
    // Pre-order ensures that we visit our dominators before we visit ourselves. Otherwise we'd miss some
    // hoisting opportunities in complex CFGs.
    for (BasicBlock* block : proc.cachedBlocksInPreOrder()) {
        Vector<const NaturalLoop*, 8> blockLoops;
        for (const NaturalLoop* loop = loops.innerMostLoopOf(block); loop; loop = loops.innerMostOuterLoop(*loop))
            blockLoops.append(loop);
        if (blockLoops.isEmpty())
            continue;
        bool mayHoistFromBlock = !hoistsOnlyFromLikelyPaths || blockLoops.containsIf([&](const NaturalLoop* loop) {
            return data[*loop].blocksOnEveryLikelyPath.contains(block);
        });
        if (!mayHoistFromBlock)
            continue;

        // Values that stay in this block run before everything later in it, so once one of them is a
        // trap barrier nothing after it can be hoisted out on the strength of trapping first.
        bool trapBarrierInBlock = false;
        for (Value*& value : *block) {
            Effects effects = value->effects();
            bool isControlDependent = effects.controlDependent && proc.effectsOf(value).controlDependent;
            bool trappingLoad = isTrappingLoad(value);
            bool hoisted = false;

            // We never hoist write effects or control constructs. A trapping load is allowed
            // through. The fault it reports as a side exit is its own, and the checks below decide
            // whether it may move.
            if (!effects.mustExecute() || trappingLoad) {
                // Try outermost loop first.
                for (unsigned i = blockLoops.size(); i--;) {
                    const NaturalLoop& loop = *blockLoops[i];

                    if (hoistsOnlyFromLikelyPaths) {
                        if (!data[loop].blocksOnEveryLikelyPath.contains(block) || data[loop].preHeader->last()->opcode() != Jump)
                            continue;
                        if (effects.reads) {
                            Bank bank = bankOfLoad(value);
                            bool hasRegisterInEveryLoop = true;
                            for (unsigned inner = 0; inner <= i; ++inner) {
                                const LoopData& innerData = data[*blockLoops[inner]];
                                hasRegisterInEveryLoop &= innerData.numberOfLoadsLiveThroughLoop[bank] < innerData.registerBudget.of(bank);
                            }
                            if (!hasRegisterInEveryLoop)
                                continue;
                            if (data[loop].numberOfHoistedLoads >= proc.limit(Limit::MaxHoistedLoadsPerLoop)) {
                                if (!std::exchange(data[loop].didReachLoadLimit, true))
                                    proc.didReachLimit(Limit::MaxHoistedLoadsPerLoop, value->origin());
                                continue;
                            }
                        }
                    }

                    bool ok = true;
                    for (Value* child : value->children()) {
                        if (!dominators.dominates(child->owner, data[loop].preHeader)) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok)
                        continue;

                    if (effects.readsPinned && data[loop].writesPinned)
                        continue;

                    if (effects.readsLocalState && data[loop].writesLocalState)
                        continue;

                    bool ignoresWrites = hoistsImmutableLoadsPastWrites && effects.readsMutability == Mutability::Immutable;
                    if (!ignoresWrites && effects.reads.overlaps(data[loop].writes))
                        continue;

                    if (isControlDependent || trappingLoad) {
                        // The block is backward-dominating the pre-header.
                        // The value needs to be guaranteed to run on every iteration.
                        if (!proc.backwardsDominators().dominates(block, data[loop].preHeader))
                            continue;

                        if (data[loop].exitsSideways) {
                            // Even if the loop can exit (after this value for example), if we do not have
                            // any exit between pre-header to this value, we can still hoist a value because
                            // this can be a first value causing a trap.
                            if (trapBarrierInBlock || !isReachedWithoutTrapBarrier(loops, loop, block, blockBarriers))
                                continue;
                        }
                    }

                    data[loop].preHeader->appendNonTerminal(value);
                    if (hoistsOnlyFromLikelyPaths && effects.reads) {
                        ++data[loop].numberOfHoistedLoads;
                        for (unsigned inner = 0; inner <= i; ++inner)
                            addLoadLiveThroughLoop(*blockLoops[inner], value);
                    }
                    if (std::optional<MovedLoad> moved = MovedLoad::of(proc, value)) {
                        proc.movedLoadLog().add(*moved, block, static_cast<unsigned>(&value - block->values().begin()), value->lastChild(), value);
                        for (unsigned inLoop = 0; inLoop < loop.size(); ++inLoop)
                            proc.movedLoadLog().addBehindEachValueThatStaysIn(loop.at(inLoop), *moved, value->lastChild(), value);
                    }
                    value = proc.add<Value>(Nop, Void, value->origin());
                    changed = true;
                    hoisted = true;
                    if (hoistsOnlyFromLikelyPaths)
                        break;
                }
            }

            if (!hoisted)
                trapBarrierInBlock |= isBarrier(value, effects);
        }
    }
    
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

