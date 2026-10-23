/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTVector.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;
class TypeTable;
struct Type;

// Told of each set that names an object outside the type of the place where it arrives: one call at a time, in no order.
using ListenerForSetsOutsideTypes = ScopedLambda<void(const String& place, const Type& held, const Type& declared)>;

// The stage AnalyzeFlow: solves the program and fills the tables that the compiler reads from then on.
FlowResults analyzeValueFlow(Program&, TypeTable&, BuildContext&, const ListenerForSetsOutsideTypes* = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
