/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include <atomic>
#include <memory>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>

namespace JSC { namespace AOT {

inline constexpr unsigned numberOfStatistics = numberOfEnumerators<StatisticID>;
static_assert(numberOfStatistics > 0);

class Statistics {
    WTF_MAKE_NONCOPYABLE(Statistics);
public:
    Statistics();
    Statistics(Statistics&&) = default;
    Statistics& operator=(Statistics&&) = default;

    void count(StatisticID, uint64_t amount = 1);
    void addTime(Stage stage, Seconds time) { count(Wire::timeOf(stage), time.nanosecondsAs<uint64_t>()); }
    JS_EXPORT_PRIVATE Vector<uint64_t> values() const;

private:
    static constexpr unsigned numberOfStripes = 16;
    static constexpr unsigned countersPerCacheLine = 8;
    static constexpr unsigned countersPerStripe = roundUpToMultipleOf(countersPerCacheLine, numberOfStatistics) + countersPerCacheLine;

    std::unique_ptr<std::atomic<uint64_t>[]> m_counters;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
