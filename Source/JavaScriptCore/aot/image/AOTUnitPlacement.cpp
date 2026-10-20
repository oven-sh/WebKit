/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnitPlacement.h"

#if ENABLE(AOT)

#include <algorithm>
#include <functional>
#include <optional>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

struct Run {
    size_t first;
    size_t end;
    size_t size;
};

class UnitPlacer {
public:
    explicit UnitPlacer(const UnitPlacementInputs&);

    UnitPlacement place();

private:
    struct PathEntry {
        uint32_t copyable;
        uint32_t nextUse;
    };

    struct UnboundUse {
        uint32_t copy;
        uint32_t use;
        uint32_t copyable;
    };

    size_t numberOfUnits() const { return m_inputs.sizes.size(); }
    size_t alignmentOf(size_t unit) const { return m_inputs.alignments[m_inputs.alignments.size() == 1 ? 0 : unit]; }
    uint32_t firstUseOfUnit(size_t unit) const { return m_inputs.uses.empty() ? 0 : m_inputs.firstUseByUnit[unit]; }
    std::span<const CopyableUse> usesOfRun(const Run&) const;
    std::span<const CopyableUse> usesOfCopyable(uint32_t copyable) const;
    void computeClosureSizes();

    Run findRun(size_t first) const;
    size_t startOfUnit(size_t position, size_t unit) const;
    size_t positionAfterBatch() const;
    size_t endOfRun(const Run& run) const { return startOfUnit(positionAfterBatch(), run.first) + run.size; }
    size_t placeUnit(size_t unit, const Run&);

    size_t offsetOfLastCopy(uint32_t copyable) const { return m_result.copies[*m_lastCopies[copyable]].offset; }
    bool isInBatch(uint32_t copyable) const { return m_lastCopies[copyable] && *m_lastCopies[copyable] >= m_firstCopyOfBatch; }
    bool needsCopy(uint32_t copyable, size_t endOfLastSite, int64_t reach) const;
    void pushOnPath(uint32_t copyable);
    void appendCopy(uint32_t copyable);
    void placeCopy(uint32_t copyable);
    void finishBatch();

    void placeCopiesBeforeUsers();

    bool shouldFlushBefore(const Run&) const;
    void bindOrWait(uint32_t use, size_t unitStart);
    void flush();
    void placeCopiesAfterUsers();

    const UnitPlacementInputs& m_inputs;
    UnitPlacement m_result;
    size_t m_position;
    size_t m_firstCopyOfBatch { 0 };
    Vector<std::optional<uint32_t>> m_lastCopies;
    Vector<size_t> m_closureSizes;
    BitVector m_isOnPath;
    Vector<PathEntry> m_path;
    Vector<UnboundUse> m_unboundUses;
    BitVector m_isWaitedFor;
    Vector<uint32_t> m_waitedFor;
    Vector<uint32_t> m_waitingUses;
    size_t m_waitingSize { 0 };
    std::optional<int64_t> m_deadline;
};

UnitPlacer::UnitPlacer(const UnitPlacementInputs& inputs)
    : m_inputs(inputs)
    , m_position(inputs.start)
    , m_lastCopies(FillWith { }, inputs.copyables.size(), std::nullopt)
    , m_closureSizes(FillWith { }, inputs.copyables.size(), 0)
    , m_isOnPath(inputs.copyables.size())
    , m_isWaitedFor(inputs.copyables.size())
{
    RELEASE_ASSERT(inputs.alignedOffsets.size() == inputs.sizes.size());
    RELEASE_ASSERT(inputs.uses.empty() || inputs.firstUseByUnit.size() == inputs.sizes.size() + 1);
    RELEASE_ASSERT(inputs.usesOfCopyables.empty() || inputs.firstUseByCopyable.size() == inputs.copyables.size() + 1);
    RELEASE_ASSERT(inputs.alignments.size() == 1 || inputs.alignments.size() == inputs.sizes.size());
    for (size_t alignment : inputs.alignments)
        RELEASE_ASSERT(isPowerOfTwo(alignment));
    RELEASE_ASSERT(inputs.sizes.empty() || !(inputs.shiftOfFirstUnit % alignmentOf(0)));
    RELEASE_ASSERT(inputs.copyables.empty() || isPowerOfTwo(inputs.alignmentAfterCopies));
    for (const CopyableUnit& unit : inputs.copyables)
        RELEASE_ASSERT(isPowerOfTwo(unit.alignment));
    m_result.unitStarts = Vector<size_t>(FillWith { }, inputs.sizes.size(), 0);
    m_result.copyOffsetByUse = Vector<size_t>(FillWith { }, inputs.uses.size(), 0);
    computeClosureSizes();
}

std::span<const CopyableUse> UnitPlacer::usesOfRun(const Run& run) const
{
    uint32_t first = firstUseOfUnit(run.first);
    return m_inputs.uses.subspan(first, firstUseOfUnit(run.end) - first);
}

std::span<const CopyableUse> UnitPlacer::usesOfCopyable(uint32_t copyable) const
{
    if (m_inputs.usesOfCopyables.empty())
        return { };
    uint32_t first = m_inputs.firstUseByCopyable[copyable];
    return m_inputs.usesOfCopyables.subspan(first, m_inputs.firstUseByCopyable[copyable + 1] - first);
}

void UnitPlacer::computeClosureSizes()
{
    BitVector isReached(m_inputs.copyables.size());
    Vector<uint32_t> reached;
    for (uint32_t root = 0; root < m_inputs.copyables.size(); ++root) {
        isReached.quickSet(root);
        reached.append(root);
        for (size_t next = 0; next < reached.size(); ++next) {
            const CopyableUnit& unit = m_inputs.copyables[reached[next]];
            m_closureSizes[root] += unit.size + unit.alignment - 1;
            for (const CopyableUse& use : usesOfCopyable(reached[next])) {
                if (!isReached.quickSet(use.copyable))
                    reached.append(use.copyable);
            }
        }
        for (uint32_t copyable : reached)
            isReached.quickClear(copyable);
        reached.shrink(0);
    }
}

Run UnitPlacer::findRun(size_t first) const
{
    Run run { first, first + 1, m_inputs.sizes[first] };
    for (; run.end < numberOfUnits() && m_inputs.fallsIntoNext.get(run.end - 1); ++run.end)
        run.size += m_inputs.sizes[run.end];
    return run;
}

size_t UnitPlacer::startOfUnit(size_t position, size_t unit) const
{
    size_t alignedOffset = m_inputs.alignedOffsets[unit];
    size_t start = WTF::roundUpToMultipleOf(alignmentOf(unit), position + alignedOffset) - alignedOffset;
    return unit ? start : start + m_inputs.shiftOfFirstUnit;
}

size_t UnitPlacer::positionAfterBatch() const
{
    if (m_result.copies.size() == m_firstCopyOfBatch)
        return m_position;
    return WTF::roundUpToMultipleOf(m_inputs.alignmentAfterCopies, m_position);
}

size_t UnitPlacer::placeUnit(size_t unit, const Run& run)
{
    size_t start = startOfUnit(m_position, unit);
    RELEASE_ASSERT(unit == run.first || start == m_position);
    m_result.unitStarts[unit] = start;
    m_position = start + m_inputs.sizes[unit];
    return start;
}

bool UnitPlacer::needsCopy(uint32_t copyable, size_t endOfLastSite, int64_t reach) const
{
    if (!m_lastCopies[copyable])
        return true;
    // A closure size bounds the size of a batch only if the batch has at most one copy of each copyable.
    if (isInBatch(copyable))
        return false;
    return static_cast<int64_t>(endOfLastSite - offsetOfLastCopy(copyable)) >= reach;
}

void UnitPlacer::pushOnPath(uint32_t copyable)
{
    m_isOnPath.quickSet(copyable);
    m_path.append(PathEntry { copyable, 0 });
}

void UnitPlacer::appendCopy(uint32_t copyable)
{
    const CopyableUnit& unit = m_inputs.copyables[copyable];
    std::span<const CopyableUse> uses = usesOfCopyable(copyable);
    uint32_t copyIndex = safeCast<uint32_t>(m_result.copies.size());
    size_t offset = WTF::roundUpToMultipleOf(unit.alignment, m_position);

    PlacedCopy copy { copyable, offset, { } };
    copy.copyOffsetByUse.reserveInitialCapacity(uses.size());
    for (uint32_t use = 0; use < uses.size(); ++use) {
        if (m_isOnPath.quickGet(uses[use].copyable)) {
            m_unboundUses.append(UnboundUse { copyIndex, use, uses[use].copyable });
            copy.copyOffsetByUse.append(0);
            continue;
        }
        copy.copyOffsetByUse.append(offsetOfLastCopy(uses[use].copyable));
    }
    m_result.copies.append(WTF::move(copy));
    m_lastCopies[copyable] = copyIndex;
    m_position = offset + unit.size;

    m_unboundUses.removeAllMatching([&](const UnboundUse& unbound) {
        if (unbound.copyable != copyable)
            return false;
        m_result.copies[unbound.copy].copyOffsetByUse[unbound.use] = offset;
        return true;
    });
}

void UnitPlacer::placeCopy(uint32_t root)
{
    pushOnPath(root);
    while (!m_path.isEmpty()) {
        auto [copyable, nextUse] = m_path.last();
        std::span<const CopyableUse> uses = usesOfCopyable(copyable);
        if (nextUse == uses.size()) {
            appendCopy(copyable);
            m_isOnPath.quickClear(copyable);
            m_path.removeLast();
            continue;
        }
        ++m_path.last().nextUse;
        const CopyableUse& use = uses[nextUse];
        if (!m_isOnPath.quickGet(use.copyable) && needsCopy(use.copyable, m_position + m_closureSizes[copyable], use.reach))
            pushOnPath(use.copyable);
    }
    RELEASE_ASSERT(m_unboundUses.isEmpty());
}

void UnitPlacer::finishBatch()
{
    m_position = positionAfterBatch();
    m_firstCopyOfBatch = m_result.copies.size();
}

void UnitPlacer::placeCopiesBeforeUsers()
{
    for (size_t first = 0; first < numberOfUnits();) {
        Run run = findRun(first);
        // The copy with the shortest reach lies next to its user.
        Vector<CopyableUse, 8> usesByReach(usesOfRun(run));
        std::ranges::stable_sort(usesByReach, std::ranges::greater { }, &CopyableUse::reach);
        bool didPlaceCopy;
        do {
            didPlaceCopy = false;
            for (const CopyableUse& use : usesByReach) {
                if (!needsCopy(use.copyable, endOfRun(run), use.reach))
                    continue;
                placeCopy(use.copyable);
                didPlaceCopy = true;
            }
        } while (didPlaceCopy);
        finishBatch();

        for (size_t unit = run.first; unit < run.end; ++unit)
            placeUnit(unit, run);
        for (uint32_t use = firstUseOfUnit(run.first); use < firstUseOfUnit(run.end); ++use)
            m_result.copyOffsetByUse[use] = offsetOfLastCopy(m_inputs.uses[use].copyable);
        first = run.end;
    }
}

bool UnitPlacer::shouldFlushBefore(const Run& run) const
{
    if (!m_deadline)
        return false;
    size_t runEnd = endOfRun(run);
    size_t batchSize = m_waitingSize;
    for (const CopyableUse& use : usesOfRun(run)) {
        if (!m_isWaitedFor.quickGet(use.copyable) && needsCopy(use.copyable, runEnd, use.reach))
            batchSize += m_closureSizes[use.copyable];
    }
    return static_cast<int64_t>(runEnd + batchSize) >= *m_deadline;
}

void UnitPlacer::bindOrWait(uint32_t useIndex, size_t unitStart)
{
    const CopyableUse& use = m_inputs.uses[useIndex];
    if (!needsCopy(use.copyable, unitStart + use.lastSite, use.reach)) {
        m_result.copyOffsetByUse[useIndex] = offsetOfLastCopy(use.copyable);
        return;
    }
    int64_t deadline = static_cast<int64_t>(unitStart + use.firstSite) + use.reach;
    m_deadline = m_deadline ? std::min(*m_deadline, deadline) : deadline;
    m_waitingUses.append(useIndex);
    if (m_isWaitedFor.quickSet(use.copyable))
        return;
    m_waitedFor.append(use.copyable);
    m_waitingSize += m_closureSizes[use.copyable];
}

void UnitPlacer::flush()
{
    std::ranges::sort(m_waitedFor);
    for (uint32_t copyable : m_waitedFor) {
        if (!isInBatch(copyable))
            placeCopy(copyable);
        m_isWaitedFor.quickClear(copyable);
    }
    for (uint32_t use : m_waitingUses)
        m_result.copyOffsetByUse[use] = offsetOfLastCopy(m_inputs.uses[use].copyable);
    m_waitedFor.shrink(0);
    m_waitingUses.shrink(0);
    m_waitingSize = 0;
    m_deadline = std::nullopt;
    finishBatch();
}

void UnitPlacer::placeCopiesAfterUsers()
{
    for (size_t first = 0; first < numberOfUnits();) {
        Run run = findRun(first);
        if (shouldFlushBefore(run))
            flush();
        for (size_t unit = run.first; unit < run.end; ++unit) {
            size_t start = placeUnit(unit, run);
            for (uint32_t use = firstUseOfUnit(unit); use < firstUseOfUnit(unit + 1); ++use)
                bindOrWait(use, start);
        }
        first = run.end;
    }
    if (m_deadline)
        flush();
}

UnitPlacement UnitPlacer::place()
{
    switch (m_inputs.policy) {
    case CopyPolicy::BeforeUser:
        placeCopiesBeforeUsers();
        break;
    case CopyPolicy::AfterUsers:
        placeCopiesAfterUsers();
        break;
    }
    m_result.end = m_position;
    return WTF::move(m_result);
}

} // anonymous namespace

UnitPlacement placeUnits(const UnitPlacementInputs& inputs)
{
    return UnitPlacer(inputs).place();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
