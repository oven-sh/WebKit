/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnitPlacement.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

struct RunOfUnits {
    size_t first;
    size_t end;
    size_t size;
};

class UnitPlacer {
public:
    explicit UnitPlacer(const UnitPlacementInputs& inputs)
        : m_inputs(inputs)
        , m_position(inputs.start)
        , m_lastCopies(FillWith { }, inputs.copyables.size(), std::nullopt)
        , m_isWaitedFor(inputs.copyables.size())
    {
        RELEASE_ASSERT(inputs.alignedOffsets.size() == inputs.sizes.size());
        RELEASE_ASSERT(inputs.uses.empty() || inputs.firstUseByUnit.size() == inputs.sizes.size() + 1);
        RELEASE_ASSERT(inputs.alignments.size() == 1 || inputs.alignments.size() == inputs.sizes.size());
        for (size_t alignment : inputs.alignments)
            RELEASE_ASSERT(isPowerOfTwo(alignment));
        RELEASE_ASSERT(inputs.copyables.empty() || isPowerOfTwo(inputs.alignmentAfterCopies));
        for (const CopyableUnit& unit : inputs.copyables)
            RELEASE_ASSERT(isPowerOfTwo(unit.alignment));
        m_result.unitStarts = Vector<size_t>(FillWith { }, inputs.sizes.size(), 0);
        m_result.copyOffsetByUse = Vector<size_t>(FillWith { }, inputs.uses.size(), 0);
    }

    UnitPlacement place()
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

private:
    size_t numberOfUnits() const { return m_inputs.sizes.size(); }
    size_t alignmentOf(size_t unit) const { return m_inputs.alignments[m_inputs.alignments.size() == 1 ? 0 : unit]; }
    uint32_t firstUseOfUnit(size_t unit) const { return m_inputs.uses.empty() ? 0 : m_inputs.firstUseByUnit[unit]; }
    size_t endOfRun(const RunOfUnits& run) const { return startOfUnit(positionAfterBatch(), run.first) + run.size; }
    size_t offsetOfLastCopy(uint32_t copyable) const { return m_result.copies[*m_lastCopies[copyable]].offset; }
    bool isInBatch(uint32_t copyable) const { return m_lastCopies[copyable] && *m_lastCopies[copyable] >= m_firstCopyOfBatch; }

    size_t paddedSizeOfCopy(uint32_t copyable) const
    {
        const CopyableUnit& unit = m_inputs.copyables[copyable];
        return unit.size + unit.alignment - 1;
    }

    std::span<const CopyableUse> usesOfRun(const RunOfUnits& run) const
    {
        uint32_t first = firstUseOfUnit(run.first);
        return m_inputs.uses.subspan(first, firstUseOfUnit(run.end) - first);
    }

    RunOfUnits findRun(size_t first) const
    {
        RunOfUnits run { first, first + 1, m_inputs.sizes[first] };
        for (; run.end < numberOfUnits() && m_inputs.fallsIntoNext.get(run.end - 1); ++run.end)
            run.size += m_inputs.sizes[run.end];
        return run;
    }

    size_t startOfUnit(size_t position, size_t unit) const
    {
        size_t alignedOffset = m_inputs.alignedOffsets[unit];
        return WTF::roundUpToMultipleOf(alignmentOf(unit), position + alignedOffset) - alignedOffset;
    }

    size_t positionAfterBatch() const
    {
        if (m_result.copies.size() == m_firstCopyOfBatch)
            return m_position;
        return WTF::roundUpToMultipleOf(m_inputs.alignmentAfterCopies, m_position);
    }

    size_t placeUnit(size_t unit, const RunOfUnits& run)
    {
        size_t start = startOfUnit(m_position, unit);
        RELEASE_ASSERT(unit == run.first || start == m_position);
        m_result.unitStarts[unit] = start;
        m_position = start + m_inputs.sizes[unit];
        return start;
    }

    bool needsCopy(uint32_t copyable, size_t endOfLastSite, int64_t reach) const
    {
        if (!m_lastCopies[copyable])
            return true;
        if (isInBatch(copyable))
            return false;
        return static_cast<int64_t>(endOfLastSite - offsetOfLastCopy(copyable)) >= reach;
    }

    void appendCopy(uint32_t copyable)
    {
        const CopyableUnit& unit = m_inputs.copyables[copyable];
        uint32_t copyIndex = safeCast<uint32_t>(m_result.copies.size());
        size_t offset = WTF::roundUpToMultipleOf(unit.alignment, m_position);

        m_result.copies.append(PlacedCopy { copyable, offset });
        m_lastCopies[copyable] = copyIndex;
        m_position = offset + unit.size;
    }

    void finishBatch()
    {
        m_position = positionAfterBatch();
        m_firstCopyOfBatch = m_result.copies.size();
    }

    void placeCopiesBeforeUsers()
    {
        for (size_t first = 0; first < numberOfUnits();) {
            RunOfUnits run = findRun(first);
            std::span<const CopyableUse> usesByReach = usesOfRun(run);
            Vector<CopyableUse, 8> sortedUses;
            if (usesByReach.size() > 1) {
                sortedUses.append(usesByReach);
                AOT::stableSortInLine(sortedUses, [](const CopyableUse& a, const CopyableUse& b) {
                    if (a.reach != b.reach)
                        return a.reach > b.reach;
                    return std::tie(a.copyable, a.firstSite, a.lastSite) < std::tie(b.copyable, b.firstSite, b.lastSite);
                });
                usesByReach = sortedUses.span();
            }
            bool didPlaceCopy;
            do {
                didPlaceCopy = false;
                for (const CopyableUse& use : usesByReach) {
                    if (!needsCopy(use.copyable, endOfRun(run), use.reach))
                        continue;
                    appendCopy(use.copyable);
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

    bool shouldFlushBefore(const RunOfUnits& run) const
    {
        if (!m_deadline)
            return false;
        size_t runEnd = endOfRun(run);
        size_t batchSize = m_waitingSize;
        for (const CopyableUse& use : usesOfRun(run)) {
            if (!m_isWaitedFor.quickGet(use.copyable) && needsCopy(use.copyable, runEnd, use.reach))
                batchSize += paddedSizeOfCopy(use.copyable);
        }
        return static_cast<int64_t>(runEnd + batchSize) >= *m_deadline;
    }

    void bindOrWait(uint32_t useIndex, size_t unitStart)
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
        m_waitingSize += paddedSizeOfCopy(use.copyable);
    }

    void flush()
    {
        AOT::sort(m_waitedFor);
        for (uint32_t copyable : m_waitedFor) {
            appendCopy(copyable);
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

    void placeCopiesAfterUsers()
    {
        for (size_t first = 0; first < numberOfUnits();) {
            RunOfUnits run = findRun(first);
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

    const UnitPlacementInputs& m_inputs;
    UnitPlacement m_result;
    size_t m_position;
    size_t m_firstCopyOfBatch { 0 };
    Vector<std::optional<uint32_t>> m_lastCopies;
    BitVector m_isWaitedFor;
    Vector<uint32_t> m_waitedFor;
    Vector<uint32_t> m_waitingUses;
    size_t m_waitingSize { 0 };
    std::optional<int64_t> m_deadline;
};

} // anonymous namespace

UnitPlacement placeUnits(const UnitPlacementInputs& inputs)
{
    return UnitPlacer(inputs).place();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
