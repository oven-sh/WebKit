/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTLeaveRows.h"
#include "AOTStubKey.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT {

// A fast body leaves for good. The code that leaves has pushed RegistersLaidDownAtLeave::addresses and calls: the return address names the unit and the site. Every other register is as the row of the point says.
constexpr StubKey leaveForSlowBody() { return StubKey(StubFamily::LeaveForSlowBody); }

// The same for a row that comes back (EndOfLeave::ComesBack): copies of callees are left and the fast body is not. It returns like a call: what the outermost part of the row has returned lies where that unit returns it,
// the stack pointer stands as in front of the pushes, and every register that a call may change has changed.
constexpr StubKey enterSlowBodiesAndComeBack() { return StubKey(StubFamily::EnterSlowBodiesAndComeBack); }

// Where the slow body of a constructor returns to whose construction stood as a copy in the fast body that was left. Nobody calls it: its address is the one that the leaving code pushes for a frame of two words
// between the constructor's frame and its caller's, with `this` in the first. It hands on what the constructor returned if that is an object, else `this`.
constexpr StubKey selectResultOfConstruction() { return StubKey(StubFamily::SelectResultOfConstruction); }
// The address to push is that of the stub and this many bytes. In front of it stands a call that never runs: a hardened build ends the process at a return address in a stub that follows no call (faultOfReturnAddressInRun()).
static constexpr unsigned bytesInFrontOfReturnIntoStubOfLeaving = isX86_64() ? 2 : 4;
static constexpr unsigned bytesInFrontOfReturnIntoSelectResultOfConstruction = bytesInFrontOfReturnIntoStubOfLeaving;
static constexpr unsigned wordsOfFrameThatSelectsResultOfConstruction = 2;
static constexpr unsigned slotOfThisInFrameThatSelectsResultOfConstruction = 1;

// A CHAIN OF COPIES, BY WORDS. Where the slow body of a callee returns to whose copy stood in a function that was left with it. Nobody calls the stub: its address, and bytesInFrontOfReturnIntoStubOfLeaving more,
// is the own address of a frame of the row that stands for the caller until the callee has returned. The frame holds what the head of the caller's slow body takes, and what the operation still owes in whose place the copy stood.
// The stub pays that, puts the value of the operation into its word of the caller's cell, takes the frame down and enters the head in its place, by a jump: under the next such frame, under the fast body, or under its caller.
constexpr StubKey goOnInSlowBodyOfCaller() { return StubKey(StubFamily::GoOnInSlowBodyOfCaller); }

// The slots of that frame. addFrameThatGoesOn() of codegen/AOTLeaveRowsOfCode.h fills them.
struct FrameThatGoesOn {
    // PlaceInLeaveRow::Address: the primary entry of the caller's slow body.
    static constexpr unsigned slotOfHead = 1;
    // The cell of the caller's heap words.
    static constexpr unsigned slotOfCell = 2;
    // From the cell to the word of the operation, in bytes. Zero: what was returned is dropped: nobody reads the operation, or it is a store.
    static constexpr unsigned slotOfOffsetOfWord = 3;
    // Of a construction: `this`, which is the value of the operation unless an object was returned. Else zero.
    static constexpr unsigned slotOfThis = 4;
    // A FormInLeaveRow: how the callee returns its result. A function whose result Link has proven returns it raw, its slow body too: a Double in the result register of that bank.
    static constexpr unsigned slotOfFormOfResult = 5;
    static constexpr unsigned slotOfNumberOfStackWords = 6;
    // directArgumentGPR(i) takes slot firstSlotOfRegisters + i, stack word j takes slot firstSlotOfStackWords + j.
    static constexpr unsigned firstSlotOfRegisters = 7;
    static constexpr unsigned firstSlotOfStackWords = firstSlotOfRegisters + numberOfDirectArgumentGPRs;

    // `stackWordsOfPartBelow`: they are the lowest words of this frame.
    static constexpr unsigned wordsFor(unsigned numberOfStackWords, unsigned stackWordsOfPartBelow) { return (firstSlotOfStackWords - 1 + numberOfStackWords + stackWordsOfPartBelow + 1) & ~1u; }
};

// The index of the first word of the row among the words of the unit: LeaveRowWriter::finish(). No value of a function ever lies in this register.
static constexpr GPRReg rowOfLeaveGPR = throwWordGPR;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
