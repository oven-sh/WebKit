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

using ListenerForSetsOutsideTypes = ScopedLambda<void(const String& place, const Type& held, const Type& declared)>;

FlowResults analyzeValueFlow(Program&, TypeTable&, BuildContext&, const ListenerForSetsOutsideTypes* = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
