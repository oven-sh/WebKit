/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestEvents.h"

#if ENABLE(AOT)

#include "AOTCodeSizeEstimate.h"
#include "AOTGrouping.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ResultReadExtent : uint8_t { LeavesBlock, StaysInBlock };

struct ResultReads {
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

static ResultReads findResultReadsOf(const UseLists& useLists, ValueID producer, BlockID block, uint32_t numberOfValues)
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
    , m_bytesAfterLastEvent(FillWith { }, m_function.blocks.size(), 0u)
    , m_isBarrier(m_function.values.size())
    , m_lastResultRead(FillWith { }, m_function.values.size(), 0u)
{
    collectEdgeItems();
    collectPointItems();
    findResultReads();
    collectEvents();
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
    std::ranges::stable_sort(m_parameterProofsByBlock, { }, orderOf);
    std::ranges::stable_sort(m_valueProofsByPoint, { }, orderOf);
}

static bool isReadAsDefinition(const Program& program, const Function& function, const Value& value)
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
            if (isReadAsDefinition(program, m_function, m_function.at(producer))) {
                ResultReads reads = findResultReadsOf(useLists, producer, block, values.size());
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

bool TestEvents::isRejoiningEdge(Edge edge) const
{
    if (edge.isHandlerEdge())
        return false;
    const BlockFrequencies& frequencies = m_inputs.analyses.blockFrequencies();
    return frequencies.classOf(edge.from) > frequencies.classOf(targetOf(m_function, edge).block);
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

void TestEvents::collectEvents()
{
    m_guardDependencies.grow(conditions().size());
    for (size_t index : conditions().stretchConditions())
        m_guardDependencies[index] = dependenciesOfGuardOf(analyses(), conditions().at(TestConditionID(index)).test);

    std::span<const BlockID> order = m_cfg.reversePostOrder();
    m_firstEvent.reserveInitialCapacity(order.size() + 1);
    for (BlockID block : order) {
        m_firstEvent.append(m_events.size());
        collectEventsOf(block);
    }
    m_firstEvent.append(m_events.size());
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
        else
            m_pendingBytes += estimatedCodeBytes(program, m_function, m_function.at(value));
        const EffectsOnEdges& effects = effectsWhenPassed ? *effectsWhenPassed : analyses().effectsOf(value);

        if (effects.normal.maySuspend)
            appendEvent(TestEvent::Kind::Suspension, index, 0);
        else if (effects.normal.writes.containsAny(heapsDependedOn))
            appendEnding(index, computeEndedSet(value, effects));

        takeItemsAt(valueProofs, index, [&](uint32_t proof) {
            appendEvent(TestEvent::Kind::ValueProof, index, proof);
        });
    }
    RELEASE_ASSERT(requirements.empty() && valueProofs.empty());

    const Effects& effectsOfTerminator = analyses().effectsOfTerminator(id).normal;
    if (effectsOfTerminator.maySuspend)
        appendEvent(TestEvent::Kind::Suspension, numberOfValues, 0);
    else if (effectsOfTerminator.writes.containsAny(heapsDependedOn))
        appendEnding(numberOfValues, computeEndedSetOfTerminator(effectsOfTerminator));

    m_pendingBytes += estimatedCodeBytes(block.terminator);
    m_bytesAfterLastEvent[id.index()] = m_pendingBytes;
}

void TestEvents::appendEvent(TestEvent::Kind kind, uint32_t index, uint32_t payload)
{
    m_events.append(TestEvent { kind, index, payload, std::exchange(m_pendingBytes, 0) });
}

void TestEvents::appendEnding(uint32_t index, EndedSet&& set)
{
    if (set.ended.isEmpty() && set.markedStale.isEmpty())
        return;
    appendEvent(TestEvent::Kind::Ending, index, internEndedSet(WTF::move(set)));
}

TestEvents::EndedSet TestEvents::computeEndedSet(ValueID writer, const EffectsOnEdges& effects) const
{
    EndedSet set;
    EndingsAtWriter endings(analyses(), writer, effects);
    for (size_t index : conditions().stretchConditions()) {
        if (endings.endingOf(conditions().at(TestConditionID(index)), m_guardDependencies[index]) == ProofEnding::MayEnd)
            set.ended.set(index);
    }
    splitByRenewal(set);
    return set;
}

TestEvents::EndedSet TestEvents::computeEndedSetOfTerminator(const Effects& effects) const
{
    EndedSet set;
    for (size_t index : conditions().stretchConditions()) {
        if (endingOfTerminator(analyses(), conditions().at(TestConditionID(index)), effects) == ProofEnding::MayEnd)
            set.ended.set(index);
    }
    splitByRenewal(set);
    return set;
}

void TestEvents::splitByRenewal(EndedSet& set) const
{
    switch (renewal()) {
    case RenewalPolicy::Never:
        return;
    case RenewalPolicy::ByShapeEpoch:
        break;
    }

    const BitVector& renewable = conditions().renewableConditions();
    BitVector endedAndRenewable = set.ended;
    endedAndRenewable.filter(renewable);
    if (endedAndRenewable.isEmpty())
        return;

    // One epoch test renews several conditions into one guard, so all renewable conditions become stale together.
    set.markedStale = renewable;
    set.ended.exclude(renewable);
}

uint32_t TestEvents::internEndedSet(EndedSet&& set)
{
    size_t known = m_endedSets.find(set);
    if (known != notFound)
        return static_cast<uint32_t>(known);
    m_endedSets.append(WTF::move(set));
    return m_endedSets.size() - 1;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
