/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowGraph.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTSystemResources.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <numeric>
#include <optional>
#if OS(LINUX)
#include <sys/mman.h>
#endif
#include <wtf/MonotonicTime.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/ParkingLot.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

struct ValueFlowGraph::Worker {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Worker);

    ALWAYS_INLINE ValueSetID setOfLowMembers(uint64_t low)
    {
        LatestSetOfLowMembers& latest = latestSetsOfLowMembers[low * 0x9e3779b97f4a7c15ull >> 56];
        if (latest.low == low) [[likely]]
            return latest.set;
        latest.low = low;
        latest.set = setsOfLowMembers.ensure(low, [&] {
            Vector<ValueFlowObjectID, 16> list;
            for (uint64_t rest = low; rest; rest &= rest - 1)
                list.append(ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest))));
            return maker.of(list.span());
        }).iterator->value;
        return latest.set;
    }

    struct LatestSetOfLowMembers {
        uint64_t low { 0 };
        ValueSetID set { ValueSets::nothing };
    };

    ValueSetMaker& maker;
    Vector<ValueSetID, 16> setsArrived;
    Vector<uint32_t> movedNodes;
    HashMap<uint64_t, ValueSetID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> setsOfLowMembers;
    std::array<LatestSetOfLowMembers, 256> latestSetsOfLowMembers;
    uint64_t evaluations { 0 };
    uint64_t edgesLookedAt { 0 };
};

static ALWAYS_INLINE void relaxProcessor()
{
#if CPU(X86_64)
    __builtin_ia32_pause();
#elif CPU(ARM64)
    asm volatile("isb" ::: "memory");
#endif
}

class ValueFlowGraph::Team final : public ThreadSafeRefCounted<Team> {
public:
    using Step = ScopedLambda<void(unsigned index, unsigned worker)>;

    static Ref<Team> create(const ParallelLoops& loops) { return adoptRef(*new Team(loops)); }

    void run(unsigned count, NOESCAPE const Step& step)
    {
        if (count < 2 || !m_mostHelpers) {
            for (unsigned index = 0; index < count; ++index)
                step(index, 0);
            return;
        }
        RELEASE_ASSERT(count <= mostIndices);
        m_step = &step;
        m_helpersWanted.store(std::min(m_mostHelpers, count / 2), std::memory_order_relaxed);
        m_numberDone.store(0);
        m_numberOfStep = (m_numberOfStep + 1) & ((1u << 24) - 1);
        m_ticket.store(static_cast<uint64_t>(m_numberOfStep) << 40 | static_cast<uint64_t>(count) << 20, std::memory_order_seq_cst);
        askForHelper();
        askForHelper();
        takeIndices(0);
        for (unsigned spins = 0; spins < 2048; ++spins) {
            if (m_numberDone.load() == count)
                return;
            relaxProcessor();
        }
        m_isWaitedFor.store(true, std::memory_order_seq_cst);
        for (uint32_t done = m_numberDone.load(); done != count; done = m_numberDone.load())
            ParkingLot::compareAndPark(&m_numberDone, done);
        m_isWaitedFor.store(false, std::memory_order_relaxed);
    }

    void finish()
    {
        m_ticket.store(isOver, std::memory_order_seq_cst);
        for (unsigned spins = 0; m_numberHelping.load(std::memory_order_seq_cst); ++spins) {
            if (spins < 2048)
                relaxProcessor();
            else
                Thread::yield();
        }
    }

private:
    static constexpr uint64_t isOver = UINT64_MAX;
    static constexpr unsigned mostIndices = (1u << 20) - 1;
    AOT_CLASS_THRESHOLD(unsigned, mostWorkers, 64);
    static constexpr Seconds timeToLookAgain = Seconds::fromMicroseconds(40);

    explicit Team(const ParallelLoops& loops)
        : m_loops(loops)
        , m_mostHelpers(std::min(loops.width(), mostWorkers) - 1)
    {
    }

    static unsigned nextOf(uint64_t ticket) { return ticket & mostIndices; }
    static unsigned countOf(uint64_t ticket) { return ticket >> 20 & mostIndices; }
    static bool hasIndexToTake(uint64_t ticket) { return ticket != isOver && nextOf(ticket) < countOf(ticket); }

    void takeIndices(unsigned worker)
    {
        uint64_t ticket = m_ticket.load(std::memory_order_acquire);
        while (hasIndexToTake(ticket)) {
            if (!m_ticket.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            (*m_step)(nextOf(ticket), worker);
            if (m_numberDone.exchangeAdd(1) + 1 == countOf(ticket) && m_isWaitedFor.load(std::memory_order_seq_cst))
                ParkingLot::unparkAll(&m_numberDone);
            ticket = m_ticket.load(std::memory_order_acquire);
        }
    }

    void askForHelper()
    {
        if (m_ticket.load(std::memory_order_relaxed) == isOver)
            return;
        unsigned present = m_numberOfHelpers.load(std::memory_order_relaxed);
        do {
            if (present >= m_helpersWanted.load(std::memory_order_relaxed))
                return;
        } while (!m_numberOfHelpers.compare_exchange_weak(present, present + 1, std::memory_order_relaxed));
        ref();
        m_loops.schedule([](void* team) {
            adoptRef(*static_cast<Team*>(team))->help();
        }, this);
    }

    void help()
    {
        m_numberHelping.fetch_add(1, std::memory_order_seq_cst);
        uint64_t free = m_freeWorkers.load(std::memory_order_relaxed);
        while (!m_freeWorkers.compare_exchange_weak(free, free & (free - 1), std::memory_order_acquire, std::memory_order_relaxed)) { }
        unsigned worker = std::countr_zero(free);
        ASSERT(free && worker && worker <= m_mostHelpers);

        MonotonicTime end = MonotonicTime::now() + timeToLookAgain;
        for (unsigned spins = 1;; ++spins) {
            uint64_t ticket = m_ticket.load(std::memory_order_seq_cst);
            if (ticket == isOver)
                break;
            if (hasIndexToTake(ticket)) {
                if (countOf(ticket) - nextOf(ticket) > 1) {
                    askForHelper();
                    askForHelper();
                }
                takeIndices(worker);
                end = MonotonicTime::now() + timeToLookAgain;
                continue;
            }
            relaxProcessor();
            if (!(spins & 63) && MonotonicTime::now() > end)
                break;
        }
        m_freeWorkers.fetch_or(1ull << worker, std::memory_order_release);
        m_numberOfHelpers.fetch_sub(1, std::memory_order_relaxed);
        m_numberHelping.fetch_sub(1, std::memory_order_seq_cst);
    }

    const ParallelLoops& m_loops;
    const unsigned m_mostHelpers;
    const Step* m_step { nullptr };
    uint32_t m_numberOfStep { 0 };
    alignas(128) std::atomic<uint64_t> m_ticket { 0 };
    alignas(128) Atomic<uint32_t> m_numberDone { 0 };
    std::atomic<bool> m_isWaitedFor { false };
    alignas(128) std::atomic<unsigned> m_numberOfHelpers { 0 };
    std::atomic<unsigned> m_helpersWanted { 0 };
    std::atomic<unsigned> m_numberHelping { 0 };
    std::atomic<uint64_t> m_freeWorkers { ~1ull };
};

template<typename T>
void ValueFlowGraph::ArrayByNode<T>::release()
{
    if (m_elements)
        releasePages(m_elements, roundUpToMultipleOf(pageSize(), m_capacity * sizeof(T)));
    m_elements = nullptr;
    m_capacity = 0;
}

template<typename T>
void ValueFlowGraph::ArrayByNode<T>::release(Team& team)
{
    constexpr size_t bytesPerShare = 8 * MB;
    size_t bytes = m_elements ? roundUpToMultipleOf(pageSize(), m_capacity * sizeof(T)) : 0;
    team.run(static_cast<unsigned>((bytes + bytesPerShare - 1) / bytesPerShare), [&](unsigned share, unsigned) {
        size_t first = share * bytesPerShare;
        OSAllocator::decommit(reinterpret_cast<uint8_t*>(m_elements) + first, std::min(bytesPerShare, bytes - first));
    });
    release();
}

template<typename T>
void ValueFlowGraph::ArrayByNode<T>::grow(size_t numberInUse, size_t capacity)
{
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    T* elements = static_cast<T*>(allocatePages(roundUpToMultipleOf(pageSize(), capacity * sizeof(T))));
    if (m_elements) {
        memcpy(static_cast<void*>(elements), m_elements, numberInUse * sizeof(T));
        releasePages(m_elements, roundUpToMultipleOf(pageSize(), m_capacity * sizeof(T)));
    }
    m_elements = elements;
    m_capacity = capacity;
}

void ValueFlowGraph::Arena::release()
{
    for (void* slab : m_slabs)
        fastFree(slab);
    m_slabs.clear();
    m_free = { };
    m_next = nullptr;
    m_end = nullptr;
    m_bytes = 0;
}

uint32_t* ValueFlowGraph::Arena::allocate(uint32_t numberOfWords)
{
    uint32_t*& free = m_free[std::countr_zero(numberOfWords)];
    if (free) {
        uint32_t* block = free;
        memcpy(&free, block, sizeof(free));
        return block;
    }
    if (static_cast<size_t>(m_end - m_next) < numberOfWords) {
        constexpr size_t smallestSlab = 16 * KB / sizeof(uint32_t);
        constexpr size_t largestSlab = 2 * MB / sizeof(uint32_t);
        size_t size = std::max<size_t>(numberOfWords, std::clamp(m_bytes / sizeof(uint32_t) / 2, smallestSlab, largestSlab));
        m_next = static_cast<uint32_t*>(fastMalloc(size * sizeof(uint32_t)));
        m_end = m_next + size;
        m_slabs.append(m_next);
        m_bytes += size * sizeof(uint32_t);
    }
    return std::exchange(m_next, m_next + numberOfWords);
}

void ValueFlowGraph::Arena::free(uint32_t* block, uint32_t numberOfWords)
{
    uint32_t*& free = m_free[std::countr_zero(numberOfWords)];
    memcpy(block, &free, sizeof(free));
    free = block;
}

ValueFlowGraph::ValueFlowGraph(const ParallelLoops& loops, Makers makers)
    : m_loops(loops)
    , m_makers(makers)
    , m_sets(makers[0]->sets())
    , m_team(Team::create(loops))
{
    RELEASE_ASSERT(makers.size() >= loops.width());
    static_assert(sizeof(Row) == sizeof(Links));
    m_shiftFromNodeToPage = std::countr_zero(pageSize() / sizeof(Links));
    m_lowMembers = makers[0]->ofRange(ValueFlowObjectID(0u), ValueFlowObjectID(std::min(numberOfLowMembers, m_sets.sizeOfUniverse())));
    for (const std::unique_ptr<ValueSetMaker>& maker : makers)
        m_workers.append(makeUnique<Worker>(*maker));
    for (unsigned shard = 0; shard < numberOfShards; ++shard)
        m_shards.append(makeUnique<Shard>());
    m_masks.append(Mask { });
}

ValueFlowGraph::~ValueFlowGraph()
{
    m_team->finish();
}

ValueFlowNodeID ValueFlowGraph::addNodes(uint32_t count)
{
    ValueFlowNodeID first(m_numberOfNodes);
    size_t size = static_cast<size_t>(m_numberOfNodes) + count;
    RELEASE_ASSERT(size < ValueFlowRoundLog::numberedByLog);
    if (size > m_capacity) {
        size_t capacity = std::max<size_t>(m_capacity ? m_capacity * 2 : size * 4, std::max<size_t>(size, 4096));
        m_gates.grow(m_numberOfNodes, capacity);
        m_rows.grow(m_numberOfNodes, capacity);
        m_successors.grow(m_numberOfNodes, capacity);
        m_predecessors.grow(m_numberOfNodes, capacity);
        m_value.grow(m_numberOfNodes, capacity);
        m_capacity = capacity;
        Vector<std::atomic<uint64_t>> arePagesWritable((capacity >> m_shiftFromNodeToPage) / 64 + 1);
        for (uint32_t page = 0; page < m_numberOfNodes >> m_shiftFromNodeToPage; ++page)
            arePagesWritable[page / 64].store(arePagesWritable[page / 64].load(std::memory_order_relaxed) | 1ull << page % 64, std::memory_order_relaxed);
        m_arePagesWritable = WTF::move(arePagesWritable);
        for (Vector<std::atomic<uint64_t>>* bits : { &m_isConstant, &m_areEdgesOutOfConstantListed }) {
            Vector<std::atomic<uint64_t>> grown(capacity / 64 + 1);
            for (size_t word = 0; word < bits->size(); ++word)
                grown[word].store((*bits)[word].load(std::memory_order_relaxed), std::memory_order_relaxed);
            *bits = WTF::move(grown);
        }
        m_hasMoved.grow(m_numberOfNodes, capacity);
        for (Covering& covering : m_coverings) {
            covering.isRetired.ensureSize(capacity);
            covering.hasStoppedAt.ensureSize(capacity);
        }
    }
    m_numberOfNodes = size;
    return first;
}

NEVER_INLINE void ValueFlowGraph::makePagesWritable(uint32_t page) const
{
#if OS(LINUX)
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif
    size_t firstNode = static_cast<size_t>(page) << m_shiftFromNodeToPage;
    madvise(m_rows.data() + firstNode, pageSize(), MADV_POPULATE_WRITE);
    madvise(m_successors.data() + firstNode, pageSize(), MADV_POPULATE_WRITE);
    madvise(m_predecessors.data() + firstNode, pageSize(), MADV_POPULATE_WRITE);
#endif
    m_arePagesWritable[page / 64].fetch_or(1ull << page % 64, std::memory_order_relaxed);
}

void ValueFlowGraph::setMask(ValueFlowNodeID node, ValueSetID admitted, ValueFlowMaskKind kind, ValueSetID familyObjects)
{
    ASSERT(m_value[node.index()] == ValueSets::nothing);
    if (admitted == ValueSets::anything) {
        m_gates[node.index()].mask = 0;
        return;
    }

    Mask mask { admitted, kind == ValueFlowMaskKind::Plain ? ValueSetID() : familyObjects, kind };
    uint64_t mixed = (static_cast<uint64_t>(mask.admitted.index()) << 32 | mask.familyObjects.index()) * 0x9e3779b97f4a7c15ull;
    uint32_t hash = static_cast<uint32_t>((mixed ^ mixed >> 32 ^ static_cast<uint64_t>(kind)) * 0xbf58476d1ce4e5b9ull >> 32);
    std::optional<uint32_t> number = m_numberOfMask.find(hash, [&](uint32_t candidate) {
        return m_masks[candidate].isSameAs(mask);
    });
    if (!number) {
        number = static_cast<uint32_t>(m_masks.size());
        RELEASE_ASSERT(*number < 1u << 22);
        m_numberOfMask.add(hash, *number);
        mask.lowMembersAdmitted = lowMembersOf(admitted, *m_makers[0]);
        m_masks.append(mask);
    }
    m_gates[node.index()].mask = *number;
}

ALWAYS_INLINE void ValueFlowGraph::addLink(Arena& arena, Links& links, uint32_t other)
{
    uint32_t count = links.count++;
    if (count < 2) {
        links.two[count] = other;
        return;
    }
    uint32_t room = std::bit_ceil(count);
    if (count == room) {
        uint32_t* block = arena.allocate(2 * room);
        memcpy(block, count <= 2 ? links.two : links.block, count * sizeof(uint32_t));
        if (count > 2)
            arena.free(links.block, room);
        links.block = block;
    }
    links.block[count] = other;
}

uint64_t ValueFlowGraph::lowMembersOf(ValueSetID set, ValueSetMaker& maker) const
{
    uint64_t low = 0;
    m_sets.forEachMember(maker.intersect(set, m_lowMembers), [&](ValueFlowObjectID member) {
        low |= 1ull << member.index();
    });
    return low;
}

ALWAYS_INLINE void ValueFlowGraph::pass(uint32_t node, uint32_t numberOfMask, uint64_t& low, uint64_t& lowInNoSet, ValueSetID& set, Worker& worker)
{
    const Mask& mask = m_masks[numberOfMask];
    if (!mask.familyObjects) [[likely]] {
        if (mask.kind == ValueFlowMaskKind::Plain || mask.kind == ValueFlowMaskKind::Checked) {
            low &= mask.lowMembersAdmitted;
            lowInNoSet &= mask.lowMembersAdmitted;
            if (set != ValueSets::nothing)
                set = worker.maker.intersect(set, mask.admitted);
        }
        return;
    }

    ValueSetID value = !lowInNoSet ? set : set == ValueSets::nothing ? worker.setOfLowMembers(lowInNoSet) : worker.maker.unite(set, worker.setOfLowMembers(lowInNoSet));
    ValueFlowCoveredObjects covered;
    for (uint32_t index = 0; index < m_coverings.size(); ++index) {
        const Covering& covering = m_coverings[index];
        if (!covering.isFirstOfPlaceholder)
            continue;
        bool isUntracked = covering.placeholder == m_placeholders.untracked;
        (isUntracked ? covered.byUntracked : covered.byUnknown) = ValueSets::nothing;
        if ((isUntracked || mask.kind == ValueFlowMaskKind::Checked) && !covering.hasStoppedAt.quickGet(node) && m_sets.contains(value, covering.placeholder))
            m_shards[shardOf(node)]->placeholdersStopped.append({ node, index });
    }
    set = worker.maker.pass(value, mask.admitted, mask.familyObjects, mask.kind, m_placeholders, covered);
    low = lowMembersOf(set, worker.maker);
    lowInNoSet = 0;
}

void ValueFlowGraph::takeNodesAtWhichPlaceholdersStopped(Vector<StoppedPlaceholder>& stopped)
{
    stopped.shrink(0);
    for (const std::unique_ptr<Shard>& shard : m_shards) {
        for (auto [node, index] : shard->placeholdersStopped) {
            if (!m_coverings[index].hasStoppedAt.quickSet(node))
                stopped.append({ ValueFlowNodeID(node), m_coverings[index].placeholder });
        }
        shard->placeholdersStopped.shrink(0);
    }
    AOT::sort(stopped, [](const StoppedPlaceholder& a, const StoppedPlaceholder& b) {
        return std::pair { a.node.index(), a.placeholder.index() } < std::pair { b.node.index(), b.placeholder.index() };
    });
}

ALWAYS_INLINE void ValueFlowGraph::hold(uint32_t node, uint64_t low, ValueSetID value, Worker& worker)
{
    m_value[node] = value;
    m_rows[node].low = low;
    m_rows[node].set = (value == worker.setOfLowMembers(low) ? ValueSets::nothing : value).index();
}

void ValueFlowGraph::makeConstant(ValueFlowNodeID node, ValueSetID set, unsigned worker, EdgesOutOfConstant edges)
{
    m_isConstant[node.index() / 64].fetch_or(1ull << node.index() % 64, std::memory_order_relaxed);
    if (edges == EdgesOutOfConstant::AreListed)
        m_areEdgesOutOfConstantListed[node.index() / 64].fetch_or(1ull << node.index() % 64, std::memory_order_relaxed);
    hold(node.index(), lowMembersOf(set, *m_makers[worker]), set, *m_workers[worker]);
}

ALWAYS_INLINE void ValueFlowGraph::startWithValueOf(ValueFlowNodeID node, ValueFlowNodeID other, Worker& worker)
{
    uint64_t low = m_rows[other.index()].low;
    uint64_t lowInNoSet = 0;
    ValueSetID value = m_value[other.index()];
    if (uint32_t mask = m_gates[node.index()].mask)
        pass(node.index(), mask, low, lowInNoSet, value, worker);
    hold(node.index(), low, value, worker);
}

void ValueFlowGraph::startWithValueOf(ValueFlowNodeID node, ValueFlowNodeID other)
{
    startWithValueOf(node, other, *m_workers[0]);
}

void ValueFlowGraph::startWithValuesOf(std::span<const ValueFlowEdge> edges)
{
    AOT_THRESHOLD(size_t, fewestForThreads, 256);
    constexpr size_t distance = 8;
    if (edges.size() < fewestForThreads || m_loops.width() == 1) {
        for (size_t index = 0; index < edges.size(); ++index) {
            if (index + distance < edges.size())
                prefetchToStartWithValueOf(edges[index + distance].to, edges[index + distance].from);
            startWithValueOf(edges[index].to, edges[index].from, *m_workers[0]);
        }
        return;
    }
    m_team->run(numberOfShards, [&](unsigned number, unsigned worker) {
        for (ValueFlowEdge edge : edges) {
            if (shardOf(edge.to.index()) == number)
                startWithValueOf(edge.to, edge.from, *m_workers[worker]);
        }
    });
}

void ValueFlowGraph::putToRest(ValueFlowNodeID first, uint32_t count)
{
    for (uint32_t offset = 0; offset < count; ++offset)
        m_gates[first.index() + offset].flags |= IsAtRest | IgnoresArrivals;
}

void ValueFlowGraph::wake(ValueFlowNodeID first, uint32_t count)
{
    for (uint32_t offset = 0; offset < count; ++offset) {
        m_gates[first.index() + offset].flags &= ~IsAtRest;
        if (!takesNoMore(first.index() + offset))
            m_gates[first.index() + offset].flags &= ~IgnoresArrivals;
        m_nodesWoken.append(first.index() + offset);
    }
}

bool ValueFlowGraph::EdgeSet::add(ValueFlowEdge edge)
{
    if ((m_size + 1) * 3 > m_slots.span().size() * 2)
        grow();
    uint64_t key = keyOf(edge);
    size_t mask = m_slots.span().size() - 1;
    size_t position = firstPositionOf(key);
    for (; m_slots[position]; position = (position + 1) & mask) {
        if (m_slots[position] == key)
            return false;
    }
    m_slots[position] = key;
    ++m_size;
    return true;
}

bool ValueFlowGraph::EdgeSet::contains(ValueFlowEdge edge) const
{
    if (!m_slots)
        return false;
    uint64_t key = keyOf(edge);
    size_t mask = m_slots.span().size() - 1;
    for (size_t position = firstPositionOf(key); m_slots[position]; position = (position + 1) & mask) {
        if (m_slots[position] == key)
            return true;
    }
    return false;
}

void ValueFlowGraph::EdgeSet::prefetch(ValueFlowEdge edge) const
{
    if (m_slots)
        __builtin_prefetch(&m_slots[firstPositionOf(keyOf(edge))]);
}

void ValueFlowGraph::EdgeSet::grow()
{
    size_t size = std::max<size_t>(m_slots.span().size() * 2, 1024);
    MallocSpan<uint64_t> previous = std::exchange(m_slots, MallocSpan<uint64_t>::malloc(size * sizeof(uint64_t)));
    zeroSpan(m_slots.mutableSpan());
    size_t mask = size - 1;
    for (uint64_t key : previous.span()) {
        if (!key)
            continue;
        size_t position = firstPositionOf(key);
        while (m_slots[position])
            position = (position + 1) & mask;
        m_slots[position] = key;
    }
}

ALWAYS_INLINE bool ValueFlowGraph::addSuccessorIfAbsent(ValueFlowEdge edge)
{
    Shard& shard = *m_shards[shardOf(edge.from.index())];
    willUse(edge.from.index());
    Links& successors = m_successors[edge.from.index()];
    if (successors.count > mostSuccessorsScanned ? !shard.edgesOfNodesWithManySuccessors.add(edge) : successors.contains(edge.to.index()))
        return false;
    if (successors.count == mostSuccessorsScanned) {
        for (uint32_t index = 0; index < successors.count; ++index)
            shard.edgesOfNodesWithManySuccessors.add(ValueFlowEdge { edge.from, ValueFlowNodeID(successors.begin()[index]) });
        shard.edgesOfNodesWithManySuccessors.add(edge);
    }
    if (successors.count == successors.older)
        shard.nodesWithRecentSuccessors.append(edge.from.index());
    addLink(shard.blocksOfSuccessors, successors, edge.to.index());
    return true;
}

void ValueFlowGraph::willHaveManyPredecessors(Shard& shard, uint32_t node)
{
    const Links& predecessors = m_predecessors[node];
    const uint32_t* others = predecessors.begin();
    for (uint32_t index = 0; index < predecessors.count; ++index) {
        if (isConstant(others[index]))
            shard.edgesOutOfConstantsIntoNodesWithManyPredecessors.add(ValueFlowEdge { ValueFlowNodeID(others[index]), ValueFlowNodeID(node) });
    }
}

ALWAYS_INLINE void ValueFlowGraph::addPredecessor(Shard& shard, ValueFlowEdge edge)
{
    Links& predecessors = m_predecessors[edge.to.index()];
    if (predecessors.count == mostPredecessorsScanned)
        willHaveManyPredecessors(shard, edge.to.index());
    if (predecessors.count == predecessors.older)
        shard.nodesWithRecentPredecessors.append(edge.to.index());
    addLink(shard.blocksOfPredecessors, predecessors, edge.from.index());
}

ALWAYS_INLINE bool ValueFlowGraph::addEdgeOutOfConstantIfAbsent(ValueFlowEdge edge)
{
    Shard& shard = *m_shards[shardOf(edge.to.index())];
    willUse(edge.to.index());
    const Links& predecessors = m_predecessors[edge.to.index()];
    if (predecessors.count > mostPredecessorsScanned ? !shard.edgesOutOfConstantsIntoNodesWithManyPredecessors.add(edge) : predecessors.contains(edge.from.index()))
        return false;
    bool willHaveMany = predecessors.count == mostPredecessorsScanned;
    addPredecessor(shard, edge);
    if (willHaveMany)
        shard.edgesOutOfConstantsIntoNodesWithManyPredecessors.add(edge);
    if (m_areEdgesOutOfConstantListed[edge.from.index() / 64].load(std::memory_order_relaxed) >> edge.from.index() % 64 & 1)
        shard.listedEdgesOutOfConstants.append(edge);
    return true;
}

ALWAYS_INLINE bool ValueFlowGraph::addEdgeIfAbsent(ValueFlowEdge edge)
{
    return isConstant(edge.from.index()) ? addEdgeOutOfConstantIfAbsent(edge) : addSuccessorIfAbsent(edge);
}

size_t ValueFlowGraph::addEdges(std::span<const ValueFlowEdge> edges)
{
    constexpr size_t distance = 12;
    AOT_THRESHOLD(size_t, fewestEdgesForThreads, 8192);
    size_t before = m_edgesNotInLists.size();
    if (edges.size() < fewestEdgesForThreads || m_loops.width() == 1) {
        for (size_t index = 0; index < edges.size(); ++index) {
            if (index + distance < edges.size()) {
                ValueFlowEdge ahead = edges[index + distance];
                __builtin_prefetch(isConstant(ahead.from.index()) ? &m_predecessors[ahead.to.index()] : &m_successors[ahead.from.index()], 1);
            }
            ValueFlowEdge edge = edges[index];
            if (edge.from != edge.to && addEdgeIfAbsent(edge))
                m_edgesNotInLists.append(edge);
        }
        return m_edgesNotInLists.size() - before;
    }

    m_isNewEdge.resize(edges.size());
    constexpr unsigned numberOfParts = 64;
    m_hasLeftMessagesWithShards = true;
    size_t edgesPerPart = (edges.size() + numberOfParts - 1) / numberOfParts;
    m_indicesBySource.grow(numberOfParts * numberOfShards);
    m_team->run(numberOfParts, [&](unsigned part, unsigned) {
        for (size_t index = part * edgesPerPart; index < std::min(edges.size(), (part + 1) * edgesPerPart); ++index) {
            m_isNewEdge[index] = 0;
            if (edges[index].from != edges[index].to)
                m_indicesBySource[part * numberOfShards + shardThatKeeps(edges[index])].append(static_cast<uint32_t>(index));
        }
    });
    m_team->run(numberOfShards, [&](unsigned number, unsigned) {
        for (unsigned part = 0; part < numberOfParts; ++part) {
            Vector<uint32_t>& own = m_indicesBySource[part * numberOfShards + number];
            for (size_t position = 0; position < own.size(); ++position) {
                if (position + distance < own.size()) {
                    ValueFlowEdge ahead = edges[own[position + distance]];
                    __builtin_prefetch(isConstant(ahead.from.index()) ? &m_predecessors[ahead.to.index()] : &m_successors[ahead.from.index()], 1);
                }
                if (addEdgeIfAbsent(edges[own[position]]))
                    m_isNewEdge[own[position]] = 1;
            }
            own.shrink(0);
        }
    });
    std::array<size_t, numberOfParts + 1> firstNewEdgeOfPart;
    firstNewEdgeOfPart[0] = before;
    m_team->run(numberOfParts, [&](unsigned part, unsigned) {
        size_t count = 0;
        for (size_t index = part * edgesPerPart; index < std::min(edges.size(), (part + 1) * edgesPerPart); ++index)
            count += m_isNewEdge[index];
        firstNewEdgeOfPart[part + 1] = count;
    });
    for (unsigned part = 0; part < numberOfParts; ++part)
        firstNewEdgeOfPart[part + 1] += firstNewEdgeOfPart[part];
    if (firstNewEdgeOfPart[numberOfParts] > m_edgesNotInLists.capacity())
        m_edgesNotInLists.reserveCapacity(std::max(firstNewEdgeOfPart[numberOfParts], m_edgesNotInLists.capacity() * 2));
    m_edgesNotInLists.grow(firstNewEdgeOfPart[numberOfParts]);
    m_team->run(numberOfParts, [&](unsigned part, unsigned) {
        size_t position = firstNewEdgeOfPart[part];
        for (size_t index = part * edgesPerPart; index < std::min(edges.size(), (part + 1) * edgesPerPart); ++index) {
            if (m_isNewEdge[index])
                m_edgesNotInLists[position++] = edges[index];
        }
    });
    return m_edgesNotInLists.size() - before;
}

void ValueFlowGraph::takeEdgesIntoLists(Team& team, std::span<const ValueFlowEdge> edges)
{
    m_numberOfEdges += edges.size();
    if (edges.empty())
        return;
    m_numberOfRecentEdges += edges.size();

    constexpr size_t distance = 12;
    AOT_THRESHOLD(size_t, fewestEdgesForThreads, 4096);
    auto takeEdge = [&](Shard& shard, ValueFlowEdge edge, Vector<Message>& messages) {
        willUse(edge.to.index());
        if (!isConstant(edge.from.index()))
            addPredecessor(shard, edge);
        const Row& row = m_rows[edge.from.index()];
        if ((!row.low && !row.set) || (m_gates[edge.to.index()].flags & IgnoresArrivals))
            return false;
        if ((m_gates[edge.to.index()].flags & IsCoveredNode) && didCoveredNodeHoldAllOf(edge.to.index(), row.low, row.set))
            return false;
        messages.append(Message { edge.to.index(), row.set, row.low });
        return true;
    };
    if (edges.size() < fewestEdgesForThreads || m_loops.width() == 1) {
        for (size_t index = 0; index < edges.size(); ++index) {
            if (index + distance < edges.size()) {
                __builtin_prefetch(&m_predecessors[edges[index + distance].to.index()], 1);
                __builtin_prefetch(&m_rows[edges[index + distance].from.index()]);
            }
            takeEdge(*m_shards[shardOf(edges[index].to.index())], edges[index], m_messages[m_parity]);
        }
    } else {
        constexpr unsigned numberOfParts = 64;
        m_hasLeftMessagesWithShards = true;
        size_t edgesPerPart = (edges.size() + numberOfParts - 1) / numberOfParts;
        m_edgesByTarget.grow(numberOfParts * numberOfShards);
        m_edgesOnlyToLink.grow(numberOfParts);
        team.run(numberOfParts, [&](unsigned part, unsigned) {
            size_t first = std::min(edges.size(), part * edgesPerPart);
            std::span<const ValueFlowEdge> edgesOfPart = edges.subspan(first, std::min(edgesPerPart, edges.size() - first));
            for (size_t index = 0; index < edgesOfPart.size(); ++index) {
                if (index + distance < edgesOfPart.size() && (m_gates[edgesOfPart[index + distance].to.index()].flags & IsCoveredNode))
                    __builtin_prefetch(&m_rows[edgesOfPart[index + distance].from.index()]);
                ValueFlowEdge edge = edgesOfPart[index];
                if (m_gates[edge.to.index()].flags & IsCoveredNode) {
                    const Row& row = m_rows[edge.from.index()];
                    if ((!row.low && !row.set) || (m_gates[edge.to.index()].flags & IgnoresArrivals) || didCoveredNodeHoldAllOf(edge.to.index(), row.low, row.set)) {
                        if (!isConstant(edge.from.index()))
                            m_edgesOnlyToLink[part].append(edge);
                        continue;
                    }
                }
                m_edgesByTarget[part * numberOfShards + shardOf(edge.to.index())].append(edge);
            }
        });
        std::array<size_t, numberOfShards> numberOfEdgesInto { };
        for (size_t index = 0; index < m_edgesByTarget.size(); ++index)
            numberOfEdgesInto[index % numberOfShards] += m_edgesByTarget[index].size();
        for (const Covering& covering : m_coverings) {
            for (const Vector<ValueFlowEdge>& onlyToLink : m_edgesOnlyToLink)
                numberOfEdgesInto[shardOf(covering.covered)] += onlyToLink.size();
        }
        ShardsInOrder shardsInOrder = longestFirst(numberOfEdgesInto);
        team.run(numberOfShards, [&](unsigned position, unsigned) {
            unsigned number = shardsInOrder[position];
            Shard& shard = *m_shards[number];
            size_t numberOfMessages = 0;
            for (const Covering& covering : m_coverings) {
                if (shardOf(covering.covered) != number)
                    continue;
                willUse(covering.covered);
                for (const Vector<ValueFlowEdge>& onlyToLink : m_edgesOnlyToLink) {
                    for (ValueFlowEdge edge : onlyToLink) {
                        if (edge.to.index() == covering.covered)
                            addPredecessor(shard, edge);
                    }
                }
            }
            for (unsigned part = 0; part < numberOfParts; ++part) {
                Vector<ValueFlowEdge>& byTarget = m_edgesByTarget[part * numberOfShards + number];
                for (size_t index = 0; index < byTarget.size(); ++index) {
                    if (index + distance < byTarget.size()) {
                        __builtin_prefetch(&m_predecessors[byTarget[index + distance].to.index()], 1);
                        __builtin_prefetch(&m_rows[byTarget[index + distance].from.index()]);
                    }
                    numberOfMessages += takeEdge(shard, byTarget[index], shard.messages[m_parity][number]);
                }
                byTarget.shrink(0);
            }
            if (numberOfMessages)
                m_shardsWithMessagesOfShard[m_parity][number] |= 1ull << number;
            m_numberOfMessagesWithShards.fetch_add(numberOfMessages, std::memory_order_relaxed);
        });
        for (Vector<ValueFlowEdge>& onlyToLink : m_edgesOnlyToLink)
            onlyToLink.shrink(0);
    }
    if (m_numberOfRecentEdges * 2 <= m_numberOfOlderEdges)
        return;
    m_numberOfOlderEdges += std::exchange(m_numberOfRecentEdges, 0);
    team.run(numberOfShards, [&](unsigned number, unsigned) {
        Shard& shard = *m_shards[number];
        for (uint32_t node : shard.nodesWithRecentSuccessors)
            m_successors[node].older = m_successors[node].count;
        for (uint32_t node : shard.nodesWithRecentPredecessors)
            m_predecessors[node].older = m_predecessors[node].count;
        shard.nodesWithRecentSuccessors.shrink(0);
        shard.nodesWithRecentPredecessors.shrink(0);
    });
}

void ValueFlowGraph::append(Covering&& covering)
{
    RELEASE_ASSERT(m_coverings.size() < mostCoverings);
    covering.bitInGates = 1 << m_coverings.size();
    covering.isRetired.ensureSize(m_capacity);
    covering.hasStoppedAt.ensureSize(m_capacity);
    m_coverings.append(WTF::move(covering));
}

void ValueFlowGraph::retireNodesCoveredBy(ValueFlowObjectID placeholder, ValueFlowNodeID covered, ValueSetID whole, ValueSetID coverable)
{
    RELEASE_ASSERT(!m_hasSolved);
    RELEASE_ASSERT(m_coverings.isEmpty() || !m_coverings.last().retiresNoNodeByItself);
    Covering covering;
    covering.placeholder = placeholder;
    covering.isFirstOfPlaceholder = !std::ranges::contains(m_coverings, placeholder, &Covering::placeholder);
    covering.covered = covered.index();
    covering.lowMembersOfWhole = lowMembersOf(whole, *m_makers[0]);
    covering.listedMembersOfWhole = m_makers[0]->intersect(whole, ValueSets::complement(m_lowMembers));
    covering.allButCoverable = ValueSets::complement(coverable);
    covering.lowMembersNotCoverable = lowMembersOf(covering.allButCoverable, *m_makers[0]);
    append(WTF::move(covering));
}

void ValueFlowGraph::handOnWhatArrivesAtRetiredNodesTo(ValueFlowNodeID taking, IsMadeForPlaceholder isMadeForPlaceholder, const void* context)
{
    RELEASE_ASSERT(!m_hasSolved);
    Covering covering;
    covering.isFirstOfPlaceholder = false;
    covering.retiresNoNodeByItself = true;
    covering.isMadeForPlaceholder = isMadeForPlaceholder;
    covering.contextOfIsMadeForPlaceholder = context;
    covering.covered = taking.index();
    covering.allButCoverable = ValueSets::anything;
    append(WTF::move(covering));
}

void ValueFlowGraph::keepOutOfNodesMadeForPlaceholders(ValueSetID keptOut, ValueFlowNodeID covered, IsMadeForPlaceholder isMadeForPlaceholder, const void* context)
{
    RELEASE_ASSERT(!m_hasSolved);
    m_coveredNodeThatTakesWhatIsKeptOut = covered.index();
    m_isMadeForPlaceholder = isMadeForPlaceholder;
    m_contextOfIsMadeForPlaceholder = context;
    m_allButKeptOutOfNodesMadeForPlaceholders = ValueSets::complement(keptOut);
    m_lowMembersKeptOutOfNodesMadeForPlaceholders = lowMembersOf(keptOut, *m_makers[0]);
}

bool ValueFlowGraph::flowsIntoNodeCoveredBy(ValueFlowObjectID placeholder, ValueFlowNodeID node) const
{
    for (const Covering& covering : m_coverings) {
        if (covering.placeholder == placeholder)
            return node.index() < m_numberOfNodes && flowsIntoCovered(covering, node.index());
    }
    return false;
}

void ValueFlowGraph::noteWhatCoveredNodesHold()
{
    for (Covering& covering : m_coverings) {
        covering.lowMembersHeldByCoveredNode = m_rows[covering.covered].low;
        covering.setHeldByCoveredNode = m_rows[covering.covered].set;
    }
}

bool ValueFlowGraph::didCoveredNodeHoldAllOf(uint32_t covered, uint64_t low, uint32_t set) const
{
    for (const Covering& covering : m_coverings) {
        if (covering.covered != covered)
            continue;
        if (low & ~covering.lowMembersHeldByCoveredNode)
            return false;
        return !set || (covering.setHeldByCoveredNode && m_sets.isSubset(ValueSetID(set), ValueSetID(covering.setHeldByCoveredNode)));
    }
    return false;
}

bool ValueFlowGraph::takesNoMore(uint32_t node) const
{
    return std::ranges::any_of(m_coverings, [&](const Covering& covering) {
        return covering.allButCoverable == ValueSets::nothing && covering.isRetired.quickGet(node);
    });
}

bool ValueFlowGraph::retiredNodeTakesAnyOf(uint32_t node, uint64_t low, uint32_t set) const
{
    for (const Covering& covering : m_coverings) {
        if (covering.retiresNoNodeByItself || !covering.isRetired.quickGet(node))
            continue;
        if (!(low & covering.lowMembersNotCoverable) && (!set || !m_sets.meet(ValueSetID(set), covering.allButCoverable)))
            return false;
    }
    return true;
}

ValueSetID ValueFlowGraph::whatRetiredNodeTakes(uint32_t node, ValueSetMaker& maker) const
{
    ValueSetID taken = ValueSets::anything;
    for (const Covering& covering : m_coverings) {
        if (!covering.retiresNoNodeByItself && covering.isRetired.quickGet(node))
            taken = maker.intersect(taken, covering.allButCoverable);
    }
    return taken;
}

void ValueFlowGraph::findIndices(Team* team, size_t count, Vector<uint32_t>& found, NOESCAPE const Invocable<bool(size_t)> auto& matches)
{
    constexpr size_t indicesPerShare = 2048;
    found.shrink(0);
    if (!team || count < 4 * indicesPerShare) {
        for (size_t index = 0; index < count; ++index) {
            if (matches(index))
                found.append(static_cast<uint32_t>(index));
        }
        return;
    }
    size_t numberOfShares = (count + indicesPerShare - 1) / indicesPerShare;
    if (m_foundInShare.size() < numberOfShares)
        m_foundInShare.grow(numberOfShares);
    team->run(numberOfShares, [&](unsigned share, unsigned) {
        Vector<uint32_t>& foundInShare = m_foundInShare[share];
        foundInShare.shrink(0);
        for (size_t index = share * indicesPerShare; index < std::min(count, (share + 1) * indicesPerShare); ++index) {
            if (matches(index))
                foundInShare.append(static_cast<uint32_t>(index));
        }
    });
    for (size_t share = 0; share < numberOfShares; ++share)
        found.appendVector(m_foundInShare[share]);
}

bool ValueFlowGraph::isToBeConnectedStraightToCoveredNode(const Covering& covering, uint32_t node) const
{
    if (node == covering.covered || !isFollowedBackThrough(covering, node))
        return false;
    // A node may have been retired before it was seen to flow into a node that retires none: then it still has to hand on what it lists.
    if (!covering.retiresNoNodeByItself && (covering.isRetired.quickGet(node) || takesNoMore(node)))
        return false;
    return !hasAdded(ValueFlowEdge { ValueFlowNodeID(node), ValueFlowNodeID(covering.covered) });
}

void ValueFlowGraph::connectStraightToCoveredNodes(Team& team, size_t count, NOESCAPE const auto& forEachNodeToConnect)
{
    AOT_THRESHOLD(size_t, mostSharesPerCovering, 4096);
    size_t indicesPerShare = std::max<size_t>(64, (count + mostSharesPerCovering - 1) / mostSharesPerCovering);
    size_t sharesPerCovering = (count + indicesPerShare - 1) / indicesPerShare;
    size_t numberOfShares = sharesPerCovering * m_coverings.size();
    if (m_straightEdgesOfShare.size() < numberOfShares)
        m_straightEdgesOfShare.grow(numberOfShares);
    team.run(numberOfShares, [&](unsigned share, unsigned) {
        const Covering& covering = m_coverings[share / sharesPerCovering];
        Vector<ValueFlowEdge>& straightEdges = m_straightEdgesOfShare[share];
        size_t first = share % sharesPerCovering * indicesPerShare;
        for (size_t index = first; index < std::min(count, first + indicesPerShare); ++index) {
            forEachNodeToConnect(covering, index, [&](uint32_t node) {
                if (isToBeConnectedStraightToCoveredNode(covering, node))
                    straightEdges.append(ValueFlowEdge { ValueFlowNodeID(node), ValueFlowNodeID(covering.covered) });
            });
        }
    });
    m_straightEdges.shrink(0);
    for (size_t share = 0; share < numberOfShares; ++share) {
        m_straightEdges.appendVector(m_straightEdgesOfShare[share]);
        m_straightEdgesOfShare[share].shrink(0);
    }
    m_counters.edgesStraightIntoCoveredNodes += addEdges(m_straightEdges.span());
}

// One covered node may flow into another. What goes straight to the nearer one reaches both.
bool ValueFlowGraph::isRetiredUnderNearerCovering(const Covering& covering, uint32_t node) const
{
    return std::ranges::any_of(m_coverings, [&](const Covering& nearer) {
        return nearer.covered != covering.covered && flowsIntoCovered(covering, nearer.covered) && nearer.isRetired.quickGet(node);
    });
}

void ValueFlowGraph::connectPredecessorsOfRetiredNodes(Team& team)
{
    if (!m_counters.nodesRetired)
        return;
    findIndices(&team, m_edgesNotInLists.size(), m_found, [&](size_t index) {
        auto flags = m_gates[m_edgesNotInLists[index].to.index()].flags;
        return (flags & IsRetired) && !(flags & IsAtRest);
    });
    connectStraightToCoveredNodes(team, m_found.size() + m_nodesWoken.size(), [&](const Covering& covering, size_t index, const auto& connect) {
        if (index < m_found.size()) {
            ValueFlowEdge edge = m_edgesNotInLists[m_found[index]];
            if (covering.isRetired.quickGet(edge.to.index()) && !isRetiredUnderNearerCovering(covering, edge.to.index()))
                connect(edge.from.index());
            return;
        }
        uint32_t node = m_nodesWoken[index - m_found.size()];
        if (covering.isRetired.quickGet(node) && !isRetiredUnderNearerCovering(covering, node))
            forEachOther(m_predecessors[node], connect);
    });
}

bool ValueFlowGraph::lengthensFlowIntoCovered(const Covering& covering, ValueFlowEdge edge) const
{
    uint32_t to = edge.to.index();
    return flowsIntoCovered(covering, to) && !flowsIntoCovered(covering, edge.from.index()) && (to == covering.covered || passesEverythingOn(to)) && isFollowedBackThrough(covering, edge.from.index());
}

void ValueFlowGraph::findEdgesThatLengthenFlowIntoCoveredNodes(Team& team, std::span<const ValueFlowEdge> added)
{
    for (Covering& covering : m_coverings)
        noteFlowIntoCovered(covering, covering.covered);
    findIndices(&team, added.size(), m_edgesThatLengthenFlow, [&](size_t index) {
        if (!(m_gates[added[index].to.index()].coveringsFlowedInto & ~m_gates[added[index].from.index()].coveringsFlowedInto))
            return false;
        return std::ranges::any_of(m_coverings, [&](const Covering& covering) {
            return lengthensFlowIntoCovered(covering, added[index]);
        });
    });
}

void ValueFlowGraph::markNodesFlowingIntoCoveredNodes(std::span<const ValueFlowEdge> added)
{
    Vector<uint32_t, 16> toVisit;
    for (Covering& covering : m_coverings) {
        auto markPredecessorsOf = [&](uint32_t node) {
            forEachOther(m_predecessors[node], [&](uint32_t predecessor) {
                if (isFollowedBackThrough(covering, predecessor) && !noteFlowIntoCovered(covering, predecessor))
                    toVisit.append(predecessor);
            });
        };

        auto passesOnWhatIsCovered = [&](uint32_t node) {
            return node == covering.covered || passesEverythingOn(node);
        };

        m_found.shrink(0);
        for (uint32_t index : m_edgesThatLengthenFlow) {
            if (lengthensFlowIntoCovered(covering, added[index]))
                m_found.append(index);
        }
        for (uint32_t index : m_found) {
            if (!noteFlowIntoCovered(covering, added[index].from.index()))
                toVisit.append(added[index].from.index());
        }
        for (uint32_t node : m_nodesWoken) {
            if (!flowsIntoCovered(covering, node) || !passesOnWhatIsCovered(node))
                continue;
            covering.nodesToLookAt.append(node);
            markPredecessorsOf(node);
        }
        while (!toVisit.isEmpty()) {
            uint32_t node = toVisit.takeLast();
            ++m_counters.nodesFlowingIntoCoveredNodes;
            covering.nodesToLookAt.append(node);
            if (passesEverythingOn(node))
                markPredecessorsOf(node);
        }
    }
}

void ValueFlowGraph::retireNodesAmong(std::span<const ValueFlowNodeID> moved)
{
    Team& team = *m_team;
    Vector<uint32_t> retired;
    auto isToBeRetiredUnder = [&](const Covering& covering, uint32_t node) {
        if (covering.retiresNoNodeByItself)
            return flowsIntoCovered(covering, node) && !covering.isRetired.quickGet(node) && (m_gates[node].flags & IsRetired);
        return flowsIntoCovered(covering, node) && !covering.isRetired.quickGet(node) && passesEverythingOn(node) && !(m_gates[node].flags & IsCoveredNode)
            && (m_rows[node].low & covering.lowMembersOfWhole) == covering.lowMembersOfWhole && m_sets.isSubset(covering.listedMembersOfWhole, ValueSetID(m_rows[node].set));
    };
    Vector<uint32_t> movedToBeRetired;
    findIndices(&team, moved.size(), movedToBeRetired, [&](size_t index) {
        return m_gates[moved[index].index()].coveringsFlowedInto && std::ranges::any_of(m_coverings, [&](const Covering& covering) {
            return isToBeRetiredUnder(covering, moved[index].index());
        });
    });
    for (Covering& covering : m_coverings) {
        auto isToBeRetired = [&](uint32_t node) {
            return isToBeRetiredUnder(covering, node);
        };
        auto retire = [&](uint32_t node) {
            if (covering.isRetired.quickSet(node))
                return;
            m_gates[node].flags |= IsRetired;
            if (covering.allButCoverable == ValueSets::nothing)
                m_gates[node].flags |= IgnoresArrivals;
            retired.append(node);
        };
        for (uint32_t index : movedToBeRetired) {
            if (isToBeRetired(moved[index].index()))
                retire(moved[index].index());
        }
        findIndices(&team, covering.nodesToLookAt.size(), m_found, [&](size_t index) {
            return isToBeRetired(covering.nodesToLookAt[index]);
        });
        for (uint32_t index : m_found)
            retire(covering.nodesToLookAt[index]);
        covering.nodesToLookAt.shrink(0);
        if (covering.retiresNoNodeByItself) {
            for (const Covering& other : m_coverings) {
                for (uint32_t node : other.nodesJustRetired) {
                    if (isToBeRetired(node))
                        retire(node);
                }
            }
        }

        AOT::sort(retired);
        if (!covering.retiresNoNodeByItself)
            m_counters.nodesRetired += retired.size();
        ASSERT(covering.nodesJustRetired.isEmpty());
        std::swap(covering.nodesJustRetired, retired);
    }
    connectPredecessorsOfNodesJustRetired();
    giveBackLargeBuffers();
}

bool ValueFlowGraph::hasNodesJustRetired() const
{
    return std::ranges::any_of(m_coverings, [](const Covering& covering) {
        return !covering.nodesJustRetired.isEmpty();
    });
}

void ValueFlowGraph::connectPredecessorsOfNodesJustRetired()
{
    size_t mostJustRetired = 0;
    for (const Covering& covering : m_coverings)
        mostJustRetired = std::max(mostJustRetired, covering.nodesJustRetired.size());
    if (!mostJustRetired)
        return;
    connectStraightToCoveredNodes(*m_team, mostJustRetired, [&](const Covering& covering, size_t index, const auto& connect) {
        if (index < covering.nodesJustRetired.size() && !isRetiredUnderNearerCovering(covering, covering.nodesJustRetired[index]))
            forEachOther(m_predecessors[covering.nodesJustRetired[index]], connect);
    });
    for (Covering& covering : m_coverings)
        covering.nodesJustRetired.shrink(0);
}

ALWAYS_INLINE void ValueFlowGraph::takeArrival(Arrivals& arrivals, const Message& message)
{
    Row& row = m_rows[message.to];
    if (!row.firstArrivalPlusOne)
        arrivals.nodes.append(message.to);
    else {
        const Arrival& last = arrivals.list[row.firstArrivalPlusOne - 1];
        if (last.set == message.set && last.low == message.low)
            return;
    }
    arrivals.list.append(Arrival { message.low, message.set, row.firstArrivalPlusOne });
    row.firstArrivalPlusOne = arrivals.list.size();
}

ALWAYS_INLINE void ValueFlowGraph::takeArrivals(Arrivals& arrivals, std::span<const Message> messages)
{
    constexpr size_t distance = 12;
    for (size_t index = 0; index < messages.size(); ++index) {
        if (index + distance < messages.size())
            __builtin_prefetch(&m_rows[messages[index + distance].to], 1);
        takeArrival(arrivals, messages[index]);
    }
}

ALWAYS_INLINE void ValueFlowGraph::evaluate(uint32_t node, Arrivals& arrivals, Worker& worker, NOESCAPE const Invocable<void(const Message&)> auto& leave)
{
    Row& row = m_rows[node];
    ValueSetMaker& maker = worker.maker;
    uint64_t low = 0;
    uint64_t lowInNoSet = 0;
    Vector<ValueSetID, 16>& setsArrived = worker.setsArrived;
    setsArrived.shrink(0);
    for (uint32_t index = std::exchange(row.firstArrivalPlusOne, 0u); index; index = arrivals.list[index - 1].nextPlusOne) {
        const Arrival& arrival = arrivals.list[index - 1];
        low |= arrival.low;
        if (arrival.set)
            setsArrived.append(ValueSetID(arrival.set));
        else
            lowInNoSet |= arrival.low;
    }
    if (setsArrived.size() > 2) {
        AOT::sort(setsArrived, { }, &ValueSetID::index);
        removeRepeatedElements(setsArrived);
    }
    ValueSetID set = setsArrived.isEmpty() ? ValueSets::nothing : setsArrived.size() == 1 ? setsArrived[0] : maker.uniteAll(setsArrived.span());
    ++worker.evaluations;
    ++worker.edgesLookedAt;

    Gate gate = m_gates[node];
    // Few nodes are made for a placeholder, and that is soon said. Whether a set brings what is kept out takes a walk over both.
    bool isMadeForPlaceholder = m_isMadeForPlaceholder && (low || set != ValueSets::nothing) && m_isMadeForPlaceholder(m_contextOfIsMadeForPlaceholder, ValueFlowNodeID(node));
    if (isMadeForPlaceholder && ((low & m_lowMembersKeptOutOfNodesMadeForPlaceholders) || (set != ValueSets::nothing && !m_sets.isSubset(set, m_allButKeptOutOfNodesMadeForPlaceholders)))) [[unlikely]] {
        uint64_t lowKeptOut = low & m_lowMembersKeptOutOfNodesMadeForPlaceholders;
        ValueSetID setKeptOut = maker.intersect(set, ValueSets::complement(m_allButKeptOutOfNodesMadeForPlaceholders));
        if (setKeptOut != ValueSets::nothing && (lowKeptOut & lowInNoSet))
            setKeptOut = maker.unite(setKeptOut, worker.setOfLowMembers(lowKeptOut));
        Message keptOut { m_coveredNodeThatTakesWhatIsKeptOut, setKeptOut.index(), lowKeptOut };
        if (!didCoveredNodeHoldAllOf(keptOut.to, keptOut.low, keptOut.set))
            leave(keptOut);
        low &= ~m_lowMembersKeptOutOfNodesMadeForPlaceholders;
        lowInNoSet &= low;
        set = maker.intersect(set, m_allButKeptOutOfNodesMadeForPlaceholders);
    }
    if (gate.flags & IsRetired) [[unlikely]] {
        ValueSetID taken = whatRetiredNodeTakes(node, maker);
        for (uint64_t rest = low; rest; rest &= rest - 1) {
            if (!m_sets.contains(taken, ValueFlowObjectID(static_cast<uint32_t>(std::countr_zero(rest)))))
                low &= ~(rest & -rest);
        }
        lowInNoSet &= low;
        set = maker.intersect(set, taken);
    } else if (gate.mask)
        pass(node, gate.mask, low, lowInNoSet, set, worker);

    uint64_t lowBefore = row.low;
    uint64_t lowAfter = lowBefore | low;
    ValueSetID before(row.set);
    ValueSetID after = before;
    if (set != ValueSets::nothing || before != ValueSets::nothing) {
        uint64_t lowToAdd = before == ValueSets::nothing ? lowBefore | lowInNoSet : lowInNoSet & ~lowBefore;
        if (lowToAdd)
            set = set == ValueSets::nothing ? worker.setOfLowMembers(lowToAdd) : maker.unite(set, worker.setOfLowMembers(lowToAdd));
        if (set != ValueSets::nothing)
            after = before == ValueSets::nothing ? set : maker.unite(before, set);
        if (before == ValueSets::nothing && after == worker.setOfLowMembers(lowAfter))
            after = ValueSets::nothing;
    }
    if (lowAfter == lowBefore && after == before)
        return;
    ValueSetID valueBefore = m_value[node];
    ValueSetID value = after == ValueSets::nothing ? worker.setOfLowMembers(lowAfter) : after;
    row.low = lowAfter;
    row.set = after.index();
    m_value[node] = value;
    if (!std::exchange(m_hasMoved[node], 1))
        worker.movedNodes.append(node);

    const Links& successors = m_successors[node];
    if (!successors.count)
        return;
    AOT_THRESHOLD(uint32_t, fewestMembersToHandOnWhatIsNew, 16);
    bool handsOnAll = valueBefore == ValueSets::nothing || ValueSets::isComplement(value) || ValueSets::isComplement(valueBefore) || (after == ValueSets::nothing ? static_cast<uint32_t>(std::popcount(lowAfter)) : m_sets.size(after)) <= fewestMembersToHandOnWhatIsNew;
    Message message;
    message.low = handsOnAll ? lowAfter : lowAfter & ~lowBefore;
    message.set = after.index();
    if (!handsOnAll && after != ValueSets::nothing) {
        ValueSetID whatIsNew = maker.intersect(after, ValueSets::complement(valueBefore));
        message.set = (message.low && whatIsNew == worker.setOfLowMembers(message.low) ? ValueSets::nothing : whatIsNew).index();
    }
    worker.edgesLookedAt += successors.count;
    forEachOther(successors, [&](uint32_t successor) {
        Gate target = m_gates[successor];
        if (target.flags & IgnoresArrivals)
            return;
        if ((target.flags & IsCoveredNode) && didCoveredNodeHoldAllOf(successor, message.low, message.set))
            return;
        if ((target.flags & IsRetired) && !retiredNodeTakesAnyOf(successor, message.low, message.set))
            return;
        if (!handsOnAll && target.mask) {
            const Mask& mask = m_masks[target.mask];
            if (mask.kind == ValueFlowMaskKind::Plain && !(message.low & mask.lowMembersAdmitted) && (!message.set || !m_sets.meet(ValueSetID(message.set), mask.admitted)))
                return;
        }
        message.to = successor;
        leave(message);
    });
}

ALWAYS_INLINE void ValueFlowGraph::evaluateAll(Arrivals& arrivals, Worker& worker, NOESCAPE const Invocable<void(const Message&)> auto& leave)
{
    constexpr size_t distance = 8;
    const Vector<uint32_t>& nodes = arrivals.nodes;
    for (size_t index = 0; index < nodes.size(); ++index) {
        if (index + distance < nodes.size()) {
            __builtin_prefetch(&m_gates[nodes[index + distance]]);
            __builtin_prefetch(&m_successors[nodes[index + distance]]);
            __builtin_prefetch(&m_value[nodes[index + distance]], 1);
            __builtin_prefetch(&m_hasMoved[nodes[index + distance]], 1);
        }
        evaluate(nodes[index], arrivals, worker, leave);
    }
    arrivals.nodes.shrink(0);
    arrivals.list.shrink(0);
}

// A step lasts as long as its longest shard from the moment that one begins.
ValueFlowGraph::ShardsInOrder ValueFlowGraph::longestFirst(const std::array<size_t, numberOfShards>& sizes)
{
    ShardsInOrder shards;
    std::iota(shards.begin(), shards.end(), 0);
    std::ranges::stable_sort(shards, std::ranges::greater(), [&](uint8_t shard) {
        return sizes[shard];
    });
    return shards;
}

void ValueFlowGraph::runWave(Team& team)
{
    AOT_THRESHOLD(size_t, fewestMessagesForThreads, 256);
    AOT_THRESHOLD(size_t, mostNodesOfWaveOnOneThread, 512);
    unsigned parity = m_parity;
    m_parity ^= 1;
    size_t numberWithShards = m_numberOfMessagesWithShards.exchange(0, std::memory_order_relaxed);
    Vector<Message>& inOneList = m_messages[parity];

    if (numberWithShards + inOneList.size() < fewestMessagesForThreads || m_loops.width() == 1) {
        if (numberWithShards) {
            for (unsigned source = 0; source < numberOfShards; ++source) {
                for (uint64_t shards = std::exchange(m_shardsWithMessagesOfShard[parity][source], 0); shards; shards &= shards - 1) {
                    Vector<Message>& messages = m_shards[source]->messages[parity][std::countr_zero(shards)];
                    takeArrivals(m_arrivals, messages.span());
                    messages.shrink(0);
                }
            }
        }
        takeArrivals(m_arrivals, inOneList.span());
        inOneList.shrink(0);
        Vector<Message>& next = m_messages[parity ^ 1];
        evaluateAll(m_arrivals, *m_workers[0], [&](const Message& message) {
            if (m_arrivals.nodes.size() < mostNodesOfWaveOnOneThread)
                takeArrival(m_arrivals, message);
            else
                next.append(message);
        });
        return;
    }

    m_hasLeftMessagesWithShards = true;
    AOT_THRESHOLD(size_t, fewestMessagesToCountByShard, 4096);
    std::array<size_t, numberOfShards> numberArriving { };
    for (unsigned source = 0; numberWithShards >= fewestMessagesToCountByShard && source < numberOfShards; ++source) {
        for (uint64_t shards = m_shardsWithMessagesOfShard[parity][source]; shards; shards &= shards - 1)
            numberArriving[std::countr_zero(shards)] += m_shards[source]->messages[parity][std::countr_zero(shards)].size();
    }
    ShardsInOrder shardsInOrder = longestFirst(numberArriving);
    team.run(numberOfShards, [&](unsigned position, unsigned worker) {
        unsigned number = shardsInOrder[position];
        Shard& shard = *m_shards[number];
        if (numberWithShards) {
            // The list that one shard leaves for another is short, and another thread has written it: what is fetched ahead within one list comes too late.
            uint64_t sources = 0;
            for (unsigned source = 0; source < numberOfShards; ++source) {
                if (m_shardsWithMessagesOfShard[parity][source] >> number & 1) {
                    sources |= 1ull << source;
                    __builtin_prefetch(&m_shards[source]->messages[parity][number]);
                }
            }
            auto forEachList = [&](NOESCAPE const Invocable<void(Vector<Message>&)> auto& functor) {
                for (uint64_t rest = sources; rest; rest &= rest - 1)
                    functor(m_shards[std::countr_zero(rest)]->messages[parity][number]);
            };
            forEachList([&](Vector<Message>& messages) {
                constexpr size_t messagesPerLine = 64 / sizeof(Message);
                for (size_t index = 0; index < messages.size(); index += messagesPerLine)
                    __builtin_prefetch(&messages[index]);
            });
            forEachList([&](Vector<Message>& messages) {
                for (const Message& message : messages)
                    __builtin_prefetch(&m_rows[message.to], 1);
            });
            forEachList([&](Vector<Message>& messages) {
                for (const Message& message : messages)
                    takeArrival(shard.arrivals, message);
                messages.shrink(0);
            });
        }
        for (const Message& message : inOneList) {
            if (shardOf(message.to) == number)
                takeArrival(shard.arrivals, message);
        }
        size_t numberOfMessages = 0;
        uint64_t shardsWithMessages = 0;
        std::array<Vector<Message>, numberOfShards>& next = shard.messages[parity ^ 1];
        evaluateAll(shard.arrivals, *m_workers[worker], [&](const Message& message) {
            if (shardOf(message.to) == number) {
                takeArrival(shard.arrivals, message);
                return;
            }
            next[shardOf(message.to)].append(message);
            shardsWithMessages |= 1ull << shardOf(message.to);
            ++numberOfMessages;
        });
        m_shardsWithMessagesOfShard[parity ^ 1][number] |= shardsWithMessages;
        m_numberOfMessagesWithShards.fetch_add(numberOfMessages, std::memory_order_relaxed);
    });
    m_shardsWithMessagesOfShard[parity].fill(0);
    inOneList.shrink(0);
}

void ValueFlowGraph::giveBackLargeBuffers()
{
    auto giveBack = [](auto& vector, size_t mostElementsKept) {
        if (vector.capacity() > mostElementsKept)
            vector.clear();
    };
    AOT_THRESHOLD(size_t, mostKeptInOneList, 1 << 16);
    giveBack(m_found, mostKeptInOneList);
    giveBack(m_arrivals.nodes, mostKeptInOneList);
    giveBack(m_arrivals.list, mostKeptInOneList);
    for (Vector<Message>& messages : m_messages)
        giveBack(messages, mostKeptInOneList);
    for (Vector<uint32_t>& found : m_foundInShare)
        giveBack(found, 1 << 12);
    for (const std::unique_ptr<Worker>& worker : m_workers)
        giveBack(worker->movedNodes, mostKeptInOneList);
    if (!std::exchange(m_hasLeftMessagesWithShards, false))
        return;
    for (Vector<uint32_t>& indices : m_indicesBySource)
        giveBack(indices, 1 << 8);
    for (Vector<ValueFlowEdge>& edges : m_edgesByTarget)
        giveBack(edges, 1 << 8);
    for (Vector<ValueFlowEdge>& edges : m_edgesOnlyToLink)
        giveBack(edges, 1 << 8);
    for (const std::unique_ptr<Shard>& shard : m_shards) {
        giveBack(shard->arrivals.nodes, 1 << 12);
        giveBack(shard->arrivals.list, 1 << 12);
        for (auto& messagesOfWave : shard->messages) {
            for (Vector<Message>& messages : messagesOfWave)
                giveBack(messages, 1 << 8);
        }
    }
}

void ValueFlowGraph::solve(Vector<ValueFlowNodeID>& moved, RetiredNodes retiredNodes)
{
    moved.shrink(0);
    m_hasSolved = true;
    connectPredecessorsOfNodesJustRetired();
    for (const Covering& covering : m_coverings)
        m_gates[covering.covered].flags |= IsCoveredNode;
    MonotonicTime start = MonotonicTime::now();
    Ref<Team> team = *m_team;

    connectPredecessorsOfRetiredNodes(team.get());
    m_edgesOfSolve.shrink(0);
    std::swap(m_edgesOfSolve, m_edgesNotInLists);
    noteWhatCoveredNodesHold();
    takeEdgesIntoLists(team.get(), m_edgesOfSolve.span());

    MonotonicTime afterRows = MonotonicTime::now();
    m_counters.rows += afterRows - start;

    auto runWaves = [&] {
        for (uint32_t node : m_nodesWoken) {
            if (m_gates[node].flags & IgnoresArrivals)
                continue;
            forEachOther(m_predecessors[node], [&](uint32_t predecessor) {
                const Row& row = m_rows[predecessor];
                if (row.low || row.set)
                    m_messages[m_parity].append(Message { node, row.set, row.low });
            });
        }
        while (m_numberOfMessagesWithShards.load(std::memory_order_relaxed) || !m_messages[m_parity].isEmpty()) {
            ++m_counters.waves;
            noteWhatCoveredNodesHold();
            runWave(team.get());
        }
    };
    AOT_THRESHOLD(size_t, fewestEdgesToMarkBesideWaves, 4096);
    findEdgesThatLengthenFlowIntoCoveredNodes(team.get(), m_edgesOfSolve.span());
    if (m_loops.width() > 1 && m_edgesOfSolve.size() >= fewestEdgesToMarkBesideWaves) {
        runTasksInParallel(m_loops, {
            runWaves,
            [&] {
                markNodesFlowingIntoCoveredNodes(m_edgesOfSolve.span());
            },
        });
    } else {
        markNodesFlowingIntoCoveredNodes(m_edgesOfSolve.span());
        runWaves();
    }
    m_nodesWoken.shrink(0);
    size_t numberMoved = 0;
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        m_counters.evaluations += std::exchange(worker->evaluations, 0);
        m_counters.edgesLookedAt += std::exchange(worker->edgesLookedAt, 0);
        numberMoved += worker->movedNodes.size();
    }

    if (numberMoved * 1024 < m_numberOfNodes) {
        for (const std::unique_ptr<Worker>& worker : m_workers) {
            for (uint32_t node : worker->movedNodes) {
                m_hasMoved[node] = 0;
                moved.append(ValueFlowNodeID(node));
            }
        }
        AOT::sort(moved);
    } else {
        constexpr size_t nodesPerRange = 1 << 16;
        size_t numberOfRanges = (m_numberOfNodes + nodesPerRange - 1) / nodesPerRange;
        if (m_foundInShare.size() < numberOfRanges)
            m_foundInShare.grow(numberOfRanges);
        team->run(numberOfRanges, [&](unsigned range, unsigned) {
            Vector<uint32_t>& found = m_foundInShare[range];
            found.shrink(0);
            size_t end = std::min<size_t>(m_numberOfNodes, (range + 1) * nodesPerRange);
            for (size_t first = range * nodesPerRange; first < end; first += sizeof(uint64_t)) {
                uint64_t eightNodes;
                memcpy(&eightNodes, &m_hasMoved[first], sizeof(eightNodes));
                if (!eightNodes)
                    continue;
                for (size_t node = first; node < first + sizeof(uint64_t); ++node) {
                    if (std::exchange(m_hasMoved[node], 0))
                        found.append(static_cast<uint32_t>(node));
                }
            }
        });
        Vector<size_t> firstOfRange(numberOfRanges);
        size_t number = 0;
        for (size_t range = 0; range < numberOfRanges; ++range)
            firstOfRange[range] = std::exchange(number, number + m_foundInShare[range].size());
        moved.grow(number);
        team->run(numberOfRanges, [&](unsigned range, unsigned) {
            size_t position = firstOfRange[range];
            for (uint32_t node : m_foundInShare[range])
                moved[position++] = ValueFlowNodeID(node);
        });
    }
    for (const std::unique_ptr<Worker>& worker : m_workers)
        worker->movedNodes.shrink(0);
    if (retiredNodes == RetiredNodes::AreFoundAndConnectedInSolve)
        retireNodesAmong(moved.span());
    m_counters.waving += MonotonicTime::now() - afterRows;
}

void ValueFlowGraph::removeFromEveryNode(std::initializer_list<ValueFlowObjectID> lowMembers)
{
    uint64_t bits = 0;
    for (ValueFlowObjectID lowMember : lowMembers) {
        RELEASE_ASSERT(lowMember.index() < numberOfLowMembers);
        bits |= 1ull << lowMember.index();
    }
    Vector<ValueFlowObjectID, 2> members(lowMembers);
    ValueSetID others = ValueSets::complement(m_makers[0]->of(members.span()));
    constexpr uint32_t nodesPerShare = 1 << 14;
    m_team->run((m_numberOfNodes + nodesPerShare - 1) / nodesPerShare, [&](unsigned share, unsigned worker) {
        ValueSetMaker& maker = *m_makers[worker];
        for (uint32_t node = share * nodesPerShare; node < std::min<uint64_t>(m_numberOfNodes, static_cast<uint64_t>(share + 1) * nodesPerShare); ++node) {
            Row& row = m_rows[node];
            if (!(row.low & bits))
                continue;
            row.low &= ~bits;
            if (row.set)
                row.set = maker.intersect(ValueSetID(row.set), others).index();
            m_value[node] = maker.intersect(m_value[node], others);
        }
    });
}

void ValueFlowGraph::releaseRows()
{
    m_rows.release(*m_team);
}

void ValueFlowGraph::releaseSolvingState(Predecessors predecessors)
{
    m_rows.release(*m_team);
    m_successors.release(*m_team);
    for (const std::unique_ptr<Shard>& shard : m_shards)
        shard->blocksOfSuccessors.release();
    m_keepsPredecessors = predecessors == Predecessors::AreKept;
    if (m_keepsPredecessors)
        layPredecessorsInRows();
    m_predecessors.release(*m_team);

    m_hasReleasedSolvingState = true;
    auto giveUp = [](auto&... lists) {
        ((lists = { }), ...);
    };
    m_hasMoved.release();
    giveUp(m_isNewEdge, m_edgesNotInLists, m_edgesOfSolve, m_indicesBySource, m_edgesByTarget, m_straightEdgesOfShare, m_straightEdges, m_messages, m_arrivals, m_nodesWoken, m_foundInShare, m_found, m_edgesThatLengthenFlow);
    for (const std::unique_ptr<Shard>& shard : m_shards) {
        shard->blocksOfPredecessors.release();
        giveUp(shard->edgesOfNodesWithManySuccessors, shard->edgesOutOfConstantsIntoNodesWithManyPredecessors, shard->listedEdgesOutOfConstants, shard->arrivals, shard->messages, shard->nodesWithRecentSuccessors, shard->nodesWithRecentPredecessors);
    }
    m_workers.clear();
}

void ValueFlowGraph::layPredecessorsInRows()
{
    constexpr uint32_t nodesPerShare = 1 << 16;
    unsigned numberOfShares = (m_numberOfNodes + nodesPerShare - 1) / nodesPerShare;
    auto forEachNodeOfShare = [&](unsigned share, const auto& functor) {
        for (uint32_t node = share * nodesPerShare; node < std::min<uint64_t>(m_numberOfNodes, static_cast<uint64_t>(share + 1) * nodesPerShare); ++node)
            functor(node);
    };
    Vector<uint32_t> predecessorsBeforeShare(numberOfShares);
    m_team->run(numberOfShares, [&](unsigned share, unsigned) {
        uint32_t numberInShare = 0;
        forEachNodeOfShare(share, [&](uint32_t node) {
            numberInShare += m_predecessors[node].count;
        });
        predecessorsBeforeShare[share] = numberInShare;
    });
    uint32_t numberOfPredecessors = 0;
    for (uint32_t& before : predecessorsBeforeShare)
        before = std::exchange(numberOfPredecessors, numberOfPredecessors + before);

    m_firstPredecessorInRows.grow(0, static_cast<size_t>(m_numberOfNodes) + 1);
    m_firstPredecessorInRows[m_numberOfNodes] = numberOfPredecessors;
    m_predecessorsInRows.grow(0, std::max<size_t>(numberOfPredecessors, 1));
    m_team->run(numberOfShares, [&](unsigned share, unsigned) {
        uint32_t first = predecessorsBeforeShare[share];
        forEachNodeOfShare(share, [&](uint32_t node) {
            m_firstPredecessorInRows[node] = first;
            uint32_t position = first;
            forEachOther(m_predecessors[node], [&](uint32_t predecessor) {
                m_predecessorsInRows[position++] = predecessor;
            });
            first += m_predecessors[node].count;
        });
    });
}

void ValueFlowGraph::addBytesTo(ValueFlowBytesByTable& bytes) const
{
    RELEASE_ASSERT(!m_hasReleasedSolvingState);
    size_t numberOfNodes = m_numberOfNodes;
    bytes.add("graph: gates of nodes"_s, numberOfNodes * sizeof(Gate));
    bytes.add("graph: rows of nodes"_s, numberOfNodes * sizeof(Row));
    bytes.add("graph: successors of nodes, the first two"_s, numberOfNodes * sizeof(Links));
    bytes.add("graph: predecessors of nodes, the first two"_s, numberOfNodes * sizeof(Links));
    bytes.add("graph: values of nodes"_s, numberOfNodes * sizeof(ValueSetID));
    bytes.add("graph: bits of nodes"_s, (3 + 2 * m_coverings.size()) * (m_capacity / 8));
    bytes.addList("graph: masks"_s, m_masks);
    size_t wordsOfSuccessors = 0;
    size_t wordsOfPredecessors = 0;
    for (uint32_t node = 0; node < m_numberOfNodes; ++node) {
        wordsOfSuccessors += m_successors[node].count > 2 ? m_successors[node].count : 0;
        wordsOfPredecessors += m_predecessors[node].count > 2 ? m_predecessors[node].count : 0;
    }
    bytes.add("graph: successors of nodes, blocks"_s, wordsOfSuccessors * sizeof(uint32_t), 0);
    bytes.add("graph: predecessors of nodes, blocks"_s, wordsOfPredecessors * sizeof(uint32_t), 0);
    for (const std::unique_ptr<Worker>& worker : m_workers)
        bytes.addMap("graph: sets of low members"_s, worker->setsOfLowMembers);
    for (const std::unique_ptr<Shard>& shard : m_shards) {
        bytes.add("graph: successors of nodes, blocks"_s, 0, shard->blocksOfSuccessors.memoryInBytes());
        bytes.add("graph: predecessors of nodes, blocks"_s, 0, shard->blocksOfPredecessors.memoryInBytes());
        bytes.add("graph: edges of nodes with many successors"_s, shard->edgesOfNodesWithManySuccessors.bytesInUse(), shard->edgesOfNodesWithManySuccessors.memoryInBytes());
        bytes.add("graph: edges out of constants into nodes with many predecessors"_s, shard->edgesOutOfConstantsIntoNodesWithManyPredecessors.bytesInUse(), shard->edgesOutOfConstantsIntoNodesWithManyPredecessors.memoryInBytes());
        bytes.addList("graph: listed edges out of constants"_s, shard->listedEdgesOutOfConstants);
    }
    forEachListOfOneRound(*this, [&](ASCIILiteral table, const auto& list) {
        bytes.addList(table, list);
    });
}

void ValueFlowGraph::forEachListOfOneRound(auto& graph, NOESCAPE const auto& functor)
{
    auto visit = [&](ASCIILiteral table, auto&... lists) {
        (functor(table, lists), ...);
    };
    auto visitEach = [&](ASCIILiteral table, auto& lists) {
        for (auto& list : lists)
            functor(table, list);
    };
    constexpr ASCIILiteral edgesOfRound = "graph: edges of one round"_s;
    constexpr ASCIILiteral waves = "graph: messages and arrivals of waves"_s;
    visit(edgesOfRound, graph.m_isNewEdge, graph.m_edgesNotInLists, graph.m_edgesOfSolve, graph.m_straightEdges);
    visitEach(edgesOfRound, graph.m_indicesBySource);
    visitEach(edgesOfRound, graph.m_edgesByTarget);
    visitEach(edgesOfRound, graph.m_edgesOnlyToLink);
    visitEach(edgesOfRound, graph.m_straightEdgesOfShare);
    visitEach(waves, graph.m_messages);
    visit(waves, graph.m_arrivals.nodes, graph.m_arrivals.list, graph.m_nodesWoken, graph.m_found, graph.m_edgesThatLengthenFlow);
    visitEach(waves, graph.m_foundInShare);
    for (auto& covering : graph.m_coverings)
        visit(waves, covering.nodesJustRetired, covering.nodesToLookAt);
    for (auto& worker : graph.m_workers)
        visit(waves, worker->movedNodes);
    for (auto& shard : graph.m_shards) {
        visit(waves, shard->arrivals.nodes, shard->arrivals.list, shard->nodesWithRecentSuccessors, shard->nodesWithRecentPredecessors, shard->placeholdersStopped);
        for (auto& messagesOfWave : shard->messages)
            visitEach(waves, messagesOfWave);
    }
}

void ValueFlowGraph::giveBackMostRoomOfListsOfOneRound()
{
    forEachListOfOneRound(*this, [](ASCIILiteral, auto& list) {
        giveBackMostRoomOf(list);
    });
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
