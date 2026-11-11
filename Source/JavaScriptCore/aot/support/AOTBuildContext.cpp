/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuildContext.h"

#if ENABLE(AOT)

#include "AOTFormatPatternPlanner.h"
#include "AOTRegExpSummary.h"
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BuildContext);

BuildContext::BuildContext(const Wire::CompileOptions& wire, const HostThreadPool* hostPool)
    : m_options(CompileOptions::fromWire(wire, m_diagnostics))
    , m_assemblerPolicy(AssemblerPolicy::forImage(CPUFeatureSet::fromBits(static_cast<uint32_t>(m_options.recorded.targetFeatures))))
    , m_parallelLoops(m_options.threadCount, hostPool)
    , m_regExpSummaries(makeUnique<RegExpSummaries>(static_cast<unsigned>(Limits::regExpParenthesesDepth)))
    , m_formatPatternPlans(makeUniqueWithoutFastMallocCheck<Typed::FormatPatternPlans>())
{
}

BuildContext::~BuildContext() = default;

bool BuildContext::exceeds(LimitID id, uint64_t amount, const Subject&)
{
    return amount > limit(id);
}

void BuildContext::reportLimitExceeded(LimitID id, uint64_t needed, const Subject& subject)
{
    m_diagnostics.add(UserDiagnostic {
        .kind = UserDiagnosticKind::LimitExceeded,
        .function = subject.function,
        .block = subject.block,
        .value = subject.value,
        .layout = subject.layout,
        .limit = id,
        .count = clampTo<uint32_t>(needed),
        .location = subject.location,
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
