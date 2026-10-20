/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEscapeAnalysis.h"

#if ENABLE(AOT)

#include "AOTGrouping.h"
#include "AOTLiveness.h"
#include "AOTPointsToSolver.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>

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

EscapeAnalysis::EscapeAnalysis(const EscapeAnalysisInputs& inputs)
    : m_program(inputs.context.program)
    , m_function(inputs.function)
{
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
    computeAliases();
    computeLiveInstances(inputs.cfg, inputs.useLists, inputs.cycles);
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
    for (ValueSet& set : results.sets)
        m_sets.append(StoredSet { WTF::move(set.objects), set.mayBeUntracked });
}

void EscapeAnalysis::computeAliases()
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

void EscapeAnalysis::computeUses(const PointsToResults& results, const UseLists& useLists, std::span<const EscapeReasons> useReasons)
{
    struct ListedUse {
        ObjectID object;
        ObjectUse objectUse;
    };

    for (unsigned index = 0; index < m_objects.size(); ++index)
        m_objects[index].uses = results.objects[index].appliedUses;

    Vector<Containment> leaked = collectLeakedContainments(results);
    Vector<ListedUse> listedUses;
    listedUses.reserveInitialCapacity(results.uses.size());
    for (size_t index = 0; index < results.uses.size(); ++index) {
        const SolvedUse& solved = results.uses[index];
        bool createsFollowedAlias = !!createdAliasSetOf(results, solved);
        if (createsFollowedAlias && solved.uses.containsOnly({ OperandUse::Inspects, OperandUse::Returns }))
            continue;

        bool isEscape = !useReasons[index].isEmpty()
            || escapesThroughContainer(*this, results, solved, leaked.span())
            || escapesThroughCall(results, solved);
        listedUses.append(ListedUse { solved.object, ObjectUse { solved.use, solved.uses, solved.retainedBy, isEscape, solved.isContentsStore, solved.readsContents } });
        m_objects[solved.object.index()].uses.add(createsFollowedAlias ? solved.uses - OperandUse::Returns : solved.uses);
    }

    std::sort(listedUses.begin(), listedUses.end(), [&](const ListedUse& a, const ListedUse& b) {
        if (a.object != b.object)
            return a.object < b.object;
        return useLists.isBefore(a.objectUse.use, b.objectUse.use);
    });
    groupByIndex(numberOfObjects(), m_firstUse, m_uses, [&](const auto& visit) {
        for (const ListedUse& listed : listedUses)
            visit(listed.object.index(), listed.objectUse);
    });
}

void EscapeAnalysis::computeStates()
{
    for (unsigned index = 0; index < m_objects.size(); ++index) {
        TrackedObject& object = m_objects[index];
        if (object.reasons.containsAny(limitReasons)) {
            object.state = EscapeState::Unknown;
            object.uses = OperandUses::all();
        } else if (object.reasons.isEmpty())
            object.state = EscapeState::DoesNotEscape;
        else
            object.state = EscapeState::Escapes;

        for (const ObjectUse& objectUse : usesOf(ObjectID(index))) {
            if (objectUse.isEscape) {
                object.firstEscape = objectUse.use;
                break;
            }
        }
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

void EscapeAnalysis::computeLiveInstances(const CFG& cfg, const UseLists& useLists, const BlockCycles& blockCycles)
{
    if (m_gaveUp)
        return;

    Vector<AllocationInCycle> candidates;
    for (unsigned index = 0; index < m_objects.size(); ++index) {
        TrackedObject& object = m_objects[index];
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
        if (!blockCycles.isInCycle(location.block))
            object.liveInstances = LiveInstances::AtMostOne;
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
                m_objects[candidate.object.index()].liveInstances = liveInstancesOf(candidate.object);
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

struct AllocationSet {
    BitVector objects;
    bool mayBeUntracked { true };
};

static AllocationSet allocationSetOf(const EscapeAnalysis& escape, ValueID value)
{
    PointsToSet set = escape.pointsTo(value);
    AllocationSet result { { }, set.mayBeUntracked };
    for (ObjectID object : set.objects) {
        switch (escape.at(object).kind) {
        case ObjectKind::Allocation:
            result.objects.set(object.index());
            break;
        case ObjectKind::EntryParameter:
            result.mayBeUntracked = true;
            break;
        }
    }
    return result;
}

AliasResult EscapeAnalysis::alias(ValueID a, ValueID b) const
{
    RELEASE_ASSERT(a.index() < m_setOfValue.size() && b.index() < m_setOfValue.size());
    if (sameObjectThroughCasts(m_function, a) == sameObjectThroughCasts(m_function, b))
        return AliasResult::MustAlias;
    if (!m_program.typeOf(m_function, a).overlaps(m_program.typeOf(m_function, b), m_program))
        return AliasResult::NoAlias;

    AllocationSet first = allocationSetOf(*this, a);
    AllocationSet second = allocationSetOf(*this, b);
    ObjectSet firstObjects(first.objects);
    ObjectSet secondObjects(second.objects);
    bool areBothTracked = !first.mayBeUntracked && !second.mayBeUntracked;
    if (firstObjects.overlaps(secondObjects)) {
        std::optional<ObjectID> single = firstObjects.onlyMember();
        bool areSameSingleObject = areBothTracked && single && secondObjects.onlyMember() == single;
        if (areSameSingleObject && at(*single).liveInstances == LiveInstances::AtMostOne)
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
