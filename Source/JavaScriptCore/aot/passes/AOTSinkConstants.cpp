/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTDominators.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PlannedCopy {
    ValueID original;
    ProgramPoint point;
    uint32_t firstUse;
    uint32_t numberOfUses;
};

class ConstantSinker {
public:
    explicit ConstantSinker(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_isSunk(context.function.values.size())
    {
    }

    PassOutcome run()
    {
        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        bool didSplitEdges = splitEdgesThatHandOnConstants(builder);
        planCopies();
        if (m_copies.isEmpty())
            return didSplitEdges ? PassOutcome::Changed : PassOutcome::Unchanged;

        insertCopies(builder);
        eraseOriginals(builder);
        return PassOutcome::Changed;
    }

private:
    bool isOnlyHandedOnOverSomeEdges(ValueID id, BlockID blockID)
    {
        const UseLists& useLists = m_analyses.useLists();
        if (useLists.locationOf(id).placement != ValuePlacement::InBlock || useLists.blockOf(id) != blockID || !isCandidate(id) || m_analyses.hasNoEffects(id))
            return false;
        BitVector targets;
        for (const Use& use : useLists.usesOf(id)) {
            if (use.kind != Use::Kind::EdgeArgument || use.block != blockID)
                return false;
            targets.set(use.targetIndex);
        }
        return targets.bitCount() < m_function.at(blockID).terminator.targets.size();
    }

    // A constant that nothing but the terminator of its own block uses, and that goes over some of the edges only, is created on those edges.
    bool splitEdgesThatHandOnConstants(IRBuilder& builder)
    {
        Vector<Edge, 4> edges;
        for (BlockID blockID : m_analyses.cfg().reversePostOrder()) {
            const Terminator& terminator = m_function.at(blockID).terminator;
            if (terminator.targets.size() < 2)
                continue;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                bool handsOnConstant = std::ranges::any_of(terminator.targets[targetIndex].arguments, [&](ValueID argument) {
                    return resultRuleOf(m_function.at(argument).opcode) == ResultRule::Constant && isOnlyHandedOnOverSomeEdges(argument, blockID);
                });
                if (handsOnConstant)
                    edges.append(Edge { blockID, targetIndex });
            }
        }
        for (Edge edge : edges)
            builder.splitEdge(edge);
        m_isSunk.ensureSize(m_function.values.size());
        return !edges.isEmpty();
    }

    void planCopies()
    {
        const UseLists& useLists = m_analyses.useLists();
        for (BlockID blockID : m_analyses.cfg().reversePostOrder()) {
            const Block& block = m_function.at(blockID);
            size_t numberOfEarlierCopies = m_copies.size();
            for (ValueID id : block.values) {
                if (!isCandidate(id))
                    continue;
                std::span<const Use> uses = useLists.usesOf(id);
                if (canSink(id, blockID, uses))
                    planCopiesOf(id, uses, useLists);
                else if (!tryPlanMove(id, blockID, uses, useLists))
                    continue;
                m_isSunk.quickSet(id.index());
            }
            if (m_copies.size() != numberOfEarlierCopies)
                m_definitionBlocks.append(blockID);
        }
    }

    bool isCandidate(ValueID id)
    {
        return resultRuleOf(m_function.at(id).opcode) == ResultRule::Constant
            && !m_analyses.effectsOf(id).onAnyEdge().interferesWith(Effects::unknownCode());
    }

    // What a block hands to its handler, or is entered with, has to be there at its head. An immediate that a terminator names costs nothing wherever it is defined.
    bool canTakeCopy(const Use& use, ValueID id)
    {
        if (use.kind == Use::Kind::Operand)
            return true;
        return (use.kind == Use::Kind::TerminatorOperand || use.kind == Use::Kind::EdgeArgument) && !m_analyses.hasNoEffects(id);
    }

    bool runsMoreOftenThan(BlockID user, BlockID definitionBlock)
    {
        return m_analyses.isInCycle(user) && !m_analyses.blockCycles().isInEveryCycleOf(definitionBlock, user);
    }

    bool canSink(ValueID id, BlockID definitionBlock, std::span<const Use> uses)
    {
        if (uses.empty())
            return false;
        m_userBlocks.shrink(0);
        for (const Use& use : uses) {
            if (!canTakeCopy(use, id) || use.block == definitionBlock || runsMoreOftenThan(use.block, definitionBlock))
                return false;
            if (m_userBlocks.isEmpty() || m_userBlocks.last() != use.block)
                m_userBlocks.append(use.block);
        }
        if (m_userBlocks.size() == 1)
            return true;
        return !isRematerializable(m_function.at(id)) && hasExclusiveUserBlocks(id);
    }

    const StronglyConnectedComponents& components()
    {
        if (!m_components) {
            const CFG& cfg = m_analyses.cfg();
            m_components = computeStronglyConnectedComponents(cfg.numNodes(), [&](unsigned index) {
                return cfg.successors(BlockID(index));
            });
        }
        return *m_components;
    }

    bool hasExclusiveUserBlocks(ValueID candidate)
    {
        const CFG& cfg = m_analyses.cfg();
        const Vector<uint32_t>& componentOf = components().componentOf;
        if (m_blockStamps.isEmpty())
            m_blockStamps.fill(ValueID(), m_function.blocks.size());
        uint32_t minComponent = std::numeric_limits<uint32_t>::max();
        Vector<BlockID, 16> worklist;
        for (BlockID block : m_userBlocks) {
            minComponent = std::min(minComponent, componentOf[block.index()]);
            worklist.append(block);
        }
        while (!worklist.isEmpty()) {
            for (BlockID target : cfg.successors(worklist.takeLast())) {
                if (componentOf[target.index()] < minComponent || m_blockStamps[target.index()] == candidate)
                    continue;
                m_blockStamps[target.index()] = candidate;
                worklist.append(target);
            }
        }
        return !m_userBlocks.containsIf([&](BlockID block) { return m_blockStamps[block.index()] == candidate; });
    }

    // Where the users lie in a cycle or reach each other, one value serves them all: it is created in the lowest block outside of every cycle that dominates them.
    bool tryPlanMove(ValueID original, BlockID definitionBlock, std::span<const Use> uses, const UseLists& useLists)
    {
        if (uses.empty() || isRematerializable(m_function.at(original)) || m_analyses.isInCycle(definitionBlock))
            return false;
        const Dominators& dominators = m_analyses.dominators();
        BlockID target;
        for (const Use& use : uses) {
            if (!canTakeCopy(use, original) || use.block == definitionBlock)
                return false;
            target = target ? dominators.nearestCommonDominator(target, use.block) : use.block;
        }
        // What a block with a handler creates is not there yet when the handler is entered.
        auto servesEveryUser = [&](BlockID block) {
            return !m_analyses.isInCycle(block) && std::ranges::all_of(uses, [&](const Use& use) {
                return use.block == block || dominators.terminatorDominates(block, use.block);
            });
        };
        while (target != definitionBlock && !servesEveryUser(target))
            target = dominators.idom(target);
        if (target == definitionBlock)
            return false;

        auto firstUseInTarget = std::ranges::find_if(uses, [&](const Use& use) { return use.block == target; });
        ProgramPoint point { target, static_cast<uint32_t>(m_function.at(target).values.size()) };
        if (firstUseInTarget != uses.end())
            point = useLists.pointOf(*firstUseInTarget);
        uint32_t firstUse = m_uses.size();
        m_uses.append(uses);
        m_copies.append(PlannedCopy { original, point, firstUse, static_cast<uint32_t>(uses.size()) });
        return true;
    }

    void planCopiesOf(ValueID original, std::span<const Use> uses, const UseLists& useLists)
    {
        uint32_t firstUse = m_uses.size();
        m_uses.append(uses);
        while (!uses.empty()) {
            BlockID block = uses.front().block;
            uint32_t numberOfUses = std::ranges::find_if(uses, [&](const Use& use) { return use.block != block; }) - uses.begin();
            m_copies.append(PlannedCopy { original, useLists.pointOf(uses.front()), firstUse, numberOfUses });
            firstUse += numberOfUses;
            uses = uses.subspan(numberOfUses);
        }
    }

    void insertCopies(IRBuilder& builder)
    {
        for (const PlannedCopy& planned : m_copies) {
            builder.setOrigin(m_function.at(planned.original).origin());
            ValueID copy = builder.insertBefore(planned.point, Value(m_function.at(planned.original)));
            for (const Use& use : m_uses.subspan(planned.firstUse, planned.numberOfUses))
                replaceUse(m_function, use, copy);
        }
        builder.applyInsertions();
    }

    void eraseOriginals(IRBuilder& builder)
    {
        for (BlockID block : m_definitionBlocks) {
            const BlockValues& values = m_function.at(block).values;
            BitVector indexes;
            for (size_t index = 0; index < values.size(); ++index) {
                if (m_isSunk.get(values[index].index()))
                    indexes.set(index);
            }
            builder.eraseValues(block, indexes);
        }
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    BitVector m_isSunk;
    std::optional<StronglyConnectedComponents> m_components;
    Vector<BlockID, 4> m_userBlocks;
    Vector<ValueID> m_blockStamps;
    Vector<PlannedCopy> m_copies;
    Vector<Use> m_uses;
    Vector<BlockID> m_definitionBlocks;
};

} // anonymous namespace

PassOutcome sinkConstants(FunctionPassContext& context)
{
    if (context.function.blocks.size() < 2)
        return PassOutcome::Unchanged;
    return ConstantSinker(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
