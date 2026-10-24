/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static_assert(!(defaultLimitValue(LimitID::MaxGuardBitsPerClassSide, hostArchitecture()) % MemberGuardData::bitsPerWord));

static std::pair<MemberGuardKind, PropertyKeyID> kindAndKeyOf(const GuardUse& guard)
{
    return { guard.kind, guard.key };
}

static bool isOnSameClassSide(const GuardUse& a, const GuardUse& b)
{
    return a.classLayout == b.classLayout && a.side == b.side;
}

static Vector<GuardUse> chooseHeaviestGuards(const PlacementContext& context, std::span<const GuardUse> guards, size_t count)
{
    Vector<GuardUse> chosen(guards);
    AOT::sort(chosen, [](const GuardUse& a, const GuardUse& b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return kindAndKeyOf(a) < kindAndKeyOf(b);
    });
    for (const GuardUse& guard : chosen.subspan(count))
        context.build.gaveUp(LimitID::MaxGuardBitsPerClassSide, { .layout = guard.classLayout });
    chosen.shrink(count);
    AOT::sort(chosen, { }, kindAndKeyOf);
    return chosen;
}

static void appendGuardBits(const PlacementContext& context, std::span<const GuardUse> guards, GuardBitTable& table)
{
    uint32_t bit = 0;
    for (const GuardUse& guard : guards) {
        table.bits.append(PlacedGuardBit {
            .classLayout = guard.classLayout,
            .side = guard.side,
            .kind = guard.kind,
            .key = guard.key,
            .bit = bit++,
            .holdsWhileClear = context.slots().classCreation(guard.classLayout).guardsHoldWhileClear,
        });
    }
}

static void placeClassSide(const PlacementContext& context, std::span<const GuardUse> guards, GuardBitTable& table)
{
    RELEASE_ASSERT(context.program.at(guards.front().classLayout).kind == LayoutKind::Instance);
    uint64_t limit = context.build.limit(LimitID::MaxGuardBitsPerClassSide);
    if (guards.size() <= limit) {
        appendGuardBits(context, guards, table);
        return;
    }
    appendGuardBits(context, chooseHeaviestGuards(context, guards, static_cast<size_t>(limit)).span(), table);
}

GuardBitTable placeGuardBits(const PlacementContext& context, const PlacedSites& sites)
{
    GuardBitTable table;
    table.bits.reserveInitialCapacity(sites.guards.size());
    std::span<const GuardUse> remaining = sites.guards.span();
    while (!remaining.empty()) {
        auto end = std::ranges::find_if_not(remaining, [&](const GuardUse& guard) {
            return isOnSameClassSide(guard, remaining.front());
        });
        size_t count = end - remaining.begin();
        placeClassSide(context, remaining.first(count), table);
        remaining = remaining.subspan(count);
    }
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
