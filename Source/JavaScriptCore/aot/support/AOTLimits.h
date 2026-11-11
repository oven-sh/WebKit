/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include "CPU.h"

namespace JSC { namespace AOT {

using Architecture = Wire::Architecture;
using LimitID = Wire::LimitID;
using Stage = Wire::Stage;
using StatisticID = Wire::StatisticID;

static_assert(isARM64() || isX86_64());
static_assert(LimitID { } == LimitID::None);

constexpr Architecture hostArchitecture() { return isX86_64() ? Architecture::X86_64 : Architecture::ARM64; }

constexpr uint64_t defaultLimitValue(LimitID limit, Architecture architecture)
{
    switch (architecture) {
    case Architecture::ARM64:
        return Wire::valueOnARM64Of(limit);
    case Architecture::X86_64:
        return Wire::valueOnX86_64Of(limit);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

namespace Limits {

inline constexpr uint64_t maxWorkers = defaultLimitValue(LimitID::MaxWorkers, hostArchitecture());
inline constexpr uint64_t regExpParenthesesDepth = 256;

} // namespace Limits

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
