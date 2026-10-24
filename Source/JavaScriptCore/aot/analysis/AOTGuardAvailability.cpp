/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGuardAvailability.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTEffectsOfOperands.h"
#include "AOTForwardDataflow.h"
#include "AOTGuardFactDetails.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
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

static bool isNeverWriter(Opcode opcode)
{
    return opcode == Opcode::GetDeferredClosureSlot || opcode == Opcode::GetDeferredClosureSlotOrUndefined;
}

static bool mayEndProofOver(const Effects& effects, const LocationMasks& dependedOn)
{
    return effects.maySuspend || effects.writes.overlaps(dependedOn);
}

static uint32_t hashOf(std::span<const MemoryLocation> locations)
{
    Hasher hasher;
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

DependedLocations::DependedLocations(const Program& program, Vector<MemoryLocation, 2>&& dependedOn)
    : locations(WTF::move(dependedOn))
{
    masksOfLocations.reserveInitialCapacity(locations.size());
    for (const MemoryLocation& location : locations) {
        uint64_t mask = maskOf(program, location);
        masksOfLocations.append(mask);
        masks.add(location.heap(), mask);
    }
}

namespace {

enum class WriteExtent : uint8_t { Nothing, InSomeObjects, Everywhere };

// What a writer may write on one edge of the locations that proofs depend on. It is the same for every object that such a proof is about.
struct WrittenLocations {
    WriteExtent extent { WriteExtent::Nothing };
    EdgeKind edge { EdgeKind::Normal };
    LocationMasks written;
    // Found when somebody asks: KillQuery::mayWriteBeyondOperands(), KillQuery::operandsWrittenThrough().
    std::optional<bool> mayWriteBeyondOperands;
    std::optional<Vector<ValueID, 4>> operandsWrittenThrough;
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

    WrittenLocations writtenOf(const DependedLocations&, EdgeKind);
    bool mayEndProofAbout(ValueID subject, WrittenLocations&);
    bool mayEndProofAbout(ValueID subject, bool subjectComesFromOutside, WrittenLocations&);
    bool reachesObjects(WrittenLocations&);
    bool mayWriteBeyondOperands(WrittenLocations&);

private:
    struct WrittenOperand {
        ValueID operand;
        LocationMasks writes;
    };

    const Terminator& terminator() const LIFETIME_BOUND { return m_function.at(m_point.block).terminator; }
    bool mayWrite(const Effects&, const MemoryLocation&, uint64_t maskOfLocation);
    std::span<const ValueID> operandsWrittenThrough(WrittenLocations&);
    bool isConfined(ValueID subject);
    bool isConfined(ObjectID);
    bool escapesAtWriter(ObjectID);
    bool isContainedInOperand(ObjectID);
    std::optional<bool> mayEndProofJudgedByDefinitions(ValueID subject, WrittenLocations&);
    bool mayEndProofJudgedByObjects(ValueID subject, WrittenLocations&);

    const MemoryAccess& exactAccess();
    const EffectsOnEdges& beyondOperands();
    std::span<const WrittenOperand> writtenOperands();
    const BitVector& operandObjects();
    const BitVector& objectsInsideOperands();

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    ProgramPoint m_point;
    ValueID m_writer;
    const EffectsOnEdges& m_effects;
    std::optional<MemoryAccess> m_exactAccess;
    std::optional<EffectsOnEdges> m_beyondOperands;
    std::optional<Vector<WrittenOperand, 4>> m_writtenOperands;
    std::optional<BitVector> m_operandObjects;
    std::optional<BitVector> m_objectsInsideOperands;
    std::optional<UseEffectHistory::AtPoint> m_history;
    ValueID m_lastSubject;
    bool m_isLastSubjectConfined { false };
    ValueID m_lastSubjectJudgedByDefinition;
    bool m_lastSubjectComesFromOutside { false };
};

WrittenLocations KillQuery::writtenOf(const DependedLocations& depended, EdgeKind edge)
{
    WrittenLocations result;
    result.edge = edge;

    const Effects& effects = effectsOn(m_effects, edge);
    if (effects.maySuspend) {
        result.extent = WriteExtent::Everywhere;
        return result;
    }
    if (!effects.writes.overlaps(depended.masks))
        return result;
    if (m_writer && leavesSlotsAsTested(m_analyses.program(), m_function, m_function.at(m_writer), effects, depended.slotState))
        return result;

    for (size_t index = 0; index < depended.locations.size(); ++index) {
        const MemoryLocation& location = depended.locations[index];
        uint64_t mask = depended.masksOfLocations[index];
        if (!mayWrite(effects, location, mask))
            continue;
        if (!perObjectHeaps.contains(location.heap())) {
            result.extent = WriteExtent::Everywhere;
            return result;
        }
        result.written.add(location.heap(), mask);
    }
    if (!result.written.isEmpty())
        result.extent = WriteExtent::InSomeObjects;
    return result;
}

// overlapOfWrites() for one writer and many locations.
bool KillQuery::mayWrite(const Effects& effects, const MemoryLocation& location, uint64_t maskOfLocation)
{
    const MemoryAccess& access = exactAccess();
    switch (access.kind) {
    case MemoryAccess::Kind::NotExact:
        return effects.writes.overlaps(location.heap(), maskOfLocation);
    case MemoryAccess::Kind::Read:
        return false;
    case MemoryAccess::Kind::Write:
        return overlapOf(m_analyses.program(), access.location, location) != LocationOverlap::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool KillQuery::mayEndProofAbout(ValueID subject, WrittenLocations& written)
{
    switch (written.extent) {
    case WriteExtent::Nothing:
        return false;
    case WriteExtent::Everywhere:
        return true;
    case WriteExtent::InSomeObjects:
        break;
    }
    if (!subject)
        return true;

    if (!reachesObjects(written))
        return false;

    std::optional<bool> mayEndProof = mayEndProofJudgedByDefinitions(subject, written);
    if (!mayEndProof)
        return mayEndProofJudgedByObjects(subject, written);
    if (m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]]
        RELEASE_ASSERT(*mayEndProof == mayEndProofJudgedByObjects(subject, written));
    return *mayEndProof;
}

bool KillQuery::mayEndProofAbout(ValueID subject, bool subjectComesFromOutside, WrittenLocations& written)
{
    m_lastSubjectJudgedByDefinition = subject;
    m_lastSubjectComesFromOutside = subjectComesFromOutside;
    return mayEndProofAbout(subject, written);
}

bool KillQuery::reachesObjects(WrittenLocations& written)
{
    return mayWriteBeyondOperands(written) || !operandsWrittenThrough(written).empty();
}

// An object from outside is confined nowhere, and two values from outside may name one object wherever their types allow it.
std::optional<bool> KillQuery::mayEndProofJudgedByDefinitions(ValueID subject, WrittenLocations& written)
{
    if (m_lastSubjectJudgedByDefinition != subject) {
        m_lastSubjectJudgedByDefinition = subject;
        m_lastSubjectComesFromOutside = comesFromOutside(m_analyses, subject);
    }
    if (!m_lastSubjectComesFromOutside)
        return std::nullopt;
    if (mayWriteBeyondOperands(written))
        return true;

    const Program& program = m_analyses.program();
    const Type& typeOfSubject = program.typeOf(m_function, subject);
    bool isSettled = true;
    for (ValueID operand : operandsWrittenThrough(written)) {
        if (m_analyses.sameObjectThroughCasts(operand) == m_analyses.sameObjectThroughCasts(subject))
            return true;
        if (!mayBeSameObject(program.typeOf(m_function, operand), typeOfSubject, program.types))
            continue;
        if (comesFromOutside(m_analyses, operand))
            return true;
        isSettled = false;
    }
    if (!isSettled)
        return std::nullopt;
    return false;
}

bool KillQuery::mayEndProofJudgedByObjects(ValueID subject, WrittenLocations& written)
{
    const EscapeAnalysis& escape = m_analyses.escape();
    if (!isConfined(subject)) {
        return mayWriteBeyondOperands(written) || std::ranges::any_of(operandsWrittenThrough(written), [&](ValueID operand) {
            return escape.mayAlias(operand, subject);
        });
    }

    ObjectSet objects = escape.pointsTo(subject).objects;
    return std::ranges::any_of(operandsWrittenThrough(written), [&](ValueID operand) {
        return escape.pointsTo(operand).objects.overlaps(objects);
    });
}

bool KillQuery::mayWriteBeyondOperands(WrittenLocations& written)
{
    if (!written.mayWriteBeyondOperands)
        written.mayWriteBeyondOperands = effectsOn(beyondOperands(), written.edge).writes.overlaps(written.written);
    return *written.mayWriteBeyondOperands;
}

std::span<const ValueID> KillQuery::operandsWrittenThrough(WrittenLocations& written)
{
    if (!written.operandsWrittenThrough) {
        written.operandsWrittenThrough.emplace();
        for (const WrittenOperand& operand : writtenOperands()) {
            if (operand.writes.overlaps(written.written))
                written.operandsWrittenThrough->append(operand.operand);
        }
    }
    return written.operandsWrittenThrough->span();
}

bool KillQuery::isConfined(ValueID subject)
{
    if (m_lastSubject == subject)
        return m_isLastSubjectConfined;

    const EscapeAnalysis& escape = m_analyses.escape();
    bool isConfinedSubject = false;
    // All that can happen to an entry parameter may have happened before the function was entered. A checking build looks whether that is what the history says.
    if (!escape.tracksOnlyEntryParameters() || m_analyses.context().build.options().verifiesAfterEveryPass) {
        PointsToSet set = escape.pointsTo(subject);
        isConfinedSubject = !set.mayBeUntracked;
        for (ObjectID object : set.objects) {
            if (!isConfinedSubject)
                break;
            isConfinedSubject = isConfined(object);
        }
        RELEASE_ASSERT(!escape.tracksOnlyEntryParameters() || !isConfinedSubject);
    }
    m_lastSubject = subject;
    m_isLastSubjectConfined = isConfinedSubject;
    return isConfinedSubject;
}

bool KillQuery::isConfined(ObjectID object)
{
    // Its history begins with all that can happen to it and never starts over. A checking build looks whether that is what the history says.
    const TrackedObject& tracked = m_analyses.escape().at(object);
    bool cameFromOutside = tracked.kind == ObjectKind::EntryParameter && tracked.definition != findFreshEntryParameter(m_function, m_analyses.cfg());
    if (cameFromOutside && !m_analyses.context().build.options().verifiesAfterEveryPass)
        return false;
    if (!m_history)
        m_history.emplace(m_analyses.escapeHistory(), m_point);
    if (m_history->occurrenceOf(object) != Occurrence::HasNotOccurred)
        return false;
    RELEASE_ASSERT(!cameFromOutside);
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

const MemoryAccess& KillQuery::exactAccess()
{
    if (!m_exactAccess)
        m_exactAccess = m_writer ? memoryAccessOf(m_function, m_function.at(m_writer)) : MemoryAccess { };
    return *m_exactAccess;
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
    GuardDependencies dependencies(program, locationsReadBy(test));
    dependencies.slotState = slotStateTestedBy(test);
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
    Vector<WrittenLocations, 8> written;
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
    WrittenLocations written = m_query->query.writtenOf(dependencies, EdgeKind::Normal);
    return m_query->query.mayEndProofAbout(dependencies.subject, written);
}

auto GuardWriter::writtenOf(const DependedLocations& depended) -> WrittenDependencies
{
    if (!m_query)
        return { EndedProofs::All, 0 };
    WrittenLocations written = m_query->query.writtenOf(depended, EdgeKind::Normal);
    switch (written.extent) {
    case WriteExtent::Nothing:
        return { EndedProofs::None, 0 };
    case WriteExtent::Everywhere:
        return { EndedProofs::All, 0 };
    case WriteExtent::InSomeObjects:
        break;
    }
    m_query->written.append(WTF::move(written));
    return { EndedProofs::AboutSomeObjects, static_cast<uint32_t>(m_query->written.size() - 1) };
}

bool GuardWriter::mayEndProofAbout(ValueID subject, bool subjectComesFromOutside, const WrittenDependencies& dependencies)
{
    switch (dependencies.endedProofs) {
    case EndedProofs::None:
        return false;
    case EndedProofs::All:
        return true;
    case EndedProofs::AboutSomeObjects:
        break;
    }
    if (!subject)
        return true;
    WrittenLocations& written = m_query->written[dependencies.index];
    if (!m_query->query.reachesObjects(written))
        return false;
    if (subjectComesFromOutside && m_query->query.mayWriteBeyondOperands(written))
        return true;
    return m_query->query.mayEndProofAbout(subject, subjectComesFromOutside, written);
}

GuardAvailability::GuardAvailability(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_function(analyses.function())
{
    collectGuards();
    m_numberOfNamedGuards = m_guards.size();
    collectUnnamedAllocations();

    describeGuards();
    if (!exceedsWorkLimit())
        return;

    reportWorkLimit();
    if (m_guards.size() > m_numberOfNamedGuards) {
        removeGuardsFrom(m_numberOfNamedGuards);
        describeGuards();
        if (!exceedsWorkLimit())
            return;
    }
    leaveOutGuardsInRarerBlocks();
}

// Every guard keeps its facts. Where they hold is solved for the guards in the most frequent blocks, each with the guards behind it, as far as the limit goes. The state of the others is unknown.
void GuardAvailability::leaveOutGuardsInRarerBlocks()
{
    const UseLists& useLists = m_analyses.useLists();
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    uint64_t limit = m_analyses.context().build.limit(LimitID::MaxGuardAvailabilityWork);
    uint64_t maxComponents = limit / std::max<uint64_t>(m_analyses.cfg().reversePostOrder().size(), 1);
    uint64_t maxKillClasses = limit / std::max<uint64_t>(countWriters(), 1);

    Vector<uint32_t> mostFrequentFirst(m_guards.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::stableSort(mostFrequentFirst, std::ranges::greater { }, [&](uint32_t guardIndex) {
        BlockID block = useLists.blockOf(m_guards[guardIndex]);
        return block ? frequencies.frequencyOf(block) : 0;
    });

    BitVector solved(m_guards.size());
    BitVector killClasses(m_killClasses.size());
    uint64_t numberOfComponents = 0;
    for (uint32_t guardIndex : mostFrequentFirst) {
        if (solved.get(guardIndex) || componentsOfGuardAt(guardIndex).empty())
            continue;
        BitVector solvedWithGuard = solved;
        addGuardsBehind(m_guards[guardIndex], solvedWithGuard);
        BitVector added = solvedWithGuard;
        added.exclude(solved);

        BitVector killClassesWithGuard = killClasses;
        uint64_t numberOfComponentsWithGuard = numberOfComponents;
        for (size_t addedIndex : added) {
            for (const Component& component : componentsOfGuardAt(addedIndex)) {
                killClassesWithGuard.set(component.killClass);
                ++numberOfComponentsWithGuard;
            }
        }
        if (numberOfComponentsWithGuard > maxComponents || killClassesWithGuard.bitCount() > maxKillClasses)
            continue;
        solved = WTF::move(solvedWithGuard);
        killClasses = WTF::move(killClassesWithGuard);
        numberOfComponents = numberOfComponentsWithGuard;
    }

    for (uint32_t guardIndex = 0; guardIndex < m_guards.size(); ++guardIndex) {
        if (!solved.get(guardIndex) && !componentsOfGuardAt(guardIndex).empty())
            m_guardsLeftOut.set(guardIndex);
    }
    m_components.clear();
    m_firstComponent.clear();
    m_dependedLocations.clear();
    m_killClasses.clear();
    m_killClassesBySubject.clear();
    m_numberOfKillClassesOfNamedGuards = 0;
    numberComponents();
    m_solution.gaveUp = m_components.isEmpty();
}

void GuardAvailability::describeGuards()
{
    m_facts.clear();
    m_components.clear();
    m_firstComponent.clear();
    m_dependedLocations.clear();
    m_killClasses.clear();
    m_killClassesBySubject.clear();
    m_numberOfKillClassesOfNamedGuards = 0;

    computeFacts();
    numberComponents();
}

void GuardAvailability::numberComponents()
{
    HashedIndex locationsIndex;
    HashedIndex killClassIndex;
    m_firstComponent.reserveInitialCapacity(m_guards.size() + 1);
    for (uint32_t guardIndex = 0; guardIndex < m_guards.size(); ++guardIndex) {
        m_firstComponent.append(m_components.size());
        const GuardFacts& facts = m_facts[guardIndex];
        if (!facts.isKnown() || m_guardsLeftOut.get(guardIndex))
            continue;
        for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
            uint32_t killClass = internKillClass(facts.facts[factIndex], locationsIndex, killClassIndex);
            m_killClasses[killClass].components.set(m_components.size());
            m_components.append(Component { m_guards[guardIndex], factIndex, killClass });
        }
        if (guardIndex < m_numberOfNamedGuards)
            m_numberOfKillClassesOfNamedGuards = m_killClasses.size();
    }
    m_firstComponent.append(m_components.size());

    m_killClassesBySubject.reserveInitialCapacity(m_killClasses.size());
    for (uint32_t killClass = 0; killClass < m_killClasses.size(); ++killClass)
        m_killClassesBySubject.append(killClass);
    AOT::sort(m_killClassesBySubject, { }, [&](uint32_t killClass) {
        return std::pair { m_killClasses[killClass].subject, killClass };
    });
}

uint32_t GuardAvailability::internLocations(const Vector<MemoryLocation, 2>& locations, TestedSlotState slotState, HashedIndex& index)
{
    uint32_t hash = computeHash(hashOf(locations.span()), slotState);
    std::optional<uint32_t> existing = index.find(hash, [&](uint32_t position) {
        return m_dependedLocations[position].locations == locations && m_dependedLocations[position].slotState == slotState;
    });
    if (existing)
        return *existing;

    uint32_t position = m_dependedLocations.size();
    m_dependedLocations.append(DependedLocations(m_analyses.program(), Vector<MemoryLocation, 2>(locations)));
    m_dependedLocations.last().slotState = slotState;
    index.add(hash, position);
    return position;
}

uint32_t GuardAvailability::internKillClass(const GuardFact& fact, HashedIndex& locationsIndex, HashedIndex& killClassIndex)
{
    TestedSlotState slotState = slotStateProvenBy(m_function, fact);
    uint32_t killClass = internKillClass(fact, slotState, locationsIndex, killClassIndex);
    if (slotState != TestedSlotState::None) {
        uint32_t classBetweenTestAndBranch = internKillClass(fact, TestedSlotState::None, locationsIndex, killClassIndex);
        m_killClasses[killClass].classBetweenTestAndBranch = classBetweenTestAndBranch;
    }
    return killClass;
}

uint32_t GuardAvailability::internKillClass(const GuardFact& fact, TestedSlotState slotState, HashedIndex& locationsIndex, HashedIndex& killClassIndex)
{
    uint32_t locations = internLocations(fact.dependsOn, slotState, locationsIndex);
    ValueID subject = exclusiveSubjectOf(m_analyses, fact);
    uint32_t hash = computeHash(locations, subject);
    std::optional<uint32_t> existing = killClassIndex.find(hash, [&](uint32_t position) {
        const KillClass& killClass = m_killClasses[position];
        return killClass.locations == locations && killClass.subject == subject;
    });
    if (existing)
        return *existing;

    uint32_t position = m_killClasses.size();
    m_killClasses.append(KillClass { locations, subject, { }, position });
    killClassIndex.add(hash, position);
    return position;
}

bool GuardAvailability::exceedsWorkLimit(uint64_t work) const
{
    return work > m_analyses.context().build.limit(LimitID::MaxGuardAvailabilityWork);
}

// A solve holds a bit for each component at each block, and at most every writer is asked about every class. Every operation and every terminator may be a writer: they are counted only where that many would be too many.
bool GuardAvailability::exceedsWorkLimit() const
{
    uint64_t numberOfBlocks = m_analyses.cfg().reversePostOrder().size();
    if (exceedsWorkLimit(numberOfBlocks * m_components.size()))
        return true;
    uint64_t mostWriters = m_function.values.size() + numberOfBlocks;
    return exceedsWorkLimit(mostWriters * m_killClasses.size()) && exceedsWorkLimit(countWriters() * m_killClasses.size());
}

uint64_t GuardAvailability::countWriters() const
{
    LocationMasks dependedOn;
    for (const KillClass& killClass : m_killClasses)
        dependedOn.merge(m_dependedLocations[killClass.locations].masks);

    const CFG& cfg = m_analyses.cfg();
    uint64_t numberOfWriters = 0;
    for (BlockID block : cfg.reversePostOrder()) {
        bool hasHandler = cfg.hasHandler(block);
        auto mayEndProof = [&](const EffectsOnEdges& effects) {
            return mayEndProofOver(effects.normal, dependedOn) || (hasHandler && mayEndProofOver(effects.handler, dependedOn));
        };
        for (ValueID value : m_function.at(block).values)
            numberOfWriters += mayEndProof(m_analyses.effectsOf(value));
        numberOfWriters += mayEndProof(m_analyses.effectsOfTerminator(block));
    }
    return numberOfWriters;
}

void GuardAvailability::reportWorkLimit() const
{
    if (!std::exchange(m_solution.didReportWorkLimit, true))
        m_analyses.context().build.gaveUp(LimitID::MaxGuardAvailabilityWork, m_analyses.subject());
}

bool GuardAvailability::gaveUp() const
{
    prepareToSolve();
    return m_solution.gaveUp;
}

// What is given up is decided for all guards together, so that it is the same whichever of them are asked about.
void GuardAvailability::prepareToSolve() const
{
    Solution& solution = m_solution;
    if (std::exchange(solution.isPrepared, true) || solution.gaveUp || m_components.isEmpty())
        return;

    LocationMasks dependedOn;
    LocationMasks dependedOnByNamedGuards;
    for (uint32_t killClass = 0; killClass < m_killClasses.size(); ++killClass) {
        const LocationMasks& masks = m_dependedLocations[m_killClasses[killClass].locations].masks;
        dependedOn.merge(masks);
        if (killClass < m_numberOfKillClassesOfNamedGuards)
            dependedOnByNamedGuards.merge(masks);
    }

    const CFG& cfg = m_analyses.cfg();
    std::span<const BlockID> order = cfg.reversePostOrder();
    uint64_t numberOfWritersForNamedGuards = 0;
    for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
        BlockID block = order[blockOrder];
        bool hasHandler = cfg.hasHandler(block);
        auto mayEndProof = [&](const EffectsOnEdges& effects, const LocationMasks& masks) {
            return mayEndProofOver(effects.normal, masks) || (hasHandler && mayEndProofOver(effects.handler, masks));
        };
        auto addWriter = [&](uint32_t index, const EffectsOnEdges& effects) {
            if (!mayEndProof(effects, dependedOn))
                return;
            solution.writers.append(Writer { blockOrder, index });
            if (mayEndProof(effects, dependedOnByNamedGuards))
                ++numberOfWritersForNamedGuards;
        };
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            // The read that makes the closure a slot stands for writes the slot. The object has the same property with the same value as before, and no proof is about more than that.
            Opcode opcode = m_function.at(values[index]).opcode;
            if (isNeverWriter(opcode))
                continue;
            addWriter(index, m_analyses.effectsOf(values[index]));
        }
        addWriter(values.size(), m_analyses.effectsOfTerminator(block));
    }

    // At most every writer is asked about every class, so the work is bounded before any of it is done.
    if (exceedsWorkLimit(static_cast<uint64_t>(solution.writers.size()) * m_killClasses.size())) {
        reportWorkLimit();
        bool hasUnnamedAllocations = m_guards.size() > m_numberOfNamedGuards;
        if (!hasUnnamedAllocations || exceedsWorkLimit(numberOfWritersForNamedGuards * m_numberOfKillClassesOfNamedGuards)) {
            solution.gaveUp = true;
            solution.writers.clear();
            return;
        }
        solution.solvesUnnamedAllocations = false;
    }

    size_t numberOfBlocks = m_function.blocks.size();
    solution.killsOfBlock.grow(numberOfBlocks);
    solution.killedOnThrow.grow(numberOfBlocks);
    solution.availableAtHead.fill(BitVector(m_components.size()), numberOfBlocks);
    solution.solvedBlocks.ensureSize(numberOfBlocks);
    for (BlockID block : order)
        solution.solvedBlocks.quickSet(block.index());
}

bool GuardAvailability::canSolve(ValueID guard) const
{
    prepareToSolve();
    if (m_solution.gaveUp)
        return false;
    std::optional<uint32_t> guardIndex = guardIndexOf(guard);
    if (guardIndex && m_guardsLeftOut.get(*guardIndex))
        return false;
    return !guardIndex || *guardIndex < m_numberOfNamedGuards || m_solution.solvesUnnamedAllocations;
}

void GuardAvailability::solveFor(ValueID guard) const
{
    if (!canSolve(guard) || componentsOf(guard).empty())
        return;

    Solution& solution = m_solution;
    uint32_t guardIndex = *guardIndexOf(guard);
    if (solution.solvedGuards.get(guardIndex))
        return;

    if (!isWorthSolvingAlone(guardIndex)) {
        if (guardIndex < m_numberOfNamedGuards)
            solveNamedGuards();
        else
            solveUnnamedAllocations();
        return;
    }

    ++solution.numberOfSeparateSolves;
    BitVector guardIndexes;
    addGuardsBehind(guard, guardIndexes);
    solve(guardIndexes);
}

// Every solve walks over the whole function, and solving all guards also asks every writer about every class.
bool GuardAvailability::isWorthSolvingAlone(uint32_t guardIndex) const
{
    const Solution& solution = m_solution;
    bool isNamed = guardIndex < m_numberOfNamedGuards;
    uint32_t firstKillClass = isNamed ? 0 : m_numberOfKillClassesOfNamedGuards;
    uint32_t endKillClass = isNamed ? m_numberOfKillClassesOfNamedGuards : m_killClasses.size();
    uint64_t numberOfKillClassesLeft = 0;
    for (uint32_t killClass = firstKillClass; killClass < endKillClass; ++killClass)
        numberOfKillClassesLeft += !solution.killClassesWithKills.get(killClass);

    uint64_t costOfWalks = static_cast<uint64_t>(solution.numberOfSeparateSolves + 1) * m_function.values.size();
    return costOfWalks <= solution.writers.size() * numberOfKillClassesLeft;
}

void GuardAvailability::solveNamedGuards() const
{
    if (std::exchange(m_solution.didSolveAllNamedGuards, true))
        return;
    prepareToSolve();
    if (!m_solution.gaveUp)
        solveGuardsBetween(0, m_numberOfNamedGuards);
}

void GuardAvailability::solveUnnamedAllocations() const
{
    if (std::exchange(m_solution.didSolveAllUnnamedAllocations, true))
        return;
    prepareToSolve();
    if (!m_solution.gaveUp && m_solution.solvesUnnamedAllocations)
        solveGuardsBetween(m_numberOfNamedGuards, m_guards.size());
}

// No named guard holds what an unnamed allocation proves, so either half is solved without the other.
void GuardAvailability::solveGuardsBetween(uint32_t firstGuardIndex, uint32_t endGuardIndex) const
{
    BitVector guardIndexes;
    bool hasUnsolvedGuard = false;
    for (uint32_t guardIndex = firstGuardIndex; guardIndex < endGuardIndex; ++guardIndex) {
        if (componentsOfGuardAt(guardIndex).empty())
            continue;
        guardIndexes.set(guardIndex);
        hasUnsolvedGuard |= !m_solution.solvedGuards.get(guardIndex);
    }
    if (hasUnsolvedGuard)
        solve(guardIndexes);
}

// A list holds what its operands hold, and a parameter what is passed for it.
void GuardAvailability::addGuardsBehind(ValueID guard, BitVector& guardIndexes) const
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& useLists = m_analyses.useLists();
    Vector<ValueID, 8> worklist { guard };
    while (!worklist.isEmpty()) {
        ValueID next = worklist.takeLast();
        if (componentsOf(next).empty() || guardIndexes.set(*guardIndexOf(next)))
            continue;

        ValueLocation location = useLists.locationOf(next);
        switch (location.placement) {
        case ValuePlacement::Detached:
            break;
        case ValuePlacement::InBlock:
            if (VariadicOperandPosition::guards(m_function.at(next).opcode))
                worklist.append(m_function.at(next).guardsOperands());
            break;
        case ValuePlacement::Parameter:
            for (Edge edge : cfg.incomingEdges(location.block)) {
                IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
                if (incoming.kind == IncomingValue::Kind::Argument && incoming.argument)
                    worklist.append(incoming.argument);
            }
            break;
        }
    }
}

// The guards come with every guard behind them.
void GuardAvailability::solve(const BitVector& guardIndexes) const
{
    BitVector killClasses;
    for (size_t guardIndex : guardIndexes) {
        for (const Component& component : componentsOfGuardAt(guardIndex)) {
            killClasses.set(component.killClass);
            killClasses.set(m_killClasses[component.killClass].classBetweenTestAndBranch);
        }
    }
    collectKills(killClasses);

    struct Problem {
        using State = BitVector;
        static constexpr bool readsEffects() { return false; }

        State stateAtEntry() const { return BitVector(availability.m_components.size()); }

        void execute(State& state, const DataflowStep& step) const
        {
            availability.applyKills(state, step.block, step.index);
            availability.applyDefinitions(state, guardIndexes, step.block, step.index);
        }

        void executeTerminator(State& state, const DataflowTerminatorStep& step) const
        {
            availability.applyKills(state, step.block, availability.m_function.at(step.block).values.size());
        }

        void enterEdge(State& state, Edge edge) const { availability.enterEdge(state, guardIndexes, edge); }

        bool merge(State& into, const State& from) const
        {
            size_t previousCount = into.bitCount();
            into.filter(from);
            return into.bitCount() != previousCount;
        }

        const GuardAvailability& availability;
        const BitVector& guardIndexes;
    };

    Solution& solution = m_solution;
    Problem problem { *this, guardIndexes };
    Vector<std::optional<BitVector>> heads = m_analyses.solveForward(problem);
    for (size_t blockIndex = 0; blockIndex < heads.size(); ++blockIndex) {
        if (heads[blockIndex])
            solution.availableAtHead[blockIndex].merge(*heads[blockIndex]);
    }
    solution.solvedGuards.merge(guardIndexes);
    ++solution.numberOfSolves;
}

void GuardAvailability::collectKillsOf(ValueID guard) const
{
    BitVector killClasses;
    for (const Component& component : componentsOf(guard))
        killClasses.set(component.killClass);
    collectKills(killClasses);
}

void GuardAvailability::collectKills(const BitVector& wantedKillClasses) const
{
    Solution& solution = m_solution;
    BitVector added = wantedKillClasses;
    added.exclude(solution.killClassesWithKills);
    if (added.isEmpty())
        return;
    solution.killClassesWithKills.merge(added);

    // The classes about one subject follow each other, so a writer finds out once whether it can reach that subject.
    Vector<uint32_t, 16> killClasses;
    LocationMasks dependedOn;
    for (uint32_t killClass : m_killClassesBySubject) {
        if (!added.get(killClass))
            continue;
        killClasses.append(killClass);
        dependedOn.merge(m_dependedLocations[m_killClasses[killClass].locations].masks);
    }

    // What a writer writes of some locations is the same for all classes that depend on them, and so is the answer wherever it does not hang on the object: nearly every writer is unknown code, which reaches every object from outside.
    struct ClassesOfLocations {
        uint32_t locations { 0 };
        bool hasSubjects { false };
        BitVector all;
        BitVector withoutSubject;
        BitVector aboutObjectsFromOutside;
    };
    struct ClassAboutSubject {
        uint32_t killClass { 0 };
        uint32_t group { 0 };
        ValueID subject;
        bool comesFromOutside { false };
    };
    Vector<ClassesOfLocations, 8> groups;
    Vector<ClassAboutSubject, 16> classesAboutSubjects;
    {
        constexpr uint32_t noGroup = UINT32_MAX;
        Vector<uint32_t, 16> groupOfLocations(FillWith { }, m_dependedLocations.size(), noGroup);
        ValueID lastSubject;
        bool lastSubjectComesFromOutside = false;
        for (uint32_t index : killClasses) {
            const KillClass& killClass = m_killClasses[index];
            uint32_t& group = groupOfLocations[killClass.locations];
            if (group == noGroup) {
                group = groups.size();
                groups.append(ClassesOfLocations { .locations = killClass.locations });
            }
            ClassesOfLocations& classes = groups[group];
            classes.all.set(index);
            if (!killClass.subject) {
                classes.withoutSubject.set(index);
                continue;
            }
            if (killClass.subject != lastSubject) {
                lastSubject = killClass.subject;
                lastSubjectComesFromOutside = comesFromOutside(m_analyses, lastSubject);
            }
            classes.hasSubjects = true;
            if (lastSubjectComesFromOutside)
                classes.aboutObjectsFromOutside.set(index);
            classesAboutSubjects.append(ClassAboutSubject { index, group, lastSubject, lastSubjectComesFromOutside });
        }
    }

    auto findKilledClassesOneByOne = [&](KillQuery& query, EdgeKind edge) -> BitVector {
        Vector<std::optional<WrittenLocations>, 8> writtenOfLocations(m_dependedLocations.size());
        BitVector killed;
        for (uint32_t index : killClasses) {
            const KillClass& killClass = m_killClasses[index];
            std::optional<WrittenLocations>& written = writtenOfLocations[killClass.locations];
            if (!written)
                written = query.writtenOf(m_dependedLocations[killClass.locations], edge);
            if (query.mayEndProofAbout(killClass.subject, *written))
                killed.set(index);
        }
        return killed;
    };

    Vector<WrittenLocations, 8> writtenOfGroups;
    auto findKilledClasses = [&](KillQuery& query, const Effects& effects, EdgeKind edge) -> BitVector {
        if (effects.maySuspend)
            return added;

        BitVector killed;
        bool hangsOnObjects = false;
        writtenOfGroups.shrink(0);
        for (const ClassesOfLocations& classes : groups) {
            writtenOfGroups.append(query.writtenOf(m_dependedLocations[classes.locations], edge));
            WrittenLocations& written = writtenOfGroups.last();
            switch (written.extent) {
            case WriteExtent::Nothing:
                break;
            case WriteExtent::Everywhere:
                killed.merge(classes.all);
                break;
            case WriteExtent::InSomeObjects:
                killed.merge(classes.withoutSubject);
                if (!classes.hasSubjects || !query.reachesObjects(written)) {
                    written.extent = WriteExtent::Nothing;
                    break;
                }
                if (query.mayWriteBeyondOperands(written))
                    killed.merge(classes.aboutObjectsFromOutside);
                hangsOnObjects = true;
                break;
            }
        }
        if (hangsOnObjects) {
            for (const ClassAboutSubject& entry : classesAboutSubjects) {
                WrittenLocations& written = writtenOfGroups[entry.group];
                if (written.extent != WriteExtent::InSomeObjects || (entry.comesFromOutside && query.mayWriteBeyondOperands(written)))
                    continue;
                if (query.mayEndProofAbout(entry.subject, entry.comesFromOutside, written))
                    killed.set(entry.killClass);
            }
        }
        if (m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]]
            RELEASE_ASSERT(killed == findKilledClassesOneByOne(query, edge));
        return killed;
    };

    const CFG& cfg = m_analyses.cfg();
    std::span<const BlockID> order = cfg.reversePostOrder();
    for (const Writer& writer : solution.writers) {
        BlockID block = order[writer.blockOrder];
        const BlockValues& values = m_function.at(block).values;
        const EffectsOnEdges& effects = writer.index < values.size() ? m_analyses.effectsOf(values[writer.index]) : m_analyses.effectsOfTerminator(block);
        bool mayKillOnNormalEdge = mayEndProofOver(effects.normal, dependedOn);
        bool mayKillOnThrow = cfg.hasHandler(block) && mayEndProofOver(effects.handler, dependedOn);
        if (!mayKillOnNormalEdge && !mayKillOnThrow)
            continue;

        KillQuery query(m_analyses, { block, writer.index }, effects);
        if (mayKillOnNormalEdge) {
            BitVector killed = findKilledClasses(query, effects.normal, EdgeKind::Normal);
            if (!killed.isEmpty())
                addKill(block, writer.index, WTF::move(killed));
        }
        if (mayKillOnThrow) {
            BitVector killed = findKilledClasses(query, effects.handler, EdgeKind::Handler);
            for (size_t killClass : killed)
                solution.killedOnThrow[block.index()].merge(m_killClasses[killClass].components);
        }
    }
}

void GuardAvailability::addKill(BlockID block, uint32_t index, BitVector&& killClasses) const
{
    Kill kill { index, internKillSet(WTF::move(killClasses)) };
    Vector<Kill>& kills = m_solution.killsOfBlock[block.index()];
    if (kills.isEmpty() || kills.last().index <= index) {
        kills.append(kill);
        return;
    }
    size_t position = std::ranges::upper_bound(kills, index, { }, &Kill::index) - kills.begin();
    kills.insert(position, kill);
}

uint32_t GuardAvailability::internKillSet(BitVector&& killClasses) const
{
    Solution& solution = m_solution;
    uint32_t hash = killClasses.hash();
    std::optional<uint32_t> existing = solution.killSetIndex.find(hash, [&](uint32_t position) {
        return solution.killSets[position].killClasses == killClasses;
    });
    if (existing)
        return *existing;

    BitVector components;
    for (size_t killClass : killClasses)
        components.merge(m_killClasses[killClass].components);

    uint32_t position = solution.killSets.size();
    solution.killSets.append(KillSet { WTF::move(killClasses), WTF::move(components) });
    solution.killSetIndex.add(hash, position);
    return position;
}

void GuardAvailability::applyKills(BitVector& available, BlockID block, uint32_t index) const
{
    for (const Kill& kill : killsAt({ block, index }))
        available.exclude(m_solution.killSets[kill.killSet].components);
}

void GuardAvailability::applyDefinitions(BitVector& available, const BitVector& guardIndexes, BlockID block, uint32_t index) const
{
    ValueID id = m_function.at(block).values[index];
    std::span<const Component> components = componentsOf(id);
    if (components.empty() || !guardIndexes.get(*guardIndexOf(id)))
        return;

    const Value& value = m_function.at(id);
    const GuardFacts& facts = factsOf(id);
    uint32_t firstComponent = firstComponentOf(id);
    bool unitesGuards = VariadicOperandPosition::guards(value.opcode).has_value();
    for (const Component& component : components) {
        bool isAvailable = !unitesGuards || std::ranges::any_of(value.guardsOperands(), [&](ValueID operand) {
            return isAvailableIn(available, operand, facts.facts[component.factIndex]);
        });
        available.quickSet(firstComponent + component.factIndex, isAvailable);
    }
}

void GuardAvailability::enterEdge(BitVector& available, const BitVector& guardIndexes, Edge edge) const
{
    if (edge.isHandlerEdge())
        available.exclude(m_solution.killedOnThrow[edge.from.index()]);

    Vector<std::pair<uint32_t, bool>, 8> assignments;
    auto& parameters = m_function.at(targetOf(m_function, edge).block).parameters;
    for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
        ValueID parameter = parameters[parameterIndex];
        std::span<const Component> components = componentsOf(parameter);
        if (components.empty() || !guardIndexes.get(*guardIndexOf(parameter)))
            continue;

        IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
        std::optional<EdgeResult> result;
        if (incoming.kind == IncomingValue::Kind::EdgeResult)
            result = tryGetEdgeResult(m_function, incoming.result);

        const GuardFacts& facts = factsOf(parameter);
        uint32_t firstComponent = firstComponentOf(parameter);
        for (const Component& component : components) {
            const GuardFact& fact = facts.facts[component.factIndex];
            bool isAvailable = leavesUnspecified(edge, fact.subject);
            if (!isAvailable)
                isAvailable = result ? isProvenOnEdge(edge, *result, component) : isAvailableBeforeEdge(available, edge, incoming.argument, fact);
            assignments.append({ firstComponent + component.factIndex, isAvailable });
        }
    }

    for (auto [bit, isAvailable] : assignments)
        available.quickSet(bit, isAvailable);
}

bool GuardAvailability::isProvenOnEdge(Edge edge, const EdgeResult& result, const Component& component) const
{
    if (result.kind != EdgeResultKind::Guard)
        return false;
    return result.caseIndex || !mayEndBetweenTestAndBranch(result.test, edge.from, m_killClasses[component.killClass].classBetweenTestAndBranch);
}

bool GuardAvailability::mayEndBetweenTestAndBranch(ValueID test, BlockID branch, uint32_t killClass) const
{
    ValueLocation location = m_analyses.useLists().locationOf(test);
    if (location.placement != ValuePlacement::InBlock)
        return true;

    auto endsProof = [&](const Kill& kill) {
        return ends(kill, killClass);
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

bool GuardAvailability::isAvailableBeforeEdge(const BitVector& available, Edge edge, ValueID guard, const GuardFact& factAfterEdge) const
{
    std::optional<ValueID> subject = subjectBeforeEdge(factAfterEdge.subject, edge);
    return subject && isAvailableIn(available, guard, factAfterEdge, *subject);
}

bool GuardAvailability::isAvailableIn(const BitVector& available, ValueID guard, const GuardFact& fact) const
{
    return isAvailableIn(available, guard, fact, fact.subject);
}

bool GuardAvailability::isAvailableIn(const BitVector& available, ValueID guard, const GuardFact& fact, ValueID subject) const
{
    if (componentsOf(guard).empty())
        return false;

    uint32_t firstComponent = firstComponentOf(guard);
    const GuardFacts& facts = factsOf(guard);
    for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
        const GuardFact& candidate = facts.facts[factIndex];
        if (available.quickGet(firstComponent + factIndex) && candidate.subject == subject && isAsStrongAs(candidate, fact))
            return true;
    }
    return false;
}

bool GuardAvailability::ends(const Kill& kill, uint32_t killClass) const
{
    return m_solution.killSets[kill.killSet].killClasses.get(killClass);
}

auto GuardAvailability::killsIn(BlockID block) const -> std::span<const Kill>
{
    return m_solution.killsOfBlock[block.index()].span();
}

auto GuardAvailability::killsAt(ProgramPoint point) const -> std::span<const Kill>
{
    return std::span<const Kill>(std::ranges::equal_range(killsIn(point.block), point.index, { }, &Kill::index));
}

std::optional<uint32_t> GuardAvailability::guardIndexOf(ValueID guard) const
{
    if (guard.index() >= m_guardIndexOfValue.size())
        return std::nullopt;
    return m_guardIndexOfValue[guard.index()];
}

auto GuardAvailability::componentsOfGuardAt(uint32_t guardIndex) const -> std::span<const Component>
{
    uint32_t first = m_firstComponent[guardIndex];
    return m_components.subspan(first, m_firstComponent[guardIndex + 1] - first);
}

auto GuardAvailability::componentsOf(ValueID guard) const -> std::span<const Component>
{
    std::optional<uint32_t> guardIndex = guardIndexOf(guard);
    if (!guardIndex)
        return { };
    return componentsOfGuardAt(*guardIndex);
}

uint32_t GuardAvailability::firstComponentOf(ValueID guard) const
{
    std::optional<uint32_t> guardIndex = guardIndexOf(guard);
    RELEASE_ASSERT(guardIndex);
    return m_firstComponent[*guardIndex];
}

GuardState GuardAvailability::stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn) const
{
    return stateIn(available, guard, reliedOn, ValueID());
}

GuardState GuardAvailability::stateIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject) const
{
    if (!canSolve(guard))
        return GuardState::Unknown;
    return stateOfSolvableGuardIn(available, guard, reliedOn, subject);
}

GuardState GuardAvailability::stateOfSolvableGuardIn(const BitVector& available, ValueID guard, const ProofSet* reliedOn, ValueID subject) const
{
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
    if (!canSolve(guard))
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
    if (!canSolve(guard) || !facts.isKnown() || !m_analyses.cfg().isReachable(writer.block))
        return true;
    // Only what is listed as a writer ends anything, and the list is there. What each writer ends takes a walk over all of them.
    auto placeOf = [](const Writer& listed) {
        return std::pair { listed.blockOrder, listed.index };
    };
    if (!std::ranges::binary_search(m_solution.writers, std::pair { m_analyses.cfg().reversePostOrderIndex(writer.block), writer.index }, { }, placeOf))
        return false;
    collectKillsOf(guard);

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
        for (const Kill& kill : killsAt(writer)) {
            if (ends(kill, component.killClass))
                return true;
        }
    }
    return false;
}

GuardState GuardAvailability::stateAt(ValueID guard, ProgramPoint point) const
{
    solveFor(guard);
    return Cursor(*this, point).stateOf(guard);
}

GuardState GuardAvailability::stateAt(ValueID guard, ProofSet reliedOn, ProgramPoint point) const
{
    return stateAt(guard, reliedOn, ValueID(), point);
}

// What canSolve() says, where that is seen without the walk that lists the writers: every operation and every terminator may be one.
bool GuardAvailability::canSolveForAnyWriters(uint32_t guardIndex) const
{
    if (m_solution.isPrepared)
        return canSolve(m_guards[guardIndex]);
    if (m_solution.gaveUp || m_guardsLeftOut.get(guardIndex))
        return false;
    uint64_t mostWriters = m_function.values.size() + m_function.blocks.size();
    return !exceedsWorkLimit(mostWriters * m_killClasses.size());
}

// What collectKills() finds for one writer and one class on the normal edge.
bool GuardAvailability::endsProofsOfClass(ProgramPoint writer, const EffectsOnEdges& effects, uint32_t killClassIndex) const
{
    const KillClass& killClass = m_killClasses[killClassIndex];
    const DependedLocations& locations = m_dependedLocations[killClass.locations];
    if (!mayEndProofOver(effects.normal, locations.masks))
        return false;
    if (effects.normal.maySuspend)
        return true;
    KillQuery query(m_analyses, writer, effects);
    WrittenLocations written = query.writtenOf(locations, EdgeKind::Normal);
    return query.mayEndProofAbout(killClass.subject, written);
}

bool GuardAvailability::hasWriterThatEndsProofsOfClass(BlockID block, uint32_t firstIndex, uint32_t killClass) const
{
    const BlockValues& values = m_function.at(block).values;
    for (uint32_t index = firstIndex; index < values.size(); ++index) {
        if (!isNeverWriter(m_function.at(values[index]).opcode) && endsProofsOfClass({ block, index }, m_analyses.effectsOf(values[index]), killClass))
            return true;
    }
    return endsProofsOfClass({ block, static_cast<uint32_t>(values.size()) }, m_analyses.effectsOfTerminator(block), killClass);
}

// mayEndBetweenTestAndBranch() without the kills of a solve.
bool GuardAvailability::hasWriterBetweenTestAndBranch(ValueID test, BlockID branch, uint32_t killClass) const
{
    ValueLocation location = m_analyses.useLists().locationOf(test);
    if (location.placement != ValuePlacement::InBlock)
        return true;

    const CFG& cfg = m_analyses.cfg();
    for (BlockID block = branch; block != location.block;) {
        std::optional<Edge> onlyWayIn = cfg.singleIncomingEdge(block);
        if (!onlyWayIn || onlyWayIn->isHandlerEdge() || hasWriterThatEndsProofsOfClass(block, 0, killClass))
            return true;
        block = onlyWayIn->from;
    }
    return hasWriterThatEndsProofsOfClass(location.block, location.index + 1, killClass);
}

// What applyDefinitions() and enterEdge() set where the guard is defined. Nothing where that is not seen from here.
std::optional<BitVector> GuardAvailability::componentsProvenAtDefinition(ValueID guard, ValueLocation location, unsigned depth) const
{
    std::span<const Component> components = componentsOf(guard);
    const GuardFacts& facts = factsOf(guard);
    uint32_t firstComponent = firstComponentOf(guard);
    BitVector available;

    if (location.placement == ValuePlacement::InBlock) {
        const Value& value = m_function.at(guard);
        if (!VariadicOperandPosition::guards(value.opcode)) {
            for (const Component& component : components)
                available.set(firstComponent + component.factIndex);
            return available;
        }

        // The solve lets the value end proofs before it looks at its operands.
        const Effects& effects = m_analyses.effectsOf(guard).normal;
        if (effects.maySuspend || !effects.writes.isEmpty())
            return std::nullopt;

        BitVector heldByOperands;
        for (ValueID operand : value.guardsOperands()) {
            if (componentsOf(operand).empty())
                continue;
            std::optional<BitVector> held = componentsHeldAt(operand, { location.block, location.index }, BehindTerminator::No, depth + 1);
            if (!held)
                return std::nullopt;
            heldByOperands.merge(*held);
        }
        heldByOperands.ensureSize(m_components.size());
        for (const Component& component : components) {
            bool isAvailable = std::ranges::any_of(value.guardsOperands(), [&](ValueID operand) {
                return isAvailableIn(heldByOperands, operand, facts.facts[component.factIndex]);
            });
            if (isAvailable)
                available.set(firstComponent + component.factIndex);
        }
        return available;
    }

    const CFG& cfg = m_analyses.cfg();
    if (location.block == cfg.root())
        return std::nullopt;
    for (const Component& component : components)
        available.set(firstComponent + component.factIndex);
    for (Edge edge : cfg.incomingEdges(location.block)) {
        if (edge.isHandlerEdge())
            return std::nullopt;
        IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
        std::optional<EdgeResult> result;
        if (incoming.kind == IncomingValue::Kind::EdgeResult)
            result = tryGetEdgeResult(m_function, incoming.result);

        std::optional<BitVector> heldBeforeEdge;
        for (const Component& component : components) {
            if (!available.get(firstComponent + component.factIndex))
                continue;
            const GuardFact& fact = facts.facts[component.factIndex];
            if (leavesUnspecified(edge, fact.subject))
                continue;
            bool isAvailable = false;
            if (result)
                isAvailable = result->kind == EdgeResultKind::Guard && (result->caseIndex || !hasWriterBetweenTestAndBranch(result->test, edge.from, m_killClasses[component.killClass].classBetweenTestAndBranch));
            else if (!componentsOf(incoming.argument).empty()) {
                if (!heldBeforeEdge) {
                    heldBeforeEdge = componentsHeldAt(incoming.argument, { edge.from, static_cast<uint32_t>(m_function.at(edge.from).values.size()) }, BehindTerminator::Yes, depth + 1);
                    if (!heldBeforeEdge)
                        return std::nullopt;
                    heldBeforeEdge->ensureSize(m_components.size());
                }
                isAvailable = isAvailableBeforeEdge(*heldBeforeEdge, edge, incoming.argument, fact);
            }
            if (!isAvailable)
                available.clear(firstComponent + component.factIndex);
        }
    }
    return available;
}

std::optional<BitVector> GuardAvailability::componentsHeldSinceDefinition(ValueID guard, ProgramPoint point) const
{
    return componentsHeldAt(guard, point, BehindTerminator::No, 0);
}

// A guard holds what it is given where it is defined, whatever held in front of that, and nothing else gives it anything.
std::optional<BitVector> GuardAvailability::componentsHeldAt(ValueID guard, ProgramPoint point, BehindTerminator behindTerminator, unsigned depth) const
{
    AOT_THRESHOLD(unsigned, maxDepth, 3);
    std::span<const Component> components = componentsOf(guard);
    if (components.empty() || depth > maxDepth)
        return std::nullopt;
    uint32_t guardIndex = *guardIndexOf(guard);
    if (m_solution.solvedGuards.get(guardIndex))
        return std::nullopt;
    // In a long function the writers have to be counted to see that nothing is given up. That is one walk, where solving asks each of them about each class.
    if (!canSolveForAnyWriters(guardIndex) && !canSolve(guard))
        return std::nullopt;

    const CFG& cfg = m_analyses.cfg();
    ValueLocation location = m_analyses.useLists().locationOf(guard);
    if (location.placement == ValuePlacement::Detached || !cfg.isReachable(point.block))
        return std::nullopt;

    if (m_solution.valuesWalkedSinceDefinition.isEmpty())
        m_solution.valuesWalkedSinceDefinition.fill(0, m_guards.size());
    uint64_t maxNumberOfValuesWalked = m_function.values.size();
    if (m_solution.valuesWalkedSinceDefinition[guardIndex] > maxNumberOfValuesWalked)
        return std::nullopt;

    std::optional<BitVector> proven = componentsProvenAtDefinition(guard, location, depth);
    if (!proven || proven->isEmpty())
        return proven;
    BitVector available = WTF::move(*proven);
    uint32_t firstComponent = m_firstComponent[guardIndex];

    LocationMasks dependedOn;
    for (const Component& component : components)
        dependedOn.merge(m_dependedLocations[m_killClasses[component.killClass].locations].masks);

    auto endProofs = [&](ProgramPoint writer, const EffectsOnEdges& effects) {
        if (!mayEndProofOver(effects.normal, dependedOn))
            return;
        if (effects.normal.maySuspend) {
            available.clearAll();
            return;
        }
        KillQuery query(m_analyses, writer, effects);
        for (uint32_t offset = 0; offset < components.size(); ++offset) {
            if (!available.get(firstComponent + offset))
                continue;
            const KillClass& killClass = m_killClasses[components[offset].killClass];
            WrittenLocations written = query.writtenOf(m_dependedLocations[killClass.locations], EdgeKind::Normal);
            if (query.mayEndProofAbout(killClass.subject, written))
                available.clear(firstComponent + offset);
        }
    };
    // What lies on a way to the point and ends a fact ends it for the point, wherever the other ways come from: the nearest writers are asked first, and most of them end everything.
    enum class Walk : uint8_t { GoesOn, IsOver, CostsTooMuch };
    auto walkThrough = [&](BlockID block, uint32_t begin, uint32_t end, bool includesTerminator) {
        uint64_t& numberOfValuesWalked = m_solution.valuesWalkedSinceDefinition[guardIndex];
        numberOfValuesWalked += end - begin + 1;
        if (numberOfValuesWalked > maxNumberOfValuesWalked)
            return Walk::CostsTooMuch;
        const BlockValues& values = m_function.at(block).values;
        if (includesTerminator)
            endProofs({ block, end }, m_analyses.effectsOfTerminator(block));
        for (uint32_t index = end; index-- > begin && !available.isEmpty();) {
            if (!isNeverWriter(m_function.at(values[index]).opcode))
                endProofs({ block, index }, m_analyses.effectsOf(values[index]));
        }
        return available.isEmpty() ? Walk::IsOver : Walk::GoesOn;
    };
    auto resultOf = [&](Walk walk) -> std::optional<BitVector> {
        if (walk == Walk::CostsTooMuch)
            return std::nullopt;
        return available;
    };

    bool isParameter = location.placement == ValuePlacement::Parameter;
    uint32_t firstIndexBehindDefinition = isParameter ? 0 : location.index + 1;
    bool includesTerminator = behindTerminator == BehindTerminator::Yes;
    if (location.block == point.block && (isParameter || location.index < point.index))
        return resultOf(walkThrough(point.block, firstIndexBehindDefinition, point.index, includesTerminator));

    if (Walk walk = walkThrough(point.block, 0, point.index, includesTerminator); walk != Walk::GoesOn)
        return resultOf(walk);
    BitVector isLeft(m_function.blocks.size());
    BitVector isEntered(m_function.blocks.size());
    Vector<BlockID, 8> enteredBlocks { point.block };
    isEntered.quickSet(point.block.index());
    while (!enteredBlocks.isEmpty()) {
        BlockID block = enteredBlocks.takeLast();
        if (block == cfg.root())
            return BitVector();
        for (Edge edge : cfg.incomingEdges(block)) {
            if (edge.isHandlerEdge())
                return std::nullopt;
            if (isLeft.quickSet(edge.from.index()))
                continue;
            uint32_t numberOfValues = m_function.at(edge.from).values.size();
            bool holdsDefinition = edge.from == location.block;
            if (Walk walk = walkThrough(edge.from, holdsDefinition ? firstIndexBehindDefinition : 0, numberOfValues, true); walk != Walk::GoesOn)
                return resultOf(walk);
            if (!holdsDefinition && !isEntered.quickSet(edge.from.index()))
                enteredBlocks.append(edge.from);
        }
    }
    return available;
}

std::optional<GuardState> GuardAvailability::stateSeenWithoutSolving(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint point) const
{
    std::optional<BitVector> available = componentsHeldSinceDefinition(guard, point);
    if (!available)
        return std::nullopt;
    GuardState state = stateOfSolvableGuardIn(*available, guard, &reliedOn, subject);
    if (m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]] {
        solveFor(guard);
        RELEASE_ASSERT(state == Cursor(*this, point).stateOf(guard, reliedOn, subject));
    }
    return state;
}

GuardState GuardAvailability::stateAt(ValueID guard, ProofSet reliedOn, ValueID subject, ProgramPoint point) const
{
    if (std::optional<GuardState> state = stateSeenWithoutSolving(guard, reliedOn, subject, point))
        return *state;
    solveFor(guard);
    return Cursor(*this, point).stateOf(guard, reliedOn, subject);
}

GuardState GuardAvailability::stateAt(ValueID guard, const Value& user, ProgramPoint point) const
{
    solveFor(guard);
    return Cursor(*this, point).stateOf(guard, user);
}

GuardState GuardAvailability::stateOnEdge(ValueID guard, Edge edge) const
{
    solveFor(guard);
    const Solution& solution = m_solution;
    BlockID block = edge.from;
    if (!solution.solvedBlocks.get(block.index()))
        return GuardState::Unknown;

    const BlockValues& values = m_function.at(block).values;
    BitVector available = solution.availableAtHead[block.index()];
    BitVector availableOnThrow = available;
    for (uint32_t index = 0; index <= values.size(); ++index) {
        bool isTerminator = index == values.size();
        bool mayThrow = edge.isHandlerEdge() && (isTerminator ? m_analyses.effectsOfTerminator(block) : m_analyses.effectsOf(values[index])).mayThrow();
        if (mayThrow)
            availableOnThrow.filter(available);
        applyKills(available, block, index);
        if (!isTerminator)
            applyDefinitions(available, solution.solvedGuards, block, index);
        if (mayThrow)
            availableOnThrow.filter(available);
    }

    switch (edge.kind) {
    case EdgeKind::Normal:
        return stateIn(available, guard, nullptr);
    case EdgeKind::Handler:
        availableOnThrow.exclude(solution.killedOnThrow[block.index()]);
        return stateIn(availableOnThrow, guard, nullptr);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

GuardAvailability::Cursor::Cursor(const GuardAvailability& availability, BlockID block)
    : m_availability(availability)
    , m_block(block)
{
    availability.solveNamedGuards();
    replay();
}

GuardAvailability::Cursor::Cursor(const GuardAvailability& availability, ProgramPoint point)
    : m_availability(availability)
    , m_block(point.block)
    , m_index(point.index)
{
    RELEASE_ASSERT(point.index <= availability.m_function.at(point.block).values.size());
    replay();
}

bool GuardAvailability::Cursor::isInSolvedBlock() const
{
    return m_availability.m_solution.solvedBlocks.get(m_block.index());
}

void GuardAvailability::Cursor::replay() const
{
    const Solution& solution = m_availability.m_solution;
    m_numberOfSolvesSeen = solution.numberOfSolves;
    if (!isInSolvedBlock())
        return;

    m_available = solution.availableAtHead[m_block.index()];
    for (uint32_t index = 0; index < m_index; ++index) {
        m_availability.applyKills(m_available, m_block, index);
        m_availability.applyDefinitions(m_available, solution.solvedGuards, m_block, index);
    }
}

void GuardAvailability::Cursor::solveFor(ValueID guard) const
{
    m_availability.solveFor(guard);
    if (m_numberOfSolvesSeen != m_availability.m_solution.numberOfSolves)
        replay();
}

void GuardAvailability::Cursor::advanceTo(uint32_t index)
{
    RELEASE_ASSERT(index >= m_index && index <= m_availability.m_function.at(m_block).values.size());
    if (!isInSolvedBlock()) {
        m_index = index;
        return;
    }
    for (; m_index < index; ++m_index) {
        m_availability.applyKills(m_available, m_block, m_index);
        m_availability.applyDefinitions(m_available, m_availability.m_solution.solvedGuards, m_block, m_index);
    }
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard) const
{
    solveFor(guard);
    if (!isInSolvedBlock())
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, nullptr);
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, ProofSet reliedOn) const
{
    return stateOf(guard, reliedOn, ValueID());
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, ProofSet reliedOn, ValueID subject) const
{
    solveFor(guard);
    if (!isInSolvedBlock())
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, &reliedOn, subject);
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, const Value& user) const
{
    solveFor(guard);
    if (!isInSolvedBlock())
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, user);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
