/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3SeparateIntegerPaths.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3FixSSA.h"
#include "B3LoopUtilities.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3ReduceStrength.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>

namespace JSC { namespace B3 {

namespace {

class SeparateIntegerPaths {
public:
    SeparateIntegerPaths(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        bool changed = false;
        for (unsigned round = 0; round < maxRounds && runOnce(); ++round)
            changed = true;
        return changed;
    }

private:
    static constexpr unsigned maxRounds = 2;
    static constexpr unsigned maxCopiedInstructions = 10;
    static constexpr unsigned maxBlocksInRegion = 4;
    static constexpr unsigned maxTestDepth = 4;
    static constexpr unsigned maxNumbers = 16;
    static constexpr double minRatioOfIntegerToOtherFrequency = 0.5;
    static constexpr double minJoinFrequency = 1.0 / 256;
    static constexpr double maxRatioOfJoinToUser = 16;

    struct Part {
        BasicBlock* block { nullptr };
        unsigned end { 0 };
    };

    struct Use {
        Value* user { nullptr };
        unsigned previous { 0 };
    };

    static constexpr unsigned noUse = UINT_MAX;

    struct Region {
        Vector<Part, 4> parts;
        Vector<BasicBlock*, 2> otherPredecessors;
        double otherShare { 0 };
    };

    static bool isInteger(Value* value, unsigned depth = 2)
    {
        if (value->opcode() == IToD)
            return true;
        if (depth && value->type() == Double && (value->opcode() == Add || value->opcode() == Sub))
            return isInteger(value->child(0), depth - 1) && isInteger(value->child(1), depth - 1);
        if (!value->hasDouble())
            return false;
        double number = value->asDouble();
        constexpr double limit = 9007199254740992.0;
        return number >= -limit && number <= limit && number == std::trunc(number) && !(!number && std::signbit(number));
    }

    static bool keepsIntegers(Value* value)
    {
        switch (value->opcode()) {
        case Add:
        case Sub:
        case Mul:
        case Abs:
        case Identity:
            return value->type() == Double;
        default:
            return false;
        }
    }

    bool mayBecomeInteger(Value* value) const
    {
        return isInteger(value) || m_numbers.contains(value) || (value->opcode() == Phi && value->type() == Double);
    }

    bool becomesIntegerOperation(Value* user) const
    {
        switch (user->opcode()) {
        case Patchpoint: {
            int32_t key = user->as<PatchpointValue>()->pureFunctionOfChildrenKey;
            return (key == toInt64TruncationKey || key == toInt32ConversionKey) && user->numChildren() == 1;
        }
        case Equal:
        case NotEqual:
        case LessThan:
        case GreaterThan:
        case LessEqual:
        case GreaterEqual:
        case EqualOrUnordered:
            return mayBecomeInteger(user->child(0)) && mayBecomeInteger(user->child(1));
        default:
            return false;
        }
    }

    bool usesNumber(Value* value) const
    {
        for (Value* child : value->children()) {
            if (m_numbers.contains(child))
                return true;
        }
        return false;
    }

    bool dependsOnNumber(Value* value, BasicBlock* block, unsigned depth) const
    {
        if (m_numbers.contains(value))
            return true;
        if (!depth || value->owner != block || value->opcode() == Phi)
            return false;
        for (Value* child : value->children()) {
            if (dependsOnNumber(child, block, depth - 1))
                return true;
        }
        return false;
    }

    template<typename Functor>
    void forEachUser(Value* value, const Functor& functor) const
    {
        for (unsigned use = m_lastUse[value->index()]; use != noUse; use = m_uses[use].previous)
            functor(m_uses[use].user);
    }

    void markLiveIn(BasicBlock* from, BasicBlock* definition)
    {
        m_worklist.shrink(0);
        m_worklist.append(from);
        while (!m_worklist.isEmpty()) {
            BasicBlock* block = m_worklist.takeLast();
            if (block == definition || !m_near.contains(block) || m_liveIn.contains(block))
                continue;
            m_liveIn.append(block);
            for (BasicBlock* predecessor : block->predecessors())
                m_worklist.append(predecessor);
        }
    }

    std::optional<Region> regionFor(Value* phi, const Vector<UpsilonValue*, 4>& upsilons)
    {
        BasicBlock* join = phi->owner;
        if (join->frequency() < minJoinFrequency || upsilons.size() != join->numPredecessors())
            return std::nullopt;
        Region region;
        double integerFrequency = 0;
        double otherFrequency = 0;
        unsigned numberOfConversions = 0;
        for (UpsilonValue* upsilon : upsilons) {
            BasicBlock* predecessor = upsilon->owner;
            if (predecessor->numSuccessors() != 1 || predecessor->successorBlock(0) != join || predecessor == join)
                return std::nullopt;
            if (isInteger(upsilon->child(0))) {
                integerFrequency += predecessor->frequency();
                numberOfConversions += !upsilon->child(0)->hasDouble();
            } else {
                otherFrequency += predecessor->frequency();
                region.otherPredecessors.append(predecessor);
            }
        }
        if (numberOfConversions != 1 || region.otherPredecessors.isEmpty() || upsilons.size() != region.otherPredecessors.size() + 1)
            return std::nullopt;
        if (!(integerFrequency > 0) || otherFrequency * minRatioOfIntegerToOtherFrequency > integerFrequency)
            return std::nullopt;
        region.otherShare = otherFrequency / (integerFrequency + otherFrequency);

        m_numbers.shrink(0);
        m_liveIn.shrink(0);
        m_near.shrink(0);
        m_near.append(join);
        for (unsigned i = 0; i < m_near.size() && m_near.size() < 2 * maxBlocksInRegion; ++i) {
            for (BasicBlock* successor : m_near[i]->successorBlocks()) {
                if (!m_near.contains(successor))
                    m_near.append(successor);
            }
        }
        m_numbers.append(phi);
        bool pays = false;
        for (unsigned i = 0; i < m_numbers.size() && m_numbers.size() < maxNumbers; ++i) {
            forEachUser(m_numbers[i], [&](Value* user) {
                if (keepsIntegers(user) && m_near.contains(user->owner) && !m_numbers.contains(user))
                    m_numbers.append(user);
            });
        }
        for (unsigned i = 0; i < m_numbers.size(); ++i) {
            Value* number = m_numbers[i];
            forEachUser(number, [&](Value* user) {
                if (!m_near.contains(user->owner))
                    return;
                markLiveIn(user->owner, number->owner);
                pays |= becomesIntegerOperation(user) && user->owner->frequency() * maxRatioOfJoinToUser >= join->frequency();
            });
        }
        if (!pays)
            return std::nullopt;

        Vector<BasicBlock*, 16> whole;
        Vector<BasicBlock*, 16> seen;
        unsigned copiedInstructions = 0;
        auto add = [&](BasicBlock* block) -> bool {
            seen.append(block);
            if (block->last()->type() != Void)
                return false;
            unsigned firstFixed = block->size();
            unsigned afterLastUse = 0;
            for (unsigned index = 0; index < block->size(); ++index) {
                Value* value = block->at(index);
                if (!canCloneForLoop(value) || value->opcode() == EntrySwitch) {
                    firstFixed = index;
                    break;
                }
                if (value->opcode() == Phi || m_numbers.contains(value) || usesNumber(value))
                    afterLastUse = index + 1;
            }
            unsigned end = afterLastUse;
            if (firstFixed == block->size()) {
                bool goesOn = block->last()->numChildren() && dependsOnNumber(block->last()->child(0), block, maxTestDepth);
                for (BasicBlock* successor : block->successorBlocks())
                    goesOn |= m_liveIn.contains(successor) && successor != join;
                if (goesOn || (end + 1 == block->size() && block->last()->opcode() == Jump))
                    end = block->size();
            }
            if (!end)
                return false;
            unsigned instructions = 0;
            for (unsigned index = 0; index < end; ++index)
                instructions += emitsInstructions(block->at(index));
            if (region.parts.size() == maxBlocksInRegion || copiedInstructions + instructions > maxCopiedInstructions)
                return false;
            copiedInstructions += instructions;
            region.parts.append(Part { block, end });
            if (end == block->size())
                whole.append(block);
            return true;
        };
        if (!add(join))
            return std::nullopt;
        for (unsigned i = 0; i < region.parts.size(); ++i) {
            BasicBlock* block = region.parts[i].block;
            if (!whole.contains(block))
                continue;
            for (BasicBlock* successor : block->successorBlocks()) {
                if (seen.contains(successor) || !m_liveIn.contains(successor))
                    continue;
                bool isOnlyEnteredFromRegion = true;
                for (BasicBlock* predecessor : successor->predecessors())
                    isOnlyEnteredFromRegion &= whole.contains(predecessor);
                if (isOnlyEnteredFromRegion)
                    add(successor);
            }
        }
        pays = false;
        for (Part& part : region.parts) {
            if (part.block->frequency() * maxRatioOfJoinToUser < join->frequency())
                continue;
            for (unsigned index = 0; index < part.end; ++index)
                pays |= usesNumber(part.block->at(index)) && becomesIntegerOperation(part.block->at(index));
        }
        if (!pays)
            return std::nullopt;
        return region;
    }

    bool runOnce()
    {
        m_upsilons.clear();
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() == Upsilon && value->child(0)->type() == Double && !value->child(0)->hasDouble() && isInteger(value->child(0)))
                    m_upsilons.add(value->as<UpsilonValue>()->phi(), Vector<UpsilonValue*, 4> { });
            }
        }
        if (m_upsilons.isEmpty())
            return false;
        m_lastUse.fill(noUse, m_proc.values().size());
        m_uses.shrink(0);
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                value->owner = block;
                if (auto* upsilon = value->as<UpsilonValue>()) {
                    if (auto found = m_upsilons.find(upsilon->phi()); found != m_upsilons.end())
                        found->value.append(upsilon);
                }
                for (Value* child : value->children()) {
                    if (child->type() != Double)
                        continue;
                    m_uses.append(Use { value, m_lastUse[child->index()] });
                    m_lastUse[child->index()] = m_uses.size() - 1;
                }
            }
        }

        Vector<Region> regions;
        IndexSet<BasicBlock*> taken;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() != Phi || value->type() != Double)
                    continue;
                auto upsilons = m_upsilons.find(value);
                if (upsilons == m_upsilons.end())
                    continue;
                auto region = regionFor(value, upsilons->value);
                if (!region)
                    continue;
                bool overlaps = false;
                for (Part& part : region->parts)
                    overlaps |= taken.contains(part.block);
                for (BasicBlock* predecessor : block->predecessors())
                    overlaps |= taken.contains(predecessor);
                if (overlaps)
                    continue;
                for (Part& part : region->parts) {
                    taken.add(part.block);
                    for (BasicBlock* successor : part.block->successorBlocks())
                        taken.add(successor);
                }
                for (BasicBlock* predecessor : block->predecessors())
                    taken.add(predecessor);
                regions.append(WTF::move(*region));
            }
        }
        if (regions.isEmpty())
            return false;

        for (Region& region : regions) {
            for (Part& part : region.parts) {
                BasicBlock* block = part.block;
                if (part.end == block->size())
                    continue;
                BasicBlock* rest = m_proc.addBlock(block->frequency());
                for (unsigned index = part.end; index < block->size(); ++index) {
                    block->at(index)->owner = rest;
                    rest->append(block->at(index));
                }
                rest->successors() = block->successors();
                for (BasicBlock* successor : rest->successorBlocks())
                    successor->replacePredecessor(block, rest);
                Origin origin = block->at(part.end)->origin();
                block->values().shrink(part.end);
                block->appendNew<Value>(m_proc, Jump, origin)->owner = block;
                block->setSuccessors(FrequentedBlock(rest));
                rest->addPredecessor(block);
            }
        }

        IndexSet<BasicBlock*> copied;
        for (Region& region : regions) {
            for (Part& part : region.parts)
                copied.add(part.block);
        }
        IndexSet<Value*> valuesToDemote;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() == Phi && copied.contains(block))
                    valuesToDemote.add(value);
                for (Value* child : value->children()) {
                    if (child->owner != block && copied.contains(child->owner))
                        valuesToDemote.add(child);
                }
            }
        }
        demoteValues(m_proc, valuesToDemote);

        for (Region& region : regions) {
            UncheckedKeyHashMap<BasicBlock*, BasicBlock*> copyOf;
            for (Part& part : region.parts)
                copyOf.add(part.block, m_proc.addBlock(part.block->frequency() * region.otherShare));
            UncheckedKeyHashMap<Value*, Value*> map;
            for (Part& part : region.parts) {
                BasicBlock* copy = copyOf.get(part.block);
                for (Value* value : *part.block) {
                    Value* clone = m_proc.clone(value);
                    for (Value*& child : clone->children()) {
                        if (Value* replacement = map.get(child))
                            child = replacement;
                    }
                    if (value->type() != Void)
                        map.add(value, clone);
                    copy->append(clone);
                }
                copy->successors() = part.block->successors();
                for (FrequentedBlock& successor : copy->successors()) {
                    if (BasicBlock* replacement = copyOf.get(successor.block()))
                        successor.block() = replacement;
                }
            }
            for (Part& part : region.parts) {
                BasicBlock* copy = copyOf.get(part.block);
                for (BasicBlock* successor : copy->successorBlocks())
                    successor->addPredecessor(copy);
            }
            BasicBlock* join = region.parts[0].block;
            BasicBlock* joinCopy = copyOf.get(join);
            for (BasicBlock* predecessor : region.otherPredecessors) {
                predecessor->replaceSuccessor(join, joinCopy);
                join->removePredecessor(predecessor);
                joinCopy->addPredecessor(predecessor);
            }
        }
        m_proc.invalidateCFG();
        fixSSA(m_proc);
        return true;
    }

    Procedure& m_proc;
    UncheckedKeyHashMap<Value*, Vector<UpsilonValue*, 4>> m_upsilons;
    Vector<unsigned> m_lastUse;
    Vector<Use> m_uses;
    Vector<Value*, 16> m_numbers;
    Vector<BasicBlock*, 32> m_liveIn;
    Vector<BasicBlock*, 32> m_near;
    Vector<BasicBlock*, 16> m_worklist;
};

} // anonymous namespace

bool separateIntegerPaths(Procedure& proc)
{
    PhaseScope phaseScope(proc, "separateIntegerPaths"_s);
    SeparateIntegerPaths separateIntegerPaths(proc);
    return separateIntegerPaths.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
