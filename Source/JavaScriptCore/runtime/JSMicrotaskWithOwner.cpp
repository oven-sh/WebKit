/*
 * Copyright (C) 2026 Codeblog Corp. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if USE(BUN_JSC_ADDITIONS)

// runInternalMicrotaskWithOwner(): what runs a VM's jobs once async code has owners
// (VM::internalMicrotaskRunner()). It is runInternalMicrotask() with the scope that knows of owners
// (MicrotaskAsyncContextSwapScope), and is made by compiling JSMicrotask.cpp again, so that JSMicrotask.cpp itself,
// which runs the jobs of every program that has no owner, is compiled to what it was before there were owners.
// What else JSMicrotask.cpp defines is made again with it, under another name, for this one to call.

#include "JSMicrotask.h"

#define JSC_MICROTASK_RUNNER_HAS_OWNER 1
#define runInternalMicrotask runInternalMicrotaskWithOwner
#define asyncGeneratorAwaitReturn asyncGeneratorAwaitReturnWithOwner
#define asyncGeneratorResume asyncGeneratorResumeWithOwner
#define enqueueAsyncGeneratorDriver enqueueAsyncGeneratorDriverWithOwner
#define asyncIteratorNextWithDriver asyncIteratorNextWithDriverWithOwner
#define asyncModuleResolveEvaluation asyncModuleResolveEvaluationWithOwner
#define asyncFunctionDrive asyncFunctionDriveWithOwner

#include "JSMicrotask.cpp"

#endif // USE(BUN_JSC_ADDITIONS)
