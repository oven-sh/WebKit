/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"
#include "AOTValueFlow.h"

namespace JSC { namespace AOT { namespace Typed {

void runFlowTestCommand(TestCommandContext&);
void runFlowReportTestCommand(TestCommandContext&);
void runValueFlowBodyTestCommand(TestCommandContext&);
void runValueFlowBodiesTestCommand(TestCommandContext&);
void runValueFlowSolveTestCommand(TestCommandContext&);
void runValueFlowFamiliesTestCommand(TestCommandContext&);
void runValueFlowWhyTestCommand(TestCommandContext&);
void runValueFlowResultsTestCommand(TestCommandContext&);
void runValueFlowApplyTestCommand(TestCommandContext&);

void prepareForFlow(TestCommandContext&);
ASCIILiteral nameOf(ValueFlowStatementKind);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
