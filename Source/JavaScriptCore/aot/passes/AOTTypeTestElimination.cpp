/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTEliminateGuards.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestedBranch {
    BlockID block;
    uint32_t index { 0 };
    ValueID test;
    TestConditionID condition;
};

enum class ProofOrigin : uint8_t { Unknown, Test, SwitchCase, Allocation };

struct BranchVerdicts {
    BitVector redundant;
    BitVector unreachable;
};

enum class TestUse : uint8_t { FoldedBranch, Boolean, Other };

struct FoldedBranch {
    uint32_t branch { 0 };
    ValueID replacementGuard;
    Vector<Use, 2> booleanUses;
    bool hasOtherUse { false };
};

struct FoldingInputs {
    FunctionPassContext& context;
    const TestConditions& conditions;
    std::span<const TestedBranch> branches;
    std::span<const ExistingProof> proofs;
    const BitVector& redundant;
};

class BranchFolder {
    WTF_MAKE_NONCOPYABLE(BranchFolder);
public:
    explicit BranchFolder(const FoldingInputs&);

    void run();

private:
    const TestedBranch& branchOf(const FoldedBranch& folded) const LIFETIME_BOUND { return m_inputs.branches[folded.branch]; }

    void adoptProofs();
    void collectUsesOfTests();
    TestUse classify(const Use&, const TestedBranch&) const;
    void defineProofs();
    void chooseReplacementGuards();
    void replaceBooleanUses();
    void fold(const FoldedBranch&);
    void eraseUnusedTests();
    void repairProofs();

    FoldingInputs m_inputs;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder m_builder;
    Vector<FoldedBranch, 4> m_foldedBranches;
    BitVector m_foldedBlocks;
    BitVector m_conditionsInPlay;
    Vector<Vector<ValueID, 2>> m_provingValues;
    Vector<Vector<ValueID, 2>> m_definitions;
};

// Where the conditions of branches hold: the answers of TestCoverage over TestEvents for requirements that only ask, without a budget, with slow paths that come back.
class ConditionCoverage {
    WTF_MAKE_NONCOPYABLE(ConditionCoverage);
public:
    ConditionCoverage(FunctionAnalyses&, const TestConditions&, std::span<const TestRequirement>, std::span<const ExistingProof>);

    void solve(const BitVector* blocksWithIgnoredFailingEdge);
    RequirementCoverage of(uint32_t requirement);

private:
    using Word = uint64_t;
    using Row = Vector<Word, 4>;

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

        bool areCollected() const { return first != UINT32_MAX; }
    };

    struct PointProof {
        BlockID block;
        bool isParameter { false };
        uint32_t index { 0 };
        uint32_t proof { 0 };
    };

    struct EdgeProof {
        Edge edge;
        uint32_t condition { 0 };
    };

    struct WrittenByWriter {
        ValueID writer;
        GuardWriter::WrittenDependencies written;
    };

    // Each number says something of the solve that it names.
    struct StateOfBlock {
        uint32_t solveThatVisited { 0 };
        uint32_t solveThatFoundDead { 0 };
        uint32_t solveThatCountedDeadEdges { 0 };
        uint32_t numberOfDeadEdges { 0 };
    };

    static bool isEmpty(std::span<const Word> row) { return std::ranges::all_of(row, [](Word word) { return !word; }); }
    static void set(std::span<Word> row, uint32_t condition) { row[condition / 64] |= Word { 1 } << condition % 64; }
    static bool get(std::span<const Word> row, uint32_t condition) { return row[condition / 64] >> condition % 64 & 1; }
    static void clearAll(std::span<Word> row) { std::ranges::fill(row, 0); }
    static void forEachCondition(std::span<const Word> row, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        for (uint32_t wordIndex = 0; wordIndex < row.size(); ++wordIndex) {
            for (Word word = row[wordIndex]; word; word &= word - 1)
                functor(64 * wordIndex + std::countr_zero(word));
        }
    }

    std::span<Word> headOf(BlockID block) { return m_heads.mutableSpan().subspan(size_t { block.index() } * m_numberOfWords, m_numberOfWords); }
    std::span<Word> tailOf(BlockID block) { return m_tails.mutableSpan().subspan(size_t { block.index() } * m_numberOfWords, m_numberOfWords); }
    bool wasVisited(BlockID block) const { return m_states[block.index()].solveThatVisited == m_solve; }
    bool isDead(BlockID block) const { return m_states[block.index()].solveThatFoundDead == m_solve; }
    bool isLeftOut(Edge edge) const { return m_blocksWithIgnoredFailingEdge && !edge.isHandlerEdge() && edge.targetIndex == failingTargetIndex && m_blocksWithIgnoredFailingEdge->get(edge.from.index()); }
    std::span<Word> askedOf(uint32_t ending) { return m_endings.mutableSpan().subspan(size_t { 2 } * ending * m_numberOfWords, m_numberOfWords); }
    std::span<Word> endedOf(uint32_t ending) { return m_endings.mutableSpan().subspan((size_t { 2 } * ending + 1) * m_numberOfWords, m_numberOfWords); }

    void collectProofs(std::span<const ExistingProof>);
    void findWhereConditionsMatter();
    void dropConditionsThatNoLongerMatter(std::span<Word> row, uint32_t position) const;
    bool isBarrier(BlockID, ValueID producer);
    void collectItemsOf(BlockID);
    void appendEnding(uint32_t index);
    void prepareToAskWriters();
    void askWriter(BlockID, const Item&, std::span<const Word> conditions);
    // Up to the place of a requirement with that index.
    void carryThrough(BlockID, std::span<Word> row, uint32_t endIndex);
    std::span<const EdgeProof> proofsOnEdgesFrom(BlockID) const;

    bool solveFromProofs();
    bool findDeadBlocks();
    void didFindDeadEdgeInto(BlockID target, Vector<BlockID, 16>& deadBlocks, Vector<BlockID, 16>& blocksWithDeadEdges);
    void markAsPending(BlockID block) { m_isPending.quickSet(m_cfg.reversePostOrderIndex(block)); }
    void meetAtHeadOf(BlockID, uint32_t position, std::span<Word> head);
    bool visitFromProofs(BlockID, uint32_t position);

    void solveEveryBlock();
    bool visit(BlockID, uint32_t position);
    bool meetAtHead(BlockID target, std::span<const Word> row, uint32_t fromPosition);

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const CFG& m_cfg;
    const TestConditions& m_conditions;
    std::span<const TestRequirement> m_requirements;
    std::span<const ExistingProof> m_proofs;
    const uint32_t m_numberOfWords;
    const bool m_checksScreens;

    Vector<EdgeProof, 8> m_edgeProofs;
    BitVector m_blocksWithEdgeProofs;
    Vector<PointProof, 4> m_pointProofs;
    BitVector m_blocksWithPointProofs;
    Vector<uint32_t, 16> m_lastRequirement;
    // By position in the reverse postorder: a position that no block that the block may reach stands in front of. Empty where nothing leads back.
    Vector<uint32_t> m_firstPositionReachedFrom;

    Vector<Item> m_items;
    Vector<ItemsOfBlock> m_itemsOfBlocks;
    // For each ending two rows: the conditions that the writer was asked about, and those among them that it may end.
    Vector<Word> m_endings;
    uint32_t m_numberOfEndings { 0 };
    bool m_isPreparedToAskWriters { false };
    Vector<std::optional<GuardDependencies>> m_guardDependencies;
    Vector<uint32_t> m_dependencyGroups;
    BitVector m_subjectsFromOutside;
    Vector<WrittenByWriter> m_writtenOfDependencyGroups;

    static constexpr uint32_t failingTargetIndex = 1;

    const BitVector* m_blocksWithIgnoredFailingEdge { nullptr };
    Vector<Word> m_heads;
    BitVector m_isPending;
    // Of a solve from the proofs.
    uint32_t m_solve { 0 };
    bool m_didSolveFromProofs { false };
    bool m_isFirstSweep { false };
    Vector<StateOfBlock> m_states;
    Vector<Word> m_tails;
    Vector<BlockID, 8> m_blocksThatWaitForEdgesBack;
    // Of a solve of every block.
    BitVector m_isReached;
};

class GuardEliminator {
    WTF_MAKE_NONCOPYABLE(GuardEliminator);
public:
    explicit GuardEliminator(FunctionPassContext&);

    PassOutcome run();

private:
    void collectBranches();
    bool addBranch(BlockID, ValueID test);
    bool mayFindRedundantBranch() const;
    bool mayBeProvenElsewhere(TestConditionID, const Vector<uint32_t>& numberOfBranches) const;
    void takeBranchesThatMayBeRedundantFrom(const GuardEliminator&);

    void findProofs(ProofsHeldByGuards);
    bool findsNoRedundantBranchWithoutGuards();
    ProofOrigin originOf(const ExistingProof&);
    Vector<ProofSet> findProofsReliedOn(const BitVector& conditions);
    bool canReplaceTests(const ExistingProof& allocationProof, ProofSet reliedOn) const;

    BitVector findRedundantBranches();
    BranchVerdicts findCoveredBranches(ConditionCoverage&);
    BranchVerdicts solveAndClassifyBranches(ConditionCoverage&, const BitVector* assumed, const BitVector& branches) const;
    BitVector findMixedConditions(const BranchVerdicts&) const;

    PassOutcome simplifyGuardLists();
    Vector<ValueID, 4> flattenedGuardsOf(ValueID list) const;
    bool eraseUnusedGuardLists();

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    TestConditions m_conditions;
    Vector<TestedBranch> m_branches;
    // One for each branch, at the index of the branch.
    Vector<TestRequirement> m_requirements;
    Vector<ValueID, 4> m_otherTests;
    BitVector m_conditionsWithOtherTests;
    bool m_hasSwitchOnShape { false };
    Vector<ExistingProof> m_proofsFromTests;
    Vector<ExistingProof> m_otherProofs;
    Vector<ExistingProof> m_proofs;
    uint64_t m_numberOfRounds { 0 };
    bool m_didReachRoundLimit { false };
};

ConditionCoverage::ConditionCoverage(FunctionAnalyses& analyses, const TestConditions& conditions, std::span<const TestRequirement> requirements, std::span<const ExistingProof> proofs)
    : m_analyses(analyses)
    , m_function(analyses.function())
    , m_cfg(analyses.cfg())
    , m_conditions(conditions)
    , m_requirements(requirements)
    , m_proofs(proofs)
    , m_numberOfWords((conditions.size() + 63) / 64)
    , m_checksScreens(analyses.context().build.options().verifiesAfterEveryPass)
    , m_blocksWithEdgeProofs(m_function.blocks.size())
    , m_blocksWithPointProofs(m_function.blocks.size())
    , m_itemsOfBlocks(m_function.blocks.size())
    , m_isPending(m_cfg.reversePostOrder().size())
    , m_states(m_function.blocks.size())
{
    collectProofs(proofs);
    findWhereConditionsMatter();
}

void ConditionCoverage::collectProofs(std::span<const ExistingProof> proofs)
{
    for (uint32_t index = 0; index < proofs.size(); ++index) {
        const ExistingProof& proof = proofs[index];
        switch (proof.source) {
        case ProofSource::TakenEdge:
        case ProofSource::SwitchCase:
            if (!m_cfg.isReachable(proof.edge.from))
                continue;
            m_edgeProofs.append(EdgeProof { proof.edge, proof.condition.index() });
            m_blocksWithEdgeProofs.quickSet(proof.edge.from.index());
            continue;
        case ProofSource::GuardValue:
        case ProofSource::Allocation:
            break;
        }
        ValueLocation location = m_analyses.useLists().locationOf(proof.value);
        if (location.placement == ValuePlacement::Detached || !m_cfg.isReachable(location.block))
            continue;
        m_pointProofs.append(PointProof { location.block, location.placement == ValuePlacement::Parameter, location.placement == ValuePlacement::Parameter ? 0 : location.index, index });
        m_blocksWithPointProofs.quickSet(location.block.index());
    }
    std::ranges::stable_sort(m_edgeProofs, { }, [](const EdgeProof& proof) {
        return proof.edge.from.index();
    });
    std::ranges::stable_sort(m_pointProofs, { }, [](const PointProof& proof) {
        return std::tuple { proof.block.index(), !proof.isParameter, proof.index };
    });
}

// Nobody asks for a condition behind its last requirement.
void ConditionCoverage::findWhereConditionsMatter()
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

void ConditionCoverage::dropConditionsThatNoLongerMatter(std::span<Word> row, uint32_t position) const
{
    uint32_t firstPositionReached = m_firstPositionReachedFrom.isEmpty() ? position : m_firstPositionReachedFrom[position];
    forEachCondition(row, [&](uint32_t condition) {
        if (m_lastRequirement[condition] < firstPositionReached)
            row[condition / 64] &= ~(Word { 1 } << condition % 64);
    });
}

// As TestEvents::findResultReads() has it where no requirement places a test.
bool ConditionCoverage::isBarrier(BlockID block, ValueID producer)
{
    const Program& program = m_analyses.program();
    const Value& value = m_function.at(producer);
    if (!numberOfResultsOf(program, m_function, value)) {
        std::optional<OpaqueKind> opaque = value.type ? program.at(value.type).opaque : std::nullopt;
        if (!opaque || !mustBeDefinition(operandKindOf(*opaque)))
            return false;
    }
    return std::ranges::any_of(m_analyses.useLists().usesOf(producer), [&](const Use& use) {
        return use.block != block || (use.kind != Use::Kind::Operand && use.kind != Use::Kind::TerminatorOperand);
    });
}

void ConditionCoverage::appendEnding(uint32_t index)
{
    m_items.append(Item { index, ItemKind::Ending, m_numberOfEndings++ });
    m_endings.grow(m_endings.size() + 2 * m_numberOfWords);
    std::ranges::fill(m_endings.mutableSpan().last(2 * m_numberOfWords), 0);
}

void ConditionCoverage::collectItemsOf(BlockID id)
{
    const Block& block = m_function.at(id);
    const OpcodeSet& opcodesThatMayEnd = opcodesThatMayEndProofs();
    AbstractHeapSet heapsDependedOn = m_conditions.heapsDependedOn();
    uint32_t numberOfValues = block.values.size();
    uint32_t first = m_items.size();

    std::span<const PointProof> proofs;
    if (m_blocksWithPointProofs.quickGet(id.index())) {
        auto range = std::ranges::equal_range(m_pointProofs.span(), id.index(), { }, [](const PointProof& proof) {
            return proof.block.index();
        });
        proofs = { range.begin(), range.end() };
    }
    for (; !proofs.empty() && proofs.front().isParameter; proofs = proofs.subspan(1))
        m_items.append(Item { 0, ItemKind::ParameterProof, proofs.front().proof });

    for (uint32_t index = 0; index < numberOfValues; ++index) {
        ValueID value = block.values[index];
        if (isBarrier(id, value))
            m_items.append(Item { index, ItemKind::Barrier, 0 });

        if (opcodesThatMayEnd.contains(m_function.at(value).opcode)) {
            const Effects& effects = m_analyses.effectsOf(value).normal;
            if (effects.maySuspend)
                m_items.append(Item { index, ItemKind::Suspension, 0 });
            else if (effects.writes.containsAny(heapsDependedOn))
                appendEnding(index);
        } else if (m_checksScreens) [[unlikely]] {
            const Effects& effects = m_analyses.effectsOf(value).normal;
            RELEASE_ASSERT(!effects.maySuspend && effects.writes.isEmpty());
        }

        for (; !proofs.empty() && proofs.front().index == index; proofs = proofs.subspan(1))
            m_items.append(Item { index, ItemKind::ValueProof, proofs.front().proof });
    }
    RELEASE_ASSERT(proofs.empty());

    const Effects& effectsOfTerminator = m_analyses.effectsOfTerminator(id).normal;
    if (effectsOfTerminator.maySuspend)
        m_items.append(Item { numberOfValues, ItemKind::Suspension, 0 });
    else if (effectsOfTerminator.writes.containsAny(heapsDependedOn))
        appendEnding(numberOfValues);

    m_itemsOfBlocks[id.index()] = { first, static_cast<uint32_t>(m_items.size()) };
}

// Conditions of one group differ in their subjects alone, as in TestEvents::groupConditionsByDependencies().
void ConditionCoverage::prepareToAskWriters()
{
    if (std::exchange(m_isPreparedToAskWriters, true))
        return;

    m_guardDependencies.grow(m_conditions.size());
    for (size_t index : m_conditions.stretchConditions())
        m_guardDependencies[index] = dependenciesOfGuardOf(m_analyses, m_conditions.at(TestConditionID(index)).test);

    auto hashOf = [](const TestCondition& condition, const GuardDependencies& dependencies) {
        Hasher hasher;
        add(hasher, condition.proof);
        add(hasher, dependencies.slotState);
        for (const Vector<MemoryLocation, 2>* locations : { &condition.dependsOn, &dependencies.locations }) {
            add(hasher, locations->size());
            for (const MemoryLocation& location : *locations) {
                add(hasher, location.heap());
                add(hasher, location.isEntireHeap());
                if (location.isEntireHeap())
                    continue;
                add(hasher, location.firstIndex());
                add(hasher, location.secondIndex());
            }
        }
        return hasher.hash();
    };

    HashedIndex index;
    Vector<uint32_t, 16> firstConditions;
    m_dependencyGroups.fill(0, m_conditions.size());
    m_subjectsFromOutside.ensureSize(m_conditions.size());
    for (size_t conditionIndex : m_conditions.stretchConditions()) {
        const TestCondition& condition = m_conditions.at(TestConditionID(conditionIndex));
        const GuardDependencies& dependencies = *m_guardDependencies[conditionIndex];
        uint32_t hash = hashOf(condition, dependencies);
        std::optional<uint32_t> group = index.find(hash, [&](uint32_t position) {
            const TestCondition& other = m_conditions.at(TestConditionID(firstConditions[position]));
            const GuardDependencies& otherDependencies = *m_guardDependencies[firstConditions[position]];
            return other.proof == condition.proof && other.dependsOn == condition.dependsOn && otherDependencies.locations == dependencies.locations && otherDependencies.slotState == dependencies.slotState;
        });
        if (!group) {
            group = firstConditions.size();
            firstConditions.append(conditionIndex);
            index.add(hash, *group);
        }
        m_dependencyGroups[conditionIndex] = *group;
        if (dependencies.subject && comesFromOutside(m_analyses, dependencies.subject))
            m_subjectsFromOutside.quickSet(conditionIndex);
    }
    m_writtenOfDependencyGroups.grow(firstConditions.size());
}

void ConditionCoverage::askWriter(BlockID block, const Item& item, std::span<const Word> conditions)
{
    prepareToAskWriters();
    std::span<Word> asked = askedOf(item.payload);
    std::span<Word> ended = endedOf(item.payload);
    const BlockValues& values = m_function.at(block).values;
    if (item.index == values.size()) {
        const Effects& effects = m_analyses.effectsOfTerminator(block).normal;
        forEachCondition(conditions, [&](uint32_t index) {
            set(asked, index);
            if (m_conditions.stretchConditions().get(index) && endingOfTerminator(m_analyses, m_conditions.at(TestConditionID(index)), effects) == ProofEnding::MayEnd)
                set(ended, index);
        });
        return;
    }

    ValueID writer = values[item.index];
    const EffectsOnEdges& effects = m_analyses.effectsOf(writer);
    EndingsAtWriter endings(m_analyses, writer, effects);
    // What a group was found to have written is held by the writer of the last question, and that of an earlier question about this writer went with its EndingsAtWriter.
    for (WrittenByWriter& group : m_writtenOfDependencyGroups)
        group.writer = ValueID();
    forEachCondition(conditions, [&](uint32_t index) {
        set(asked, index);
        if (!m_conditions.stretchConditions().get(index))
            return;
        const GuardDependencies& dependencies = *m_guardDependencies[index];
        WrittenByWriter& group = m_writtenOfDependencyGroups[m_dependencyGroups[index]];
        if (group.writer != writer) {
            group.writer = writer;
            group.written = { };
            if (effects.normal.writes.overlaps(dependencies.masks))
                group.written = endings.writtenOf(m_conditions.at(TestConditionID(index)), dependencies);
        }
        if (endings.endingOf(dependencies.subject, m_subjectsFromOutside.quickGet(index), group.written) == ProofEnding::MayEnd)
            set(ended, index);
    });
}

void ConditionCoverage::carryThrough(BlockID block, std::span<Word> row, uint32_t endIndex)
{
    if (isEmpty(row) && !m_blocksWithPointProofs.quickGet(block.index()))
        return;
    if (!m_itemsOfBlocks[block.index()].areCollected())
        collectItemsOf(block);

    ItemsOfBlock range = m_itemsOfBlocks[block.index()];
    Row unasked(FillWith { }, m_numberOfWords, Word { 0 });
    for (uint32_t itemIndex = range.first; itemIndex < range.end; ++itemIndex) {
        Item item = m_items[itemIndex];
        if (item.index > endIndex || (item.index == endIndex && item.kind > ItemKind::Barrier))
            return;
        switch (item.kind) {
        case ItemKind::ParameterProof:
        case ItemKind::ValueProof:
            set(row, m_proofs[item.payload].condition.index());
            break;
        case ItemKind::Barrier:
        case ItemKind::Suspension:
            clearAll(row);
            break;
        case ItemKind::Ending: {
            Word anyUnasked = 0;
            std::span<const Word> asked = askedOf(item.payload);
            for (uint32_t word = 0; word < m_numberOfWords; ++word)
                anyUnasked |= unasked[word] = row[word] & ~asked[word];
            if (anyUnasked)
                askWriter(block, item, unasked.span());
            std::span<const Word> ended = endedOf(item.payload);
            for (uint32_t word = 0; word < m_numberOfWords; ++word)
                row[word] &= ~ended[word];
            break;
        }
        }
    }
}

bool ConditionCoverage::meetAtHead(BlockID target, std::span<const Word> row, uint32_t fromPosition)
{
    std::span<Word> head = headOf(target);
    if (!m_isReached.quickSet(target.index()))
        std::ranges::copy(row, head.begin());
    else {
        Word dropped = 0;
        for (uint32_t word = 0; word < m_numberOfWords; ++word) {
            dropped |= head[word] & ~row[word];
            head[word] &= row[word];
        }
        if (!dropped)
            return false;
    }
    uint32_t targetPosition = m_cfg.reversePostOrderIndex(target);
    m_isPending.quickSet(targetPosition);
    return targetPosition <= fromPosition;
}

std::span<const ConditionCoverage::EdgeProof> ConditionCoverage::proofsOnEdgesFrom(BlockID block) const
{
    if (!m_blocksWithEdgeProofs.quickGet(block.index()))
        return { };
    auto range = std::ranges::equal_range(m_edgeProofs.span(), block.index(), { }, [](const EdgeProof& proof) {
        return proof.edge.from.index();
    });
    return { range.begin(), range.end() };
}

bool ConditionCoverage::visit(BlockID id, uint32_t position)
{
    const Block& block = m_function.at(id);
    Row row(std::span<const Word>(headOf(id)));
    dropConditionsThatNoLongerMatter(row.mutableSpan(), position);
    carryThrough(id, row.mutableSpan(), UINT32_MAX);

    std::span<const EdgeProof> proofs = proofsOnEdgesFrom(id);

    bool shouldRepeat = false;
    uint32_t numberOfTargets = block.terminator.targets.size();
    for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
        const Target& target = block.terminator.targets[targetIndex];
        if (isLeftOut(Edge { id, targetIndex }))
            continue;
        if (proofs.empty()) {
            shouldRepeat |= meetAtHead(target.block, row.span(), position);
            continue;
        }
        Row rowOnEdge = row;
        for (const EdgeProof& proof : proofs) {
            if (proof.edge.targetIndex == targetIndex)
                set(rowOnEdge.mutableSpan(), proof.condition);
        }
        shouldRepeat |= meetAtHead(target.block, rowOnEdge.span(), position);
    }
    if (block.handler) {
        clearAll(row.mutableSpan());
        shouldRepeat |= meetAtHead(block.handler->block, row.span(), position);
    }
    return shouldRepeat;
}

void ConditionCoverage::didFindDeadEdgeInto(BlockID target, Vector<BlockID, 16>& deadBlocks, Vector<BlockID, 16>& blocksWithDeadEdges)
{
    StateOfBlock& state = m_states[target.index()];
    if (state.solveThatCountedDeadEdges != m_solve) {
        state.solveThatCountedDeadEdges = m_solve;
        state.numberOfDeadEdges = 0;
        blocksWithDeadEdges.append(target);
    }
    if (++state.numberOfDeadEdges < m_cfg.incomingEdges(target).size() || target == m_cfg.root())
        return;
    state.solveThatFoundDead = m_solve;
    deadBlocks.append(target);
}

// The blocks that nothing leads to once the failing edges are left out: those with dead edges alone.
bool ConditionCoverage::findDeadBlocks()
{
    if (!m_blocksWithIgnoredFailingEdge)
        return true;

    Vector<BlockID, 16> deadBlocks;
    Vector<BlockID, 16> blocksWithDeadEdges;
    for (size_t index : *m_blocksWithIgnoredFailingEdge)
        didFindDeadEdgeInto(m_function.at(BlockID(index)).terminator.targets[failingTargetIndex].block, deadBlocks, blocksWithDeadEdges);
    while (!deadBlocks.isEmpty()) {
        BlockID block = deadBlocks.takeLast();
        forEachOutgoingEdge(m_function, block, [&](Edge edge, const Target& target) {
            if (!isLeftOut(edge))
                didFindDeadEdgeInto(target.block, deadBlocks, blocksWithDeadEdges);
        });
    }

    for (BlockID block : blocksWithDeadEdges) {
        if (isDead(block) || block == m_cfg.root())
            continue;
        uint32_t position = m_cfg.reversePostOrderIndex(block);
        bool isEnteredForward = std::ranges::any_of(m_cfg.incomingEdges(block), [&](Edge edge) {
            return !isLeftOut(edge) && !isDead(edge.from) && m_cfg.reversePostOrderIndex(edge.from) < position;
        });
        if (!isEnteredForward)
            return false;
    }
    return true;
}

// A block that was not visited has an empty row at its end. In the first sweep a block that comes later may still be visited, so an edge back from it is left out until the sweep is over.
void ConditionCoverage::meetAtHeadOf(BlockID block, uint32_t position, std::span<Word> head)
{
    clearAll(head);
    if (block == m_cfg.root())
        return;

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

        Row row(FillWith { }, m_numberOfWords, Word { 0 });
        if (!edge.isHandlerEdge()) {
            if (wasSourceVisited)
                std::ranges::copy(tailOf(edge.from), row.begin());
            for (const EdgeProof& proof : proofsOnEdgesFrom(edge.from)) {
                if (proof.edge.targetIndex == edge.targetIndex)
                    set(row.mutableSpan(), proof.condition);
            }
        }
        for (uint32_t word = 0; word < m_numberOfWords; ++word)
            head[word] = isFirstEdge ? row[word] : head[word] & row[word];
        isFirstEdge = false;
    }
    if (waitsForEdgeBack)
        m_blocksThatWaitForEdgesBack.append(block);
    if (isFirstEdge && waitsForEdgeBack) {
        for (uint32_t condition = 0; condition < m_conditions.size(); ++condition)
            set(head, condition);
    }
}

// False where a block is first visited behind the first sweep with a row that is not empty: blocks behind it took its row for empty.
bool ConditionCoverage::visitFromProofs(BlockID id, uint32_t position)
{
    const Block& block = m_function.at(id);
    Row row(FillWith { }, m_numberOfWords, Word { 0 });
    meetAtHeadOf(id, position, row.mutableSpan());

    bool isFirstVisit = !wasVisited(id);
    if (isFirstVisit && !m_isFirstSweep && !isEmpty(row.span()))
        return false;
    if (!isFirstVisit && std::ranges::equal(row, headOf(id)))
        return true;
    m_states[id.index()].solveThatVisited = m_solve;
    std::ranges::copy(row, headOf(id).begin());

    dropConditionsThatNoLongerMatter(row.mutableSpan(), position);
    carryThrough(id, row.mutableSpan(), UINT32_MAX);
    if (!isFirstVisit && std::ranges::equal(row, tailOf(id)))
        return true;
    std::ranges::copy(row, tailOf(id).begin());

    bool bringsConditions = !isEmpty(row.span());
    std::span<const EdgeProof> proofs = proofsOnEdgesFrom(id);
    forEachOutgoingEdge(m_function, id, [&](Edge edge, const Target& target) {
        if (isLeftOut(edge))
            return;
        bool bringsConditionsOnEdge = !edge.isHandlerEdge() && (bringsConditions || std::ranges::any_of(proofs, [&](const EdgeProof& proof) {
            return proof.edge.targetIndex == edge.targetIndex;
        }));
        if (bringsConditionsOnEdge || wasVisited(target.block))
            markAsPending(target.block);
    });
    UNUSED_PARAM(block);
    return true;
}

bool ConditionCoverage::solveFromProofs()
{
    ++m_solve;
    m_isPending.clearAll();
    m_blocksThatWaitForEdgesBack.shrink(0);
    if (!findDeadBlocks())
        return false;
    if (m_tails.isEmpty()) {
        m_heads.grow(m_function.blocks.size() * size_t { m_numberOfWords });
        m_tails.grow(m_function.blocks.size() * size_t { m_numberOfWords });
    }

    for (const EdgeProof& proof : m_edgeProofs) {
        if (!isDead(proof.edge.from) && !isLeftOut(proof.edge))
            markAsPending(targetOf(m_function, proof.edge).block);
    }
    for (const PointProof& proof : m_pointProofs) {
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

void ConditionCoverage::solve(const BitVector* blocksWithIgnoredFailingEdge)
{
    m_blocksWithIgnoredFailingEdge = blocksWithIgnoredFailingEdge;
    m_didSolveFromProofs = solveFromProofs();
    if (!m_didSolveFromProofs)
        solveEveryBlock();
}

void ConditionCoverage::solveEveryBlock()
{
    m_heads.fill(0, m_function.blocks.size() * size_t { m_numberOfWords });
    m_isPending.clearAll();
    m_isReached.ensureSize(m_function.blocks.size());
    m_isReached.clearAll();
    m_isReached.quickSet(m_cfg.root().index());
    m_isPending.quickSet(m_cfg.reversePostOrderIndex(m_cfg.root()));
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    for (bool shouldRepeat = true; shouldRepeat;) {
        shouldRepeat = false;
        for (uint32_t position = 0; position < order.size(); ++position) {
            if (m_isPending.quickClear(position))
                shouldRepeat |= visit(order[position], position);
        }
    }
}

RequirementCoverage ConditionCoverage::of(uint32_t requirement)
{
    ProgramPoint point = m_requirements[requirement].point;
    if (m_didSolveFromProofs ? isDead(point.block) : !m_isReached.get(point.block.index()))
        return RequirementCoverage::Unknown;
    Row row(FillWith { }, m_numberOfWords, Word { 0 });
    if (!m_didSolveFromProofs || wasVisited(point.block))
        std::ranges::copy(headOf(point.block), row.begin());
    carryThrough(point.block, row.mutableSpan(), point.index);
    return get(row.span(), m_requirements[requirement].condition.index()) ? RequirementCoverage::Covered : RequirementCoverage::NeedsTest;
}

BranchFolder::BranchFolder(const FoldingInputs& inputs)
    : m_inputs(inputs)
    , m_function(inputs.context.function)
    , m_analyses(inputs.context.analyses)
    , m_builder(inputs.context.analyses.context(), inputs.context.function, inputs.context.types, &inputs.context.analyses)
    , m_provingValues(inputs.conditions.size())
    , m_definitions(inputs.conditions.size())
{
    for (size_t index : inputs.redundant) {
        const TestedBranch& branch = inputs.branches[index];
        m_foldedBranches.append(FoldedBranch { static_cast<uint32_t>(index), { }, { }, false });
        m_foldedBlocks.set(branch.block.index());
        m_conditionsInPlay.set(branch.condition.index());
    }
}

void BranchFolder::run()
{
    adoptProofs();
    collectUsesOfTests();
    defineProofs();
    chooseReplacementGuards();
    replaceBooleanUses();
    m_builder.applyInsertions();
    // The builder knows the values that it has put in, not the uses that were handed to them.
    m_analyses.didChangeValues();

    for (const FoldedBranch& folded : m_foldedBranches)
        fold(folded);
    // The builder sees the jump that it emits, not the branch that was taken away for it.
    m_analyses.didChangeControlFlow();
    eraseUnusedTests();
    m_builder.emptyUnreachableBlocks();
    repairProofs();
}

void BranchFolder::adoptProofs()
{
    for (const ExistingProof& proof : m_inputs.proofs) {
        if (!m_conditionsInPlay.get(proof.condition.index()))
            continue;

        ValueID value = proof.value;
        switch (proof.source) {
        case ProofSource::TakenEdge:
            if (m_foldedBlocks.get(proof.edge.from.index()))
                continue;
            [[fallthrough]];
        case ProofSource::SwitchCase:
            value = ensureEdgeResult(m_builder, m_analyses, proof.edge, testedOperandIndex);
            break;
        case ProofSource::GuardValue:
        case ProofSource::Allocation:
            break;
        }
        m_provingValues[proof.condition.index()].appendIfNotContains(value);
    }
}

TestUse BranchFolder::classify(const Use& use, const TestedBranch& branch) const
{
    switch (use.kind) {
    case Use::Kind::TerminatorOperand:
        if (use.block == branch.block)
            return TestUse::FoldedBranch;
        return m_function.at(use.block).terminator.isConditionalBranch() ? TestUse::Other : TestUse::Boolean;
    case Use::Kind::Operand:
        return m_function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard ? TestUse::Other : TestUse::Boolean;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return TestUse::Boolean;
    case Use::Kind::BlockSubject:
        return TestUse::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void BranchFolder::collectUsesOfTests()
{
    const UseLists& useLists = m_analyses.useLists();
    for (FoldedBranch& folded : m_foldedBranches) {
        const TestedBranch& branch = branchOf(folded);
        for (const Use& use : useLists.usesOf(branch.test)) {
            switch (classify(use, branch)) {
            case TestUse::FoldedBranch:
                break;
            case TestUse::Boolean:
                folded.booleanUses.append(use);
                break;
            case TestUse::Other:
                folded.hasOtherUse = true;
                break;
            }
        }
    }
}

void BranchFolder::defineProofs()
{
    struct PendingDefinition {
        size_t condition { 0 };
        ValueID value;
        ProgramPoint pointAfter;
    };

    Vector<PendingDefinition, 8> pendingDefinitions;
    const UseLists& useLists = m_analyses.useLists();
    for (size_t condition : m_conditionsInPlay) {
        for (ValueID value : m_provingValues[condition])
            pendingDefinitions.append(PendingDefinition { condition, value, useLists.pointAfter(value) });
    }

    // SSARepair redirects every use of a definition, and a proving value has uses of its own: an allocation is also the object.
    for (const PendingDefinition& pending : pendingDefinitions) {
        m_builder.setOrigin(m_function.at(pending.value).origin());
        m_definitions[pending.condition].append(m_builder.insertBefore(pending.pointAfter, IRBuilder::make(Opcode::AllGuards, { pending.value })));
    }
}

void BranchFolder::chooseReplacementGuards()
{
    for (FoldedBranch& folded : m_foldedBranches) {
        const TestedBranch& branch = branchOf(folded);
        if (m_function.at(branch.block).terminator.taken().proofs.isEmpty())
            continue;

        const Vector<ValueID, 2>& definitions = m_definitions[branch.condition.index()];
        RELEASE_ASSERT(!definitions.isEmpty());
        folded.replacementGuard = definitions.first();
    }
}

void BranchFolder::replaceBooleanUses()
{
    bool keepsTests = m_inputs.context.build.validates();
    for (FoldedBranch& folded : m_foldedBranches) {
        if (folded.booleanUses.isEmpty() && !keepsTests)
            continue;

        const TestedBranch& branch = branchOf(folded);
        Value boolean = IRBuilder::make(Opcode::Boolean);
        boolean.setBoolean(true);
        m_builder.setOrigin(m_function.at(branch.test).origin());
        ValueID constant = m_builder.insertBefore({ branch.block, branch.index }, WTF::move(boolean));
        for (const Use& use : folded.booleanUses)
            replaceUse(m_function, use, constant);
        if (!keepsTests)
            continue;
        // In a validating build the test is made all the same, and its answer is compared with the one that an earlier test stands for.
        m_builder.insertBefore(pointBeforeTerminatorOf(m_function, branch.block), IRBuilder::make(Opcode::ValidateSameValue, { branch.test, constant }));
        folded.hasOtherUse = true;
    }
}

void BranchFolder::fold(const FoldedBranch& folded)
{
    BlockID block = branchOf(folded).block;
    Terminator& terminator = m_function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
    Target taken = terminator.taken();
    Arguments arguments = WTF::move(taken.arguments);
    for (size_t result = 0; result < taken.proofs.size(); ++result)
        arguments.append(folded.replacementGuard);

    m_builder.setOrigin(terminator.origin());
    terminator = Terminator { };
    m_builder.moveToEndOf(block);
    m_builder.emitJump(taken.block, WTF::move(arguments));
}

void BranchFolder::eraseUnusedTests()
{
    for (const FoldedBranch& folded : m_foldedBranches) {
        if (folded.hasOtherUse)
            continue;

        const TestedBranch& branch = branchOf(folded);
        size_t index = m_function.at(branch.block).values.find(branch.test);
        RELEASE_ASSERT(index != notFound);
        BitVector indexes;
        indexes.set(index);
        m_builder.eraseValues(branch.block, indexes);
    }
}

void BranchFolder::repairProofs()
{
    SSARepair repair(m_builder, m_analyses);
    for (size_t condition : m_conditionsInPlay) {
        std::span<const ValueID> definitions = m_definitions[condition].span();
        if (definitions.size() < 2)
            continue;
        SSARepair::Variable variable = repair.addVariable(definitions.front());
        for (ValueID definition : definitions.subspan(1))
            repair.addDefinition(variable, definition);
    }
    repair.run();
}

GuardEliminator::GuardEliminator(FunctionPassContext& context)
    : m_context(context)
    , m_function(context.function)
    , m_analyses(context.analyses)
    , m_conditions(context.analyses)
{
}

PassOutcome GuardEliminator::run()
{
    PassOutcome outcome = PassOutcome::Unchanged;
    collectBranches();
    if (mayFindRedundantBranch()) {
        BitVector redundant = findRedundantBranches();
        if (!redundant.isEmpty()) {
            BranchFolder(FoldingInputs { m_context, m_conditions, m_branches.span(), m_proofs.span(), redundant }).run();
            m_context.build.statistics().count(StatisticID::GuardsEliminated, redundant.bitCount());
            outcome = PassOutcome::Changed;
        }
    }
    outcome |= simplifyGuardLists();
    return outcome;
}

void GuardEliminator::collectBranches()
{
    Vector<ValueID, 4>& otherTests = m_otherTests;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = m_function.at(block).terminator;
        m_hasSwitchOnShape |= terminator.kind == TerminatorKind::SwitchOnShape;
        if (!terminator.isConditionalBranch())
            continue;

        ValueID test = terminator.conditionOperand();
        if (!testsThatProveForStretch.contains(m_function.at(test).opcode))
            continue;
        if (!addBranch(block, test))
            otherTests.append(test);
    }

    for (ValueID test : otherTests) {
        if (std::optional<TestConditionID> condition = m_conditions.find(m_function.at(test)))
            m_conditionsWithOtherTests.set(condition->index());
    }
}

bool GuardEliminator::addBranch(BlockID block, ValueID test)
{
    const Terminator& terminator = m_function.at(block).terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.taken().block == terminator.notTaken().block)
        return false;

    // A test stands near the branch on it.
    size_t indexOfTest = m_function.at(block).values.reverseFind(test);
    if (indexOfTest == notFound)
        return false;

    const Value& value = m_function.at(test);
    Effects bound = declaredBoundOf(value.opcode);
    if (!bound.writes.isEmpty() || bound.mayExitSideways() || bound.mayCollect)
        return false;

    if (!m_conditions.find(value) && m_conditions.size() >= m_context.limit(LimitID::MaxTestedTypesPerFunction)) {
        m_context.gaveUp(LimitID::MaxTestedTypesPerFunction, test);
        return false;
    }

    TestConditionID condition = m_conditions.add(value);
    uint32_t index = static_cast<uint32_t>(indexOfTest);
    m_requirements.append(TestRequirement { 0, 0, condition, { block, index }, RequirementKind::AsksOnly });
    m_branches.append(TestedBranch { block, index, test, condition });
    return true;
}

bool GuardEliminator::mayFindRedundantBranch() const
{
    if (m_branches.isEmpty())
        return false;
    if (m_requirements.size() > m_conditions.size() || !m_conditionsWithOtherTests.isEmpty())
        return true;

    for (size_t index : m_conditions.stretchConditions()) {
        const TestCondition& condition = m_conditions.at(TestConditionID(index));
        if (m_hasSwitchOnShape && condition.proof == Proof::Shape)
            return true;
        if (condition.subject && !proofsOfAllocation(m_context.program, m_function, m_function.at(condition.subject)).isEmpty())
            return true;
    }
    return false;
}

// mayFindRedundantBranch(), for one condition.
bool GuardEliminator::mayBeProvenElsewhere(TestConditionID id, const Vector<uint32_t>& numberOfBranches) const
{
    if (numberOfBranches[id.index()] > 1 || m_conditionsWithOtherTests.get(id.index()))
        return true;
    const TestCondition& condition = m_conditions.at(id);
    if (!m_conditions.stretchConditions().get(id.index()))
        return false;
    if (m_hasSwitchOnShape && condition.proof == Proof::Shape)
        return true;
    return condition.subject && !proofsOfAllocation(m_context.program, m_function, m_function.at(condition.subject)).isEmpty();
}

// The one branch on a condition that nothing else proves is not redundant: on the way to its first turn nothing has proven the condition.
void GuardEliminator::takeBranchesThatMayBeRedundantFrom(const GuardEliminator& all)
{
    Vector<uint32_t> numberOfBranches(FillWith { }, all.m_conditions.size(), 0u);
    for (const TestedBranch& branch : all.m_branches)
        ++numberOfBranches[branch.condition.index()];

    m_hasSwitchOnShape = all.m_hasSwitchOnShape;
    for (const TestedBranch& branch : all.m_branches) {
        if (all.mayBeProvenElsewhere(branch.condition, numberOfBranches))
            addBranch(branch.block, branch.test);
    }
    for (ValueID test : all.m_otherTests) {
        if (std::optional<TestConditionID> condition = m_conditions.find(m_function.at(test)))
            m_conditionsWithOtherTests.set(condition->index());
    }
}

ProofOrigin GuardEliminator::originOf(const ExistingProof& proof)
{
    switch (proof.source) {
    case ProofSource::TakenEdge:
        return ProofOrigin::Test;
    case ProofSource::SwitchCase:
        return ProofOrigin::SwitchCase;
    case ProofSource::Allocation:
        return ProofOrigin::Allocation;
    case ProofSource::GuardValue:
        break;
    }

    // A user needs everything its guard proves about what it relies on to hold, so a guard that proves more than the folded test did cannot replace it.
    const GuardFacts& proven = m_analyses.guardFacts(proof.value);
    const GuardOrigin* origin = proven.facts.size() == 1 ? proven.facts.first().singleOrigin() : nullptr;
    if (!origin)
        return ProofOrigin::Unknown;

    switch (origin->source) {
    case GuardSource::Test:
        return ProofOrigin::Test;
    case GuardSource::SwitchCase:
        return ProofOrigin::SwitchCase;
    case GuardSource::Allocation:
        return ProofOrigin::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Vector<ProofSet> GuardEliminator::findProofsReliedOn(const BitVector& conditions)
{
    Vector<ProofSet> reliedOn(m_conditions.size());
    auto addUser = [&](const auto& user, ProofSet proofs) {
        if (proofs.isEmpty())
            return;
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (user.operandKind(operandIndex) != OperandKind::Guard)
                return;
            for (const GuardFact& fact : m_analyses.guardFacts(operand).facts) {
                for (const GuardOrigin& origin : fact.origins) {
                    if (origin.source != GuardSource::Test)
                        continue;
                    std::optional<TestConditionID> condition = m_conditions.find(m_function.at(origin.test));
                    if (condition && conditions.get(condition->index()))
                        reliedOn[condition->index()] |= proofs;
                }
            }
        });
    };

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Block& contents = m_function.at(block);
        for (ValueID id : contents.values) {
            const Value& value = m_function.at(id);
            addUser(value, proofsNeededFromGuard(m_context.program, m_function, value));
        }
        addUser(contents.terminator, reliesOf(contents.terminator.kind));
    }
    return reliedOn;
}

bool GuardEliminator::canReplaceTests(const ExistingProof& allocationProof, ProofSet reliedOn) const
{
    const TestCondition& condition = m_conditions.at(allocationProof.condition);
    const Value& allocation = m_function.at(allocationProof.value);

    AbstractHeapSet heapsDependedOn;
    for (const MemoryLocation& location : condition.dependsOn)
        heapsDependedOn.add(location.heap());

    // Whoever names the allocation gets all it proves, and each of those proofs has to last as long as a passed test does.
    reliedOn.add(condition.proof);
    ProofSet proven = proofsOfAllocation(m_context.program, m_function, allocation);
    return std::ranges::all_of(allMembersOf<Proof>, [&](Proof proof) {
        if (!reliedOn.contains(proof) || !proven.contains(proof))
            return true;
        return std::ranges::all_of(locationsBehindAllocationProof(proof), [&](const MemoryLocation& location) {
            return heapsDependedOn.contains(location.heap());
        });
    });
}

void GuardEliminator::findProofs(ProofsHeldByGuards heldByGuards)
{
    m_proofsFromTests.shrink(0);
    m_otherProofs.shrink(0);
    Vector<ExistingProof> proofs = findExistingProofs(m_context, m_conditions, heldByGuards);

    BitVector conditionsOnAllocations;
    for (const ExistingProof& proof : proofs) {
        if (proof.source == ProofSource::Allocation)
            conditionsOnAllocations.set(proof.condition.index());
    }

    BitVector conditionsProvenByAllocation;
    if (!conditionsOnAllocations.isEmpty()) {
        Vector<ProofSet> reliedOn = findProofsReliedOn(conditionsOnAllocations);
        for (const ExistingProof& proof : proofs) {
            if (proof.source == ProofSource::Allocation && canReplaceTests(proof, reliedOn[proof.condition.index()]))
                conditionsProvenByAllocation.set(proof.condition.index());
        }
    }

    for (const ExistingProof& proof : proofs) {
        bool isProvenByAllocation = conditionsProvenByAllocation.get(proof.condition.index());
        switch (originOf(proof)) {
        case ProofOrigin::Unknown:
            break;
        case ProofOrigin::Test:
            m_proofsFromTests.append(proof);
            break;
        case ProofOrigin::SwitchCase:
            if (!isProvenByAllocation)
                m_otherProofs.append(proof);
            break;
        case ProofOrigin::Allocation:
            if (isProvenByAllocation)
                m_otherProofs.append(proof);
            break;
        }
    }
}

// Nearly every function has no redundant branch.
bool GuardEliminator::findsNoRedundantBranchWithoutGuards()
{
    findProofs(ProofsHeldByGuards::AreLeftOut);
    uint64_t maxNumberOfRounds = m_context.limit(LimitID::MaxGuardEliminationRounds);
    uint64_t numberOfRounds = 0;
    for (;;) {
        m_proofs = m_proofsFromTests;
        m_proofs.appendVector(m_otherProofs);
        ConditionCoverage coverage(m_analyses, m_conditions, m_requirements.span(), m_proofs.span());

        BitVector assumed(m_branches.size());
        for (size_t index = 0; index < m_branches.size(); ++index)
            assumed.quickSet(index);
        BranchVerdicts verdicts;
        for (;;) {
            if (numberOfRounds++ >= maxNumberOfRounds)
                return false;
            verdicts = solveAndClassifyBranches(coverage, &assumed, assumed);
            BitVector stillAssumed = verdicts.redundant;
            stillAssumed.merge(verdicts.unreachable);
            if (stillAssumed == assumed)
                break;
            assumed = WTF::move(stillAssumed);
        }
        BitVector mixedConditions = findMixedConditions(verdicts);
        if (mixedConditions.isEmpty())
            return verdicts.redundant.isEmpty();
        m_otherProofs.removeAllMatching([&](const ExistingProof& proof) {
            return mixedConditions.get(proof.condition.index());
        });
    }
}

BitVector GuardEliminator::findRedundantBranches()
{
    {
        GuardEliminator firstSearch(m_context);
        firstSearch.takeBranchesThatMayBeRedundantFrom(*this);
        if (firstSearch.m_branches.isEmpty() || firstSearch.findsNoRedundantBranchWithoutGuards())
            return { };
    }
    findProofs(ProofsHeldByGuards::AreListed);
    for (;;) {
        m_proofs = m_proofsFromTests;
        m_proofs.appendVector(m_otherProofs);

        ConditionCoverage coverage(m_analyses, m_conditions, m_requirements.span(), m_proofs.span());
        BranchVerdicts verdicts = findCoveredBranches(coverage);
        BitVector mixedConditions = findMixedConditions(verdicts);
        if (mixedConditions.isEmpty())
            return WTF::move(verdicts.redundant);

        m_otherProofs.removeAllMatching([&](const ExistingProof& proof) {
            return m_didReachRoundLimit || mixedConditions.get(proof.condition.index());
        });
    }
}

BranchVerdicts GuardEliminator::findCoveredBranches(ConditionCoverage& coverage)
{
    BitVector assumed(m_branches.size());
    for (size_t index = 0; index < m_branches.size(); ++index)
        assumed.quickSet(index);

    uint64_t maxNumberOfRounds = m_context.limit(LimitID::MaxGuardEliminationRounds);
    while (m_numberOfRounds < maxNumberOfRounds) {
        ++m_numberOfRounds;

        BranchVerdicts verdicts = solveAndClassifyBranches(coverage, &assumed, assumed);
        BitVector stillAssumed = verdicts.redundant;
        stillAssumed.merge(verdicts.unreachable);
        if (stillAssumed == assumed)
            return verdicts;
        assumed = WTF::move(stillAssumed);
    }

    if (!std::exchange(m_didReachRoundLimit, true))
        m_context.gaveUp(LimitID::MaxGuardEliminationRounds);

    BitVector allBranches(m_branches.size());
    for (size_t index = 0; index < m_branches.size(); ++index)
        allBranches.quickSet(index);
    return solveAndClassifyBranches(coverage, nullptr, allBranches);
}

// The failing edges of the branches that are assumed to be redundant are left out.
BranchVerdicts GuardEliminator::solveAndClassifyBranches(ConditionCoverage& coverage, const BitVector* assumed, const BitVector& branches) const
{
    if (branches.isEmpty())
        return { };

    BitVector blocksWithIgnoredFailingEdge(m_function.blocks.size());
    if (assumed) {
        for (size_t index : *assumed)
            blocksWithIgnoredFailingEdge.quickSet(m_branches[index].block.index());
    }
    coverage.solve(assumed ? &blocksWithIgnoredFailingEdge : nullptr);

    if (m_context.build.options().verifiesAfterEveryPass) [[unlikely]] {
        TestEvents events(TestEventInputs { m_analyses, m_conditions, { }, m_requirements.span(), m_proofs.span(), { }, false });
        TestCoverage coverageOfEveryBlock(TestCoverageInputs { events, GapBudget::unlimited(), assumed ? &blocksWithIgnoredFailingEdge : nullptr });
        for (size_t index : branches)
            RELEASE_ASSERT(coverage.of(static_cast<uint32_t>(index)) == coverageOfEveryBlock.of(static_cast<uint32_t>(index)));
    }

    BranchVerdicts verdicts;
    for (size_t index : branches) {
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
BitVector GuardEliminator::findMixedConditions(const BranchVerdicts& verdicts) const
{
    BitVector mixedConditions;
    if (m_otherProofs.isEmpty())
        return mixedConditions;

    BitVector conditionsInPlay;
    BitVector conditionsStillTested = m_conditionsWithOtherTests;
    for (size_t index = 0; index < m_branches.size(); ++index) {
        size_t condition = m_branches[index].condition.index();
        if (verdicts.redundant.get(index))
            conditionsInPlay.set(condition);
        else if (!verdicts.unreachable.get(index))
            conditionsStillTested.set(condition);
    }

    for (const ExistingProof& proof : m_otherProofs)
        mixedConditions.set(proof.condition.index());
    mixedConditions.filter(conditionsInPlay);
    mixedConditions.filter(conditionsStillTested);
    return mixedConditions;
}

Vector<ValueID, 4> GuardEliminator::flattenedGuardsOf(ValueID list) const
{
    Vector<ValueID, 4> guards;
    for (ValueID operand : m_function.at(list).guardsOperands()) {
        const Value& guard = m_function.at(operand);
        if (guard.opcode != Opcode::AllGuards) {
            guards.appendIfNotContains(operand);
            continue;
        }
        for (ValueID nested : guard.guardsOperands())
            guards.appendIfNotContains(nested);
    }
    return guards;
}

PassOutcome GuardEliminator::simplifyGuardLists()
{
    if (!m_analyses.opcodesOfValues().contains(Opcode::AllGuards))
        return PassOutcome::Unchanged;

    Vector<ValueID, 8> lists;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID value : m_function.at(block).values) {
            if (m_function.at(value).opcode == Opcode::AllGuards)
                lists.append(value);
        }
    }
    if (lists.isEmpty())
        return PassOutcome::Unchanged;

    auto isOperandUse = [](const Use& use) {
        return use.kind == Use::Kind::Operand || use.kind == Use::Kind::TerminatorOperand;
    };

    const UseLists& useLists = m_analyses.useLists();
    ValueSubstitution substitution(m_function);
    bool didChange = false;
    for (ValueID list : lists) {
        Vector<ValueID, 4> guards = flattenedGuardsOf(list);
        if (!std::ranges::equal(guards, m_function.at(list).guardsOperands())) {
            m_function.edit(list, ValueEdit::Definition).setGuardsOperands(guards.span());
            didChange = true;
        }
        if (guards.size() != 1)
            continue;

        // An allocation may stand where an operand of kind Guard is wanted, not as an argument for a parameter of type guard.
        if (isGuardValue(m_function, guards.first()) || std::ranges::all_of(useLists.usesOf(list), isOperandUse)) {
            substitution.replace(list, guards.first());
            didChange = true;
        }
    }

    if (didChange)
        m_analyses.didChangeValues();
    substitution.apply(m_function, &m_analyses);
    didChange |= eraseUnusedGuardLists();
    return didChange ? PassOutcome::Changed : PassOutcome::Unchanged;
}

bool GuardEliminator::eraseUnusedGuardLists()
{
    Vector<std::pair<BlockID, BitVector>, 4> unusedLists;
    const UseLists& useLists = m_analyses.useLists();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        BitVector indexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (m_function.at(values[index]).opcode == Opcode::AllGuards && !useLists.useCount(values[index]))
                indexes.set(index);
        }
        if (!indexes.isEmpty())
            unusedLists.append({ block, WTF::move(indexes) });
    }
    if (unusedLists.isEmpty())
        return false;

    IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
    for (const auto& [block, indexes] : unusedLists)
        builder.eraseValues(block, indexes);
    return true;
}

} // anonymous namespace

static Vector<BlockID, 4> findInstanceTestsOfTypedObjects(FunctionPassContext& context)
{
    const Function& function = context.function;
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
    return blocks;
}

static PassOutcome foldInstanceTestsOfTypedObjects(FunctionPassContext& context)
{
    if (context.build.validates() || !context.analyses.opcodesOfValues().contains(Opcode::BelongsToInstance))
        return PassOutcome::Unchanged;
    Vector<BlockID, 4> blocks = findInstanceTestsOfTypedObjects(context);
    if (blocks.isEmpty())
        return PassOutcome::Unchanged;

    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (BlockID block : blocks) {
        Terminator& terminator = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        ValueID test = terminator.conditionOperand();
        Target taken = terminator.taken();
        Arguments arguments = WTF::move(taken.arguments);

        builder.setOrigin(terminator.origin());
        terminator = Terminator { };
        builder.moveToEndOf(block);
        if (!taken.proofs.isEmpty()) {
            ValueID noProof = builder.append(Opcode::AllGuards, { });
            for (size_t result = 0; result < taken.proofs.size(); ++result)
                arguments.append(noProof);
        }
        builder.emitJump(taken.block, WTF::move(arguments));

        BitVector indexes;
        indexes.set(function.at(block).values.find(test));
        builder.eraseValues(block, indexes);
    }
    // The builder sees the jumps that it emits, not the branches that were taken away for them.
    context.analyses.didChangeControlFlow();
    builder.emptyUnreachableBlocks();
    context.build.statistics().count(StatisticID::GuardsEliminated, blocks.size());
    return PassOutcome::Changed;
}

PassOutcome eliminateGuards(FunctionPassContext& context)
{
    PassOutcome outcome = foldInstanceTestsOfTypedObjects(context);
    outcome |= GuardEliminator(context).run();
    // The analyses were told of every edit, so whatever they hold was made behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
