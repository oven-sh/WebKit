/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

// For thrownNode() of CallInLinkCalls and of AccessInLinkAccesses, where the record is no shared one. `thrownTo`: SummaryCall::thrownTo, SummaryAccess::thrownTo.
FlowNodeID nodeTakingWhatIsThrown(const LinkFlow&, FunctionID, uint32_t thrownTo);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
