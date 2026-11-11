/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineSlots.h"

namespace JSC { namespace AOT { namespace Machine {

void printProcedure(PrintStream&, const Procedure&);
void printOperation(PrintStream&, const Procedure&, const Operation*);
void printFailure(PrintStream&, const Procedure&);

void printOrder(PrintStream&, const Procedure&);
void printLiveness(PrintStream&, const Procedure&);
void printAllocation(PrintStream&, const Procedure&);
void printFrame(PrintStream&, const Procedure&);

void printStep(PrintStream&, const Procedure&, const Step&);

} } } // namespace JSC::AOT::Machine

namespace WTF {

void printInternal(PrintStream&, JSC::AOT::Machine::Opcode);
void printInternal(PrintStream&, JSC::AOT::Machine::Type);
void printInternal(PrintStream&, JSC::AOT::Machine::EdgeClass);
void printInternal(PrintStream&, JSC::AOT::Machine::FailureKind);
void printInternal(PrintStream&, JSC::AOT::Machine::StepKind);
void printInternal(PrintStream&, JSC::AOT::Machine::FrameCoverage);

} // namespace WTF

#endif // ENABLE(AOT)
