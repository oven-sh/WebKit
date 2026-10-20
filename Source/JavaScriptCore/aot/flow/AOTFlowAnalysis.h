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

class FlowObjectTable;
class Program;
class TypeTable;
struct FlowSolution;

// Adds the derived layouts to the program. Nothing else of the program changes before the results are applied.
FlowResults analyzeFlow(Program&, TypeTable&, BuildContext&);
void applyFlowResults(Program&, const FlowResults&, BuildContext&);
void checkLockedBuiltinWrites(const Program&, const FlowResults&, BuildContext&);
void validateFlowResults(const Program&, const FlowResults&, BuildContext&);

FlowResults computeFlowResults(const FlowSolution&, Program&, FlowObjectTable&, TypeTable&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
