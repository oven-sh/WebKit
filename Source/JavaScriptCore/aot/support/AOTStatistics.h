/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPassID.h"
#include "AOTSchemaEnums.h"
#include "JSExportMacros.h"
#include <atomic>
#include <memory>
#include <wtf/Forward.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>

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
    void countHit(LimitID limit) { count(hitsOf(limit)); }
    void countChanges(PassID pass, uint64_t amount) { count(Wire::changesOf(pass), amount); }
    void countCodeBytesAdded(PassID pass, uint64_t amount) { count(Wire::codeBytesAddedOf(pass), amount); }
    void countCodeBytesRemoved(PassID pass, uint64_t amount) { count(Wire::codeBytesRemovedOf(pass), amount); }
    void addTime(Stage stage, Seconds time) { count(Wire::timeOf(stage), time.nanosecondsAs<uint64_t>()); }
    JS_EXPORT_PRIVATE Vector<uint64_t> values() const;

    struct PassTime {
        Seconds wall;
        Seconds cpu;
        uint64_t runs { 0 };
    };

    void addPassTime(PassID, const PassTime&);
    void dumpPassTimes(PrintStream&) const;
    const Vector<PassTime>& passTimes() const { return m_passTimes; }

private:
    static constexpr unsigned numberOfStripes = 16;
    static constexpr unsigned countersPerCacheLine = 8;
    static constexpr unsigned countersPerStripe = roundUpToMultipleOf(countersPerCacheLine, numberOfStatistics) + countersPerCacheLine;

    std::unique_ptr<std::atomic<uint64_t>[]> m_counters;
    Vector<PassTime> m_passTimes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
