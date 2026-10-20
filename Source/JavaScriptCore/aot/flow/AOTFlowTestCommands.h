/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTestCommand.h"

namespace JSC { namespace AOT { namespace Typed {

struct FlowSolvedProgram;

void runFlowObjectSetTestCommand(TestCommandContext&);
void runFlowValueTestCommand(TestCommandContext&);
void runFlowKeySetTestCommand(TestCommandContext&);
void runFlowSolveTestCommand(TestCommandContext&);
void runFlowQueryTestCommand(TestCommandContext&);
void printUnknownObjectOrigins(PrintStream&, const FlowSolvedProgram&);
void printUnknownObjectOriginsOfApplication(PrintStream&, const FlowSolvedProgram&);
void printObjectMutationsByWitness(PrintStream&, const FlowSolvedProgram&);
void printAccessSites(PrintStream&, const FlowSolvedProgram&);
void printFunctionInSolvedProgram(PrintStream&, const FlowSolvedProgram&, FunctionID);

void runFlowSitesTestCommand(TestCommandContext&);

void runFlowPropertyWalkTestCommand(TestCommandContext&);
void runFlowConstructionTestCommand(TestCommandContext&);

void runFlowSummaryTestCommand(TestCommandContext&);
void runFlowCallLinksTestCommand(TestCommandContext&);
void runFlowOrderTestCommand(TestCommandContext&);

void runFlowTestCommand(TestCommandContext&);
void runFlowApplyTestCommand(TestCommandContext&);
void runFlowValidateTestCommand(TestCommandContext&);
void runFlowReportTestCommand(TestCommandContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
