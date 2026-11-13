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

bool stubCanRunCallback(const Program&, const Facts&, FunctionID method, FunctionID callback, bool hasInitialValue);
bool stubCanRunCallback(const Program&, const Facts&, BuiltinID method, FunctionID callback, bool hasInitialValue);
unsigned elementParameterOfCallback(const Program&, FunctionID method);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
