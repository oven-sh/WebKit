/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowSolver.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTValueFlowBodies.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/CPUTime.h>
#include <wtf/MonotonicTime.h>
#include <wtf/OSAllocator.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t noSolverIndex = UINT32_MAX;
AOT_THRESHOLD(uint32_t, mostObjectsOfFew, 8);
AOT_THRESHOLD(uint32_t, mostObjectsListedByMember, 1024);
AOT_THRESHOLD(uint32_t, mostObjectsLookedUpOneByOne, 64);
static constexpr uint32_t mostNodesReportedOutsideFamily = 16;

bool ValueFlowPairSet::add(uint32_t first, uint32_t second)
{
    if ((m_size + 1) * 3 > m_slots.span().size() * 2)
        grow();
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
    size_t mask = m_slots.span().size() - 1;
    size_t position = key * 0x9e3779b97f4a7c15ull >> std::countl_zero(mask);
    for (; m_slots[position]; position = (position + 1) & mask) {
        if (m_slots[position] == key)
            return false;
    }
    m_slots[position] = key;
    ++m_size;
    return true;
}

bool ValueFlowPairSet::contains(uint32_t first, uint32_t second) const
{
    if (!m_slots)
        return false;
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
    size_t mask = m_slots.span().size() - 1;
    for (size_t position = key * 0x9e3779b97f4a7c15ull >> std::countl_zero(mask); m_slots[position]; position = (position + 1) & mask) {
        if (m_slots[position] == key)
            return true;
    }
    return false;
}

void ValueFlowPairSet::prefetch(uint32_t first, uint32_t second) const
{
    if (!m_slots)
        return;
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
    __builtin_prefetch(&m_slots[key * 0x9e3779b97f4a7c15ull >> std::countl_zero(m_slots.span().size() - 1)], 1);
}

void ValueFlowPairSet::grow()
{
    size_t size = std::max<size_t>(m_slots.span().size() * 2, 1024);
    // Written here: a page of zeroed memory fresh from the system that is read first costs a second fault at its first write.
    MallocSpan<uint64_t> previous = std::exchange(m_slots, MallocSpan<uint64_t>::malloc(size * sizeof(uint64_t)));
    zeroSpan(m_slots.mutableSpan());
    size_t mask = size - 1;
    for (uint64_t key : previous.span()) {
        if (!key)
            continue;
        size_t position = key * 0x9e3779b97f4a7c15ull >> std::countl_zero(mask);
        while (m_slots[position])
            position = (position + 1) & mask;
        m_slots[position] = key;
    }
}

struct ValueFlowSolver::Thread {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Thread);

    Thread(const ValueFlowSolver& solver, ValueSetMaker& maker)
        : context(solver, log, maker)
    {
    }

    ValueFlowNodeID numberInGraph(ValueFlowNodeID node) const
    {
        return ValueFlowRoundLog::isNumberedByLog(node) ? ValueFlowNodeID(numberOfMadeNode[node.index() & ~ValueFlowRoundLog::numberedByLog]) : node;
    }

    ValueFlowRoundLog log;
    ValueFlowRuleContext context;
    Seconds busyInTurn;
    Seconds longestShareInTurn;
    // Of the statements that have begun to wait in this round: where what they throw goes.
    Vector<ValueFlowNodeID> thrownOfStatementsThatWait;
    uint32_t numberOfDueStatementsThatWait { 0 };
    uint32_t numberOfDueStatementsTaken { 0 };
    // What the functions that this thread has reached bring: for the loop that runs, for the next turn, for the merge.
    Vector<ValueFlowStatementID> statementsFound;
    Vector<ValueFlowStatementID> dueInNextTurn;
    Vector<uint32_t> bodiesReached;
    uint32_t numberOfFunctionsReached { 0 };
    struct PartWatched {
        ValueFlowNodeID node;
        ValueSetID within;
        ValueFlowStatementID statement;
        bool isAskedLast { false };
    };
    // The parts that the log says are watched, for noteWatchers().
    std::array<Vector<PartWatched>, numberOfWatchShards> partsWatchedOfShard;
    // What it says is read, for noteReaders().
    std::array<Vector<NodeRead>, numberOfReaderShards> nodesReadOfShard;
    std::array<Vector<NodeRead>, numberOfReaderShards> nodesAskedWhetherEmptyOfShard;
    // By the log's number of a node: its number in the graph.
    Vector<uint32_t> numberOfMadeNode;
    // How much of the log the turns before this one have dealt with.
    size_t numberOfReachedTaken { 0 };
    size_t numberOfMadeTaken { 0 };
    size_t numberOfRunsTaken { 0 };
    size_t numberOfFunctionsAskedTaken { 0 };
    uint32_t numberOfValuesShownTaken { 0 };
    uint32_t numberOfNodesAskedForTaken { 0 };
    // Of the runs of the last turn that ask for something else than the last run of their statement, in the order of the runs: where what they ask for ends in the list.
    struct RunThatAsksAnew {
        size_t run { 0 };
        uint32_t endOfAskedFor { 0 };
        bool addsToEarlierRuns { false };
    };
};

static Vector<std::unique_ptr<ValueSetMaker>> makeMakers(ValueSets& sets, const ParallelLoops& loops)
{
    Vector<std::unique_ptr<ValueSetMaker>> makers(loops.width());
    forEachIndexInParallel(loops, makers.size(), [&](unsigned index) {
        makers[index] = makeUnique<ValueSetMaker>(sets);
    });
    return makers;
}

ValueFlowSolver::ValueFlowSolver(const ValueFlowSolverInput& input, ValueSets& sets, ValueFlowRule rule, const ParallelLoops& loops, Rounds rounds)
    : m_input(input)
    , m_sets(sets)
    , m_rule(rule)
    , m_loops(loops)
    , m_rounds(rounds)
    , m_makers(makeMakers(sets, loops))
    , m_graph(loops, m_makers.span())
{
    // A solve makes about two keys and two and a half nodes for each node of its input. Room that is asked for here and not used is never there; room that is doubled later is copied, on one thread.
    m_numberOfUsers.reserveCapacity(2 * static_cast<size_t>(m_input.numberOfNodes));
    m_askerThatKeepsNothingAwake.reserveCapacity(2 * static_cast<size_t>(m_input.numberOfNodes));
    m_partsOfMadeNodes.reserveCapacity(2 * static_cast<size_t>(m_input.numberOfNodes));
    m_madeNodesOfNode.reserveCapacity(5 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_firstReaderOfNode.reserveCapacity(7 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_seenByReadersOfNode.reserveCapacity(7 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_isWaitedForToHoldSomething.reserveCapacity(7 * static_cast<size_t>(m_input.numberOfNodes) / 2);
    m_madeStatements.reserveCapacity(m_input.statements.size() / 2);
    m_madeNodesOfStatement.reserveCapacity(m_input.statements.size() / 2);
    m_fewestSlotsOfIndexOfMadeNodes = std::max<size_t>(64, roundUpToPowerOfTwo(4 * static_cast<size_t>(m_input.numberOfNodes) / numberOfIndicesOfMadeNodes));

    ValueSetMaker& maker = *m_makers[0];
    const ValueFlowPlaceholders& placeholders = m_input.placeholders;
    m_graph.setPlaceholders(placeholders);
    ValueSetID unknownObjectAlone = placeholders.unknown ? maker.of(placeholders.unknown) : ValueSets::nothing;
    m_untrackedObjectAlone = placeholders.untracked ? maker.of(placeholders.untracked) : ValueSets::nothing;
    m_objectsThatPlaceholdersMayCover = m_input.universe ? m_input.universe->knownObjects() : ValueSets::complement(maker.unite(unknownObjectAlone, m_untrackedObjectAlone));
    m_objectsSaidToBeSomething = m_input.universe ? maker.intersect(m_objectsThatPlaceholdersMayCover, ValueSets::complement(m_input.universe->objectsThatMayBeAnything())) : m_objectsThatPlaceholdersMayCover;
    if (m_input.program && m_input.universe) {
        const BuiltinTable& builtins = m_input.program->builtins();
        Vector<ValueFlowObjectID, 32> objects;
        for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
            if (builtins.canonicalID(BuiltinID(index)) == BuiltinID(index) && builtins.letsInUnseenCode(BuiltinID(index)))
                m_input.universe->appendObjectsOf(BuiltinID(index), objects);
        }
        m_objectsAlwaysFollowed = maker.of(objects.span());
    }
    if (placeholders.objectsWithLayoutOfProgram)
        m_objectsOfProgram = placeholders.objectsWithLayoutOfProgram;
    if (placeholders.closures)
        m_objectsOfProgram = maker.unite(m_objectsOfProgram, placeholders.closures);
    if (m_input.universe) {
        ValueSetID keys = m_input.universe->keys();
        m_kindsThatCoverKeys.append({ ValueFlowUniverse::primitive(PrimitiveSet::String), ValueSets::complement(maker.intersect(m_input.universe->strings(), keys)) });
        m_kindsThatCoverKeys.append({ ValueFlowUniverse::primitive(PrimitiveSet::Symbol), ValueSets::complement(maker.intersect(m_input.universe->symbols(), keys)) });
    }

    // A node that hands on all it gets to what a placeholder covers, and holds the placeholder, need not list what the placeholder stands for.
    if (placeholders.unknown && m_input.escaped) {
        // The object on which the exports of a module of the library hang is kept and not searched when it escapes: what hangs on it has not escaped with it.
        m_coverings.append({ placeholders.unknown, m_input.escaped, maker.intersect(m_objectsThatPlaceholdersMayCover, ValueSets::complement(m_objectsAlwaysFollowed)) });
        // That a holder of exports or an evaluator has escaped keeps none of its members: the rules ask whether the operand lists it. So it is listed wherever it comes by ways that are followed, however late:
        // a retired node goes on taking it. What is read, looked up or called through the unknown object is a value nobody knows and lists nothing, however early.
        if (m_input.universe) {
            const ValueFlowHeapTables& heap = m_input.universe->builtinTables().heap();
            m_objectsLookedForBesideUnknownObject = maker.unite(m_input.universe->holdersOfLibraryExports(), maker.intersect(m_objectsAlwaysFollowed, heap.holdersWithMembers()));
        }
        m_graph.retireNodesCoveredBy(placeholders.unknown, m_input.escaped, m_input.universe ? maker.unite(m_input.universe->anyPrimitive(), unknownObjectAlone) : unknownObjectAlone, ValueSets::complement(m_objectsLookedForBesideUnknownObject));
        if (m_objectsLookedForBesideUnknownObject != ValueSets::nothing) {
            m_graph.keepOutOfNodesMadeForPlaceholders(m_objectsLookedForBesideUnknownObject, m_input.escaped, [](const void* solver, ValueFlowNodeID node) {
                return static_cast<const ValueFlowSolver*>(solver)->isMadeForUnknownObject(node);
            }, this);
        }
    }
    if (placeholders.untracked && m_input.lost) {
        m_coverings.append({ placeholders.untracked, m_input.lost, m_objectsThatPlaceholdersMayCover });
        m_graph.retireNodesCoveredBy(placeholders.untracked, m_input.lost, m_untrackedObjectAlone, maker.intersect(m_objectsThatPlaceholdersMayCover, ValueSets::complement(m_objectsAlwaysFollowed)));
        if (m_input.program && m_input.universe && m_input.nodes)
            prepareSaturation();
    }
    // The rules ask for each of these again and again, from every thread.
    constexpr size_t operandsInStretch = 1 << 16;
    Vector<Vector<uint32_t>> masksInStretches((m_input.operands.size() + operandsInStretch - 1) / operandsInStretch);
    forEachIndexInParallel(loops, masksInStretches.size(), [&](unsigned stretch) {
        Vector<uint32_t> masks;
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> isListed;
        size_t first = stretch * operandsInStretch;
        for (const ValueFlowOperand& operand : m_input.operands.subspan(first, std::min(operandsInStretch, m_input.operands.size() - first))) {
            if (operand.maskKind == ValueFlowMaskKind::Plain || !operand.mask || operand.mask == ValueSets::anything)
                continue;
            if (isListed.add(operand.mask.index()).isNewEntry)
                masks.append(operand.mask.index());
        }
        masksInStretches[stretch] = WTF::move(masks);
    });
    Vector<uint32_t> masksOfOperands;
    for (const Vector<uint32_t>& masks : masksInStretches)
        masksOfOperands.appendVector(masks);
    sortAndRemoveDuplicates(loops, masksOfOperands);
    Vector<ValueSetID> familyObjectsOfMasks(masksOfOperands.size());
    forEachIndexInParallel(loops, masksOfOperands.size(), [&](unsigned index, unsigned worker) {
        familyObjectsOfMasks[index] = findFamilyObjectsOf(ValueSetID(masksOfOperands[index]), *m_makers[worker]);
    });
    for (size_t index = 0; index < masksOfOperands.size(); ++index)
        m_familyObjectsOfMask.add(masksOfOperands[index], familyObjectsOfMasks[index]);
    for (unsigned written = 0; written < mostKindsOfNames; ++written) {
        for (unsigned read = 0; read < mostKindsOfNames; ++read) {
            if (!mayBeTheSameName(static_cast<ValueFlowNameKind>(written), static_cast<ValueFlowNameKind>(read)))
                continue;
            m_kindsOfNames[written].readKindsThatMayFindIt |= 1ull << read;
            m_kindsOfNames[read].writtenKindsItMayFind |= 1ull << written;
        }
    }
    for (const std::unique_ptr<ValueSetMaker>& makerOfThread : m_makers)
        m_threads.append(makeUnique<Thread>(*this, *makerOfThread));
    m_isReached.growTo(m_input.ranges.size());
    m_reachedBy.grow(m_input.ranges.size());
    m_isDue.growTo(m_input.statements.size());
    m_hasStatementOfBodyRun.growTo(m_input.statements.size());
    m_statementsWait = m_input.universe && m_input.nodes && m_input.placeholders.unknown;
    if (m_statementsWait) {
        growAndFill(m_firstStatementWaitingForNode, m_input.numberOfNodes, 0u);
        growAndFill(m_nextStatementWaiting, m_input.statements.size(), doesNotWait);
    }
}

ALWAYS_INLINE ValueFlowNodeID ValueFlowSolver::nodeAwaitedBy(ValueFlowStatementID id, const ValueFlowStatement& statement) const
{
    if (!m_statementsWait || id.index() >= m_input.statements.size() || statement.kind != ValueFlowStatementKind::Operation || !statement.operandCount)
        return { };
    if (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::Call && statement.opcode != Opcode::Construct)
        return { };
    const ValueFlowOperand& first = m_input.operands[statement.firstOperand];
    if (!first.node || (first.constant && first.constant != ValueSets::nothing) || m_graph.valueOf(first.node) != ValueSets::nothing)
        return { };
    // The statements of such a function run on the nodes of its instances.
    if (m_input.instancedFunctions && m_input.instancedFunctions->find(statement.function))
        return { };
    return first.node;
}

ValueFlowSolver::~ValueFlowSolver() = default;

template<typename T>
ListThatStaysInPlace<T>::~ListThatStaysInPlace()
{
    if (!m_elements)
        return;
    OSAllocator::decommit(m_elements, m_numberWithMemory.load(std::memory_order_relaxed) * sizeof(T));
    OSAllocator::releaseDecommitted(m_elements, mostElements * sizeof(T), 0);
}

template<typename T>
void ListThatStaysInPlace<T>::takeMemoryFor(size_t size)
{
    constexpr size_t elementsPerStep = 1u << 16;
    static_assert(!(elementsPerStep * sizeof(T) % (64 * KB)) && !(mostElements % elementsPerStep));
    RELEASE_ASSERT(size <= mostElements);
    Locker locker { m_lock };
    if (!m_elements)
        m_elements = static_cast<T*>(OSAllocator::reserveUncommitted(mostElements * sizeof(T), OSAllocator::FastMallocPages));
    size_t withMemory = m_numberWithMemory.load(std::memory_order_relaxed);
    if (size <= withMemory)
        return;
    size_t end = roundUpToMultipleOf<elementsPerStep>(size);
    OSAllocator::commit(m_elements + withMemory, (end - withMemory) * sizeof(T), true, false);
    m_numberWithMemory.store(end, std::memory_order_release);
}

ValueFlowSolver::IndexOfMadeNodes::~IndexOfMadeNodes()
{
    for (Table table : tables)
        fastFree(table.slots());
}

void ValueFlowSolver::IndexOfMadeNodes::add(std::atomic<Table>& current, uint32_t hash, uint32_t number, size_t fewestSlots)
{
    auto slotsOf = [](Table table) {
        return unsafeMakeSpan(table.slots(), table.numberOfSlots());
    };
    auto put = [&](Table table, uint64_t slot) {
        std::span<Slot> slots = slotsOf(table);
        size_t mask = slots.size() - 1;
        size_t index = (slot >> 32) & mask;
        while (slots[index].load(std::memory_order_relaxed))
            index = (index + 1) & mask;
        slots[index].store(slot, std::memory_order_release);
    };
    if (tables.isEmpty() || (size + 1) * 2 > tables.last().numberOfSlots()) {
        size_t numberOfSlots = tables.isEmpty() ? fewestSlots : tables.last().numberOfSlots() * 2;
        Table larger { static_cast<Slot*>(fastAlignedMalloc(64, numberOfSlots * sizeof(Slot))), static_cast<unsigned>(std::countr_zero(numberOfSlots)) };
        // Written, not asked for as zeroes: a page that is read before it is written costs a second fault, which stops every thread.
        zeroSpan(slotsOf(larger));
        if (!tables.isEmpty()) {
            for (const Slot& slot : slotsOf(tables.last())) {
                if (uint64_t taken = slot.load(std::memory_order_relaxed))
                    put(larger, taken);
            }
        }
        tables.append(larger);
        current.store(larger, std::memory_order_release);
    }
    put(tables.last(), static_cast<uint64_t>(hash) << 32 | (number + 1));
    ++size;
}

void ValueFlowSolver::IndexOfMadeNodes::freeOutgrownTables()
{
    while (tables.size() > 1) {
        fastFree(tables.first().slots());
        tables.removeAt(0);
    }
}

ALWAYS_INLINE std::optional<uint32_t> ValueFlowSolver::findNumberOfMadeNodes(const ValueFlowNodeKey& key) const
{
    uint32_t hash = key.hash();
    return findInIndexOfMadeNodes(hash, [&](uint32_t candidate) {
        return m_madeNodes[candidate].key == key;
    });
}

std::optional<ValueFlowNodeID> ValueFlowSolver::findMadeNode(const ValueFlowNodeKey& key) const
{
    std::optional<uint32_t> number = findNumberOfMadeNodes(key);
    if (!number)
        return std::nullopt;
    return m_madeNodes[*number].first;
}

std::optional<ValueFlowNodeID> ValueFlowSolver::findNodeMadeBeforeThisTurn(const ValueFlowNodeKey& key) const
{
    std::optional<uint32_t> number = findNumberOfMadeNodes(key);
    if (!number || m_madeNodes[*number].isOfThisTurn)
        return std::nullopt;
    return m_madeNodes[*number].first;
}

bool ValueFlowSolver::isAtRest(ValueFlowStatementID statement) const
{
    if (statement.index() < m_input.statements.size())
        return false;
    uint32_t madeNodes = m_madeNodesOfStatement[statement.index() - m_input.statements.size()];
    return madeNodes != noSolverIndex && !m_numberOfUsers[madeNodes];
}

uint64_t ValueFlowSolver::contentOf(ValueFlowStatementID statement) const
{
    if (!isMadeByRule(statement))
        return statement.index();
    return contentOf(keyOf(statement));
}

uint64_t ValueFlowSolver::contentOf(const ValueFlowNodeKey& key) const
{
    uint64_t content = (static_cast<uint64_t>(key.kind) << 40 | static_cast<uint64_t>(key.detail) << 32 | key.payload) * 0x9e3779b97f4a7c15ull;
    content = (content ^ content >> 29 ^ (key.set ? m_sets.hashOfMembers(key.set) : 0)) * 0xbf58476d1ce4e5b9ull;
    return content | 1ull << 63;
}

void ValueFlowSolver::makeDue(ValueFlowStatementID statement)
{
    if (isAtRest(statement) || m_isDue.set(statement.index()))
        return;
    if (hasLongRowBehindIt(statement))
        m_numberDueWithLongRows.fetch_add(1, std::memory_order_relaxed);
    m_due.append(statement);
}

void ValueFlowSolver::listIfItMayBecomeDue(ValueFlowStatementID statement, Vector<ValueFlowStatementID>& list) const
{
    // On any thread: whoever marks it lists it, so no two lists hold one statement.
    if (isAtRest(statement) || m_isDue.get(statement.index()) || m_isDue.set(statement.index()))
        return;
    list.append(statement);
    if (hasLongRowBehindIt(statement))
        m_numberDueWithLongRows.fetch_add(1, std::memory_order_relaxed);
}

void ValueFlowSolver::makeStatementsDue(uint32_t first, uint32_t end)
{
    if (m_due.size() + (end - first) > m_due.capacity())
        m_due.reserveCapacity(std::max<size_t>({ 1024, m_due.capacity() * 2, m_due.size() + (end - first) }));
    for (uint32_t statement = first; statement < end; ++statement)
        makeDue(ValueFlowStatementID(statement));
}

void ValueFlowSolver::makeEveryStatementDue()
{
    for (uint32_t body = 0; body + 1 < m_input.ranges.size(); ++body)
        makeStatementsDue(m_input.ranges[body].statement, m_isReached.get(body) ? m_input.ranges[body + 1].statement : m_input.ranges[body].statementLeavingBody);
    makeStatementsDue(m_input.statements.size(), numberOfStatements());
}

bool ValueFlowSolver::reach(uint32_t firstBody, ValueFlowReachWitness witness)
{
    if (m_isReached.get(firstBody))
        return false;
    Vector<uint32_t, 16> bodies { firstBody };
    m_isReached.set(firstBody);
    witness.round = m_counters.rounds;
    m_reachedBy[firstBody] = witness;
    while (!bodies.isEmpty()) {
        uint32_t body = bodies.takeLast();
        ++m_counters.functionsReached;
        m_bodiesReachedInRound.append(body);
        makeStatementsDue(m_input.ranges[body].statementLeavingBody, m_input.ranges[body + 1].statement);
        auto readers = m_readersOfFunction.find(body);
        if (readers != m_readersOfFunction.end()) {
            for (ValueFlowStatementID statement : readers->value)
                makeDue(statement);
        }
        for (FunctionID entered : m_input.functionsEnteredOnceReached.subspan(m_input.ranges[body].functionEntered, m_input.ranges[body + 1].functionEntered - m_input.ranges[body].functionEntered)) {
            if (!m_isReached.set(entered.index())) {
                m_reachedBy[entered.index()] = ValueFlowReachWitness { { }, body, m_counters.rounds };
                bodies.append(entered.index());
            }
        }
    }
    return true;
}

template<typename T, size_t inlineCapacity>
static ALWAYS_INLINE void makeRoomByDoubling(Vector<T, inlineCapacity>& list, size_t more = 1)
{
    if (list.size() + more > list.capacity()) [[unlikely]]
        list.reserveCapacity(std::max<size_t>({ 1024, list.capacity() * 2, list.size() + more }));
}

template<typename T>
static ALWAYS_INLINE void growByDoubling(Vector<T>& list, size_t size, T value)
{
    if (size <= list.size())
        return;
    makeRoomByDoubling(list, size - list.size());
    list.insertFill(list.size(), value, size - list.size());
}

template<typename T>
void ValueFlowSolver::growAndFill(ListWithRoomToFill<T>& list, size_t size, T value)
{
    size_t before = list.size();
    if (size <= before)
        return;
    list.makeRoomToFill(size - before);
    std::span<T> room = list.mutableSpan().subspan(before);
    constexpr size_t elementsPerShare = 1 << 15;
    forEachIndexInParallel(m_loops, (room.size() + elementsPerShare - 1) / elementsPerShare, [&](unsigned share) {
        std::ranges::fill(room.subspan(share * elementsPerShare, std::min(elementsPerShare, room.size() - share * elementsPerShare)), value);
    });
}

static ALWAYS_INLINE void growByDoubling(ValueFlowSharedBits& bits, size_t size)
{
    if (size > bits.size()) [[unlikely]]
        bits.growTo(std::max(size, bits.size() * 2));
}

static ALWAYS_INLINE void growByDoubling(BitVector& bits, size_t size)
{
    if (size > bits.size()) [[unlikely]]
        bits.ensureSize(std::max(size, bits.size() * 2));
}

void ValueFlowSolver::StepTimes::begin()
{
    if (!isOn)
        return;
    startOfStep = MonotonicTime::now();
    cpuAtStartOfStep = CPUTime::forCurrentThread();
}

void ValueFlowSolver::StepTimes::didFinish(ASCIILiteral name)
{
    if (!isOn)
        return;
    MonotonicTime now = MonotonicTime::now();
    Seconds wall = now - std::exchange(startOfStep, now);
    Seconds cpuNow = CPUTime::forCurrentThread();
    Seconds cpu = cpuNow - std::exchange(cpuAtStartOfStep, cpuNow);
    for (Step& step : steps) {
        if (step.name.characters() == name.characters()) {
            step.wall += wall;
            step.cpuOfThisThread += cpu;
            return;
        }
    }
    steps.append(Step { name, wall, cpu });
}

void ValueFlowSolver::StepTimes::dump() const
{
    if (!isOn)
        return;
    for (const Step& step : steps)
        dataLogLn("AOT flow step | ", step.name, " | ", step.wall.milliseconds(), " ms | this thread's CPU ", step.cpuOfThisThread.milliseconds(), " ms");
    dataLogLn("AOT flow loop over due statements | busy on all threads ", busyOnAllThreads.milliseconds(), " ms | the busiest thread of each turn ", busiestThreadOfTurn.milliseconds(), " ms | the longest share of each turn ", longestShareOfTurn.milliseconds(), " ms");
}

// A statement that runs marks itself as run before it stops being due, and whoever makes it due looks in the other order: so it either sees what has happened or runs again in the next turn.
void ValueFlowSolver::makeDueInRunningLoop(Thread& thread, ValueFlowStatementID statement)
{
    if (isAtRest(statement) || m_isDue.setInOrder(statement.index()))
        return;
    (m_hasRunInTurn.get(statement.index()) ? thread.dueInNextTurn : thread.statementsFound).append(statement);
}

void ValueFlowSolver::reachFunctionsFoundBy(Thread& thread)
{
    const ValueFlowRoundLog& log = thread.log;
    for (; thread.numberOfReachedTaken < log.reached.size(); ++thread.numberOfReachedTaken) {
        uint32_t firstBody = log.reached[thread.numberOfReachedTaken].index();
        if (m_isReached.get(firstBody) || m_isReached.setInOrder(firstBody))
            continue;
        m_reachedBy[firstBody] = ValueFlowReachWitness { log.whoReached[thread.numberOfReachedTaken].statement, noSolverIndex, m_counters.rounds };
        Vector<uint32_t, 16> bodies { firstBody };
        while (!bodies.isEmpty()) {
            uint32_t body = bodies.takeLast();
            ++thread.numberOfFunctionsReached;
            thread.bodiesReached.append(body);
            for (uint32_t statement = m_input.ranges[body].statementLeavingBody; statement < m_input.ranges[body + 1].statement; ++statement)
                makeDueInRunningLoop(thread, ValueFlowStatementID(statement));
            auto readers = m_readersOfFunction.find(body);
            if (readers != m_readersOfFunction.end()) {
                for (ValueFlowStatementID statement : readers->value)
                    makeDueInRunningLoop(thread, statement);
            }
            for (FunctionID entered : m_input.functionsEnteredOnceReached.subspan(m_input.ranges[body].functionEntered, m_input.ranges[body + 1].functionEntered - m_input.ranges[body].functionEntered)) {
                if (!m_isReached.setInOrder(entered.index())) {
                    m_reachedBy[entered.index()] = ValueFlowReachWitness { { }, body, m_counters.rounds };
                    bodies.append(entered.index());
                }
            }
        }
    }
}

// The statements of a function that a run reaches join the loop. A further lap runs only what few threads found when the others had gone.
void ValueFlowSolver::runDueStatements(bool isFirstLapOfTurn)
{
    AOT_THRESHOLD(size_t, mostStatementsPerShare, 32);
    AOT_THRESHOLD(size_t, mostFoundLeftToOneThread, 256);
    constexpr size_t sharesPerThread = 4;
    Vector<ValueFlowStatementID> due = std::exchange(m_due, Vector<ValueFlowStatementID>());
    growByDoubling(m_hasRunInTurn, numberOfStatements());
    // One dear statement costs as much as thousands of others. Each has a thread to itself from the start, and the others fill the threads beside them. Who runs what makes no difference to what is logged.
    // Under a seed, and behind long rows, some that are due wait outside the list.
    Vector<ValueFlowStatementID, 16> dearStatements;
    for (ValueFlowStatementID statement : m_dearStatements) {
        if (m_isDue.get(statement.index()) && !m_hasRunInTurn.get(statement.index()) && ((!m_seedOfSchedule && m_dueBehindLongRows.isEmpty()) || due.contains(statement)))
            dearStatements.append(statement);
    }
    growAndFill(m_rememberedByStatement, numberOfStatements(), ValueSetID());
    uint64_t edgesLogged = m_counters.edgesLogged;
    for (const std::unique_ptr<Thread>& thread : m_threads)
        edgesLogged += thread->log.edges.size();
    // Where sets can be given up no edge is dropped: which edges a thread drops depends on what else it has run.
    for (const std::unique_ptr<Thread>& thread : m_threads)
        thread->log.mostEdges = m_saturates ? UINT64_MAX : thread->log.edges.size() + (m_input.mostEdgesLogged - std::min(edgesLogged, m_input.mostEdgesLogged));
    size_t statementsPerShare = std::clamp<size_t>(due.size() / (m_threads.size() * sharesPerThread), 1, mostStatementsPerShare);
    if (isFirstLapOfTurn)
        letRulesMakeNodes();
    m_stepTimes.didFinish("rules: in front of the loop"_s);
    size_t numberOfShares = dearStatements.size() + (due.size() + statementsPerShare - 1) / statementsPerShare;
    alignas(128) std::atomic<size_t> nextShare { 0 };
    alignas(128) std::atomic<size_t> numberFound { 0 };
    alignas(128) std::atomic<unsigned> numberAtWork { 0 };
    Lock lockOfFound;
    Vector<ValueFlowStatementID> found;
    forEachIndexInParallel(m_loops, static_cast<unsigned>(std::min<size_t>(m_threads.size(), numberOfShares)), [&](unsigned, unsigned worker) {
        Thread& thread = *m_threads[worker];
        ValueFlowRuleContext& context = thread.context;
        numberAtWork.fetch_add(1, std::memory_order_relaxed);
        Vector<ValueFlowStatementID, 32> taken;
        for (;;) {
            std::span<const ValueFlowStatementID> toRun;
            bool isDear = false;
            bool isOfDueList = false;
            size_t share = nextShare.load(std::memory_order_relaxed);
            if (share < numberOfShares)
                share = nextShare.fetch_add(1, std::memory_order_relaxed);
            if (share < numberOfShares) {
                isDear = share < dearStatements.size();
                isOfDueList = true;
                size_t first = isDear ? share : (share - dearStatements.size()) * statementsPerShare;
                toRun = isDear ? dearStatements.span().subspan(first, 1) : due.subspan(first, std::min(statementsPerShare, due.size() - first));
            } else {
                if (!numberFound.load(std::memory_order_relaxed))
                    break;
                Locker locker { lockOfFound };
                if (unsigned atWork = numberAtWork.load(std::memory_order_relaxed); found.size() > mostFoundLeftToOneThread * atWork && atWork * 2 <= m_threads.size())
                    break;
                size_t count = std::min<size_t>(found.size(), mostStatementsPerShare);
                if (!count)
                    break;
                taken.shrink(0);
                taken.append(found.span().last(count));
                found.shrink(found.size() - count);
                numberFound.store(found.size(), std::memory_order_relaxed);
                toRun = taken.span();
            }
            MonotonicTime startOfShare = m_stepTimes.isOn ? MonotonicTime::now() : MonotonicTime();
            for (ValueFlowStatementID statement : toRun) {
                if (isOfDueList && !isDear && !dearStatements.isEmpty() && m_isDear.get(statement.index()))
                    continue;
                if (m_hasRunInTurn.setInOrder(statement.index())) {
                    thread.dueInNextTurn.append(statement);
                    continue;
                }
                m_isDue.clearInOrder(statement.index());
                if (statement.index() < m_isDueLast.size() && m_isDueLast.get(statement.index()))
                    m_isDueLast.clear(statement.index());
                if (isAtRest(statement))
                    continue;
                ++thread.numberOfDueStatementsTaken;
                if (thread.log.hasDroppedEdges)
                    break;
                const ValueFlowStatement& statementToRun = statementAt(statement);
                if (ValueFlowNodeID awaited = nodeAwaitedBy(statement, statementToRun)) {
                    ++thread.numberOfDueStatementsThatWait;
                    m_hasStatementOfBodyRun.set(statement.index());
                    uint32_t& next = m_nextStatementWaiting[statement.index()];
                    if (next != doesNotWait)
                        continue;
                    next = WTF::atomicExchange(&m_firstStatementWaitingForNode[awaited.index()], statement.index() + 1);
                    if (statementToRun.thrown)
                        thread.thrownOfStatementsThatWait.append(statementToRun.thrown);
                    continue;
                }
                context.begin(statement);
                m_rule(statementToRun, context);
                context.end();
                if (statement.index() < m_input.statements.size())
                    m_hasStatementOfBodyRun.set(statement.index());
                if (m_statementsWait && statement.index() < m_input.statements.size())
                    m_nextStatementWaiting[statement.index()] = doesNotWait;
                runStatementsMadeBy(thread);
                runStatementsWokenBy(thread);
                reachFunctionsFoundBy(thread);
            }
            if (m_stepTimes.isOn) {
                Seconds wallOfShare = MonotonicTime::now() - startOfShare;
                thread.busyInTurn += wallOfShare;
                thread.longestShareInTurn = std::max(thread.longestShareInTurn, wallOfShare);
            }
            if (thread.log.hasDroppedEdges)
                break;
            if (!thread.statementsFound.isEmpty()) {
                Locker locker { lockOfFound };
                found.appendVector(thread.statementsFound);
                numberFound.store(found.size(), std::memory_order_relaxed);
                thread.statementsFound.shrink(0);
            }
        }
        numberAtWork.fetch_sub(1, std::memory_order_relaxed);
    });
    m_due = WTF::move(found);
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        m_counters.statementRuns += std::exchange(thread->numberOfDueStatementsTaken, 0u);
        m_counters.statementRuns += std::exchange(thread->log.numberOfRunsWhereMade, 0u);
        m_counters.statementRuns -= std::exchange(thread->numberOfDueStatementsThatWait, 0u);
        m_counters.functionsReached += std::exchange(thread->numberOfFunctionsReached, 0u);
        m_due.appendVector(thread->statementsFound);
        thread->statementsFound.shrink(0);
        m_dueInNextTurn.appendVector(thread->dueInNextTurn);
        thread->dueInNextTurn.shrink(0);
        m_bodiesReachedInRound.appendVector(thread->bodiesReached);
        thread->bodiesReached.shrink(0);
    }
    m_stepTimes.didFinish("rules: the loop over due statements"_s);
    if (m_stepTimes.isOn) {
        Seconds busiest;
        Seconds longest;
        for (const std::unique_ptr<Thread>& thread : m_threads) {
            m_stepTimes.busyOnAllThreads += thread->busyInTurn;
            busiest = std::max(busiest, std::exchange(thread->busyInTurn, 0_s));
            longest = std::max(longest, std::exchange(thread->longestShareInTurn, 0_s));
        }
        m_stepTimes.busiestThreadOfTurn += busiest;
        m_stepTimes.longestShareOfTurn += longest;
    }
}

void ValueFlowSolver::findStatementsWithLongRowsBehindThem()
{
    AOT_THRESHOLD(unsigned, fewestStatementsOfLongRow, 16);
    if (!m_statementsWait)
        return;
    Vector<uint32_t> producerOfNode;
    producerOfNode.fill(noSolverIndex, m_input.numberOfNodes);
    Vector<uint16_t> lengthBehind;
    lengthBehind.fill(static_cast<uint16_t>(0), m_input.statements.size());
    uint32_t numberOfBodies = m_input.ranges.size() - 1;
    constexpr uint32_t bodiesPerShare = 512;
    Vector<Vector<uint32_t>> foundOfShare((numberOfBodies + bodiesPerShare - 1) / bodiesPerShare);
    forEachIndexInParallel(m_loops, foundOfShare.size(), [&](unsigned share) {
        for (uint32_t body = share * bodiesPerShare; body < std::min(numberOfBodies, (share + 1) * bodiesPerShare); ++body) {
            uint32_t first = m_input.ranges[body].statement;
            uint32_t end = m_input.ranges[body + 1].statement;
            if (end - first <= fewestStatementsOfLongRow)
                continue;
            for (uint32_t index = first; index < end; ++index) {
                if (ValueFlowNodeID result = m_input.statements[index].result; result && result.index() < m_input.numberOfNodes)
                    producerOfNode[result.index()] = index;
            }
            for (uint32_t index = end; index-- > first;) {
                const ValueFlowStatement& statement = m_input.statements[index];
                if (lengthBehind[index] >= fewestStatementsOfLongRow)
                    foundOfShare[share].append(index);
                // As in nodeAwaitedBy().
                if (statement.kind != ValueFlowStatementKind::Operation || !statement.operandCount || (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::Call && statement.opcode != Opcode::Construct))
                    continue;
                const ValueFlowOperand& operand = m_input.operands[statement.firstOperand];
                if (!operand.node || operand.node.index() >= m_input.numberOfNodes || (operand.constant && operand.constant != ValueSets::nothing))
                    continue;
                uint32_t producer = producerOfNode[operand.node.index()];
                if (producer == noSolverIndex || producer < first || producer >= index)
                    continue;
                lengthBehind[producer] = std::max<uint16_t>(lengthBehind[producer], static_cast<uint16_t>(std::min<unsigned>(lengthBehind[index] + 1, UINT16_MAX)));
            }
        }
    });
    for (const Vector<uint32_t>& found : foundOfShare) {
        for (uint32_t index : found) {
            if (!std::exchange(m_hasLongRows, true))
                m_hasLongRowBehindIt.ensureSize(m_input.statements.size());
            m_hasLongRowBehindIt.quickSet(index);
        }
    }
}

// Whatever else is due waits until no statement with a long row behind it is.
void ValueFlowSolver::letLongRowsGoFirst()
{
    if (m_numberDueWithLongRows.exchange(0, std::memory_order_relaxed)) {
        size_t numberThatRun = 0;
        size_t numberThatWaited = m_dueBehindLongRows.size();
        makeRoomByDoubling(m_dueBehindLongRows, m_due.size());
        for (ValueFlowStatementID statement : m_due) {
            if (hasLongRowBehindIt(statement))
                m_due[numberThatRun++] = statement;
            else
                m_dueBehindLongRows.append(statement);
        }
        if (numberThatRun) {
            m_due.shrink(numberThatRun);
            return;
        }
        m_dueBehindLongRows.shrink(numberThatWaited);
    }
    if (m_dueBehindLongRows.isEmpty())
        return;
    if (m_due.size() > m_dueBehindLongRows.size())
        std::swap(m_due, m_dueBehindLongRows);
    m_dueBehindLongRows.appendVector(m_due);
    m_due = std::exchange(m_dueBehindLongRows, { });
}

// About half of the due statements, chosen by the seed, wait for a later round. Nothing is dropped: whoever waits stays due, and the solve does not end while a statement waits.
void ValueFlowSolver::letSomeDueStatementsWait()
{
    m_due.appendVector(std::exchange(m_dueStatementsThatWait, { }));
    size_t numberThatRun = 0;
    for (ValueFlowStatementID statement : m_due) {
        uint64_t hash = (m_seedOfSchedule * 0x9E3779B97F4A7C15ull) ^ (contentOf(statement) * 0xC2B2AE3D27D4EB4Full) ^ (m_counters.rounds * 0x165667B19E3779F9ull);
        hash ^= hash >> 29;
        hash *= 0xBF58476D1CE4E5B9ull;
        hash ^= hash >> 32;
        if (hash & 1)
            m_dueStatementsThatWait.append(statement);
        else
            m_due[numberThatRun++] = statement;
    }
    if (numberThatRun)
        m_due.shrink(numberThatRun);
    else
        m_dueStatementsThatWait.shrink(0);
}

bool ValueFlowSolver::runRules()
{
    for (m_turnsOfRound = 0; !m_due.isEmpty(); ++m_turnsOfRound) {
        ++m_numberOfTurn;
        for (bool isFirstLapOfTurn = true; !m_due.isEmpty(); isFirstLapOfTurn = false) {
            runDueStatements(isFirstLapOfTurn);
            uint64_t edgesLogged = m_counters.edgesLogged;
            bool hasDroppedEdges = false;
            for (const std::unique_ptr<Thread>& thread : m_threads) {
                edgesLogged += thread->log.edges.size();
                hasDroppedEdges |= thread->log.hasDroppedEdges;
            }
            if (hasDroppedEdges || edgesLogged > m_input.mostEdgesLogged) {
                if (!m_saturates)
                    return false;
                m_counters.isPastEdgeBudget = true;
            }
        }
        m_due = std::exchange(m_dueInNextTurn, { });
        m_hasRunInTurn.clearAll();
        takeWhatRulesHaveMade();
        for (const std::unique_ptr<Thread>& thread : m_threads) {
            thread->log.recordsOfRunsWhereMade.shrink(0);
        }
        m_stepTimes.didFinish("rules: what the rules have made"_s);
        numberMadeNodes();
        m_stepTimes.didFinish("rules: made nodes numbered"_s);
        noteFunctionsAsked();
        m_stepTimes.didFinish("rules: functions asked"_s);
        noteNodesAskedFor();
        m_stepTimes.didFinish("rules: nodes asked for"_s);
    }
    return true;
}


// Makes the nodes of some keys beside the other makers. What it makes has the numbers that follow what was there before all of them; takeWhatWasMade() moves them up behind the makers before it.
class ValueFlowSolver::NodeMaker {
    WTF_MAKE_NONCOPYABLE(NodeMaker);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NodeMaker);
public:
    // The nodes of `key` are wanted by those of `madeNodes`: they are its parts once they are there.
    struct Wish {
        uint32_t madeNodes { 0 };
        ValueFlowNodeKey key;
        uint32_t granted { noSolverIndex };
    };

    struct Masked {
        uint32_t madeNodes { 0 };
        ValueSetID familyObjects;
    };

    explicit NodeMaker(ValueFlowSolver& solver)
        : m_solver(solver)
        , m_sets(solver.m_sets)
        , m_firstMadeNodes(solver.m_madeNodes.size())
        , m_firstNode(solver.m_graph.numberOfNodes())
        , m_firstStatement(solver.numberOfStatements())
        , m_firstPart(solver.m_partsOfMadeNodes.size())
    {
    }

    static unsigned makerOfName(uint64_t name) { return static_cast<unsigned>(name * 0x9e3779b97f4a7c15ull >> 58); }
    static uint64_t nameOf(const ValueFlowNodeKey& key) { return static_cast<uint64_t>(key.kind) << 40 | static_cast<uint64_t>(key.detail) << 32 | key.payload; }
    static bool readsOrWrites(ValueFlowMadeNodeKind kind)
    {
        return kind == ValueFlowMadeNodeKind::Read || kind == ValueFlowMadeNodeKind::Written || kind == ValueFlowMadeNodeKind::ReadUnderKind || kind == ValueFlowMadeNodeKind::WrittenUnderKind;
    }
    // Only a node that reads or writes many objects brings others with it, and those have its name: see makeUnionWithEarlierNode().
    static bool mayBringOthers(const ValueSets& sets, const ValueFlowNodeKey& key)
    {
        return readsOrWrites(key.kind) && !ValueSets::isComplement(key.set) && sets.size(key.set) > mostObjectsLookedUpOneByOne;
    }
    static unsigned makerOf(const ValueSets& sets, const KeyToMake& toMake)
    {
        return makerOfName(mayBringOthers(sets, toMake.key) ? nameOf(toMake.key) : nameOf(toMake.key) ^ toMake.hashOfMembers);
    }

    // The keys of a name stand in a row.
    void run(std::span<KeyToMake> all, ValueSetMaker& maker, bool mayBeThereAsParts = false)
    {
        m_maker = &maker;
        for (uint32_t index : keys) {
            KeyToMake& toMake = all[index];
            if (mayBeThereAsParts && readsOrWrites(toMake.key.kind)) {
                if (std::optional<uint32_t> existing = m_solver.findNumberOfMadeNodes(toMake.key)) {
                    toMake.madeNodes = *existing;
                    continue;
                }
            }
            if (readsOrWrites(toMake.key.kind)) {
                if (uint64_t name = nameOf(toMake.key); name != m_name) {
                    m_name = name;
                    m_firstOfName = madeNodes.size();
                    m_hasMadeMoreOfName = false;
                }
                if (m_hasMadeMoreOfName) {
                    if (std::optional<uint32_t> existing = findAmongNew(toMake.key)) {
                        toMake.madeNodes = *existing;
                        continue;
                    }
                }
            }
            toMake.madeNodes = makeNodes(toMake.key);
        }
    }

    uint32_t movedUp(uint32_t number, uint32_t first, uint32_t distance) const { return number < first ? number : number + distance; }
    uint32_t madeNodesMovedUp(uint32_t number) const { return movedUp(number, m_firstMadeNodes, madeNodesBefore); }
    ValueFlowNodeID nodeMovedUp(ValueFlowNodeID node) const { return node ? ValueFlowNodeID(movedUp(node.index(), m_firstNode, nodesBefore)) : node; }
    ValueFlowStatementID statementMovedUp(ValueFlowStatementID statement) const { return statement ? ValueFlowStatementID(movedUp(statement.index(), m_firstStatement, statementsBefore)) : statement; }
    uint32_t partMovedUp(uint32_t position) const { return movedUp(position, m_firstPart, partsBefore); }

    // Positions among all the keys to make.
    Vector<uint32_t> keys;
    Vector<MadeNodes> madeNodes;
    uint32_t numberOfNodes { 0 };
    Vector<MadeStatement> madeStatements;
    Vector<uint32_t> madeNodesOfStatement;
    Vector<uint32_t> parts;
    Vector<ValueFlowEdge> edges;
    Vector<NodeToLink> readToLink;
    Vector<NodeToLink> writtenToLink;
    Vector<Wish> wishes;
    Vector<uint32_t> constants;
    Vector<uint32_t> instances;
    Vector<uint32_t> stayingAtRest;
    Vector<Masked> masked;

    // What the makers before this one have made.
    uint32_t madeNodesBefore { 0 };
    uint32_t nodesBefore { 0 };
    uint32_t statementsBefore { 0 };
    uint32_t partsBefore { 0 };
    size_t edgesBefore { 0 };
    size_t readToLinkBefore { 0 };
    size_t writtenToLinkBefore { 0 };
    size_t wishesBefore { 0 };

private:
    const MadeNodes& at(uint32_t number) const LIFETIME_BOUND { return number < m_firstMadeNodes ? m_solver.m_madeNodes[number] : madeNodes[number - m_firstMadeNodes]; }
    MadeNodes& newAt(uint32_t number) LIFETIME_BOUND { return madeNodes[number - m_firstMadeNodes]; }
    std::span<const uint32_t> partsOf(const MadeNodes& made) const LIFETIME_BOUND
    {
        if (made.firstPart < m_firstPart)
            return m_solver.m_partsOfMadeNodes.subspan(made.firstPart, made.numberOfParts);
        return parts.subspan(made.firstPart - m_firstPart, made.numberOfParts);
    }

    std::optional<uint32_t> findAmongNew(const ValueFlowNodeKey& key)
    {
        for (size_t position = std::max(m_numberFoundByKey, m_firstOfName); position < madeNodes.size(); ++position)
            m_byKey.add(madeNodes[position].key.hash(), position);
        m_numberFoundByKey = madeNodes.size();
        std::optional<uint32_t> position = m_byKey.find(key.hash(), [&](uint32_t candidate) {
            return madeNodes[candidate].key == key;
        });
        if (!position)
            return std::nullopt;
        return m_firstMadeNodes + *position;
    }

    uint32_t ensure(const ValueFlowNodeKey& key)
    {
        if (std::optional<uint32_t> existing = m_solver.findNumberOfMadeNodes(key))
            return *existing;
        if (std::optional<uint32_t> existing = findAmongNew(key))
            return *existing;
        m_hasMadeMoreOfName = true;
        return makeNodes(key);
    }

    void setParts(uint32_t number, std::span<const uint32_t> numbersOfParts)
    {
        newAt(number).firstPart = m_firstPart + parts.size();
        newAt(number).numberOfParts = numbersOfParts.size();
        parts.append(numbersOfParts);
    }

    void makeStatement(uint32_t number, ValueFlowStatementKind kind, ValueFlowNodeID result, ValueFlowNodeID thrown)
    {
        ValueFlowStatement statement;
        statement.kind = kind;
        statement.result = result;
        statement.thrown = thrown;
        newAt(number).statement = ValueFlowStatementID(m_firstStatement + madeStatements.size());
        madeStatements.append(MadeStatement { statement, newAt(number).key });
        madeNodesOfStatement.append(number);
    }

    void listToLink(uint32_t number)
    {
        const ValueFlowNodeKey& key = at(number).key;
        bool standsForKind = key.kind == ValueFlowMadeNodeKind::ReadUnderKind || key.kind == ValueFlowMadeNodeKind::WrittenUnderKind;
        bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
        (isRead ? readToLink : writtenToLink).append(NodeToLink { { key.set, at(number).first }, key.name(), standsForKind });
    }

    // While somebody asks for a node that reads or writes, it asks for the node that stands for its kind.
    void joinNodeUnderKind(uint32_t number)
    {
        const ValueFlowNodeKey& key = at(number).key;
        bool isRead = key.kind == ValueFlowMadeNodeKind::Read;
        const KindOfNames& kind = m_solver.m_kindsOfNames[static_cast<unsigned>(key.name().kind)];
        if (isRead ? kind.writtenKindsItMayFind : kind.readKindsThatMayFindIt)
            wishes.append(Wish { number, ValueFlowNodeKey::underKind(isRead ? ValueFlowMadeNodeKind::ReadUnderKind : ValueFlowMadeNodeKind::WrittenUnderKind, key.set, key.name().kind) });
    }

    uint32_t makeNodes(const ValueFlowNodeKey& key)
    {
        uint32_t number = m_firstMadeNodes + madeNodes.size();
        ValueFlowNodeID first(m_firstNode + numberOfNodes);
        numberOfNodes += key.numberOfNodes();
        madeNodes.append(MadeNodes { key, first, { } });
        if (key.set == ValueSets::nothing && (key.kind == ValueFlowMadeNodeKind::Written || key.kind == ValueFlowMadeNodeKind::CallGroup)) {
            madeNodes.last().staysAtRest = true;
            stayingAtRest.append(number);
            return number;
        }

        switch (key.kind) {
        case ValueFlowMadeNodeKind::Constant:
            constants.append(number);
            break;
        case ValueFlowMadeNodeKind::Read:
        case ValueFlowMadeNodeKind::Written:
            if (makeUnionWithEarlierNode(number))
                break;
            listToLink(number);
            joinNodeUnderKind(number);
            break;
        case ValueFlowMadeNodeKind::ReadUnderKind:
        case ValueFlowMadeNodeKind::WrittenUnderKind:
            if (makeUnionWithEarlierNode(number))
                break;
            listToLink(number);
            break;
        case ValueFlowMadeNodeKind::Lookup:
            // What lies above a set of objects is what lies above each of them.
            m_setsOfParts.shrink(0);
            if (key.name().kind == ValueFlowNameKind::PrototypeLink)
                m_maker->appendParts(key.set, m_setsOfParts);
            for (ValueSetID part : m_setsOfParts)
                wishes.append(Wish { number, ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Lookup, part, key.name()) });
            if (m_setsOfParts.isEmpty())
                makeStatement(number, ValueFlowStatementKind::Lookup, first, { });
            break;
        case ValueFlowMadeNodeKind::CallGroup: {
            // The group of a big set of callees hands on to the groups of the parts of the set.
            m_setsOfParts.shrink(0);
            m_maker->appendParts(key.set, m_setsOfParts);
            for (ValueSetID part : m_setsOfParts)
                wishes.append(Wish { number, ValueFlowNodeKey::callGroup(part, key.callKind(), key.markerOfCallGroup()) });
            if (m_setsOfParts.isEmpty()) {
                ValueFlowCallGroup group = callGroupAt(first);
                makeStatement(number, ValueFlowStatementKind::CallGroup, group.result, group.thrown);
            }
            break;
        }
        case ValueFlowMadeNodeKind::Hub:
            makeStatement(number, ValueFlowStatementKind::Behavior, first, { });
            break;
        case ValueFlowMadeNodeKind::Masked:
            masked.append(Masked { number, key.maskKind() == ValueFlowMaskKind::Plain ? ValueSetID() : m_solver.familyObjectsOf(key.set, *m_maker) });
            edges.append(ValueFlowEdge { key.maskedNode(), first });
            break;
        case ValueFlowMadeNodeKind::Marked:
            break;
        case ValueFlowMadeNodeKind::Instance:
            if (key.isInstanceOfNode())
                instances.append(number);
            break;
        }
        return number;
    }

    // The sets that the sites of a program ask for under one name are mostly no supersets of one another: they have a core in common, and each has a few members of its own.
    bool makeUnionOfCoreAndOthers(uint32_t number, size_t sizeOfEarlierSubset, std::span<const uint32_t> similar)
    {
        ValueFlowNodeKey key = at(number).key;
        size_t size = m_sets.size(key.set);
        uint32_t sharingMost = noSolverIndex;
        size_t sizeOfCore = std::max<size_t>({ sizeOfEarlierSubset, size / 2, mostObjectsLookedUpOneByOne });
        for (uint32_t candidate : similar) {
            // What two sets share is no larger than either.
            if (m_sets.size(at(candidate).key.set) <= sizeOfCore)
                continue;
            size_t shared = m_sets.numberOfCommonMembers(key.set, at(candidate).key.set);
            if (shared != size && shared > sizeOfCore) {
                sharingMost = candidate;
                sizeOfCore = shared;
            }
        }
        if (sharingMost == noSolverIndex)
            return false;
        ValueSetID core = m_maker->intersect(key.set, at(sharingMost).key.set);

        bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
        ValueFlowNodeID first = at(number).first;
        std::array<uint32_t, 2> numbersOfParts;
        std::array<ValueSetID, 2> sets { core, m_maker->intersect(key.set, ValueSets::complement(core)) };
        for (size_t index = 0; index < 2; ++index) {
            ValueFlowNodeKey keyOfPart = key;
            keyOfPart.set = sets[index];
            numbersOfParts[index] = ensure(keyOfPart);
            ValueFlowNodeID part = at(numbersOfParts[index]).first;
            edges.append(isRead ? ValueFlowEdge { part, first } : ValueFlowEdge { first, part });
        }
        newAt(number).isUnionOfParts = true;
        setParts(number, numbersOfParts);
        return true;
    }

    // A set that climbs is asked for again round after round, each time with a few members more, and linking a node of many objects costs a look at every node on the other side.
    bool makeUnionWithEarlierNode(uint32_t number)
    {
        constexpr size_t mostLeaves = 64;
        ValueFlowNodeKey key = at(number).key;
        if (m_isMakingLeaf || ValueSets::isComplement(key.set) || m_sets.size(key.set) <= mostObjectsLookedUpOneByOne)
            return false;
        uint64_t name = nameOf(key);
        NodesOfManyObjects& nodes = m_solver.m_nodesOfManyObjects[makerOfName(name)];
        uint32_t position = nodes.madeNodes.size();
        nodes.madeNodes.append(number);
        LatestNodes& latest = nodes.latest.add(name, LatestNodes { noSolverIndex, noSolverIndex, noSolverIndex, noSolverIndex }).iterator->value;
        uint32_t earlier = noSolverIndex;
        Vector<uint32_t, 8> similar;
        size_t sizeOfSet = m_sets.size(key.set);
        auto consider = [&](uint32_t positionOfCandidate) {
            if (positionOfCandidate == noSolverIndex)
                return;
            uint32_t candidate = nodes.madeNodes[positionOfCandidate];
            if (candidate == earlier || similar.contains(candidate))
                return;
            if (m_sets.size(at(candidate).key.set) > sizeOfSet || !m_sets.isSubset(at(candidate).key.set, key.set)) {
                similar.append(candidate);
                return;
            }
            if (earlier == noSolverIndex || m_sets.size(at(candidate).key.set) > m_sets.size(at(earlier).key.set))
                earlier = candidate;
        };
        for (uint32_t candidate : latest)
            consider(candidate);
        std::ranges::copy_backward(latest.begin(), latest.end() - 1, latest.end());
        latest[0] = position;

        // Many sites ask under one name in turn, and each of their sets climbs by itself. A set holds most of the blocks of its earlier self.
        std::array<uint64_t, 4> sample = nodes.sampleOfSet.ensure(key.set.index(), [&] {
            return m_sets.sampleOfBlocks(key.set);
        }).iterator->value;
        for (uint64_t block : sample) {
            if (block == ValueSets::noBlock)
                continue;
            uint32_t& latestWithBlock = nodes.latestWithBlock.add(((name * 0x9e3779b97f4a7c15ull) ^ (block * 0xc2b2ae3d27d4eb4full)) >> 1, noSolverIndex).iterator->value;
            if (latestWithBlock != noSolverIndex) {
                const ValueFlowNodeKey& other = at(nodes.madeNodes[latestWithBlock]).key;
                if (other.kind == key.kind && other.detail == key.detail && other.payload == key.payload)
                    consider(latestWithBlock);
            }
            latestWithBlock = position;
        }
        if (makeUnionOfCoreAndOthers(number, earlier == noSolverIndex ? 0 : m_sets.size(at(earlier).key.set), similar.span()))
            return true;
        if (earlier == noSolverIndex)
            return false;
        ValueSetID earlierSet = at(earlier).key.set;

        Vector<uint32_t, mostLeaves> leaves;
        if (at(earlier).isUnionOfParts)
            leaves.append(partsOf(at(earlier)));
        else
            leaves.append(earlier);
        if (leaves.size() >= mostLeaves)
            return false;

        ValueFlowNodeKey keyOfDifference = key;
        keyOfDifference.set = m_maker->intersect(key.set, ValueSets::complement(earlierSet));
        m_isMakingLeaf = true;
        uint32_t difference = ensure(keyOfDifference);
        m_isMakingLeaf = false;
        if (at(difference).isUnionOfParts)
            leaves.append(partsOf(at(difference)));
        else
            leaves.append(difference);

        bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
        ValueFlowNodeID first = at(number).first;
        for (uint32_t leaf : leaves)
            edges.append(isRead ? ValueFlowEdge { at(leaf).first, first } : ValueFlowEdge { first, at(leaf).first });
        newAt(number).isUnionOfParts = true;
        setParts(number, leaves.span());
        return true;
    }

    ValueFlowSolver& m_solver;
    const ValueSets& m_sets;
    ValueSetMaker* m_maker { nullptr };
    const uint32_t m_firstMadeNodes;
    const uint32_t m_firstNode;
    const uint32_t m_firstStatement;
    const uint32_t m_firstPart;
    // Of what was made under the name at hand, once something is looked for.
    HashedIndex m_byKey;
    size_t m_numberFoundByKey { 0 };
    uint64_t m_name { UINT64_MAX };
    size_t m_firstOfName { 0 };
    bool m_hasMadeMoreOfName { false };
    Vector<ValueSetID> m_setsOfParts;
    bool m_isMakingLeaf { false };
};

void ValueFlowSolver::takeWhatWasMade(std::span<const std::unique_ptr<NodeMaker>> makers, std::span<KeyToMake> keys)
{
    uint32_t firstMadeNodes = m_madeNodes.size();
    uint32_t firstStatement = m_madeStatements.size();
    uint32_t firstPart = m_partsOfMadeNodes.size();
    size_t firstEdge = m_edgesOfRound.size();
    size_t firstReadToLink = m_readToLink.size();
    size_t firstWrittenToLink = m_writtenToLink.size();
    uint32_t madeNodes = 0;
    uint32_t nodes = 0;
    uint32_t statements = 0;
    uint32_t parts = 0;
    size_t edges = 0;
    size_t readToLink = 0;
    size_t writtenToLink = 0;
    for (const std::unique_ptr<NodeMaker>& maker : makers) {
        maker->madeNodesBefore = madeNodes;
        maker->nodesBefore = nodes;
        maker->statementsBefore = statements;
        maker->partsBefore = parts;
        maker->edgesBefore = edges;
        maker->readToLinkBefore = readToLink;
        maker->writtenToLinkBefore = writtenToLink;
        madeNodes += maker->madeNodes.size();
        nodes += maker->numberOfNodes;
        statements += maker->madeStatements.size();
        parts += maker->parts.size();
        edges += maker->edges.size();
        readToLink += maker->readToLink.size();
        writtenToLink += maker->writtenToLink.size();
    }

    uint32_t firstNode = m_graph.addNodes(nodes).index();
    m_numberOfNodesMade += nodes;
    auto grow = [](auto& list, size_t more) {
        makeRoomByDoubling(list, more);
        list.grow(list.size() + more);
    };
    m_madeNodes.makeRoomToFill(madeNodes);
    growAndFill(m_numberOfUsers, m_madeNodes.size(), 0u);
    m_askerThatKeepsNothingAwake.makeRoomToFill(madeNodes);
    m_madeNodesOfNode.makeRoomToFill(nodes);
    m_madeStatements.makeRoomToFill(statements);
    grow(m_madeNodesOfStatement, statements);
    grow(m_partsOfMadeNodes, parts);
    grow(m_edgesOfRound, edges);
    m_readToLink.makeRoomToFill(readToLink);
    m_writtenToLink.makeRoomToFill(writtenToLink);
    growByDoubling(m_isDue, numberOfStatements());

    m_stepTimes.didFinish("rules: made: taken: sizes summed, room made (one thread)"_s);
    bool isOneMaker = makers.size() == 1;
    auto take = [&](unsigned index, unsigned worker) {
        NodeMaker& maker = *makers[index];
        for (size_t position = 0; position < maker.madeNodes.size(); ++position) {
            MadeNodes made = maker.madeNodes[position];
            uint32_t number = firstMadeNodes + maker.madeNodesBefore + position;
            made.first = maker.nodeMovedUp(made.first);
            made.statement = maker.statementMovedUp(made.statement);
            if (made.numberOfParts)
                made.firstPart = maker.partMovedUp(made.firstPart);
            for (unsigned offset = 0; offset < made.key.numberOfNodes(); ++offset)
                m_madeNodesOfNode[made.first.index() + offset - m_input.numberOfNodes] = number;
            m_madeNodes[number] = made;
            m_askerThatKeepsNothingAwake[number] = made.staysAtRest ? everyAsker : made.statement.index();
            uint32_t hash = made.key.hash();
            IndexOfMadeNodes& madeNodes = indexOfMadeNodesWith(hash);
            Locker locker { madeNodes.lock };
            madeNodes.add(tableOfMadeNodesWith(hash), hash, number, m_fewestSlotsOfIndexOfMadeNodes);
        }
        for (size_t position = 0; position < maker.madeStatements.size(); ++position) {
            MadeStatement made = maker.madeStatements[position];
            made.statement.result = maker.nodeMovedUp(made.statement.result);
            made.statement.thrown = maker.nodeMovedUp(made.statement.thrown);
            m_madeStatements[firstStatement + maker.statementsBefore + position] = made;
            m_madeNodesOfStatement[firstStatement + maker.statementsBefore + position] = maker.madeNodesMovedUp(maker.madeNodesOfStatement[position]);
        }
        for (size_t position = 0; position < maker.parts.size(); ++position)
            m_partsOfMadeNodes[firstPart + maker.partsBefore + position] = maker.madeNodesMovedUp(maker.parts[position]);
        for (size_t position = 0; position < maker.edges.size(); ++position)
            m_edgesOfRound[firstEdge + maker.edgesBefore + position] = ValueFlowEdge { maker.nodeMovedUp(maker.edges[position].from), maker.nodeMovedUp(maker.edges[position].to) };
        for (size_t position = 0; position < maker.readToLink.size(); ++position) {
            NodeToLink toLink = maker.readToLink[position];
            toLink.node.node = maker.nodeMovedUp(toLink.node.node);
            m_readToLink[firstReadToLink + maker.readToLinkBefore + position] = toLink;
        }
        for (size_t position = 0; position < maker.writtenToLink.size(); ++position) {
            NodeToLink toLink = maker.writtenToLink[position];
            toLink.node.node = maker.nodeMovedUp(toLink.node.node);
            m_writtenToLink[firstWrittenToLink + maker.writtenToLinkBefore + position] = toLink;
        }
        for (NodeMaker::Wish& wish : maker.wishes)
            wish.madeNodes = maker.madeNodesMovedUp(wish.madeNodes);
        for (uint32_t key : maker.keys)
            keys[key].madeNodes = maker.madeNodesMovedUp(keys[key].madeNodes);
        for (uint32_t number : maker.stayingAtRest) {
            const MadeNodes& made = m_madeNodes[maker.madeNodesMovedUp(number)];
            m_graph.putToRest(made.first, made.key.numberOfNodes());
        }
        for (uint32_t number : maker.constants) {
            const MadeNodes& made = m_madeNodes[maker.madeNodesMovedUp(number)];
            m_graph.makeConstant(made.first, made.key.set, worker, made.key.unknownSource() ? ValueFlowGraph::EdgesOutOfConstant::AreListed : ValueFlowGraph::EdgesOutOfConstant::AreKeptAtTargetsAlone);
        }
        if (!isOneMaker && maker.madeNodesBefore) {
            Vector<uint32_t>& ofManyObjects = m_nodesOfManyObjects[index].madeNodes;
            for (size_t position = ofManyObjects.size(); position && ofManyObjects[position - 1] >= firstMadeNodes; --position)
                ofManyObjects[position - 1] += maker.madeNodesBefore;
        }
    };
    if (isOneMaker)
        take(0, 0);
    else
        forEachIndexInParallel(m_loops, makers.size(), take);
    RELEASE_ASSERT(firstNode + nodes == m_graph.numberOfNodes());
    m_stepTimes.didFinish("rules: made: taken: moved up"_s);

    for (const std::unique_ptr<NodeMaker>& maker : makers) {
        for (const NodeMaker::Masked& masked : maker->masked) {
            const MadeNodes& made = m_madeNodes[maker->madeNodesMovedUp(masked.madeNodes)];
            m_graph.setMask(made.first, made.key.set, made.key.maskKind(), masked.familyObjects);
            m_graph.startWithValueOf(made.first, made.key.maskedNode());
        }
        for (uint32_t number : maker->instances) {
            // What stands in the way of a value at the node stands in its way here.
            const MadeNodes& made = m_madeNodes[maker->madeNodesMovedUp(number)];
            ValueFlowNodeID node = made.key.nodeOfInstance();
            if (m_graph.maskOf(node) != ValueSets::anything)
                m_graph.setMask(made.first, m_graph.maskOf(node), m_graph.maskKindOf(node), m_graph.familyObjectsOf(node));
        }
    }
    m_stepTimes.didFinish("rules: made: taken: masks set (one thread)"_s);
}

void ValueFlowSolver::makeNodesOf(std::span<KeyToMake> keys)
{
    if (keys.empty())
        return;
    AOT_THRESHOLD(size_t, fewestKeysForAllThreads, 256);
    Vector<NodeMaker::Wish> wishes;
    if (keys.size() < fewestKeysForAllThreads) {
        Vector<std::unique_ptr<NodeMaker>> makers(1, [&](size_t) {
            return makeUnique<NodeMaker>(*this);
        });
        for (uint32_t index = 0; index < keys.size(); ++index)
            makers[0]->keys.append(index);
        makers[0]->run(keys, *m_makers[0]);
        m_stepTimes.didFinish("rules: made: one maker runs (one thread)"_s);
        takeWhatWasMade(makers.span(), keys);
        wishes = WTF::move(makers[0]->wishes);
    } else {
        // The maker of each key, and in the highest bit whether the key may bring others.
        constexpr size_t keysPerShare = 256;
        Vector<uint8_t> makerOfKey(keys.size());
        static_assert(numberOfNodeMakers <= 128);
        forEachIndexInParallel(m_loops, (keys.size() + keysPerShare - 1) / keysPerShare, [&](unsigned share) {
            for (size_t index = share * keysPerShare; index < std::min(keys.size(), (share + 1) * keysPerShare); ++index)
                makerOfKey[index] = NodeMaker::makerOf(m_sets, keys[index]) | (NodeMaker::mayBringOthers(m_sets, keys[index].key) ? 128 : 0);
        });
        // First the keys that may bring others with them, each name by one maker. Then the others, any of which may have come as a part.
        for (bool bringOthers : { true, false }) {
            Vector<std::unique_ptr<NodeMaker>> makers(numberOfNodeMakers, [&](size_t) {
                return makeUnique<NodeMaker>(*this);
            });
            size_t taken = 0;
            for (uint32_t index = 0; index < keys.size(); ++index) {
                if (!!(makerOfKey[index] & 128) != bringOthers)
                    continue;
                makers[makerOfKey[index] & 127]->keys.append(index);
                ++taken;
            }
            m_stepTimes.didFinish("rules: made: keys dealt to the makers (one thread)"_s);
            if (!taken)
                continue;
            forEachIndexInParallel(m_loops, makers.size(), [&](unsigned index, unsigned worker) {
                makers[index]->run(keys, *m_makers[worker], !bringOthers);
            });
            m_stepTimes.didFinish("rules: made: all makers run"_s);
            takeWhatWasMade(makers.span(), keys);
            for (const std::unique_ptr<NodeMaker>& maker : makers)
                wishes.appendVector(maker->wishes);
            m_stepTimes.didFinish("rules: made: wishes in one list (one thread)"_s);
        }
        m_stepTimes.didFinish("rules: made: makers destroyed (one thread)"_s);
    }
    if (wishes.isEmpty())
        return;

    constexpr size_t wishesPerShare = 1024;
    unsigned numberOfShares = (wishes.size() + wishesPerShare - 1) / wishesPerShare;
    auto forEachWishInParallel = [&](NOESCAPE const Invocable<void(unsigned share, NodeMaker::Wish&)> auto& functor) {
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            for (size_t index = share * wishesPerShare; index < std::min(wishes.size(), (share + 1) * wishesPerShare); ++index)
                functor(share, wishes[index]);
        });
    };
    Vector<Vector<KeyToMake>> missingOfShare(numberOfShares);
    forEachWishInParallel([&](unsigned share, NodeMaker::Wish& wish) {
        if (std::optional<uint32_t> existing = findNumberOfMadeNodes(wish.key))
            wish.granted = *existing;
        else
            missingOfShare[share].append(KeyToMake { wish.key, wish.key.set ? m_sets.hashOfMembers(wish.key.set) : 0 });
    });
    m_stepTimes.didFinish("rules: made: wishes looked up"_s);
    Vector<KeyToMake> missing;
    for (const Vector<KeyToMake>& ofShare : missingOfShare)
        missing.appendVector(ofShare);
    if (!missing.isEmpty()) {
        auto orderOf = [](const KeyToMake& toMake) {
            return std::tuple { toMake.key.kind, toMake.key.detail, toMake.key.payload, toMake.hashOfMembers, toMake.key.set.index() };
        };
        parallelSort(m_loops, missing.mutableSpan(), [&](const KeyToMake& first, const KeyToMake& second) {
            return orderOf(first) < orderOf(second);
        });
        missing.shrink(std::unique(missing.begin(), missing.end(), [](const KeyToMake& first, const KeyToMake& second) {
            return first.key == second.key;
        }) - missing.begin());
        m_stepTimes.didFinish("rules: made: missing wishes sorted"_s);
        makeNodesOf(missing.mutableSpan());
        forEachWishInParallel([&](unsigned, NodeMaker::Wish& wish) {
            if (wish.granted == noSolverIndex)
                wish.granted = *findNumberOfMadeNodes(wish.key);
        });
        m_stepTimes.didFinish("rules: made: wishes looked up again"_s);
    }

    // The wishes of one node stand in a row.
    size_t firstPart = m_partsOfMadeNodes.size();
    makeRoomByDoubling(m_partsOfMadeNodes, wishes.size());
    m_partsOfMadeNodes.grow(firstPart + wishes.size());
    Vector<size_t> firstEdgeOfShare(numberOfShares + 1);
    firstEdgeOfShare[0] = m_edgesOfRound.size();
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        size_t edges = 0;
        for (size_t index = share * wishesPerShare; index < std::min(wishes.size(), (share + 1) * wishesPerShare); ++index)
            edges += wishes[index].key.numberOfNodes();
        firstEdgeOfShare[share + 1] = edges;
    });
    for (unsigned share = 0; share < numberOfShares; ++share)
        firstEdgeOfShare[share + 1] += firstEdgeOfShare[share];
    makeRoomByDoubling(m_edgesOfRound, firstEdgeOfShare.last() - m_edgesOfRound.size());
    m_edgesOfRound.grow(firstEdgeOfShare.last());
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        size_t edge = firstEdgeOfShare[share];
        for (size_t index = share * wishesPerShare; index < std::min(wishes.size(), (share + 1) * wishesPerShare); ++index) {
            const NodeMaker::Wish& wish = wishes[index];
            MadeNodes& made = m_madeNodes[wish.madeNodes];
            m_partsOfMadeNodes[firstPart + index] = wish.granted;
            if (!index || wishes[index - 1].madeNodes != wish.madeNodes) {
                size_t end = index + 1;
                while (end < wishes.size() && wishes[end].madeNodes == wish.madeNodes)
                    ++end;
                made.firstPart = firstPart + index;
                made.numberOfParts = end - index;
            }
            ValueFlowNodeID first = made.first;
            ValueFlowNodeID firstOfPart = m_madeNodes[wish.granted].first;
            if (wish.key.kind != ValueFlowMadeNodeKind::CallGroup) {
                bool takesFromPart = wish.key.kind == ValueFlowMadeNodeKind::ReadUnderKind || wish.key.kind == ValueFlowMadeNodeKind::Lookup;
                m_edgesOfRound[edge++] = takesFromPart ? ValueFlowEdge { firstOfPart, first } : ValueFlowEdge { first, firstOfPart };
                continue;
            }
            ValueFlowCallGroup group = callGroupAt(first);
            for (unsigned offset = 0; offset < ValueFlowNodeKey::numberOfNodesOfCallGroup; ++offset) {
                ValueFlowEdge down { ValueFlowNodeID(first.index() + offset), ValueFlowNodeID(firstOfPart.index() + offset) };
                bool comesBack = down.from == group.result || down.from == group.thrown;
                m_edgesOfRound[edge++] = comesBack ? ValueFlowEdge { down.to, down.from } : down;
            }
        }
    });
    m_stepTimes.didFinish("rules: made: parts and their edges"_s);
}

ValueFlowStatement ValueFlowSolver::statementMadeWith(const MadeNodes& made)
{
    const ValueFlowNodeKey& key = made.key;
    ValueFlowStatement statement;
    statement.kind = key.kind == ValueFlowMadeNodeKind::Lookup ? ValueFlowStatementKind::Lookup : key.kind == ValueFlowMadeNodeKind::CallGroup ? ValueFlowStatementKind::CallGroup : ValueFlowStatementKind::Behavior;
    statement.result = key.kind == ValueFlowMadeNodeKind::CallGroup ? callGroupAt(made.first).result : made.first;
    if (key.kind == ValueFlowMadeNodeKind::CallGroup)
        statement.thrown = callGroupAt(made.first).thrown;
    return statement;
}

void ValueFlowSolver::runStatementsMadeBy(Thread& thread)
{
    ValueFlowRoundLog& log = thread.log;
    while (!log.madeWithStatementToRun.isEmpty() && !log.hasDroppedEdges) {
        uint32_t number = log.madeWithStatementToRun.takeLast();
        log.recordsOfRunsWhereMade.append(number);
        MadeNodes made = m_madeNodes[number];
        ValueFlowStatement statement = statementMadeWith(made);
        thread.context.beginWhereMade(made.statement, made.key, number);
        m_rule(statement, thread.context);
        thread.context.end();
        ++log.numberOfRunsWhereMade;
    }
}

// Whoever asks for a record that is at rest runs its statement behind its own run, as it does for a record that it makes: no further turn. The books at the end of the turn wake the record.
void ValueFlowSolver::runStatementsWokenBy(Thread& thread)
{
    ValueFlowRoundLog& log = thread.log;
    while (!log.askedForAtRest.isEmpty() && !log.hasDroppedEdges) {
        MadeNodes& made = m_madeNodes[log.askedForAtRest.takeLast()];
        if (made.staysAtRest || made.isOfThisTurn)
            continue;
        for (uint32_t part : m_partsOfMadeNodes.subspan(made.firstPart, made.numberOfParts)) {
            if (isRecordAtRest(part))
                log.askedForAtRest.append(part);
        }
        if (!made.statement || m_hasRunInTurn.setInOrder(made.statement.index()))
            continue;
        made.hasStatementRunWhereMade = true;
        thread.context.begin(made.statement);
        m_rule(statementAt(made.statement), thread.context);
        thread.context.end();
        ++log.numberOfRunsWhereMade;
        runStatementsMadeBy(thread);
    }
}

void ValueFlowSolver::letRulesMakeNodes()
{
    m_nextNode.store(m_graph.numberOfNodes(), std::memory_order_relaxed);
    m_nextStatement.store(numberOfStatements(), std::memory_order_relaxed);
}

std::optional<uint32_t> ValueFlowSolver::findOrMakeNodesWhereAskedFor(const ValueFlowNodeKey& key, uint32_t hash, ValueFlowRoundLog& log, ValueSetMaker& maker) const
{
    std::optional<uint32_t> number = findInIndexOfMadeNodes(hash, [&](uint32_t candidate) {
        return m_madeNodes[candidate].key == key;
    });
    if (number) [[likely]]
        return number;
    return makeNodesWhereAskedFor(key, hash, log, maker);
}

std::optional<uint32_t> ValueFlowSolver::makeNodesWhereAskedFor(const ValueFlowNodeKey& key, uint32_t hash, ValueFlowRoundLog& log, ValueSetMaker& maker) const
{
    if (NodeMaker::mayBringOthers(m_sets, key))
        return std::nullopt;

    MadeNodes made { key, { }, { } };
    made.staysAtRest = key.set == ValueSets::nothing && (key.kind == ValueFlowMadeNodeKind::Written || key.kind == ValueFlowMadeNodeKind::CallGroup);
    // What lies above a set of objects is what lies above each of them, and the group of a big set of callees hands on to the groups of the parts of the set.
    Vector<ValueSetID> setsOfParts;
    bool mayHaveParts = key.kind == ValueFlowMadeNodeKind::CallGroup || (key.kind == ValueFlowMadeNodeKind::Lookup && key.name().kind == ValueFlowNameKind::PrototypeLink);
    if (mayHaveParts && !made.staysAtRest)
        maker.appendParts(key.set, setsOfParts);
    bool hasStatement = !made.staysAtRest && setsOfParts.isEmpty() && (key.kind == ValueFlowMadeNodeKind::Lookup || key.kind == ValueFlowMadeNodeKind::CallGroup || key.kind == ValueFlowMadeNodeKind::Hub);

    IndexOfMadeNodes& index = indexOfMadeNodesWith(hash);
    auto give = [](ValueFlowRoundLog::NumbersToGive& numbers, uint32_t count, uint32_t mostInBlock, NOESCAPE const Invocable<uint32_t(uint32_t)> auto& takeBlock) {
        if (numbers.next == numbers.end) {
            numbers.sizeOfLastBlock = std::min(std::max(numbers.sizeOfLastBlock * 2, count), mostInBlock);
            numbers.next = takeBlock(numbers.sizeOfLastBlock);
            numbers.end = numbers.next + numbers.sizeOfLastBlock;
        }
        return std::exchange(numbers.next, numbers.next + count);
    };
    auto takeNodes = [&](uint32_t count) {
        uint32_t first = m_nextNode.fetch_add(count, std::memory_order_relaxed);
        RELEASE_ASSERT(first + count < ValueFlowRoundLog::numberedByLog);
        return first;
    };
    made.isOfThisTurn = true;
    uint32_t number;
    {
        Locker locker { index.lock };
        std::optional<uint32_t> existing = findInIndexOfMadeNodes(hash, [&](uint32_t candidate) {
            return m_madeNodes[candidate].key == key;
        });
        if (existing)
            return existing;
        number = give(log.recordsToGive, 1, 64, [&](uint32_t count) {
            return static_cast<uint32_t>(m_madeNodes.makeRoomToFill(count));
        });
        if (key.kind == ValueFlowMadeNodeKind::CallGroup)
            made.first = ValueFlowNodeID(give(log.nodesOfCallGroupsToGive, ValueFlowNodeKey::numberOfNodesOfCallGroup, 16 * ValueFlowNodeKey::numberOfNodesOfCallGroup, takeNodes));
        else
            made.first = ValueFlowNodeID(give(log.nodesToGive, 1, 256, takeNodes));
        if (hasStatement) {
            made.statement = ValueFlowStatementID(m_nextStatement.fetch_add(1, std::memory_order_relaxed));
            made.hasStatementRunWhereMade = true;
        }
        m_madeNodes[number] = made;
        index.add(tableOfMadeNodesWith(hash), hash, number, m_fewestSlotsOfIndexOfMadeNodes);
    }
    ++log.numberMadeWhereAskedFor;
    log.madeWhereAskedFor.append(number);
    if (made.statement)
        log.madeWithStatementToRun.append(number);
    if (made.staysAtRest)
        return number;

    // Outside the lock: a part has a lock of its own.
    Vector<uint32_t, 8> parts;
    auto join = [&](const ValueFlowNodeKey& keyOfPart) {
        std::optional<uint32_t> part = findOrMakeNodesWhereAskedFor(keyOfPart, keyOfPart.hash(), log, maker);
        parts.append(*part);
        ValueFlowNodeID firstOfPart = m_madeNodes[*part].first;
        if (keyOfPart.kind != ValueFlowMadeNodeKind::CallGroup) {
            bool takesFromPart = keyOfPart.kind == ValueFlowMadeNodeKind::ReadUnderKind || keyOfPart.kind == ValueFlowMadeNodeKind::Lookup;
            log.edgesOfMadeNodes.append(takesFromPart ? ValueFlowEdge { firstOfPart, made.first } : ValueFlowEdge { made.first, firstOfPart });
            return;
        }
        ValueFlowCallGroup group = callGroupAt(made.first);
        for (unsigned offset = 0; offset < ValueFlowNodeKey::numberOfNodesOfCallGroup; ++offset) {
            ValueFlowEdge down { ValueFlowNodeID(made.first.index() + offset), ValueFlowNodeID(firstOfPart.index() + offset) };
            bool comesBack = down.from == group.result || down.from == group.thrown;
            log.edgesOfMadeNodes.append(comesBack ? ValueFlowEdge { down.to, down.from } : down);
        }
    };
    switch (key.kind) {
    case ValueFlowMadeNodeKind::Read:
    case ValueFlowMadeNodeKind::Written: {
        // While somebody asks for a node that reads or writes, it asks for the node that stands for its kind.
        bool isRead = key.kind == ValueFlowMadeNodeKind::Read;
        const KindOfNames& kind = m_kindsOfNames[static_cast<unsigned>(key.name().kind)];
        if (isRead ? kind.writtenKindsItMayFind : kind.readKindsThatMayFindIt)
            join(ValueFlowNodeKey::underKind(isRead ? ValueFlowMadeNodeKind::ReadUnderKind : ValueFlowMadeNodeKind::WrittenUnderKind, key.set, key.name().kind));
        break;
    }
    case ValueFlowMadeNodeKind::Lookup:
        for (ValueSetID part : setsOfParts)
            join(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Lookup, part, key.name()));
        break;
    case ValueFlowMadeNodeKind::CallGroup:
        for (ValueSetID part : setsOfParts)
            join(ValueFlowNodeKey::callGroup(part, key.callKind(), key.markerOfCallGroup()));
        break;
    case ValueFlowMadeNodeKind::Masked:
        log.edgesOfMadeNodes.append(ValueFlowEdge { key.maskedNode(), made.first });
        break;
    case ValueFlowMadeNodeKind::Constant:
    case ValueFlowMadeNodeKind::Hub:
    case ValueFlowMadeNodeKind::Marked:
    case ValueFlowMadeNodeKind::WrittenUnderKind:
    case ValueFlowMadeNodeKind::ReadUnderKind:
    case ValueFlowMadeNodeKind::Instance:
        break;
    }
    for (uint32_t part : parts)
        log.partsOfMadeNodes.append({ number, part });
    return number;
}

// What NodeMaker and takeWhatWasMade() do for the keys of the logs, for the records that the rules of the turn have written.
void ValueFlowSolver::takeWhatRulesHaveMade()
{
    for (IndexOfMadeNodes& index : m_madeNodesByKey)
        index.freeOutgrownTables();

    constexpr size_t recordsPerShare = 2048;
    struct Share {
        unsigned thread { 0 };
        size_t first { 0 };
        size_t end { 0 };
        uint32_t nodes { 0 };
        Vector<NodeToLink> readToLink;
        Vector<NodeToLink> writtenToLink;
        Vector<NodeMaker::Masked> masked;
        Vector<uint32_t> instances;
        size_t firstReadToLink { 0 };
        size_t firstWrittenToLink { 0 };
    };
    Vector<Share> shares;
    for (unsigned thread = 0; thread < m_threads.size(); ++thread) {
        size_t made = m_threads[thread]->log.madeWhereAskedFor.size();
        for (size_t first = 0; first < made; first += recordsPerShare)
            shares.append(Share { thread, first, std::min(made, first + recordsPerShare), 0, { }, { }, { }, { }, 0, 0 });
    }
    if (shares.isEmpty())
        return;

    auto grow = [](auto& list, size_t more) {
        makeRoomByDoubling(list, more);
        list.grow(list.size() + more);
    };
    // With the numbers that threads hold and have not given yet.
    uint32_t statements = m_nextStatement.load(std::memory_order_relaxed) - numberOfStatements();
    m_graph.addNodes(m_nextNode.load(std::memory_order_relaxed) - m_graph.numberOfNodes());
    growAndFill(m_numberOfUsers, m_madeNodes.size(), 0u);
    growAndFill(m_askerThatKeepsNothingAwake, m_madeNodes.size(), everyAsker);
    growAndFill(m_madeNodesOfNode, m_graph.numberOfNodes() - m_input.numberOfNodes, noSolverIndex);
    m_madeStatements.makeRoomToFill(statements);
    grow(m_madeNodesOfStatement, statements);
    growByDoubling(m_isDue, numberOfStatements());
    growAndFill(m_rememberedByStatement, numberOfStatements(), ValueSetID());
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (auto [statement, remembered] : thread->log.rememberedByStatementsRunWhereMade)
            m_rememberedByStatement[statement.index()] = remembered;
        thread->log.rememberedByStatementsRunWhereMade.shrink(0);
    }

    forEachIndexInParallel(m_loops, shares.size(), [&](unsigned index, unsigned worker) {
        Share& share = shares[index];
        for (uint32_t number : m_threads[share.thread]->log.madeWhereAskedFor.subspan(share.first, share.end - share.first)) {
            MadeNodes& made = m_madeNodes[number];
            const ValueFlowNodeKey& key = made.key;
            share.nodes += key.numberOfNodes();
            for (unsigned offset = 0; offset < key.numberOfNodes(); ++offset)
                m_madeNodesOfNode[made.first.index() + offset - m_input.numberOfNodes] = number;
            m_askerThatKeepsNothingAwake[number] = made.staysAtRest ? everyAsker : made.statement.index();
            if (made.statement) {
                size_t position = made.statement.index() - m_input.statements.size();
                m_madeStatements[position] = MadeStatement { statementMadeWith(made), key };
                m_madeNodesOfStatement[position] = number;
            }
            // A masked node stays of this turn until it has its mask.
            made.isOfThisTurn = key.kind == ValueFlowMadeNodeKind::Masked;
            if (made.staysAtRest) {
                m_graph.putToRest(made.first, key.numberOfNodes());
                continue;
            }
            switch (key.kind) {
            case ValueFlowMadeNodeKind::Constant:
                m_graph.makeConstant(made.first, key.set, worker, key.unknownSource() ? ValueFlowGraph::EdgesOutOfConstant::AreListed : ValueFlowGraph::EdgesOutOfConstant::AreKeptAtTargetsAlone);
                break;
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::ReadUnderKind:
                share.readToLink.append(NodeToLink { { key.set, made.first }, key.name(), key.kind == ValueFlowMadeNodeKind::ReadUnderKind });
                break;
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
                share.writtenToLink.append(NodeToLink { { key.set, made.first }, key.name(), key.kind == ValueFlowMadeNodeKind::WrittenUnderKind });
                break;
            case ValueFlowMadeNodeKind::Masked:
                share.masked.append(NodeMaker::Masked { number, key.maskKind() == ValueFlowMaskKind::Plain ? ValueSetID() : familyObjectsOf(key.set, *m_makers[worker]) });
                break;
            case ValueFlowMadeNodeKind::Instance:
                if (key.isInstanceOfNode())
                    share.instances.append(number);
                break;
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::CallGroup:
            case ValueFlowMadeNodeKind::Hub:
            case ValueFlowMadeNodeKind::Marked:
                break;
            }
        }
    });

    size_t readToLink = m_readToLink.size();
    size_t writtenToLink = m_writtenToLink.size();
    for (Share& share : shares) {
        m_numberOfNodesMade += share.nodes;
        share.firstReadToLink = readToLink;
        share.firstWrittenToLink = writtenToLink;
        readToLink += share.readToLink.size();
        writtenToLink += share.writtenToLink.size();
    }
    m_readToLink.makeRoomToFill(readToLink - m_readToLink.size());
    m_writtenToLink.makeRoomToFill(writtenToLink - m_writtenToLink.size());

    // The parts of a whole stand in a row in the log of the thread that has made it.
    Vector<size_t> firstPartOfThread;
    Vector<size_t> firstEdgeOfThread;
    size_t parts = m_partsOfMadeNodes.size();
    size_t edges = m_edgesOfRound.size();
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        firstPartOfThread.append(parts);
        firstEdgeOfThread.append(edges);
        parts += thread->log.partsOfMadeNodes.size();
        edges += thread->log.edgesOfMadeNodes.size();
    }
    grow(m_partsOfMadeNodes, parts - m_partsOfMadeNodes.size());
    grow(m_edgesOfRound, edges - m_edgesOfRound.size());
    forEachIndexInParallel(m_loops, shares.size() + m_threads.size(), [&](unsigned task) {
        if (task < shares.size()) {
            const Share& share = shares[task];
            std::ranges::copy(share.readToLink, m_readToLink.mutableSpan().subspan(share.firstReadToLink).begin());
            std::ranges::copy(share.writtenToLink, m_writtenToLink.mutableSpan().subspan(share.firstWrittenToLink).begin());
            return;
        }
        unsigned number = task - shares.size();
        ValueFlowRoundLog& log = m_threads[number]->log;
        size_t position = firstPartOfThread[number];
        uint32_t whole = noSolverIndex;
        for (ValueFlowRoundLog::PartOfMadeNodes part : log.partsOfMadeNodes) {
            if (std::exchange(whole, part.whole) != part.whole)
                m_madeNodes[whole].firstPart = position;
            ++m_madeNodes[whole].numberOfParts;
            m_partsOfMadeNodes[position++] = part.part;
        }
        std::ranges::copy(log.edgesOfMadeNodes, m_edgesOfRound.mutableSpan().subspan(firstEdgeOfThread[number]).begin());
        log.madeWhereAskedFor.shrink(0);
        log.partsOfMadeNodes.shrink(0);
        log.edgesOfMadeNodes.shrink(0);
    });

    // A masked node begins with what the node that it masks holds. Where that is a masked node of this turn too, it comes first.
    Vector<NodeMaker::Masked> masked;
    for (const Share& share : shares)
        masked.appendVector(share.masked);
    Vector<ValueFlowEdge> toStart;
    Vector<uint32_t> started;
    while (!masked.isEmpty()) {
        size_t numberThatWait = 0;
        constexpr size_t distance = 8;
        toStart.shrink(0);
        started.shrink(0);
        for (size_t index = 0; index < masked.size(); ++index) {
            if (index + distance < masked.size())
                __builtin_prefetch(&m_madeNodes[masked[index + distance].madeNodes], 1);
            NodeMaker::Masked toMask = masked[index];
            MadeNodes& made = m_madeNodes[toMask.madeNodes];
            ValueFlowNodeID node = made.key.maskedNode();
            if (node.index() >= m_input.numberOfNodes && m_madeNodes[m_madeNodesOfNode[node.index() - m_input.numberOfNodes]].isOfThisTurn) {
                masked[numberThatWait++] = toMask;
                continue;
            }
            m_graph.setMask(made.first, made.key.set, made.key.maskKind(), toMask.familyObjects);
            toStart.append(ValueFlowEdge { node, made.first });
            started.append(toMask.madeNodes);
        }
        // Whoever waits for one of these looks at it in the next walk.
        m_graph.startWithValuesOf(toStart.span());
        for (uint32_t number : started)
            m_madeNodes[number].isOfThisTurn = false;
        RELEASE_ASSERT(numberThatWait < masked.size());
        masked.shrink(numberThatWait);
    }
    for (const Share& share : shares) {
        for (uint32_t number : share.instances) {
            // What stands in the way of a value at the node stands in its way here.
            const MadeNodes& made = m_madeNodes[number];
            ValueFlowNodeID node = made.key.nodeOfInstance();
            if (m_graph.maskOf(node) != ValueSets::anything)
                m_graph.setMask(made.first, m_graph.maskOf(node), m_graph.maskKindOf(node), m_graph.familyObjectsOf(node));
        }
    }
}

void ValueFlowSolver::fillNumbersThatNobodyGave()
{
    Vector<uint32_t> records;
    Vector<uint32_t> nodes;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        ValueFlowRoundLog& log = thread->log;
        for (uint32_t number = log.recordsToGive.next; number < log.recordsToGive.end; ++number)
            records.append(number);
        for (ValueFlowRoundLog::NumbersToGive* numbers : { &log.nodesToGive, &log.nodesOfCallGroupsToGive }) {
            for (uint32_t number = numbers->next; number < numbers->end; ++number)
                nodes.append(number);
            *numbers = { };
        }
        log.recordsToGive = { };
    }
    // A record stands for a node and a node has a record.
    while (records.size() < nodes.size())
        records.append(m_madeNodes.makeRoomToFill(1));
    if (nodes.size() < records.size()) {
        uint32_t more = records.size() - nodes.size();
        uint32_t first = m_graph.addNodes(more).index();
        for (uint32_t offset = 0; offset < more; ++offset)
            nodes.append(first + offset);
    }
    growAndFill(m_numberOfUsers, m_madeNodes.size(), 0u);
    growAndFill(m_askerThatKeepsNothingAwake, m_madeNodes.size(), everyAsker);
    growAndFill(m_madeNodesOfNode, m_graph.numberOfNodes() - m_input.numberOfNodes, noSolverIndex);
    for (size_t index = 0; index < records.size(); ++index) {
        MadeNodes made { ValueFlowNodeKey::constant(ValueSets::nothing), ValueFlowNodeID(nodes[index]), { } };
        made.staysAtRest = true;
        m_madeNodes[records[index]] = made;
        m_madeNodesOfNode[nodes[index] - m_input.numberOfNodes] = records[index];
        m_graph.putToRest(made.first, 1);
    }
}

ValueFlowNodeID ValueFlowSolver::makeNodes(const ValueFlowNodeKey& key)
{
    KeyToMake toMake { key, key.set ? m_sets.hashOfMembers(key.set) : 0 };
    makeNodesOf(singleElementSpan(toMake));
    return m_madeNodes[toMake.madeNodes].first;
}

void ValueFlowSolver::listToLink(const MadeNodes& madeNodes)
{
    const ValueFlowNodeKey& key = madeNodes.key;
    bool standsForKind = key.kind == ValueFlowMadeNodeKind::ReadUnderKind || key.kind == ValueFlowMadeNodeKind::WrittenUnderKind;
    bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
    (isRead ? m_readToLink : m_writtenToLink).append(NodeToLink { { key.set, madeNodes.first }, key.name(), standsForKind });
}

ValueFlowNodeID ValueFlowSolver::ensureConstant(ValueSetID set)
{
    ValueFlowNodeKey key = ValueFlowNodeKey::constant(set);
    if (std::optional<ValueFlowNodeID> existing = findMadeNode(key))
        return *existing;
    return makeNodes(key);
}

// The nodes that the logs have made get their numbers in an order that depends on their keys alone, and a set counts in it by its members, not by its number.
void ValueFlowSolver::numberMadeNodes()
{
    struct Candidate {
        ValueFlowNodeKey key;
        uint64_t hashOfMembers;
        uint32_t thread;
        uint32_t position;
    };

    Vector<Vector<uint32_t>> waiting(m_threads.size());
    for (unsigned number = 0; number < m_threads.size(); ++number) {
        Thread& thread = *m_threads[number];
        thread.numberOfMadeNode.insertFill(thread.numberOfMadeNode.size(), noSolverIndex, thread.log.numberOfMadeNodes - thread.numberOfMadeNode.size());
        for (size_t position = thread.numberOfMadeTaken; position < thread.log.made.size(); ++position)
            waiting[number].append(position);
        thread.numberOfMadeTaken = thread.log.made.size();
    }
    m_stepTimes.didFinish("rules: made: lists of what waits"_s);

    constexpr size_t candidatesPerShare = 4096;
    Vector<Candidate> candidates;
    Vector<Vector<Candidate>> candidatesOfThread(m_threads.size());
    Vector<Vector<KeyToMake>> keysToMakeOfShare;
    Vector<size_t> firstKeyToMakeOfShare;
    Vector<KeyToMake> keysToMake;
    Vector<uint32_t> madeNodesOfCandidate;
    for (bool isFirstPass = true;; isFirstPass = false) {
        // The key of a masked node names another node, which has to have its number first.
        auto gatherCandidatesOf = [&](unsigned thread) {
            candidatesOfThread[thread].shrink(0);
            waiting[thread].removeAllMatching([&](uint32_t position) {
                ValueFlowNodeKey key = m_threads[thread]->log.made[position].key;
                if (key.kind == ValueFlowMadeNodeKind::Masked) {
                    ValueFlowNodeID node = m_threads[thread]->numberInGraph(key.maskedNode());
                    if (!node)
                        return false;
                    key.payload = node.index();
                }
                candidatesOfThread[thread].append(Candidate { key, key.set ? m_sets.hashOfMembers(key.set) : 0, thread, position });
                return true;
            });
        };
        size_t numberWaiting = 0;
        for (const Vector<uint32_t>& ofThread : waiting)
            numberWaiting += ofThread.size();
        if (numberWaiting < candidatesPerShare) {
            for (unsigned thread = 0; thread < m_threads.size(); ++thread)
                gatherCandidatesOf(thread);
        } else
            forEachIndexInParallel(m_loops, m_threads.size(), gatherCandidatesOf);
        m_stepTimes.didFinish("rules: made: candidates gathered"_s);
        candidates.shrink(0);
        for (const Vector<Candidate>& ofThread : candidatesOfThread)
            candidates.appendVector(ofThread);
        m_stepTimes.didFinish("rules: made: candidates in one list (one thread)"_s);
        if (candidates.isEmpty())
            break;

        auto orderOf = [](const Candidate& candidate) {
            return std::tuple { candidate.key.kind, candidate.key.detail, candidate.key.payload, candidate.hashOfMembers, candidate.key.set.index() };
        };
        parallelSort(m_loops, candidates.mutableSpan(), [&](const Candidate& first, const Candidate& second) {
            return orderOf(first) < orderOf(second);
        });
        m_stepTimes.didFinish("rules: made: candidates sorted"_s);
        // Of the first candidate with a key: the nodes that are there, or where the key stands among those to make, with the highest bit set.
        constexpr uint32_t isToMake = 1u << 31;
        unsigned numberOfShares = (candidates.size() + candidatesPerShare - 1) / candidatesPerShare;
        keysToMakeOfShare.resize(numberOfShares);
        madeNodesOfCandidate.resize(candidates.size());
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            keysToMakeOfShare[share].shrink(0);
            for (size_t index = share * candidatesPerShare; index < std::min(candidates.size(), (share + 1) * candidatesPerShare); ++index) {
                const Candidate& candidate = candidates[index];
                if (index && candidates[index - 1].key == candidate.key)
                    continue;
                // A rule logs a key only after it has looked for it among the made nodes, and none has been made since. What waited for a node of this turn was not looked for.
                ASSERT(!isFirstPass || !findNumberOfMadeNodes(candidate.key));
                if (std::optional<uint32_t> existing = isFirstPass ? std::nullopt : findNumberOfMadeNodes(candidate.key)) {
                    madeNodesOfCandidate[index] = *existing;
                    continue;
                }
                madeNodesOfCandidate[index] = isToMake | keysToMakeOfShare[share].size();
                keysToMakeOfShare[share].append(KeyToMake { candidate.key, candidate.hashOfMembers });
            }
        });
        m_stepTimes.didFinish("rules: made: candidates looked up"_s);
        keysToMake.shrink(0);
        firstKeyToMakeOfShare.shrink(0);
        for (const Vector<KeyToMake>& ofShare : keysToMakeOfShare) {
            firstKeyToMakeOfShare.append(keysToMake.size());
            keysToMake.appendVector(ofShare);
        }
        m_stepTimes.didFinish("rules: made: keys in one list (one thread)"_s);
        makeNodesOf(keysToMake.mutableSpan());
        forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
            ValueFlowNodeID first;
            for (size_t index = share * candidatesPerShare; index < std::min(candidates.size(), (share + 1) * candidatesPerShare); ++index) {
                const Candidate& candidate = candidates[index];
                if (!first || candidates[index - 1].key != candidate.key) {
                    size_t firstWithKey = index;
                    while (firstWithKey && candidates[firstWithKey - 1].key == candidate.key)
                        --firstWithKey;
                    uint32_t madeNodes = madeNodesOfCandidate[firstWithKey];
                    if (madeNodes & isToMake)
                        madeNodes = keysToMake[firstKeyToMakeOfShare[firstWithKey / candidatesPerShare] + (madeNodes & ~isToMake)].madeNodes;
                    first = m_madeNodes[madeNodes].first;
                }
                Thread& thread = *m_threads[candidate.thread];
                uint32_t firstInLog = thread.log.made[candidate.position].first;
                for (unsigned offset = 0; offset < candidate.key.numberOfNodes(); ++offset)
                    thread.numberOfMadeNode[firstInLog + offset] = first.index() + offset;
            }
        });
        m_stepTimes.didFinish("rules: made: numbers written back"_s);
    }
    for (const Vector<uint32_t>& left : waiting)
        RELEASE_ASSERT(left.isEmpty());
}

auto ValueFlowSolver::ensureName(ValueFlowName name) -> Name&
{
    uint64_t key = static_cast<uint64_t>(name.kind) << 32 | name.payload;
    uint32_t number = m_numberOfName.ensure(key, [&] {
        makeRoomByDoubling(m_names);
        m_names.append(Name { name, { }, { } });
        return static_cast<uint32_t>(m_names.size() - 1);
    }).iterator->value;
    return m_names[number];
}

static bool isListedByMember(const ValueSets& sets, ValueSetID set)
{
    return !ValueSets::isComplement(set) && sets.size(set) <= mostObjectsListedByMember;
}

static bool hasFewObjects(const ValueSets& sets, ValueSetID set)
{
    return !ValueSets::isComplement(set) && sets.size(set) <= mostObjectsOfFew;
}

// By name, side and whether the nodes listed have few objects: what the chains of members are found under. The number of a name and that of a member have 31 bits each.
static uint64_t tableOfMembers(uint32_t numberOfName, bool isRead, bool hasFew)
{
    return static_cast<uint64_t>(numberOfName) << 33 | static_cast<uint64_t>(isRead) << 32 | static_cast<uint64_t>(hasFew) << 31;
}

// A node at rest is passed over: it is linked when it wakes.
void ValueFlowSolver::link(const NodeOfSet& node, bool isRead, NodesOfName& others, uint32_t numberOfName, Linked& linked)
{
    auto connect = [&](ValueFlowNodeID other) {
        if (!m_graph.isAtRest(other))
            linked.edges.append(isRead ? ValueFlowEdge { other, node.node } : ValueFlowEdge { node.node, other });
    };
    ValueFlowObjectID onlyObject = m_sets.onlyMember(node.set);
    auto connectWhereSetsMeet = [&](Vector<NodeOfSet>& list) {
        size_t numberAtRest = 0;
        m_sets.forEachSetMeeting(node.set, list.size(), [&](size_t index) {
            if (m_graph.isAtRest(list[index].node)) {
                ++numberAtRest;
                return ValueSetID();
            }
            ++linked.testsWhetherSetsMeet;
            return list[index].set;
        }, [&](size_t index) {
            connect(list[index].node);
        });
        if (numberAtRest * 2 > list.size())
            linked.listsMostlyAtRest.append(&list);
    };

    auto connectByMembers = [&](bool hasFew) {
        uint64_t table = tableOfMembers(numberOfName, !isRead, hasFew);
        const NodesOfMembers& nodesOfMembers = m_linkShards[linkShardOf(numberOfName)].nodesOfMembers[!isRead];
        m_sets.forEachMember(node.set, [&](ValueFlowObjectID member) {
            auto first = nodesOfMembers.first.find(table | member.index());
            if (first == nodesOfMembers.first.end())
                return;
            for (uint32_t entry = first->value; entry != noSolverIndex; entry = nodesOfMembers.chained[entry].next)
                connect(nodesOfMembers.chained[entry].node);
        });
    };

    connectWhereSetsMeet(others.ofVeryManyObjects);
    // Two nodes of many objects may share many: one look at the pair is cheaper than finding it once for each.
    if (hasFewObjects(m_sets, node.set) && !others.ofManyObjects.isEmpty())
        connectByMembers(false);
    else
        connectWhereSetsMeet(others.ofManyObjects);
    if (ValueSets::isComplement(node.set) || m_sets.size(node.set) > std::max<size_t>(mostObjectsLookedUpOneByOne, others.ofFewObjects.size()))
        connectWhereSetsMeet(others.ofFewObjects);
    else if (!others.ofFewObjects.isEmpty())
        connectByMembers(true);
}

void ValueFlowSolver::removeNodesAtRestFrom(Vector<NodeOfSet>& list)
{
    list.removeAllMatching([&](const NodeOfSet& other) {
        if (!m_graph.isAtRest(other.node))
            return false;
        m_madeNodes[m_madeNodesOfNode[other.node.index() - m_input.numberOfNodes]].isListedUnderName = false;
        return true;
    });
}

void ValueFlowSolver::insert(const NodeToLink& toLink, bool isRead, NodesOfName& nodes)
{
    const NodeOfSet& node = toLink.node;
    MadeNodes& madeNodes = m_madeNodes[toLink.madeNodes];
    if (std::exchange(madeNodes.isListedUnderName, true))
        return;
    if (toLink.size == SizeOfSetToLink::VeryMany) {
        nodes.ofVeryManyObjects.append(node);
        return;
    }
    bool hasFew = toLink.size == SizeOfSetToLink::Few;
    (hasFew ? nodes.ofFewObjects : nodes.ofManyObjects).append(node);
    if (std::exchange(madeNodes.isListedUnderMembers, true))
        return;
    uint64_t table = tableOfMembers(toLink.numberOfName, isRead, hasFew);
    NodesOfMembers& nodesOfMembers = m_linkShards[linkShardOf(toLink.numberOfName)].nodesOfMembers[isRead];
    auto add = [&](ValueFlowObjectID member) {
        uint32_t& first = nodesOfMembers.first.add(table | member.index(), noSolverIndex).iterator->value;
        nodesOfMembers.chained.append(ChainedNode { node.node, first });
        first = nodesOfMembers.chained.size() - 1;
    };
    if (toLink.onlyMember)
        add(toLink.onlyMember);
    else
        m_sets.forEachMember(node.set, add);
}

// What is written through a set reaches what is read under a name that may be the same through every set that shares an object with it. Each pair of nodes is looked at once, when the later of the two is made.
void ValueFlowSolver::prepareNodesToLink()
{
    constexpr size_t nodesPerShare = 2048;
    for (ListWithRoomToFill<NodeToLink>* list : { &m_writtenToLink, &m_readToLink }) {
        // All that waits for memory and asks nobody: on all threads. A shard has one thread when its nodes go into the tables.
        Vector<Vector<uint32_t>> withNewNamesOfShare((list->size() + nodesPerShare - 1) / nodesPerShare);
        forEachIndexInParallel(m_loops, withNewNamesOfShare.size(), [&](unsigned share) {
            for (NodeToLink& toLink : list->mutableSpan().subspan(share * nodesPerShare, std::min(nodesPerShare, list->size() - share * nodesPerShare))) {
                ValueSetID set = toLink.node.set;
                toLink.madeNodes = m_madeNodesOfNode[toLink.node.node.index() - m_input.numberOfNodes];
                toLink.size = !isListedByMember(m_sets, set) ? SizeOfSetToLink::VeryMany : hasFewObjects(m_sets, set) ? SizeOfSetToLink::Few : SizeOfSetToLink::Many;
                toLink.isDearToLink = toLink.size == SizeOfSetToLink::VeryMany;
                toLink.onlyMember = m_sets.onlyMember(set);
                if (toLink.standsForKind) {
                    toLink.numberOfName = numberOfFirstKindAmongNames + static_cast<uint32_t>(toLink.name.kind);
                    continue;
                }
                auto found = m_numberOfName.find(static_cast<uint64_t>(toLink.name.kind) << 32 | toLink.name.payload);
                toLink.numberOfName = found == m_numberOfName.end() ? noSolverIndex : found->value;
                if (toLink.numberOfName == noSolverIndex)
                    withNewNamesOfShare[share].append(static_cast<uint32_t>(&toLink - list->mutableSpan().data()));
            }
        });
        // The new names, in the order of the list.
        for (const Vector<uint32_t>& withNewNames : withNewNamesOfShare) {
            for (uint32_t index : withNewNames) {
                NodeToLink& toLink = (*list)[index];
                toLink.numberOfName = static_cast<uint32_t>(&ensureName(toLink.name) - m_names.begin());
            }
        }
    }
}

// A shard first with its nodes that write, then with those that read.
void ValueFlowSolver::linkNamedNodes(unsigned numberOfShard, bool isRead)
{
    Linked& linked = m_linkShards[numberOfShard].linked;
    for (const NodeToLink& toLink : isRead ? m_readToLink : m_writtenToLink) {
        if (linkShardOf(toLink.numberOfName) != numberOfShard || m_graph.isAtRest(toLink.node.node))
            continue;
        linkNode(toLink, isRead, linked);
        for (Vector<NodeOfSet>* list : linked.listsMostlyAtRest)
            removeNodesAtRestFrom(*list);
        linked.listsMostlyAtRest.shrink(0);
        insertNode(toLink, isRead);
    }
}

void ValueFlowSolver::linkNode(const NodeToLink& toLink, bool isRead, Linked& linked)
{
    if (!toLink.standsForKind) {
        Name& name = m_names[toLink.numberOfName];
        link(toLink.node, isRead, isRead ? name.written : name.read, toLink.numberOfName, linked);
        return;
    }
    unsigned kind = static_cast<unsigned>(toLink.name.kind);
    for (uint64_t others = isRead ? m_kindsOfNames[kind].writtenKindsItMayFind : m_kindsOfNames[kind].readKindsThatMayFindIt; others; others &= others - 1) {
        unsigned other = std::countr_zero(others);
        link(toLink.node, isRead, isRead ? m_kindsOfNames[other].written : m_kindsOfNames[other].read, numberOfFirstKindAmongNames + other, linked);
    }
}

void ValueFlowSolver::insertNode(const NodeToLink& toLink, bool isRead)
{
    if (!toLink.standsForKind) {
        Name& name = m_names[toLink.numberOfName];
        insert(toLink, isRead, isRead ? name.read : name.written);
        return;
    }
    unsigned kind = static_cast<unsigned>(toLink.name.kind);
    insert(toLink, isRead, isRead ? m_kindsOfNames[kind].read : m_kindsOfNames[kind].written);
}

// Linking a node reads the tables of the other side alone, so the nodes of one side are linked on all threads, whatever their names. They go into the tables of their own side afterwards.
void ValueFlowSolver::linkInChunks(bool isRead, unsigned numberOfOtherTasks, NOESCAPE const ScopedLambda<void(unsigned task, unsigned worker)>& otherTask)
{
    constexpr size_t nodesPerChunk = 16;
    const ListWithRoomToFill<NodeToLink>& toLink = isRead ? m_readToLink : m_writtenToLink;
    size_t numberOfChunks = (toLink.size() + nodesPerChunk - 1) / nodesPerChunk;
    // One dear node takes as long as thousands of others. Each is a task of its own, and they come first: a loop lasts as long as the task that ends last.
    Vector<uint32_t> dearNodes;
    for (size_t index = 0; index < toLink.size(); ++index) {
        if (toLink[index].isDearToLink)
            dearNodes.append(index);
    }
    Vector<Linked> linkedOfWorker(m_threads.size());
    forEachIndexInParallel(m_loops, dearNodes.size() + numberOfOtherTasks + numberOfChunks, [&](unsigned task, unsigned worker) {
        if (task < dearNodes.size()) {
            if (!m_graph.isAtRest(toLink[dearNodes[task]].node.node))
                linkNode(toLink[dearNodes[task]], isRead, linkedOfWorker[worker]);
            return;
        }
        if (task < dearNodes.size() + numberOfOtherTasks) {
            otherTask(task - dearNodes.size(), worker);
            return;
        }
        size_t chunk = task - numberOfOtherTasks - dearNodes.size();
        for (size_t index = chunk * nodesPerChunk; index < std::min(toLink.size(), (chunk + 1) * nodesPerChunk); ++index) {
            if (!toLink[index].isDearToLink && !m_graph.isAtRest(toLink[index].node.node))
                linkNode(toLink[index], isRead, linkedOfWorker[worker]);
        }
    });

    Vector<Vector<NodeOfSet>*> listsMostlyAtRest;
    for (const Linked& linked : linkedOfWorker) {
        m_edgesOfRound.appendVector(linked.edges);
        m_counters.testsWhetherSetsMeet += linked.testsWhetherSetsMeet;
        listsMostlyAtRest.appendVector(linked.listsMostlyAtRest);
    }
    AOT::sort(listsMostlyAtRest);
    removeRepeatedElements(listsMostlyAtRest);
    for (Vector<NodeOfSet>* list : listsMostlyAtRest)
        removeNodesAtRestFrom(*list);

    for (size_t index = 0; index < toLink.size(); ++index) {
        if (!m_graph.isAtRest(toLink[index].node.node))
            m_nodesToInsertOfShards[linkShardOf(toLink[index].numberOfName)].append(index);
    }
    if (!isRead) {
        insertNodesOfShards(m_writtenToLink, false);
        return;
    }
    std::swap(m_readToLink, m_readToInsert);
    m_readToLink.shrink(0);
    m_insertionOfNodesThatRead = BackgroundTask::start(m_loops, TaskPriority::Foreground, { }, [this] {
        insertNodesOfShards(m_readToInsert, true);
    });
}

void ValueFlowSolver::insertNodesOfShards(const ListWithRoomToFill<NodeToLink>& toInsert, bool isRead)
{
    forEachIndexInParallel(m_loops, numberOfLinkShards, [&](unsigned task) {
        Vector<uint32_t>& nodesOfShard = m_nodesToInsertOfShards[numberOfLinkShards - 1 - task];
        constexpr size_t distance = 8;
        for (size_t position = 0; position < nodesOfShard.size(); ++position) {
            if (position + distance < nodesOfShard.size())
                __builtin_prefetch(&m_madeNodes[toInsert[nodesOfShard[position + distance]].madeNodes], 1);
            insertNode(toInsert[nodesOfShard[position]], isRead);
        }
        nodesOfShard.shrink(0);
    });
}

void ValueFlowSolver::finishInsertionOfNodesThatRead()
{
    if (RefPtr<BackgroundTask> task = std::exchange(m_insertionOfNodesThatRead, nullptr))
        task->finish();
}

// Shard by shard, so that the order of the edges does not depend on who was done first.
void ValueFlowSolver::takeEdgesOfLinkShards()
{
    for (LinkShard& shard : m_linkShards) {
        m_edgesOfRound.appendVector(shard.linked.edges);
        shard.linked.edges.shrink(0);
        m_counters.testsWhetherSetsMeet += std::exchange(shard.linked.testsWhetherSetsMeet, 0);
    }
    m_writtenToLink.shrink(0);
    m_readToLink.shrink(0);
}

void ValueFlowSolver::noteReaders(unsigned numberOfShard, ValueSetMaker& maker)
{
    ReaderShard& shard = m_readerShards[numberOfShard];
    constexpr size_t distance = 12;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        Vector<NodeRead>& read = thread->nodesReadOfShard[numberOfShard];
        for (size_t index = 0; index < read.size(); ++index) {
            if (index + distance < read.size()) {
                const NodeRead& ahead = read[index + distance];
                shard.known.prefetch(ahead.node.index(), ahead.statement.index());
                __builtin_prefetch(&m_firstReaderOfNode[ahead.node.index()], 1);
            }
            auto [node, statement] = read[index];
            if (!shard.known.add(node.index(), statement.index()))
                continue;
            if (m_firstReaderOfNode[node.index()] == noSolverIndex)
                m_seenByReadersOfNode[node.index()] = valueForReadersOf(node, maker);
            shard.readers.append(Reader { statement, m_firstReaderOfNode[node.index()] });
            m_firstReaderOfNode[node.index()] = shard.readers.size() - 1;
        }
        read.shrink(0);
    }

    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (auto [node, statement] : thread->nodesAskedWhetherEmptyOfShard[numberOfShard]) {
            if (!shard.knownToWaitForSomething.add(node.index(), statement.index()))
                continue;
            uint32_t& first = shard.firstReaderWaitingForSomething.add(node.index(), noSolverIndex).iterator->value;
            shard.readers.append(Reader { statement, first });
            first = shard.readers.size() - 1;
            m_isWaitedForToHoldSomething[node.index()] = 1;
        }
        thread->nodesAskedWhetherEmptyOfShard[numberOfShard].shrink(0);
    }
}

void ValueFlowSolver::noteWatchers(unsigned numberOfShard)
{
    WatchShard& shard = m_watchShards[numberOfShard];
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (const Thread::PartWatched& part : thread->partsWatchedOfShard[numberOfShard])
            watch(shard, part.node, part.within, part.statement, part.isAskedLast);
        thread->partsWatchedOfShard[numberOfShard].shrink(0);
    }
}

void ValueFlowSolver::chainSetsWatchedForCover()
{
    AOT_THRESHOLD(size_t, fewestMembersForThreads, 4096);
    size_t numberOfMembers = 0;
    size_t endOfMembers = 0;
    std::span<WatchShard> shards = std::span(m_watchShards).subspan(numberOfWatchShardsByNode);
    for (WatchShard& shard : shards) {
        for (const Vector<MemberOfSetWatchedForCover>& members : shard.membersToChain)
            numberOfMembers += members.size();
        endOfMembers = std::max<size_t>(endOfMembers, std::exchange(shard.endOfMembersToChain, 0));
    }
    if (!numberOfMembers)
        return;
    Vector<uint32_t>& firstOfMember = m_firstSetWatchedForCoverOfMember;
    if (endOfMembers > firstOfMember.size())
        firstOfMember.insertFill(firstOfMember.size(), noSolverIndex, std::max(endOfMembers, firstOfMember.size() * 2) - firstOfMember.size());
    auto chain = [&](unsigned range) {
        Vector<ChainedWatch>& chained = m_setsWatchedForCoverOfMembers[range];
        for (WatchShard& shard : shards) {
            for (MemberOfSetWatchedForCover member : shard.membersToChain[range])
                chained.append(ChainedWatch { member.set, std::exchange(firstOfMember[member.member], static_cast<uint32_t>(chained.size())) });
            shard.membersToChain[range].shrink(0);
        }
    };
    if (numberOfMembers < fewestMembersForThreads) {
        for (unsigned range = 0; range < numberOfRangesOfMembers; ++range)
            chain(range);
    } else
        forEachIndexInParallel(m_loops, numberOfRangesOfMembers, chain);
}

size_t ValueFlowSolver::numberOfReaders() const
{
    size_t number = 0;
    for (const WatchShard& shard : m_watchShards)
        number += shard.readers.size();
    for (const ReaderShard& shard : m_readerShards)
        number += shard.readers.size();
    return number;
}

void ValueFlowSolver::chainByMember(WatchShard& shard, uint32_t watchedNode, uint32_t number)
{
    ValueSetID within = shard.watches[number].within;
    if (shard.watchedNodes[watchedNode].holdsWhatPlaceholderCovers) {
        auto addedSet = shard.numberOfSetWatchedForCover.add(within.index(), shard.watchesOfSetWatchedForCover.size());
        uint32_t numberOfSet = addedSet.iterator->value;
        if (addedSet.isNewEntry) {
            shard.watchesOfSetWatchedForCover.append(Vector<uint32_t, 2>());
            uint32_t nameOfSet = numberOfSet * numberOfWatchShardsBySet + static_cast<uint32_t>(&shard - m_watchShards.data() - numberOfWatchShardsByNode);
            m_sets.forEachMember(within, [&](ValueFlowObjectID member) {
                shard.membersToChain[rangeOfMember(member.index())].append({ member.index(), nameOfSet });
                shard.endOfMembersToChain = std::max(shard.endOfMembersToChain, member.index() + 1);
            });
        }
        shard.watchesOfSetWatchedForCover[numberOfSet].append(number);
        return;
    }
    m_sets.forEachMember(within, [&](ValueFlowObjectID member) {
        uint32_t& first = shard.firstWatchOfMember.add(static_cast<uint64_t>(watchedNode) << 32 | member.index(), noSolverIndex).iterator->value;
        shard.watchesOfMembers.append(ChainedWatch { number, first });
        first = shard.watchesOfMembers.size() - 1;
    });
}

void ValueFlowSolver::watch(WatchShard& shard, ValueFlowNodeID node, ValueSetID within, ValueFlowStatementID statement, bool isAskedLast)
{
    auto added = shard.watchOfPart.add(static_cast<uint64_t>(node.index()) << 32 | within.index(), shard.watches.size());
    uint32_t number = added.iterator->value;
    if (added.isNewEntry) {
        shard.watches.append(Watch { within, noSolverIndex, noSolverIndex, 0 });
        uint32_t watchedNode = shard.watchedNodeOfNode.ensure(node.index(), [&] {
            bool holdsWhatPlaceholderCovers = this->holdsWhatPlaceholderCovers(node);
            shard.watchedNodes.append(WatchedNode { node, valueOf(node), { }, { }, holdsWhatPlaceholderCovers });
            if (!holdsWhatPlaceholderCovers)
                m_isWatched.quickSet(node.index());
            return static_cast<uint32_t>(shard.watchedNodes.size() - 1);
        }).iterator->value;
        shard.watches[number].watchedNode = watchedNode;
        shard.watchedNodes[watchedNode].watches.append(number);
        AOT_THRESHOLD(size_t, mostWatchesWithoutChains, 8);
        WatchedNode& watched = shard.watchedNodes[watchedNode];
        if (!isListedByMember(m_sets, within) || (!watched.holdsWhatPlaceholderCovers && !watched.hasChainsByMember && watched.watches.size() <= mostWatchesWithoutChains))
            watched.watchesOfManyObjects.append(number);
        else {
            if (!std::exchange(watched.hasChainsByMember, true)) {
                watched.watchesOfManyObjects.removeAllMatching([&](uint32_t earlier) {
                    if (!isListedByMember(m_sets, shard.watches[earlier].within))
                        return false;
                    chainByMember(shard, watchedNode, earlier);
                    return true;
                });
            }
            chainByMember(shard, watchedNode, number);
        }
    }
    if (!shard.knownWatchers.add(number, statement.index()))
        return;
    uint32_t& firstReader = isAskedLast ? shard.watches[number].firstReaderAskingLast : shard.watches[number].firstReader;
    shard.readers.append(Reader { statement, firstReader });
    firstReader = shard.readers.size() - 1;
}

void ValueFlowSolver::listReadersOfWatch(const WatchShard& shard, uint32_t watch, Vector<ValueFlowStatementID>& readers) const
{
    for (uint32_t reader = shard.watches[watch].firstReader; reader != noSolverIndex; reader = shard.readers[reader].next)
        listIfItMayBecomeDue(shard.readers[reader].statement, readers);
    for (uint32_t reader = shard.watches[watch].firstReaderAskingLast; reader != noSolverIndex; reader = shard.readers[reader].next) {
        ValueFlowStatementID statement = shard.readers[reader].statement;
        if (isAtRest(statement) || m_isDue.get(statement.index()) || m_isDueLast.set(statement.index()))
            continue;
        Locker locker { m_lockOfDueLast };
        m_dueLast.append(statement);
    }
}

// The watches of single members are left to whoever goes through the new members: one node may get a hundred thousand of them at once.
void ValueFlowSolver::listWatchersOf(ValueFlowNodeID node, ValueSetMaker& maker, Vector<ValueFlowStatementID>& readers, Vector<NewMemberOfWatchedNode>& newMembers, Vector<WatchesToTest>& watchesToTest)
{
    if (holdsWhatPlaceholderCovers(node)) {
        ValueSetID value = valueOf(node);
        ValueSetID whatIsNew = maker.intersect(value, ValueSets::complement(std::exchange(seenByWatchersOf(node), value)));
        if (!ValueSets::isComplement(whatIsNew)) {
            m_sets.forEachMember(whatIsNew, [&](ValueFlowObjectID member) {
                if (member.index() < m_firstSetWatchedForCoverOfMember.size() && m_firstSetWatchedForCoverOfMember[member.index()] != noSolverIndex)
                    newMembers.append({ numberOfWatchShards, node.index(), member });
            });
        }
        for (unsigned numberOfShard = numberOfWatchShardsByNode; numberOfShard < numberOfWatchShards; ++numberOfShard) {
            const WatchShard& shard = m_watchShards[numberOfShard];
            auto number = shard.watchedNodeOfNode.find(node.index());
            if (number == shard.watchedNodeOfNode.end())
                continue;
            const WatchedNode& watchedNode = shard.watchedNodes[number->value];
            if (ValueSets::isComplement(whatIsNew)) {
                for (uint32_t watch : watchedNode.watches)
                    listReadersOfWatch(shard, watch, readers);
            } else if (!watchedNode.watchesOfManyObjects.isEmpty())
                watchesToTest.append({ numberOfShard, number->value, whatIsNew });
        }
        return;
    }
    unsigned numberOfShard = watchShardOf(node, { });
    WatchShard& shard = m_watchShards[numberOfShard];
    uint32_t number = shard.watchedNodeOfNode.get(node.index());
    WatchedNode& watchedNode = shard.watchedNodes[number];
    ValueSetID value = valueOf(node);
    ValueSetID whatIsNew = maker.intersect(value, ValueSets::complement(watchedNode.seen));
    watchedNode.seen = value;
    if (ValueSets::isComplement(whatIsNew)) {
        for (uint32_t watch : watchedNode.watches)
            listReadersOfWatch(shard, watch, readers);
        return;
    }
    if (watchedNode.hasChainsByMember) {
        m_sets.forEachMember(whatIsNew, [&](ValueFlowObjectID member) {
            newMembers.append({ numberOfShard, number, member });
        });
    }
    if (!watchedNode.watchesOfManyObjects.isEmpty())
        watchesToTest.append({ numberOfShard, number, whatIsNew });
}

// Answers the list of uses with one more in front.
ALWAYS_INLINE uint32_t ValueFlowSolver::takeUse(UsesOfThread& uses)
{
    constexpr size_t usesInBlock = 256;
    for (;;) {
        if (uses.nextFree < uses.endOfFree)
            return m_freeUses[uses.nextFree++];
        if (uses.nextFresh < uses.endOfFresh)
            return static_cast<uint32_t>(uses.nextFresh++);
        size_t left = m_numberOfFreeUsesLeft.load(std::memory_order_relaxed);
        while (left) {
            size_t taken = std::min(left, usesInBlock);
            if (m_numberOfFreeUsesLeft.compare_exchange_weak(left, left - taken, std::memory_order_relaxed)) {
                uses.nextFree = left - taken;
                uses.endOfFree = left;
                break;
            }
        }
        if (uses.nextFree < uses.endOfFree)
            continue;
        uses.nextFresh = m_uses.makeRoomToFill(usesInBlock);
        uses.endOfFresh = uses.nextFresh + usesInBlock;
    }
}

void ValueFlowSolver::letThreadsTakeUses()
{
    m_usesOfThreads.fill(UsesOfThread { }, m_threads.size());
    m_numberOfFreeUsesLeft.store(m_freeUses.size(), std::memory_order_relaxed);
}

// What a thread has taken and not given out is free again.
void ValueFlowSolver::takeBackUsesOfThreads()
{
    Vector<uint32_t, 64> left;
    for (const UsesOfThread& uses : m_usesOfThreads) {
        left.append(m_freeUses.subspan(uses.nextFree, uses.endOfFree - uses.nextFree));
        for (size_t use = uses.nextFresh; use < uses.endOfFresh; ++use)
            left.append(static_cast<uint32_t>(use));
    }
    m_freeUses.shrink(m_numberOfFreeUsesLeft.load(std::memory_order_relaxed));
    m_freeUses.appendVector(left);
}

// While somebody asks for nodes, they ask for their parts. The counts are raised by whoever comes; whoever raises one from zero takes up the parts of those nodes at once.
void ValueFlowSolver::takeUpPartsOfNodesWithFirstUser(Vector<uint32_t>&& nodes)
{
    if (nodes.isEmpty())
        return;
    constexpr size_t nodesPerShare = 128;
    struct Share {
        Vector<uint32_t> woken;
        Vector<ValueFlowStatementID> due;
    };
    Vector<Share> shares((nodes.size() + nodesPerShare - 1) / nodesPerShare);
    letThreadsTakeUses();
    forEachIndexInParallel(m_loops, shares.size(), [&](unsigned index, unsigned worker) {
        Share& share = shares[index];
        UsesOfThread& uses = m_usesOfThreads[worker];
        Vector<uint32_t, 128> toTakeUp;
        toTakeUp.append(nodes.subspan(index * nodesPerShare, std::min(nodesPerShare, nodes.size() - index * nodesPerShare)));
        while (!toTakeUp.isEmpty()) {
            uint32_t number = toTakeUp.takeLast();
            MadeNodes& madeNodes = m_madeNodes[number];
            if (madeNodes.hasBeenAtRest)
                share.woken.append(number);
            if (madeNodes.statement && !std::exchange(madeNodes.hasStatementRunWhereMade, false))
                share.due.append(madeNodes.statement);
            for (uint32_t part : m_partsOfMadeNodes.subspan(madeNodes.firstPart, madeNodes.numberOfParts)) {
                if (!WTF::atomicExchangeAdd(&m_numberOfUsers[part], 1u, std::memory_order_relaxed))
                    toTakeUp.append(part);
                uint32_t use = takeUse(uses);
                m_uses[use] = Use { part, madeNodes.firstUseOfParts };
                madeNodes.firstUseOfParts = use;
            }
        }
    });
    takeBackUsesOfThreads();
    for (const Share& share : shares) {
        for (uint32_t number : share.woken) {
            const MadeNodes& madeNodes = m_madeNodes[number];
            --m_counters.madeNodesAtRest;
            m_graph.wake(madeNodes.first, madeNodes.key.numberOfNodes());
            // Nothing was linked to it while it was at rest.
            switch (madeNodes.key.kind) {
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::ReadUnderKind:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
                if (!madeNodes.isUnionOfParts)
                    listToLink(madeNodes);
                break;
            case ValueFlowMadeNodeKind::Constant:
            case ValueFlowMadeNodeKind::Lookup:
            case ValueFlowMadeNodeKind::CallGroup:
            case ValueFlowMadeNodeKind::Hub:
            case ValueFlowMadeNodeKind::Masked:
            case ValueFlowMadeNodeKind::Marked:
            case ValueFlowMadeNodeKind::Instance:
                break;
            }
        }
        for (ValueFlowStatementID statement : share.due) {
            if (!m_isDue.set(statement.index()))
                m_due.append(statement);
        }
    }
}

// Nodes that nobody asks for any more go to rest, and let go of their parts and of what their statement has asked for: whoever brings a count to zero does that at once.
void ValueFlowSolver::letGoOf(Vector<uint32_t>&& lists)
{
    if (lists.isEmpty())
        return;
    constexpr size_t listsPerShare = 32;
    struct Share {
        Vector<uint32_t> freedUses;
        Vector<ValueFlowStatementID> statements;
        uint32_t numberOfNodesPutToRest { 0 };
    };
    Vector<Share> shares((lists.size() + listsPerShare - 1) / listsPerShare);
    forEachIndexInParallel(m_loops, shares.size(), [&](unsigned index) {
        Share& share = shares[index];
        Vector<uint32_t, 64> toLetGo;
        toLetGo.append(lists.subspan(index * listsPerShare, std::min(listsPerShare, lists.size() - index * listsPerShare)));
        while (!toLetGo.isEmpty()) {
            for (uint32_t use = toLetGo.takeLast(); use != noSolverIndex; use = m_uses[use].next) {
                share.freedUses.append(use);
                uint32_t number = m_uses[use].madeNodes;
                if (WTF::atomicExchangeAdd(&m_numberOfUsers[number], static_cast<uint32_t>(-1), std::memory_order_relaxed) != 1)
                    continue;
                ++share.numberOfNodesPutToRest;
                MadeNodes& madeNodes = m_madeNodes[number];
                madeNodes.hasBeenAtRest = true;
                m_graph.putToRest(madeNodes.first, madeNodes.key.numberOfNodes());
                // What its statement has asked for is no longer asked for by it. Once it wakes it has to ask for all of that again, so it is shown everything again.
                if (madeNodes.statement) {
                    if (uint32_t list = std::exchange(m_firstUseOfStatement[madeNodes.statement.index()], noSolverIndex); list != noSolverIndex)
                        toLetGo.append(list);
                    share.statements.append(madeNodes.statement);
                }
                if (uint32_t list = std::exchange(madeNodes.firstUseOfParts, noSolverIndex); list != noSolverIndex)
                    toLetGo.append(list);
            }
        }
    });
    size_t numberOfFreedUses = 0;
    for (const Share& share : shares)
        numberOfFreedUses += share.freedUses.size();
    makeRoomByDoubling(m_freeUses, numberOfFreedUses);
    for (const Share& share : shares) {
        m_freeUses.appendVector(share.freedUses);
        m_counters.madeNodesAtRest += share.numberOfNodesPutToRest;
        if (m_valuesShownToStatement.isEmpty())
            continue;
        for (ValueFlowStatementID statement : share.statements)
            m_valuesShownToStatement.remove(statement.index());
    }
}

void ValueFlowSolver::noteFunctionsAsked()
{
    for (const std::unique_ptr<Thread>& owned : m_threads) {
        Thread& thread = *owned;
        for (const ValueFlowRoundLog::Run& run : thread.log.runs.subspan(thread.numberOfRunsTaken)) {
            for (; thread.numberOfFunctionsAskedTaken < run.endOfFunctionsAsked; ++thread.numberOfFunctionsAskedTaken) {
                FunctionID function = thread.log.functionsAsked[thread.numberOfFunctionsAskedTaken];
                if (m_isReached.get(function.index())) {
                    makeDue(run.statement);
                    continue;
                }
                Vector<ValueFlowStatementID>& readers = m_readersOfFunction.add(function.index(), Vector<ValueFlowStatementID>()).iterator->value;
                if (!readers.contains(run.statement))
                    readers.append(run.statement);
            }
        }
    }
}

// After each turn, in which a statement runs once at the most: its last run says what the statement asks for.
void ValueFlowSolver::noteNodesAskedFor()
{
    growAndFill(m_firstUseOfStatement, numberOfStatements(), noSolverIndex);
    // A statement runs once in a turn at the most, so nothing that is looked at here changes before all logs are through.
    constexpr size_t runsPerShare = 256;
    struct ShareOfRuns {
        unsigned thread { 0 };
        size_t firstRun { 0 };
        size_t endOfRuns { 0 };
        Vector<Thread::RunThatAsksAnew> runsThatAskAnew;
        Vector<uint32_t> askedForAnew;
        Vector<uint32_t> usesBefore;
        Vector<uint32_t> nodesWithFirstUser;
    };
    Vector<ShareOfRuns> sharesOfRuns;
    for (unsigned index = 0; index < m_threads.size(); ++index) {
        size_t endOfRuns = m_threads[index]->log.runs.size();
        for (size_t firstRun = m_threads[index]->numberOfRunsTaken; firstRun < endOfRuns; firstRun += runsPerShare)
            sharesOfRuns.append(ShareOfRuns { index, firstRun, std::min(endOfRuns, firstRun + runsPerShare), { }, { }, { }, { } });
    }
    letThreadsTakeUses();
    forEachIndexInParallel(m_loops, sharesOfRuns.size(), [&](unsigned index, unsigned worker) {
        ShareOfRuns& share = sharesOfRuns[index];
        const Thread& thread = *m_threads[share.thread];
        AOT_THRESHOLD(size_t, mostComparedOneByOne, 32);
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seen;
        bool isFirstOfTurn = share.firstRun == thread.numberOfRunsTaken;
        uint32_t valuesShown = isFirstOfTurn ? thread.numberOfValuesShownTaken : thread.log.runs[share.firstRun - 1].endOfValuesShown;
        uint32_t nodeAskedFor = isFirstOfTurn ? thread.numberOfNodesAskedForTaken : thread.log.runs[share.firstRun - 1].endOfNodesAskedFor;
        for (size_t number = share.firstRun; number < share.endOfRuns; ++number) {
            const ValueFlowRoundLog::Run& run = thread.log.runs[number];
            size_t first = share.askedForAnew.size();
            seen.clear();
            for (; nodeAskedFor < run.endOfNodesAskedFor; ++nodeAskedFor) {
                uint32_t madeNodes = thread.log.madeNodesAskedFor[nodeAskedFor];
                if (madeNodes & ValueFlowRoundLog::numberedByLog) [[unlikely]] {
                    madeNodes = m_madeNodesOfNode[thread.numberInGraph(ValueFlowNodeID(madeNodes)).index() - m_input.numberOfNodes];
                    if (!keepsAwake(run.statement, madeNodes))
                        continue;
                }
                std::span<const uint32_t> askedFor = share.askedForAnew.span().subspan(first);
                if (askedFor.size() < mostComparedOneByOne) {
                    if (std::ranges::find(askedFor, madeNodes) != askedFor.end())
                        continue;
                } else {
                    if (seen.isEmpty()) {
                        for (uint32_t earlier : askedFor)
                            seen.add(earlier);
                    }
                    if (!seen.add(madeNodes).isNewEntry)
                        continue;
                }
                share.askedForAnew.append(madeNodes);
            }
            // A run that was shown only what is new in a node has asked on behalf of that alone. What the runs before it have asked for stays asked for.
            bool addsToEarlierRuns = std::exchange(valuesShown, run.endOfValuesShown) < run.endOfValuesShown;
            if (!addsToEarlierRuns) {
                uint32_t use = m_firstUseOfStatement[run.statement.index()];
                size_t left = share.askedForAnew.size();
                for (; use != noSolverIndex && left > first && m_uses[use].madeNodes == share.askedForAnew[left - 1]; --left)
                    use = m_uses[use].next;
                if (use == noSolverIndex && left == first) {
                    share.askedForAnew.shrink(first);
                    continue;
                }
            }
            share.runsThatAskAnew.append({ number, static_cast<uint32_t>(share.askedForAnew.size()), addsToEarlierRuns });
        }
        // A statement has one run in a turn, so its list has one writer. The counts are raised by whoever comes; whoever raises one from zero says so.
        uint32_t next = 0;
        for (const Thread::RunThatAsksAnew& asksAnew : share.runsThatAskAnew) {
            uint32_t& firstUse = m_firstUseOfStatement[thread.log.runs[asksAnew.run].statement.index()];
            if (!asksAnew.addsToEarlierRuns && firstUse != noSolverIndex)
                share.usesBefore.append(std::exchange(firstUse, noSolverIndex));
            for (; next < asksAnew.endOfAskedFor; ++next) {
                constexpr uint32_t distance = 8;
                if (next + distance < share.askedForAnew.size())
                    __builtin_prefetch(&m_numberOfUsers[share.askedForAnew[next + distance]], 1);
                uint32_t number = share.askedForAnew[next];
                if (!WTF::atomicExchangeAdd(&m_numberOfUsers[number], 1u, std::memory_order_relaxed))
                    share.nodesWithFirstUser.append(number);
                uint32_t use = takeUse(m_usesOfThreads[worker]);
                m_uses[use] = Use { number, firstUse };
                firstUse = use;
            }
        }
    });
    takeBackUsesOfThreads();

    for (ShareOfRuns& share : sharesOfRuns) {
        Thread& thread = *m_threads[share.thread];
        thread.numberOfRunsTaken = share.endOfRuns;
        thread.numberOfNodesAskedForTaken = thread.log.runs[share.endOfRuns - 1].endOfNodesAskedFor;
        for (const Thread::RunThatAsksAnew& asksAnew : share.runsThatAskAnew) {
            const ValueFlowRoundLog::Run& run = thread.log.runs[asksAnew.run];
            // The runs in between were shown nothing of this sort: they would be among these.
            for (; thread.numberOfValuesShownTaken < run.endOfValuesShown; ++thread.numberOfValuesShownTaken) {
                ValueFlowNodeSet shown = thread.log.valuesShown[thread.numberOfValuesShownTaken];
                shown.node = thread.numberInGraph(shown.node);
                Vector<ValueFlowNodeSet, 1>& valuesShown = m_valuesShownToStatement.add(run.statement.index(), Vector<ValueFlowNodeSet, 1>()).iterator->value;
                size_t position = valuesShown.findIf([&](const ValueFlowNodeSet& earlier) {
                    return earlier.node == shown.node;
                });
                if (position == notFound)
                    valuesShown.append(shown);
                else
                    valuesShown[position].set = shown.set;
            }
        }
    }

    Vector<uint32_t> nodesWithFirstUser;
    Vector<uint32_t> usesBefore;
    for (const ShareOfRuns& share : sharesOfRuns) {
        nodesWithFirstUser.appendVector(share.nodesWithFirstUser);
        usesBefore.appendVector(share.usesBefore);
    }
    takeUpPartsOfNodesWithFirstUser(WTF::move(nodesWithFirstUser));
    // Afterwards, so that what one statement lets go of and another one takes up does not go to rest in between, whichever of the two comes first.
    letGoOf(WTF::move(usesBefore));
}

void ValueFlowSolver::countRunsAndEdges()
{
    AOT_THRESHOLD(uint32_t, fewestConnectionsOfDearStatement, 4096);
    m_runsAndEdges.grow(numberOfOpcodes + 256);
    struct CountsOfLog {
        Vector<RunsAndEdges> runsAndEdges;
        Vector<std::pair<ValueFlowStatementID, bool>> statementsThatTurnDearOrCheap;
        Vector<ValueFlowStatementID> statementsThatWereNotDueAndAdded;
    };
    Vector<CountsOfLog> countsOfLog(m_threads.size());
    forEachIndexInParallel(m_loops, m_threads.size(), [&](unsigned number) {
        CountsOfLog& counts = countsOfLog[number];
        counts.runsAndEdges.grow(m_runsAndEdges.size());
        uint32_t edge = 0;
        uint32_t reached = 0;
        uint32_t made = 0;
        uint32_t partWatched = 0;
        size_t nextWatchedLast = 0;
        uint32_t nodeRead = 0;
        uint32_t nodeAskedWhetherEmpty = 0;
        Thread& thread = *m_threads[number];
        for (const ValueFlowRoundLog::Run& run : thread.log.runs) {
            for (; nodeRead < run.endOfNodesRead; ++nodeRead) {
                ValueFlowNodeID node = thread.numberInGraph(thread.log.nodesRead[nodeRead]);
                thread.nodesReadOfShard[readerShardOf(node)].append({ node, run.statement });
            }
            for (; nodeAskedWhetherEmpty < run.endOfNodesAskedWhetherEmpty; ++nodeAskedWhetherEmpty) {
                ValueFlowNodeID node = thread.numberInGraph(thread.log.nodesAskedWhetherEmpty[nodeAskedWhetherEmpty]);
                thread.nodesAskedWhetherEmptyOfShard[readerShardOf(node)].append({ node, run.statement });
            }
            for (; partWatched < run.endOfPartsWatched; ++partWatched) {
                ValueFlowNodeID node = thread.numberInGraph(thread.log.partsWatched[partWatched].node);
                ValueSetID within = thread.log.partsWatched[partWatched].set;
                bool isAskedLast = nextWatchedLast < thread.log.partsWatchedLast.size() && thread.log.partsWatchedLast[nextWatchedLast] == partWatched;
                nextWatchedLast += isAskedLast;
                thread.partsWatchedOfShard[watchShardOf(node, within)].append({ node, within, run.statement, isAskedLast });
            }
            const ValueFlowStatement& statement = statementAt(run.statement);
            unsigned index = statement.kind == ValueFlowStatementKind::Operation ? static_cast<unsigned>(statement.opcode) : numberOfOpcodes + static_cast<unsigned>(statement.kind);
            RunsAndEdges& row = counts.runsAndEdges[index];
            ++row.runs;
            bool hasReached = std::exchange(reached, run.endOfReached) != run.endOfReached;
            bool hasMade = std::exchange(made, run.endOfMade) != run.endOfMade;
            bool hasAdded = hasReached || hasMade || edge != run.endOfEdges;
            row.runsThatAddedNothing += !hasAdded;
            row.runsThatWereNotDueAndAdded += hasAdded && m_isFullRound;
            if (hasAdded && m_isFullRound)
                counts.statementsThatWereNotDueAndAdded.append(run.statement);
            row.connections += run.numberOfConnections;
            bool isDear = run.numberOfConnections > fewestConnectionsOfDearStatement;
            if (m_isDear.get(run.statement.index()) != isDear)
                counts.statementsThatTurnDearOrCheap.append({ run.statement, isDear });
            row.edgesLogged += run.endOfEdges - std::exchange(edge, run.endOfEdges);
        }
    });
    for (const CountsOfLog& counts : countsOfLog) {
        for (unsigned index = 0; index < m_runsAndEdges.size(); ++index) {
            const RunsAndEdges& row = counts.runsAndEdges[index];
            m_runsAndEdges[index].runs += row.runs;
            m_runsAndEdges[index].runsThatAddedNothing += row.runsThatAddedNothing;
            m_runsAndEdges[index].runsThatWereNotDueAndAdded += row.runsThatWereNotDueAndAdded;
            m_runsAndEdges[index].connections += row.connections;
            m_runsAndEdges[index].edgesLogged += row.edgesLogged;
        }
        for (auto [statement, isDear] : counts.statementsThatTurnDearOrCheap) {
            // A statement that has run in several turns of the round may stand in several logs.
            if (m_isDear.get(statement.index()) == isDear)
                continue;
            m_isDear.set(statement.index(), isDear);
            if (isDear)
                m_dearStatements.append(statement);
            else
                m_dearStatements.removeFirst(statement);
        }
    }

    AOT_THRESHOLD(size_t, mostStatementsKept, 16);
    Vector<ValueFlowStatementID> statementsOfRound;
    for (const CountsOfLog& counts : countsOfLog)
        statementsOfRound.appendVector(counts.statementsThatWereNotDueAndAdded);
    AOT::sort(statementsOfRound);
    m_numberOfRunsThatWereNotDueAndAdded += statementsOfRound.size();
    size_t room = mostStatementsKept - m_firstStatementsThatWereNotDueAndAdded.size();
    m_firstStatementsThatWereNotDueAndAdded.append(statementsOfRound.span().first(std::min(room, statementsOfRound.size())));
}

static uint64_t numberOf(ValueFlowEdge edge)
{
    return static_cast<uint64_t>(edge.from.index()) << 32 | edge.to.index();
}

void ValueFlowSolver::rememberWhoLoggedTheEdges()
{
    struct Logged {
        uint64_t edge;
        ValueFlowStatementID statement;
    };
    Vector<Logged> logged;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        uint32_t edge = 0;
        for (const ValueFlowRoundLog::Run& run : thread->log.runs) {
            for (; edge < run.endOfEdges; ++edge)
                logged.append(Logged { numberOf(ValueFlowEdge { thread->numberInGraph(thread->log.edges[edge].from), thread->numberInGraph(thread->log.edges[edge].to) }), run.statement });
        }
    }
    AOT::sort(logged, [](const Logged& first, const Logged& second) {
        return std::tuple { first.edge, first.statement } < std::tuple { second.edge, second.statement };
    });
    for (const Logged& entry : logged) {
        if (m_statementOfEdge.add(entry.edge, entry.statement).isNewEntry)
            m_edgesInOrderLogged.append(LoggedEdge { ValueFlowEdge { ValueFlowNodeID(static_cast<uint32_t>(entry.edge >> 32)), ValueFlowNodeID(static_cast<uint32_t>(entry.edge)) }, entry.statement, m_counters.rounds });
    }
}

ValueFlowStatementID ValueFlowSolver::statementThatLogged(ValueFlowEdge edge) const
{
    return m_statementOfEdge.get(numberOf(edge));
}

const ValueFlowNodeKey* ValueFlowSolver::keyOfMadeNode(ValueFlowNodeID node) const
{
    return node.index() < m_input.numberOfNodes ? nullptr : &m_madeNodes[m_madeNodesOfNode[node.index() - m_input.numberOfNodes]].key;
}

Vector<ValueFlowNodeID> ValueFlowSolver::explain(ValueFlowNodeID start, ValueFlowObjectID member) const
{
    if (!m_sets.contains(valueOf(start), member))
        return { };

    // Breadth first, against the edges, through the nodes that hold what is looked for: the member, and behind a mask that has put the member in the place of a placeholder, the placeholder.
    struct Step {
        ValueFlowNodeID node;
        ValueFlowObjectID lookedFor;
        uint32_t cameFrom { noSolverIndex };
    };
    Vector<Step> found { Step { start, member, noSolverIndex } };
    ValueFlowPairSet seen;
    seen.add(start.index(), member.index());
    uint32_t end = noSolverIndex;
    for (uint32_t next = 0; next < found.size(); ++next) {
        Step step = found[next];
        const ValueFlowNodeKey* key = keyOfMadeNode(step.node);
        if (key && key->kind == ValueFlowMadeNodeKind::Constant) {
            end = next;
            break;
        }
        Vector<ValueFlowNodeID, 8> predecessors;
        m_graph.forEachPredecessor(step.node, [&](ValueFlowNodeID predecessor) {
            predecessors.append(predecessor);
        });
        AOT::sort(predecessors);
        auto goOnThroughHoldersOf = [&](ValueFlowObjectID lookedFor) {
            bool hasHolder = false;
            for (ValueFlowNodeID predecessor : predecessors) {
                // What is kept out of a node that is made for the unknown object has come by it all the same.
                bool hasComeBy = m_sets.contains(m_objectsLookedForBesideUnknownObject, lookedFor) && isMadeForUnknownObject(predecessor);
                if (!hasComeBy && !m_sets.contains(valueOf(predecessor), lookedFor))
                    continue;
                hasHolder = true;
                if (seen.add(predecessor.index(), lookedFor.index()))
                    found.append(Step { predecessor, lookedFor, next });
            }
            return hasHolder;
        };
        if (goOnThroughHoldersOf(step.lookedFor) || m_graph.maskOf(step.node) == ValueSets::anything)
            continue;
        for (ValueFlowObjectID placeholder : { m_input.placeholders.untracked, m_input.placeholders.unknown }) {
            if (placeholder && placeholder != step.lookedFor)
                goOnThroughHoldersOf(placeholder);
        }
    }

    Vector<ValueFlowNodeID> way;
    for (uint32_t step = end; step != noSolverIndex; step = found[step].cameFrom)
        way.append(found[step].node);
    way.reverse();
    return way;
}

std::optional<ValueFlowUnknownSource> ValueFlowSolver::unknownSourceOf(ValueFlowEdge edge) const
{
    const ValueFlowNodeKey* key = keyOfMadeNode(edge.from);
    if (!key || key->kind != ValueFlowMadeNodeKind::Constant)
        return std::nullopt;
    return key->unknownSource();
}

std::optional<ValueFlowUnknownSource> ValueFlowSolver::unknownSourceOf(ValueFlowNodeID node) const
{
    if (node.index() >= m_unknownSourceOfNode.size() || !m_unknownSourceOfNode[node.index()])
        return std::nullopt;
    return static_cast<ValueFlowUnknownSource>(m_unknownSourceOfNode[node.index()] - 1);
}

bool ValueFlowSolver::merge()
{
    uint32_t numberOfNodesBefore = m_numberOfNodesBeforeRound;
    bool hasReached = !m_bodiesReachedInRound.isEmpty();

    m_sets.freeOutgrownTables();
    // Both read the logs. Neither asks for what the other one makes, and much of each runs on one thread.
    auto takeEdgesOfLogsAndBodies = [&] {
    Vector<size_t> firstEdgeOfLog;
    size_t numberOfEdgesOfRound = m_edgesOfRound.size();
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        m_counters.edgesLogged += thread->log.edges.size();
        firstEdgeOfLog.append(numberOfEdgesOfRound);
        numberOfEdgesOfRound += thread->log.edges.size();
    }
    m_edgesOfRound.grow(numberOfEdgesOfRound);
    forEachIndexInParallel(m_loops, m_threads.size(), [&](unsigned number) {
        const Thread& thread = *m_threads[number];
        size_t position = firstEdgeOfLog[number];
        for (ValueFlowEdge edge : thread.log.edges)
            m_edgesOfRound[position++] = ValueFlowEdge { thread.numberInGraph(edge.from), thread.numberInGraph(edge.to) };
    });

    m_stepTimes.didFinish("merge: edges of the logs copied"_s);
    AOT::sort(m_bodiesReachedInRound);
    Vector<ValueFlowNodeSet> seeds;
    Vector<KeyToMake> constantsToMake;
    // By the set: the node that holds it; none until it is made.
    HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> holding;
    for (uint32_t body : m_bodiesReachedInRound) {
        const ValueFlowBodyRange& range = m_input.ranges[body];
        const ValueFlowBodyRange& end = m_input.ranges[body + 1];
        m_edgesOfRound.append(m_input.edgesOnceReached.subspan(range.edgeOnceReached, end.edgeOnceReached - range.edgeOnceReached));
        for (const ValueFlowNodeSet& seed : m_input.seedsOnceReached.subspan(range.seedOnceReached, end.seedOnceReached - range.seedOnceReached)) {
            ValueSetID set = seed.set;
            if (!m_input.unknownSourcesLeftOut.isEmpty() && m_input.placeholders.unknown)
                set = m_makers[0]->intersect(set, ValueSets::complement(m_makers[0]->of(m_input.placeholders.unknown)));
            if (set == ValueSets::nothing)
                continue;
            seeds.append(ValueFlowNodeSet { seed.node, set });
            auto added = holding.add(set.index(), ValueFlowNodeID());
            if (!added.isNewEntry)
                continue;
            if (std::optional<ValueFlowNodeID> existing = findMadeNode(ValueFlowNodeKey::constant(set)))
                added.iterator->value = *existing;
            else
                constantsToMake.append(KeyToMake { ValueFlowNodeKey::constant(set), m_sets.hashOfMembers(set) });
        }
    }
    m_bodiesReachedInRound.shrink(0);
    AOT::sort(constantsToMake, [](const KeyToMake& first, const KeyToMake& second) {
        return std::tuple { first.hashOfMembers, first.key.set.index() } < std::tuple { second.hashOfMembers, second.key.set.index() };
    });
    constantsToMake.shrink(std::unique(constantsToMake.begin(), constantsToMake.end(), [](const KeyToMake& first, const KeyToMake& second) {
        return first.key == second.key;
    }) - constantsToMake.begin());
    makeNodesOf(constantsToMake.mutableSpan());
    for (const KeyToMake& made : constantsToMake)
        holding.set(made.key.set.index(), m_madeNodes[made.madeNodes].first);
    for (const ValueFlowNodeSet& seed : seeds)
        m_edgesOfRound.append(ValueFlowEdge { holding.get(seed.set.index()), seed.node });
    // What the rule of a statement that waits would have said: unknownValue(ThrownByRunTime) into `thrown`.
    ValueFlowNodeID holdingWhatRunTimeThrows;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        if (thread->thrownOfStatementsThatWait.isEmpty())
            continue;
        if (!holdingWhatRunTimeThrows) {
            ValueSetID thrown = m_input.universe->anyPrimitive();
            if (!m_input.unknownSourcesLeftOut.contains(ValueFlowUnknownSource::ThrownByRunTime))
                thrown = m_makers[0]->unite(thrown, m_input.universe->unknownObjectAlone());
            ValueFlowNodeKey key = ValueFlowNodeKey::constant(thrown, ValueFlowUnknownSource::ThrownByRunTime);
            std::optional<ValueFlowNodeID> existing = findMadeNode(key);
            holdingWhatRunTimeThrows = existing ? *existing : makeNodes(key);
        }
        for (ValueFlowNodeID thrown : thread->thrownOfStatementsThatWait)
            m_edgesOfRound.append(ValueFlowEdge { holdingWhatRunTimeThrows, thrown });
        thread->thrownOfStatementsThatWait.shrink(0);
    }
    m_stepTimes.didFinish("merge: bodies reached"_s);
    };
    runTasksInParallel(m_loops, {
        takeEdgesOfLogsAndBodies,
        [&] {
            countRunsAndEdges();
            if (m_input.remembersWhoLoggedEachEdge)
                rememberWhoLoggedTheEdges();
        },
    });
    m_stepTimes.didFinish("merge: runs and edges counted, beside"_s);

    // Linking fills the tables of names and the edges of the graph.
    growAndFill(m_firstReaderOfNode, m_graph.numberOfNodes(), noSolverIndex);
    growAndFill(m_seenByReadersOfNode, m_graph.numberOfNodes(), ValueSets::nothing);
    growAndFill(m_isWaitedForToHoldSomething, m_graph.numberOfNodes(), static_cast<uint8_t>(0));
    growByDoubling(m_isWatched, m_graph.numberOfNodes());
    if (m_input.escaped && !m_isWatched.quickSet(m_input.escaped.index()))
        m_seenByWatchersOfEscaped = valueOf(m_input.escaped);
    if (m_input.lost && !m_isWatched.quickSet(m_input.lost.index()))
        m_seenByWatchersOfLost = valueOf(m_input.lost);
    for (ValueFlowNodeID node : { m_objectsThatAreNotPlain, m_objectsWrittenUnderUnknownKey }) {
        if (node)
            m_isWatched.quickSet(node.index());
    }
    for (unsigned write = 0; m_firstOfObjectsWithRareWrite && write < numberOfValueFlowRareWrites; ++write)
        m_isWatched.quickSet(m_firstOfObjectsWithRareWrite.index() + write);
    prepareNodesToLink();
    m_stepTimes.didFinish("merge: tables grown, names numbered"_s);
    AOT_THRESHOLD(size_t, fewestNodesToLinkInChunks, 1024);
    size_t numberOfEdgesAdded = 0;
    auto note = [&](unsigned task, unsigned worker) {
        if (task < numberOfWatchShards)
            noteWatchers(task);
        else
            noteReaders(task - numberOfWatchShards, *m_makers[worker]);
    };
    if (m_writtenToLink.size() + m_readToLink.size() < fewestNodesToLinkInChunks) {
        forEachIndexInParallel(m_loops, 1 + numberOfLinkShardsOfNames + numberOfWatchShards + numberOfReaderShards, [&](unsigned task, unsigned worker) {
            if (task > numberOfLinkShardsOfNames) {
                note(task - numberOfLinkShardsOfNames - 1, worker);
                return;
            }
            if (task) {
                linkNamedNodes(task - 1, false);
                linkNamedNodes(task - 1, true);
                return;
            }
            for (unsigned shard = numberOfLinkShardsOfNames; shard < numberOfLinkShards; ++shard) {
                linkNamedNodes(shard, false);
                linkNamedNodes(shard, true);
            }
        });
    } else {
        ASSERT(m_edgesInFrontOfLinks.isEmpty());
        std::swap(m_edgesOfRound, m_edgesInFrontOfLinks);
        runTasksInParallel(m_loops, {
            [&] {
                linkInChunks(false, numberOfWatchShards + numberOfReaderShards, note);
                linkInChunks(true, 0, note);
            },
            [&] {
                numberOfEdgesAdded = m_graph.addEdges(m_edgesInFrontOfLinks.span());
            },
        });
        m_edgesInFrontOfLinks.shrink(0);
    }
    chainSetsWatchedForCover();
    m_stepTimes.didFinish("merge: link, watchers, readers"_s);
    takeEdgesOfLinkShards();
    numberOfEdgesAdded += m_graph.addEdges(m_edgesOfRound.span());
    m_edgesOfRound.shrink(0);
    m_stepTimes.didFinish("merge: edges added"_s);

    bool hasUses = false;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (const ValueFlowRoundLog::Use& use : thread->log.uses) {
            if (use.builtin.index() >= m_usesOfBuiltin.size())
                m_usesOfBuiltin.grow(use.builtin.index() + 1);
            OptionSet<BuiltinUse>& uses = m_usesOfBuiltin[use.builtin.index()];
            hasUses |= !uses.containsAll(use.uses);
            uses.add(use.uses);
        }
        for (ValueFlowStatementID statement : thread->log.statementsWithMembersNotToldApart)
            m_statementsWithMembersNotToldApart.set(statement.index());
    }

    for (const std::unique_ptr<Thread>& thread : m_threads) {
        thread->log.clear();
        thread->numberOfMadeNode.shrink(0);
        thread->numberOfReachedTaken = 0;
        thread->numberOfMadeTaken = 0;
        thread->numberOfRunsTaken = 0;
        thread->numberOfFunctionsAskedTaken = 0;
        thread->numberOfValuesShownTaken = 0;
        thread->numberOfNodesAskedForTaken = 0;
    }
    m_stepTimes.didFinish("merge: uses of built-ins, logs cleared"_s);
    return hasReached || hasUses || numberOfEdgesAdded || m_numberOfNodesMade != numberOfNodesBefore;
}

ValueSetID ValueFlowSolver::withoutWhatOtherMembersCover(ValueFlowNodeID node, ValueSetMaker& maker) const
{
    ValueSetID value = valueOf(node);
    if (node == m_input.escaped || node == m_input.lost)
        return value;
    return withoutWhatOtherMembersCover(value, maker);
}

ValueSetID ValueFlowSolver::withoutWhatOtherMembersCover(ValueSetID value, ValueSetMaker& maker) const
{
    for (const Covering& covering : m_coverings) {
        if (m_sets.contains(value, covering.placeholder))
            value = maker.intersect(value, covering.allButCovered);
    }
    for (const KindThatCoversKeys& covering : m_kindsThatCoverKeys) {
        if (m_sets.contains(value, covering.kind))
            value = maker.intersect(value, covering.allButKeys);
    }
    return value;
}

ValueSetID ValueFlowSolver::valueForReadersOf(ValueFlowNodeID node, ValueSetMaker& maker) const
{
    // A node of this turn holds nothing yet, whether it has its number from a log or from the solver.
    if (node.index() >= m_graph.numberOfNodes())
        return ValueSets::nothing;
    if (node.index() < m_firstReaderOfNode.size() && m_firstReaderOfNode[node.index()] != noSolverIndex)
        return m_seenByReadersOfNode[node.index()];
    return withoutWhatOtherMembersCover(node, maker);
}

ValueSetID ValueFlowSolver::valueShownTo(ValueFlowStatementID statement, ValueFlowNodeID node) const
{
    auto valuesShown = m_valuesShownToStatement.find(statement.index());
    if (valuesShown == m_valuesShownToStatement.end())
        return ValueSets::nothing;
    for (const ValueFlowNodeSet& shown : valuesShown->value) {
        if (shown.node == node)
            return shown.set;
    }
    return ValueSets::nothing;
}

// A mask that admits an object of which somebody says that it is something else than an object with a layout of the program or a closure is the mask of a type that other code makes values of too.
ValueSetID ValueFlowSolver::findFamilyObjectsOf(ValueSetID mask, ValueSetMaker& maker) const
{
    ValueSetID listed = maker.intersect(mask, m_objectsSaidToBeSomething);
    return m_sets.isSubset(listed, m_objectsOfProgram) ? listed : ValueSetID();
}

ValueSetID ValueFlowSolver::familyObjectsOf(ValueSetID mask, ValueSetMaker& maker) const
{
    auto found = m_familyObjectsOfMask.find(mask.index());
    return found != m_familyObjectsOfMask.end() ? found->value : findFamilyObjectsOf(mask, maker);
}

void ValueFlowSolver::noteWhatPlaceholdersCover()
{
    // The untracked object among what was handed to code that nobody compiles may be any object that it stands for.
    if (m_input.escaped && m_input.lost && m_input.placeholders.untracked && !m_haveLostObjectsEscaped && m_sets.contains(valueOf(m_input.escaped), m_input.placeholders.untracked)) {
        m_haveLostObjectsEscaped = true;
        m_edgesOfRound.append(ValueFlowEdge { m_input.lost, m_input.escaped });
    }
    for (Covering& covering : m_coverings)
        covering.allButCovered = ValueSets::complement(m_makers[0]->intersect(valueOf(covering.covered), covering.objectsThatMayBeCovered));
}

// Behind a type that names objects of the program alone an object nobody knows is one of those objects, and of those one that the placeholder covers: one that has escaped, or that the analysis has stopped following.
void ValueFlowSolver::connectWhatStoppedPlaceholdersCover()
{
    m_graph.takeNodesAtWhichPlaceholdersStopped(m_placeholdersStopped);
    for (auto [node, placeholder] : m_placeholdersStopped) {
        ValueSetID taken = m_makers[0]->intersect(m_graph.familyObjectsOf(node), m_graph.maskOf(node));
        if (taken == ValueSets::nothing)
            continue;
        ValueFlowNodeKey key = ValueFlowNodeKey::masked(placeholder == m_input.placeholders.unknown ? m_input.escaped : m_input.lost, taken);
        std::optional<ValueFlowNodeID> existing = findMadeNode(key);
        m_edgesOfRound.append(ValueFlowEdge { existing ? *existing : makeNodes(key), node });
    }
}

// In which order the statements become due makes no difference: a turn runs all that are due, each on whichever thread takes it.
void ValueFlowSolver::makeReadersDue(std::span<const ValueFlowNodeID> moved)
{
    constexpr size_t nodesPerShare = 256;
    constexpr size_t membersPerShare = 512;
    growByDoubling(m_isDueLast, numberOfStatements());
    Vector<Vector<ValueFlowStatementID>>& readersOfWorker = m_readersOfWorker;
    Vector<Vector<NewMemberOfWatchedNode>>& newMembersOfWorker = m_newMembersOfWorker;
    Vector<Vector<WatchesToTest>>& watchesToTestOfWorker = m_watchesToTestOfWorker;
    readersOfWorker.grow(m_makers.size());
    newMembersOfWorker.grow(m_makers.size());
    watchesToTestOfWorker.grow(m_makers.size());
    forEachIndexInParallel(m_loops, (moved.size() + nodesPerShare - 1) / nodesPerShare, [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        Vector<ValueFlowStatementID>& found = readersOfWorker[worker];
        for (size_t index = share * nodesPerShare; index < std::min(moved.size(), (share + 1) * nodesPerShare); ++index) {
            ValueFlowNodeID node = moved[index];
            if (node.index() < m_firstStatementWaitingForNode.size()) {
                for (uint32_t waiting = std::exchange(m_firstStatementWaitingForNode[node.index()], 0u); waiting;) {
                    listIfItMayBecomeDue(ValueFlowStatementID(waiting - 1), found);
                    waiting = std::exchange(m_nextStatementWaiting[waiting - 1], hasWaitedAndNotRun);
                }
            }
            if (m_isWatched.get(node.index()))
                listWatchersOf(node, maker, found, newMembersOfWorker[worker], watchesToTestOfWorker[worker]);
            if (node.index() < m_isWaitedForToHoldSomething.size() && std::exchange(m_isWaitedForToHoldSomething[node.index()], 0)) {
                const ReaderShard& shard = m_readerShards[readerShardOf(node)];
                for (uint32_t reader = shard.firstReaderWaitingForSomething.get(node.index()); reader != noSolverIndex; reader = shard.readers[reader].next)
                    listIfItMayBecomeDue(shard.readers[reader].statement, found);
            }
            if (node.index() >= m_firstReaderOfNode.size() || m_firstReaderOfNode[node.index()] == noSolverIndex)
                continue;
            // What the readers of the node are shown from now on.
            ValueSetID value = withoutWhatOtherMembersCover(node, maker);
            if (m_sets.isSubset(value, m_seenByReadersOfNode[node.index()]))
                continue;
            m_seenByReadersOfNode[node.index()] = value;
            const Vector<Reader>& readers = m_readerShards[readerShardOf(node)].readers;
            for (uint32_t reader = m_firstReaderOfNode[node.index()]; reader != noSolverIndex; reader = readers[reader].next)
                listIfItMayBecomeDue(readers[reader].statement, found);
        }
    });

    Vector<NewMemberOfWatchedNode>& newMembers = m_newMembers;
    for (Vector<NewMemberOfWatchedNode>& ofWorker : newMembersOfWorker) {
        makeRoomByDoubling(newMembers, ofWorker.size());
        newMembers.appendVector(ofWorker);
        ofWorker.shrink(0);
    }
    forEachIndexInParallel(m_loops, (newMembers.size() + membersPerShare - 1) / membersPerShare, [&](unsigned share, unsigned worker) {
        for (size_t index = share * membersPerShare; index < std::min(newMembers.size(), (share + 1) * membersPerShare); ++index) {
            if (newMembers[index].shard == numberOfWatchShards) {
                uint32_t member = newMembers[index].member.index();
                const Vector<ChainedWatch>& chained = m_setsWatchedForCoverOfMembers[rangeOfMember(member)];
                for (uint32_t entry = m_firstSetWatchedForCoverOfMember[member]; entry != noSolverIndex; entry = chained[entry].next) {
                    const WatchShard& shard = m_watchShards[numberOfWatchShardsByNode + chained[entry].watch % numberOfWatchShardsBySet];
                    for (uint32_t watch : shard.watchesOfSetWatchedForCover[chained[entry].watch / numberOfWatchShardsBySet]) {
                        if (shard.watchedNodes[shard.watches[watch].watchedNode].node.index() == newMembers[index].watchedNode)
                            listReadersOfWatch(shard, watch, readersOfWorker[worker]);
                    }
                }
                continue;
            }
            const WatchShard& shard = m_watchShards[newMembers[index].shard];
            auto first = shard.firstWatchOfMember.find(static_cast<uint64_t>(newMembers[index].watchedNode) << 32 | newMembers[index].member.index());
            if (first == shard.firstWatchOfMember.end())
                continue;
            for (uint32_t entry = first->value; entry != noSolverIndex; entry = shard.watchesOfMembers[entry].next)
                listReadersOfWatch(shard, shard.watchesOfMembers[entry].watch, readersOfWorker[worker]);
        }
    });

    // One node may have thousands of watches of many objects.
    constexpr uint32_t watchesPerShare = 128;
    struct ShareOfWatches {
        WatchesToTest watches;
        uint32_t first { 0 };
    };
    Vector<ShareOfWatches> sharesOfWatches;
    for (Vector<WatchesToTest>& ofWorker : watchesToTestOfWorker) {
        for (const WatchesToTest& watches : ofWorker) {
            uint32_t count = m_watchShards[watches.shard].watchedNodes[watches.watchedNode].watchesOfManyObjects.size();
            for (uint32_t first = 0; first < count; first += watchesPerShare)
                sharesOfWatches.append({ watches, first });
        }
        ofWorker.shrink(0);
    }
    newMembers.shrink(0);
    forEachIndexInParallel(m_loops, sharesOfWatches.size(), [&](unsigned share, unsigned worker) {
        const WatchesToTest& watches = sharesOfWatches[share].watches;
        const WatchShard& shard = m_watchShards[watches.shard];
        std::span<const uint32_t> all = shard.watchedNodes[watches.watchedNode].watchesOfManyObjects.span();
        for (uint32_t watch : all.subspan(sharesOfWatches[share].first, std::min<size_t>(watchesPerShare, all.size() - sharesOfWatches[share].first))) {
            if (m_sets.meet(watches.whatIsNew, shard.watches[watch].within))
                listReadersOfWatch(shard, watch, readersOfWorker[worker]);
        }
    });

    // Whether a statement is at rest was looked at where it was listed, and there it was marked.
    size_t numberFound = 0;
    for (const Vector<ValueFlowStatementID>& found : readersOfWorker)
        numberFound += found.size();
    m_due.reserveCapacity(m_due.size() + numberFound);
    for (Vector<ValueFlowStatementID>& found : readersOfWorker) {
        m_due.appendVector(found);
        found.shrink(0);
    }
}

// What counts is what the results count: five hundred sites of one layout are a good answer.
void ValueFlowSolver::prepareSaturation()
{
    const ValueFlowUniverse& universe = *m_input.universe;
    m_saturates = true;
    uint32_t numberOfLayouts = m_input.program->layouts.size();
    uint32_t numberOfClasses = m_input.program->builtins().classCount();
    m_layoutOrCalleeOfMember.makeRoomToFill(universe.numberOfObjects());
    constexpr uint32_t membersInStretch = 1 << 14;
    forEachIndexInParallel(m_loops, (universe.numberOfObjects() + membersInStretch - 1) / membersInStretch, [&](unsigned stretch) {
        uint32_t end = std::min(universe.numberOfObjects(), (stretch + 1) * membersInStretch);
        for (uint32_t index = stretch * membersInStretch; index < end; ++index) {
            ValueFlowObjectID member(index);
            if (!universe.isObject(member) || member == m_input.placeholders.unknown || member == m_input.placeholders.untracked)
                m_layoutOrCalleeOfMember[index] = 0;
            else if (LayoutID layout = universe.layoutOf(member))
                m_layoutOrCalleeOfMember[index] = 1 + layout.index();
            else if (BuiltinClassID builtinClass = universe.closureFunction(member) ? BuiltinClassID() : universe.classOf(member))
                m_layoutOrCalleeOfMember[index] = 1 + numberOfLayouts + builtinClass.index();
            else
                m_layoutOrCalleeOfMember[index] = 1 + numberOfLayouts + numberOfClasses + index;
        }
    });
    growAndFill(m_lastCountThatSaw, 1 + numberOfLayouts + numberOfClasses + universe.numberOfObjects(), 0u);
}

ValueSetID ValueFlowSolver::findClosuresFollowedTo(ValueFlowNodeID callee)
{
    AOT_THRESHOLD(unsigned, mostNodesVisited, 128);
    AOT_THRESHOLD(unsigned, mostPredecessorsFollowed, 8);
    ValueSetMaker& maker = *m_makers[0];
    Vector<ValueFlowNodeID, 32> visited { callee };
    Vector<ValueFlowNodeID, 8> predecessors;
    ValueSetID closures = ValueSets::nothing;
    for (unsigned next = 0; next < visited.size(); ++next) {
        predecessors.shrink(0);
        unsigned numberOfPredecessors = 0;
        m_graph.forEachPredecessor(visited[next], [&](ValueFlowNodeID predecessor) {
            if (++numberOfPredecessors <= mostPredecessorsFollowed)
                predecessors.append(predecessor);
        });
        if (!numberOfPredecessors)
            closures = maker.unite(closures, maker.intersect(valueOf(visited[next]), m_input.placeholders.closures));
        if (numberOfPredecessors > mostPredecessorsFollowed)
            continue;
        for (ValueFlowNodeID predecessor : predecessors) {
            if (predecessor == m_input.escaped || predecessor == m_input.lost || isMadeForUnknownObject(predecessor) || visited.contains(predecessor))
                continue;
            if (visited.size() >= mostNodesVisited)
                return ValueSets::nothing;
            visited.append(predecessor);
        }
    }
    return m_sets.size(closures) > m_input.mostMembersToldApart ? ValueSets::nothing : closures;
}

void ValueFlowSolver::findClosuresFollowedToConstructions()
{
    if (!m_input.program || !m_input.placeholders.unknown || !m_input.placeholders.closures)
        return;
    for (const ValueFlowStatement& statement : m_input.statements) {
        if (statement.kind != ValueFlowStatementKind::Operation || !OperandPosition::newTarget(statement.opcode) || !isReached(statement.function))
            continue;
        ValueFlowNodeID callee = operandsOf(statement)[0].node;
        if (callee && m_sets.contains(valueOf(callee), m_input.placeholders.unknown)) {
            m_closuresFollowedToConstruction.ensure(callee.index(), [&] {
                return findClosuresFollowedTo(callee);
            });
        }
    }
}

bool ValueFlowSolver::isMadeForUnknownObject(ValueFlowNodeID node) const
{
    const ValueFlowNodeKey* key = keyOfMadeNode(node);
    if (!key)
        return false;
    switch (key->kind) {
    case ValueFlowMadeNodeKind::Read:
    case ValueFlowMadeNodeKind::Written:
    case ValueFlowMadeNodeKind::Lookup:
    case ValueFlowMadeNodeKind::CallGroup:
    case ValueFlowMadeNodeKind::WrittenUnderKind:
    case ValueFlowMadeNodeKind::ReadUnderKind:
        // What is read, looked up or called through a set that holds known objects too may come out of one of those.
        return key->set && m_sets.onlyMember(key->set) == m_input.placeholders.unknown;
    case ValueFlowMadeNodeKind::Constant:
    case ValueFlowMadeNodeKind::Hub:
    case ValueFlowMadeNodeKind::Masked:
    case ValueFlowMadeNodeKind::Marked:
    case ValueFlowMadeNodeKind::Instance:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool ValueFlowSolver::maySaturate(ValueFlowNodeID node) const
{
    if (node == m_input.escaped || node == m_input.lost || m_graph.maskOf(node) != ValueSets::anything || m_graph.isRetired(node))
        return false;
    if (const ValueFlowNodeKey* key = keyOfMadeNode(node)) {
        switch (key->kind) {
        case ValueFlowMadeNodeKind::Read:
        case ValueFlowMadeNodeKind::Written:
        case ValueFlowMadeNodeKind::ReadUnderKind:
        case ValueFlowMadeNodeKind::WrittenUnderKind:
        case ValueFlowMadeNodeKind::CallGroup:
            return true;
        case ValueFlowMadeNodeKind::Lookup:
            return key->name().kind == ValueFlowNameKind::Property || key->name().kind == ValueFlowNameKind::Elements || key->name().kind == ValueFlowNameKind::UnknownKey;
        case ValueFlowMadeNodeKind::Constant:
        case ValueFlowMadeNodeKind::Hub:
        case ValueFlowMadeNodeKind::Masked:
        case ValueFlowMadeNodeKind::Marked:
        case ValueFlowMadeNodeKind::Instance:
            return false;
        }
    }
    return node.index() < m_input.nodes->ofHost(static_cast<ValueFlowHostNode>(0)).index();
}

bool ValueFlowSolver::hasTooManyLayoutsAndCallees(ValueSetID set)
{
    if (ValueSets::isComplement(set) || m_sets.size(set) <= m_input.mostSetMembers)
        return false;
    return m_isTooLarge.ensure(set.index(), [&] {
        uint32_t count = 0;
        ++m_numberOfCounts;
        return m_sets.anyMember(set, [&](ValueFlowObjectID member) {
            uint32_t layoutOrCallee = m_layoutOrCalleeOfMember[member.index()];
            if (layoutOrCallee && std::exchange(m_lastCountThatSaw[layoutOrCallee], m_numberOfCounts) != m_numberOfCounts)
                ++count;
            return count > m_input.mostSetMembers;
        });
    }).iterator->value;
}

// The members of a set that is given up go to `lost`, whatever arrives later too, and the node holds the untracked object.
void ValueFlowSolver::saturate(std::span<const ValueFlowNodeID> moved)
{
    ValueFlowNodeID holdingUntrackedObject;
    ValueSetMaker& maker = *m_makers[0];
    constexpr size_t nodesPerShare = 4096;
    Vector<Vector<ValueFlowNodeID>> nodesToLookAtOfShare((moved.size() + nodesPerShare - 1) / nodesPerShare);
    forEachIndexInParallel(m_loops, nodesToLookAtOfShare.size(), [&](unsigned share) {
        for (ValueFlowNodeID node : moved.subspan(share * nodesPerShare, std::min(nodesPerShare, moved.size() - share * nodesPerShare))) {
            ValueSetID listed = valueOf(node);
            bool isSmall = !m_counters.isPastEdgeBudget && !ValueSets::isComplement(listed) && m_sets.size(listed) <= m_input.mostSetMembers;
            if (!isSmall && !m_isSaturated.get(node.index()) && maySaturate(node))
                nodesToLookAtOfShare[share].append(node);
        }
    });
    Vector<ValueFlowNodeID> nodesToLookAt;
    for (const Vector<ValueFlowNodeID>& ofShare : nodesToLookAtOfShare)
        nodesToLookAt.appendVector(ofShare);
    for (ValueFlowNodeID node : nodesToLookAt) {
        // What a placeholder beside it covers is no member that anybody sees.
        ValueSetID value = withoutWhatOtherMembersCover(node, maker);
        bool isGivenUp = m_counters.isPastEdgeBudget ? m_sets.meet(value, m_objectsThatPlaceholdersMayCover) : hasTooManyLayoutsAndCallees(value);
        if (!isGivenUp)
            continue;
        if (!holdingUntrackedObject)
            holdingUntrackedObject = ensureConstant(m_untrackedObjectAlone);
        m_isSaturated.set(node.index());
        ++m_counters.nodesSaturated;
        m_edgesOfRound.append(ValueFlowEdge { node, m_input.lost });
        m_edgesOfRound.append(ValueFlowEdge { holdingUntrackedObject, node });
    }
}

void ValueFlowSolver::spreadMarks()
{
    m_marksOfObject.fill(OptionSet<ObjectMutation>(), m_sets.sizeOfUniverse());
    for (ObjectMutation mutation : OptionSet<ObjectMutation>::all()) {
        std::optional<ValueFlowNodeID> node = findMadeNode(ValueFlowNodeKey::marked(mutation));
        if (!node)
            continue;
        m_sets.forEachMember(valueOf(*node), [&](ValueFlowObjectID object) {
            m_marksOfObject[object.index()].add(mutation);
        });
    }

    // What was done through the untracked object may have been done to any object that it stands for.
    if (!m_input.lost || !m_input.placeholders.untracked)
        return;
    OptionSet<ObjectMutation> marksOfUntrackedObject = m_marksOfObject[m_input.placeholders.untracked.index()];
    if (marksOfUntrackedObject.isEmpty())
        return;
    m_sets.forEachMember(m_makers[0]->intersect(valueOf(m_input.lost), m_objectsThatPlaceholdersMayCover), [&](ValueFlowObjectID object) {
        m_marksOfObject[object.index()].add(marksOfUntrackedObject);
    });
}

// A mask that is made from a type nobody tests takes nothing away but the untracked object, in whose place it puts the family.
void ValueFlowSolver::findObjectsOutsideFamilies()
{
    Vector<ValueFlowNodeSet> masks;
    for (const ValueFlowNodeMask& mask : m_input.masks) {
        if (mask.kind == ValueFlowMaskKind::Declared)
            masks.append(ValueFlowNodeSet { mask.node, mask.mask });
    }
    for (const MadeNodes& madeNodes : m_madeNodes) {
        if (madeNodes.key.kind == ValueFlowMadeNodeKind::Masked && madeNodes.key.maskKind() == ValueFlowMaskKind::Declared)
            masks.append(ValueFlowNodeSet { madeNodes.first, madeNodes.key.set });
    }
    if (masks.isEmpty())
        return;

    constexpr size_t masksPerShare = 256;
    bool looksAtArrivals = m_input.build && m_input.build->validates();
    ValueSetID whatIsNotFollowed = ValueSets::complement(m_objectsThatPlaceholdersMayCover);
    ValueSetID whatMaskMayTakeAway = m_input.placeholders.unknown ? m_makers[0]->intersect(whatIsNotFollowed, ValueSets::complement(m_makers[0]->of(m_input.placeholders.unknown))) : whatIsNotFollowed;
    Vector<Vector<ObjectOutsideFamily>> foundByWorker(m_makers.size());
    forEachIndexInParallel(m_loops, (masks.size() + masksPerShare - 1) / masksPerShare, [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        // None if all of the first set lies in the second.
        auto firstMemberOutside = [&](ValueSetID set, ValueSetID within) {
            ValueFlowObjectID first;
            if (m_sets.isSubset(set, within))
                return first;
            m_sets.forEachMember(maker.intersect(set, ValueSets::complement(within)), [&](ValueFlowObjectID member) {
                if (!first)
                    first = member;
            });
            return first;
        };
        for (const ValueFlowNodeSet& mask : masks.subspan(share * masksPerShare, std::min(masksPerShare, masks.size() - share * masksPerShare))) {
            ValueSetID value = valueOf(mask.node);
            ValueFlowObjectID object = firstMemberOutside(value, maker.unite(mask.set, whatIsNotFollowed));
            if (!object && looksAtArrivals && !m_graph.isAtRest(mask.node)) {
                ValueSetID accountedFor = maker.unite(value, whatMaskMayTakeAway);
                m_graph.forEachPredecessor(mask.node, [&](ValueFlowNodeID predecessor) {
                    ValueFlowObjectID takenAway = firstMemberOutside(valueOf(predecessor), accountedFor);
                    if (takenAway && (!object || takenAway < object))
                        object = takenAway;
                });
            }
            if (object)
                foundByWorker[worker].append(ObjectOutsideFamily { mask.node, object });
        }
    });

    for (Vector<ObjectOutsideFamily>& found : foundByWorker) {
        // Values flow in the body of a function that is never reached too. What arrives there arrives nowhere.
        found.removeAllMatching([&](const ObjectOutsideFamily& entry) {
            FunctionID function = subjectOf(entry.node).function;
            return function && !isReached(function);
        });
        m_objectsOutsideFamilies.appendVector(found);
    }
    AOT::sort(m_objectsOutsideFamilies, { }, &ObjectOutsideFamily::node);
    for (const ObjectOutsideFamily& entry : m_objectsOutsideFamilies)
        m_holdsObjectOutsideFamily.set(entry.node.index());
    m_counters.nodesHoldingObjectsOutsideFamily = m_objectsOutsideFamilies.size();
}

// Breadth first, along the edges, from the constants through which the rules have brought the unknown object into being, through the nodes that hold it.
void ValueFlowSolver::findUnknownSources(NOESCAPE const ScopedLambda<void()>& whatRunsBesideTheWalk)
{
    ValueFlowObjectID unknownObject = m_input.placeholders.unknown;
    if (!unknownObject || !m_input.isAskedHowValuesCame) {
        whatRunsBesideTheWalk();
        return;
    }
    uint32_t numberOfNodes = m_graph.numberOfNodes();
    m_unknownSourceOfNode.fill(uint8_t { 0 }, numberOfNodes);

    constexpr size_t madeNodesPerShare = 1 << 12;
    Vector<uint32_t> found = collectInParallel<uint32_t>(m_loops, static_cast<unsigned>((m_madeNodes.size() + madeNodesPerShare - 1) / madeNodesPerShare), [&](unsigned share, Vector<uint32_t>& foundInShare) {
        size_t first = share * madeNodesPerShare;
        for (const MadeNodes& madeNodes : m_madeNodes.span().subspan(first, std::min(madeNodesPerShare, m_madeNodes.size() - first))) {
            if (madeNodes.key.kind != ValueFlowMadeNodeKind::Constant || !madeNodes.key.unknownSource() || !m_sets.contains(madeNodes.key.set, unknownObject))
                continue;
            m_unknownSourceOfNode[madeNodes.first.index()] = 1 + static_cast<uint8_t>(*madeNodes.key.unknownSource());
            foundInShare.append(madeNodes.first.index());
        }
    });
    if (found.isEmpty()) {
        whatRunsBesideTheWalk();
        return;
    }

    // A share is a multiple of 64 nodes, so that no two threads write to one word of the bits.
    constexpr uint32_t nodesPerShare = 1 << 14;
    unsigned numberOfShares = (numberOfNodes + nodesPerShare - 1) / nodesPerShare;
    auto forEachNodeOfShare = [&](unsigned share, const auto& functor) {
        for (uint32_t node = share * nodesPerShare; node < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++node)
            functor(node);
    };
    BitVector holdsUnknownObject(numberOfNodes);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        forEachNodeOfShare(share, [&](uint32_t node) {
            if (m_sets.contains(valueOf(ValueFlowNodeID(node)), unknownObject))
                holdsUnknownObject.quickSet(node);
        });
    });
    // The graph keeps an edge out of a constant at its target, and lists those of the constants that the walk begins at. So far nobody else has a source.
    size_t numberOfConstantsFound = found.size();
    Vector<ValueFlowEdge> edgesOutOfConstantsFound;
    m_graph.forEachListedEdgeOutOfConstants([&](ValueFlowEdge edge) {
        if (m_unknownSourceOfNode[edge.from.index()] && holdsUnknownObject.quickGet(edge.to.index()))
            edgesOutOfConstantsFound.append(edge);
    });
    parallelSort(m_loops, edgesOutOfConstantsFound.mutableSpan(), [](ValueFlowEdge first, ValueFlowEdge second) {
        return std::pair { first.from.index(), first.to.index() } < std::pair { second.from.index(), second.to.index() };
    });
    auto successorsOfConstantFound = [&](uint32_t constant) {
        auto range = std::ranges::equal_range(edgesOutOfConstantsFound.span(), constant, { }, [](ValueFlowEdge edge) {
            return edge.from.index();
        });
        return std::span<const ValueFlowEdge>(range.begin(), range.end());
    };

    Vector<uint32_t> firstSuccessor;
    firstSuccessor.fill(0u, numberOfNodes + 1);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        forEachNodeOfShare(share, [&](uint32_t node) {
            if (!holdsUnknownObject.quickGet(node))
                return;
            m_graph.forEachSuccessor(ValueFlowNodeID(node), [&](ValueFlowNodeID successor) {
                firstSuccessor[node + 1] += holdsUnknownObject.quickGet(successor.index());
            });
        });
    });
    Vector<uint32_t> successorsBeforeShare(numberOfShares);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        uint32_t numberInShare = 0;
        forEachNodeOfShare(share, [&](uint32_t node) {
            numberInShare += firstSuccessor[node + 1];
        });
        successorsBeforeShare[share] = numberInShare;
    });
    uint32_t numberOfSuccessors = 0;
    for (uint32_t& before : successorsBeforeShare)
        before = std::exchange(numberOfSuccessors, numberOfSuccessors + before);
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        uint32_t end = successorsBeforeShare[share];
        forEachNodeOfShare(share, [&](uint32_t node) {
            end += firstSuccessor[node + 1];
            firstSuccessor[node + 1] = end;
        });
    });
    Vector<uint32_t> successors;
    successors.grow(firstSuccessor.last());
    forEachIndexInParallel(m_loops, numberOfShares, [&](unsigned share) {
        forEachNodeOfShare(share, [&](uint32_t node) {
            if (firstSuccessor[node] == firstSuccessor[node + 1])
                return;
            uint32_t next = firstSuccessor[node];
            m_graph.forEachSuccessor(ValueFlowNodeID(node), [&](ValueFlowNodeID successor) {
                if (holdsUnknownObject.quickGet(successor.index()))
                    successors[next++] = successor.index();
            });
            AOT::sort(successors.mutableSpan().subspan(firstSuccessor[node], next - firstSuccessor[node]));
        });
    });

    // The walk goes wherever the edges lead, and waits for memory at every step. The queue says where it will be a few steps on: that is asked for ahead.
    constexpr size_t stepsAheadForRow = 16;
    constexpr size_t stepsAheadForSuccessors = 8;
    constexpr size_t stepsAheadForSources = 4;
    AOT_THRESHOLD(uint32_t, mostSourcesAskedFor, 8);
    // Of the sources that are nearest to a node it takes the lowest: the numbers of the nodes follow the order of the solve, and the answer must not.
    // That native code may answer too is said of every read out of an object nobody knows, so that source is named where no other is as near.
    auto rankOf = [](uint8_t source) {
        return source == 1 + static_cast<uint8_t>(ValueFlowUnknownSource::UndescribedNativeResult) ? std::numeric_limits<uint8_t>::max() : source;
    };
    BitVector isInNextLevel(numberOfNodes);
    auto reach = [&](uint32_t from, uint32_t to) {
        uint8_t& source = m_unknownSourceOfNode[to];
        if (!source) {
            source = m_unknownSourceOfNode[from];
            isInNextLevel.quickSet(to);
            found.append(to);
        } else if (isInNextLevel.quickGet(to) && rankOf(m_unknownSourceOfNode[from]) < rankOf(source))
            source = m_unknownSourceOfNode[from];
    };
    auto walk = [&] {
        for (size_t next = 0; next < numberOfConstantsFound; ++next) {
            for (ValueFlowEdge edge : successorsOfConstantFound(found[next]))
                reach(found[next], edge.to.index());
        }
        size_t endOfLevel = numberOfConstantsFound;
        for (size_t next = numberOfConstantsFound; next < found.size(); ++next) {
            if (next == endOfLevel) {
                endOfLevel = found.size();
                for (uint32_t node : found.subspan(next))
                    isInNextLevel.quickClear(node);
            }
            if (next + stepsAheadForRow < found.size())
                __builtin_prefetch(&firstSuccessor[found[next + stepsAheadForRow]]);
            if (next + stepsAheadForSuccessors < found.size())
                __builtin_prefetch(successors.span().data() + firstSuccessor[found[next + stepsAheadForSuccessors]]);
            if (next + stepsAheadForSources < found.size()) {
                uint32_t later = found[next + stepsAheadForSources];
                for (uint32_t successor : successors.subspan(firstSuccessor[later], std::min(mostSourcesAskedFor, firstSuccessor[later + 1] - firstSuccessor[later])))
                    __builtin_prefetch(&m_unknownSourceOfNode[successor]);
            }
            uint32_t node = found[next];
            for (uint32_t successor : successors.subspan(firstSuccessor[node], firstSuccessor[node + 1] - firstSuccessor[node]))
                reach(node, successor);
        }
    };
    runTasksInParallel(m_loops, {
        walk,
        [&] {
            whatRunsBesideTheWalk();
        },
    });
}

// The function and the value that a node of a body stands for, as far as the solver can tell: an entry parameter, or the result of an operation that has a statement.
Subject ValueFlowSolver::subjectOf(ValueFlowNodeID node) const
{
    Subject subject;
    if (!m_input.nodes || !m_input.program)
        return subject;
    if (const ValueFlowNodeKey* key = keyOfMadeNode(node)) {
        if (key->kind != ValueFlowMadeNodeKind::Masked || keyOfMadeNode(key->maskedNode()))
            return subject;
        node = key->maskedNode();
    }
    FunctionID function = m_input.nodes->functionOf(node);
    if (!function || function.index() >= m_input.program->functions.size())
        return subject;
    subject.function = function;

    const Function& body = m_input.program->at(function);
    uint32_t index = node.index() - m_input.nodes->firstNodeOf(function);
    if (body.hasBody() && index < body.numberOfEntryParameters()) {
        subject.value = body.at(entryBlock).parameters[index];
        return subject;
    }
    uint32_t firstStatement = m_input.ranges[function.index()].statement;
    for (const ValueFlowStatement& statement : m_input.statements.subspan(firstStatement, m_input.ranges[function.index() + 1].statement - firstStatement)) {
        if (statement.result == node && statement.function == function) {
            subject.value = statement.site;
            break;
        }
    }
    return subject;
}

void ValueFlowSolver::report()
{
    if (!m_input.build)
        return;
    BuildContext& build = *m_input.build;
    if (m_counters.isPastEdgeBudget)
        build.gaveUp(LimitID::MaxValueFlowEdgesLogged, Subject { });
    for (size_t index : m_statementsWithMembersNotToldApart) {
        const ValueFlowStatement& statement = statementAt(ValueFlowStatementID(index));
        build.gaveUp(LimitID::MaxValueFlowMembersToldApart, Subject { .function = statement.function, .value = statement.site });
    }
    build.statistics().count(hitsOf(LimitID::MaxValueFlowSetMembers), m_counters.nodesSaturated);
    build.statistics().count(StatisticID::LayoutsOutsideFamily, m_counters.nodesHoldingObjectsOutsideFamily);

    // A product is built all the same: the node is not proven, so nothing rests on it. A validating build is there to find the fault.
    if (!build.validates())
        return;
    for (const ObjectOutsideFamily& entry : m_objectsOutsideFamilies.span().first(std::min<size_t>(m_objectsOutsideFamilies.size(), mostNodesReportedOutsideFamily))) {
        Subject subject = subjectOf(entry.node);
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::MaskCutKnownValue,
            .function = subject.function,
            .value = subject.value,
            .layout = m_input.universe ? m_input.universe->layoutOf(entry.object) : LayoutID(),
            .pass = PassID::AnalyzeFlow,
            .detail = describeWayOf(entry),
        });
    }
}

// In the words that valueflow-why takes (holds=nN:#M), and the values on the way from where the member comes into being.
String ValueFlowSolver::describeWayOf(const ObjectOutsideFamily& entry) const
{
    AOT_THRESHOLD(size_t, mostValuesNamed, 12);
    StringPrintStream out;
    out.print("n", entry.node.index(), ":#", entry.object.index());
    size_t numberOfValuesNamed = 0;
    for (ValueFlowNodeID node : explain(entry.node, entry.object)) {
        Subject subject = subjectOf(node);
        if (!subject.function || !subject.value)
            continue;
        if (++numberOfValuesNamed > mostValuesNamed) {
            out.print(" ..");
            break;
        }
        out.print(numberOfValuesNamed == 1 ? "; from " : " to ", "f", subject.function.index(), ":v", subject.value.index());
    }
    return out.toString();
}

bool ValueFlowSolver::solve()
{
    if (m_input.build)
        m_seedOfSchedule = m_input.build->recordedOptions().has(TestingSwitch::DelayFlowRules) + 2 * m_input.build->recordedOptions().has(TestingSwitch::DelayOtherFlowRules);
    m_graph.addNodes(m_input.numberOfNodes);
    // A mask stands at the gate of a node and an edge in the links of two: whoever sets the one looks at nothing of the other.
    runTasksInParallel(m_loops, {
        [&] {
            for (const ValueFlowNodeMask& mask : m_input.masks)
                m_graph.setMask(mask.node, mask.mask, mask.kind, mask.familyObjects);
            if (m_input.lost)
                m_graph.setMask(m_input.lost, ValueSets::complement(m_objectsAlwaysFollowed));
        },
        [&] {
            m_graph.addInitialEdges(m_input.edges);
        },
    });

    uint32_t numberOfBodies = m_input.ranges.size() - 1;
    for (uint32_t body = 0; body < numberOfBodies; ++body)
        makeStatementsDue(m_input.ranges[body].statement, m_input.ranges[body].statementLeavingBody);
    reach(numberOfBodies - 1, { });
    if (m_input.escaped) {
        ValueFlowStatement statement;
        statement.kind = ValueFlowStatementKind::Escape;
        m_madeStatements.append(MadeStatement { statement, { } });
        m_madeNodesOfStatement.append(noSolverIndex);
        growByDoubling(m_isDue, numberOfStatements());
        makeDue(ValueFlowStatementID(numberOfStatements() - 1));
    }

    if (m_input.nodes && m_input.universe) {
        m_objectsThatAreNotPlain = makeNodes(ValueFlowNodeKey::objectsThatAreNotPlain());
        m_objectsWrittenUnderUnknownKey = makeNodes(ValueFlowNodeKey::objectsWrittenUnderUnknownKey());
        for (unsigned write = 0; write < numberOfValueFlowRareWrites; ++write) {
            ValueFlowNodeID node = makeNodes(ValueFlowNodeKey::objectsWithRareWrite(static_cast<ValueFlowRareWrite>(write)));
            if (!write)
                m_firstOfObjectsWithRareWrite = node;
            RELEASE_ASSERT(node.index() == m_firstOfObjectsWithRareWrite.index() + write);
        }
    }

    findStatementsWithLongRowsBehindThem();

    Vector<ValueSetID> heldAtRest;
    Vector<OptionSet<BuiltinUse>> usesAtRest;
    uint32_t functionsReachedAtRest = 0;

    Vector<ValueFlowNodeID> moved;
    bool isFullRound = m_rounds == Rounds::Full;
    m_stepTimes.isOn = m_input.build && m_input.build->options().logging.logsPhaseTimes;
    for (;;) {
        ++m_counters.rounds;
        m_counters.fullRounds += isFullRound;
        m_isFullRound = isFullRound;
        Counters before = m_counters;
        ValueFlowGraph::Counters graphBefore = m_graph.counters();
        size_t numberOfEdgesBefore = m_graph.numberOfEdges();

        MonotonicTime start = MonotonicTime::now();
        m_stepTimes.begin();
        m_numberOfNodesBeforeRound = m_numberOfNodesMade;
        // Not in the round that runs every statement once more to see that nothing moves.
        if (m_due.isEmpty() && m_dueBehindLongRows.isEmpty()) {
            for (ValueFlowStatementID statement : std::exchange(m_dueLast, { })) {
                if (m_isDueLast.clear(statement.index()))
                    makeDue(statement);
            }
        }
        if (m_seedOfSchedule && !isFullRound)
            letSomeDueStatementsWait();
        if (m_hasLongRows && !isFullRound)
            letLongRowsGoFirst();
        if (!runRules())
            return false;
        MonotonicTime afterRules = MonotonicTime::now();
        bool hasAdded = merge();
        MonotonicTime afterMerge = MonotonicTime::now();
        m_graph.solve(moved, ValueFlowGraph::RetiredNodes::AreFoundAndConnectedByCaller);
        m_stepTimes.didFinish("values: the graph solved"_s);
        if (m_saturates && std::max<uint64_t>(m_graph.numberOfEdges(), numberOfReaders()) > m_input.mostEdgesLogged)
            m_counters.isPastEdgeBudget = true;
        noteWhatPlaceholdersCover();
        connectWhatStoppedPlaceholdersCover();
        m_stepTimes.didFinish("values: placeholders"_s);
        runTasksInParallel(m_loops, {
            [&] {
                makeReadersDue(moved.span());
            },
            [&] {
                m_graph.retireNodesAmong(moved.span());
            },
        });
        m_stepTimes.didFinish("values: readers made due, beside nodes retired"_s);
        // It asks who is retired.
        if (m_saturates)
            saturate(moved.span());
        m_stepTimes.didFinish("values: saturation"_s);
        finishInsertionOfNodesThatRead();
        m_stepTimes.didFinish("values: nodes that read listed"_s);
        m_counters.rules += afterRules - start;
        m_counters.merge += afterMerge - afterRules;
        m_counters.values += MonotonicTime::now() - afterMerge;
        if (m_input.build && m_input.build->options().logging.logsPhaseTimes)
            dataLogLn("AOT flow round ", m_counters.rounds, " | rules ", (afterRules - start).milliseconds(), " ms | merge ", (afterMerge - afterRules).milliseconds(), " ms | values ", (MonotonicTime::now() - afterMerge).milliseconds(), " ms | turns ", m_turnsOfRound, " | statement runs ", m_counters.statementRuns - before.statementRuns, " | nodes ", m_numberOfNodesMade - m_numberOfNodesBeforeRound, " | edges ", m_graph.numberOfEdges() - numberOfEdgesBefore, " | moved ", moved.size(), " | waves ", m_graph.counters().waves - graphBefore.waves, " | evaluations ", m_graph.counters().evaluations - graphBefore.evaluations);

        m_roundsDone.append(Round {
            isFullRound,
            m_turnsOfRound,
            static_cast<uint32_t>(m_counters.statementRuns - before.statementRuns),
            m_counters.functionsReached - before.functionsReached,
            m_numberOfNodesMade - m_numberOfNodesBeforeRound,
            static_cast<uint32_t>(m_graph.numberOfEdges() - numberOfEdgesBefore),
            static_cast<uint32_t>(moved.size()),
            static_cast<uint32_t>(m_graph.counters().waves - graphBefore.waves),
            static_cast<uint32_t>(m_graph.counters().evaluations - graphBefore.evaluations),
        });

        bool isQuiet = !hasAdded && moved.isEmpty() && m_due.isEmpty() && m_dueBehindLongRows.isEmpty() && m_dueLast.isEmpty() && m_dueStatementsThatWait.isEmpty() && m_edgesOfRound.isEmpty() && m_readToLink.isEmpty() && m_writtenToLink.isEmpty() && m_bodiesReachedInRound.isEmpty();
        if (isQuiet && isFullRound && m_rounds == Rounds::Due && !m_usesGeneralRulesOnly) {
            m_usesGeneralRulesOnly = true;
            heldAtRest.appendRange(m_graph.values().begin(), m_graph.values().end());
            // A node at rest takes nothing, and a number that a thread holds is given to a new node: what either holds later says nothing.
            for (uint32_t index = 0; index < heldAtRest.size(); ++index) {
                if (m_graph.isAtRest(ValueFlowNodeID(index)) || (index >= m_input.numberOfNodes && m_madeNodesOfNode[index - m_input.numberOfNodes] == noSolverIndex))
                    heldAtRest[index] = ValueSetID();
            }
            usesAtRest = m_usesOfBuiltin;
            functionsReachedAtRest = m_counters.functionsReached;
            isFullRound = false;
            makeEveryStatementDue();
            continue;
        }
        if ((isQuiet && (isFullRound || m_rounds == Rounds::DueWithoutCheck)) || m_counters.rounds >= m_input.mostRounds)
            break;
        isFullRound = isQuiet || m_rounds == Rounds::Full;
        if (isFullRound)
            makeEveryStatementDue();
    }
    if (m_usesGeneralRulesOnly) {
        AOT_THRESHOLD(size_t, mostNodesNamed, 8);
        for (uint32_t index = 0; index < heldAtRest.size(); ++index) {
            if (!heldAtRest[index] || m_graph.valueOf(ValueFlowNodeID(index)) == heldAtRest[index])
                continue;
            if (m_addedByGeneralRules.nodesThatHoldMore++ < mostNodesNamed)
                m_addedByGeneralRules.firstNodesThatHoldMore.append(ValueFlowNodeID(index));
        }
        m_addedByGeneralRules.functionsReached = m_counters.functionsReached - functionsReachedAtRest;
        for (size_t index = 0; index < m_usesOfBuiltin.size(); ++index)
            m_addedByGeneralRules.builtinsUsedMore += index < usesAtRest.size() ? m_usesOfBuiltin[index] != usesAtRest[index] : !m_usesOfBuiltin[index].isEmpty();
    }
    m_stepTimes.dump();
    fillNumbersThatNobodyGave();
    m_graph.releaseRows();
    spreadMarks();
    findObjectsOutsideFamilies();
    findClosuresFollowedToConstructions();
    findUnknownSources([&] {
        report();
        releaseRoundState();
    });
    return true;
}

// Who is to run again when what moves, which stores and loads have met, what each thread has logged: the rounds read that and nobody else does.
void ValueFlowSolver::releaseRoundState()
{
    m_memoryInBytesAtEndOfRounds = memoryInBytes();
    m_graph.releaseSolvingState(m_input.isAskedHowValuesCame || m_input.remembersWhoLoggedEachEdge ? ValueFlowGraph::Predecessors::AreKept : ValueFlowGraph::Predecessors::AreReleased);
    // Millions of small blocks: they are freed beside the results, as the rest of the solver is freed beside Optimize.
    auto release = [&](auto& state) {
        auto taken = std::exchange(state, { });
        if (m_input.build)
            m_input.build->destroyLater(WTF::move(taken));
    };
    release(m_threads);
    release(m_readerShards);
    release(m_watchShards);
    release(m_firstSetWatchedForCoverOfMember);
    release(m_setsWatchedForCoverOfMembers);
    release(m_isWatched);
    release(m_isWaitedForToHoldSomething);
    release(m_readersOfFunction);
    release(m_valuesShownToStatement);
    release(m_freeUses);
    release(m_readersOfWorker);
    release(m_newMembersOfWorker);
    release(m_watchesToTestOfWorker);
    release(m_newMembers);
    release(m_nodesOfManyObjects);
    release(m_rememberedByStatement);
    release(m_linkShards);
    release(m_edgesOfRound);
    release(m_edgesInFrontOfLinks);
    release(m_readToLink);
    release(m_writtenToLink);
}

OptionSet<BuiltinUse> ValueFlowSolver::usesOf(BuiltinID builtin) const
{
    return builtin.index() < m_usesOfBuiltin.size() ? m_usesOfBuiltin[builtin.index()] : OptionSet<BuiltinUse>();
}

OptionSet<ObjectMutation> ValueFlowSolver::marksOf(ValueFlowObjectID object) const
{
    return m_marksOfObject[object.index()];
}

size_t ValueFlowSolver::memoryInBytes() const
{
    if (m_memoryInBytesAtEndOfRounds)
        return *m_memoryInBytesAtEndOfRounds;
    auto bytesOf = [](const auto& vector) {
        return vector.capacity() * sizeof(vector[0]);
    };
    size_t bytes = m_graph.memoryInBytes() + m_sets.memoryInBytes() + bytesOf(m_madeNodes) + bytesOf(m_madeStatements) + bytesOf(m_firstReaderOfNode) + bytesOf(m_seenByReadersOfNode);
    for (const WatchShard& shard : m_watchShards)
        bytes += bytesOf(shard.readers);
    for (const LinkShard& shard : m_linkShards) {
        for (const NodesOfMembers& nodesOfMembers : shard.nodesOfMembers)
            bytes += bytesOf(nodesOfMembers.chained) + nodesOfMembers.first.capacity() * 12;
    }
    for (const ReaderShard& shard : m_readerShards)
        bytes += bytesOf(shard.readers) + shard.known.memoryInBytes();
    bytes += bytesOf(m_layoutOrCalleeOfMember) + bytesOf(m_lastCountThatSaw) + bytesOf(m_unknownSourceOfNode);
    for (const Name& name : m_names) {
        for (const NodesOfName* nodes : { &name.read, &name.written })
            bytes += bytesOf(nodes->ofFewObjects) + bytesOf(nodes->ofManyObjects) + bytesOf(nodes->ofVeryManyObjects);
    }
    return bytes + bytesOf(m_names);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
