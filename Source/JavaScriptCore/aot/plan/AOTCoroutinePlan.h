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
bool isAsyncFunctionInWhichNothingSuspends(const Program&, const Facts&, FunctionID);
bool isNonYieldingGenerator(const Program&, const Facts&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
