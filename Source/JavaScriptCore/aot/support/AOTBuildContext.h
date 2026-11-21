/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTDiagnostics.h"
#include "AOTHostFunctionSources.h"
#include "AOTLimits.h"
#include "AOTStatistics.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "AssemblerPolicy.h"
#include "CPUFeatureSet.h"
#include <memory>
#include <utility>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class RegExpSummaries;

namespace Typed {
class FormatPatternPlans;
class LinkFlow;
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

    const HostFunctionSources& functionSources() const { return m_functionSources; }
    void setFunctionSources(const HostFunctionSources& sources) { m_functionSources = sources; }
    const HostStringEncoders& stringEncoders() const { return m_stringEncoders; }
    void setStringEncoders(const HostStringEncoders& encoders) { m_stringEncoders = encoders; }

    std::span<const uint8_t> orderFile() const { return m_orderFile; }
    void setOrderFile(std::span<const uint8_t> bytes) { m_orderFile = bytes; }
    std::span<const uint32_t> startRanks() const { return m_startRanks.span(); }
    void setStartRanks(Vector<uint32_t>&& ranks) { m_startRanks = WTF::move(ranks); }
    uint32_t numberOfUnitsOfStart() const { return m_numberOfUnitsOfStart; }
    void setNumberOfUnitsOfStart(uint32_t number) { m_numberOfUnitsOfStart = number; }
    const Function<void(const Typed::LinkFlow&)>& askedOfFlowAtRest() const { return m_askedOfFlowAtRest; }
    void askOfFlowAtRest(Function<void(const Typed::LinkFlow&)>&& asked) { m_askedOfFlowAtRest = WTF::move(asked); }

    Diagnostics takeDiagnostics() { return WTF::move(m_diagnostics); }

private:
    Diagnostics m_diagnostics;
    Statistics m_statistics;
    const CompileOptions m_options;
    const AssemblerPolicy m_assemblerPolicy;
    ParallelLoops m_parallelLoops;
    const Ref<BackgroundJobs> m_backgroundJobs { BackgroundJobs::create(m_parallelLoops) };
    HostFunctionSources m_functionSources;
    HostStringEncoders m_stringEncoders;
    Vector<uint32_t> m_startRanks;
    uint32_t m_numberOfUnitsOfStart { 0 };
    std::span<const uint8_t> m_orderFile;
    Function<void(const Typed::LinkFlow&)> m_askedOfFlowAtRest;
    std::unique_ptr<RegExpSummaries> m_regExpSummaries;
    std::unique_ptr<Typed::FormatPatternPlans> m_formatPatternPlans;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
