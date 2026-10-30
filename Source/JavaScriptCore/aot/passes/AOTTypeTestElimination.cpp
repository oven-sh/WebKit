/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTProgramPasses.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRuleSupport.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/BitSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Where the conditions of branches hold: the answers of TestCoverage over TestEvents for requirements that only ask, without a budget, with slow paths that come back.
class ConditionCoverage {
    WTF_MAKE_NONCOPYABLE(ConditionCoverage);
public:
    ConditionCoverage(FunctionAnalyses& analyses, const TestConditions& conditions, std::span<const TestRequirement> requirements, std::span<const ExistingProof> proofs)
        : m_analyses(analyses)
        , m_conditions(conditions)
        , m_requirements(requirements)
    {
        RELEASE_ASSERT(conditions.size() <= Row::size());
        collectProofs(proofs);
        findWhereConditionsMatter();
    }

    void solve(const BitVector* blocksWithIgnoredFailingEdge)
    {
        m_blocksWithIgnoredFailingEdge = blocksWithIgnoredFailingEdge;
        m_didSolveFromProofs = solveFromProofs();
        if (!m_didSolveFromProofs)
            solveEveryBlock();
    }

    RequirementCoverage of(uint32_t requirement)
    {
        ProgramPoint point = m_requirements[requirement].point;
        if (m_didSolveFromProofs ? isDead(point.block) : !m_isReached.get(point.block.index()))
            return RequirementCoverage::Unknown;
        Row row;
        if (!m_didSolveFromProofs || wasVisited(point.block))
            row = m_heads[point.block.index()];
        carryThrough(point.block, row, point.index);
        return row.get(m_requirements[requirement].condition.index()) ? RequirementCoverage::Covered : RequirementCoverage::NeedsTest;
    }

private:
    // A bit for each condition. A build can only lower the limit.
    using Row = WTF::BitSet<256>;
    static_assert(defaultLimitValue(LimitID::MaxTestedTypesPerFunction, Architecture::ARM64) <= Row::size());
    static_assert(defaultLimitValue(LimitID::MaxTestedTypesPerFunction, Architecture::X86_64) <= Row::size());

    // In the order in which they act where they have one index. A requirement stands between Barrier and Suspension.
    enum class ItemKind : uint8_t { ParameterProof, Barrier, Suspension, Ending, ValueProof };

    struct Item {
        uint32_t index { 0 };
        ItemKind kind { ItemKind::Barrier };
        uint32_t payload { 0 };
    };

    struct ItemsOfBlock {
        uint32_t first { UINT32_MAX };
        uint32_t end { 0 };
    };

    // The conditions that the writer was asked about, and those among them that it may end.
    struct Ending {
        Row asked;
        Row ended;
    };

    struct PlacedProof {
        BlockID block;
        // On an edge the index of its target. In a block zero for a parameter, and for a value one more than its index.
        uint32_t place { 0 };
        uint32_t condition { 0 };
    };

    struct PlacedProofs {
        Vector<PlacedProof, 8> sorted;
        BitVector blocks;

        std::span<const PlacedProof> in(BlockID block) const
        {
            if (!blocks.quickGet(block.index()))
                return { };
            return recordsWith(sorted.span(), block.index(), [](const PlacedProof& proof) { return proof.block.index(); });
        }
    };

    // Each number says something of the solve that it names.
    struct StateOfBlock {
        uint32_t solveThatVisited { 0 };
        uint32_t solveThatFoundDead { 0 };
        uint32_t solveThatCountedDeadEdges { 0 };
        uint32_t numberOfDeadEdges { 0 };
    };

    static constexpr uint32_t failingTargetIndex = 1;

    bool wasVisited(BlockID block) const { return m_states[block.index()].solveThatVisited == m_solve; }
    bool isDead(BlockID block) const { return m_states[block.index()].solveThatFoundDead == m_solve; }
    bool isLeftOut(Edge edge) const { return m_blocksWithIgnoredFailingEdge && !edge.isHandlerEdge() && edge.targetIndex == failingTargetIndex && m_blocksWithIgnoredFailingEdge->get(edge.from.index()); }
    void markAsPending(BlockID block) { m_isPending.quickSet(m_cfg.reversePostOrderIndex(block)); }

    void collectProofs(std::span<const ExistingProof> proofs)
    {
        auto add = [&](PlacedProofs& list, BlockID block, uint32_t place, TestConditionID condition) {
            if (!m_cfg.isReachable(block))
                return;
            list.sorted.append(PlacedProof { block, place, condition.index() });
            list.blocks.quickSet(block.index());
        };
        for (const ExistingProof& proof : proofs) {
            if (isOnEdge(proof.source)) {
                add(m_edgeProofs, proof.edge.from, proof.edge.targetIndex, proof.condition);
                continue;
            }
            ValueLocation location = m_analyses.useLists().locationOf(proof.value);
            if (location.placement != ValuePlacement::Detached)
                add(m_pointProofs, location.block, location.placement == ValuePlacement::Parameter ? 0 : location.index + 1, proof.condition);
        }
        for (PlacedProofs* list : { &m_edgeProofs, &m_pointProofs })
            AOT::stableSortInLine(list->sorted, { }, [](const PlacedProof& proof) { return std::pair { proof.block.index(), proof.place }; });
    }

    // Nobody asks for a condition behind its last requirement.
    void findWhereConditionsMatter()
    {
        m_lastRequirement.fill(0, m_conditions.size());
        for (const TestRequirement& requirement : m_requirements) {
            if (!m_cfg.isReachable(requirement.point.block))
                continue;
            uint32_t& last = m_lastRequirement[requirement.condition.index()];
            last = std::max(last, m_cfg.reversePostOrderIndex(requirement.point.block));
        }
        if (!m_cfg.hasCycle())
            return;

        std::span<const BlockID> order = m_cfg.reversePostOrder();
        m_firstPositionReachedFrom = Vector<uint32_t>(order.size(), [](size_t position) {
            return static_cast<uint32_t>(position);
        });
        for (uint32_t position = 0; position < order.size(); ++position) {
            for (BlockID successor : m_cfg.successors(order[position]))
                m_firstPositionReachedFrom[position] = std::min(m_firstPositionReachedFrom[position], m_cfg.reversePostOrderIndex(successor));
        }
        uint32_t firstTarget = UINT32_MAX;
        for (uint32_t position = order.size(); position--;) {
            firstTarget = std::min(firstTarget, m_firstPositionReachedFrom[position]);
            m_firstPositionReachedFrom[position] = std::min(firstTarget, position);
        }
        for (uint32_t position = 0; position < order.size(); ++position)
            m_firstPositionReachedFrom[position] = m_firstPositionReachedFrom[m_firstPositionReachedFrom[position]];
    }

    // As TestEvents has it where no requirement places a test.
    bool isBarrier(BlockID block, ValueID producer)
    {
        return readsAsDefinition(m_analyses.program(), m_function, m_function.at(producer)) && std::ranges::any_of(m_analyses.useLists().usesOf(producer), [&](const Use& use) {
            return use.block != block || (use.kind != Use::Kind::Operand && use.kind != Use::Kind::TerminatorOperand);
        });
    }

    void collectItemsOf(BlockID id)
    {
        const Block& block = m_function.at(id);
        const OpcodeSet& opcodesThatMayEnd = opcodesThatMayEndProofs();
        bool checksScreens = m_analyses.context().build.options().verifiesAfterEveryPass;
        uint32_t first = m_items.size();

        std::span<const PlacedProof> proofs = m_pointProofs.in(id);
        auto appendProofsAt = [&](uint32_t place, uint32_t index, ItemKind kind) {
            for (; !proofs.empty() && proofs.front().place == place; proofs = proofs.subspan(1))
                m_items.append(Item { index, kind, proofs.front().condition });
        };
        auto appendWhatEndsProofs = [&](uint32_t index, const Effects& effects) {
            if (effects.maySuspend)
                m_items.append(Item { index, ItemKind::Suspension, 0 });
            else if (effects.writes.containsAny(m_conditions.heapsDependedOn()) && !m_analyses.context().build.isSabotaged(Sabotage::RemovedTestsOutlastWrites)) {
                m_items.append(Item { index, ItemKind::Ending, static_cast<uint32_t>(m_endings.size()) });
                m_endings.append(Ending { });
            }
        };

        appendProofsAt(0, 0, ItemKind::ParameterProof);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID value = block.values[index];
            if (isBarrier(id, value))
                m_items.append(Item { index, ItemKind::Barrier, 0 });
            if (opcodesThatMayEnd.contains(m_function.at(value).opcode))
                appendWhatEndsProofs(index, m_analyses.effectsOf(value).normal);
            else if (checksScreens) [[unlikely]] {
                const Effects& effects = m_analyses.effectsOf(value).normal;
                RELEASE_ASSERT(!effects.maySuspend && effects.writes.isEmpty());
            }
            appendProofsAt(index + 1, index, ItemKind::ValueProof);
        }
        RELEASE_ASSERT(proofs.empty());
        appendWhatEndsProofs(block.values.size(), m_analyses.effectsOfTerminator(id).normal);
        m_itemsOfBlocks[id.index()] = { first, static_cast<uint32_t>(m_items.size()) };
    }

    void askWriter(BlockID block, const Item& item, const Row& conditions)
    {
        if (!m_groups)
            m_groups.emplace(m_analyses, m_conditions);
        Vector<uint32_t, 16> stretchConditions;
        conditions.forEachSetBit([&](size_t index) {
            if (m_conditions.stretchConditions().get(index))
                stretchConditions.append(index);
        });
        const BlockValues& values = m_function.at(block).values;
        BitVector ended = item.index == values.size()
            ? m_groups->findConditionsEndedByTerminator(m_analyses.effectsOfTerminator(block).normal, stretchConditions.span())
            : m_groups->findConditionsEndedBy(values[item.index], m_analyses.effectsOf(values[item.index]), stretchConditions.span());
        m_endings[item.payload].asked.merge(conditions);
        for (size_t index : ended)
            m_endings[item.payload].ended.set(index);
    }

    // Up to the place of a requirement with that index.
    void carryThrough(BlockID block, Row& row, uint32_t endIndex)
    {
        if (row.isEmpty() && !m_pointProofs.blocks.quickGet(block.index()))
            return;
        if (m_itemsOfBlocks[block.index()].first == UINT32_MAX)
            collectItemsOf(block);

        ItemsOfBlock range = m_itemsOfBlocks[block.index()];
        for (uint32_t itemIndex = range.first; itemIndex < range.end; ++itemIndex) {
            Item item = m_items[itemIndex];
            if (item.index > endIndex || (item.index == endIndex && item.kind > ItemKind::Barrier))
                return;
            switch (item.kind) {
            case ItemKind::ParameterProof:
            case ItemKind::ValueProof:
                row.set(item.payload);
                break;
            case ItemKind::Barrier:
            case ItemKind::Suspension:
                row.clearAll();
                break;
            case ItemKind::Ending: {
                Row unasked = row;
                unasked.exclude(m_endings[item.payload].asked);
                if (!unasked.isEmpty())
                    askWriter(block, item, unasked);
                row.exclude(m_endings[item.payload].ended);
                break;
            }
            }
        }
    }

    // From the head of a block to its end.
    void carryThroughWholeBlock(BlockID block, uint32_t position, Row& row)
    {
        uint32_t firstPositionReached = m_firstPositionReachedFrom.isEmpty() ? position : m_firstPositionReachedFrom[position];
        row.forEachSetBit([&](size_t condition) {
            if (m_lastRequirement[condition] < firstPositionReached)
                row.clear(condition);
        });
        carryThrough(block, row, UINT32_MAX);
    }

    // Nothing holds in a handler. Elsewhere what held at the end of the source does, and what the edge proves.
    Row rowOn(Edge edge, const Row& atEndOfSource) const
    {
        if (edge.isHandlerEdge())
            return { };
        Row row = atEndOfSource;
        for (const PlacedProof& proof : m_edgeProofs.in(edge.from)) {
            if (proof.place == edge.targetIndex)
                row.set(proof.condition);
        }
        return row;
    }

    // The blocks that nothing leads to once the failing edges are left out: those with dead edges alone.
    bool findDeadBlocks()
    {
        if (!m_blocksWithIgnoredFailingEdge)
            return true;

        Vector<BlockID, 16> deadBlocks;
        Vector<BlockID, 16> blocksWithDeadEdges;
        auto didFindDeadEdgeInto = [&](BlockID target) {
            StateOfBlock& state = m_states[target.index()];
            if (std::exchange(state.solveThatCountedDeadEdges, m_solve) != m_solve) {
                state.numberOfDeadEdges = 0;
                blocksWithDeadEdges.append(target);
            }
            if (++state.numberOfDeadEdges < m_cfg.incomingEdges(target).size() || target == m_cfg.root())
                return;
            state.solveThatFoundDead = m_solve;
            deadBlocks.append(target);
        };
        for (size_t index : *m_blocksWithIgnoredFailingEdge)
            didFindDeadEdgeInto(m_function.at(BlockID(index)).terminator.targets[failingTargetIndex].block);
        while (!deadBlocks.isEmpty()) {
            forEachOutgoingEdge(m_function, deadBlocks.takeLast(), [&](Edge edge, const Target& target) {
                if (!isLeftOut(edge))
                    didFindDeadEdgeInto(target.block);
            });
        }

        return std::ranges::all_of(blocksWithDeadEdges, [&](BlockID block) {
            if (isDead(block) || block == m_cfg.root())
                return true;
            uint32_t position = m_cfg.reversePostOrderIndex(block);
            return std::ranges::any_of(m_cfg.incomingEdges(block), [&](Edge edge) {
                return !isLeftOut(edge) && !isDead(edge.from) && m_cfg.reversePostOrderIndex(edge.from) < position;
            });
        });
    }

    // A block that was not visited has an empty row at its end. In the first sweep a block that comes later may still be visited, so an edge back from it is left out until the sweep is over.
    Row meetAtHeadOf(BlockID block, uint32_t position)
    {
        Row head;
        if (block == m_cfg.root())
            return head;

        bool isFirstEdge = true;
        bool waitsForEdgeBack = false;
        for (Edge edge : m_cfg.incomingEdges(block)) {
            if (isLeftOut(edge) || isDead(edge.from))
                continue;
            bool wasSourceVisited = wasVisited(edge.from);
            if (!wasSourceVisited && m_isFirstSweep && m_cfg.reversePostOrderIndex(edge.from) >= position) {
                waitsForEdgeBack = true;
                continue;
            }
            Row row = rowOn(edge, wasSourceVisited ? m_tails[edge.from.index()] : Row());
            if (std::exchange(isFirstEdge, false))
                head = row;
            else
                head.filter(row);
        }
        if (waitsForEdgeBack)
            m_blocksThatWaitForEdgesBack.append(block);
        if (isFirstEdge && waitsForEdgeBack) {
            for (uint32_t condition = 0; condition < m_conditions.size(); ++condition)
                head.set(condition);
        }
        return head;
    }

    // False where a block is first visited behind the first sweep with a row that is not empty: blocks behind it took its row for empty.
    bool visitFromProofs(BlockID id, uint32_t position)
    {
        Row row = meetAtHeadOf(id, position);
        bool isFirstVisit = !wasVisited(id);
        if (isFirstVisit && !m_isFirstSweep && !row.isEmpty())
            return false;
        if (!isFirstVisit && row == m_heads[id.index()])
            return true;
        m_states[id.index()].solveThatVisited = m_solve;
        m_heads[id.index()] = row;

        carryThroughWholeBlock(id, position, row);
        if (!isFirstVisit && row == m_tails[id.index()])
            return true;
        m_tails[id.index()] = row;

        std::span<const PlacedProof> proofs = m_edgeProofs.in(id);
        forEachOutgoingEdge(m_function, id, [&](Edge edge, const Target& target) {
            if (isLeftOut(edge))
                return;
            bool bringsConditions = !edge.isHandlerEdge() && (!row.isEmpty() || std::ranges::contains(proofs, edge.targetIndex, &PlacedProof::place));
            if (bringsConditions || wasVisited(target.block))
                markAsPending(target.block);
        });
        return true;
    }

    bool solveFromProofs()
    {
        ++m_solve;
        m_isPending.clearAll();
        m_blocksThatWaitForEdgesBack.shrink(0);
        if (!findDeadBlocks())
            return false;
        m_heads.grow(m_function.blocks.size());
        m_tails.grow(m_function.blocks.size());

        for (const PlacedProof& proof : m_edgeProofs.sorted) {
            if (!isDead(proof.block) && !isLeftOut(Edge { proof.block, proof.place }))
                markAsPending(m_function.at(proof.block).terminator.targets[proof.place].block);
        }
        for (const PlacedProof& proof : m_pointProofs.sorted) {
            if (!isDead(proof.block))
                markAsPending(proof.block);
        }

        std::span<const BlockID> order = m_cfg.reversePostOrder();
        m_isFirstSweep = true;
        while (!m_isPending.isEmpty()) {
            for (size_t position = m_isPending.findBit(0, true); position < order.size(); position = m_isPending.findBit(position + 1, true)) {
                m_isPending.quickClear(position);
                if (!visitFromProofs(order[position], position))
                    return false;
            }
            if (std::exchange(m_isFirstSweep, false)) {
                for (BlockID block : m_blocksThatWaitForEdgesBack)
                    markAsPending(block);
            }
        }
        return true;
    }

    bool meetAtHead(BlockID target, const Row& row, uint32_t fromPosition)
    {
        Row& head = m_heads[target.index()];
        if (m_isReached.quickSet(target.index()) && row.subsumes(head))
            return false;
        head.filter(row);
        uint32_t targetPosition = m_cfg.reversePostOrderIndex(target);
        m_isPending.quickSet(targetPosition);
        return targetPosition <= fromPosition;
    }

    // The head of a block that is not reached yet holds every condition.
    void solveEveryBlock()
    {
        Row allConditions;
        allConditions.setAll();
        m_heads.fill(allConditions, m_function.blocks.size());
        m_heads[m_cfg.root().index()].clearAll();
        m_isPending.clearAll();
        m_isReached.ensureSize(m_function.blocks.size());
        m_isReached.clearAll();
        m_isReached.quickSet(m_cfg.root().index());
        m_isPending.quickSet(m_cfg.reversePostOrderIndex(m_cfg.root()));
        std::span<const BlockID> order = m_cfg.reversePostOrder();
        for (bool shouldRepeat = true; shouldRepeat;) {
            shouldRepeat = false;
            for (uint32_t position = 0; position < order.size(); ++position) {
                if (!m_isPending.quickClear(position))
                    continue;
                Row row = m_heads[order[position].index()];
                carryThroughWholeBlock(order[position], position, row);
                forEachOutgoingEdge(m_function, order[position], [&](Edge edge, const Target& target) {
                    if (!isLeftOut(edge))
                        shouldRepeat |= meetAtHead(target.block, rowOn(edge, row), position);
                });
            }
        }
    }

    FunctionAnalyses& m_analyses;
    const TestConditions& m_conditions;
    std::span<const TestRequirement> m_requirements;
    const Function& m_function { m_analyses.function() };
    const CFG& m_cfg { m_analyses.cfg() };

    PlacedProofs m_edgeProofs { { }, BitVector(m_function.blocks.size()) };
    PlacedProofs m_pointProofs { { }, BitVector(m_function.blocks.size()) };
    Vector<uint32_t, 16> m_lastRequirement;
    // By position in the reverse postorder: a position that no block that the block may reach stands in front of. Empty where nothing leads back.
    Vector<uint32_t> m_firstPositionReachedFrom;

    Vector<Item> m_items;
    Vector<ItemsOfBlock> m_itemsOfBlocks = Vector<ItemsOfBlock>(m_function.blocks.size());
    Vector<Ending> m_endings;
    std::optional<ConditionGroups> m_groups;

    const BitVector* m_blocksWithIgnoredFailingEdge { nullptr };
    Vector<Row> m_heads;
    BitVector m_isPending { m_cfg.reversePostOrder().size() };
    // Of a solve from the proofs.
    uint32_t m_solve { 0 };
    bool m_didSolveFromProofs { false };
    bool m_isFirstSweep { false };
    Vector<StateOfBlock> m_states = Vector<StateOfBlock>(m_function.blocks.size());
    Vector<Row> m_tails;
    Vector<BlockID, 8> m_blocksThatWaitForEdgesBack;
    // Of a solve of every block.
    BitVector m_isReached;
};

struct BranchVerdicts {
    BitVector redundant;
    BitVector unreachable;
};

// The branches on tests that prove something for a stretch, and what else proves the conditions of those tests.
struct RedundantBranchSearch {
    FunctionPassContext& context;
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    TestConditions conditions { context.analyses };
    // One for each branch: where its test stands, and what that asks.
    Vector<TestRequirement> branches { };
    Vector<ValueID, 4> otherTests { };
    BitVector conditionsWithOtherTests { };
    bool hasSwitchOnShape { false };
    Vector<ExistingProof> proofsFromTests { };
    Vector<ExistingProof> otherProofs { };
    Vector<ExistingProof> proofs { };

    bool addBranch(BlockID block, ValueID test)
    {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.taken().block == terminator.notTaken().block)
            return false;

        // A test stands near the branch on it.
        size_t index = function.at(block).values.reverseFind(test);
        if (index == notFound)
            return false;

        const Value& value = function.at(test);
        Effects bound = declaredBoundOf(value.opcode);
        if (!bound.writes.isEmpty() || bound.mayExitSideways() || bound.mayCollect)
            return false;

        if (!conditions.find(value) && conditions.size() >= context.limit(LimitID::MaxTestedTypesPerFunction)) {
            context.gaveUp(LimitID::MaxTestedTypesPerFunction, test);
            return false;
        }
        branches.append(TestRequirement { 0, 0, conditions.add(value), { block, static_cast<uint32_t>(index) }, RequirementKind::AsksOnly });
        return true;
    }

    void addOtherTests(std::span<const ValueID> tests)
    {
        for (ValueID test : tests) {
            if (std::optional<TestConditionID> condition = conditions.find(function.at(test)))
                conditionsWithOtherTests.set(condition->index());
        }
    }

    void collectBranches()
    {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            const Terminator& terminator = function.at(block).terminator;
            hasSwitchOnShape |= terminator.kind == TerminatorKind::SwitchOnShape;
            if (!terminator.isConditionalBranch())
                continue;
            ValueID test = terminator.conditionOperand();
            if (testsThatProveForStretch.contains(function.at(test).opcode) && !addBranch(block, test))
                otherTests.append(test);
        }
        addOtherTests(otherTests.span());
    }

    // The one branch on a condition that nothing else proves is not redundant: on the way to its first turn nothing has proven the condition.
    void takeBranchesThatMayBeRedundantFrom(const RedundantBranchSearch& all)
    {
        Vector<uint32_t, 16> numberOfBranches(FillWith { }, all.conditions.size(), 0u);
        for (const TestRequirement& branch : all.branches)
            ++numberOfBranches[branch.condition.index()];
        auto mayBeProvenElsewhere = [&](TestConditionID id) {
            if (numberOfBranches[id.index()] > 1 || all.conditionsWithOtherTests.get(id.index()))
                return true;
            const TestCondition& condition = all.conditions.at(id);
            if (!all.conditions.stretchConditions().get(id.index()))
                return false;
            if (all.hasSwitchOnShape && condition.proof == Proof::Shape)
                return true;
            return condition.subject && !proofsOfAllocation(context.program, function, function.at(condition.subject)).isEmpty();
        };

        hasSwitchOnShape = all.hasSwitchOnShape;
        for (const TestRequirement& branch : all.branches) {
            if (mayBeProvenElsewhere(branch.condition))
                addBranch(branch.point.block, function.at(branch.point.block).terminator.conditionOperand());
        }
        addOtherTests(all.otherTests.span());
    }

    // Nothing for a guard that may not stand for the tests of the condition.
    std::optional<ProofSource> originOf(const ExistingProof& proof)
    {
        switch (proof.source) {
        case ProofSource::TakenEdge:
        case ProofSource::SwitchCase:
        case ProofSource::Allocation:
            return proof.source;
        case ProofSource::GuardValue:
            break;
        }

        // A user needs everything its guard proves about what it relies on to hold, so a guard that proves more than the folded test did cannot replace it.
        const GuardFacts& proven = analyses.guardFacts(proof.value);
        const GuardOrigin* origin = proven.facts.size() == 1 ? proven.facts.first().singleOrigin() : nullptr;
        if (!origin)
            return std::nullopt;

        switch (origin->source) {
        case GuardSource::Test:
            return ProofSource::TakenEdge;
        case GuardSource::SwitchCase:
            return ProofSource::SwitchCase;
        case GuardSource::Allocation:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Vector<ProofSet> findProofsReliedOn()
    {
        Vector<ProofSet> reliedOn(conditions.size());
        auto addUser = [&](const auto& user, ProofSet proofsOfUser) {
            if (proofsOfUser.isEmpty())
                return;
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (user.operandKind(operandIndex) != OperandKind::Guard)
                    return;
                for (const GuardFact& fact : analyses.guardFacts(operand).facts) {
                    for (const GuardOrigin& origin : fact.origins) {
                        if (origin.source != GuardSource::Test)
                            continue;
                        if (std::optional<TestConditionID> condition = conditions.find(function.at(origin.test)))
                            reliedOn[condition->index()] |= proofsOfUser;
                    }
                }
            });
        };
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (ValueID id : function.at(block).values)
                addUser(function.at(id), proofsNeededFromGuard(context.program, function, function.at(id)));
            addUser(function.at(block).terminator, reliesOf(function.at(block).terminator.kind));
        }
        return reliedOn;
    }

    bool canReplaceTests(const ExistingProof& allocationProof, ProofSet reliedOn) const
    {
        const TestCondition& condition = conditions.at(allocationProof.condition);
        const Value& allocation = function.at(allocationProof.value);
        // Select gives such a call an inline way and a slow one, and the parameter that joins the two proves nothing. So a call that is named as a guard keeps the slow way alone.
        if (!reliedOn.isEmpty() && allocation.opcode == Opcode::CallBuiltin && inlineImplementationOf(context.program, allocation) && overloadOf(context.program, allocation).code.implementation.otherwise)
            return false;

        AbstractHeapSet heapsDependedOn;
        for (const MemoryLocation& location : condition.dependsOn)
            heapsDependedOn.add(location.heap());

        // Whoever names the allocation gets all it proves, and each of those proofs has to last as long as a passed test does.
        reliedOn.add(condition.proof);
        ProofSet proven = proofsOfAllocation(context.program, function, allocation);
        return std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
            if (!reliedOn.contains(proof) || !proven.contains(proof))
                return true;
            return std::ranges::all_of(locationsBehindAllocationProof(proof), [&](const MemoryLocation& location) {
                return heapsDependedOn.contains(location.heap());
            });
        });
    }

    void findProofs(ProofsHeldByGuards heldByGuards)
    {
        Vector<ExistingProof> found = findExistingProofs(context, conditions, heldByGuards);

        BitVector conditionsProvenByAllocation;
        Vector<ProofSet> reliedOn;
        for (const ExistingProof& proof : found) {
            if (proof.source != ProofSource::Allocation)
                continue;
            if (reliedOn.isEmpty())
                reliedOn = findProofsReliedOn();
            if (canReplaceTests(proof, reliedOn[proof.condition.index()]))
                conditionsProvenByAllocation.set(proof.condition.index());
        }

        for (const ExistingProof& proof : found) {
            bool isProvenByAllocation = conditionsProvenByAllocation.get(proof.condition.index());
            std::optional<ProofSource> origin = originOf(proof);
            if (!origin)
                continue;
            switch (*origin) {
            case ProofSource::TakenEdge:
                proofsFromTests.append(proof);
                break;
            case ProofSource::SwitchCase:
                if (!isProvenByAllocation)
                    otherProofs.append(proof);
                break;
            case ProofSource::Allocation:
                if (isProvenByAllocation)
                    otherProofs.append(proof);
                break;
            case ProofSource::GuardValue:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    }

    // The failing edges of the branches that are assumed to be redundant are left out.
    BranchVerdicts solveAndClassify(ConditionCoverage& coverage, const BitVector& askedBranches, bool areAssumedRedundant) const
    {
        if (askedBranches.isEmpty())
            return { };

        BitVector blocksOfAssumedBranches(function.blocks.size());
        for (size_t index : askedBranches)
            blocksOfAssumedBranches.quickSet(branches[index].point.block.index());
        const BitVector* blocksWithIgnoredFailingEdge = areAssumedRedundant ? &blocksOfAssumedBranches : nullptr;
        coverage.solve(blocksWithIgnoredFailingEdge);

        if (context.build.options().verifiesAfterEveryPass) [[unlikely]] {
            TestEvents events(TestEventInputs { analyses, conditions, { }, branches.span(), proofs.span(), { }, false });
            TestCoverage coverageOfEveryBlock(TestCoverageInputs { events, GapBudget::unlimited(), blocksWithIgnoredFailingEdge });
            for (size_t index : askedBranches)
                RELEASE_ASSERT(coverage.of(static_cast<uint32_t>(index)) == coverageOfEveryBlock.of(static_cast<uint32_t>(index)));
        }

        BranchVerdicts verdicts;
        for (size_t index : askedBranches) {
            switch (coverage.of(static_cast<uint32_t>(index))) {
            case RequirementCoverage::Unknown:
                verdicts.unreachable.set(index);
                break;
            case RequirementCoverage::NeedsTest:
                break;
            case RequirementCoverage::Covered:
                verdicts.redundant.set(index);
                break;
            }
        }
        return verdicts;
    }

    // GuardAvailability does not merge what a test proved with what a switch case or an allocation proved, so one of those stands for the tests of a condition only where no such test remains.
    BitVector findMixedConditions(const BranchVerdicts& verdicts) const
    {
        BitVector mixedConditions;
        if (otherProofs.isEmpty())
            return mixedConditions;

        BitVector conditionsInPlay;
        BitVector conditionsStillTested = conditionsWithOtherTests;
        for (size_t index = 0; index < branches.size(); ++index) {
            size_t condition = branches[index].condition.index();
            if (verdicts.redundant.get(index))
                conditionsInPlay.set(condition);
            else if (!verdicts.unreachable.get(index))
                conditionsStillTested.set(condition);
        }

        for (const ExistingProof& proof : otherProofs)
            mixedConditions.set(proof.condition.index());
        mixedConditions.filter(conditionsInPlay);
        mixedConditions.filter(conditionsStillTested);
        return mixedConditions;
    }

    // Nothing where the rounds run out in a search without the guards, which only says whether the other search is worth making.
    std::optional<BitVector> findRedundantBranches(ProofsHeldByGuards heldByGuards)
    {
        findProofs(heldByGuards);
        BitVector allBranches(branches.size());
        for (size_t index = 0; index < branches.size(); ++index)
            allBranches.quickSet(index);

        uint64_t numberOfRoundsLeft = context.limit(LimitID::MaxGuardEliminationRounds);
        bool didReachRoundLimit = false;
        for (;;) {
            proofs = proofsFromTests;
            proofs.appendVector(otherProofs);
            ConditionCoverage coverage(analyses, conditions, branches.span(), proofs.span());

            BranchVerdicts verdicts;
            for (BitVector assumed = allBranches;;) {
                if (!numberOfRoundsLeft) {
                    if (heldByGuards == ProofsHeldByGuards::AreLeftOut)
                        return std::nullopt;
                    if (!std::exchange(didReachRoundLimit, true))
                        context.gaveUp(LimitID::MaxGuardEliminationRounds);
                    verdicts = solveAndClassify(coverage, allBranches, false);
                    break;
                }
                --numberOfRoundsLeft;
                verdicts = solveAndClassify(coverage, assumed, true);
                BitVector stillAssumed = verdicts.redundant;
                stillAssumed.merge(verdicts.unreachable);
                if (stillAssumed == assumed)
                    break;
                assumed = WTF::move(stillAssumed);
            }

            BitVector mixedConditions = findMixedConditions(verdicts);
            if (mixedConditions.isEmpty())
                return WTF::move(verdicts.redundant);
            otherProofs.removeAllMatching([&](const ExistingProof& proof) {
                return didReachRoundLimit || mixedConditions.get(proof.condition.index());
            });
        }
    }
};

} // anonymous namespace

// The branch becomes a jump over its taken edge. What that edge proved is handed over as arguments.
static void replaceBranchWithJumpToTaken(IRBuilder& builder, Function& function, BlockID block, NOESCAPE const Invocable<ValueID()> auto& guardInPlaceOfProofs)
{
    Terminator& terminator = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    Target taken = terminator.taken();
    builder.setOrigin(terminator.origin());
    terminator = Terminator { };
    builder.moveToEndOf(block);
    if (!taken.proofs.isEmpty()) {
        ValueID guard = guardInPlaceOfProofs();
        for (size_t result = 0; result < taken.proofs.size(); ++result)
            taken.arguments.append(guard);
    }
    builder.emitJump(taken.block, WTF::move(taken.arguments));
}

static void eraseTest(IRBuilder& builder, const Function& function, BlockID block, ValueID test)
{
    size_t index = function.at(block).values.find(test);
    RELEASE_ASSERT(index != notFound);
    BitVector indexes;
    indexes.set(index);
    builder.eraseValues(block, indexes);
}

static bool isReadAsBoolean(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::TerminatorOperand:
        return !function.at(use.block).terminator.isConditionalBranch();
    case Use::Kind::Operand:
        return function.at(use.user).operandKind(use.operandIndex) != OperandKind::Guard;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return true;
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void foldRedundantBranches(FunctionPassContext& context, const RedundantBranchSearch& search, const BitVector& redundant)
{
    struct FoldedBranch {
        ProgramPoint pointOfTest;
        ValueID test;
        size_t condition { 0 };
        Vector<Use, 2> booleanUses { };
        bool hasOtherUse { false };
    };

    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    Vector<FoldedBranch, 4> foldedBranches;
    BitVector foldedBlocks;
    BitVector conditionsInPlay;
    for (size_t index : redundant) {
        const TestRequirement& branch = search.branches[index];
        foldedBranches.append(FoldedBranch { branch.point, function.at(branch.point.block).terminator.conditionOperand(), branch.condition.index() });
        foldedBlocks.set(branch.point.block.index());
        conditionsInPlay.set(branch.condition.index());
    }

    Vector<Vector<ValueID, 2>> provingValues(search.conditions.size());
    for (const ExistingProof& proof : search.proofs) {
        if (!conditionsInPlay.get(proof.condition.index()) || (proof.source == ProofSource::TakenEdge && foldedBlocks.get(proof.edge.from.index())))
            continue;
        provingValues[proof.condition.index()].appendIfNotContains(isOnEdge(proof.source) ? ensureEdgeResult(builder, analyses, proof.edge, testedOperandIndex) : proof.value);
    }

    const UseLists& useLists = analyses.useLists();
    for (FoldedBranch& folded : foldedBranches) {
        for (const Use& use : useLists.usesOf(folded.test)) {
            if (use.kind == Use::Kind::TerminatorOperand && use.block == folded.pointOfTest.block)
                continue;
            if (isReadAsBoolean(function, use))
                folded.booleanUses.append(use);
            else
                folded.hasOtherUse = true;
        }
    }

    // SSARepair redirects every use of a definition, and a proving value has uses of its own: an allocation is also the object.
    Vector<std::tuple<size_t, ValueID, ProgramPoint>, 8> pendingDefinitions;
    for (size_t condition : conditionsInPlay) {
        for (ValueID value : provingValues[condition])
            pendingDefinitions.append({ condition, value, useLists.pointAfter(value) });
    }
    Vector<Vector<ValueID, 2>> definitions(search.conditions.size());
    for (auto [condition, value, pointAfter] : pendingDefinitions) {
        builder.setOrigin(function.at(value).origin());
        definitions[condition].append(builder.insertBefore(pointAfter, IRBuilder::make(Opcode::AllGuards, { value })));
    }

    bool keepsTests = context.build.validates();
    for (FoldedBranch& folded : foldedBranches) {
        if (folded.booleanUses.isEmpty() && !keepsTests)
            continue;
        Value boolean = IRBuilder::make(Opcode::Boolean);
        boolean.setBoolean(true);
        builder.setOrigin(function.at(folded.test).origin());
        ValueID constant = builder.insertBefore(folded.pointOfTest, WTF::move(boolean));
        for (const Use& use : folded.booleanUses)
            replaceUse(function, use, constant);
        if (!keepsTests)
            continue;
        // In a validating build the test is made all the same, and its answer is compared with the one that an earlier test stands for.
        builder.insertBefore(pointBeforeTerminatorOf(function, folded.pointOfTest.block), IRBuilder::make(Opcode::ValidateSameValue, { folded.test, constant }));
        folded.hasOtherUse = true;
    }
    builder.applyInsertions();
    // The builder knows the values that it has put in, not the uses that were handed to them.
    analyses.didChangeValues();

    for (const FoldedBranch& folded : foldedBranches) {
        replaceBranchWithJumpToTaken(builder, function, folded.pointOfTest.block, [&] {
            RELEASE_ASSERT(!definitions[folded.condition].isEmpty());
            return definitions[folded.condition].first();
        });
    }
    // The builder sees the jump that it emits, not the branch that was taken away for it.
    analyses.didChangeControlFlow();
    for (const FoldedBranch& folded : foldedBranches) {
        if (!folded.hasOtherUse)
            eraseTest(builder, function, folded.pointOfTest.block, folded.test);
    }
    builder.emptyUnreachableBlocks();

    SSARepair repair(builder, analyses);
    for (size_t condition : conditionsInPlay) {
        if (definitions[condition].size() < 2)
            continue;
        SSARepair::Variable variable = repair.addVariable(definitions[condition].first());
        for (ValueID definition : definitions[condition].span().subspan(1))
            repair.addDefinition(variable, definition);
    }
    repair.run();
}

static PassOutcome eliminateRedundantBranches(FunctionPassContext& context)
{
    RedundantBranchSearch search { context };
    search.collectBranches();
    if (search.branches.isEmpty())
        return PassOutcome::Unchanged;

    // Nearly every function has no redundant branch.
    RedundantBranchSearch firstSearch { context };
    firstSearch.takeBranchesThatMayBeRedundantFrom(search);
    if (firstSearch.branches.isEmpty())
        return PassOutcome::Unchanged;
    if (std::optional<BitVector> found = firstSearch.findRedundantBranches(ProofsHeldByGuards::AreLeftOut); found && found->isEmpty())
        return PassOutcome::Unchanged;

    BitVector redundant = *search.findRedundantBranches(ProofsHeldByGuards::AreListed);
    if (redundant.isEmpty())
        return PassOutcome::Unchanged;
    foldRedundantBranches(context, search, redundant);
    context.build.statistics().count(StatisticID::GuardsEliminated, redundant.bitCount());
    return PassOutcome::Changed;
}

static PassOutcome simplifyGuardLists(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!context.analyses.opcodesOfValues().contains(Opcode::AllGuards))
        return PassOutcome::Unchanged;

    Vector<ValueID, 8> lists;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (function.at(value).opcode == Opcode::AllGuards)
                lists.append(value);
        }
    }
    if (lists.isEmpty())
        return PassOutcome::Unchanged;

    const UseLists& useListsBefore = context.analyses.useLists();
    ValueSubstitution substitution(function);
    bool didChange = false;
    for (ValueID list : lists) {
        Vector<ValueID, 4> guards;
        for (ValueID operand : function.at(list).guardsOperands()) {
            if (function.at(operand).opcode != Opcode::AllGuards) {
                guards.appendIfNotContains(operand);
                continue;
            }
            for (ValueID nested : function.at(operand).guardsOperands())
                guards.appendIfNotContains(nested);
        }
        if (!std::ranges::equal(guards, function.at(list).guardsOperands())) {
            function.edit(list, ValueEdit::Definition).setGuardsOperands(guards.span());
            didChange = true;
        }
        if (guards.size() != 1)
            continue;

        // An allocation may stand where an operand of kind Guard is wanted, not as an argument for a parameter of type guard.
        bool canStandForList = isGuardValue(function, guards.first()) || std::ranges::all_of(useListsBefore.usesOf(list), [](const Use& use) {
            return use.kind == Use::Kind::Operand || use.kind == Use::Kind::TerminatorOperand;
        });
        if (canStandForList) {
            substitution.replace(list, guards.first());
            didChange = true;
        }
    }

    if (didChange)
        context.analyses.didChangeValues();
    substitution.apply(function, &context.analyses);

    Vector<std::pair<BlockID, BitVector>, 4> unusedLists;
    const UseLists& useLists = context.analyses.useLists();
    for (ValueID list : lists) {
        if (useLists.useCount(list))
            continue;
        ValueLocation location = useLists.locationOf(list);
        if (unusedLists.isEmpty() || unusedLists.last().first != location.block)
            unusedLists.append({ location.block, BitVector() });
        unusedLists.last().second.set(location.index);
    }
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const auto& [block, indexes] : unusedLists)
        builder.eraseValues(block, indexes);
    return didChange || !unusedLists.isEmpty() ? PassOutcome::Changed : PassOutcome::Unchanged;
}

static PassOutcome foldInstanceTestsOfTypedObjects(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::BelongsToInstance))
        return PassOutcome::Unchanged;

    Function& function = context.function;
    const ValueIdentities& identities = context.analyses.identities();
    const UseLists& useLists = context.analyses.useLists();
    Vector<BlockID, 4> blocks;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.taken().block == terminator.notTaken().block)
            continue;
        ValueID test = terminator.conditionOperand();
        if (function.at(test).opcode != Opcode::BelongsToInstance || useLists.useCount(test) != 1 || !function.at(block).values.contains(test))
            continue;
        ValueID object = identities.identityOf(function.at(test).objectOperand());
        ValueLocation definition = useLists.locationOf(object);
        bool mayJoinTestedValues = definition.placement != ValuePlacement::InBlock && definition.block != entryBlock;
        if (!mayJoinTestedValues && receiverCandidates(context.program, function, object).proof == ReceiverProof::Type)
            blocks.append(block);
    }
    if (blocks.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    bool keepsTests = context.build.validates();
    for (BlockID block : blocks) {
        ValueID test = function.at(block).terminator.conditionOperand();
        replaceBranchWithJumpToTaken(builder, function, block, [&] {
            return builder.append(Opcode::AllGuards, { });
        });
        if (!keepsTests) {
            eraseTest(builder, function, block, test);
            continue;
        }
        // A validating build still performs the test, and compares its result with the one the type implies.
        builder.setOrigin(function.at(test).origin());
        ProgramPoint end = pointBeforeTerminatorOf(function, block);
        Value boolean = IRBuilder::make(Opcode::Boolean);
        boolean.setBoolean(true);
        ValueID constant = builder.insertBefore(end, WTF::move(boolean));
        builder.insertBefore(end, IRBuilder::make(Opcode::ValidateSameValue, { test, constant }));
    }
    builder.applyInsertions();
    // The builder sees the jumps that it emits, not the branches that were taken away for them.
    context.analyses.didChangeControlFlow();
    builder.emptyUnreachableBlocks();
    context.build.statistics().count(StatisticID::GuardsEliminated, blocks.size());
    return PassOutcome::Changed;
}

PassOutcome eliminateGuards(FunctionPassContext& context)
{
    PassOutcome outcome = foldInstanceTestsOfTypedObjects(context);
    outcome |= eliminateRedundantBranches(context);
    outcome |= simplifyGuardLists(context);
    // The analyses were told of every edit, so whatever they hold was made behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
