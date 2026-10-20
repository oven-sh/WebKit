/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowRecords.h"
#include "AOTFlowResults.h"
#include <utility>
#include <wtf/PrintStream.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

ASCIILiteral nameOf(FlowContext);
ASCIILiteral nameOf(FlowCertainty);
ASCIILiteral nameOf(FlowKeyClass);
ASCIILiteral nameOf(FlowObjectKind);
ASCIILiteral nameOf(FlowSiteKind);
ASCIILiteral nameOf(FlowSiteOrigin);
ASCIILiteral nameOf(FlowNodeKind);
ASCIILiteral nameOf(FlowInstanceState);
ASCIILiteral nameOf(FlowStatementKind);
ASCIILiteral nameOf(FlowSeedKind);
ASCIILiteral nameOf(FlowExternalKind);
ASCIILiteral nameOf(FlowFilterKind);
ASCIILiteral nameOf(FlowAccessKind);
ASCIILiteral nameOf(FlowReadUse);
ASCIILiteral nameOf(FlowAccessFlag);
ASCIILiteral nameOf(FlowCallMode);
ASCIILiteral nameOf(FlowCallFlag);
ASCIILiteral nameOf(FlowIterationStep);
ASCIILiteral nameOf(FlowModuleOperationKind);
ASCIILiteral nameOf(FlowClosureUse);
ASCIILiteral nameOf(FlowClosureCapture);
ASCIILiteral nameOf(FlowSummaryFlag);
ASCIILiteral nameOf(FlowObjectUse);
ASCIILiteral nameOf(FlowObjectEvent);
ASCIILiteral nameOf(FlowLocationEvent);
ASCIILiteral nameOf(FlowCallLinkKind);
ASCIILiteral nameOf(FlowIndirectCallKind);
ASCIILiteral nameOf(LegacyRegExpStatics);
ASCIILiteral nameOf(ProgramInstances);

template<typename Flag>
void dumpFlags(PrintStream& out, OptionSet<Flag> flags)
{
    out.print("[");
    bool isFirst = true;
    for (Flag flag : flags) {
        if (!std::exchange(isFirst, false))
            out.print(", ");
        out.print(nameOf(flag));
    }
    out.print("]");
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
