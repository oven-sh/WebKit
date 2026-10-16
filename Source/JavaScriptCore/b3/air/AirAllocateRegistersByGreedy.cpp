/*
 * Copyright (C) 2024-2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AirAllocateRegistersByGreedy.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirCode.h"
#include "AirDominators.h"
#include "AirEliminateDeadCode.h"
#include "AirEmitShuffle.h"
#include "AirEnsureDedicatedLoopEntryExitBlocks.h"
#include "AirFixSpillsAfterTerminals.h"
#include "AirFormTable.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLiveness.h"
#include "AirNaturalLoops.h"
#include "AirPhaseInsertionSet.h"
#include "AirPhaseScope.h"
#include "AirRegLiveness.h"
#include "AirRegisterAllocatorStats.h"
#include "AirTmpMap.h"
#include "AirTmpWidthInlines.h"
#include "AirUseCounts.h"
#include <wtf/GenerationalSet.h>
#include <wtf/GraphOrdering.h>
#include <wtf/IntervalSet.h>
#include <wtf/IterationStatus.h>
#include <wtf/ListDump.h>
#include <wtf/PriorityQueue.h>
#include <wtf/Range.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

namespace Greedy {

struct UseDefCostTag { };
struct SpillCostTag { };

template<typename Tag>
class Cost {
public:
    Cost() = default;
    explicit constexpr Cost(float value) : m_value(value) { }

    float value() const { return m_value; }

    Cost& operator+=(Cost other) { m_value += other.m_value; return *this; }
    Cost& operator-=(Cost other) { m_value -= other.m_value; return *this; }
    Cost& operator*=(float scalar) { m_value *= scalar; return *this; }
    Cost operator*(float scalar) const { return Cost(m_value * scalar); }
    Cost operator/(float scalar) const { return Cost(m_value / scalar); }

    // Subtract other from this cost, clamping to zero. Skips non-finite values
    // since an inf cost has lost precision and inf - inf = NaN.
    void subtractSaturating(Cost other)
    {
        if (std::isfinite(m_value)) {
            m_value -= other.m_value;
            m_value = std::max(m_value, 0.0f);
        }
    }

#if COMPILER(CLANG) && OS(WINDOWS)
    // clang-cl emits MSVC STL's consteval _Literal_zero ctor as non-immediate
    // when <=> is defaulted inside a class template, referencing the
    // _Literal_zero_is_expected poison symbol at link time.
    friend constexpr bool operator==(const Cost& a, const Cost& b) { return a.m_value == b.m_value; }
    friend constexpr bool operator<(const Cost& a, const Cost& b) { return a.m_value < b.m_value; }
    friend constexpr bool operator>(const Cost& a, const Cost& b) { return a.m_value > b.m_value; }
    friend constexpr bool operator<=(const Cost& a, const Cost& b) { return a.m_value <= b.m_value; }
    friend constexpr bool operator>=(const Cost& a, const Cost& b) { return a.m_value >= b.m_value; }
#else
    friend auto operator<=>(const Cost&, const Cost&) = default;
#endif

private:
    float m_value { 0.0f };
};

using UseDefCost = Cost<UseDefCostTag>;
using SpillCost = Cost<SpillCostTag>;

static constexpr SpillCost unspillableCost { std::numeric_limits<float>::infinity() };
static constexpr SpillCost fastTmpSpillCost { std::numeric_limits<float>::max() };
static constexpr SpillCost maxSpillableSpillCost { std::numeric_limits<float>::max() };
static_assert(unspillableCost > fastTmpSpillCost);
static_assert(unspillableCost > maxSpillableSpillCost);

static constexpr unsigned maxLoopSplitDepth = 32;
static constexpr UseDefCost zeroCostEpsilon { 1e-6f };

// Phase constants used for the PhaseInsertionSet. Ensures that the fixup and spill/fill instructions
// inserted in a particular gap end up in the correct order.
// "MoveTo" = value flows toward the original tmp (or its spill slot).
// "MoveFrom" = value flows away from the original tmp (or its spill slot).
enum InsertionPhase : unsigned {
    SpillMoveTo,
    AroundLoopExitFixup,
    ClobberMoveTo,
    IntraBlockMoveTo,
    IntraBlockMoveFrom,
    ClobberMoveFrom,
    AroundLoopEntryFixup,
    SpillMoveFrom,
};

static bool LEGACY_NODELETE verbose() { return Options::airGreedyRegAllocVerbose(); }

// Terminology / core data-structures:
//
// Point: a position within the IR stream. There are four points associated with each
// instruction, see PointOffsets. The early point for an instruction occurs immediately
// before the instruction and the late occurs immediately following. Early use/defs
// for that instruction are modeled as occurring at the early point whereas late use/defs
// are modeled as occurring at the late point. The late point for an instruction and
// early point for the subsequent instruction are distinct in order to avoid false
// conflicts, e.g. when a late use is followed by an early def. Two "gap" points, Pre & Post,
// exist for each instruction to allow distinguishing e.g. a Tmp being live into a block from
// a Tmp becoming live due to an early def by the first instruction of the block
// (and similarly for block exits and late use).
//
// Interval: contiguous set of points, represented by a half open interval [begin, end).
//
// LiveRange: a set of intervals, usually used to model the liveness of a particular Tmp.
// In this implementation, Intervals of a LiveRange must be non-overlapping and
// non-contigous (if contigous, they should have been merged), and stored in sorted order.
//
// RegisterRange: a set of intervals, usually used to model assignments to a particular
// register at each Point. Each Interval of a RegisterRange is associated with a single
// Tmp, which may be a register Tmp (to represent fixed register lifetime and clobbers) or
// a Tmp that is currently assigned to this register. This structure models which Tmp occupies
// the register at each Point. Tmps and their associated Intervals can be assigned and
// evicted to/from the RegisterRange as the register allocation algorithm progresses.
//
// AffinityGroup: a set of Tmps connected by move instructions where simultaneously-live
// members are guaranteed to hold the same value (due to the connecting moves), so their
// live ranges may overlap and they can still be coalesced into a single representative Tmp.
//
// LivenessMap: for an AffinityGroup, tracks which member Tmps are live at each point.
// Used to accelerate conflict detection during coalescing.
//
// Algorithm:
//
// 1. Initialization:
//   a. Define where Points are located in the IR.
//   b. Run liveness analysis and build a LiveRange for each Tmp (including fixed registers).
//   c. Run analysis to determine the cost to spill each Tmp.
//   d. Coalesce Tmps by building affinity groups from move instructions and replacing
//      each group's members with a single representative Tmp.
// 2. Register allocation:
//   a. Process each Tmp in order of priority. Priority is mostly related to the "stage" of the
//      Tmp, whether a Tmp has a preferred register, and the size of a Tmp's LiveRange. The idea
//      is to fit the most constrained Tmps first, and let the smaller and less constrained ranges
//      fit into the remaining gaps.
//   b. Each Tmp is driven through a state machine (see Stage enum), where the further a Tmp
//      progresses, the more aggressive we try to fit it somewhere.
//     - First, simply try to find space where the LiveRange can fit in a RegisterRange.
//     - If that's not successful, we may evict LiveRanges in favor of a LiveRange with higher
//       spill cost. The evicted LiveRanges will requeued for further processing.
//     - If that's not successful, we may split LiveRanges, e.g. around register clobbers or
//       at intra-block boundaries, producing new Tmps/LiveRanges, some of which may be
//       assignable to registers.
//     - Finally, if all else fails, the Tmp is spilled and new Tmps for the spill/fill fixups
//       are queued for processing.
// 3. Finalization: fixup IR code is inserted to handle Tmps' split ranges and spills.
//
// Note that stack slots are allocated during a subsequent compiler phase.

using Point = uint32_t;
using Interval = WTF::Range<Point>;

enum PointOffsets {
    Pre = 0,
    Early = 1,
    // Inst lives between the Early and Late points.
    Late = 2,
    Post = 3,
    PointsPerInst
};

// Quickly filters out short ranges from live range splitting consideration.
static constexpr size_t splitMinRangeSize = 4 * PointOffsets::PointsPerInst;

static constexpr unsigned spillCostSizeBias = 25000 * PointOffsets::PointsPerInst;

class IntervalArena {
    WTF_MAKE_NONCOPYABLE(IntervalArena);
public:
    IntervalArena() = default;

    ~IntervalArena()
    {
        for (Interval* chunk : m_chunks)
            fastFree(chunk);
    }

    Interval* allocate(size_t count)
    {
        if (static_cast<size_t>(m_end - m_next) < count) [[unlikely]]
            addChunk(count);
        Interval* result = m_next;
        m_next += count;
        return result;
    }

    bool tryResizeInPlace(Interval* allocation, size_t oldCount, size_t newCount)
    {
        if (allocation + oldCount != m_next || static_cast<size_t>(m_end - allocation) < newCount)
            return false;
        m_next = allocation + newCount;
        return true;
    }

private:
    NEVER_INLINE void addChunk(size_t count)
    {
        size_t size = std::max(count + count / 2, m_sizeOfNextChunk);
        m_sizeOfNextChunk = size * 2;
        m_next = static_cast<Interval*>(fastMalloc(size * sizeof(Interval)));
        m_end = m_next + size;
        m_chunks.append(m_next);
    }

    Vector<Interval*, 4> m_chunks;
    Interval* m_next { nullptr };
    Interval* m_end { nullptr };
    size_t m_sizeOfNextChunk { 64 };
};

class LiveRange {
public:
    class Intervals {
    public:
        Intervals(const Interval* data, unsigned size)
            : m_data(data)
            , m_size(size)
        {
        }

        bool isEmpty() const { return !m_size; }
        size_t size() const { return m_size; }
        const Interval* begin() const { return m_data; }
        const Interval* end() const { return m_data + m_size; }
        const Interval& first() const { return m_data[0]; }
        const Interval& last() const { return m_data[m_size - 1]; }
        const Interval& operator[](size_t index) const { return m_data[index]; }
        std::span<const Interval> span() const { return { m_data, m_size }; }

    private:
        const Interval* m_data;
        unsigned m_size;
    };

    LiveRange() = default;

    LiveRange(const LiveRange& other)
    {
        RELEASE_ASSERT(!other.m_count);
    }

    LiveRange(LiveRange&& other)
        : m_intervals(std::exchange(other.m_intervals, nullptr))
        , m_count(std::exchange(other.m_count, 0))
        , m_capacity(std::exchange(other.m_capacity, 0))
        , m_size(std::exchange(other.m_size, 0))
    {
    }

    LiveRange& operator=(const LiveRange&) = delete;

    LiveRange& operator=(LiveRange&& other)
    {
        m_intervals = std::exchange(other.m_intervals, nullptr);
        m_count = std::exchange(other.m_count, 0);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_size = std::exchange(other.m_size, 0);
        return *this;
    }

    LiveRange(Interval* intervals, unsigned count, size_t size)
        : m_intervals(intervals)
        , m_count(count)
        , m_capacity(count)
        , m_size(size)
    {
        validate();
    }

    inline void LEGACY_NODELETE validate()
    {
#if ASSERT_ENABLED
        size_t size = 0;
        const Interval* prevInterval = nullptr;
        for (auto& interval : intervals()) {
            ASSERT(interval.begin() < interval.end());
            if (prevInterval)
                ASSERT(prevInterval->end() < interval.begin());
            size += interval.distance();
            prevInterval = &interval;
        }
        ASSERT(size == m_size);
#endif
    }

    static LiveRange unionOf(IntervalArena& arena, const LiveRange& left, const LiveRange& right)
    {
        const Interval* a = left.m_intervals;
        const Interval* endOfA = a + left.m_count;
        const Interval* b = right.m_intervals;
        const Interval* endOfB = b + right.m_count;
        const size_t capacity = left.m_count + right.m_count;
        if (!capacity)
            return { };
        Interval* const first = arena.allocate(capacity);
        Interval* next = first;
        size_t size = 0;
        auto take = [&]() ALWAYS_INLINE_LAMBDA {
            return b == endOfB || (a != endOfA && a->begin() <= b->begin()) ? *a++ : *b++;
        };
        Interval current = take();
        while (a != endOfA || b != endOfB) {
            Interval interval = take();
            if (interval.begin() <= current.end())
                current |= interval;
            else {
                *next++ = current;
                size += current.distance();
                current = interval;
            }
        }
        *next++ = current;
        size += current.distance();
        arena.tryResizeInPlace(first, capacity, next - first);
        return LiveRange(first, next - first, size);
    }

    // interval must come after all the intervals already in this LiveRange.
    void append(IntervalArena& arena, Interval interval)
    {
        ASSERT(interval);
        if (!m_count || m_intervals[m_count - 1].end() < interval.begin()) {
            if (m_count == m_capacity) [[unlikely]]
                grow(arena);
            m_intervals[m_count++] = interval;
        } else {
            ASSERT(m_intervals[m_count - 1].end() == interval.begin());
            m_intervals[m_count - 1] |= interval;
        }
        m_size += interval.distance();
        validate();
    }

    Intervals LEGACY_NODELETE intervals() const
    {
        return { m_intervals, m_count };
    }

    size_t LEGACY_NODELETE size() const
    {
        return m_size;
    }

    bool LEGACY_NODELETE contains(Point point) const
    {
        for (auto& interval : intervals()) {
            if (interval.end() <= point)
                continue;
            return interval.begin() <= point;
        }
        return false;
    }

    bool LEGACY_NODELETE overlaps(const LiveRange& other) const
    {
        if (!m_count || !other.m_count || m_intervals[m_count - 1].end() <= other.m_intervals[0].begin() || other.m_intervals[other.m_count - 1].end() <= m_intervals[0].begin())
            return false;
        auto otherIter = other.intervals().begin();
        auto otherEnd = other.intervals().end();
        for (auto interval : intervals()) {
            while (otherIter != otherEnd && otherIter->end() <= interval.begin())
                ++otherIter; // otherIter was entirely before interval
            if (otherIter == otherEnd)
                return false;
            // Either otherIter overlaps interval or otherIter is entirely after interval.
            if (otherIter->begin() < interval.end())
                return true;
        }
        return false;
    }

    static LiveRange subtract(IntervalArena& arena, const LiveRange& a, const LiveRange& b)
    {
        LiveRange result;
        auto aIter = a.intervals().begin();
        auto bIter = b.intervals().begin();

        if (aIter == a.intervals().end())
            return result;
        Interval interval = *aIter;
        ++aIter;

        while (true) {
            // Skip over intervals in b that come before the current interval of a.
            while (bIter != b.intervals().end() && bIter->end() <= interval.begin())
                ++bIter;

            if (bIter != b.intervals().end() && bIter->overlaps(interval)) {
                // Overlap: Split the interval into 0, 1 or 2 intervals.
                if (interval.begin() < bIter->begin())
                    result.append(arena, { interval.begin(), bIter->begin() });
                if (bIter->end() < interval.end()) {
                    // Process remaining portion of the interval.
                    interval = { bIter->end(), interval.end() };
                    continue;
                }
            } else {
                // No overlap: include entire interval in result.
                result.append(arena, interval);
            }
            // Finished processing interval of a; move on to the next.
            if (aIter == a.intervals().end())
                break;
            interval = *aIter;
            ++aIter;
        }
        result.validate();
        return result;
    }

    void dump(PrintStream& out) const
    {
        WTF::CommaPrinter comma;
        out.print("{ ");
        for (auto& interval : intervals())
            out.print(comma, interval);
        out.print(" }[", m_size, "]");
    }

private:
    NEVER_INLINE void grow(IntervalArena& arena)
    {
        unsigned newCapacity = m_capacity ? m_capacity * 2 : 1;
        if (!m_capacity || !arena.tryResizeInPlace(m_intervals, m_capacity, newCapacity)) {
            Interval* newIntervals = arena.allocate(newCapacity);
            for (unsigned i = 0; i < m_count; ++i)
                newIntervals[i] = m_intervals[i];
            m_intervals = newIntervals;
        }
        m_capacity = newCapacity;
    }

    Interval* m_intervals { nullptr };
    unsigned m_count { 0 };
    unsigned m_capacity { 0 };
    size_t m_size { 0 }; // Sum of the distances over m_intervals
};

enum class Stage : uint8_t {
    New,
    Unspillable,
    TryAllocate,
    TrySplit,
    Spill,
    MaxInQueue = Spill,
    Assigned,
    Coalesced,
    Spilled,
    Replaced,
};

class TmpPriority {
private:
    // Priority is encoded into a 64-bit unsigned integer for fast comparison.
    static constexpr size_t stageBits = 3;
    static_assert(static_cast<uint64_t>(Stage::MaxInQueue) < (1ULL << stageBits));
    static constexpr size_t stageShift = 64 - stageBits;

    static constexpr size_t maybeCoalescableBits = 1;
    static constexpr size_t maybeCoalescableShift = stageShift - maybeCoalescableBits;

    static constexpr size_t isGlobalBits = 1;
    static constexpr size_t isGlobalShift = maybeCoalescableShift - isGlobalBits;

    static constexpr size_t rangeSizeBits = 39;
    static constexpr size_t rangeSizeShift = isGlobalShift - rangeSizeBits;

    static constexpr size_t tmpIndexBits = 20;
    static constexpr size_t tmpIndexShift = rangeSizeShift - tmpIndexBits;
    static_assert(!tmpIndexShift);

    inline void LEGACY_NODELETE packPriority(uint64_t val, size_t numBits, size_t shift, bool reverse)
    {
        const uint64_t mask = (1ull << numBits) - 1;
        val &= mask;
        if (reverse)
            val = mask - val;
        m_priority |= val << shift;
    }

public:
    TmpPriority(Tmp tmp, Stage stage, size_t rangeSizeOrStart, bool maybeCoalescable, bool isGlobal)
        : m_tmp(tmp)
    {
        ASSERT(!tmp.isReg());
        ASSERT(stage <= Stage::MaxInQueue);
        // Earlier stages are higher priority.
        packPriority(static_cast<uint64_t>(stage), stageBits, stageShift, true);
        packPriority(maybeCoalescable, maybeCoalescableBits, maybeCoalescableShift, false);
        packPriority(isGlobal, isGlobalBits, isGlobalShift, false);
        // If range is global, then rangeSizeOrStart is size, and larger ranges are higher priority.
        // If range is local, then rangeSizeOrStart is start, and earlier ranges are higher priority.
        packPriority(rangeSizeOrStart, rangeSizeBits, rangeSizeShift, !isGlobal);
        // Make a strict total order for determinism.
        packPriority(tmp.tmpIndex(), tmpIndexBits, tmpIndexShift, true);
    }

    Tmp LEGACY_NODELETE tmp() { return m_tmp; }

    Stage LEGACY_NODELETE stage() const
    {
        const uint64_t mask = (1 << stageBits) - 1;
        uint64_t stageReversed = m_priority >> stageShift;
        stageReversed &= mask;
        return static_cast<Stage>(mask - stageReversed);
    }

    void dump(PrintStream& out) const
    {
        out.print("<", m_tmp, ", ", WTF::RawHex(m_priority), ">");
    }

    // The packed priority is built so that a numerically greater value is more urgent, which is the
    // order PriorityQueue serves.
    friend bool LEGACY_NODELETE operator<(const TmpPriority& left, const TmpPriority& right)
    {
        return left.m_priority < right.m_priority;
    }

private:
    Tmp m_tmp;
    uint64_t m_priority { 0 };
};

class RegisterRange {
public:
    // Diminishing returns on perf after 2 cache lines, but 3 is more space efficient than 2.
    // InnerNodes store arrays of Range<Point> & uintptr_t (16 bytes per order)
    // LeafNodes store arrays of Range<Point> & Tmp (12 bytes per order)
    // Both are multiples of 192, which is 3 cache lines.
    static constexpr unsigned cacheLinesPerNode = 3;
    using AllocatedIntervalSet = IntervalSet<Point, Tmp, cacheLinesPerNode>;

    RegisterRange() = default;

    struct AllocatedInterval {
        Interval interval;
        Tmp tmp;

        AllocatedInterval(const std::pair<Interval, Tmp>& pair)
            : interval(pair.first)
            , tmp(pair.second)
        { }

        bool LEGACY_NODELETE operator<(const AllocatedInterval& other) const
        {
            return this->interval.end() < other.interval.end();
        }

        void dump(PrintStream& out) const
        {
            out.print("{ ", tmp, " ", interval, " }");
        }
    };

    void add(Tmp tmp, LiveRange& range)
    {
        ASSERT(!hasConflict(range, Width64)); // Can't add overlapping LiveRanges

        // The first range can build the tree directly from its intervals in one pass.
        if (m_allocations.isEmpty()) {
            m_allocations = AllocatedIntervalSet(range.intervals().span(), tmp);
            return;
        }

        for (auto& interval : range.intervals()) {
            ASSERT(interval != Interval()); // Strict ordering requires no empty intervals.
            m_allocations.insert(interval, tmp);
        }
    }

    void addClobberHigh64(Reg reg, Point point)
    {
        ASSERT(reg.isFPR());
        m_allocationsHigh64.insert(Interval(point), Tmp(reg));
    }

    void evict(LiveRange& range)
    {
        for (auto& interval : range.intervals())
            m_allocations.erase(interval);
    }

    bool LEGACY_NODELETE hasConflict(LiveRange& range, Width width)
    {
        for (auto interval : range.intervals()) {
            if (m_allocations.hasOverlap(interval))
                return true;
        }
        if (width <= Width64)
            return false;
        for (auto interval : range.intervals()) {
            if (m_allocationsHigh64.hasOverlap(interval))
                return true;
        }
        return false;
    }

    // func is called with each (Tmp, Interval) pair (i.e. AllocatedInterval) of this
    // RegisterRange that overlaps with the given LiveRange 'range'.
    //
    // func is allowed to modify this RegisterRange, e.g. by calling evict().
    // func must not modify 'range' for the duration of this forEachConflict invocation.
    void forEachConflict(const LiveRange& range, Width width, NOESCAPE const Invocable<IterationStatus(AllocatedInterval&)> auto& func)
    {
        auto status = forEachConflictImpl(m_allocations, range, func);
        if (width > Width64) [[unlikely]] {
            if (status == IterationStatus::Continue)
                forEachConflictImpl(m_allocationsHigh64, range, func);
        }
    }

    std::optional<AllocatedInterval> firstConflict(Interval interval)
    {
        auto intervalAndTmp = m_allocations.find(interval);
        if (!intervalAndTmp)
            return std::nullopt;
        return AllocatedInterval { *intervalAndTmp };
    }

    void forEachConflictInHigh64(const LiveRange& range, NOESCAPE const Invocable<IterationStatus(AllocatedInterval&)> auto& func)
    {
        forEachConflictImpl(m_allocationsHigh64, range, func);
    }

    bool LEGACY_NODELETE isEmpty() const
    {
        return m_allocations.isEmpty() && m_allocationsHigh64.isEmpty();
    }

    void dump(PrintStream& out) const
    {
        auto dumpSet = [&out](const AllocatedIntervalSet& allocationSet) {
            CommaPrinter comma;
            out.print("[");
            for (auto alloc : allocationSet) {
                out.print(comma);
                out.print(AllocatedInterval(alloc));
            }
            out.print("]");
        };

        dumpSet(m_allocations);
        if (!m_allocationsHigh64.isEmpty()) {
            out.print(", ↑");
            dumpSet(m_allocationsHigh64);
        }
    }

private:
    static IterationStatus forEachConflictImpl(AllocatedIntervalSet& allocatedSet, const LiveRange& range, NOESCAPE const Invocable<IterationStatus(AllocatedInterval&)> auto& func)
    {
        for (auto interval : range.intervals()) {
            while (true) {
                auto intervalAndTmp = allocatedSet.find(interval);
                if (!intervalAndTmp)
                    break;
                AllocatedInterval conflict = { *intervalAndTmp };
                if (func(conflict) == IterationStatus::Done)
                    return IterationStatus::Done;
                if (interval.end() <= conflict.interval.end())
                    break; // There can't exist other conflicts with interval
                // Search for remaining conflicts with 'interval'
                interval = { conflict.interval.end(), interval.end() };
            }
        }
        return IterationStatus::Continue;
    }

    AllocatedIntervalSet m_allocations;
    AllocatedIntervalSet m_allocationsHigh64; // Tracks clobbers to vector registers that preserve lower 64-bits
};

// Per-Tmp list of coalescable partners and their move costs.
// Mutable until sort() is called, after which it is binary searchable.
class Coalescables {
public:
    void add(Tmp tmp, float moveCost)
    {
        ASSERT(!m_isSorted);
        ASSERT(m_entries.isEmpty() || m_entries.last().tmp.bank() == tmp.bank());
        for (auto& entry : m_entries) {
            if (entry.tmp == tmp) {
                entry.moveCost += moveCost;
                return;
            }
        }
        m_entries.append({ tmp, moveCost });
    }

    void remove(Tmp target)
    {
        ASSERT(!m_isSorted);
        for (size_t i = 0; i < m_entries.size(); i++) {
            if (m_entries[i].tmp == target) {
                m_entries[i] = m_entries.last();
                m_entries.shrink(m_entries.size() - 1);
                return;
            }
        }
        ASSERT_NOT_REACHED();
    }

    void removeAllMatching(NOESCAPE const Invocable<bool(Tmp)> auto& predicate)
    {
        ASSERT(!m_isSorted);
        for (size_t i = 0; i < m_entries.size(); ) {
            if (predicate(m_entries[i].tmp)) {
                m_entries[i] = m_entries.last();
                m_entries.shrink(m_entries.size() - 1);
            } else
                i++;
        }
    }

    template<Bank bank>
    void sort()
    {
        ASSERT(!m_isSorted);
        std::ranges::sort(m_entries, [](const auto& a, const auto& b) {
            return a.tmp.tmpIndex(bank) < b.tmp.tmpIndex(bank);
        });
#if ASSERT_ENABLED
        m_isSorted = true;
#endif
    }

    template<Bank bank>
    bool contains(Tmp target) const
    {
        ASSERT(m_isSorted || m_entries.isEmpty());
        auto it = std::ranges::lower_bound(m_entries, target,
            [](const auto& a, const auto& b) {
                return a.tmpIndex(bank) < b.tmpIndex(bank);
            },
            &Entry::tmp);
        return it != m_entries.end() && it->tmp == target;
    }

    size_t size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    auto begin() const { return m_entries.begin(); }
    auto end() const { return m_entries.end(); }

    void dump(PrintStream& out) const
    {
        out.print(listDump(m_entries));
    }

private:
    struct Entry {
        Tmp tmp;
        float moveCost;

        void dump(PrintStream& out) const
        {
            out.print("(", tmp, ", ", moveCost, ")");
        }
    };

    // Most Tmps that are coalescable at all have just one or two partners, and this list lives in
    // the per-Tmp map, so an out-of-line buffer here is another malloc per Tmp.
    Vector<Entry, 2> m_entries;
#if ASSERT_ENABLED
    bool m_isSorted { false };
#endif
};

// Auxiliary register allocator data per Tmp.
struct TmpData {
    // When an unspillable or fastTmp is coalesced with another tmp, we don't want the spillCost of the
    // group to be unspillableCost or fastTmpCost, so this property is tracked independent of useDefCost.
    enum class Spillability : uint8_t {
        Spillable,
        FastTmp,
        Unspillable,
    };

    void dump(PrintStream& out) const
    {
        out.print("{stage = ", stage, " liveRange = ", liveRange, ", preferredReg = ", preferredReg,
            ", coalescables = ", coalescables, ", useDefCost = ", useDefCost.value(), ", spillability = ", spillability,
            ", assigned = ", assigned, ", spillSlotTableIndex = ", spillSlotTableIndex, ", splitAroundClobbersMetadataIndex = ", splitAroundClobbersMetadataIndex, "}");
    }

    SpillCost spillCost()
    {
        switch (spillability) {
        case Spillability::Unspillable:
            return unspillableCost;
        case Spillability::FastTmp:
            return fastTmpSpillCost;
        case Spillability::Spillable:
            break;
        default:
            ASSERT_NOT_REACHED();
        }
        ASSERT(liveRange.size()); // 0-sized ranges shouldn't be allocated
        // Heuristic that primarily favors not spilling higher use/def frequency-adjusted counts and
        // secondarily favors smaller ranges. The range size is a crude proxy for degree of
        // interference, since the register allocator never directly computes that.
        // The spillCostSizeBias causes the range size penalty to be relatively insignificant
        // for smaller ranges but become significant for very larger ranges.
        float weight = useDefCost.value();
#if USE(BUN_JSC_ADDITIONS)
        if (isX86_64())
            weight = std::max(weight - preferredWeight - secondPreferredWeight, weight / 8);
#endif
        SpillCost cost = SpillCost(weight / (liveRange.size() + spillCostSizeBias));
        return std::min(cost, maxSpillableSpillCost);
    }

    void LEGACY_NODELETE validate()
    {
        ASSERT(!!assigned == (stage == Stage::Assigned));
        ASSERT(liveRange.intervals().isEmpty() == !liveRange.size());
        ASSERT_IMPLIES(stage == Stage::Spilled, spillCost() != unspillableCost);
    }

    LiveRange liveRange;
    Coalescables coalescables;
    UseDefCost useDefCost { 0 };
    UseDefCost cumulativeFixupCost { 0 };
    uint32_t spillSlotTableIndex { 0 };
#if USE(BUN_JSC_ADDITIONS)
    uint32_t allocationFailedAtEpoch { 0 };
    uint32_t evictionFailedAtEpoch { 0 };
    uint64_t clobberedRegs { 0 };
    bool hasClobberedRegs { false };
#endif
    uint32_t splitAroundClobbersMetadataIndex : 31 { 0 };
    uint32_t hasColdUse : 1 { 0 };
    bool paysForCalleeSave { false };
#if USE(BUN_JSC_ADDITIONS)
    bool goesRoundAStretch { false };
    bool isClusterOverBlocks { false };
#endif
    Stage stage { Stage::New };
    Spillability spillability { Spillability::Spillable };
    Reg preferredReg;
    Reg assigned;
    Tmp splitFrom;
#if USE(BUN_JSC_ADDITIONS)
    Reg secondPreferredReg;
    float preferredWeight { 0 };
    float secondPreferredWeight { 0 };

    void prefer(Reg reg, float weight)
    {
        if (!reg)
            return;
        if (!preferredReg || preferredReg == reg) {
            preferredReg = reg;
            preferredWeight += weight;
            return;
        }
        if (!secondPreferredReg || secondPreferredReg == reg) {
            secondPreferredReg = reg;
            secondPreferredWeight += weight;
        }
    }

    float wishFor(Reg reg) const
    {
        if (reg == preferredReg)
            return preferredWeight;
        return reg == secondPreferredReg ? secondPreferredWeight : 0;
    }

    Tmp neighbours[2];

    void addNeighbour(Tmp tmp)
    {
        if (neighbours[0] == tmp || neighbours[1] == tmp)
            return;
        if (!neighbours[0])
            neighbours[0] = tmp;
        else if (!neighbours[1])
            neighbours[1] = tmp;
    }
#endif
};

struct LoopData {
    LiveRange range;
    LiveRange boundary;
};

class UseDefList {
public:
    using List = Vector<Point, 4>;

    void add(Point instPoint)
    {
        if (m_instPoints.isEmpty() || m_instPoints.last() != instPoint) {
            ASSERT(m_instPoints.isEmpty() || m_instPoints.last() < instPoint);
            m_instPoints.append(instPoint);
        }
    }

    const List& LEGACY_NODELETE useDefs() const { return m_instPoints; }

    void dump(PrintStream& out) const
    {
        out.print(listDump(m_instPoints));
    }

private:
    List m_instPoints;
};

// AroundClobbersSplitMetadata tracks a Tmp that has been split to carry its value
// across register-clobbering instructions.
struct AroundClobbersSplitMetadata {
    struct Split {
        Tmp tmp;

        void dump(PrintStream& out) const { out.print(tmp); }
    };

    AroundClobbersSplitMetadata() = default;

    AroundClobbersSplitMetadata(Tmp tmp)
        : originalTmp(tmp) { }

    void dump(PrintStream& out) const
    {
        out.print(originalTmp, " : AroundClobbers { ", listDump(splits), " } ");
    }

    Tmp originalTmp;
    Vector<Split> splits;
};

// IntraBlockSplitMetadata tracks a Tmp that has been split into per-cluster Tmps
// within basic blocks. The original Tmp is spilled and each cluster Tmp may get its
// own register allocation for the duration of a cluster of uses/defs.
struct IntraBlockSplitMetadata {
    struct Split {
        Tmp tmp;
        Point lastDefPoint;

        void dump(PrintStream& out) const
        {
            out.print(tmp);
            if (lastDefPoint)
                out.print(" defPoint=", lastDefPoint);
        }
    };

    IntraBlockSplitMetadata(Tmp tmp)
        : originalTmp(tmp) { }

    void dump(PrintStream& out) const
    {
        out.print(originalTmp, " : IntraBlock { ", listDump(splits), " } ");
    }

    Tmp originalTmp;
    Vector<Split> splits;
};

// AroundLoopSplitMetadata tracks a Tmp that has been split at a loop boundary.
// The original Tmp covers the non-loop portion and the split Tmp covers the loop portion.
struct AroundLoopSplitMetadata {
    AroundLoopSplitMetadata(Tmp originalTmp, const NaturalLoop* loop)
        : originalTmp(originalTmp)
        , loop(loop) { }

    void dump(PrintStream& out) const
    {
        out.print(originalTmp, " : AroundLoop(BB", *loop->header(), ") { ", loopTmp, " loopHasDef=", loopHasDef, " } ");
    }

    Tmp originalTmp;
    Tmp loopTmp;
    bool loopHasDef { false };
    const NaturalLoop* loop { nullptr };
};

struct Mentions {
    static constexpr unsigned earlyUse = 1;
    static constexpr unsigned lateUse = 2;
    static constexpr unsigned earlyDef = 4;
    static constexpr unsigned lateDef = 8;
    static constexpr unsigned timingMask = 15;
    static constexpr unsigned wasDecodedSlowly = 16;
    static constexpr unsigned anyUse = 16;
    static constexpr unsigned zDef = 32;
    static constexpr unsigned anyDef = 64;
    static constexpr unsigned warmUseOrDef = 128;
    static constexpr unsigned coldUse = 256;
    static constexpr unsigned widthShift = 9;
    static constexpr unsigned widthMask = 7;
    static constexpr unsigned isFP = 1 << 12;
    static constexpr unsigned saysNothingOfWidth = 1 << 13;
    static constexpr unsigned isDefByConstant = 1 << 14;
    static constexpr unsigned numberOfRoles = 12;

    static constexpr unsigned traitsOf(Arg::Role role)
    {
        switch (role) {
        case Arg::Use:
            return earlyUse | anyUse | warmUseOrDef;
        case Arg::ColdUse:
            return earlyUse | anyUse | coldUse;
        case Arg::LateUse:
            return lateUse | anyUse | warmUseOrDef;
        case Arg::LateColdUse:
            return lateUse | anyUse | coldUse;
        case Arg::Def:
            return lateDef | anyDef | warmUseOrDef;
        case Arg::ZDef:
            return lateDef | anyDef | zDef | warmUseOrDef;
        case Arg::UseDef:
            return earlyUse | lateDef | anyUse | anyDef | warmUseOrDef;
        case Arg::UseZDef:
            return earlyUse | lateDef | anyUse | anyDef | zDef | warmUseOrDef;
        case Arg::EarlyDef:
            return earlyDef | anyDef | warmUseOrDef;
        case Arg::EarlyZDef:
            return earlyDef | anyDef | zDef | warmUseOrDef;
        case Arg::Scratch:
            return earlyDef | lateUse | anyUse | anyDef | warmUseOrDef;
        case Arg::UseAddr:
            return 0;
        }
        return 0;
    }

    static constexpr unsigned traitsOf(Arg::Role role, Bank bank, Width width)
    {
        return traitsOf(role) | static_cast<unsigned>(width) << widthShift | (bank == FP ? isFP : 0);
    }

    static constexpr std::array<uint16_t, 256> makeTraitsOfForms()
    {
        std::array<uint16_t, 256> result { };
        for (unsigned form = 0; form < 256; ++form) {
            unsigned role = (form >> formRoleShift) & formRoleMask;
            unsigned width = (form >> formWidthShift) & formWidthMask;
            if (role >= numberOfRoles || !width || width > 5)
                continue;
            result[form] = traitsOf(static_cast<Arg::Role>(role), static_cast<Bank>((form >> formBankShift) & formBankMask), static_cast<Width>(width - 1));
        }
        return result;
    }

    Vector<unsigned> tmps;
    Vector<uint16_t> traits;
    Vector<unsigned> firstOfInst;
    Vector<uint8_t> timingsOfInst;
    unsigned numberOfInsts { 0 };
};

static constexpr std::array<uint16_t, 256> traitsOfForms = Mentions::makeTraitsOfForms();

static void validateTraitsOfForms()
{
    for (unsigned i = 0; i < Mentions::numberOfRoles; ++i) {
        Arg::Role role = static_cast<Arg::Role>(i);
        unsigned traits = (Arg::isEarlyUse(role) ? Mentions::earlyUse : 0) | (Arg::isLateUse(role) ? Mentions::lateUse : 0) | (Arg::isEarlyDef(role) ? Mentions::earlyDef : 0) | (Arg::isLateDef(role) ? Mentions::lateDef : 0)
            | (Arg::isAnyUse(role) ? Mentions::anyUse : 0) | (Arg::isZDef(role) ? Mentions::zDef : 0) | (Arg::isAnyDef(role) ? Mentions::anyDef : 0) | (Arg::isWarmUse(role) || Arg::isAnyDef(role) ? Mentions::warmUseOrDef : 0) | (Arg::isColdUse(role) ? Mentions::coldUse : 0);
        RELEASE_ASSERT(traits == Mentions::traitsOf(role));
        for (Width width : { Width8, Width16, Width32, Width64, Width128 }) {
            for (Bank bank : { GP, FP }) {
                uint8_t form = ENCODE_INST_FORM(role, bank, width);
                RELEASE_ASSERT(decodeFormRole(form) == role && decodeFormBank(form) == bank && decodeFormWidth(form) == width);
                RELEASE_ASSERT(traitsOfForms[form] == Mentions::traitsOf(role, bank, width));
            }
        }
    }
}

static constexpr unsigned noPiece = std::numeric_limits<unsigned>::max();

struct Piece {
    Point begin;
    Point end;
    unsigned higher;
};

struct StateOfRange {
    Point activeEnd;
    unsigned lowestPiece;
};

class WidthsOfTmps {
public:
    struct Widths {
        Width use;
        Width def;
    };

    Widths* prepare(Code& code)
    {
        unsigned endOfGP = Tmp::absoluteIndexEnd(code, GP);
        unsigned endOfFP = Tmp::absoluteIndexEnd(code, FP);
        m_gp.grow(endOfGP + endOfFP);
        Widths* widths = m_gp.mutableSpan().data();
        std::fill(widths, widths + endOfGP, Widths { minimumWidth(GP), minimumWidth(GP) });
        std::fill(widths + endOfGP, widths + endOfGP + endOfFP, Widths { minimumWidth(FP), minimumWidth(FP) });
        return widths;
    }

    void finish(unsigned endOfGP)
    {
        m_fp.append(m_gp.span().subspan(endOfGP));
        m_gp.shrink(endOfGP);
    }

    Width useWidth(Tmp tmp) const { return widths(tmp).use; }
    Width defWidth(Tmp tmp) const { return widths(tmp).def; }
    Width requiredWidth(Tmp tmp) const { return std::max(widths(tmp).use, widths(tmp).def); }

    void setWidths(Tmp tmp, Width useWidth, Width defWidth)
    {
        Vector<Widths>& vector = tmp.isGP() ? m_gp : m_fp;
        unsigned index = tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
        if (index >= vector.size())
            vector.grow(index + 1);
        vector[index] = { useWidth, defWidth };
    }

private:
    const Widths& widths(Tmp tmp) const
    {
        if (tmp.isGP())
            return m_gp[AbsoluteTmpMapper<GP>::absoluteIndex(tmp)];
        return m_fp[AbsoluteTmpMapper<FP>::absoluteIndex(tmp)];
    }

    Vector<Widths> m_gp;
    Vector<Widths> m_fp;
};

class CountsOfTmps {
public:
    static constexpr uint8_t hasConstDef = 1;
    static constexpr uint8_t hasOtherDef = 2;

    void prepare(Code& code)
    {
        m_ends[GP] = Tmp::absoluteIndexEnd(code, GP);
        m_ends[FP] = Tmp::absoluteIndexEnd(code, FP);
        counts.fill(0, m_ends[GP] + m_ends[FP]);
        kindsOfDefs.fill(0, m_ends[GP] + m_ends[FP]);
        gpConstants.grow(m_ends[GP]);
        fpConstants.grow(m_ends[FP]);
        fpConstantWidths.grow(m_ends[FP]);
    }

    template<Bank bank>
    bool isConstDef(unsigned absoluteIndex) const
    {
        if (absoluteIndex >= m_ends[bank])
            return false;
        return kindsOfDefs[linearIndex<bank>(absoluteIndex)] == hasConstDef;
    }

    template<Bank bank>
    decltype(auto) constant(unsigned absoluteIndex) const
    {
        if constexpr (bank == GP)
            return gpConstants[absoluteIndex];
        else
            return fpConstants[absoluteIndex];
    }

    template<Bank bank>
    float numWarmUsesAndDefs(unsigned absoluteIndex) const
    {
        return counts[linearIndex<bank>(absoluteIndex)];
    }

    template<Bank bank>
    Width constantWidth(unsigned absoluteIndex) const
    {
        static_assert(bank == FP, "constantWidth only valid for FP bank");
        return fpConstantWidths[absoluteIndex];
    }

    Vector<float> counts;
    Vector<uint8_t> kindsOfDefs;
    Vector<int64_t> gpConstants;
    Vector<v128_t> fpConstants;
    Vector<Width> fpConstantWidths;

private:
    template<Bank bank>
    unsigned linearIndex(unsigned absoluteIndex) const
    {
        return bank == GP ? absoluteIndex : m_ends[GP] + absoluteIndex;
    }

    std::array<unsigned, numBanks> m_ends { };
};

class MapOfTmpData {
public:
    explicit MapOfTmpData(Code& code)
    {
        fill(m_gp, Tmp::absoluteIndexEnd(code, GP));
        fill(m_fp, Tmp::absoluteIndexEnd(code, FP));
    }

    TmpData& operator[](Tmp tmp)
    {
        if (tmp.isGP())
            return m_gp[AbsoluteTmpMapper<GP>::absoluteIndex(tmp)];
        return m_fp[AbsoluteTmpMapper<FP>::absoluteIndex(tmp)];
    }

    const TmpData& operator[](Tmp tmp) const
    {
        if (tmp.isGP())
            return m_gp[AbsoluteTmpMapper<GP>::absoluteIndex(tmp)];
        return m_fp[AbsoluteTmpMapper<FP>::absoluteIndex(tmp)];
    }

    template<Bank bank>
    const TmpData& get(Tmp tmp) const
    {
        ASSERT(bank == tmp.bank());
        if constexpr (bank == GP)
            return m_gp[AbsoluteTmpMapper<GP>::absoluteIndex(tmp)];
        return m_fp[AbsoluteTmpMapper<FP>::absoluteIndex(tmp)];
    }

    template<Bank bank>
    TmpData& get(Tmp tmp)
    {
        ASSERT(bank == tmp.bank());
        if constexpr (bank == GP)
            return m_gp[AbsoluteTmpMapper<GP>::absoluteIndex(tmp)];
        return m_fp[AbsoluteTmpMapper<FP>::absoluteIndex(tmp)];
    }

    void append(Tmp tmp, TmpData&& data)
    {
        if (tmp.isGP()) {
            RELEASE_ASSERT(AbsoluteTmpMapper<GP>::absoluteIndex(tmp) == m_gp.size());
            m_gp.append(WTF::move(data));
        } else {
            RELEASE_ASSERT(AbsoluteTmpMapper<FP>::absoluteIndex(tmp) == m_fp.size());
            m_fp.append(WTF::move(data));
        }
    }

private:
    static void fill(Vector<TmpData>& vector, size_t size)
    {
        vector.reserveInitialCapacity(size + size / 2 + 16);
        vector.grow(size);
    }

    Vector<TmpData> m_gp;
    Vector<TmpData> m_fp;
};

class GreedyAllocator {
public:
    enum class DeadCodeSearch : bool { No, Yes };

    GreedyAllocator(Code& code, DeadCodeSearch deadCodeSearch)
        : m_code(code)
        , m_blockToHeadPoint(code.size())
        , m_tailPoints(code.size())
        , m_searchesForDeadCode(deadCodeSearch == DeadCodeSearch::Yes)
        , m_map(code)
        , m_aroundClobbersMetadata(1) // Sacrifice index 0.
        , m_spillSlotTable(FillWith { }, 1, nullptr) // Sacrifice index 0.
        , m_regRanges(Reg::maxIndex() + 1)
#if !USE(BUN_JSC_ADDITIONS)
        , m_insertionSets(code.size())
#endif
    {
        decode();
        if (m_hasDeadCode)
            return;

        if (Options::airValidateGreedRegAlloc()) [[unlikely]] {
            validateTraitsOfForms();
            validateFusedAnalyses();
            for (BasicBlock* block : m_code) {
                for (Inst& inst : *block)
                    validateFastTmpEnumeration(inst);
            }
        }
    }

    void decode()
    {
        m_endOfGPIndices = Tmp::absoluteIndexEnd(m_code, GP);
        m_numberOfLinearIndices = Tmp::linearIndexEnd(m_code);
        m_useCounts.prepare(m_code);
        m_endsOfBlock.grow(m_code.size());

        m_fastBlocks.push(m_code[0]);
        while (BasicBlock* block = m_fastBlocks.pop()) {
            for (FrequentedBlock& successor : block->successors()) {
                if (!successor.isRare())
                    m_fastBlocks.push(successor.block());
            }
        }

        Vector<unsigned> movesForWidths;
        Vector<PaddingSite> paddingSites;
        writeDownMentions(movesForWidths, paddingSites);
        findWidths(movesForWidths);
        countUsesAndDefs();
        findWhatBlocksExposeAndDefine();
        if (m_searchesForDeadCode && m_hasDefOfUnreadTmp && findDeadCode()) [[unlikely]] {
            m_hasDeadCode = true;
            return;
        }
        m_isRead.clear();

        InsertionSet insertionSet(m_code);
        for (size_t i = 0; i < paddingSites.size();) {
            BasicBlock* block = m_code[paddingSites[i].blockIndex];
            for (; i < paddingSites.size() && paddingSites[i].blockIndex == block->index(); ++i)
                insertionSet.insert(paddingSites[i].instIndex, Padding, block->at(paddingSites[i].instIndex).origin);
            insertionSet.execute(block);
        }
    }

    bool hasDeadCode() const { return m_hasDeadCode; }

    NEVER_INLINE bool findDeadCode()
    {
        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                if (inst.hasNonArgEffects())
                    continue;
                bool isLive = false;
                inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
                    if (!Arg::isAnyDef(role) || role == Arg::Scratch)
                        return;
                    if (arg.isTmp())
                        isLive |= m_isRead[linearIndex(arg.tmp())];
                    else
                        isLive |= !arg.isStack() || arg.stackSlot()->isLocked();
                });
                if (!isLive)
                    return true;
            }
        }
        return false;
    }

    struct PaddingSite {
        unsigned blockIndex;
        unsigned instIndex;
    };

    void writeDownMentions(Vector<unsigned>& movesForWidths, Vector<PaddingSite>& paddingSites)
    {
        const unsigned endOfGPIndices = m_endOfGPIndices;

        unsigned numberOfInstsAhead = 0;
        for (BasicBlock* block : m_code)
            numberOfInstsAhead += block->size();
        m_mentions.firstOfInst.grow(static_cast<size_t>(numberOfInstsAhead) * 2 + 1);
        m_mentions.timingsOfInst.grow(static_cast<size_t>(numberOfInstsAhead) * 2 + 1);
        unsigned* firstOfInst = m_mentions.firstOfInst.mutableSpan().data();
        uint8_t* timingsOfInsts = m_mentions.timingsOfInst.mutableSpan().data();
        unsigned ordinal = 0;

        size_t capacity = static_cast<size_t>(numberOfInstsAhead) * 2 + 64;
        m_mentions.tmps.grow(capacity);
        m_mentions.traits.grow(capacity);
        unsigned* tmps = m_mentions.tmps.mutableSpan().data();
        uint16_t* traitsOfMentions = m_mentions.traits.mutableSpan().data();
        unsigned numberOfMentions = 0;

        m_isRead.fill(0, m_numberOfLinearIndices);
        uint8_t* isRead = m_isRead.mutableSpan().data();
        for (unsigned i = 0; i <= Reg::maxIndex(); ++i)
            isRead[linearIndex(Tmp(Reg::fromIndex(i)))] = 1;

        movesForWidths.grow(numberOfInstsAhead);
        unsigned* nextMoveForWidths = movesForWidths.mutableSpan().data();
        m_movesOfTmps.grow(numberOfInstsAhead);
        unsigned* nextMoveOfTmps = m_movesOfTmps.mutableSpan().data();

        struct Clobbers {
            RegisterSet set;
            uint64_t whole { 0 };
            uint64_t high64 { 0 };

            void update(const RegisterSet& newSet)
            {
                if (newSet == set) [[likely]]
                    return;
                set = newSet;
                whole = 0;
                high64 = 0;
                set.forEachWithWidthAndPreserved(
                    [&](Reg reg, Width, PreservedWidth preservedWidth) {
                        ASSERT(preservedWidth == PreservesNothing || preservedWidth == Preserves64);
                        (preservedWidth == PreservesNothing ? whole : high64) |= static_cast<uint64_t>(1) << reg.index();
                    });
            }
        };
        Clobbers early;
        Clobbers late;
        Vector<MentionDecodedSlowly, 16> mentionsDecodedSlowly;
        const bool notesNeighbours = isX86() && m_code.chargesForCalleeSaves();
        constexpr unsigned traitsOfBase = Mentions::traitsOf(Arg::Use, GP, pointerWidth());
        constexpr unsigned kindsWithBase = 1u << Arg::SimpleAddr | 1u << Arg::Addr | 1u << Arg::ExtendedOffsetAddr | 1u << Arg::Index | 1u << Arg::PreIndex | 1u << Arg::PostIndex;

        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            BasicBlock* block = m_code[blockIndex];
            m_endsOfBlock[blockIndex].firstInst = ordinal;
            if (!block)
                continue;

            bool previousHasLateUseOrDef = false;
            unsigned blockSize = block->size();
            Inst* insts = block->insts().mutableSpan().data();
            unsigned instIndex = 0;
            for (unsigned oldInstIndex = 0; oldInstIndex < blockSize; ++oldInstIndex, ++instIndex) {
                if (insts[oldInstIndex].kind.opcode == Padding) [[unlikely]] {
                    --instIndex;
                    continue;
                }
                if (instIndex != oldInstIndex)
                    insts[instIndex] = WTF::move(insts[oldInstIndex]);
                Inst& inst = insts[instIndex];
                Arg* args = inst.args().data();
                const unsigned numberOfArgs = inst.args().size();
                const Opcode opcode = inst.kind.opcode;

                if (numberOfMentions + 2 * numberOfArgs > capacity) [[unlikely]] {
                    capacity = capacity * 2 + 2 * numberOfArgs;
                    m_mentions.tmps.grow(capacity);
                    m_mentions.traits.grow(capacity);
                    tmps = m_mentions.tmps.mutableSpan().data();
                    traitsOfMentions = m_mentions.traits.mutableSpan().data();
                }

                unsigned traitsOfAllMentions = 0;
                bool isMoveOfTmps = false;
                bool isConditionalMove = false;
                switch (opcode) {
                case Move:
                case Move32:
                case MoveFloat:
                case MoveDouble:
                case MoveVector:
                    if (!args[1].isTmp())
                        break;
                    if (args[0].isTmp()) {
                        isMoveOfTmps = numberOfArgs == 2;
                        if (opcode == Move) {
                            *nextMoveForWidths++ = numberOfMentions;
                            traitsOfAllMentions = Mentions::saysNothingOfWidth;
                        }
                        break;
                    }
                    if (args[0].isSomeImm())
                        traitsOfAllMentions = noteMoveOfConstant(opcode, args[0], args[1].tmp());
                    break;
                case MoveConditionally32:
                case MoveConditionally64:
                case MoveConditionallyTest32:
                case MoveConditionallyTest64:
                case MoveConditionallyDouble:
                case MoveConditionallyFloat:
                    isConditionalMove = notesNeighbours && numberOfArgs == 6;
                    break;
                default:
                    break;
                }

                unsigned traitsOfArgs = 0;
                unsigned traitsOfInst = 0;
                unsigned wasDecodedSlowly = 0;
                const unsigned firstMention = numberOfMentions;
                auto note = [&](Tmp tmp, unsigned traits) ALWAYS_INLINE_LAMBDA {
                    unsigned index = tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : endOfGPIndices + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
                    tmps[numberOfMentions] = index;
                    if ((traits & (Mentions::anyUse | Mentions::anyDef)) == Mentions::anyUse)
                        isRead[index] = 1;
                    traitsOfMentions[numberOfMentions] = traits | traitsOfAllMentions;
                    ++numberOfMentions;
                    traitsOfInst |= traits;
                };
                unsigned argIndex = 0;
                if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
                    for (; argIndex < numberOfArgs; ++argIndex) {
                        Arg& arg = args[argIndex];
                        unsigned traits = traitsOfForms[forms[argIndex]];
                        traitsOfArgs |= traits;
                        unsigned kind = arg.kind();
                        if (kind == Arg::Tmp) {
                            note(arg.tmp(), traits);
                            continue;
                        }
                        if (!(kindsWithBase >> kind & 1))
                            continue;
                        unsigned traitsOfTmp = traitsOfBase;
                        if (((forms[argIndex] >> formRoleShift) & formRoleMask) == Arg::UseAddr) [[unlikely]]
                            traitsOfTmp = Mentions::traitsOf(Arg::Use) | (traits & (Mentions::widthMask << Mentions::widthShift));
                        if (kind == Arg::PreIndex || kind == Arg::PostIndex) [[unlikely]]
                            traitsOfTmp = Mentions::traitsOf(Arg::UseDef) | (traitsOfTmp & (Mentions::widthMask << Mentions::widthShift));
                        note(arg.base(), traitsOfTmp);
                        if (kind == Arg::Index)
                            note(arg.index(), traitsOfTmp);
                    }
                } else {
                    mentionsDecodedSlowly.shrink(0);
                    traitsOfArgs = decodeSlowly(inst, mentionsDecodedSlowly);
                    for (auto& mention : mentionsDecodedSlowly)
                        note(mention.tmp, Mentions::traitsOf(mention.role, mention.bank, mention.width));
                    wasDecodedSlowly = Mentions::wasDecodedSlowly;
                }

                bool hasEarlyDef = traitsOfArgs & Mentions::earlyDef;
                bool hasLateUseOrDef = traitsOfArgs & (Mentions::lateUse | Mentions::lateDef);
                if (opcode == Patch) [[unlikely]] {
                    early.update(inst.extraEarlyClobberedRegs());
                    late.update(inst.extraClobberedRegs());
                    hasEarlyDef |= early.whole || early.high64;
                    hasLateUseOrDef |= late.whole || late.high64;
                    if ((traitsOfInst & (Mentions::earlyDef | Mentions::lateDef)) && inst.hasNonArgEffects()) {
                        for (unsigned i = firstMention; i < numberOfMentions; ++i)
                            isRead[tmps[i]] = 1;
                    }
                }

                if (previousHasLateUseOrDef && hasEarlyDef) {
                    paddingSites.append(PaddingSite { blockIndex, instIndex });
                    firstOfInst[ordinal] = firstMention;
                    timingsOfInsts[ordinal] = 0;
                    ++ordinal;
                }
                previousHasLateUseOrDef = hasLateUseOrDef;

                firstOfInst[ordinal] = firstMention;
                timingsOfInsts[ordinal] = (traitsOfInst & Mentions::timingMask) | wasDecodedSlowly;

                if (isMoveOfTmps)
                    *nextMoveOfTmps++ = ordinal;
                else if (opcode == Patch || isConditionalMove || (traitsOfInst & Mentions::coldUse)) [[unlikely]] {
                    if (isConditionalMove)
                        m_conditionalMoves.append(unsigned { ordinal });
                    if (traitsOfInst & Mentions::coldUse) {
                        for (unsigned i = firstMention; i < numberOfMentions; ++i) {
                            if (traitsOfMentions[i] & Mentions::coldUse)
                                m_map[tmpForLinearIndex(tmps[i])].hasColdUse = true;
                        }
                    }
                    if (opcode == Patch)
                        noteClobbers(ordinal, early.whole, early.high64, late.whole, late.high64);
                }
                ++ordinal;
            }
            if (instIndex != blockSize)
                block->insts().shrink(instIndex);
        }
        firstOfInst[ordinal] = numberOfMentions;
        m_mentions.numberOfInsts = ordinal;
        movesForWidths.shrink(nextMoveForWidths - movesForWidths.mutableSpan().data());
        m_movesOfTmps.shrink(nextMoveOfTmps - m_movesOfTmps.mutableSpan().data());
    }

    NEVER_INLINE unsigned noteMoveOfConstant(Opcode opcode, const Arg& constant, Tmp tmp)
    {
        unsigned traits = 0;
        unsigned index = linearIndex(tmp);
        if (opcode == Move && constant.isImm() && constant.value() >= 0) {
            m_defsOfSmallConstants.append({ index, constant.value() });
            traits = Mentions::saysNothingOfWidth;
        }
        bool isGPMove = opcode == Move || opcode == Move32;
        if (isGPMove != tmp.isGP())
            return traits;
        uint8_t& kindsOfDefs = m_useCounts.kindsOfDefs[index];
        if (kindsOfDefs & CountsOfTmps::hasConstDef) {
            kindsOfDefs |= CountsOfTmps::hasOtherDef;
            return traits | Mentions::isDefByConstant;
        }
        kindsOfDefs |= CountsOfTmps::hasConstDef;
        if (isGPMove)
            m_useCounts.gpConstants[index] = opcode == Move32 ? static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<uint64_t>(constant.value())))) : constant.value();
        else {
            unsigned fpIndex = index - m_endOfGPIndices;
            if (constant.isFPImm128())
                m_useCounts.fpConstants[fpIndex] = constant.asV128();
            else {
                v128_t value { };
                value.u64x2[0] = static_cast<uint64_t>(constant.value());
                m_useCounts.fpConstants[fpIndex] = value;
            }
            m_useCounts.fpConstantWidths[fpIndex] = opcode == MoveFloat ? Width32 : opcode == MoveDouble ? Width64 : Width128;
        }
        return traits | Mentions::isDefByConstant;
    }

    NEVER_INLINE void noteClobbers(unsigned ordinal, uint64_t early, uint64_t earlyHigh64, uint64_t late, uint64_t lateHigh64)
    {
        Point positionOfEarly = ordinal * PointOffsets::PointsPerInst + PointOffsets::Early;
        Point positionOfLate = ordinal * PointOffsets::PointsPerInst + PointOffsets::Late;
        if (early) {
            m_clobberPoints.append(positionOfEarly);
            m_clobberedRegsAtPoint.append(early);
        }
        if (late) {
            m_clobberPoints.append(positionOfLate);
            m_clobberedRegsAtPoint.append(late);
        }
        m_allClobberedRegs |= early | late;
        if (earlyHigh64 | lateHigh64) [[unlikely]] {
            WTF::forEachSetBit(std::span<const uint64_t> { &earlyHigh64, 1 }, [&](size_t index) {
                m_regRanges[Reg::fromIndex(index)].addClobberHigh64(Reg::fromIndex(index), positionOfEarly);
            });
            WTF::forEachSetBit(std::span<const uint64_t> { &lateHigh64, 1 }, [&](size_t index) {
                m_regRanges[Reg::fromIndex(index)].addClobberHigh64(Reg::fromIndex(index), positionOfLate);
            });
        }
    }

    void findWidths(Vector<unsigned>& movesForWidths)
    {
        WidthsOfTmps::Widths* widths = m_tmpWidth.prepare(m_code);
        std::array<Width, numBanks> conservativeWidths;
        conservativeWidths[GP] = m_code.usesSIMD() ? conservativeWidth(GP) : conservativeWidthWithoutVectors(GP);
        conservativeWidths[FP] = m_code.usesSIMD() ? conservativeWidth(FP) : conservativeWidthWithoutVectors(FP);
        RegisterSet::allRegisters().forEach([&](Reg reg) {
            Width conservative = conservativeWidths[bankForReg(reg)];
            widths[linearIndex(Tmp(reg))] = { conservative, conservative };
        });

        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* traitsOfMentions = m_mentions.traits.span().data();
        const unsigned numberOfMentions = m_mentions.firstOfInst[m_mentions.numberOfInsts];
        for (unsigned i = 0; i < numberOfMentions; ++i) {
            unsigned traits = traitsOfMentions[i];
            if (traits & Mentions::saysNothingOfWidth)
                continue;
            WidthsOfTmps::Widths& tmpWidths = widths[tmps[i]];
            Width width = static_cast<Width>((traits >> Mentions::widthShift) & Mentions::widthMask);
            if (traits & Mentions::anyUse)
                tmpWidths.use = std::max(tmpWidths.use, width);
            if (traits & Mentions::zDef)
                tmpWidths.def = std::max(tmpWidths.def, width);
            else if (traits & Mentions::anyDef)
                tmpWidths.def = conservativeWidths[traits & Mentions::isFP ? FP : GP];
        }
        for (auto& def : m_defsOfSmallConstants) {
            Width maxWidth = Width64;
            if (def.value <= std::numeric_limits<int8_t>::max())
                maxWidth = Width8;
            else if (def.value <= std::numeric_limits<int16_t>::max())
                maxWidth = Width16;
            else if (def.value <= std::numeric_limits<int32_t>::max())
                maxWidth = Width32;
            widths[def.index].def = std::max(widths[def.index].def, maxWidth);
        }
        m_defsOfSmallConstants.clear();

        bool changed = true;
        bool goesDown = false;
        unsigned* moves = movesForWidths.mutableSpan().data();
        size_t numberOfMoves = movesForWidths.size();
        const Width widest = conservativeWidths[GP];
        while (changed) {
            changed = false;
            goesDown = !goesDown;
            size_t numberOfMovesKept = 0;
            for (size_t step = 0; step < numberOfMoves; ++step) {
                unsigned mention = moves[goesDown ? step : numberOfMoves - 1 - step];
                WidthsOfTmps::Widths& srcWidths = widths[tmps[mention]];
                WidthsOfTmps::Widths& dstWidths = widths[tmps[mention + 1]];
                if (dstWidths.def < srcWidths.def) {
                    dstWidths.def = srcWidths.def;
                    changed = true;
                }
                if (srcWidths.use < dstWidths.use) {
                    srcWidths.use = dstWidths.use;
                    changed = true;
                }
                if (goesDown && (dstWidths.def != widest || srcWidths.use != widest))
                    moves[numberOfMovesKept++] = mention;
            }
            if (goesDown)
                numberOfMoves = numberOfMovesKept;
        }
        m_tmpWidth.finish(m_endOfGPIndices);
    }

    void countUsesAndDefs()
    {
        float* counts = m_useCounts.counts.mutableSpan().data();
        uint8_t* kindsOfDefs = m_useCounts.kindsOfDefs.mutableSpan().data();
        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* traitsOfMentions = m_mentions.traits.span().data();
        const unsigned* firstOfInst = m_mentions.firstOfInst.span().data();
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;
            double frequency = block->frequency();
            if (!m_fastBlocks.saw(block))
                frequency *= Options::rareBlockPenalty();
            unsigned end = firstOfInst[blockIndex + 1 < m_code.size() ? m_endsOfBlock[blockIndex + 1].firstInst : m_mentions.numberOfInsts];
            for (unsigned i = firstOfInst[m_endsOfBlock[blockIndex].firstInst]; i < end; ++i) {
                unsigned traits = traitsOfMentions[i];
                if (!(traits & Mentions::warmUseOrDef))
                    continue;
                unsigned index = tmps[i];
                counts[index] += frequency;
                if ((traits & (Mentions::anyDef | Mentions::isDefByConstant)) == Mentions::anyDef)
                    kindsOfDefs[index] |= CountsOfTmps::hasOtherDef;
            }
        }
    }

    void findWhatBlocksExposeAndDefine()
    {
        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* traitsOfMentions = m_mentions.traits.span().data();
        const unsigned* firstOfInst = m_mentions.firstOfInst.span().data();
        const uint8_t* timingsOfInsts = m_mentions.timingsOfInst.span().data();
        const unsigned numberOfMentions = firstOfInst[m_mentions.numberOfInsts];

        Vector<unsigned> stampStore(FillWith { }, m_numberOfLinearIndices, 0u);
        unsigned* stampOfTmp = stampStore.mutableSpan().data();
        m_crossesBoundary.fill(0, m_numberOfLinearIndices);
        uint8_t* crossesBoundary = m_crossesBoundary.mutableSpan().data();
        m_exposedUses.grow(numberOfMentions);
        m_defsInBlocks.grow(numberOfMentions);
        unsigned* exposedUses = m_exposedUses.mutableSpan().data();
        unsigned* defsInBlocks = m_defsInBlocks.mutableSpan().data();
        unsigned numberOfExposedUses = 0;
        unsigned numberOfDefs = 0;
        const uint8_t* isRead = m_isRead.span().data();
        unsigned areAllDefsRead = 1;

        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            const unsigned stampOfUse = (blockIndex + 1) << 1;
            const unsigned stampOfDef = stampOfUse | 1;
            auto noteUse = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
                if ((stampOfTmp[index] | 1) == stampOfDef)
                    return;
                stampOfTmp[index] = stampOfUse;
                crossesBoundary[index] = 1;
                exposedUses[numberOfExposedUses++] = index;
            };
            auto noteDef = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
                if (stampOfTmp[index] == stampOfDef)
                    return;
                stampOfTmp[index] = stampOfDef;
                defsInBlocks[numberOfDefs++] = index;
                areAllDefsRead &= isRead[index];
            };

            unsigned endOfInsts = blockIndex + 1 < m_code.size() ? m_endsOfBlock[blockIndex + 1].firstInst : m_mentions.numberOfInsts;
            for (unsigned ordinal = m_endsOfBlock[blockIndex].firstInst; ordinal < endOfInsts; ++ordinal) {
                unsigned timings = timingsOfInsts[ordinal];
                unsigned begin = firstOfInst[ordinal];
                unsigned end = firstOfInst[ordinal + 1];
                if (timings & Mentions::earlyDef) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (traitsOfMentions[i] & Mentions::earlyDef)
                            noteDef(tmps[i]);
                    }
                }
                if (timings & Mentions::earlyUse) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (traitsOfMentions[i] & Mentions::earlyUse)
                            noteUse(tmps[i]);
                    }
                }
                if (timings & Mentions::lateDef) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (traitsOfMentions[i] & Mentions::lateDef)
                            noteDef(tmps[i]);
                    }
                }
                if (timings & Mentions::lateUse) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (!(traitsOfMentions[i] & Mentions::lateUse))
                            continue;
                        if (ordinal + 1 < endOfInsts) {
                            noteUse(tmps[i]);
                            continue;
                        }
                        crossesBoundary[tmps[i]] = 1;
                        m_usesAtTails.append(unsigned { tmps[i] });
                    }
                }
            }
            m_endsOfBlock[blockIndex].exposedUses = numberOfExposedUses;
            m_endsOfBlock[blockIndex].defs = numberOfDefs;
            m_endsOfBlock[blockIndex].usesAtTail = m_usesAtTails.size();
        }
        m_hasDefOfUnreadTmp = !areAllDefsRead;
    }

    struct MentionDecodedSlowly {
        Tmp tmp;
        Arg::Role role;
        Bank bank;
        Width width;
    };

    NEVER_INLINE static unsigned decodeSlowly(Inst& inst, Vector<MentionDecodedSlowly, 16>& mentions)
    {
        unsigned traitsOfArgs = 0;
        inst.forEachArg(
            [&](Arg& arg, Arg::Role argRole, Bank argBank, Width argWidth) {
                traitsOfArgs |= Mentions::traitsOf(argRole);
                arg.forEachTmp(argRole, argBank, argWidth,
                    [&](Tmp& tmp, Arg::Role role, Bank bank, Width width) {
                        mentions.append(MentionDecodedSlowly { tmp, role, bank, width });
                    });
            });
        return traitsOfArgs;
    }

    void solveLiveness()
    {
        constexpr unsigned wordBits = 64;
        unsigned numberOfBlocks = m_code.size();

        Vector<unsigned> columnStore(m_numberOfLinearIndices);
        unsigned* columnOfIndex = columnStore.mutableSpan().data();
        for (unsigned index = 0; index < m_numberOfLinearIndices; ++index) {
            if (!m_crossesBoundary[index])
                continue;
            columnOfIndex[index] = m_indexOfColumn.size();
            m_indexOfColumn.append(index);
        }
        m_wordsPerSet = (m_indexOfColumn.size() + wordBits - 1) / wordBits;
        const size_t wordsPerSet = m_wordsPerSet;
        if (!wordsPerSet)
            return;

        size_t matrixWords = static_cast<size_t>(numberOfBlocks) * wordsPerSet;
        if (matrixWords > maxWordsPerLivenessMatrix) [[unlikely]] {
            solveLivenessInSlices(columnOfIndex);
            return;
        }
        m_liveAtBoundaries.fill(0, 2 * matrixWords);
        Vector<uint64_t> transferStore;
        transferStore.fill(0, 2 * matrixWords);
        uint64_t* liveAtHead = m_liveAtBoundaries.mutableSpan().data();
        uint64_t* liveAtTail = liveAtHead + matrixWords;
        uint64_t* gen = transferStore.mutableSpan().data();
        uint64_t* kill = gen + matrixWords;

        auto setBit = [](uint64_t* set, unsigned bit) {
            set[bit / wordBits] |= static_cast<uint64_t>(1) << (bit % wordBits);
        };

        EndsOfBlock begin;
        unsigned numberOfActiveBlocks = 0;
        for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            EndsOfBlock end = m_endsOfBlock[blockIndex];
            size_t offset = static_cast<size_t>(blockIndex) * wordsPerSet;
            for (unsigned i = begin.exposedUses; i < end.exposedUses; ++i)
                setBit(gen + offset, columnOfIndex[m_exposedUses[i]]);
            for (unsigned i = begin.defs; i < end.defs; ++i) {
                unsigned index = m_defsInBlocks[i];
                if (m_crossesBoundary[index])
                    setBit(kill + offset, columnOfIndex[index]);
            }
            for (unsigned i = begin.usesAtTail; i < end.usesAtTail; ++i)
                setBit(liveAtTail + offset, columnOfIndex[m_usesAtTails[i]]);
            begin = end;
            if (m_code[blockIndex])
                ++numberOfActiveBlocks;
        }

        Vector<unsigned, 64> worklist;
        worklist.reserveInitialCapacity(numberOfActiveBlocks);
        BitVector dirtyBlocks(numberOfBlocks);
        appendNodeIndicesInOrder(m_code.cfg(), GraphOrder::PostOrder, dirtyBlocks, worklist);
        worklist.reverse();
        for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            if (m_code[blockIndex] && !dirtyBlocks.quickSet(blockIndex))
                worklist.append(blockIndex);
        }

        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            dirtyBlocks.quickClear(blockIndex);

            size_t offset = static_cast<size_t>(blockIndex) * wordsPerSet;
            uint64_t* head = liveAtHead + offset;
            uint64_t changed = 0;
            for (size_t i = 0; i < wordsPerSet; ++i) {
                uint64_t newWord = gen[offset + i] | (liveAtTail[offset + i] & ~kill[offset + i]);
                changed |= newWord ^ head[i];
                head[i] = newWord;
            }
            if (!changed)
                continue;

            for (BasicBlock* predecessor : m_code[blockIndex]->predecessors()) {
                uint64_t* tail = liveAtTail + static_cast<size_t>(predecessor->index()) * wordsPerSet;
                uint64_t predecessorChanged = 0;
                for (size_t i = 0; i < wordsPerSet; ++i) {
                    uint64_t newWord = tail[i] | head[i];
                    predecessorChanged |= newWord ^ tail[i];
                    tail[i] = newWord;
                }
                if (predecessorChanged && !dirtyBlocks.quickSet(predecessor->index()))
                    worklist.append(predecessor->index());
            }
        }
    }

    NEVER_INLINE void solveLivenessInSlices(const unsigned* columnOfIndex)
    {
        constexpr unsigned wordBits = 64;
        const unsigned numberOfBlocks = m_code.size();
        const size_t wordsPerSlice = std::max<size_t>(1, maxWordsPerLivenessMatrix / numberOfBlocks);
        const size_t numberOfSlices = (m_wordsPerSet + wordsPerSlice - 1) / wordsPerSlice;
        m_livenessIsInLists = true;

        Vector<unsigned, 64> order;
        BitVector dirtyBlocks(numberOfBlocks);
        appendNodeIndicesInOrder(m_code.cfg(), GraphOrder::PostOrder, dirtyBlocks, order);
        order.reverse();
        for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            if (m_code[blockIndex] && !dirtyBlocks.quickSet(blockIndex))
                order.append(blockIndex);
        }
        Vector<unsigned> positionInOrder(numberOfBlocks);
        for (unsigned position = 0; position < order.size(); ++position) {
            positionInOrder[order[position]] = position;
            dirtyBlocks.quickClear(order[position]);
        }

        constexpr unsigned noBlock = std::numeric_limits<unsigned>::max();
        Vector<unsigned> blockBefore(FillWith { }, numberOfBlocks, noBlock);
        Vector<unsigned> blockAfter(FillWith { }, numberOfBlocks, noBlock);
        unsigned firstBlock = noBlock;
        for (unsigned blockIndex = 0, previous = noBlock; blockIndex < numberOfBlocks; ++blockIndex) {
            if (!m_code[blockIndex])
                continue;
            if (previous == noBlock)
                firstBlock = blockIndex;
            else {
                blockBefore[blockIndex] = previous;
                blockAfter[previous] = blockIndex;
            }
            previous = blockIndex;
        }

        enum class Set : unsigned { Gen, Kill, Tail };
        struct Fact {
            unsigned blockIndex;
            unsigned columnAndSet;
        };
        auto forEachFact = [&](const auto& func) {
            EndsOfBlock begin;
            for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
                EndsOfBlock end = m_endsOfBlock[blockIndex];
                for (unsigned i = begin.exposedUses; i < end.exposedUses; ++i)
                    func(blockIndex, columnOfIndex[m_exposedUses[i]], Set::Gen);
                for (unsigned i = begin.defs; i < end.defs; ++i) {
                    unsigned index = m_defsInBlocks[i];
                    if (m_crossesBoundary[index])
                        func(blockIndex, columnOfIndex[index], Set::Kill);
                }
                for (unsigned i = begin.usesAtTail; i < end.usesAtTail; ++i)
                    func(blockIndex, columnOfIndex[m_usesAtTails[i]], Set::Tail);
                begin = end;
            }
        };
        Vector<size_t> endOfSlice(FillWith { }, numberOfSlices, static_cast<size_t>(0));
        forEachFact([&](unsigned, unsigned column, Set) {
            ++endOfSlice[column / wordBits / wordsPerSlice];
        });
        size_t numberOfFacts = 0;
        for (size_t& end : endOfSlice)
            numberOfFacts += std::exchange(end, numberOfFacts);
        Vector<Fact> facts(numberOfFacts);
        forEachFact([&](unsigned blockIndex, unsigned column, Set set) {
            facts[endOfSlice[column / wordBits / wordsPerSlice]++] = Fact { blockIndex, column << 2 | static_cast<unsigned>(set) };
        });

        const size_t maxMatrixWords = static_cast<size_t>(numberOfBlocks) * std::min(wordsPerSlice, m_wordsPerSet);
        Vector<uint64_t> store(FillWith { }, 4 * maxMatrixWords, static_cast<uint64_t>(0));
        Vector<unsigned> sliceThatTouched(FillWith { }, numberOfBlocks, 0u);
        Vector<unsigned> sliceThatLookedBehind(FillWith { }, numberOfBlocks, 0u);
        Vector<unsigned> touched;
        Vector<unsigned, 64> worklist;
        size_t firstFact = 0;
        for (size_t slice = 0; slice < numberOfSlices; ++slice) {
            const size_t firstWord = slice * wordsPerSlice;
            const size_t words = std::min(wordsPerSlice, m_wordsPerSet - firstWord);
            const size_t matrixWords = static_cast<size_t>(numberOfBlocks) * words;
            uint64_t* liveAtHead = store.mutableSpan().data();
            uint64_t* liveAtTail = liveAtHead + matrixWords;
            uint64_t* gen = liveAtTail + matrixWords;
            uint64_t* kill = gen + matrixWords;
            const unsigned stamp = slice + 1;

            auto touch = [&](unsigned blockIndex) {
                if (sliceThatTouched[blockIndex] == stamp)
                    return;
                sliceThatTouched[blockIndex] = stamp;
                touched.append(blockIndex);
            };
            touched.shrink(0);
            worklist.shrink(0);
            for (; firstFact < endOfSlice[slice]; ++firstFact) {
                Fact fact = facts[firstFact];
                unsigned column = fact.columnAndSet >> 2;
                Set set = static_cast<Set>(fact.columnAndSet & 3);
                uint64_t* matrix = set == Set::Gen ? gen : set == Set::Kill ? kill : liveAtTail;
                matrix[static_cast<size_t>(fact.blockIndex) * words + (column / wordBits - firstWord)] |= static_cast<uint64_t>(1) << (column % wordBits);
                touch(fact.blockIndex);
                if (set != Set::Kill && !dirtyBlocks.quickSet(fact.blockIndex))
                    worklist.append(fact.blockIndex);
            }
            std::ranges::sort(worklist, { }, [&](unsigned blockIndex) {
                return positionInOrder[blockIndex];
            });

            while (!worklist.isEmpty()) {
                unsigned blockIndex = worklist.takeLast();
                dirtyBlocks.quickClear(blockIndex);
                size_t offset = static_cast<size_t>(blockIndex) * words;
                uint64_t* head = liveAtHead + offset;
                uint64_t changed = 0;
                for (size_t i = 0; i < words; ++i) {
                    uint64_t newWord = gen[offset + i] | (liveAtTail[offset + i] & ~kill[offset + i]);
                    changed |= newWord ^ head[i];
                    head[i] = newWord;
                }
                if (!changed)
                    continue;
                for (BasicBlock* predecessor : m_code[blockIndex]->predecessors()) {
                    uint64_t* tail = liveAtTail + static_cast<size_t>(predecessor->index()) * words;
                    uint64_t predecessorChanged = 0;
                    for (size_t i = 0; i < words; ++i) {
                        uint64_t newWord = tail[i] | head[i];
                        predecessorChanged |= newWord ^ tail[i];
                        tail[i] = newWord;
                    }
                    if (!predecessorChanged)
                        continue;
                    touch(predecessor->index());
                    if (!dirtyBlocks.quickSet(predecessor->index()))
                        worklist.append(predecessor->index());
                }
            }

            auto note = [&](Vector<AtEdgeOfBlock>& list, unsigned blockIndex, const uint64_t* a, const uint64_t* b) {
                for (size_t wordIndex = 0; wordIndex < words; ++wordIndex) {
                    uint64_t word = a[wordIndex] & ~(b ? b[wordIndex] : 0);
                    for (; word; word &= word - 1)
                        list.append(AtEdgeOfBlock { blockIndex, m_indexOfColumn[(firstWord + wordIndex) * wordBits + std::countr_zero(word)] });
                }
            };
            auto lookBehind = [&](unsigned blockIndex) {
                if (sliceThatLookedBehind[blockIndex] == stamp)
                    return;
                sliceThatLookedBehind[blockIndex] = stamp;
                const uint64_t* tail = liveAtTail + static_cast<size_t>(blockIndex) * words;
                const uint64_t* headAfter = blockAfter[blockIndex] != noBlock ? liveAtHead + static_cast<size_t>(blockAfter[blockIndex]) * words : nullptr;
                if (headAfter)
                    note(m_closesBehindBlock, blockIndex, headAfter, tail);
                note(m_opensAtTailOfBlock, blockIndex, tail, headAfter);
            };
            for (unsigned blockIndex : touched) {
                lookBehind(blockIndex);
                if (blockBefore[blockIndex] != noBlock)
                    lookBehind(blockBefore[blockIndex]);
                else if (blockIndex == firstBlock)
                    note(m_closesBehindBlock, numberOfBlocks, liveAtHead + static_cast<size_t>(blockIndex) * words, nullptr);
            }
            for (unsigned blockIndex : touched) {
                size_t offset = static_cast<size_t>(blockIndex) * words;
                for (uint64_t* matrix : { liveAtHead, liveAtTail, gen, kill }) {
                    for (size_t i = 0; i < words; ++i)
                        matrix[offset + i] = 0;
                }
            }
        }

        auto byBlock = [](const AtEdgeOfBlock& left, const AtEdgeOfBlock& right) {
            return left.blockIndex < right.blockIndex;
        };
        std::stable_sort(m_closesBehindBlock.begin(), m_closesBehindBlock.end(), byBlock);
        std::stable_sort(m_opensAtTailOfBlock.begin(), m_opensAtTailOfBlock.end(), byBlock);
    }

    unsigned linearIndex(Tmp tmp) const
    {
        if (tmp.isGP())
            return AbsoluteTmpMapper<GP>::absoluteIndex(tmp);
        return m_endOfGPIndices + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
    }

    Tmp tmpForLinearIndex(unsigned index) const
    {
        if (index < m_endOfGPIndices)
            return AbsoluteTmpMapper<GP>::tmpFromAbsoluteIndex(index);
        return AbsoluteTmpMapper<FP>::tmpFromAbsoluteIndex(index - m_endOfGPIndices);
    }

    void validateFusedAnalyses()
    {
        TmpWidth referenceWidth(m_code);
        UseCounts referenceUseCounts(m_code);

        auto checkWidths = [&](Tmp tmp) {
            RELEASE_ASSERT(m_tmpWidth.useWidth(tmp) == referenceWidth.useWidth(tmp));
            RELEASE_ASSERT(m_tmpWidth.defWidth(tmp) == referenceWidth.defWidth(tmp));
        };
        m_code.forEachTmp(checkWidths);
        RegisterSet::allRegisters().forEach([&](Reg reg) {
            checkWidths(Tmp(reg));
        });

        for (unsigned i = 0; i < AbsoluteTmpMapper<GP>::absoluteIndex(m_code.numTmps(GP)); ++i) {
            RELEASE_ASSERT(m_useCounts.numWarmUsesAndDefs<GP>(i) == referenceUseCounts.numWarmUsesAndDefs<GP>(i));
            RELEASE_ASSERT(m_useCounts.isConstDef<GP>(i) == referenceUseCounts.isConstDef<GP>(i));
            if (m_useCounts.isConstDef<GP>(i))
                RELEASE_ASSERT(m_useCounts.constant<GP>(i) == referenceUseCounts.constant<GP>(i));
        }
        for (unsigned i = 0; i < AbsoluteTmpMapper<FP>::absoluteIndex(m_code.numTmps(FP)); ++i) {
            RELEASE_ASSERT(m_useCounts.numWarmUsesAndDefs<FP>(i) == referenceUseCounts.numWarmUsesAndDefs<FP>(i));
            RELEASE_ASSERT(m_useCounts.isConstDef<FP>(i) == referenceUseCounts.isConstDef<FP>(i));
            if (m_useCounts.isConstDef<FP>(i)) {
                RELEASE_ASSERT(m_useCounts.constant<FP>(i).u64x2[0] == referenceUseCounts.constant<FP>(i).u64x2[0]);
                RELEASE_ASSERT(m_useCounts.constant<FP>(i).u64x2[1] == referenceUseCounts.constant<FP>(i).u64x2[1]);
                RELEASE_ASSERT(m_useCounts.constantWidth<FP>(i) == referenceUseCounts.constantWidth<FP>(i));
            }
        }
    }

    void run()
    {
        m_stats[GP].numTmpsIn = m_code.numTmps(GP);
        m_stats[FP].numTmpsIn = m_code.numTmps(FP);

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " input IR:\n", m_code);

        // Many functions do not have any FPs. We can easily reduce the cost by skipping FP version of setup completely when we have zero FP tmps.
        bool hasFPTmps = m_code.numTmps(FP);
        dataLogLnIf(Options::airGreedyRegAllocVerbose(), "Greedy: ", m_code.proc().name(), " GP tmps=", m_code.numTmps(GP), " FP tmps=", m_code.numTmps(FP), " (FP path ", hasFPTmps ? "RUN" : "SKIPPED", ")");

        buildRegisterSets();
        buildIndices();
        buildLiveRanges(hasFPTmps);
        initSpillCosts<GP>();
        if (hasFPTmps)
            initSpillCosts<FP>();
        coalesceWithPinnedRegisters();
        coalesceTmps<GP>();
        if (hasFPTmps)
            coalesceTmps<FP>();
        rewriteCoalescedTmps();
#if USE(BUN_JSC_ADDITIONS)
        noteNeighbours();
#endif

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " state before allocate registers:\n", *this, "IR:\n", m_code);

        allocateRegisters<GP>();
        if (hasFPTmps)
            allocateRegisters<FP>();

        insertFixupCode();

        validateAssignments<GP>();
        if (hasFPTmps)
            validateAssignments<FP>();

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " about to assign registers:\n", *this, "IR:\n", m_code);

        assignRegisters();
        fixSpillsAfterTerminals(m_code);

        m_stats[GP].didSpill += m_didSpill[GP];
        m_stats[FP].didSpill += m_didSpill[FP];
        m_stats[GP].numTmpsOut = m_code.numTmps(GP);
        m_stats[FP].numTmpsOut = m_code.numTmps(FP);

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " output IR:\n", m_code);
    }

    bool shouldDumpFunction() const
    {
        const char8_t* filter = Options::airGreedyRegAllocDumpFunction();
        if (!filter)
            return false;
        return m_code.proc().name().find(String { filter }) != notFound;
    }

    void dump(PrintStream& out) const
    {
        out.println("usesSIMD=", m_code.usesSIMD());
        out.println("Block to Point:");
        for (BasicBlock* block : m_code)
            out.println("    BB", pointerDump(block), ": ", positionOfHead(block));
        out.println("RegRanges:");
        dumpRegRanges<GP>(out);
        dumpRegRanges<FP>(out);
        out.println("LiveRanges:");
        auto dumpRegTmpData = [&](Reg r) {
            const TmpData& tmpData = m_map[Tmp(r)];
            if (tmpData.liveRange.size())
                out.println("    ", r, ": ", m_map[Tmp(r)]);
        };
        for (Reg r : m_allowedRegistersInPriorityOrder[GP])
            dumpRegTmpData(r);
        m_code.pinnedRegisters().forEachReg([&](Reg r) {
            if (bankForReg(r) == GP)
                dumpRegTmpData(r);
        });
        for (Reg r : m_allowedRegistersInPriorityOrder[FP])
            dumpRegTmpData(r);
        m_code.pinnedRegisters().forEachReg([&](Reg r) {
            if (bankForReg(r) == FP)
                dumpRegTmpData(r);
        });
        m_code.forEachTmp([&](Tmp tmp) {
            out.println("    ", tmp, ": ", m_map[tmp], " useWidth=", m_tmpWidth.useWidth(tmp));
        });
        out.println("AroundClobbers splits:\n", listDump(m_aroundClobbersMetadata, "\n"));
        out.println("IntraBlock splits:\n", listDump(m_intraBlockMetadata, "\n"));
        out.println("AroundLoop splits:\n", listDump(m_aroundLoopMetadata, "\n"));
        out.println("SpillSlotTable: ", pointerListDump(m_spillSlotTable));
        out.println("Loops: ", pointerDump(m_naturalLoops.get()));
        out.println("Stats (GP):", m_stats[GP]);
        out.println("Stats (FP):", m_stats[FP]);
    }

private:
    template<Bank bank>
    void dumpRegRanges(PrintStream& out) const
    {
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (!m_regRanges[r].isEmpty())
                out.println("    ", r, ": ", m_regRanges[r]);
        }
        m_code.pinnedRegisters().forEachReg([&](Reg r) {
            if (bankForReg(r) == bank)
                out.println("    ", r, ": ", m_regRanges[r]);
        });
    }

    bool LEGACY_NODELETE shouldSpillEverything()
    {
        if (!Options::airGreedyRegAllocSpillsEverything())
            return false;

        // You're meant to hack this so that you selectively spill everything depending on reasons.
        // That's super useful for debugging.
        return true;
    }

    void buildRegisterSets()
    {
        forEachBank([&] (Bank bank) {
            m_allowedRegistersInPriorityOrder[bank] = m_code.regsInPriorityOrder(bank);
            for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
                m_allAllowedRegisters.add(r, IgnoreVectors);
                ASSERT(m_code.mutableRegs().contains(r, IgnoreVectors));
                ASSERT(!m_code.pinnedRegisters().contains(r, IgnoreVectors));
                ASSERT(!m_code.isPinned(r));
            }
        });
        ASSERT(m_allAllowedRegisters == m_code.mutableRegs().toScalarRegisterSet());
#if USE(BUN_JSC_ADDITIONS)
        if (m_code.chargesForCalleeSaves()) {
            RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
            calleeSaves.exclude(m_code.unsavedCalleeSaves());
            m_allAllowedRegisters.forEach([&](Reg reg) {
                if (!calleeSaves.contains(reg, IgnoreVectors))
                    return;
                m_freshCalleeSaves.add(reg);
                ++m_numberOfFreshCalleeSaves[reg.isGPR() ? GP : FP];
                ++m_numberOfCalleeSaves[reg.isGPR() ? GP : FP];
            });
        }
#endif
    }

    // The prologue saves, and every epilogue restores, a callee-saved register that anything is assigned to. The first to take it pays.
    bool mayTake(const TmpData& tmpData, Reg reg) const
    {
        return tmpData.paysForCalleeSave || !m_freshCalleeSaves.contains(reg);
    }

    void buildIndices()
    {
        Point headPosition = 0;
        Point tailPosition = 0;
        m_blockIndexOfInst.grow(m_mentions.numberOfInsts);
        unsigned* blockIndexOfInst = m_blockIndexOfInst.mutableSpan().data();
        for (size_t i = 0; i < m_code.size(); i++) {
            BasicBlock* block = m_code[i];
            if (!block) {
                m_tailPoints[i] = tailPosition;
                continue;
            }
            tailPosition = headPosition + block->size() * PointOffsets::PointsPerInst - 1;
            m_blockToHeadPoint[block] = headPosition;
            m_tailPoints[i] = tailPosition;
            headPosition += block->size() * PointOffsets::PointsPerInst;
            for (unsigned count = block->size(); count--;)
                *blockIndexOfInst++ = i;
        }
        RELEASE_ASSERT(blockIndexOfInst == m_blockIndexOfInst.mutableSpan().data() + m_blockIndexOfInst.size());

        if (m_allClobberedRegs) {
            m_firstClobberOfInst.grow(m_mentions.numberOfInsts);
            unsigned* firstClobberOfInst = m_firstClobberOfInst.mutableSpan().data();
            const Point* points = m_clobberPoints.span().data();
            const unsigned numberOfPoints = m_clobberPoints.size();
            unsigned next = 0;
            for (unsigned ordinal = 0; ordinal < m_mentions.numberOfInsts; ++ordinal) {
                while (next < numberOfPoints && points[next] < ordinal * PointOffsets::PointsPerInst)
                    ++next;
                firstClobberOfInst[ordinal] = next;
            }
            if (numberOfPoints >= minClobberPointsForChunks) {
                m_clobberedRegsInChunk.fill(0, (numberOfPoints + clobberPointsPerChunk - 1) / clobberPointsPerChunk);
                for (unsigned index = 0; index < numberOfPoints; ++index)
                    m_clobberedRegsInChunk[index / clobberPointsPerChunk] |= m_clobberedRegsAtPoint[index];
            }
        }
        m_stats[GP].numInsts += (tailPosition + 1) / PointOffsets::PointsPerInst;
    }

    void forEachBlockInLiveRange(const LiveRange& liveRange, NOESCAPE const Invocable<IterationStatus(BasicBlock*)> auto& func)
    {
        for (auto& interval : liveRange.intervals()) {
            size_t blockIdx = findBlockIndexContainingPoint(interval.begin());
            while (true) {
                BasicBlock* block = m_code[blockIdx];
                if (func(block) == IterationStatus::Done)
                    return;
                Point nextHead = m_tailPoints[blockIdx] + 1;
                if (nextHead >= interval.end())
                    break;
                do {
                    ++blockIdx;
                } while (!m_code[blockIdx]);
            };
        }
    }

    size_t findBlockIndexContainingPoint(Point point)
    {
        size_t blockIndex = m_blockIndexOfInst[point / PointOffsets::PointsPerInst];
        ASSERT(m_code[blockIndex]);
        ASSERT(positionOfHead(m_code[blockIndex]) <= point && point <= m_tailPoints[blockIndex]);
        return blockIndex;
    }

    BasicBlock* findBlockContainingPoint(Point point)
    {
        return m_code[findBlockIndexContainingPoint(point)];
    }

    Point LEGACY_NODELETE positionOfHead(BasicBlock* block) const
    {
        Point point = m_blockToHeadPoint[block];
        ASSERT(pointAtOffset(point, PointOffsets::Pre) == point);
        return point;
    }

    Point LEGACY_NODELETE positionOfTail(BasicBlock* block)
    {
        Point point = positionOfHead(block) + block->size() * PointOffsets::PointsPerInst - 1;
        ASSERT(pointAtOffset(point, PointOffsets::Post) == point);
        return point;
    }

    static size_t LEGACY_NODELETE instIndex(Point positionOfHead, Point point)
    {
        return (point - positionOfHead) / PointOffsets::PointsPerInst;
    }

    static Point LEGACY_NODELETE pointAtOffset(Point point, PointOffsets offset)
    {
        static_assert(!(PointOffsets::PointsPerInst & (PointOffsets::PointsPerInst - 1)));
        return (point & ~(PointOffsets::PointsPerInst - 1)) + offset;
    }

    static Point LEGACY_NODELETE positionOfEarly(Point positionOfHead, unsigned instIndex)
    {
        ASSERT(!(positionOfHead % PointOffsets::PointsPerInst));
        return positionOfHead + instIndex * PointOffsets::PointsPerInst + PointOffsets::Early;
    }

    static Point LEGACY_NODELETE positionOfLate(Point positionOfHead, unsigned instIndex)
    {
        ASSERT(!(positionOfHead % PointOffsets::PointsPerInst));
        return positionOfHead + instIndex * PointOffsets::PointsPerInst + PointOffsets::Late;
    }

    static Point LEGACY_NODELETE positionOfEarly(Interval interval)
    {
        return pointAtOffset(interval.begin(), PointOffsets::Early);
    }

    static Interval LEGACY_NODELETE earlyInterval(Point positionOfEarly)
    {
        ASSERT((positionOfEarly % PointOffsets::PointsPerInst) == PointOffsets::Early);
        return Interval(positionOfEarly);
    }

    static Interval LEGACY_NODELETE lateInterval(Point positionOfEarly)
    {
        ASSERT((positionOfEarly % PointOffsets::PointsPerInst) == PointOffsets::Early);
        return Interval(positionOfEarly + (PointOffsets::Late - PointOffsets::Early));
    }

    static Interval earlyAndLateInterval(Point positionOfEarly)
    {
        return earlyInterval(positionOfEarly) | lateInterval(positionOfEarly);
    }

    static Interval intervalForTiming(Point positionOfEarly, Arg::Timing timing)
    {
        switch (timing) {
        case Arg::OnlyEarly:
            return earlyInterval(positionOfEarly);
        case Arg::OnlyLate:
            return lateInterval(positionOfEarly);
        case Arg::EarlyAndLate:
            return earlyAndLateInterval(positionOfEarly);
        }
        ASSERT_NOT_REACHED();
        return Interval();
    }

    static Interval intervalForSpill(Point positionOfEarly, Arg::Role role)
    {
        Arg::Timing timing = Arg::timing(role);
        switch (timing) {
        case Arg::OnlyEarly:
            if (Arg::isAnyDef(role))
                return earlyAndLateInterval(positionOfEarly); // We have a spill store after this insn.
            return earlyInterval(positionOfEarly);
        case Arg::OnlyLate:
            if (Arg::isAnyUse(role))
                return earlyAndLateInterval(positionOfEarly); // We had a spill load before this insn.
            return lateInterval(positionOfEarly);
        case Arg::EarlyAndLate:
            return earlyAndLateInterval(positionOfEarly);
        }
        ASSERT_NOT_REACHED();
        return Interval();
    }

    template<Bank bank>
    bool isConstDef(Tmp tmp)
    {
        return m_useCounts.isConstDef<bank>(AbsoluteTmpMapper<bank>::absoluteIndex(tmp));
    }

    Reg LEGACY_NODELETE assignedReg(Tmp tmp)
    {
        return m_map[tmp].assigned;
    }

    // Returns the stack slot a Tmp should use if spilled. Otherwise, returns nullptr.
    template<Bank bank>
    StackSlot* LEGACY_NODELETE spillSlot(Tmp tmp)
    {
        TmpData& tmpData = m_map.get<bank>(tmp);
        if (tmpData.stage == Stage::Spilled) {
            StackSlot* slot = m_spillSlotTable[tmpData.spillSlotTableIndex];
            ASSERT(slot);
            return slot;
        }
        return nullptr;
    }

    StackSlot* LEGACY_NODELETE spillSlot(Tmp tmp)
    {
        ASSERT(tmp.isGP() || tmp.isFP());
        return tmp.isGP() ? spillSlot<GP>(tmp) : spillSlot<FP>(tmp);
    }

    void ensureSpillSlotTableEntry(TmpData& tmpData)
    {
        if (tmpData.spillSlotTableIndex)
            return;
        m_spillSlotTable.append(nullptr);
        tmpData.spillSlotTableIndex = m_spillSlotTable.size() - 1;
    }

    float LEGACY_NODELETE adjustedBlockFrequency(BasicBlock* block)
    {
        float freq = block->frequency();
        if (!m_fastBlocks.saw(block)) [[unlikely]]
            freq *= Options::rareBlockPenalty();
        return freq;
    }

    template<Bank bank>
    Width LEGACY_NODELETE widthForConflicts(Tmp tmp)
    {
        if constexpr (bank == GP)
            return Width64;
        ASSERT(bank == FP);
        // For FP, the top 64-bits of vector registers are checked for conflicts only when
        // those top 64-bits of the tmp are used. The low 64-bits are always checked.
        return m_tmpWidth.useWidth(tmp);
    }

    // Debug code to verify that the results of register allocation and finalization fixup is valid.
    // That is, no two Tmps simultaneously alive share the same register (unless they were coalesced).
    template<Bank bank>
    void validateAssignments()
    {
        if (!Options::airValidateGreedRegAlloc())
            return;
        bool anyFailures = false;

        auto fail = [&](BasicBlock* block, Tmp a, Tmp b) {
            dataLogLn("AIR GREEDY REGISTER ALLOCATION VALIDATION FAILURE");
            dataLogLn("   In BB", *block);
            dataLogLn("     tmp = ", a, " : ", m_map[a]);
            dataLogLn("     tmp = ", b, " : ", m_map[b]);
            anyFailures = true;
        };

        auto checkConflicts = [&](BasicBlock* block, const typename TmpLiveness<bank>::LocalCalc& localCalc) {
            for (Tmp a : localCalc.live()) {
                Reg aReg = assignedReg(a);
                if (!aReg)
                    continue;
                for (Tmp b : localCalc.live()) {
                    if (a == b)
                        continue;
                    Reg bReg = assignedReg(b);
                    // a & b interfere so b must either have been spilled or assigned a different register.
                    if (!bReg)
                        continue;
                    if (aReg == bReg) {
                        if (m_code.isPinned(aReg)) {
                            // It's okay if both Tmps were coalesced to the same pinned register.
                            TmpData& regData = m_map[Tmp(aReg)];
                            bool foundA = false, foundB = false;
                            for (auto& with : regData.coalescables) {
                                if (with.tmp == a)
                                    foundA = true;
                                else if (with.tmp == b)
                                    foundB = true;
                                if (foundA && foundB)
                                    break;
                            }
                            if (foundA && foundB)
                                continue;
                        }
                        fail(block, a, b);
                    }
                }
            }
        };

        TmpLiveness<bank> liveness(m_code);
        for (BasicBlock* block : m_code) {
            typename TmpLiveness<bank>::LocalCalc localCalc(liveness, block);
            for (unsigned instIndex = block->size(); instIndex--;) {
                checkConflicts(block, localCalc);
                localCalc.execute(instIndex);
            }
            checkConflicts(block, localCalc);
        }
        if (anyFailures) {
            dataLogLn("IR:\n", m_code);
            dataLogLn("State:\n", *this);
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void buildLiveRanges(bool hasFPTmps)
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::buildLiveRanges"_s);
        solveLiveness();

        Vector<Piece> pieceStore;
        unsigned capacityOfPieces = m_numberOfLinearIndices + m_numberOfLinearIndices / 2 + 8 * m_clobberPoints.size() + 16;
        pieceStore.grow(capacityOfPieces);
        Piece* pieces = pieceStore.mutableSpan().data();
        unsigned numberOfPieces = 0;
        Vector<StateOfRange> stateStore(FillWith { }, m_numberOfLinearIndices, StateOfRange { 0, noPiece });
        StateOfRange* states = stateStore.mutableSpan().data();
        constexpr uint8_t isPinnedFlag = 1;
        constexpr uint8_t mayBeCoalescedFlag = 2;
        constexpr uint8_t isRegFlag = 4;
        Vector<uint8_t> flagStore(FillWith { }, m_numberOfLinearIndices, static_cast<uint8_t>(0));
        uint8_t* flags = flagStore.mutableSpan().data();

        auto addMaybeCoalescable = [&](Tmp a, Tmp b, BasicBlock* block) {
            if (a == b)
                return;
            float freq = adjustedBlockFrequency(block);
            m_map[a].coalescables.add(b, freq);
            m_map[b].coalescables.add(a, freq);
            for (Tmp tmp : { a, b }) {
                uint8_t& flagsOfTmp = flags[linearIndex(tmp)];
                if (!(flagsOfTmp & mayBeCoalescedFlag))
                    m_tmpsWithCoalescables.append(tmp);
                flagsOfTmp |= mayBeCoalescedFlag;
            }
        };

        std::array<unsigned, Reg::maxIndex() + 1> linearIndexOfReg;
        for (unsigned i = 0; i <= Reg::maxIndex(); ++i) {
            linearIndexOfReg[i] = linearIndex(Tmp(Reg::fromIndex(i)));
            flags[linearIndexOfReg[i]] |= isRegFlag;
        }
        const Point* clobberPoints = m_clobberPoints.span().data();
        const uint64_t* clobberedRegsAtPoint = m_clobberedRegsAtPoint.span().data();
        const size_t numberOfClobberPoints = m_clobberPoints.size();
        size_t nextClobber = numberOfClobberPoints;
        m_deadClobbersAtPoint.fill(0, numberOfClobberPoints);
        uint64_t* deadClobbersAtPoint = m_deadClobbersAtPoint.mutableSpan().data();
        uint64_t activeRegs = 0;

        auto bitOfReg = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
            return static_cast<uint64_t>(1) << tmpForLinearIndex(index).reg().index();
        };
        auto takeDeadClobberAt = [&](uint64_t bit, Point point) ALWAYS_INLINE_LAMBDA {
            if (nextClobber == numberOfClobberPoints || clobberPoints[nextClobber] != point || !(deadClobbersAtPoint[nextClobber] & bit))
                return false;
            deadClobbersAtPoint[nextClobber] &= ~bit;
            return true;
        };
        auto markUse = [&](unsigned index, Point point) ALWAYS_INLINE_LAMBDA {
            Point& end = states[index].activeEnd;
            ASSERT(!end || point < end);
            if (!end) {
                end = point + 1;
                if (flags[index] & isRegFlag) [[unlikely]] {
                    uint64_t bit = bitOfReg(index);
                    activeRegs |= bit;
                    if (takeDeadClobberAt(bit, point + 1))
                        end = point + 2;
                }
            }
        };
        auto appendPiece = [&](Point begin, Point end, unsigned higher) ALWAYS_INLINE_LAMBDA {
            if (numberOfPieces == capacityOfPieces) [[unlikely]] {
                capacityOfPieces *= 2;
                pieceStore.grow(capacityOfPieces);
                pieces = pieceStore.mutableSpan().data();
            }
            pieces[numberOfPieces++] = Piece { begin, end, higher };
        };
        auto closeRange = [&](unsigned index, Point point) ALWAYS_INLINE_LAMBDA {
            StateOfRange& state = states[index];
            Point end = state.activeEnd;
            if (!end) [[unlikely]]
                end = point + 1;
            state.activeEnd = 0;
            unsigned lowest = state.lowestPiece;
            if (lowest != noPiece && pieces[lowest].begin == end) {
                pieces[lowest].begin = point;
                return;
            }
            ASSERT(lowest == noPiece || end < pieces[lowest].begin);
            state.lowestPiece = numberOfPieces;
            appendPiece(point, end, lowest);
        };
        auto markDef = [&](BasicBlock* block, unsigned instIndex, unsigned index, Point point) ALWAYS_INLINE_LAMBDA {
            uint8_t flagsOfTmp = flags[index];
            if ((flagsOfTmp & (isRegFlag | isPinnedFlag)) == isRegFlag) [[unlikely]] {
                uint64_t bit = bitOfReg(index);
                activeRegs &= ~bit;
                if (!states[index].activeEnd && takeDeadClobberAt(bit, point + 1))
                    states[index].activeEnd = point + 2;
            }
            if (!(flagsOfTmp & isPinnedFlag)) [[likely]]
                closeRange(index, point);
            if (flagsOfTmp & mayBeCoalescedFlag)
                pruneCoalescables(block->at(instIndex), tmpForLinearIndex(index), point, states, pieces);
        };

        for (unsigned ordinal : m_movesOfTmps) {
            BasicBlock* block = m_code[m_blockIndexOfInst[ordinal]];
            Inst& inst = block->at(ordinal - positionOfHead(block) / PointOffsets::PointsPerInst);
            if (!mayBeCoalescable(inst))
                continue;
            ASSERT(inst.args().size() == 2);
            if (inst.args()[0].isReg() || inst.args()[1].isReg()) {
                unsigned regIdx = inst.args()[0].isReg() ? 0 : 1;
                Reg reg = inst.args()[regIdx].reg();
                Tmp other = inst.args()[regIdx ^ 1].tmp();
                if (other.isReg())
                    continue;
                if (m_allAllowedRegisters.contains(reg, IgnoreVectors)) {
#if USE(BUN_JSC_ADDITIONS)
                    m_map[other].prefer(reg, adjustedBlockFrequency(block));
#else
                    if (!m_map[other].preferredReg)
                        m_map[other].preferredReg = inst.args()[regIdx].reg();
#endif
                    continue;
                }
                if (!m_code.isPinned(reg))
                    continue;
            }
            ASSERT(inst.args()[0].isTmp() && inst.args()[1].isTmp());
            addMaybeCoalescable(inst.args()[0].tmp(), inst.args()[1].tmp(), block);
        }

        Point funcEndPoint = m_tailPoints[m_code.size() - 1];
        m_code.pinnedRegisters().forEachReg([&](Reg reg) {
            unsigned index = linearIndex(Tmp(reg));
            flags[index] |= isPinnedFlag;
            markUse(index, funcEndPoint);
        });

        uint64_t regsToMark = ~static_cast<uint64_t>(0);
        if (!hasFPTmps)
            regsToMark = (static_cast<uint64_t>(1) << MacroAssembler::numGPRs) - 1;

        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* timings = m_mentions.traits.span().data();
        const unsigned* firstOfInst = m_mentions.firstOfInst.span().data();
        const uint8_t* timingsOfInsts = m_mentions.timingsOfInst.span().data();
        Point nextClobberPoint = nextClobber ? clobberPoints[nextClobber - 1] : std::numeric_limits<Point>::max();

        const size_t wordsPerSet = m_livenessIsInLists ? 0 : m_wordsPerSet;
        const uint64_t* liveAtHead = m_liveAtBoundaries.span().data();
        const uint64_t* liveAtTail = liveAtHead + static_cast<size_t>(m_code.size()) * wordsPerSet;
        size_t nextClose = m_closesBehindBlock.size();
        size_t nextOpen = m_opensAtTailOfBlock.size();
        auto forEachAtEdgeOf = [&](const Vector<AtEdgeOfBlock>& list, size_t& next, unsigned blockIndex, const auto& func) {
            size_t end = next;
            while (next && list[next - 1].blockIndex == blockIndex)
                --next;
            for (size_t i = next; i < end; ++i)
                func(list[i].index);
        };
        Vector<unsigned> liveAtHeadOfFirstBlock;
        forEachAtEdgeOf(m_closesBehindBlock, nextClose, m_code.size(), [&](unsigned index) {
            liveAtHeadOfFirstBlock.append(index);
        });
        const unsigned* indexOfColumn = m_indexOfColumn.span().data();
        auto forEachInDifference = [&](const uint64_t* a, const uint64_t* b, const auto& func) {
            for (size_t wordIndex = 0; wordIndex < wordsPerSet; ++wordIndex) {
                uint64_t word = a[wordIndex] & ~(b ? b[wordIndex] : 0);
                for (; word; word &= word - 1)
                    func(indexOfColumn[wordIndex * 64 + std::countr_zero(word)]);
            }
        };

        BasicBlock* blockAfter = nullptr;
        for (size_t blockIndex = m_code.size(); blockIndex--;) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;

            Point positionOfHead = this->positionOfHead(block);
            Point positionOfTail = this->positionOfTail(block);
            const uint64_t* tail = liveAtTail + blockIndex * wordsPerSet;

            if (blockAfter) {
                Point blockAfterPositionOfHead = this->positionOfHead(blockAfter);
                const uint64_t* headAfter = liveAtHead + blockAfter->index() * wordsPerSet;
                forEachInDifference(headAfter, tail, [&](unsigned index) {
                    if (states[index].activeEnd && !(flags[index] & isPinnedFlag)) [[likely]]
                        closeRange(index, blockAfterPositionOfHead);
                });
                forEachInDifference(tail, headAfter, [&](unsigned index) {
                    markUse(index, positionOfTail);
                });
            } else {
                forEachInDifference(tail, nullptr, [&](unsigned index) {
                    markUse(index, positionOfTail);
                });
            }
            if (m_livenessIsInLists) [[unlikely]] {
                forEachAtEdgeOf(m_closesBehindBlock, nextClose, blockIndex, [&](unsigned index) {
                    if (states[index].activeEnd && !(flags[index] & isPinnedFlag)) [[likely]]
                        closeRange(index, this->positionOfHead(blockAfter));
                });
                forEachAtEdgeOf(m_opensAtTailOfBlock, nextOpen, blockIndex, [&](unsigned index) {
                    markUse(index, positionOfTail);
                });
            }

            unsigned headOrdinal = positionOfHead / PointOffsets::PointsPerInst;
            for (unsigned instIndex = block->size(); instIndex--;) {
                Point positionOfEarly = this->positionOfEarly(positionOfHead, instIndex);
                Point positionOfLate = this->positionOfLate(positionOfHead, instIndex);
                unsigned begin = firstOfInst[headOrdinal + instIndex];
                unsigned end = firstOfInst[headOrdinal + instIndex + 1];
                uint8_t timingsOfInst = timingsOfInsts[headOrdinal + instIndex];

                if (timingsOfInst & Mentions::lateUse) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::lateUse)
                            markUse(tmps[i], positionOfLate);
                    }
                }
                if (timingsOfInst & Mentions::lateDef) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::lateDef)
                            markDef(block, instIndex, tmps[i], positionOfLate);
                    }
                }
                if (nextClobberPoint == positionOfLate) [[unlikely]] {
                    uint64_t regs = clobberedRegsAtPoint[--nextClobber] & regsToMark;
                    nextClobberPoint = nextClobber ? clobberPoints[nextClobber - 1] : std::numeric_limits<Point>::max();
                    deadClobbersAtPoint[nextClobber] = regs & ~activeRegs;
                    m_regsWithDeadClobbers |= regs & ~activeRegs;
                    for (regs &= activeRegs; regs; regs &= regs - 1)
                        markDef(block, instIndex, linearIndexOfReg[std::countr_zero(regs)], positionOfLate);
                }
                if (timingsOfInst & Mentions::earlyUse) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::earlyUse)
                            markUse(tmps[i], positionOfEarly);
                    }
                }
                if (timingsOfInst & Mentions::earlyDef) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::earlyDef)
                            markDef(block, instIndex, tmps[i], positionOfEarly);
                    }
                }
                if (nextClobberPoint == positionOfEarly) [[unlikely]] {
                    uint64_t regs = clobberedRegsAtPoint[nextClobber - 1] & regsToMark;
                    uint64_t deadClobbers = 0;
                    for (; regs; regs &= regs - 1) {
                        unsigned index = linearIndexOfReg[std::countr_zero(regs)];
                        const StateOfRange& state = states[index];
                        uint64_t bit = regs & -regs;
                        bool meetsWhatIsAbove = (nextClobber < numberOfClobberPoints && clobberPoints[nextClobber] == positionOfLate && (deadClobbersAtPoint[nextClobber] & bit))
                            || (state.lowestPiece != noPiece && pieces[state.lowestPiece].begin == positionOfLate);
                        if (!(activeRegs & bit) && !meetsWhatIsAbove)
                            deadClobbers |= bit;
                        else
                            markDef(block, instIndex, index, positionOfEarly);
                    }
                    deadClobbersAtPoint[--nextClobber] = deadClobbers;
                    m_regsWithDeadClobbers |= deadClobbers;
                    nextClobberPoint = nextClobber ? clobberPoints[nextClobber - 1] : std::numeric_limits<Point>::max();
                }
            }
#if ASSERT_ENABLED
            if (!m_livenessIsInLists) {
                Vector<bool> isLiveAtHead(FillWith { }, m_numberOfLinearIndices, false);
                forEachInDifference(liveAtHead + blockIndex * wordsPerSet, nullptr, [&](unsigned index) {
                    isLiveAtHead[index] = true;
                });
                m_code.forEachTmp([&](Tmp tmp) {
                    ASSERT(!!states[linearIndex(tmp)].activeEnd == isLiveAtHead[linearIndex(tmp)]);
                });
                m_code.pinnedRegisters().forEachReg([&](Reg reg) {
                    ASSERT(states[linearIndex(Tmp(reg))].activeEnd);
                });
            }
#endif
            blockAfter = block;
        }
        RELEASE_ASSERT(!nextClobber);
        if (blockAfter) {
            Point firstBlockPositionOfHead = this->positionOfHead(blockAfter);
            forEachInDifference(liveAtHead + blockAfter->index() * wordsPerSet, nullptr, [&](unsigned index) {
                if (!(flags[index] & isPinnedFlag))
                    closeRange(index, firstBlockPositionOfHead);
            });
            for (unsigned index : liveAtHeadOfFirstBlock) {
                if (!(flags[index] & isPinnedFlag))
                    closeRange(index, firstBlockPositionOfHead);
            }
        }
        m_code.pinnedRegisters().forEachReg([&](Reg reg) {
            unsigned index = linearIndex(Tmp(reg));
            ASSERT(states[index].activeEnd == funcEndPoint + 1 && states[index].lowestPiece == noPiece);
            states[index].lowestPiece = numberOfPieces;
            appendPiece(0, states[index].activeEnd, noPiece);
        });

#if ASSERT_ENABLED
        m_code.forEachTmp([&](Tmp tmp) {
            ASSERT(!states[linearIndex(tmp)].activeEnd);
        });
#endif

        Interval* const allIntervals = m_intervalArena.allocate(numberOfPieces);
        Interval* nextInterval = allIntervals;
        const Piece* allPieces = pieces;

        for (unsigned index = 0; index < m_numberOfLinearIndices; ++index) {
            unsigned piece = states[index].lowestPiece;
            if (piece == noPiece)
                continue;
            Interval* first = nextInterval;
            size_t size = 0;
            do {
                const Piece& current = allPieces[piece];
                *nextInterval++ = Interval(current.begin, current.end);
                size += current.end - current.begin;
                piece = current.higher;
            } while (piece != noPiece);
            m_map[tmpForLinearIndex(index)].liveRange = LiveRange(first, nextInterval - first, size);
        }
        m_intervalArena.tryResizeInPlace(allIntervals, numberOfPieces, nextInterval - allIntervals);

        m_liveAtBoundaries.clear();
        m_closesBehindBlock.clear();
        m_opensAtTailOfBlock.clear();
        m_indexOfColumn.clear();
        m_crossesBoundary.clear();
        m_exposedUses.clear();
        m_defsInBlocks.clear();
        m_usesAtTails.clear();
        m_endsOfBlock.clear();
    }

    NEVER_INLINE void pruneCoalescables(Inst& inst, Tmp def, Point point, const StateOfRange* states, const Piece* pieces)
    {
        TmpData& defData = m_map[def];
        if (defData.coalescables.isEmpty())
            return;
        Tmp movSrc = mayBeCoalescable(inst) ? inst.args()[0].tmp() : Tmp();
        defData.coalescables.removeAllMatching([&](Tmp tmp) {
            ASSERT(tmp != def);
            if (tmp == movSrc)
                return false;
            const StateOfRange& state = states[linearIndex(tmp)];
            if (!state.activeEnd) {
                if (state.lowestPiece == noPiece)
                    return false;
                const Piece& piece = pieces[state.lowestPiece];
                if (point < piece.begin || point >= piece.end)
                    return false;
            }
            dataLogLnIf(verbose(), "Pruning affinity ", def, " ", tmp);
            m_map[tmp].coalescables.remove(def);
            return true;
        });
    }

    Inst& instAtOrdinal(unsigned ordinal)
    {
        BasicBlock* block = m_code[m_blockIndexOfInst[ordinal]];
        return block->at(ordinal - positionOfHead(block) / PointOffsets::PointsPerInst);
    }

    bool cannotBeSpilledInPlace(Tmp tmp, Interval interval)
    {
        unsigned last = (interval.end() - 1) / PointOffsets::PointsPerInst;
        for (unsigned ordinal = interval.begin() / PointOffsets::PointsPerInst; ordinal <= last; ++ordinal) {
            Inst& inst = instAtOrdinal(ordinal);
            for (Arg& arg : inst.args()) {
                if (arg.isTmp()) {
                    if (arg.tmp() == tmp && !inst.admitsStack(arg))
                        return true;
                    continue;
                }
                bool namesIt = false;
                arg.forEachTmpFast([&](Tmp& named) {
                    namesIt |= named == tmp;
                });
                if (namesIt)
                    return true;
            }
        }
        return false;
    }

    void coalesceWithPinnedRegisters()
    {
        // If a Tmp is in a pinned register's coalescables set, that means the
        // Tmp's defs are always moves from the pinned register. Since no other
        // Tmps can be assigned to the pinned register, might as well assign
        // the coalescable tmps to the pinned register upfront.
        // Doing this eagerly could be the wrong decision if these Tmps are
        // coalescable with other Tmps, but that doesn't seem to happen in practice.
        m_code.pinnedRegisters().forEachReg([&](Reg reg) {
            Tmp tmp = Tmp(reg);
            TmpData& data = m_map[tmp];
            for (auto& with : data.coalescables) {
                ASSERT(!with.tmp.isReg());
                coalesceWithPinned(with.tmp, m_map[with.tmp], reg);
                m_stats[tmp.bank()].numCoalescedPinned++;
            }
        });
    }

    // Maps intervals to lists of Tmps live during that interval.
    //
    // Example: Tmp A live [0,10), Tmp B live [5,15):
    //   [0, 5)  -> {A}
    //   [5, 10) -> {A, B}
    //   [10,15) -> {B}
    template<Bank bank>
    class LivenessMap {
        WTF_MAKE_NONCOPYABLE(LivenessMap);
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LivenessMap);
    public:
        using TmpList = Vector<Tmp, 4>;

        LivenessMap()
        : m_singletonScratch({ Tmp() })
        {
        }
        LivenessMap(LivenessMap&&) = default;
        LivenessMap& operator=(LivenessMap&&) = default;

        void add(Tmp tmp, const LiveRange& range)
        {
            EncodedTmpList encodedList = encodeSingleton(tmp);
            for (auto& interval : range.intervals())
                addInterval(interval, encodedList);
        }

        void merge(LivenessMap&& other)
        {
            if (other.m_numIntervals > m_numIntervals)
                std::swap(*this, other);

            // Bulk-move other's multi-element TmpLists into ours, recording the base offset
            // so we can translate EncodedTmpList indices.
            size_t baseIndex = m_tmpLists.size();
            m_tmpLists.appendVector(WTF::move(other.m_tmpLists));

            for (auto [interval, encodedList] : other.m_intervals) {
                EncodedTmpList translated = isSingleton(encodedList) ?
                    encodedList : encodeIndex(decodeIndex(encodedList) + baseIndex);
                addInterval(interval, translated);
            }
            other = LivenessMap();
        }

        // Calls func with the TmpList for any intervals in this map that overlaps with the given range.
        IterationStatus forEachOverlap(const LiveRange& range, NOESCAPE const Invocable<IterationStatus(const TmpList&)> auto& func) const
        {
            for (auto interval : range.intervals()) {
                while (true) {
                    auto entry = m_intervals.find(interval);
                    if (!entry)
                        break;
                    auto [overlappingInterval, overlappingList] = *entry;
                    if (func(decodeTmpList(overlappingList)) == IterationStatus::Done)
                        return IterationStatus::Done;
                    if (interval.end() <= overlappingInterval.end())
                        break;
                    interval = { overlappingInterval.end(), interval.end() };
                }
            }
            return IterationStatus::Continue;
        }

        // Calls func with both TmpLists for each pair of overlapping intervals in a and b.
        static IterationStatus forEachPairwiseOverlap(const LivenessMap& a, const LivenessMap& b, NOESCAPE const Invocable<IterationStatus(const TmpList&, const TmpList&)> auto& func)
        {
            const LivenessMap* smaller = &a;
            const LivenessMap* larger = &b;
            if (a.m_numIntervals > b.m_numIntervals)
                std::swap(smaller, larger);

            for (auto [interval, encodedList] : smaller->m_intervals) {
                const auto& outerList = smaller->decodeTmpList(encodedList);
                while (true) {
                    auto entry = larger->m_intervals.find(interval);
                    if (!entry)
                        break;
                    auto [overlapInterval, overlapEncodedList] = *entry;
                    const auto& innerList = larger->decodeTmpList(overlapEncodedList);
                    if (func(outerList, innerList) == IterationStatus::Done)
                        return IterationStatus::Done;
                    if (interval.end() <= overlapInterval.end())
                        break;
                    interval = { overlapInterval.end(), interval.end() };
                }
            }
            return IterationStatus::Continue;
        }

        LiveRange buildLiveRange(IntervalArena& arena) const
        {
            LiveRange result;
            Interval current = { };
            for (auto [interval, listIdx] : m_intervals) {
                UNUSED_PARAM(listIdx);
                if (!current)
                    current = interval;
                else if (interval.begin() <= current.end())
                    current |= interval;
                else {
                    result.append(arena, current);
                    current = interval;
                }
            }
            if (current)
                result.append(arena, current);
            return result;
        }

    private:
        // EncodedTmpList is either a Tmp or an index into m_tmpLists.
        struct EncodedTmpList {
            uint32_t m_value { 0 };

            void dump(PrintStream& out) const
            {
                if (m_value & isIndexBit)
                    out.print("list#", m_value & (isIndexBit - 1));
                else
                    out.print(Tmp::tmpForIndex(bank, m_value));
            }
        };

        static constexpr uint32_t isIndexBit = 1u << 31;
        static constexpr uint32_t indexMask = isIndexBit - 1;

        static bool LEGACY_NODELETE isSingleton(EncodedTmpList idx) { return !(idx.m_value & isIndexBit); }

        static EncodedTmpList LEGACY_NODELETE encodeSingleton(Tmp tmp)
        {
            unsigned tIdx = tmp.tmpIndex();
            ASSERT(tIdx < isIndexBit);
            return { tIdx };
        }

        static Tmp LEGACY_NODELETE decodeSingleton(EncodedTmpList encoded)
        {
            ASSERT(isSingleton(encoded));
            return Tmp::tmpForIndex(bank, encoded.m_value);
        }

        static EncodedTmpList LEGACY_NODELETE encodeIndex(size_t index)
        {
            ASSERT(index <= indexMask);
            return { isIndexBit | static_cast<uint32_t>(index) };
        }

        static unsigned LEGACY_NODELETE decodeIndex(EncodedTmpList encoded)
        {
            ASSERT(!isSingleton(encoded));
            return encoded.m_value & indexMask;
        }

        const TmpList& LEGACY_NODELETE decodeTmpList(EncodedTmpList encodedList) const
        {
            if (isSingleton(encodedList)) [[likely]] {
                m_singletonScratch[0] = decodeSingleton(encodedList);
                return m_singletonScratch;
            }
            return m_tmpLists[decodeIndex(encodedList)];
        }

        void insertInterval(const Interval& interval, EncodedTmpList listIdx)
        {
            m_intervals.insert(interval, listIdx);
            m_numIntervals++;
        }

        void eraseInterval(const Interval& interval)
        {
            m_intervals.erase(interval);
            m_numIntervals--;
        }

        void addInterval(Interval interval, EncodedTmpList tmpList)
        {
            while (true) {
                auto entry = m_intervals.find(interval);

                if (!entry) {
                    // No overlap: insert remainder.
                    insertInterval(interval, tmpList);
                    break;
                }

                auto [overlapInterval, overlapList] = *entry;
                // Gap before the overlapping interval.
                if (interval.begin() < overlapInterval.begin())
                    insertInterval({ interval.begin(), overlapInterval.begin() }, tmpList);

                // Erase the existing interval; we'll re-insert split pieces.
                eraseInterval(overlapInterval);

                // Part before our interval keeps the original list.
                if (overlapInterval.begin() < interval.begin())
                    insertInterval({ overlapInterval.begin(), interval.begin() }, overlapList);

                // Overlapping part gets both lists combined.
                Interval combined = { std::max(interval.begin(), overlapInterval.begin()),
                    std::min(interval.end(), overlapInterval.end()) };
                insertInterval(combined, concatLists(overlapList, tmpList));

                if (interval.end() <= overlapInterval.end()) {
                    // Part after overlap keeps the original list.
                    if (interval.end() < overlapInterval.end())
                        insertInterval({ interval.end(), overlapInterval.end() }, overlapList);
                    break;
                }
                interval = { overlapInterval.end(), interval.end() };
            }
        }

        EncodedTmpList concatLists(EncodedTmpList a, EncodedTmpList b)
        {
            TmpList newList = decodeTmpList(a);
            if (isSingleton(b))
                newList.append(decodeSingleton(b));
            else
                newList.appendVector(m_tmpLists[decodeIndex(b)]);
            EncodedTmpList result = encodeIndex(m_tmpLists.size());
            m_tmpLists.append(WTF::move(newList));
            return result;
        }

        static constexpr unsigned cacheLinesPerNode = 3;
        using LivenessIntervalSet = IntervalSet<Point, EncodedTmpList, cacheLinesPerNode>;

        LivenessIntervalSet m_intervals;
        Vector<TmpList> m_tmpLists;
        mutable TmpList m_singletonScratch;
        size_t m_numIntervals { 0 };
    };

    // Represents a group of coalescable Tmps with their combined liveness information. Tmps within
    // the group can have overlapping live-ranges as long as they are in each other's coalescables list.
    template<Bank bank>
    class AffinityGroup {
        WTF_MAKE_NONCOPYABLE(AffinityGroup);
    public:
        static constexpr unsigned maxSizeWithoutMap = 24;

        AffinityGroup(GreedyAllocator& allocator, Tmp tmp0, Tmp tmp1)
            : m_range(LiveRange::unionOf(allocator.m_intervalArena, allocator.m_map.template get<bank>(tmp0).liveRange, allocator.m_map.template get<bank>(tmp1).liveRange))
        {
            reserve(allocator, 4);
            m_members[0] = tmp0;
            m_members[1] = tmp1;
            m_size = 2;
        }
        AffinityGroup(AffinityGroup&&) = default;
        AffinityGroup& operator=(AffinityGroup&&) = default;

        void addMember(GreedyAllocator& allocator, Tmp tmp)
        {
            reserve(allocator, m_size + 1);
            m_members[m_size++] = tmp;
            const LiveRange& range = allocator.m_map.template get<bank>(tmp).liveRange;
            if (m_liveness) {
                m_liveness->add(tmp, range);
                return;
            }
            m_range = LiveRange::unionOf(allocator.m_intervalArena, m_range, range);
            makeMapIfLarge(allocator);
        }

        // Merges the other group into this one.
        void merge(GreedyAllocator& allocator, AffinityGroup& other)
        {
            if (m_liveness || other.m_liveness) {
                ensureMap(allocator);
                other.ensureMap(allocator);
                m_liveness->merge(WTF::move(*other.m_liveness));
                other.m_liveness = nullptr;
            } else
                m_range = LiveRange::unionOf(allocator.m_intervalArena, m_range, other.m_range);
            other.m_range = LiveRange();
            reserve(allocator, m_size + other.m_size);
            for (Tmp member : other.members())
                m_members[m_size++] = member;
            other.m_size = 0;
            if (!m_liveness)
                makeMapIfLarge(allocator);
        }

        std::span<const Tmp> LEGACY_NODELETE members() const { return { m_members, m_size }; }
        size_t LEGACY_NODELETE size() const { return m_size; }
        bool LEGACY_NODELETE isEmpty() const { return !m_size; }
        bool LEGACY_NODELETE hasMap() const { return !!m_liveness; }
        const LiveRange& LEGACY_NODELETE range() const { ASSERT(!hasMap()); return m_range; }

        LiveRange buildLiveRange(IntervalArena& arena)
        {
            if (m_liveness)
                return m_liveness->buildLiveRange(arena);
            return WTF::move(m_range);
        }

        void ensureMap(GreedyAllocator& allocator)
        {
            if (m_liveness)
                return;
            m_liveness = makeUnique<LivenessMap<bank>>();
            for (Tmp member : members())
                m_liveness->add(member, allocator.m_map.template get<bank>(member).liveRange);
            m_range = LiveRange();
        }

        using TmpList = typename LivenessMap<bank>::TmpList;

        IterationStatus forEachOverlap(const LiveRange& range, NOESCAPE const Invocable<IterationStatus(const TmpList&)> auto& func) const
        {
            return m_liveness->forEachOverlap(range, func);
        }

        static IterationStatus forEachPairwiseOverlap(const AffinityGroup& a, const AffinityGroup& b, NOESCAPE const Invocable<IterationStatus(const TmpList&, const TmpList&)> auto& func)
        {
            return LivenessMap<bank>::forEachPairwiseOverlap(*a.m_liveness, *b.m_liveness, func);
        }

        void dump(PrintStream& out) const
        {
            if (m_representative)
                out.print(m_representative, " <= ");
            out.print("{ ", listDump(members()), " }");
        }

        Tmp m_representative; // Only set for non-empty groups at finalization

    private:
        void makeMapIfLarge(GreedyAllocator& allocator)
        {
            if (m_size > maxSizeWithoutMap || m_range.intervals().size() > maxSizeWithoutMap) [[unlikely]]
                ensureMap(allocator);
        }

        void reserve(GreedyAllocator& allocator, unsigned size)
        {
            if (size <= m_capacity) [[likely]]
                return;
            static_assert(sizeof(Interval) == 2 * sizeof(Tmp));
            unsigned capacity = std::max(size, m_capacity * 2);
            capacity += capacity & 1;
            Tmp* members = reinterpret_cast<Tmp*>(allocator.m_intervalArena.allocate(capacity / 2));
            for (unsigned i = 0; i < m_size; ++i)
                members[i] = m_members[i];
            m_members = members;
            m_capacity = capacity;
        }

        Tmp* m_members { nullptr };
        unsigned m_size { 0 };
        unsigned m_capacity { 0 };
        LiveRange m_range;
        std::unique_ptr<LivenessMap<bank>> m_liveness;
    };

    using GroupIndex = uint32_t;
    template<Bank bank>
    using TmpGroupMap = IndexMap<Tmp::Indexed<bank>, GroupIndex>;
    static constexpr GroupIndex noGroup = std::numeric_limits<GroupIndex>::max();

    template<Bank bank>
    void coalesceSingletons(Tmp tmp0, Tmp tmp1, Vector<AffinityGroup<bank>>& groups, TmpGroupMap<bank>& tmpToGroup)
    {
        ASSERT(m_map.get<bank>(tmp0).coalescables.template contains<bank>(tmp1) && m_map.get<bank>(tmp1).coalescables.template contains<bank>(tmp0));
        auto newIndex = groups.size();
        groups.constructAndAppend(*this, tmp0, tmp1);
        tmpToGroup[tmp0] = newIndex;
        tmpToGroup[tmp1] = newIndex;
        dataLogLnIf(verbose(), "Created group ", newIndex, ": ", groups[newIndex]);
    }

    template<Bank bank>
    bool tryCoalesceSingletonWithGroup(Tmp singleton, GroupIndex groupIndex, Vector<AffinityGroup<bank>>& groups, TmpGroupMap<bank>& tmpToGroup)
    {
        const auto& group = groups[groupIndex];
        TmpData& singletonData = m_map.get<bank>(singleton);

        bool conflict = false;
        if (!group.hasMap()) {
            if (group.range().overlaps(singletonData.liveRange)) {
                for (Tmp member : group.members()) {
                    if (m_map.get<bank>(member).liveRange.overlaps(singletonData.liveRange) && !singletonData.coalescables.template contains<bank>(member)) {
                        conflict = true;
                        break;
                    }
                }
            }
        } else group.forEachOverlap(singletonData.liveRange, [&](const auto& tmpList) {
            if (tmpList.size() > singletonData.coalescables.size()) {
                conflict = true; // Pigeonhole principle
                return IterationStatus::Done;
            }
            for (Tmp member : tmpList) {
                if (!singletonData.coalescables.contains<bank>(member)) {
                    conflict = true;
                    return IterationStatus::Done;
                }
            }
            return IterationStatus::Continue;
        });
        if (conflict)
            return false;

        groups[groupIndex].addMember(*this, singleton);
        tmpToGroup[singleton] = groupIndex;
        dataLogLnIf(verbose(), "Added ", singleton, " to group ", groupIndex);
        return true;
    }

    template<Bank bank>
    bool tryCoalesceGroups(GroupIndex groupIndex0, GroupIndex groupIndex1, Vector<AffinityGroup<bank>>& groups, TmpGroupMap<bank>& tmpToGroup)
    {
        auto& group0 = groups[groupIndex0];
        auto& group1 = groups[groupIndex1];

        bool conflict = false;
        if (!group0.hasMap() && !group1.hasMap()) {
            if (group0.range().overlaps(group1.range())) {
                for (Tmp tmpA : group0.members()) {
                    const TmpData& dataA = m_map.get<bank>(tmpA);
                    if (!dataA.liveRange.overlaps(group1.range()))
                        continue;
                    for (Tmp tmpB : group1.members()) {
                        if (dataA.liveRange.overlaps(m_map.get<bank>(tmpB).liveRange) && !dataA.coalescables.template contains<bank>(tmpB)) {
                            conflict = true;
                            break;
                        }
                    }
                    if (conflict)
                        break;
                }
            }
        } else {
            group0.ensureMap(*this);
            group1.ensureMap(*this);
        }
        if (group0.hasMap()) AffinityGroup<bank>::forEachPairwiseOverlap(group0, group1, [&](const auto& tmpListA, const auto& tmpListB) {
            for (Tmp tmpA : tmpListA) {
                const Coalescables& coalescables = m_map.get<bank>(tmpA).coalescables;
                if (tmpListB.size() > coalescables.size()) {
                    conflict = true; // Pigeonhole principle
                    return IterationStatus::Done;
                }
                for (Tmp tmpB : tmpListB) {
                    if (!coalescables.contains<bank>(tmpB)) {
                        conflict = true;
                        return IterationStatus::Done;
                    }
                }
            }
            return IterationStatus::Continue;
        });
        if (conflict)
            return false;

        for (Tmp member : groups[groupIndex1].members())
            tmpToGroup[member] = groupIndex0;
        groups[groupIndex0].merge(*this, groups[groupIndex1]);
        dataLogLnIf(verbose(), "Merged group ", groupIndex1, " into ", groupIndex0);
        return true;
    }

    template<Bank bank>
    void coalesceTmps()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::coalesceTmps"_s);

        Vector<AffinityGroup<bank>> groups;
        TmpGroupMap<bank> tmpToGroup(Tmp::indexEnd(m_code, bank), noGroup);

        buildCoalescingGroups<bank>(groups, tmpToGroup);
        validateCoalescing<bank>(groups, tmpToGroup);
        createGroupRepresentatives<bank>(groups);

        if (m_representatives.isEmpty())
            m_representatives.fill(0, m_numberOfLinearIndices);
        for (auto& group : groups) {
            for (Tmp member : group.members())
                m_representatives[linearIndex(member)] = group.m_representative.internalValue();
        }
    }

    template<Bank bank>
    void buildCoalescingGroups(Vector<AffinityGroup<bank>>& groups, TmpGroupMap<bank>& tmpToGroup)
    {
        struct Move {
            Tmp tmp0, tmp1;
            float cost;

            void dump(PrintStream& out) const
            {
                out.print(tmp0, ", ", tmp1, " ", cost);
            }
        };
        Vector<Move> moves;

        for (Tmp tmp : m_tmpsWithCoalescables) {
            if (tmp.isReg() || tmp.bank() != bank)
                continue;
            TmpData& data = m_map.get<bank>(tmp);
            if (data.stage != Stage::New) {
                ASSERT(assignedReg(tmp) && m_code.isPinned(assignedReg(tmp)));
                continue; // Already coalesced with a pinned register
            }
            // Sort coalescables for binary search in Coalescables::contains.
            data.coalescables.sort<bank>();
            for (auto& with : data.coalescables) {
                if (tmp.tmpIndex(bank) < with.tmp.tmpIndex(bank))
                    moves.append({ tmp, with.tmp, with.moveCost });
            }
        }

        std::ranges::sort(moves, [](auto& a, auto& b) {
            if (a.cost != b.cost)
                return a.cost > b.cost;
            if (a.tmp0.tmpIndex(bank) != b.tmp0.tmpIndex(bank))
                return a.tmp0.tmpIndex(bank) < b.tmp0.tmpIndex(bank);
            ASSERT_IMPLIES(&a != &b, a.tmp1.tmpIndex(bank) != b.tmp1.tmpIndex(bank));
            return a.tmp1.tmpIndex(bank) < b.tmp1.tmpIndex(bank);
        });

        for (Move& move : moves) {
            dataLogLnIf(verbose(), "Processing move: ", move);
            auto groupIndex0 = tmpToGroup[move.tmp0];
            auto groupIndex1 = tmpToGroup[move.tmp1];

            if (groupIndex0 != noGroup && groupIndex0 == groupIndex1) {
                dataLogLnIf(verbose(), "Already grouped");
                continue;
            }

            if (groupIndex0 == noGroup && groupIndex1 == noGroup)
                coalesceSingletons<bank>(move.tmp0, move.tmp1, groups, tmpToGroup);
            else if (groupIndex0 != noGroup && groupIndex1 != noGroup)
                tryCoalesceGroups<bank>(groupIndex0, groupIndex1, groups, tmpToGroup);
            else {
                auto groupIndex = groupIndex0 != noGroup ? groupIndex0 : groupIndex1;
                Tmp singleton = groupIndex0 != noGroup ? move.tmp1 : move.tmp0;
                tryCoalesceSingletonWithGroup<bank>(singleton, groupIndex, groups, tmpToGroup);
            }
        }
    }

    // Validate that coalescing groups are correct: at every non-move def of a group member,
    // no other member of the same group should be live. This ensures simultaneously-live
    // group members always hold the same value (connected by moves), so replacing them with
    // a single representative is semantically correct.
    template<Bank bank>
    void validateCoalescing(const Vector<AffinityGroup<bank>>& groups, const TmpGroupMap<bank>& tmpToGroup)
    {
        if (!Options::airValidateGreedRegAlloc())
            return;
        bool anyFailures = false;

        auto isSameGroupMove = [&](Inst& inst) {
            if (!mayBeCoalescable(inst))
                return false;
            Tmp src = inst.args()[0].tmp();
            Tmp dst = inst.args()[1].tmp();
            if (src.isReg() || dst.isReg() || src.bank() != bank || dst.bank() != bank)
                return false;
            auto srcGroup = tmpToGroup[src];
            return srcGroup != noGroup && srcGroup == tmpToGroup[dst];
        };

        auto checkDefs = [&](BasicBlock* block, unsigned instIndex, const typename TmpLiveness<bank>::LocalCalc& localCalc, bool earlyDefs) {
            Inst& inst = block->at(instIndex);
            if (isSameGroupMove(inst))
                return;
            inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank b, Width) {
                if (b != bank || tmp.isReg() || !Arg::isAnyDef(role))
                    return;
                if (earlyDefs ? !Arg::isEarlyDef(role) : !Arg::isLateDef(role))
                    return;
                auto groupIdx = tmpToGroup[tmp];
                if (groupIdx == noGroup)
                    return;
                for (Tmp live : localCalc.live()) {
                    if (live == tmp || live.isReg())
                        continue;
                    if (tmpToGroup[live] == groupIdx) {
                        dataLogLn("AIR GREEDY COALESCING VALIDATION FAILURE");
                        dataLogLn("   In BB", *block, " at inst ", instIndex, ": ", inst);
                        dataLogLn("   Non-move ", earlyDefs ? "early" : "late", " def of group member ", tmp);
                        dataLogLn("   But group member ", live, " is simultaneously live");
                        dataLogLn("   Group: ", groups[groupIdx]);
                        anyFailures = true;
                    }
                }
            });
        };

        TmpLiveness<bank> liveness(m_code);
        for (BasicBlock* block : m_code) {
            typename TmpLiveness<bank>::LocalCalc localCalc(liveness, block);
            for (unsigned instIndex = block->size(); instIndex--;) {
                // Before execute: live set at boundary instIndex+1. Check late defs.
                checkDefs(block, instIndex, localCalc, false);
                localCalc.execute(instIndex);
                // After execute: live set at boundary instIndex. Check early defs.
                checkDefs(block, instIndex, localCalc, true);
            }
        }
        if (anyFailures) {
            dataLogLn("IR:\n", m_code);
            dataLogLn("State:\n", *this);
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    template<Bank bank>
    void createGroupRepresentatives(Vector<AffinityGroup<bank>>& groups)
    {
        for (auto& group : groups) {
            if (group.isEmpty())
                continue;

            m_stats[bank].numGroupsCreated++;
            m_stats[bank].maxGroupSize = std::max(m_stats[bank].maxGroupSize, static_cast<unsigned>(group.size()));

            Tmp representative = m_code.newTmp(bank);
            Width useWidth = Width8;
            Width defWidth = Width8;
            UseDefCost cost(0);
            Reg preferred;
            bool hasColdUse = false;
#if USE(BUN_JSC_ADDITIONS)
            TmpData preferences;
#endif
            for (Tmp member : group.members()) {
                m_stats[bank].numGroupTmpsCoalesced++;
                if (isConstDef<bank>(member))
                    m_stats[bank].numGroupConstDefMerged++;
                TmpData& memberData = m_map.get<bank>(member);
                useWidth = std::max(useWidth, m_tmpWidth.useWidth(member));
                defWidth = std::max(defWidth, m_tmpWidth.defWidth(member));
                cost += memberData.useDefCost;
                if (!preferred)
                    preferred = memberData.preferredReg;
                hasColdUse |= memberData.hasColdUse;
#if USE(BUN_JSC_ADDITIONS)
                preferences.prefer(memberData.preferredReg, memberData.preferredWeight);
                preferences.prefer(memberData.secondPreferredReg, memberData.secondPreferredWeight);
#endif
                memberData.stage = Stage::Coalesced;
            }
            m_tmpWidth.setWidths(representative, useWidth, defWidth);

            m_map.append(representative, TmpData());
            TmpData& representativeData = m_map.get<bank>(representative);
            representativeData.useDefCost = cost;
            representativeData.liveRange = group.buildLiveRange(m_intervalArena);
            representativeData.preferredReg = preferred;
            representativeData.hasColdUse = hasColdUse;
#if USE(BUN_JSC_ADDITIONS)
            ASSERT(preferences.preferredReg == preferred);
            representativeData.preferredWeight = preferences.preferredWeight;
            representativeData.secondPreferredReg = preferences.secondPreferredReg;
            representativeData.secondPreferredWeight = preferences.secondPreferredWeight;
#endif
            representativeData.validate();

            group.m_representative = representative;
            dataLogLnIf(verbose(), "Coalescing group: ", group);
        }
    }

    // Phase 3: Replace member Tmps with representative in all instructions.
    // Delete self-Moves and adjust useDefCost for removed moves.
    void rewriteCoalescedTmps()
    {
        unsigned* tmps = m_mentions.tmps.mutableSpan().data();
        const unsigned* firstOfInst = m_mentions.firstOfInst.span().data();
        const uint8_t* timingsOfInsts = m_mentions.timingsOfInst.span().data();
        const unsigned* representatives = m_representatives.span().data();
        const unsigned endOfGPIndices = m_endOfGPIndices;
        for (BasicBlock* block : m_code) {
            unsigned headOrdinal = positionOfHead(block) / PointOffsets::PointsPerInst;
            unsigned blockSize = block->size();
            for (unsigned instIndex = 0; instIndex < blockSize; ++instIndex) {
                unsigned begin = firstOfInst[headOrdinal + instIndex];
                unsigned end = firstOfInst[headOrdinal + instIndex + 1];
                bool mustBeLookedAt = end - begin == 2 && tmps[begin] == tmps[begin + 1];
                for (unsigned i = begin; i < end; ++i) {
                    unsigned index = tmps[i];
                    if (unsigned representative = representatives[index]) {
                        tmps[i] = representative;
                        mustBeLookedAt = true;
                    } else if (index >= endOfGPIndices) [[unlikely]]
                        tmps[i] = AbsoluteTmpMapper<FP>::tmpFromAbsoluteIndex(index - endOfGPIndices).internalValue();
                }
                if (!mustBeLookedAt)
                    continue;

                Inst& inst = block->at(instIndex);
                bool maybeCoalescable = mayBeCoalescable(inst);
                if (timingsOfInsts[headOrdinal + instIndex] & Mentions::wasDecodedSlowly) [[unlikely]] {
                    inst.forEachTmpFast([&](Tmp& tmp) {
                        if (unsigned representative = representatives[linearIndex(tmp)])
                            tmp = Tmp::tmpForInternalValue(representative);
                    });
                } else {
                    unsigned next = begin;
                    inst.forEachTmpFast([&](Tmp& tmp) {
                        tmp = Tmp::tmpForInternalValue(tmps[next++]);
                    });
                    ASSERT(next == end);
                }
                if (maybeCoalescable && inst.args()[0].tmp() == inst.args()[1].tmp()) {
                    Tmp tmp = inst.args()[0].tmp();
                    if (tmp.isReg())
                        continue;
                    inst = Inst();
                    for (unsigned i = begin; i < end; ++i)
                        tmps[i] = 0;
                    m_stats[tmp.bank()].numGroupMovesCoalesced++;
                    float freq = adjustedBlockFrequency(block);
                    TmpData& tmpData = m_map[tmp];
                    tmpData.useDefCost.subtractSaturating(UseDefCost(freq)); // For args[0]
                    tmpData.useDefCost.subtractSaturating(UseDefCost(freq)); // For args[1]
                }
            }
        }
        m_representatives.clear();
    }

    template <Bank bank>
    void initSpillCosts()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::initSpillCosts"_s);

        for (Reg reg : m_allowedRegistersInPriorityOrder[bank])
            m_map.get<bank>(Tmp(reg)).spillability = TmpData::Spillability::Unspillable;

        m_code.forEachTmp<bank>([&](Tmp tmp) {
            ASSERT(tmp.bank() == bank);
            ASSERT(!tmp.isReg());
            TmpData& tmpData = m_map.get<bank>(tmp);
            ASSERT(tmpData.useDefCost == UseDefCost(0));
            auto index = AbsoluteTmpMapper<bank>::absoluteIndex(tmp);
            UseDefCost useDefCost = UseDefCost(m_useCounts.numWarmUsesAndDefs<bank>(index));
            if (m_useCounts.isConstDef<bank>(index))
                useDefCost *= 0.1; // Can rematerialize rather than spill in many cases.
            tmpData.useDefCost = useDefCost;

            if (tmpData.liveRange.intervals().size() == 1 && tmpData.liveRange.size() <= PointOffsets::PointsPerInst
                && cannotBeSpilledInPlace(tmp, tmpData.liveRange.intervals().first())) {
                tmpData.spillability = TmpData::Spillability::Unspillable;
                m_stats[bank].numUnspillableTmps++;
            }
            tmpData.validate();
        });
        m_code.forEachFastTmp([&](Tmp tmp) {
            if (tmp.bank() != bank)
                return;
            m_map.get<bank>(tmp).spillability = TmpData::Spillability::FastTmp;
            m_stats[bank].numFastTmps++;
            dataLogLnIf(verbose(), "FastTmp: ", tmp);
        });
    }

    // addTmpImpl creates and returns a new tmp that can hold the values of 'from'.
    // Note that all TmpData references invalidated since it may expand/realloc the TmpData map.
    Tmp addTmpImpl(Tmp from, UseDefCost useDefCost, Interval interval)
    {
        Tmp tmp = m_code.newTmp(from.bank());
        m_tmpWidth.setWidths(tmp, m_tmpWidth.useWidth(from), m_tmpWidth.defWidth(from));

        m_map.append(tmp, TmpData());
        TmpData& tmpData = m_map[tmp];
        if (interval)
            tmpData.liveRange.append(m_intervalArena, interval);
        tmpData.useDefCost = useDefCost;
        tmpData.validate();
        return tmp;
    }

    Tmp addSpillTmpWithInterval(Tmp spilledTmp, Interval interval)
    {
        Tmp tmp = addTmpImpl(spilledTmp, UseDefCost(0), interval);
        TmpData& tmpData = m_map[tmp];
        tmpData.spillability = TmpData::Spillability::Unspillable;
        dataLogLnIf(verbose(), "New spill for ", spilledTmp, " tmp: ", tmp, ": ", tmpData);
        setStageAndEnqueue(tmp, tmpData, Stage::Unspillable);
        m_stats[tmp.bank()].numSpillTmps++;
        return tmp;
    }

    Tmp addSplitTmp(Tmp originalTmp, UseDefCost useDefCost, Interval interval)
    {
        Tmp splitTmp = addTmpImpl(originalTmp, useDefCost, interval);
        // All tmps split from originalTmp share the spill slot to avoid moving between spill slots
        // unnecessarily if parts of the original live-range spill.
        TmpData& originalData = m_map[originalTmp];
        ensureSpillSlotTableEntry(originalData);
        m_map[splitTmp].spillSlotTableIndex = originalData.spillSlotTableIndex;
        m_map[splitTmp].splitFrom = originalData.splitFrom ? originalData.splitFrom : originalTmp;
        return splitTmp;
    }

    void setStageAndEnqueue(Tmp tmp, TmpData& tmpData, Stage stage)
    {
        ASSERT(!tmp.isReg());
        ASSERT(&tmpData == &m_map[tmp]);
        ASSERT(m_map[tmp].liveRange.size()); // 0-size ranges don't need a register and spillCost() depends on size() != 0
        ASSERT(tmpData.stage != Stage::Coalesced && tmpData.stage != Stage::Spilled && tmpData.stage != Stage::Replaced);
        ASSERT(stage == Stage::Unspillable || stage == Stage::TryAllocate || stage == Stage::TrySplit || stage == Stage::Spill);
        ASSERT(m_map[tmp].stage != Stage::Coalesced); // Only the roots of register-groups should be enqueued
        tmpData.validate();

        tmpData.stage = stage;

        bool isLocal = false;
        size_t rangeSizeOrStart = tmpData.liveRange.size();
        if (tmpData.liveRange.intervals().size() == 1) {
            const Interval& interval = tmpData.liveRange.intervals().first();
            BasicBlock* block = findBlockContainingPoint(interval.begin());
            Point blockTail = positionOfTail(block);
            if (interval.end() - 1 <= blockTail) {
                isLocal = true;
                rangeSizeOrStart = interval.begin();
            }
        }
        TmpPriority entry { tmp, stage, rangeSizeOrStart, tmpData.preferredReg || !tmpData.coalescables.isEmpty(), !isLocal };
        if (m_isFillingQueueAtStart)
            m_queueAtStart.append(entry);
        else
            m_queue.enqueue(entry);
        dataLogLnIf(verbose(), "Enqueued (", stage, ") ", tmp);
    }

    template <Bank bank>
    void allocateRegisters()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::allocateRegisters"_s);

        m_calleeSaveCost = UseDefCost(2 * adjustedBlockFrequency(m_code[0]));
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank])
            assign(Tmp(reg), m_map.get<bank>(Tmp(reg)), reg);

        ASSERT(m_queue.isEmpty() && m_nextInQueueAtStart == m_queueAtStart.size());
        m_queueAtStart.shrink(0);
        m_nextInQueueAtStart = 0;
        m_isFillingQueueAtStart = true;
        m_code.forEachTmp<bank>([&](Tmp tmp) {
            ASSERT(!tmp.isReg());
            TmpData& tmpData = m_map.get<bank>(tmp);
            if (tmpData.stage == Stage::Coalesced)
                return;
            if (tmpData.liveRange.intervals().isEmpty())
                return;
            if (tmpData.stage != Stage::New) {
                ASSERT(assignedReg(tmp) && m_code.isPinned(assignedReg(tmp)));
                return;
            }
            m_stats[bank].maxLiveRangeSize = std::max(m_stats[bank].maxLiveRangeSize, static_cast<unsigned>(tmpData.liveRange.size()));
            m_stats[bank].maxLiveRangeIntervals = std::max(m_stats[bank].maxLiveRangeIntervals, static_cast<unsigned>(tmpData.liveRange.intervals().size()));
            Stage initialStage = shouldSpillEverything() ? Stage::Spill : Stage::TryAllocate;
            setStageAndEnqueue(tmp, tmpData, initialStage);
        });
        m_isFillingQueueAtStart = false;
        std::ranges::sort(m_queueAtStart, [](const TmpPriority& left, const TmpPriority& right) {
            return right < left;
        });

        auto isQueueEmpty = [&] {
            return m_queue.isEmpty() && m_nextInQueueAtStart == m_queueAtStart.size();
        };
        auto dequeue = [&] {
            if (m_nextInQueueAtStart < m_queueAtStart.size() && (m_queue.isEmpty() || m_queue.peek() < m_queueAtStart[m_nextInQueueAtStart]))
                return m_queueAtStart[m_nextInQueueAtStart++];
            return m_queue.dequeue();
        };

        do {
            while (!isQueueEmpty()) {
                auto entry = dequeue();
                Tmp tmp = entry.tmp();
                ASSERT(tmp && tmp.bank() == bank);
                TmpData& tmpData = m_map.get<bank>(tmp);
                if (verbose()) {
                    StringPrintStream out;
                    out.println("Pop: ", entry, " tmp: ", tmpData);
                    dumpRegRanges<bank>(out);
                    dataLog(out.toUTF8CString());
                }
                switch (tmpData.stage) {
                case Stage::Unspillable:
                case Stage::TryAllocate:
                    doStageTryAllocate<bank>(tmp, tmpData);
                    continue;
                case Stage::TrySplit:
                    doStageTrySplit<bank>(tmp, tmpData);
                    continue;
                case Stage::Spill:
                    doStageSpill<bank>(tmp, tmpData);
                    continue;
                case Stage::Replaced:
                    continue; // Split tmp no longer needed; skip stale queue entry.
                default:
                    dataLogLn("Invalid stage tmp = ", tmp, " tmpData = ", tmpData);
                    // Tmps in these stages should not have been enqueued.
                    RELEASE_ASSERT_NOT_REACHED();
                }
            }
            if (m_needsEmitSpillCode) {
                emitSpillCodeAndEnqueueNewTmps<bank>();
                m_needsEmitSpillCode = false;
                m_didSpill[bank] = true;
            }
            // Process the spill/fill tmps,
        } while (!isQueueEmpty());
    }

    template <Bank bank>
    void doStageTryAllocate(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(tmpData.stage == Stage::TryAllocate || tmpData.stage == Stage::Unspillable);
#if USE(BUN_JSC_ADDITIONS)
        if (tmpData.spillability != TmpData::Spillability::Spillable && !tmpData.paysForCalleeSave) {
#else
        if (tmpData.spillability != TmpData::Spillability::Spillable)
#endif
            tmpData.paysForCalleeSave = true;
#if USE(BUN_JSC_ADDITIONS)
            tmpData.allocationFailedAtEpoch = 0;
            tmpData.evictionFailedAtEpoch = 0;
        }
#endif
        if (tryAllocate<bank>(tmp, tmpData))
            return;
        if (tryEvict<bank>(tmp, tmpData))
            return;
        RELEASE_ASSERT(tmpData.stage == Stage::TryAllocate); // Unspillable must have succeeded
#if USE(BUN_JSC_ADDITIONS)
        if (m_code.chargesForCalleeSaves()) {
            if (nextCalleeSaveIsFree<bank>() && tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
                return;
            if (trySplitAroundClobbers<bank>(tmp, tmpData, true))
                return;
            if (tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
                return;
        }
#endif
        // If we couldn't allocate tmp, allow it to split next time.
        Stage nextStage = Stage::TrySplit;
        // If we already know splitting won't be profitable, skip it.
        if (tmpData.liveRange.size() < splitMinRangeSize)
            nextStage = Stage::Spill;
        setStageAndEnqueue(tmp, tmpData, nextStage);
    }

    template <Bank bank>
    void doStageTrySplit(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(tmpData.stage == Stage::TrySplit);
        if (tryAllocate<bank>(tmp, tmpData))
            return;
        if (trySplit<bank>(tmp, tmpData))
            return;
        setStageAndEnqueue(tmp, tmpData, Stage::Spill);
    }

    template <Bank bank>
    void doStageSpill(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(tmpData.stage == Stage::Spill);
        if (tryAllocate<bank>(tmp, tmpData))
            return;
        if (tryEvict<bank>(tmp, tmpData))
            return;
        if (tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
            return;
        ASSERT(queueContainsOnlySpills());
        spill(tmp, tmpData);
    }

    template <Bank bank>
    bool nextCalleeSaveIsFree() const
    {
#if USE(BUN_JSC_ADDITIONS)
        return isARM64() && bank == GP && ((m_numberOfCalleeSaves[GP] - m_numberOfFreshCalleeSaves[GP]) & 1);
#else
        return false;
#endif
    }

    template <Bank bank>
    bool tryAllocateFreshCalleeSave(Tmp tmp, TmpData& tmpData)
    {
        UseDefCost worth = tmpData.useDefCost;
#if USE(BUN_JSC_ADDITIONS)
        if (isX86_64())
            worth.subtractSaturating(UseDefCost(tmpData.preferredWeight + tmpData.secondPreferredWeight));
#endif
        if (!m_numberOfFreshCalleeSaves[bank] || tmpData.paysForCalleeSave || (worth <= m_calleeSaveCost && !nextCalleeSaveIsFree<bank>()))
            return false;
        tmpData.paysForCalleeSave = true;
#if USE(BUN_JSC_ADDITIONS)
        tmpData.allocationFailedAtEpoch = 0;
        tmpData.evictionFailedAtEpoch = 0;
#endif
        return tryAllocate<bank>(tmp, tmpData);
    }

#if USE(BUN_JSC_ADDITIONS)
    uint64_t clobberedRegs(TmpData& tmpData)
    {
        static_assert(Reg::maxIndex() < 64);
        if (tmpData.hasClobberedRegs)
            return tmpData.clobberedRegs;
        uint64_t result = 0;
        const Point* points = m_clobberPoints.span().data();
        const size_t numberOfPoints = m_clobberPoints.size();
        for (auto& interval : tmpData.liveRange.intervals()) {
            if (result == m_allClobberedRegs)
                break;
            size_t index = m_firstClobberOfInst[interval.begin() / PointOffsets::PointsPerInst];
            while (index < numberOfPoints && points[index] < interval.begin())
                ++index;
            for (; index < numberOfPoints && points[index] < interval.end() && result != m_allClobberedRegs; ++index) {
                if (!(index % clobberPointsPerChunk) && !m_clobberedRegsInChunk.isEmpty()) [[unlikely]] {
                    while (index + clobberPointsPerChunk <= numberOfPoints && points[index + clobberPointsPerChunk - 1] < interval.end()) {
                        result |= m_clobberedRegsInChunk[index / clobberPointsPerChunk];
                        index += clobberPointsPerChunk;
                    }
                    if (index == numberOfPoints || points[index] >= interval.end())
                        break;
                }
                result |= m_clobberedRegsAtPoint[index];
            }
        }
        tmpData.clobberedRegs = result;
        tmpData.hasClobberedRegs = true;
        return result;
    }

    bool m_isTakingFromHolder { false };

    void noteNeighbours()
    {
        if (!isX86() || !m_code.chargesForCalleeSaves())
            return;
        for (unsigned ordinal : m_conditionalMoves) {
            Inst& inst = instAtOrdinal(ordinal);
            Arg& yielded = inst.args()[5];
            if (!yielded.isTmp() || yielded.isReg() || yielded.tmp().bank() != GP)
                continue;
            for (unsigned i = 3; i < 5; ++i) {
                Arg& taken = inst.args()[i];
                if (!taken.isTmp() || taken.isReg() || taken.tmp().bank() != GP || taken.tmp() == yielded.tmp())
                    continue;
                m_map[yielded.tmp()].addNeighbour(taken.tmp());
                m_map[taken.tmp()].addNeighbour(yielded.tmp());
            }
        }
    }

    NEVER_INLINE void tellNeighbours(TmpData& tmpData, Reg reg)
    {
        for (Tmp neighbour : tmpData.neighbours) {
            if (neighbour)
                m_map[neighbour].prefer(reg, 0);
        }
    }

#endif
    template <Bank bank>
    bool tryAllocate(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(&m_map.get<bank>(tmp) == &tmpData);
        ASSERT(!assignedReg(tmp));

        Width width = widthForConflicts<bank>(tmp);
#if USE(BUN_JSC_ADDITIONS)
        uint32_t failedAtEpoch = tmpData.allocationFailedAtEpoch;
        uint64_t clobbered = clobberedRegs(tmpData);
#endif

        auto tryAllocateToReg = [&](Reg r) {
            if (!mayTake(tmpData, r))
                return false;
#if USE(BUN_JSC_ADDITIONS)
            if (failedAtEpoch && m_epochOfLastRemoval[r.index()] <= failedAtEpoch)
                return false;
            if (clobbered >> r.index() & 1)
                return false;
#endif
            LiveRange& liveRange = tmpData.liveRange;
            RegisterRange& regRanges = m_regRanges[r];
            if (!regRanges.hasConflict(liveRange, width)) {
                assign(tmp, tmpData, r);
                return true;
            }
            return false;
        };

        if (tmpData.preferredReg) {
            if (tryAllocateToReg(tmpData.preferredReg))
                return true;
        }
#if USE(BUN_JSC_ADDITIONS)
        Reg secondPreferredReg = m_code.chargesForCalleeSaves() ? tmpData.secondPreferredReg : Reg();
        if (secondPreferredReg && tryAllocateToReg(secondPreferredReg))
            return true;
        if (m_code.chargesForCalleeSaves() && !m_isTakingFromHolder && tmpData.spillability != TmpData::Spillability::Unspillable) {
            auto tryTakeFromHolder = [&](Reg r) {
                float wish = tmpData.wishFor(r);
                if (!r || !(wish > 0) || !mayTake(tmpData, r) || (clobbered >> r.index() & 1))
                    return false;
                Tmp holder;
                bool isPossible = true;
                m_regRanges[r].forEachConflict(tmpData.liveRange, width, [&](auto& conflict) -> IterationStatus {
                    if (conflict.tmp.isReg() || (holder && holder != conflict.tmp)) {
                        isPossible = false;
                        return IterationStatus::Done;
                    }
                    holder = conflict.tmp;
                    return IterationStatus::Continue;
                });
                if (!isPossible || !holder)
                    return false;
                TmpData& holderData = m_map.get<bank>(holder);
                if (holderData.spillability == TmpData::Spillability::Unspillable || holderData.stage != Stage::Assigned || holderData.wishFor(r) >= wish)
                    return false;
                Stage stage = tmpData.stage;
                uint32_t holderFailedAtEpoch = holderData.allocationFailedAtEpoch;
                evict(holder, holderData, r);
                assign(tmp, tmpData, r);
                m_isTakingFromHolder = true;
                bool isMoved = tryAllocate<bank>(holder, holderData);
                m_isTakingFromHolder = false;
                if (isMoved)
                    return true;
                evict(tmp, tmpData, r);
                tmpData.stage = stage;
                holderData.allocationFailedAtEpoch = holderFailedAtEpoch;
                assign(holder, holderData, r);
                return false;
            };
            if (tryTakeFromHolder(tmpData.preferredReg) || tryTakeFromHolder(secondPreferredReg))
                return true;
        }
#endif
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (r == tmpData.preferredReg)
                continue;
#if USE(BUN_JSC_ADDITIONS)
            if (r == secondPreferredReg)
                continue;
#endif
            if (tryAllocateToReg(r))
                return true;
        }
#if USE(BUN_JSC_ADDITIONS)
        tmpData.allocationFailedAtEpoch = m_removalEpoch;
#endif
        return false;
    }

    template <Bank bank>
    bool tryEvict(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(&m_map.get<bank>(tmp) == &tmpData);
        ASSERT(tmp.bank() == bank);

        auto failOutOfRegisters = [this](Tmp tmp) {
            insertFixupCode(); // So that the log shows the fixup code too
            StringPrintStream out;
            out.println("FATAL: no register for ", tmp);
            out.println("Unspillable Conflicts:");
            for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
                out.print("  ", r, ": ");
                m_regRanges[r].forEachConflict(m_map.get<bank>(tmp).liveRange, widthForConflicts<bank>(tmp),
                    [&](auto& conflict) -> IterationStatus {
                        if (m_map.get<bank>(conflict.tmp).spillCost() == unspillableCost)
                            out.print("{", conflict.tmp, ", ", conflict.interval, "}, ");
                        return IterationStatus::Continue;
                    });
                out.println("");
            }
            out.println("Code:", m_code);
            out.println("Register Allocator State:\n", pointerDump(this));
            dataLogLn(out.toUTF8CString());
            RELEASE_ASSERT_NOT_REACHED();
        };

        Reg bestEvictReg;
        SpillCost minSpillCost = tmpData.spillCost();
        m_visited.resize(m_code.numTmps(bank));
        LiveRange& liveRange = tmpData.liveRange;
        Width width = widthForConflicts<bank>(tmp);
#if USE(BUN_JSC_ADDITIONS)
        uint32_t failedAtEpoch = tmpData.evictionFailedAtEpoch;
        uint64_t clobbered = clobberedRegs(tmpData);
#endif
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (!mayTake(tmpData, r))
                continue;
#if USE(BUN_JSC_ADDITIONS)
            if (failedAtEpoch && m_epochOfLastRemoval[r.index()] <= failedAtEpoch)
                continue;
            if (clobbered >> r.index() & 1)
                continue;
#endif
            SpillCost conflictsSpillCost(0);
            m_visited.clear();
            m_regRanges[r].forEachConflict(liveRange, width,
                [&](auto& conflict) -> IterationStatus {
                    if (conflict.tmp.isReg()) {
                        // Conflicts with a fixed register use/def, cannot evict.
                        conflictsSpillCost = unspillableCost;
                        return IterationStatus::Done;
                    }
                    unsigned conflictTmpIndex = conflict.tmp.tmpIndex(bank);
                    if (m_visited.contains(conflictTmpIndex))
                        return IterationStatus::Continue;
                    m_visited.add(conflictTmpIndex);
                    auto cost = m_map.get<bank>(conflict.tmp).spillCost();
                    if (cost == unspillableCost) {
                        conflictsSpillCost = unspillableCost;
                        return IterationStatus::Done;
                    }
                    if (cost == maxSpillableSpillCost || conflictsSpillCost == maxSpillableSpillCost) [[unlikely]]
                        conflictsSpillCost = maxSpillableSpillCost;
                    else
                        conflictsSpillCost += cost;
                    return conflictsSpillCost >= minSpillCost ? IterationStatus::Done : IterationStatus::Continue;
            });
            if (conflictsSpillCost < minSpillCost) {
                minSpillCost = conflictsSpillCost;
                bestEvictReg = r;
            }
        }
        if (minSpillCost >= tmpData.spillCost()) {
            // If 'tmp' was unspillable, we better have found at least one suitable register.
            if (tmpData.spillCost() == unspillableCost) [[unlikely]]
                failOutOfRegisters(tmp);
#if USE(BUN_JSC_ADDITIONS)
            tmpData.evictionFailedAtEpoch = m_removalEpoch;
#endif
            return false;
        }
        // It's cheaper to spill all the already-assigned conflicting tmps, so evict them in favor of assigning 'tmp'.
        m_regRanges[bestEvictReg].forEachConflict(liveRange, widthForConflicts<bank>(tmp),
            [&](auto& conflict) -> IterationStatus {
                TmpData& conflictData = m_map.get<bank>(conflict.tmp);
                evict(conflict.tmp, conflictData, bestEvictReg);
                setStageAndEnqueue(conflict.tmp, conflictData, Stage::TryAllocate);
                return IterationStatus::Continue;
            });
        assign(tmp, tmpData, bestEvictReg);
        return true;
    }

    void assignImpl(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(tmpData.stage != Stage::Assigned && tmpData.stage != Stage::Spilled);
        ASSERT(&m_map[tmp] == &tmpData);
        tmpData.stage = Stage::Assigned;
        tmpData.assigned = reg;
        dataLogLnIf(verbose(), "Assigned ", tmp, " to ", reg);
        tmpData.validate();
    }

    void coalesceWithPinned(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(m_code.isPinned(reg));
        assignImpl(tmp, tmpData, reg);
    }

    void assign(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(m_allAllowedRegisters.contains(reg, IgnoreVectors));
        if (m_freshCalleeSaves.contains(reg) && (!tmp.isReg() || tmpData.liveRange.size() || ((m_regsWithDeadClobbers >> reg.index() & 1) && !m_code.calleeSavesLostAtSuspensions().contains(reg, IgnoreVectors)))) {
            m_freshCalleeSaves.remove(reg);
#if USE(BUN_JSC_ADDITIONS)
            m_epochOfLastRemoval[reg.index()] = ++m_removalEpoch;
#endif
            --m_numberOfFreshCalleeSaves[reg.isGPR() ? GP : FP];
        }
        m_regRanges[reg].add(tmp, tmpData.liveRange);
        assignImpl(tmp, tmpData, reg);
#if USE(BUN_JSC_ADDITIONS)
        if (tmpData.neighbours[0]) [[unlikely]]
            tellNeighbours(tmpData, reg);
#endif
    }

    void evict(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(tmpData.stage == Stage::Assigned);
        ASSERT(tmpData.spillCost() != unspillableCost);
        ASSERT(tmpData.assigned == reg);
        m_regRanges[reg].evict(tmpData.liveRange);
#if USE(BUN_JSC_ADDITIONS)
        m_epochOfLastRemoval[reg.index()] = ++m_removalEpoch;
#endif
        tmpData.stage = Stage::New;
        tmpData.assigned = Reg();
        dataLogLnIf(verbose(), "Evicted ", tmp, " from ", reg);
        tmpData.validate();
    }

    bool isLiveRangeBlockLocal(const LiveRange& liveRange)
    {
        BasicBlock* startBlock = findBlockContainingPoint(liveRange.intervals().first().begin());
        Point last = liveRange.intervals().last().end() - 1;
        return last <= positionOfTail(startBlock);
    }

    template<Bank bank>
    bool trySplit(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(tmpData.spillCost() != unspillableCost); // Should have evicted.
        if (!m_code.chargesForCalleeSaves()) {
            if (trySplitAroundClobbers<bank>(tmp, tmpData))
                return true;
            if (tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
                return true;
        }
        if (trySplitAroundLoop<bank>(tmp, tmpData))
            return true;
        return trySplitIntraBlock<bank>(tmp, tmpData);
    }

    void analyzeLoop(const NaturalLoop& loop)
    {
        // Collect boundary points for fast live-range-crosses-loop-boundary checks.
        Vector<Point, 16> boundaryPoints;

        BasicBlock* header = loop.header();
        for (BasicBlock* pred : header->predecessors()) {
            if (m_naturalLoops->belongsTo(pred, loop))
                continue; // Back-edge predecessor, skip.
            // ensureDedicatedLoopEntryExitBlocks() guarantees non-critical entry edges and dedicated exit blocks.
            ASSERT(pred->numSuccessors() == 1 && pred->successor(0).block() == header);
            boundaryPoints.append(positionOfTail(pred));
        }

        for (unsigned i = 0; i < loop.size(); i++) {
            BasicBlock* loopBlock = loop.at(i);
            for (auto& succ : loopBlock->successors()) {
                if (m_naturalLoops->belongsTo(succ.block(), loop))
                    continue;
#if ASSERT_ENABLED
                // ensureDedicatedLoopEntryExitBlocks() ensured the exit successor has only loop
                // predecessors so it's safe to emit the fixup code in the successor block.
                for (BasicBlock* exitPred : succ.block()->predecessors())
                    ASSERT(m_naturalLoops->belongsTo(exitPred, loop));
#endif
                boundaryPoints.append(positionOfHead(succ.block()));
            }
        }

        LoopData& loopData = m_loopData[loop.index()];
        ASSERT(!loopData.range.size() && !loopData.boundary.size());

        std::ranges::sort(boundaryPoints);
        removeRepeatedElements(boundaryPoints);
        LiveRange& boundary = loopData.boundary;
        for (Point point : boundaryPoints)
            boundary.append(m_intervalArena, Interval(point));

        Vector<Interval, 32> loopIntervals;
        for (unsigned i = 0; i < loop.size(); i++) {
            auto block = loop.at(i);
            loopIntervals.constructAndAppend(positionOfHead(block), positionOfTail(block) + 1);
        }
        std::ranges::sort(loopIntervals, [](const Interval& a, const Interval& b) {
            return a.begin() < b.begin();
        });
        LiveRange& loopRange = loopData.range;
        for (auto& interval : loopIntervals)
            loopRange.append(m_intervalArena, interval);
    }

    void ensureLoopAnalysis()
    {
        if (m_naturalLoops)
            return;
        m_dominators = makeUnique<Dominators>(m_code);
        m_naturalLoops = makeUnique<NaturalLoops>(m_code, *m_dominators);
        m_loopData.resize(m_naturalLoops->numLoops());

        for (unsigned i = 0; i < m_naturalLoops->numLoops(); i++)
            analyzeLoop(m_naturalLoops->loop(i));
    }

    // Pick the outermost loop that meaningfully reduces the live range when split out.
    // Returns the chosen loop and the non-loop remainder, or {nullptr, {}} if none qualifies.
    std::pair<const NaturalLoop*, LiveRange> chooseLoopForSplit(const LiveRange& liveRange)
    {
        const NaturalLoop* resultLoop = nullptr;
        LiveRange resultNonLoopRange;

        BitVector visitedLoops;
        Vector<const NaturalLoop*, 16> nestedLoops;
        forEachBlockInLiveRange(liveRange, [&](BasicBlock* block) {
            nestedLoops.shrink(0);
            for (const NaturalLoop* loop = m_naturalLoops->innerMostLoopOf(block); loop; loop = m_naturalLoops->innerMostOuterLoop(*loop)) {
                if (visitedLoops.get(loop->index()))
                    break;
                nestedLoops.append(loop);
            }

            // Iterate from outer-most to inner-most
            for (auto* loop : std::views::reverse(nestedLoops)) {
                ASSERT(!visitedLoops.get(loop->index()));
                visitedLoops.set(loop->index());

                auto& loopData = m_loopData[loop->index()];
                if (liveRange.overlaps(loopData.boundary)) {
                    // Skip loops that are too large relative to the live range since they are unlikely to improve allocation progress.
                    if (loopData.range.size() > liveRange.size() * Options::airGreedyRegAllocLoopSplitMaxLoopFraction())
                        continue;
                    resultLoop = loop;
                    resultNonLoopRange = LiveRange::subtract(m_intervalArena, liveRange, loopData.range);
                    return IterationStatus::Done;
                }
            }
            return IterationStatus::Continue;
        });
        return { resultLoop, WTF::move(resultNonLoopRange) };
    }

    template<Bank bank>
    bool trySplitAroundLoop(Tmp tmp, TmpData& tmpData)
    {
        if (!Options::airGreedyRegAllocSplitAroundLoops())
            return false;

        // Clobber fixup references originalTmp which would be stale after loop splitting.
        if (tmpData.splitAroundClobbersMetadataIndex)
            return false;
        if (tmpData.liveRange.size() < splitMinRangeSize)
            return false;
        if (isConstDef<bank>(tmp))
            return false;
        if (isLiveRangeBlockLocal(tmpData.liveRange))
            return false;

        ensureLoopAnalysis();
        if (!m_naturalLoops->numLoops())
            return false;

        auto [loop, nonLoopRange] = chooseLoopForSplit(tmpData.liveRange);
        if (!loop)
            return false;

        if (m_naturalLoops->loopDepth(loop->header()) > maxLoopSplitDepth)
            return false;

        auto findFirstLoopPredecessor = [this](const NaturalLoop* loop) -> BasicBlock* {
            BasicBlock* header = loop->header();
            for (BasicBlock* pred : header->predecessors()) {
                if (!m_naturalLoops->belongsTo(pred, *loop))
                    return pred;
            }
            ASSERT_NOT_REACHED();
            return nullptr;
        };

        // Estimate the fixup cost: each loop invocation pays one entry + one exit move.
        // If the cumulative fixup cost across sibling loop splits of this tmp exceeds useDefCost, bail.
        BasicBlock* entryBlock = findFirstLoopPredecessor(loop);
        UseDefCost fixupCost(2 * adjustedBlockFrequency(entryBlock));
        UseDefCost totalFixupCost = tmpData.cumulativeFixupCost;
        totalFixupCost += fixupCost;
        if (totalFixupCost > tmpData.useDefCost)
            return false;
        tmpData.cumulativeFixupCost += fixupCost;

        ensureUseDefLists();

        Tmp loopTmp = addSplitTmp(tmp, UseDefCost(0), { });
        // Note: addSplitTmp() may have resized m_map, invalidating the tmpData parameter.
        TmpData& originalData = m_map.get<bank>(tmp);
        TmpData& loopData = m_map.get<bank>(loopTmp);
        loopData.liveRange = LiveRange::subtract(m_intervalArena, originalData.liveRange, nonLoopRange);
        ASSERT(loopData.liveRange.size());

        bool loopHasDef = false;
        size_t cursor = 0;
        for (Interval interval : loopData.liveRange.intervals()) {
            do {
                interval = forEachUseDefWithin(tmp, interval, cursor, [&](Point point, Inst& inst, BasicBlock& block) {
                    inst.forEachTmp([&](Tmp& t, Arg::Role role, Bank, Width) {
                        if (t != tmp)
                            return;
                        t = loopTmp;
                        if (Arg::isAnyDef(role))
                            loopHasDef = true;
                        if (Arg::isColdUse(role))
                            loopData.hasColdUse = true;
                        else
                            loopData.useDefCost += UseDefCost(block.frequency());
                        m_useDefListsOfNewTmps.ensure(loopTmp, [] {
                            return UseDefList();
                        }).iterator->value.add(point);
                    });
                });
            } while (interval);
        }

        // originalTmp no longer live within the loop.
        originalData.liveRange = WTF::move(nonLoopRange);
#if USE(BUN_JSC_ADDITIONS)
        originalData.allocationFailedAtEpoch = 0;
        originalData.evictionFailedAtEpoch = 0;
        originalData.hasClobberedRegs = false;
#endif
        originalData.useDefCost.subtractSaturating(loopData.useDefCost);

        m_aroundLoopMetadata.constructAndAppend(tmp, loop);
        AroundLoopSplitMetadata& metadata = m_aroundLoopMetadata.last();
        metadata.loopTmp = loopTmp;
        metadata.loopHasDef = loopHasDef;

        // Spill zero-cost sides directly — a register would just carry a value nothing reads,
        // while still requiring fixup moves at loop boundaries. Epsilon handles FP rounding.
        auto enqueueOrSpill = [&](Tmp t, TmpData& data) {
            if (data.useDefCost < zeroCostEpsilon) {
                spill(t, data);
                m_stats[bank].numSplitAroundLoopZeroCostSpilled++;
            } else
                setStageAndEnqueue(t, data, Stage::TryAllocate);
        };
        enqueueOrSpill(loopTmp, loopData);
        enqueueOrSpill(tmp, originalData);

        m_stats[bank].numSplitAroundLoop++;
        dataLogLnIf(verbose(), "Split (around loop): ", tmp, " -> loopTmp=", loopTmp, " header=BB", loop->header(), " loopHasDef=", loopHasDef);
        return true;
    }

    NEVER_INLINE size_t nextDeadClobberOf(Reg reg, size_t from)
    {
        if (m_firstDeadClobberOfReg.isEmpty()) {
            m_firstDeadClobberOfReg.fill(0, Reg::maxIndex() + 2);
            for (uint64_t regs : m_deadClobbersAtPoint) {
                for (; regs; regs &= regs - 1)
                    ++m_firstDeadClobberOfReg[std::countr_zero(regs) + 1];
            }
            for (unsigned i = 0; i <= Reg::maxIndex(); ++i)
                m_firstDeadClobberOfReg[i + 1] += m_firstDeadClobberOfReg[i];
            m_deadClobbersOfRegs.grow(m_firstDeadClobberOfReg.last());
            Vector<unsigned> next = m_firstDeadClobberOfReg;
            for (unsigned clobber = 0; clobber < m_deadClobbersAtPoint.size(); ++clobber) {
                for (uint64_t regs = m_deadClobbersAtPoint[clobber]; regs; regs &= regs - 1)
                    m_deadClobbersOfRegs[next[std::countr_zero(regs)]++] = clobber;
            }
        }
        auto own = m_deadClobbersOfRegs.span().subspan(m_firstDeadClobberOfReg[reg.index()], m_firstDeadClobberOfReg[reg.index() + 1] - m_firstDeadClobberOfReg[reg.index()]);
        auto iterator = std::ranges::lower_bound(own, static_cast<unsigned>(from));
        return iterator == own.end() ? m_deadClobbersAtPoint.size() : *iterator;
    }

    void forEachConflictAndDeadClobber(Reg reg, const LiveRange& range, Width width, NOESCAPE const Invocable<IterationStatus(RegisterRange::AllocatedInterval&)> auto& func)
    {
        if (m_clobberPoints.size() > fewClobberPoints) [[unlikely]]
            forEachConflictAndDeadClobberImpl<true>(reg, range, width, func);
        else
            forEachConflictAndDeadClobberImpl<false>(reg, range, width, func);
    }

    template<bool leaps>
    void forEachConflictAndDeadClobberImpl(Reg reg, const LiveRange& range, Width width, NOESCAPE const Invocable<IterationStatus(RegisterRange::AllocatedInterval&)> auto& func)
    {
        RegisterRange& regRange = m_regRanges[reg];
        const uint64_t bit = static_cast<uint64_t>(1) << reg.index();
        if (!(m_regsWithDeadClobbers & bit)) {
            regRange.forEachConflict(range, width, func);
            return;
        }
        const Point* points = m_clobberPoints.span().data();
        const uint64_t* deadClobbersAtPoint = m_deadClobbersAtPoint.span().data();
        const size_t numberOfPoints = m_clobberPoints.size();
        for (auto interval : range.intervals()) {
            size_t clobber = m_firstClobberOfInst[interval.begin() / PointOffsets::PointsPerInst];
            while (clobber < numberOfPoints && points[clobber] < interval.begin())
                ++clobber;
            while (true) {
                auto conflict = regRange.firstConflict(interval);
                Point limit = conflict ? conflict->interval.begin() : interval.end();
                [[maybe_unused]] unsigned missesInARow = 0;
                for (; clobber < numberOfPoints && points[clobber] < limit; ++clobber) {
                    if (!(deadClobbersAtPoint[clobber] & bit)) {
                        if constexpr (leaps) {
                            if (++missesInARow > mostMissesInARow) [[unlikely]] {
                                clobber = nextDeadClobberOf(reg, clobber) - 1;
                                missesInARow = 0;
                            }
                        }
                        continue;
                    }
                    missesInARow = 0;
                    RegisterRange::AllocatedInterval deadClobber { std::pair { Interval(points[clobber]), Tmp(reg) } };
                    if (func(deadClobber) == IterationStatus::Done)
                        return;
                }
                if (!conflict)
                    break;
                if (func(*conflict) == IterationStatus::Done)
                    return;
                if (interval.end() <= conflict->interval.end())
                    break;
                interval = { conflict->interval.end(), interval.end() };
            }
        }
        if (width > Width64) [[unlikely]]
            regRange.forEachConflictInHigh64(range, func);
    }

    template<Bank bank>
    bool trySplitAroundClobbers(Tmp tmp, TmpData& tmpData, bool mayEvict = false)
    {
        if (tmpData.splitAroundClobbersMetadataIndex)
            return false; // Already split around clobbers
#if USE(BUN_JSC_ADDITIONS)
        if (tmpData.goesRoundAStretch)
            return false;
#endif
        if (tmpData.liveRange.size() < splitMinRangeSize)
            return false; // Not enough instructions to be worthwhile

        struct ClobberSite {
            Point point;
            float cost;
            bool usesOrDefsTmp;
        };
        Vector<ClobberSite, 16> clobberSites;
        size_t nextClobberSite = 0;
        // A call clobbers every caller-saved register at a single point, so the same instruction is
        // reached once per candidate register below. What is computed here depends only on the
        // point, thus caching here.
        auto clobberSiteAt = [&](Point point) -> ClobberSite {
            while (nextClobberSite < clobberSites.size() && clobberSites[nextClobberSite].point < point)
                ++nextClobberSite;
            if (nextClobberSite < clobberSites.size() && clobberSites[nextClobberSite].point == point)
                return clobberSites[nextClobberSite];
            BasicBlock* block = findBlockContainingPoint(point);
            Inst& inst = block->at(this->instIndex(positionOfHead(block), point));
            bool usesOrDefsTmp = false;
            inst.forEachTmpFast([&](Tmp useOrDef) {
                usesOrDefsTmp |= useOrDef == tmp;
            });
            ClobberSite site { point, adjustedBlockFrequency(block) * 2, usesOrDefsTmp };
            clobberSites.insert(nextClobberSite, site);
            return site;
        };
        ASSERT(tmpData.spillCost() != unspillableCost); // Should have evicted.
#if USE(BUN_JSC_ADDITIONS)
        const bool goesRoundStretches = m_code.chargesForCalleeSaves();
#endif

        Reg bestSplitReg;
        constexpr UseDefCost invalidSplitCost(std::numeric_limits<float>::infinity());
        const float splitMultiplier = Options::airGreedyRegAllocSplitMultiplier();
        UseDefCost splitCostLimit = splitMultiplier > 0.0 ? tmpData.useDefCost / splitMultiplier : invalidSplitCost;
        if (m_numberOfFreshCalleeSaves[bank] && !tmpData.paysForCalleeSave)
            splitCostLimit = std::min(splitCostLimit, m_calleeSaveCost);
        UseDefCost minSplitCost = splitCostLimit;
        Reg bestSplitRegWithEviction;
        UseDefCost minSplitCostWithEviction = splitCostLimit;
        Width width = widthForConflicts<bank>(tmp);
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (!mayTake(tmpData, r))
                continue;
            UseDefCost splitCost(0);
            unsigned numberOfClobbers = 0;
            nextClobberSite = 0;
            SpillCost evictionCost(0);
            bool evicts = false;
            if (mayEvict) {
                m_visited.resize(m_code.numTmps(bank));
                m_visited.clear();
            }
            forEachConflictAndDeadClobber(r, tmpData.liveRange, width,
                [&](auto& conflict) -> IterationStatus {
                    if (conflict.tmp.isReg() && conflict.interval.distance() == 1) {
                        ClobberSite site = clobberSiteAt(conflict.interval.begin());
                        if (site.usesOrDefsTmp || ++numberOfClobbers > 32) {
                            // If the inst that clobbers regs also use/def the tmp trying to be split, then
                            // can't split the tmp around this clobber.
                            // FIXME: could allow uses, but then we'd have to make split tmp conflict with any
                            // spill tmps used by this instruction, so unclear if that's better.
                            splitCost = invalidSplitCost;
                            return IterationStatus::Done;
                        }
                        // Times 2 for 'MOV tmp, gapTmp' and 'MOV gapTmp, tmp'
                        splitCost += UseDefCost(site.cost);
                        if (splitCost >= (evicts ? minSplitCostWithEviction : mayEvict ? std::max(minSplitCost, minSplitCostWithEviction) : minSplitCost))
                            return IterationStatus::Done; // Not the best or already over limit, exit early.
                        return IterationStatus::Continue;
                    }
                    if (mayEvict && !conflict.tmp.isReg()) {
                        evicts = true;
                        unsigned conflictTmpIndex = conflict.tmp.tmpIndex(bank);
                        if (m_visited.contains(conflictTmpIndex))
                            return IterationStatus::Continue;
                        m_visited.add(conflictTmpIndex);
                        auto cost = m_map.get<bank>(conflict.tmp).spillCost();
                        if (cost != unspillableCost && cost != maxSpillableSpillCost) {
                            evictionCost += cost;
                            if (evictionCost < tmpData.spillCost())
                                return IterationStatus::Continue;
                        }
                    }
                    // Conflicts with another Tmp already assigned to this register so splitting around the clobbers won't help.
                    splitCost = invalidSplitCost;
                    return IterationStatus::Done;
                });
            if (evicts) {
                if (UseDefCost(0) < splitCost && splitCost < minSplitCostWithEviction) {
                    minSplitCostWithEviction = splitCost;
                    bestSplitRegWithEviction = r;
                }
                continue;
            }
            if (splitCost < minSplitCost) {
                minSplitCost = splitCost;
                bestSplitReg = r;
            }
        }
        if (minSplitCost >= splitCostLimit && bestSplitRegWithEviction) {
            minSplitCost = minSplitCostWithEviction;
            bestSplitReg = bestSplitRegWithEviction;
        }
        if (minSplitCost >= splitCostLimit)
            return false; // No register found or better to spill than to split
        ASSERT(bestSplitReg);

        LiveRange holeRange;
        forEachConflictAndDeadClobber(bestSplitReg, tmpData.liveRange, width,
            [&](auto& conflict) -> IterationStatus {
                if (!conflict.tmp.isReg())
                    return IterationStatus::Continue;
                ASSERT(conflict.interval.distance() == 1);
                // Punched hole should always include the instructions late interval so the
                // split tmp won't be modeled as conflicting with late defs.
                Interval hole = conflict.interval | lateInterval(positionOfEarly(conflict.interval));
                ASSERT(hole.distance() == 1 || hole.distance() == 2);
                holeRange.append(m_intervalArena, hole);
                return IterationStatus::Continue;
            });
#if USE(BUN_JSC_ADDITIONS)
        if (goesRoundStretches && holeRange.intervals().size() > 1) {
            LiveRange stretches;
            Interval stretch;
            unsigned sitesInStretch = 0;
            size_t liveIndex = 0;
            auto liveIntervals = tmpData.liveRange.intervals();
            auto joins = [&](Interval hole) {
                BasicBlock* block = findBlockContainingPoint(stretch.begin());
                if (hole.end() - 1 > positionOfTail(block))
                    return false;
                while (liveIndex < liveIntervals.size() && liveIntervals[liveIndex].end() <= stretch.begin())
                    ++liveIndex;
                if (liveIndex == liveIntervals.size() || liveIntervals[liveIndex].begin() > stretch.begin() || liveIntervals[liveIndex].end() < hole.end())
                    return false;
                Point head = positionOfHead(block);
                size_t site = instIndex(head, hole.begin());
                for (size_t index = instIndex(head, stretch.end() - 1) + 1; index < site; ++index) {
                    bool namesIt = false;
                    block->at(index).forEachTmpFast([&](Tmp& named) {
                        namesIt |= named == tmp;
                    });
                    if (namesIt)
                        return false;
                }
                return true;
            };
            auto finish = [&] {
                stretches.append(m_intervalArena, stretch);
                if (sitesInStretch > 1) {
                    m_stats[bank].numSplitAroundClobberStretches++;
                    m_stats[bank].numSplitAroundClobberSitesInStretches += sitesInStretch;
                }
            };
            for (Interval hole : holeRange.intervals()) {
                if (sitesInStretch && joins(hole)) {
                    stretch |= hole;
                    ++sitesInStretch;
                    continue;
                }
                if (sitesInStretch)
                    finish();
                stretch = hole;
                sitesInStretch = 1;
            }
            finish();
            holeRange = WTF::move(stretches);
        }
#endif

        tmpData.liveRange = LiveRange::subtract(m_intervalArena, tmpData.liveRange, holeRange);
#if USE(BUN_JSC_ADDITIONS)
        tmpData.allocationFailedAtEpoch = 0;
        tmpData.evictionFailedAtEpoch = 0;
        tmpData.hasClobberedRegs = false;
#endif
        tmpData.splitAroundClobbersMetadataIndex = m_aroundClobbersMetadata.size();
        setStageAndEnqueue(tmp, tmpData, Stage::TryAllocate);

        AroundClobbersSplitMetadata metadata(tmp);
        // Create tmps to carry the value across register clobbering instructions. These tmps
        // might spill or be assigned another register.
        for (Interval hole : holeRange.intervals()) {
            BasicBlock* block = findBlockContainingPoint(hole.begin());
            UseDefCost freq(2 * adjustedBlockFrequency(block));
            // padInterference() ensures this.
            // FIXME: reconsider that, see https://bugs.webkit.org/show_bug.cgi?id=288122
            ASSERT(hole.begin() > positionOfHead(block));
            // Model gapTmp interference with any other tmp split at this location by starting
            // the gapTmp's range one position before the hole. Otherwise, the same register
            // may be chosen for the gapTmp and another splitTmp, which wouldn't be valid
            // since there could be a cycle among the set of fixup Move instructions.
            // An alternative would be to use the Shuffle opcode (which can handle that
            // rotation of register assignments) but that would trigger an extra liveness
            // analysis (see lowerAfterRegAlloc()), and that's unlikely to be worth it.
#if USE(BUN_JSC_ADDITIONS)
            Interval gapInterval = hole | Interval(hole.begin() - 1);
            if (goesRoundStretches)
                gapInterval = Interval(pointAtOffset(hole.begin(), PointOffsets::Pre), pointAtOffset(hole.end() - 1, PointOffsets::Post) + 1);
#else
            Interval gapInterval = hole | Interval(hole.begin() - 1);
#endif
            Tmp gapTmp = addSplitTmp(tmp, UseDefCost(freq), gapInterval);
            metadata.splits.append({ gapTmp });
#if USE(BUN_JSC_ADDITIONS)
            m_map.get<bank>(gapTmp).goesRoundAStretch = hole.distance() > 2;
#endif
            setStageAndEnqueue(gapTmp, m_map.get<bank>(gapTmp), Stage::TryAllocate);
        }
        dataLogLnIf(verbose(), "Split (clobbers): reg = ", bestSplitReg, " spillCost = ", m_map.get<bank>(tmp).spillCost().value(), " splitCost = ", minSplitCost.value(), " split tmp = ", metadata);
        m_aroundClobbersMetadata.append(WTF::move(metadata));
        m_stats[bank].numSplitAroundClobbers++;
        return true;
    }

    // Note that the use/def lists are computed only once and not kept up to date.
    // So after a Tmp is split or spilled that Tmp's use/def list may include instructions that now
    // reference the new split tmp or spill slot rather than the Tmp itself.
    // Only the Tmps matter when indexing use/def sites, not their roles, so the role-free walk is
    // used. This asserts the two walks really do enumerate the same Tmps for every opcode form.
    static void validateFastTmpEnumeration(Inst& inst)
    {
        auto collect = [](auto&& walk) {
            Vector<int, 8> values;
            walk([&](Tmp& tmp) { values.append(tmp.internalValue()); });
            std::ranges::sort(values);
            Vector<int, 8> unique;
            for (int value : values) {
                if (unique.isEmpty() || unique.last() != value)
                    unique.append(value);
            }
            return unique;
        };
        auto fast = collect([&](auto&& functor) { inst.forEachTmpFast(functor); });
        auto viaArgs = collect([&](auto&& functor) {
            inst.forEachArg([&](Arg& arg, Arg::Role, Bank, Width) { arg.forEachTmpFast(functor); });
        });
        RELEASE_ASSERT(fast == viaArgs, fast.size(), viaArgs.size());
    }

    void ensureUseDefLists()
    {
        if (m_hasUseDefLists)
            return;
        m_hasUseDefLists = true;

        const unsigned* tmps = m_mentions.tmps.span().data();
        const unsigned* firstOfInst = m_mentions.firstOfInst.span().data();
        const unsigned numberOfInsts = m_mentions.numberOfInsts;
        const unsigned numberOfMentions = firstOfInst[numberOfInsts];
        m_endOfGPUseDefLists = Tmp::absoluteIndexEnd(m_code, GP);
        const unsigned endOfGP = m_endOfGPUseDefLists;
        const unsigned numberOfLists = endOfGP + Tmp::absoluteIndexEnd(m_code, FP);
        auto listOf = [&](unsigned value) ALWAYS_INLINE_LAMBDA {
            int signedValue = static_cast<int>(value);
            return signedValue > 0 ? value : endOfGP - signedValue;
        };

        m_firstUseDef.fill(0, numberOfLists + 1);
        unsigned* firstUseDef = m_firstUseDef.mutableSpan().data();
        for (unsigned i = 0; i < numberOfMentions; ++i) {
            if (tmps[i] && !Tmp::tmpForInternalValue(tmps[i]).isReg())
                ++firstUseDef[listOf(tmps[i]) + 1];
        }
        for (unsigned list = 0; list < numberOfLists; ++list)
            firstUseDef[list + 1] += firstUseDef[list];

        m_useDefs.grow(firstUseDef[numberOfLists]);
        Point* useDefs = m_useDefs.mutableSpan().data();
        m_endOfUseDefs.grow(numberOfLists);
        unsigned* endOfUseDefs = m_endOfUseDefs.mutableSpan().data();
        for (unsigned list = 0; list < numberOfLists; ++list)
            endOfUseDefs[list] = firstUseDef[list];
        for (unsigned ordinal = 0; ordinal < numberOfInsts; ++ordinal) {
            Point instPoint = ordinal * PointOffsets::PointsPerInst;
            for (unsigned i = firstOfInst[ordinal]; i < firstOfInst[ordinal + 1]; ++i) {
                if (!tmps[i] || Tmp::tmpForInternalValue(tmps[i]).isReg())
                    continue;
                unsigned list = listOf(tmps[i]);
                unsigned end = endOfUseDefs[list];
                if (end != firstUseDef[list] && useDefs[end - 1] == instPoint)
                    continue;
                useDefs[end] = instPoint;
                endOfUseDefs[list] = end + 1;
            }
        }
    }

    std::span<const Point> useDefsOf(Tmp tmp)
    {
        ASSERT(m_hasUseDefLists && !tmp.isReg());
        unsigned list = tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : m_endOfGPUseDefLists + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
        if ((tmp.isGP() && list >= m_endOfGPUseDefLists) || list >= m_endOfUseDefs.size()) [[unlikely]] {
            auto iterator = m_useDefListsOfNewTmps.find(tmp);
            if (iterator == m_useDefListsOfNewTmps.end())
                return { };
            return iterator->value.useDefs().span();
        }
        return m_useDefs.span().subspan(m_firstUseDef[list], m_endOfUseDefs[list] - m_firstUseDef[list]);
    }

    // forEachUseDefWithin calls `func` for each instruction in the given interval that
    // uses or defs the given tmp, up to the end of the basic block.
    // Returns the unprocessed portion of the interval (if interval spans multiple blocks).
    // `cursor` can be used to perform a "sort-merge join" when the caller is making queries over a sorted set of intervals for the same tmp
    Interval forEachUseDefWithin(Tmp tmp, Interval interval, size_t& cursor, NOESCAPE const Invocable<void(Point, Inst&, BasicBlock&)> auto& func)
    {
        auto useDefs = useDefsOf(tmp);

        Point start = pointAtOffset(interval.begin(), PointOffsets::Pre);
        Point end = interval.end();

        ASSERT(useDefs.empty() || !cursor || useDefs[cursor - 1] < start);

        size_t i = cursor;
        for (; i < useDefs.size(); i++) {
            if (start <= useDefs[i])
                break;
        }
        if (i == useDefs.size() || end <= useDefs[i]) {
            cursor = i;
            return { }; // No more use/defs in `interval`
        }

        BasicBlock* block = findBlockContainingPoint(useDefs[i]);
        Point positionOfHead = this->positionOfHead(block);
        Point positionOfTail = this->positionOfTail(block);
        Point last = std::min(positionOfTail, end - 1);

        do {
            func(useDefs[i], block->at(instIndex(positionOfHead, useDefs[i])), *block);
            i++;
        } while (i < useDefs.size() && useDefs[i] <= last);
        cursor = i - 1; // i-1 since the next interval may include this final instruction
        return { last + 1, end }; // Return the unprocessed portion of interval
    }

#if USE(BUN_JSC_ADDITIONS)
    template<Bank bank>
    bool trySplitIntoClustersOverBlocks(Tmp tmp)
    {
        struct Group {
            BasicBlock* block { nullptr };
            Interval cluster;
            Point lastDefPoint { 0 };
            unsigned endOfPtrs { 0 };
            unsigned endOfColdUsePtrs { 0 };
            unsigned endOfPoints { 0 };
            unsigned root { 0 };
            unsigned numberOfPtrsInCluster { 0 };
            unsigned numberOfGroupsInCluster { 1 };
            float weight { 0 };
            bool isLiveIn { false };
            Tmp clusterTmp;
        };
        constexpr unsigned maxBlocksOnTheWays = 6;
        Vector<Group, 16> groups;
        Vector<Tmp*, 32> ptrs;
        Vector<std::pair<Tmp*, Point>, 8> coldUsePtrs;
        Vector<Point, 32> points;
        size_t cursor = 0;
        for (Interval interval : m_map.get<bank>(tmp).liveRange.intervals()) {
            Interval remaining = interval;
            do {
                Group group;
                remaining = forEachUseDefWithin(tmp, remaining, cursor, [&](Point point, Inst& inst, BasicBlock& block) {
                    inst.forEachTmp([&](Tmp& t, Arg::Role role, Bank, Width) {
                        if (t != tmp)
                            return;
                        Point early = point + PointOffsets::Early;
                        Interval timingInterval = intervalForTiming(early, Arg::timing(role));
                        if (Arg::isColdUse(role)) {
                            coldUsePtrs.append({ &t, timingInterval.begin() });
                            return;
                        }
                        ptrs.append(&t);
                        points.append(point);
                        if (Arg::isAnyDef(role))
                            group.lastDefPoint = early;
                        group.cluster |= timingInterval;
                        group.block = &block;
                        group.weight += adjustedBlockFrequency(&block);
                    });
                });
                if (!group.block) {
                    coldUsePtrs.shrink(groups.isEmpty() ? 0 : groups.last().endOfColdUsePtrs);
                    continue;
                }
                group.isLiveIn = interval.contains(pointAtOffset(group.cluster.begin(), PointOffsets::Pre));
                group.endOfPtrs = ptrs.size();
                group.endOfColdUsePtrs = coldUsePtrs.size();
                group.endOfPoints = points.size();
                group.root = groups.size();
                group.numberOfPtrsInCluster = ptrs.size() - (groups.isEmpty() ? 0 : groups.last().endOfPtrs);
                groups.append(group);
            } while (remaining);
        }

        RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
        calleeSaves.exclude(m_code.unsavedCalleeSaves());
        uint64_t regsLostAtCalls = 0;
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank]) {
            if (!calleeSaves.contains(reg, IgnoreVectors))
                regsLostAtCalls |= 1ULL << reg.index();
        }
        regsLostAtCalls &= m_allClobberedRegs;
        auto keepsSomeRegisterWithin = [&](Interval interval) {
            auto clobberPoints = m_clobberPoints.span();
            size_t index = std::ranges::lower_bound(clobberPoints, interval.begin()) - clobberPoints.begin();
            for (; index < clobberPoints.size() && clobberPoints[index] < interval.end(); ++index) {
                if ((m_clobberedRegsAtPoint[index] & regsLostAtCalls) == regsLostAtCalls)
                    return false;
            }
            return true;
        };
        auto isLiveThroughout = [&](Interval interval) {
            auto intervals = m_map.get<bank>(tmp).liveRange.intervals();
            auto live = std::ranges::upper_bound(intervals, interval.begin(), { }, [](Interval candidate) {
                return candidate.end();
            });
            return live != intervals.end() && live->begin() <= interval.begin() && interval.end() <= live->end();
        };
        Vector<Vector<Interval, 4>, 16> pieces(groups.size());
        for (unsigned i = 0; i < groups.size(); ++i) {
            Group& group = groups[i];
            pieces[i].append(group.cluster);
            if (!regsLostAtCalls || group.lastDefPoint || !group.isLiveIn || (i && groups[i - 1].block == group.block))
                continue;
            Vector<Interval, maxBlocksOnTheWays + 1> way;
            way.append(Interval(positionOfHead(group.block), group.cluster.end()));
            Vector<BasicBlock*, maxBlocksOnTheWays + 1> seen { group.block };
            auto earlier = groups.span().first(i);
            unsigned rootOfWays = i;
            bool isOpen = true;
            for (unsigned next = 0; isOpen && next < seen.size(); ++next) {
                BasicBlock* block = seen[next];
                isOpen = block->numPredecessors();
                for (BasicBlock* predecessor : block->predecessors()) {
                    if (predecessor->index() >= block->index()) {
                        isOpen = false;
                        break;
                    }
                    if (seen.contains(predecessor))
                        continue;
                    unsigned candidate = std::ranges::upper_bound(earlier, predecessor->index(), { }, [](const Group& other) {
                        return other.block->index();
                    }) - earlier.begin();
                    if (candidate && groups[candidate - 1].block == predecessor) {
                        const Group& above = groups[candidate - 1];
                        if ((rootOfWays != i && rootOfWays != above.root) || !keepsSomeRegisterWithin(above.cluster)) {
                            isOpen = false;
                            break;
                        }
                        rootOfWays = above.root;
                        way.append(Interval(above.cluster.end() - 1, positionOfTail(predecessor) + 1));
                        continue;
                    }
                    if (seen.size() > maxBlocksOnTheWays) {
                        isOpen = false;
                        break;
                    }
                    way.append(Interval(positionOfHead(predecessor), positionOfTail(predecessor) + 1));
                    seen.append(predecessor);
                }
            }
            if (!isOpen || rootOfWays == i)
                continue;
            for (Interval interval : way)
                isOpen &= isLiveThroughout(interval) && keepsSomeRegisterWithin(interval);
            if (!isOpen)
                continue;
            Group& root = groups[rootOfWays];
            group.root = rootOfWays;
            root.numberOfPtrsInCluster += group.numberOfPtrsInCluster;
            root.numberOfGroupsInCluster++;
            root.weight += group.weight;
            pieces[group.root].appendVector(way);
        }

        IntraBlockSplitMetadata* metadata = nullptr;
        for (unsigned i = 0; i < groups.size(); ++i) {
            if (groups[i].root != i || groups[i].numberOfPtrsInCluster < 2)
                continue;
            if (!metadata) {
                m_intraBlockMetadata.constructAndAppend(tmp);
                metadata = &m_intraBlockMetadata.last();
            }
            if (groups[i].isLiveIn)
                pieces[i].append(Interval(pointAtOffset(groups[i].cluster.begin(), PointOffsets::Pre)));
            if (groups[i].lastDefPoint)
                pieces[i].append(Interval(pointAtOffset(groups[i].lastDefPoint, PointOffsets::Post)));
            std::ranges::sort(pieces[i], { }, [](Interval interval) {
                return interval.begin();
            });
            Tmp clusterTmp = addSplitTmp(tmp, UseDefCost(groups[i].weight), { });
            TmpData& clusterData = m_map.get<bank>(clusterTmp);
            Interval current = pieces[i][0];
            for (Interval interval : pieces[i]) {
                if (interval.begin() <= current.end()) {
                    current |= interval;
                    continue;
                }
                clusterData.liveRange.append(m_intervalArena, current);
                current = interval;
            }
            clusterData.liveRange.append(m_intervalArena, current);
            clusterData.isClusterOverBlocks = groups[i].numberOfGroupsInCluster > 1;
            groups[i].clusterTmp = clusterTmp;
            m_stats[bank].numSplitIntraBlockClusterTmps++;
            metadata->splits.append({ clusterTmp, groups[i].lastDefPoint });
        }
        if (!metadata) {
            m_stats[bank].numSplitIntraBlockNoCluster++;
            return false;
        }
        for (unsigned i = 0; i < groups.size(); ++i) {
            Tmp clusterTmp = groups[groups[i].root].clusterTmp;
            if (!clusterTmp)
                continue;
            TmpData& clusterData = m_map.get<bank>(clusterTmp);
            for (unsigned k = i ? groups[i - 1].endOfPtrs : 0; k < groups[i].endOfPtrs; ++k)
                *ptrs[k] = clusterTmp;
            for (unsigned k = i ? groups[i - 1].endOfColdUsePtrs : 0; k < groups[i].endOfColdUsePtrs; ++k) {
                if (clusterData.liveRange.contains(coldUsePtrs[k].second)) {
                    *coldUsePtrs[k].first = clusterTmp;
                    clusterData.hasColdUse = true;
                }
            }
            if (clusterData.isClusterOverBlocks) {
                UseDefList& list = m_useDefListsOfNewTmps.ensure(clusterTmp, [] {
                    return UseDefList();
                }).iterator->value;
                for (unsigned k = i ? groups[i - 1].endOfPoints : 0; k < groups[i].endOfPoints; ++k)
                    list.add(points[k]);
            }
        }
        for (unsigned i = 0; i < groups.size(); ++i) {
            if (Tmp clusterTmp = groups[i].clusterTmp)
                setStageAndEnqueue(clusterTmp, m_map.get<bank>(clusterTmp), Stage::TryAllocate);
        }
        spill(tmp, m_map.get<bank>(tmp));
        m_stats[bank].numSplitIntraBlock++;
        dataLogLnIf(verbose(), "Split (clusters over blocks): ", *metadata);
        return true;
    }
#endif

    template<Bank bank>
    bool trySplitIntraBlock(Tmp tmp, TmpData& tmpData)
    {
        if (tmpData.splitAroundClobbersMetadataIndex)
            return false;
#if USE(BUN_JSC_ADDITIONS)
        if (tmpData.goesRoundAStretch)
            return false;
#endif

        if (isConstDef<bank>(tmp))
            return false; // Constant will be rematerialized instead

        // Don't split an already intra-block tmp. Otherwise, we might recursively try to
        // split a cluster tmp that couldn't be allocated.
        if (isLiveRangeBlockLocal(tmpData.liveRange))
            return false;

        ensureUseDefLists();
#if USE(BUN_JSC_ADDITIONS)
        if (m_code.chargesForCalleeSaves() && !tmpData.isClusterOverBlocks)
            return trySplitIntoClustersOverBlocks<bank>(tmp);
#endif

        IntraBlockSplitMetadata* metadata = nullptr;
        Vector<Tmp*, 8> tmpPtrs;
        Vector<std::pair<Tmp*, Point>, 4> coldUsePtrs;
        size_t cursor = 0;

        size_t numIntervals = tmpData.liveRange.intervals().size();
        for (size_t i = 0; i < numIntervals; i++) {
            // Note: this loop calls newTmp() which invalidates the tmpData reference.
            Interval interval = *(m_map.get<bank>(tmp).liveRange.intervals().begin() + i);
            Interval remaining = interval;

            do {
                Point lastDefPoint = 0;
                Interval cluster = { };
                tmpPtrs.shrink(0);
                coldUsePtrs.shrink(0);

                remaining = forEachUseDefWithin(tmp, remaining, cursor, [&](Point point, Inst& inst, BasicBlock&) {
                    inst.forEachTmp([&](Tmp& t, Arg::Role role, Bank, Width) {
                        if (t != tmp)
                            return;
                        Point early = point + PointOffsets::Early;
                        Interval timingInterval = intervalForTiming(early, Arg::timing(role));
                        if (Arg::isColdUse(role))
                            coldUsePtrs.append({ &t, timingInterval.begin() });
                        else {
                            tmpPtrs.append(&t);
                            if (Arg::isAnyDef(role))
                                lastDefPoint = early; // Remember where the fixup store to spill is needed
                            cluster |= timingInterval;
                        }
                    });
                });

                if (tmpPtrs.size() > 1) {
                    ASSERT(cluster);
                    if (!metadata) {
                        m_intraBlockMetadata.constructAndAppend(tmp);
                        metadata = &m_intraBlockMetadata.last();
                    }
                    Point pre = pointAtOffset(cluster.begin(), PointOffsets::Pre);
                    // If the Tmp is live into the cluster then cluster range is extended to model the fixup load from
                    // the Tmp's spill slot before the first use. This also allows insertSplitIntraBlockFixupCode()
                    // to quickly determine whether the fixup load is needed.
                    if (interval.contains(pre))
                        cluster |= Interval(pre);
                    // Likewise, if the cluster defs Tmp then a store to Tmp's spill slot will be needed after the final def point.
                    if (lastDefPoint)
                        cluster |= Interval(pointAtOffset(lastDefPoint, PointOffsets::Post));

                    BasicBlock* block = findBlockContainingPoint(cluster.begin());
                    Tmp clusterTmp = addSplitTmp(tmp, UseDefCost(tmpPtrs.size() * adjustedBlockFrequency(block)), cluster);
                    TmpData& clusterData = m_map.get<bank>(clusterTmp);
                    m_stats[bank].numSplitIntraBlockClusterTmps++;
                    for (auto& ptr : tmpPtrs)
                        *ptr = clusterTmp; // Within this cluster, use clusterTmp rather than Tmp
                    for (auto& [ptr, point] : coldUsePtrs) {
                        // Include coldUses only when they are within the warm cluster
                        if (cluster.contains(point)) {
                            *ptr = clusterTmp;
                            clusterData.hasColdUse = true;
                        }
                    }
                    metadata->splits.append({ clusterTmp, lastDefPoint });
                    setStageAndEnqueue(clusterTmp, clusterData, Stage::TryAllocate);
                }
            } while (remaining);
        }

        if (!metadata) {
            m_stats[bank].numSplitIntraBlockNoCluster++;
            return false;
        }

        // The original Tmp is spilled, but the cluster Tmps will hopefully carry the value in registers
        // within the cluster intervals.
        spill(tmp, m_map.get<bank>(tmp));
        m_stats[bank].numSplitIntraBlock++;
        dataLogLnIf(verbose(), "Split (intra-block): ", *metadata);
        return true;
    }

    static unsigned LEGACY_NODELETE stackSlotMinimumWidth(Width width)
    {
        if (width <= Width32)
            return 4;
        if (width <= Width64)
            return 8;
        ASSERT(width == Width128);
        return 16;
    }

    void spill(Tmp tmp, TmpData& tmpData)
    {
        RELEASE_ASSERT(tmpData.spillCost() != unspillableCost);
        ASSERT(tmpData.assigned == Reg());
        tmpData.stage = Stage::Spilled;
        m_newlySpilledTmps.append(tmp);

        m_stats[tmp.bank()].numSpilledTmps++;
        dataLogLnIf(verbose(), "Spilled ", tmp);

        if (tmpData.splitAroundClobbersMetadataIndex) {
            auto& metadata = m_aroundClobbersMetadata[tmpData.splitAroundClobbersMetadataIndex];
            // Splitting didn't prevent originalTmp from spilling after all, so no point assigning
            // registers or stack slots to the gap tmps for this split.
            dataLogLnIf(verbose(), "   evicting tmps created during split");
            ASSERT(metadata.originalTmp == tmp);
            for (auto& split : metadata.splits) {
                Tmp gapTmp = split.tmp;
                Reg reg = m_map[gapTmp].assigned;
                if (reg)
                    evict(gapTmp, m_map[gapTmp], reg);
                m_map[gapTmp].stage = Stage::Replaced;
            }
        }
        // Batch the generation of spill/fill tmps so that we can limit traversals of the code while
        // not tracking each tmp's use/defs explicitly.
        m_needsEmitSpillCode = true;
        tmpData.validate();
    }

    bool LEGACY_NODELETE queueContainsOnlySpills()
    {
        for (auto& elem : m_queue) {
            if (elem.stage() != Stage::Spill)
                return false;
        }
        for (size_t i = m_nextInQueueAtStart; i < m_queueAtStart.size(); ++i) {
            if (m_queueAtStart[i].stage() != Stage::Spill)
                return false;
        }
        return true;
    }

    Opcode moveOpcode(Tmp tmp)
    {
        Opcode move = Oops;
        Width width = m_tmpWidth.requiredWidth(tmp);
        switch (stackSlotMinimumWidth(width)) {
        case 4:
            move = tmp.bank() == GP ? Move32 : MoveFloat;
            break;
        case 8:
            move = tmp.bank() == GP ? Move : MoveDouble;
            break;
        case 16:
            ASSERT(tmp.bank() == FP);
            move = MoveVector;
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        return move;
    }

    template <Bank bank>
    void emitSpillCodeAndEnqueueNewTmps()
    {
        ensureUseDefLists();
        std::ranges::sort(m_newlySpilledTmps, [](Tmp a, Tmp b) {
            return a.tmpIndex(bank) < b.tmpIndex(bank);
        });
        Vector<unsigned> ordinals;
        for (Tmp tmp : m_newlySpilledTmps) {
            ASSERT(tmp.bank() == bank);
            TmpData& tmpData = m_map.get<bank>(tmp);
            ASSERT(tmpData.stage == Stage::Spilled);

            ensureSpillSlotTableEntry(tmpData);
            StackSlot*& slot = m_spillSlotTable[tmpData.spillSlotTableIndex];
            if (!slot) {
                slot = m_code.addStackSlot(stackSlotMinimumWidth(m_tmpWidth.requiredWidth(tmp)), StackSlotKind::Spill);
                m_stats[bank].numSpillStackSlots++;
            }
            ASSERT(spillSlot<bank>(tmp));

            if (!tmpData.splitFrom) {
                for (Point point : useDefsOf(tmp))
                    ordinals.append(point / PointOffsets::PointsPerInst);
                continue;
            }
            if (tmpData.liveRange.intervals().isEmpty())
                continue;
            Point first = pointAtOffset(tmpData.liveRange.intervals().first().begin(), PointOffsets::Pre);
            Point end = tmpData.liveRange.intervals().last().end();
            auto useDefs = useDefsOf(tmpData.splitFrom);
            for (auto iterator = std::ranges::lower_bound(useDefs, first); iterator != useDefs.end() && *iterator < end; ++iterator)
                ordinals.append(*iterator / PointOffsets::PointsPerInst);
        }
        m_newlySpilledTmps.shrink(0);
        std::ranges::sort(ordinals);
        removeRepeatedElements(ordinals);

        for (unsigned ordinal : ordinals) {
            {
                BasicBlock* block = m_code[m_blockIndexOfInst[ordinal]];
                Point positionOfHead = this->positionOfHead(block);
                unsigned instIndex = ordinal - positionOfHead / PointOffsets::PointsPerInst;
                Inst& inst = block->at(instIndex);

                bool mayMentionSpilledTmp = false;
                inst.forEachTmpFast([&](Tmp& tmp) {
                    if (!mayMentionSpilledTmp && tmp.bank() == bank && spillSlot<bank>(tmp))
                        mayMentionSpilledTmp = true;
                });
                if (!mayMentionSpilledTmp)
                    continue;

                unsigned indexOfEarly = positionOfEarly(positionOfHead, instIndex);

                bool useMove32IfDidSpill = false;
                bool didSpill = false;
                bool needScratch = false;
                Tmp scratchForTmp;

                if (bank == GP && inst.kind.opcode == Move) {
                    Arg& srcArg = inst.args()[0];
                    Arg& dstArg = inst.args()[1];
                    if (dstArg.isTmp() && spillSlot(dstArg.tmp())) {
                        // Storing to spill. If storing to a 4-byte slot, use store32, otherwise storePtr.
                        useMove32IfDidSpill = spillSlot(dstArg.tmp())->byteSize() == 4;
                    } else if (srcArg.isTmp() && spillSlot(srcArg.tmp())) {
                        // Loading from a spill slot (but not storing to a spill slot). If loading from a
                        // 4-byte slot, then use load32, otherwise loadPtr.
                        useMove32IfDidSpill = spillSlot(srcArg.tmp())->byteSize() == 4;
                    }
                }

                bool maybeCoalescable = this->mayBeCoalescable(inst);
                // Try to replace the register use by memory use when possible.
                inst.forEachArg(
                    [&] (Arg& arg, Arg::Role role, Bank argBank, Width width) {
                        if (!arg.isTmp())
                            return;
                        if (argBank != bank)
                            return;
                        StackSlot* spilled = spillSlot<bank>(arg.tmp());
                        if (!spilled)
                            return;
                        ASSERT(!arg.isReg());
                        bool needScratchIfSpilledInPlace = false;
                        if (!inst.admitsStack(arg)) {
                            switch (inst.kind.opcode) {
                            case Move:
                            case Move32:
                            case MoveDouble:
                            case MoveFloat:
                            case MoveVector: {
                                unsigned argIndex = &arg - &inst.args()[0];
                                unsigned otherArgIndex = argIndex ^ 1;
                                Arg otherArg = inst.args()[otherArgIndex];
                                if (inst.args().size() == 2
                                    && otherArg.isStack()
                                    && otherArg.stackSlot()->isSpill()) {
                                    needScratchIfSpilledInPlace = true;
                                    break;
                                }
                                return;
                            }
                            default:
                                return;
                            }
                        }
                        // If the Tmp holds a constant then we want to rematerialize its
                        // value rather than loading it from the stack.
                        unsigned tmpIndex = AbsoluteTmpMapper<bank>::absoluteIndex(arg.tmp());
                        ASSERT_IMPLIES(Arg::isColdUse(role), m_map.get<bank>(arg.tmp()).hasColdUse);
                        if (!Arg::isColdUse(role) && m_useCounts.isConstDef<bank>(tmpIndex)) {
                            // We do not handle FP here as there is almost zero instructions which can take FPImm except for Move.
                            if constexpr (bank == GP) {
                                int64_t value = m_useCounts.constant<bank>(tmpIndex);
                                Arg oldArg = arg;
                                Arg imm;
                                if (Arg::isValidImmForm(value))
                                    imm = Arg::imm(value);
                                else
                                    imm = Arg::bigImm(value);
                                ASSERT(inst.isValidForm());
                                arg = imm;
                                if (inst.isValidForm()) {
                                    m_stats[bank].numRematerializeConst++;
                                    dataLogLnIf(verbose(), "Rematerialized (direct imm), BB", *block, " arg=", oldArg, ", inst=", inst);
                                    return;
                                }
                                // Couldn't insert the immediate into the instruction directly, so undo.
                                arg = oldArg;
                            }
                            // We can still rematerialize it into a register. In order for that optimization to kick in,
                            // we need to avoid placing the Tmp's stack address into the instruction.
                            return;
                        }

                        Arg spilledArg = Arg::stack(spilled);
                        if (Arg::isZDef(role) && bytesForWidth(width) < spilled->byteSize()) {
                            // ZDef32 to 64 slot would require two 32-bit accesses (second one for zero extend), so
                            // usually it will be better to ZDef into a register and then storePtr the register.
                            // Unless, this move can have its live range coalesced.
                            ASSERT_IMPLIES(maybeCoalescable, &arg == &inst.args()[1]);
                            if (!maybeCoalescable || spilledArg != inst.args()[0]) {
                                m_stats[bank].numInPlaceSpillGiveUpSpillWidth++;
                                return;
                            }
                        }
                        spilled->ensureSize(useMove32IfDidSpill ? 4 : bytesForWidth(width));
                        didSpill = true;
                        if (needScratchIfSpilledInPlace) {
                            needScratch = true;
                            scratchForTmp = arg.tmp();
                        }
                        arg = spilledArg;
                        m_stats[bank].numInPlaceSpill++;
                    });

                if (didSpill && useMove32IfDidSpill)
                    inst.kind.opcode = Move32;

                if (needScratch) {
                    ASSERT(scratchForTmp != Tmp());
                    // This has become a Move spillN, spillM. If N==M, we can remove this instruction. Otherwise,
                    // a scratch register is needed in order to execute the move between spill slots.
                    if (maybeCoalescable && inst.args()[0] == inst.args()[1]) {
                        m_stats[bank].numCoalescedStackSlotMoves++;
                        inst = Inst(); // Will be removed during assignRegisters final pass
                        continue;
                    }
                    Tmp tmp = addSpillTmpWithInterval(scratchForTmp, intervalForSpill(indexOfEarly, Arg::Scratch));
                    ASSERT(inst.args().size() == 2);
                    inst.setArgs(inst.args()[0], inst.args()[1], tmp);
                    RELEASE_ASSERT(inst.args().size() == 3);
                    m_stats[bank].numMoveSpillSpillInsts++;
                    ASSERT(inst.isValidForm());
                    // WTF::Liveness and Air::LivenessAdapter do not handle a late-def/use followed by early-def
                    // correctly. While this register allocator does handle it correctly (since it models distinct
                    // late and early points between instructions (i.e. intervalForSpill() won't overlap for different
                    // scratch Tmps)), insert a Padding inst so that subsequent liveness analysis correctly computes
                    // lifetime interference when there are back-to-back Move spill-spill-scratch instructions
                    // (scratch is early-def, late-use).
                    // See https://bugs.webkit.org/show_bug.cgi?id=163548#c2 for more info.
                    // FIXME: reconsider this, https://bugs.webkit.org/show_bug.cgi?id=288122
#if USE(BUN_JSC_ADDITIONS)
                    insert(block, instIndex, SpillMoveFrom, Padding, inst.origin);
#else
                    m_insertionSets[block].insert(instIndex, SpillMoveFrom, Padding, inst.origin);
#endif
                    continue;
                }

                bool doKillInst = false;
                // For every other case, add Load/Store as needed.
                inst.forEachTmp([&] (Tmp& tmp, Arg::Role role, Bank argBank, Width) {
                    if (tmp.isReg() || argBank != bank)
                        return;
                    StackSlot* spilled = spillSlot<bank>(tmp);
                    if (!spilled)
                        return;

                    Opcode move = moveOpcode(tmp);
                    auto originalTmp = tmp;
                    auto tmpIndex = AbsoluteTmpMapper<bank>::absoluteIndex(tmp);

                    bool canRematerializeConstant = m_useCounts.isConstDef<bank>(tmpIndex);
                    ASSERT_IMPLIES(canRematerializeConstant, !(Arg::isAnyUse(role) && Arg::isAnyDef(role)));
                    ASSERT_IMPLIES(canRematerializeConstant, role != Arg::Scratch);

                    bool canKillDef = canRematerializeConstant && !m_map.get<bank>(originalTmp).hasColdUse;

                    if (Arg::isAnyUse(role) || (!canKillDef && Arg::isAnyDef(role)))
                        tmp = addSpillTmpWithInterval(tmp, intervalForSpill(indexOfEarly, role));

                    if (role == Arg::Scratch)
                        return;

                    Arg arg = Arg::stack(spilled);
                    if (Arg::isAnyUse(role)) {
                        auto tryRematerialize = [&]() {
                            if (!canRematerializeConstant)
                                return false;

                            if constexpr (bank == GP) {
                                int64_t value = m_useCounts.constant<bank>(tmpIndex);
                                if (Arg::isValidImmForm(value) && isValidForm(Move, Arg::Imm, Arg::Tmp)) {
#if USE(BUN_JSC_ADDITIONS)
                                    insert(block, instIndex, SpillMoveFrom, Move, inst.origin, Arg::imm(value), tmp);
#else
                                    m_insertionSets[block].insert(instIndex, SpillMoveFrom, Move, inst.origin, Arg::imm(value), tmp);
#endif
                                    m_stats[bank].numRematerializeConst++;
                                    dataLogLnIf(verbose(), "Rematerialized (imm) BB", *block, " ", originalTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                    return true;
                                }
                                RELEASE_ASSERT(isValidForm(Move, Arg::BigImm, Arg::Tmp));
#if USE(BUN_JSC_ADDITIONS)
                                insert(block, instIndex, SpillMoveFrom, Move, inst.origin, Arg::bigImm(value), tmp);
#else
                                m_insertionSets[block].insert(instIndex, SpillMoveFrom, Move, inst.origin, Arg::bigImm(value), tmp);
#endif
                                m_stats[bank].numRematerializeConst++;
                                dataLogLnIf(verbose(), "Rematerialized (bigImm) BB", *block, " ", originalTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                return true;
                            } else {
                                v128_t constant = m_useCounts.constant<bank>(tmpIndex);
                                Width constWidth = m_useCounts.constantWidth<bank>(tmpIndex);
                                Arg imm;
                                Opcode constMove = Oops;
                                switch (constWidth) {
                                case Width32:
                                    if (Arg::isValidFPImm32Form(constant.u64x2[0]))
                                        imm = Arg::fpImm32(constant.u64x2[0]);
                                    constMove = MoveFloat;
                                    break;
                                case Width64:
                                    if (Arg::isValidFPImm64Form(constant.u64x2[0]))
                                        imm = Arg::fpImm64(constant.u64x2[0]);
                                    constMove = MoveDouble;
                                    break;
                                case Width128:
                                    if (Arg::isValidFPImm128Form(constant))
                                        imm = Arg::fpImm128(constant);
                                    constMove = MoveVector;
                                    break;
                                default:
                                    RELEASE_ASSERT_NOT_REACHED();
                                }

                                if (imm && isValidForm(constMove, imm.kind(), Arg::Tmp)) {
#if USE(BUN_JSC_ADDITIONS)
                                    insert(block, instIndex, SpillMoveFrom, constMove, inst.origin, imm, tmp);
#else
                                    m_insertionSets[block].insert(instIndex, SpillMoveFrom, constMove, inst.origin, imm, tmp);
#endif
                                    m_stats[bank].numRematerializeConst++;
                                    dataLogLnIf(verbose(), "Rematerialized (FP) BB", *block, " ", originalTmp, ": ", tmp);
                                    return true;
                                }

                                return false;
                            }
                        };

                        if (!tryRematerialize()) {
#if USE(BUN_JSC_ADDITIONS)
                            insert(block, instIndex, SpillMoveFrom, move, inst.origin, arg, tmp);
#else
                            m_insertionSets[block].insert(instIndex, SpillMoveFrom, move, inst.origin, arg, tmp);
#endif
                            m_stats[bank].numLoadSpill++;
                        }
                    }
                    if (Arg::isAnyDef(role)) {
                        if (canKillDef) {
                            ASSERT(inst.kind.opcode == Move || inst.kind.opcode == Move32
                                || inst.kind.opcode == MoveFloat || inst.kind.opcode == MoveDouble
                                || inst.kind.opcode == MoveVector);
                            ASSERT(inst.args()[0].isSomeImm() && inst.args()[1] == originalTmp);
                            doKillInst = true;
                            dataLogLnIf(verbose(), "Rematerialized BB", *block, " removing def inst: ", inst);
                        } else {
                            ASSERT(!doKillInst);
#if USE(BUN_JSC_ADDITIONS)
                            insert(block, instIndex + 1, SpillMoveTo, move, inst.origin, tmp, arg);
#else
                            m_insertionSets[block].insert(instIndex + 1, SpillMoveTo, move, inst.origin, tmp, arg);
#endif
                            m_stats[bank].numStoreSpill++;
                        }
                    }
                });
                ASSERT(inst.isValidForm());
                if (doKillInst)
                    inst = Inst(); // Will be removed by assignRegisters()
            }
        }

        if (Options::airValidateGreedRegAlloc()) [[unlikely]] {
            for (BasicBlock* block : m_code) {
                for (Inst& inst : *block) {
                    inst.forEachTmpFast([&](Tmp& tmp) {
                        RELEASE_ASSERT(tmp.bank() != bank || !spillSlot<bank>(tmp));
                    });
                }
            }
        }
    }

#if USE(BUN_JSC_ADDITIONS)
    void validateMovesRoundClobbers()
    {
        auto& moves = m_movesRoundClobbers;
        std::ranges::sort(moves, [](const MoveRoundClobber& a, const MoveRoundClobber& b) {
            return std::tuple(a.blockIndex, a.index, a.isBehind) < std::tuple(b.blockIndex, b.index, b.isBehind);
        });
        bool anyFailures = false;
        for (size_t begin = 0, end; begin < moves.size(); begin = end) {
            for (end = begin + 1; end < moves.size() && moves[end].blockIndex == moves[begin].blockIndex && moves[end].index == moves[begin].index && moves[end].isBehind == moves[begin].isBehind; ++end) { }
            for (size_t i = begin; i < end; ++i) {
                for (size_t j = begin; j < end; ++j) {
                    if (i == j || !moves[i].source || moves[i].source != moves[j].destination)
                        continue;
                    dataLogLn("AIR GREEDY REGISTER ALLOCATION VALIDATION FAILURE: MOVES ROUND A CLOBBER");
                    dataLogLn("   In BB#", moves[i].blockIndex, moves[i].isBehind ? " behind" : " ahead of", " index ", moves[i].index, ": ", moves[i].source, " -> ", moves[i].destination, " and ", moves[j].source, " -> ", moves[j].destination);
                    anyFailures = true;
                }
            }
        }
        if (anyFailures) {
            dataLogLn("IR:\n", m_code);
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

#endif
    void insertFixupCode()
    {
        // Skip index 0 which is sacrificed so that splitAroundClobbersMetadataIndex == 0 means "none".
        for (size_t i = 1; i < m_aroundClobbersMetadata.size(); i++)
            insertSplitAroundClobbersFixupCode(m_aroundClobbersMetadata[i]);
#if USE(BUN_JSC_ADDITIONS)
        validateMovesRoundClobbers();
#endif
        for (auto& metadata : m_intraBlockMetadata)
            insertSplitIntraBlockFixupCode(metadata);

        if (Options::airGreedyRegAllocSplitAroundLoops()) {
            HashMap<Tmp, unsigned> loopTmpToMetadataIndex;
            for (unsigned index = 0; index < m_aroundLoopMetadata.size(); index++)
                loopTmpToMetadataIndex.add(m_aroundLoopMetadata[index].loopTmp, index);

            // Loop fixups for different Tmps may introduce register cycles. Use Shuffle to deal with potential register conflicts.
            HashMap<BasicBlock*, Vector<ShufflePair>> loopEntryFixups;
            HashMap<BasicBlock*, Vector<ShufflePair>> loopExitFixups;

            for (auto& metadata : m_aroundLoopMetadata)
                insertSplitAroundLoopFixupCode(metadata, loopTmpToMetadataIndex, loopEntryFixups, loopExitFixups);

            if (!loopEntryFixups.isEmpty() || !loopExitFixups.isEmpty())
                m_code.proc().setUsesShuffle(true);

            for (auto& [block, shufflePairs] : loopEntryFixups)
#if USE(BUN_JSC_ADDITIONS)
                insertInst(block, block->size() - 1, AroundLoopEntryFixup, createShuffle(block->at(block->size() - 1).origin, shufflePairs.span()));
#else
                m_insertionSets[block].insertInst(block->size() - 1, AroundLoopEntryFixup, createShuffle(block->at(block->size() - 1).origin, shufflePairs.span()));
#endif
            for (auto& [block, shufflePairs] : loopExitFixups)
#if USE(BUN_JSC_ADDITIONS)
                insertInst(block, 0, AroundLoopExitFixup, createShuffle(block->at(0).origin, shufflePairs.span()));
#else
                m_insertionSets[block].insertInst(0, AroundLoopExitFixup, createShuffle(block->at(0).origin, shufflePairs.span()));
#endif
        }

#if USE(BUN_JSC_ADDITIONS)
        executeInsertions();
    }

    void insertInst(BasicBlock* block, size_t index, unsigned phase, Inst&& inst)
    {
        m_insertions.append(PhaseInsertion(index, phase, WTF::move(inst)));
        m_blockOfInsertion.append(block->index());
    }

    template<typename... Arguments>
    void insert(BasicBlock* block, size_t index, unsigned phase, Arguments&&... arguments)
    {
        insertInst(block, index, phase, Inst(std::forward<Arguments>(arguments)...));
    }

    void executeInsertions()
    {
        if (m_insertions.isEmpty())
            return;
        Vector<unsigned> endOfBlock(FillWith { }, m_code.size(), 0u);
        for (unsigned blockIndex : m_blockOfInsertion)
            ++endOfBlock[blockIndex];
        unsigned sum = 0;
        for (unsigned& end : endOfBlock)
            sum += std::exchange(end, sum);
        Vector<unsigned> order(m_insertions.size());
        for (unsigned i = 0; i < m_insertions.size(); ++i)
            order[endOfBlock[m_blockOfInsertion[i]]++] = i;
        PhaseInsertionSet insertionSet;
        unsigned begin = 0;
        for (unsigned blockIndex = 0; blockIndex < endOfBlock.size(); ++blockIndex) {
            unsigned end = endOfBlock[blockIndex];
            if (begin == end)
                continue;
            constexpr unsigned manyInsertions = 16;
            if (end - begin >= manyInsertions) {
                std::stable_sort(order.begin() + begin, order.begin() + end, [&](unsigned left, unsigned right) {
                    return m_insertions[left] < m_insertions[right];
                });
            }
            for (; begin < end; ++begin)
                insertionSet.appendInsertion(WTF::move(m_insertions[order[begin]]));
            insertionSet.execute(m_code[blockIndex]);
        }
        m_insertions.shrink(0);
        m_blockOfInsertion.shrink(0);
#else
        for (BasicBlock* block : m_code)
            m_insertionSets[block].execute(block);
#endif
    }

    void insertSplitAroundClobbersFixupCode(AroundClobbersSplitMetadata& metadata)
    {
        ASSERT(metadata.originalTmp);

        if (m_map[metadata.originalTmp].stage == Stage::Spilled) {
            m_stats[metadata.originalTmp.bank()].numSplitAroundClobberSpilled++;
            return; // If spilled, better to not split after all. See spill().
        }
        ASSERT(assignedReg(metadata.originalTmp));
        // Emit moves to and from the gapTmps (or stack stot) that fill the split holes.
        for (auto& split : metadata.splits) {
            Tmp gapTmp = split.tmp;
            TmpData& gapData = m_map[gapTmp];
            for (auto& interval : gapData.liveRange.intervals()) {
#if USE(BUN_JSC_ADDITIONS)
                ASSERT(interval.distance() >= 2);
#else
                ASSERT(interval.distance() == 2);
#endif
                Point lastPoint = interval.end() - 1;
                BasicBlock* block = findBlockContainingPoint(lastPoint);
                unsigned instIndex = this->instIndex(positionOfHead(block), lastPoint);
                Inst& inst = block->at(instIndex);

                Arg arg = gapTmp;
                StackSlot* spilled = spillSlot(gapTmp);
                if (spilled)
                    arg = Arg::stack(spilled);
                Opcode move = moveOpcode(gapTmp);
#if USE(BUN_JSC_ADDITIONS)
                unsigned indexAhead = this->instIndex(positionOfHead(block), interval.begin());
                if (Options::airValidateGreedRegAlloc()) [[unlikely]] {
                    Reg home = assignedReg(metadata.originalTmp);
                    Reg away = spilled ? Reg() : assignedReg(gapTmp);
                    m_movesRoundClobbers.append({ block->index(), indexAhead, false, home, away });
                    m_movesRoundClobbers.append({ block->index(), instIndex + 1, true, away, home });
                    if (Options::airGreedyRegAllocRecordsFalseMoveForTesting() && away)
                        m_movesRoundClobbers.append({ block->index(), indexAhead, false, away, home });
                }
                insert(block, indexAhead, ClobberMoveFrom, move, inst.origin, metadata.originalTmp, arg);
#else
                m_insertionSets[block].insert(instIndex, ClobberMoveFrom, move, inst.origin, metadata.originalTmp, arg);
#endif
#if USE(BUN_JSC_ADDITIONS)
                insert(block, instIndex + 1, ClobberMoveTo, move, inst.origin, arg, metadata.originalTmp);
#else
                m_insertionSets[block].insert(instIndex + 1, ClobberMoveTo, move, inst.origin, arg, metadata.originalTmp);
#endif
#if USE(BUN_JSC_ADDITIONS)
                if (spilled)
                    m_stats[gapTmp.bank()].numSplitAroundClobberStackMoves += 2;
                else
                    m_stats[gapTmp.bank()].numSplitAroundClobberRegisterMoves += 2;
#endif
                dataLogLnIf(verbose(), "Inserted Moves around clobber tmp=", metadata.originalTmp, " gapTmp=", gapTmp, " gapReg = ", assignedReg(gapTmp), " block=", *block, " index=", instIndex, " inst = ", inst);
            }
        }
    }

    void insertSplitIntraBlockFixupCode(IntraBlockSplitMetadata& metadata)
    {

        Tmp originalTmp = metadata.originalTmp;
        Bank bank = originalTmp.bank();
        Opcode move = moveOpcode(originalTmp);
        StackSlot* spilled = spillSlot(originalTmp);
        ASSERT(spilled);

        for (auto& split : metadata.splits) {
            Tmp clusterTmp = split.tmp;
            if (spillSlot(clusterTmp)) {
                ASSERT(spillSlot(clusterTmp) == spillSlot(originalTmp));
                m_stats[bank].numSplitIntraBlockClusterTmpsSpilled++;
                continue; // No fixup needed since the same spill slot is used
            }

            LiveRange& clusterRange = m_map[clusterTmp].liveRange;
            // Cluster tmp may have multiple intervals if it was also split around clobbers,
            // but the first interval will still coincide with the beginning of the cluster.
            Point clusterBegin = clusterRange.intervals().first().begin();

            BasicBlock* block = findBlockContainingPoint(clusterBegin);
            Point positionOfHead = this->positionOfHead(block);

            // trySplitIntraBlock includes the Pre point in the cluster interval iff the original Tmp is live into the cluster.
            if (pointAtOffset(clusterBegin, PointOffsets::Pre) == clusterBegin) {
                unsigned instIndex = this->instIndex(positionOfHead, clusterBegin);
#if USE(BUN_JSC_ADDITIONS)
                insert(block, instIndex, IntraBlockMoveFrom, move, block->at(instIndex).origin, Arg::stack(spilled), clusterTmp);
#else
                m_insertionSets[block].insert(instIndex, IntraBlockMoveFrom, move, block->at(instIndex).origin, Arg::stack(spilled), clusterTmp);
#endif
                m_stats[bank].numSplitIntraBlockLoad++;
            } else
                ASSERT(split.lastDefPoint);

            // Need to store back into the original Tmp's spill slot only if the cluster def'ed the Tmp
            if (split.lastDefPoint) {
                unsigned instIndex = this->instIndex(positionOfHead, split.lastDefPoint);
#if USE(BUN_JSC_ADDITIONS)
                insert(block, instIndex + 1, IntraBlockMoveTo, move, block->at(instIndex).origin, clusterTmp, Arg::stack(spilled));
#else
                m_insertionSets[block].insert(instIndex + 1, IntraBlockMoveTo, move, block->at(instIndex).origin, clusterTmp, Arg::stack(spilled));
#endif
                m_stats[bank].numSplitIntraBlockStore++;
            }
        }
    }

    void insertSplitAroundLoopFixupCode(AroundLoopSplitMetadata& metadata, const HashMap<Tmp, unsigned>& loopTmpToMetadataIndex, HashMap<BasicBlock*, Vector<ShufflePair>>& entryFixups, HashMap<BasicBlock*, Vector<ShufflePair>>& exitFixups)
    {
        Tmp nonLoopTmp = metadata.originalTmp;
        Tmp loopTmp = metadata.loopTmp;
        const NaturalLoop& loop = *metadata.loop;
        BasicBlock* header = loop.header();

        Bank bank = nonLoopTmp.bank();
        if (spillSlot(loopTmp))
            m_stats[bank].numSplitAroundLoopLoopSpilled++;
        if (spillSlot(nonLoopTmp))
            m_stats[bank].numSplitAroundLoopNonLoopSpilled++;
        if (spillSlot(loopTmp) && spillSlot(nonLoopTmp))
            m_stats[bank].numSplitAroundLoopBothSpilled++;

        auto argFor = [&](Tmp tmp) -> Arg {
            StackSlot* spilled = spillSlot(tmp);
            if (spilled)
                return Arg::stack(spilled);
            return tmp;
        };
        Arg nonLoopArg = argFor(nonLoopTmp);
        Arg loopArg = argFor(loopTmp);

        LiveRange& loopLiveRange = m_map[loopTmp].liveRange;
        Width width = canonicalWidth(m_tmpWidth.requiredWidth(nonLoopTmp));

        // Entry fixup: if either tmp got a register and the loop tmp is live at the header,
        // transfer nonLoopTmp to loopTmp at the end of each entry edge.
        if (nonLoopArg != loopArg && loopLiveRange.contains(positionOfHead(header))) {
            for (BasicBlock* pred : header->predecessors()) {
                if (m_naturalLoops->belongsTo(pred, loop))
                    continue; // Skip back-edge predecessors.
                // ensureDedicatedLoopEntryExitBlocks() guarantees non-critical entry edges.
                ASSERT(pred->numSuccessors() == 1 && pred->successor(0).block() == header);
                ASSERT(!nonLoopArg.isStack() || !loopArg.isStack());
                entryFixups.ensure(pred, [] {
                    return Vector<ShufflePair>();
                }).iterator->value.append(ShufflePair(nonLoopArg, loopArg, width));
            }
        }

        // Exit fixup: on each exit edge, transfer loopTmp to the destination nonLoopTmp.
        // For nested loop splits, traverse outward to find the right destination.
        IndexSet<BasicBlock*> visitedExitSuccessors;
        for (unsigned i = 0; i < loop.size(); i++) {
            BasicBlock* loopBlock = loop.at(i);
            // If loopTmp isn't live at the exiting block then this block is within a nested loop that
            // was also split. The inner fixup will handle this multi-level loop exit.
            if (!loopLiveRange.contains(positionOfTail(loopBlock)))
                continue;
            for (auto& succ : loopBlock->successors()) {
                if (m_naturalLoops->belongsTo(succ.block(), loop))
                    continue;
                if (!visitedExitSuccessors.add(succ.block()))
                    continue; // Already inserted fixup for this exit successor.
#if ASSERT_ENABLED
                // ensureDedicatedLoopEntryExitBlocks() ensured the exit successor has only loop
                // predecessors so it's safe to emit the fixup code in the successor block.
                for (BasicBlock* exitPred : succ.block()->predecessors())
                    ASSERT(m_naturalLoops->belongsTo(exitPred, loop));
#endif
                Point exitHead = positionOfHead(succ.block());
                // Find the nonLoopTmp that carries the value into succ block by traversing splits outward.
                Tmp traverseLoopTmp = loopTmp;
                std::optional<unsigned> destSplitIndex;
                bool anyDefsToRegisters = false;
                while (true) {
                    auto it = loopTmpToMetadataIndex.find(traverseLoopTmp);
                    if (it == loopTmpToMetadataIndex.end()) {
                        ASSERT(traverseLoopTmp != loopTmp);
                        destSplitIndex = std::nullopt;
                        break;
                    }
                    destSplitIndex = it->value;
                    auto& splitMetadata = m_aroundLoopMetadata[*destSplitIndex];
                    ASSERT(splitMetadata.loopTmp == traverseLoopTmp);
                    TmpData& loopTmpData = m_map[traverseLoopTmp];
                    anyDefsToRegisters |= loopTmpData.assigned && splitMetadata.loopHasDef;

                    if (m_map[splitMetadata.originalTmp].liveRange.contains(exitHead))
                        break;
                    traverseLoopTmp = splitMetadata.originalTmp;
                }

                // There won't be a destSplitIndex if the original range wasn't live out of this loop. In that case,
                // no exit fixup is needed.
                if (destSplitIndex.has_value()) {
                    Tmp destTmp = m_aroundLoopMetadata[*destSplitIndex].originalTmp;
                    Arg destArg = argFor(destTmp);
                    // If both sides spilled and the loop never wrote to a register, the
                    // shared spill slot already holds the correct value.
                    bool spillSlotAlreadyCoherent = destArg.isStack() && !anyDefsToRegisters;
                    if (loopArg != destArg && !spillSlotAlreadyCoherent) {
                        ASSERT(!loopArg.isStack() || !destArg.isStack());
                        exitFixups.ensure(succ.block(), [] {
                            return Vector<ShufflePair>();
                        }).iterator->value.append(ShufflePair(loopArg, destArg, width));
                    }
                }
            }
        }
        dataLogLnIf(verbose(), "AroundLoop fixup: nonLoop=", nonLoopTmp, " loop=", loopTmp, " header=BB", *header);
    }

    bool LEGACY_NODELETE mayBeCoalescable(Inst& inst)
    {
        switch (inst.kind.opcode) {
        case Move:
        case Move32:
        case MoveFloat:
        case MoveDouble:
        case MoveVector:
            break;
        default:
            return false;
        }

        // Avoid the three-argument coalescable spill moves.
        if (inst.args().size() != 2)
            return false;

        if (!inst.args()[0].isTmp() || !inst.args()[1].isTmp())
            return false;

        // We can coalesce a Move32 so long as either of the following holds:
        // - The input is already zero-filled.
        // - The output only cares about the low 32 bits.
        if (inst.kind.opcode == Move32 && m_tmpWidth.defWidth(inst.args()[0].tmp()) > Width32)
            return false;
        return true;
    }

    void assignRegisters()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::assignRegisters"_s);

        for (BasicBlock* block : m_code) {
            for (Inst& inst : *block) {
                bool mayBeCoalescable = this->mayBeCoalescable(inst);
                dataLogLnIf(verbose(), "At: ", inst, mayBeCoalescable ? " [coalescable]" : "");

                if constexpr (isX86_64()) {
                    // Move32 is cheaper if we know that it's equivalent to a Move in x86_64. It's
                    // equivalent if the destination's high bits are not observable or if the source's high
                    // bits are all zero.
                    if (inst.kind.opcode == Move && inst.args()[0].isTmp() && inst.args()[1].isTmp()) {
                        if (m_tmpWidth.useWidth(inst.args()[1].tmp()) <= Width32 || m_tmpWidth.defWidth(inst.args()[0].tmp()) <= Width32)
                            inst.kind.opcode = Move32;
                    }
                }
                if constexpr (isARM64()) {
                    // On the other hand, on ARM64, Move is cheaper than Move32. We would like to use Move instead of Move32.
                    // Move32 on ARM64 is explicitly selected in B3LowerToAir for ZExt32 for example. But using ZDef information
                    // here can optimize it from Move32 to Move.
                    if (inst.kind.opcode == Move32 && inst.args()[0].isTmp() && inst.args()[1].isTmp()) {
                        if (m_tmpWidth.defWidth(inst.args()[0].tmp()) <= Width32)
                            inst.kind.opcode = Move;
                    }
                }

                inst.forEachTmpFast([&](Tmp& tmp) {
                    if (tmp.isReg())
                        return;
                    Reg reg = assignedReg(tmp);
                    if (!reg) {
                        dataLogLn("Failed to allocate register: BB", *block, " inst=", inst, " tmp=", tmp);
                        dataLogLn("Reg Alloc State: ", *this);
                        RELEASE_ASSERT_NOT_REACHED();
                    }
                    tmp = Tmp(reg);
                });
                ASSERT(inst.isValidForm());

                if (mayBeCoalescable && inst.args()[0].isTmp() && inst.args()[1].isTmp()
                    && inst.args()[0].tmp() == inst.args()[1].tmp()) {
                    m_stats[inst.args()[0].tmp().bank()].numCoalescedRegisterMoves++;
                    inst = Inst();
                }
            }
            // Remove all the useless moves we created in this block.
            block->insts().removeAllMatching([&] (const Inst& inst) {
                return !inst;
            });
        }
    }

    Code& m_code;
    Vector<Reg> m_allowedRegistersInPriorityOrder[numBanks];
    ScalarRegisterSet m_allAllowedRegisters;
    ScalarRegisterSet m_freshCalleeSaves;
    std::array<unsigned, numBanks> m_numberOfFreshCalleeSaves { };
#if USE(BUN_JSC_ADDITIONS)
    std::array<unsigned, numBanks> m_numberOfCalleeSaves { };
#endif
    UseDefCost m_calleeSaveCost { 0 };
    IndexMap<BasicBlock*, Point> m_blockToHeadPoint;
    Vector<Point> m_tailPoints;
    Vector<unsigned> m_blockIndexOfInst;
    Mentions m_mentions;
    Vector<unsigned> m_movesOfTmps;
    Vector<uint8_t> m_isRead;
    const bool m_searchesForDeadCode;
    bool m_hasDefOfUnreadTmp { false };
    bool m_hasDeadCode { false };
    Vector<Tmp> m_tmpsWithCoalescables;
    Vector<unsigned> m_conditionalMoves;
    unsigned m_endOfGPIndices { 0 };
    unsigned m_numberOfLinearIndices { 0 };
    struct DefOfSmallConstant {
        unsigned index;
        int64_t value;
    };
    Vector<DefOfSmallConstant> m_defsOfSmallConstants;
    struct EndsOfBlock {
        unsigned firstInst { 0 };
        unsigned exposedUses { 0 };
        unsigned defs { 0 };
        unsigned usesAtTail { 0 };
    };
    Vector<EndsOfBlock> m_endsOfBlock;
    Vector<unsigned> m_exposedUses;
    Vector<unsigned> m_defsInBlocks;
    Vector<unsigned> m_usesAtTails;
    Vector<uint8_t> m_crossesBoundary;
    Vector<unsigned> m_indexOfColumn;
    Vector<uint64_t> m_liveAtBoundaries;
    size_t m_wordsPerSet { 0 };
    static constexpr size_t maxWordsPerLivenessMatrix = 1 << 19;
    struct AtEdgeOfBlock {
        unsigned blockIndex;
        unsigned index;
    };
    Vector<AtEdgeOfBlock> m_closesBehindBlock;
    Vector<AtEdgeOfBlock> m_opensAtTailOfBlock;
    bool m_livenessIsInLists { false };
    IntervalArena m_intervalArena;
    MapOfTmpData m_map;
    Vector<unsigned> m_firstUseDef;
    Vector<unsigned> m_endOfUseDefs;
    Vector<Point> m_useDefs;
    unsigned m_endOfGPUseDefLists { 0 };
    HashMap<Tmp, UseDefList> m_useDefListsOfNewTmps;
    Vector<Tmp> m_newlySpilledTmps;
    Vector<unsigned> m_firstClobberOfInst;
    static constexpr unsigned clobberPointsPerChunk = 64;
    static constexpr unsigned minClobberPointsForChunks = 1024;
    Vector<uint64_t> m_clobberedRegsInChunk;
    Vector<uint64_t> m_deadClobbersAtPoint;
    uint64_t m_regsWithDeadClobbers { 0 };
    static constexpr unsigned mostMissesInARow = 64;
    static constexpr size_t fewClobberPoints = 4096;
    Vector<unsigned> m_firstDeadClobberOfReg;
    Vector<unsigned> m_deadClobbersOfRegs;
    Vector<unsigned> m_representatives;
    Vector<AroundClobbersSplitMetadata> m_aroundClobbersMetadata;
#if USE(BUN_JSC_ADDITIONS)
    struct MoveRoundClobber {
        unsigned blockIndex;
        unsigned index;
        bool isBehind;
        Reg source;
        Reg destination;
    };
    Vector<MoveRoundClobber> m_movesRoundClobbers;
#endif
    Vector<IntraBlockSplitMetadata> m_intraBlockMetadata;
    Vector<AroundLoopSplitMetadata> m_aroundLoopMetadata;
    Vector<StackSlot*> m_spillSlotTable;
    IndexMap<Reg, RegisterRange> m_regRanges;
#if USE(BUN_JSC_ADDITIONS)
    std::array<uint32_t, Reg::maxIndex() + 1> m_epochOfLastRemoval { };
    uint32_t m_removalEpoch { 1 };
#endif
    Vector<Point> m_clobberPoints;
    Vector<uint64_t> m_clobberedRegsAtPoint;
    uint64_t m_allClobberedRegs { 0 };
    GenerationalSet<uint8_t, SaVector> m_visited;
    PriorityQueue<TmpPriority> m_queue;
    Vector<TmpPriority> m_queueAtStart;
    size_t m_nextInQueueAtStart { 0 };
    bool m_isFillingQueueAtStart { false };
#if USE(BUN_JSC_ADDITIONS)
    Vector<PhaseInsertion> m_insertions;
    Vector<unsigned> m_blockOfInsertion;
#else
    IndexMap<BasicBlock*, PhaseInsertionSet> m_insertionSets;
#endif
    BlockWorklist m_fastBlocks;
    CountsOfTmps m_useCounts;
    WidthsOfTmps m_tmpWidth;
    std::array<AirAllocateRegistersStats, numBanks> m_stats = { { { GP, m_code.proc().name() }, { FP, m_code.proc().name() } } };
    std::array<bool, numBanks> m_didSpill { };
    bool m_needsEmitSpillCode { false };
    bool m_hasUseDefLists { false };
    std::unique_ptr<Dominators> m_dominators;
    std::unique_ptr<NaturalLoops> m_naturalLoops;
    Vector<LoopData> m_loopData;
};

} // namespace JSC::B3::Air::Greedy

#if USE(BUN_JSC_ADDITIONS)
static void moveBlocksWithoutSuccessorsToEnd(Code& code)
{
    Vector<BasicBlock*> order;
    order.reserveInitialCapacity(code.size());
    for (BasicBlock* block : code) {
        if (block->numSuccessors() || block == code[0])
            order.append(block);
    }
    for (BasicBlock* block : code) {
        if (!block->numSuccessors() && block != code[0])
            order.append(block);
    }
    for (auto& entry : code.blockList())
        entry.release();
    code.blockList().shrink(0);
    for (unsigned i = 0; i < order.size(); ++i) {
        order[i]->setIndex(i);
        code.blockList().append(std::unique_ptr<BasicBlock>(order[i]));
    }
}
#endif

void allocateRegistersByGreedy(Code& code)
{
    PhaseScope phaseScope(code, "allocateRegistersByGreedy"_s);
#if USE(BUN_JSC_ADDITIONS)
    constexpr unsigned manyBlocks = 16384;
    if (code.size() >= manyBlocks)
        moveBlocksWithoutSuccessorsToEnd(code);
#endif
    if (Options::airGreedyRegAllocSplitAroundLoops())
        ensureDedicatedLoopEntryExitBlocks(code);
    {
        Greedy::GreedyAllocator allocator(code, Greedy::GreedyAllocator::DeadCodeSearch::Yes);
        if (!allocator.hasDeadCode()) [[likely]] {
            allocator.run();
            return;
        }
    }
    eliminateDeadCode(code);
    Greedy::GreedyAllocator allocator(code, Greedy::GreedyAllocator::DeadCodeSearch::No);
    allocator.run();
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
