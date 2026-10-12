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
#include "B3LoopUtilities.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
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

#if USE(BUN_JSC_ADDITIONS)
// Ahead of time nothing says how often a loop goes around, and most go around a few times at most. So in a position-independent procedure a value is
// hoisted only where that cannot cost anything: out of a block that is guaranteed to execute once the loop is entered, into a pre-header that leads nowhere
// else. A load stays where the loop has a call that is not rare: across a call it would take a callee-saved register or be reloaded, which costs what the load costs.
static constexpr unsigned maxHoistedLoadsPerLoop = 8;
#endif

bool hoistLoopInvariantValues(Procedure& proc)
{
    PhaseScope phaseScope(proc, "hoistLoopInvariantValues"_s);
    
    ensureLoopPreHeaders(proc);
    
    NaturalLoops& loops = proc.naturalLoops();
    if (!loops.numLoops())
        return false;

    proc.resetValueOwners();
    Dominators& dominators = proc.dominators();
    BackwardsDominators& backwardsDominators = proc.backwardsDominators();
    
    // FIXME: We should have a reusable B3::EffectsSet data structure.
    // https://bugs.webkit.org/show_bug.cgi?id=174762
    struct LoopData {
        RangeSet<HeapRange> writes;
        bool writesLocalState { false };
        bool writesPinned { false };
        bool exitsSideways { false };
        BasicBlock* preHeader { nullptr };
#if USE(BUN_JSC_ADDITIONS)
        // On every way around the loop that takes no rare edge.
        IndexSet<BasicBlock*> blocksGuaranteedToExecute;
        bool hasLikelyCall { false };
        unsigned numberOfHoistedLoads { 0 };
        Vector<Value*, 4> fixedLoadsInPreHeader;
#endif
    };

    IndexMap<NaturalLoop, LoopData> data(loops.numLoops());
    IndexSet<BasicBlock*> blockBarriers;
#if USE(BUN_JSC_ADDITIONS)
    constexpr unsigned maxBlocksOfLoopInOneWord = 64;
    IndexMap<BasicBlock*, unsigned> numberInLoop(proc.size(), 0u);
    Vector<uint64_t, maxBlocksOfLoopInOneWord> onEveryWayFrom;
    static const char* const fixedLoadsAsked = getenv("BUN_AOT_SWITCH_FIXED_LOADS_LEAVE_ARMS");
    const char fixedLoadsLeaveArms = fixedLoadsAsked && proc.positionIndependent() && fixedLoadsAsked[0] >= '1' && fixedLoadsAsked[0] <= '3' ? fixedLoadsAsked[0] : 0;
    auto isFixedLoad = [](Value* value) {
        if (value->opcode() != Load || value->traps())
            return false;
        MemoryValue* memory = value->as<MemoryValue>();
        return memory->readsMutability() == Mutability::Immutable && !memory->controlDependent() && !memory->hasFence();
    };
    auto isSameLoad = [](Value* a, Value* b) {
        return a->type() == b->type() && a->child(0) == b->child(0) && a->as<MemoryValue>()->offset() == b->as<MemoryValue>()->offset();
    };
    auto isMetOnEveryWay = [&](const NaturalLoop& loop, Value* load) {
        if (loop.size() > maxBlocksOfLoopInOneWord)
            return false;
        uint64_t holds = 0;
        for (unsigned i = loop.size(); i--;) {
            numberInLoop[loop.at(i)] = i;
            for (Value* other : *loop.at(i)) {
                if (isFixedLoad(other) && isSameLoad(other, load)) {
                    holds |= 1ull << i;
                    break;
                }
            }
        }
        uint64_t passes = ~0ull;
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned i = loop.size(); i--;) {
                if ((holds | ~passes) >> i & 1)
                    continue;
                bool goesOn = false;
                bool every = true;
                for (FrequentedBlock successor : loop.at(i)->successors()) {
                    if (isRare(successor, loop.header()))
                        continue;
                    goesOn = true;
                    if (successor.block() == loop.header() || !loops.belongsTo(successor.block(), loop))
                        every = false;
                    else
                        every &= passes >> numberInLoop[successor.block()] & 1;
                }
                if (!goesOn || !every) {
                    passes &= ~(1ull << i);
                    changed = true;
                }
            }
        }
        return !!(passes >> numberInLoop[loop.header()] & 1);
    };
#endif

    for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (loops.belongsTo(predecessor, loop))
                continue;
            RELEASE_ASSERT(!data[loop].preHeader);
            data[loop].preHeader = predecessor;
        }
#if USE(BUN_JSC_ADDITIONS)
        if (fixedLoadsLeaveArms) {
            for (Value* value : *data[loop].preHeader) {
                if (isFixedLoad(value))
                    data[loop].fixedLoadsInPreHeader.append(value);
            }
        }
        if (proc.positionIndependent() && loop.size() <= maxBlocksOfLoopInOneWord) {
            for (unsigned i = loop.size(); i--;)
                numberInLoop[loop.at(i)] = i;
            onEveryWayFrom.fill(~0ull, loop.size());
            for (bool changed = true; changed;) {
                changed = false;
                for (unsigned i = loop.size(); i--;) {
                    uint64_t common = ~0ull;
                    bool goesOn = false;
                    for (FrequentedBlock successor : loop.at(i)->successors()) {
                        if (isRare(successor, loop.header()))
                            continue;
                        goesOn = true;
                        if (successor.block() == loop.header() || !loops.belongsTo(successor.block(), loop))
                            common = 0;
                        else
                            common &= onEveryWayFrom[numberInLoop[successor.block()]];
                    }
                    uint64_t blocks = (goesOn ? common : 0) | 1ull << i;
                    changed |= blocks != onEveryWayFrom[i];
                    onEveryWayFrom[i] = blocks;
                }
            }
            for (unsigned i = loop.size(); i--;) {
                if (onEveryWayFrom[numberInLoop[loop.header()]] >> i & 1)
                    data[loop].blocksGuaranteedToExecute.add(loop.at(i));
            }
        } else if (proc.positionIndependent()) {
            for (BasicBlock* block = loop.header(); block && loops.belongsTo(block, loop) && data[loop].blocksGuaranteedToExecute.add(block);) {
                BasicBlock* next = nullptr;
                unsigned numberOfLikelySuccessors = 0;
                for (FrequentedBlock successor : block->successors()) {
                    if (isRare(successor, loop.header()))
                        continue;
                    ++numberOfLikelySuccessors;
                    next = successor.block();
                }
                block = numberOfLikelySuccessors == 1 ? next : nullptr;
            }
        }
#endif
    }
    
    for (BasicBlock* block : proc) {
        const NaturalLoop* loop = loops.innerMostLoopOf(block);
        if (!loop)
            continue;
        for (Value* value : *block) {
            Effects effects = value->effects();
            data[*loop].writes.add(effects.writes);
            data[*loop].writesLocalState |= effects.writesLocalState;
            data[*loop].writesPinned |= effects.writesPinned;
            data[*loop].exitsSideways |= effects.exitsSideways;
#if USE(BUN_JSC_ADDITIONS)
            if (proc.positionIndependent() && isCall(value) && block->frequency() * maxRatioOfHeaderToLikelyBlock >= loop->header()->frequency())
                data[*loop].hasLikelyCall = true;
#endif
            if (effects.isTrapBarrier())
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
#if USE(BUN_JSC_ADDITIONS)
            data[*current].hasLikelyCall |= data[loop].hasLikelyCall;
#endif
        }
    }
    
    bool changed = false;
    
    // Pre-order ensures that we visit our dominators before we visit ourselves. Otherwise we'd miss some
    // hoisting opportunities in complex CFGs.
    for (BasicBlock* block : proc.blocksInPreOrder()) {
        Vector<const NaturalLoop*> blockLoops = loops.loopsOf(block);
        if (blockLoops.isEmpty())
            continue;

        // Values that stay in this block run before everything later in it, so once one of them is a
        // trap barrier nothing after it can be hoisted out on the strength of trapping first.
        bool trapBarrierInBlock = false;
        for (Value*& value : *block) {
            Effects effects = value->effects();
            bool trappingLoad = isTrappingLoad(value);
            bool hoisted = false;

            // We never hoist write effects or control constructs. A trapping load is allowed
            // through. The fault it reports as a side exit is its own, and the checks below decide
            // whether it may move.
            if (!effects.mustExecute() || trappingLoad) {
                // Try outermost loop first.
                for (unsigned i = blockLoops.size(); i--;) {
                    const NaturalLoop& loop = *blockLoops[i];

#if USE(BUN_JSC_ADDITIONS)
                    if (proc.positionIndependent()) {
                        bool isSureToRun = data[loop].blocksGuaranteedToExecute.contains(block);
                        if (fixedLoadsLeaveArms && !data[loop].hasLikelyCall && isFixedLoad(value)) {
                            Value* same = nullptr;
                            for (Value* other : data[loop].fixedLoadsInPreHeader) {
                                if (isFixedLoad(other) && isSameLoad(other, value))
                                    same = other;
                            }
                            if (same) {
                                value->replaceWithIdentity(same);
                                changed = true;
                                hoisted = true;
                                break;
                            }
                            if (!isSureToRun && fixedLoadsLeaveArms == '1')
                                isSureToRun = isMetOnEveryWay(loop, value);
                            if (!isSureToRun && fixedLoadsLeaveArms == '3')
                                isSureToRun = block->frequency() * maxRatioOfHeaderToLikelyBlock >= loop.header()->frequency();
                        }
                        if (!isSureToRun || data[loop].preHeader->last()->opcode() != Jump)
                            continue;
                        if (effects.reads && (data[loop].hasLikelyCall || data[loop].numberOfHoistedLoads >= maxHoistedLoadsPerLoop))
                            continue;
                    }
#endif

                    bool ok = true;
                    for (Value* child : value->children()) {
                        if (!dominators.dominates(child->owner, data[loop].preHeader)) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok)
                        continue;

                    if (effects.controlDependent || trappingLoad) {
                        // The block is backward-dominating the pre-header.
                        // The value needs to be guaranteed to run on every iteration.
                        if (!backwardsDominators.dominates(block, data[loop].preHeader))
                            continue;

                        if (data[loop].exitsSideways) {
                            // Even if the loop can exit (after this value for example), if we do not have
                            // any exit between pre-header to this value, we can still hoist a value because
                            // this can be a first value causing a trap.
                            if (trapBarrierInBlock || !isReachedWithoutTrapBarrier(loops, loop, block, blockBarriers))
                                continue;
                        }
                    }

                    if (effects.readsPinned && data[loop].writesPinned)
                        continue;

                    if (effects.readsLocalState && data[loop].writesLocalState)
                        continue;

                    if (effects.readsMutability != Mutability::Immutable && data[loop].writes.overlaps(effects.reads))
                        continue;

                    data[loop].preHeader->appendNonTerminal(value);
#if USE(BUN_JSC_ADDITIONS)
                    if (fixedLoadsLeaveArms && isFixedLoad(value))
                        data[loop].fixedLoadsInPreHeader.append(value);
#endif
                    value = proc.add<Value>(Nop, Void, value->origin());
                    changed = true;
                    hoisted = true;
#if USE(BUN_JSC_ADDITIONS)
                    if (proc.positionIndependent()) {
                        data[loop].numberOfHoistedLoads += !!effects.reads;
                        break;
                    }
#endif
                }
            }

            if (!hoisted)
                trapBarrierInBlock |= effects.isTrapBarrier();
        }
    }
#if USE(BUN_JSC_ADDITIONS)
    for (unsigned loopIndex = fixedLoadsLeaveArms ? loops.numLoops() : 0; loopIndex--;) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        if (data[loop].hasLikelyCall)
            continue;
        for (Value* kept : data[loop].fixedLoadsInPreHeader) {
            if (!isFixedLoad(kept))
                continue;
            for (unsigned i = loop.size(); i--;) {
                for (Value* value : *loop.at(i)) {
                    if (isFixedLoad(value) && isSameLoad(kept, value)) {
                        value->replaceWithIdentity(kept);
                        changed = true;
                    }
                }
            }
        }
    }
#endif
    
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)

