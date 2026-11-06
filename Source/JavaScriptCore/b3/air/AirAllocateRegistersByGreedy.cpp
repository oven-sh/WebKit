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
#include "AirBitMatrixLiveness.h"
#include "AirBlockInsertionSet.h"
#include "AirCFG.h"
#include "AirCheckSpillSlots.h"
#include "AirCode.h"
#include "AirDominators.h"
#include "AirEmitShuffle.h"
#include "AirEnsureDedicatedLoopEntryExitBlocks.h"
#include "AirExtendedForms.h"
#include "AirFixSpillsAfterTerminals.h"
#include "AirFormTable.h"
#include "AirFormTraits.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLimits.h"
#include "AirLiveness.h"
#include "AirNaturalLoops.h"
#include "AirPhaseInsertionSet.h"
#include "AirPhaseScope.h"
#include "AirRegLiveness.h"
#include "AirRegisterAllocatorStats.h"
#include "AirTmpBoundaryRows.h"
#include "AirTmpMap.h"
#include "AirTmpWidthInlines.h"
#include "AirUseCounts.h"
#include "B3Procedure.h"
#include <wtf/CycleForest.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/GenerationalSet.h>
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
        size_t size = std::max(count + count / 2, m_nextChunkSize);
        m_nextChunkSize = size * 2;
        m_next = static_cast<Interval*>(fastMalloc(size * sizeof(Interval)));
        m_end = m_next + size;
        m_chunks.append(m_next);
    }

    Vector<Interval*, 4> m_chunks;
    Interval* m_next { nullptr };
    Interval* m_end { nullptr };
    size_t m_nextChunkSize { 64 };
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

    bool LEGACY_NODELETE hasConflict(const LiveRange& range, Width width)
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
    // The result of `a && b && ..` is defined by one move per operand. A list that long is not walked for each move or def.
    static constexpr size_t maxSizeWalked = 16;

    // mergeDuplicates() follows the last call.
    void add(Tmp tmp, float moveCost)
    {
        ASSERT(!m_isSorted);
        ASSERT(m_entries.isEmpty() || m_entries.last().tmp.bank() == tmp.bank());
        if (m_entries.size() <= maxSizeWalked) {
            for (auto& entry : m_entries) {
                if (entry.tmp == tmp) {
                    entry.moveCost += moveCost;
                    return;
                }
            }
        }
        m_entries.append({ tmp, moveCost });
    }

    // The costs of one Tmp are added in the order of the calls of add().
    void mergeDuplicates()
    {
        if (m_entries.size() <= maxSizeWalked)
            return;
        std::ranges::stable_sort(m_entries, { }, [](const Entry& entry) { return entry.tmp.internalValue(); });
        size_t size = 0;
        for (size_t index = 0; index < m_entries.size(); ++index) {
            Entry entry = m_entries[index];
            if (size && m_entries[size - 1].tmp == entry.tmp)
                m_entries[size - 1].moveCost += entry.moveCost;
            else
                m_entries[size++] = entry;
        }
        m_entries.shrink(size);
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
        weight = std::max(weight - spillCostDiscount, weight / 8);
        SpillCost cost = SpillCost(weight / (liveRange.size() + spillCostSizeBias));
        return std::min(cost, maxSpillableSpillCost);
    }

    void LEGACY_NODELETE validate()
    {
        ASSERT(!!assigned == (stage == Stage::Assigned));
        ASSERT(liveRange.intervals().isEmpty() == !liveRange.size());
        ASSERT_IMPLIES(stage == Stage::Spilled, spillCost() != unspillableCost);
    }

    void setLiveRange(LiveRange&& range)
    {
        liveRange = WTF::move(range);
        allocationFailedAtEpoch = 0;
        evictionFailedAtEpoch = 0;
        hasClobberedRegs = false;
    }

    void setPaysForCalleeSave()
    {
        paysForCalleeSave = true;
        allocationFailedAtEpoch = 0;
        evictionFailedAtEpoch = 0;
    }

    LiveRange liveRange;
    Coalescables coalescables;
    UseDefCost useDefCost { 0 };
    UseDefCost cumulativeFixupCost { 0 };
    uint32_t spillSlotTableIndex { 0 };
    uint32_t allocationFailedAtEpoch { 0 };
    uint32_t evictionFailedAtEpoch { 0 };
    uint64_t clobberedRegs { 0 };
    bool hasClobberedRegs { false };
    uint32_t splitAroundClobbersMetadataIndex : 31 { 0 };
    uint32_t hasColdUse : 1 { 0 };
    bool paysForCalleeSave { false };
    bool spansStretch { false };
    bool isClusterOverBlocks { false };
    bool isOnlyReadAtSomeClobber { false };
    bool isOnStackInSomeBlocks { false };
    Stage stage { Stage::New };
    Spillability spillability { Spillability::Spillable };
    Reg preferredReg;
    Reg assigned;
    Tmp splitFrom;
    Reg secondPreferredReg;
    float preferredWeight { 0 };
    float secondPreferredWeight { 0 };
    float spillCostDiscount { 0 };

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

    void forgetWeightlessWishFor(Reg reg)
    {
        if (reg == secondPreferredReg && !secondPreferredWeight)
            secondPreferredReg = Reg();
        if (reg != preferredReg || preferredWeight)
            return;
        preferredReg = std::exchange(secondPreferredReg, Reg());
        preferredWeight = std::exchange(secondPreferredWeight, 0);
    }

    Tmp neighbours[2];
    // How often the Shuffles run that hand this half of a cut live range over. Zero for any other Tmp.
    float frequencyOfCuts { 0 };

    void addNeighbor(Tmp tmp)
    {
        if (neighbours[0] == tmp || neighbours[1] == tmp)
            return;
        if (!neighbours[0])
            neighbours[0] = tmp;
        else if (!neighbours[1])
            neighbours[1] = tmp;
    }
};

class RegisterOccupancy {
    WTF_MAKE_NONCOPYABLE(RegisterOccupancy);
public:
    enum class FailureMemo : uint8_t { Allocation, Eviction, None };
    enum class Clobbers : bool { Exclude, Ignore };

    class Candidates {
    public:
        ALWAYS_INLINE bool contains(Reg reg) const
        {
            if (m_excludedRegs >> reg.index() & 1)
                return false;
            return !(m_failedAtEpoch && m_occupancy.m_lastRemovalEpoch[reg.index()] <= m_failedAtEpoch);
        }

    private:
        friend class RegisterOccupancy;

        Candidates(const RegisterOccupancy& occupancy, uint64_t excludedRegs, uint32_t failedAtEpoch)
            : m_occupancy(occupancy)
            , m_excludedRegs(excludedRegs)
            , m_failedAtEpoch(failedAtEpoch)
        {
        }

        const RegisterOccupancy& m_occupancy;
        uint64_t m_excludedRegs;
        uint32_t m_failedAtEpoch;
    };

    class ClobberWalk {
    public:
        explicit ClobberWalk(RegisterOccupancy& occupancy)
            : m_occupancy(occupancy)
            , m_points(occupancy.m_clobberPoints.span().data())
            , m_clobberedRegsAtPoint(occupancy.m_clobberedRegsAtPoint.span().data())
            , m_numberOfPoints(occupancy.m_clobberPoints.size())
            , m_next(m_numberOfPoints)
        {
            occupancy.m_deadClobbersAtPoint.fill(0, m_numberOfPoints);
            m_deadClobbersAtPoint = occupancy.m_deadClobbersAtPoint.mutableSpan().data();
            updateNextPoint();
        }

        ALWAYS_INLINE Point nextPoint() const { return m_nextPoint; }
        ALWAYS_INLINE uint64_t regsAtNextPoint() const { return m_clobberedRegsAtPoint[m_next - 1]; }

        ALWAYS_INLINE void finishNextPoint(uint64_t deadClobbers)
        {
            m_deadClobbersAtPoint[--m_next] = deadClobbers;
            m_occupancy.m_regsWithDeadClobbers |= deadClobbers;
            updateNextPoint();
        }

        ALWAYS_INLINE bool lastPointHasDeadClobber(uint64_t bit, Point point) const
        {
            return m_next < m_numberOfPoints && m_points[m_next] == point && (m_deadClobbersAtPoint[m_next] & bit);
        }

        ALWAYS_INLINE bool takeDeadClobberAtLastPoint(uint64_t bit, Point point)
        {
            if (!lastPointHasDeadClobber(bit, point))
                return false;
            m_deadClobbersAtPoint[m_next] &= ~bit;
            return true;
        }

        bool isDone() const { return !m_next; }

    private:
        ALWAYS_INLINE void updateNextPoint()
        {
            m_nextPoint = m_next ? m_points[m_next - 1] : std::numeric_limits<Point>::max();
        }

        RegisterOccupancy& m_occupancy;
        const Point* m_points;
        const uint64_t* m_clobberedRegsAtPoint;
        uint64_t* m_deadClobbersAtPoint { nullptr };
        const size_t m_numberOfPoints;
        size_t m_next;
        Point m_nextPoint { 0 };
    };

    RegisterOccupancy()
        : m_regRanges(Reg::maxIndex() + 1)
    {
    }

    void addFreshCalleeSave(Reg reg)
    {
        m_freshCalleeSaves |= bitOf(reg);
        ++m_numberOfFreshCalleeSaves[reg.isGPR() ? GP : FP];
        ++m_numberOfCalleeSaves[reg.isGPR() ? GP : FP];
    }

    void setCalleeSavesClobberedAtSuspensions(uint64_t regs) { m_calleeSavesClobberedAtSuspensions = regs; }

    NEVER_INLINE void recordClobbers(unsigned ordinal, uint64_t early, uint64_t earlyHigh64, uint64_t late, uint64_t lateHigh64)
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

    void buildClobberIndices(unsigned numberOfInsts)
    {
        if (!m_allClobberedRegs)
            return;
        m_instFirstClobber.grow(numberOfInsts);
        unsigned* instFirstClobber = m_instFirstClobber.mutableSpan().data();
        const Point* points = m_clobberPoints.span().data();
        const unsigned numberOfPoints = m_clobberPoints.size();
        unsigned next = 0;
        for (unsigned ordinal = 0; ordinal < numberOfInsts; ++ordinal) {
            while (next < numberOfPoints && points[next] < ordinal * PointOffsets::PointsPerInst)
                ++next;
            instFirstClobber[ordinal] = next;
        }
        if (numberOfPoints >= AirLimits::minClobberPointsForChunks) {
            m_clobberedRegsInChunk.fill(0, (numberOfPoints + AirLimits::clobberPointsPerChunk - 1) / AirLimits::clobberPointsPerChunk);
            for (unsigned index = 0; index < numberOfPoints; ++index)
                m_clobberedRegsInChunk[index / AirLimits::clobberPointsPerChunk] |= m_clobberedRegsAtPoint[index];
        }
    }

    size_t numberOfClobberPoints() const { return m_clobberPoints.size(); }
    uint64_t allClobberedRegs() const { return m_allClobberedRegs; }

    ALWAYS_INLINE Candidates candidatesFor(TmpData& tmpData, FailureMemo memo, Clobbers clobbers)
    {
        uint64_t excludedRegs = tmpData.paysForCalleeSave ? 0 : m_freshCalleeSaves;
        if (clobbers == Clobbers::Exclude)
            excludedRegs |= clobberedRegs(tmpData);
        return Candidates(*this, excludedRegs, failedAtEpoch(tmpData, memo));
    }

    void rememberFailure(TmpData& tmpData, FailureMemo memo) const
    {
        switch (memo) {
        case FailureMemo::Allocation:
            tmpData.allocationFailedAtEpoch = m_removalEpoch;
            return;
        case FailureMemo::Eviction:
            tmpData.evictionFailedAtEpoch = m_removalEpoch;
            return;
        case FailureMemo::None:
            return;
        }
    }

    ALWAYS_INLINE bool hasConflict(Reg reg, const TmpData& tmpData, Width width)
    {
        return m_regRanges[reg].hasConflict(tmpData.liveRange, width);
    }

    // Sees no dead clobber, so reg has to come from Candidates that exclude clobbered registers.
    ALWAYS_INLINE void forEachTmpConflict(Reg reg, const LiveRange& range, Width width, NOESCAPE const Invocable<IterationStatus(RegisterRange::AllocatedInterval&)> auto& func)
    {
        m_regRanges[reg].forEachConflict(range, width, func);
    }

    void forEachConflict(Reg reg, const LiveRange& range, Width width, NOESCAPE const Invocable<IterationStatus(RegisterRange::AllocatedInterval&)> auto& func)
    {
        if (m_clobberPoints.size() > AirLimits::minClobberPointsToLeap) [[unlikely]]
            forEachConflictImpl<true>(reg, range, width, func);
        else
            forEachConflictImpl<false>(reg, range, width, func);
    }

    uint64_t clobberedRegs(TmpData& tmpData)
    {
        if (tmpData.hasClobberedRegs)
            return tmpData.clobberedRegs;
        uint64_t result = 0;
        const Point* points = m_clobberPoints.span().data();
        const size_t numberOfPoints = m_clobberPoints.size();
        for (auto& interval : tmpData.liveRange.intervals()) {
            if (result == m_allClobberedRegs)
                break;
            size_t index = m_instFirstClobber[interval.begin() / PointOffsets::PointsPerInst];
            while (index < numberOfPoints && points[index] < interval.begin())
                ++index;
            for (; index < numberOfPoints && points[index] < interval.end() && result != m_allClobberedRegs; ++index) {
                if (!(index % AirLimits::clobberPointsPerChunk) && !m_clobberedRegsInChunk.isEmpty()) [[unlikely]] {
                    while (index + AirLimits::clobberPointsPerChunk <= numberOfPoints && points[index + AirLimits::clobberPointsPerChunk - 1] < interval.end()) {
                        result |= m_clobberedRegsInChunk[index / AirLimits::clobberPointsPerChunk];
                        index += AirLimits::clobberPointsPerChunk;
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

    // How many points of the range clobber the register while nothing is live in it. Goes by the intervals of the range, not by the points, and stops at `enough`.
    size_t numberOfDeadClobbers(Reg reg, const LiveRange& range, size_t enough)
    {
        if (!(m_regsWithDeadClobbers & bitOf(reg)))
            return 0;
        std::span<const unsigned> own = deadClobbersOf(reg);
        size_t count = 0;
        for (auto& interval : range.intervals()) {
            auto first = std::ranges::lower_bound(own, firstClobberNotBefore(interval.begin()));
            count += std::ranges::lower_bound(first, own.end(), firstClobberNotBefore(interval.end())) - first;
            if (count >= enough)
                break;
        }
        return count;
    }

    bool hasPointClobberingAll(uint64_t regs, Interval interval) const
    {
        auto clobberPoints = m_clobberPoints.span();
        size_t index = std::ranges::lower_bound(clobberPoints, interval.begin()) - clobberPoints.begin();
        for (; index < clobberPoints.size() && clobberPoints[index] < interval.end(); ++index) {
            if ((m_clobberedRegsAtPoint[index] & regs) == regs)
                return true;
        }
        return false;
    }

    ALWAYS_INLINE void add(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        const uint64_t bit = bitOf(reg);
        if ((m_freshCalleeSaves & bit) && (!tmp.isReg() || tmpData.liveRange.size() || (m_regsWithDeadClobbers & ~m_calleeSavesClobberedAtSuspensions & bit))) {
            m_freshCalleeSaves &= ~bit;
            m_lastRemovalEpoch[reg.index()] = ++m_removalEpoch;
            --m_numberOfFreshCalleeSaves[reg.isGPR() ? GP : FP];
        }
        m_regRanges[reg].add(tmp, tmpData.liveRange);
    }

    ALWAYS_INLINE void remove(TmpData& tmpData, Reg reg)
    {
        m_regRanges[reg].evict(tmpData.liveRange);
        m_lastRemovalEpoch[reg.index()] = ++m_removalEpoch;
    }

    unsigned numberOfFreshCalleeSaves(Bank bank) const { return m_numberOfFreshCalleeSaves[bank]; }
    unsigned numberOfCalleeSaves(Bank bank) const { return m_numberOfCalleeSaves[bank]; }
    const RegisterRange& rangeOf(Reg reg) const { return m_regRanges[reg]; }

private:
    static_assert(Reg::maxIndex() < 64);

    static uint64_t bitOf(Reg reg) { return static_cast<uint64_t>(1) << reg.index(); }

    static uint32_t failedAtEpoch(const TmpData& tmpData, FailureMemo memo)
    {
        switch (memo) {
        case FailureMemo::Allocation:
            return tmpData.allocationFailedAtEpoch;
        case FailureMemo::Eviction:
            return tmpData.evictionFailedAtEpoch;
        case FailureMemo::None:
            return 0;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    unsigned firstClobberNotBefore(Point point) const
    {
        const unsigned numberOfPoints = m_clobberPoints.size();
        size_t ordinal = point / PointOffsets::PointsPerInst;
        if (ordinal >= m_instFirstClobber.size())
            return numberOfPoints;
        unsigned index = m_instFirstClobber[ordinal];
        while (index < numberOfPoints && m_clobberPoints[index] < point)
            ++index;
        return index;
    }

    NEVER_INLINE std::span<const unsigned> deadClobbersOf(Reg reg)
    {
        if (m_regFirstDeadClobber.isEmpty()) {
            m_regFirstDeadClobber.fill(0, Reg::maxIndex() + 2);
            for (uint64_t regs : m_deadClobbersAtPoint) {
                for (; regs; regs &= regs - 1)
                    ++m_regFirstDeadClobber[std::countr_zero(regs) + 1];
            }
            for (unsigned i = 0; i <= Reg::maxIndex(); ++i)
                m_regFirstDeadClobber[i + 1] += m_regFirstDeadClobber[i];
            m_regDeadClobbers.grow(m_regFirstDeadClobber.last());
            Vector<unsigned> next = m_regFirstDeadClobber;
            for (unsigned clobber = 0; clobber < m_deadClobbersAtPoint.size(); ++clobber) {
                for (uint64_t regs = m_deadClobbersAtPoint[clobber]; regs; regs &= regs - 1)
                    m_regDeadClobbers[next[std::countr_zero(regs)]++] = clobber;
            }
        }
        return m_regDeadClobbers.span().subspan(m_regFirstDeadClobber[reg.index()], m_regFirstDeadClobber[reg.index() + 1] - m_regFirstDeadClobber[reg.index()]);
    }

    size_t nextDeadClobberFor(Reg reg, size_t from)
    {
        auto own = deadClobbersOf(reg);
        auto iterator = std::ranges::lower_bound(own, static_cast<unsigned>(from));
        return iterator == own.end() ? m_deadClobbersAtPoint.size() : *iterator;
    }

    template<bool leaps>
    void forEachConflictImpl(Reg reg, const LiveRange& range, Width width, NOESCAPE const Invocable<IterationStatus(RegisterRange::AllocatedInterval&)> auto& func)
    {
        RegisterRange& regRange = m_regRanges[reg];
        const uint64_t bit = bitOf(reg);
        if (!(m_regsWithDeadClobbers & bit)) {
            regRange.forEachConflict(range, width, func);
            return;
        }
        const Point* points = m_clobberPoints.span().data();
        const uint64_t* deadClobbersAtPoint = m_deadClobbersAtPoint.span().data();
        const size_t numberOfPoints = m_clobberPoints.size();
        for (auto interval : range.intervals()) {
            size_t clobber = m_instFirstClobber[interval.begin() / PointOffsets::PointsPerInst];
            while (clobber < numberOfPoints && points[clobber] < interval.begin())
                ++clobber;
            while (true) {
                auto conflict = regRange.firstConflict(interval);
                Point limit = conflict ? conflict->interval.begin() : interval.end();
                [[maybe_unused]] unsigned consecutiveMisses = 0;
                for (; clobber < numberOfPoints && points[clobber] < limit; ++clobber) {
                    if (!(deadClobbersAtPoint[clobber] & bit)) {
                        if constexpr (leaps) {
                            if (++consecutiveMisses > AirLimits::maxConsecutiveClobberMisses) [[unlikely]] {
                                clobber = nextDeadClobberFor(reg, clobber) - 1;
                                consecutiveMisses = 0;
                            }
                        }
                        continue;
                    }
                    consecutiveMisses = 0;
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

    IndexMap<Reg, RegisterRange> m_regRanges;
    std::array<uint32_t, Reg::maxIndex() + 1> m_lastRemovalEpoch { };
    uint32_t m_removalEpoch { 1 };
    Vector<Point> m_clobberPoints;
    Vector<uint64_t> m_clobberedRegsAtPoint;
    uint64_t m_allClobberedRegs { 0 };
    Vector<unsigned> m_instFirstClobber;
    Vector<uint64_t> m_clobberedRegsInChunk;
    Vector<uint64_t> m_deadClobbersAtPoint;
    uint64_t m_regsWithDeadClobbers { 0 };
    Vector<unsigned> m_regFirstDeadClobber;
    Vector<unsigned> m_regDeadClobbers;
    uint64_t m_freshCalleeSaves { 0 };
    uint64_t m_calleeSavesClobberedAtSuspensions { 0 };
    std::array<unsigned, numBanks> m_numberOfFreshCalleeSaves { };
    std::array<unsigned, numBanks> m_numberOfCalleeSaves { };
};

struct LoopData {
    LiveRange range;
    LiveRange boundary;
    bool hasExitWithEntryClobbers { false };
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

static constexpr unsigned maxArgsWalkedToFindTmp = 64;

struct MentionInLargeInst {
    MentionInLargeInst() = default;

    MentionInLargeInst(Inst& inst, Tmp& tmp, Arg::Role role)
        : isKnown(true)
        , isFollowedByOthers(false)
        , roleBits(role)
        , offsetInArgs(reinterpret_cast<uintptr_t>(&tmp) - reinterpret_cast<uintptr_t>(inst.args().data()))
    {
        RELEASE_ASSERT(tmpIn(inst) == &tmp);
    }

    Tmp* tmpIn(Inst& inst) const
    {
        uintptr_t address = reinterpret_cast<uintptr_t>(inst.args().data()) + offsetInArgs;
        Tmp* result = nullptr;
        inst.args()[offsetInArgs / sizeof(Arg)].forEachTmpFast([&](Tmp& tmp) {
            if (reinterpret_cast<uintptr_t>(&tmp) == address)
                result = &tmp;
        });
        return result;
    }

    Arg::Role role() const { return static_cast<Arg::Role>(roleBits); }
    bool isOnlyColdUse() const { return isKnown && !isFollowedByOthers && Arg::isColdUse(role()); }

    unsigned isKnown : 1 { 0 };
    unsigned isFollowedByOthers : 1 { 0 };
    unsigned roleBits : 4 { 0 };
    unsigned offsetInArgs : 26 { 0 };
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

static void validateFormTraits()
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
                RELEASE_ASSERT(formTraits[form] == Mentions::traitsOf(role, bank, width));
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

struct RangeState {
    Point activeEnd;
    unsigned lowestPiece;
};

class TmpWidths {
public:
    struct Widths {
        Width use;
        Width def;
    };

    Widths* prepare(Code& code)
    {
        unsigned gpEnd = Tmp::absoluteIndexEnd(code, GP);
        unsigned fpEnd = Tmp::absoluteIndexEnd(code, FP);
        m_gp.grow(gpEnd + fpEnd);
        Widths* widths = m_gp.mutableSpan().data();
        std::fill(widths, widths + gpEnd, Widths { minimumWidth(GP), minimumWidth(GP) });
        std::fill(widths + gpEnd, widths + gpEnd + fpEnd, Widths { minimumWidth(FP), minimumWidth(FP) });
        return widths;
    }

    void finish(unsigned gpEnd)
    {
        m_fp.append(m_gp.span().subspan(gpEnd));
        m_gp.shrink(gpEnd);
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

class TmpCounts {
public:
    static constexpr uint8_t hasConstDef = 1;
    static constexpr uint8_t hasOtherDef = 2;

    void prepare(Code& code)
    {
        m_absoluteIndexEnds[GP] = Tmp::absoluteIndexEnd(code, GP);
        m_absoluteIndexEnds[FP] = Tmp::absoluteIndexEnd(code, FP);
        counts.fill(0, m_absoluteIndexEnds[GP] + m_absoluteIndexEnds[FP]);
        defKinds.fill(0, m_absoluteIndexEnds[GP] + m_absoluteIndexEnds[FP]);
        gpConstants.grow(m_absoluteIndexEnds[GP]);
        fpConstants.grow(m_absoluteIndexEnds[FP]);
        fpConstantWidths.grow(m_absoluteIndexEnds[FP]);
    }

    template<Bank bank>
    bool isConstDef(unsigned absoluteIndex) const
    {
        if (absoluteIndex >= m_absoluteIndexEnds[bank])
            return false;
        return defKinds[linearIndex<bank>(absoluteIndex)] == hasConstDef;
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
    Vector<uint8_t> defKinds;
    Vector<int64_t> gpConstants;
    Vector<v128_t> fpConstants;
    Vector<Width> fpConstantWidths;

private:
    template<Bank bank>
    unsigned linearIndex(unsigned absoluteIndex) const
    {
        return bank == GP ? absoluteIndex : m_absoluteIndexEnds[GP] + absoluteIndex;
    }

    std::array<unsigned, numBanks> m_absoluteIndexEnds { };
};

class TmpDataMap {
public:
    explicit TmpDataMap(Code& code)
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
    GreedyAllocator(Code& code)
        : m_code(code)
        , m_registerPolicy(code.policy().registers)
        , m_splitPolicy(code.policy().splitting)
        , m_usesExtendedForms(code.proc().policy().selection.extendsInstructionSelection)
        , m_discountsPreferredRegisterMoves(isX86_64() && m_registerPolicy.discountsPreferredRegisterMoves)
        , m_blockToHeadPoint(code.size())
        , m_tailPoints(code.size())
        , m_map(code)
        , m_aroundClobbersMetadata(1) // Sacrifice index 0.
        , m_spillSlotTable(FillWith { }, 1, nullptr) // Sacrifice index 0.
    {
        decode();

        if (Options::airValidateGreedRegAlloc()) [[unlikely]] {
            validateFormTraits();
            validateFusedAnalyses();
            for (BasicBlock* block : m_code) {
                for (Inst& inst : *block)
                    validateFastTmpEnumeration(inst);
            }
        }
    }

    void decode()
    {
        m_gpIndicesEnd = Tmp::absoluteIndexEnd(m_code, GP);
        m_numberOfLinearIndices = Tmp::linearIndexEnd(m_code);
        m_useCounts.prepare(m_code);
        m_blockEnds.grow(m_code.size());

        m_fastBlocks.push(m_code[0]);
        while (BasicBlock* block = m_fastBlocks.pop()) {
            for (FrequentedBlock& successor : block->successors()) {
                if (!successor.isRare())
                    m_fastBlocks.push(successor.block());
            }
        }

        Vector<unsigned> movesForWidths;
        Vector<PaddingSite> paddingSites;
        recordMentions(movesForWidths, paddingSites);
        findWidths(movesForWidths);
        countUsesAndDefs();
        findBlockExposedUsesAndDefs();

        InsertionSet insertionSet(m_code);
        for (size_t i = 0; i < paddingSites.size();) {
            BasicBlock* block = m_code[paddingSites[i].blockIndex];
            for (; i < paddingSites.size() && paddingSites[i].blockIndex == block->index(); ++i)
                insertionSet.insert(paddingSites[i].instIndex, Padding, block->at(paddingSites[i].instIndex).origin);
            insertionSet.execute(block);
        }
    }

    struct PaddingSite {
        unsigned blockIndex;
        unsigned instIndex;
    };

    void recordMentions(Vector<unsigned>& movesForWidths, Vector<PaddingSite>& paddingSites)
    {
        const unsigned gpIndicesEnd = m_gpIndicesEnd;

        unsigned precedingInstCount = 0;
        for (BasicBlock* block : m_code)
            precedingInstCount += block->size();
        m_mentions.instFirstMention.grow(static_cast<size_t>(precedingInstCount) * 2 + 1);
        m_mentions.instTimings.grow(static_cast<size_t>(precedingInstCount) * 2 + 1);
        unsigned* instFirstMention = m_mentions.instFirstMention.mutableSpan().data();
        uint8_t* allInstTimings = m_mentions.instTimings.mutableSpan().data();
        unsigned ordinal = 0;

        size_t capacity = static_cast<size_t>(precedingInstCount) * 2 + 64;
        m_mentions.tmps.grow(capacity);
        m_mentions.traits.grow(capacity);
        unsigned* tmps = m_mentions.tmps.mutableSpan().data();
        uint16_t* mentionTraits = m_mentions.traits.mutableSpan().data();
        unsigned numberOfMentions = 0;

        movesForWidths.grow(precedingInstCount);
        unsigned* nextMoveForWidths = movesForWidths.mutableSpan().data();
        Vector<unsigned> wholeCopiesOfShuffles;
        m_tmpMoves.grow(precedingInstCount);
        unsigned* nextTmpMove = m_tmpMoves.mutableSpan().data();

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
        const bool recordsNeighbors = isX86() && m_registerPolicy.hintsTwoAddressOperands;
        constexpr unsigned baseTraits = Mentions::traitsOf(Arg::Use, GP, pointerWidth());
        constexpr unsigned kindsWithBase = 1u << Arg::SimpleAddr | 1u << Arg::Addr | 1u << Arg::ExtendedOffsetAddr | 1u << Arg::Index | 1u << Arg::PreIndex | 1u << Arg::PostIndex;

        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            BasicBlock* block = m_code[blockIndex];
            m_blockEnds[blockIndex].firstInst = ordinal;
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
                    mentionTraits = m_mentions.traits.mutableSpan().data();
                }

                unsigned allMentionTraits = 0;
                bool isTmpMove = false;
                bool namesNeighbors = false;
                switch (opcode) {
                case Move:
                case Move32:
                case MoveFloat:
                case MoveDouble:
                case MoveVector:
                    if (!args[1].isTmp())
                        break;
                    if (args[0].isTmp()) {
                        isTmpMove = numberOfArgs == 2;
                        if (opcode == Move) {
                            *nextMoveForWidths++ = numberOfMentions;
                            allMentionTraits = Mentions::hasNoWidthInformation;
                        }
                        break;
                    }
                    if (args[0].isSomeImm())
                        allMentionTraits = recordConstantMove(opcode, args[0], args[1].tmp());
                    break;
                case MoveConditionally32:
                case MoveConditionally64:
                case MoveConditionallyTest32:
                case MoveConditionallyTest64:
                case MoveConditionallyDouble:
                case MoveConditionallyFloat:
                    namesNeighbors = recordsNeighbors && numberOfArgs == 6;
                    break;
                case Shuffle:
                    namesNeighbors = true;
                    break;
                default:
                    break;
                }

                unsigned argTraits = 0;
                unsigned instTraits = 0;
                unsigned wasDecodedSlowly = 0;
                const unsigned firstMention = numberOfMentions;
                auto note = [&](Tmp tmp, unsigned traits) ALWAYS_INLINE_LAMBDA {
                    unsigned index = tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : gpIndicesEnd + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
                    tmps[numberOfMentions] = index;
                    mentionTraits[numberOfMentions] = traits | allMentionTraits;
                    ++numberOfMentions;
                    instTraits |= traits;
                };
                unsigned argIndex = 0;
                if (const uint8_t* forms = inst.fixedForms(argIndex)) [[likely]] {
                    for (; argIndex < numberOfArgs; ++argIndex) {
                        Arg& arg = args[argIndex];
                        unsigned traits = formTraits[forms[argIndex]];
                        argTraits |= traits;
                        unsigned kind = arg.kind();
                        if (kind == Arg::Tmp) {
                            note(arg.tmp(), traits);
                            continue;
                        }
                        if (!(kindsWithBase >> kind & 1))
                            continue;
                        unsigned tmpTraits = baseTraits;
                        if (((forms[argIndex] >> formRoleShift) & formRoleMask) == Arg::UseAddr) [[unlikely]]
                            tmpTraits = Mentions::traitsOf(Arg::Use) | (traits & (Mentions::widthMask << Mentions::widthShift));
                        if (kind == Arg::PreIndex || kind == Arg::PostIndex) [[unlikely]]
                            tmpTraits = Mentions::traitsOf(Arg::UseDef) | (tmpTraits & (Mentions::widthMask << Mentions::widthShift));
                        note(arg.base(), tmpTraits);
                        if (kind == Arg::Index)
                            note(arg.index(), tmpTraits);
                    }
                } else {
                    mentionsDecodedSlowly.shrink(0);
                    argTraits = decodeSlowly(inst, mentionsDecodedSlowly);
                    for (auto& mention : mentionsDecodedSlowly)
                        note(mention.tmp, Mentions::traitsOf(mention.role, mention.bank, mention.width));
                    wasDecodedSlowly = Mentions::wasDecodedSlowly;
                }
                if (opcode == Shuffle) [[unlikely]]
                    recordWholeCopiesOfShuffle(inst, firstMention, mentionTraits, wholeCopiesOfShuffles);

                bool hasEarlyDef = argTraits & Mentions::earlyDef;
                bool hasLateUseOrDef = argTraits & (Mentions::lateUse | Mentions::lateDef);
                if (opcode == Patch) [[unlikely]] {
                    early.update(inst.extraEarlyClobberedRegs());
                    late.update(inst.extraClobberedRegs());
                    hasEarlyDef |= early.whole || early.high64;
                    hasLateUseOrDef |= late.whole || late.high64;
                }

                if (previousHasLateUseOrDef && hasEarlyDef) {
                    paddingSites.append(PaddingSite { blockIndex, instIndex });
                    instFirstMention[ordinal] = firstMention;
                    allInstTimings[ordinal] = 0;
                    ++ordinal;
                }
                previousHasLateUseOrDef = hasLateUseOrDef;

                instFirstMention[ordinal] = firstMention;
                allInstTimings[ordinal] = (instTraits & Mentions::timingMask) | wasDecodedSlowly;

                if (isTmpMove)
                    *nextTmpMove++ = ordinal;
                else if (opcode == Patch || namesNeighbors || (instTraits & Mentions::coldUse)) [[unlikely]] {
                    if (namesNeighbors)
                        m_instsThatNameNeighbors.append(unsigned { ordinal });
                    if (instTraits & Mentions::coldUse) {
                        for (unsigned i = firstMention; i < numberOfMentions; ++i) {
                            if (mentionTraits[i] & Mentions::coldUse)
                                m_map[tmpForLinearIndex(tmps[i])].hasColdUse = true;
                        }
                    }
                    if (opcode == Patch) {
                        m_occupancy.recordClobbers(ordinal, early.whole, early.high64, late.whole, late.high64);
                        if (numberOfArgs > maxArgsWalkedToFindTmp) [[unlikely]]
                            m_largeInsts.append(unsigned { ordinal });
                    }
                }
                ++ordinal;
            }
            if (instIndex != blockSize)
                block->insts().shrink(instIndex);
        }
        instFirstMention[ordinal] = numberOfMentions;
        m_mentions.numberOfInsts = ordinal;
        movesForWidths.shrink(nextMoveForWidths - movesForWidths.mutableSpan().data());
        movesForWidths.appendVector(wholeCopiesOfShuffles);
        m_tmpMoves.shrink(nextTmpMove - m_tmpMoves.mutableSpan().data());
    }

    NEVER_INLINE unsigned recordConstantMove(Opcode opcode, const Arg& constant, Tmp tmp)
    {
        unsigned traits = 0;
        unsigned index = linearIndex(tmp);
        if (opcode == Move && constant.isImm() && constant.value() >= 0) {
            m_smallConstantDefs.append({ index, constant.value() });
            traits = Mentions::hasNoWidthInformation;
        }
        bool isGPMove = opcode == Move || opcode == Move32;
        if (isGPMove != tmp.isGP())
            return traits;
        uint8_t& defKinds = m_useCounts.defKinds[index];
        if (defKinds & TmpCounts::hasConstDef) {
            defKinds |= TmpCounts::hasOtherDef;
            return traits | Mentions::isDefByConstant;
        }
        defKinds |= TmpCounts::hasConstDef;
        if (isGPMove)
            m_useCounts.gpConstants[index] = opcode == Move32 ? static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<uint64_t>(constant.value())))) : constant.value();
        else {
            unsigned fpIndex = index - m_gpIndicesEnd;
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

    // The mention of the destination follows that of the source, as in a Move.
    NEVER_INLINE static void recordWholeCopiesOfShuffle(Inst& inst, unsigned mention, uint16_t* mentionTraits, Vector<unsigned>& copies)
    {
        auto numberOfTmpsIn = [](Arg& arg) {
            unsigned result = 0;
            arg.forEachTmpFast([&](Tmp&) {
                ++result;
            });
            return result;
        };
        for (unsigned i = 0; i + 2 < inst.args().size(); i += 3) {
            Arg& source = inst.args()[i];
            Arg& destination = inst.args()[i + 1];
            if (TmpWidth::isCopyOfWholeGeneralRegister(source, destination, inst.args()[i + 2].width())) {
                copies.append(mention);
                mentionTraits[mention + 1] |= Mentions::hasNoWidthInformation;
            }
            mention += numberOfTmpsIn(source) + numberOfTmpsIn(destination);
        }
    }

    void findWidths(Vector<unsigned>& movesForWidths)
    {
        TmpWidths::Widths* widths = m_tmpWidth.prepare(m_code);
        std::array<Width, numBanks> conservativeWidths;
        conservativeWidths[GP] = m_code.usesSIMD() ? conservativeWidth(GP) : conservativeWidthWithoutVectors(GP);
        conservativeWidths[FP] = m_code.usesSIMD() ? conservativeWidth(FP) : conservativeWidthWithoutVectors(FP);
        RegisterSet::allRegisters().forEach([&](Reg reg) {
            Width conservative = conservativeWidths[bankForReg(reg)];
            widths[linearIndex(Tmp(reg))] = { conservative, conservative };
        });

        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* mentionTraits = m_mentions.traits.span().data();
        const unsigned numberOfMentions = m_mentions.instFirstMention[m_mentions.numberOfInsts];
        for (unsigned i = 0; i < numberOfMentions; ++i) {
            unsigned traits = mentionTraits[i];
            if (traits & Mentions::hasNoWidthInformation)
                continue;
            TmpWidths::Widths& tmpWidths = widths[tmps[i]];
            Width width = static_cast<Width>((traits >> Mentions::widthShift) & Mentions::widthMask);
            if (traits & Mentions::anyUse)
                tmpWidths.use = std::max(tmpWidths.use, width);
            if (traits & Mentions::zDef)
                tmpWidths.def = std::max(tmpWidths.def, width);
            else if (traits & Mentions::anyDef)
                tmpWidths.def = conservativeWidths[traits & Mentions::isFP ? FP : GP];
        }
        for (auto& def : m_smallConstantDefs) {
            Width maxWidth = Width64;
            if (def.value <= std::numeric_limits<int8_t>::max())
                maxWidth = Width8;
            else if (def.value <= std::numeric_limits<int16_t>::max())
                maxWidth = Width16;
            else if (def.value <= std::numeric_limits<int32_t>::max())
                maxWidth = Width32;
            widths[def.index].def = std::max(widths[def.index].def, maxWidth);
        }
        m_smallConstantDefs.clear();

        bool changed = true;
        bool goesDown = false;
        unsigned* moves = movesForWidths.mutableSpan().data();
        size_t numberOfMoves = movesForWidths.size();
        const Width widest = conservativeWidths[GP];
        while (changed) {
            changed = false;
            goesDown = !goesDown;
            size_t keptMoveCount = 0;
            for (size_t step = 0; step < numberOfMoves; ++step) {
                unsigned mention = moves[goesDown ? step : numberOfMoves - 1 - step];
                TmpWidths::Widths& srcWidths = widths[tmps[mention]];
                TmpWidths::Widths& dstWidths = widths[tmps[mention + 1]];
                if (dstWidths.def < srcWidths.def) {
                    dstWidths.def = srcWidths.def;
                    changed = true;
                }
                if (srcWidths.use < dstWidths.use) {
                    srcWidths.use = dstWidths.use;
                    changed = true;
                }
                if (goesDown && (dstWidths.def != widest || srcWidths.use != widest))
                    moves[keptMoveCount++] = mention;
            }
            if (goesDown)
                numberOfMoves = keptMoveCount;
        }
        m_tmpWidth.finish(m_gpIndicesEnd);
    }

    void countUsesAndDefs()
    {
        float* counts = m_useCounts.counts.mutableSpan().data();
        uint8_t* defKinds = m_useCounts.defKinds.mutableSpan().data();
        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* mentionTraits = m_mentions.traits.span().data();
        const unsigned* instFirstMention = m_mentions.instFirstMention.span().data();
        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;
            double frequency = block->frequency();
            if (!m_fastBlocks.saw(block))
                frequency *= m_registerPolicy.rareBlockPenalty;
            unsigned end = instFirstMention[blockIndex + 1 < m_code.size() ? m_blockEnds[blockIndex + 1].firstInst : m_mentions.numberOfInsts];
            for (unsigned i = instFirstMention[m_blockEnds[blockIndex].firstInst]; i < end; ++i) {
                unsigned traits = mentionTraits[i];
                if (!(traits & Mentions::warmUseOrDef))
                    continue;
                unsigned index = tmps[i];
                counts[index] += frequency;
                if ((traits & (Mentions::anyDef | Mentions::isDefByConstant)) == Mentions::anyDef)
                    defKinds[index] |= TmpCounts::hasOtherDef;
            }
        }
    }

    void findBlockExposedUsesAndDefs()
    {
        const unsigned* tmps = m_mentions.tmps.span().data();
        const uint16_t* mentionTraits = m_mentions.traits.span().data();
        const unsigned* instFirstMention = m_mentions.instFirstMention.span().data();
        const uint8_t* allInstTimings = m_mentions.instTimings.span().data();
        const unsigned numberOfMentions = instFirstMention[m_mentions.numberOfInsts];

        Vector<unsigned> stampStore(FillWith { }, m_numberOfLinearIndices, 0u);
        unsigned* tmpStamp = stampStore.mutableSpan().data();
        m_crossesBoundary.fill(0, m_numberOfLinearIndices);
        uint8_t* crossesBoundary = m_crossesBoundary.mutableSpan().data();
        m_exposedUses.grow(numberOfMentions);
        m_defsInBlocks.grow(numberOfMentions);
        unsigned* exposedUses = m_exposedUses.mutableSpan().data();
        unsigned* defsInBlocks = m_defsInBlocks.mutableSpan().data();
        unsigned numberOfExposedUses = 0;
        unsigned numberOfDefs = 0;

        for (unsigned blockIndex = 0; blockIndex < m_code.size(); ++blockIndex) {
            const unsigned useStamp = (blockIndex + 1) << 1;
            const unsigned defStamp = useStamp | 1;
            auto noteUse = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
                if ((tmpStamp[index] | 1) == defStamp)
                    return;
                tmpStamp[index] = useStamp;
                crossesBoundary[index] = 1;
                exposedUses[numberOfExposedUses++] = index;
            };
            auto recordDef = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
                if (tmpStamp[index] == defStamp)
                    return;
                tmpStamp[index] = defStamp;
                defsInBlocks[numberOfDefs++] = index;
            };

            unsigned instsEnd = blockIndex + 1 < m_code.size() ? m_blockEnds[blockIndex + 1].firstInst : m_mentions.numberOfInsts;
            for (unsigned ordinal = m_blockEnds[blockIndex].firstInst; ordinal < instsEnd; ++ordinal) {
                unsigned timings = allInstTimings[ordinal];
                unsigned begin = instFirstMention[ordinal];
                unsigned end = instFirstMention[ordinal + 1];
                if (timings & Mentions::earlyDef) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (mentionTraits[i] & Mentions::earlyDef)
                            recordDef(tmps[i]);
                    }
                }
                if (timings & Mentions::earlyUse) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (mentionTraits[i] & Mentions::earlyUse)
                            noteUse(tmps[i]);
                    }
                }
                if (timings & Mentions::lateDef) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (mentionTraits[i] & Mentions::lateDef)
                            recordDef(tmps[i]);
                    }
                }
                if (timings & Mentions::lateUse) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (!(mentionTraits[i] & Mentions::lateUse))
                            continue;
                        if (ordinal + 1 < instsEnd) {
                            noteUse(tmps[i]);
                            continue;
                        }
                        crossesBoundary[tmps[i]] = 1;
                        m_usesAtTails.append(unsigned { tmps[i] });
                    }
                }
            }
            m_blockEnds[blockIndex].exposedUses = numberOfExposedUses;
            m_blockEnds[blockIndex].defs = numberOfDefs;
            m_blockEnds[blockIndex].usesAtTail = m_usesAtTails.size();
        }
    }

    struct MentionDecodedSlowly {
        Tmp tmp;
        Arg::Role role;
        Bank bank;
        Width width;
    };

    NEVER_INLINE static unsigned decodeSlowly(Inst& inst, Vector<MentionDecodedSlowly, 16>& mentions)
    {
        unsigned argTraits = 0;
        inst.forEachArg(
            [&](Arg& arg, Arg::Role argRole, Bank argBank, Width argWidth) {
                argTraits |= Mentions::traitsOf(argRole);
                arg.forEachTmp(argRole, argBank, argWidth,
                    [&](Tmp& tmp, Arg::Role role, Bank bank, Width width) {
                        mentions.append(MentionDecodedSlowly { tmp, role, bank, width });
                    });
            });
        return argTraits;
    }

    unsigned linearIndex(Tmp tmp) const
    {
        if (tmp.isGP())
            return AbsoluteTmpMapper<GP>::absoluteIndex(tmp);
        return m_gpIndicesEnd + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
    }

    Tmp tmpForLinearIndex(unsigned index) const
    {
        if (index < m_gpIndicesEnd)
            return AbsoluteTmpMapper<GP>::tmpFromAbsoluteIndex(index);
        return AbsoluteTmpMapper<FP>::tmpFromAbsoluteIndex(index - m_gpIndicesEnd);
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

        auto isSameCount = [](float count, float referenceCount) {
            return count == referenceCount || (std::isnan(count) && std::isnan(referenceCount));
        };
        for (unsigned i = 0; i < AbsoluteTmpMapper<GP>::absoluteIndex(m_code.numTmps(GP)); ++i) {
            RELEASE_ASSERT(isSameCount(m_useCounts.numWarmUsesAndDefs<GP>(i), referenceUseCounts.numWarmUsesAndDefs<GP>(i)));
            RELEASE_ASSERT(m_useCounts.isConstDef<GP>(i) == referenceUseCounts.isConstDef<GP>(i));
            if (m_useCounts.isConstDef<GP>(i))
                RELEASE_ASSERT(m_useCounts.constant<GP>(i) == referenceUseCounts.constant<GP>(i));
        }
        for (unsigned i = 0; i < AbsoluteTmpMapper<FP>::absoluteIndex(m_code.numTmps(FP)); ++i) {
            RELEASE_ASSERT(isSameCount(m_useCounts.numWarmUsesAndDefs<FP>(i), referenceUseCounts.numWarmUsesAndDefs<FP>(i)));
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
        recordNeighbors();

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " state before allocate registers:\n", *this, "IR:\n", m_code);

        allocateRegisters<GP>();
        if (hasFPTmps)
            allocateRegisters<FP>();

        insertFixupCode();

        validateAssignments<GP>();
        if (hasFPTmps)
            validateAssignments<FP>();

        if (m_code.proc().policy().validation.checksSpillSlots || m_code.proc().policy().validation.checksAllocatedRegisters) [[unlikely]]
            checkPlacesOfValues();

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " about to assign registers:\n", *this, "IR:\n", m_code);

        assignRegisters();
        fixSpillsAfterTerminals(m_code);

        m_stats[GP].didSpill += m_didSpill[GP];
        m_stats[FP].didSpill += m_didSpill[FP];
        m_stats[GP].numTmpsOut = m_code.numTmps(GP);
        m_stats[FP].numTmpsOut = m_code.numTmps(FP);

        dataLogLnIf(verbose() || shouldDumpFunction(), "Greedy register allocator: function ", m_code.proc().name(), " output IR:\n", m_code);
    }

    // All the Tmps that were split from one Tmp share its spill slot, and the slot stands for the value that they carry. An instruction that defines one of them changes that
    // value, unless it only brings it from the slot or from another of them. Until the new value is stored, nothing may read the slot.
    // A register holds the Tmp that was defined into it last, until an instruction clobbers it.
    void checkPlacesOfValues()
    {
        bool checksSlots = m_code.proc().policy().validation.checksSpillSlots;
        bool checksRegisters = m_code.proc().policy().validation.checksAllocatedRegisters;
        // The moves around loops are Shuffles here, which say nothing of what they copy.
        if (m_splitPolicy.splitsAroundLoops) {
            if (checksSlots)
                checkSpillSlotsAreWritten(m_code, PlacesOfValues::Words::OfSlots, "behind register allocation"_s);
            return;
        }
        auto slotOf = [&](Tmp tmp) -> StackSlot* {
            return tmp.isReg() ? nullptr : m_spillSlotTable[m_map[tmp].spillSlotTableIndex];
        };
        // Where gaps count as live (buildLiveRanges()), a Tmp is saved around a call, or on the way into a loop, also in blocks where it is dead. A predecessor of such a block may lie
        // outside the live range, so the move reads a register that is somebody else's by then. Nobody reads what it writes: were the Tmp live there, it would be live in every predecessor.
        auto savesValueThatMayBeDead = [&](Inst& inst) {
            if (!m_countsGapsAsLive || inst.args().size() != 2 || !inst.args()[0].isTmp() || inst.args()[0].isReg())
                return false;
            Tmp tmp = inst.args()[0].tmp();
            uint32_t slotIndex = m_map[tmp].spillSlotTableIndex;
            if (!slotIndex || inst.kind.opcode != moveOpcode(tmp))
                return false;
            Arg& target = inst.args()[1];
            if (target.isStack())
                return target.stackSlot() == m_spillSlotTable[slotIndex];
            return target.isTmp() && !target.isReg() && m_map[target.tmp()].spillSlotTableIndex == slotIndex;
        };
        PlacesOfValues places(m_code, PlacesOfValues::Words::OfSlots);
        for (BasicBlock* block : m_code) {
            places.beginBlock(block);
            for (unsigned instIndex = 0; instIndex < block->size(); ++instIndex) {
                Inst& inst = block->at(instIndex);
                auto addReadsOfRegisters = [&](bool (*isRead)(Arg::Role)) {
                    inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                        if (!tmp.isReg() && isRead(role))
                            places.addRead(assignedReg(tmp), tmp, instIndex);
                    });
                };
                auto addWritesOfRegisters = [&](bool (*isWrite)(Arg::Role), const RegisterSet& clobbered) {
                    clobbered.forEach([&](Reg reg) {
                        places.addClobber(reg, instIndex);
                    });
                    inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                        if (!isWrite(role))
                            return;
                        if (tmp.isReg())
                            places.addClobber(tmp.reg(), instIndex);
                        else
                            places.addWrite(assignedReg(tmp), tmp, instIndex);
                    });
                };
                bool isPatch = inst.kind.opcode == Patch;

                if (checksSlots)
                    places.addReadsOfSlots(inst, instIndex);
                if (checksRegisters) {
                    if (!savesValueThatMayBeDead(inst))
                        addReadsOfRegisters(Arg::isEarlyUse);
                    addWritesOfRegisters(Arg::isEarlyDef, isPatch ? inst.extraEarlyClobberedRegs() : RegisterSet());
                    addReadsOfRegisters(Arg::isLateUse);
                    addWritesOfRegisters(Arg::isLateDef, isPatch ? inst.extraClobberedRegs() : RegisterSet());
                }
                if (!checksSlots)
                    continue;
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (!Arg::isAnyDef(role))
                        return;
                    StackSlot* slot = slotOf(tmp);
                    if (!slot)
                        return;
                    if (inst.args().size() == 2 && inst.args()[1] == Arg(tmp) && inst.kind.opcode == moveOpcode(tmp)) {
                        Arg& source = inst.args()[0];
                        if ((source.isStack() && source.stackSlot() == slot) || (source.isTmp() && slotOf(source.tmp()) == slot))
                            return;
                    }
                    places.addChangeOfValue(slot, instIndex);
                });
                places.addWritesOfSlots(inst, instIndex);
            }
        }
        places.check("behind register allocation"_s);
    }

    bool leavesOut(MoveLeftOutByRegisterAllocator move) const
    {
        return m_code.proc().policy().validation.moveLeftOutByRegisterAllocator == move;
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
            if (!m_occupancy.rangeOf(r).isEmpty())
                out.println("    ", r, ": ", m_occupancy.rangeOf(r));
        }
        m_code.pinnedRegisters().forEachReg([&](Reg r) {
            if (bankForReg(r) == bank)
                out.println("    ", r, ": ", m_occupancy.rangeOf(r));
        });
    }

    bool LEGACY_NODELETE shouldSpillEverything()
    {
        if (!m_registerPolicy.spillsEverything)
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
        if (m_registerPolicy.chargesForCalleeSaves) {
            RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
            calleeSaves.exclude(m_code.unsavedCalleeSaves());
            calleeSaves.merge(m_registerPolicy.savedByCallee);
            m_allAllowedRegisters.forEach([&](Reg reg) {
                if (calleeSaves.contains(reg, IgnoreVectors))
                    m_occupancy.addFreshCalleeSave(reg);
            });
            uint64_t clobberedAtSuspensions = 0;
            m_code.calleeSavesClobberedAtSuspensions().forEach([&](Reg reg) {
                clobberedAtSuspensions |= static_cast<uint64_t>(1) << reg.index();
            });
            m_occupancy.setCalleeSavesClobberedAtSuspensions(clobberedAtSuspensions);
        }
    }

    void buildIndices()
    {
        Point headPosition = 0;
        Point tailPosition = 0;
        m_instBlockIndex.grow(m_mentions.numberOfInsts);
        unsigned* instBlockIndex = m_instBlockIndex.mutableSpan().data();
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
                *instBlockIndex++ = i;
        }
        RELEASE_ASSERT(instBlockIndex == m_instBlockIndex.mutableSpan().data() + m_instBlockIndex.size());

        m_occupancy.buildClobberIndices(m_mentions.numberOfInsts);
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
        size_t blockIndex = m_instBlockIndex[point / PointOffsets::PointsPerInst];
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
            freq *= m_registerPolicy.rareBlockPenalty;
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

    static constexpr size_t maxValidationFailuresListed = 8;

    void reportValidationFailuresNotListed(size_t failureCount)
    {
        if (failureCount > maxValidationFailuresListed)
            dataLogLn("AIR GREEDY VALIDATION: ", failureCount - maxValidationFailuresListed, " more failures are not listed");
    }

    // Debug code to verify that the results of register allocation and finalization fixup is valid.
    // That is, no two Tmps simultaneously alive share the same register (unless they were coalesced).
    template<Bank bank>
    void validateAssignments()
    {
        if (!Options::airValidateGreedRegAlloc())
            return;
        // A move that is left out on purpose is there for the checks that report what they find. This one stops the process.
        if (!leavesOut(MoveLeftOutByRegisterAllocator::None)) [[unlikely]]
            return;
        size_t failureCount = 0;

        auto fail = [&](BasicBlock* block, Tmp a, Tmp b) {
            if (failureCount++ >= maxValidationFailuresListed)
                return;
            dataLogLn("AIR GREEDY REGISTER ALLOCATION VALIDATION FAILURE");
            dataLogLn("   In BB", *block);
            dataLogLn("     tmp = ", a, " : ", m_map[a]);
            dataLogLn("     tmp = ", b, " : ", m_map[b]);
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

        auto sitsInClobberedRegister = [&](Tmp tmp, const RegisterSet& clobberedRegs) {
            Reg reg = assignedReg(tmp);
            if (!reg)
                return false;
            bool result = false;
            clobberedRegs.forEachWithWidthAndPreserved([&](Reg clobberedReg, Width, PreservedWidth preservedWidth) {
                if (clobberedReg == reg)
                    result = preservedWidth == PreservesNothing || widthForConflicts<bank>(tmp) > Width64;
            });
            return result;
        };

        auto failAtClobber = [&](BasicBlock* block, Inst& patch, Tmp tmp) {
            if (failureCount++ >= maxValidationFailuresListed)
                return;
            dataLogLn("AIR GREEDY REGISTER ALLOCATION VALIDATION FAILURE");
            dataLogLn("   In BB", *block, " at ", patch);
            dataLogLn("     clobbers the register of tmp = ", tmp, " : ", m_map[tmp]);
        };

        auto checkClobbers = [&](BasicBlock* block, Inst& patch, const Vector<Tmp>& liveAfter, const typename TmpLiveness<bank>::LocalCalc& liveBefore) {
            RegisterSet earlyClobberedRegs = patch.extraEarlyClobberedRegs();
            RegisterSet lateClobberedRegs = patch.extraClobberedRegs();
            for (Tmp tmp : liveBefore.live()) {
                if (!tmp.isReg() && sitsInClobberedRegister(tmp, earlyClobberedRegs))
                    failAtClobber(block, patch, tmp);
            }
            for (Tmp tmp : liveAfter) {
                if (liveBefore.isLive(tmp) && sitsInClobberedRegister(tmp, lateClobberedRegs))
                    failAtClobber(block, patch, tmp);
            }
        };

        TmpLiveness<bank> liveness(m_code);

        auto checkInstAfterTerminal = [&](Inst& inst, BasicBlock* successor) {
            Inst& first = successor->at(0);
            if (!first.clobbersAtBlockEntry())
                return;
            auto liveAtHead = liveness.liveAtHead(successor);
            bool definesLiveTmp = false;
            inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank tmpBank, Width) {
                definesLiveTmp |= tmpBank == bank && Arg::isAnyDef(role) && liveAtHead.contains(tmp);
            });
            if (!definesLiveTmp)
                return;
            RegisterSet clobberedRegs = first.extraEarlyClobberedRegs();
            inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank tmpBank, Width) {
                if (tmpBank == bank && Arg::isAnyUse(role) && sitsInClobberedRegister(tmp, clobberedRegs))
                    failAtClobber(successor, first, tmp);
            });
        };

        Vector<Tmp> liveAfterPatch;
        for (BasicBlock* block : m_code) {
            for (unsigned instIndex = block->size(); instIndex-- && !block->at(instIndex).isTerminal();) {
                for (BasicBlock* successor : block->successorBlocks())
                    checkInstAfterTerminal(block->at(instIndex), successor);
            }
            typename TmpLiveness<bank>::LocalCalc localCalc(liveness, block);
            for (unsigned instIndex = block->size(); instIndex--;) {
                checkConflicts(block, localCalc);
                Inst& inst = block->at(instIndex);
                if (inst.kind.opcode != Patch) {
                    localCalc.execute(instIndex);
                    continue;
                }
                liveAfterPatch.shrink(0);
                for (Tmp tmp : localCalc.live()) {
                    if (!tmp.isReg())
                        liveAfterPatch.append(tmp);
                }
                localCalc.execute(instIndex);
                checkClobbers(block, inst, liveAfterPatch, localCalc);
            }
            checkConflicts(block, localCalc);
        }
        if (failureCount) {
            reportValidationFailuresNotListed(failureCount);
            dataLogLn("IR:\n", m_code);
            dataLogLn("State:\n", *this);
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void buildLiveRanges(bool hasFPTmps)
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::buildLiveRanges"_s);

        Vector<unsigned> indexToColumn(m_numberOfLinearIndices);
        for (unsigned index = 0; index < m_numberOfLinearIndices; ++index) {
            if (!m_crossesBoundary[index])
                continue;
            indexToColumn[index] = m_columnToIndex.size();
            m_columnToIndex.append(index);
        }
        BitMatrixLiveness<TmpBoundaryRows> liveness(m_code, BlockFacts { m_blockEnds.span(), m_exposedUses.span(), m_defsInBlocks.span(), m_usesAtTails.span(), m_crossesBoundary.span(), indexToColumn.span(), static_cast<unsigned>(m_columnToIndex.size()) });
        // A Tmp that counts as live in a gap costs a register there, and no more.
        liveness.countGapsAsLivePast(m_tailPoints[m_code.size() - 1] / PointOffsets::PointsPerInst * m_code.proc().limit(Limit::MaxBoundaryIndicesPerInst));
        liveness.compute();
        RELEASE_ASSERT(liveness.status() == BitMatrixLivenessStatus::Solved);
        m_countsGapsAsLive = liveness.hasCountedGapsAsLive();
        if (m_countsGapsAsLive)
            m_code.proc().didReachLimit(Limit::MaxBoundaryIndicesPerInst);

        Vector<Piece> pieceStore;
        unsigned pieceCapacity = m_numberOfLinearIndices + m_numberOfLinearIndices / 2 + 8 * m_occupancy.numberOfClobberPoints() + 16;
        pieceStore.grow(pieceCapacity);
        Piece* pieces = pieceStore.mutableSpan().data();
        unsigned numberOfPieces = 0;
        Vector<RangeState> stateStore(FillWith { }, m_numberOfLinearIndices, RangeState { 0, noPiece });
        RangeState* states = stateStore.mutableSpan().data();
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
                uint8_t& tmpFlags = flags[linearIndex(tmp)];
                if (!(tmpFlags & mayBeCoalescedFlag))
                    m_tmpsWithCoalescables.append(tmp);
                tmpFlags |= mayBeCoalescedFlag;
            }
        };

        std::array<unsigned, Reg::maxIndex() + 1> regLinearIndex;
        for (unsigned i = 0; i <= Reg::maxIndex(); ++i) {
            regLinearIndex[i] = linearIndex(Tmp(Reg::fromIndex(i)));
            flags[regLinearIndex[i]] |= isRegFlag;
        }
        RegisterOccupancy::ClobberWalk clobberWalk(m_occupancy);
        uint64_t activeRegs = 0;

        auto regBit = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
            return static_cast<uint64_t>(1) << tmpForLinearIndex(index).reg().index();
        };
        auto markUse = [&](unsigned index, Point point) ALWAYS_INLINE_LAMBDA {
            Point& end = states[index].activeEnd;
            ASSERT(!end || point < end);
            if (!end) {
                end = point + 1;
                if (flags[index] & isRegFlag) [[unlikely]] {
                    uint64_t bit = regBit(index);
                    activeRegs |= bit;
                    if (clobberWalk.takeDeadClobberAtLastPoint(bit, point + 1))
                        end = point + 2;
                }
            }
        };
        auto appendPiece = [&](Point begin, Point end, unsigned higher) ALWAYS_INLINE_LAMBDA {
            if (numberOfPieces == pieceCapacity) [[unlikely]] {
                pieceCapacity *= 2;
                pieceStore.grow(pieceCapacity);
                pieces = pieceStore.mutableSpan().data();
            }
            pieces[numberOfPieces++] = Piece { begin, end, higher };
        };
        auto closeRange = [&](unsigned index, Point point) ALWAYS_INLINE_LAMBDA {
            RangeState& state = states[index];
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
            uint8_t tmpFlags = flags[index];
            if ((tmpFlags & (isRegFlag | isPinnedFlag)) == isRegFlag) [[unlikely]] {
                uint64_t bit = regBit(index);
                activeRegs &= ~bit;
                if (!states[index].activeEnd && clobberWalk.takeDeadClobberAtLastPoint(bit, point + 1))
                    states[index].activeEnd = point + 2;
            }
            if (!(tmpFlags & isPinnedFlag)) [[likely]]
                closeRange(index, point);
            if (tmpFlags & mayBeCoalescedFlag)
                pruneCoalescables(block->at(instIndex), tmpForLinearIndex(index), point, states, pieces);
        };

        for (unsigned ordinal : m_tmpMoves) {
            BasicBlock* block = m_code[m_instBlockIndex[ordinal]];
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
                    prefer(m_map[other], reg, adjustedBlockFrequency(block));
                    continue;
                }
                if (!m_code.isPinned(reg))
                    continue;
            }
            ASSERT(inst.args()[0].isTmp() && inst.args()[1].isTmp());
            addMaybeCoalescable(inst.args()[0].tmp(), inst.args()[1].tmp(), block);
        }
        for (Tmp tmp : m_tmpsWithCoalescables)
            m_map[tmp].coalescables.mergeDuplicates();

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
        const unsigned* instFirstMention = m_mentions.instFirstMention.span().data();
        const uint8_t* allInstTimings = m_mentions.instTimings.span().data();
        const unsigned* columnToIndex = m_columnToIndex.span().data();

        BasicBlock* blockAfter = nullptr;
        for (size_t blockIndex = m_code.size(); blockIndex--;) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;

            Point positionOfHead = this->positionOfHead(block);
            Point positionOfTail = this->positionOfTail(block);

            if (blockAfter) {
                Point blockAfterPositionOfHead = this->positionOfHead(blockAfter);
                liveness.forEachLiveOnlyAtHeadOfNextBlock(block, [&](unsigned column) {
                    unsigned index = columnToIndex[column];
                    if (states[index].activeEnd && !(flags[index] & isPinnedFlag)) [[likely]]
                        closeRange(index, blockAfterPositionOfHead);
                });
            }
            liveness.forEachLiveOnlyAtTail(block, [&](unsigned column) {
                markUse(columnToIndex[column], positionOfTail);
            });

            unsigned headOrdinal = positionOfHead / PointOffsets::PointsPerInst;
            for (unsigned instIndex = block->size(); instIndex--;) {
                Point positionOfEarly = this->positionOfEarly(positionOfHead, instIndex);
                Point positionOfLate = this->positionOfLate(positionOfHead, instIndex);
                unsigned begin = instFirstMention[headOrdinal + instIndex];
                unsigned end = instFirstMention[headOrdinal + instIndex + 1];
                uint8_t instTimings = allInstTimings[headOrdinal + instIndex];

                if (instTimings & Mentions::lateUse) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::lateUse)
                            markUse(tmps[i], positionOfLate);
                    }
                }
                if (instTimings & Mentions::lateDef) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::lateDef)
                            markDef(block, instIndex, tmps[i], positionOfLate);
                    }
                }
                if (clobberWalk.nextPoint() == positionOfLate) [[unlikely]] {
                    uint64_t regs = clobberWalk.regsAtNextPoint() & regsToMark;
                    clobberWalk.finishNextPoint(regs & ~activeRegs);
                    for (regs &= activeRegs; regs; regs &= regs - 1)
                        markDef(block, instIndex, regLinearIndex[std::countr_zero(regs)], positionOfLate);
                }
                if (instTimings & Mentions::earlyUse) {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::earlyUse)
                            markUse(tmps[i], positionOfEarly);
                    }
                }
                if (instTimings & Mentions::earlyDef) [[unlikely]] {
                    for (unsigned i = begin; i < end; ++i) {
                        if (timings[i] & Mentions::earlyDef)
                            markDef(block, instIndex, tmps[i], positionOfEarly);
                    }
                }
                if (clobberWalk.nextPoint() == positionOfEarly) [[unlikely]] {
                    uint64_t regs = clobberWalk.regsAtNextPoint() & regsToMark;
                    uint64_t deadClobbers = 0;
                    for (; regs; regs &= regs - 1) {
                        unsigned index = regLinearIndex[std::countr_zero(regs)];
                        const RangeState& state = states[index];
                        uint64_t bit = regs & -regs;
                        bool reachesUpperPiece = clobberWalk.lastPointHasDeadClobber(bit, positionOfLate)
                            || (state.lowestPiece != noPiece && pieces[state.lowestPiece].begin == positionOfLate);
                        if (!(activeRegs & bit) && !reachesUpperPiece)
                            deadClobbers |= bit;
                        else
                            markDef(block, instIndex, index, positionOfEarly);
                    }
                    clobberWalk.finishNextPoint(deadClobbers);
                }
            }
            blockAfter = block;
        }
        RELEASE_ASSERT(clobberWalk.isDone());
        if (blockAfter) {
            Point firstBlockPositionOfHead = this->positionOfHead(blockAfter);
            liveness.forEachLiveAtHeadOfFirstBlock([&](unsigned column) {
                unsigned index = columnToIndex[column];
                if (!(flags[index] & isPinnedFlag))
                    closeRange(index, firstBlockPositionOfHead);
            });
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
        pruneCoalescablesAtDefsSetAside();

        m_columnToIndex.clear();
        m_crossesBoundary.clear();
        m_exposedUses.clear();
        m_defsInBlocks.clear();
        m_usesAtTails.clear();
        m_blockEnds.clear();

        validateLiveRanges();
    }

    void validateLiveRanges()
    {
        if (!Options::airValidateGreedRegAlloc())
            return;
        UnifiedTmpLiveness liveness(m_code);
        for (BasicBlock* block : m_code) {
            Point head = positionOfHead(block);
            for (Tmp tmp : liveness.liveAtHead(block)) {
                if (tmp.isReg() && m_code.isPinned(tmp.reg()))
                    continue;
                RELEASE_ASSERT(m_map[tmp].liveRange.contains(head));
            }
        }
        if (m_countsGapsAsLive)
            return;
        m_code.forEachTmp([&](Tmp tmp) {
            const LiveRange& liveRange = m_map[tmp].liveRange;
            forEachBlockInLiveRange(liveRange, [&](BasicBlock* block) {
                RELEASE_ASSERT_IMPLIES(liveRange.contains(positionOfHead(block)), liveness.liveAtHead(block).contains(tmp));
                return IterationStatus::Continue;
            });
        });
    }

    NEVER_INLINE void pruneCoalescables(Inst& inst, Tmp def, Point point, const RangeState* states, const Piece* pieces)
    {
        TmpData& defData = m_map[def];
        if (defData.coalescables.isEmpty())
            return;
        Tmp movSrc = mayBeCoalescable(inst) ? inst.args()[0].tmp() : Tmp();
        if (defData.coalescables.size() > Coalescables::maxSizeWalked) {
            m_defsSetAside.append({ def, movSrc, point });
            return;
        }
        defData.coalescables.removeAllMatching([&](Tmp tmp) {
            ASSERT(tmp != def);
            if (tmp == movSrc)
                return false;
            const RangeState& state = states[linearIndex(tmp)];
            if (!state.activeEnd) {
                if (state.lowestPiece == noPiece)
                    return false;
                const Piece& piece = pieces[state.lowestPiece];
                if (point < piece.begin || point >= piece.end)
                    return false;
            }
            dataLogLnIf(verbose(), "Pruning affinity ", def, " ", tmp);
            Coalescables& coalescablesOfTmp = m_map[tmp].coalescables;
            if (coalescablesOfTmp.size() > Coalescables::maxSizeWalked)
                m_removalsSetAside.append({ tmp, def });
            else
                coalescablesOfTmp.remove(def);
            return true;
        });
    }

    // A long list is gone through once for all that leave it. It stays long until then, so its Tmp's defs are all set aside.
    void removeCoalescablesSetAside()
    {
        std::ranges::sort(m_removalsSetAside, { }, [](const RemovalSetAside& removal) { return std::pair { removal.from.internalValue(), removal.partner.internalValue() }; });
        for (std::span<const RemovalSetAside> rest = m_removalsSetAside.span(); !rest.empty();) {
            Tmp from = rest.front().from;
            size_t count = std::ranges::find_if(rest, [&](const RemovalSetAside& other) { return other.from != from; }) - rest.begin();
            std::span<const RemovalSetAside> removals = rest.first(count);
            rest = rest.subspan(count);
            m_map[from].coalescables.removeAllMatching([&](Tmp tmp) {
                return std::ranges::binary_search(removals, tmp.internalValue(), { }, [](const RemovalSetAside& removal) { return removal.partner.internalValue(); });
            });
        }
        m_removalsSetAside.clear();
    }

    // Each partner looks for a def in its own live range, in place of each def looking at every partner.
    void pruneCoalescablesAtDefsSetAside()
    {
        removeCoalescablesSetAside();
        std::ranges::sort(m_defsSetAside, { }, [](const DefSetAside& def) { return std::pair { def.tmp.internalValue(), def.point }; });
        for (std::span<const DefSetAside> rest = m_defsSetAside.span(); !rest.empty();) {
            Tmp def = rest.front().tmp;
            size_t count = std::ranges::find_if(rest, [&](const DefSetAside& other) { return other.tmp != def; }) - rest.begin();
            std::span<const DefSetAside> defs = rest.first(count);
            rest = rest.subspan(count);
            m_map[def].coalescables.removeAllMatching([&](Tmp tmp) {
                for (const Interval& interval : m_map[tmp].liveRange.intervals()) {
                    for (auto at = std::ranges::lower_bound(defs, interval.begin(), { }, &DefSetAside::point); at != defs.end() && at->point < interval.end(); ++at) {
                        if (at->movSrc == tmp)
                            continue;
                        dataLogLnIf(verbose(), "Pruning affinity ", def, " ", tmp);
                        m_removalsSetAside.append({ tmp, def });
                        return true;
                    }
                }
                return false;
            });
        }
        m_defsSetAside.clear();
        removeCoalescablesSetAside();
    }

    Inst& instAtOrdinal(unsigned ordinal)
    {
        BasicBlock* block = m_code[m_instBlockIndex[ordinal]];
        return block->at(ordinal - positionOfHead(block) / PointOffsets::PointsPerInst);
    }

    bool admitsStack(Inst& inst, Arg& arg)
    {
        if (!inst.admitsStack(arg))
            return false;
        return m_usesExtendedForms || !admitsStackOnlyInExtendedForm(inst, &arg - &inst.args()[0]);
    }

    bool cannotSpillInPlace(Tmp tmp, Interval interval)
    {
        unsigned last = (interval.end() - 1) / PointOffsets::PointsPerInst;
        for (unsigned ordinal = interval.begin() / PointOffsets::PointsPerInst; ordinal <= last; ++ordinal) {
            Inst& inst = instAtOrdinal(ordinal);
            for (Arg& arg : inst.args()) {
                if (arg.isTmp()) {
                    if (arg.tmp() == tmp && !admitsStack(inst, arg))
                        return true;
                    continue;
                }
                bool mentionsTmp = false;
                arg.forEachTmpFast([&](Tmp& named) {
                    mentionsTmp |= named == tmp;
                });
                if (mentionsTmp)
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
            if (other.m_size > m_size) {
                other.reserveInFront(allocator, m_size);
                other.m_members -= m_size;
                other.m_roomInFront -= m_size;
                other.m_capacity += m_size;
                for (unsigned i = 0; i < m_size; ++i)
                    other.m_members[i] = m_members[i];
                m_members = other.m_members;
                m_roomInFront = other.m_roomInFront;
                m_capacity = other.m_capacity;
                m_size += other.m_size;
            } else {
                reserve(allocator, m_size + other.m_size);
                for (Tmp member : other.members())
                    m_members[m_size++] = member;
            }
            other.m_members = nullptr;
            other.m_roomInFront = 0;
            other.m_capacity = 0;
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
            if (m_size > AirLimits::maxAffinityGroupSizeWithoutMap || m_range.intervals().size() > AirLimits::maxAffinityGroupSizeWithoutMap) [[unlikely]]
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
            m_roomInFront = 0;
            m_capacity = capacity;
        }

        void reserveInFront(GreedyAllocator& allocator, unsigned room)
        {
            if (room <= m_roomInFront)
                return;
            room = std::max(room, m_size);
            unsigned total = room + m_capacity;
            total += total & 1;
            Tmp* members = reinterpret_cast<Tmp*>(allocator.m_intervalArena.allocate(total / 2)) + room;
            for (unsigned i = 0; i < m_size; ++i)
                members[i] = m_members[i];
            m_members = members;
            m_roomInFront = room;
            m_capacity = total - room;
        }

        Tmp* m_members { nullptr };
        unsigned m_size { 0 };
        unsigned m_roomInFront { 0 };
        unsigned m_capacity { 0 };
        LiveRange m_range;
        std::unique_ptr<LivenessMap<bank>> m_liveness;
    };

    using GroupIndex = uint32_t;
    static constexpr GroupIndex noGroup = std::numeric_limits<GroupIndex>::max();

    // A Tmp has the label of its group. Where two groups become one, the members of the smaller one get the label of the larger one.
    template<Bank bank>
    class TmpGroupMap {
    public:
        explicit TmpGroupMap(unsigned numberOfTmps)
            : m_labels(numberOfTmps, noGroup)
        {
        }

        GroupIndex operator[](Tmp tmp) const
        {
            GroupIndex label = m_labels[tmp];
            return label == noGroup ? noGroup : m_groupOfLabel[label];
        }

        void addGroup(GroupIndex group)
        {
            ASSERT(group == m_groupOfLabel.size());
            m_groupOfLabel.append(group);
            m_labelOfGroup.append(group);
        }

        void addMember(GroupIndex group, Tmp tmp) { m_labels[tmp] = m_labelOfGroup[group]; }

        void merge(GroupIndex group, std::span<const Tmp> members, GroupIndex mergedGroup, std::span<const Tmp> mergedMembers)
        {
            if (mergedMembers.size() <= members.size()) {
                for (Tmp member : mergedMembers)
                    m_labels[member] = m_labelOfGroup[group];
                return;
            }
            GroupIndex label = m_labelOfGroup[mergedGroup];
            for (Tmp member : members)
                m_labels[member] = label;
            m_labelOfGroup[group] = label;
            m_groupOfLabel[label] = group;
        }

    private:
        IndexMap<Tmp::Indexed<bank>, GroupIndex> m_labels;
        Vector<GroupIndex> m_groupOfLabel;
        Vector<GroupIndex> m_labelOfGroup;
    };

    template<Bank bank>
    void coalesceSingletons(Tmp tmp0, Tmp tmp1, Vector<AffinityGroup<bank>>& groups, TmpGroupMap<bank>& tmpToGroup)
    {
        ASSERT(m_map.get<bank>(tmp0).coalescables.template contains<bank>(tmp1) && m_map.get<bank>(tmp1).coalescables.template contains<bank>(tmp0));
        auto newIndex = groups.size();
        groups.constructAndAppend(*this, tmp0, tmp1);
        tmpToGroup.addGroup(newIndex);
        tmpToGroup.addMember(newIndex, tmp0);
        tmpToGroup.addMember(newIndex, tmp1);
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
        tmpToGroup.addMember(groupIndex, singleton);
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

        tmpToGroup.merge(groupIndex0, groups[groupIndex0].members(), groupIndex1, groups[groupIndex1].members());
        groups[groupIndex0].merge(*this, groups[groupIndex1]);
        dataLogLnIf(verbose(), "Merged group ", groupIndex1, " into ", groupIndex0);
        return true;
    }

    template<Bank bank>
    void coalesceTmps()
    {
        CompilerTimingScope timingScope("Air"_s, "GreedyRegAlloc::coalesceTmps"_s);

        Vector<AffinityGroup<bank>> groups;
        TmpGroupMap<bank> tmpToGroup(Tmp::indexEnd(m_code, bank));

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
                if (tmp.tmpIndex(bank) < with.tmp.tmpIndex(bank) && m_map.get<bank>(with.tmp).stage == Stage::New)
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
        size_t failureCount = 0;

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
                    if (tmpToGroup[live] == groupIdx && failureCount++ < maxValidationFailuresListed) {
                        dataLogLn("AIR GREEDY COALESCING VALIDATION FAILURE");
                        dataLogLn("   In BB", *block, " at inst ", instIndex, ": ", inst);
                        dataLogLn("   Non-move ", earlyDefs ? "early" : "late", " def of group member ", tmp);
                        dataLogLn("   But group member ", live, " is simultaneously live");
                        dataLogLn("   Group: ", groups[groupIdx]);
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
        if (failureCount) {
            reportValidationFailuresNotListed(failureCount);
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
            TmpData preferences;
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
                prefer(preferences, memberData.preferredReg, memberData.preferredWeight);
                prefer(preferences, memberData.secondPreferredReg, memberData.secondPreferredWeight);
                memberData.stage = Stage::Coalesced;
            }
            m_tmpWidth.setWidths(representative, useWidth, defWidth);

            m_map.append(representative, TmpData());
            TmpData& representativeData = m_map.get<bank>(representative);
            representativeData.useDefCost = cost;
            representativeData.liveRange = group.buildLiveRange(m_intervalArena);
            representativeData.preferredReg = preferred;
            representativeData.hasColdUse = hasColdUse;
            ASSERT(preferences.preferredReg == preferred);
            representativeData.preferredWeight = preferences.preferredWeight;
            representativeData.secondPreferredReg = preferences.secondPreferredReg;
            representativeData.secondPreferredWeight = preferences.secondPreferredWeight;
            representativeData.spillCostDiscount = preferences.spillCostDiscount;
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
        const unsigned* instFirstMention = m_mentions.instFirstMention.span().data();
        const uint8_t* allInstTimings = m_mentions.instTimings.span().data();
        const unsigned* representatives = m_representatives.span().data();
        const unsigned gpIndicesEnd = m_gpIndicesEnd;
        for (BasicBlock* block : m_code) {
            unsigned headOrdinal = positionOfHead(block) / PointOffsets::PointsPerInst;
            unsigned blockSize = block->size();
            for (unsigned instIndex = 0; instIndex < blockSize; ++instIndex) {
                unsigned begin = instFirstMention[headOrdinal + instIndex];
                unsigned end = instFirstMention[headOrdinal + instIndex + 1];
                bool needsVisit = end - begin == 2 && tmps[begin] == tmps[begin + 1];
                for (unsigned i = begin; i < end; ++i) {
                    unsigned index = tmps[i];
                    if (unsigned representative = representatives[index]) {
                        tmps[i] = representative;
                        needsVisit = true;
                    } else if (index >= gpIndicesEnd) [[unlikely]]
                        tmps[i] = AbsoluteTmpMapper<FP>::tmpFromAbsoluteIndex(index - gpIndicesEnd).internalValue();
                }
                if (!needsVisit)
                    continue;

                Inst& inst = block->at(instIndex);
                bool maybeCoalescable = mayBeCoalescable(inst);
                if (allInstTimings[headOrdinal + instIndex] & Mentions::wasDecodedSlowly) [[unlikely]] {
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
                && cannotSpillInPlace(tmp, tmpData.liveRange.intervals().first())) {
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
        if (tmpData.spillability != TmpData::Spillability::Spillable && !tmpData.paysForCalleeSave)
            tmpData.setPaysForCalleeSave();
        if (tryAllocate<bank>(tmp, tmpData))
            return;
        if (tryEvict<bank>(tmp, tmpData))
            return;
        RELEASE_ASSERT(tmpData.stage == Stage::TryAllocate); // Unspillable must have succeeded
        if (m_splitPolicy.splitsAroundClobbersEarly) {
            if (nextCalleeSaveIsFree<bank>() && tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
                return;
            if (trySplitAroundClobbers<bank>(tmp, tmpData, true))
                return;
            if (tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
                return;
        }
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
        if (tryTakeRegisterOfTmpsCheaperOnStack<bank>(tmp, tmpData))
            return;
        if (tryTakeRegisterOfUnusedTmps<bank>(tmp, tmpData))
            return;
        ASSERT(queueContainsOnlySpills());
        spill(tmp, tmpData);
    }

    template <Bank bank>
    bool nextCalleeSaveIsFree() const
    {
        return isARM64() && bank == GP && ((m_occupancy.numberOfCalleeSaves(GP) - m_occupancy.numberOfFreshCalleeSaves(GP)) & 1);
    }

    template <Bank bank>
    bool tryAllocateFreshCalleeSave(Tmp tmp, TmpData& tmpData)
    {
        UseDefCost worth = tmpData.useDefCost;
        worth.subtractSaturating(UseDefCost(tmpData.spillCostDiscount));
        if (!m_occupancy.numberOfFreshCalleeSaves(bank) || tmpData.paysForCalleeSave || (worth <= m_calleeSaveCost && !nextCalleeSaveIsFree<bank>()))
            return false;
        tmpData.setPaysForCalleeSave();
        return tryAllocate<bank>(tmp, tmpData);
    }

    bool m_isEvictingHolder { false };

    void prefer(TmpData& tmpData, Reg reg, float weight)
    {
        tmpData.prefer(reg, weight);
        if (m_discountsPreferredRegisterMoves)
            tmpData.spillCostDiscount = tmpData.preferredWeight + tmpData.secondPreferredWeight;
    }

    // Two Tmps that get one register: a conditional move on x86 then needs no copy in front of it, and a pair of a Shuffle no move at all.
    // A Shuffle stands where a live range was cut in two on purpose, so its pairs are not made one Tmp again: whichever half is assigned first names its register to the other.
    bool isSavedByFrame(Reg reg) const
    {
        return (RegisterSet::calleeSaveRegisters().contains(reg, IgnoreVectors) && !m_code.unsavedCalleeSaves().contains(reg, IgnoreVectors)) || m_registerPolicy.savedByCallee.contains(reg, IgnoreVectors);
    }

    void recordNeighbors()
    {
        for (unsigned ordinal : m_instsThatNameNeighbors) {
            Inst& inst = instAtOrdinal(ordinal);
            if (inst.kind.opcode == Shuffle) {
                for (unsigned i = 0; i + 2 < inst.args().size(); i += 3) {
                    Arg& source = inst.args()[i];
                    Arg& destination = inst.args()[i + 1];
                    if (!source.isTmp() || source.isReg() || !destination.isTmp() || destination.isReg() || source.tmp() == destination.tmp() || source.tmp().bank() != destination.tmp().bank())
                        continue;
                    TmpData& sourceData = m_map[source.tmp()];
                    TmpData& destinationData = m_map[destination.tmp()];
                    sourceData.addNeighbor(destination.tmp());
                    destinationData.addNeighbor(source.tmp());
                    float frequency = adjustedBlockFrequency(m_code[m_instBlockIndex[ordinal]]);
                    sourceData.frequencyOfCuts += frequency;
                    destinationData.frequencyOfCuts += frequency;
                    // What one half already wishes for, as the register that an argument arrives in, the other wishes for too.
                    prefer(destinationData, sourceData.preferredReg, 0);
                    prefer(sourceData, destinationData.preferredReg, 0);
                }
                continue;
            }
            Arg& yielded = inst.args()[5];
            if (!yielded.isTmp() || yielded.isReg() || yielded.tmp().bank() != GP)
                continue;
            for (unsigned i = 3; i < 5; ++i) {
                Arg& taken = inst.args()[i];
                if (!taken.isTmp() || taken.isReg() || taken.tmp().bank() != GP || taken.tmp() == yielded.tmp())
                    continue;
                m_map[yielded.tmp()].addNeighbor(taken.tmp());
                m_map[taken.tmp()].addNeighbor(yielded.tmp());
            }
        }
    }

    NEVER_INLINE void notifyNeighbors(TmpData& tmpData, Reg reg)
    {
        // A live range is cut so that the half outside needs no frame. In a register that a frame saves it would need one wherever it lives:
        // that is a wish only for a half that does little but cross the border, where the frame is set up anyway.
        bool needsFrame = tmpData.frequencyOfCuts && isSavedByFrame(reg);
        for (Tmp neighbour : tmpData.neighbours) {
            if (!neighbour)
                continue;
            TmpData& neighbourData = m_map[neighbour];
            if (needsFrame && neighbourData.useDefCost.value() > 2 * neighbourData.frequencyOfCuts)
                continue;
            prefer(neighbourData, reg, 0);
        }
    }

    // What notifyNeighbors() said no longer holds.
    void takeBackFromNeighbors(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        for (Tmp neighbour : tmpData.neighbours) {
            if (!neighbour)
                continue;
            TmpData& neighbourData = m_map[neighbour];
            bool isNamedByAnother = false;
            for (Tmp other : neighbourData.neighbours)
                isNamedByAnother |= other && other != tmp && m_map[other].assigned == reg;
            if (!isNamedByAnother)
                neighbourData.forgetWeightlessWishFor(reg);
        }
    }

    // In the register of its other half, a half of a cut live range needs no move in the Shuffles between the two.
    float wishFor(const TmpData& tmpData, Reg reg)
    {
        float wish = tmpData.wishFor(reg);
        if (!tmpData.frequencyOfCuts)
            return wish;
        for (Tmp neighbour : tmpData.neighbours) {
            if (neighbour && m_map[neighbour].assigned == reg)
                return wish + tmpData.frequencyOfCuts;
        }
        return wish;
    }

    template <Bank bank>
    bool tryAllocate(Tmp tmp, TmpData& tmpData)
    {
        ASSERT(&m_map.get<bank>(tmp) == &tmpData);
        ASSERT(!assignedReg(tmp));

        Width width = widthForConflicts<bank>(tmp);
        auto candidates = m_occupancy.candidatesFor(tmpData, RegisterOccupancy::FailureMemo::Allocation, RegisterOccupancy::Clobbers::Exclude);

        auto tryAllocateToReg = [&](Reg r) {
            if (!candidates.contains(r))
                return false;
            if (!m_occupancy.hasConflict(r, tmpData, width)) {
                assign(tmp, tmpData, r);
                return true;
            }
            return false;
        };

        if (tmpData.preferredReg) {
            if (tryAllocateToReg(tmpData.preferredReg))
                return true;
        }
        Reg secondPreferredReg = m_registerPolicy.usesSecondPreferredRegister ? tmpData.secondPreferredReg : Reg();
        if (secondPreferredReg && tryAllocateToReg(secondPreferredReg))
            return true;
        if (m_registerPolicy.evictsHolderOfPreferredRegister && !m_isEvictingHolder && tmpData.spillability != TmpData::Spillability::Unspillable) {
            auto holderCandidates = m_occupancy.candidatesFor(tmpData, RegisterOccupancy::FailureMemo::None, RegisterOccupancy::Clobbers::Exclude);
            auto tryEvictHolder = [&](Reg r) {
                float wish = r ? wishFor(tmpData, r) : 0;
                if (!r || !(wish > 0) || !holderCandidates.contains(r))
                    return false;
                Tmp holder;
                bool isPossible = true;
                m_occupancy.forEachTmpConflict(r, tmpData.liveRange, width, [&](auto& conflict) -> IterationStatus {
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
                if (holderData.spillability == TmpData::Spillability::Unspillable || holderData.stage != Stage::Assigned || wishFor(holderData, r) >= wish)
                    return false;
                // One move less at a cut is not worth a move more for the holder.
                if (!(tmpData.wishFor(r) > 0) && wishFor(holderData, r) > 0)
                    return false;
                Stage stage = tmpData.stage;
                uint32_t holderFailedAtEpoch = holderData.allocationFailedAtEpoch;
                evict(holder, holderData, r);
                takeBackFromNeighbors(holder, holderData, r);
                assign(tmp, tmpData, r);
                m_isEvictingHolder = true;
                bool isMoved = tryAllocate<bank>(holder, holderData);
                m_isEvictingHolder = false;
                if (isMoved)
                    return true;
                evict(tmp, tmpData, r);
                tmpData.stage = stage;
                holderData.allocationFailedAtEpoch = holderFailedAtEpoch;
                assign(holder, holderData, r);
                return false;
            };
            if (tryEvictHolder(tmpData.preferredReg) || tryEvictHolder(secondPreferredReg))
                return true;
        }
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (r == tmpData.preferredReg)
                continue;
            if (r == secondPreferredReg)
                continue;
            if (tryAllocateToReg(r))
                return true;
        }
        m_occupancy.rememberFailure(tmpData, RegisterOccupancy::FailureMemo::Allocation);
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
            uint64_t clobbered = m_occupancy.clobberedRegs(m_map.get<bank>(tmp));
            for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
                out.print("  ", r, ": ");
                if (clobbered >> r.index() & 1)
                    out.print("clobbered, ");
                m_occupancy.forEachTmpConflict(r, m_map.get<bank>(tmp).liveRange, widthForConflicts<bank>(tmp),
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
        auto candidates = m_occupancy.candidatesFor(tmpData, RegisterOccupancy::FailureMemo::Eviction, RegisterOccupancy::Clobbers::Exclude);
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (!candidates.contains(r))
                continue;
            SpillCost conflictsSpillCost(0);
            m_visited.clear();
            m_occupancy.forEachTmpConflict(r, liveRange, width,
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
            m_occupancy.rememberFailure(tmpData, RegisterOccupancy::FailureMemo::Eviction);
            return false;
        }
        // It's cheaper to spill all the already-assigned conflicting tmps, so evict them in favor of assigning 'tmp'.
        m_occupancy.forEachTmpConflict(bestEvictReg, liveRange, widthForConflicts<bank>(tmp),
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
        m_occupancy.add(tmp, tmpData, reg);
        assignImpl(tmp, tmpData, reg);
        if (tmpData.neighbours[0]) [[unlikely]]
            notifyNeighbors(tmpData, reg);
    }

    void evict(Tmp tmp, TmpData& tmpData, Reg reg)
    {
        ASSERT(tmpData.stage == Stage::Assigned);
        ASSERT(tmpData.spillCost() != unspillableCost);
        ASSERT(tmpData.assigned == reg);
        m_occupancy.remove(tmpData, reg);
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
        if (!m_splitPolicy.splitsAroundClobbersEarly) {
            if (trySplitAroundClobbers<bank>(tmp, tmpData))
                return true;
            if (tryAllocateFreshCalleeSave<bank>(tmp, tmpData))
                return true;
        }
        if (tmpData.isOnlyReadAtSomeClobber && trySplitAroundClobbers<bank>(tmp, tmpData, true, ClobbersThatRead::AreSplitAround))
            return true;
        if (trySplitAroundLoop<bank>(tmp, tmpData))
            return true;
        if (tryTakeRegisterOfTmpsCheaperOnStack<bank>(tmp, tmpData))
            return true;
        if (tryTakeRegisterOfUnusedTmps<bank>(tmp, tmpData))
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

        bool hasExitWithEntryClobbers = false;
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
                hasExitWithEntryClobbers |= succ.block()->at(0).clobbersAtBlockEntry();
            }
        }

        LoopData& loopData = m_loopData[loop.index()];
        ASSERT(!loopData.range.size() && !loopData.boundary.size());
        loopData.hasExitWithEntryClobbers = hasExitWithEntryClobbers;

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
                    if (loopData.hasExitWithEntryClobbers)
                        continue;
                    // Skip loops that are too large relative to the live range since they are unlikely to improve allocation progress.
                    if (loopData.range.size() > liveRange.size() * m_splitPolicy.maxLoopFractionOfLiveRange)
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
        if (!m_splitPolicy.splitsAroundLoops)
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
        loopData.setLiveRange(LiveRange::subtract(m_intervalArena, originalData.liveRange, nonLoopRange));
        ASSERT(loopData.liveRange.size());

        UseDefList& loopUseDefs = m_newTmpUseDefLists.ensure(loopTmp, [] {
            return UseDefList();
        }).iterator->value;

        bool loopHasDef = false;
        size_t cursor = 0;
        for (Interval interval : loopData.liveRange.intervals()) {
            do {
                interval = forEachUseDefWithin(tmp, interval, cursor, [&](Point point, Inst& inst, BasicBlock& block) {
                    forEachMention(tmp, point, inst, [&](Tmp& t, Arg::Role role) {
                        t = loopTmp;
                        if (Arg::isAnyDef(role))
                            loopHasDef = true;
                        if (Arg::isColdUse(role))
                            loopData.hasColdUse = true;
                        else
                            loopData.useDefCost += UseDefCost(block.frequency());
                        loopUseDefs.add(point);
                    });
                });
            } while (interval);
        }

        // originalTmp no longer live within the loop.
        originalData.setLiveRange(WTF::move(nonLoopRange));
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

    struct BlocksOnStack {
        Tmp tmp;
        Vector<unsigned, 8> blocks;
    };

    static bool containsPoint(const LiveRange& range, Point point)
    {
        auto intervals = range.intervals().span();
        auto next = std::ranges::upper_bound(intervals, point, { }, [](const Interval& interval) {
            return interval.begin();
        });
        return next != intervals.begin() && point < std::prev(next)->end();
    }

    static bool canPlaceMovesBetween(BasicBlock* from, BasicBlock* to)
    {
        return from->last().kind.opcode != Patch && !to->at(0).clobbersAtBlockEntry();
    }

    static bool endsInLoneJump(BasicBlock* block)
    {
        return block->numSuccessors() == 1 && block->last().kind.opcode == Jump;
    }

    static double shareOfFrequency(BasicBlock* from, BasicBlock* to)
    {
        constexpr WTF::FrequencyEstimatorConstants constants;
        double all = 0;
        double taken = 0;
        for (const FrequentedBlock& edge : from->successors()) {
            double share = edge.isRare() ? constants.rareEdgeShare : edge.frequency() == FrequencyClass::Unlikely ? constants.unlikelyEdgeShare : 1;
            all += share;
            if (edge.block() == to)
                taken += share;
        }
        return taken / all;
    }

    float costOfMoveBetween(BasicBlock* from, BasicBlock* to)
    {
        if (!canPlaceMovesBetween(from, to))
            return std::numeric_limits<float>::infinity();
        float frequency = std::min<float>(adjustedBlockFrequency(from) * shareOfFrequency(from, to), adjustedBlockFrequency(to));
        return endsInLoneJump(from) || to->numPredecessors() == 1 ? frequency : 2 * frequency;
    }

    // The blocks of `blocks`, which are in the order of the code, that `other` is live in. Nothing where it is named in one of them, or the moves cost `limit` or more.
    template<Bank bank>
    std::optional<Vector<unsigned, 8>> findBlocksToPassOnStack(Tmp other, const Vector<unsigned, 16>& blocks, const BitVector& isAmongBlocks, float limit, float& cost)
    {
        TmpData& otherData = m_map.get<bank>(other);
        auto intervals = otherData.liveRange.intervals().span();
        auto useDefs = useDefsOf(other);
        size_t intervalIndex = 0;
        size_t useDefIndex = 0;
        Vector<unsigned, 8> result;
        for (unsigned blockIndex : blocks) {
            BasicBlock* block = m_code[blockIndex];
            Point head = positionOfHead(block);
            Point tail = positionOfTail(block);
            while (intervalIndex < intervals.size() && intervals[intervalIndex].end() <= head)
                ++intervalIndex;
            if (intervalIndex == intervals.size())
                break;
            if (intervals[intervalIndex].begin() > tail)
                continue;
            if (intervals[intervalIndex].begin() > head || intervals[intervalIndex].end() <= tail)
                return std::nullopt;
            while (useDefIndex < useDefs.size() && useDefs[useDefIndex] < head)
                ++useDefIndex;
            if (useDefIndex < useDefs.size() && useDefs[useDefIndex] <= tail)
                return std::nullopt;
            result.append(blockIndex);
        }
        for (unsigned blockIndex : result) {
            BasicBlock* block = m_code[blockIndex];
            for (BasicBlock* predecessor : block->predecessors()) {
                if (!isAmongBlocks.quickGet(predecessor->index()))
                    cost += costOfMoveBetween(predecessor, block);
            }
            BasicBlock* previous = nullptr;
            for (BasicBlock* successor : block->successorBlocks()) {
                if (successor != std::exchange(previous, successor) && !isAmongBlocks.quickGet(successor->index()) && containsPoint(otherData.liveRange, positionOfHead(successor)))
                    cost += costOfMoveBetween(block, successor);
            }
            if (cost >= limit)
                return std::nullopt;
        }
        return result;
    }

    struct Cycles {
        explicit Cycles(Code& code)
            : forest(code.cfg(), [](std::span<BasicBlock* const> members, const auto&, const auto&) { return members[0]; })
            , blocks(forest.size())
            , namedTmps(forest.size())
            , likelyBlocks(forest.size())
        {
            for (BasicBlock* block : code) {
                for (unsigned cycle = forest.innermost(block); cycle != forest.none; cycle = forest[cycle].parent)
                    blocks[cycle].append(block->index());
            }
        }

        bool names(Code& code, unsigned cycle, Tmp tmp)
        {
            NamedTmps& named = namedTmps[cycle];
            if (!named.numberOfTmps[GP] && !named.numberOfTmps[FP]) {
                forEachBank([&](Bank bank) {
                    named.numberOfTmps[bank] = code.numTmps(bank);
                    named[bank].ensureSize(code.numTmps(bank));
                });
                for (unsigned blockIndex : blocks[cycle]) {
                    for (Inst& inst : *code[blockIndex]) {
                        inst.forEachTmpFast([&](Tmp& mentioned) {
                            if (!mentioned.isReg())
                                named[mentioned.bank()].quickSet(mentioned.tmpIndex());
                        });
                    }
                }
            }
            return tmp.tmpIndex() >= named.numberOfTmps[tmp.bank()] || named[tmp.bank()].quickGet(tmp.tmpIndex());
        }

        // Whether a turn of the innermost cycle around the block gets there from the head without taking a rare edge. A cycle inside is left sooner or later, however its ways out are marked,
        // and in code that is rare itself every edge is marked: an edge counts as rare only beside one that is not.
        bool isLikelyInItsCycle(Code& code, BasicBlock* block)
        {
            unsigned cycle = forest.innermost(block);
            if (cycle == forest.none)
                return false;
            BitVector& likely = likelyBlocks[cycle];
            if (!hasLikelyBlocks.get(cycle)) {
                hasLikelyBlocks.set(cycle);
                likely.ensureSize(code.size());
                Vector<BasicBlock*, 8> worklist { forest[cycle].header() };
                likely.quickSet(worklist[0]->index());
                while (!worklist.isEmpty()) {
                    BasicBlock* from = worklist.takeLast();
                    unsigned inner = forest.innermost(from);
                    bool everyEdgeIsRare = std::ranges::all_of(from->successors(), [](const FrequentedBlock& successor) { return successor.isRare(); });
                    for (FrequentedBlock& successor : from->successors()) {
                        bool leavesCycleInside = inner != cycle && !forest.contains(inner, successor.block());
                        if ((!successor.isRare() || everyEdgeIsRare || leavesCycleInside) && forest.contains(cycle, successor.block()) && !likely.quickSet(successor.block()->index()))
                            worklist.append(successor.block());
                    }
                }
            }
            return likely.quickGet(block->index());
        }

        struct NamedTmps {
            BitVector& operator[](Bank bank) { return tmps[bank]; }

            std::array<BitVector, numBanks> tmps;
            std::array<unsigned, numBanks> numberOfTmps { };
        };

        WTF::CycleForest<CFG> forest;
        Vector<Vector<unsigned>> blocks;
        Vector<NamedTmps> namedTmps;
        Vector<BitVector> likelyBlocks;
        BitVector hasLikelyBlocks;
    };

    // In a loop a Tmp competes with Tmps that the loop never names, and they bring the weight of their uses elsewhere.
    // Those go to the stack in the loop and wherever else this one lives, if the moves at the borders run less often than the loads and stores of a spill.
    template<Bank bank>
    bool tryTakeRegisterOfUnusedTmps(Tmp tmp, TmpData& tmpData)
    {
        if (!m_splitPolicy.keepsUnusedTmpsOnStackThroughLoops || isConstDef<bank>(tmp))
            return false;
        CostOfSpill spill = costOfSpill(tmp);
        if (!spill.mostFrequentBlock)
            return false;
        if (!m_cycles)
            m_cycles.emplace(m_code);
        unsigned cycle = m_cycles->forest.innermost(spill.mostFrequentBlock);
        if (cycle == m_cycles->forest.none)
            return false;

        Vector<unsigned, 16> blocks;
        BitVector isAmongBlocks;
        auto findBlocks = [&] {
            if (!blocks.isEmpty())
                return;
            isAmongBlocks.ensureSize(m_code.size());
            for (unsigned blockIndex : m_cycles->blocks[cycle]) {
                isAmongBlocks.quickSet(blockIndex);
                blocks.append(blockIndex);
            }
            size_t numberOfBlocksInCycle = blocks.size();
            forEachBlockInLiveRange(tmpData.liveRange, [&](BasicBlock* block) {
                if (!isAmongBlocks.quickSet(block->index()))
                    blocks.append(block->index());
                return IterationStatus::Continue;
            });
            std::ranges::inplace_merge(blocks, blocks.begin() + numberOfBlocksInCycle);
            ASSERT(std::ranges::is_sorted(blocks));
        };
        auto passesThroughBlockWhereTheyMeet = [&](Tmp other, const Interval& intervalOfOther) {
            TmpData& otherData = m_map.get<bank>(other);
            if (otherData.stage != Stage::Assigned || otherData.spillability != TmpData::Spillability::Spillable || otherData.splitFrom || isConstDef<bank>(other))
                return false;
            auto intervals = tmpData.liveRange.intervals().span();
            auto first = std::ranges::upper_bound(intervals, intervalOfOther.begin(), { }, [](const Interval& interval) {
                return interval.end();
            });
            ASSERT(first != intervals.end() && first->begin() < intervalOfOther.end());
            BasicBlock* block = findBlockContainingPoint(std::max(first->begin(), intervalOfOther.begin()));
            Point head = positionOfHead(block);
            Point tail = positionOfTail(block);
            if (intervalOfOther.begin() > head || intervalOfOther.end() <= tail)
                return false;
            auto useDefs = useDefsOf(other);
            auto next = std::ranges::lower_bound(useDefs, head);
            return next == useDefs.end() || *next > tail;
        };

        const float splitMultiplier = m_splitPolicy.splitMultiplier;
        float leastCost = spill.frequency;
        if (splitMultiplier > 0.0)
            leastCost /= splitMultiplier;
        Reg bestReg;
        Vector<BlocksOnStack, 2> best;
        Width width = widthForConflicts<bank>(tmp);
        m_visited.resize(m_code.numTmps(bank));
        auto candidates = m_occupancy.candidatesFor(tmpData, RegisterOccupancy::FailureMemo::None, RegisterOccupancy::Clobbers::Exclude);
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank]) {
            if (!candidates.contains(reg))
                continue;
            float cost = 0;
            bool isPossible = true;
            Vector<BlocksOnStack, 2> others;
            m_visited.clear();
            m_occupancy.forEachTmpConflict(reg, tmpData.liveRange, width, [&](auto& conflict) -> IterationStatus {
                if (!conflict.tmp.isReg() && !m_cycles->names(m_code, cycle, conflict.tmp)) {
                    unsigned index = conflict.tmp.tmpIndex(bank);
                    if (m_visited.contains(index))
                        return IterationStatus::Continue;
                    m_visited.add(index);
                    if (passesThroughBlockWhereTheyMeet(conflict.tmp, conflict.interval)) {
                        findBlocks();
                        if (auto blocksOnStack = findBlocksToPassOnStack<bank>(conflict.tmp, blocks, isAmongBlocks, leastCost, cost)) {
                            others.append({ conflict.tmp, WTF::move(*blocksOnStack) });
                            return IterationStatus::Continue;
                        }
                    }
                }
                isPossible = false;
                return IterationStatus::Done;
            });
            if (!isPossible || others.isEmpty() || cost >= leastCost)
                continue;
            leastCost = cost;
            bestReg = reg;
            best = WTF::move(others);
        }
        if (!bestReg)
            return false;

        for (BlocksOnStack& other : best) {
            TmpData& otherData = m_map.get<bank>(other.tmp);
            LiveRange rangeOnStack;
            for (unsigned blockIndex : other.blocks)
                rangeOnStack.append(m_intervalArena, Interval(positionOfHead(m_code[blockIndex]), positionOfTail(m_code[blockIndex]) + 1));
            evict(other.tmp, otherData, bestReg);
            otherData.setLiveRange(LiveRange::subtract(m_intervalArena, otherData.liveRange, rangeOnStack));
            otherData.isOnStackInSomeBlocks = true;
            ensureSpillSlotTableEntry(otherData);
            assign(other.tmp, otherData, bestReg);
            dataLogLnIf(verbose(), "On the stack where ", tmp, " lives: ", other.tmp, " in blocks ", listDump(other.blocks));
            m_blocksOnStack.append(WTF::move(other));
        }
        assign(tmp, tmpData, bestReg);
        return true;
    }

    void insertMovesOfTmpsOnStack(BlockInsertionSet& blockInsertionSet)
    {
        if (m_blocksOnStack.isEmpty())
            return;

        struct MoveOnEdge {
            BasicBlock* from;
            BasicBlock* to;
            Inst move;
        };
        Vector<MoveOnEdge, 8> stores;
        Vector<MoveOnEdge, 8> loads;
        std::ranges::stable_sort(m_blocksOnStack, { }, [](const BlocksOnStack& entry) {
            return entry.tmp.internalValue();
        });
        for (size_t first = 0; first < m_blocksOnStack.size();) {
            Tmp tmp = m_blocksOnStack[first].tmp;
            Vector<unsigned, 8> blocks;
            for (; first < m_blocksOnStack.size() && m_blocksOnStack[first].tmp == tmp; ++first)
                blocks.appendVector(m_blocksOnStack[first].blocks);
            TmpData& tmpData = m_map[tmp];
            if (tmpData.stage == Stage::Spilled)
                continue;
            RELEASE_ASSERT(tmpData.stage == Stage::Assigned);
            StackSlot*& slot = m_spillSlotTable[tmpData.spillSlotTableIndex];
            if (!slot)
                slot = m_code.addStackSlot(stackSlotMinimumWidth(m_tmpWidth.requiredWidth(tmp)), StackSlotKind::Spill);
            Opcode move = moveOpcode(tmp);
            std::ranges::sort(blocks);
            auto isOnStackIn = [&](BasicBlock* block) {
                return std::ranges::binary_search(blocks, block->index());
            };
            for (unsigned blockIndex : blocks) {
                BasicBlock* block = m_code[blockIndex];
                for (BasicBlock* predecessor : block->predecessors()) {
                    if (!isOnStackIn(predecessor))
                        stores.append({ predecessor, block, Inst(move, predecessor->last().origin, tmp, Arg::stack(slot)) });
                }
                Vector<BasicBlock*, 2> successors;
                for (BasicBlock* successor : block->successorBlocks()) {
                    if (successors.contains(successor) || isOnStackIn(successor) || !containsPoint(tmpData.liveRange, positionOfHead(successor)))
                        continue;
                    successors.append(successor);
                    loads.append({ block, successor, Inst(move, block->last().origin, Arg::stack(slot), tmp) });
                }
            }
        }

        // Where one Tmp leaves a register to the stack and another comes back into that register, the store is first.
        HashMap<std::pair<unsigned, unsigned>, BasicBlock*> blocksOnEdges;
        auto place = [&](MoveOnEdge& entry, InsertionPhase phaseAtTail, InsertionPhase phaseAtHead) {
            auto [from, to, move] = WTF::move(entry);
            if (endsInLoneJump(from)) {
                insertInst(from, from->size() - 1, phaseAtTail, WTF::move(move));
                return;
            }
            if (to->numPredecessors() == 1) {
                insertInst(to, 0, phaseAtHead, WTF::move(move));
                return;
            }
            BasicBlock* block = blocksOnEdges.ensure({ from->index() + 1, to->index() + 1 }, [&] {
                double frequency = std::min(from->frequency() * shareOfFrequency(from, to), to->frequency());
                BasicBlock* added = to == m_code[0] ? blockInsertionSet.insertAfter(from, frequency) : blockInsertionSet.insertBefore(to, frequency);
                added->append(Jump, move.origin);
                added->setSuccessors(FrequentedBlock(to));
                for (FrequentedBlock& edge : from->successors()) {
                    if (edge.block() == to)
                        edge.block() = added;
                }
                return added;
            }).iterator->value;
            block->insts().insert(block->size() - 1, WTF::move(move));
        };
        for (MoveOnEdge& store : stores)
            place(store, ClobberMoveFrom, SpillMoveTo);
        for (MoveOnEdge& load : loads)
            place(load, AroundLoopEntryFixup, AroundLoopExitFixup);
    }

    bool isLiveIntoBlockEntryClobbers(BasicBlock* block, const LiveRange& liveRange)
    {
        for (BasicBlock* successor : block->successorBlocks()) {
            if (successor->at(0).clobbersAtBlockEntry() && liveRange.contains(positionOfHead(successor)))
                return true;
        }
        return false;
    }

    // What an instruction has read by the time it clobbers a register may stay in that register up to the instruction. The moves around such an
    // instruction take more room than a callee save, so this is for a Tmp that would be spilled otherwise.
    enum class ClobbersThatRead : bool { StopSplit, AreSplitAround };

    float frequencyOfAllInsts()
    {
        if (!m_frequencyOfAllInsts) {
            for (BasicBlock* block : m_code)
                m_frequencyOfAllInsts += adjustedBlockFrequency(block) * block->size();
        }
        return m_frequencyOfAllInsts;
    }

    uint64_t regsClobberedAtCalls(Bank bank)
    {
        RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
        calleeSaves.exclude(m_code.unsavedCalleeSaves());
        uint64_t result = 0;
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank]) {
            if (!calleeSaves.contains(reg, IgnoreVectors))
                result |= 1ULL << reg.index();
        }
        return result & m_occupancy.allClobberedRegs();
    }

    // A spilled Tmp costs an instruction for each stretch of a block, from one call to the next, in which some instruction cannot take it from the
    // stack. A split one costs two at each clobber. The split has to run shorter, and where it is the longer of the two, by a larger factor than
    // the function grows.
    struct CostOfSpill {
        size_t insts { 0 };
        float frequency { 0 };
        // A stretch that reads the Tmp first takes a load, and one that writes it a store.
        float frequencyOfLoadsAndStores { 0 };
        // Asked only once the cycles are known.
        bool costsInLikelyBlockOfCycle { false };
        BasicBlock* mostFrequentBlock { nullptr };
    };

    bool needsRegisterIn(Tmp tmp, Inst& inst)
    {
        if (isLargeInst(inst))
            return false;
        bool result = false;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (arg.isTmp())
                result |= arg.tmp() == tmp && !Arg::isColdUse(role) && !admitsStack(inst, arg);
            else
                result |= arg.usesTmp(tmp);
        });
        return result;
    }

    // A move to or from the stack is a store or a load, where a move between two Tmps may get one register for both and go.
    bool isOnlyReadInPlaceBy(Tmp tmp, Inst& inst)
    {
        if (isLargeInst(inst))
            return true;
        bool isMove = inst.kind.opcode == Move || inst.kind.opcode == Move32 || inst.kind.opcode == MoveFloat || inst.kind.opcode == MoveDouble;
        bool result = true;
        inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
            if (!arg.isTmp())
                result &= !arg.usesTmp(tmp);
            else if (arg.tmp() == tmp && !Arg::isColdUse(role))
                result &= !isMove && !Arg::isAnyDef(role) && admitsStack(inst, arg);
        });
        return result;
    }

    bool isInCycle(BasicBlock* block, unsigned cycle)
    {
        for (unsigned inner = m_cycles->forest.innermost(block); inner != m_cycles->forest.none; inner = m_cycles->forest[inner].parent) {
            if (inner == cycle)
                return true;
        }
        return false;
    }

    // With a cycle: what the blocks of that cycle alone cost.
    CostOfSpill costOfSpill(Tmp tmp, std::optional<unsigned> cycle = std::nullopt)
    {
        ensureUseDefLists();
        uint64_t clobberedAtCalls = regsClobberedAtCalls(tmp.bank());
        CostOfSpill result;
        BasicBlock* previousBlock = nullptr;
        Point previousPoint = 0;
        bool isStoredInStretch = false;
        for (Point point : useDefsOf(tmp)) {
            BasicBlock* block = findBlockContainingPoint(point);
            if (cycle && !isInCycle(block, *cycle))
                continue;
            Inst& inst = block->at(instIndex(positionOfHead(block), point));
            if (m_cycles && !result.costsInLikelyBlockOfCycle && m_cycles->isLikelyInItsCycle(m_code, block))
                result.costsInLikelyBlockOfCycle = !isOnlyReadInPlaceBy(tmp, inst);
            if (!needsRegisterIn(tmp, inst))
                continue;
            bool isInSameStretch = block == previousBlock && (!clobberedAtCalls || !m_occupancy.hasPointClobberingAll(clobberedAtCalls, Interval(previousPoint, point)));
            previousBlock = block;
            previousPoint = point;
            bool reads = false;
            bool writes = false;
            inst.forEachArg([&](Arg& arg, Arg::Role role, Bank, Width) {
                if (!arg.isTmp())
                    reads |= arg.usesTmp(tmp);
                else if (arg.tmp() == tmp && !Arg::isColdUse(role) && !admitsStack(inst, arg)) {
                    reads |= Arg::isAnyUse(role);
                    writes |= Arg::isAnyDef(role);
                }
            });
            if (!isInSameStretch) {
                isStoredInStretch = false;
                if (reads)
                    result.frequencyOfLoadsAndStores += adjustedBlockFrequency(block);
            }
            if (writes && !std::exchange(isStoredInStretch, true))
                result.frequencyOfLoadsAndStores += adjustedBlockFrequency(block);
            if (isInSameStretch)
                continue;
            ++result.insts;
            result.frequency += adjustedBlockFrequency(block);
            if (!result.mostFrequentBlock || block->frequency() > result.mostFrequentBlock->frequency())
                result.mostFrequentBlock = block;
        }
        return result;
    }

    // The lists of uses and definitions of the Tmps that the allocator was handed do not change, so each is asked once.
    template<Bank bank>
    CostOfSpill knownCostOfSpill(Tmp tmp)
    {
        Vector<std::optional<CostOfSpill>>& known = m_costsOfSpill[bank];
        unsigned index = tmp.tmpIndex(bank);
        if (index >= known.size()) {
            if (!known.isEmpty())
                return costOfSpill(tmp);
            known.grow(m_code.numTmps(bank));
        }
        if (!known[index])
            known[index] = costOfSpill(tmp);
        return *known[index];
    }

    float frequencyOfLoadsAndStoresIfSpilledInCycle(Tmp tmp, unsigned cycle)
    {
        return m_frequencyIfSpilledInCycle.ensure({ static_cast<unsigned>(tmp.internalValue()), cycle + 1 }, [&] {
            return costOfSpill(tmp, cycle).frequencyOfLoadsAndStores;
        }).iterator->value;
    }

    // Who is evicted goes by a weight that counts every use. An instruction that reads its operand from the stack in place needs no load, so a Tmp that the likely blocks of every loop
    // read only that way costs a turn nothing: a bound that the counter is compared with. It leaves its register to a Tmp of a loop whose loads and stores would run more often than its own,
    // which all stand outside loops or behind rare edges. A loop that is entered rarely weighs little beside what stands around it, however many turns it takes, so the one that leaves
    // has to be the cheaper one in the whole function, in the loop of the one that comes, and in its own.
    template<Bank bank>
    bool tryTakeRegisterOfTmpsCheaperOnStack(Tmp tmp, TmpData& tmpData)
    {
        if (!m_splitPolicy.spillsTmpsReadInPlaceFirst || isConstDef<bank>(tmp))
            return false;
        CostOfSpill ifSpilled = costOfSpill(tmp);
        if (!ifSpilled.mostFrequentBlock)
            return false;
        if (!m_cycles)
            m_cycles.emplace(m_code);
        unsigned cycle = m_cycles->forest.innermost(ifSpilled.mostFrequentBlock);
        if (cycle == m_cycles->forest.none)
            return false;
        float leastCostInCycle = costOfSpill(tmp, cycle).frequencyOfLoadsAndStores;
        Reg bestReg;
        Width width = widthForConflicts<bank>(tmp);
        m_visited.resize(m_code.numTmps(bank));
        auto candidates = m_occupancy.candidatesFor(tmpData, RegisterOccupancy::FailureMemo::None, RegisterOccupancy::Clobbers::Exclude);
        for (Reg reg : m_allowedRegistersInPriorityOrder[bank]) {
            if (!candidates.contains(reg))
                continue;
            float cost = 0;
            float costInCycle = 0;
            bool isPossible = true;
            m_visited.clear();
            m_occupancy.forEachTmpConflict(reg, tmpData.liveRange, width, [&](auto& conflict) -> IterationStatus {
                if (!conflict.tmp.isReg() && m_visited.contains(conflict.tmp.tmpIndex(bank)))
                    return IterationStatus::Continue;
                isPossible = false;
                if (conflict.tmp.isReg())
                    return IterationStatus::Done;
                m_visited.add(conflict.tmp.tmpIndex(bank));
                TmpData& otherData = m_map.get<bank>(conflict.tmp);
                if (otherData.stage != Stage::Assigned || otherData.spillability != TmpData::Spillability::Spillable || otherData.splitFrom || otherData.isOnStackInSomeBlocks || isConstDef<bank>(conflict.tmp))
                    return IterationStatus::Done;
                CostOfSpill otherIfSpilled = knownCostOfSpill<bank>(conflict.tmp);
                if (otherIfSpilled.costsInLikelyBlockOfCycle)
                    return IterationStatus::Done;
                cost += otherIfSpilled.frequencyOfLoadsAndStores;
                if (cost >= ifSpilled.frequencyOfLoadsAndStores)
                    return IterationStatus::Done;
                costInCycle += frequencyOfLoadsAndStoresIfSpilledInCycle(conflict.tmp, cycle);
                if (costInCycle >= leastCostInCycle)
                    return IterationStatus::Done;
                unsigned cycleOfOther = otherIfSpilled.mostFrequentBlock ? m_cycles->forest.innermost(otherIfSpilled.mostFrequentBlock) : cycle;
                if (cycleOfOther != cycle && cycleOfOther != m_cycles->forest.none && frequencyOfLoadsAndStoresIfSpilledInCycle(conflict.tmp, cycleOfOther) >= frequencyOfLoadsAndStoresIfSpilledInCycle(tmp, cycleOfOther))
                    return IterationStatus::Done;
                isPossible = true;
                return IterationStatus::Continue;
            });
            if (!isPossible)
                continue;
            leastCostInCycle = costInCycle;
            bestReg = reg;
        }
        if (!bestReg)
            return false;

        Vector<Tmp, 2> others;
        m_occupancy.forEachTmpConflict(bestReg, tmpData.liveRange, width, [&](auto& conflict) -> IterationStatus {
            if (!others.contains(conflict.tmp))
                others.append(conflict.tmp);
            return IterationStatus::Continue;
        });
        for (Tmp other : others)
            evict(other, m_map.get<bank>(other), bestReg);
        assign(tmp, tmpData, bestReg);
        for (Tmp other : others) {
            TmpData& otherData = m_map.get<bank>(other);
            dataLogLnIf(verbose(), "Cheaper on the stack than ", tmp, ": ", other);
            if (!tryAllocate<bank>(other, otherData))
                spill(other, otherData);
        }
        return true;
    }

    bool splitIsWorthItsSize(Tmp tmp, UseDefCost splitCost, size_t numberOfSites)
    {
        CostOfSpill ifSpilled = costOfSpill(tmp);
        size_t instsIfSpilled = ifSpilled.insts;
        float frequencyIfSpilled = ifSpilled.frequency;
        if (frequencyIfSpilled <= splitCost.value())
            return false;
        size_t instsIfSplit = 2 * numberOfSites;
        if (instsIfSplit <= instsIfSpilled)
            return true;
        float shorterRun = (frequencyIfSpilled - splitCost.value()) / frequencyOfAllInsts();
        float growth = static_cast<float>(instsIfSplit - instsIfSpilled) / m_mentions.numberOfInsts;
        return shorterRun > growth;
    }

    template<Bank bank>
    bool trySplitAroundClobbers(Tmp tmp, TmpData& tmpData, bool mayEvict = false, ClobbersThatRead clobbersThatRead = ClobbersThatRead::StopSplit)
    {
        if (tmpData.splitAroundClobbersMetadataIndex)
            return false; // Already split around clobbers
        if (tmpData.spansStretch)
            return false;
        if (tmpData.liveRange.size() < splitMinRangeSize)
            return false; // Not enough instructions to be worthwhile
        // Each site gets a value of its own, with two moves. At one site no more values are held in registers than there are registers, so more than that many for each site
        // are of no use: they are what is left of values that were split and then evicted, and there may be as many of those as there are values.
        const float leastCostOfSite = m_splitPolicy.leastCostOfSplitAroundClobber;
        size_t sitesLeft = std::numeric_limits<size_t>::max();
        if (leastCostOfSite > 0 && m_occupancy.numberOfClobberPoints()) {
            size_t mostGapTmps = m_allowedRegistersInPriorityOrder[bank].size() * m_occupancy.numberOfClobberPoints();
            if (m_numberOfGapTmps[bank] >= mostGapTmps)
                return false;
            sitesLeft = mostGapTmps - m_numberOfGapTmps[bank];
        }

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
            if (usesOrDefsTmp && m_splitPolicy.splitsAroundClobbersThatRead && pointAtOffset(point, PointOffsets::Late) == point) {
                bool isOnlyReadEarly = true;
                if (isLargeInst(inst))
                    ensureUseDefLists();
                forEachMention(tmp, pointAtOffset(point, PointOffsets::Pre), inst, [&](Tmp&, Arg::Role role) {
                    isOnlyReadEarly &= !Arg::isAnyDef(role) && !Arg::isLateUse(role);
                });
                if (isOnlyReadEarly) {
                    tmpData.isOnlyReadAtSomeClobber = true;
                    usesOrDefsTmp = clobbersThatRead == ClobbersThatRead::StopSplit;
                }
            }
            usesOrDefsTmp |= inst.clobbersAtBlockEntry();
            usesOrDefsTmp |= inst.isTerminal() && isLiveIntoBlockEntryClobbers(block, tmpData.liveRange);
            ClobberSite site { point, adjustedBlockFrequency(block) * 2, usesOrDefsTmp };
            clobberSites.insert(nextClobberSite, site);
            return site;
        };
        ASSERT(tmpData.spillCost() != unspillableCost); // Should have evicted.
        const bool spansStretches = m_splitPolicy.spansStretchesOfClobbers;

        Reg bestSplitReg;
        constexpr UseDefCost invalidSplitCost(std::numeric_limits<float>::infinity());
        const float splitMultiplier = m_splitPolicy.splitMultiplier;
        UseDefCost splitCostLimit = splitMultiplier > 0.0 ? tmpData.useDefCost / splitMultiplier : invalidSplitCost;
        if (m_occupancy.numberOfFreshCalleeSaves(bank) && !tmpData.paysForCalleeSave)
            splitCostLimit = std::min(splitCostLimit, m_calleeSaveCost);
        UseDefCost minSplitCost = splitCostLimit;
        Reg bestSplitRegWithEviction;
        UseDefCost minSplitCostWithEviction = splitCostLimit;
        size_t numberOfSitesOfBest = 0;
        size_t numberOfSitesOfBestWithEviction = 0;
        Width width = widthForConflicts<bank>(tmp);
        auto candidates = m_occupancy.candidatesFor(tmpData, RegisterOccupancy::FailureMemo::None, RegisterOccupancy::Clobbers::Ignore);
        for (Reg r : m_allowedRegistersInPriorityOrder[bank]) {
            if (!candidates.contains(r))
                continue;
            // A register with too many sites for what each costs at least is found by counting. Walking them would take the sites of the range for each value and register.
            if (leastCostOfSite > 0 && m_occupancy.numberOfClobberPoints() >= AirLimits::minClobberPointsToCountPerRegister) {
                double mostSites = std::max(minSplitCost, minSplitCostWithEviction).value() / leastCostOfSite;
                size_t tooManySites = mostSites < static_cast<double>(sitesLeft) ? std::min(static_cast<size_t>(mostSites * 1.01) + 2 + AirLimits::clobberSitesWithoutLeastCost, sitesLeft + 1) : sitesLeft + 1;
                if (m_occupancy.numberOfDeadClobbers(r, tmpData.liveRange, tooManySites) >= tooManySites)
                    continue;
            }
            UseDefCost splitCost(0);
            size_t numberOfSites = 0;
            nextClobberSite = 0;
            SpillCost evictionCost(0);
            bool evicts = false;
            if (mayEvict) {
                m_visited.resize(m_code.numTmps(bank));
                m_visited.clear();
            }
            m_occupancy.forEachConflict(r, tmpData.liveRange, width,
                [&](auto& conflict) -> IterationStatus {
                    if (conflict.tmp.isReg() && conflict.interval.distance() == 1) {
                        ClobberSite site = clobberSiteAt(conflict.interval.begin());
                        if (site.usesOrDefsTmp || ++numberOfSites > sitesLeft) {
                            // If the inst that clobbers regs also use/def the tmp trying to be split, then
                            // can't split the tmp around this clobber.
                            // FIXME: could allow uses, but then we'd have to make split tmp conflict with any
                            // spill tmps used by this instruction, so unclear if that's better.
                            splitCost = invalidSplitCost;
                            return IterationStatus::Done;
                        }
                        // Times 2 for 'MOV tmp, gapTmp' and 'MOV gapTmp, tmp'
                        splitCost += UseDefCost(numberOfSites > AirLimits::clobberSitesWithoutLeastCost ? std::max(site.cost, leastCostOfSite) : site.cost);
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
                    numberOfSitesOfBestWithEviction = numberOfSites;
                }
                continue;
            }
            if (splitCost < minSplitCost) {
                minSplitCost = splitCost;
                bestSplitReg = r;
                numberOfSitesOfBest = numberOfSites;
            }
        }
        auto holeAround = [&](Interval conflict) {
            return conflict | lateInterval(positionOfEarly(conflict)) | lateInterval(positionOfEarly(Interval(conflict.end() - 1)));
        };
        if (spansStretches && minSplitCost < splitCostLimit) {
            for (Reg r : { tmpData.preferredReg, tmpData.secondPreferredReg }) {
                if (!r || r == bestSplitReg || !candidates.contains(r) || minSplitCost >= UseDefCost(tmpData.wishFor(r)))
                    continue;
                UseDefCost splitCost(0);
                size_t numberOfSites = 0;
                unsigned numberOfMovesKept = 0;
                bool isHeld = false;
                size_t liveIndex = 0;
                auto liveIntervals = tmpData.liveRange.intervals();
                nextClobberSite = 0;
                m_occupancy.forEachConflict(r, tmpData.liveRange, width,
                    [&](auto& conflict) -> IterationStatus {
                        bool stopsSplit = !conflict.tmp.isReg() || ++numberOfSites > numberOfSitesOfBest;
                        float cost = 0;
                        if (!stopsSplit && conflict.interval.distance() == 1) {
                            ClobberSite site = clobberSiteAt(conflict.interval.begin());
                            stopsSplit = site.usesOrDefsTmp;
                            cost = site.cost;
                        } else if (!stopsSplit) {
                            isHeld = true;
                            Interval hole = holeAround(conflict.interval);
                            BasicBlock* block = findBlockContainingPoint(hole.begin());
                            while (liveIndex < liveIntervals.size() && liveIntervals[liveIndex].end() <= hole.begin())
                                ++liveIndex;
                            stopsSplit = hole.end() - 1 > positionOfTail(block) || liveIndex == liveIntervals.size()
                                || liveIntervals[liveIndex].begin() > pointAtOffset(hole.begin(), PointOffsets::Pre)
                                || liveIntervals[liveIndex].end() <= pointAtOffset(hole.end() - 1, PointOffsets::Post);
                            if (!stopsSplit) {
                                Point head = positionOfHead(block);
                                size_t first = this->instIndex(head, hole.begin());
                                size_t last = this->instIndex(head, hole.end() - 1);
                                stopsSplit = block->at(last).isTerminal();
                                for (size_t index = first; index <= last; ++index) {
                                    Inst& inst = block->at(index);
                                    inst.forEachTmpFast([&](Tmp useOrDef) {
                                        stopsSplit |= useOrDef == tmp;
                                    });
                                    stopsSplit |= inst.clobbersAtBlockEntry();
                                }
                                Inst& move = block->at(first);
                                if (mayBeCoalescable(move) && move.args()[1].tmp() == Tmp(r) && !move.args()[0].tmp().isReg()) {
                                    const TmpData& source = m_map[move.args()[0].tmp()];
                                    numberOfMovesKept += !source.assigned && source.preferredReg == r;
                                }
                                cost = adjustedBlockFrequency(block) * 2;
                            }
                        }
                        splitCost += UseDefCost(numberOfSites > AirLimits::clobberSitesWithoutLeastCost ? std::max(cost, leastCostOfSite) : cost);
                        if (stopsSplit || numberOfMovesKept > 1 || minSplitCost < splitCost) {
                            splitCost = invalidSplitCost;
                            return IterationStatus::Done;
                        }
                        return IterationStatus::Continue;
                    });
                if (!isHeld || minSplitCost < splitCost)
                    continue;
                minSplitCost = splitCost;
                bestSplitReg = r;
                numberOfSitesOfBest = numberOfSites;
            }
        }
        if (minSplitCost >= splitCostLimit && bestSplitRegWithEviction) {
            minSplitCost = minSplitCostWithEviction;
            bestSplitReg = bestSplitRegWithEviction;
            numberOfSitesOfBest = numberOfSitesOfBestWithEviction;
        }
        if (minSplitCost >= splitCostLimit)
            return false; // No register found or better to spill than to split
        ASSERT(bestSplitReg);
        if (clobbersThatRead == ClobbersThatRead::AreSplitAround && !splitIsWorthItsSize(tmp, minSplitCost, numberOfSitesOfBest))
            return false;

        LiveRange holeRange;
        Interval lastHole;
        m_occupancy.forEachConflict(bestSplitReg, tmpData.liveRange, width,
            [&](auto& conflict) -> IterationStatus {
                if (!conflict.tmp.isReg())
                    return IterationStatus::Continue;
                ASSERT(spansStretches || conflict.interval.distance() == 1);
                // Punched hole should always include the instructions late interval so the
                // split tmp won't be modeled as conflicting with late defs.
                Interval hole = holeAround(conflict.interval);
                ASSERT(spansStretches || hole.distance() == 1 || hole.distance() == 2);
                if (lastHole && hole.begin() <= lastHole.end()) {
                    lastHole |= hole;
                    return IterationStatus::Continue;
                }
                if (lastHole)
                    holeRange.append(m_intervalArena, lastHole);
                lastHole = hole;
                return IterationStatus::Continue;
            });
        if (lastHole)
            holeRange.append(m_intervalArena, lastHole);
        if (spansStretches && holeRange.intervals().size() > 1) {
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
                size_t end = clobbersThatRead == ClobbersThatRead::AreSplitAround ? site + 1 : site;
                for (size_t index = instIndex(head, stretch.end() - 1) + 1; index < end; ++index) {
                    bool mentionsTmp = false;
                    block->at(index).forEachTmpFast([&](Tmp& named) {
                        mentionsTmp |= named == tmp;
                    });
                    if (mentionsTmp)
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

        tmpData.setLiveRange(LiveRange::subtract(m_intervalArena, tmpData.liveRange, holeRange));
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
            Interval gapInterval = hole | Interval(hole.begin() - 1);
            if (spansStretches)
                gapInterval = Interval(pointAtOffset(hole.begin(), PointOffsets::Pre), pointAtOffset(hole.end() - 1, PointOffsets::Post) + 1);
            Tmp gapTmp = addSplitTmp(tmp, UseDefCost(freq), gapInterval);
            metadata.splits.append({ gapTmp });
            ++m_numberOfGapTmps[bank];
            m_map.get<bank>(gapTmp).spansStretch = hole.distance() > 2;
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
        const unsigned* instFirstMention = m_mentions.instFirstMention.span().data();
        const unsigned numberOfInsts = m_mentions.numberOfInsts;
        const unsigned numberOfMentions = instFirstMention[numberOfInsts];
        m_gpUseDefListsEnd = Tmp::absoluteIndexEnd(m_code, GP);
        const unsigned gpEnd = m_gpUseDefListsEnd;
        const unsigned numberOfLists = gpEnd + Tmp::absoluteIndexEnd(m_code, FP);
        auto listOf = [&](unsigned value) ALWAYS_INLINE_LAMBDA {
            int signedValue = static_cast<int>(value);
            return signedValue > 0 ? value : gpEnd - signedValue;
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
        m_useDefsEnd.grow(numberOfLists);
        unsigned* useDefsEnd = m_useDefsEnd.mutableSpan().data();
        for (unsigned list = 0; list < numberOfLists; ++list)
            useDefsEnd[list] = firstUseDef[list];
        UseDefListsBeingFilled lists { tmps, instFirstMention, firstUseDef, useDefsEnd, useDefs, gpEnd };
        if (m_largeInsts.isEmpty()) [[likely]] {
            lists.addInsts(0, numberOfInsts);
            return;
        }
        addInstsAndMentionsInLargeInsts(lists, numberOfInsts);
    }

    struct UseDefListsBeingFilled {
        ALWAYS_INLINE unsigned listOf(unsigned internalValue) const
        {
            int signedValue = static_cast<int>(internalValue);
            return signedValue > 0 ? internalValue : gpEnd - signedValue;
        }

        ALWAYS_INLINE void addInsts(unsigned ordinal, unsigned ordinalsEnd) const
        {
            for (; ordinal < ordinalsEnd; ++ordinal) {
                Point instPoint = ordinal * PointOffsets::PointsPerInst;
                for (unsigned i = instFirstMention[ordinal]; i < instFirstMention[ordinal + 1]; ++i) {
                    if (!tmps[i] || Tmp::tmpForInternalValue(tmps[i]).isReg())
                        continue;
                    unsigned list = listOf(tmps[i]);
                    unsigned end = useDefsEnd[list];
                    if (end != firstUseDef[list] && useDefs[end - 1] == instPoint)
                        continue;
                    useDefs[end] = instPoint;
                    useDefsEnd[list] = end + 1;
                }
            }
        }

        const unsigned* tmps;
        const unsigned* instFirstMention;
        const unsigned* firstUseDef;
        unsigned* useDefsEnd;
        Point* useDefs;
        unsigned gpEnd;
    };

    NEVER_INLINE void addInstsAndMentionsInLargeInsts(UseDefListsBeingFilled lists, unsigned numberOfInsts)
    {
        m_mentionsInLargeInsts.grow(m_useDefs.size());
        MentionInLargeInst* mentionsInLargeInsts = m_mentionsInLargeInsts.mutableSpan().data();
        unsigned ordinal = 0;
        for (unsigned largeInst : m_largeInsts) {
            lists.addInsts(ordinal, largeInst);
            Point instPoint = largeInst * PointOffsets::PointsPerInst;
            Inst& inst = instAtOrdinal(largeInst);
            inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                if (tmp.isReg())
                    return;
                MentionInLargeInst mention(inst, tmp, role);
                unsigned list = lists.listOf(tmp.internalValue());
                unsigned end = lists.useDefsEnd[list];
                if (end != lists.firstUseDef[list] && lists.useDefs[end - 1] == instPoint) {
                    mentionsInLargeInsts[end - 1].isFollowedByOthers = true;
                    m_furtherMentionsInLargeInsts.append({ end - 1, mention });
                    return;
                }
                lists.useDefs[end] = instPoint;
                mentionsInLargeInsts[end] = mention;
                lists.useDefsEnd[list] = end + 1;
            });
            ordinal = largeInst + 1;
        }
        lists.addInsts(ordinal, numberOfInsts);
        std::ranges::stable_sort(m_furtherMentionsInLargeInsts, { }, &std::pair<unsigned, MentionInLargeInst>::first);
    }

    const MentionInLargeInst* mentionsInLargeInstsOf(Tmp tmp)
    {
        if (m_mentionsInLargeInsts.isEmpty()) [[likely]]
            return nullptr;
        unsigned list = tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : m_gpUseDefListsEnd + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
        if (tmp.isGP() ? list >= m_gpUseDefListsEnd : list >= m_useDefsEnd.size())
            return nullptr;
        return m_mentionsInLargeInsts.span().data() + m_firstUseDef[list];
    }

    static bool isLargeInst(const Inst& inst) { return inst.args().size() > maxArgsWalkedToFindTmp && inst.kind.opcode == Patch; }

    ALWAYS_INLINE void forEachMention(Tmp tmp, Point instPoint, Inst& inst, NOESCAPE const Invocable<void(Tmp&, Arg::Role)> auto& func)
    {
        if (isLargeInst(inst) && forEachMentionInLargeInst(tmp, instPoint, inst, func)) [[unlikely]]
            return;
        inst.forEachTmp([&](Tmp& mentioned, Arg::Role role, Bank, Width) ALWAYS_INLINE_LAMBDA {
            if (mentioned == tmp) [[unlikely]]
                func(mentioned, role);
        });
    }

    NEVER_INLINE bool forEachMentionInLargeInst(Tmp tmp, Point instPoint, Inst& inst, NOESCAPE const Invocable<void(Tmp&, Arg::Role)> auto& func)
    {
        Tmp listed = m_map[tmp].splitFrom ? m_map[tmp].splitFrom : tmp;
        unsigned list = listed.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(listed) : m_gpUseDefListsEnd + AbsoluteTmpMapper<FP>::absoluteIndex(listed);
        if (m_mentionsInLargeInsts.isEmpty() || (listed.isGP() ? list >= m_gpUseDefListsEnd : list >= m_useDefsEnd.size()))
            return false;
        auto useDefs = m_useDefs.span().subspan(m_firstUseDef[list], m_useDefsEnd[list] - m_firstUseDef[list]);
        auto useDef = std::ranges::lower_bound(useDefs, instPoint);
        if (useDef == useDefs.end() || *useDef != instPoint)
            return false;
        unsigned index = m_firstUseDef[list] + (useDef - useDefs.begin());
        if (!m_mentionsInLargeInsts[index].isKnown)
            return false;
        auto visit = [&](MentionInLargeInst mention) ALWAYS_INLINE_LAMBDA {
            Tmp* mentioned = mention.tmpIn(inst);
            if (mentioned && *mentioned == tmp)
                func(*mentioned, mention.role());
        };
        visit(m_mentionsInLargeInsts[index]);
        if (!m_mentionsInLargeInsts[index].isFollowedByOthers)
            return true;
        auto further = std::ranges::lower_bound(m_furtherMentionsInLargeInsts, index, { }, &std::pair<unsigned, MentionInLargeInst>::first);
        for (; further != m_furtherMentionsInLargeInsts.end() && further->first == index; ++further)
            visit(further->second);
        return true;
    }

    std::span<const Point> useDefsOf(Tmp tmp)
    {
        ASSERT(m_hasUseDefLists && !tmp.isReg());
        unsigned list = tmp.isGP() ? AbsoluteTmpMapper<GP>::absoluteIndex(tmp) : m_gpUseDefListsEnd + AbsoluteTmpMapper<FP>::absoluteIndex(tmp);
        if ((tmp.isGP() && list >= m_gpUseDefListsEnd) || list >= m_useDefsEnd.size()) [[unlikely]] {
            auto iterator = m_newTmpUseDefLists.find(tmp);
            if (iterator == m_newTmpUseDefLists.end())
                return { };
            return iterator->value.useDefs().span();
        }
        return m_useDefs.span().subspan(m_firstUseDef[list], m_useDefsEnd[list] - m_firstUseDef[list]);
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

    enum class ClusterReach : bool { WithinBlock, OverBlocks };

    struct ClusterGroup {
        BasicBlock* block { nullptr };
        Interval cluster;
        Point firstDefPoint { 0 };
        Point lastDefPoint { 0 };
        unsigned ptrsEnd { 0 };
        unsigned coldUsePtrsEnd { 0 };
        unsigned pointsEnd { 0 };
        unsigned root { 0 };
        unsigned numberOfPtrsInCluster { 0 };
        unsigned numberOfGroupsInCluster { 1 };
        float weight { 0 };
        bool isLiveIn { false };
        Tmp clusterTmp;
    };
    using ClusterGroups = Vector<ClusterGroup, 16>;
    using ClusterPieces = Vector<Vector<Interval, 4>, 16>;

    struct ClusterMentions {
        Vector<Tmp*, 32> ptrs;
        Vector<std::pair<Tmp*, Point>, 8> coldUsePtrs;
        Vector<Point, 32> points;
    };

    template<Bank bank>
    ClusterGroups collectClusterGroups(Tmp tmp, ClusterMentions& mentions)
    {
        ClusterGroups groups;
        size_t cursor = 0;
        auto useDefs = useDefsOf(tmp);
        const MentionInLargeInst* mentionsInLargeInsts = mentionsInLargeInstsOf(tmp);
        auto skipBlockWithOnlyColdUses = [&](Interval& remaining) ALWAYS_INLINE_LAMBDA {
            Point start = pointAtOffset(remaining.begin(), PointOffsets::Pre);
            size_t first = cursor;
            while (first < useDefs.size() && useDefs[first] < start)
                ++first;
            if (first == useDefs.size() || remaining.end() <= useDefs[first] || !mentionsInLargeInsts[first].isOnlyColdUse())
                return false;
            Point last = std::min(positionOfTail(findBlockContainingPoint(useDefs[first])), remaining.end() - 1);
            size_t next = first + 1;
            while (next < useDefs.size() && useDefs[next] <= last && mentionsInLargeInsts[next].isOnlyColdUse())
                ++next;
            if (next < useDefs.size() && useDefs[next] <= last)
                return false;
            cursor = next - 1;
            remaining = { last + 1, remaining.end() };
            return true;
        };
        for (Interval interval : m_map.get<bank>(tmp).liveRange.intervals()) {
            Interval remaining = interval;
            do {
                if (mentionsInLargeInsts && skipBlockWithOnlyColdUses(remaining)) [[unlikely]]
                    continue;
                ClusterGroup group;
                remaining = forEachUseDefWithin(tmp, remaining, cursor, [&](Point point, Inst& inst, BasicBlock& block) {
                    auto noteMention = [&](Tmp& t, Arg::Role role) NEVER_INLINE {
                        Point early = point + PointOffsets::Early;
                        Interval timingInterval = intervalForTiming(early, Arg::timing(role));
                        if (Arg::isColdUse(role)) {
                            mentions.coldUsePtrs.append({ &t, timingInterval.begin() });
                            return;
                        }
                        mentions.ptrs.append(&t);
                        mentions.points.append(point);
                        if (Arg::isAnyDef(role)) {
                            if (!group.firstDefPoint)
                                group.firstDefPoint = early;
                            group.lastDefPoint = early; // Remember where the fixup store to spill is needed
                        }
                        group.cluster |= timingInterval;
                        group.block = &block;
                    };
                    forEachMention(tmp, point, inst, noteMention);
                });
                if (!group.block) {
                    mentions.coldUsePtrs.shrink(groups.isEmpty() ? 0 : groups.last().coldUsePtrsEnd);
                    continue;
                }
                group.isLiveIn = interval.contains(pointAtOffset(group.cluster.begin(), PointOffsets::Pre));
                group.ptrsEnd = mentions.ptrs.size();
                group.coldUsePtrsEnd = mentions.coldUsePtrs.size();
                group.pointsEnd = mentions.points.size();
                group.root = groups.size();
                group.numberOfPtrsInCluster = mentions.ptrs.size() - (groups.isEmpty() ? 0 : groups.last().ptrsEnd);
                group.weight = group.numberOfPtrsInCluster * adjustedBlockFrequency(group.block);
                groups.append(group);
            } while (remaining);
        }
        return groups;
    }

    template<Bank bank>
    void joinClusterGroups(Tmp tmp, ClusterGroups& groups, ClusterPieces& pieces)
    {
        uint64_t regsClobberedAtCalls = this->regsClobberedAtCalls(bank);
        if (!regsClobberedAtCalls)
            return;
        auto keepsSomeRegisterWithin = [&](Interval interval) {
            return !m_occupancy.hasPointClobberingAll(regsClobberedAtCalls, interval);
        };
        auto isLiveThroughout = [&](Interval interval) {
            auto intervals = m_map.get<bank>(tmp).liveRange.intervals();
            auto live = std::ranges::upper_bound(intervals, interval.begin(), { }, [](Interval candidate) {
                return candidate.end();
            });
            return live != intervals.end() && live->begin() <= interval.begin() && interval.end() <= live->end();
        };
        for (unsigned i = 0; i < groups.size(); ++i) {
            ClusterGroup& group = groups[i];
            if (group.lastDefPoint || !group.isLiveIn || (i && groups[i - 1].block == group.block))
                continue;
            Vector<Interval, AirLimits::maxBlocksOnClusterPaths + 1> pathIntervals;
            pathIntervals.append(Interval(positionOfHead(group.block), group.cluster.end()));
            Vector<BasicBlock*, AirLimits::maxBlocksOnClusterPaths + 1> seen { group.block };
            auto earlier = groups.span().first(i);
            unsigned pathsRoot = i;
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
                    unsigned candidate = std::ranges::upper_bound(earlier, predecessor->index(), { }, [](const ClusterGroup& other) {
                        return other.block->index();
                    }) - earlier.begin();
                    if (candidate && groups[candidate - 1].block == predecessor) {
                        const ClusterGroup& above = groups[candidate - 1];
                        if ((pathsRoot != i && pathsRoot != above.root) || !keepsSomeRegisterWithin(above.cluster)) {
                            isOpen = false;
                            break;
                        }
                        pathsRoot = above.root;
                        pathIntervals.append(Interval(above.cluster.end() - 1, positionOfTail(predecessor) + 1));
                        continue;
                    }
                    if (seen.size() > AirLimits::maxBlocksOnClusterPaths) {
                        isOpen = false;
                        break;
                    }
                    pathIntervals.append(Interval(positionOfHead(predecessor), positionOfTail(predecessor) + 1));
                    seen.append(predecessor);
                }
            }
            if (!isOpen || pathsRoot == i)
                continue;
            for (Interval interval : pathIntervals)
                isOpen &= isLiveThroughout(interval) && keepsSomeRegisterWithin(interval);
            if (!isOpen)
                continue;
            ClusterGroup& root = groups[pathsRoot];
            // A lone read that takes the value from the stack needs no load. One in front of it for the sake of a block that runs less often would cost more than it saves.
            if (m_splitPolicy.keepsUnusedTmpsOnStackThroughLoops && root.numberOfPtrsInCluster == 1 && adjustedBlockFrequency(group.block) * m_splitPolicy.splitMultiplier < adjustedBlockFrequency(root.block)
                && !needsRegisterIn(tmp, root.block->at(instIndex(positionOfHead(root.block), root.cluster.begin()))))
                continue;
            group.root = pathsRoot;
            root.numberOfPtrsInCluster += group.numberOfPtrsInCluster;
            root.numberOfGroupsInCluster++;
            root.weight += group.weight;
            pieces[group.root].appendVector(pathIntervals);
        }
    }

    template<Bank bank>
    bool trySplitIntoClusters(Tmp tmp, ClusterReach reach)
    {
        const bool isSecondSplit = m_map.get<bank>(tmp).isClusterOverBlocks;
        ClusterMentions mentions;
        ClusterGroups groups = collectClusterGroups<bank>(tmp, mentions);
        ClusterPieces pieces(groups.size());
        for (unsigned i = 0; i < groups.size(); ++i)
            pieces[i].append(groups[i].cluster);
        if (reach == ClusterReach::OverBlocks)
            joinClusterGroups<bank>(tmp, groups, pieces);

        IntraBlockSplitMetadata* metadata = nullptr;
        for (unsigned i = 0; i < groups.size(); ++i) {
            if (groups[i].root != i || groups[i].numberOfPtrsInCluster < 2)
                continue;
            if (!metadata) {
                m_intraBlockMetadata.constructAndAppend(tmp);
                metadata = &m_intraBlockMetadata.last();
            }
            Interval& ownPiece = pieces[i][0];
            // If the Tmp is live into the cluster then cluster range is extended to model the fixup load from
            // the Tmp's spill slot before the first use. This also allows insertSplitIntraBlockFixupCode()
            // to quickly determine whether the fixup load is needed.
            if (groups[i].isLiveIn)
                ownPiece |= Interval(pointAtOffset(groups[i].cluster.begin(), PointOffsets::Pre));
            // Likewise, if the cluster defs Tmp then a store to Tmp's spill slot will be needed after the final def point.
            if (groups[i].lastDefPoint)
                ownPiece |= Interval(pointAtOffset(groups[i].lastDefPoint, PointOffsets::Post));
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
            for (unsigned k = i ? groups[i - 1].ptrsEnd : 0; k < groups[i].ptrsEnd; ++k)
                *mentions.ptrs[k] = clusterTmp; // Within this cluster, use clusterTmp rather than Tmp
            Vector<Point, 8> coldUsePointsInCluster;
            for (unsigned k = i ? groups[i - 1].coldUsePtrsEnd : 0; k < groups[i].coldUsePtrsEnd; ++k) {
                // Include coldUses only when they are within the warm cluster
                if (clusterData.liveRange.contains(mentions.coldUsePtrs[k].second)) {
                    *mentions.coldUsePtrs[k].first = clusterTmp;
                    clusterData.hasColdUse = true;
                    Point coldUsePoint = pointAtOffset(mentions.coldUsePtrs[k].second, PointOffsets::Pre);
                    if (!m_code.proc().policy().validation.overlooksColdUsesOfClustersOverBlocks) [[likely]]
                        coldUsePointsInCluster.append(coldUsePoint);
                    if (isSecondSplit) [[unlikely]] {
                        // Left on the name of the cluster over blocks, which is spilled, it would read the slot, and this cluster stores to the slot behind its last def alone.
                        // Counted here and not where the registers are known: from here on the instruction mentions this cluster, which the split around clobbers asks about.
                        m_stats[bank].numColdUsesKeptThroughSecondSplit++;
                        m_code.proc().count(Counter::ColdUsesKeptThroughSecondSplit);
                        const ClusterGroup& root = groups[groups[i].root];
                        if (root.firstDefPoint && pointAtOffset(root.firstDefPoint, PointOffsets::Pre) < coldUsePoint && coldUsePoint <= pointAtOffset(root.lastDefPoint, PointOffsets::Pre)) {
                            m_stats[bank].numColdUsesKeptThroughSecondSplitBetweenDefs++;
                            m_code.proc().count(Counter::ColdUsesKeptThroughSecondSplitBetweenDefs);
                        }
                    }
                }
            }
            if (clusterData.isClusterOverBlocks) {
                // This cluster may be split again, and that split finds its mentions through this list. A cold use it
                // missed would keep this cluster's name, and read the spill slot ahead of the store behind the last def.
                UseDefList& list = m_newTmpUseDefLists.ensure(clusterTmp, [] {
                    return UseDefList();
                }).iterator->value;
                unsigned nextColdUse = 0;
                for (unsigned k = i ? groups[i - 1].pointsEnd : 0; k < groups[i].pointsEnd; ++k) {
                    while (nextColdUse < coldUsePointsInCluster.size() && coldUsePointsInCluster[nextColdUse] < mentions.points[k])
                        list.add(coldUsePointsInCluster[nextColdUse++]);
                    list.add(mentions.points[k]);
                }
                while (nextColdUse < coldUsePointsInCluster.size())
                    list.add(coldUsePointsInCluster[nextColdUse++]);
            }
        }
        for (unsigned i = 0; i < groups.size(); ++i) {
            if (Tmp clusterTmp = groups[i].clusterTmp)
                setStageAndEnqueue(clusterTmp, m_map.get<bank>(clusterTmp), Stage::TryAllocate);
        }

        // The original Tmp is spilled, but the cluster Tmps will hopefully carry the value in registers
        // within the cluster intervals.
        spill(tmp, m_map.get<bank>(tmp));
        m_stats[bank].numSplitIntraBlock++;
        dataLogLnIf(verbose(), "Split (intra-block): ", *metadata);
        return true;
    }

    template<Bank bank>
    bool trySplitIntraBlock(Tmp tmp, TmpData& tmpData)
    {
        if (tmpData.splitAroundClobbersMetadataIndex)
            return false;
        if (tmpData.spansStretch)
            return false;

        if (isConstDef<bank>(tmp))
            return false; // Constant will be rematerialized instead

        // Don't split an already intra-block tmp. Otherwise, we might recursively try to
        // split a cluster tmp that couldn't be allocated.
        if (isLiveRangeBlockLocal(tmpData.liveRange))
            return false;

        ensureUseDefLists();
        return trySplitIntoClusters<bank>(tmp, m_splitPolicy.splitsIntoClustersOverBlocks && !tmpData.isClusterOverBlocks && !tmpData.isOnStackInSomeBlocks ? ClusterReach::OverBlocks : ClusterReach::WithinBlock);
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

    class SetOfOrdinals {
    public:
        explicit SetOfOrdinals(unsigned numberOfInsts)
            : m_numberOfInsts(numberOfInsts)
        {
        }

        ALWAYS_INLINE void add(unsigned ordinal)
        {
            if (m_hasBits) {
                m_bits.quickSet(ordinal);
                return;
            }
            m_list.append(ordinal);
            if (m_list.size() >= m_numberOfInsts / 8) [[unlikely]]
                moveListToBits();
        }

        Vector<unsigned> takeInOrder()
        {
            if (m_hasBits) {
                for (size_t ordinal : m_bits)
                    m_list.append(static_cast<unsigned>(ordinal));
                return WTF::move(m_list);
            }
            std::ranges::sort(m_list);
            removeRepeatedElements(m_list);
            return WTF::move(m_list);
        }

    private:
        NEVER_INLINE void moveListToBits()
        {
            m_hasBits = true;
            m_bits.ensureSize(m_numberOfInsts);
            for (unsigned ordinal : m_list)
                m_bits.quickSet(ordinal);
            m_list.shrink(0);
        }

        Vector<unsigned> m_list;
        BitVector m_bits;
        unsigned m_numberOfInsts;
        bool m_hasBits { false };
    };

    template <Bank bank>
    void emitSpillCodeAndEnqueueNewTmps()
    {
        ensureUseDefLists();
        std::ranges::sort(m_newlySpilledTmps, [](Tmp a, Tmp b) {
            return a.tmpIndex(bank) < b.tmpIndex(bank);
        });
        SetOfOrdinals mentioningInsts(m_mentions.numberOfInsts);
        for (Tmp tmp : m_newlySpilledTmps) {
            ASSERT(tmp.bank() == bank);
            TmpData& tmpData = m_map.get<bank>(tmp);
            if (tmpData.stage != Stage::Spilled)
                continue;

            ensureSpillSlotTableEntry(tmpData);
            StackSlot*& slot = m_spillSlotTable[tmpData.spillSlotTableIndex];
            if (!slot) {
                slot = m_code.addStackSlot(stackSlotMinimumWidth(m_tmpWidth.requiredWidth(tmp)), StackSlotKind::Spill);
                m_stats[bank].numSpillStackSlots++;
            }
            ASSERT(spillSlot<bank>(tmp));

            if (!tmpData.splitFrom) {
                for (Point point : useDefsOf(tmp))
                    mentioningInsts.add(point / PointOffsets::PointsPerInst);
                continue;
            }
            auto useDefs = useDefsOf(tmpData.splitFrom);
            auto iterator = useDefs.begin();
            for (Interval interval : tmpData.liveRange.intervals()) {
                iterator = std::lower_bound(iterator, useDefs.end(), pointAtOffset(interval.begin(), PointOffsets::Pre));
                for (; iterator != useDefs.end() && *iterator < interval.end(); ++iterator)
                    mentioningInsts.add(*iterator / PointOffsets::PointsPerInst);
            }
        }
        m_newlySpilledTmps.shrink(0);

        for (unsigned ordinal : mentioningInsts.takeInOrder()) {
            {
                BasicBlock* block = m_code[m_instBlockIndex[ordinal]];
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
                        if (!admitsStack(inst, arg)) {
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
                                ASSERT(inst.isValidForm(m_code.targetFeatures()));
                                arg = imm;
                                if (inst.isValidForm(m_code.targetFeatures())) {
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
                    ASSERT(inst.isValidForm(m_code.targetFeatures()));
                    // WTF::Liveness and Air::LivenessAdapter do not handle a late-def/use followed by early-def
                    // correctly. While this register allocator does handle it correctly (since it models distinct
                    // late and early points between instructions (i.e. intervalForSpill() won't overlap for different
                    // scratch Tmps)), insert a Padding inst so that subsequent liveness analysis correctly computes
                    // lifetime interference when there are back-to-back Move spill-spill-scratch instructions
                    // (scratch is early-def, late-use).
                    // See https://bugs.webkit.org/show_bug.cgi?id=163548#c2 for more info.
                    // FIXME: reconsider this, https://bugs.webkit.org/show_bug.cgi?id=288122
                    insert(block, instIndex, SpillMoveFrom, Padding, inst.origin);
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

                    if (Arg::isAnyUse(role) || (!canKillDef && Arg::isAnyDef(role))) {
                        tmp = addSpillTmpWithInterval(tmp, intervalForSpill(indexOfEarly, role));
                        // For checkPlacesOfValues(): the instruction computes the value of the slot anew into this Tmp. Nothing else reads the index of a Tmp that cannot be spilled.
                        if (m_code.proc().policy().validation.checksSpillSlots && Arg::isAnyDef(role) && role != Arg::Scratch) [[unlikely]]
                            m_map[tmp].spillSlotTableIndex = m_map[originalTmp].spillSlotTableIndex;
                    }

                    if (role == Arg::Scratch)
                        return;

                    Arg arg = Arg::stack(spilled);
                    if (Arg::isAnyUse(role)) {
                        auto tryRematerialize = [&]() {
                            if (!canRematerializeConstant)
                                return false;

                            if constexpr (bank == GP) {
                                int64_t value = m_useCounts.constant<bank>(tmpIndex);
                                if (Arg::isValidImmForm(value) && isValidForm(m_code.targetFeatures(), Move, Arg::Imm, Arg::Tmp)) {
                                    insert(block, instIndex, SpillMoveFrom, Move, inst.origin, Arg::imm(value), tmp);
                                    m_stats[bank].numRematerializeConst++;
                                    dataLogLnIf(verbose(), "Rematerialized (imm) BB", *block, " ", originalTmp, ": ", tmp, " <- ", WTF::RawHex(value));
                                    return true;
                                }
                                RELEASE_ASSERT(isValidForm(m_code.targetFeatures(), Move, Arg::BigImm, Arg::Tmp));
                                insert(block, instIndex, SpillMoveFrom, Move, inst.origin, Arg::bigImm(value), tmp);
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

                                if (imm && isValidForm(m_code.targetFeatures(), constMove, imm.kind(), Arg::Tmp)) {
                                    insert(block, instIndex, SpillMoveFrom, constMove, inst.origin, imm, tmp);
                                    m_stats[bank].numRematerializeConst++;
                                    dataLogLnIf(verbose(), "Rematerialized (FP) BB", *block, " ", originalTmp, ": ", tmp);
                                    return true;
                                }

                                return false;
                            }
                        };

                        if (!tryRematerialize()) {
                            insert(block, instIndex, SpillMoveFrom, move, inst.origin, arg, tmp);
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
                            if (!leavesOut(MoveLeftOutByRegisterAllocator::StoreBehindSpilledDefinition)) [[likely]]
                                insert(block, instIndex + 1, SpillMoveTo, move, inst.origin, tmp, arg);
                            m_stats[bank].numStoreSpill++;
                        }
                    }
                });
                ASSERT(inst.isValidForm(m_code.targetFeatures()));
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

    void validateMovesAroundClobbers()
    {
        auto& moves = m_movesAroundClobbers;
        std::ranges::sort(moves, [](const MoveAroundClobber& a, const MoveAroundClobber& b) {
            return std::tuple(a.blockIndex, a.index, a.isAfterClobber) < std::tuple(b.blockIndex, b.index, b.isAfterClobber);
        });
        size_t failureCount = 0;
        for (size_t begin = 0, end; begin < moves.size(); begin = end) {
            for (end = begin + 1; end < moves.size() && moves[end].blockIndex == moves[begin].blockIndex && moves[end].index == moves[begin].index && moves[end].isAfterClobber == moves[begin].isAfterClobber; ++end) { }
            for (size_t i = begin; i < end; ++i) {
                for (size_t j = begin; j < end; ++j) {
                    if (i == j || !moves[i].source || moves[i].source != moves[j].destination)
                        continue;
                    if (failureCount++ >= maxValidationFailuresListed)
                        continue;
                    dataLogLn("AIR GREEDY REGISTER ALLOCATION VALIDATION FAILURE: MOVES AROUND A CLOBBER");
                    dataLogLn("   In BB#", moves[i].blockIndex, moves[i].isAfterClobber ? " after the clobber," : " before the clobber,", " index ", moves[i].index, ": ", moves[i].source, " -> ", moves[i].destination, " and ", moves[j].source, " -> ", moves[j].destination);
                }
            }
        }
        if (failureCount) {
            reportValidationFailuresNotListed(failureCount);
            dataLogLn("IR:\n", m_code);
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void insertFixupCode()
    {
        // Skip index 0 which is sacrificed so that splitAroundClobbersMetadataIndex == 0 means "none".
        for (size_t i = 1; i < m_aroundClobbersMetadata.size(); i++)
            insertSplitAroundClobbersFixupCode(m_aroundClobbersMetadata[i]);
        validateMovesAroundClobbers();
        for (auto& metadata : m_intraBlockMetadata)
            insertSplitIntraBlockFixupCode(metadata);

        if (m_splitPolicy.splitsAroundLoops) {
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
                insertInst(block, block->size() - 1, AroundLoopEntryFixup, createShuffle(block->at(block->size() - 1).origin, shufflePairs.span()));
            for (auto& [block, shufflePairs] : loopExitFixups)
                insertInst(block, 0, AroundLoopExitFixup, createShuffle(block->at(0).origin, shufflePairs.span()));
        }

        BlockInsertionSet blockInsertionSet(m_code);
        insertMovesOfTmpsOnStack(blockInsertionSet);
        executeInsertions();
        if (blockInsertionSet.execute())
            m_code.resetReachability();
    }

    void insertInst(BasicBlock* block, size_t index, unsigned phase, Inst&& inst)
    {
        m_insertions.append(PhaseInsertion(index, phase, WTF::move(inst)));
        m_insertionBlock.append(block->index());
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
        for (unsigned blockIndex : m_insertionBlock)
            ++endOfBlock[blockIndex];
        unsigned sum = 0;
        for (unsigned& end : endOfBlock)
            sum += std::exchange(end, sum);
        Vector<unsigned> order(m_insertions.size());
        for (unsigned i = 0; i < m_insertions.size(); ++i)
            order[endOfBlock[m_insertionBlock[i]]++] = i;
        PhaseInsertionSet insertionSet;
        unsigned begin = 0;
        for (unsigned blockIndex = 0; blockIndex < endOfBlock.size(); ++blockIndex) {
            unsigned end = endOfBlock[blockIndex];
            if (begin == end)
                continue;
            if (end - begin >= AirLimits::minInsertionsPerBlockToSort) {
                std::stable_sort(order.begin() + begin, order.begin() + end, [&](unsigned left, unsigned right) {
                    return m_insertions[left] < m_insertions[right];
                });
            }
            for (; begin < end; ++begin)
                insertionSet.appendInsertion(WTF::move(m_insertions[order[begin]]));
            insertionSet.execute(m_code[blockIndex]);
        }
        m_insertions.shrink(0);
        m_insertionBlock.shrink(0);
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
                ASSERT(interval.distance() >= 2);
                Point lastPoint = interval.end() - 1;
                BasicBlock* block = findBlockContainingPoint(lastPoint);
                unsigned instIndex = this->instIndex(positionOfHead(block), lastPoint);
                Inst& inst = block->at(instIndex);

                Arg arg = gapTmp;
                StackSlot* spilled = spillSlot(gapTmp);
                if (spilled)
                    arg = Arg::stack(spilled);
                Opcode move = moveOpcode(gapTmp);
                unsigned precedingIndex = this->instIndex(positionOfHead(block), interval.begin());
                if (Options::airValidateGreedRegAlloc()) [[unlikely]] {
                    Reg home = assignedReg(metadata.originalTmp);
                    Reg away = spilled ? Reg() : assignedReg(gapTmp);
                    m_movesAroundClobbers.append({ block->index(), precedingIndex, false, home, away });
                    m_movesAroundClobbers.append({ block->index(), instIndex + 1, true, away, home });
                }
                if (!spilled || !leavesOut(MoveLeftOutByRegisterAllocator::StoreAheadOfClobber)) [[likely]]
                    insert(block, precedingIndex, ClobberMoveFrom, move, inst.origin, metadata.originalTmp, arg);
                if (!spilled || !leavesOut(MoveLeftOutByRegisterAllocator::LoadBehindClobber)) [[likely]]
                    insert(block, instIndex + 1, ClobberMoveTo, move, inst.origin, arg, metadata.originalTmp);
                if (spilled)
                    m_stats[gapTmp.bank()].numSplitAroundClobberStackMoves += 2;
                else
                    m_stats[gapTmp.bank()].numSplitAroundClobberRegisterMoves += 2;
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
                if (!leavesOut(MoveLeftOutByRegisterAllocator::LoadAheadOfCluster)) [[likely]]
                    insert(block, instIndex, IntraBlockMoveFrom, move, block->at(instIndex).origin, Arg::stack(spilled), clusterTmp);
                m_stats[bank].numSplitIntraBlockLoad++;
            } else
                ASSERT(split.lastDefPoint);

            // Need to store back into the original Tmp's spill slot only if the cluster def'ed the Tmp
            if (split.lastDefPoint) {
                unsigned instIndex = this->instIndex(positionOfHead, split.lastDefPoint);
                if (!leavesOut(MoveLeftOutByRegisterAllocator::StoreBehindCluster)) [[likely]]
                    insert(block, instIndex + 1, IntraBlockMoveTo, move, block->at(instIndex).origin, clusterTmp, Arg::stack(spilled));
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
                ASSERT(inst.isValidForm(m_code.targetFeatures()));

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
    const RegisterPolicy& m_registerPolicy;
    const LiveRangeSplitPolicy& m_splitPolicy;
    const bool m_usesExtendedForms;
    const bool m_discountsPreferredRegisterMoves;
    Vector<Reg> m_allowedRegistersInPriorityOrder[numBanks];
    ScalarRegisterSet m_allAllowedRegisters;
    UseDefCost m_calleeSaveCost { 0 };
    IndexMap<BasicBlock*, Point> m_blockToHeadPoint;
    Vector<Point> m_tailPoints;
    Vector<unsigned> m_instBlockIndex;
    Mentions m_mentions;
    Vector<unsigned> m_tmpMoves;
    Vector<Tmp> m_tmpsWithCoalescables;
    struct DefSetAside {
        Tmp tmp;
        Tmp movSrc;
        Point point;
    };
    Vector<DefSetAside> m_defsSetAside;
    struct RemovalSetAside {
        Tmp from;
        Tmp partner;
    };
    Vector<RemovalSetAside> m_removalsSetAside;
    Vector<unsigned> m_instsThatNameNeighbors;
    unsigned m_gpIndicesEnd { 0 };
    unsigned m_numberOfLinearIndices { 0 };
    struct SmallConstantDef {
        unsigned index;
        int64_t value;
    };
    Vector<SmallConstantDef> m_smallConstantDefs;
    Vector<BlockEnds> m_blockEnds;
    Vector<unsigned> m_exposedUses;
    Vector<unsigned> m_defsInBlocks;
    Vector<unsigned> m_usesAtTails;
    Vector<uint8_t> m_crossesBoundary;
    Vector<unsigned> m_columnToIndex;
    bool m_countsGapsAsLive { false };
    IntervalArena m_intervalArena;
    TmpDataMap m_map;
    Vector<unsigned> m_firstUseDef;
    std::array<size_t, numBanks> m_numberOfGapTmps { };
    Vector<unsigned> m_useDefsEnd;
    Vector<Point> m_useDefs;
    Vector<unsigned> m_largeInsts;
    Vector<MentionInLargeInst> m_mentionsInLargeInsts;
    Vector<std::pair<unsigned, MentionInLargeInst>> m_furtherMentionsInLargeInsts;
    unsigned m_gpUseDefListsEnd { 0 };
    HashMap<Tmp, UseDefList> m_newTmpUseDefLists;
    Vector<Tmp> m_newlySpilledTmps;
    Vector<unsigned> m_representatives;
    Vector<AroundClobbersSplitMetadata> m_aroundClobbersMetadata;
    struct MoveAroundClobber {
        unsigned blockIndex;
        unsigned index;
        bool isAfterClobber;
        Reg source;
        Reg destination;
    };
    Vector<MoveAroundClobber> m_movesAroundClobbers;
    Vector<IntraBlockSplitMetadata> m_intraBlockMetadata;
    Vector<AroundLoopSplitMetadata> m_aroundLoopMetadata;
    Vector<BlocksOnStack> m_blocksOnStack;
    std::optional<Cycles> m_cycles;
    std::array<Vector<std::optional<CostOfSpill>>, numBanks> m_costsOfSpill;
    HashMap<std::pair<unsigned, unsigned>, float> m_frequencyIfSpilledInCycle;
    Vector<StackSlot*> m_spillSlotTable;
    RegisterOccupancy m_occupancy;
    GenerationalSet<uint8_t, SaVector> m_visited;
    PriorityQueue<TmpPriority> m_queue;
    Vector<TmpPriority> m_queueAtStart;
    size_t m_nextInQueueAtStart { 0 };
    bool m_isFillingQueueAtStart { false };
    Vector<PhaseInsertion> m_insertions;
    Vector<unsigned> m_insertionBlock;
    BlockWorklist m_fastBlocks;
    TmpCounts m_useCounts;
    TmpWidths m_tmpWidth;
    std::array<AirAllocateRegistersStats, numBanks> m_stats = { { { GP, m_code.proc().name() }, { FP, m_code.proc().name() } } };
    std::array<bool, numBanks> m_didSpill { };
    bool m_needsEmitSpillCode { false };
    float m_frequencyOfAllInsts { 0 };
    bool m_hasUseDefLists { false };
    std::unique_ptr<Dominators> m_dominators;
    std::unique_ptr<NaturalLoops> m_naturalLoops;
    Vector<LoopData> m_loopData;
};

} // namespace JSC::B3::Air::Greedy

// The moves that fill the registers of a call do not depend on each other, but the source of each is live while the registers in front of it are written.
// The result of a call that came in one of those registers is read first: then it may stay in that register until this call.
// That saves a move behind the call that made it and may cost one here, so it is done only behind a rare edge, for a result that came in front of every rare edge.
static void orderMovesIntoRegisters(Code& code)
{
    BlockWorklist fastBlocks;
    fastBlocks.push(code[0]);
    while (BasicBlock* block = fastBlocks.pop()) {
        for (FrequentedBlock& successor : block->successors()) {
            if (!successor.isRare())
                fastBlocks.push(successor.block());
        }
    }

    TmpMap<Reg> cameIn(code);
    bool hasAny = false;
    auto isMove = [](const Inst& inst) {
        return (inst.kind.opcode == Move || inst.kind.opcode == MoveDouble) && inst.args().size() == 2;
    };
    for (BasicBlock* block : code) {
        if (!fastBlocks.saw(block))
            continue;
        bool isBehindCall = false;
        for (Inst& inst : *block) {
            if (inst.kind.opcode == Patch) {
                isBehindCall = true;
                continue;
            }
            isBehindCall &= isMove(inst) && inst.args()[0].isReg();
            if (!isBehindCall || !inst.args()[1].isTmp() || inst.args()[1].isReg() || code.isPinned(inst.args()[0].reg()))
                continue;
            cameIn[inst.args()[1].tmp()] = inst.args()[0].reg();
            hasAny = true;
        }
    }
    if (!hasAny)
        return;

    auto fillsRegister = [&](const Inst& inst) {
        if (!isMove(inst) || !inst.args()[1].isReg() || code.isPinned(inst.args()[1].reg()))
            return false;
        const Arg& source = inst.args()[0];
        return source.isSomeImm() || (source.isTmp() && (!source.isReg() || code.isPinned(source.reg())));
    };
    Vector<Inst, 8> moves;
    Vector<Inst, 8> ordered;
    auto isStillRead = [&](Reg reg, unsigned except) {
        for (unsigned i = 0; i < moves.size(); ++i) {
            const Arg& source = moves[i].args()[0];
            if (i != except && source.isTmp() && !source.isReg() && cameIn[source.tmp()] == reg)
                return true;
        }
        return false;
    };
    for (BasicBlock* block : code) {
        if (fastBlocks.saw(block))
            continue;
        for (unsigned begin = 0; begin < block->size();) {
            unsigned end = begin;
            RegisterSet filled;
            while (end < block->size() && fillsRegister(block->at(end)) && !filled.contains(block->at(end).args()[1].reg(), IgnoreVectors))
                filled.add(block->at(end++).args()[1].reg(), IgnoreVectors);
            if (end - begin < 2) {
                begin = std::max(end, begin + 1);
                continue;
            }
            moves.shrink(0);
            ordered.shrink(0);
            for (unsigned i = begin; i < end; ++i)
                moves.append(block->at(i));
            while (!moves.isEmpty()) {
                unsigned next = 0;
                for (unsigned i = 0; i < moves.size(); ++i) {
                    if (!isStillRead(moves[i].args()[1].reg(), i)) {
                        next = i;
                        break;
                    }
                }
                ordered.append(moves[next]);
                moves.removeAt(next);
            }
            for (unsigned i = begin; i < end; ++i)
                block->at(i) = ordered[i - begin];
            begin = end;
        }
    }
}

void allocateRegistersByGreedy(Code& code)
{
    PhaseScope phaseScope(code, "allocateRegistersByGreedy"_s);
    if (code.policy().splitting.splitsAroundLoops)
        ensureDedicatedLoopEntryExitBlocks(code);
    // Where results come in the register of first arguments.
    if (code.policy().registers.evictsHolderOfPreferredRegister && GPRInfo::returnValueGPR == GPRInfo::argumentGPR0)
        orderMovesIntoRegisters(code);
    Greedy::GreedyAllocator allocator(code);
    allocator.run();
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
