/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEscapeAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGrouping.h"
#include "AOTLiveness.h"
#include "AOTPointsToSolver.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeRelations.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>

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
    AOT::sortInLine(leaked);
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

static bool escapesThroughContainer(const EscapeAnalysis& escape, const PointsToResults& results, const SolvedUse& use, ObjectID object, std::span<const Containment> leaked)
{
    if (!use.retainedBy)
        return false;
    const ValueSet* containers = results.setOf(use.retainedBy);
    if (!containers)
        return false;
    for (ObjectID container : ObjectSet(containers->objects)) {
        if (isOpenContainer(escape.at(container)) || std::ranges::binary_search(leaked, Containment { container, object }))
            return true;
    }
    return false;
}

static bool escapesThroughCall(const PointsToResults& results, const SolvedUse& use, ObjectID object)
{
    return use.uses.containsAny(callingUses) && results.objects[object.index()].reasons.containsAny(callReasons);
}

void SparseObjectSet::add(std::span<const Entry> others, SparseObjectSet& added)
{
    size_t position = 0;
    for (const Entry& other : others) {
        position = positionOf(other.index, position);
        if (position == m_entries.size() || m_entries[position].index != other.index) {
            m_entries.insert(position, other);
            added.add(other);
            continue;
        }
        Word missing = other.word & ~m_entries[position].word;
        if (!missing)
            continue;
        m_entries[position].word |= missing;
        added.add({ other.index, missing });
    }
}

void SparseObjectSet::add(const Entry& entry)
{
    size_t position = !m_entries.isEmpty() && m_entries.last().index < entry.index ? m_entries.size() : positionOf(entry.index, 0);
    if (position == m_entries.size() || m_entries[position].index != entry.index)
        m_entries.insert(position, entry);
    else
        m_entries[position].word |= entry.word;
}

void SparseObjectSet::remove(const SparseObjectSet& others)
{
    size_t position = 0;
    m_entries.removeAllMatching([&](Entry& entry) {
        position = others.positionOf(entry.index, position);
        if (position < others.m_entries.size() && others.m_entries[position].index == entry.index)
            entry.word &= ~others.m_entries[position].word;
        return !entry.word;
    });
}

size_t ObjectSet::size() const
{
    size_t size = 0;
    for (const SparseObjectSet::Entry& entry : m_words.entries())
        size += std::popcount(entry.word);
    return size;
}

bool SparseObjectSet::overlaps(const SparseObjectSet& other) const
{
    // One object is held against all that an array holds, once for each read and store.
    if (m_entries.size() > other.m_entries.size())
        return other.overlaps(*this);
    size_t position = 0;
    for (const Entry& entry : m_entries) {
        position = other.positionOf(entry.index, position);
        if (position == other.m_entries.size())
            return false;
        if (other.m_entries[position].index == entry.index && (other.m_entries[position].word & entry.word))
            return true;
    }
    return false;
}

std::optional<ObjectID> ObjectSet::onlyMember() const
{
    std::span<const SparseObjectSet::Entry> entries = m_words.entries();
    if (entries.size() != 1 || !hasOneBitSet(entries.front().word))
        return std::nullopt;
    return *begin();
}

namespace {

struct SolveOfEntryParameters {
    uint64_t numberOfObjects { 0 };
    uint64_t maxWork { 0 };
    uint64_t maxHistoryBits { 0 };
};

} // anonymous namespace

// What the solve and the history of a body cost at most whose only objects are entry parameters that the caller knows.
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

uint64_t AnalysisContext::limit(LimitID limit) const
{
    uint64_t ofBuild = build.limit(limit);
    return workLimits == WorkLimits::OfBuild ? ofBuild : std::max(ofBuild, defaultLimitValue(limit, hostArchitecture()));
}

EscapeAnalysis::EscapeAnalysis(FunctionAnalyses& analyses)
    : m_analyses(analyses)
    , m_program(analyses.program())
    , m_function(analyses.function())
{
    const BuildContext& build = analyses.context().build;
    std::optional<SolveOfEntryParameters> solveOfEntryParameters = describeSolveOfEntryParameters(analyses);
    m_tracksOnlyEntryParameters = solveOfEntryParameters
        && solveOfEntryParameters->maxWork <= analyses.context().limit(LimitID::MaxEscapeAnalysisWork)
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
    RELEASE_ASSERT(m_gaveUp || m_objects.size() == solveOfEntryParameters->numberOfObjects);
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
    m_objectsInLocalHeapWords = WTF::move(results.objectsInLocalHeapWords);
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
        return TypesOfValue { value.type, value.expected().predicted() };
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
        TypesOfValue types { value.type, value.expected().predicted() };
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
    // A function that constructs thousands of derived classes has as many results of super().
    SparseObjectSet entryParameters;
    SparseObjectSet superConstructResults;
    SparseObjectSet frameWords;
    SparseObjectSet entryParametersBesideFrameWords;
    for (size_t index = 0; index < m_objects.size(); ++index) {
        if (m_objects[index].kind == ObjectKind::Allocation)
            continue;
        SparseObjectSet::Entry entry = SparseObjectSet::entryOf(ObjectID(index));
        entryParameters.add(entry);
        const Value& definition = m_function.at(m_objects[index].definition);
        if (definition.opcode == Opcode::GetHeapFrameWord) {
            frameWords.add(entry);
            continue;
        }
        entryParametersBesideFrameWords.add(entry);
        if (definition.opcode != Opcode::Parameter)
            superConstructResults.add(entry);
    }
    // Once for each set, not once for each question about two values: the sets of a row of a thousand branches hold hundreds of objects, and each read is held against each store.
    for (ValueSet& set : results.sets) {
        StoredSet stored { WTF::move(set.objects), { }, set.mayBeUntracked, false, false, false, { } };
        stored.holdsEntryParameter = stored.objects.overlaps(entryParameters);
        if (stored.holdsEntryParameter) {
            stored.holdsSuperConstructResult = stored.objects.overlaps(superConstructResults);
            stored.allocationsBesideEntryParameters = stored.objects;
            stored.allocationsBesideEntryParameters.remove(entryParameters);
            stored.holdsNothingElseFromOutside = !stored.mayBeUntracked && !stored.objects.overlaps(entryParametersBesideFrameWords);
            if (stored.holdsNothingElseFromOutside) {
                for (ObjectID object : ObjectSet(stored.objects)) {
                    if (frameWords.contains(object))
                        stored.groupsOfFrameWords.appendIfNotContains(*m_function.at(m_objects[object.index()].definition).allocationGroup());
                }
            }
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
    AOT::sortInLine(containments);
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
        for (ObjectID object : results.objectsOf(use))
            m_objects[object.index()].reasons.add(reasons);
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
    m_sharedUses.reserveInitialCapacity(2 * results.uses.size());
    m_isEscapeOfUnlistedUse.ensureSize(results.objectsOfUses.size());
    for (size_t index = 0; index < results.uses.size(); ++index) {
        const SolvedUse& solved = results.uses[index];
        bool createsFollowedAlias = !!createdAliasSetOf(results, solved);
        if (createsFollowedAlias && solved.uses.containsOnly({ OperandUse::Inspects, OperandUse::Returns }))
            continue;

        m_unlistedUses.append(UnlistedUse { static_cast<uint32_t>(m_sharedUses.size()), solved.firstObject, solved.numberOfObjects });
        for (bool isEscape : { false, true })
            m_sharedUses.append(ObjectUse { solved.use, solved.uses, solved.retainedBy, isEscape, solved.isContentsStore, solved.readsContents });
        std::optional<UseOrder> order;
        for (uint32_t position = solved.firstObject; position < solved.firstObject + solved.numberOfObjects; ++position) {
            ObjectID id = results.objectsOfUses[position];
            bool isEscape = !useReasons[index].isEmpty()
                || escapesThroughContainer(*this, results, solved, id, leaked.span())
                || escapesThroughCall(results, solved, id);
            TrackedObject& object = m_objects[id.index()];
            ++m_firstUse[id.index() + 1];
            object.uses.add(createsFollowedAlias ? solved.uses - OperandUse::Returns : solved.uses);
            if (!isEscape)
                continue;
            m_isEscapeOfUnlistedUse.quickSet(position);
            if (!order)
                order = useLists.orderOf(solved.use);
            if (!object.firstEscape || *order < orderOfFirstEscape[id.index()]) {
                object.firstEscape = solved.use;
                orderOfFirstEscape[id.index()] = *order;
            }
        }
    }
    m_objectsOfUnlistedUses = WTF::move(results.objectsOfUses);
    for (unsigned object = 0; object < numberOfObjects(); ++object)
        m_firstUse[object + 1] += m_firstUse[object];
}

void EscapeAnalysis::listUses() const
{
    RELEASE_ASSERT(m_isSolved);
    m_hasListedUses = true;
    const UseLists& useLists = m_analyses.useLists();

    // Sorting the uses once puts every object's list in order.
    struct OrderedUse {
        UseOrder order;
        uint32_t unlistedUse { 0 };
    };
    Vector<OrderedUse> orderedUses(m_unlistedUses.size(), [&](size_t index) {
        return OrderedUse { useLists.orderOf(m_sharedUses[m_unlistedUses[index].sharedUse].use), static_cast<uint32_t>(index) };
    });
    if (!std::ranges::is_sorted(orderedUses, { }, &OrderedUse::order))
        AOT::stableSortInLine(orderedUses, { }, &OrderedUse::order);
    Vector<uint32_t, 32> nextPosition(m_firstUse.span().first(m_objects.size()));
    m_uses.grow(m_firstUse.last());
    for (const OrderedUse& ordered : orderedUses) {
        const UnlistedUse& unlisted = m_unlistedUses[ordered.unlistedUse];
        for (uint32_t position = unlisted.firstObject; position < unlisted.firstObject + unlisted.numberOfObjects; ++position)
            m_uses[nextPosition[m_objectsOfUnlistedUses[position].index()]++] = unlisted.sharedUse + m_isEscapeOfUnlistedUse.quickGet(position);
    }
    m_unlistedUses.clear();
    m_objectsOfUnlistedUses.clear();
    m_isEscapeOfUnlistedUse = { };
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
        if (definition.opcode != Opcode::GetHeapFrameWord && definingOpcodes.contains(definition.opcode))
            return true;
        if (accessesLocalHeapWords(function, definition))
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
        if (escape.isInLocalHeapWords(object))
            return LiveInstances::Unknown;
        for (ValueID alias : escape.aliasesOf(object)) {
            if (liveBeforeAllocation.isLive(alias))
                return LiveInstances::Unknown;
        }
    }
    return LiveInstances::AtMostOne;
}

void EscapeAnalysis::computeLiveInstances(const CFG& cfg, const UseLists& useLists, const ScopedLambda<const BlockCycles&()>& cycles, OutOfSight outOfSight)
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

    AOT::sortInLine(candidates, { }, [](const AllocationInCycle& candidate) {
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

    if (outOfSight == OutOfSight::NotAsked)
        return;
    BitVector mayGoOutOfSight(m_objects.size());
    for (const AllocationInCycle& candidate : candidates) {
        if (!candidate.isMadeOnEdge && m_objects[candidate.object.index()].state == EscapeState::Escapes && !isInLocalHeapWords(candidate.object))
            mayGoOutOfSight.quickSet(candidate.object.index());
    }
    if (mayGoOutOfSight.isEmpty())
        return;
    m_allocationsInSightAtEnd.grow(cfg.numNodes());
    BitVector isInSight(m_objects.size());
    Vector<ObjectID, 16> inSight;
    auto add = [&](ObjectID object) {
        if (!isInSight.quickSet(object.index()))
            inSight.append(object);
    };
    for (BlockID block : cfg.reversePostOrder()) {
        if (!isInCycle(block))
            continue;
        Liveness::LocalCalc live(liveness, block);
        live.forEachLive([&](ValueID value) {
            for (ObjectID object : pointsTo(value).objects)
                add(object);
        });
        for (size_t next = 0; next < inSight.size(); ++next) {
            ObjectID container = inSight[next];
            if (isOpenContainer(m_objects[container.index()]))
                continue;
            for (ObjectID content : contentsOf(container))
                add(content);
        }
        Vector<ObjectID, 2> allocations;
        for (ObjectID object : inSight) {
            isInSight.quickClear(object.index());
            if (mayGoOutOfSight.quickGet(object.index()))
                allocations.append(object);
        }
        inSight.shrink(0);
        AOT::sortInLine(allocations);
        m_allocationsInSightAtEnd[block.index()] = WTF::move(allocations);
    }
    m_allocationsThatMayGoOutOfSight = WTF::move(mayGoOutOfSight);
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
    return singleObject(m_function.at(use.user).operand(use.operandIndex)) == object && isOnlyOperandHolding(object, use);
}

bool EscapeAnalysis::isOnlyOperandHolding(ObjectID object, const Use& use) const
{
    bool isOnlyOne = true;
    m_function.at(use.user).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operandIndex == use.operandIndex)
            return;
        isOnlyOne &= !pointsTo(operand).objects.contains(object);
    });
    return isOnlyOne;
}

EscapeAnalysis::AllocationSet EscapeAnalysis::allocationSetOf(ValueID value) const
{
    ValueSetID id = m_setOfValue[value.index()];
    if (!id) {
        // The function that resumes a generator reads the words of the frame before it stores any, and returns behind its stores: what a word holds an earlier run has put there.
        const Value& definition = m_function.at(sameObjectThroughCasts(m_function, value).valueInFrontOfFacts());
        bool isWordOfOwnFrame = m_function.role == FunctionRole::CoroutineBody && definition.opcode == Opcode::GetHeapFrameWord && !accessesLocalHeapWords(m_function, definition);
        return { m_emptySet, true, isWordOfOwnFrame, nullptr };
    }
    const StoredSet& set = m_sets[id.index()];
    return { set.holdsEntryParameter ? set.allocationsBesideEntryParameters : set.objects, set.mayBeUntracked || set.holdsEntryParameter, !set.mayBeUntracked && !set.holdsSuperConstructResult, set.holdsNothingElseFromOutside ? &set.groupsOfFrameWords : nullptr };
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
    if (first.mayBeUntracked && second.mayBeUntracked) {
        bool areWordsOfOtherGroups = first.groupsOfFrameWords && second.groupsOfFrameWords && std::ranges::none_of(*first.groupsOfFrameWords, [&](uint32_t group) {
            return second.groupsOfFrameWords->contains(group);
        });
        return areWordsOfOtherGroups ? AliasResult::NoAlias : AliasResult::MayAlias;
    }
    if ((first.mayBeUntracked ? first : second).existedAtEntryIfUntracked)
        return AliasResult::NoAlias;

    for (ObjectID object : first.mayBeUntracked ? secondObjects : firstObjects) {
        if (at(object).state != EscapeState::DoesNotEscape)
            return AliasResult::MayAlias;
    }
    return AliasResult::NoAlias;
}

struct RetainingObject {
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    uint32_t entryParameter { 0 };

    friend bool operator==(const RetainingObject&, const RetainingObject&) = default;
};

static std::optional<uint32_t> entryParameterIndexOf(FunctionAnalyses& analyses, const TrackedObject& object)
{
    if (object.kind != ObjectKind::EntryParameter)
        return std::nullopt;
    ValueLocation location = analyses.useLists().locationOf(object.definition);
    if (location.placement != ValuePlacement::Parameter || location.block != entryBlock)
        return std::nullopt;
    return location.index;
}

static bool isConfinedToActivation(const TrackedObject& container)
{
    return container.kind == ObjectKind::Allocation && container.state == EscapeState::DoesNotEscape;
}

static RetainingObject retainingObjectFor(FunctionAnalyses& analyses, const TrackedObject& container)
{
    if (std::optional<uint32_t> entryParameter = entryParameterIndexOf(analyses, container))
        return { RetainedIn::Operand, *entryParameter };
    if (container.kind == ObjectKind::Allocation && container.reasons == EscapeReasons { EscapeReason::Returned })
        return { RetainedIn::Result, 0 };
    return { };
}

static OperandSummary summarizeRetention(FunctionAnalyses& analyses, OperandUses uses, std::span<const ObjectID> containers)
{
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<RetainingObject> common;
    for (ObjectID id : containers) {
        const TrackedObject& container = escape.at(id);
        if (isConfinedToActivation(container))
            continue;
        RetainingObject retaining = retainingObjectFor(analyses, container);
        if (common && *common != retaining)
            retaining = { };
        common = retaining;
    }
    if (!common)
        return { uses - OperandUse::Retains, RetainedIn::UnknownMemory, 0 };
    return { uses | OperandUse::Retains, common->retainedIn, common->entryParameter };
}

static bool mayLoadReference(FunctionAnalyses& analyses, const ObjectUse& objectUse)
{
    if (!objectUse.readsContents)
        return false;
    std::optional<ObjectID> made = analyses.escape().objectDefinedBy(objectUse.use.user);
    return !made || analyses.function().at(analyses.escape().at(*made).definition).opcode == Opcode::GetHeapFrameWord;
}

// The escape analysis does not follow what is read out of a parameter. The summaries that the function is analyzed against cover it.
static OperandUses usesOfLoadedReferences(FunctionAnalyses& analyses, uint32_t entryParameter)
{
    const AnalysisContext& context = analyses.context();
    OperandUses known = context.effects ? context.effects->of(context.id).operand(entryParameter).uses : OperandUses::all();
    if (known.contains(OperandUse::RetainsContents))
        return OperandUse::RetainsContents;
    return known - leakingUses;
}

std::unique_ptr<FunctionAnalyses> createAnalysesForSummary(BuildContext& build, const EffectContext& context, const FlowResults& flow, FunctionID id, const Function& function)
{
    return makeUnique<FunctionAnalyses>(AnalysisContext { build, context.program, &flow, &context.summaries, id }, function);
}

void forEachBaseObject(FunctionAnalyses& analyses, ValueID value, NOESCAPE const ScopedLambda<void(const BaseObject&)>& functor)
{
    if (!mayBeReference(analyses.program().typeOf(analyses.function(), value)))
        return;

    const EscapeAnalysis& escape = analyses.escape();
    if (escape.gaveUp()) {
        functor(BaseObject { });
        return;
    }

    PointsToSet set = escape.pointsTo(value);
    bool mayBeUntracked = set.mayBeUntracked;
    for (ObjectID id : set.objects) {
        const TrackedObject& object = escape.at(id);
        switch (object.kind) {
        case ObjectKind::Allocation:
            functor(BaseObject { BaseObject::Kind::OwnAllocation, 0 });
            break;
        case ObjectKind::EntryParameter:
            if (std::optional<uint32_t> entryParameter = entryParameterIndexOf(analyses, object))
                functor(BaseObject { BaseObject::Kind::EntryParameter, *entryParameter });
            else
                mayBeUntracked = true;
            break;
        }
    }
    if (mayBeUntracked)
        functor(BaseObject { });
}

// A saved frame holds what is live in it, and the function goes on when its caller has long gone on: what it then does to a parameter happens at a time of which the caller knows nothing.
static bool maySaveFrame(FunctionAnalyses& analyses)
{
    static const OpcodeSet opcodesThatMaySuspend = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (maySuspend(opcode))
                result.add(opcode);
        }
        return result;
    }();
    if (!analyses.opcodesOfValues().intersects(opcodesThatMaySuspend))
        return false;
    const Function& function = analyses.function();
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return opcodesThatMaySuspend.contains(function.at(id).opcode);
        });
    });
}

OperandSummary summarizeEntryParameter(FunctionAnalyses& analyses, uint32_t entryParameter)
{
    const Function& function = analyses.function();
    ValueID parameter = function.at(entryBlock).parameters[entryParameter];
    if (!mayBeReference(analyses.program().typeOf(function, parameter)))
        return { .uses = OperandUse::Inspects };

    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> id = escape.objectDefinedBy(parameter);
    if (escape.gaveUp() || !id)
        return { };

    const TrackedObject& object = escape.at(*id);
    OperandUses uses = object.uses;
    auto loadsReference = [&](const ObjectUse& objectUse) {
        return mayLoadReference(analyses, objectUse);
    };
    if (std::ranges::any_of(escape.usesOf(*id), loadsReference))
        uses.add(usesOfLoadedReferences(analyses, entryParameter));
    auto callsInCycle = [&](const ObjectUse& objectUse) {
        return objectUse.uses.containsAny(callingUses) && analyses.isInCycle(objectUse.use.block);
    };
    if (std::ranges::any_of(escape.usesOf(*id), callsInCycle))
        uses.add(OperandUse::CallsRepeatedly);

    EscapeReasons direct = object.reasons - EscapeReason::StoredInEscapingObject;
    if (direct.contains(EscapeReason::Returned))
        uses.add(OperandUse::Returns);
    if (function.isUnsplitCoroutine() || maySaveFrame(analyses) || !(direct - EscapeReason::Returned).isEmpty())
        return { uses | OperandUse::Retains, RetainedIn::UnknownMemory, 0 };
    return summarizeRetention(analyses, uses, escape.containersOf(*id));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
