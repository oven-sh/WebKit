/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTLimits.h"
#include "AOTPassID.h"
#include "AOTThreads.h"
#include <atomic>
#include <optional>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

// The instructions that the threads of a build retire in user mode, by the hardware counter of each thread.
class InstructionCounts {
public:
    // From here on each thread of the pool counts from the moment it takes a task.
    static void begin() { s_hasBegun.store(true, std::memory_order_relaxed); }
    static bool hasBegun() { return s_hasBegun.load(std::memory_order_relaxed); }
    static void countOnCurrentThread();
    // No system call. In a virtual machine the read traps all the same and takes as long as one: a microsecond.
    static std::optional<uint64_t> ofCurrentThread();
    // Of the threads that count and of those that have ended.
    static std::optional<uint64_t> ofAllThreads();

private:
    static std::atomic<bool> s_hasBegun;
};

class PhaseTimes {
    WTF_MAKE_NONCOPYABLE(PhaseTimes);
public:
    PhaseTimes(BuildContext&, Stage);
    // Only logs: the pass runner adds the time of the pass, and the PhaseTimes of the enclosing stage adds the time of the stage.
    PhaseTimes(BuildContext&, PassID);
    ~PhaseTimes();

    void didFinish(ASCIILiteral phase);

private:
    PhaseTimes(BuildContext&, ASCIILiteral namePrefix, ASCIILiteral name, std::optional<Stage> timedStage, PassID);

    BuildContext& m_build;
    const ASCIILiteral m_namePrefix;
    const ASCIILiteral m_name;
    const std::optional<Stage> m_timedStage;
    const MonotonicTime m_start;
    const CurrentWorkScope m_currentWork;
    MonotonicTime m_phaseStart;
    Seconds m_processCPUTimeAtPhaseStart;
    Seconds m_serialCPUTimeAtPhaseStart;
    BusyThreads::Times m_busyThreadsTimesAtPhaseStart;
    std::optional<uint64_t> m_instructionsAtPhaseStart;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
