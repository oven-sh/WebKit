/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowStamps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

FlowAccessSiteNumbering numberAccessSites(const Program& program, BuildContext& build)
{
    unsigned count = program.functions.size();
    FlowAccessSiteNumbering numbering;
    numbering.firstSiteOfFunction = Vector<uint32_t>(FillWith { }, count + 1, 0u);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        uint32_t numberOfSites = 0;
        forEachAccessSiteValue(program.functions[index], [&](ValueID, uint32_t) {
            ++numberOfSites;
        });
        numbering.firstSiteOfFunction[index + 1] = numberOfSites;
    });
    for (unsigned index = 0; index < count; ++index)
        numbering.firstSiteOfFunction[index + 1] += numbering.firstSiteOfFunction[index];
    return numbering;
}

Vector<AccessSiteID> accessSitesByValue(const Function& function, const FlowAccessSiteNumbering& numbering, FunctionID functionAsRead)
{
    Vector<AccessSiteID> sites(function.values.size());
    forEachAccessSiteValue(function, [&](ValueID value, uint32_t ordinal) {
        sites[value.index()] = numbering.siteOf(functionAsRead, ordinal);
    });
    return sites;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
