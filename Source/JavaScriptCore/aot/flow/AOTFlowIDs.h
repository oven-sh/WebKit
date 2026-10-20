/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTID.h"
#include "AOTSchemaRecords.h"
#include "AOTWire.h"
#include <tuple>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

using FlowObjectID = ID<struct FlowObjectIDTag>;
using FlowNodeID = ID<struct FlowNodeIDTag>;
using FlowEdgeID = ID<struct FlowEdgeIDTag>;
using FlowInstanceID = ID<struct FlowInstanceIDTag>;
using FlowLocalID = ID<struct FlowLocalIDTag>;
using FlowStringSetID = ID<struct FlowStringSetIDTag>;
using FlowSymbolSetID = ID<struct FlowSymbolSetIDTag>;
using FlowRangeID = ID<struct FlowRangeIDTag>;
using FlowCallLinkID = ID<struct FlowCallLinkIDTag>;

inline constexpr FlowObjectID unknownFlowObject { 0u };

// For a HashedIndex. Every bit of the two numbers reaches every bit of the hash.
constexpr uint32_t hashOfNumbers(uint64_t first, uint64_t second)
{
    uint64_t mixed = (first + 0x9E3779B97F4A7C15ull) * 0xFF51AFD7ED558CCDull ^ second * 0xC4CEB9FE1A85EC53ull;
    mixed ^= mixed >> 32;
    return static_cast<uint32_t>(mixed * 0x9E3779B97F4A7C15ull >> 32);
}

inline constexpr Witness noWitness { Wire::none, Wire::none, noSourceLocation };

constexpr bool hasFunction(const Witness& witness) { return witness.function != Wire::none; }

constexpr bool isLowerWitness(const Witness& witness, const Witness& other)
{
    if (!hasFunction(witness))
        return false;
    return !hasFunction(other) || std::tie(witness.function, witness.value) < std::tie(other.function, other.value);
}

constexpr void keepLowerWitness(Witness& current, const Witness& candidate)
{
    if (isLowerWitness(candidate, current))
        current = candidate;
}

enum class FlowContext : uint8_t {
    Generic,
    Proven,
    // Every operand is tracked, and the caller runs with unknown operands. What its objects hold never meets what the callers of the proven instance pass.
    ProvenUnderGeneric,
};

inline constexpr unsigned numberOfFlowContexts = 3;

constexpr bool hasTrackedOperands(FlowContext context)
{
    return context != FlowContext::Generic;
}

enum class FlowProgress : uint8_t {
    Finished,
    QueuedWork,
};

enum class FlowCertainty : uint8_t {
    Predicted,
    Proven,
};

constexpr FlowCertainty weakerOf(FlowCertainty a, FlowCertainty b) { return a == FlowCertainty::Predicted ? a : b; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
