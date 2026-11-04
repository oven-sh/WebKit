/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "testb3_bun_random_program.h"

#if ENABLE(B3_JIT)

#include "B3InductionRecords.h"
#include <wtf/SetForScope.h>

struct RandomProgram::CountedLoopShape {
    enum class Bound : uint8_t { Constant, Invariant, Variable, ImmutableLoad, Load };
    enum class Next : uint8_t { Plain, WrapsInt32, Saturates };

    Type indexType() const { return is32 ? Int32 : Int64; }
    int64_t delta() const { return countsDown ? -step : step; }
    bool cannotWrap() const { return mask + step <= std::numeric_limits<int32_t>::max(); }

    bool addsStepOncePerTrip() const
    {
        if (hasVariableStep || mayAdvanceTwice || hasSecondEntry)
            return false;
        switch (nextKind) {
        case Next::Plain:
            return true;
        case Next::WrapsInt32:
            return cannotWrap();
        case Next::Saturates:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }

    bool is32 { false };
    bool countsDown { false };
    bool isBottomTested { false };
    bool isGuarded { false };
    bool isLong { false };
    bool mayAdvanceTwice { false };
    bool hasSecondEntry { false };
    bool mutatesBound { false };
    bool pollsInHeader { false };
    bool pollsInBody { false };
    bool hasVariableStep { false };
    Bound boundKind { Bound::Constant };
    Next nextKind { Next::Plain };
    B3::Opcode relation { LessThan };
    int64_t mask { 0 };
    int64_t step { 1 };
    int64_t boundMask { 0 };
    int64_t constantBound { 0 };
    int32_t boundOffset { 0 };
};

struct RandomProgram::FuelBoundedLoopShape {
    enum class Next : uint8_t { AddStep, AddStepToOtherValue, AddNonConstant, DoubleIndex, SubtractFromBound, WrapsInt32, SaturateAtInt64Limit };

    bool addsStepOncePerTrip() const { return nextKind == Next::AddStep && step && !hasSecondEntry; }

    B3::Opcode relation { Equal };
    int64_t step { 0 };
    Next nextKind { Next::AddStep };
    bool isBottomTested { false };
    bool isGuarded { false };
    bool hasSecondEntry { false };
    bool boundIsInvariant { false };
    bool spendsFuelFirst { false };
};

struct RandomProgram::LoopParts {
    BasicBlock* loopHeader() const { return header ? header : body; }

    Variable* index { nullptr };
    Variable* fuel { nullptr };
    Variable* boundVariable { nullptr };
    Value* invariantBound { nullptr };
    BasicBlock* header { nullptr };
    BasicBlock* body { nullptr };
    BasicBlock* latch { nullptr };
    BasicBlock* exit { nullptr };
};

static void publishInductionSometimes(Procedure& proc, WeakRandom& promiseRandom, const DemotedInduction& induction)
{
    if (promiseRandom.getUint32(100) < 50)
        proc.setKnownInduction(induction);
}

static void appendSaturatingSet(Graph& graph, BasicBlock*& block, Variable* index, Value* next, B3::Opcode relation, int64_t limit)
{
    BasicBlock* saturated = graph.proc.addBlock();
    BasicBlock* usual = graph.proc.addBlock();
    BasicBlock* join = graph.proc.addBlock();
    graph.branch(block, graph.make(block, relation, next, graph.int64(block, limit)), rarely(saturated), FrequentedBlock(usual));
    graph.set(saturated, index, graph.int64(saturated, limit));
    graph.jump(saturated, join);
    graph.set(usual, index, next);
    graph.jump(usual, join);
    block = join;
}

auto RandomProgram::chooseCountedLoopShape() -> CountedLoopShape
{
    using Bound = CountedLoopShape::Bound;
    using Next = CountedLoopShape::Next;
    static constexpr Bound boundKinds[] = { Bound::Constant, Bound::Invariant, Bound::Variable, Bound::ImmutableLoad, Bound::Load };

    CountedLoopShape shape;
    shape.is32 = chance(30);
    shape.countsDown = chance(25);
    shape.isBottomTested = chance(30);
    shape.isGuarded = shape.isBottomTested && chance(60);
    shape.isLong = m_loops.isEmpty() && chance(3);
    shape.mask = shape.isLong ? (1 << 17) - 1 : m_loops.isEmpty() ? 31 : m_loops.size() == 1 ? 7 : 3;
    shape.step = chance(70) ? 1 : 2 + pick(2);
    shape.boundKind = boundKinds[pick(std::size(boundKinds))];
    shape.nextKind = shape.is32 ? Next::Plain : chance(12) ? Next::WrapsInt32 : chance(8) ? Next::Saturates : Next::Plain;
    shape.mayAdvanceTwice = chance(8);
    shape.hasSecondEntry = !shape.isBottomTested && chance(4);
    shape.mutatesBound = shape.boundKind == Bound::Variable && chance(30);
    shape.pollsInHeader = !shape.isBottomTested && chance(10);
    shape.pollsInBody = !shape.pollsInHeader && chance(35);
    shape.hasVariableStep = !shape.is32 && shape.nextKind == Next::Plain && chance(6);

    bool canUseExactTest = shape.step == 1 && !shape.hasVariableStep && !shape.mayAdvanceTwice && !shape.hasSecondEntry && !shape.mutatesBound && (!shape.isBottomTested || shape.isGuarded);
    if (canUseExactTest && (shape.boundKind == Bound::Constant || shape.boundKind == Bound::Invariant) && chance(12))
        shape.relation = NotEqual;
    else if (shape.countsDown)
        shape.relation = canUseExactTest && chance(20) ? Above : chance(50) ? GreaterThan : GreaterEqual;
    else if (!shape.mutatesBound && chance(30))
        shape.relation = chance(50) ? Below : BelowEqual;
    else
        shape.relation = chance(60) ? LessThan : LessEqual;

    shape.boundMask = shape.countsDown ? 3 : shape.mask;
    shape.constantBound = shape.relation == NotEqual && shape.countsDown ? 0 : pick(shape.boundMask + 1);
    if (shape.relation == NotEqual && shape.countsDown)
        shape.boundKind = Bound::Constant;
    switch (shape.boundKind) {
    case Bound::Constant:
    case Bound::Invariant:
    case Bound::Variable:
        break;
    case Bound::ImmutableLoad:
        shape.boundOffset = (2 * wordsPerRegion + pick(wordsPerRegion)) * sizeof(int64_t);
        break;
    case Bound::Load:
        shape.boundOffset = pick(2 * wordsPerRegion) * sizeof(int64_t);
        break;
    }
    return shape;
}

auto RandomProgram::appendCountedLoopEntry(const CountedLoopShape& shape) -> LoopParts
{
    using Bound = CountedLoopShape::Bound;
    auto maskedExpression = [&](int64_t mask) {
        Value* result = make(BitAnd, expression(2), int64(mask));
        if (mask > longRun && chance(70))
            return make(BitOr, result, int64(longRun));
        return result;
    };
    auto integer = [&](int64_t value) {
        return m_block->appendIntConstant(m_proc, Origin(), shape.indexType(), value);
    };
    auto toIndexType = [&](Value* value) {
        return shape.is32 ? make(Trunc, value) : value;
    };

    LoopParts parts;
    switch (shape.boundKind) {
    case Bound::Constant:
    case Bound::ImmutableLoad:
    case Bound::Load:
        break;
    case Bound::Invariant:
        parts.invariantBound = maskedExpression(shape.boundMask);
        break;
    case Bound::Variable:
        parts.boundVariable = m_proc.addVariable(Int64);
        set(parts.boundVariable, maskedExpression(shape.boundMask));
        break;
    }

    parts.index = m_proc.addVariable(shape.indexType());
    if (shape.countsDown)
        set(parts.index, toIndexType(maskedExpression(shape.mask)));
    else
        set(parts.index, shape.relation == NotEqual || chance(60) ? integer(0) : chance(50) ? integer(pick(4)) : toIndexType(make(BitAnd, expression(1), int64(3))));

    parts.exit = newBlock();
    parts.header = shape.isBottomTested ? nullptr : newBlock();
    parts.body = newBlock();
    parts.latch = newBlock();
    if (shape.isGuarded)
        appendCountedLoopTest(shape, parts);
    else if (shape.hasSecondEntry)
        branch(condition(1), FrequentedBlock(parts.body), FrequentedBlock(parts.header));
    else
        jump(parts.loopHeader());
    return parts;
}

Value* RandomProgram::appendCountedLoopTest(const CountedLoopShape& shape, const LoopParts& parts)
{
    using Bound = CountedLoopShape::Bound;
    auto maskedLoad = [&](HeapRange range) {
        return make(BitAnd, m_graph.load(m_block, Int64, m_pointer, shape.boundOffset, range), int64(shape.boundMask));
    };

    Value* left = get(parts.index);
    Value* right = nullptr;
    switch (shape.boundKind) {
    case Bound::Constant:
        right = m_block->appendIntConstant(m_proc, Origin(), shape.indexType(), shape.constantBound);
        break;
    case Bound::Invariant:
        right = parts.invariantBound;
        break;
    case Bound::Variable:
        right = get(parts.boundVariable);
        break;
    case Bound::ImmutableLoad:
        right = maskedLoad(thirdHeap);
        break;
    case Bound::Load:
        right = maskedLoad(bothHeaps);
        break;
    }
    if (right->type() != shape.indexType())
        right = make(Trunc, right);

    B3::Opcode opcode = shape.relation;
    if (chance(20)) {
        std::swap(left, right);
        opcode = relationWithSwappedOperands(opcode);
    }
    FrequentedBlock stay(parts.body);
    FrequentedBlock exit(parts.exit, chance(10) ? FrequencyClass::Rare : FrequencyClass::Normal);
    bool isNegated = chance(25);
    Value* test = make(isNegated ? negatedRelation(opcode) : opcode, left, right);
    if (isNegated)
        branch(test, exit, stay);
    else
        branch(test, stay, exit);
    return test;
}

Value* RandomProgram::appendCountedLoopBody(const CountedLoopShape& shape, const LoopParts& parts, unsigned depth)
{
    using Next = CountedLoopShape::Next;
    auto integer = [&](int64_t value) {
        return m_block->appendIntConstant(m_proc, Origin(), shape.indexType(), value);
    };
    SetForScope isInLongLoop(m_isInLongLoop, shape.isLong);
    size_t availableValueCount = m_available.size();

    m_block = parts.body;
    if (shape.pollsInBody)
        poll();
    m_loops.append(Loop { parts.latch, parts.exit, shape.nextKind == Next::Plain && !shape.hasVariableStep ? parts.header : nullptr, parts.index, shape.delta() });
    statements(shape.isLong ? 0 : depth);
    m_loops.removeLast();
    if (shape.mayAdvanceTwice) {
        BasicBlock* again = newBlock();
        BasicBlock* join = newBlock();
        branch(condition(1), FrequentedBlock(again), FrequentedBlock(join));
        m_block = again;
        set(parts.index, make(Add, get(parts.index), integer(shape.delta())));
        jump(join);
        m_block = join;
    }
    if (shape.mutatesBound)
        set(parts.boundVariable, make(Add, get(parts.boundVariable), int64(shape.countsDown ? 1 : -1)));
    jump(parts.latch);

    m_available.shrink(availableValueCount);
    m_block = parts.latch;
    Value* current = get(parts.index);
    if (shape.hasVariableStep) {
        Value* amount = make(BitOr, make(BitAnd, expression(1), int64(3)), int64(1));
        Value* next = make(shape.countsDown ? Sub : Add, current, amount);
        set(parts.index, next);
        return next;
    }
    Value* next = shape.countsDown && chance(50) ? make(Sub, current, integer(shape.step)) : make(Add, current, integer(shape.delta()));
    switch (shape.nextKind) {
    case Next::Plain:
        break;
    case Next::WrapsInt32:
        next = make(SExt32, make(Trunc, next));
        break;
    case Next::Saturates:
        appendSaturatingSet(m_graph, m_block, parts.index, next, shape.countsDown ? LessThan : GreaterThan, shape.countsDown ? -(1ll << 40) : 1ll << 40);
        return next;
    }
    set(parts.index, next);
    return next;
}

void RandomProgram::countedLoop(unsigned depth)
{
    CountedLoopShape shape = chooseCountedLoopShape();
    LoopParts parts = appendCountedLoopEntry(shape);
    Value* test = nullptr;
    if (parts.header) {
        m_block = parts.header;
        if (shape.pollsInHeader)
            poll();
        test = appendCountedLoopTest(shape, parts);
    }
    Value* next = appendCountedLoopBody(shape, parts, depth);
    if (parts.header)
        jump(parts.header);
    else
        test = appendCountedLoopTest(shape, parts);
    if (shape.addsStepOncePerTrip()) {
        bool doesNotWrap = shape.cannotWrap() && m_promiseRandom.getUint32(100) < 50;
        publishInductionSometimes(m_proc, m_promiseRandom, { parts.index, parts.loopHeader(), next, test, shape.delta(), doesNotWrap });
    }
    m_block = parts.exit;
}

auto RandomProgram::chooseFuelBoundedLoopShape() -> FuelBoundedLoopShape
{
    using Next = FuelBoundedLoopShape::Next;
    static constexpr B3::Opcode relations[] = { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual };
    static constexpr int64_t steps[] = { 0, 1, -1, 2, -2, 3, longRun, -longRun, longRun + 1, -longRun - 1, 100000, 1ll << 40, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() };
    static constexpr Next nextKinds[] = { Next::AddStep, Next::AddStepToOtherValue, Next::AddNonConstant, Next::DoubleIndex, Next::SubtractFromBound, Next::WrapsInt32, Next::SaturateAtInt64Limit };

    FuelBoundedLoopShape shape;
    shape.relation = relations[pick(std::size(relations))];
    shape.step = steps[pick(std::size(steps))];
    shape.nextKind = chance(50) ? Next::AddStep : nextKinds[pick(std::size(nextKinds))];
    shape.isBottomTested = chance(40);
    shape.isGuarded = shape.isBottomTested && chance(50);
    shape.boundIsInvariant = chance(75);
    shape.spendsFuelFirst = chance(50);
    shape.hasSecondEntry = !shape.isGuarded && chance(8);
    return shape;
}

auto RandomProgram::appendFuelBoundedLoopEntry(const FuelBoundedLoopShape& shape) -> LoopParts
{
    LoopParts parts;
    parts.fuel = m_proc.addVariable(Int64);
    parts.index = m_proc.addVariable(Int64);
    parts.boundVariable = m_proc.addVariable(Int64);
    set(parts.fuel, make(Add, make(BitAnd, expression(1), int64(m_loops.isEmpty() ? 31 : 3)), int64(1)));
    set(parts.index, expression(2));
    parts.invariantBound = expression(2);
    set(parts.boundVariable, parts.invariantBound);

    parts.exit = newBlock();
    parts.header = shape.isBottomTested ? nullptr : newBlock();
    parts.body = newBlock();
    parts.latch = newBlock();
    if (shape.isGuarded)
        appendFuelBoundedLoopTest(shape, parts);
    else if (shape.hasSecondEntry)
        branch(condition(1), FrequentedBlock(parts.latch), FrequentedBlock(parts.loopHeader()));
    else
        jump(parts.loopHeader());
    return parts;
}

Value* RandomProgram::appendFuelBoundedLoopTest(const FuelBoundedLoopShape& shape, const LoopParts& parts)
{
    Value* left = get(parts.index);
    Value* right = shape.boundIsInvariant ? parts.invariantBound : get(parts.boundVariable);
    bool isNegated = chance(25);
    Value* test = make(isNegated ? negatedRelation(shape.relation) : shape.relation, left, right);
    if (isNegated)
        branch(test, FrequentedBlock(parts.exit), FrequentedBlock(parts.body));
    else
        branch(test, FrequentedBlock(parts.body), FrequentedBlock(parts.exit));
    return test;
}

Value* RandomProgram::appendFuelBoundedLoopBody(const FuelBoundedLoopShape& shape, const LoopParts& parts, unsigned depth)
{
    using Next = FuelBoundedLoopShape::Next;
    auto spendFuel = [&] {
        BasicBlock* next = newBlock();
        Value* remainingFuel = make(Sub, get(parts.fuel), int64(1));
        set(parts.fuel, remainingFuel);
        Value* isSpent = make(Equal, get(parts.fuel), int64(0));
        branch(isSpent, FrequentedBlock(parts.exit, chance(50) ? FrequencyClass::Rare : FrequencyClass::Normal), FrequentedBlock(next));
        m_block = next;
        if (!shape.hasSecondEntry)
            publishInductionSometimes(m_proc, m_promiseRandom, { parts.fuel, parts.loopHeader(), remainingFuel, isSpent, -1, true });
    };
    size_t availableValueCount = m_available.size();

    m_block = parts.body;
    if (shape.spendsFuelFirst)
        spendFuel();
    if (chance(35))
        poll();
    m_loops.append(Loop { parts.latch, parts.exit, nullptr, parts.index, 0 });
    statements(depth);
    m_loops.removeLast();
    if (!shape.boundIsInvariant && chance(50))
        set(parts.boundVariable, expression(1));
    jump(parts.latch);

    m_available.shrink(availableValueCount);
    m_block = parts.latch;
    if (!shape.spendsFuelFirst)
        spendFuel();
    Value* current = get(parts.index);
    Value* next = nullptr;
    switch (shape.nextKind) {
    case Next::AddStep:
        next = make(Add, current, int64(shape.step));
        break;
    case Next::AddStepToOtherValue:
        next = make(Add, get(randomVariable()), int64(shape.step));
        break;
    case Next::AddNonConstant:
        next = make(Add, current, expression(1));
        break;
    case Next::DoubleIndex:
        next = make(Add, current, current);
        break;
    case Next::SubtractFromBound:
        next = make(Sub, shape.boundIsInvariant ? parts.invariantBound : get(parts.boundVariable), current);
        break;
    case Next::WrapsInt32:
        next = make(SExt32, make(Trunc, make(Add, current, int64(shape.step))));
        break;
    case Next::SaturateAtInt64Limit: {
        int64_t limit = chance(50) ? std::numeric_limits<int64_t>::max() : chance(50) ? std::numeric_limits<int64_t>::min() : 20;
        next = make(Add, current, int64(shape.step));
        appendSaturatingSet(m_graph, m_block, parts.index, next, shape.step < 0 ? LessThan : GreaterThan, limit);
        return next;
    }
    }
    set(parts.index, next);
    return next;
}

void RandomProgram::fuelBoundedLoop(unsigned depth)
{
    FuelBoundedLoopShape shape = chooseFuelBoundedLoopShape();
    LoopParts parts = appendFuelBoundedLoopEntry(shape);
    Value* test = nullptr;
    if (parts.header) {
        m_block = parts.header;
        test = appendFuelBoundedLoopTest(shape, parts);
    }
    Value* next = appendFuelBoundedLoopBody(shape, parts, depth);
    if (parts.header)
        jump(parts.header);
    else
        test = appendFuelBoundedLoopTest(shape, parts);
    if (shape.addsStepOncePerTrip())
        publishInductionSometimes(m_proc, m_promiseRandom, { parts.index, parts.loopHeader(), next, test, shape.step, false });
    m_block = parts.exit;
}

void RandomProgram::whileLoop(unsigned depth)
{
    Variable* fuel = m_proc.addVariable(Int64);
    set(fuel, make(Add, make(BitAnd, expression(1), int64(m_loops.isEmpty() ? 15 : 3)), int64(1)));
    BasicBlock* exit = newBlock();
    size_t availableValueCount = m_available.size();
    BasicBlock* header = newBlock();
    BasicBlock* body = newBlock();
    BasicBlock* end = newBlock();
    if (chance(8))
        branch(condition(1), FrequentedBlock(chance(50) ? body : end), FrequentedBlock(header));
    else
        jump(header);
    m_block = header;
    if (chance(80))
        branch(condition(2), FrequentedBlock(body), FrequentedBlock(exit, chance(15) ? FrequencyClass::Rare : FrequencyClass::Normal));
    else
        jump(body);
    m_block = body;
    if (chance(30))
        poll();
    m_loops.append(Loop { end, exit, nullptr, nullptr, 0 });
    statements(depth);
    m_loops.removeLast();
    jump(end);
    m_block = end;
    set(fuel, make(Sub, get(fuel), int64(1)));
    branch(make(NotEqual, get(fuel), int64(0)), FrequentedBlock(header), FrequentedBlock(exit));
    m_available.shrink(availableValueCount);
    m_block = exit;
}

#endif // ENABLE(B3_JIT)
