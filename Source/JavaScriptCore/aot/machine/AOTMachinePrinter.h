/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocation.h"
#include "AOTMachineFrame.h"
#include "AOTMachineSlots.h"

// The text that tests compare. It names operations by @index, blocks by #index, spills by s<index> and registers as the assembler does, and holds no address, so equal procedures print alike.

namespace JSC { namespace AOT { namespace Machine {

// AOTMachinePrinter.cpp also defines the dump() methods of AOTMachine.h and printInternal() for Opcode, Type, EdgeClass, FailureKind.

// As appended: each block with its Phis, operations, Upsilons and successors, then the operations without position that are used.
//     #2: <- #0 #1
//         Int64 @7 = Phi()
//         Int64 @9 = Load(@7, offset = 16, JSObject_butterfly)
//         Void @10 = Branch(@9) -> #3, rare #4
void printProcedure(PrintStream&, const Procedure&);
// One line, without the newline.
void printOperation(PrintStream&, const Procedure&, const Operation*);

// From orderBlocks() on: the blocks in the order of allocation with loop, rare region and position of the head, then the order of the layout.
void printOrder(PrintStream&, const Procedure&);
// From scanBackward() on: for each operation its flags, definition, last use and hint, and for each loop its pressure.
void printLiveness(PrintStream&, const Procedure&);
// From allocateRegisters() on: for each block the registers at its head, its steps, the moves of its edge and its end.
//     #2: x0 = @7
//         x1 = Load [x0 + 16]            @9
//         save x1 -> s0                  @9
//         Branch x1 -> #3, rare #4       @10
void printAllocation(PrintStream&, const Procedure&);
// From planFrame() on: coverage, size, callee saves, the offset of each spill and StackSlot, the dead words of each suspension, the words that are cleared there and the spills that it holds, the bytes that suspensions keep.
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
