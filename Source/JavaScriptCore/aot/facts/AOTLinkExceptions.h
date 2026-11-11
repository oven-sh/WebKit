/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

ALWAYS_INLINE FlowNodeID nodeTakingWhatIsThrown(const LinkFlow& flow, FunctionID function, uint32_t thrownTo)
{
    return thrownTo == noSummaryNode ? flow.thrown(function) : flow.nodeOf(function, thrownTo);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
