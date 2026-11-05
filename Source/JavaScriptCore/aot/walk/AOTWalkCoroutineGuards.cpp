/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The guard of a coroutine stands in its ramp: the parameters are tested once, in front of the start, and the CoroutineBody takes what the tests have proven however often it is resumed.
// At a suspension: nothing, the ramp never suspends, and either body saves its own frame. At a throw: either body hands back what its role says, so the two starts meet.
// Where a test fails the SlowBody is started, which takes nothing for granted. Withheld (ParameterGuards): the plan gives the function one body, and nothing is built here.

#include "config.h"
#include "AOTWalkCoroutineCalls.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTPlan.h"

namespace JSC { namespace AOT { namespace Typed {

bool rampTestsParametersOf(const CallingConventions& conventions, FunctionID function)
{
    FunctionConvention convention = conventions.of(function);
    return convention.has(CodeRole::CoroutineBody) && convention.has(CodeRole::SlowBody);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
