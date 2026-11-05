/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTDiagnostics.h"
#include "AOTHostFunctionTexts.h"
#include "AOTLimits.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTWire.h"
#include "AssemblerPolicy.h"
#include "CPUFeatureSet.h"
#include <memory>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class RegExpSummaries;

namespace Typed {
class FormatPatternPlans;
}

class BuildContext {
    WTF_MAKE_TZONE_ALLOCATED(BuildContext);
    WTF_MAKE_NONCOPYABLE(BuildContext);
    WTF_MAKE_NONMOVABLE(BuildContext);
public:
    explicit BuildContext(const Wire::CompileOptions&, const HostThreadPool* = nullptr);
    ~BuildContext();

    const CompileOptions& options() const { return m_options; }
    const RecordedOptions& recordedOptions() const { return m_options.recorded; }
    bool validates() const { return m_options.recorded.validates; }

    const ParallelLoops& parallelLoops() const { return m_parallelLoops.ofCurrentThread(); }
    unsigned threadCount() const { return m_parallelLoops.width(); }
    BackgroundJobs& backgroundJobs() { return m_backgroundJobs.get(); }

    CPUFeatureSet targetFeatures() const { return m_assemblerPolicy.features; }
    const AssemblerPolicy& assemblerPolicy() const { return m_assemblerPolicy; }

    Diagnostics& diagnostics() { return m_diagnostics; }
    Statistics& statistics() { return m_statistics; }

    uint64_t limit(LimitID limit) const { return defaultLimitValue(limit, hostArchitecture()); }
    bool exceeds(LimitID, uint64_t amount, const Subject& = { });
    void reportLimitExceeded(LimitID, uint64_t needed, const Subject& = { });

    RegExpSummaries& regExpSummaries() { return *m_regExpSummaries; }
    Typed::FormatPatternPlans& formatPatternPlans() { return *m_formatPatternPlans; }

    const HostFunctionTexts& functionTexts() const { return m_functionTexts; }
    void setFunctionTexts(const HostFunctionTexts& texts) { m_functionTexts = texts; }

    Diagnostics takeDiagnostics() { return WTF::move(m_diagnostics); }

private:
    Diagnostics m_diagnostics;
    Statistics m_statistics;
    const CompileOptions m_options;
    const AssemblerPolicy m_assemblerPolicy;
    ParallelLoops m_parallelLoops;
    const Ref<BackgroundJobs> m_backgroundJobs { BackgroundJobs::create(m_parallelLoops) };
    HostFunctionTexts m_functionTexts;
    std::unique_ptr<RegExpSummaries> m_regExpSummaries;
    std::unique_ptr<Typed::FormatPatternPlans> m_formatPatternPlans;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
