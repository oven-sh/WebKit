/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTBuildContext.h"
#include "AOTEffectsOfOperands.h"
#include "AOTForwardDataflow.h"
#include "AOTGuardFactDetails.h"
#include <algorithm>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static const Effects& effectsOn(const EffectsOnEdges& effects, EdgeKind edge)
{
    switch (edge) {
    case EdgeKind::Normal:
        return effects.normal;
    case EdgeKind::Handler:
        return effects.handler;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LocationMasks masksOf(const Program& program, std::span<const MemoryLocation> locations)
{
    LocationMasks masks;
    for (const MemoryLocation& location : locations)
        masks.add(program, location);
    return masks;
}

static uint32_t hashOf(std::span<const MemoryLocation> locations, ValueID subject)
{
    Hasher hasher;
    add(hasher, subject);
    for (const MemoryLocation& location : locations) {
        add(hasher, location.heap());
        add(hasher, location.isEntireHeap());
        if (location.isEntireHeap())
            continue;
        add(hasher, location.firstIndex());
        add(hasher, location.secondIndex());
    }
    return hasher.hash();
}

static bool isShapeEpoch(FunctionAnalyses& analyses, ValueID value)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    BitVector isVisited;
    Vector<ValueID, 4> worklist { value };
    while (!worklist.isEmpty()) {
        ValueID id = analyses.identityOf(worklist.takeLast());
        if (isVisited.set(id.index()))
            continue;
        ValueLocation location = useLists.locationOf(id);
        switch (location.placement) {
        case ValuePlacement::Detached:
            return false;
        case ValuePlacement::InBlock:
            if (function.at(id).opcode != Opcode::GetShapeEpoch)
                return false;
            break;
        case ValuePlacement::Parameter:
            if (location.block == entryBlock)
                return false;
            for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
                IncomingValue incoming = incomingValueOf(function, edge, location.index);
                if (!incoming.argument)
                    return false;
                worklist.append(incoming.argument);
            }
            break;
        }
    }
    return true;
}

static ValueID valueBeforeEdge(FunctionAnalyses& analyses, ValueID value, Edge edge)
{
    const Function& function = analyses.function();
    ValueLocation location = analyses.useLists().locationOf(value);
    if (location.placement != ValuePlacement::Parameter || location.block != targetOf(function, edge).block)
        return value;
    return incomingValueOf(function, edge, location.index).argument;
}

static ValueID epochBeforeEdge(FunctionAnalyses& analyses, ValueID epoch, Edge edge)
{
    ValueID before = valueBeforeEdge(analyses, epoch, edge);
    return before ? analyses.identityOf(before) : ValueID();
}

static bool isDefinedBeforeEdge(FunctionAnalyses& analyses, ValueID value, Edge edge)
{
    ProgramPoint point = edge.isHandlerEdge() ? ProgramPoint::atHeadOf(edge.from) : pointBeforeTerminatorOf(analyses.function(), edge.from);
    return analyses.dominates(value, point);
}

namespace {

struct ProofDependencies {
    std::span<const MemoryLocation> locations;
    const LocationMasks& masks;
    ValueID subject;
};

// The writer is the value at the point, or the terminator of the block where the point stands behind its last value.
class KillQuery {
    WTF_MAKE_NONCOPYABLE(KillQuery);
public:
    KillQuery(FunctionAnalyses& analyses, ProgramPoint point, const EffectsOnEdges& effects)
        : m_analyses(analyses)
        , m_function(analyses.function())
        , m_point(point)
        , m_effects(effects)
    {
        const BlockValues& values = m_function.at(point.block).values;
        if (point.index < values.size())
            m_writer = values[point.index];
    }

    bool mayKill(const ProofDependencies&, EdgeKind);

private:
    struct WrittenOperand {
        ValueID operand;
        LocationMasks writes;
    };

    const Terminator& terminator() const LIFETIME_BOUND { return m_function.at(m_point.block).terminator; }
    bool mayWrite(const Effects&, const MemoryLocation&) const;
    bool mayKillThroughOperands(ValueID subject, const LocationMasks& written, EdgeKind);
    bool isConfined(ValueID subject);
    bool isConfined(ObjectID);
    bool escapesAtWriter(ObjectID);
    bool isContainedInOperand(ObjectID);

    const EffectsOnEdges& beyondOperands();
    std::span<const WrittenOperand> writtenOperands();
    const BitVector& operandObjects();
    const BitVector& objectsInsideOperands();

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    ProgramPoint m_point;
    ValueID m_writer;
    const EffectsOnEdges& m_effects;
    std::optional<EffectsOnEdges> m_beyondOperands;
    std::optional<Vector<WrittenOperand, 4>> m_writtenOperands;
    std::optional<BitVector> m_operandObjects;
    std::optional<BitVector> m_objectsInsideOperands;
};

bool KillQuery::mayKill(const ProofDependencies& dependencies, EdgeKind edge)
{
    const Effects& effects = effectsOn(m_effects, edge);
    if (effects.maySuspend)
        return true;
    if (!effects.writes.overlaps(dependencies.masks))
        return false;

    LocationMasks written;
    for (const MemoryLocation& location : dependencies.locations) {
        if (!mayWrite(effects, location))
            continue;
        if (!dependencies.subject || !perObjectHeaps.contains(location.heap()))
            return true;
        written.add(m_analyses.program(), location);
    }
    return !written.isEmpty() && mayKillThroughOperands(dependencies.subject, written, edge);
}

bool KillQuery::mayWrite(const Effects& effects, const MemoryLocation& location) const
{
    const Program& program = m_analyses.program();
    if (!m_writer)
        return effects.writes.overlaps(program, location);
    return overlapOfWrites(program, m_function, m_function.at(m_writer), effects, location) != LocationOverlap::None;
}

bool KillQuery::mayKillThroughOperands(ValueID subject, const LocationMasks& written, EdgeKind edge)
{
    auto mayWriteThrough = [&](const WrittenOperand& operand) {
        return operand.writes.overlaps(written);
    };
    bool mayWriteBeyondOperands = effectsOn(beyondOperands(), edge).writes.overlaps(written);
    if (!mayWriteBeyondOperands && std::ranges::none_of(writtenOperands(), mayWriteThrough))
        return false;

    const EscapeAnalysis& escape = m_analyses.escape();
    if (!isConfined(subject)) {
        return mayWriteBeyondOperands || std::ranges::any_of(writtenOperands(), [&](const WrittenOperand& operand) {
            return mayWriteThrough(operand) && escape.alias(operand.operand, subject) != AliasResult::NoAlias;
        });
    }

    ObjectSet objects = escape.pointsTo(subject).objects;
    return std::ranges::any_of(writtenOperands(), [&](const WrittenOperand& operand) {
        return mayWriteThrough(operand) && escape.pointsTo(operand.operand).objects.overlaps(objects);
    });
}

bool KillQuery::isConfined(ValueID subject)
{
    PointsToSet set = m_analyses.escape().pointsTo(subject);
    if (set.mayBeUntracked)
        return false;
    for (ObjectID object : set.objects) {
        if (!isConfined(object))
            return false;
    }
    return true;
}

bool KillQuery::isConfined(ObjectID object)
{
    if (m_analyses.escapeHistory().occurredBefore(object, m_point) != Occurrence::HasNotOccurred)
        return false;
    return !escapesAtWriter(object) && !isContainedInOperand(object);
}

bool KillQuery::escapesAtWriter(ObjectID object)
{
    if (!operandObjects().get(object.index()))
        return false;
    auto isUseByWriter = [&](const Use& use) {
        if (m_writer)
            return use.kind == Use::Kind::Operand && use.user == m_writer;
        return use.kind == Use::Kind::TerminatorOperand && use.block == m_point.block;
    };
    return std::ranges::any_of(m_analyses.escape().usesOf(object), [&](const ObjectUse& objectUse) {
        return objectUse.isEscape && isUseByWriter(objectUse.use);
    });
}

bool KillQuery::isContainedInOperand(ObjectID object)
{
    return objectsInsideOperands().get(object.index());
}

const EffectsOnEdges& KillQuery::beyondOperands()
{
    if (!m_beyondOperands)
        m_beyondOperands = m_writer ? effectsBeyondOperands(m_analyses.effectContext(), m_function, m_function.at(m_writer), m_effects) : m_effects;
    return *m_beyondOperands;
}

auto KillQuery::writtenOperands() -> std::span<const WrittenOperand>
{
    if (m_writtenOperands)
        return m_writtenOperands->span();

    m_writtenOperands.emplace();
    if (!m_writer) {
        LocationMasks writes = m_effects.onAnyEdge().writes.restrictedTo(perObjectHeaps);
        terminator().forEachOperand([&](ValueID operand, uint32_t) {
            if (!writes.isEmpty() && mayBeReference(m_analyses.program().typeOf(m_function, operand)))
                m_writtenOperands->append(WrittenOperand { operand, writes });
        });
        return m_writtenOperands->span();
    }

    const Value& writer = m_function.at(m_writer);
    writer.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        OperandMemory memory = memoryAccessedThrough(m_analyses.effectContext(), m_function, writer, operandIndex, m_effects);
        if (!memory.writes.isEmpty())
            m_writtenOperands->append(WrittenOperand { operand, memory.writes });
    });
    return m_writtenOperands->span();
}

const BitVector& KillQuery::operandObjects()
{
    if (!m_operandObjects) {
        m_operandObjects.emplace();
        const EscapeAnalysis& escape = m_analyses.escape();
        auto addObjectsOf = [&](ValueID operand) {
            m_operandObjects->merge(escape.pointsTo(operand).objects.bits());
        };
        if (m_writer)
            m_function.at(m_writer).forEachChild(addObjectsOf);
        else {
            terminator().forEachOperand([&](ValueID operand, uint32_t) {
                addObjectsOf(operand);
            });
        }
    }
    return *m_operandObjects;
}

const BitVector& KillQuery::objectsInsideOperands()
{
    if (!m_objectsInsideOperands) {
        m_objectsInsideOperands.emplace();
        const EscapeAnalysis& escape = m_analyses.escape();
        Vector<ObjectID, 8> worklist;
        for (size_t object : operandObjects())
            worklist.append(ObjectID(object));
        while (!worklist.isEmpty()) {
            for (ObjectID content : escape.contentsOf(worklist.takeLast())) {
                if (!m_objectsInsideOperands->set(content.index()))
                    worklist.append(content);
            }
        }
    }
    return *m_objectsInsideOperands;
}

} // anonymous namespace

std::optional<GuardDependencies> dependenciesOfGuardOf(FunctionAnalyses& analyses, const Value& test)
{
    if (!provesForStretch(provesOf(test.opcode)))
        return std::nullopt;

    const Program& program = analyses.program();
    GuardDependencies dependencies;
    dependencies.locations = locationsReadBy(test);
    dependencies.masks = masksOf(program, dependencies.locations.span());
    if (test.numberOfOperands() && test.operand(subjectOperandIndex) && testReadsOnlySubject(program, analyses.function(), test))
        dependencies.subject = analyses.identityOf(test.operand(subjectOperandIndex));
    return dependencies;
}

struct GuardWriter::Query {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Query);

    Query(FunctionAnalyses& analyses, ProgramPoint point, const EffectsOnEdges& effectsOfWriter)
        : effects(effectsOfWriter)
        , query(analyses, point, effects)
    {
    }

    const EffectsOnEdges effects;
    KillQuery query;
};

GuardWriter::GuardWriter(FunctionAnalyses& analyses, ValueID writer)
{
    ValueLocation location = analyses.useLists().locationOf(writer);
    if (location.placement == ValuePlacement::InBlock)
        m_query = makeUnique<Query>(analyses, ProgramPoint { location.block, location.index }, analyses.effectsOf(writer));
}

GuardWriter::~GuardWriter() = default;

bool GuardWriter::mayEnd(const GuardDependencies& dependencies)
{
    if (!m_query)
        return true;
    return m_query->query.mayKill({ dependencies.locations.span(), dependencies.masks, dependencies.subject }, EdgeKind::Normal);
}

GuardAvailability::GuardAvailability(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_function(analyses.function())
{
    collectGuards();
    size_t numberOfNamedGuards = m_guards.size();
    collectUnnamedAllocations();
    if (trySolve())
        return;

    analyses.context().build.gaveUp(LimitID::MaxGuardAvailabilityWork, analyses.subject());
    if (m_guards.size() > numberOfNamedGuards) {
        removeGuardsFrom(numberOfNamedGuards);
        if (trySolve())
            return;
    }
    m_gaveUp = true;
}

bool GuardAvailability::trySolve()
{
    m_facts.clear();
    m_components.clear();
    m_firstComponent.clear();
    m_epochSnapshots.clear();
    m_epochSnapshotsByGuard.clear();
    m_killClasses.clear();

    computeFacts();
    numberComponents();
    numberEpochSnapshots();
    if (m_components.isEmpty())
        return true;

    uint64_t numberOfBlocks = m_analyses.cfg().reversePostOrder().size();
    if (numberOfBlocks * m_numberOfBits > m_analyses.context().build.limit(LimitID::MaxGuardAvailabilityWork))
        return false;
    if (!tryCollectKills())
        return false;
    solve();
    return true;
}

void GuardAvailability::numberComponents()
{
    HashedIndex killClassIndex;
    m_firstComponent.reserveInitialCapacity(m_guards.size() + 1);
    for (uint32_t guardIndex = 0; guardIndex < m_guards.size(); ++guardIndex) {
        m_firstComponent.append(m_components.size());
        const GuardFacts& facts = m_facts[guardIndex];
        if (!facts.isKnown())
            continue;
        for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
            uint32_t killClass = internKillClass(facts.facts[factIndex], killClassIndex);
            m_killClasses[killClass].components.set(m_components.size());
            m_components.append(Component { m_guards[guardIndex], factIndex, killClass });
        }
    }
    m_firstComponent.append(m_components.size());
}

uint32_t GuardAvailability::internKillClass(const GuardFact& fact, HashedIndex& index)
{
    ValueID subject = exclusiveSubjectOf(m_analyses, fact);
    uint32_t hash = hashOf(fact.dependsOn.span(), subject);
    std::optional<uint32_t> existing = index.find(hash, [&](uint32_t position) {
        const KillClass& killClass = m_killClasses[position];
        return killClass.subject == subject && killClass.locations == fact.dependsOn;
    });
    if (existing)
        return *existing;

    uint32_t position = m_killClasses.size();
    m_killClasses.append(KillClass { fact.dependsOn, masksOf(m_analyses.program(), fact.dependsOn.span()), subject, { } });
    index.add(hash, position);
    return position;
}

void GuardAvailability::numberEpochSnapshots()
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    m_numberOfBits = m_components.size();

    HashedIndex snapshotIndex;
    auto addSnapshot = [&](ValueID epoch, ValueID guard) {
        uint32_t numberOfFacts = componentsOf(guard).size();
        if (!epoch || !numberOfFacts)
            return;
        uint32_t hash = computeHash(epoch, guard);
        std::optional<uint32_t> existing = snapshotIndex.find(hash, [&](uint32_t position) {
            return m_epochSnapshots[position].epochRead == epoch && m_epochSnapshots[position].renewedGuard == guard;
        });
        if (existing)
            return;
        snapshotIndex.add(hash, m_epochSnapshots.size());
        m_epochSnapshots.append(EpochSnapshot { .epochRead = epoch, .renewedGuard = guard });
        m_numberOfBits += numberOfFacts;
    };

    for (ValueID guard : m_guards) {
        ValueLocation location = useLists.locationOf(guard);
        if (location.placement != ValuePlacement::Parameter || componentsOf(guard).empty())
            continue;
        for (Edge edge : cfg.incomingEdges(location.block)) {
            IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
            if (incoming.kind != IncomingValue::Kind::EdgeResult)
                continue;
            std::optional<EdgeResult> result = tryGetEdgeResult(m_function, incoming.result);
            if (!result || result->kind != EdgeResultKind::Guard || result->proof != Proof::EpochCurrent)
                continue;
            const Value& test = m_function.at(result->test);
            if (test.epochOperand() && isShapeEpoch(m_analyses, test.epochOperand()))
                addSnapshot(m_analyses.identityOf(test.epochOperand()), test.guardOperand());
        }
    }

    uint64_t maxNumberOfBits = m_analyses.context().build.limit(LimitID::MaxGuardAvailabilityWork) / cfg.reversePostOrder().size();
    for (size_t next = 0; next < m_epochSnapshots.size() && m_numberOfBits <= maxNumberOfBits; ++next) {
        ValueID epoch = m_epochSnapshots[next].epochRead;
        ValueID guard = m_epochSnapshots[next].renewedGuard;
        ValueLocation guardLocation = useLists.locationOf(guard);
        bool unitesGuards = guardLocation.placement == ValuePlacement::InBlock && VariadicOperandPosition::guards(m_function.at(guard).opcode);
        if (unitesGuards && m_analyses.dominates(epoch, ProgramPoint { guardLocation.block, guardLocation.index })) {
            for (ValueID operand : m_function.at(guard).guardsOperands())
                addSnapshot(epoch, operand);
        }
        for (ValueID value : { epoch, guard }) {
            ValueLocation location = useLists.locationOf(value);
            if (location.placement != ValuePlacement::Parameter)
                continue;
            for (Edge edge : cfg.incomingEdges(location.block)) {
                ValueID epochBefore = epochBeforeEdge(m_analyses, epoch, edge);
                ValueID guardBefore = valueBeforeEdge(m_analyses, guard, edge);
                if (!epochBefore || !guardBefore)
                    continue;
                if (isDefinedBeforeEdge(m_analyses, epochBefore, edge) && isDefinedBeforeEdge(m_analyses, guardBefore, edge))
                    addSnapshot(epochBefore, guardBefore);
            }
        }
    }

    std::ranges::sort(m_epochSnapshots, { }, [](const EpochSnapshot& snapshot) {
        return std::pair { snapshot.epochRead, snapshot.renewedGuard };
    });
    uint32_t nextBit = m_components.size();
    for (EpochSnapshot& snapshot : m_epochSnapshots) {
        snapshot.firstBit = nextBit;
        nextBit += componentsOf(snapshot.renewedGuard).size();
    }
    m_epochSnapshotsByGuard = m_epochSnapshots;
    std::ranges::sort(m_epochSnapshotsByGuard, { }, [](const EpochSnapshot& snapshot) {
        return std::pair { snapshot.renewedGuard, snapshot.epochRead };
    });
}

bool GuardAvailability::tryCollectKills()
{
    const CFG& cfg = m_analyses.cfg();

    LocationMasks dependedOn;
    for (const KillClass& killClass : m_killClasses)
        dependedOn.merge(killClass.masks);
    auto mayKillAnyClass = [&](const Effects& effects) {
        return effects.maySuspend || effects.writes.overlaps(dependedOn);
    };

    struct Writer {
        uint32_t blockOrder;
        uint32_t index;
        bool mayKillOnNormalEdge;
        bool mayKillOnThrow;
    };
    Vector<Writer> writers;
    std::span<const BlockID> order = cfg.reversePostOrder();
    for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
        BlockID block = order[blockOrder];
        auto addWriter = [&](uint32_t index, const EffectsOnEdges& effects) {
            bool mayKillOnNormalEdge = mayKillAnyClass(effects.normal);
            bool mayKillOnThrow = cfg.hasHandler(block) && mayKillAnyClass(effects.handler);
            if (mayKillOnNormalEdge || mayKillOnThrow)
                writers.append(Writer { blockOrder, index, mayKillOnNormalEdge, mayKillOnThrow });
        };
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index)
            addWriter(index, m_analyses.effectsOf(values[index]));
        addWriter(values.size(), m_analyses.effectsOfTerminator(block));
    }

    // Every writer is asked about every class, so the work is known before any of it is done.
    if (static_cast<uint64_t>(writers.size()) * m_killClasses.size() > m_analyses.context().build.limit(LimitID::MaxGuardAvailabilityWork))
        return false;

    m_killedOnThrow.grow(m_function.blocks.size());
    m_firstKill.reserveInitialCapacity(order.size() + 1);
    size_t nextWriter = 0;
    for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
        BlockID block = order[blockOrder];
        const BlockValues& values = m_function.at(block).values;
        m_firstKill.append(m_kills.size());
        for (; nextWriter < writers.size() && writers[nextWriter].blockOrder == blockOrder; ++nextWriter) {
            const Writer& writer = writers[nextWriter];
            const EffectsOnEdges& effects = writer.index < values.size() ? m_analyses.effectsOf(values[writer.index]) : m_analyses.effectsOfTerminator(block);
            KillQuery query(m_analyses, { block, writer.index }, effects);
            for (uint32_t classIndex = 0; classIndex < m_killClasses.size(); ++classIndex) {
                const KillClass& killClass = m_killClasses[classIndex];
                ProofDependencies dependencies { killClass.locations.span(), killClass.masks, killClass.subject };
                if (writer.mayKillOnNormalEdge && query.mayKill(dependencies, EdgeKind::Normal))
                    m_kills.append(Kill { writer.index, classIndex });
                if (writer.mayKillOnThrow && query.mayKill(dependencies, EdgeKind::Handler))
                    m_killedOnThrow[block.index()].merge(killClass.components);
            }
        }
    }
    m_firstKill.append(m_kills.size());
    return true;
}

void GuardAvailability::solve()
{
    struct Problem {
        using State = BitVector;

        State stateAtEntry() const { return BitVector(availability.m_numberOfBits); }

        void execute(State& state, const DataflowStep& step) const
        {
            availability.applyKills(state, step.block, step.index);
            availability.applyDefinitions(state, step.block, step.index);
        }

        void executeTerminator(State& state, const DataflowTerminatorStep& step) const
        {
            availability.applyKills(state, step.block, availability.m_function.at(step.block).values.size());
        }

        void enterEdge(State& state, Edge edge) const { availability.enterEdge(state, edge); }

        bool merge(State& into, const State& from) const
        {
            size_t previousCount = into.bitCount();
            into.filter(from);
            return into.bitCount() != previousCount;
        }

        const GuardAvailability& availability;
    };

    Problem problem { *this };
    Vector<std::optional<BitVector>> heads = m_analyses.solveForward(problem);
    m_availableAtHead.grow(heads.size());
    m_isSolved.ensureSize(heads.size());
    for (size_t blockIndex = 0; blockIndex < heads.size(); ++blockIndex) {
        if (!heads[blockIndex])
            continue;
        m_availableAtHead[blockIndex] = WTF::move(*heads[blockIndex]);
        m_isSolved.quickSet(blockIndex);
    }
}

void GuardAvailability::applyKills(BitVector& available, BlockID block, uint32_t index) const
{
    for (const Kill& kill : std::ranges::equal_range(killsIn(block), index, { }, &Kill::index))
        available.exclude(m_killClasses[kill.killClass].components);
}

void GuardAvailability::applyDefinitions(BitVector& available, BlockID block, uint32_t index) const
{
    ValueID id = m_function.at(block).values[index];
    for (const EpochSnapshot& snapshot : std::ranges::equal_range(m_epochSnapshots, id, { }, &EpochSnapshot::epochRead)) {
        uint32_t firstComponent = firstComponentOf(snapshot.renewedGuard);
        for (const Component& component : componentsOf(snapshot.renewedGuard))
            available.quickSet(snapshot.firstBit + component.factIndex, available.quickGet(firstComponent + component.factIndex));
    }

    std::span<const Component> components = componentsOf(id);
    if (components.empty())
        return;

    const Value& value = m_function.at(id);
    const GuardFacts& facts = factsOf(id);
    uint32_t firstComponent = firstComponentOf(id);
    bool unitesGuards = VariadicOperandPosition::guards(value.opcode).has_value();
    auto define = [&](ValueID epoch, uint32_t firstBit) {
        for (const Component& component : components) {
            bool isAvailable = !unitesGuards || std::ranges::any_of(value.guardsOperands(), [&](ValueID operand) {
                return isAvailableIn(available, epoch, operand, facts.facts[component.factIndex]);
            });
            available.quickSet(firstBit + component.factIndex, isAvailable || (epoch && available.quickGet(firstComponent + component.factIndex)));
        }
    };
    define(ValueID(), firstComponent);
    for (const EpochSnapshot& snapshot : std::ranges::equal_range(m_epochSnapshotsByGuard, id, { }, &EpochSnapshot::renewedGuard))
        define(snapshot.epochRead, snapshot.firstBit);
}

void GuardAvailability::enterEdge(BitVector& available, Edge edge) const
{
    if (edge.isHandlerEdge())
        available.exclude(m_killedOnThrow[edge.from.index()]);

    const UseLists& useLists = m_analyses.useLists();
    BlockID targetBlock = targetOf(m_function, edge).block;
    auto isBoundOnEdge = [&](ValueLocation location) {
        return location.placement == ValuePlacement::Parameter && location.block == targetBlock;
    };

    Vector<std::pair<uint32_t, bool>, 8> assignments;
    auto bind = [&](ValueID epoch, ValueID guard, uint32_t firstBit) {
        ValueID epochBefore = epoch ? epochBeforeEdge(m_analyses, epoch, edge) : ValueID();
        ValueID guardBefore = guard;
        std::optional<EdgeResult> result;
        if (ValueLocation location = useLists.locationOf(guard); isBoundOnEdge(location)) {
            IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
            guardBefore = incoming.argument;
            if (incoming.kind == IncomingValue::Kind::EdgeResult)
                result = tryGetEdgeResult(m_function, incoming.result);
        }

        const GuardFacts& facts = factsOf(guard);
        for (const Component& component : componentsOf(guard)) {
            const GuardFact& fact = facts.facts[component.factIndex];
            bool isAvailable = leavesUnspecified(edge, fact.subject);
            if (!isAvailable)
                isAvailable = result ? isProvenOnEdge(available, edge, *result, component) : isAvailableBeforeEdge(available, edge, ValueID(), guardBefore, fact);
            if (!isAvailable && epochBefore)
                isAvailable = isAvailableBeforeEdge(available, edge, epochBefore, guardBefore, fact);
            assignments.append({ firstBit + component.factIndex, isAvailable });
        }
    };

    for (ValueID parameter : m_function.at(targetBlock).parameters) {
        if (!componentsOf(parameter).empty()) {
            bind(ValueID(), parameter, firstComponentOf(parameter));
            for (const EpochSnapshot& snapshot : std::ranges::equal_range(m_epochSnapshotsByGuard, parameter, { }, &EpochSnapshot::renewedGuard))
                bind(snapshot.epochRead, parameter, snapshot.firstBit);
            continue;
        }
        for (const EpochSnapshot& snapshot : std::ranges::equal_range(m_epochSnapshots, parameter, { }, &EpochSnapshot::epochRead)) {
            if (!isBoundOnEdge(useLists.locationOf(snapshot.renewedGuard)))
                bind(parameter, snapshot.renewedGuard, snapshot.firstBit);
        }
    }

    for (auto [bit, isAvailable] : assignments)
        available.quickSet(bit, isAvailable);
}

bool GuardAvailability::isProvenOnEdge(const BitVector& available, Edge edge, const EdgeResult& result, const Component& component) const
{
    if (result.kind != EdgeResultKind::Guard)
        return false;
    if (result.caseIndex)
        return true;

    if (mayEndBetweenTestAndBranch(result.test, edge.from, component.killClass))
        return false;
    if (result.proof != Proof::EpochCurrent)
        return true;

    const Value& test = m_function.at(result.test);
    const GuardFact& fact = factsOf(component.guard).facts[component.factIndex];
    if (isAvailableBeforeEdge(available, edge, ValueID(), test.guardOperand(), fact))
        return true;
    if (!test.epochOperand() || !fact.isRenewedByShapeEpoch())
        return false;
    return isAvailableBeforeEdge(available, edge, m_analyses.identityOf(test.epochOperand()), test.guardOperand(), fact);
}

bool GuardAvailability::mayEndBetweenTestAndBranch(ValueID test, BlockID branch, uint32_t killClass) const
{
    ValueLocation location = m_analyses.useLists().locationOf(test);
    if (location.placement != ValuePlacement::InBlock)
        return true;

    auto endsProof = [&](const Kill& kill) {
        return kill.killClass == killClass;
    };
    const CFG& cfg = m_analyses.cfg();
    for (BlockID block = branch; block != location.block;) {
        std::optional<Edge> onlyWayIn = cfg.singleIncomingEdge(block);
        if (!onlyWayIn || onlyWayIn->isHandlerEdge() || std::ranges::any_of(killsIn(block), endsProof))
            return true;
        block = onlyWayIn->from;
    }

    std::span<const Kill> kills = killsIn(location.block);
    std::ranges::subrange killsAfterTest(std::ranges::upper_bound(kills, location.index, { }, &Kill::index), kills.end());
    return std::ranges::any_of(killsAfterTest, endsProof);
}

bool GuardAvailability::isAvailableBeforeEdge(const BitVector& available, Edge edge, ValueID epoch, ValueID guard, const GuardFact& factAfterEdge) const
{
    std::optional<ValueID> subject = subjectBeforeEdge(factAfterEdge.subject, edge);
    return subject && isAvailableIn(available, epoch, guard, factAfterEdge, *subject);
}

bool GuardAvailability::isAvailableIn(const BitVector& available, ValueID epoch, ValueID guard, const GuardFact& fact) const
{
    return isAvailableIn(available, epoch, guard, fact, fact.subject);
}

bool GuardAvailability::isAvailableIn(const BitVector& available, ValueID epoch, ValueID guard, const GuardFact& fact, ValueID subject) const
{
    std::optional<uint32_t> firstBit = firstBitOf(epoch, guard);
    if (!firstBit)
        return false;
    const GuardFacts& facts = factsOf(guard);
    for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
        const GuardFact& candidate = facts.facts[factIndex];
        if (available.quickGet(*firstBit + factIndex) && candidate.subject == subject && isAsStrongAs(candidate, fact))
            return true;
    }
    return false;
}

std::optional<uint32_t> GuardAvailability::firstBitOf(ValueID epoch, ValueID guard) const
{
    if (componentsOf(guard).empty())
        return std::nullopt;
    if (!epoch)
        return firstComponentOf(guard);

    auto keyOf = [](const EpochSnapshot& snapshot) {
        return std::pair { snapshot.epochRead, snapshot.renewedGuard };
    };
    std::pair key { epoch, guard };
    auto snapshot = std::ranges::lower_bound(m_epochSnapshots, key, { }, keyOf);
    if (snapshot == m_epochSnapshots.end() || keyOf(*snapshot) != key)
        return std::nullopt;
    return snapshot->firstBit;
}

auto GuardAvailability::killsIn(BlockID block) const -> std::span<const Kill>
{
    uint32_t blockOrder = m_analyses.cfg().reversePostOrderIndex(block);
    uint32_t first = m_firstKill[blockOrder];
    return m_kills.subspan(first, m_firstKill[blockOrder + 1] - first);
}

auto GuardAvailability::componentsOf(ValueID guard) const -> std::span<const Component>
{
    if (guard.index() >= m_guardIndexOfValue.size())
        return { };
    std::optional<uint32_t> guardIndex = m_guardIndexOfValue[guard.index()];
    if (!guardIndex)
        return { };
    uint32_t first = m_firstComponent[*guardIndex];
    return m_components.subspan(first, m_firstComponent[*guardIndex + 1] - first);
}

uint32_t GuardAvailability::firstComponentOf(ValueID guard) const
{
    RELEASE_ASSERT(guard.index() < m_guardIndexOfValue.size());
    std::optional<uint32_t> guardIndex = m_guardIndexOfValue[guard.index()];
    RELEASE_ASSERT(guardIndex);
    return m_firstComponent[*guardIndex];
}

GuardState GuardAvailability::stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn) const
{
    return stateIn(available, guard, reliedOn, ValueID());
}

GuardState GuardAvailability::stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject) const
{
    if (m_gaveUp)
        return GuardState::Unknown;
    const GuardFacts& facts = factsOf(guard);
    if (!facts.isKnown() || facts.facts.isEmpty())
        return GuardState::Unknown;

    ValueID identity = subject ? m_analyses.identityOf(subject) : ValueID();
    uint32_t firstComponent = firstComponentOf(guard);
    auto hasAvailableFact = [&](Proof proof, ValueID provenSubject) {
        for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
            const GuardFact& fact = facts.facts[factIndex];
            if (fact.proves(proof) && fact.subject == provenSubject && available.get(firstComponent + factIndex))
                return true;
        }
        return false;
    };

    bool foundAny = false;
    bool foundUnavailable = false;
    for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
        const GuardFact& fact = facts.facts[factIndex];
        if (reliedOn && !reliedOn->intersects(fact.proofs))
            continue;
        if (identity && fact.subject != identity)
            continue;
        foundAny = true;
        if (available.get(firstComponent + factIndex))
            continue;

        // Another fact may prove the same about this subject with another payload, and only the user knows which of the two it needs.
        foundUnavailable = true;
        for (Proof proof : allMembersOf<Proof>) {
            bool isAskedFor = fact.proves(proof) && (!reliedOn || reliedOn->contains(proof));
            if (isAskedFor && !hasAvailableFact(proof, fact.subject))
                return GuardState::Unavailable;
        }
    }
    return foundAny && !foundUnavailable ? GuardState::Available : GuardState::Unknown;
}

GuardState GuardAvailability::stateIn(const BitVector& available, ValueID guard, const Value& user) const
{
    if (m_gaveUp)
        return GuardState::Unknown;
    const GuardFacts& facts = factsOf(guard);
    if (!facts.isKnown())
        return GuardState::Unknown;

    const Program& program = m_analyses.program();
    ProofSet needed = proofsNeededFromGuard(program, m_function, user);
    ValueID subject = guardedOperandOf(user);
    ValueID identity = subject ? m_analyses.identityOf(subject) : ValueID();
    uint32_t firstComponent = firstComponentOf(guard);

    GuardState state = GuardState::Available;
    for (Proof proof : allMembersOf<Proof>) {
        if (!needed.contains(proof))
            continue;
        bool isProven = false;
        bool isAvailable = false;
        for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
            const GuardFact& fact = facts.facts[factIndex];
            if ((identity && fact.subject != identity) || !provesForUser(m_analyses, fact, proof, user))
                continue;
            isProven = true;
            isAvailable |= available.get(firstComponent + factIndex);
        }
        if (isProven && !isAvailable)
            return GuardState::Unavailable;
        if (!isProven)
            state = GuardState::Unknown;
    }
    return state;
}

bool GuardAvailability::mayBeEndedAt(ValueID guard, const Value& user, ProgramPoint writer) const
{
    const GuardFacts& facts = factsOf(guard);
    if (m_gaveUp || !facts.isKnown() || !m_analyses.cfg().isReachable(writer.block))
        return true;

    const Program& program = m_analyses.program();
    ProofSet needed = proofsNeededFromGuard(program, m_function, user);
    ValueID subject = guardedOperandOf(user);
    ValueID identity = subject ? m_analyses.identityOf(subject) : ValueID();
    auto isNeeded = [&](const GuardFact& fact) {
        if (identity && fact.subject != identity)
            return false;
        return std::ranges::any_of(allMembersOf<Proof>, [&](Proof proof) {
            return needed.contains(proof) && provesForUser(m_analyses, fact, proof, user);
        });
    };

    for (const Component& component : componentsOf(guard)) {
        if (!isNeeded(facts.facts[component.factIndex]))
            continue;
        for (const Kill& kill : std::ranges::equal_range(killsIn(writer.block), writer.index, { }, &Kill::index)) {
            if (kill.killClass == component.killClass)
                return true;
        }
    }
    return false;
}

GuardState GuardAvailability::stateAt(ValueID guard, ProgramPoint point) const
{
    Cursor cursor(*this, point.block);
    cursor.advanceTo(point.index);
    return cursor.stateOf(guard);
}

GuardState GuardAvailability::stateAt(ValueID guard, ProofSet reliedOn, ProgramPoint point) const
{
    Cursor cursor(*this, point.block);
    cursor.advanceTo(point.index);
    return cursor.stateOf(guard, reliedOn);
}

GuardState GuardAvailability::stateAt(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint point) const
{
    Cursor cursor(*this, point.block);
    cursor.advanceTo(point.index);
    return cursor.stateOf(guard, reliedOn, subject);
}

GuardState GuardAvailability::stateAt(ValueID guard, const Value& user, ProgramPoint point) const
{
    Cursor cursor(*this, point.block);
    cursor.advanceTo(point.index);
    return cursor.stateOf(guard, user);
}

GuardState GuardAvailability::stateOnEdge(ValueID guard, Edge edge) const
{
    BlockID block = edge.from;
    if (!m_isSolved.get(block.index()))
        return GuardState::Unknown;

    const BlockValues& values = m_function.at(block).values;
    BitVector available = m_availableAtHead[block.index()];
    BitVector availableOnThrow = available;
    for (uint32_t index = 0; index <= values.size(); ++index) {
        bool isTerminator = index == values.size();
        bool mayThrow = edge.isHandlerEdge() && (isTerminator ? m_analyses.effectsOfTerminator(block) : m_analyses.effectsOf(values[index])).mayThrow();
        if (mayThrow)
            availableOnThrow.filter(available);
        applyKills(available, block, index);
        if (!isTerminator)
            applyDefinitions(available, block, index);
        if (mayThrow)
            availableOnThrow.filter(available);
    }

    switch (edge.kind) {
    case EdgeKind::Normal:
        return stateIn(available, guard, nullptr);
    case EdgeKind::Handler:
        availableOnThrow.exclude(m_killedOnThrow[block.index()]);
        return stateIn(availableOnThrow, guard, nullptr);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

GuardAvailability::Cursor::Cursor(const GuardAvailability& availability, BlockID block)
    : m_availability(availability)
    , m_block(block)
{
    if (availability.m_isSolved.get(block.index()))
        m_available = availability.m_availableAtHead[block.index()];
}

void GuardAvailability::Cursor::advanceTo(uint32_t index)
{
    RELEASE_ASSERT(index >= m_index && index <= m_availability.m_function.at(m_block).values.size());
    if (!m_availability.m_isSolved.get(m_block.index())) {
        m_index = index;
        return;
    }
    for (; m_index < index; ++m_index) {
        m_availability.applyKills(m_available, m_block, m_index);
        m_availability.applyDefinitions(m_available, m_block, m_index);
    }
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard) const
{
    if (!m_availability.m_isSolved.get(m_block.index()))
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, nullptr);
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, ProofSet reliedOn) const
{
    return stateOf(guard, reliedOn, ValueID());
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, ProofSet reliedOn, ValueID subject) const
{
    if (!m_availability.m_isSolved.get(m_block.index()))
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, &reliedOn, subject);
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, const Value& user) const
{
    if (!m_availability.m_isSolved.get(m_block.index()))
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, user);
}

void GuardAvailability::dump(PrintStream& out) const
{
    const Program& program = m_analyses.program();
    for (ValueID guard : m_guards) {
        dumpID(out, guard);
        out.print(" facts: ");
        dumpGuardFacts(out, program, factsOf(guard));
        out.println();
    }

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        if (!m_isSolved.get(block.index()))
            continue;
        dumpID(out, block);
        out.print(" available at head: [");
        CommaPrinter separator(" "_s);
        for (size_t bit : m_availableAtHead[block.index()]) {
            if (bit >= m_components.size())
                break;
            const Component& component = m_components[bit];
            out.print(separator);
            dumpID(out, component.guard);
            out.print(".", component.factIndex);
        }
        out.println("]");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
