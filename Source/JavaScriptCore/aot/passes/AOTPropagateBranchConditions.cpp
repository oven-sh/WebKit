/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTDecidedBranches.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSort.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DecidingEdge {
    Edge edge;
    bool isTaken;
    ValueID constant;
};

struct DecidedUse {
    Use use;
    unsigned edgeIndex;
};

struct TruthinessTest {
    ValueID tested;
    ValueID test;
    BlockID block;
};

class BranchConditionPropagator {
public:
    explicit BranchConditionPropagator(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
    {
    }

    PassOutcome run()
    {
        bool hasMergedTests = mergeRepeatedTruthinessTests();
        collectDecidedUses();
        if (m_uses.isEmpty())
            return hasMergedTests ? PassOutcome::Changed : PassOutcome::Unchanged;

        insertConstants();
        for (const DecidedUse& decided : m_uses)
            replaceUse(m_function, decided.use, m_edges[decided.edgeIndex].constant);
        m_analyses.didChangeValues();
        m_context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

private:
    // Whether a value is truthy never changes, so a test that another test of the same value dominates is that test.
    bool mergeRepeatedTruthinessTests()
    {
        if (!m_analyses.opcodesOfValues().contains(Opcode::ToBoolean))
            return false;
        Vector<TruthinessTest, 16> tests;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (m_function.at(id).opcode == Opcode::ToBoolean)
                    tests.append({ m_function.at(id).operand(0), id, block });
            }
        }
        if (tests.size() < 2)
            return false;
        AOT::stableSort(tests, { }, &TruthinessTest::tested);

        bool hasMerged = false;
        size_t firstKept = 0;
        size_t endOfKept = 0;
        for (const TruthinessTest& test : tests) {
            if (endOfKept == firstKept || tests[firstKept].tested != test.tested)
                firstKept = endOfKept;
            auto kept = tests.span().subspan(firstKept, endOfKept - firstKept);
            auto dominating = std::ranges::find_if(kept, [&](const TruthinessTest& earlier) {
                return m_analyses.dominators().dominates(earlier.block, test.block);
            });
            if (dominating == kept.end()) {
                tests[endOfKept++] = test;
                continue;
            }
            for (const Use& use : m_analyses.useLists().usesOf(test.test))
                replaceUse(m_function, use, dominating->test);
            hasMerged = true;
        }
        if (hasMerged) {
            m_analyses.didChangeValues();
            m_context.keptAnalyses = allAnalyses;
        }
        return hasMerged;
    }

    void collectDecidedUses()
    {
        BitVector visitedConditions;
        BitVector isUndecided(m_function.values.size());
        Vector<ValueID, 16> undecidedConditions;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            const Terminator& terminator = m_function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch || !visitedConditions.add(terminator.conditionOperand().index()))
                continue;
            const Value& condition = m_function.at(terminator.conditionOperand());
            if (condition.opcode != Opcode::Boolean && condition.type == TypeTable::boolean) {
                isUndecided.quickSet(terminator.conditionOperand().index());
                undecidedConditions.append(terminator.conditionOperand());
            }
        }
        if (undecidedConditions.isEmpty())
            return;
        if (!m_analyses.computed().contains(FunctionAnalysis::UseLists) && !isAnyUsedTwice(isUndecided))
            return;
        for (ValueID condition : undecidedConditions)
            collectDecidedUsesOf(condition);
    }

    bool isAnyUsedTwice(const BitVector& values)
    {
        BitVector isUsed(m_function.values.size());
        bool isUsedTwice = false;
        auto addUse = [&](ValueID used) {
            if (used && values.quickGet(used.index()))
                isUsedTwice |= isUsed.quickSet(used.index());
        };
        for (BlockID id : m_analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(id);
            for (ValueID value : block.values)
                m_function.at(value).forEachChild(addUse);
            block.forEachChildOutsideValues(addUse);
            if (isUsedTwice)
                return true;
        }
        return false;
    }

    void collectDecidedUsesOf(ValueID condition)
    {
        std::span<const Use> uses = m_analyses.useLists().usesOf(condition);
        if (uses.size() < 2)
            return;

        unsigned firstEdgeIndex = m_edges.size();
        for (const Use& use : uses) {
            if (use.kind == Use::Kind::TerminatorOperand)
                collectDecidingEdges(use.block);
        }
        for (const Use& use : uses) {
            if (isConditionOfProvingBranch(use))
                continue;
            if (std::optional<unsigned> edgeIndex = findDecidingEdge(firstEdgeIndex, use))
                m_uses.append({ use, *edgeIndex });
        }
    }

    bool isConditionOfProvingBranch(const Use& use) const
    {
        if (use.kind != Use::Kind::TerminatorOperand || !m_function.at(use.block).terminator.isConditionalBranch())
            return false;
        return hasEdgeResults(m_function, Edge { use.block, 0 }) || hasEdgeResults(m_function, Edge { use.block, 1 });
    }

    std::optional<unsigned> findDecidingEdge(unsigned firstEdgeIndex, const Use& use)
    {
        for (unsigned edgeIndex = firstEdgeIndex; edgeIndex < m_edges.size(); ++edgeIndex) {
            if (canUseValuesOf(targetOf(m_function, m_edges[edgeIndex].edge).block, use))
                return edgeIndex;
        }
        return std::nullopt;
    }

    bool canUseValuesOf(BlockID block, const Use& use)
    {
        if (use.block != block)
            return m_analyses.dominators().terminatorDominates(block, use.block);

        switch (use.kind) {
        case Use::Kind::Operand:
        case Use::Kind::TerminatorOperand:
        case Use::Kind::EdgeArgument:
            return true;
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void collectDecidingEdges(BlockID block)
    {
        const Terminator& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            return;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
            Edge edge { block, targetIndex };
            const Target& target = terminator.targets[targetIndex];
            if (m_analyses.cfg().singleIncomingEdge(target.block) == edge)
                m_edges.append({ edge, &target == &terminator.taken(), ValueID() });
        }
    }

    void insertConstants()
    {
        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        for (const DecidedUse& decided : m_uses) {
            DecidingEdge& deciding = m_edges[decided.edgeIndex];
            if (deciding.constant)
                continue;
            ValueID condition = m_function.at(deciding.edge.from).terminator.conditionOperand();
            BlockID target = targetOf(m_function, deciding.edge).block;
            Value constant = IRBuilder::make(Opcode::Boolean);
            constant.setBoolean(deciding.isTaken);
            builder.setOrigin(m_function.at(condition).origin());
            deciding.constant = builder.insertBefore({ target, indexAfterCaughtValues(m_function, m_function.at(target)) }, WTF::move(constant));
        }
        builder.applyInsertions();
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    Vector<DecidingEdge, 8> m_edges;
    Vector<DecidedUse, 8> m_uses;
};

} // anonymous namespace

PassOutcome propagateBranchConditions(FunctionPassContext& context)
{
    return BranchConditionPropagator(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
