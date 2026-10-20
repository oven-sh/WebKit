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

// Solves the program and fills the tables that the stage AnalyzeFlow hands to the compiler. What the solve does not answer stays unknown.
// A test may ask for one line about each set that names an object outside the type as read of the place where it arrives.
FlowResults analyzeValueFlow(Program&, TypeTable&, BuildContext&, Vector<String>* setsOutsideTypes = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
