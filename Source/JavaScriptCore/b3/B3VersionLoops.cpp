/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3VersionLoops.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3EnsureLoopPreHeaders.h"
#include "B3FixSSA.h"
#include "B3LoopUtilities.h"
#include "B3MemoryValueInlines.h"
#include "B3NaturalLoops.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3VariableValue.h"
#include <wtf/IndexSet.h>
#include <wtf/RangeSet.h>

namespace JSC { namespace B3 {

namespace {

class VersionLoops {
public:
    VersionLoops(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        ensureLoopPreHeaders(m_proc);
        NaturalLoops& loops = m_proc.naturalLoops();
        m_proc.resetValueOwners();
        m_upsilons = collectUpsilonsByPhi(m_proc);
        for (Value* value : m_proc.values()) {
            for (Value* child : value->children())
                m_numberOfUses.add(child, 0).iterator->value++;
        }
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;)
            considerLoop(loops, loops.loop(loopIndex));
        if (m_versions.isEmpty())
            return false;

        for (auto& version : m_versions)
            hoist(version);
        demoteValuesThatLeaveHotBlocks();
        for (auto& version : m_versions)
            cloneHotBlocks(version);
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr unsigned maxNumberOfClonedValues = 48;
    static constexpr unsigned minNumberOfHoistedValues = 2;
    static constexpr double frequencyScaleOfOriginalLoop = 1.0 / 1024;

    // The poll of a counted loop, which the fast version makes once per strip. See B3StripMineLoops.h.
    struct Poll {
        // Ends in the Branch on the condition, which compares the limit with a constant.
        BasicBlock* block;
        Value* limit;
        Value* condition;
        BasicBlock* continuation;
    };

    struct Version {
        BasicBlock* preHeader { nullptr };
        BasicBlock* header { nullptr };
        Vector<BasicBlock*, 8> hotBlocks;
        // In the order in which they execute. A terminal stands for a guard.
        Vector<Value*, 8> hoistedValues;
        UncheckedKeyHashMap<Value*, Value*> hoistedClones;
        UncheckedKeyHashMap<Value*, BasicBlock*> likelySuccessorsOfHoistedGuards;
        std::optional<CountedLoop> countedLoop;
        std::optional<Poll> poll;
        // With a poll: set in the last pre-header of the fast version.
        Variable* bound { nullptr };
        Variable* endOfStrip { nullptr };
        BasicBlock* firstPreHeaderOfFastVersion { nullptr };
        BasicBlock* lastPreHeaderOfFastVersion { nullptr };
    };

    // A guard: a Branch, or a patchpoint that branches, with one successor that is rare or, if none is, leaves the loop.
    static BasicBlock* likelySuccessorOfGuard(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block)
    {
        if (block->numSuccessors() != 2 || (block->last()->opcode() != Branch && block->last()->opcode() != Patchpoint))
            return nullptr;
        bool firstIsUnlikely = isRare(block->successor(0), loop.header());
        bool secondIsUnlikely = isRare(block->successor(1), loop.header());
        if (!firstIsUnlikely && !secondIsUnlikely) {
            firstIsUnlikely = !loops.belongsTo(block->successorBlock(0), loop);
            secondIsUnlikely = !loops.belongsTo(block->successorBlock(1), loop);
        }
        if (firstIsUnlikely == secondIsUnlikely)
            return nullptr;
        return block->successorBlock(firstIsUnlikely ? 1 : 0);
    }

    // Nothing depends on how a poll comes out.
    bool endsInPoll(BasicBlock* block) const
    {
        if (!m_proc.loopPollGenerator() || block->last()->opcode() != Branch)
            return false;
        for (Value* child : block->last()->child(0)->children()) {
            auto* patchpoint = child->as<PatchpointValue>();
            if (patchpoint && patchpoint->generator() == m_proc.loopPollGenerator())
                return true;
        }
        return false;
    }

    void considerLoop(NaturalLoops& loops, const NaturalLoop& loop)
    {
        for (unsigned i = loop.size(); i--;) {
            if (loops.innerMostLoopOf(loop.at(i)) != &loop)
                return;
        }
        Version version;
        version.header = loop.header();
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (loops.belongsTo(predecessor, loop))
                continue;
            if (version.preHeader)
                return;
            version.preHeader = predecessor;
        }
        if (!version.preHeader || version.preHeader->numSuccessors() != 1)
            return;

        IndexSet<BasicBlock*> hotBlocks;
        hotBlocks.add(loop.header());
        version.hotBlocks.append(loop.header());
        bool hasBackEdge = false;
        for (unsigned i = 0; i < version.hotBlocks.size(); ++i) {
            for (const FrequentedBlock& successor : version.hotBlocks[i]->successors()) {
                if (isRare(successor, loop.header()) || !loops.belongsTo(successor.block(), loop))
                    continue;
                hasBackEdge |= successor.block() == loop.header();
                if (hotBlocks.add(successor.block()))
                    version.hotBlocks.append(successor.block());
            }
        }
        if (!hasBackEdge)
            return;

        RangeSet<HeapRange> writes;
        bool writesPinned = false;
        unsigned numberOfClonedValues = 0;
        for (BasicBlock* block : version.hotBlocks) {
            for (Value* value : *block) {
                if (!canCloneForLoop(value) || isCall(value))
                    return;
                numberOfClonedValues += takesInstructions(value);
                Effects effects = value->effects();
                if (effects.fence)
                    return;
                writes.add(effects.writes);
                writesPinned |= effects.writesPinned;
            }
        }
        if (numberOfClonedValues > maxNumberOfClonedValues)
            return;

        auto isInvariant = [&](Value* value) {
            return !loops.belongsTo(value->owner, loop) || m_hoistedValues.contains(value);
        };
        auto hasInvariantChildren = [&](Value* value) {
            for (Value* child : value->children()) {
                if (!isInvariant(child))
                    return false;
            }
            return true;
        };
        // A load from where a hoisted load loads from is as safe as that one, whatever guards it. By the hoisted load it repeats, so that what hangs on the two is seen to repeat too.
        UncheckedKeyHashMap<Value*, Value*> repeatedLoads;
        auto withoutRepetition = [&](Value* value) {
            Value* repeated = repeatedLoads.get(value);
            return repeated ? repeated : value;
        };
        auto repeatsHoistedLoad = [&](Value* value) {
            auto* load = value->as<MemoryValue>();
            if (!load || !load->isLoad())
                return false;
            for (Value* hoisted : version.hoistedValues) {
                auto* other = hoisted->as<MemoryValue>();
                if (!other || other->opcode() != load->opcode() || other->type() != load->type() || other->offset() != load->offset())
                    continue;
                if (withoutRepetition(other->lastChild()) != withoutRepetition(load->lastChild()))
                    continue;
                repeatedLoads.add(value, withoutRepetition(hoisted));
                return true;
            }
            return false;
        };
        bool everyGuardSoFarIsHoisted = true;
        unsigned numberOfHoistedValues = 0;
        auto hoist = [&](Value* value) {
            version.hoistedValues.append(value);
            m_hoistedValues.add(value);
            numberOfHoistedValues += takesInstructions(value);
        };
        IndexSet<BasicBlock*> visited;
        for (BasicBlock* block = loop.header(); block && hotBlocks.contains(block) && visited.add(block);) {
            for (Value* value : *block) {
                if (value == block->last() || value->type() == Void || value->opcode() == Phi || !hasInvariantChildren(value))
                    continue;
                Effects effects = value->effects();
                if (effects.mustExecute() || effects.readsLocalState || (effects.readsPinned && writesPinned))
                    continue;
                if (effects.reads && effects.readsMutability != Mutability::Immutable && writes.overlaps(effects.reads))
                    continue;
                if (effects.controlDependent && !everyGuardSoFarIsHoisted && !repeatsHoistedLoad(value))
                    continue;
                hoist(value);
            }
            if (block->last()->opcode() == Jump) {
                block = block->successorBlock(0);
                continue;
            }
            BasicBlock* likelySuccessor = likelySuccessorOfGuard(loops, loop, block);
            if (!likelySuccessor)
                break;
            if (hasInvariantChildren(block->last())) {
                hoist(block->last());
                version.likelySuccessorsOfHoistedGuards.add(block->last(), likelySuccessor);
            } else if (!endsInPoll(block))
                everyGuardSoFarIsHoisted = false;
            block = likelySuccessor;
        }
        if (numberOfHoistedValues < minNumberOfHoistedValues) {
            for (Value* value : version.hoistedValues)
                m_hoistedValues.remove(value);
            return;
        }
        version.countedLoop = analyzeCountedLoop(loops, loop, m_upsilons, isInvariant);
        if (version.countedLoop && hotBlocks.contains(version.countedLoop->test) && hotBlocks.contains(version.countedLoop->latch))
            version.poll = findPoll(version);
        if (version.poll) {
            m_valuesLeftOut.add(version.poll->limit);
            m_valuesLeftOut.add(version.poll->condition);
            m_hoistedValues.add(version.poll->block->last());
            version.likelySuccessorsOfHoistedGuards.add(version.poll->block->last(), version.poll->continuation);
        }
        for (BasicBlock* block : version.hotBlocks)
            m_hotBlocks.add(block);
        m_versions.append(WTF::move(version));
    }

    std::optional<Poll> findPoll(const Version& version)
    {
        Poll result;
        result.limit = nullptr;
        for (BasicBlock* block : version.hotBlocks) {
            for (Value* value : *block) {
                auto* patchpoint = value->as<PatchpointValue>();
                if (!patchpoint || !m_proc.loopPollGenerator() || patchpoint->generator() != m_proc.loopPollGenerator())
                    continue;
                if (result.limit)
                    return std::nullopt;
                result.limit = value;
            }
        }
        if (!result.limit || m_numberOfUses.get(result.limit) != 1)
            return std::nullopt;
        result.block = result.limit->owner;
        if (result.block->last()->opcode() != Branch || m_hoistedValues.contains(result.block->last()))
            return std::nullopt;
        result.condition = result.block->last()->child(0);
        if (result.condition->owner != result.block || m_numberOfUses.get(result.condition) != 1 || result.condition->numChildren() != 2)
            return std::nullopt;
        unsigned indexOfLimit = result.condition->child(0) == result.limit ? 0 : 1;
        if (result.condition->child(indexOfLimit) != result.limit || !result.condition->child(1 - indexOfLimit)->hasInt64())
            return std::nullopt;
        unsigned indexOfSlowPath = result.block->successor(0).frequency() == FrequencyClass::Rare ? 0 : 1;
        if (result.block->successor(indexOfSlowPath).frequency() != FrequencyClass::Rare)
            return std::nullopt;
        result.continuation = result.block->successorBlock(1 - indexOfSlowPath);
        return result;
    }

    void hoist(Version& version)
    {
        double frequency = version.preHeader->frequency();
        BasicBlock* block = m_proc.addBlock(frequency);
        version.firstPreHeaderOfFastVersion = block;
        for (Value* value : version.hoistedValues) {
            Value* clone = m_proc.clone(value);
            for (Value*& child : clone->children()) {
                if (Value* replacement = version.hoistedClones.get(child))
                    child = replacement;
            }
            block->append(clone);
            if (value != value->owner->last()) {
                version.hoistedClones.add(value, clone);
                continue;
            }
            BasicBlock* next = m_proc.addBlock(frequency);
            block->successors() = value->owner->successors();
            BasicBlock* likelySuccessor = version.likelySuccessorsOfHoistedGuards.get(value);
            for (FrequentedBlock& successor : block->successors())
                successor = successor.block() == likelySuccessor ? FrequentedBlock(next) : FrequentedBlock(version.header, FrequencyClass::Rare);
            block = next;
        }
        version.lastPreHeaderOfFastVersion = block;
        if (!version.poll)
            return;
        const CountedLoop& loop = *version.countedLoop;
        Origin origin = loop.test->last()->origin();
        Value* hoistedBound = version.hoistedClones.get(loop.bound);
        Value* bound = hoistedBound ? hoistedBound : loop.bound;
        version.bound = m_proc.addVariable(Int64);
        version.endOfStrip = m_proc.addVariable(Int64);
        block->appendNew<VariableValue>(m_proc, Set, origin, version.bound, bound);
        block->appendNew<VariableValue>(m_proc, Set, origin, version.endOfStrip, appendEndOfStrip(m_proc, block, loop, loop.start, bound, !relationHoldsOnEntry(loop)));
    }

    void demoteValuesThatLeaveHotBlocks()
    {
        m_proc.resetValueOwners();
        IndexSet<Value*> valuesToDemote;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                if (value->opcode() == Phi && m_hotBlocks.contains(block))
                    valuesToDemote.add(value);
                for (Value* child : value->children()) {
                    if (child->owner != block && m_hotBlocks.contains(child->owner))
                        valuesToDemote.add(child);
                }
            }
        }
        demoteValues(m_proc, valuesToDemote);
    }

    void cloneHotBlocks(Version& version)
    {
        UncheckedKeyHashMap<BasicBlock*, BasicBlock*> clones;
        for (BasicBlock* block : version.hotBlocks)
            clones.add(block, m_proc.addBlock(block->frequency()));
        Variable* nextOfInductionVariable = version.poll ? m_proc.addVariable(Int64) : nullptr;
        for (BasicBlock* block : version.hotBlocks) {
            BasicBlock* cloneOfBlock = clones.get(block);
            UncheckedKeyHashMap<Value*, Value*> map;
            for (Value* value : *block) {
                if (m_valuesLeftOut.contains(value))
                    continue;
                if (Value* hoisted = version.hoistedClones.get(value)) {
                    map.add(value, hoisted);
                    continue;
                }
                if (m_hoistedValues.contains(value)) {
                    cloneOfBlock->appendNew<Value>(m_proc, Jump, value->origin());
                    continue;
                }
                Value* clone = m_proc.clone(value);
                for (Value*& child : clone->children()) {
                    if (Value* replacement = map.get(child))
                        child = replacement;
                }
                if (value->type() != Void)
                    map.add(value, clone);
                cloneOfBlock->append(clone);
                if (nextOfInductionVariable && value == version.countedLoop->next)
                    cloneOfBlock->appendNew<VariableValue>(m_proc, Set, value->origin(), nextOfInductionVariable, clone);
            }
            if (m_hoistedValues.contains(block->last()))
                cloneOfBlock->setSuccessors(FrequentedBlock(version.likelySuccessorsOfHoistedGuards.get(block->last())));
            else
                cloneOfBlock->successors() = block->successors();
            for (BasicBlock*& successor : cloneOfBlock->successorBlocks()) {
                if (BasicBlock* cloneOfSuccessor = clones.get(successor))
                    successor = cloneOfSuccessor;
            }
            block->setFrequency(block->frequency() * frequencyScaleOfOriginalLoop);
        }
        if (version.poll)
            stripMine(version, clones, nextOfInductionVariable);
        version.lastPreHeaderOfFastVersion->appendNew<Value>(m_proc, Jump, version.header->at(0)->origin());
        version.lastPreHeaderOfFastVersion->setSuccessors(FrequentedBlock(clones.get(version.header)));
        version.preHeader->replaceSuccessor(version.header, version.firstPreHeaderOfFastVersion);
    }

    // As B3::stripMineLoops() does it, on variables. Where a stop is requested the original loop goes on, which polls every time around.
    void stripMine(Version& version, const UncheckedKeyHashMap<BasicBlock*, BasicBlock*>& clones, Variable* nextOfInductionVariable)
    {
        const CountedLoop& loop = *version.countedLoop;
        Origin origin = loop.test->last()->origin();
        double frequency = loop.header->frequency() * frequencyScaleOfOriginalLoop;
        BasicBlock* test = clones.get(loop.test);
        BasicBlock* latch = clones.get(loop.latch);
        BasicBlock* realTest = m_proc.addBlock(version.preHeader->frequency());
        BasicBlock* pollOfStrip = m_proc.addBlock(frequency);
        BasicBlock* nextStrip = m_proc.addBlock(frequency);
        BasicBlock* latchOfOriginalLoop = m_proc.addBlock(frequency);
        // By now a value of another block is only to be had from a variable.
        auto get = [&](BasicBlock* block, Variable* variable) {
            return block->appendNew<VariableValue>(m_proc, Get, origin, variable);
        };

        BasicBlock* exit = test->successorBlock(1 - loop.indexOfLatchInSuccessors);
        test->removeLast(m_proc);
        Value* nextAtTest = get(test, nextOfInductionVariable);
        test->appendNew<Value>(m_proc, Branch, origin, test->appendNew<Value>(m_proc, loop.relation, origin, nextAtTest, get(test, version.endOfStrip)));
        test->setSuccessors(FrequentedBlock(latch), FrequentedBlock(realTest));

        Value* nextAtRealTest = get(realTest, nextOfInductionVariable);
        realTest->appendNew<Value>(m_proc, Branch, origin, realTest->appendNew<Value>(m_proc, loop.relation, origin, nextAtRealTest, get(realTest, version.bound)));
        realTest->setSuccessors(FrequentedBlock(pollOfStrip, FrequencyClass::Rare), FrequentedBlock(exit));

        Value* limit = m_proc.clone(version.poll->limit);
        pollOfStrip->append(limit);
        // A stop request is handed over to the original loop, which therefore has to go on polling every time around: stripMineLoops() must not know its poll.
        version.poll->limit->as<PatchpointValue>()->setGenerator([generator = RefPtr { m_proc.loopPollGenerator() }](CCallHelpers& jit, const StackmapGenerationParams& params) {
            generator->run(jit, params);
        });
        Value* condition = m_proc.clone(version.poll->condition);
        for (Value*& child : condition->children())
            child = child == version.poll->limit ? limit : pollOfStrip->appendNew<Const64Value>(m_proc, origin, child->asInt64());
        pollOfStrip->append(condition);
        pollOfStrip->appendNew<Value>(m_proc, Branch, origin, condition);
        pollOfStrip->successors() = version.poll->block->successors();
        for (FrequentedBlock& successor : pollOfStrip->successors())
            successor = successor.block() == version.poll->continuation ? FrequentedBlock(nextStrip) : FrequentedBlock(latchOfOriginalLoop, FrequencyClass::Rare);

        Value* nextAtNextStrip = get(nextStrip, nextOfInductionVariable);
        nextStrip->appendNew<VariableValue>(m_proc, Set, origin, version.endOfStrip, appendEndOfStrip(m_proc, nextStrip, loop, nextAtNextStrip, get(nextStrip, version.bound), false));
        nextStrip->appendNew<Value>(m_proc, Jump, origin);
        nextStrip->setSuccessors(FrequentedBlock(latch));

        // After the demotion a block refers to no value of another block of the loop.
        UncheckedKeyHashMap<Value*, Value*> map;
        for (Value* value : *loop.latch) {
            Value* clone = m_proc.clone(value);
            for (Value*& child : clone->children()) {
                if (Value* replacement = map.get(child))
                    child = replacement;
            }
            if (value->type() != Void)
                map.add(value, clone);
            latchOfOriginalLoop->append(clone);
        }
        latchOfOriginalLoop->setSuccessors(FrequentedBlock(loop.header));
    }

    Procedure& m_proc;
    UpsilonsByPhi m_upsilons;
    UncheckedKeyHashMap<Value*, unsigned> m_numberOfUses;
    IndexSet<Value*> m_valuesLeftOut;
    Vector<Version> m_versions;
    IndexSet<BasicBlock*> m_hotBlocks;
    IndexSet<Value*> m_hoistedValues;
};

} // anonymous namespace

bool versionLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "versionLoops"_s);
    VersionLoops versionLoops(proc);
    return versionLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
