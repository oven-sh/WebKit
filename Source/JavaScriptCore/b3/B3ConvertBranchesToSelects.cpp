/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3ConvertBranchesToSelects.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3Limits.h"
#include "B3MemoryValue.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

class ConvertBranchesToSelects {
public:
    ConvertBranchesToSelects(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        NaturalLoops& loops = m_proc.naturalLoops();
        if (!loops.numLoops())
            return false;
        Vector<BasicBlock*, 8> heads;
        IndexSet<Value*> branchesOnLoadedValues;
        bool didResetValueOwners = false;
        for (BasicBlock* block : m_proc) {
            if (block->last()->opcode() != Branch || block->taken().isRare() || block->notTaken().isRare())
                continue;
            if (block->taken().block()->numPredecessors() != 1 && block->notTaken().block()->numPredecessors() != 1)
                continue;
            const NaturalLoop* loop = loops.innerMostLoopOf(block);
            if (!loop)
                continue;
            if (!std::exchange(didResetValueOwners, true))
                m_proc.resetValueOwners();
            if (dependsOnValueLoadedInLoop(loops, *loop, block->last()->child(0))) {
                heads.append(block);
                branchesOnLoadedValues.add(block->last());
            }
        }
        bool changed = false;
        for (BasicBlock* head : heads) {
            while (!m_removedBlocks.contains(head) && branchesOnLoadedValues.contains(head->last())) {
                Region region;
                unsigned cost = 0;
                if (!analyze(head, 0, cost, region))
                    break;
                convert(head, region);
                changed = true;
            }
        }
        if (!changed)
            return false;
        for (Value* value : m_proc.values())
            value->performSubstitution();
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    struct Region {
        BasicBlock* join { nullptr };
        std::array<BasicBlock*, 2> arms { };
        std::array<std::unique_ptr<Region>, 2> nested;
    };

    bool dependsOnValueLoadedInLoop(NaturalLoops& loops, const NaturalLoop& loop, Value* condition)
    {
        Vector<Value*, 16> worklist;
        IndexSet<Value*> seen;
        worklist.append(condition);
        while (!worklist.isEmpty() && seen.size() < m_proc.limit(Limit::MaxValuesSearchedForDominatingLoad)) {
            Value* value = worklist.takeLast();
            if (!seen.add(value) || value->opcode() == Phi || !loops.belongsTo(value->owner, loop))
                continue;
            if (isLoad(value->opcode()))
                return true;
            for (Value* child : value->children())
                worklist.append(child);
        }
        if (!worklist.isEmpty())
            m_proc.didReachLimit(Limit::MaxValuesSearchedForDominatingLoad, condition->origin());
        return false;
    }

    bool isSpeculated(Value* value, unsigned& cost)
    {
        switch (value->opcode()) {
        case Nop:
        case Identity:
        case Const32:
        case Const64:
            return true;
        case Add:
        case Sub:
        case Neg:
        case BitAnd:
        case BitOr:
        case BitXor:
        case Shl:
        case SShr:
        case ZShr:
        case SExt8:
        case SExt16:
        case SExt32:
        case ZExt32:
        case Trunc:
        case Equal:
        case NotEqual:
        case LessThan:
        case GreaterThan:
        case LessEqual:
        case GreaterEqual:
        case Above:
        case Below:
        case AboveEqual:
        case BelowEqual:
        case Select:
            break;
        default:
            return false;
        }
        if (!value->type().isInt())
            return false;
        for (Value* child : value->children()) {
            if (!child->type().isInt())
                return false;
        }
        if (++cost <= m_proc.limit(Limit::MaxSpeculatedValuesPerSelect))
            return true;
        m_proc.didReachLimit(Limit::MaxSpeculatedValuesPerSelect, value->origin());
        return false;
    }

    bool containsOnlySpeculatedValues(BasicBlock* block, BasicBlock* phiBlock, BasicBlock* otherPhiBlock, bool mayContainPhis, unsigned& cost)
    {
        for (unsigned i = 0; i + 1 < block->size(); ++i) {
            Value* value = block->at(i);
            if (auto* upsilon = value->as<UpsilonValue>()) {
                if (upsilon->phi()->owner != phiBlock && upsilon->phi()->owner != otherPhiBlock)
                    return false;
                continue;
            }
            if (value->opcode() == Phi) {
                if (!mayContainPhis)
                    return false;
                continue;
            }
            if (!isSpeculated(value, cost))
                return false;
        }
        return true;
    }

    static UpsilonValue* lastUpsilonFor(BasicBlock* block, Value* phi)
    {
        for (unsigned i = block->size(); i--;) {
            auto* upsilon = block->at(i)->as<UpsilonValue>();
            if (upsilon && upsilon->phi() == phi)
                return upsilon;
        }
        return nullptr;
    }

    bool analyze(BasicBlock* head, unsigned depth, unsigned& cost, Region& region)
    {
        if (head->last()->opcode() != Branch || head->taken().isRare() || head->notTaken().isRare() || head->taken().block() == head->notTaken().block())
            return false;
        std::array<BasicBlock*, 2> exits { };
        for (unsigned i = 0; i < 2; ++i) {
            BasicBlock* successor = head->successorBlock(i);
            if (successor == head)
                return false;
            if (successor->numPredecessors() != 1) {
                exits[i] = successor;
                continue;
            }
            region.arms[i] = successor;
            if (successor->last()->opcode() == Jump) {
                exits[i] = successor->successorBlock(0);
                continue;
            }
            if (successor->last()->opcode() != Branch)
                return false;
            if (depth >= m_proc.limit(Limit::MaxSelectNestingDepth)) {
                m_proc.didReachLimit(Limit::MaxSelectNestingDepth, successor->last()->origin());
                return false;
            }
            region.nested[i] = makeUniqueWithoutFastMallocCheck<Region>();
            if (!analyze(successor, depth + 1, cost, *region.nested[i]))
                return false;
            BasicBlock* innerJoin = region.nested[i]->join;
            if (innerJoin->last()->opcode() != Jump || innerJoin == head)
                return false;
            for (BasicBlock* predecessor : innerJoin->predecessors()) {
                if (predecessor != successor && predecessor != region.nested[i]->arms[0] && predecessor != region.nested[i]->arms[1])
                    return false;
            }
            exits[i] = innerJoin->successorBlock(0);
        }
        if ((!region.arms[0] && !region.arms[1]) || exits[0] != exits[1])
            return false;
        region.join = exits[0];
        if (region.join == region.arms[0] || region.join == region.arms[1])
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!region.arms[i])
                continue;
            BasicBlock* innerJoin = region.nested[i] ? region.nested[i]->join : nullptr;
            if (!containsOnlySpeculatedValues(region.arms[i], region.join, innerJoin, false, cost))
                return false;
            if (innerJoin && !containsOnlySpeculatedValues(innerJoin, region.join, nullptr, true, cost))
                return false;
        }
        unsigned numberOfSelects = 0;
        for (Value* phi : *region.join) {
            if (phi->opcode() != Phi)
                continue;
            bool isWritten = false;
            for (unsigned i = 0; i < 2; ++i) {
                bool isWrittenInArm = region.arms[i] && (lastUpsilonFor(region.arms[i], phi) || (region.nested[i] && lastUpsilonFor(region.nested[i]->join, phi)));
                isWritten |= isWrittenInArm;
                if (!isWrittenInArm && !lastUpsilonFor(head, phi))
                    return false;
            }
            if (!isWritten)
                continue;
            if (!phi->type().isInt())
                return false;
            if (++numberOfSelects > m_proc.limit(Limit::MaxSelectsPerJoin)) {
                m_proc.didReachLimit(Limit::MaxSelectsPerJoin, head->last()->origin());
                return false;
            }
        }
        return true;
    }

    void mergeWithOnlySuccessor(BasicBlock* block)
    {
        BasicBlock* successor = block->successorBlock(0);
        RELEASE_ASSERT(successor->numPredecessors() == 1 && successor->predecessor(0) == block);
        for (Value* phi : *successor) {
            if (phi->opcode() != Phi)
                continue;
            UpsilonValue* upsilon = lastUpsilonFor(block, phi);
            RELEASE_ASSERT(upsilon);
            phi->replaceWithIdentity(upsilon->child(0));
            for (Value* value : *block) {
                auto* other = value->as<UpsilonValue>();
                if (other && other->phi() == phi)
                    other->replaceWithNop();
            }
        }
        block->removeLast(m_proc);
        for (Value* value : *successor) {
            value->owner = block;
            block->values().append(value);
        }
        block->successors() = successor->successors();
        for (BasicBlock* next : block->successorBlocks())
            next->replacePredecessor(successor, block);
        successor->values().shrink(0);
        successor->clearSuccessors();
        successor->predecessors().shrink(0);
        successor->appendNew<Value>(m_proc, Oops, block->last()->origin());
        m_removedBlocks.add(successor);
    }

    void convert(BasicBlock* head, Region& region)
    {
        for (unsigned i = 0; i < 2; ++i) {
            if (!region.nested[i])
                continue;
            convert(region.arms[i], *region.nested[i]);
            RELEASE_ASSERT(m_removedBlocks.contains(region.nested[i]->join));
        }
        Value* branch = head->last();
        Value* condition = branch->child(0);
        Origin origin = branch->origin();
        struct Written {
            Value* phi;
            std::array<Value*, 2> values;
        };
        Vector<Written, 4> written;
        for (Value* phi : *region.join) {
            if (phi->opcode() != Phi)
                continue;
            Written entry { phi, { } };
            bool isWrittenInArm = false;
            for (unsigned i = 0; i < 2; ++i) {
                UpsilonValue* upsilon = region.arms[i] ? lastUpsilonFor(region.arms[i], phi) : nullptr;
                isWrittenInArm |= !!upsilon;
                if (!upsilon)
                    upsilon = lastUpsilonFor(head, phi);
                RELEASE_ASSERT(upsilon);
                entry.values[i] = upsilon->child(0);
            }
            if (isWrittenInArm)
                written.append(entry);
        }
        head->removeLast(m_proc);
        for (BasicBlock* arm : region.arms) {
            if (!arm)
                continue;
            Vector<Value*, 4> remainingValues;
            for (Value* value : *arm) {
                if (value->opcode() == Upsilon || value == arm->last()) {
                    remainingValues.append(value);
                    continue;
                }
                value->owner = head;
                head->values().append(value);
            }
            arm->values().shrink(0);
            arm->values().appendVector(remainingValues);
            region.join->removePredecessor(arm);
            arm->predecessors().shrink(0);
            m_removedBlocks.add(arm);
        }
        for (auto& entry : written) {
            for (Value* value : *head) {
                auto* upsilon = value->as<UpsilonValue>();
                if (upsilon && upsilon->phi() == entry.phi)
                    upsilon->replaceWithNop();
            }
            Value* chosen = entry.values[0] == entry.values[1] ? entry.values[0] : head->appendNew<Value>(m_proc, Select, origin, condition, entry.values[0], entry.values[1]);
            head->appendNew<UpsilonValue>(m_proc, origin, chosen, entry.phi);
        }
        head->appendNew<Value>(m_proc, Jump, origin);
        head->setSuccessors(FrequentedBlock(region.join));
        region.join->addPredecessor(head);
        if (region.join->numPredecessors() == 1)
            mergeWithOnlySuccessor(head);
    }

    Procedure& m_proc;
    IndexSet<BasicBlock*> m_removedBlocks;
};

} // anonymous namespace

bool convertBranchesToSelects(Procedure& proc)
{
    PhaseScope phaseScope(proc, "convertBranchesToSelects"_s);
    ConvertBranchesToSelects convertBranchesToSelects(proc);
    return convertBranchesToSelects.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
