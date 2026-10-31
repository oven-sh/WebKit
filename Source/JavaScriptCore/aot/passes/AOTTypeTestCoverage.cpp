/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestStretches.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTForwardDataflow.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include <ranges>
#include <utility>
#include <wtf/Hasher.h>
#include <wtf/SaturatingArithmetic.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

static bool mayWriteDependenciesOf(const TestCondition& condition, const Program& program, const Effects& effects)
{
    return effects.maySuspend || std::ranges::any_of(condition.dependsOn, [&](const MemoryLocation& location) {
        return effects.writes.overlaps(program, location);
    });
}

namespace {

struct EndingsAtWriter {
    using EndedProofs = GuardWriter::EndedProofs;

    FunctionAnalyses& analyses;
    ValueID writer;
    const Effects& effects;
    // A use that is about to be rewritten comes with the effects of its fast form. The operation as it stands bounds the objects that form reaches only if it writes at least as much.
    bool isBoundedByWriter { !effects.maySuspend && effects.writes.isSubsetOf(analyses.effectsOf(writer).normal.writes) };
    std::optional<GuardWriter> guardWriter { };

    // Nothing where it hangs on the objects that the writer reaches.
    std::optional<bool> mayEndForEveryObject(const TestCondition& condition, const GuardDependencies* dependencies)
    {
        if (condition.lastsForever() || !mayWriteDependenciesOf(condition, analyses.program(), effects))
            return false;
        if (dependencies && leavesSlotsAsTested(analyses.program(), analyses.function(), analyses.function().at(writer), effects, dependencies->slotState))
            return false;
        if (dependencies && leavesArrayStorageAsTested(analyses.program(), analyses.function(), analyses.function().at(writer), effects, dependencies->arrayStorage))
            return false;
        if (!isBoundedByWriter)
            return true;
        if (!dependencies)
            return false;
        if (!guardWriter)
            guardWriter.emplace(analyses, writer);
        return std::nullopt;
    }

    bool mayEnd(const TestCondition& condition, const std::optional<GuardDependencies>& dependencies)
    {
        std::optional<bool> mayEnd = mayEndForEveryObject(condition, dependencies ? &*dependencies : nullptr);
        return mayEnd ? *mayEnd : guardWriter->mayEnd(*dependencies);
    }

    // mayEnd() in two steps, for many conditions that differ in their subjects alone.
    GuardWriter::WrittenDependencies writtenOf(const TestCondition& condition, const GuardDependencies& dependencies)
    {
        std::optional<bool> mayEnd = mayEndForEveryObject(condition, &dependencies);
        return mayEnd ? GuardWriter::WrittenDependencies { *mayEnd ? EndedProofs::All : EndedProofs::None, 0 } : guardWriter->writtenOf(dependencies);
    }

    bool mayEnd(ValueID subject, bool subjectComesFromOutside, const GuardWriter::WrittenDependencies& written)
    {
        switch (written.endedProofs) {
        case EndedProofs::None:
            return false;
        case EndedProofs::All:
            return !analyses.context().build.isSabotaged(Sabotage::ProofsOutlastWritesToEverything);
        case EndedProofs::AboutSomeObjects:
            break;
        }
        return guardWriter->mayEndProofAbout(subject, subjectComesFromOutside, written) && !analyses.context().build.isSabotaged(Sabotage::ProofsOutlastWritesToTheirSubject);
    }
};

} // anonymous namespace

ProofEnding endingBetweenTestAndBranch(FunctionAnalyses& analyses, const TestCondition& condition, ValueID writer, const EffectsOnEdges& effectsOfWriter)
{
    if (condition.lastsForever())
        return ProofEnding::DoesNotEnd;
    std::optional<GuardDependencies> dependencies = dependenciesOfGuardOf(analyses, condition.test);
    if (dependencies)
        dependencies->slotState = TestedSlotState::None;
    return EndingsAtWriter { analyses, writer, effectsOfWriter.normal }.mayEnd(condition, dependencies) ? ProofEnding::MayEnd : ProofEnding::DoesNotEnd;
}

ConditionGroups::ConditionGroups(FunctionAnalyses& analyses, const TestConditions& conditions)
    : analyses(analyses)
    , conditions(conditions)
    , dependencies(conditions.size())
    , groupOf(FillWith { }, conditions.size(), 0u)
    , subjectsFromOutside(conditions.size())
{
    for (size_t index : conditions.stretchConditions()) {
        const Value& test = conditions.at(TestConditionID(index)).test;
        dependencies[index] = dependenciesOfGuardOf(analyses, test);
        dependencies[index]->arrayStorage = arrayStorageTestedBy(test);
    }

    auto hashOf = [](const TestCondition& condition, const GuardDependencies& dependencies) {
        Hasher hasher;
        add(hasher, condition.proof);
        add(hasher, dependencies.slotState);
        add(hasher, dependencies.arrayStorage);
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

    HashedIndex groupsByHash;
    Vector<uint32_t, 16> firstConditions;
    for (size_t index : conditions.stretchConditions()) {
        const TestCondition& condition = conditions.at(TestConditionID(index));
        const GuardDependencies& dependenciesOfCondition = *dependencies[index];
        uint32_t hash = hashOf(condition, dependenciesOfCondition);
        std::optional<uint32_t> group = groupsByHash.find(hash, [&](uint32_t position) {
            const TestCondition& other = conditions.at(TestConditionID(firstConditions[position]));
            const GuardDependencies& otherDependencies = *dependencies[firstConditions[position]];
            return other.proof == condition.proof && other.dependsOn == condition.dependsOn && otherDependencies.locations == dependenciesOfCondition.locations && otherDependencies.slotState == dependenciesOfCondition.slotState && otherDependencies.arrayStorage == dependenciesOfCondition.arrayStorage;
        });
        if (!group) {
            group = firstConditions.size();
            firstConditions.append(index);
            groupsByHash.add(hash, *group);
        }
        groupOf[index] = *group;
        if (dependenciesOfCondition.subject && comesFromOutside(analyses, dependenciesOfCondition.subject))
            subjectsFromOutside.quickSet(index);
    }
    writtenOfGroups.grow(firstConditions.size());
}

BitVector ConditionGroups::findConditionsEndedBy(ValueID writer, const EffectsOnEdges& effects, std::span<const uint32_t> stretchConditions)
{
    BitVector ended;
    EndingsAtWriter endings { analyses, writer, effects.normal };
    ++numberOfWriters;
    for (uint32_t index : stretchConditions) {
        WrittenByWriter& group = writtenOfGroups[groupOf[index]];
        if (std::exchange(group.writer, numberOfWriters) != numberOfWriters) {
            group.written = { };
            if (effects.normal.writes.overlaps(dependencies[index]->masks))
                group.written = endings.writtenOf(conditions.at(TestConditionID(index)), *dependencies[index]);
        }
        if (endings.mayEnd(dependencies[index]->subject, subjectsFromOutside.quickGet(index), group.written))
            ended.set(index);
    }
    return ended;
}

BitVector ConditionGroups::findConditionsEndedByTerminator(const Effects& effects, std::span<const uint32_t> stretchConditions) const
{
    BitVector ended;
    for (uint32_t index : stretchConditions) {
        if (mayWriteDependenciesOf(conditions.at(TestConditionID(index)), analyses.program(), effects))
            ended.set(index);
    }
    return ended;
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

bool readsAsDefinition(const Program& program, const Function& function, const Value& value)
{
    if (numberOfResultsOf(program, function, value))
        return true;
    std::optional<OpaqueKind> opaque = value.type ? program.at(value.type).opaque : std::nullopt;
    return opaque && mustBeDefinition(operandKindOf(*opaque));
}

struct TestEvents::Collector {
    struct PointItem {
        ProgramPoint point;
        uint32_t index { 0 };
    };
    struct BlocksWhereConditionMatters {
        uint32_t condition { 0 };
        uint32_t first { 0 };
        uint32_t last { 0 };
    };

    TestEvents& events;
    const TestEventInputs& inputs { events.m_inputs };
    FunctionAnalyses& analyses { inputs.analyses };
    const TestConditions& conditions { inputs.conditions };
    const Function& function { events.m_function };
    const CFG& cfg { events.m_cfg };
    const UseLists& useLists { analyses.useLists() };
    Vector<PointItem> requirementItems { };
    Vector<PointItem> valueProofItems { };
    Vector<PointItem> parameterProofItems { };
    // What the walk has yet to come to.
    std::span<const PointItem> requirements { };
    std::span<const PointItem> valueProofs { };
    std::span<const PointItem> parameterProofs { };
    std::optional<ConditionGroups> groups { };
    Vector<BlocksWhereConditionMatters> whereConditionsMatter { };
    // By position in the reverse postorder: the first position that holds a block that the block may reach.
    Vector<uint32_t> firstBlockReachedFrom { };
    Vector<uint32_t, 16> conditionsThatMatter { };
    BlockID blockOfConditionsThatMatter { };
    uint32_t pendingBytes { 0 };

    // Each pass that makes a slow path marks its own blocks, so a block may be marked likelier than all that leads to it.
    void findBlocksEnteredOnlyFromRarerCode()
    {
        BitVector isReached(function.blocks.size());
        Vector<BlockID, 16> worklist;
        auto reachFrom = [&](BlockID first) {
            isReached.set(first.index());
            worklist.append(first);
            while (!worklist.isEmpty()) {
                BlockID from = worklist.takeLast();
                for (BlockID target : cfg.successors(from)) {
                    if (isReached.get(target.index()))
                        continue;
                    bool comesBack = isRarerThan(function.at(from).frequency, function.at(target).frequency).predicted() && !std::ranges::contains(cfg.incomingEdges(target), Edge::toHandlerOf(from));
                    if (comesBack)
                        continue;
                    isReached.set(target.index());
                    worklist.append(target);
                }
            }
        };

        reachFrom(cfg.root());
        for (BlockID block : cfg.reversePostOrder()) {
            if (isReached.get(block.index()))
                continue;
            events.m_isEnteredOnlyFromRarerCode.set(block.index());
            reachFrom(block);
        }
    }

    void collectItems()
    {
        groupByIndex(function.blocks.size(), events.m_firstEdgeProof, events.m_edgeProofs, [&](const auto& visit) {
            for (uint32_t index = 0; index < inputs.proofs.size(); ++index) {
                const ExistingProof& proof = inputs.proofs[index];
                if (isOnEdge(proof.source) && cfg.isReachable(proof.edge.from))
                    visit(proof.edge.from.index(), EdgeItem { proof.edge, index });
            }
        });
        groupByIndex(function.blocks.size(), events.m_firstEdgeTest, events.m_edgeTests, [&](const auto& visit) {
            for (uint32_t index = 0; index < inputs.hoistedTests.size(); ++index) {
                Edge edge = inputs.hoistedTests[index].edge;
                if (cfg.isReachable(edge.from))
                    visit(edge.from.index(), EdgeItem { edge, index });
            }
        });

        auto orderOf = [&](const PointItem& item) {
            return std::pair { cfg.reversePostOrderIndex(item.point.block), item.point.index };
        };
        for (uint32_t index = 0; index < inputs.requirements.size(); ++index) {
            PointItem item { inputs.requirements[index].point, index };
            if (!cfg.isReachable(item.point.block))
                continue;
            RELEASE_ASSERT(requirementItems.isEmpty() || orderOf(requirementItems.last()) <= orderOf(item));
            requirementItems.append(item);
        }
        for (uint32_t index = 0; index < inputs.proofs.size(); ++index) {
            if (isOnEdge(inputs.proofs[index].source))
                continue;
            ValueLocation location = useLists.locationOf(inputs.proofs[index].value);
            switch (location.placement) {
            case ValuePlacement::Detached:
                break;
            case ValuePlacement::Parameter:
                if (cfg.isReachable(location.block))
                    parameterProofItems.append(PointItem { ProgramPoint::atHeadOf(location.block), index });
                break;
            case ValuePlacement::InBlock:
                if (cfg.isReachable(location.block))
                    valueProofItems.append(PointItem { { location.block, location.index }, index });
                break;
            }
        }
        AOT::stableSortInLine(parameterProofItems, { }, orderOf);
        AOT::stableSortInLine(valueProofItems, { }, orderOf);
        requirements = requirementItems.span();
        valueProofs = valueProofItems.span();
        parameterProofs = parameterProofItems.span();
    }

    // A proof about a value begins behind the definition of the value, which dominates it, and nobody asks for it behind the last requirement.
    void findWhereConditionsMatter()
    {
        Vector<uint32_t> lastRequirement(FillWith { }, conditions.size(), 0u);
        for (const TestRequirement& requirement : inputs.requirements) {
            uint32_t& last = lastRequirement[requirement.condition.index()];
            last = std::max(last, cfg.reversePostOrderIndex(requirement.point.block));
        }
        for (size_t index : conditions.stretchConditions()) {
            ValueID subject = conditions.at(TestConditionID(index)).subject;
            BlockID definingBlock = subject ? useLists.blockOf(subject) : BlockID();
            uint32_t first = definingBlock && cfg.isReachable(definingBlock) ? cfg.reversePostOrderIndex(definingBlock) : 0;
            whereConditionsMatter.append({ static_cast<uint32_t>(index), first, lastRequirement[index] });
        }

        std::span<const BlockID> order = cfg.reversePostOrder();
        firstBlockReachedFrom = Vector<uint32_t>(order.size(), [](size_t position) {
            return static_cast<uint32_t>(position);
        });
        if (!cfg.hasCycle())
            return;
        const BlockCycles& cycles = analyses.blockCycles();
        Vector<uint32_t, 8> firstBlockOfCycle;
        for (uint32_t position = 0; position < order.size(); ++position) {
            std::optional<unsigned> cycle = cycles.outermostCycleOf(order[position]);
            if (!cycle)
                continue;
            while (firstBlockOfCycle.size() <= *cycle)
                firstBlockOfCycle.append(UINT32_MAX);
            firstBlockOfCycle[*cycle] = std::min(firstBlockOfCycle[*cycle], position);
            firstBlockReachedFrom[position] = firstBlockOfCycle[*cycle];
        }
    }

    std::span<const uint32_t> conditionsThatMatterIn(BlockID block)
    {
        if (std::exchange(blockOfConditionsThatMatter, block) == block)
            return conditionsThatMatter.span();
        conditionsThatMatter.shrink(0);
        uint32_t order = cfg.reversePostOrderIndex(block);
        uint32_t firstBlockReached = firstBlockReachedFrom[order];
        for (const BlocksWhereConditionMatters& range : whereConditionsMatter) {
            if (range.first <= order && firstBlockReached <= range.last)
                conditionsThatMatter.append(range.condition);
        }
        return conditionsThatMatter.span();
    }

    void appendEvent(TestEvent::Kind kind, uint32_t index, uint32_t payload)
    {
        events.m_events.append(TestEvent { kind, index, payload, std::exchange(pendingBytes, 0) });
    }

    void appendEnding(uint32_t index, BitVector&& endedConditions)
    {
        if (endedConditions.isEmpty())
            return;
        size_t set = events.m_endedSets.find(endedConditions);
        if (set == notFound) {
            set = events.m_endedSets.size();
            events.m_endedSets.append(WTF::move(endedConditions));
        }
        appendEvent(TestEvent::Kind::Ending, index, static_cast<uint32_t>(set));
    }

    BitVector findConditionsEndedBy(ValueID writer, const EffectsOnEdges& effects, std::span<const uint32_t> askedConditions)
    {
        BitVector ended = groups->findConditionsEndedBy(writer, effects, askedConditions);
        if (analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]] {
            EndingsAtWriter endingsOneByOne { analyses, writer, effects.normal };
            for (uint32_t index : askedConditions) {
                bool mayEnd = effects.normal.writes.overlaps(groups->dependencies[index]->masks) && endingsOneByOne.mayEnd(conditions.at(TestConditionID(index)), groups->dependencies[index]);
                RELEASE_ASSERT(mayEnd == ended.get(index));
            }
        }
        return ended;
    }

    // Nothing where a read leaves the block.
    std::optional<uint32_t> findEndOfResultReads(ValueID producer, BlockID block, uint32_t numberOfValues)
    {
        uint32_t end = 0;
        for (const Use& use : useLists.usesOf(producer)) {
            if (use.block != block)
                return std::nullopt;
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
                return std::nullopt;
            }
        }
        return end;
    }

    void collectEventsOf(BlockID id)
    {
        const Block& block = function.at(id);
        uint32_t numberOfValues = block.values.size();
        AbstractHeapSet heapsDependedOn = conditions.heapsDependedOn();
        auto takeItemsAt = [&](std::span<const PointItem>& items, uint32_t index, const auto& functor) {
            for (; !items.empty() && items.front().point == ProgramPoint { id, index }; items = items.subspan(1))
                functor(items.front().index);
        };
        // A failed test enters the generic copy, where a read after the test would have two producers: the one before the test and its copy.
        auto mayPlaceTestBetween = [&](uint32_t producerIndex, uint32_t endOfReads) {
            for (const PointItem& item : requirements) {
                if (item.point.block != id || item.point.index >= endOfReads)
                    return false;
                if (item.point.index > producerIndex && inputs.requirements[item.index].kind == RequirementKind::PlacesTest)
                    return true;
            }
            return false;
        };

        pendingBytes = 0;
        takeItemsAt(parameterProofs, 0, [&](uint32_t proof) {
            appendEvent(TestEvent::Kind::ParameterProof, 0, proof);
        });

        uint32_t lastResultRead = 0;
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID value = block.values[index];
            if (events.startsNewStretch(value))
                appendEvent(TestEvent::Kind::Barrier, index, 0);
            if (readsAsDefinition(analyses.program(), function, function.at(value))) {
                std::optional<uint32_t> endOfReads = findEndOfResultReads(value, id, numberOfValues);
                if (!endOfReads || mayPlaceTestBetween(index, *endOfReads))
                    appendEvent(TestEvent::Kind::Barrier, index, 0);
                else
                    lastResultRead = std::max(lastResultRead, *endOfReads);
            }
            // The entry of a value covers every earlier value of its block, so that one lookup answers for a prefix of the block.
            events.m_lastResultRead[value.index()] = lastResultRead;

            const TestSite* site = nullptr;
            bool takesProofs = false;
            takeItemsAt(requirements, index, [&](uint32_t requirementIndex) {
                appendEvent(TestEvent::Kind::Requirement, index, requirementIndex);
                const TestRequirement& requirement = inputs.requirements[requirementIndex];
                if (requirement.kind != RequirementKind::AsksOnly)
                    site = &inputs.sites[requirement.site];
                takesProofs |= requirement.kind == RequirementKind::TakesProof;
            });
            if (takesProofs)
                appendEvent(TestEvent::Kind::EndOfRequirements, index, 0);

            std::optional<EffectsOnEdges> effectsWhenPassed;
            if (site)
                effectsWhenPassed = EffectsOnEdges::onBothEdges(site->effectsWhenPassed);
            else if (inputs.measuresCodeBetweenEvents)
                pendingBytes += estimatedCodeBytes(analyses.program(), function, function.at(value));

            // Most values are of operations that write nothing and suspend nothing, whatever their operands are.
            if (site || opcodesThatMayEndProofs().contains(function.at(value).opcode)) {
                const EffectsOnEdges& effects = effectsWhenPassed ? *effectsWhenPassed : analyses.effectsOf(value);
                if (effects.normal.maySuspend)
                    appendEvent(TestEvent::Kind::Suspension, index, 0);
                else if (effects.normal.writes.containsAny(heapsDependedOn))
                    appendEnding(index, findConditionsEndedBy(value, effects, conditionsThatMatterIn(id)));
            }

            takeItemsAt(valueProofs, index, [&](uint32_t proof) {
                appendEvent(TestEvent::Kind::ValueProof, index, proof);
            });
        }

        const Effects& effectsOfTerminator = analyses.effectsOfTerminator(id).normal;
        if (effectsOfTerminator.maySuspend)
            appendEvent(TestEvent::Kind::Suspension, numberOfValues, 0);
        else if (effectsOfTerminator.writes.containsAny(heapsDependedOn))
            appendEnding(numberOfValues, groups->findConditionsEndedByTerminator(effectsOfTerminator, conditionsThatMatterIn(id)));

        if (inputs.measuresCodeBetweenEvents)
            pendingBytes += estimatedCodeBytes(block.terminator);
        events.m_bytesAfterLastEvent[id.index()] = pendingBytes;

        for (const TestEvent& event : events.m_events.span().subspan(events.m_firstEvent.last())) {
            if (event.index < numberOfValues)
                events.m_hasEvent.quickSet(block.values[event.index].index());
        }
    }

    void run()
    {
        findBlocksEnteredOnlyFromRarerCode();
        collectItems();
        groups.emplace(analyses, conditions);
        findWhereConditionsMatter();

        events.m_firstEvent.reserveInitialCapacity(cfg.reversePostOrder().size() + 1);
        for (BlockID block : cfg.reversePostOrder()) {
            events.m_firstEvent.append(events.m_events.size());
            collectEventsOf(block);
        }
        events.m_firstEvent.append(events.m_events.size());
        RELEASE_ASSERT(requirements.empty() && valueProofs.empty() && parameterProofs.empty());
    }
};

TestEvents::TestEvents(const TestEventInputs& inputs)
    : m_inputs(inputs)
    , m_function(inputs.analyses.function())
    , m_cfg(inputs.analyses.cfg())
    , m_hasEvent(m_function.values.size())
    , m_bytesAfterLastEvent(FillWith { }, m_function.blocks.size(), 0u)
    , m_isEnteredOnlyFromRarerCode(m_function.blocks.size())
    , m_lastResultRead(FillWith { }, m_function.values.size(), 0u)
{
    Collector { *this }.run();
}

std::span<const TestEvent> TestEvents::eventsOf(BlockID block) const
{
    uint32_t order = m_cfg.reversePostOrderIndex(block);
    return m_events.subspan(m_firstEvent[order], m_firstEvent[order + 1] - m_firstEvent[order]);
}

// Only a mark says that a block is a slow path: the arm of a failed test, a generic copy.
Expected<bool> TestEvents::isRejoiningEdge(Edge edge) const
{
    if (edge.isHandlerEdge())
        return false;
    BlockID target = targetOf(m_function, edge).block;
    return !m_isEnteredOnlyFromRarerCode.get(target.index()) && isRarerThan(m_function.at(edge.from).frequency, m_function.at(target).frequency).predicted();
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

namespace {

struct TestState {
    bool isReachable { true };
    // Between the requirements of a site that takes proofs and their end.
    bool lacksProofOfSite { false };
    BitVector passed;
    Vector<uint32_t, 8> bytesSinceUse;
    uint32_t pendingBytes { 0 };

    void addPendingBytes(uint32_t bytes) { pendingBytes = saturatingSum<uint32_t>(pendingBytes, bytes); }

    void markAsPassed(TestConditionID condition)
    {
        passed.set(condition.index());
        if (!bytesSinceUse.isEmpty())
            bytesSinceUse[condition.index()] = 0;
    }

    void endAll()
    {
        passed.clearAll();
        bytesSinceUse.fill(0);
        pendingBytes = 0;
    }
};

struct TestCoverageProblem {
    using State = TestState;
    static constexpr bool readsEffects() { return false; }

    explicit TestCoverageProblem(const TestCoverageInputs& inputs)
        : inputs(inputs)
        , events(inputs.events)
        , cycles(events.analyses().blockCycles())
    {
        RELEASE_ASSERT(inputs.budget.outsideCycles.has_value() == inputs.budget.insideCycles.has_value());
        if (inputs.budget.outsideCycles)
            largestBudget = std::max(*inputs.budget.outsideCycles, *inputs.budget.insideCycles);
    }

    // No budget bridges a gap wider than the largest one, so one width stands for all of them. That bounds the solve.
    uint32_t widenedGap(uint32_t gap, uint32_t bytes) const
    {
        return std::min(saturatingSum<uint32_t>(gap, bytes), saturatingSum<uint32_t>(*largestBudget, 1u));
    }

    State stateAtEntry() const
    {
        State state;
        if (largestBudget)
            state.bytesSinceUse.fill(0, events.conditions().size());
        return state;
    }

    bool merge(State& into, const State& from) const
    {
        if (!from.isReachable)
            return false;
        if (!into.isReachable) {
            into = from;
            settlePendingBytes(into);
            return true;
        }

        settlePendingBytes(into);
        bool changed = false;
        BitVector passedBefore = into.passed;
        for (size_t condition : passedBefore) {
            if (!from.passed.get(condition)) {
                into.passed.clear(condition);
                if (largestBudget)
                    into.bytesSinceUse[condition] = 0;
                changed = true;
                continue;
            }
            if (!largestBudget)
                continue;
            uint32_t gap = widenedGap(from.bytesSinceUse[condition], from.pendingBytes);
            if (gap > into.bytesSinceUse[condition]) {
                into.bytesSinceUse[condition] = gap;
                changed = true;
            }
        }
        return changed;
    }

    bool isIgnoredFailingEdge(Edge edge) const
    {
        return inputs.blocksWithIgnoredFailingEdge && inputs.blocksWithIgnoredFailingEdge->get(edge.from.index())
            && &targetOf(events.function(), edge) == &events.function().at(edge.from).terminator.notTaken();
    }

    bool leavesOut(Edge edge) const { return inputs.slowPaths == SlowPaths::MayLeave && events.isRejoiningEdge(edge).predicted(); }

    void enterEdge(State& state, Edge edge) const
    {
        if (!state.isReachable)
            return;
        if (edge.isHandlerEdge()) {
            state.endAll();
            return;
        }
        if (isIgnoredFailingEdge(edge) || leavesOut(edge)) {
            state.isReachable = false;
            state.endAll();
            return;
        }

        settlePendingBytes(state);
        events.forEachProofOn(edge, [&](uint32_t proof) {
            state.markAsPassed(events.proofs()[proof].condition);
        });
        events.forEachHoistedTestOn(edge, [&](uint32_t hoistedTest) {
            state.markAsPassed(events.hoistedTests()[hoistedTest].condition);
        });
    }

    void applyEventsAt(State& state, BlockID block, uint32_t index) const
    {
        for (const TestEvent& event : std::ranges::equal_range(events.eventsOf(block), index, { }, &TestEvent::index))
            applyEvent(state, event);
    }

    void execute(State& state, const DataflowStep& step) const
    {
        if (state.isReachable && events.hasEventAt(step.id))
            applyEventsAt(state, step.block, step.index);
    }

    void executeValuesOf(State& state, BlockID block) const
    {
        if (!state.isReachable)
            return;
        uint32_t terminatorIndex = events.function().at(block).values.size();
        for (const TestEvent& event : events.eventsOf(block)) {
            if (event.index >= terminatorIndex)
                break;
            applyEvent(state, event);
        }
    }

    void executeTerminator(State& state, const DataflowTerminatorStep& step) const
    {
        if (!state.isReachable)
            return;
        applyEventsAt(state, step.block, events.function().at(step.block).values.size());
        state.addPendingBytes(events.bytesAfterLastEventOf(step.block));
    }

    RequirementCoverage classify(const State& state, TestConditionID condition, BlockID block) const
    {
        ASSERT(!state.pendingBytes);
        if (!state.passed.get(condition.index()))
            return RequirementCoverage::NeedsTest;
        std::optional<uint32_t> budget = cycles.isInCycle(block) ? inputs.budget.insideCycles : inputs.budget.outsideCycles;
        return budget && state.bytesSinceUse[condition.index()] > *budget ? RequirementCoverage::NeedsTest : RequirementCoverage::Covered;
    }

    void applyEvent(State& state, const TestEvent& event) const
    {
        state.addPendingBytes(event.bytesBefore);
        switch (event.kind) {
        case TestEvent::Kind::ParameterProof:
        case TestEvent::Kind::ValueProof:
            settlePendingBytes(state);
            state.markAsPassed(events.proofs()[event.payload].condition);
            return;
        case TestEvent::Kind::Barrier:
        case TestEvent::Kind::Suspension:
            state.endAll();
            return;
        case TestEvent::Kind::Requirement:
            applyRequirement(state, events.requirements()[event.payload]);
            return;
        case TestEvent::Kind::EndOfRequirements:
            // The site stays an operation that may run any code.
            if (std::exchange(state.lacksProofOfSite, false))
                state.endAll();
            return;
        case TestEvent::Kind::Ending:
            state.passed.exclude(events.endedBy(event));
            if (largestBudget) {
                for (size_t condition : events.endedBy(event))
                    state.bytesSinceUse[condition] = 0;
            }
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void applyRequirement(State& state, const TestRequirement& requirement) const
    {
        switch (requirement.kind) {
        case RequirementKind::AsksOnly:
            return;
        case RequirementKind::TakesProof:
            settlePendingBytes(state);
            if (classify(state, requirement.condition, requirement.point.block) == RequirementCoverage::Covered)
                state.markAsPassed(requirement.condition);
            else
                state.lacksProofOfSite = true;
            return;
        case RequirementKind::PlacesTest:
            break;
        }

        settlePendingBytes(state);
        switch (classify(state, requirement.condition, requirement.point.block)) {
        case RequirementCoverage::Covered:
        case RequirementCoverage::NeedsTest:
            state.markAsPassed(requirement.condition);
            return;
        case RequirementCoverage::Unknown:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void settlePendingBytes(State& state) const
    {
        uint32_t bytes = std::exchange(state.pendingBytes, 0u);
        if (!largestBudget || !bytes)
            return;
        for (size_t condition : state.passed)
            state.bytesSinceUse[condition] = widenedGap(state.bytesSinceUse[condition], bytes);
    }

    const TestCoverageInputs& inputs;
    const TestEvents& events;
    const BlockCycles& cycles;
    std::optional<uint32_t> largestBudget { };
};

} // anonymous namespace

TestCoverage::TestCoverage(const TestCoverageInputs& inputs)
    : m_requirements(FillWith { }, inputs.events.requirements().size(), RequirementCoverage::Unknown)
    , m_hoistedTests(FillWith { }, inputs.events.hoistedTests().size(), RequirementCoverage::Unknown)
{
    const TestEvents& events = inputs.events;
    const Function& function = events.function();
    TestCoverageProblem problem(inputs);
    Vector<std::optional<TestState>> heads = events.analyses().solveForward(problem);

    auto classify = [&](const TestState& state, TestConditionID condition, BlockID block, uint32_t requirement) {
        RequirementCoverage coverage = problem.classify(state, condition, block);
        if (coverage == RequirementCoverage::NeedsTest && state.passed.get(condition.index()))
            m_exceededBudget.set(requirement);
        return coverage;
    };
    auto classifyHoistedTestsOn = [&](Edge edge, const TestState& atTail) {
        Vector<uint32_t, 4> hoistedTests;
        events.forEachHoistedTestOn(edge, [&](uint32_t hoistedTest) {
            hoistedTests.append(hoistedTest);
        });
        if (hoistedTests.isEmpty())
            return;

        TestState state = atTail;
        problem.settlePendingBytes(state);
        events.forEachProofOn(edge, [&](uint32_t proof) {
            state.markAsPassed(events.proofs()[proof].condition);
        });
        for (uint32_t index : hoistedTests) {
            const HoistedTest& hoistedTest = events.hoistedTests()[index];
            m_hoistedTests[index] = classify(state, hoistedTest.condition, targetOf(function, edge).block, hoistedTest.requirement);
            state.markAsPassed(hoistedTest.condition);
        }
    };
    auto findWhatIsMissingOn = [&](Edge edge, const TestState& atTail) {
        const std::optional<TestState>& atHead = heads[targetOf(function, edge).block.index()];
        if (!problem.leavesOut(edge) || !atHead || !atHead->isReachable)
            return;
        BitVector passedOnEdge = atTail.passed;
        events.forEachProofOn(edge, [&](uint32_t proof) {
            passedOnEdge.set(events.proofs()[proof].condition.index());
        });
        BitVector missing = atHead->passed;
        missing.exclude(passedOnEdge);
        if (missing.isEmpty())
            return;
        if (m_firstMissingOnEdgeFrom.isEmpty())
            m_firstMissingOnEdgeFrom.fill(UINT32_MAX, function.blocks.size());
        if (m_firstMissingOnEdgeFrom[edge.from.index()] == UINT32_MAX)
            m_firstMissingOnEdgeFrom[edge.from.index()] = m_missingOnEdges.size();
        m_missingOnEdges.append({ edge, WTF::move(missing) });
    };

    for (BlockID block : events.cfg().reversePostOrder()) {
        if (!heads[block.index()] || !heads[block.index()]->isReachable)
            continue;
        TestState state = *heads[block.index()];
        for (TestEvent event : events.eventsOf(block)) {
            if (event.kind == TestEvent::Kind::Requirement) {
                ASSERT(events.requirements()[event.payload].point.block == block);
                state.addPendingBytes(std::exchange(event.bytesBefore, 0));
                problem.settlePendingBytes(state);
                m_requirements[event.payload] = classify(state, events.requirements()[event.payload].condition, block, event.payload);
            }
            problem.applyEvent(state, event);
        }
        state.addPendingBytes(events.bytesAfterLastEventOf(block));
        for (uint32_t targetIndex = 0; targetIndex < function.at(block).terminator.targets.size(); ++targetIndex) {
            classifyHoistedTestsOn(Edge { block, targetIndex }, state);
            findWhatIsMissingOn(Edge { block, targetIndex }, state);
        }
    }
}

const BitVector* TestCoverage::missingOn(Edge edge) const
{
    if (m_missingOnEdges.isEmpty())
        return nullptr;
    for (uint32_t index = m_firstMissingOnEdgeFrom[edge.from.index()]; index < m_missingOnEdges.size() && m_missingOnEdges[index].first.from == edge.from; ++index) {
        if (m_missingOnEdges[index].first == edge)
            return &m_missingOnEdges[index].second;
    }
    return nullptr;
}

static uint32_t firstValueAfter(const TestEvent& event)
{
    switch (event.kind) {
    case TestEvent::Kind::ParameterProof:
    case TestEvent::Kind::Barrier:
    case TestEvent::Kind::Requirement:
    case TestEvent::Kind::EndOfRequirements:
        return event.index;
    case TestEvent::Kind::Ending:
    case TestEvent::Kind::Suspension:
    case TestEvent::Kind::ValueProof:
        return event.index + 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool needsTestInFront(RequirementCoverage coverage)
{
    switch (coverage) {
    case RequirementCoverage::NeedsTest:
        return true;
    case RequirementCoverage::Unknown:
    case RequirementCoverage::Covered:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool haveCommonBit(const BitVector& a, const BitVector& b)
{
    for (size_t bit : a) {
        if (b.get(bit))
            return true;
    }
    return false;
}

WantedProofs::WantedProofs(const TestEvents& events, const TestCoverage& coverage)
    : m_events(events)
    , m_coverage(coverage)
    , m_wantedAtHead(events.cfg().numNodes())
    , m_wantedAtTail(events.cfg().numNodes())
    , m_wantedHoistedTests(events.hoistedTests().size())
    , m_wantedProofs(events.proofs().size())
    , m_valuesInStretch(events.function().values.size())
{
    const CFG& cfg = m_events.cfg();
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : cfg.reversePostOrder() | std::views::reverse) {
            BitVector wanted = wantedAtTail(block);
            walkBackward(block, wanted);
            if (wanted != m_wantedAtHead[block.index()]) {
                m_wantedAtHead[block.index()] = WTF::move(wanted);
                changed = true;
            }
        }
        // Without a cycle a block comes behind every block that it leads to, so another round would find the same.
        changed &= cfg.hasCycle();
    }
    RELEASE_ASSERT(m_wantedAtHead[cfg.root().index()].isEmpty());

    m_isRecording = true;
    for (BlockID block : cfg.reversePostOrder()) {
        BitVector wanted = wantedAtTail(block);
        m_wantedAtTail[block.index()] = wanted;
        walkBackward(block, wanted);
        ASSERT(wanted == m_wantedAtHead[block.index()]);
    }
    m_isRecording = false;
}

// The points of one block are asked for one after another, so one walk over the events of the block answers for all of them.
bool WantedProofs::isAnyProofWantedAt(ProgramPoint point)
{
    std::span<const TestEvent> events = m_events.eventsOf(point.block);
    if (m_blockOfEventFlags != point.block) {
        m_blockOfEventFlags = point.block;
        m_isProofWantedInFrontOfEvent.clearAll();
        BitVector wanted = m_wantedAtTail[point.block.index()];
        for (size_t index = events.size();; --index) {
            if (!wanted.isEmpty())
                m_isProofWantedInFrontOfEvent.set(index);
            if (!index)
                break;
            applyEventBackward(point.block, events[index - 1], wanted);
        }
    }
    size_t firstEventBehindPoint = std::ranges::lower_bound(events, point.index, { }, &TestEvent::index) - events.begin();
    return m_isProofWantedInFrontOfEvent.get(firstEventBehindPoint);
}

BitVector WantedProofs::wantedAtTail(BlockID block)
{
    BitVector wantedOnAnyEdge;
    for (uint32_t targetIndex = 0; targetIndex < m_events.function().at(block).terminator.targets.size(); ++targetIndex) {
        Edge edge { block, targetIndex };
        // An edge that has left stays out, so that the solve ends.
        if (const BitVector* missing = m_coverage.missingOn(edge)) {
            bool leaves = m_edgesIntoGenericCopy.contains(edge);
            if (!leaves && haveCommonBit(m_wantedAtHead[targetOf(m_events.function(), edge).block.index()], *missing)) {
                m_edgesIntoGenericCopy.append(edge);
                leaves = true;
            }
            if (leaves)
                continue;
        }

        BitVector wanted = wantedInFrontOfTestsOn(edge);
        m_events.forEachProofOn(edge, [&](uint32_t proof) {
            if (wanted.clear(m_events.proofs()[proof].condition.index()) && m_isRecording)
                m_wantedProofs.set(proof);
        });
        wantedOnAnyEdge.merge(wanted);
    }
    return wantedOnAnyEdge;
}

BitVector WantedProofs::wantedInFrontOfTestsOn(Edge edge)
{
    BitVector wanted = m_wantedAtHead[targetOf(m_events.function(), edge).block.index()];
    Vector<uint32_t, 4> hoistedTests;
    m_events.forEachHoistedTestOn(edge, [&](uint32_t hoistedTest) {
        hoistedTests.append(hoistedTest);
    });
    for (uint32_t hoistedTest : hoistedTests | std::views::reverse) {
        const HoistedTest& hoisted = m_events.hoistedTests()[hoistedTest];
        if (!needsTestInFront(m_coverage.ofHoistedTest(hoistedTest)) || !wanted.clear(hoisted.condition.index()))
            continue;
        if (m_isRecording)
            m_wantedHoistedTests.set(hoistedTest);
        forEachTestPrerequisite(m_events.requirements(), m_events.conditions(), hoisted.requirement, [&](const TestPrerequisite& prerequisite) {
            wanted.set(prerequisite.condition.index());
        });
    }
    return wanted;
}

void WantedProofs::walkBackward(BlockID block, BitVector& wanted)
{
    const BlockValues& values = m_events.function().at(block).values;
    uint32_t end = values.size();
    auto markValuesFrom = [&](uint32_t begin) {
        if (m_isRecording && !wanted.isEmpty()) {
            for (uint32_t index = begin; index < end; ++index)
                m_valuesInStretch.set(values[index].index());
        }
        end = std::min(end, begin);
    };
    for (const TestEvent& event : m_events.eventsOf(block) | std::views::reverse) {
        markValuesFrom(firstValueAfter(event));
        applyEventBackward(block, event, wanted);
    }
    markValuesFrom(0);
}

void WantedProofs::applyEventBackward(BlockID block, const TestEvent& event, BitVector& wanted)
{
    switch (event.kind) {
    case TestEvent::Kind::ParameterProof:
    case TestEvent::Kind::ValueProof:
        if (wanted.clear(m_events.proofs()[event.payload].condition.index()) && m_isRecording)
            m_wantedProofs.set(event.payload);
        return;
    case TestEvent::Kind::Barrier:
    case TestEvent::Kind::Suspension:
        wanted.clearAll();
        return;
    case TestEvent::Kind::Ending:
        wanted.exclude(m_events.endedBy(event));
        return;
    case TestEvent::Kind::EndOfRequirements:
        // Whoever places tests has sorted out the sites that take proofs.
        RELEASE_ASSERT_NOT_REACHED();
    case TestEvent::Kind::Requirement:
        break;
    }

    const TestRequirement& requirement = m_events.requirements()[event.payload];
    RELEASE_ASSERT(requirement.kind == RequirementKind::PlacesTest);
    if (m_isRecording)
        m_valuesInStretch.set(m_events.function().at(block).values[event.index].index());

    switch (m_coverage.of(event.payload)) {
    case RequirementCoverage::Unknown:
        return;
    case RequirementCoverage::NeedsTest:
        wanted.clear(requirement.condition.index());
        return;
    case RequirementCoverage::Covered:
        wanted.set(requirement.condition.index());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TestStretches::TestStretches(const TestEvents& events, const TestCoverage& coverage, const WantedProofs& wanted)
    : m_firstRangeOf(events.function().blocks.size())
{
    struct RangeSet : WTF::UnionFind<RangeSet> { };

    const Function& function = events.function();
    BitVector hasTestInFront(function.values.size());
    BitVector isPlannedUse(function.values.size());
    BitVector blocksWhereCopyIsEntered(function.blocks.size());
    for (uint32_t index = 0; index < events.requirements().size(); ++index) {
        if (coverage.of(index) == RequirementCoverage::Unknown)
            continue;
        ProgramPoint point = events.requirements()[index].point;
        size_t use = function.at(point.block).values[point.index].index();
        isPlannedUse.set(use);
        if (needsTestInFront(coverage.of(index)))
            hasTestInFront.set(use);
    }
    for (uint32_t index = 0; index < events.hoistedTests().size(); ++index) {
        if (wanted.isHoistedTestWanted(index))
            blocksWhereCopyIsEntered.set(targetOf(function, events.hoistedTests()[index].edge).block.index());
    }
    for (Edge edge : wanted.edgesIntoGenericCopy()) {
        BlockID target = targetOf(function, edge).block;
        if (wanted.isInStretchAtHead(target))
            blocksWhereCopyIsEntered.set(target.index());
    }

    auto startsNewCopy = [&](BlockID block, uint32_t index) {
        return hasTestInFront.get(function.at(block).values[index].index());
    };
    auto appendRange = [&](BlockID block, uint32_t begin, uint32_t end) {
        if (!m_firstRangeOf[block.index()])
            m_firstRangeOf[block.index()] = m_ranges.size();
        m_ranges.append(StretchRange { .stretch = 0, .block = block, .begin = begin, .end = end, .isCloned = false });
    };
    for (BlockID block : events.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        if (wanted.isInStretchAtHead(block) && (values.isEmpty() || startsNewCopy(block, 0)))
            appendRange(block, 0, 0);

        std::optional<uint32_t> begin;
        uint32_t extendedUntil = 0;
        for (uint32_t index = 0; index < values.size(); ++index) {
            bool isInStretch = wanted.isInStretch(values[index]) || index < extendedUntil;
            if (begin && !isInStretch) {
                extendedUntil = events.endOfResultReadsAfter({ block, index });
                isInStretch = index < extendedUntil;
            }
            if (begin && (!isInStretch || startsNewCopy(block, index) || events.startsNewStretch(values[index]))) {
                appendRange(block, *begin, index);
                begin = std::nullopt;
            }
            if (isInStretch && !begin)
                begin = index;
        }
        if (begin)
            appendRange(block, *begin, values.size());
    }

    auto forEachNextRange = [&](unsigned index, const auto& functor) {
        BlockID id = m_ranges[index].block;
        uint32_t end = m_ranges[index].end;
        if (index + 1 < m_ranges.size() && m_ranges[index + 1].block == id && m_ranges[index + 1].begin == end && !events.startsNewStretch(function.at(id).values[end]))
            functor(index + 1);
        if (end != function.at(id).values.size())
            return;
        for (const Target& target : function.at(id).terminator.targets) {
            if (wanted.isInStretchAtHead(target.block))
                functor(firstRangeOf(target.block));
        }
    };

    Vector<RangeSet> sets(m_ranges.size());
    for (unsigned index = 0; index < m_ranges.size(); ++index) {
        forEachNextRange(index, [&](unsigned next) {
            sets[next].unify(&sets[index]);
        });
    }
    Vector<std::optional<unsigned>> stretchOfRoot(m_ranges.size());
    for (unsigned index = 0; index < m_ranges.size(); ++index) {
        std::optional<unsigned>& stretch = stretchOfRoot[sets[index].find() - sets.span().data()];
        if (!stretch) {
            stretch = m_stretches.size();
            m_stretches.append(Stretch { .outcome = StretchOutcome::Guarded });
        }
        m_ranges[index].stretch = *stretch;
    }

    Vector<unsigned> worklist;
    auto markCloned = [&](unsigned index) {
        if (!std::exchange(m_ranges[index].isCloned, true))
            worklist.append(index);
    };
    for (unsigned index = 0; index < m_ranges.size(); ++index) {
        const StretchRange& range = m_ranges[index];
        bool startsAtTest = range.begin < range.end && startsNewCopy(range.block, range.begin);
        bool isEnteredAtHead = blocksWhereCopyIsEntered.get(range.block.index()) && firstRangeOf(range.block) == index;
        if (startsAtTest || isEnteredAtHead)
            markCloned(index);
    }
    while (!worklist.isEmpty())
        forEachNextRange(worklist.takeLast(), markCloned);

    for (const StretchRange& range : m_ranges) {
        const Block& block = function.at(range.block);
        Stretch& stretch = m_stretches[range.stretch];
        for (uint32_t index = range.begin; index < range.end; ++index) {
            if (isPlannedUse.get(block.values[index].index()))
                ++stretch.numberOfUses;
            else if (range.isCloned)
                stretch.clonedBytes += estimatedCodeBytes(events.analyses().program(), function, function.at(block.values[index]));
        }
        if (range.isCloned && range.end == block.values.size())
            stretch.clonedBytes += estimatedCodeBytes(block.terminator);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
