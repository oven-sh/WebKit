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
void runValueFlowCompareTestCommand(TestCommandContext&);
void runValueFlowFamiliesTestCommand(TestCommandContext&);
void runValueFlowWhyTestCommand(TestCommandContext&);
void runValueFlowResultsTestCommand(TestCommandContext&);
void runValueFlowApplyTestCommand(TestCommandContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
