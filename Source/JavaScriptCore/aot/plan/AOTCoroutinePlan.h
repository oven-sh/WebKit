/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;
// chooseRolesOfConvention(): THE writer of FunctionConvention::coroutineNeverSuspends. Async, no generator, no body of a module, the summary without SummaryTrait::Suspends, TypedOperations not withheld. False claims nothing.
bool isAsyncFunctionInWhichNothingSuspends(const Program&, const Facts&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
