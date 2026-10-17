/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3RotateLoops.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3FixSSA.h"
#include "B3LoopUtilities.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include <wtf/IndexSet.h>

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
        demoteValuesEscapingTestBlocks();
        for (auto& rotation : m_rotations)
            rotate(rotation);
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr unsigned maxNumberOfCopiedValues = 12;
    static constexpr unsigned maxNumberOfRareBlocks = 8;

    struct Rotation {
        unsigned depth { 0 };
        Vector<BasicBlock*, 4> testBlocks;
        Vector<BasicBlock*, 2> latches;
        BasicBlock* body { nullptr };
    };

    static bool leadsBackToHeaderOnly(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* start, BasicBlock* body)
    {
        Vector<BasicBlock*, maxNumberOfRareBlocks> region { start };
        for (unsigned i = 0; i < region.size(); ++i) {
            if (region[i] == body || loops.innerMostLoopOf(region[i]) != &loop)
                return false;
            for (BasicBlock* successor : region[i]->successorBlocks()) {
                if (successor == loop.header() || !loops.belongsTo(successor, loop) || region.contains(successor))
                    continue;
                if (region.size() == maxNumberOfRareBlocks)
                    return false;
                region.append(successor);
            }
        }
        return true;
    }

    static bool isBottomTested(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* latch)
    {
        BasicBlock* block = latch;
        for (unsigned i = 0; i < maxNumberOfRareBlocks; ++i) {
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
        return false;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        Rotation rotation;
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (loops.belongsTo(predecessor, loop) && predecessor->frequency() * 8 >= loop.header()->frequency())
                rotation.latches.append(predecessor);
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
            if (numberOfCopiedValues > maxNumberOfCopiedValues)
                return;
            rotation.testBlocks.append(block);

            BasicBlock* next = nullptr;
            if (block->last()->opcode() == Jump)
                next = block->successorBlock(0);
            else if (block->last()->opcode() == Branch) {
                for (unsigned i = 0; i < 2; ++i) {
                    const FrequentedBlock& staying = block->successor(i);
                    const FrequentedBlock& other = block->successor(1 - i);
                    if (!loops.belongsTo(staying.block(), loop) || isRare(staying, block))
                        continue;
                    bool otherIsRare = isRare(other, block);
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
                    for (BasicBlock* testBlock : rotation.testBlocks)
                        m_testBlocks.add(testBlock);
                    m_rotations.append(WTF::move(rotation));
                    return;
                }
            }
            if (!next || next == loop.header() || !loops.belongsTo(next, loop))
                return;
            block = next;
        }
    }

    void demoteValuesEscapingTestBlocks()
    {
        m_proc.resetValueOwners();
        IndexSet<Value*> valuesToDemote;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() == Phi && m_testBlocks.contains(block))
                    valuesToDemote.add(value);
                for (Value* child : value->children()) {
                    if (child->owner != block && m_testBlocks.contains(child->owner))
                        valuesToDemote.add(child);
                }
            }
        }
        demoteValues(m_proc, valuesToDemote);
    }

    void rotate(const Rotation& rotation)
    {
        double guardFrequency = 0;
        for (BasicBlock* predecessor : rotation.testBlocks[0]->predecessors()) {
            if (!rotation.latches.contains(predecessor))
                guardFrequency += predecessor->frequency();
        }
        Vector<BasicBlock*, 4> copies;
        for (BasicBlock* block : rotation.testBlocks)
            copies.append(m_proc.addBlock(block->frequency()));
        for (unsigned i = 0; i < rotation.testBlocks.size(); ++i) {
            BasicBlock* block = rotation.testBlocks[i];
            BasicBlock* copy = copies[i];
            UncheckedKeyHashMap<Value*, Value*> map;
            for (Value* value : *block) {
                Value* clone = m_proc.clone(value);
                for (Value*& child : clone->children()) {
                    if (Value* replacement = map.get(child))
                        child = replacement;
                }
                if (value->type() != Void)
                    map.add(value, clone);
                copy->append(clone);
            }
            copy->successors() = block->successors();
            if (i + 1 < rotation.testBlocks.size())
                copy->replaceSuccessor(rotation.testBlocks[i + 1], copies[i + 1]);
            block->setFrequency(std::min(block->frequency(), guardFrequency));
        }
        for (BasicBlock* latch : rotation.latches)
            latch->replaceSuccessor(rotation.testBlocks[0], copies[0]);

        BasicBlock* preHeader = m_proc.addBlock(rotation.testBlocks.last()->frequency());
        preHeader->appendNew<Value>(m_proc, Jump, rotation.body->at(0)->origin());
        preHeader->setSuccessors(FrequentedBlock(rotation.body));
        rotation.testBlocks.last()->replaceSuccessor(rotation.body, preHeader);
    }

    Procedure& m_proc;
    const Vector<BasicBlock*>* m_allowedHeaders;
    Vector<Rotation> m_rotations;
    IndexSet<BasicBlock*> m_testBlocks;
};

} // anonymous namespace

bool simplifyTerminals(Procedure& proc)
{
    PhaseScope phaseScope(proc, "simplifyTerminals"_s);
    bool changed = false;
    for (BasicBlock* block : proc) {
        Value* terminal = block->last();
        if (terminal->opcode() != Branch || !terminal->child(0)->hasInt())
            continue;
        terminal->replaceWithJump(block, terminal->child(0)->asInt() ? block->taken() : block->notTaken());
        changed = true;
    }
    if (changed) {
        proc.resetReachability();
        proc.invalidateCFG();
    }
    return changed;
}

bool rotateLoops(Procedure& proc, const Vector<BasicBlock*>* allowedHeaders)
{
    PhaseScope phaseScope(proc, "rotateLoops"_s);
    RotateLoops rotateLoops(proc, allowedHeaders);
    return rotateLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
