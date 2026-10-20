/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class TypeArena;

// Whether a closure of a bound function can stand for what bind makes of a closure of the target.
bool canCreateBoundFunction(const Program&, FunctionID target);

// A closure of the function captures what a closure of the target captures, the bound this if the target takes one, the bound arguments that the target has parameters for, and the closure of the target.
inline constexpr unsigned numberOfBoundThisAndTargetCaptures = 2;

Function createBoundFunction(const Program&, const FlowResults&, TypeArena&, BuildContext&, FunctionID target, uint32_t numberOfBoundArguments);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
