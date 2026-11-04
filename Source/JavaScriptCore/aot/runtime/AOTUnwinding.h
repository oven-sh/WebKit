/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTInstance.h"
#include "Exception.h"
#include "HandlerInfo.h"
#include "JSGlobalObject.h"
#include "Options.h"
#include "VM.h"

namespace JSC {

class VM;
struct VMEntryRecord;

namespace AOT {

struct ReturnAddressTrace;

enum class CompiledHandlerKind : uint8_t {
    Catch,
    Finally,
    RejectPromise,
};

struct CompiledHandler {
    CallFrame* callFrame { nullptr };
    void* code { nullptr };
    CompiledHandlerKind kind { CompiledHandlerKind::Catch };
    // What the frame pointer register holds in the frame: callFrame, if the frame has a record.
    CallFrame* framePointer { nullptr };
};

enum class CompiledUnwindStatus : uint8_t {
    FoundHandler,
    LeftCompiledCode,
    ReachedEntryFrame,
};

// A return address in a run of compiled frames, and what the two registers hold in the frame that it runs in.
struct ReturnSiteInRun {
    void* rawPC { nullptr };
    void* stackPointer { nullptr };
    CallFrame* framePointer { nullptr };
    // The frame that returns there, where the walk has seen it.
    CallFrame* callee { nullptr };
};

struct CompiledUnwindResult {
    CompiledUnwindStatus status { CompiledUnwindStatus::LeftCompiledCode };
    // False where compiled code has not called the frame that the walk began at.
    bool hasLeftRun { false };
    CompiledHandler handler;
    ReturnSiteInRun siteOfHandler;
    // Of a run that has no handler.
    CompiledFrameIterator::RunEnd runEnd;
    CompiledFrameExit exit;
};

ALWAYS_INLINE bool needsExceptionForEveryThrow(const Instance& instance)
{
    return instance.globalObject->debugger() || Options::breakOnThrow();
}

CompiledUnwindResult unwindCompiledFrames(VM&, const CompiledFrameIterator&, VMEntryRecord&, bool isTermination);
// Leaves the frames of the run from the site up to the nearest handler.
CompiledUnwindResult unwindRun(VM&, const ReturnSiteInRun&, EntryFrame*, VMEntryRecord&, bool isTermination, CallFrame* asyncGeneratorBodyFrameLeft = nullptr);
// The same from the operation of a throw in compiled code, which has the instance of the run in a register.
CompiledUnwindResult unwindRunOfThrow(VM&, Instance& instanceThatThrows, const ReturnSiteInRun&, EntryFrame*, VMEntryRecord&);
// The same for a throw in a VM that runs several images, from the operation of the throw. It goes on where compiled code of another image has called the run: no frame of the engine
// lies between the two, so they are as good as one run. `instance` is that of the code that throws, and afterwards that of the last run, which has the handler if there is one.
CompiledUnwindResult unwindRunsOfImages(VM&, const ReturnSiteInRun&, EntryFrame*, VMEntryRecord&, Instance*& instance);
// For a throw in compiled code, after unwindRun() has found no handler in the callers of the stub that asks. The exception is pending.
void unwindBeyondCompiledFrames(VM&, CallFrame* frameRecord, CompiledUnwindStatus);
bool isCaughtByCompiledCode(VM&, CompiledFrameIterator);
// For the Throw at the end of a cleanup that has kept something: the Exception of the throw that entered the cleanup, if the cleanup throws on the same value.
Exception* exceptionKeptByCleanup(uint64_t kept, JSValue thrown);
// For what a `throw` of the program throws out of a run of compiled frames. The frames are walked only if somebody may list them.
Exception::StackCaptureAction stackCaptureActionOfThrow(VM&, const Instance&, JSValue thrown, const CallerFrameAndPC* frameRecordOfOperation, const CompiledUnwindResult* runWithoutHandler);
bool nearestHandlerIgnoresValue(VM&, const Instance&, CallFrame* stubFrame);
// Of the run of compiled frames that has called the frame. Nothing is no answer.
std::optional<HandlerType> findNearestHandler(VM&, CallFrame* stubFrame);
// Under validation, before a throw is unwound. A handler is found by the site of the call that the throw leaves.
void JIT_OPERATION_ATTRIBUTES checkThatThrowingCallHasSite(Instance*, CallFrame* stubFrame);
// For a throw whose nearest handler takes the Exception and throws it on.
bool isCaughtAboveCleanup(VM&, const ReturnSiteInRun& siteOfCleanup);
// Whether the trace was taken where native code now stands. True where compiled code has not called it.
bool beginsInTopCompiledFrame(VM&, const ReturnAddressTrace&);

// Of the innermost call in compiled code on the stack. Native and interpreter frames may lie above it. Null if no compiled code is running.
// Only where a collection may run: vm.topCallFrame is stale in an operation that records no frame.
const void* returnAddressOfInnermostCompiledCall(VM&);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
