/*
 * Copyright (C) 2013, 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include "Register.h"
#include "StackAlignment.h"
#include <wtf/Assertions.h>
#include <wtf/StackPointer.h>

namespace JSC {

// The maxFrameExtentForSlowPathCall is the max amount of stack space (in bytes)
// that can be used for outgoing args when calling a slow path C function
// from JS code.

#if !ENABLE(ASSEMBLER)
static constexpr size_t maxFrameExtentForSlowPathCall = 0;

#elif CPU(X86_64)
// All args in registers. Windows also uses System V ABI.
static constexpr size_t maxFrameExtentForSlowPathCall = 0;

#elif CPU(ARM64) || CPU(ARM64E) || CPU(RISCV64)
// All args in registers.
static constexpr size_t maxFrameExtentForSlowPathCall = 0;

#else
#error "Unsupported CPU: need value for maxFrameExtentForSlowPathCall"

#endif

static_assert(!(maxFrameExtentForSlowPathCall % sizeof(Register)), "Extent must be in multiples of registers");

#if ENABLE(ASSEMBLER)
// Make sure that cfr - maxFrameExtentForSlowPathCall bytes will make the stack pointer aligned
static_assert((maxFrameExtentForSlowPathCall % 16) == 16 - sizeof(CallerFrameAndPC), "Extent must align stack from callframe pointer");
#endif

static constexpr size_t maxFrameExtentForSlowPathCallInRegisters = maxFrameExtentForSlowPathCall / sizeof(Register);

// The slow paths for calls (llint_default_call() and the like, operationDefaultCall() and the like) run with the stack pointer at
// the callee's frame: their own frame lies over whatever the last callee at that depth left there, and sanitizeStackForVM(),
// which they call, clears only what is below that frame. What such a frame does not write stays where the conservative scan of
// a later, shallower callee's native frames finds it. So the thunks that call those slow paths clear this much of the stack
// first, before any C++ frame exists: room for the frame of the function they call (152 bytes at most in a release build for
// x86_64, a few times that without optimization or with ASan); everything that function calls is below what it clears itself.
#if ASSERT_ENABLED || ASAN_ENABLED
static constexpr size_t stackBytesClearedForCallSlowPath = 2048;
#else
static constexpr size_t stackBytesClearedForCallSlowPath = 256;
#endif
static_assert(!(stackBytesClearedForCallSlowPath % 64), "the LLInt clears it 64 bytes at a time");

// For the first thing such a slow path does after sanitizeStackForVM(): its frame must not reach below what its thunk cleared.
// (The thunk's frame ends at the callee's frame, less maxFrameExtentForSlowPathCall.)
#if ASSERT_ENABLED
#define ASSERT_CALL_SLOW_PATH_RUNS_IN_CLEARED_STACK(calleeFrame) \
    ASSERT(std::bit_cast<uintptr_t>(currentStackPointer()) + maxFrameExtentForSlowPathCall + stackBytesClearedForCallSlowPath >= std::bit_cast<uintptr_t>(calleeFrame))
#else
#define ASSERT_CALL_SLOW_PATH_RUNS_IN_CLEARED_STACK(calleeFrame) ((void)0)
#endif

} // namespace JSC
