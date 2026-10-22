/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEscapeAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include "AOTLiveness.h"
#include "AOTPointsToSolver.h"
#include "AOTTypeRelations.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

using Containment = std::pair<ObjectID, ObjectID>;

static constexpr EscapeReasons limitReasons { EscapeReason::LimitReached };

static Vector<Containment> collectLeakedContainments(const PointsToResults& results)
{
    Vector<Containment> leaked;
    for (const SolvedContents& entry : results.contents) {
        if (entry.isLeaked)
            leaked.append({ entry.container, entry.object });
    }
    std::ranges::sort(leaked);
    removeRepeatedElements(leaked);
    return leaked;
}

static const ValueSet* createdAliasSetOf(const PointsToResults& results, const SolvedUse& use)
{
    if (!use.flowsTo)
        return nullptr;
    const ValueSet* set = results.setOf(use.flowsTo);
    RELEASE_ASSERT(set);
    return set;
}

static bool isOpenContainer(const TrackedObject& object)
{
    return object.kind == ObjectKind::EntryParameter || !object.reasons.isEmpty();
}

static bool escapesThroughContainer(const EscapeAnalysis& escape, const PointsToResults& results, const SolvedUse& use, std::span<const Containment> leaked)
{
    if (!use.retainedBy)
        return false;
    const ValueSet* containers = results.setOf(use.retainedBy);
    if (!containers)
        return false;
    for (size_t index : containers->objects) {
        ObjectID container(index);
        if (isOpenContainer(escape.at(container)) || std::ranges::binary_search(leaked, Containment { container, use.object }))
            return true;
    }
    return false;
}

static bool escapesThroughCall(const PointsToResults& results, const SolvedUse& use)
{
    return use.uses.containsAny(callingUses) && results.objects[use.object.index()].reasons.containsAny(callReasons);
}

bool ObjectSet::overlaps(ObjectSet other) const
{
    for (ObjectID object : *this) {
        if (other.contains(object))
            return true;
    }
    return false;
}

std::optional<ObjectID> ObjectSet::onlyMember() const
{
    Iterator first = begin();
    if (first == end())
        return std::nullopt;
    ObjectID object = *first;
    if (++first != end())
        return std::nullopt;
    return object;
}

namespace {

struct SolveOfEntryParameters {
    uint64_t numberOfObjects { 0 };
    uint64_t maxWork { 0 };
    uint64_t maxHistoryBits { 0 };
};

} // anonymous namespace

// What the solve and the history of a body cost at most whose only objects are entry parameters that the caller knows. Nothing where the body may have another object.
//
// The solver counts an object that arrives at a value, once for the two, and an application that meets an entry of a container, once for the two.
// An entry is an object in a slot of a container. An operand of a value makes it, and the operand and the container name the slot: at most the square of the objects for each operand.
// An application to the contents of an entry parameter is for all its slots. It is the one for contents that leak, or has the uses that the summary of some operand names: at most one more than there are operands.
static std::optional<SolveOfEntryParameters> describeSolveOfEntryParameters(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    // The promise that a coroutine body is given is an entry parameter that nobody else has seen.
    if (function.role == FunctionRole::CoroutineBody)
        return std::nullopt;
    if (analyses.opcodesOfValues().intersects(opcodesThatMayDefineObjects()))
        return std::nullopt;

    CheckedUint64 numberOfOperands = 0;
    for (const Block& block : function.blocks) {
        if (resultMayBeFresh(block.terminator.kind))
            return std::nullopt;
        for (ValueID id : block.values)
            numberOfOperands += function.at(id).numberOfOperands();
        numberOfOperands += block.terminator.numberOfOperands();
    }

    CheckedUint64 numberOfObjects = 0;
    for (ValueID parameter : function.at(entryBlock).parameters) {
        if (mayBeReference(analyses.program().typeOf(function, parameter)))
            ++numberOfObjects;
    }

    CheckedUint64 maxArrivals = numberOfObjects * function.values.size();
    CheckedUint64 maxEntries = numberOfObjects * numberOfObjects * numberOfOperands;
    CheckedUint64 maxWork = maxArrivals + maxEntries * (numberOfOperands + uint64_t { 1 });
    CheckedUint64 maxHistoryBits = numberOfObjects * function.blocks.size();
    if (maxWork.hasOverflowed() || maxHistoryBits.hasOverflowed())
        return std::nullopt;
    return SolveOfEntryParameters { numberOfObjects.value(), maxWork.value(), maxHistoryBits.value() };
}

EscapeAnalysis::EscapeAnalysis(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_program(analyses.program())
    , m_function(analyses.function())
{
    const BuildContext& build = analyses.context().build;
    std::optional<SolveOfEntryParameters> solveOfEntryParameters = describeSolveOfEntryParameters(analyses);
    m_tracksOnlyEntryParameters = solveOfEntryParameters
        && solveOfEntryParameters->maxWork <= build.limit(LimitID::MaxEscapeAnalysisWork)
        && solveOfEntryParameters->maxHistoryBits <= build.limit(LimitID::MaxUseEffectHistoryBits);
    // None of them is an allocation, so there is nothing to compute.
    m_hasLiveInstances = m_tracksOnlyEntryParameters;

    if (!build.options().verifiesAfterEveryPass) [[likely]] {
        if (!m_tracksOnlyEntryParameters)
            solve();
        return;
    }

    uint64_t work = solve();
    if (!solveOfEntryParameters)
        return;
    RELEASE_ASSERT(work <= solveOfEntryParameters->maxWork);
    RELEASE_ASSERT(m_objects.size() == solveOfEntryParameters->numberOfObjects);
    for (const TrackedObject& object : m_objects)
        RELEASE_ASSERT(object.kind == ObjectKind::EntryParameter);
    RELEASE_ASSERT(!m_tracksOnlyEntryParameters || !m_gaveUp);
}

uint64_t EscapeAnalysis::solve()
{
    RELEASE_ASSERT(!m_isSolved);
    m_isSolved = true;

    m_analyses.didBuild(FunctionAnalysis::Escape);
    EscapeAnalysisInputs inputs = m_analyses.inputsOfEscapeAnalysis();
    PointsToResults results = solvePointsTo(inputs);
    RELEASE_ASSERT(results.setOfValue.size() == inputs.useLists.numberOfValues());
    m_gaveUp = results.gaveUp;

    createObjects(results);
    computeContents(results);
    Vector<EscapeReasons> useReasons = computeReasons(results);
    propagateEscapeToContents();
    computeUses(results, inputs.useLists, useReasons.span());
    computeStates();
    takeSets(results);
    if (m_tracksOnlyEntryParameters)
        m_liveInstances.fill(LiveInstances::Unknown, m_objects.size());
    m_remembersAnswersFromTypes = inputs.remembersAnswersFromTypes;
    m_answersFromTypes = WTF::move(results.answersFromTypes);
    rememberTypes();
    return results.work;
}

void EscapeAnalysis::rememberTypes()
{
    if (!m_remembersAnswersFromTypes)
        return;
    m_typesWhenAnswered = Vector<TypesOfValue>(m_function.values.size(), [&](size_t index) {
        const Value& value = m_function.values[index];
        return TypesOfValue { value.type, value.expected() };
    });
}

bool EscapeAnalysis::didRetypeValues()
{
    // Whether it may wait depends on the types of the entry parameters, and it has nothing to lose.
    if (!m_isSolved || !m_remembersAnswersFromTypes)
        return false;

    using Question = AnswerFromTypes::Question;

    size_t numberOfValues = m_function.values.size();
    RELEASE_ASSERT(m_typesWhenAnswered.size() == numberOfValues);
    BitVector isRetyped(numberOfValues);
    for (size_t index = 0; index < numberOfValues; ++index) {
        const Value& value = m_function.values[index];
        TypesOfValue types { value.type, value.expected() };
        if (m_typesWhenAnswered[index] == types)
            continue;
        m_typesWhenAnswered[index] = types;
        isRetyped.quickSet(index);
    }
    if (isRetyped.isEmpty())
        return true;

    auto hasRetypedOperand = [&](const Instruction& instruction) {
        bool result = false;
        instruction.forEachOperand([&](ValueID operand, uint32_t) {
            result |= isRetyped.quickGet(operand.index());
        });
        return result;
    };
    // What an answer about a value is computed from is what the stamp of the value in the effects cache is computed from.
    auto mayBeDifferentNow = [&](const AnswerFromTypes& answer) {
        switch (answer.question) {
        case Question::IsReference:
        case Question::MayHoldReference:
        case Question::RefersToOperand:
            return isRetyped.quickGet(answer.subject);
        case Question::HasFreshResult:
        case Question::MayRunOtherCode:
        case Question::SummaryOfOperand: {
            const Value& value = m_function.values[answer.subject];
            return isRetyped.quickGet(answer.subject) || effectFactsOf(value.opcode).inputs == EffectInputs::Function || hasRetypedOperand(value);
        }
        case Question::MakesObjectOnEdge:
        case Question::SummaryOfTerminatorOperand:
            return hasRetypedOperand(m_function.blocks[answer.subject].terminator);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    EscapeAnalysisInputs inputs = m_analyses.inputsOfEscapeAnalysis();
    for (const AnswerFromTypes& answer : m_answersFromTypes) {
        if (mayBeDifferentNow(answer) && answerFromTypes(inputs, answer.question, answer.subject, answer.operandIndex) != answer)
            return false;
    }
    return true;
}

void EscapeAnalysis::didRenumber(const Renumbering& renumbering)
{
    if (!m_isSolved)
        return;

    auto renumberValue = [&](ValueID& value) {
        if (!value)
            return;
        value = renumbering.newIDOf(value);
        RELEASE_ASSERT(value);
    };
    auto renumberUse = [&](Use& use) {
        use.block = renumbering.newIDOf(use.block);
        RELEASE_ASSERT(use.block);
        renumberValue(use.user);
    };

    for (TrackedObject& object : m_objects) {
        renumberValue(object.definition);
        if (object.firstEscape)
            renumberUse(*object.firstEscape);
    }
    for (ObjectUse& objectUse : m_uses) {
        renumberUse(objectUse.use);
        renumberValue(objectUse.retainedBy);
    }
    for (UnlistedUse& unlisted : m_unlistedUses) {
        renumberUse(unlisted.use.use);
        renumberValue(unlisted.use.retainedBy);
    }

    Vector<ValueSetID> setOfValue(FillWith { }, m_function.values.size(), ValueSetID());
    for (size_t index = 0; index < m_setOfValue.size(); ++index) {
        if (!m_setOfValue[index])
            continue;
        ValueID value = renumbering.newIDOf(ValueID(index));
        RELEASE_ASSERT(value);
        setOfValue[value.index()] = m_setOfValue[index];
    }
    m_setOfValue = WTF::move(setOfValue);

    // They are listed by their numbers.
    m_aliases.clear();
    m_firstAlias.clear();

    for (AnswerFromTypes& answer : m_answersFromTypes) {
        switch (answer.question) {
        case AnswerFromTypes::Question::IsReference:
        case AnswerFromTypes::Question::MayHoldReference:
        case AnswerFromTypes::Question::RefersToOperand:
        case AnswerFromTypes::Question::HasFreshResult:
        case AnswerFromTypes::Question::MayRunOtherCode:
        case AnswerFromTypes::Question::SummaryOfOperand: {
            ValueID value = renumbering.newIDOf(ValueID(answer.subject));
            RELEASE_ASSERT(value);
            answer.subject = value.index();
            break;
        }
        case AnswerFromTypes::Question::MakesObjectOnEdge:
        case AnswerFromTypes::Question::SummaryOfTerminatorOperand: {
            BlockID block = renumbering.newIDOf(BlockID(answer.subject));
            RELEASE_ASSERT(block);
            answer.subject = block.index();
            break;
        }
        }
    }
    rememberTypes();
}

void EscapeAnalysis::createObjects(const PointsToResults& results)
{
    m_objects.reserveInitialCapacity(results.objects.size());
    for (const SolvedObject& solved : results.objects) {
        TrackedObject object;
        object.definition = solved.definition;
        object.kind = solved.kind;
        m_objects.append(object);
    }
}

void EscapeAnalysis::takeSets(PointsToResults& results)
{
    m_setOfValue = WTF::move(results.setOfValue);
    m_sets.reserveInitialCapacity(results.sets.size());
    BitVector allocations(m_objects.size());
    Vector<size_t, 8> entryParameters;
    for (size_t index = 0; index < m_objects.size(); ++index) {
        if (m_objects[index].kind == ObjectKind::Allocation)
            allocations.quickSet(index);
        else
            entryParameters.append(index);
    }
    // Once for each set, not once for each question about two values: the sets of a row of a thousand branches hold hundreds of objects, and each read is held against each store.
    for (ValueSet& set : results.sets) {
        StoredSet stored { WTF::move(set.objects), { }, set.mayBeUntracked, false };
        stored.holdsEntryParameter = std::ranges::any_of(entryParameters, [&](size_t index) {
            return stored.objects.get(index);
        });
        if (stored.holdsEntryParameter) {
            stored.allocationsBesideEntryParameters = stored.objects;
            stored.allocationsBesideEntryParameters.filter(allocations);
        }
        m_sets.append(WTF::move(stored));
    }
}

void EscapeAnalysis::computeAliases() const
{
    groupByIndex(numberOfObjects(), m_firstAlias, m_aliases, [&](const auto& visit) {
        for (size_t index = 0; index < m_setOfValue.size(); ++index) {
            ValueID value(index);
            for (ObjectID object : pointsTo(value).objects)
                visit(object.index(), value);
        }
    });
}

void EscapeAnalysis::computeContents(const PointsToResults& results)
{
    Vector<Containment> containments;
    containments.reserveInitialCapacity(results.contents.size());
    for (const SolvedContents& entry : results.contents)
        containments.append({ entry.container, entry.object });
    std::ranges::sort(containments);
    removeRepeatedElements(containments);

    groupByIndex(numberOfObjects(), m_firstContent, m_contents, [&](const auto& visit) {
        for (auto [container, object] : containments)
            visit(container.index(), object);
    });
    groupByIndex(numberOfObjects(), m_firstContainer, m_containers, [&](const auto& visit) {
        for (auto [container, object] : containments)
            visit(object.index(), container);
    });
}

Vector<EscapeReasons> EscapeAnalysis::computeReasons(const PointsToResults& results)
{
    for (unsigned index = 0; index < m_objects.size(); ++index) {
        m_objects[index].reasons = results.objects[index].reasons;
        if (results.gaveUp)
            m_objects[index].reasons.add(limitReasons);
    }

    Vector<EscapeReasons> useReasons;
    useReasons.reserveInitialCapacity(results.uses.size());
    for (const SolvedUse& use : results.uses) {
        EscapeReasons reasons = use.reasons;
        if (use.retainedBy) {
            const ValueSet* containers = results.setOf(use.retainedBy);
            if (!containers || containers->mayBeUntracked)
                reasons.add(EscapeReason::StoredInUnknownMemory);
        }
        m_objects[use.object.index()].reasons.add(reasons);
        useReasons.append(reasons);
    }
    return useReasons;
}

void EscapeAnalysis::propagateEscapeToContents()
{
    Vector<ObjectID, 16> worklist;
    for (unsigned index = 0; index < m_objects.size(); ++index) {
        if (isOpenContainer(m_objects[index]))
            worklist.append(ObjectID(index));
    }
    while (!worklist.isEmpty()) {
        for (ObjectID content : contentsOf(worklist.takeLast())) {
            TrackedObject& object = m_objects[content.index()];
            if (object.reasons.contains(EscapeReason::StoredInEscapingObject))
                continue;
            bool wasOpen = isOpenContainer(object);
            object.reasons.add(EscapeReason::StoredInEscapingObject);
            if (!wasOpen)
                worklist.append(content);
        }
    }
}

void EscapeAnalysis::computeUses(PointsToResults& results, const UseLists& useLists, std::span<const EscapeReasons> useReasons)
{
    for (unsigned index = 0; index < m_objects.size(); ++index)
        m_objects[index].uses = results.objects[index].appliedUses;

    Vector<Containment> leaked = collectLeakedContainments(results);
    Vector<UseOrder, 16> orderOfFirstEscape(m_objects.size());
    m_firstUse.fill(0, numberOfObjects() + 1);
    m_unlistedUses.reserveInitialCapacity(results.uses.size());
    for (size_t index = 0; index < results.uses.size(); ++index) {
        const SolvedUse& solved = results.uses[index];
        bool createsFollowedAlias = !!createdAliasSetOf(results, solved);
        if (createsFollowedAlias && solved.uses.containsOnly({ OperandUse::Inspects, OperandUse::Returns }))
            continue;

        bool isEscape = !useReasons[index].isEmpty()
            || escapesThroughContainer(*this, results, solved, leaked.span())
            || escapesThroughCall(results, solved);
        TrackedObject& object = m_objects[solved.object.index()];
        ++m_firstUse[solved.object.index() + 1];
        object.uses.add(createsFollowedAlias ? solved.uses - OperandUse::Returns : solved.uses);
        m_unlistedUses.append(UnlistedUse { solved.object, ObjectUse { solved.use, solved.uses, solved.retainedBy, isEscape, solved.isContentsStore, solved.readsContents } });
        if (!isEscape)
            continue;
        UseOrder order = useLists.orderOf(solved.use);
        if (!object.firstEscape || order < orderOfFirstEscape[solved.object.index()]) {
            object.firstEscape = solved.use;
            orderOfFirstEscape[solved.object.index()] = order;
        }
    }
    for (unsigned object = 0; object < numberOfObjects(); ++object)
        m_firstUse[object + 1] += m_firstUse[object];
}

void EscapeAnalysis::listUses() const
{
    RELEASE_ASSERT(m_isSolved);
    m_hasListedUses = true;
    const UseLists& useLists = m_analyses.useLists();

    struct ListPosition {
        UseOrder order;
        uint32_t unlistedUse { 0 };
    };
    Vector<ListPosition> positions(m_firstUse.last());
    Vector<uint32_t, 32> nextPosition(m_firstUse.span().first(m_objects.size()));
    for (size_t index = 0; index < m_unlistedUses.size(); ++index)
        positions[nextPosition[m_unlistedUses[index].object.index()]++] = ListPosition { useLists.orderOf(m_unlistedUses[index].use.use), static_cast<uint32_t>(index) };
    for (unsigned object = 0; object < m_objects.size(); ++object) {
        auto positionsOfObject = positions.mutableSpan().subspan(m_firstUse[object], m_firstUse[object + 1] - m_firstUse[object]);
        if (!std::ranges::is_sorted(positionsOfObject, { }, &ListPosition::order))
            std::ranges::sort(positionsOfObject, { }, &ListPosition::order);
    }
    m_uses.reserveInitialCapacity(positions.size());
    for (const ListPosition& position : positions)
        m_uses.append(m_unlistedUses[position.unlistedUse].use);
    m_unlistedUses.clear();
}

void EscapeAnalysis::computeStates()
{
    for (TrackedObject& object : m_objects) {
        if (object.reasons.containsAny(limitReasons)) {
            object.state = EscapeState::Unknown;
            object.uses = OperandUses::all();
        } else if (object.reasons.isEmpty())
            object.state = EscapeState::DoesNotEscape;
        else
            object.state = EscapeState::Escapes;
    }
}

class WatchedObjects {
public:
    explicit WatchedObjects(const EscapeAnalysis& escape)
        : m_escape(escape)
        , m_isWatched(escape.numberOfObjects())
    {
    }

    std::span<const ObjectID> objects() const { return m_objects.span(); }

    void addWithContainers(ObjectID object)
    {
        size_t next = m_objects.size();
        add(object);
        for (; next < m_objects.size(); ++next) {
            for (ObjectID container : m_escape.containersOf(m_objects[next])) {
                if (!isOpenContainer(m_escape.at(container)))
                    add(container);
            }
        }
    }

    void clear()
    {
        for (ObjectID object : m_objects)
            m_isWatched.quickClear(object.index());
        m_objects.shrink(0);
    }

private:
    void add(ObjectID object)
    {
        if (!m_isWatched.quickSet(object.index()))
            m_objects.append(object);
    }

    const EscapeAnalysis& m_escape;
    BitVector m_isWatched;
    Vector<ObjectID, 16> m_objects;
};

struct AllocationInCycle {
    ObjectID object;
    BlockID block;
    uint32_t index { 0 };
    bool isMadeOnEdge { false };
};

static void watch(WatchedObjects& watchedObjects, const EscapeAnalysis& escape, const AllocationInCycle& allocation)
{
    watchedObjects.addWithContainers(allocation.object);
    if (!allocation.isMadeOnEdge)
        return;
    // The call on the other edge makes another object, the two meet in a parameter, and whoever asks about that value asks about both.
    for (size_t other : escape.objectsSharingValuesWith(allocation.object))
        watchedObjects.addWithContainers(ObjectID(other));
}

BitVector EscapeAnalysis::objectsSharingValuesWith(ObjectID object) const
{
    BitVector objects(m_objects.size());
    objects.quickSet(object.index());
    for (ValueID alias : aliasesOf(object)) {
        for (ObjectID other : pointsTo(alias).objects)
            objects.quickSet(other.index());
    }
    return objects;
}

// An object gets to a value that defines none and is no parameter of a block from an operand of the value: the value may be the operand, refer to it, or be read out of it.
// So where all that a value is computed from begins at entry parameters and at operations without operands, it names an entry parameter, or what stands in one, or what stands in that.
bool mayNameUnescapedAllocation(const Function& function, ValueID value)
{
    constexpr size_t maxDefinitions = 32;

    const OpcodeSet& definingOpcodes = opcodesThatMayDefineObjects();
    std::span<const ValueID> entryParameters = function.at(entryBlock).parameters.span();
    Vector<ValueID, maxDefinitions> definitions { value };
    for (size_t next = 0; next < definitions.size(); ++next) {
        const Value& definition = function.at(definitions[next]);
        if (definition.opcode == Opcode::Parameter) {
            if (std::ranges::find(entryParameters, definitions[next]) == entryParameters.end())
                return true;
            continue;
        }
        if (definingOpcodes.contains(definition.opcode))
            return true;
        definition.forEachOperand([&](ValueID operand, uint32_t) {
            definitions.appendIfNotContains(operand);
        });
        if (definitions.size() > maxDefinitions)
            return true;
    }
    return false;
}

Vector<BlockID, 2> blocksWithResultIn(const Function& function, const CFG& cfg, BlockID blockOfParameter, ValueID parameter)
{
    Vector<BlockID, 2> blocks;
    for (BlockID from : cfg.predecessors(blockOfParameter)) {
        forEachEdgeResult(function, from, [&](const EdgeResult& result, ValueID filled) {
            if (result.kind == EdgeResultKind::DeclaredResult && filled == parameter)
                blocks.appendIfNotContains(from);
        });
    }
    return blocks;
}

static BitVector collectWatchedValues(const EscapeAnalysis& escape, std::span<const AllocationInCycle> allocations, unsigned numberOfValues)
{
    WatchedObjects watchedObjects(escape);
    for (const AllocationInCycle& allocation : allocations)
        watch(watchedObjects, escape, allocation);

    BitVector watchedValues(numberOfValues);
    for (ObjectID object : watchedObjects.objects()) {
        for (ValueID alias : escape.aliasesOf(object))
            watchedValues.quickSet(alias.index());
    }
    return watchedValues;
}

static LiveInstances computeLiveInstancesAt(const EscapeAnalysis& escape, const Liveness::LocalCalc& liveBeforeAllocation, const WatchedObjects& watchedObjects)
{
    for (ObjectID object : watchedObjects.objects()) {
        for (ValueID alias : escape.aliasesOf(object)) {
            if (liveBeforeAllocation.isLive(alias))
                return LiveInstances::Unknown;
        }
    }
    return LiveInstances::AtMostOne;
}

void EscapeAnalysis::computeLiveInstances(const CFG& cfg, const UseLists& useLists, const ScopedLambda<const BlockCycles&()>& cycles)
{
    RELEASE_ASSERT(!m_hasLiveInstances);
    m_hasLiveInstances = true;
    m_liveInstances.fill(LiveInstances::Unknown, m_objects.size());
    if (m_gaveUp)
        return;

    const BlockCycles* blockCycles = nullptr;
    auto isInCycle = [&](BlockID block) {
        if (!cfg.hasCycle())
            return false;
        if (!blockCycles)
            blockCycles = &cycles();
        return blockCycles->isInCycle(block);
    };

    Vector<AllocationInCycle> candidates;
    for (unsigned index = 0; index < m_objects.size(); ++index) {
        const TrackedObject& object = m_objects[index];
        if (object.kind != ObjectKind::Allocation)
            continue;
        ValueLocation location = useLists.locationOf(object.definition);
        // An object made on an edge is known by the parameter it is passed to, which is no point in a block. It is made by the terminators in front of the edges.
        if (location.placement == ValuePlacement::Parameter) {
            // Outside of cycles too: an object that shares a value with this one may be older than the terminator.
            if (m_objectsRenewedAtMakers.isEmpty())
                m_objectsRenewedAtMakers.grow(m_objects.size());
            m_objectsRenewedAtMakers[index] = objectsSharingValuesWith(ObjectID(index));
            for (BlockID maker : blocksWithResultIn(m_function, cfg, location.block, object.definition))
                candidates.append(AllocationInCycle { ObjectID(index), maker, static_cast<uint32_t>(m_function.at(maker).values.size()), true });
            continue;
        }
        RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
        if (!isInCycle(location.block))
            m_liveInstances[index] = LiveInstances::AtMostOne;
        else
            candidates.append(AllocationInCycle { ObjectID(index), location.block, location.index });
    }
    if (candidates.isEmpty())
        return;

    std::ranges::sort(candidates, { }, [](const AllocationInCycle& candidate) {
        return std::pair { candidate.block, candidate.index };
    });

    BitVector watchedValues = collectWatchedValues(*this, candidates.span(), useLists.numberOfValues());
    LivenessOptions options;
    options.trackedValues = &watchedValues;
    Liveness liveness(m_function, cfg, useLists, options);

    WatchedObjects watchedObjects(*this);
    for (size_t remaining = candidates.size(); remaining;) {
        BlockID block = candidates[remaining - 1].block;
        Liveness::LocalCalc live(liveness, block);
        uint32_t index = m_function.at(block).values.size();
        for (; remaining && candidates[remaining - 1].block == block; --remaining) {
            const AllocationInCycle& candidate = candidates[remaining - 1];
            RELEASE_ASSERT(candidate.index <= index);
            while (index > candidate.index)
                live.execute(--index);
            auto liveInstancesOf = [&](ObjectID object) {
                watchedObjects.addWithContainers(object);
                LiveInstances liveInstances = computeLiveInstancesAt(*this, live, watchedObjects);
                watchedObjects.clear();
                return liveInstances;
            };
            if (!candidate.isMadeOnEdge) {
                m_liveInstances[candidate.object.index()] = liveInstancesOf(candidate.object);
                continue;
            }
            BitVector& renewed = m_objectsRenewedAtMakers[candidate.object.index()];
            for (size_t other : BitVector(renewed)) {
                if (liveInstancesOf(ObjectID(other)) == LiveInstances::Unknown)
                    renewed.clear(other);
            }
        }
    }
}

std::optional<ObjectID> EscapeAnalysis::objectDefinedBy(ValueID value) const
{
    for (ObjectID object : pointsTo(value).objects) {
        if (m_objects[object.index()].definition == value)
            return object;
    }
    return std::nullopt;
}

bool EscapeAnalysis::isExactUse(ObjectID object, const Use& use) const
{
    if (use.kind != Use::Kind::Operand)
        return false;
    const Value& user = m_function.at(use.user);
    if (singleObject(user.operand(use.operandIndex)) != object)
        return false;

    bool isExact = true;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex == use.operandIndex)
            return;
        isExact &= !pointsTo(operand).objects.contains(object);
    });
    return isExact;
}

EscapeAnalysis::AllocationSet EscapeAnalysis::allocationSetOf(ValueID value) const
{
    ValueSetID id = m_setOfValue[value.index()];
    if (!id)
        return { m_noObjects, true };
    const StoredSet& set = m_sets[id.index()];
    return { set.holdsEntryParameter ? set.allocationsBesideEntryParameters : set.objects, set.mayBeUntracked || set.holdsEntryParameter };
}

AliasResult EscapeAnalysis::alias(ValueID a, ValueID b) const
{
    return aliasWhere(a, b, [&](ObjectID object) {
        return liveInstancesOf(object) == LiveInstances::AtMostOne;
    });
}

bool EscapeAnalysis::mayAlias(ValueID a, ValueID b) const
{
    AliasResult result = aliasWhere(a, b, [](ObjectID) {
        return false;
    });
    return result != AliasResult::NoAlias;
}

AliasResult EscapeAnalysis::aliasWhere(ValueID a, ValueID b, NOESCAPE const Invocable<bool(ObjectID)> auto& hasAtMostOneLiveInstance) const
{
    RELEASE_ASSERT(a.index() < m_function.values.size() && b.index() < m_function.values.size());
    if (sameObjectThroughCasts(m_function, a) == sameObjectThroughCasts(m_function, b))
        return AliasResult::MustAlias;
    if (!mayBeSameObject(m_program.typeOf(m_function, a), m_program.typeOf(m_function, b), m_program.types))
        return AliasResult::NoAlias;
    if (!m_isSolved)
        return AliasResult::MayAlias;

    AliasResult result = aliasAmongAllocations(a, b, hasAtMostOneLiveInstance);
    RELEASE_ASSERT(!m_tracksOnlyEntryParameters || result == AliasResult::MayAlias);
    return result;
}

AliasResult EscapeAnalysis::aliasAmongAllocations(ValueID a, ValueID b, NOESCAPE const Invocable<bool(ObjectID)> auto& hasAtMostOneLiveInstance) const
{
    AllocationSet first = allocationSetOf(a);
    AllocationSet second = allocationSetOf(b);
    ObjectSet firstObjects(first.objects);
    ObjectSet secondObjects(second.objects);
    bool areBothTracked = !first.mayBeUntracked && !second.mayBeUntracked;
    if (firstObjects.overlaps(secondObjects)) {
        std::optional<ObjectID> single = firstObjects.onlyMember();
        bool areSameSingleObject = areBothTracked && single && secondObjects.onlyMember() == single;
        if (areSameSingleObject && hasAtMostOneLiveInstance(*single))
            return AliasResult::MustAlias;
        return AliasResult::MayAlias;
    }
    if (areBothTracked)
        return AliasResult::NoAlias;
    if (first.mayBeUntracked && second.mayBeUntracked)
        return AliasResult::MayAlias;

    for (ObjectID object : first.mayBeUntracked ? secondObjects : firstObjects) {
        if (at(object).state != EscapeState::DoesNotEscape)
            return AliasResult::MayAlias;
    }
    return AliasResult::NoAlias;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
