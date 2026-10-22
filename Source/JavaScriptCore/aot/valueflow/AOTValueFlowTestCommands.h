/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"

namespace JSC { namespace AOT { namespace Typed {

void runValueFlowBodyTestCommand(TestCommandContext&);
void runValueFlowBodiesTestCommand(TestCommandContext&);
void runValueFlowSolveTestCommand(TestCommandContext&);
void runValueFlowFamiliesTestCommand(TestCommandContext&);
void runValueFlowWhyTestCommand(TestCommandContext&);
void runValueFlowResultsTestCommand(TestCommandContext&);
void runValueFlowApplyTestCommand(TestCommandContext&);
void runRunsNoCodeTestCommand(TestCommandContext&);
void runRelationalSummaryTestCommand(TestCommandContext&);
void runOriginsBeforeFlowTestCommand(TestCommandContext&);
void runFatesAtCallsTestCommand(TestCommandContext&);

// The word `split` of valueflow-solve and valueflow-why: the passes that the pipeline runs between the read stage and the analysis run first, so the analysis sees what it sees in a build:
// the positions that a check taints are guarded, and a function of the program is split into the tests of its arguments, its typed body and its generic version.
void prepareAsInFrontOfFlow(TestCommandContext&);
// What every valueflow-* command runs first: GuardTaintedPositions, as Pipeline::runFlowStage() does. `unguarded` skips it, `split` is prepareAsInFrontOfFlow().
void prepareForFlow(TestCommandContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
