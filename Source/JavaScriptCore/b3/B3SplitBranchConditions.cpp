/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3SplitBranchConditions.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CompareChains.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UseCounts.h"
#include "B3ValueInlines.h"
#include <limits>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/IndexMap.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace B3 {

static bool isOneBitTest(const Value* value)
{
    if ((value->opcode() != Equal && value->opcode() != NotEqual) || !value->child(1)->isInt(0))
        return false;
    const Value* masked = value->child(0);
    return masked->opcode() == BitAnd && masked->child(1)->hasInt() && hasOneBitSet(static_cast<uint64_t>(masked->child(1)->asInt()));
}

static bool isEqualityWithConstant(Value* value)
{
    return (value->opcode() == Equal || value->opcode() == NotEqual) && value->child(1)->hasInt();
}

namespace {

// Answers as Value::returnsBool() does, once for each value.
class BooleanValues {
public:
    bool contains(Value* value)
    {
        m_pending.append(value);
        while (!m_pending.isEmpty()) {
            Value* pending = m_pending.last();
            if (m_answers.contains(pending)) {
                m_pending.removeLast();
                continue;
            }
            auto [begin, end] = operandsThatDecide(pending);
            size_t sizeBefore = m_pending.size();
            for (unsigned index = begin; index < end; ++index) {
                if (!m_answers.contains(pending->child(index)))
                    m_pending.append(pending->child(index));
            }
            if (m_pending.size() != sizeBefore)
                continue;
            m_pending.removeLast();
            m_answers.add(pending, answerFor(pending, begin, end));
        }
        return m_answers.get(value);
    }

private:
    static std::pair<unsigned, unsigned> operandsThatDecide(const Value* value)
    {
        if (value->type() != Int32)
            return { 0, 0 };
        switch (value->opcode()) {
        case BitAnd:
        case BitOr:
        case BitXor:
            return { 0, 2 };
        case Select:
            return { 1, 3 };
        case Identity:
            return { 0, 1 };
        default:
            return { 0, 0 };
        }
    }

    bool answerFor(const Value* value, unsigned begin, unsigned end) const
    {
        if (begin == end)
            return value->returnsBool();
        if (value->opcode() == BitAnd)
            return m_answers.get(value->child(0)) || m_answers.get(value->child(1));
        for (unsigned index = begin; index < end; ++index) {
            if (!m_answers.get(value->child(index)))
                return false;
        }
        return true;
    }

    UncheckedKeyHashMap<Value*, bool> m_answers;
    Vector<Value*, 16> m_pending;
};

class BranchConditionSplitter {
public:
    BranchConditionSplitter(Procedure& proc)
        : m_proc(proc)
        , m_useCounts(proc)
        , m_indexInBlock(proc.values().size())
    {
    }

    bool split(BasicBlock* block)
    {
        if (!canSplit(block, block->last()->child(0)))
            return false;

        m_block = block;
        auto& values = block->values();
        for (unsigned index = 0; index < values.size(); ++index)
            m_indexInBlock[values[index]] = index;
        m_firstIndexThatChanges = values.size();
        m_endOfWrites = std::nullopt;

        makeBlocks();
        chooseBlocksOfValues();

        unsigned kept = m_firstIndexThatChanges;
        for (unsigned index = m_firstIndexThatChanges; index < values.size(); ++index) {
            Value* value = values[index];
            if (!value)
                continue;
            if (unsigned part = m_partOfValueAt[index])
                m_parts[part].block->append(value);
            else
                values[kept++] = value;
        }
        values.shrink(kept);
        for (unsigned part = 1; part < m_parts.size(); ++part)
            m_parts[part].block->append(m_parts[part].branch);
        return true;
    }

private:
    // An operand of the condition that is not split further. Parts are numbered in the order in which they are tested, and the first stays in the block.
    struct Part {
        BasicBlock* block;
        Value* branch;
        Value* condition;
        // The outermost operator that begins with this part is the right operand of one that begins with that part, and ends with lastPart.
        unsigned firstPartOfOuterOperator;
        unsigned lastPart;
    };

    struct Uses {
        unsigned count { 0 };
        unsigned firstPart { std::numeric_limits<unsigned>::max() };
    };

    bool canSplit(BasicBlock* block, Value* condition)
    {
        if ((condition->opcode() != BitAnd && condition->opcode() != BitOr) || condition->owner != block || m_useCounts.numUses(condition) != 1)
            return false;
        Value* first = condition->child(0);
        Value* second = condition->child(1);
        if (!m_booleans.contains(first) || !m_booleans.contains(second) || first->isConstant() || second->isConstant() || first == second)
            return false;
        if (m_useCounts.numUses(first) != 1 || m_useCounts.numUses(second) != 1)
            return false;
        bool isCompareChain = isSelectedAsCompareChain(m_proc, condition, [&](Value* operand) {
            return m_useCounts.numUses(operand) == 1 && !isOneBitTest(operand);
        });
        if (isCompareChain)
            return false;
        if (isEqualityWithConstant(first) && isEqualityWithConstant(second) && first->child(0) == second->child(0))
            return false;
        return true;
    }

    void makeBlocks()
    {
        struct Operand {
            BasicBlock* block;
            Value* branch;
            Value* condition;
            unsigned firstPartOfOuterOperator;
        };
        Value* branch = m_block->last();
        auto& values = m_block->values();
        m_parts.shrink(0);
        Vector<Operand, 16> operands;
        operands.append({ m_block, branch, branch->child(0), 0 });
        while (!operands.isEmpty()) {
            Operand operand = operands.takeLast();
            unsigned part = m_parts.size();
            Value* condition = operand.condition;
            while (canSplit(m_block, condition)) {
                FrequentedBlock taken = operand.block->successor(0);
                FrequentedBlock notTaken = operand.block->successor(1);
                BasicBlock* next = m_proc.addBlock();
                FrequentedBlock toNext(next, maxFrequency(taken.frequency(), notTaken.frequency()));
                Value* first = condition->child(0);
                Value* second = condition->child(1);
                operands.append({ next, m_proc.add<Value>(Branch, branch->origin(), second), second, part });
                next->setSuccessors(taken, notTaken);
                if (condition->opcode() == BitAnd)
                    operand.block->setSuccessors(toNext, notTaken);
                else
                    operand.block->setSuccessors(taken, toNext);
                unsigned index = m_indexInBlock[condition];
                values[index] = nullptr;
                m_firstIndexThatChanges = std::min(m_firstIndexThatChanges, index);
                m_proc.deleteValue(condition);
                condition = first;
            }
            operand.branch->child(0) = condition;
            m_parts.append({ operand.block, operand.branch, condition, operand.firstPartOfOuterOperator, part });
        }
        for (unsigned part = m_parts.size(); --part;) {
            Part& outer = m_parts[m_parts[part].firstPartOfOuterOperator];
            outer.lastPart = std::max(outer.lastPart, m_parts[part].lastPart);
        }
    }

    // `last` never falls from one call to the next, so an operator that ends in front of it is skipped for good.
    unsigned firstPartOfSmallestOperatorWith(unsigned first, unsigned last)
    {
        unsigned found = first;
        while (m_parts[found].lastPart < last)
            found = m_parts[found].firstPartOfOuterOperator;
        while (m_parts[first].lastPart < last)
            first = std::exchange(m_parts[first].firstPartOfOuterOperator, found);
        return found;
    }

    bool isInFrontOfWrite(unsigned index)
    {
        if (!m_endOfWrites) {
            auto& values = m_block->values();
            m_endOfWrites = 0;
            for (unsigned candidate = values.size() - 1; candidate--;) {
                if (values[candidate] && values[candidate]->effects().isWrite()) {
                    m_endOfWrites = candidate + 1;
                    break;
                }
            }
        }
        return index < *m_endOfWrites;
    }

    bool canSink(Value* value)
    {
        Effects effects = value->effects();
        if (effects.mustExecute() || effects.readsLocalState)
            return false;
        return !(effects.reads || effects.readsPinned) || !isInFrontOfWrite(m_indexInBlock[value]);
    }

    void chooseBlocksOfValues()
    {
        m_partOfValueAt.fill(0, m_block->size());
        m_uses.clear();
        for (unsigned part = 1; part < m_parts.size(); ++part) {
            use(m_parts[part].condition, part);
            while (!m_usedByPartsAlone.isEmpty()) {
                Value* value = m_usedByPartsAlone.takeLast();
                unsigned firstPart = m_uses.get(value).firstPart;
                unsigned partOfValue = firstPartOfSmallestOperatorWith(firstPart, part);
                if (!partOfValue || !canSink(value))
                    continue;
                unsigned index = m_indexInBlock[value];
                m_partOfValueAt[index] = partOfValue;
                m_firstIndexThatChanges = std::min(m_firstIndexThatChanges, index);
                for (Value* child : value->children())
                    use(child, firstPart);
            }
        }
    }

    void use(Value* value, unsigned firstPart)
    {
        if (value->owner != m_block)
            return;
        Uses& uses = m_uses.add(value, Uses { }).iterator->value;
        uses.firstPart = std::min(uses.firstPart, firstPart);
        if (++uses.count == m_useCounts.numUses(value))
            m_usedByPartsAlone.append(value);
    }

    Procedure& m_proc;
    UseCountsWithoutUsingInstructions m_useCounts;
    BooleanValues m_booleans;
    IndexMap<Value*, unsigned> m_indexInBlock;
    BasicBlock* m_block { nullptr };
    Vector<Part, 16> m_parts;
    Vector<unsigned> m_partOfValueAt;
    UncheckedKeyHashMap<Value*, Uses> m_uses;
    Vector<Value*, 16> m_usedByPartsAlone;
    unsigned m_firstIndexThatChanges { 0 };
    std::optional<unsigned> m_endOfWrites;
};

} // anonymous namespace

bool splitBranchConditions(Procedure& proc)
{
    PhaseScope phaseScope(proc, "splitBranchConditions"_s);
    Vector<BasicBlock*> worklist;
    for (BasicBlock* block : proc) {
        Value* last = block->last();
        if (last->opcode() == Branch && (last->child(0)->opcode() == BitAnd || last->child(0)->opcode() == BitOr))
            worklist.append(block);
    }
    if (worklist.isEmpty())
        return false;
    proc.resetValueOwners();
    BranchConditionSplitter splitter(proc);
    bool changed = false;
    while (!worklist.isEmpty())
        changed |= splitter.split(worklist.takeLast());
    if (changed) {
        proc.resetReachability();
        proc.invalidateCFG();
    }
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
