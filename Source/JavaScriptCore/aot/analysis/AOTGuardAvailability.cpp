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
#include <algorithm>
#include <functional>
#include <utility>
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

using WriteExtent = GuardWriter::EndedProofs;

struct WrittenLocations {
    WriteExtent extent { WriteExtent::None };
    EdgeKind edge { EdgeKind::Normal };
    LocationMasks written;
    bool isLeftByPropertiesBesideFields { false };
    std::optional<bool> mayWriteBeyondOperands;
    std::optional<Vector<ValueID, 4>> operandsWrittenThrough;
};

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

    WrittenLocations writtenOf(const DependedLocations& depended, EdgeKind edge)
    {
        WrittenLocations result;
        result.edge = edge;
        result.isLeftByPropertiesBesideFields = depended.isLeftByPropertiesBesideFields;

        const Effects& effects = effectsOn(m_effects, edge);
        if (effects.maySuspend) {
            result.extent = WriteExtent::All;
            return result;
        }
        if (!effects.writes.overlaps(depended.masks))
            return result;
        if (m_writer && leavesSlotsAsTested(m_analyses.program(), m_function, m_function.at(m_writer), effects, depended.slotState))
            return result;
        if (m_writer && leavesArrayStorageAsTested(m_analyses.program(), m_function, m_function.at(m_writer), effects, depended.arrayStorage))
            return result;

        for (size_t index = 0; index < depended.locations.size(); ++index) {
            const MemoryLocation& location = depended.locations[index];
            uint64_t mask = depended.masksOfLocations[index];
            if (!mayWrite(effects, location, mask))
                continue;
            if (!perObjectHeaps.contains(location.heap())) {
                result.extent = WriteExtent::All;
                return result;
            }
            result.written.add(location.heap(), mask);
        }
        if (!result.written.isEmpty())
            result.extent = WriteExtent::AboutSomeObjects;
        return result;
    }

    bool mayEndProofAbout(ValueID subject, WrittenLocations& written)
    {
        switch (written.extent) {
        case WriteExtent::None:
            return false;
        case WriteExtent::All:
            return true;
        case WriteExtent::AboutSomeObjects:
            break;
        }
        if (!subject)
            return true;

        if (!reachesObjects(written))
            return false;
        if (written.isLeftByPropertiesBesideFields && m_writer && definesPropertyBesideFields(m_analyses.program(), m_function.at(m_writer), effectsOn(m_effects, written.edge), m_function.at(subject)))
            return false;
        if (written.isLeftByPropertiesBesideFields && m_writer && definesDataPropertyAsLayoutDeclares(m_analyses, m_function.at(m_writer), m_function.at(subject)))
            return false;

        std::optional<bool> mayEndProof = mayEndProofJudgedByDefinitions(subject, written);
        if (!mayEndProof)
            return mayEndProofJudgedByObjects(subject, written);
        if (m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]]
            RELEASE_ASSERT(*mayEndProof == mayEndProofJudgedByObjects(subject, written));
        return *mayEndProof;
    }

    bool mayEndProofAbout(ValueID subject, bool subjectComesFromOutside, WrittenLocations& written)
    {
        m_lastSubjectJudgedByDefinition = subject;
        m_lastSubjectComesFromOutside = subjectComesFromOutside;
        return mayEndProofAbout(subject, written);
    }

    // Code that runs other code writes beyond its operands whatever it writes, and is no store that leaves a slot or a storage as it was tested: writtenOf() reads nothing of it but its effects on the edge.
    bool isJudgedByEffectsAlone(EdgeKind edge)
    {
        if (!m_writer || !m_effects.normal.mayRunOtherCode())
            return false;
        const Effects& effects = effectsOn(m_effects, edge);
        if (!effects.mayRunOtherCode() || effects.writes.isSubsetOf(declaredBoundOf(Opcode::PutSlotOrAdd).writes))
            return false;
        if (!m_exactAccess)
            m_exactAccess = memoryAccessOf(m_function, m_function.at(m_writer));
        return m_exactAccess->kind == MemoryAccess::Kind::NotExact;
    }

    std::optional<BitVector> allocationsWhoseProofsEnd(WrittenLocations& written)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        if (written.extent != WriteExtent::AboutSomeObjects || written.isLeftByPropertiesBesideFields || escape.tracksOnlyEntryParameters() || !mayWriteBeyondOperands(written))
            return std::nullopt;
        if (!m_allocationsNotConfined) {
            if (!m_history)
                m_history.emplace(m_analyses.escapeHistory(), m_point);
            if (m_history->isUnknown())
                return std::nullopt;
            m_allocationsNotConfined.emplace();
            m_history->addObjectsThatMayHaveOccurredTo(*m_allocationsNotConfined);
            m_allocationsNotConfined->merge(objectsInsideOperands());
            for (size_t object : operandObjects()) {
                if (escapesAtWriter(ObjectID(object)))
                    m_allocationsNotConfined->set(object);
            }
        }
        BitVector allocations = *m_allocationsNotConfined;
        for (ValueID operand : operandsWrittenThrough(written)) {
            if (mayBeMadeHere(operand))
                escape.pointsTo(operand).objects.addTo(allocations);
        }
        return allocations;
    }

    std::optional<Vector<ObjectID, 4>> allocationsWithinReach()
    {
        if (!m_beyondOperands)
            m_beyondOperands = m_writer ? effectsBeyondOperands(m_analyses.effectContext(), m_function, m_function.at(m_writer), m_effects) : m_effects;
        if (!m_beyondOperands->onAnyEdge().writes.restrictedTo(perObjectHeaps).isEmpty())
            return std::nullopt;
        Vector<ObjectID, 4> objects;
        for (const WrittenOperand& operand : writtenOperands()) {
            std::optional<Vector<ObjectID, 4>> objectsOfOperand = GuardWriter::allocationsOf(m_analyses, operand.operand);
            if (!objectsOfOperand)
                return std::nullopt;
            objects.appendVector(*objectsOfOperand);
        }
        return objects;
    }

    bool reachesObjects(WrittenLocations& written)
    {
        return mayWriteBeyondOperands(written) || !operandsWrittenThrough(written).empty();
    }

    bool mayWriteBeyondOperands(WrittenLocations& written)
    {
        if (!written.mayWriteBeyondOperands) {
            if (!m_beyondOperands)
                m_beyondOperands = m_writer ? effectsBeyondOperands(m_analyses.effectContext(), m_function, m_function.at(m_writer), m_effects) : m_effects;
            written.mayWriteBeyondOperands = effectsOn(*m_beyondOperands, written.edge).writes.overlaps(written.written);
        }
        return *written.mayWriteBeyondOperands;
    }

private:
    struct WrittenOperand {
        ValueID operand;
        LocationMasks writes;
    };

    const Terminator& terminator() const LIFETIME_BOUND { return m_function.at(m_point.block).terminator; }

    bool mayWrite(const Effects& effects, const MemoryLocation& location, uint64_t maskOfLocation)
    {
        if (!m_exactAccess)
            m_exactAccess = m_writer ? memoryAccessOf(m_function, m_function.at(m_writer)) : MemoryAccess { };
        switch (m_exactAccess->kind) {
        case MemoryAccess::Kind::NotExact:
            return effects.writes.overlaps(location.heap(), maskOfLocation);
        case MemoryAccess::Kind::Read:
            return false;
        case MemoryAccess::Kind::Write:
            return overlapOf(m_analyses.program(), m_exactAccess->location, location) != LocationOverlap::None;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::optional<bool> mayEndProofJudgedByDefinitions(ValueID subject, WrittenLocations& written)
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

    bool mayEndProofJudgedByObjects(ValueID subject, WrittenLocations& written)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        if (!isConfined(subject)) {
            bool wasThereAtEntry = namesOnlyObjectsOfKind(ObjectKind::EntryParameter, subject);
            return mayWriteBeyondOperands(written) || std::ranges::any_of(operandsWrittenThrough(written), [&](ValueID operand) {
                return escape.mayAlias(operand, subject) && !(wasThereAtEntry && namesOnlyObjectsOfKind(ObjectKind::Allocation, operand)) && !wasThereBeforeWhatIsNamedBy(operand, subject);
            });
        }

        ObjectSet objects = escape.pointsTo(subject).objects;
        return std::ranges::any_of(operandsWrittenThrough(written), [&](ValueID operand) {
            return escape.pointsTo(operand).objects.overlaps(objects) && mayBeMadeHere(operand);
        });
    }

    // What was computed once, ahead of every allocation that the operand names, is none of those objects. Only what judges finished code is told: told to the passes it moves code.
    bool wasThereBeforeWhatIsNamedBy(ValueID operand, ValueID subject)
    {
        if (!m_analyses.context().onlyJudges() || !namesOnlyObjectsOfKind(ObjectKind::Allocation, operand))
            return false;
        ValueLocation computed = m_analyses.useLists().locationOf(m_analyses.identityOf(subject));
        if (computed.placement != ValuePlacement::InBlock || m_analyses.isInCycle(computed.block))
            return false;
        const EscapeAnalysis& escape = m_analyses.escape();
        for (ObjectID object : escape.pointsTo(operand).objects) {
            ValueLocation allocation = m_analyses.useLists().locationOf(escape.at(object).definition);
            if (allocation.placement != ValuePlacement::InBlock)
                return false;
            if (allocation.block == computed.block ? allocation.index <= computed.index : !m_analyses.dominators().dominates(computed.block, allocation.block))
                return false;
        }
        return true;
    }

    bool mayBeMadeHere(ValueID operand)
    {
        return !m_analyses.context().onlyJudges() || !m_writer || m_analyses.escape().pointsTo(operand).objects.isEmpty() || !namesOnlyWhatCodeOutsideReaches(m_analyses, operand);
    }

    bool namesOnlyObjectsOfKind(ObjectKind kind, ValueID value)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        PointsToSet set = escape.pointsTo(value);
        if (set.mayBeUntracked)
            return false;
        for (ObjectID object : set.objects) {
            if (escape.at(object).kind != kind)
                return false;
        }
        return true;
    }

    std::span<const ValueID> operandsWrittenThrough(WrittenLocations& written)
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

    bool isConfined(ValueID subject)
    {
        if (m_lastSubject == subject)
            return m_isLastSubjectConfined;

        const EscapeAnalysis& escape = m_analyses.escape();
        bool isConfinedSubject = false;
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

    bool isConfined(ObjectID object)
    {
        const TrackedObject& tracked = m_analyses.escape().at(object);
        bool cameFromOutside = tracked.kind == ObjectKind::EntryParameter && tracked.definition != findFreshEntryParameter(m_function, m_analyses.cfg());
        if (cameFromOutside && !m_analyses.context().build.options().verifiesAfterEveryPass)
            return false;
        if (!m_history)
            m_history.emplace(m_analyses.escapeHistory(), m_point);
        if (m_history->occurrenceOf(object) != Occurrence::HasNotOccurred)
            return false;
        RELEASE_ASSERT(!cameFromOutside);
        return !escapesAtWriter(object) && !objectsInsideOperands().get(object.index());
    }

    bool escapesAtWriter(ObjectID object)
    {
        if (!operandObjects().get(object.index()))
            return false;
        auto isUseByWriter = [&](const Use& use) {
            if (m_writer)
                return use.kind == Use::Kind::Operand && use.user == m_writer;
            return use.kind == Use::Kind::TerminatorOperand && use.block == m_point.block;
        };
        // The uses of an object stand in the order of the function, so those at one place stand together.
        const UseLists& useLists = m_analyses.useLists();
        const CFG& cfg = m_analyses.cfg();
        auto placeOf = [&](ProgramPoint point) {
            return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
        };
        auto placeOfWriter = placeOf(m_writer ? useLists.pointOf(m_writer) : pointBeforeTerminatorOf(m_function, m_point.block));
        ObjectUses uses = m_analyses.escape().usesOf(object);
        AOT_THRESHOLD(size_t, fewestUsesSearched, 16);
        size_t first = 0;
        for (size_t end = uses.size() < fewestUsesSearched ? 0 : uses.size(); first < end;) {
            size_t middle = first + (end - first) / 2;
            if (placeOf(useLists.pointOf(uses[middle].use)) < placeOfWriter)
                first = middle + 1;
            else
                end = middle;
        }
        bool escapes = false;
        for (size_t index = first; !escapes && index < uses.size() && (uses.size() < fewestUsesSearched || placeOf(useLists.pointOf(uses[index].use)) == placeOfWriter); ++index) {
            const ObjectUse& objectUse = uses[index];
            escapes = objectUse.isEscape && isUseByWriter(objectUse.use) && (!m_writer || mayBeMadeHere(m_function.at(m_writer).operand(objectUse.use.operandIndex)));
        }
        return escapes;
    }

    std::span<const WrittenOperand> writtenOperands()
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

    const BitVector& operandObjects()
    {
        if (!m_operandObjects) {
            m_operandObjects.emplace();
            const EscapeAnalysis& escape = m_analyses.escape();
            auto addObjectsOf = [&](ValueID operand) {
                if (mayBeMadeHere(operand))
                    escape.pointsTo(operand).objects.addTo(*m_operandObjects);
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

    const BitVector& objectsInsideOperands()
    {
        if (!m_objectsInsideOperands) {
            m_objectsInsideOperands.emplace();
            const EscapeAnalysis& escape = m_analyses.escape();
            Vector<ObjectID, 8> worklist;
            for (size_t object : operandObjects())
                worklist.append(ObjectID(object));
            while (!worklist.isEmpty()) {
                ObjectID container = worklist.takeLast();
                // A store into an open container is an escape, and the history says whether it has happened yet.
                if (isOpenContainer(escape.at(container)))
                    continue;
                for (ObjectID content : escape.contentsOf(container)) {
                    if (!m_objectsInsideOperands->set(content.index()))
                        worklist.append(content);
                }
            }
        }
        return *m_objectsInsideOperands;
    }

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
    std::optional<BitVector> m_allocationsNotConfined;
    std::optional<UseEffectHistory::AtPoint> m_history;
    ValueID m_lastSubject;
    bool m_isLastSubjectConfined { false };
    ValueID m_lastSubjectJudgedByDefinition;
    bool m_lastSubjectComesFromOutside { false };
};

} // anonymous namespace

std::optional<GuardDependencies> dependenciesOfGuardOf(FunctionAnalyses& analyses, const Value& test)
{
    if (!provesForStretch(provesOf(test.opcode)))
        return std::nullopt;

    const Program& program = analyses.program();
    GuardDependencies dependencies(program, locationsReadBy(test));
    dependencies.slotState = slotStateTestedBy(test);
    dependencies.isLeftByPropertiesBesideFields = areLeftByPropertiesBesideFields(ProofSet { provesOf(test.opcode) }, dependencies.locations.span());
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
    if (written.extent != EndedProofs::AboutSomeObjects)
        return { written.extent, 0 };
    m_query->written.append(WTF::move(written));
    return { EndedProofs::AboutSomeObjects, static_cast<uint32_t>(m_query->written.size() - 1) };
}

std::optional<BitVector> GuardWriter::allocationsWhoseProofsEnd(const WrittenDependencies& dependencies)
{
    if (!m_query || dependencies.endedProofs != EndedProofs::AboutSomeObjects)
        return std::nullopt;
    return m_query->query.allocationsWhoseProofsEnd(m_query->written[dependencies.index]);
}

std::optional<Vector<ObjectID, 4>> GuardWriter::allocationsWithinReach()
{
    if (!m_query)
        return std::nullopt;
    return m_query->query.allocationsWithinReach();
}

// Two values with one root are one object whatever their sets say, so the root's members are among the answer, and there is at least one.
std::optional<Vector<ObjectID, 4>> GuardWriter::allocationsOf(FunctionAnalyses& analyses, ValueID value)
{
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectSet> own = escape.allocationsIfNothingElse(value);
    std::optional<ObjectSet> ofRoot = escape.allocationsIfNothingElse(analyses.sameObjectThroughCasts(value).valueInFrontOfFacts());
    if (!own || !ofRoot || ofRoot->isEmpty())
        return std::nullopt;
    Vector<ObjectID, 4> objects;
    for (ObjectID object : *ofRoot)
        objects.append(object);
    for (ObjectID object : *own) {
        if (!ofRoot->contains(object))
            objects.append(object);
    }
    return objects;
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
    describeGuards();
    if (!exceedsWorkLimit())
        return;

    reportWorkLimit();
    if (m_guards.size() > m_numberOfNamedGuards) {
        for (ValueID guard : m_guards.span().subspan(m_numberOfNamedGuards))
            m_guardIndexOfValue[guard.index()] = std::nullopt;
        m_guards.shrink(m_numberOfNamedGuards);
        describeGuards();
        if (!exceedsWorkLimit())
            return;
    }
    leaveOutGuardsInRarerBlocks();
}

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
    AOT::stableSortInLine(mostFrequentFirst, std::ranges::greater { }, [&](uint32_t guardIndex) {
        BlockID block = useLists.blockOf(m_guards[guardIndex]);
        return block ? frequencies.frequencyOf(block).predicted() : 0;
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
    numberComponents();
    m_solution.gaveUp = m_components.isEmpty();
}

void GuardAvailability::describeGuards()
{
    m_facts.clear();
    computeFacts();
    numberComponents();
}

void GuardAvailability::numberComponents()
{
    m_components.clear();
    m_firstComponent.clear();
    m_dependedLocations.clear();
    m_killClasses.clear();
    m_killClassesBySubject.clear();
    m_numberOfKillClassesOfNamedGuards = 0;

    HashedIndex locationsIndex;
    auto internLocations = [&](const Vector<MemoryLocation, 2>& locations, TestedSlotState slotState, ArrayStorage arrayStorage, bool isLeftByPropertiesBesideFields) -> uint32_t {
        uint32_t hash = computeHash(hashOf(locations.span()), slotState, arrayStorage, isLeftByPropertiesBesideFields);
        std::optional<uint32_t> existing = locationsIndex.find(hash, [&](uint32_t position) {
            const DependedLocations& known = m_dependedLocations[position];
            return known.locations == locations && known.slotState == slotState && known.arrayStorage == arrayStorage && known.isLeftByPropertiesBesideFields == isLeftByPropertiesBesideFields;
        });
        if (existing)
            return *existing;

        uint32_t position = m_dependedLocations.size();
        m_dependedLocations.append(DependedLocations(m_analyses.program(), Vector<MemoryLocation, 2>(locations)));
        m_dependedLocations.last().slotState = slotState;
        m_dependedLocations.last().arrayStorage = arrayStorage;
        m_dependedLocations.last().isLeftByPropertiesBesideFields = isLeftByPropertiesBesideFields;
        locationsIndex.add(hash, position);
        return position;
    };

    HashedIndex killClassIndex;
    auto internKillClassWith = [&](const GuardFact& fact, TestedSlotState slotState, ArrayStorage arrayStorage) -> uint32_t {
        uint32_t locations = internLocations(fact.dependsOn, slotState, arrayStorage, areLeftByPropertiesBesideFields(fact.proofs, fact.dependsOn.span()));
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
    };
    auto internKillClass = [&](const GuardFact& fact) {
        TestedSlotState slotState = slotStateProvenBy(m_function, fact);
        ArrayStorage arrayStorage = arrayStorageProvenBy(m_analyses.program(), m_function, fact);
        uint32_t killClass = internKillClassWith(fact, slotState, arrayStorage);
        if (slotState != TestedSlotState::None || arrayStorage != ArrayStorage::Other) {
            uint32_t classBetweenTestAndBranch = internKillClassWith(fact, TestedSlotState::None, ArrayStorage::Other);
            m_killClasses[killClass].classBetweenTestAndBranch = classBetweenTestAndBranch;
        }
        return killClass;
    };

    m_firstComponent.reserveInitialCapacity(m_guards.size() + 1);
    for (uint32_t guardIndex = 0; guardIndex < m_guards.size(); ++guardIndex) {
        m_firstComponent.append(m_components.size());
        const GuardFacts& facts = m_facts[guardIndex];
        if (!facts.isKnown() || m_guardsLeftOut.get(guardIndex))
            continue;
        for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
            uint32_t killClass = internKillClass(facts.facts[factIndex]);
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
    AOT::sortInLine(m_killClassesBySubject, { }, [&](uint32_t killClass) {
        return std::pair { m_killClasses[killClass].subject, killClass };
    });
}

bool GuardAvailability::exceedsWorkLimit(uint64_t work) const
{
    return work > m_analyses.context().build.limit(LimitID::MaxGuardAvailabilityWork);
}

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
            Opcode opcode = m_function.at(values[index]).opcode;
            if (isNeverWriter(opcode))
                continue;
            addWriter(index, m_analyses.effectsOf(values[index]));
        }
        addWriter(values.size(), m_analyses.effectsOfTerminator(block));
    }

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

    bool isNamed = guardIndex < m_numberOfNamedGuards;
    uint32_t firstKillClass = isNamed ? 0 : m_numberOfKillClassesOfNamedGuards;
    uint32_t endKillClass = isNamed ? m_numberOfKillClassesOfNamedGuards : m_killClasses.size();
    uint64_t numberOfKillClassesLeft = 0;
    for (uint32_t killClass = firstKillClass; killClass < endKillClass; ++killClass)
        numberOfKillClassesLeft += !solution.killClassesWithKills.get(killClass);
    uint64_t costOfWalks = static_cast<uint64_t>(solution.numberOfSeparateSolves + 1) * m_function.values.size();
    if (costOfWalks > solution.writers.size() * numberOfKillClassesLeft) {
        if (isNamed)
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

void GuardAvailability::collectKills(const BitVector& wantedKillClasses) const
{
    Solution& solution = m_solution;
    BitVector added = wantedKillClasses;
    added.exclude(solution.killClassesWithKills);
    if (added.isEmpty())
        return;
    solution.killClassesWithKills.merge(added);

    Vector<uint32_t, 16> killClasses;
    LocationMasks dependedOn;
    for (uint32_t killClass : m_killClassesBySubject) {
        if (!added.get(killClass))
            continue;
        killClasses.append(killClass);
        dependedOn.merge(m_dependedLocations[m_killClasses[killClass].locations].masks);
    }

    struct ClassesOfLocations {
        uint32_t locations { 0 };
        bool hasSubjects { false };
        BitVector all;
        BitVector withoutSubject;
        BitVector aboutObjectsFromOutside;
        BitVector aboutObjectsNeverConfined;
    };
    struct ClassAboutSubject {
        uint32_t killClass { 0 };
        uint32_t group { 0 };
        ValueID subject;
        bool comesFromOutside { false };
    };
    Vector<ClassesOfLocations, 8> groups;
    // A write beyond the operands of its writer ends every proof about an object from outside, and about one that is confined nowhere. Only the others are asked about at each such writer.
    Vector<ClassAboutSubject, 16> classesAboutSubjectsFromOutside;
    Vector<ClassAboutSubject, 16> classesAboutSubjectsNeverConfined;
    Vector<ClassAboutSubject, 16> classesAboutOtherSubjects;
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
            (lastSubjectComesFromOutside ? classesAboutSubjectsFromOutside : classesAboutOtherSubjects).append(ClassAboutSubject { index, group, lastSubject, lastSubjectComesFromOutside });
        }
    }

    // Where KillQuery::isConfined() says no at every writer, and no writer defines a property beside the fields of the subject or as its layout declares: those speak of an object that the subject itself allocates.
    bool didFindSubjectsNeverConfined = false;
    auto findSubjectsNeverConfined = [&] {
        if (std::exchange(didFindSubjectsNeverConfined, true) || classesAboutOtherSubjects.isEmpty())
            return;
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ValueID> freshEntryParameter = findFreshEntryParameter(m_function, m_analyses.cfg());
        auto isNeverConfined = [&](ValueID subject) {
            const Value& definition = m_function.at(subject);
            if (Value::opcodesWithLayout.contains(definition.opcode) && definition.layout() && allocatesOwnLayout(m_analyses.program(), definition))
                return false;
            if (escape.tracksOnlyEntryParameters())
                return true;
            PointsToSet set = escape.pointsTo(subject);
            if (set.mayBeUntracked)
                return true;
            for (ObjectID object : set.objects) {
                const TrackedObject& tracked = escape.at(object);
                if (tracked.kind == ObjectKind::EntryParameter && tracked.definition != freshEntryParameter)
                    return true;
            }
            return false;
        };
        ValueID lastSubject;
        bool lastSubjectIsNeverConfined = false;
        classesAboutOtherSubjects.removeAllMatching([&](const ClassAboutSubject& entry) {
            if (entry.subject != lastSubject) {
                lastSubject = entry.subject;
                lastSubjectIsNeverConfined = isNeverConfined(lastSubject);
            }
            if (!lastSubjectIsNeverConfined)
                return false;
            groups[entry.group].aboutObjectsNeverConfined.set(entry.killClass);
            classesAboutSubjectsNeverConfined.append(entry);
            return true;
        });
    };

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

    // A large body has thousands of calls and a few dozen different effects.
    struct AnswerForEffects {
        Effects effects;
        // But for the proofs about the other subjects.
        BitVector killed;
        bool hangsOnObjects { false };
        Vector<WrittenLocations, 8> writtenOfGroups;
    };
    Vector<AnswerForEffects, 8> answersForEffects;

    Vector<WrittenLocations, 8> writtenOfGroups;
    uint64_t numberOfQuestions = 0;
    auto findKilledClasses = [&](KillQuery& query, const Effects& effects, EdgeKind edge) -> BitVector {
        ++numberOfQuestions;
        if (effects.maySuspend)
            return added;

        BitVector killed;
        auto askAbout = [&](std::span<const ClassAboutSubject> entries, NOESCAPE const Invocable<bool(WrittenLocations&)> auto& isDecided) {
            numberOfQuestions += entries.size();
            for (const ClassAboutSubject& entry : entries) {
                WrittenLocations& written = writtenOfGroups[entry.group];
                if (written.extent != WriteExtent::AboutSomeObjects || isDecided(written))
                    continue;
                if (query.mayEndProofAbout(entry.subject, entry.comesFromOutside, written))
                    killed.set(entry.killClass);
            }
        };
        auto askAboutOtherSubjects = [&] {
            askAbout(classesAboutOtherSubjects.span(), [](WrittenLocations&) {
                return false;
            });
        };
        auto checked = [&](BitVector&& result) {
            if (m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]]
                RELEASE_ASSERT(result == findKilledClassesOneByOne(query, edge));
            return WTF::move(result);
        };

        const bool isJudgedByEffectsAlone = query.isJudgedByEffectsAlone(edge);
        if (isJudgedByEffectsAlone) {
            for (const AnswerForEffects& answer : answersForEffects) {
                if (answer.effects != effects)
                    continue;
                killed = answer.killed;
                if (answer.hangsOnObjects && !classesAboutOtherSubjects.isEmpty()) {
                    writtenOfGroups = answer.writtenOfGroups;
                    for (WrittenLocations& written : writtenOfGroups)
                        written.edge = edge;
                    askAboutOtherSubjects();
                }
                return checked(WTF::move(killed));
            }
        }

        bool hangsOnObjects = false;
        bool asksAboutObjectsThatWritesBeyondOperandsReach = false;
        writtenOfGroups.shrink(0);
        numberOfQuestions += groups.size();
        for (const ClassesOfLocations& classes : groups) {
            writtenOfGroups.append(query.writtenOf(m_dependedLocations[classes.locations], edge));
            WrittenLocations& written = writtenOfGroups.last();
            switch (written.extent) {
            case WriteExtent::None:
                break;
            case WriteExtent::All:
                killed.merge(classes.all);
                break;
            case WriteExtent::AboutSomeObjects:
                killed.merge(classes.withoutSubject);
                if (!classes.hasSubjects || !query.reachesObjects(written)) {
                    written.extent = WriteExtent::None;
                    break;
                }
                findSubjectsNeverConfined();
                if (query.mayWriteBeyondOperands(written)) {
                    killed.merge(classes.aboutObjectsFromOutside);
                    killed.merge(classes.aboutObjectsNeverConfined);
                } else
                    asksAboutObjectsThatWritesBeyondOperandsReach = true;
                hangsOnObjects = true;
                break;
            }
        }
        if (isJudgedByEffectsAlone) {
            RELEASE_ASSERT(!asksAboutObjectsThatWritesBeyondOperandsReach);
            answersForEffects.append(AnswerForEffects { effects, killed, hangsOnObjects, writtenOfGroups });
        }
        if (asksAboutObjectsThatWritesBeyondOperandsReach) {
            auto isDecided = [&](WrittenLocations& written) {
                return query.mayWriteBeyondOperands(written);
            };
            askAbout(classesAboutSubjectsFromOutside.span(), isDecided);
            askAbout(classesAboutSubjectsNeverConfined.span(), isDecided);
        }
        if (hangsOnObjects)
            askAboutOtherSubjects();
        return checked(WTF::move(killed));
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
            if (!killed.isEmpty())
                solution.killedOnThrow[block.index()].merge(solution.killSets[ensureKillSet(WTF::move(killed))].components);
        }
    }
    m_analyses.context().build.statistics().count(StatisticID::QuestionsAboutWhoEndsGuards, numberOfQuestions);
}

uint32_t GuardAvailability::ensureKillSet(BitVector&& killClasses) const
{
    Solution& solution = m_solution;
    uint32_t hash = killClasses.hash();
    std::optional<uint32_t> killSet = solution.killSetIndex.find(hash, [&](uint32_t position) {
        return solution.killSets[position].killClasses == killClasses;
    });
    if (!killSet) {
        BitVector components;
        for (size_t killClass : killClasses)
            components.merge(m_killClasses[killClass].components);
        killSet = solution.killSets.size();
        solution.killSets.append(KillSet { WTF::move(killClasses), WTF::move(components) });
        solution.killSetIndex.add(hash, *killSet);
    }
    return *killSet;
}

void GuardAvailability::addKill(BlockID block, uint32_t index, BitVector&& killClasses) const
{
    Kill kill { index, ensureKillSet(WTF::move(killClasses)) };
    Vector<Kill>& kills = m_solution.killsOfBlock[block.index()];
    if (kills.isEmpty() || kills.last().index <= index) {
        kills.append(kill);
        return;
    }
    size_t position = std::ranges::upper_bound(kills, index, { }, &Kill::index) - kills.begin();
    kills.insert(position, kill);
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
            return isAvailableIn(available, operand, facts.facts[component.factIndex], facts.facts[component.factIndex].subject, facts.facts[component.factIndex].position);
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
                isAvailable = result ? isProvenOnEdge(edge, *result, component, KillsOfSolve::Yes) : isAvailableBeforeEdge(available, edge, incoming.argument, fact);
            assignments.append({ firstComponent + component.factIndex, isAvailable });
        }
    }

    for (auto [bit, isAvailable] : assignments)
        available.quickSet(bit, isAvailable);
}

bool GuardAvailability::isProvenOnEdge(Edge edge, const EdgeResult& result, const Component& component, KillsOfSolve killsOfSolve) const
{
    if (result.kind != EdgeResultKind::Guard)
        return false;
    if (result.caseIndex)
        return true;
    ValueLocation location = m_analyses.useLists().locationOf(result.test);
    if (location.placement != ValuePlacement::InBlock)
        return false;

    uint32_t killClass = m_killClasses[component.killClass].classBetweenTestAndBranch;
    auto mayEndIn = [&](BlockID block, uint32_t firstIndex) {
        if (killsOfSolve == KillsOfSolve::No)
            return hasWriterThatEndsProofsOfClass(block, firstIndex, killClass);
        std::span<const Kill> kills = killsIn(block);
        std::ranges::subrange killsFromIndex(std::ranges::lower_bound(kills, firstIndex, { }, &Kill::index), kills.end());
        return std::ranges::any_of(killsFromIndex, [&](const Kill& kill) {
            return ends(kill, killClass);
        });
    };
    const CFG& cfg = m_analyses.cfg();
    for (BlockID block = edge.from; block != location.block;) {
        std::optional<Edge> onlyWayIn = cfg.singleIncomingEdge(block);
        if (!onlyWayIn || onlyWayIn->isHandlerEdge() || mayEndIn(block, 0))
            return false;
        block = onlyWayIn->from;
    }
    return !mayEndIn(location.block, location.index + 1);
}

bool GuardAvailability::isAvailableBeforeEdge(const BitVector& available, Edge edge, ValueID guard, const GuardFact& factAfterEdge) const
{
    std::optional<ValueID> subject = valueBeforeEdge(factAfterEdge.subject, edge);
    std::optional<ValueID> position = valueBeforeEdge(factAfterEdge.position, edge);
    return subject && position && isAvailableIn(available, guard, factAfterEdge, *subject, *position);
}

bool GuardAvailability::isAvailableIn(const BitVector& available, ValueID guard, const GuardFact& fact, ValueID subject, ValueID position) const
{
    if (componentsOf(guard).empty())
        return false;

    uint32_t firstComponent = firstComponentOf(guard);
    const GuardFacts& facts = factsOf(guard);
    for (uint32_t factIndex = 0; factIndex < facts.facts.size(); ++factIndex) {
        const GuardFact& candidate = facts.facts[factIndex];
        if (available.quickGet(firstComponent + factIndex) && candidate.subject == subject && isSamePosition(candidate.position, position) && isAsStrongAs(candidate, fact))
            return true;
    }
    return false;
}

uint32_t GuardAvailability::firstComponentOf(ValueID guard) const
{
    std::optional<uint32_t> guardIndex = guardIndexOf(guard);
    RELEASE_ASSERT(guardIndex);
    return m_firstComponent[*guardIndex];
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
    auto placeOf = [](const Writer& listed) {
        return std::pair { listed.blockOrder, listed.index };
    };
    if (!std::ranges::binary_search(m_solution.writers, std::pair { m_analyses.cfg().reversePostOrderIndex(writer.block), writer.index }, { }, placeOf))
        return false;
    BitVector killClasses;
    for (const Component& component : componentsOf(guard))
        killClasses.set(component.killClass);
    collectKills(killClasses);

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
    return cursorAt(point).stateOf(guard);
}

bool GuardAvailability::canSolveForAnyWriters(uint32_t guardIndex) const
{
    if (m_solution.isPrepared)
        return canSolve(m_guards[guardIndex]);
    if (m_solution.gaveUp || m_guardsLeftOut.get(guardIndex))
        return false;
    uint64_t mostWriters = m_function.values.size() + m_function.blocks.size();
    return !exceedsWorkLimit(mostWriters * m_killClasses.size());
}

bool GuardAvailability::hasWriterThatEndsProofsOfClass(BlockID block, uint32_t firstIndex, uint32_t killClassIndex) const
{
    const KillClass& killClass = m_killClasses[killClassIndex];
    const DependedLocations& locations = m_dependedLocations[killClass.locations];
    auto endsProofs = [&](uint32_t index, const EffectsOnEdges& effects) {
        if (!mayEndProofOver(effects.normal, locations.masks))
            return false;
        if (effects.normal.maySuspend)
            return true;
        KillQuery query(m_analyses, { block, index }, effects);
        WrittenLocations written = query.writtenOf(locations, EdgeKind::Normal);
        return query.mayEndProofAbout(killClass.subject, written);
    };
    const BlockValues& values = m_function.at(block).values;
    for (uint32_t index = firstIndex; index < values.size(); ++index) {
        if (!isNeverWriter(m_function.at(values[index]).opcode) && endsProofs(index, m_analyses.effectsOf(values[index])))
            return true;
    }
    return endsProofs(values.size(), m_analyses.effectsOfTerminator(block));
}

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
                return isAvailableIn(heldByOperands, operand, facts.facts[component.factIndex], facts.facts[component.factIndex].subject, facts.facts[component.factIndex].position);
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
                isAvailable = isProvenOnEdge(edge, *result, component, KillsOfSolve::No);
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

std::optional<BitVector> GuardAvailability::componentsHeldAt(ValueID guard, ProgramPoint point, BehindTerminator behindTerminator, unsigned depth) const
{
    AOT_THRESHOLD(unsigned, maxDepth, 3);
    std::span<const Component> components = componentsOf(guard);
    if (components.empty() || depth > maxDepth)
        return std::nullopt;
    uint32_t guardIndex = *guardIndexOf(guard);
    if (m_solution.solvedGuards.get(guardIndex))
        return std::nullopt;
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
    std::optional<BitVector> available = componentsHeldAt(guard, point, BehindTerminator::No, 0);
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
    return cursorAt(point).stateOf(guard, reliedOn, subject);
}

GuardState GuardAvailability::stateAt(ValueID guard, const Value& user, ProgramPoint point) const
{
    solveFor(guard);
    return cursorAt(point).stateOf(guard, user);
}

const GuardAvailability::Cursor& GuardAvailability::cursorAt(ProgramPoint point) const
{
    std::optional<Cursor>& cursor = m_cursorOfLastQuestion;
    if (cursor && cursor->m_block == point.block && cursor->m_index <= point.index && cursor->m_numberOfSolvesSeen == m_solution.numberOfSolves)
        cursor->advanceTo(point.index);
    else
        cursor.emplace(Cursor(*this, point));
    return *cursor;
}

void GuardAvailability::walk(BitVector& available, BlockID block, uint32_t firstIndex, uint32_t endIndex) const
{
    std::span<const Kill> kills = killsIn(block);
    auto kill = std::ranges::lower_bound(kills, firstIndex, { }, &Kill::index);
    for (uint32_t index = firstIndex; index < endIndex; ++index) {
        for (; kill != kills.end() && kill->index == index; ++kill)
            available.exclude(m_solution.killSets[kill->killSet].components);
        applyDefinitions(available, m_solution.solvedGuards, block, index);
    }
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

void GuardAvailability::Cursor::replay() const
{
    const Solution& solution = m_availability.m_solution;
    m_numberOfSolvesSeen = solution.numberOfSolves;
    if (!isInSolvedBlock())
        return;

    m_available = solution.availableAtHead[m_block.index()];
    m_availability.walk(m_available, m_block, 0, m_index);
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
    if (isInSolvedBlock())
        m_availability.walk(m_available, m_block, m_index, index);
    m_index = index;
}

GuardState GuardAvailability::Cursor::stateOf(ValueID guard, const ProofSet* reliedOn, ValueID subject) const
{
    solveFor(guard);
    if (!isInSolvedBlock())
        return GuardState::Unknown;
    return m_availability.stateIn(m_available, guard, reliedOn, subject);
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
