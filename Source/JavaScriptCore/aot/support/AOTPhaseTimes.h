/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPassID.h"
#include <optional>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;

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
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
