/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3StripMineLoops.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3EnsureLoopPreHeaders.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>

namespace JSC { namespace B3 {

namespace {

class StripMineLoops {
public:
    StripMineLoops(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        if (!m_proc.loopPollGenerator())
            return false;
        ensureLoopPreHeaders(m_proc);
        NaturalLoops& loops = m_proc.naturalLoops();
        if (!loops.numLoops())
            return false;
        m_proc.resetValueOwners();
        m_numberOfUses.resize(m_proc.values().size());
        for (Value* value : m_proc.values()) {
            for (Value* child : value->children())
                m_numberOfUses[child]++;
        }
        UpsilonsByPhi upsilonsByPhi(m_proc);
        Vector<std::pair<CountedLoop, Poll>> countedLoops;
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
            const NaturalLoop& loop = loops.loop(loopIndex);
            auto poll = findPoll(loops, loop);
            if (!poll)
                continue;
            auto countedLoop = analyzeCountedLoop(loops, loop, upsilonsByPhi, [&](Value* value) {
                return !loops.belongsTo(value->owner, loop);
            });
            if (countedLoop)
                countedLoops.append({ *countedLoop, *poll });
        }
        if (countedLoops.isEmpty())
            return false;
        for (auto& [loop, poll] : countedLoops)
            stripMine(loop, poll);
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    static constexpr double frequencyScaleOfStripEnd = 1.0 / 1024;

    struct Poll {
        // Ends in the Branch on the condition, which compares the limit with a constant.
        BasicBlock* block;
        Value* limit;
        Value* condition;
        BasicBlock* slowPath;
        // The one call of the slow path. duplicateTails() may have put a copy of what follows the poll behind it.
        Value* handler;
        BasicBlock* continuation;
    };

    std::optional<Poll> findPoll(NaturalLoops& loops, const NaturalLoop& loop)
    {
        Poll result;
        result.limit = nullptr;
        for (unsigned i = loop.size(); i--;) {
            for (Value* value : *loop.at(i)) {
                auto* patchpoint = value->as<PatchpointValue>();
                if (!patchpoint || patchpoint->generator() != m_proc.loopPollGenerator())
                    continue;
                if (result.limit)
                    return std::nullopt;
                result.limit = value;
            }
        }
        if (!result.limit || m_numberOfUses[result.limit] != 1)
            return std::nullopt;
        result.block = result.limit->owner;
        if (result.block->last()->opcode() != Branch)
            return std::nullopt;
        result.condition = result.block->last()->child(0);
        if (result.condition->owner != result.block || m_numberOfUses[result.condition] != 1 || result.condition->numChildren() != 2)
            return std::nullopt;
        unsigned indexOfLimit = result.condition->child(0) == result.limit ? 0 : 1;
        if (result.condition->child(indexOfLimit) != result.limit || !result.condition->child(1 - indexOfLimit)->hasInt64())
            return std::nullopt;
        unsigned indexOfSlowPath = result.block->successor(0).frequency() == FrequencyClass::Rare ? 0 : 1;
        if (result.block->successor(indexOfSlowPath).frequency() != FrequencyClass::Rare)
            return std::nullopt;
        result.slowPath = result.block->successorBlock(indexOfSlowPath);
        result.continuation = result.block->successorBlock(1 - indexOfSlowPath);
        if (result.slowPath->numPredecessors() != 1 || !loops.belongsTo(result.slowPath, loop))
            return std::nullopt;
        result.handler = nullptr;
        for (Value* value : *result.slowPath) {
            if (value->opcode() != Patchpoint && value->opcode() != CCall)
                continue;
            if (result.handler)
                return std::nullopt;
            result.handler = value;
        }
        if (!result.handler || result.handler == result.slowPath->last() || m_numberOfUses[result.handler])
            return std::nullopt;
        for (Value* child : result.handler->children()) {
            if (loops.belongsTo(child->owner, loop) && !child->isConstant())
                return std::nullopt;
        }
        return result;
    }

    void stripMine(const CountedLoop& loop, const Poll& poll)
    {
        Origin origin = loop.test->last()->origin();
        double frequency = loop.header->frequency() * frequencyScaleOfStripEnd;
        BasicBlock* realTest = m_proc.addBlock(loop.preHeader->frequency());
        BasicBlock* pollOfStrip = m_proc.addBlock(frequency);
        BasicBlock* slowPathOfStrip = m_proc.addBlock(poll.slowPath->frequency());
        BasicBlock* nextStrip = m_proc.addBlock(frequency);

        Value* end = m_proc.add<Value>(Phi, Int64, origin);
        loop.header->values().insert(0, end);
        Value* endAtLatch = m_proc.add<Value>(Phi, Int64, origin);
        loop.latch->values().insert(0, endAtLatch);
        loop.latch->appendNonTerminal(m_proc.add<UpsilonValue>(origin, endAtLatch, end));

        loop.preHeader->appendNonTerminal(m_proc.add<UpsilonValue>(origin, appendEndOfStrip(m_proc, loop.preHeader, loop, loop.start, loop.bound, !relationHoldsOnEntry(loop)), end));

        BasicBlock* exit = loop.test->successorBlock(1 - loop.indexOfLatchInSuccessors);
        Value* branch = loop.test->last();
        realTest->append(m_proc.add<Value>(loop.relation, origin, loop.next, loop.bound));
        realTest->append(m_proc.add<Value>(Branch, origin, realTest->at(0)));
        realTest->setSuccessors(FrequentedBlock(pollOfStrip, FrequencyClass::Rare), FrequentedBlock(exit));

        loop.test->appendNonTerminal(m_proc.add<UpsilonValue>(origin, end, endAtLatch));
        Value* isWithinStrip = m_proc.add<Value>(loop.relation, origin, loop.next, end);
        loop.test->appendNonTerminal(isWithinStrip);
        branch->child(0) = isWithinStrip;
        loop.test->setSuccessors(FrequentedBlock(loop.latch), FrequentedBlock(realTest));

        for (Value*& value : *poll.block) {
            if (value == poll.limit || value == poll.condition)
                value = m_proc.add<Value>(Nop, Void, value->origin());
        }
        pollOfStrip->append(poll.limit);
        for (Value*& child : poll.condition->children()) {
            if (child != poll.limit) {
                child = m_proc.add<Const64Value>(origin, child->asInt64());
                pollOfStrip->append(child);
            }
        }
        pollOfStrip->append(poll.condition);
        pollOfStrip->append(poll.block->last());
        pollOfStrip->successors() = poll.block->successors();
        pollOfStrip->replaceSuccessor(poll.continuation, nextStrip);
        pollOfStrip->replaceSuccessor(poll.slowPath, slowPathOfStrip);
        poll.block->last() = m_proc.add<Value>(Jump, origin);
        poll.block->setSuccessors(FrequentedBlock(poll.continuation));

        // Nothing leads to the old slow path any more.
        for (Value*& value : *poll.slowPath) {
            if (value == poll.handler)
                value = m_proc.add<Value>(Nop, Void, value->origin());
        }
        for (Value*& child : poll.handler->children()) {
            if (child->isConstant()) {
                child = m_proc.clone(child);
                slowPathOfStrip->append(child);
            }
        }
        slowPathOfStrip->append(poll.handler);
        slowPathOfStrip->append(m_proc.add<Value>(Jump, origin));
        slowPathOfStrip->setSuccessors(FrequentedBlock(nextStrip));

        nextStrip->append(m_proc.add<Value>(Jump, origin));
        nextStrip->setSuccessors(FrequentedBlock(loop.latch));
        nextStrip->appendNonTerminal(m_proc.add<UpsilonValue>(origin, appendEndOfStrip(m_proc, nextStrip, loop, loop.next, loop.bound, false), endAtLatch));
    }

    Procedure& m_proc;
    IndexMap<Value*, unsigned> m_numberOfUses;
};

} // anonymous namespace

bool stripMineLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "stripMineLoops"_s);
    StripMineLoops stripMineLoops(proc);
    return stripMineLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
