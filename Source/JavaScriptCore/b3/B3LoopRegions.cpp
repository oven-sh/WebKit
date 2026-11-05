/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LoopRegions.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3LoopInvariance.h"
#include "B3LoopUtilities.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

static bool loopsBackThroughInvariantTests(NaturalLoops& loops, const NaturalLoop& loop)
{
    for (BasicBlock* block : loop.header()->predecessors()) {
        if (!loops.belongsTo(block, loop))
            continue;
        while (block != loop.header() && block->last()->opcode() == Jump && block->numPredecessors() == 1)
            block = block->predecessor(0);
        if (block->last()->opcode() != Branch || loops.belongsTo(block->last()->child(0)->owner, loop))
            return false;
    }
    return true;
}

std::expected<HotRegion, GaveUp> findHotRegion(NaturalLoops& loops, const NaturalLoop& loop, std::optional<UnswitchedEdge> unswitched)
{
    HotRegion region;
    region.loop = &loop;
    region.header = loop.header();
    region.preHeader = findPreHeader(loops, loop);
    if (!region.preHeader)
        return std::unexpected(GaveUp(GaveUpReason::SeveralEntries));

    region.blockSet.add(loop.header());
    region.blocks.append(loop.header());
    bool hasBackEdge = false;
    for (unsigned i = 0; i < region.blocks.size(); ++i) {
        BasicBlock* block = region.blocks[i];
        FrequencyClass likeliestClass = likeliestSuccessorClass(block);
        for (const FrequentedBlock& successor : block->successors()) {
            if (!isLikelySuccessor(likeliestClass, successor) || !loops.belongsTo(successor.block(), loop))
                continue;
            if (unswitched && block == unswitched->block && successor.block() != unswitched->successor)
                continue;
            hasBackEdge |= successor.block() == loop.header();
            // A loop inside that only rare edges lead into is one more slow path.
            if (loops.innerMostLoopOf(successor.block()) != &loop)
                return std::unexpected(GaveUp(GaveUpReason::NotInnermost));
            if (region.blockSet.add(successor.block()))
                region.blocks.append(successor.block());
        }
    }
    if (!hasBackEdge || loopsBackThroughInvariantTests(loops, loop))
        return std::unexpected(GaveUp(GaveUpReason::NoSingleLikelyPath));

    for (unsigned i = loop.size(); i--;) {
        if (!region.contains(loop.at(i)))
            region.slowPathBlocks.append(loop.at(i));
    }
    return region;
}

BasicBlock* likelySuccessorOfGuard(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block)
{
    if (block->numSuccessors() != 2 || (block->last()->opcode() != Branch && block->last()->opcode() != Patchpoint))
        return nullptr;
    FrequencyClass first = block->successor(0).frequency();
    FrequencyClass second = block->successor(1).frequency();
    if (isRarerThan(first, second))
        return block->successorBlock(1);
    if (isRarerThan(second, first))
        return block->successorBlock(0);
    if (first == FrequencyClass::Rare)
        return nullptr;
    bool firstLeaves = !loops.belongsTo(block->successorBlock(0), loop);
    bool secondLeaves = !loops.belongsTo(block->successorBlock(1), loop);
    if (firstLeaves == secondLeaves)
        return nullptr;
    return block->successorBlock(firstLeaves ? 1 : 0);
}

static bool canCloneRareBlock(BasicBlock* block, const InvalidationSet& invalidation)
{
    for (Value* value : *block) {
        if (!canCloneForLoop(value) || invalidation.isInvalidatedBy(value->effects()))
            return false;
    }
    return true;
}

IndexSet<BasicBlock*> findRejoiningBlocks(NaturalLoops& loops, const HotRegion& region, const InvalidationSet& invalidation)
{
    IndexSet<BasicBlock*> rejoiningBlocks;
    IndexSet<BasicBlock*> visited;
    Vector<BasicBlock*, 16> worklist { region.header };
    while (!worklist.isEmpty()) {
        for (BasicBlock* predecessor : worklist.takeLast()->predecessors()) {
            if (predecessor == region.header || !loops.belongsTo(predecessor, *region.loop) || !visited.add(predecessor))
                continue;
            if (!region.contains(predecessor) && (loops.innerMostLoopOf(predecessor) != region.loop || !canCloneRareBlock(predecessor, invalidation)))
                continue;
            rejoiningBlocks.add(predecessor);
            worklist.append(predecessor);
        }
    }
    return rejoiningBlocks;
}

static Vector<FrequentedBlock, 8> findExcursionEntries(const HotRegion& region, const HoistPlan& hoisted, const IndexSet<BasicBlock*>& rejoiningBlocks)
{
    Vector<FrequentedBlock, 8> edges;
    for (BasicBlock* block : region.blocks) {
        if (hoisted.isHoisted(block->last()))
            continue;
        for (const FrequentedBlock& successor : block->successors()) {
            if (!region.contains(successor.block()) && rejoiningBlocks.contains(successor.block()))
                edges.append(successor);
        }
    }
    std::ranges::stable_sort(edges, { }, [](const FrequentedBlock& edge) {
        return rarityOf(edge.frequency());
    });
    return edges;
}

template<typename Functor>
static void forEachSuccessorInExcursion(BasicBlock* block, const HoistPlan& hoisted, const Functor& functor)
{
    if (hoisted.valuesValidInExcursion.contains(block->last())) {
        functor(FrequentedBlock(hoisted.likelySuccessors.get(block->last())));
        return;
    }
    for (const FrequentedBlock& successor : block->successors())
        functor(successor);
}

static IndexSet<BasicBlock*> findShareableBlocks(const HotRegion& region, const HoistPlan& hoisted, const ScopedLambda<bool(Value*)>& isReplacedOnlyInFastLoop)
{
    IndexSet<BasicBlock*> result;
    for (BasicBlock* block : region.blocks) {
        bool isSame = block != region.header;
        for (Value* value : *block)
            isSame &= hoisted.isHoisted(value) ? hoisted.valuesValidInExcursion.contains(value) : !isReplacedOnlyInFastLoop(value);
        if (isSame)
            result.add(block);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = region.blocks.size(); i--;) {
            BasicBlock* block = region.blocks[i];
            if (!result.contains(block))
                continue;
            bool staysSame = true;
            forEachSuccessorInExcursion(block, hoisted, [&](const FrequentedBlock& successor) {
                staysSame &= successor.block() == region.header || !region.contains(successor.block()) || result.contains(successor.block());
            });
            if (!staysSame) {
                result.remove(block);
                changed = true;
            }
        }
    }
    return result;
}

static void pruneNonRejoiningBlocks(Excursion& excursion, const HotRegion& region, const HoistPlan& hoisted)
{
    IndexSet<BasicBlock*> rejoins;
    for (bool changed = true; changed;) {
        changed = false;
        for (BasicBlock* block : excursion.blocks) {
            if (rejoins.contains(block))
                continue;
            bool reachesHeader = false;
            forEachSuccessorInExcursion(block, hoisted, [&](const FrequentedBlock& successor) {
                reachesHeader |= successor.block() == region.header || rejoins.contains(successor.block()) || excursion.rejoinsFastLoop(block, successor);
            });
            if (reachesHeader)
                changed |= rejoins.add(block);
        }
    }
    excursion.blocks.removeAllMatching([&](BasicBlock* block) {
        return !rejoins.contains(block);
    });
}

static void dropUnreachedSharedBlocks(Excursion& excursion, const HoistPlan& hoisted)
{
    IndexSet<BasicBlock*> reached;
    Vector<BasicBlock*, 8> worklist;
    for (BasicBlock* block : excursion.blocks) {
        forEachSuccessorInExcursion(block, hoisted, [&](const FrequentedBlock& successor) {
            if (excursion.rejoinsFastLoop(block, successor) && reached.add(successor.block()))
                worklist.append(successor.block());
        });
    }
    while (!worklist.isEmpty()) {
        forEachSuccessorInExcursion(worklist.takeLast(), hoisted, [&](const FrequentedBlock& successor) {
            if (excursion.sharedBlocks.contains(successor.block()) && reached.add(successor.block()))
                worklist.append(successor.block());
        });
    }
    excursion.sharedBlocks = WTF::move(reached);
}

Excursion findExcursion(Procedure& proc, NaturalLoops& loops, const HotRegion& region, const HoistPlan& hoisted, const ScopedLambda<bool(Value*)>& isReplacedOnlyInFastLoop)
{
    IndexSet<BasicBlock*> rejoiningBlocks = findRejoiningBlocks(loops, region, hoisted.invalidation);
    Excursion excursion;
    excursion.sharedBlocks = findShareableBlocks(region, hoisted, isReplacedOnlyInFastLoop);
    IndexSet<BasicBlock*> reached;
    uint64_t numberOfClonedValues = 0;
    auto add = [&](BasicBlock* block) {
        if (!rejoiningBlocks.contains(block) || !reached.add(block))
            return;
        uint64_t cost = 0;
        for (Value* value : *block)
            cost += emitsInstructions(value) && !hoisted.valuesValidInExcursion.contains(value);
        if (numberOfClonedValues + cost > proc.limit(Limit::MaxClonedValuesPerExcursion)) {
            proc.didReachLimit(Limit::MaxClonedValuesPerExcursion, block->at(0)->origin());
            return;
        }
        numberOfClonedValues += cost;
        excursion.blocks.append(block);
    };
    Vector<FrequentedBlock, 8> entries = findExcursionEntries(region, hoisted, rejoiningBlocks);
    if (!excursion.sharedBlocks.isEmpty()) {
        auto addUnlikely = [&](const FrequentedBlock& edge) {
            if (edge.isRare() || excursion.sharedBlocks.contains(edge.block()))
                return;
            unsigned numberOfBlocks = excursion.blocks.size();
            add(edge.block());
            if (excursion.blocks.size() > numberOfBlocks)
                excursion.unlikelyBlocks.add(edge.block());
        };
        for (const FrequentedBlock& entry : entries)
            addUnlikely(entry);
        for (unsigned i = 0; i < excursion.blocks.size(); ++i)
            forEachSuccessorInExcursion(excursion.blocks[i], hoisted, addUnlikely);
    }
    for (const FrequentedBlock& entry : entries)
        add(entry.block());
    for (unsigned i = 0; i < excursion.blocks.size(); ++i) {
        forEachSuccessorInExcursion(excursion.blocks[i], hoisted, [&](const FrequentedBlock& successor) {
            if (!excursion.rejoinsFastLoop(excursion.blocks[i], successor))
                add(successor.block());
        });
    }
    pruneNonRejoiningBlocks(excursion, region, hoisted);
    dropUnreachedSharedBlocks(excursion, hoisted);
    return excursion;
}

FastPreHeader::FastPreHeader(Procedure& proc, BasicBlock* slowLoopHeader)
    : m_proc(proc)
    , m_slowLoopHeader(slowLoopHeader)
    , m_first(proc.addBlock())
    , m_last(m_first)
{
}

BasicBlock* FastPreHeader::startNextBlock()
{
    BasicBlock* closed = m_last;
    m_last = m_proc.addBlock();
    return closed;
}

Value* FastPreHeader::appendIntConstant(Origin origin, Type type, int64_t value)
{
    return m_last->appendIntConstant(m_proc, origin, type, value);
}

Value* FastPreHeader::append(Origin origin, Opcode opcode, Value* left, Value* right)
{
    return m_last->appendNew<Value>(m_proc, opcode, origin, left, right);
}

Value* FastPreHeader::appendAddConstant(Origin origin, Value* value, int64_t addend)
{
    return addend ? append(origin, Add, value, appendIntConstant(origin, value->type(), addend)) : value;
}

Value* FastPreHeader::appendWidened(Origin origin, Value* value, Signedness signedness)
{
    if (value->type() == Int64)
        return value;
    return m_last->appendNew<Value>(m_proc, signedness == Signedness::Signed ? SExt32 : ZExt32, origin, value);
}

void FastPreHeader::appendGuard(Origin origin, Opcode relation, Value* left, Value* right)
{
    m_last->appendNew<Value>(m_proc, Branch, origin, append(origin, relation, left, right));
    BasicBlock* closed = startNextBlock();
    closed->setSuccessors(FrequentedBlock(m_last), FrequentedBlock(m_slowLoopHeader, FrequencyClass::Rare));
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
