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

// The stage AnalyzeFlow: solves the program and fills the tables that the compiler reads from then on.
FlowResults analyzeValueFlow(Program&, TypeTable&, BuildContext&, Vector<String>* setsOutsideTypes = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
