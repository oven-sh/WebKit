/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowReport.h"
#include "AOTFlowResults.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

using ListenerForSetsOutsideTypes = ScopedLambda<void(const String& place, const Type& held, const Type& declared)>;

// `goesOn` is asked once, as soon as its lists are known. False: nobody reads the results, so they say nothing.
FlowResults analyzeValueFlow(Program&, TypeTable&, BuildContext&, const ListenerForSetsOutsideTypes* = nullptr, const JudgeOfUsesOfLibraryCode* goesOn = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
