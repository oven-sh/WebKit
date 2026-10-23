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
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct MergeCandidate {
    BlockID target;
    bool isLatch;
    unsigned hash;
    BlockID block;

    bool isInSameGroupAs(const MergeCandidate& other) const { return target == other.target && isLatch == other.isLatch && hash == other.hash; }
};

struct Duplicate {
    BlockID block;
    BlockID representative;
};

struct Redirection {
    Edge edge;
    BlockID representative;
};

class DuplicateBlockMerger {
public:
    explicit DuplicateBlockMerger(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_cfg(context.analyses.cfg())
    {
    }

    PassOutcome run()
    {
        collectCandidates();
        findDuplicates();
        if (m_duplicates.isEmpty())
            return PassOutcome::Unchanged;

        rewrite();
        return PassOutcome::Changed;
    }

private:
    const Arguments& argumentsOf(BlockID block) const { return m_function.at(block).terminator.next().arguments; }
    bool isOwnValue(BlockID block, ValueID value) const { return m_function.at(block).values.contains(value); }

    bool hasFormOfCandidate(BlockID id) const
    {
        const Block& block = m_function.at(id);
        if (block.terminator.kind != TerminatorKind::Jump || !block.parameters.isEmpty() || block.values.isEmpty() || block.isHandlerBlock())
            return false;
        const Target& target = block.terminator.next();
        if (target.arguments.isEmpty())
            return false;
        return std::ranges::all_of(block.values, [&](ValueID value) {
            return isRematerializable(m_function.at(value));
        });
    }

    unsigned hashOf(BlockID block) const
    {
        Hasher hasher;
        for (ValueID argument : argumentsOf(block)) {
            if (!isOwnValue(block, argument)) {
                add(hasher, argument);
                continue;
            }
            const Value& constant = m_function.at(argument);
            add(hasher, constant.opcode, constant.type);
            constant.addPayloadTo(hasher);
        }
        return hasher.hash();
    }

    void collectCandidates()
    {
        Vector<std::pair<BlockID, BlockID>, 8> blocksByTarget;
        for (BlockID block : m_cfg.reversePostOrder()) {
            if (hasFormOfCandidate(block))
                blocksByTarget.append({ m_function.at(block).terminator.next().block, block });
        }
        if (blocksByTarget.size() < 2)
            return;
        AOT::sort(blocksByTarget);
        for (size_t index = 0; index < blocksByTarget.size(); ++index) {
            auto [target, block] = blocksByTarget[index];
            bool isOnlyOneWithTarget = (!index || blocksByTarget[index - 1].first != target) && (index + 1 == blocksByTarget.size() || blocksByTarget[index + 1].first != target);
            if (isOnlyOneWithTarget || m_analyses.dominators().dominates(block, target))
                continue;
            m_candidates.append({ target, m_analyses.dominators().dominates(target, block), hashOf(block), block });
        }
        AOT::sort(m_candidates, [](const MergeCandidate& a, const MergeCandidate& b) {
            return std::tie(a.target, a.isLatch, a.hash, a.block) < std::tie(b.target, b.isLatch, b.hash, b.block);
        });
    }

    static bool areEqualConstants(const Value& a, const Value& b)
    {
        return a.opcode == b.opcode && a.type == b.type && a.expected() == b.expected() && a.hasSamePayloadAs(b);
    }

    bool areEqual(BlockID a, BlockID b) const
    {
        const Arguments& argumentsOfA = argumentsOf(a);
        const Arguments& argumentsOfB = argumentsOf(b);
        if (argumentsOfA.size() != argumentsOfB.size())
            return false;
        for (unsigned position = 0; position < argumentsOfA.size(); ++position) {
            ValueID argumentOfA = argumentsOfA[position];
            ValueID argumentOfB = argumentsOfB[position];
            bool isOwnOfA = isOwnValue(a, argumentOfA);
            if (isOwnOfA != isOwnValue(b, argumentOfB))
                return false;
            if (!isOwnOfA) {
                if (argumentOfA != argumentOfB)
                    return false;
                continue;
            }
            if (!areEqualConstants(m_function.at(argumentOfA), m_function.at(argumentOfB)))
                return false;
        }
        return true;
    }

    void findDuplicates()
    {
        Vector<BlockID, 4> representatives;
        for (size_t index = 0; index < m_candidates.size(); ++index) {
            const MergeCandidate& candidate = m_candidates[index];
            if (index && !m_candidates[index - 1].isInSameGroupAs(candidate))
                representatives.shrink(0);
            auto representative = std::ranges::find_if(representatives, [&](BlockID other) {
                return areEqual(other, candidate.block);
            });
            if (representative == representatives.end())
                representatives.append(candidate.block);
            else
                m_duplicates.append({ candidate.block, *representative });
        }
    }

    void rewrite()
    {
        Vector<Redirection, 8> redirections;
        Vector<BlockID, 4> uncoveredRepresentatives;
        for (const Duplicate& duplicate : m_duplicates) {
            for (const Edge& edge : m_cfg.incomingEdges(duplicate.block)) {
                ASSERT(!edge.isHandlerEdge() && !hasEdgeResults(m_function, edge));
                redirections.append({ edge, duplicate.representative });
            }
            Block& representative = m_function.edit(duplicate.representative);
            const Block& block = m_function.at(duplicate.block);
            representative.frequency = moreFrequentOf(representative.frequency, block.frequency);
            if (representative.handler && representative.handler != block.handler)
                uncoveredRepresentatives.append(duplicate.representative);
        }
        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        for (BlockID representative : uncoveredRepresentatives)
            builder.removeHandler(representative);
        for (const Redirection& redirection : redirections)
            builder.redirectEdge(redirection.edge, Target { redirection.representative, { }, { } });
        // The builder reports the edges, not the frequency of a representative.
        m_analyses.didChangeControlFlow();
        m_context.keptAnalyses = allAnalyses;
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    Vector<MergeCandidate, 8> m_candidates;
    Vector<Duplicate, 8> m_duplicates;
};

} // anonymous namespace

PassOutcome mergeDuplicateBlocks(FunctionPassContext& context)
{
    return DuplicateBlockMerger(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
