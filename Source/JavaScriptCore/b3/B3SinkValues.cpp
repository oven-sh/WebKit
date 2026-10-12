/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3SinkValues.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

class SinkValues {
public:
    SinkValues(Procedure& proc)
        : m_proc(proc)
        , m_dominators(proc.dominators())
        , m_uses(proc.values().size())
    {
    }

    bool run()
    {
        m_preOrder = m_proc.blocksInPreOrder();
        for (unsigned i = m_preOrder.size(); i--;) {
            BasicBlock* block = m_preOrder[i];
            for (unsigned index = block->size(); index--;) {
                Value* value = block->at(index);
                Place place { block, index };
                const Uses& uses = m_uses[value];
                if (uses.first.block && uses.first.block != block && canSink(value))
                    place = sink(value, place, uses);
                for (Value* child : value->children())
                    addUse(child, place);
            }
        }
        if (m_sunk.isEmpty())
            return false;

        m_sunk.reverse();
        std::ranges::stable_sort(m_sunk, [](const Sunk& a, const Sunk& b) {
            if (a.place.block != b.place.block)
                return a.place.block->index() < b.place.block->index();
            return a.place.index < b.place.index;
        });
        unsigned next = 0;
        for (BasicBlock* block : m_proc) {
            if (!m_changedBlocks.contains(block))
                continue;
            BasicBlock::ValueList values;
            values.reserveInitialCapacity(block->size());
            for (unsigned index = 0; index < block->size(); ++index) {
                while (next < m_sunk.size() && m_sunk[next].place.block == block && m_sunk[next].place.index == index)
                    values.append(m_sunk[next++].value);
                if (Value* value = block->at(index))
                    values.append(value);
            }
            block->values() = WTF::move(values);
        }
        RELEASE_ASSERT(next == m_sunk.size());

        if (m_clones.size()) {
            for (BasicBlock* block : m_preOrder) {
                BasicBlock* parent = m_dominators.idom(block);
                if (!m_blocksBelowClones.contains(block) && !(parent && m_blocksBelowClones.contains(parent)))
                    continue;
                m_blocksBelowClones.add(block);
                for (Value* value : *block) {
                    for (Value*& child : value->children()) {
                        if (child->index() >= m_clones.size())
                            continue;
                        const Clone& clone = m_clones[child];
                        if (clone.value && clone.value != value && m_dominators.dominates(clone.block, block))
                            child = clone.value;
                    }
                }
            }
        }
        return true;
    }

private:
    static constexpr bool makesClones = true;
    static constexpr unsigned maxLoopsLeft = 8;

    struct Place {
        BasicBlock* block { nullptr };
        unsigned index { 0 };
    };

    struct Uses {
        Place first;
        Place second;
    };

    struct Sunk {
        Place place;
        Value* value;
    };

    struct Clone {
        Value* value { nullptr };
        BasicBlock* block { nullptr };
    };

    static Place aheadOfTerminal(BasicBlock* block) { return { block, static_cast<unsigned>(block->size() - 1) }; }

    BasicBlock* nearestCommonDominator(BasicBlock* a, BasicBlock* b)
    {
        if (m_dominators.dominates(a, b))
            return a;
        if (!m_jump.size()) {
            m_jump.resize(m_proc.size());
            IndexMap<BasicBlock*, unsigned> depth(m_proc.size(), 0u);
            for (BasicBlock* block : m_preOrder) {
                BasicBlock* parent = m_dominators.idom(block);
                if (!parent) {
                    m_jump[block] = block;
                    continue;
                }
                depth[block] = depth[parent] + 1;
                BasicBlock* jump = m_jump[parent];
                m_jump[block] = depth[parent] - depth[jump] == depth[jump] - depth[m_jump[jump]] ? m_jump[jump] : parent;
            }
        }
        do {
            BasicBlock* jump = m_jump[a];
            a = m_dominators.dominates(jump, b) ? m_dominators.idom(a) : jump;
        } while (!m_dominators.dominates(a, b));
        return a;
    }

    bool covers(Place& kept, Place place)
    {
        if (kept.block == place.block) {
            kept.index = std::min(kept.index, place.index);
            return true;
        }
        return m_dominators.strictlyDominates(kept.block, place.block);
    }

    void addUse(Value* child, Place place)
    {
        Uses& uses = m_uses[child];
        if (!uses.first.block) {
            uses.first = place;
            return;
        }
        if (covers(uses.first, place) || (uses.second.block && covers(uses.second, place)))
            return;
        bool isAboveSecond = uses.second.block && m_dominators.strictlyDominates(place.block, uses.second.block);
        if (m_dominators.strictlyDominates(place.block, uses.first.block)) {
            uses.first = place;
            if (isAboveSecond)
                uses.second = { };
            return;
        }
        if (isAboveSecond || (makesClones && !uses.second.block)) {
            uses.second = place;
            return;
        }
        BasicBlock* common = nearestCommonDominator(uses.first.block, place.block);
        if (uses.second.block)
            common = nearestCommonDominator(common, uses.second.block);
        uses.first = aheadOfTerminal(common);
        uses.second = { };
    }

    bool canSink(Value* value)
    {
        switch (value->opcode()) {
        case Nop:
        case Phi:
        case Upsilon:
        case ArgumentReg:
        case FramePointer:
        case Extract:
        case Opaque:
            return false;
        default:
            break;
        }
        if (value->isConstant() || value->type() == Void || value->type().isTuple())
            return false;
        Effects effects = value->effects();
        return !effects.terminal && !effects.exitsSideways && !effects.writesLocalState && !effects.readsLocalState && !effects.readsPinned && !effects.writesPinned && !effects.fence && !effects.writes && !effects.reads;
    }

    bool leaveLoops(BasicBlock* from, Place& place)
    {
        auto& loops = m_proc.naturalLoops();
        for (unsigned left = 0; left <= maxLoopsLeft; ++left) {
            const NaturalLoop* outermost = nullptr;
            for (auto* loop = loops.innerMostLoopOf(place.block); loop && !loops.belongsTo(from, *loop); loop = loops.innerMostOuterLoop(*loop))
                outermost = loop;
            if (!outermost)
                return true;
            place = aheadOfTerminal(m_dominators.idom(outermost->header()));
        }
        return false;
    }

    bool isOrdered(BasicBlock* block)
    {
        if (m_isOrdered.size())
            return m_isOrdered[block];
        m_isOrdered.resize(m_proc.size());
        IndexMap<BasicBlock*, unsigned> pending(m_proc.size(), 0u);
        auto forEachForwardEdge = [&](BasicBlock* from, const auto& functor) {
            for (BasicBlock* successor : from->successorBlocks()) {
                if (!m_dominators.dominates(successor, from))
                    functor(successor);
            }
        };
        for (BasicBlock* from : m_proc) {
            forEachForwardEdge(from, [&](BasicBlock* successor) {
                pending[successor]++;
            });
        }
        Vector<BasicBlock*, 16> worklist { m_proc[0] };
        while (!worklist.isEmpty()) {
            BasicBlock* from = worklist.takeLast();
            m_isOrdered[from] = true;
            forEachForwardEdge(from, [&](BasicBlock* successor) {
                if (!--pending[successor])
                    worklist.append(successor);
            });
        }
        return m_isOrdered[block];
    }

    static bool isAtMostHalf(double frequency, BasicBlock* of) { return frequency * 2 <= of->frequency(); }

    void put(Value* value, Place place)
    {
        m_sunk.append(Sunk { place, value });
        m_changedBlocks.add(place.block);
    }

    Place move(Value* value, Place from, Place to)
    {
        from.block->at(from.index) = nullptr;
        m_changedBlocks.add(from.block);
        put(value, to);
        return to;
    }

    Place sink(Value* value, Place from, Uses uses)
    {
        if (uses.second.block) {
            Place first = uses.first;
            Place second = uses.second;
            if (value->opcode() != Patchpoint && value->opcode() != CCall
                && leaveLoops(from.block, first) && leaveLoops(from.block, second)
                && !m_dominators.dominates(first.block, second.block) && !m_dominators.dominates(second.block, first.block)
                && isAtMostHalf(first.block->frequency() + second.block->frequency(), from.block)
                && isOrdered(first.block) && isOrdered(second.block)) {
                if (!m_clones.size())
                    m_clones.resize(m_uses.size());
                Value* clone = m_proc.clone(value);
                m_clones[value] = { clone, second.block };
                m_blocksBelowClones.add(second.block);
                put(clone, second);
                for (Value* child : value->children())
                    addUse(child, second);
                return move(value, from, first);
            }
            uses.first = aheadOfTerminal(nearestCommonDominator(uses.first.block, uses.second.block));
        }
        Place place = uses.first;
        if (place.block == from.block || !leaveLoops(from.block, place) || !isAtMostHalf(place.block->frequency(), from.block) || !isOrdered(place.block))
            return from;
        return move(value, from, place);
    }

    Procedure& m_proc;
    Dominators& m_dominators;
    Vector<BasicBlock*> m_preOrder;
    IndexMap<Value*, Uses> m_uses;
    IndexMap<Value*, Clone> m_clones;
    IndexMap<BasicBlock*, BasicBlock*> m_jump;
    IndexMap<BasicBlock*, bool> m_isOrdered;
    IndexSet<BasicBlock*> m_changedBlocks;
    IndexSet<BasicBlock*> m_blocksBelowClones;
    Vector<Sunk> m_sunk;
};

} // anonymous namespace

bool sinkValues(Procedure& proc)
{
    PhaseScope phaseScope(proc, "sinkValues"_s);
    if (proc.size() < 2)
        return false;
    SinkValues sinkValues(proc);
    return sinkValues.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
