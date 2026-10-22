/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;
class TypeTable;

// The stage AnalyzeFlow: solves the program and fills the tables that the compiler reads from then on. What the solve does not answer stays unknown, and applying an unknown record changes nothing.
// A build that admits code which the compiler never sees is not solved: its tables state nothing.
// A test may ask for one line about each set that names an object outside the type as read of the place where it arrives.
FlowResults analyzeValueFlow(Program&, TypeTable&, BuildContext&, Vector<String>* setsOutsideTypes = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
