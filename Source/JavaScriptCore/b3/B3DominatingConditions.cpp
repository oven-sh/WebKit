/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3DominatingConditions.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3Dominators.h"

namespace JSC { namespace B3 {

std::optional<DominatingCondition> conditionAtHead(BasicBlock* block)
{
    if (block->numPredecessors() != 1)
        return std::nullopt;
    BasicBlock* predecessor = block->predecessor(0);
    Value* branch = predecessor->last();
    if (branch->opcode() != Branch)
        return std::nullopt;
    BasicBlock* taken = predecessor->taken().block();
    BasicBlock* notTaken = predecessor->notTaken().block();
    if (taken == notTaken || (block != taken && block != notTaken))
        return std::nullopt;
    DominatingCondition condition = stripNegations(branch->child(0));
    if (block == notTaken)
        condition.isNonZero = !condition.isNonZero;
    return condition;
}

void DominatingConditionStack::reset(unsigned numberOfValues)
{
    m_knowledge.fill(0, numberOfValues);
    m_added.shrink(0);
    m_layerStarts.shrink(0);
}

void DominatingConditionStack::popLayer()
{
    unsigned start = m_layerStarts.takeLast();
    for (unsigned i = m_added.size(); i-- > start;)
        m_knowledge[m_added[i] >> 2] &= ~(m_added[i] & 3);
    m_added.shrink(start);
}

bool DominatingConditionStack::add(Value* value, bool isNonZero)
{
    unsigned index = value->index();
    if (index >= m_knowledge.size())
        m_knowledge.insertFill(m_knowledge.size(), 0, index + 1 - m_knowledge.size());
    Knowledge bit = isNonZero ? IsNonZero : IsZero;
    uint8_t& knowledge = m_knowledge[index];
    if (knowledge & bit)
        return false;
    knowledge |= bit;
    m_added.append(index << 2 | bit);
    return true;
}

DominatingConditions::DominatingConditions(Procedure& proc)
    : m_proc(proc)
    , m_chainAtBlock(proc.size())
{
    Dominators& dominators = proc.dominators();
    for (BasicBlock* block : proc.blocksInPreOrder()) {
        Chain chain;
        if (BasicBlock* dominator = dominators.idom(block))
            chain = m_chainAtBlock[dominator];
        if (std::optional<DominatingCondition> condition = conditionAtHead(block))
            chain = extend(chain, *condition);
        m_chainAtBlock[block] = chain;
    }
}

DominatingConditions::Chain DominatingConditions::extend(Chain chain, DominatingCondition condition)
{
    unsigned index = m_nodes.size();
    m_nodes.append(Node { condition, chain });
    return index;
}

bool isComparisonWithConstant(const Value* value)
{
    switch (value->opcode()) {
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
        return value->child(1)->hasInt() && !value->child(0)->hasInt();
    default:
        return false;
    }
}

IntRange rangeImpliedBy(const Value* comparison, bool holds)
{
    ASSERT(isComparisonWithConstant(comparison));
    Type type = comparison->child(0)->type();
    IntRange top = IntRange::top(type);
    int64_t constant = comparison->child(1)->asInt();
    int64_t least = top.min();
    int64_t most = top.max();
    std::optional<Opcode> opcode = comparison->opcode();
    if (!holds)
        opcode = invertedCompare(*opcode, type);
    if (!opcode)
        return top;
    switch (*opcode) {
    case Equal:
        return IntRange(constant, constant);
    case LessThan:
        if (constant == least)
            return top;
        return IntRange(least, constant - 1);
    case LessEqual:
        return IntRange(least, constant);
    case GreaterThan:
        if (constant == most)
            return top;
        return IntRange(constant + 1, most);
    case GreaterEqual:
        return IntRange(constant, most);
    default:
        return top;
    }
}

TriState evaluateImpliedComparison(const Value* comparison, const Value* known, bool knownHolds)
{
    ASSERT(isComparisonWithConstant(comparison));
    ASSERT(comparison->child(0)->type() == known->child(0)->type());
    return evaluateComparison(comparison->opcode(), rangeImpliedBy(known, knownHolds), comparison->child(1)->asInt());
}

static bool isSameOperand(const Value* a, const Value* b)
{
    a = a->foldIdentity();
    b = b->foldIdentity();
    return a == b || (a->hasInt() && b->hasInt() && a->type() == b->type() && a->asInt() == b->asInt());
}

static bool impliesRelation(const DominatingCondition& condition, Opcode relation, Value* left, Value* right)
{
    Value* known = condition.value;
    if (known->numChildren() != 2 || !known->child(0)->type().isInt())
        return false;
    std::optional<Opcode> inverted = invertedRelation(known->opcode());
    if (!inverted)
        return false;
    Opcode knownRelation = condition.isNonZero ? known->opcode() : *inverted;
    if (knownRelation == relation && isSameOperand(known->child(0), left) && isSameOperand(known->child(1), right))
        return true;
    return flippedRelation(knownRelation) == relation && isSameOperand(known->child(1), left) && isSameOperand(known->child(0), right);
}

bool isRelationImpliedAt(Procedure& proc, BasicBlock* block, Opcode relation, Value* left, Value* right)
{
    Dominators& dominators = proc.dominators();
    uint64_t maxQueries = proc.limit(Limit::MaxConditionQueriesPerBlock);
    uint64_t queryCount = 0;
    for (BasicBlock* dominator = block; dominator; dominator = dominators.idom(dominator)) {
        std::optional<DominatingCondition> condition = conditionAtHead(dominator);
        if (!condition)
            continue;
        if (queryCount++ == maxQueries) {
            proc.didReachLimit(Limit::MaxConditionQueriesPerBlock, block->at(0)->origin());
            return false;
        }
        if (impliesRelation(*condition, relation, left, right))
            return true;
    }
    return false;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
