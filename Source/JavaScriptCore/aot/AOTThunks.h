/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubs.h"

namespace JSC {

class CCallHelpers;
class VM;

namespace AOT {

// The common cases of some operations, in machine code: sequences that are too long to duplicate at every site and too hot to leave
// to C++. They are stubs like any other (AOTStubs.h), but nothing calls them by name: a front end replaces its operation in the
// runtime table. It takes the same arguments and returns the same results, and tail-calls the operation, with the arguments
// untouched, for anything that it cannot handle. So the compiler does not know about them, and on a CPU that they are not written
// for, only speed is lost.
void installOperationFrontEnds(VM&, void** runtimeTableEntries);

#if CPU(ARM64)
#define AOT_DECLARE_FRONT_END(name) void generateFrontEnd##name(CCallHelpers&);
FOR_EACH_AOT_OPERATION_WITH_FRONT_END(AOT_DECLARE_FRONT_END)
#undef AOT_DECLARE_FRONT_END
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
