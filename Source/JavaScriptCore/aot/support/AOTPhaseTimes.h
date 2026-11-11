/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTLimits.h"
#include <atomic>
#include <optional>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>

namespace JSC { namespace AOT {

class BuildContext;

class InstructionCounts {
public:
    static void begin() { s_hasBegun.store(true, std::memory_order_relaxed); }
    static bool hasBegun() { return s_hasBegun.load(std::memory_order_relaxed); }
    static void countOnCurrentThread();
    static std::optional<uint64_t> ofCurrentThread();
    static std::optional<uint64_t> ofAllThreads();
    static void noteSpentInsideLoop(uint64_t instructions);
    static uint64_t spentInsideLoopsOnCurrentThread();

private:
    static std::atomic<bool> s_hasBegun;
};

class PhaseTimes {
    WTF_MAKE_NONCOPYABLE(PhaseTimes);
public:
    PhaseTimes(BuildContext&, Stage);
    PhaseTimes(BuildContext&, ASCIILiteral name);
    ~PhaseTimes();

    void didFinish(ASCIILiteral phase);
    static size_t residentMegabytes();
    static size_t peakResidentMegabytes();
    static size_t takePeakResidentMegabytesSinceLastTaken();
    struct PeakResidentMegabytes {
        size_t ofProcess { 0 };
        size_t sinceLastTaken { 0 };
    };
    static PeakResidentMegabytes takePeakResidentMegabytes();

private:
    PhaseTimes(BuildContext&, ASCIILiteral name, std::optional<Stage> timedStage);

    BuildContext& m_build;
    const ASCIILiteral m_name;
    const std::optional<Stage> m_timedStage;
    const MonotonicTime m_start;
    const CurrentWorkScope m_currentWork;
    MonotonicTime m_phaseStart;
    Seconds m_processCPUTimeAtPhaseStart;
    std::optional<uint64_t> m_instructionsAtPhaseStart;
};

class StepTimes {
    WTF_MAKE_NONCOPYABLE(StepTimes);
public:
    StepTimes(const BuildContext&, ASCIILiteral name);

    void didFinish(ASCIILiteral step);
    void didFinish(ASCIILiteral step, const UTF8CString& note);

private:
    const bool m_logs;
    const ASCIILiteral m_name;
    MonotonicTime m_stepStart;
    Seconds m_threadCPUTimeAtStepStart;
    std::optional<uint64_t> m_instructionsOfThreadAtStepStart;
    uint64_t m_instructionsInsideLoopsAtStepStart;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
