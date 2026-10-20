/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTReuseEnvironments.h"
#include "AOTThreads.h"
#include "CPU.h"
#include "JSCell.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(offsetOfFirstEnvironmentVariable == sizeof(JSCell));

namespace {

class EnvironmentPacker {
public:
    EnvironmentPacker(const SlotPlanContext& context, const SlotUses& uses, std::span<const SlotClaim> claims, PackedLayouts& packed, LayoutID layout)
        : m_context(context)
        , m_uses(uses)
        , m_packed(packed)
        , m_layout(layout)
        , m_isReused(context.inputs.environmentReuse.isReused(layout))
    {
        for (unsigned field = 0; field < context.rows.numberOfFieldsOf(layout); ++field) {
            const SlotClaim& claim = claims[rowOf(field)];
            if (!claim.hasStorage)
                continue;
            m_order.append(field);
            PlacedSlot& slot = slotOf(field);
            slot.representation = claim.representation;
            slot.storage = SlotStorage::Inline;
        }
    }

    void run();

private:
    SlotRow rowOf(unsigned field) const { return m_context.rows.rowOf(m_layout, field); }
    PlacedSlot& slotOf(unsigned field) { return m_packed.slots[rowOf(field)]; }
    uint32_t sizeOfField(unsigned field) { return widthOf(slotOf(field).representation); }
    uint32_t useCountOf(unsigned field) const { return m_uses.rows[rowOf(field)].environmentUses; }
    uint32_t allocationSizeFor(uint32_t fieldsEnd) const { return WTF::roundUpToMultipleOf<16>(fieldsEnd + (m_isReused ? sizeof(bool) : 0)); }

    void packWidestFirst();
    void packMostUsedFirst();
    void setFieldsEnd(uint32_t end);
    void setInlineLimit();

    const SlotPlanContext& m_context;
    const SlotUses& m_uses;
    PackedLayouts& m_packed;
    LayoutID m_layout;
    bool m_isReused;
    Vector<unsigned> m_order;
};

void EnvironmentPacker::run()
{
    packWidestFirst();
    constexpr uint32_t oneByteRange = 128;
    if (isX86() && m_packed.sizes[m_layout.index()] > oneByteRange)
        packMostUsedFirst();
    setInlineLimit();
}

void EnvironmentPacker::setFieldsEnd(uint32_t end)
{
    if (m_isReused)
        m_packed.isEscapedOffsets[m_layout.index()] = end;
    m_packed.sizes[m_layout.index()] = allocationSizeFor(end);
}

void EnvironmentPacker::setInlineLimit()
{
    uint32_t size = m_packed.sizes[m_layout.index()];
    m_packed.inlineLimits[m_layout.index()] = static_cast<uint32_t>(m_context.sizeClasses.cellSizeFor(size));
    if (size > m_context.build.limit(LimitID::MaxFieldOffset))
        m_context.build.reportLimitExceeded(LimitID::MaxFieldOffset, size, { .layout = m_layout });
}

void EnvironmentPacker::packWidestFirst()
{
    std::ranges::stable_sort(m_order, [&](unsigned a, unsigned b) {
        return sizeOfField(a) > sizeOfField(b);
    });
    uint32_t offset = offsetOfFirstEnvironmentVariable;
    for (unsigned i : m_order) {
        slotOf(i).offset = offset;
        offset += sizeOfField(i);
    }
    setFieldsEnd(offset);
}

void EnvironmentPacker::packMostUsedFirst()
{
    std::ranges::stable_sort(m_order, [&](unsigned a, unsigned b) {
        return useCountOf(a) > useCountOf(b);
    });
    Vector<std::pair<uint32_t, uint32_t>> holes;
    Vector<uint32_t> offsets(FillWith { }, m_context.rows.numberOfFieldsOf(m_layout), 0u);
    uint32_t end = offsetOfFirstEnvironmentVariable;
    for (unsigned i : m_order) {
        uint32_t size = sizeOfField(i);
        RELEASE_ASSERT(hasOneBitSet(size));
        std::optional<size_t> best;
        for (size_t k = 0; k < holes.size(); ++k) {
            if (holes[k].second >= size && (!best || holes[k].first < holes[*best].first))
                best = k;
        }
        if (best) {
            auto [at, holeSize] = holes[*best];
            holes.removeAt(*best);
            offsets[i] = at;
            for (uint32_t piece = size; piece < holeSize; piece *= 2)
                holes.append({ at + piece, piece });
            continue;
        }
        while (end % size) {
            uint32_t piece = end & -end;
            holes.append({ end, piece });
            end += piece;
        }
        offsets[i] = end;
        end += size;
    }
    if (allocationSizeFor(end) > m_packed.sizes[m_layout.index()])
        return;
    for (unsigned i : m_order)
        slotOf(i).offset = offsets[i];
    setFieldsEnd(end);
}

} // anonymous namespace

void packEnvironments(const SlotPlanContext& context, const SlotUses& uses, std::span<const SlotClaim> claims, PackedLayouts& packed)
{
    forEachIndexInParallel(context.build.parallelLoops(), context.program.layouts.size(), [&](unsigned index) {
        LayoutID layout(index);
        if (context.isEnvironment(layout))
            EnvironmentPacker(context, uses, claims, packed, layout).run();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
