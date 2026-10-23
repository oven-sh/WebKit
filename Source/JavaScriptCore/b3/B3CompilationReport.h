/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Limits.h"
#include "B3Origin.h"
#include <array>
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

enum class FailureKind : uint8_t {
    AbsoluteAddress,
    RelocationNotLowered,
    VariableReadBeforeSet,
};

struct Failure {
    FailureKind kind;
    Origin origin;
};

enum class Counter : uint8_t {
    TrapsFoldedIntoAccesses,
};

static constexpr unsigned numberOfCounters = 1;
static_assert(static_cast<unsigned>(Counter::TrapsFoldedIntoAccesses) + 1 == numberOfCounters);

struct LimitHit {
    uint32_t count { 0 };
    Origin firstOrigin { };
};

struct CompilationReport {
    std::optional<Failure> failure;
    std::array<LimitHit, numberOfLimits> limitHits { };
    std::array<uint32_t, numberOfCounters> counters { };

    uint32_t hitsOf(Limit limit) const { return limitHits[static_cast<size_t>(limit)].count; }
    Origin firstOriginOf(Limit limit) const { return limitHits[static_cast<size_t>(limit)].firstOrigin; }
    uint32_t countOf(Counter counter) const { return counters[static_cast<size_t>(counter)]; }
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
