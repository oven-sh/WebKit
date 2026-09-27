/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "AOTStubs.h"

namespace JSC {

class CCallHelpers;
class VM;

namespace AOT {

// The common cases of some operations, in machine code: what is too long to have a copy of at every site, and too hot to go to C++
// for. They are stubs like any other (AOTStubs.h), but nothing calls them by that name: a front end stands in for its operation in
// the runtime table. It takes the same arguments and hands back the same results, and whatever it has no quick answer for it
// passes on to the operation, arguments untouched, as if that had been called in the first place. So the compiler knows nothing of
// them, and where there are none (they are written for one kind of CPU) all that is lost is speed.
void installOperationFrontEnds(VM&, void** runtimeTableEntries);

#if CPU(ARM64)
#define AOT_DECLARE_FRONT_END(name) void generateFrontEnd##name(CCallHelpers&);
FOR_EACH_AOT_OPERATION_WITH_FRONT_END(AOT_DECLARE_FRONT_END)
#undef AOT_DECLARE_FRONT_END
#endif

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
