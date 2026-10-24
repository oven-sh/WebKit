/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include "AOTWire.h"
#include "CPU.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <array>
#include <expected>
#include <span>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

using Architecture = Wire::Architecture;
using LimitID = Wire::LimitID;
using Stage = Wire::Stage;
using StatisticID = Wire::StatisticID;

using Wire::degradesToOf;
using Wire::hitsOf;
using Wire::protectsOf;
using Wire::purposeOf;
using Wire::smallestOf;
using Wire::stageOf;

inline constexpr unsigned numberOfLimits = numberOfEnumerators<LimitID>;

static_assert(isARM64() || isX86_64());
static_assert(LimitID { } == LimitID::None);
static_assert(hitsOf(LimitID::None) == StatisticID::None);

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

constexpr bool failsBuild(LimitID limit) { return degradesToOf(limit) == Wire::DegradesTo::BuildError; }
constexpr bool canBeLowered(LimitID limit) { return limit != LimitID::None && !failsBuild(limit); }

// What `limit` is worth in a build that lowers `lowered` to `value`. The compiler and the run time both ask here.
constexpr uint64_t valueWhenLowered(LimitID limit, uint64_t usual, LimitID lowered, uint64_t value)
{
    if (lowered == limit)
        return std::min(usual, value);
    if (lowered == LimitID::All && canBeLowered(limit))
        return std::min(usual, std::max(value, smallestOf(limit)));
    return usual;
}

class LimitValues {
public:
    JS_EXPORT_PRIVATE explicit LimitValues(Architecture);

    uint64_t operator[](LimitID limit) const { return m_values[static_cast<size_t>(limit)]; }
    std::span<const uint64_t, numberOfLimits> span() const LIFETIME_BOUND { return m_values; }

    JS_EXPORT_PRIVATE void lower(LimitID, uint64_t value);

    friend bool operator==(const LimitValues&, const LimitValues&) = default;

private:
    std::array<uint64_t, numberOfLimits> m_values;
};

namespace Limits {

inline constexpr uint64_t maxWorkers = defaultLimitValue(LimitID::MaxWorkers, hostArchitecture());
inline constexpr uint64_t regExpParenthesesDepth = defaultLimitValue(LimitID::RegExpNestingDepthWithCode, hostArchitecture());
inline constexpr uint64_t regExpPatternLengthWithCode = defaultLimitValue(LimitID::RegExpPatternLengthWithCode, hostArchitecture());

} // namespace Limits

struct GaveUp {
    LimitID limit;
};

template<typename T> using OrGaveUp = std::expected<T, GaveUp>;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
