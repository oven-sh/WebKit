/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPhaseTimes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include <wtf/CPUTime.h>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT {

static Seconds processCPUTime()
{
    auto time = CPUTime::get();
    return time ? time->userTime + time->systemTime : 0_s;
}

PhaseTimes::PhaseTimes(BuildContext& build, ASCIILiteral namePrefix, ASCIILiteral name, std::optional<Stage> timedStage)
    : m_build(build)
    , m_namePrefix(namePrefix)
    , m_name(name)
    , m_timedStage(timedStage)
    , m_start(MonotonicTime::now())
    , m_phaseStart(m_start)
{
    if (!m_build.options().logging.logsPhaseTimes) [[likely]]
        return;
    m_processCPUTimeAtPhaseStart = processCPUTime();
}

PhaseTimes::PhaseTimes(BuildContext& build, Stage stage)
    : PhaseTimes(build, ""_s, nameOf(stage), stage)
{
}

PhaseTimes::PhaseTimes(BuildContext& build, PassID pass)
    : PhaseTimes(build, "pass "_s, nameOf(pass), std::nullopt)
{
    ASSERT(pass != PassID::None);
}

PhaseTimes::~PhaseTimes()
{
    if (m_timedStage)
        m_build.statistics().addTime(*m_timedStage, MonotonicTime::now() - m_start);
}

void PhaseTimes::didFinish(ASCIILiteral phase)
{
    if (!m_build.options().logging.logsPhaseTimes) [[likely]]
        return;
    Seconds wallTime = MonotonicTime::now() - m_phaseStart;
    Seconds cpuTime = processCPUTime() - m_processCPUTimeAtPhaseStart;
    dataLogLn("AOT phase times: ", m_namePrefix, m_name, " | ", phase, " | wall ", wallTime.milliseconds(), " ms | cpu ", cpuTime.milliseconds(), " ms");
    m_phaseStart = MonotonicTime::now();
    m_processCPUTimeAtPhaseStart = processCPUTime();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
