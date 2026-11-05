/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

void RandomProgram::returnStatement()
{
    m_graph.ret(m_block, expression(1));
    m_block = newBlock();
}

void RandomProgram::exitLoop()
{
    Loop& loop = m_loops[chance(85) ? m_loops.size() - 1 : pick(m_loops.size())];
    switch (pick(5)) {
    case 0:
    case 1:
        jump(loop.breakTarget);
        break;
    case 2:
    case 3:
        if (loop.headerForExtraLatch && chance(30)) {
            Value* index = get(loop.index);
            set(loop.index, make(Add, index, index->type() == Int32 ? int32(static_cast<int32_t>(loop.delta)) : int64(loop.delta)));
            jump(loop.headerForExtraLatch);
        } else
            jump(loop.continueTarget);
        break;
    default:
        m_graph.ret(m_block, expression(1));
        break;
    }
    m_block = newBlock();
}

void RandomProgram::branchTarget(BasicBlock* block, FrequencyClass frequency, unsigned depth, BasicBlock* join)
{
    size_t availableValueCount = m_available.size();
    bool isRare = frequency == FrequencyClass::Rare;
    m_block = block;
    statements(depth);
    if (!m_loops.isEmpty() && chance(isRare ? 50 : 15))
        exitLoop();
    else if (chance(isRare ? 25 : 4))
        returnStatement();
    jump(join);
    m_available.shrink(availableValueCount);
}

void RandomProgram::ifStatement(unsigned depth)
{
    Value* predicate = condition(2);
    unsigned shape = pick(100);
    bool hasElse = chance(50);
    FrequencyClass thenFrequency = FrequencyClass::Normal;
    FrequencyClass elseFrequency = FrequencyClass::Normal;
    if (shape < 25)
        thenFrequency = FrequencyClass::Rare;
    else if (shape < 32)
        thenFrequency = FrequencyClass::Unlikely;
    else if (shape < 42 && hasElse)
        elseFrequency = FrequencyClass::Rare;
    else if (shape < 47 && hasElse)
        elseFrequency = FrequencyClass::Unlikely;
    BasicBlock* thenBlock = newBlock();
    BasicBlock* join = newBlock();
    BasicBlock* elseBlock = hasElse ? newBlock() : join;
    branch(predicate, FrequentedBlock(thenBlock, thenFrequency), FrequentedBlock(elseBlock, elseFrequency));
    branchTarget(thenBlock, thenFrequency, depth, join);
    if (hasElse)
        branchTarget(elseBlock, elseFrequency, depth, join);
    m_block = join;
}

void RandomProgram::switchStatement(unsigned depth)
{
    Value* subject = expression(2);
    if (chance(60))
        subject = make(BitAnd, subject, int64(7));
    unsigned numberOfCases = 2 + pick(7);
    int64_t firstCase = pick(4);
    int64_t stride = 1 + pick(2);
    BasicBlock* join = newBlock();
    Vector<BasicBlock*, 10> caseBlocks;
    Vector<int64_t, 10> caseValues;
    for (unsigned i = 0; i <= numberOfCases; ++i)
        caseBlocks.append(newBlock());
    for (unsigned i = 0; i < numberOfCases; ++i)
        caseValues.append(firstCase + i * stride);
    if (chance(10))
        caseValues[pick(numberOfCases)] = chance(50) ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
    for (unsigned begin = 0; begin < numberOfCases;) {
        unsigned end = chance(60) ? numberOfCases : begin + 1 + pick(numberOfCases - begin);
        BasicBlock* next = end < numberOfCases ? newBlock() : caseBlocks.last();
        if (chance(50)) {
            SwitchValue* switchValue = m_block->appendNew<SwitchValue>(m_proc, Origin(), subject);
            switchValue->setFallThrough(FrequentedBlock(next));
            for (unsigned i = begin; i < end; ++i)
                switchValue->appendCase(SwitchCase(caseValues[i], FrequentedBlock(chance(10) ? caseBlocks[pick(caseBlocks.size())] : caseBlocks[i])));
        } else {
            for (unsigned i = begin; i < end; ++i) {
                BasicBlock* nextTest = i + 1 < end ? newBlock() : next;
                Value* tested = chance(6) ? expression(1) : subject;
                int64_t caseValue = chance(6) ? caseValues[pick(numberOfCases)] : caseValues[i];
                if (chance(20))
                    branch(make(NotEqual, tested, int64(caseValue)), FrequentedBlock(nextTest), FrequentedBlock(caseBlocks[i]));
                else
                    branch(make(Equal, tested, int64(caseValue)), FrequentedBlock(caseBlocks[i]), FrequentedBlock(nextTest));
                m_block = nextTest;
                if (chance(8))
                    set(randomVariable(), expression(1));
            }
        }
        m_block = next;
        begin = end;
    }
    for (BasicBlock* block : caseBlocks)
        branchTarget(block, FrequencyClass::Normal, std::min(depth, 1u), join);
    m_block = join;
}

void RandomProgram::longEqualityChain()
{
    Value* subject = make(BitAnd, expression(2), int64(255));
    Value* otherSubject = expression(1);
    unsigned numberOfTests = 125 + pick(60);
    unsigned stride = chance(50) ? 1 : 3;
    unsigned oddTestIndex = chance(50) ? pick(numberOfTests) : numberOfTests;
    unsigned oddTestKind = pick(3);
    BasicBlock* join = newBlock();
    Vector<BasicBlock*, 5> caseBlocks;
    for (unsigned i = 0; i < 5; ++i)
        caseBlocks.append(newBlock());
    for (unsigned i = 0; i < numberOfTests; ++i) {
        BasicBlock* next = i + 1 < numberOfTests ? newBlock() : caseBlocks.last();
        Value* tested = subject;
        int64_t caseValue = i * stride % 256;
        if (i == oddTestIndex) {
            if (!oddTestKind)
                tested = otherSubject;
            else
                caseValue = oddTestKind == 1 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
        }
        branch(make(Equal, tested, int64(caseValue)), FrequentedBlock(caseBlocks[pick(4)]), FrequentedBlock(next));
        m_block = next;
    }
    for (BasicBlock* block : caseBlocks)
        branchTarget(block, FrequencyClass::Normal, 0, join);
    m_block = join;
}

void RandomProgram::boundsCheckedAccess()
{
    bool reliesOnBoundsCheck = chance(50);
    Value* index = randomLoopIndex();
    if (!index || (reliesOnBoundsCheck && chance(30)))
        index = reliesOnBoundsCheck ? expression(1) : make(BitAnd, expression(1), int64(31));
    if (chance(25))
        index = make(Add, index, int64(pick(3)));
    Value* length;
    switch (pick(4)) {
    case 0:
    case 1:
        length = loadSmallWord();
        break;
    case 2:
        length = int64(pick(reliesOnBoundsCheck ? wordsPerRegion + 1 : 40));
        break;
    default:
        length = make(BitAnd, get(randomVariable()), int64(31));
        break;
    }
    if (reliesOnBoundsCheck && !length->hasInt64())
        length = make(BitAnd, length, int64(wordsPerRegion - 1));
    BasicBlock* outOfBounds = newBlock();
    BasicBlock* inBounds = newBlock();
    BasicBlock* join = newBlock();
    if (chance(50))
        branch(make(AboveEqual, index, length), rarely(outOfBounds), FrequentedBlock(inBounds));
    else
        branch(make(Below, index, length), FrequentedBlock(inBounds), rarely(outOfBounds));

    m_block = outOfBounds;
    switch (pick(4)) {
    case 0:
        m_graph.ret(m_block, int64(-1));
        m_block = newBlock();
        break;
    case 1:
        if (!m_loops.isEmpty())
            exitLoop();
        break;
    case 2:
        set(randomVariable(), expression(1));
        break;
    default:
        break;
    }
    jump(join);

    m_block = inBounds;
    unsigned region = pick(3);
    int32_t offset = region * wordsPerRegion * sizeof(int64_t);
    HeapRange range = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
    Value* address = reliesOnBoundsCheck ? make(Add, m_pointer, make(Shl, index, int32(3))) : wordAddress(index);
    if (region < 2 && chance(40))
        m_graph.store(m_block, expression(1), address, offset, range);
    else {
        Variable* variable = randomVariable();
        set(variable, make(Add, get(variable), m_graph.load(m_block, Int64, address, offset, range)));
    }
    jump(join);
    m_block = join;
}

void RandomProgram::nullCheckedAccess()
{
    static constexpr FrequencyClass nullEdgeFrequencies[] = { FrequencyClass::Normal, FrequencyClass::Unlikely, FrequencyClass::Rare, FrequencyClass::Rare };
    unsigned region = pick(3);
    int32_t offset = (region * wordsPerRegion + pick(chance(60) ? 3 : wordsPerRegion)) * sizeof(int64_t);
    HeapRange range = !region ? firstHeap : region == 1 ? secondHeap : thirdHeap;
    auto appendLoad = [&](B3::Kind kind, Value* pointer) {
        Variable* variable = randomVariable();
        MemoryValue* loaded = m_block->appendNew<MemoryValue>(m_proc, kind, Int64, Origin(), pointer, offset, range);
        if (region == 2 && chance(50))
            loaded->setReadsMutability(Mutability::Immutable);
        set(variable, make(BitXor, get(variable), loaded));
    };
    if (!pick(3)) {
        appendLoad(trapping(Load), m_pointer);
        return;
    }

    Value* pointer = chance(70) ? m_pointerOrNull : m_graph.select(m_block, condition(1), m_pointer, int64(0));
    BasicBlock* isNull = newBlock();
    BasicBlock* isNotNull = newBlock();
    BasicBlock* join = newBlock();
    FrequentedBlock ifNull(isNull, nullEdgeFrequencies[pick(std::size(nullEdgeFrequencies))]);
    if (chance(50))
        branch(make(Equal, pointer, int64(0)), ifNull, FrequentedBlock(isNotNull));
    else
        branch(pointer, FrequentedBlock(isNotNull), ifNull);

    m_block = isNull;
    if (chance(30))
        set(randomVariable(), expression(1));
    else if (chance(20) && !m_loops.isEmpty())
        exitLoop();
    jump(join);

    m_block = isNotNull;
    if (region < 2 && chance(30))
        m_graph.store(m_block, expression(1), pointer, offset, range);
    else
        appendLoad(Load, pointer);
    jump(join);
    m_block = join;
}

void RandomProgram::flagTestedAfterJoin(unsigned depth)
{
    Variable* flag = randomVariable();
    int64_t first = pick(3);
    int64_t second = pick(3);
    BasicBlock* thenBlock = newBlock();
    BasicBlock* elseBlock = newBlock();
    BasicBlock* join = newBlock();
    Value* tested = expression(1);
    branch(make(chance(50) ? LessThan : Equal, tested, constant()), FrequentedBlock(thenBlock), FrequentedBlock(elseBlock));
    m_block = thenBlock;
    set(flag, int64(first));
    jump(join);
    m_block = elseBlock;
    set(flag, chance(70) ? int64(second) : expression(1));
    jump(join);
    m_block = join;
    BasicBlock* taken = newBlock();
    BasicBlock* after = newBlock();
    Value* seen = get(flag);
    if (chance(30))
        seen = make(chance(50) ? Add : BitXor, seen, int64(1));
    static constexpr B3::Opcode relations[] = { Equal, NotEqual, LessThan, GreaterEqual, Below };
    Value* predicate = chance(30) ? seen : make(relations[pick(std::size(relations))], seen, int64(pick(3)));
    if (chance(20))
        predicate = make(Equal, predicate, predicate->type() == Int32 ? int32(0) : int64(0));
    branch(predicate, FrequentedBlock(taken), FrequentedBlock(after));
    branchTarget(taken, FrequencyClass::Normal, depth, after);
    m_block = after;
}

void RandomProgram::branchToSameBlock()
{
    BasicBlock* next = newBlock();
    branch(condition(1), FrequentedBlock(next), FrequentedBlock(next));
    m_block = next;
}

void RandomProgram::statements(unsigned depth)
{
    unsigned count = 1 + pick(5);
    for (unsigned i = 0; i < count && m_statementsLeft; ++i) {
        --m_statementsLeft;
        statement(depth);
    }
}

void RandomProgram::statement(unsigned depth)
{
    unsigned kind = pick(100);
    bool mayNestLoop = depth && m_loops.size() < 3 && !m_isInLongLoop;
    if (kind < 1)
        storeBurst();
    else if (kind < 10)
        store();
    else if (kind < 16)
        m_available.append(expression(2));
    else if (kind < 22)
        patchpoint();
    else if (kind < 23)
        check();
    else if (kind < 40 && depth)
        ifStatement(depth - 1);
    else if (kind < 56 && mayNestLoop)
        countedLoop(depth - 1);
    else if (kind < 61 && mayNestLoop)
        fuelBoundedLoop(depth - 1);
    else if (kind < 65 && mayNestLoop)
        whileLoop(depth - 1);
    else if (kind < 71 && depth)
        switchStatement(depth - 1);
    else if (kind < 75 && depth)
        flagTestedAfterJoin(depth - 1);
    else if (kind < 76) {
        if (depth == 3 && chance(25))
            longEqualityChain();
        else
            branchToSameBlock();
    } else if (kind < 82)
        boundsCheckedAccess();
    else if (kind < 86)
        nullCheckedAccess();
    else
        set(randomVariable(), expression(3));
}

#endif // ENABLE(B3_JIT)
