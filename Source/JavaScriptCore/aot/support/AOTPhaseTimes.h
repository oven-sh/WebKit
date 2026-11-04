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

// The instructions that the threads of a build retire in user mode, by the hardware counter of each thread.
class InstructionCounts {
public:
    // From here on each thread of the pool counts from the moment it takes a task.
    static void begin() { s_hasBegun.store(true, std::memory_order_relaxed); }
    static bool hasBegun() { return s_hasBegun.load(std::memory_order_relaxed); }
    static void countOnCurrentThread();
    // Of the calling thread alone, which counts from its first call of this or of countOnCurrentThread(). Nothing where the system has no counter or refuses one to the thread.
    static std::optional<uint64_t> ofCurrentThread();
    // Of the threads that count and of those that have ended. Nothing where the system has no counter or refuses one to any thread.
    static std::optional<uint64_t> ofAllThreads();

private:
    static std::atomic<bool> s_hasBegun;
};

// It dies on the thread that made it. Nothing is printed unless LoggingOptions::logsPhaseTimes is set.
class PhaseTimes {
    WTF_MAKE_NONCOPYABLE(PhaseTimes);
public:
    // Adds the time it is alive to the time of the stage, so a second one of the same stage that is alive beside it counts that time twice.
    PhaseTimes(BuildContext&, Stage);
    // Only logs: the PhaseTimes of the enclosing stage adds the time of the stage.
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

// The steps of a phase by the clock alone, for steps of a few milliseconds. A line of PhaseTimes asks the system about every thread and takes the lock of the address space: milliseconds where there are two hundred threads.
// The line of the phase, which counts all of them, comes behind them. Nothing is printed unless LoggingOptions::logsPhaseTimes is set.
class StepTimes {
    WTF_MAKE_NONCOPYABLE(StepTimes);
public:
    StepTimes(const BuildContext&, ASCIILiteral name);

    void didFinish(ASCIILiteral step);
    // The note stands behind the step, in its label: `packObjects: cells: sets 12, the largest of 7 layouts`.
    void didFinish(ASCIILiteral step, const UTF8CString& note);

private:
    const bool m_logs;
    const ASCIILiteral m_name;
    MonotonicTime m_stepStart;
    Seconds m_threadCPUTimeAtStepStart;
    std::optional<uint64_t> m_instructionsOfThreadAtStepStart;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
