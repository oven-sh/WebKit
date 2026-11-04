/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3EstimateFrequencies.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3Common.h"
#include "B3CycleForest.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <ranges>
#include <wtf/FrequencyEstimator.h>

namespace JSC { namespace B3 {

namespace {

class FrequencyGraph {
public:
    using Graph = CFG;
    using Node = BasicBlock*;

    explicit FrequencyGraph(Procedure& proc)
        : m_proc(proc)
    {
    }

    Graph& graph() { return m_proc.cfg(); }
    unsigned numSuccessors(Node block) { return block->numSuccessors(); }
    Node successor(Node block, unsigned index) { return block->successorBlock(index); }

    WTF::EdgeClass edgeClass(Node block, unsigned index)
    {
        switch (block->successor(index).frequency()) {
        case FrequencyClass::Normal:
            return WTF::EdgeClass::Normal;
        case FrequencyClass::Unlikely:
            return WTF::EdgeClass::Unlikely;
        case FrequencyClass::Rare:
            return WTF::EdgeClass::Rare;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void setEdgeClass(Node block, unsigned index, WTF::EdgeClass value)
    {
        FrequencyClass& frequency = block->successor(index).frequency();
        switch (value) {
        case WTF::EdgeClass::Normal:
            frequency = FrequencyClass::Normal;
            return;
        case WTF::EdgeClass::Unlikely:
            frequency = FrequencyClass::Unlikely;
            return;
        case WTF::EdgeClass::Rare:
            frequency = FrequencyClass::Rare;
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    double initialWeight(Node block, const WTF::FrequencyEstimatorConstants& constants)
    {
        return block->last()->opcode() == Oops ? constants.rareEdgeShare : 1;
    }

    double weightCapAfterPropagation(Node block, const WTF::FrequencyEstimatorConstants& constants)
    {
        if (m_proc.policy().optimization.returnsAreUnlikely && block->last()->opcode() == Return)
            return constants.returningNodeWeight;
        return 1;
    }

    bool entersEverySuccessor(Node block) { return block->last()->opcode() == EntrySwitch; }

    template<typename Functor>
    void forEachDecidedPredecessor(Node block, const Functor& functor)
    {
        if (block->last()->opcode() != Branch)
            return;
        Vector<Opcode, 4> inversions;
        Value* phi = block->last()->child(0);
        for (; invertsBoolean(phi); phi = phi->child(0))
            inversions.append(phi->opcode());
        if (phi->opcode() != Phi || phi->owner != block)
            return;
        for (BasicBlock* predecessor : block->predecessors()) {
            if (predecessor->numSuccessors() != 1)
                continue;
            UpsilonValue* upsilon = findLastUpsilon(predecessor, phi);
            if (!upsilon || !upsilon->child(0)->hasInt32())
                continue;
            int32_t condition = upsilon->child(0)->asInt32();
            for (Opcode opcode : inversions | std::views::reverse)
                condition = opcode == Equal ? !condition : condition ^ 1;
            functor(predecessor, condition ? 0u : 1u);
        }
    }

private:
    static bool invertsBoolean(Value* value)
    {
        return (value->opcode() == Equal && value->child(1)->isInt32(0)) || (value->opcode() == BitXor && value->child(1)->isInt32(1));
    }

    static UpsilonValue* findLastUpsilon(BasicBlock* block, Value* phi)
    {
        for (unsigned index = block->size(); index--;) {
            auto* upsilon = block->at(index)->as<UpsilonValue>();
            if (upsilon && upsilon->phi() == phi)
                return upsilon;
        }
        return nullptr;
    }

    Procedure& m_proc;
};

class FrequencyEstimateOfProcedure final : public FrequencyEstimate {
public:
    explicit FrequencyEstimateOfProcedure(Procedure& proc)
        : m_proc(proc)
        , m_cycles(proc)
        , m_graph(proc)
        , m_estimator(m_graph, m_cycles)
    {
        m_estimator.estimateWeights();
        if (shouldValidateIRAtEachPhase(proc))
            m_estimatedBlocks = collectBlocks();
    }

    void setBlockFrequencies() final
    {
        if (shouldValidateIRAtEachPhase(m_proc))
            RELEASE_ASSERT(m_estimatedBlocks == collectBlocks());
        m_estimator.estimateFrequencies();
        for (BasicBlock* block : m_proc)
            block->setFrequency(static_cast<float>(m_estimator.frequencyOf(block)));
    }

private:
    struct EstimatedBlock {
        friend bool operator==(const EstimatedBlock&, const EstimatedBlock&) = default;

        BasicBlock* block;
        unsigned index;
        BasicBlock::SuccessorList successors;
        bool entersEverySuccessor;
    };

    Vector<EstimatedBlock> collectBlocks()
    {
        Vector<EstimatedBlock> result;
        for (BasicBlock* block : m_proc)
            result.append(EstimatedBlock { block, block->index(), block->successors(), m_graph.entersEverySuccessor(block) });
        return result;
    }

    Procedure& m_proc;
    CycleForest m_cycles;
    FrequencyGraph m_graph;
    WTF::FrequencyEstimator<FrequencyGraph> m_estimator;
    Vector<EstimatedBlock> m_estimatedBlocks;
};

} // anonymous namespace

std::unique_ptr<FrequencyEstimate> FrequencyEstimate::create(Procedure& proc)
{
    PhaseScope phaseScope(proc, "estimateFrequencies"_s);
    proc.resetValueOwners();
    return makeUnique<FrequencyEstimateOfProcedure>(proc);
}

bool estimateFrequencies(Procedure& proc)
{
    FrequencyEstimate::create(proc)->setBlockFrequencies();
    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
