/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

// The instructions that the threads of a build retire in user mode, by the hardware counter of each thread. A time moves with the load of the machine, and a number of instructions does not.
// Only a build that logs its phase times counts. There are none where the system refuses the counter, and on any system but Linux on x86-64.
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
    PhaseTimes(BuildContext&, ASCIILiteral namePrefix, ASCIILiteral name, std::optional<Stage> timedStage);

    BuildContext& m_build;
    const ASCIILiteral m_namePrefix;
    const ASCIILiteral m_name;
    const std::optional<Stage> m_timedStage;
    const MonotonicTime m_start;
    MonotonicTime m_phaseStart;
    Seconds m_processCPUTimeAtPhaseStart;
    Seconds m_serialCPUTimeAtPhaseStart;
    BusyThreads::Times m_busyThreadsTimesAtPhaseStart;
    std::optional<uint64_t> m_instructionsAtPhaseStart;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
