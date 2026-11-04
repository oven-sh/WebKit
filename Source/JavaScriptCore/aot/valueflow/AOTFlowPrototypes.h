/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIRFunction.h"

namespace JSC { namespace AOT { namespace Typed {

BuiltinClassID classOfFunctionObject(const Function&);
BuiltinID prototypeOfFunctionObject(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
