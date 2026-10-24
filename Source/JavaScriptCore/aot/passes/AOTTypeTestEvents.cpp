/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestEvents.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTGrouping.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ResultReadExtent : uint8_t { LeavesBlock, StaysInBlock };

struct TypeTestResultReads {
    ResultReadExtent extent { ResultReadExtent::LeavesBlock };
    uint32_t end { 0 };
};

} // anonymous namespace

static bool isOnEdge(ProofSource source)
{
    switch (source) {
    case ProofSource::TakenEdge:
    case ProofSource::SwitchCase:
        return true;
    case ProofSource::GuardValue:
    case ProofSource::Allocation:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static TypeTestResultReads findResultReadsOf(const UseLists& useLists, ValueID producer, BlockID block, uint32_t numberOfValues)
{
    uint32_t end = 0;
    for (const Use& use : useLists.usesOf(producer)) {
        if (use.block != block)
            return { };
        switch (use.kind) {
        case Use::Kind::Operand:
            end = std::max(end, useLists.locationOf(use.user).index + 1);
            break;
        case Use::Kind::TerminatorOperand:
            end = numberOfValues;
            break;
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return { };
        }
    }
    return { ResultReadExtent::StaysInBlock, end };
}

TestEvents::TestEvents(const TestEventInputs& inputs)
    : m_inputs(inputs)
    , m_function(inputs.analyses.function())
    , m_cfg(inputs.analyses.cfg())
    , m_hasEvent(m_function.values.size())
    , m_bytesAfterLastEvent(FillWith { }, m_function.blocks.size(), 0u)
    , m_isBarrier(m_function.values.size())
    , m_isEnteredOnlyFromRarerCode(m_function.blocks.size())
    , m_lastResultRead(FillWith { }, m_function.values.size(), 0u)
{
    findBlocksEnteredOnlyFromRarerCode();
    collectEdgeItems();
    collectPointItems();
    findResultReads();
    collectEvents();
}

// Each pass that makes a slow path marks its own blocks, so a block may be marked likelier than all that leads to it.
void TestEvents::findBlocksEnteredOnlyFromRarerCode()
{
    BitVector isReached(m_function.blocks.size());
    Vector<BlockID, 16> worklist;
    auto reachFrom = [&](BlockID first) {
        isReached.set(first.index());
        worklist.append(first);
        while (!worklist.isEmpty()) {
            BlockID from = worklist.takeLast();
            for (BlockID target : m_cfg.successors(from)) {
                if (isReached.get(target.index()))
                    continue;
                bool comesBack = isRarerThan(m_function.at(from).frequency, m_function.at(target).frequency) && !std::ranges::contains(m_cfg.incomingEdges(target), Edge::toHandlerOf(from));
                if (comesBack)
                    continue;
                isReached.set(target.index());
                worklist.append(target);
            }
        }
    };

    reachFrom(m_cfg.root());
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (isReached.get(block.index()))
            continue;
        m_isEnteredOnlyFromRarerCode.set(block.index());
        reachFrom(block);
    }
}

void TestEvents::collectEdgeItems()
{
    size_t numberOfBlocks = m_function.blocks.size();
    groupByIndex(numberOfBlocks, m_firstEdgeProof, m_edgeProofs, [&](const auto& visit) {
        for (uint32_t index = 0; index < m_inputs.proofs.size(); ++index) {
            const ExistingProof& proof = m_inputs.proofs[index];
            if (isOnEdge(proof.source) && m_cfg.isReachable(proof.edge.from))
                visit(proof.edge.from.index(), EdgeItem { proof.edge, index });
        }
    });
    groupByIndex(numberOfBlocks, m_firstEdgeTest, m_edgeTests, [&](const auto& visit) {
        for (uint32_t index = 0; index < m_inputs.hoistedTests.size(); ++index) {
            Edge edge = m_inputs.hoistedTests[index].edge;
            if (m_cfg.isReachable(edge.from))
                visit(edge.from.index(), EdgeItem { edge, index });
        }
    });
}

std::span<const TestEvents::EdgeItem> TestEvents::itemsFrom(BlockID block, const Vector<EdgeItem>& items, const Vector<uint32_t>& firstItem)
{
    return entriesOfGroup(firstItem, items, block.index());
}

void TestEvents::collectPointItems()
{
    auto orderOf = [&](const PointItem& item) {
        return std::pair { m_cfg.reversePostOrderIndex(item.point.block), item.point.index };
    };

    for (uint32_t index = 0; index < m_inputs.requirements.size(); ++index) {
        PointItem item { m_inputs.requirements[index].point, index };
        if (!m_cfg.isReachable(item.point.block))
            continue;
        RELEASE_ASSERT(m_requirementsByPoint.isEmpty() || orderOf(m_requirementsByPoint.last()) <= orderOf(item));
        m_requirementsByPoint.append(item);
    }

    const UseLists& useLists = analyses().useLists();
    for (uint32_t index = 0; index < m_inputs.proofs.size(); ++index) {
        const ExistingProof& proof = m_inputs.proofs[index];
        if (isOnEdge(proof.source))
            continue;
        ValueLocation location = useLists.locationOf(proof.value);
        switch (location.placement) {
        case ValuePlacement::Detached:
            break;
        case ValuePlacement::Parameter:
            if (m_cfg.isReachable(location.block))
                m_parameterProofsByBlock.append(PointItem { ProgramPoint::atHeadOf(location.block), index });
            break;
        case ValuePlacement::InBlock:
            if (m_cfg.isReachable(location.block))
                m_valueProofsByPoint.append(PointItem { { location.block, location.index }, index });
            break;
        }
    }
    AOT::stableSort(m_parameterProofsByBlock, { }, orderOf);
    AOT::stableSort(m_valueProofsByPoint, { }, orderOf);
}

static bool readsAsDefinition(const Program& program, const Function& function, const Value& value)
{
    if (numberOfResultsOf(program, function, value))
        return true;
    std::optional<OpaqueKind> opaque = value.type ? program.at(value.type).opaque : std::nullopt;
    return opaque && mustBeDefinition(operandKindOf(*opaque));
}

void TestEvents::findResultReads()
{
    const Program& program = analyses().program();
    const UseLists& useLists = analyses().useLists();
    std::span<const PointItem> remainingRequirements = m_requirementsByPoint.span();
    for (BlockID block : m_cfg.reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;

        size_t numberOfRequirements = 0;
        while (numberOfRequirements < remainingRequirements.size() && remainingRequirements[numberOfRequirements].point.block == block)
            ++numberOfRequirements;
        std::span<const PointItem> requirements = remainingRequirements.first(numberOfRequirements);
        remainingRequirements = remainingRequirements.subspan(numberOfRequirements);

        // A failed test enters the generic copy, where a read after the test would have two producers: the one before the test and its copy.
        auto mayPlaceTestBetween = [&](uint32_t producerIndex, uint32_t endOfReads) {
            return std::ranges::any_of(requirements, [&](const PointItem& item) {
                return item.point.index > producerIndex && item.point.index < endOfReads && m_inputs.requirements[item.index].kind == RequirementKind::PlacesTest;
            });
        };

        uint32_t lastRead = 0;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID producer = values[index];
            if (readsAsDefinition(program, m_function, m_function.at(producer))) {
                TypeTestResultReads reads = findResultReadsOf(useLists, producer, block, values.size());
                switch (reads.extent) {
                case ResultReadExtent::LeavesBlock:
                    m_isBarrier.set(producer.index());
                    break;
                case ResultReadExtent::StaysInBlock:
                    if (mayPlaceTestBetween(index, reads.end))
                        m_isBarrier.set(producer.index());
                    else
                        lastRead = std::max(lastRead, reads.end);
                    break;
                }
            }
            // The entry of a value covers every earlier value of its block, so that one lookup answers for a prefix of the block.
            m_lastResultRead[producer.index()] = lastRead;
        }
    }
}

// Only a mark says that a block is a slow path: the arm of a failed test, a generic copy.
bool TestEvents::isRejoiningEdge(Edge edge) const
{
    if (edge.isHandlerEdge())
        return false;
    BlockID target = targetOf(m_function, edge).block;
    if (m_isEnteredOnlyFromRarerCode.get(target.index()))
        return false;
    return isRarerThan(m_function.at(edge.from).frequency, m_function.at(target).frequency);
}

uint32_t TestEvents::endOfResultReadsAfter(ProgramPoint point) const
{
    const BlockValues& values = m_function.at(point.block).values;
    uint32_t end = point.index;
    while (end) {
        uint32_t lastRead = m_lastResultRead[values[end - 1].index()];
        if (lastRead <= end)
            break;
        end = lastRead;
    }
    return end;
}

const OpcodeSet& opcodesThatMayEndProofs()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            Effects bound = declaredBoundOf(opcode);
            if (!effectFactsOf(opcode).staysWithinDeclaredBound || bound.maySuspend || !bound.writes.isEmpty())
                result.add(opcode);
        }
        return result;
    }();
    return opcodes;
}

void TestEvents::collectEvents()
{
    m_guardDependencies.grow(conditions().size());
    for (size_t index : conditions().stretchConditions())
        m_guardDependencies[index] = dependenciesOfGuardOf(analyses(), conditions().at(TestConditionID(index)).test);
    groupConditionsByDependencies();
    findWhereConditionsMatter();

    std::span<const BlockID> order = m_cfg.reversePostOrder();
    m_firstEvent.reserveInitialCapacity(order.size() + 1);
    for (BlockID block : order) {
        m_firstEvent.append(m_events.size());
        collectEventsOf(block);
    }
    m_firstEvent.append(m_events.size());
}

void TestEvents::groupConditionsByDependencies()
{
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
    m_dependencyGroups.fill(0, conditions().size());
    m_subjectsFromOutside.ensureSize(conditions().size());
    for (size_t conditionIndex : conditions().stretchConditions()) {
        const TestCondition& condition = conditions().at(TestConditionID(conditionIndex));
        const GuardDependencies& dependencies = *m_guardDependencies[conditionIndex];
        uint32_t hash = hashOf(condition, dependencies);
        std::optional<uint32_t> group = index.find(hash, [&](uint32_t position) {
            const TestCondition& other = conditions().at(TestConditionID(firstConditions[position]));
            const GuardDependencies& otherDependencies = *m_guardDependencies[firstConditions[position]];
            return other.proof == condition.proof && other.dependsOn == condition.dependsOn && otherDependencies.locations == dependencies.locations && otherDependencies.slotState == dependencies.slotState;
        });
        if (!group) {
            group = firstConditions.size();
            firstConditions.append(conditionIndex);
            index.add(hash, *group);
        }
        m_dependencyGroups[conditionIndex] = *group;
        if (dependencies.subject && comesFromOutside(analyses(), dependencies.subject))
            m_subjectsFromOutside.quickSet(conditionIndex);
    }
    m_writtenOfDependencyGroups.grow(firstConditions.size());
}

// A proof about a value begins behind the definition of the value, which dominates it, and nobody asks for it behind the last requirement.
void TestEvents::findWhereConditionsMatter()
{
    const UseLists& useLists = analyses().useLists();
    Vector<uint32_t> lastRequirement(FillWith { }, conditions().size(), 0u);
    for (const TestRequirement& requirement : m_inputs.requirements) {
        uint32_t& last = lastRequirement[requirement.condition.index()];
        last = std::max(last, m_cfg.reversePostOrderIndex(requirement.point.block));
    }
    for (size_t index : conditions().stretchConditions()) {
        ValueID subject = conditions().at(TestConditionID(index)).subject;
        BlockID definingBlock = subject ? useLists.blockOf(subject) : BlockID();
        uint32_t first = definingBlock && m_cfg.isReachable(definingBlock) ? m_cfg.reversePostOrderIndex(definingBlock) : 0;
        m_whereConditionsMatter.append({ static_cast<uint32_t>(index), first, lastRequirement[index] });
    }

    std::span<const BlockID> order = m_cfg.reversePostOrder();
    m_firstBlockReachedFrom = Vector<uint32_t>(order.size(), [](size_t position) {
        return static_cast<uint32_t>(position);
    });
    if (!m_cfg.hasCycle())
        return;
    const BlockCycles& cycles = analyses().blockCycles();
    Vector<uint32_t, 8> firstBlockOfCycle;
    for (uint32_t position = 0; position < order.size(); ++position) {
        std::optional<unsigned> cycle = cycles.outermostCycleOf(order[position]);
        if (!cycle)
            continue;
        while (firstBlockOfCycle.size() <= *cycle)
            firstBlockOfCycle.append(UINT32_MAX);
        firstBlockOfCycle[*cycle] = std::min(firstBlockOfCycle[*cycle], position);
        m_firstBlockReachedFrom[position] = firstBlockOfCycle[*cycle];
    }
}

std::span<const uint32_t> TestEvents::conditionsThatMatterIn(BlockID block)
{
    if (m_blockOfConditionsThatMatter == block)
        return m_conditionsThatMatter.span();
    m_blockOfConditionsThatMatter = block;
    m_conditionsThatMatter.shrink(0);
    uint32_t order = m_cfg.reversePostOrderIndex(block);
    uint32_t firstBlockReached = m_firstBlockReachedFrom[order];
    for (const BlocksWhereConditionMatters& range : m_whereConditionsMatter) {
        if (range.first <= order && firstBlockReached <= range.last)
            m_conditionsThatMatter.append(range.condition);
    }
    return m_conditionsThatMatter.span();
}

std::span<const TestEvent> TestEvents::eventsOf(BlockID block) const
{
    uint32_t order = m_cfg.reversePostOrderIndex(block);
    return m_events.subspan(m_firstEvent[order], m_firstEvent[order + 1] - m_firstEvent[order]);
}

void TestEvents::collectEventsOf(BlockID id)
{
    const Program& program = analyses().program();
    const Block& block = m_function.at(id);
    uint32_t numberOfValues = block.values.size();
    uint32_t order = m_cfg.reversePostOrderIndex(id);
    AbstractHeapSet heapsDependedOn = conditions().heapsDependedOn();

    auto itemsOfBlock = [&](const Vector<PointItem>& items) -> std::span<const PointItem> {
        auto range = std::ranges::equal_range(items.span(), order, { }, [&](const PointItem& item) {
            return m_cfg.reversePostOrderIndex(item.point.block);
        });
        return { range.begin(), range.end() };
    };
    auto takeItemsAt = [](std::span<const PointItem>& items, uint32_t index, const auto& functor) {
        for (; !items.empty() && items.front().point.index == index; items = items.subspan(1))
            functor(items.front().index);
    };
    std::span<const PointItem> requirements = itemsOfBlock(m_requirementsByPoint);
    std::span<const PointItem> valueProofs = itemsOfBlock(m_valueProofsByPoint);

    m_pendingBytes = 0;
    for (const PointItem& item : itemsOfBlock(m_parameterProofsByBlock))
        appendEvent(TestEvent::Kind::ParameterProof, 0, item.index);

    for (uint32_t index = 0; index < numberOfValues; ++index) {
        ValueID value = block.values[index];
        if (m_isBarrier.get(value.index()))
            appendEvent(TestEvent::Kind::Barrier, index, 0);

        const TestSite* site = nullptr;
        takeItemsAt(requirements, index, [&](uint32_t requirementIndex) {
            appendEvent(TestEvent::Kind::Requirement, index, requirementIndex);
            const TestRequirement& requirement = m_inputs.requirements[requirementIndex];
            if (requirement.kind == RequirementKind::PlacesTest)
                site = &m_inputs.sites[requirement.site];
        });

        std::optional<EffectsOnEdges> effectsWhenPassed;
        if (site)
            effectsWhenPassed = EffectsOnEdges::onBothEdges(site->effectsWhenPassed);
        else if (m_inputs.measuresCodeBetweenEvents)
            m_pendingBytes += estimatedCodeBytes(program, m_function, m_function.at(value));

        // Most values are of operations that write nothing and suspend nothing, whatever their operands are.
        if (site || opcodesThatMayEndProofs().contains(m_function.at(value).opcode)) {
            const EffectsOnEdges& effects = effectsWhenPassed ? *effectsWhenPassed : analyses().effectsOf(value);
            if (effects.normal.maySuspend)
                appendEvent(TestEvent::Kind::Suspension, index, 0);
            else if (effects.normal.writes.containsAny(heapsDependedOn))
                appendEnding(index, findConditionsEndedBy(value, effects, conditionsThatMatterIn(id)));
        }

        takeItemsAt(valueProofs, index, [&](uint32_t proof) {
            appendEvent(TestEvent::Kind::ValueProof, index, proof);
        });
    }
    RELEASE_ASSERT(requirements.empty() && valueProofs.empty());

    const Effects& effectsOfTerminator = analyses().effectsOfTerminator(id).normal;
    if (effectsOfTerminator.maySuspend)
        appendEvent(TestEvent::Kind::Suspension, numberOfValues, 0);
    else if (effectsOfTerminator.writes.containsAny(heapsDependedOn))
        appendEnding(numberOfValues, findConditionsEndedByTerminator(effectsOfTerminator, conditionsThatMatterIn(id)));

    if (m_inputs.measuresCodeBetweenEvents)
        m_pendingBytes += estimatedCodeBytes(block.terminator);
    m_bytesAfterLastEvent[id.index()] = m_pendingBytes;

    for (const TestEvent& event : m_events.span().subspan(m_firstEvent.last())) {
        if (event.index < numberOfValues)
            m_hasEvent.quickSet(block.values[event.index].index());
    }
}

void TestEvents::appendEvent(TestEvent::Kind kind, uint32_t index, uint32_t payload)
{
    m_events.append(TestEvent { kind, index, payload, std::exchange(m_pendingBytes, 0) });
}

void TestEvents::appendEnding(uint32_t index, BitVector&& endedConditions)
{
    if (endedConditions.isEmpty())
        return;
    appendEvent(TestEvent::Kind::Ending, index, internEndedSet(WTF::move(endedConditions)));
}

BitVector TestEvents::findConditionsEndedBy(ValueID writer, const EffectsOnEdges& effects, std::span<const uint32_t> conditionsThatMatter) const
{
    BitVector ended;
    EndingsAtWriter endings(analyses(), writer, effects);
    for (uint32_t index : conditionsThatMatter) {
        const GuardDependencies& dependencies = *m_guardDependencies[index];
        WrittenByWriter& group = m_writtenOfDependencyGroups[m_dependencyGroups[index]];
        if (group.writer != writer) {
            group.writer = writer;
            group.written = { };
            if (effects.normal.writes.overlaps(dependencies.masks))
                group.written = endings.writtenOf(conditions().at(TestConditionID(index)), dependencies);
        }
        if (endings.endingOf(dependencies.subject, m_subjectsFromOutside.quickGet(index), group.written) == ProofEnding::MayEnd)
            ended.set(index);
    }
    if (analyses().context().build.options().verifiesAfterEveryPass) [[unlikely]] {
        EndingsAtWriter endingsOneByOne(analyses(), writer, effects);
        for (uint32_t index : conditionsThatMatter) {
            bool mayEnd = effects.normal.writes.overlaps(m_guardDependencies[index]->masks) && endingsOneByOne.endingOf(conditions().at(TestConditionID(index)), m_guardDependencies[index]) == ProofEnding::MayEnd;
            RELEASE_ASSERT(mayEnd == ended.get(index));
        }
    }
    return ended;
}

BitVector TestEvents::findConditionsEndedByTerminator(const Effects& effects, std::span<const uint32_t> conditionsThatMatter) const
{
    BitVector ended;
    for (uint32_t index : conditionsThatMatter) {
        if (endingOfTerminator(analyses(), conditions().at(TestConditionID(index)), effects) == ProofEnding::MayEnd)
            ended.set(index);
    }
    return ended;
}

uint32_t TestEvents::internEndedSet(BitVector&& endedConditions)
{
    size_t known = m_endedSets.find(endedConditions);
    if (known != notFound)
        return static_cast<uint32_t>(known);
    m_endedSets.append(WTF::move(endedConditions));
    return m_endedSets.size() - 1;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
