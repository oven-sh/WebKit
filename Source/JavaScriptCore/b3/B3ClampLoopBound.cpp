/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3ClampLoopBound.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CountedLoops.h"
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

static size_t indexInOwner(Value* value)
{
    size_t index = value->owner->values().find(value);
    RELEASE_ASSERT(index != notFound);
    return index;
}

namespace {

class LoopTestRewriter {
public:
    LoopTestRewriter(Procedure& proc, const CountedLoop& loop, const ClampTargets& targets)
        : m_proc(proc)
        , m_loop(loop)
        , m_targets(targets)
        , m_origin(loop.test->last()->origin())
        , m_tested(proc.addVariable(loop.testedValue()->type()))
    {
        ASSERT(loop.cannotWrap());
        ASSERT(targets.limit && targets.beyondLimit);
        RELEASE_ASSERT(loop.test->last()->opcode() == Branch);
        setTestedVariable(loop.testedValue());
        for (const LoopTestCopy& copy : targets.otherTests) {
            RELEASE_ASSERT(copy.test->last()->opcode() == Branch);
            RELEASE_ASSERT(!loop.isTopTested || copy.failed);
            if (copy.testedValue)
                setTestedVariable(copy.testedValue);
        }
    }

    Value* appendTested(BasicBlock* block)
    {
        return appendBeforeTerminal(block, m_proc.add<VariableValue>(Get, m_origin, m_tested));
    }

    void rewriteTests(BasicBlock* failed)
    {
        rewriteTest(m_loop.test, m_loop.latch, failed);
        for (const LoopTestCopy& copy : m_targets.otherTests)
            rewriteTest(copy.test, copy.latch, copy.failed ? copy.failed : failed);
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

    void rewriteTest(BasicBlock* test, BasicBlock* latch, BasicBlock* failed)
    {
        Value* relationHolds = m_proc.add<Value>(m_loop.relation, m_origin, appendTested(test), m_targets.limit);
        test->appendNonTerminal(relationHolds);
        test->last()->child(0) = relationHolds;
        test->setSuccessors(FrequentedBlock(latch), FrequentedBlock(failed, m_loop.isTopTested ? FrequencyClass::Rare : FrequencyClass::Normal));
    }

    Procedure& m_proc;
    const CountedLoop& m_loop;
    const ClampTargets& m_targets;
    Origin m_origin;
    Variable* m_tested;
};

} // anonymous namespace

void clampLoopBound(Procedure& proc, const CountedLoop& loop, const ClampTargets& targets)
{
    LoopTestRewriter rewriter(proc, loop, targets);
    BasicBlock* realTest = proc.addBlock();
    BasicBlock* exit = loop.test->successorBlock(1 - loop.latchSuccessorIndex);
    rewriter.rewriteTests(realTest);
    rewriter.appendTest(realTest, loop.bound);
    realTest->setSuccessors(FrequentedBlock(targets.beyondLimit, FrequencyClass::Rare), FrequentedBlock(exit));
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
