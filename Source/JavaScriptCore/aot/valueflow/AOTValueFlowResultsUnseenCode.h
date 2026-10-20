/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowAnalysis.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The events of the solved program, in the order of the program: each operation of a reached function that lets in code the build has not seen, or that names a module at run time.
Vector<UnseenCodeRecord> unseenCodeRecordsOf(ValueFlowSolution&);
bool unseenCodeCanRun(const ValueFlowSolution&);
bool modulesAreLoadedByName(const ValueFlowSolution&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
