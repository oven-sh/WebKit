/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3SinkValues.h"

#if ENABLE(B3_JIT)

#include "AirArg.h"
#include "B3BasicBlockInlines.h"
#include "B3CycleForest.h"
#include "B3DominatorJumpPointers.h"
#include "B3Dominators.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3MemoryValueInlines.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
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
        , m_preOrder(proc.cachedBlocksInPreOrder())
        , m_uses(proc.values().size())
    {
    }

    bool run()
    {
        // No edge changes.
        MovedLoadLog::OriginalsKeepTheirMeaning originalsKeepTheirMeaning(m_proc.movedLoadLog());
        for (unsigned i = m_preOrder.size(); i--;) {
            BasicBlock* block = m_preOrder[i];
            Value* comparisonFusedIntoBranch = nullptr;
            for (unsigned index = block->size(); index--;) {
                Value* value = block->at(index);
                Position position { block, index };
                bool isPartOfAddresses = takeUsesAsPartOfAddresses(value, block);
                const Uses& uses = m_uses[value];
                bool isOnlyPartOfAddresses = isPartOfAddresses && !uses.first.block;
                if (uses.first.block && uses.first.block != block && canSink(value)) {
                    if (value == comparisonFusedIntoBranch)
                        cloneForSlowPath(value, position, uses);
                    else if (isPartOfAddresses)
                        isOnlyPartOfAddresses = cloneForSlowPaths(value, position, uses);
                    else
                        position = sink(value, position, uses);
                } else if (!isPartOfAddresses && uses.first.block == block && uses.first.index > index + 1 && isInductionStep(value))
                    position = move(value, position, uses.first);
                else if (uses.first.block == block && m_stepsOfWalks.contains(value)) {
                    if (unsigned lowest = lowestIndexFor(position, uses.first.index); lowest > index + 1)
                        position = move(value, position, { block, lowest });
                }
                if (auto* upsilon = value->as<UpsilonValue>(); upsilon && isLoad(upsilon->child(0)->opcode()) && upsilon->child(0)->lastChild() == upsilon->phi())
                    m_stepsOfWalks.add(upsilon->child(0));
                if (index + 1 == block->size())
                    comparisonFusedIntoBranch = findComparisonFusedIntoBranch(block);
                std::optional<unsigned> partOfAddress = isOnlyPartOfAddresses ? scaledIndexOf(value) : addressFusedInto(value);
                for (unsigned childIndex = 0; childIndex < value->numChildren(); ++childIndex) {
                    Value* child = value->child(childIndex);
                    if (!hasSinkableKind(child) || (child == comparisonFusedIntoBranch && index + 1 == block->size()))
                        continue;
                    if (childIndex == partOfAddress)
                        addUseAsPartOfAddress(child, position);
                    else
                        addUse(child, position);
                }
            }
        }
        if (m_sunk.isEmpty())
            return false;

        m_sunk.reverse();
        std::ranges::stable_sort(m_sunk, [](const SunkValue& a, const SunkValue& b) {
            if (a.position.block != b.position.block)
                return a.position.block->index() < b.position.block->index();
            return a.position.index < b.position.index;
        });
        unsigned next = 0;
        for (BasicBlock* block : m_proc) {
            if (!m_changedBlocks.contains(block))
                continue;
            BasicBlock::ValueList values;
            values.reserveInitialCapacity(block->size());
            for (unsigned index = 0; index < block->size(); ++index) {
                while (next < m_sunk.size() && m_sunk[next].position.block == block && m_sunk[next].position.index == index)
                    values.append(m_sunk[next++].value);
                if (Value* value = block->at(index); !m_moved.contains(value))
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
                m_clonesSeenInBlock.clear();
                for (Value* value : *block) {
                    for (Value*& child : value->children()) {
                        if (child->index() >= m_clones.size())
                            continue;
                        for (const Clone& clone : { m_clones[child], secondCloneOf(child) }) {
                            if (!clone.value || clone.value == value || !m_dominators.dominates(clone.block, block))
                                continue;
                            if (clone.block == block && !m_clonesSeenInBlock.contains(clone.value))
                                continue;
                            child = clone.value;
                            break;
                        }
                    }
                    if (m_cloneValues.contains(value))
                        m_clonesSeenInBlock.append(value);
                }
            }
        }
        return true;
    }

private:
    struct Position {
        BasicBlock* block { nullptr };
        unsigned index { 0 };
    };

    struct Uses {
        Position first;
        Position second;
    };

    struct SunkValue {
        Position position;
        Value* value;
    };

    struct Clone {
        Value* value { nullptr };
        BasicBlock* block { nullptr };
    };

    struct PartOfAddresses {
        Uses uses;
        unsigned numberOfUses { 0 };
    };

    static Position beforeTerminal(BasicBlock* block) { return { block, static_cast<unsigned>(block->size() - 1) }; }

    BasicBlock* nearestCommonDominator(BasicBlock* a, BasicBlock* b)
    {
        if (m_dominators.dominates(a, b))
            return a;
        if (!m_jumpPointers)
            m_jumpPointers.emplace(m_proc);
        return m_jumpPointers->nearestCommonDominator(a, b);
    }

    bool covers(Position& existing, Position position)
    {
        if (existing.block == position.block) {
            existing.index = std::min(existing.index, position.index);
            return true;
        }
        return m_dominators.strictlyDominates(existing.block, position.block);
    }

    void addUse(Value* child, Position position)
    {
        addUse(m_uses[child], position);
    }

    void addUse(Uses& uses, Position position)
    {
        if (!uses.first.block) {
            uses.first = position;
            return;
        }
        if (covers(uses.first, position) || (uses.second.block && covers(uses.second, position)))
            return;
        bool isAboveSecond = uses.second.block && m_dominators.strictlyDominates(position.block, uses.second.block);
        if (m_dominators.strictlyDominates(position.block, uses.first.block)) {
            uses.first = position;
            if (isAboveSecond)
                uses.second = { };
            return;
        }
        if (isAboveSecond || !uses.second.block) {
            uses.second = position;
            return;
        }
        BasicBlock* common = nearestCommonDominator(uses.first.block, position.block);
        common = nearestCommonDominator(common, uses.second.block);
        uses.first = beforeTerminal(common);
        uses.second = { };
    }

    static bool hasSinkableKind(Value* value)
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
        return !value->isConstant() && value->type() != Void && !value->type().isTuple();
    }

    bool canSink(Value* value)
    {
        if (!hasSinkableKind(value))
            return false;
        Effects effects = value->effects();
        return !effects.terminal && !effects.exitsSideways && !effects.writesLocalState && !effects.readsLocalState && !effects.readsPinned && !effects.writesPinned && !effects.fence && !effects.writes && !effects.reads;
    }

    static bool isInductionStep(Value* value)
    {
        if (value->opcode() == Add)
            return value->child(0)->opcode() == Phi || value->child(1)->opcode() == Phi;
        return value->opcode() == Sub && value->child(0)->opcode() == Phi;
    }

    // For link = link.next. Below the other reads through the link, the next link can take the register of this one.
    static unsigned lowestIndexFor(Position from, unsigned firstUse)
    {
        unsigned index = from.index + 1;
        for (; index < firstUse; ++index) {
            Effects effects = from.block->at(index)->effects();
            if (effects.terminal || effects.exitsSideways || effects.writesLocalState || effects.writesPinned || effects.fence || effects.writes)
                break;
        }
        return index;
    }

    Value* findComparisonFusedIntoBranch(BasicBlock* block)
    {
        if (block->last()->opcode() != Branch)
            return nullptr;
        Value* condition = block->last()->child(0);
        if (!isIntegerComparison(condition->opcode()) || !condition->child(0)->type().isInt())
            return nullptr;
        return block->values().contains(condition) ? condition : nullptr;
    }

    bool cloneForSlowPath(Value* value, Position from, Uses uses)
    {
        if (uses.second.block)
            uses.first = beforeTerminal(nearestCommonDominator(uses.first.block, uses.second.block));
        Position position = uses.first;
        if (position.block == from.block || !leaveLoops(value, from.block, position) || position.block == from.block || !isAtMostHalf(position.block->frequency(), from.block) || !isOrdered(from.block, position.block))
            return false;
        if (!m_clones.size())
            m_clones.resize(m_uses.size());
        m_clones[value] = insertCloneAt(value, position);
        return true;
    }

    Clone insertCloneAt(Value* value, Position position)
    {
        Value* clone = m_proc.clone(value);
        m_cloneValues.add(clone);
        m_blocksBelowClones.add(position.block);
        insertAt(clone, position);
        for (Value* child : value->children())
            addUse(child, position);
        return { clone, position.block };
    }

    Clone secondCloneOf(Value* value) const
    {
        if (m_secondClones.isEmpty())
            return { };
        return m_secondClones.get(value);
    }

    // The value stays where it is for the accesses that take it into their addresses, and costs nothing there.
    bool cloneForSlowPaths(Value* value, Position from, const Uses& uses)
    {
        if (!uses.second.block)
            return cloneForSlowPath(value, from, uses);
        Position first = uses.first;
        Position second = uses.second;
        bool canCloneTwice = first.block != from.block && second.block != from.block
            && leaveLoops(value, from.block, first) && leaveLoops(value, from.block, second)
            && first.block != from.block && second.block != from.block
            && !m_dominators.dominates(first.block, second.block) && !m_dominators.dominates(second.block, first.block)
            && isAtMostHalf(first.block->frequency() + second.block->frequency(), from.block)
            && isOrdered(from.block, first.block) && isOrdered(from.block, second.block);
        if (!canCloneTwice)
            return cloneForSlowPath(value, from, uses);
        if (!m_clones.size())
            m_clones.resize(m_uses.size());
        m_clones[value] = insertCloneAt(value, first);
        m_secondClones.set(value, insertCloneAt(value, second));
        return true;
    }

    static std::optional<unsigned> scaleOf(Value* index)
    {
        if (index->opcode() != Shl || !index->child(1)->hasInt32())
            return std::nullopt;
        unsigned logScale = index->child(1)->asInt32() & (index->type() == Int32 ? 31 : 63);
        if (logScale > 3)
            return std::nullopt;
        return 1u << logScale;
    }

    // The operand that lowering makes the index of an address, in the order in which it looks.
    static std::optional<unsigned> scaledIndexOf(Value* address)
    {
        if (address->opcode() != Add)
            return std::nullopt;
        for (unsigned childIndex = 0; childIndex < 2; ++childIndex) {
            if (scaleOf(address->child(childIndex)) && address->child(0) != address->child(1))
                return childIndex;
        }
        return std::nullopt;
    }

    // The operand that is a sum which lowering takes into the address of the access, so that no instruction computes it.
    static std::optional<unsigned> addressFusedInto(Value* value)
    {
        MemoryValue* access = value->as<MemoryValue>();
        if (!access || access->isExotic() || access->requiresSimpleAddr())
            return std::nullopt;
        Value* address = access->lastChild();
        if (address->opcode() != Add || address->type() != pointerType())
            return std::nullopt;
        std::optional<unsigned> index = scaledIndexOf(address);
        // Whoever handles the fault reads the reference out of a register of the access: out of the index where the base is a pinned register, else out of the base.
        if (access->traps() && (!index || address->child(1 - *index)->opcode() != ArgumentReg))
            return std::nullopt;
        if (!Air::Arg::isValidIndexForm(index ? *scaleOf(address->child(*index)) : 1, access->offset(), access->accessWidth()))
            return std::nullopt;
        return access->numChildren() - 1;
    }

    void addUseAsPartOfAddress(Value* child, Position position)
    {
        PartOfAddresses& part = m_partsOfAddresses.add(child, PartOfAddresses()).iterator->value;
        addUse(part.uses, position);
        ++part.numberOfUses;
    }

    // Lowering computes a sum that has many users once. One whose accesses are all rare goes to them with everything it is made of.
    bool takeUsesAsPartOfAddresses(Value* value, BasicBlock* block)
    {
        if (m_partsOfAddresses.isEmpty())
            return false;
        std::optional<PartOfAddresses> part = m_partsOfAddresses.takeOptional(value);
        if (!part)
            return false;
        constexpr unsigned mostUsesTakenIntoAddresses = 8;
        auto isFrequent = [&](const Position& position) {
            return position.block && !isAtMostHalf(position.block->frequency(), block);
        };
        if (part->numberOfUses <= mostUsesTakenIntoAddresses && canSink(value) && (isFrequent(part->uses.first) || isFrequent(part->uses.second)))
            return true;
        addUse(value, part->uses.first);
        if (part->uses.second.block)
            addUse(value, part->uses.second);
        return false;
    }

    bool leaveLoops(const Value* value, BasicBlock* from, Position& position)
    {
        auto& loops = m_proc.naturalLoops();
        for (unsigned numberOfExitedLoops = 0; numberOfExitedLoops <= m_proc.limit(Limit::MaxLoopsLeftBySinking); ++numberOfExitedLoops) {
            const NaturalLoop* outermost = nullptr;
            for (auto* loop = loops.innerMostLoopOf(position.block); loop && !loops.belongsTo(from, *loop); loop = loops.innerMostOuterLoop(*loop))
                outermost = loop;
            if (!outermost)
                return true;
            position = beforeTerminal(m_dominators.idom(outermost->header()));
        }
        m_proc.didReachLimit(Limit::MaxLoopsLeftBySinking, value->origin());
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
        bool isReducible = true;
        for (BasicBlock* candidate : m_proc)
            isReducible &= m_isOrdered[candidate];
        if (!isReducible) {
            m_cycles.emplace(m_proc);
            CycleForest& cycles = *m_cycles;
            for (BasicBlock* candidate : m_proc) {
                bool isOnIrreducibleCycle = false;
                for (unsigned cycle = cycles.innermost(candidate); cycle != CycleForest::none; cycle = cycles[cycle].parent)
                    isOnIrreducibleCycle |= !cycles[cycle].isReducible();
                m_isOrdered[candidate] = !isOnIrreducibleCycle;
            }
        }
        return m_isOrdered[block];
    }

    bool isOrdered(BasicBlock* from, BasicBlock* block)
    {
        if (isOrdered(block))
            return true;
        for (unsigned cycle = m_cycles->innermost(block); cycle != CycleForest::none; cycle = (*m_cycles)[cycle].parent) {
            if (!(*m_cycles)[cycle].isReducible() && !m_cycles->contains(cycle, from))
                return false;
        }
        return true;
    }

    static bool isAtMostHalf(double frequency, BasicBlock* block) { return frequency * 2 <= block->frequency(); }

    void insertAt(Value* value, Position position)
    {
        m_sunk.append(SunkValue { position, value });
        m_changedBlocks.add(position.block);
    }

    Position move(Value* value, Position from, Position to)
    {
        m_moved.add(value);
        m_changedBlocks.add(from.block);
        insertAt(value, to);
        return to;
    }

    Position sink(Value* value, Position from, Uses uses)
    {
        if (uses.second.block) {
            Position first = uses.first;
            Position second = uses.second;
            if ((value->opcode() != Patchpoint || value->as<PatchpointValue>()->pureFunction.has_value()) && value->opcode() != CCall && !value->kind().isCloningForbidden()
                && leaveLoops(value, from.block, first) && leaveLoops(value, from.block, second)
                && !m_dominators.dominates(first.block, second.block) && !m_dominators.dominates(second.block, first.block)
                && isAtMostHalf(first.block->frequency() + second.block->frequency(), from.block)
                && isOrdered(from.block, first.block) && isOrdered(from.block, second.block)) {
                if (!m_clones.size())
                    m_clones.resize(m_uses.size());
                m_clones[value] = insertCloneAt(value, second);
                return move(value, from, first);
            }
            uses.first = beforeTerminal(nearestCommonDominator(uses.first.block, uses.second.block));
        }
        Position position = uses.first;
        if (position.block == from.block || !leaveLoops(value, from.block, position) || !isAtMostHalf(position.block->frequency(), from.block) || !isOrdered(from.block, position.block))
            return from;
        return move(value, from, position);
    }

    Procedure& m_proc;
    Dominators& m_dominators;
    const Vector<BasicBlock*>& m_preOrder;
    IndexMap<Value*, Uses> m_uses;
    IndexMap<Value*, Clone> m_clones;
    UncheckedKeyHashMap<Value*, Clone> m_secondClones;
    UncheckedKeyHashMap<Value*, PartOfAddresses> m_partsOfAddresses;
    UncheckedKeyHashSet<Value*> m_cloneValues;
    Vector<Value*, 8> m_clonesSeenInBlock;
    std::optional<DominatorJumpPointers> m_jumpPointers;
    IndexMap<BasicBlock*, bool> m_isOrdered;
    std::optional<CycleForest> m_cycles;
    IndexSet<Value*> m_moved;
    IndexSet<Value*> m_stepsOfWalks;
    IndexSet<BasicBlock*> m_changedBlocks;
    IndexSet<BasicBlock*> m_blocksBelowClones;
    Vector<SunkValue> m_sunk;
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

#endif // ENABLE(B3_JIT)
