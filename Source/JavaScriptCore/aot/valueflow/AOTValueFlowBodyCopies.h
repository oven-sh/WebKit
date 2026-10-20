/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTValueFlowBodies.h"

namespace JSC { namespace AOT { namespace Typed {

// All callers of a function share its nodes, so a helper that hands a parameter on (to its result, to a call of it, into an object) mixes what its callers pass.
// A call whose callee is one known function of that kind gets a copy of the function's body for itself: nodes, statements and allocation sites. The function's own
// body stays for the callers without a copy. What holds for an operation of the function holds for the body and all its copies together.
// The bodies are those of all functions in the order of their numbers, then the host's.
void copyBodiesIntoCallSites(const Program&, BuildContext&, ParallelArray<ValueFlowBody>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
