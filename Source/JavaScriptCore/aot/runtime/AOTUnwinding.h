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
    CallFrame* framePointer { nullptr };
};

enum class CompiledUnwindStatus : uint8_t {
    FoundHandler,
    LeftCompiledCode,
    ReachedEntryFrame,
};

struct ReturnSiteInRun {
    void* rawPC { nullptr };
    void* stackPointer { nullptr };
    CallFrame* framePointer { nullptr };
    CallFrame* callee { nullptr };
};

struct CompiledUnwindResult {
    CompiledUnwindStatus status { CompiledUnwindStatus::LeftCompiledCode };
    bool hasLeftRun { false };
    CompiledHandler handler;
    ReturnSiteInRun siteOfHandler;
    CompiledFrameIterator::RunEnd runEnd;
    CompiledFrameExit exit;
};

ALWAYS_INLINE bool needsExceptionForEveryThrow(const Instance& instance)
{
    return instance.globalObject->debugger() || Options::breakOnThrow();
}

CompiledUnwindResult unwindCompiledFrames(VM&, const CompiledFrameIterator&, VMEntryRecord&, bool isTermination);
CompiledUnwindResult unwindRun(VM&, const ReturnSiteInRun&, EntryFrame*, VMEntryRecord&, bool isTermination, CallFrame* asyncGeneratorBodyFrameLeft = nullptr);
CompiledUnwindResult unwindRunOfThrow(VM&, Instance& instanceThatThrows, const ReturnSiteInRun&, EntryFrame*, VMEntryRecord&);
void unwindBeyondCompiledFrames(VM&, CallFrame* frameRecord, CompiledUnwindStatus);
bool isCaughtByCompiledCode(VM&, CompiledFrameIterator);
Exception* exceptionKeptByCleanup(uint64_t kept, JSValue thrown);
Exception::StackCaptureAction stackCaptureActionOfThrow(VM&, const Instance&, JSValue thrown, const CallerFrameAndPC* frameRecordOfOperation, const CompiledUnwindResult* runWithoutHandler);
bool nearestHandlerIgnoresValue(VM&, const Instance&, CallFrame* stubFrame);
std::optional<HandlerType> findNearestHandler(VM&, CallFrame* stubFrame);
bool isCaughtAboveCleanup(VM&, const ReturnSiteInRun& siteOfCleanup);
bool beginsInTopCompiledFrame(VM&, const ReturnAddressTrace&);

// Only where a collection may run: vm.topCallFrame is stale in an operation that records no frame.
const void* returnAddressOfInnermostCompiledCall(VM&);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
