/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RotateLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3LoopUtilities.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3RegionCloner.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"

namespace JSC { namespace B3 {

namespace {

class RotateLoops {
public:
    RotateLoops(Procedure& proc, const Vector<BasicBlock*>* allowedHeaders)
        : m_proc(proc)
        , m_allowedHeaders(allowedHeaders)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
            if (!m_allowedHeaders || m_allowedHeaders->contains(loops.loop(loopIndex).header()))
                considerLoop(loops, loops.loop(loopIndex));
        }
        if (m_rotations.isEmpty())
            return false;

        std::ranges::stable_sort(m_rotations, { }, &Rotation::depth);
        RegionCloner cloner(m_proc);
        for (auto& rotation : m_rotations) {
            for (BasicBlock* block : rotation.testBlocks)
                cloner.addBlock(block);
        }
        cloner.demoteEscapingValues();
        m_proc.resetValueOwners();
        for (auto& rotation : m_rotations)
            rotate(cloner, rotation);
        return cloner.finish();
    }

private:
    struct Rotation {
        unsigned depth { 0 };
        Vector<BasicBlock*, 4> testBlocks;
        Vector<BasicBlock*, 2> latches;
        BasicBlock* body { nullptr };
        bool keepsBackEdgeToGuard { false };
    };

    bool leadsBackToHeaderOnly(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* start, BasicBlock* body)
    {
        Vector<BasicBlock*, 8> region { start };
        for (unsigned i = 0; i < region.size(); ++i) {
            if (region[i] == body || loops.innerMostLoopOf(region[i]) != &loop)
                return false;
            for (BasicBlock* successor : region[i]->successorBlocks()) {
                if (successor == loop.header() || !loops.belongsTo(successor, loop) || region.contains(successor))
                    continue;
                if (region.size() == m_proc.limit(Limit::MaxRareBlocksPerRotatedLoop)) {
                    m_proc.didReachLimit(Limit::MaxRareBlocksPerRotatedLoop, start->at(0)->origin());
                    return false;
                }
                region.append(successor);
            }
        }
        for (unsigned i = 1; i < region.size(); ++i) {
            for (BasicBlock* predecessor : region[i]->predecessors()) {
                if (!region.contains(predecessor))
                    return false;
            }
        }
        return start->numPredecessors() == 1;
    }

    bool isBottomTested(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* latch)
    {
        BasicBlock* block = latch;
        for (uint64_t i = 0; i < m_proc.limit(Limit::MaxRareBlocksPerRotatedLoop); ++i) {
            if (block->last()->opcode() == Branch)
                return !loops.belongsTo(block->successorBlock(0), loop) || !loops.belongsTo(block->successorBlock(1), loop);
            if (block->last()->opcode() != Jump || block->numPredecessors() != 1 || block == loop.header())
                return false;
            for (Value* value : *block) {
                if (emitsInstructions(value))
                    return false;
            }
            block = block->predecessor(0);
        }
        m_proc.didReachLimit(Limit::MaxRareBlocksPerRotatedLoop, latch->at(0)->origin());
        return false;
    }

    static bool hasLikelyEdgeTo(BasicBlock* from, BasicBlock* to)
    {
        for (const FrequentedBlock& successor : from->successors()) {
            if (successor.block() == to && isLikelySuccessor(from, successor))
                return true;
        }
        return false;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        Rotation rotation;
        IndexSet<BasicBlock*> likelyBlocks = findLikelyBlocks(loops, loop);
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (likelyBlocks.contains(predecessor) && hasLikelyEdgeTo(predecessor, loop.header()))
                rotation.latches.append(predecessor);
            else if (loops.belongsTo(predecessor, loop))
                rotation.keepsBackEdgeToGuard = true;
        }
        if (rotation.latches.isEmpty())
            return;
        if (m_allowedHeaders) {
            for (BasicBlock* latch : rotation.latches) {
                if (isBottomTested(loops, loop, latch))
                    return;
            }
        }
        for (const NaturalLoop* outer = loops.innerMostOuterLoop(loop); outer; outer = loops.innerMostOuterLoop(*outer))
            ++rotation.depth;

        unsigned numberOfCopiedValues = 0;
        for (BasicBlock* block = loop.header();;) {
            if (block != loop.header() && (block->numPredecessors() != 1 || loops.headerOf(block)))
                return;
            if (loops.innerMostLoopOf(block) != &loop)
                return;
            for (Value* value : *block) {
                if (!canCloneForLoop(value))
                    return;
                numberOfCopiedValues += emitsInstructions(value);
            }
            if (numberOfCopiedValues > m_proc.limit(Limit::MaxCopiedValuesPerRotatedLoop)) {
                m_proc.didReachLimit(Limit::MaxCopiedValuesPerRotatedLoop, loop.header()->at(0)->origin());
                return;
            }
            rotation.testBlocks.append(block);

            BasicBlock* next = nullptr;
            if (block->last()->opcode() == Jump)
                next = block->successorBlock(0);
            else if (block->last()->opcode() == Branch) {
                for (unsigned i = 0; i < 2; ++i) {
                    const FrequentedBlock& staying = block->successor(i);
                    const FrequentedBlock& other = block->successor(1 - i);
                    if (!loops.belongsTo(staying.block(), loop) || staying.isRare())
                        continue;
                    bool otherIsRare = other.isRare();
                    bool otherLeaves = !loops.belongsTo(other.block(), loop);
                    if (otherLeaves && otherIsRare) {
                        next = staying.block();
                        break;
                    }
                    if (!otherLeaves && !(otherIsRare && leadsBackToHeaderOnly(loops, loop, other.block(), staying.block())))
                        continue;
                    if (staying.block() == loop.header() || loops.headerOf(staying.block()) || rotation.latches.contains(block))
                        return;
                    rotation.body = staying.block();
                    m_rotations.append(WTF::move(rotation));
                    return;
                }
            }
            if (!next || next == loop.header() || !loops.belongsTo(next, loop))
                return;
            block = next;
        }
    }

    void rotate(RegionCloner& cloner, const Rotation& rotation)
    {
        RegionCloner::BlockMap copies = cloner.cloneBlocks(rotation.testBlocks.span());
        for (BasicBlock* latch : rotation.latches)
            latch->replaceSuccessor(rotation.testBlocks[0], copies.get(rotation.testBlocks[0]));
        if (!rotation.keepsBackEdgeToGuard)
            removePolls(rotation.testBlocks.span());

        BasicBlock* preHeader = m_proc.addBlock();
        preHeader->appendNew<Value>(m_proc, Jump, rotation.body->at(0)->origin());
        preHeader->setSuccessors(FrequentedBlock(rotation.body));
        rotation.testBlocks.last()->replaceSuccessor(rotation.body, preHeader);

    }

    static void removePolls(std::span<BasicBlock* const> blocks)
    {
        for (BasicBlock* block : blocks) {
            for (Value* value : *block) {
                if (isLoopPoll(value))
                    value->replaceWithNop();
            }
        }
    }

    Procedure& m_proc;
    const Vector<BasicBlock*>* m_allowedHeaders;
    Vector<Rotation> m_rotations;
};

} // anonymous namespace

bool rotateLoops(Procedure& proc, const Vector<BasicBlock*>* allowedHeaders)
{
    PhaseScope phaseScope(proc, "rotateLoops"_s);
    RotateLoops rotateLoops(proc, allowedHeaders);
    return rotateLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
