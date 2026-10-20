/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

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
        planCopies();
        if (m_copies.isEmpty())
            return PassOutcome::Unchanged;

        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        insertCopies(builder);
        eraseOriginals(builder);
        return PassOutcome::Changed;
    }

private:
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
                if (!canSink(id, blockID, uses))
                    continue;
                planCopiesOf(id, uses, useLists);
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

    bool canSink(ValueID id, BlockID definitionBlock, std::span<const Use> uses)
    {
        if (uses.empty())
            return false;
        m_userBlocks.shrink(0);
        for (const Use& use : uses) {
            if (use.kind != Use::Kind::Operand || use.block == definitionBlock || m_analyses.blockCycles().isInCycle(use.block))
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
                m_function.at(use.user).setOperand(use.operandIndex, copy);
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
