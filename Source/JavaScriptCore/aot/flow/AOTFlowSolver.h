/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowRecords.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowObjectState;
struct FlowPropertyState;
struct FlowCallState;

struct FlowObjectStateDeleter {
    void operator()(FlowObjectState*) const;
};

struct FlowPropertyStateDeleter {
    void operator()(FlowPropertyState*) const;
};

struct FlowCallStateDeleter {
    void operator()(FlowCallState*) const;
};

struct FlowSolverStates {
    std::unique_ptr<FlowObjectState, FlowObjectStateDeleter> objects;
    std::unique_ptr<FlowPropertyState, FlowPropertyStateDeleter> properties;
    std::unique_ptr<FlowCallState, FlowCallStateDeleter> calls;
};

// From the weakest on. A second wait of the same statement on the same node keeps the stronger of the two.
enum class FlowWakeUp : uint8_t {
    ContinueAtUnknownObject,
    ContinueAtObject,
    Continue,
    Revisit,
};

// What a step of a rule takes to be absent. The answer of such a rule shrinks as sets grow, so it does not commute with the rules that make them grow,
// and the step is taken only while nothing is queued: between two such moments the solve is a plain fixpoint, which every order of the work reaches.
// The steps that wait are released kind by kind, in this order, all of one kind at once.
enum class FlowAbsence : uint8_t {
    SetterAboveReceiver,
    PropertyOnHolder,
    UnknownOperand,
};

inline constexpr size_t numberOfFlowAbsences = 3;

enum class FlowTemporaryOwner : uint8_t {
    Core,
    Objects,
    Properties,
    Calls,
};

struct FlowTemporaryKey {
    FlowStatementSite site;
    FlowTemporaryOwner owner { FlowTemporaryOwner::Core };
    uint16_t role { 0 };
    uint32_t index { 0 };

    friend bool operator==(const FlowTemporaryKey&, const FlowTemporaryKey&) = default;
};

inline uint32_t hashOf(FlowStatementSite site)
{
    return hashOfNumbers(static_cast<uint64_t>(site.instance.index()) << 32 | site.statement.index, static_cast<uint64_t>(site.statement.kind));
}

inline uint32_t hashOf(const FlowTemporaryKey& key)
{
    uint64_t kinds = static_cast<uint64_t>(key.site.statement.kind) << 24 | static_cast<uint64_t>(key.owner) << 16 | key.role;
    return hashOfNumbers(static_cast<uint64_t>(key.site.instance.index()) << 32 | key.site.statement.index, kinds << 32 | key.index);
}

// A set of indexes that is taken in ascending order, round and round: the lowest index after the one taken last, or else the lowest of all.
// A bit of a level says that a word of the level below has a bit, so neither adding nor taking depends on how far apart the indexes are.
class FlowSweepQueue {
public:
    bool isEmpty() const { return !m_size; }
    size_t size() const { return m_size; }
    void continueAt(size_t index) { m_position = index; }

    bool add(size_t index)
    {
        size_t word = index / bitsPerWord;
        if (word >= m_levels[0].size())
            growFor(index);

        uint64_t bit = 1ull << (index % bitsPerWord);
        if (m_levels[0][word] & bit)
            return false;
        ++m_size;
        for (Vector<uint64_t>& level : m_levels) {
            bool wasEmpty = !level[index / bitsPerWord];
            level[index / bitsPerWord] |= 1ull << (index % bitsPerWord);
            if (!wasEmpty)
                break;
            index /= bitsPerWord;
        }
        return true;
    }

    size_t takeNext()
    {
        ASSERT(m_size);
        std::optional<size_t> found = findAtOrAfter(m_position);
        size_t index = found ? *found : *findAtOrAfter(0);
        m_position = index + 1;
        --m_size;

        size_t position = index;
        for (Vector<uint64_t>& level : m_levels) {
            level[position / bitsPerWord] &= ~(1ull << (position % bitsPerWord));
            if (level[position / bitsPerWord])
                break;
            position /= bitsPerWord;
        }
        return index;
    }

private:
    static constexpr size_t bitsPerWord = 64;
    static constexpr size_t numberOfLevels = 3;

    void growFor(size_t index)
    {
        size_t numberOfIndexes = std::max(index + 1, m_levels[0].size() * bitsPerWord * 2);
        for (Vector<uint64_t>& level : m_levels) {
            size_t numberOfWords = (numberOfIndexes + bitsPerWord - 1) / bitsPerWord;
            level.insertFill(level.size(), 0ull, numberOfWords - level.size());
            numberOfIndexes = numberOfWords;
        }
    }

    std::optional<size_t> findAtOrAfter(size_t start) const
    {
        size_t level = 0;
        size_t position = start;
        while (true) {
            const Vector<uint64_t>& words = m_levels[level];
            size_t word = position / bitsPerWord;
            if (word >= words.size())
                return std::nullopt;

            uint64_t bits = words[word] & (~0ull << (position % bitsPerWord));
            if (bits) {
                position = word * bitsPerWord + static_cast<size_t>(std::countr_zero(bits));
                if (!level)
                    return position;
                --level;
                position *= bitsPerWord;
                continue;
            }
            if (level + 1 < numberOfLevels) {
                ++level;
                position = word + 1;
            } else
                position = (word + 1) * bitsPerWord;
        }
    }

    std::array<Vector<uint64_t>, numberOfLevels> m_levels;
    size_t m_position { 0 };
    size_t m_size { 0 };
};

using FlowCursorID = ID<struct FlowCursorIDTag>;

// What the walk of a kept lookup waits on: the prototype link of an object, or else mutations of the object, or else its property under a key, or else all its properties.
struct FlowLookupWait {
    FlowNodeID node;
    FlowObjectID object;
    PropertyKeyID key;
    OptionSet<ObjectMutation> mutations;
};

using FlowLookupWaits = Vector<FlowLookupWait, 8>;

// Something that the solved program gains when a rule of the solve runs once more. The site is null for a rule that is no statement.
struct FlowFixpointChange {
    FlowStatementSite site;
    ValueID origin;
    FlowNodeID node;
    String description;
};

// What a cursor hands out: the objects that came into its node since it was last taken. A set that no cursor handed out converts to one without a cursor.
class FlowNewObjects : public FlowObjectSet {
public:
    FlowNewObjects() = default;

    FlowNewObjects(const FlowObjectSet& objects)
        : FlowObjectSet(objects)
    {
    }

    FlowNewObjects(FlowObjectSet&& objects)
        : FlowObjectSet(WTF::move(objects))
    {
    }

    FlowNewObjects(FlowObjectSet&& objects, FlowStatementSite site, FlowCursorID cursor)
        : FlowObjectSet(WTF::move(objects))
        , m_site(site)
        , m_cursor(cursor)
    {
    }

    FlowStatementSite site() const { return m_site; }
    FlowCursorID cursor() const { return m_cursor; }
    FlowNewObjects withObjects(FlowObjectSet&& subset) const { return { WTF::move(subset), m_site, m_cursor }; }

private:
    FlowStatementSite m_site;
    FlowCursorID m_cursor;
};

class FlowSolver {
    WTF_MAKE_NONCOPYABLE(FlowSolver);
public:
    FlowSolver(const Program&, FlowObjectTable&, FlowGraph&, BuildContext&);
    ~FlowSolver();

    void solve();
    Vector<FlowFixpointChange> takeFixpointChanges() { return WTF::move(m_fixpointChanges); }
    FlowRecords takeRecords();
    FlowSolverStates takeStates();

    const Program& program() const { return m_program; }
    const BuiltinTable& builtins() const { return m_program.builtins(); }
    BuildContext& build() { return m_build; }
    FlowObjectTable& objects() { return m_objects; }
    const FlowObjectTable& objects() const { return m_objects; }
    FlowGraph& graph() { return m_graph; }
    const FlowGraph& graph() const { return m_graph; }
    FlowValueTable& values() { return m_graph.values(); }
    TypeArena& types() { return m_graph.values().types(); }
    FlowRecords& records() { return m_records; }
    const FlowRecords& records() const { return m_records; }
    FlowObjectState& objectState() { return *m_states.objects; }
    FlowPropertyState& propertyState() { return *m_states.properties; }
    FlowCallState& callState() { return *m_states.calls; }

    FlowNodeID node(FlowInstanceID instance, FlowLocalID local) const { return local ? m_graph.nodeOf(instance, local) : FlowNodeID(); }
    FlowNodeID node(FlowStatementSite site, FlowLocalID local) const { return node(site.instance, local); }
    const FlowValue& valueOf(FlowNodeID node) const LIFETIME_BOUND { return m_graph.valueOf(node); }
    const FlowValue& valueAt(FlowStatementSite site, FlowLocalID local) const LIFETIME_BOUND { return m_graph.valueOf(m_graph.nodeOf(site.instance, local)); }
    const FlowSummary& summaryOf(FlowStatementSite site) const LIFETIME_BOUND { return m_graph.summaryOf(site.instance); }
    const Function& functionOf(FlowStatementSite site) const LIFETIME_BOUND { return m_program.at(m_graph.at(site.instance).function); }
    const Value& operationAt(FlowStatementSite site, ValueID origin) const LIFETIME_BOUND { return functionOf(site).at(origin); }
    Witness witnessAt(FlowInstanceID instance, ValueID origin) const { return m_graph.witnessOf(instance, origin); }
    Witness witnessAt(FlowStatementSite site, ValueID origin) const { return m_graph.witnessOf(site.instance, origin); }
    Witness witnessOfNode(FlowNodeID) const;
    Vector<FlowNodeID, 8> nodesOf(FlowStatementSite, FlowLocalList) const;

    void join(FlowNodeID target, const FlowValue&, const Witness& = noWitness);
    void joinObject(FlowNodeID target, FlowObjectID, const Witness& = noWitness);
    void joinObjects(FlowNodeID target, const FlowObjectSet&, const Witness& = noWitness);
    void joinPrimitives(FlowNodeID target, uint16_t primitives, const Witness& = noWitness);
    void joinUnknown(FlowNodeID target, const Witness&);
    void joinUnknownObject(FlowNodeID target, const Witness&);
    void connect(FlowNodeID from, FlowNodeID to, TypeID filter = { }, const Witness& = noWitness);
    FlowValue valueOfSeed(FlowInstanceID, const FlowSeed&);

    void waitOn(FlowNodeID, FlowStatementSite, FlowWakeUp = FlowWakeUp::Continue);
    // A mutation outside the relevant ones wakes nobody. A second wait of the same waiter adds its mutations to the first.
    void waitOnMutations(FlowObjectID, FlowStatementSite, OptionSet<ObjectMutation> relevant = allObjectMutations, FlowWakeUp = FlowWakeUp::Revisit);
    void waitOnProperty(FlowObjectID, PropertyKeyID, FlowStatementSite);
    void waitOnProperties(FlowObjectID, FlowStatementSite);
    // A wait of a statement of kind Lookup is added to the list and registers nothing. Whoever takes the kept answer of the lookup makes the waits of the list.
    void listWaitsOfLookup(FlowLookupWaits*);
    void waitOn(const FlowLookupWait&, FlowStatementSite);
    void queueStatement(FlowStatementSite);
    void revisit(FlowStatementSite);
    void revisitMutationWaiters(FlowObjectID);
    void revisitPropertyWaiters(FlowObjectID, PropertyKeyID);
    bool hasQueuedWork() const;
    bool isCheckingFixpoint() const { return m_fixpointCheck; }
    // Whether the statement may act on the absence in this run, for the member in progress. Where it may not, it leaves the step out:
    // the member comes out again once nothing is queued, and the statement decides again on what the sets hold then.
    bool mayRelyOn(FlowAbsence, FlowStatementSite);

    // Each object of the node comes out once. The work is in proportion to what comes out, not to what the node holds.
    // A cursor with a key wakes its statement when the node gets an object that has not come out, or primitives. Code that reads the whole value of the node waits on the node.
    FlowNewObjects takeNewObjects(FlowStatementSite, uint32_t cursorInStatement, FlowNodeID);
    FlowNewObjects takeNewObjects(const FlowTemporaryKey& cursor, FlowNodeID);
    // No node for a cursor that has handed out nothing. What a cursor hands out is in its node.
    FlowNodeID nodeReadBy(FlowCursorID cursor) const { return m_cursors.all[cursor.index()].node; }
    // Whether the node holds a kind of primitive that it did not hold when the same cursor was last asked.
    bool takeNewPrimitives(FlowStatementSite, uint32_t cursorInStatement, FlowNodeID);
    bool takeNewPrimitives(const FlowTemporaryKey& cursor, FlowNodeID);

    // The loop over what a cursor handed out. A wait that a body makes for the statement of the cursor (on mutations, on properties, or on a node with FlowWakeUp::Revisit)
    // brings back the member of that body alone, and every cursor taken inside that body starts over with it. The outermost loop decides.
    // A wait made outside such a loop makes every cursor of the statement start over.
    void forEachMember(const FlowNewObjects& objects, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
    {
        if (!objects.cursor() || m_memberInProgress.cursor) {
            objects.forEach(functor);
            return;
        }
        objects.forEach([&](FlowObjectID member) {
            m_memberInProgress = { objects.site(), objects.cursor(), member };
            functor(member);
        });
        m_memberInProgress = { };
    }

    FlowNodeID temporaryFor(const FlowTemporaryKey&);
    // A temporary that never holds the unknown object.
    FlowNodeID gateFor(const FlowTemporaryKey&);
    // A gate that lists every object it is offered. Nothing in the solve reads it.
    FlowNodeID predictionFor(const FlowTemporaryKey&);
    // What native code calls of its own accord, without seen code naming it as an operand, is no object whose call would be an event: the run time refuses that call.
    void keepObjectsLettingInUnseenCodeOutOf(FlowNodeID node) { m_nodesWithoutObjectsLettingInUnseenCode.set(node.index()); }
    void noteObjectLettingInUnseenCode(FlowObjectID object) { m_objectsLettingInUnseenCode.add(object); }

    FlowInstanceID enter(FunctionID, FlowContext);
    FlowInstanceID enterAtCallSite(FunctionID, const FlowCallSiteKey&);

    FlowObjectID appendObject(const FlowObject&);
    FlowNodeID ensureOpenPart(FlowObjectID, FlowOpenPart);
    FlowNodeID ensurePropertyValue(FlowObjectID, PropertyKeyID);
    FlowNodeID ensureGetter(FlowObjectID, PropertyKeyID);
    FlowNodeID ensureSetter(FlowObjectID, PropertyKeyID);

    void noteMutation(FlowObjectID, OptionSet<ObjectMutation>, const Witness&);
    void noteMutationOf(const FlowObjectSet&, OptionSet<ObjectMutation>, const Witness&);
    void noteUse(FlowObjectID, FlowObjectUse);
    void noteUseOf(const FlowObjectSet&, FlowObjectUse);
    bool noteEvent(FlowObjectID, FlowObjectEvent, const Witness&);
    void noteLocationEvent(FlowNodeID, OptionSet<FlowLocationEvent>);
    void noteDirectUses(FlowNodeID, OperandUses);

    // The objects of nativeCode.
    const FlowObjectBitmap& escapedObjects() const LIFETIME_BOUND { return m_escapedObjects; }
    bool hasReachedNativeCode(FlowObjectID object) const { return m_escapedObjects.contains(object); }
    bool hasReachedUnknownJavaScript(FlowObjectID object) const { return m_graph.valueOf(m_graph.unknownJavaScript()).objects.contains(object); }

    void charge(FunctionID, uint64_t steps);
    void charge(FlowStatementSite, uint64_t steps);
    bool isPastWorkLimit(FunctionID function) const { return m_records.functionsPastWorkLimit().get(function.index()); }
    // True from the step that goes past the work limit until the function, or the call of the host, is abandoned. A loop over objects stops then:
    // the abandonment, which comes before any other work, covers what the statement leaves undone.
    bool isAboutToBeAbandoned(FlowStatementSite) const;

private:
    class FixpointCheck;

    struct Worklist {
        Deque<FlowInstanceID> instancesToInitialize;
        Deque<FlowObjectID> objectsReachingNativeCode;
        Deque<FlowObjectID> objectsReachingUnknownJavaScript;
        Deque<FunctionID> functionsToAbandon;
        Deque<FlowStatementSite> hostCallsToAbandon;
        // The nodes of an instance lie side by side, in the order of the values of the function: one sweep carries a value through a body.
        FlowSweepQueue nodesToProcess;
        // A node with many successors and waiters has its turn when nothing else is left, and hands on all that has come into it by then at once.
        FlowSweepQueue widelyReadNodesToProcess;
        BitVector widelyReadNodes;
        BitVector nodesWithNewPrimitives;
        Vector<FlowObjectSet> pendingObjectsOfNode;
        Deque<FlowStatementSite> statementsToRun;
        BitVector queuedSummaryStatements;
        BitVector queuedBuiltinCalls;
        Vector<uint32_t> firstStatementOfInstance;
        uint32_t numberOfSummaryStatements { 0 };
        HashMap<FlowStatementSite, Vector<FlowCursorID, 2>> cursorsWithMembersToRepeat;
    };

    struct Member {
        FlowStatementSite site;
        FlowCursorID cursor;
        FlowObjectID object;
    };

    using WaiterID = ID<struct FlowWaiterIDTag>;

    // With a cursor, the wake-up makes that cursor hand out the members again. Without one, every cursor of the statement starts over.
    struct Waiter {
        FlowStatementSite site;
        FlowCursorID cursor;
        FlowObjectSet members;
        WaiterID next;
    };

    // A waiter that continues and has a cursor is the cursor's own: what the statement has taken already does not wake it.
    struct NodeWaiter : Waiter {
        FlowWakeUp wakeUp { FlowWakeUp::ContinueAtUnknownObject };
    };

    struct MutationWaiter : Waiter {
        OptionSet<ObjectMutation>& mutationsFor(FlowWakeUp wakeUp) LIFETIME_BOUND { return wakeUp == FlowWakeUp::Revisit ? mutationsToRevisitFor : mutationsToContinueFor; }

        OptionSet<ObjectMutation> mutationsToRevisitFor;
        OptionSet<ObjectMutation> mutationsToContinueFor;
    };

    // One waiter for each statement and cursor that waits on one node, object or property.
    // The waiters on one of these form a ring through `next`, in the order in which they came.
    template<typename Watched, typename WaiterType>
    struct WaiterTable {
        struct Entry : WaiterType {
            Watched watched;
        };

        static constexpr bool isWatchedByIndex = requires(Watched watched) { watched.index(); };
        using LastWaiters = std::conditional_t<isWatchedByIndex, Vector<WaiterID>, HashMap<Watched, WaiterID>>;

        static uint64_t numberOf(FlowNodeID node) { return node.index(); }
        static uint64_t numberOf(FlowObjectID object) { return object.index(); }
        static uint64_t numberOf(std::pair<FlowObjectID, PropertyKeyID> property) { return static_cast<uint64_t>(property.first.index()) << 32 | property.second.index(); }

        static uint32_t hashOf(const Watched& watched, const Member& member)
        {
            if (member.cursor)
                return hashOfNumbers(numberOf(watched), member.cursor.index());
            FlowStatementReference statement = member.site.statement;
            return hashOfNumbers(numberOf(watched), (static_cast<uint64_t>(member.site.instance.index()) << 32 | statement.index) + (static_cast<uint64_t>(statement.kind) + 1 << 56));
        }

        WaiterType& ensure(const Watched& watched, const Member& member, uint64_t& numberOfChanges) LIFETIME_BOUND
        {
            uint32_t hash = hashOf(watched, member);
            std::optional<uint32_t> position = positions.find(hash, [&](uint32_t candidate) {
                const Entry& entry = waiters[candidate];
                return entry.watched == watched && entry.cursor == member.cursor && (member.cursor || entry.site == member.site);
            });
            if (!position) {
                ++numberOfChanges;
                position = static_cast<uint32_t>(waiters.size());
                positions.add(hash, *position);
                append(watched, member);
            }

            Entry& entry = waiters[*position];
            if (member.object && entry.members.add(member.object))
                ++numberOfChanges;
            return entry;
        }

        void forEachWaiterOn(const Watched& watched, NOESCAPE const Invocable<void(const WaiterType&)> auto& functor) const
        {
            WaiterID last = lastWaiterOn(watched);
            if (!last)
                return;
            WaiterID current = last;
            do {
                current = waiters[current.index()].next;
                functor(waiters[current.index()]);
            } while (current != last);
        }

        WaiterID lastWaiterOn(const Watched& watched) const
        {
            if constexpr (isWatchedByIndex)
                return watched.index() < lastWaiters.size() ? lastWaiters[watched.index()] : WaiterID();
            else
                return lastWaiters.get(watched);
        }

        void append(const Watched& watched, const Member& member)
        {
            WaiterID id(waiters.size());
            waiters.append(Entry { });
            Entry& entry = waiters.last();
            entry.site = member.site;
            entry.cursor = member.cursor;
            entry.watched = watched;

            auto link = [&](WaiterID& last) {
                entry.next = last ? std::exchange(waiters[last.index()].next, id) : id;
                last = id;
            };
            if constexpr (isWatchedByIndex) {
                size_t index = watched.index();
                if (index >= lastWaiters.size())
                    lastWaiters.grow(std::max(index + 1, lastWaiters.size() * 2));
                link(lastWaiters[index]);
            } else
                link(lastWaiters.add(watched, WaiterID()).iterator->value);
        }

        Vector<Entry> waiters;
        HashedIndex positions;
        LastWaiters lastWaiters;
    };

    struct Waiters {
        WaiterTable<FlowNodeID, NodeWaiter> onNodes;
        WaiterTable<FlowObjectID, MutationWaiter> onMutations;
        WaiterTable<FlowObjectID, Waiter> onProperties;
        WaiterTable<std::pair<FlowObjectID, PropertyKeyID>, Waiter> onNamedProperties;
    };

    struct HeldBackValue {
        FlowNodeID target;
        FlowValue value;
        Witness witness;
        bool recordsOrigin { false };
    };

    // By statement and cursor.
    using StepsRelyingOnAbsence = HashMap<std::pair<FlowStatementSite, FlowCursorID>, Waiter>;

    struct QuietSteps {
        std::array<StepsRelyingOnAbsence, numberOfFlowAbsences> waiting;
        FlowAbsence releasedAbsence { FlowAbsence::SetterAboveReceiver };
        bool isReleasing { false };
        // Every released step decides on the same sets: a node that was there when the release began gets what the steps put into it after the last of them.
        uint32_t numberOfNodesAtRelease { 0 };
        Vector<HeldBackValue> heldBackValues;
        uint64_t numberOfReleases { 0 };
    };

    struct Cursor {
        FlowObjectSet membersToRepeat;
        FlowNodeID node;
        uint32_t numberOfArrivalsTaken { 0 };
        uint32_t lastRun { 0 };
        uint16_t primitivesTaken { 0 };
        bool hasCursorsInsideMembers { false };
    };

    // The keyed cursors of one statement form a chain that starts at the first of them.
    struct KeyedCursor {
        FlowTemporaryKey key;
        FlowNodeID node;
        FlowCursorID cursor;
        uint32_t nextOfStatement { 0 };
    };

    using ArrivalListID = ID<struct FlowArrivalListIDTag>;

    // What the node held when the list was made is only counted: the first take of a cursor hands out the whole set, so every later take starts behind those objects.
    struct ArrivalList {
        uint32_t numberOfArrivals() const { return numberOfEarlierObjects + arrivals.size(); }
        std::span<const FlowObjectID> arrivalsAfter(uint32_t numberOfArrivalsTaken) const LIFETIME_BOUND { return arrivals.subspan(numberOfArrivalsTaken - numberOfEarlierObjects); }

        Vector<FlowObjectID, 2, CrashOnOverflow, 4> arrivals;
        uint32_t numberOfEarlierObjects { 0 };
    };

    struct Cursors {
        Vector<Cursor> all;
        // The objects of a node that some cursor reads, in the order in which they came.
        Vector<ArrivalList> arrivalLists;
        Vector<ArrivalListID> arrivalListOfNode;
        Vector<FlowCursorID> firstOfInstance;
        Vector<KeyedCursor> keyed;
        HashedIndex keyedByKeyAndNode;
        HashedIndex firstKeyedOfStatement;
        HashMap<std::pair<FlowCursorID, FlowObjectID>, Vector<FlowCursorID, 1>> takenInsideMember;
    };

    struct Temporary {
        FlowTemporaryKey key;
        FlowNodeID node;
    };

    struct Temporaries {
        Vector<Temporary> all;
        HashedIndex byKey;
    };

    struct Counters {
        static constexpr unsigned numberOfStatementKinds = numberOfSummaryStatementKinds + 1;
        static constexpr unsigned numberOfNodeKinds = static_cast<unsigned>(FlowNodeKind::Local) + 1;

        uint64_t nodeSteps { 0 };
        uint64_t valuesPushedAlongEdges { 0 };
        uint64_t valuesPushedWithoutChange { 0 };
        uint64_t statementRuns { 0 };
        uint64_t statementRunsWithoutChange { 0 };
        uint64_t wakeUps { 0 };
        uint64_t statementsStartedOver { 0 };
        uint64_t membersRepeated { 0 };
        uint64_t cursorsNotReached { 0 };
        uint64_t changes { 0 };
        std::array<uint64_t, numberOfStatementKinds> runsOfKind { };
        std::array<uint64_t, numberOfStatementKinds> runsWithoutChangeOfKind { };
        std::array<uint64_t, numberOfNodeKinds> nodeStepsOfKind { };
        uint64_t stepsAtNextLog { UINT64_MAX };
    };

    enum class UnknownObjectArrival : uint8_t {
        None,
        Arrived,
    };

    void drainQueues();
    void drainQueuesAndRefine();
    bool releaseStepsRelyingOnAbsence();
    bool holdsBackValuesOf(FlowNodeID node) const { return m_quietSteps.isReleasing && node && node.index() < m_quietSteps.numberOfNodesAtRelease; }
    bool takeShuffledStep();
    // What join() does, along an edge: the records keep only the operations that put the unknown object into a node, each of them, also where the node holds it already.
    UnknownObjectArrival propagate(FlowNodeID target, const FlowValue&, const Witness&);
    // The sink itself, and unknown JavaScript for a local of an abandoned instance.
    FlowNodeID unseenCodeInPlaceOf(FlowNodeID target) const;
    UnknownObjectArrival addTo(FlowNodeID target, const FlowValue&);
    // The unknown object stands for every object that has reached native code. A node that holds it lists a few of them, for what is predicted from them, and takes no more.
    // The set means the same with and without them, so no result loses anything it may rely on, and the limit records no loss.
    bool listsNoMoreEscapedObjects(FlowNodeID target, const FlowValue& arriving) const;
    FlowObjectSet withoutEscapedObjects(const FlowObjectSet&) const;
    void growNodeTablesFor(FlowNodeID);
    void queueNode(FlowNodeID, FlowObjectSet&& addedObjects, bool hasNewPrimitives);
    uint32_t firstStatementOf(FlowInstanceID);
    void queueStatement(FlowStatementSite, BitVector& queuedStatements, size_t bit);
    std::pair<BitVector&, size_t> queuedBitOf(FlowStatementSite) LIFETIME_BOUND;
    void runQueuedStatement(FlowStatementSite);
    Member memberWaitingFor(FlowStatementSite, FlowWakeUp) const;
    void revisit(const Waiter&);
    void bringBack(const Waiter&);
    void processNode(FlowNodeID);
    void reach(FlowNodeID unseenCodeNode, const FlowObjectSet&, const Witness&);

    FlowCursorID numberedCursor(FlowStatementSite, uint32_t cursorInStatement);
    FlowCursorID cursorFor(FlowStatementSite, uint32_t cursorInStatement, FlowNodeID);
    FlowCursorID cursorFor(const FlowTemporaryKey&, FlowNodeID);
    void noteTakeInsideMember(FlowCursorID, FlowStatementSite);
    const ArrivalList& ensureArrivalList(FlowNodeID) LIFETIME_BOUND;
    FlowNewObjects takeObjects(FlowCursorID, FlowStatementSite, FlowNodeID);
    bool takePrimitives(FlowCursorID, FlowStatementSite, FlowNodeID);
    bool hasObjectsToTake(FlowCursorID) const;
    bool startOver(FlowCursorID);
    bool startOverCursorsOf(FlowStatementSite);
    void forEachKeyedCursorOf(FlowStatementSite site, NOESCAPE const Invocable<void(FlowCursorID)> auto& functor) const
    {
        std::optional<uint32_t> position = m_cursors.firstKeyedOfStatement.find(hashOf(site), [&](uint32_t candidate) {
            return m_cursors.keyed[candidate].key.site == site;
        });
        while (position) {
            const KeyedCursor& keyedCursor = m_cursors.keyed[*position];
            functor(keyedCursor.cursor);
            position = keyedCursor.nextOfStatement ? std::optional(keyedCursor.nextOfStatement) : std::nullopt;
        }
    }

    void initializeInstance(FlowInstanceID);
    void run(FlowStatementSite);
    void runStatement(FlowStatementSite);
    void runFilter(FlowStatementSite, const FlowFilter&);
    void runConditionalSeed(FlowStatementSite, const FlowConditionalSeed&);
    void runNaturalOperation(FlowStatementSite, const FlowNaturalOperation&);
    FlowValue widenGrowingRange(FlowStatementSite, const FlowValue& previous, FlowValue&& result);
    uint64_t numberOfChanges() const;
    void logCounters() const;

    void didCreateNode(FlowObjectID, PropertyKeyID, FlowNodeID);
    void revisitLookupsEndingAt(FlowNodeID location);
    void chargeHostCall(FlowStatementSite, uint64_t steps);
    bool isHostCallPastWorkLimit(FlowStatementSite) const;
    void abandon(FunctionID);
    void abandon(FlowInstanceID);
    void reachObjectsWaitedOnBy(FunctionID, size_t firstWaiter, const Witness&);
    void abandonHostCall(FlowStatementSite);

    const Program& m_program;
    FlowObjectTable& m_objects;
    FlowGraph& m_graph;
    BuildContext& m_build;
    FlowRecords m_records;
    FlowSolverStates m_states;
    FlowNaturalEvaluator m_evaluator;
    Worklist m_worklist;
    Waiters m_waiters;
    Cursors m_cursors;
    Counters m_counters;
    Member m_memberInProgress;
    QuietSteps m_quietSteps;
    bool m_isInLastRunOfInstance { false };
    FlowLookupWaits* m_waitsOfLookup { nullptr };
    uint32_t m_runNumber { 0 };
    Temporaries m_temporaries;
    BitVector m_gates;
    BitVector m_predictions;
    BitVector m_nodesWithoutObjectsLettingInUnseenCode;
    FlowObjectSet m_objectsLettingInUnseenCode;
    FlowObjectBitmap m_escapedObjects;
    Vector<uint64_t> m_workByFunction;
    HashMap<FlowStatementSite, uint64_t> m_workByHostCall;
    uint64_t m_maxWorkPerFunction;
    uint64_t m_maxEscapedObjectsBesideUnknown;
    FixpointCheck* m_fixpointCheck { nullptr };
    Vector<FlowFixpointChange> m_fixpointChanges;
    bool m_losesWakeUps { false };
    // Any order of the queued work leads to the same solution. A seed picks another order.
    std::optional<WeakRandom> m_shuffle;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
