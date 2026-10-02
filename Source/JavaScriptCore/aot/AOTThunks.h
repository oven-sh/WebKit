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

void installOperationFrontEnds(VM&, void** runtimeTableEntries);

#if CPU(ARM64)
#define AOT_DECLARE_FRONT_END(name) void generateFrontEnd##name(CCallHelpers&);
FOR_EACH_AOT_OPERATION_WITH_FRONT_END(AOT_DECLARE_FRONT_END)
#undef AOT_DECLARE_FRONT_END
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
