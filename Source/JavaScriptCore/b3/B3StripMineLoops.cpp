/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3StripMineLoops.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
#include "B3Dominators.h"
#include "B3EnsureLoopPreHeaders.h"
#include "B3LoopUtilities.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3PhaseScope.h"
#include "B3PhiChildren.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"

namespace JSC { namespace B3 {

static Value* appendBeforeTerminal(BasicBlock* block, Value* value)
{
    if (block->size() && block->last()->effects().terminal)
        block->appendNonTerminal(value);
    else
        block->append(value);
    return value;
}

static bool canDistanceBeNegative(const Procedure& proc, const CountedLoop& loop, Value* from, Value* limit)
{
    IntegerInterval fromInterval = intervalFor(proc, from, loop.signedness());
    IntegerInterval limitInterval = intervalFor(proc, limit, loop.signedness());
    return loop.increases() ? limitInterval.min < fromInterval.max : fromInterval.min < limitInterval.max;
}

Value* appendStripEnd(Procedure& proc, BasicBlock* block, const CountedLoop& loop, Value* from, Value* limit, bool relationMayNotHold)
{
    Origin origin = from->origin();
    auto append = [&](Value* value) {
        return appendBeforeTerminal(block, value);
    };
    auto constant = [&](int64_t value) {
        return append(proc.addIntConstant(origin, from->type(), value));
    };
    bool isIncreasing = loop.increases();
    bool endCanPassLimit = relationMayNotHold && canDistanceBeNegative(proc, loop, from, limit);
    if (!from->isConstant())
        from = append(proc.add<Value>(Opaque, origin, from));
    Value* distance = append(isIncreasing ? proc.add<Value>(Sub, origin, limit, from) : proc.add<Value>(Sub, origin, from, limit));
    Value* most = constant(Limits::maxIterationsPerStrip - (loop.includesEqual() ? 1 : 0));
    Value* length = append(proc.add<Value>(Select, origin, append(proc.add<Value>(Below, origin, distance, most)), distance, most));
    Value* end = append(proc.add<Value>(isIncreasing ? Add : Sub, origin, from, length));
    if (!endCanPassLimit)
        return end;
    IntegerInterval full = fullInterval(from->type(), loop.signedness());
    Value* noStrip = constant(static_cast<int64_t>(isIncreasing ? full.min : full.max));
    Value* relationHolds = append(proc.add<Value>(loop.relation, origin, from, limit));
    return append(proc.add<Value>(Select, origin, relationHolds, end, noStrip));
}

static size_t indexInOwner(Value* value)
{
    size_t index = value->owner->values().find(value);
    RELEASE_ASSERT(index != notFound);
    return index;
}

namespace {

class LoopTestRewriter {
public:
    LoopTestRewriter(Procedure& proc, const CountedLoop& loop, const StripTargets& targets)
        : m_proc(proc)
        , m_loop(loop)
        , m_targets(targets)
        , m_origin(loop.test->last()->origin())
        , m_tested(proc.addVariable(loop.testedValue()->type()))
    {
        ASSERT(loop.cannotWrap());
        ASSERT(!!targets.limit == !!targets.beyondLimit);
        RELEASE_ASSERT(loop.test->last()->opcode() == Branch);
        setTestedVariable(loop.testedValue());
        for (const LoopTestCopy& copy : targets.otherTests) {
            RELEASE_ASSERT(copy.test->last()->opcode() == Branch);
            RELEASE_ASSERT(!loop.isTopTested || copy.failed);
            if (copy.testedValue)
                setTestedVariable(copy.testedValue);
        }
    }

    Origin origin() const { return m_origin; }

    Value* appendTested(BasicBlock* block)
    {
        return appendBeforeTerminal(block, m_proc.add<VariableValue>(Get, m_origin, m_tested));
    }

    template<typename Functor>
    void rewriteTests(BasicBlock* failed, const Functor& farthestAt)
    {
        rewriteTest(m_loop.test, m_loop.latch, failed, farthestAt(m_loop.test));
        for (const LoopTestCopy& copy : m_targets.otherTests)
            rewriteTest(copy.test, copy.latch, copy.failed ? copy.failed : failed, farthestAt(copy.test));
    }

    void appendTest(BasicBlock* block, Value* farthest)
    {
        Value* relationHolds = block->appendNew<Value>(m_proc, m_loop.relation, m_origin, appendTested(block), farthest);
        block->appendNew<Value>(m_proc, Branch, m_origin, relationHolds);
    }

private:
    void setTestedVariable(Value* value)
    {
        Value* set = m_proc.add<VariableValue>(Set, m_origin, m_tested, value);
        set->owner = value->owner;
        value->owner->values().insert(indexInOwner(value) + 1, set);
    }

    void rewriteTest(BasicBlock* test, BasicBlock* latch, BasicBlock* failed, Value* farthest)
    {
        Value* relationHolds = m_proc.add<Value>(m_loop.relation, m_origin, appendTested(test), farthest);
        test->appendNonTerminal(relationHolds);
        test->last()->child(0) = relationHolds;
        test->setSuccessors(FrequentedBlock(latch), FrequentedBlock(failed, m_loop.isTopTested ? FrequencyClass::Rare : FrequencyClass::Normal));
    }

    Procedure& m_proc;
    const CountedLoop& m_loop;
    const StripTargets& m_targets;
    Origin m_origin;
    Variable* m_tested;
};

} // anonymous namespace

void stripMineLoop(Procedure& proc, const CountedLoop& loop, PatchpointValue* poll, const StripTargets& targets)
{
    ASSERT(isLoopPoll(poll));
    LoopTestRewriter rewriter(proc, loop, targets);
    Origin origin = rewriter.origin();
    Value* limit = targets.limit ? targets.limit : loop.bound;
    Variable* end = proc.addVariable(loop.testedValue()->type());
    BasicBlock* realTest = proc.addBlock();
    BasicBlock* strip = proc.addBlock();
    BasicBlock* limitTest = targets.limit ? proc.addBlock() : nullptr;
    BasicBlock* exit = loop.test->successorBlock(1 - loop.latchSuccessorIndex);

    Value* firstEnd = appendStripEnd(proc, loop.preHeader, loop, loop.start, limit, !targets.relationHoldsOnEntry);
    appendBeforeTerminal(loop.preHeader, proc.add<VariableValue>(Set, origin, end, firstEnd));

    rewriter.rewriteTests(realTest, [&](BasicBlock* test) {
        return appendBeforeTerminal(test, proc.add<VariableValue>(Get, origin, end));
    });
    rewriter.appendTest(realTest, loop.bound);
    realTest->setSuccessors(FrequentedBlock(limitTest ? limitTest : strip, FrequencyClass::Rare), FrequentedBlock(exit));
    if (limitTest) {
        rewriter.appendTest(limitTest, limit);
        limitTest->setSuccessors(FrequentedBlock(strip), FrequentedBlock(targets.beyondLimit, FrequencyClass::Rare));
    }

    Value* nop = proc.add<Value>(Nop, Void, poll->origin());
    nop->owner = poll->owner;
    poll->owner->at(indexInOwner(poll)) = nop;
    strip->append(poll);
    Value* nextEnd = appendStripEnd(proc, strip, loop, rewriter.appendTested(strip), limit, false);
    strip->appendNew<VariableValue>(proc, Set, origin, end, nextEnd);
    strip->appendNew<Value>(proc, Jump, origin);
    strip->setSuccessors(FrequentedBlock(loop.isTopTested ? loop.header : loop.latch));

    proc.addRemark({ RemarkKind::PollStripMined, GaveUpReason::None, loop.header->at(0)->origin(), std::nullopt });
}

void clampLoopBound(Procedure& proc, const CountedLoop& loop, const StripTargets& targets)
{
    ASSERT(targets.limit);
    LoopTestRewriter rewriter(proc, loop, targets);
    BasicBlock* realTest = proc.addBlock();
    BasicBlock* exit = loop.test->successorBlock(1 - loop.latchSuccessorIndex);
    rewriter.rewriteTests(realTest, [&](BasicBlock*) {
        return targets.limit;
    });
    rewriter.appendTest(realTest, loop.bound);
    realTest->setSuccessors(FrequentedBlock(targets.beyondLimit, FrequencyClass::Rare), FrequentedBlock(exit));
}

namespace {

class StripMineLoops {
public:
    StripMineLoops(Procedure& proc)
        : m_proc(proc)
    {
    }

    bool run()
    {
        if (!m_proc.hasEverAdded(Patchpoint))
            return false;
        ensureLoopPreHeaders(m_proc);
        NaturalLoops& loops = m_proc.naturalLoops();
        if (!loops.numLoops())
            return false;
        m_proc.resetValueOwners();
        PhiChildren phiChildren(m_proc);
        Vector<Candidate> candidates;
        for (unsigned loopIndex = loops.numLoops(); loopIndex--;) {
            const NaturalLoop& loop = loops.loop(loopIndex);
            if (!isInnermostLoop(loops, loop))
                continue;
            IndexSet<BasicBlock*> likelyBlocks = findLikelyBlocks(loops, loop);
            Polls likelyPolls = findPolls(loop, likelyBlocks);
            if (likelyPolls.isEmpty() || !hasLikelyBackEdge(loop, likelyBlocks))
                continue;
            auto candidate = findCandidate(loops, loop, phiChildren, likelyPolls);
            if (candidate)
                candidates.append(*candidate);
            else
                m_proc.addRemark({ RemarkKind::PollNotStripMined, candidate.error(), loop.header()->at(0)->origin(), std::nullopt });
        }
        if (candidates.isEmpty())
            return false;
        for (const Candidate& candidate : candidates) {
            StripTargets targets;
            targets.relationHoldsOnEntry = candidate.loop.relationHoldsOnEntry;
            stripMineLoop(m_proc, candidate.loop, candidate.poll, targets);
        }
        m_proc.resetReachability();
        m_proc.invalidateCFG();
        return true;
    }

private:
    using Polls = Vector<PatchpointValue*, 2>;

    struct Candidate {
        CountedLoop loop;
        PatchpointValue* poll;
    };

    static Polls findPolls(const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks)
    {
        Polls result;
        for (unsigned i = loop.size(); i--;) {
            BasicBlock* block = loop.at(i);
            if (!likelyBlocks.contains(block))
                continue;
            for (Value* value : *block) {
                if (isLoopPoll(value))
                    result.append(value->as<PatchpointValue>());
            }
        }
        return result;
    }

    std::expected<Candidate, GaveUpReason> findCandidate(NaturalLoops& loops, const NaturalLoop& loop, PhiChildren& phiChildren, const Polls& likelyPolls)
    {
        if (likelyPolls.size() > 1)
            return makeUnexpected(GaveUpReason::SeveralPolls);
        PatchpointValue* poll = likelyPolls[0];
        auto countedLoop = analyzeCountedLoop({ m_proc, loops, loop, phiChildren, [&](Value* value) { return !loops.belongsTo(value->owner, loop); }, std::nullopt });
        if (!countedLoop)
            return makeUnexpected(countedLoop.error());
        if (countedLoop->isTopTested)
            return makeUnexpected(GaveUpReason::NotCounted);
        if (!countedLoop->cannotWrap())
            return makeUnexpected(GaveUpReason::CounterMayWrap);
        Dominators& dominators = m_proc.dominators();
        for (BasicBlock* predecessor : loop.header()->predecessors()) {
            if (!loops.belongsTo(predecessor, loop))
                continue;
            if (!dominators.dominates(countedLoop->test, predecessor))
                return makeUnexpected(GaveUpReason::TestNotOnLikelyPath);
            if (!dominators.dominates(poll->owner, predecessor))
                return makeUnexpected(GaveUpReason::PollNotOnEveryIteration);
        }
        return Candidate { *countedLoop, poll };
    }

    Procedure& m_proc;
};

} // anonymous namespace

bool stripMineLoops(Procedure& proc)
{
    PhaseScope phaseScope(proc, "stripMineLoops"_s);
    StripMineLoops stripMineLoops(proc);
    return stripMineLoops.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
