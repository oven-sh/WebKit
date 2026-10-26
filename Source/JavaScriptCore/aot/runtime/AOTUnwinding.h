/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledFrame.h"
#include "AOTInstance.h"
#include "Exception.h"
#include "VM.h"

namespace JSC {

class VM;
struct VMEntryRecord;

namespace AOT {

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

struct CompiledUnwindResult {
    CompiledUnwindStatus status;
    CompiledHandler handler;
};

bool needsExceptionForEveryThrow(const Instance&);
std::optional<CompiledFrameIterator> findCompiledCallers(VM&, const Instance&, CallFrame* stubFrame);
CompiledUnwindResult unwindCompiledFrames(VM&, CompiledFrameIterator&, VMEntryRecord&, bool isTermination);
// For a throw in compiled code, after unwindCompiledFrames() has found no handler in the frames from findCompiledCallers(). The exception is pending.
void unwindBeyondCompiledFrames(VM&, CallFrame* frameRecord, CompiledUnwindStatus);
bool isCaughtByCompiledCode(VM&, CompiledFrameIterator);
// For what a `throw` of the program throws out of a run of compiled frames. The frames are walked only if somebody may list them.
Exception::StackCaptureAction stackCaptureActionOfThrow(VM&, const Instance&, const CallerFrameAndPC* frameRecordOfOperation, const CompiledFrameIterator* framesAtEndOfRun);
bool nearestHandlerIgnoresValue(VM&, const Instance&, CallFrame* stubFrame);

inline constexpr ptrdiff_t offsetOfDivertedReturnInLoopPollPage = 8;
CallFrame* findReturnIntoFunctionWithoutHandlers(VM&, CallFrame* operationFrame);
void throwOutOfMemoryErrorAtReturnOf(Instance&, CallFrame* frameThatReturns);
void throwOwedOutOfMemoryErrorAtReturnOfOperation(Instance&, CallerFrameAndPC* frameRecordOfOperation);

class ThrowsOwedOutOfMemoryErrorAtReturn {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(ThrowsOwedOutOfMemoryErrorAtReturn);
public:
    ALWAYS_INLINE ThrowsOwedOutOfMemoryErrorAtReturn(Instance* instance, CallerFrameAndPC* frameRecordOfOperation)
        : m_instance(*instance)
        , m_frameRecordOfOperation(frameRecordOfOperation)
    {
    }

    ALWAYS_INLINE ~ThrowsOwedOutOfMemoryErrorAtReturn()
    {
        if (m_instance.vm->traps().needsOutOfMemoryError()) [[unlikely]]
            throwOwedOutOfMemoryErrorAtReturnOfOperation(m_instance, m_frameRecordOfOperation);
    }

private:
    Instance& m_instance;
    CallerFrameAndPC* m_frameRecordOfOperation;
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
