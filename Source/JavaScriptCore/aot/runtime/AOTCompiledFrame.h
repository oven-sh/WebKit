/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "AOTCompiledCaller.h"

#if ENABLE(AOT)

#include "AOTCallSiteCache.h"
#include "AOTCompiledFrameExit.h"
#include "CallFrame.h"
#include "LineColumn.h"
#include <bit>
#include <optional>
#include <wtf/StackBounds.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class SourceOrigin;
class VM;
struct EntryFrame;

namespace AOT {

struct Instance;

// A frame of compiled code is named by the place of its frame record, the word below its return address, whether it has a record or not. stackPointer and framePointer are what the two registers hold in the frame at the return site.
ALWAYS_INLINE CallFrame* frameAtReturnSite(const ImageFrame& frame, const void* stackPointer, CallFrame* framePointer)
{
    if (frame.hasFrameRecord())
        return framePointer;
    if (frame.isEmpty()) [[unlikely]]
        return std::bit_cast<CallFrame*>(std::bit_cast<uintptr_t>(stackPointer) - sizeof(void*));
    return std::bit_cast<CallFrame*>(std::bit_cast<uintptr_t>(stackPointer) + frame.frameSizeInBytes());
}

// A run of compiled code ends at the frame of one of these.
constexpr bool isAdapterFrame(StubFrameKind kind)
{
    return kind == StubFrameKind::Adapter || kind == StubFrameKind::ConstructAdapter || kind == StubFrameKind::SavedFrameAdapter || kind == StubFrameKind::HandlerOfJob;
}

constexpr ImageFrame frameAtStubReturnSite(StubFrameKind kind)
{
    return kind == StubFrameKind::AtStubEntry ? ImageFrame::empty() : ImageFrame { };
}

// What the stack pointer is once the frame has returned.
ALWAYS_INLINE void* stackPointerOfCaller(const CallFrame* frame, unsigned bytesAboveReturnAddress = 0)
{
    return std::bit_cast<void*>(std::bit_cast<uintptr_t>(frame) + sizeof(CallerFrameAndPC) + bytesAboveReturnAddress);
}

ALWAYS_INLINE unsigned bytesAboveReturnAddressOf(const ImageFrame& frame)
{
    return frame.bytesAboveFrameRecord() + (frame.liesBelowSpilledArguments() ? sizeOfSpilledArgumentRegisters : 0);
}

ALWAYS_INLINE unsigned bytesAboveReturnAddressOf(const CallSiteCache::Entry& site)
{
    return site.isStubReturnSite() ? bytesAboveReturnAddressOf(site.stubFrameKind) : bytesAboveReturnAddressOf(site.frame);
}

#if defined(BUN_HARDENED)
// Ends the process at a word that neither follows a call in compiled code nor lies in the code of the engine.
JS_EXPORT_PRIVATE void checkReturnAddressInRun(const Image&, const void* returnAddress, const CallFrame* callee);
JS_EXPORT_PRIVATE const CallSiteCache::Entry* resolveReturnAddressInRun(CallSiteCache&, const Image&, const void* returnAddress, const CallFrame* callee);
JS_EXPORT_PRIVATE ASCIILiteral faultOfReturnAddressInRun(const Image&, const void* returnAddress);
#else
ALWAYS_INLINE void checkReturnAddressInRun(const Image&, const void*, const CallFrame*) { }
ALWAYS_INLINE const CallSiteCache::Entry* resolveReturnAddressInRun(CallSiteCache& cache, const Image& image, const void* returnAddress, const CallFrame*) { return cache.resolve(image, returnAddress); }
#endif

struct MachineFrame {
    CallFrame* callFrame { nullptr };
    const void* pc { nullptr };
    CodeLocation code;
    CallFrame* framePointer { nullptr };
    bool hasFrameRecord { true };
    unsigned bytesAboveReturnAddress { 0 };
};

struct SuspendedThreadStack {
    bool contains(const void* frame) const { return !(std::bit_cast<uintptr_t>(frame) % sizeof(void*)) && bounds.contains(const_cast<void*>(frame)); }

    StackBounds bounds;
};

template<typename Stack>
class MachineFrameIterator {
public:
    // framePointer is null for a frame that has a record.
    MachineFrameIterator(const Image& image, CallFrame* callFrame, const void* pc, Stack stack = { }, CallFrame* framePointer = nullptr)
        : m_image(&image)
        , m_stack(stack)
    {
        m_frame = { callFrame, pc, callFrame && m_stack.contains(callFrame) ? m_image->locate(pc) : CodeLocation { }, framePointer ? framePointer : callFrame, !framePointer };
        m_frame.bytesAboveReturnAddress = bytesAboveReturnAddressAt(m_frame.code, imageFrameAt(m_frame.code));
    }

    explicit operator bool() const { return m_frame.code.kind != CodeKind::NotInImage; }
    const MachineFrame& operator*() const { return m_frame; }

    SUPPRESS_ASAN MachineFrameIterator& operator++()
    {
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(m_frame.callFrame);
        CallFrame* framePointer = m_frame.hasFrameRecord ? record->callerFrame : m_frame.framePointer;
        const void* pc = removeCodePtrTag(record->returnPC);
        CodeLocation code = m_image->locate(pc);
        ImageFrame imageFrame = imageFrameAt(code);
        CallFrame* callFrame = frameAtReturnSite(imageFrame, stackPointerOfCaller(m_frame.callFrame, m_frame.bytesAboveReturnAddress), framePointer);
        if (callFrame <= m_frame.callFrame || !m_stack.contains(callFrame)) {
            m_frame = { };
            return *this;
        }
        m_frame = { callFrame, pc, code, framePointer, imageFrame.hasFrameRecord(), bytesAboveReturnAddressAt(code, imageFrame) };
        return *this;
    }

private:
    ImageFrame imageFrameAt(const CodeLocation& code) const
    {
        return code.kind == CodeKind::Function ? m_image->frameAtReturnOffset(code.function, code.offset) : frameAtStubReturnSite(code.stubFrameKind);
    }

    static unsigned bytesAboveReturnAddressAt(const CodeLocation& code, const ImageFrame& imageFrame)
    {
        return code.kind == CodeKind::Stub ? bytesAboveReturnAddressOf(code.stubFrameKind) : bytesAboveReturnAddressOf(imageFrame);
    }

    const Image* m_image;
    Stack m_stack;
    MachineFrame m_frame;
};

struct SampledRegisters {
    void* framePointer;
    void* pc;
    void* linkRegister;
    void* stackPointer;
};

struct SampledTop {
    CallFrame* callFrame;
    const void* pc;
    const void* callerPCOfFramelessTop;
    // Of a frame without a record. Null for any other.
    CallFrame* framePointer { nullptr };
};

JS_EXPORT_PRIVATE std::optional<SampledTop> findSampledTop(const Image&, const SampledRegisters&, CallFrame* topCallFrame, const StackBounds&);

struct LogicalFrame {
    bool hasCallerInMachineFrame() const { return inlineFrame || isOfCalledBuiltin; }

    FunctionID function;
    CallSiteIndex callSite;
    unsigned inlineFrame { 0 };
    bool isInlinedConstructCall { false };
    bool isTailDeleted { false };
    // The site and the inline frame are those of the caller.
    bool isOfCalledBuiltin { false };
    bool callerIsTailDeleted { false };
};

struct LogicalFrameSite {
    CompiledFunctionID code;
    CallSiteIndex callSite;
    unsigned inlineFrame { 0 };
    bool isTailDeleted { false };
    BuiltinID calledBuiltin { };
};

// After a tail call the frame above a construct belongs to another function than the one that was constructed.
bool isFrameOfConstructedCallee(CallFrame* plainFunctionConstructFrame, FunctionID);

LogicalFrame innermostLogicalFrame(const Image&, InlineFrameReader&, const LogicalFrameSite&);
LogicalFrame callerOfInlinedFrame(const Image&, InlineFrameReader&, CompiledFunctionID, const LogicalFrame&);
bool isEntrypointOf(FunctionID candidate, FunctionID sourceFunction);
// The frame, or a hidden one of the same function below it, stands in the code of a caller that made the function object itself and handed it to nobody.
bool isCopiedForOwnFunctionObject(const Image&, InlineFrameReader&, CompiledFunctionID, LogicalFrame);
// All frames below it in its machine frame are hidden ones of the function.
bool hasOnlyEntrypointsBelow(const Image&, InlineFrameReader&, CompiledFunctionID, LogicalFrame, FunctionID sourceFunction);
// The frame is that of the stub that stepped the body of an async generator. Behind an await of the body its callers wait for the step, though they are still on the stack.
bool callersWaitForStep(CallFrame* frameOfStepStub);

struct CompiledFrame {
    JS_EXPORT_PRIVATE JSGlobalObject* globalObject() const;
    JS_EXPORT_PRIVATE String sourceURL() const;
    JS_EXPORT_PRIVATE LineColumn lineColumn() const;
    JS_EXPORT_PRIVATE SourceOrigin sourceOrigin() const;
    JS_EXPORT_PRIVATE bool isActivationOf(JSCell* function) const;

    Instance* instance { nullptr };
    CallFrame* callFrame { nullptr };
    const void* returnAddress { nullptr };
    CompiledFunctionID code;
    FunctionID function;
    CallSiteIndex callSite;
    unsigned inlineFrame { 0 };
    TraceVisibility visibility { TraceVisibility::Visible };
    bool isTailDeleted { false };
    BuiltinID calledBuiltin { };
};

class CompiledFrameIterator {
public:
    using Exit = CompiledFrameExit;

    struct RunEnd {
        JS_EXPORT_PRIVATE bool constructs(FunctionID outermost) const;

        CallFrame* adapterFrame { nullptr };
        bool isConstruct { false };
        bool resumesSavedFrame { false };
        bool hasCalledHandlerOfJob { false };
    };

    enum class OperationState : bool { MayNotHaveCalled, HasCalled };
    // framePointer and stackPointer are what the registers hold in the frame that returnAddress runs in. Without stackPointer only a frame that has a record is found.
    JS_EXPORT_PRIVATE static std::optional<CompiledFrameIterator> tryCreate(VM&, CallFrame* framePointer, const void* returnAddress, EntryFrame*, OperationState = OperationState::MayNotHaveCalled, void* stackPointer = nullptr);
    JS_EXPORT_PRIVATE static std::optional<CompiledFrameIterator> tryCreateAtTop(VM&);
    JS_EXPORT_PRIVATE static std::optional<CompiledFrameIterator> tryCreateAtCallerOf(VM&, CallFrame* callee, EntryFrame*);

    bool isAtEnd() const { return m_isAtEnd; }
    const CompiledFrame& frame() const
    {
        ASSERT(!m_isAtEnd);
        if (!m_hasReadFrame)
            readFrame();
        return m_frame;
    }
    // A walk over machine frames that asks for these and for callSite() never pays for frame().
    Instance* instance() const
    {
        ASSERT(!m_isAtEnd);
        return m_instance;
    }
    CallFrame* machineFrame() const
    {
        ASSERT(!m_isAtEnd);
        return m_cursor.callFrame;
    }
    const Exit& exit() const
    {
        ASSERT(m_isAtEnd);
        return m_exit;
    }
    const RunEnd& runEnd() const
    {
        ASSERT(m_isAtEnd);
        return m_runEnd;
    }
    const CallSiteCache::Entry& callSite() const
    {
        ASSERT(!m_isAtEnd);
        return m_callSite;
    }

    JS_EXPORT_PRIVATE void advance();
    JS_EXPORT_PRIVATE void advanceToCallerMachineFrame();
    // For a walk that has gone over the rest of the run by itself.
    void leaveRunAt(const Exit& exit, const RunEnd& runEnd)
    {
        m_isAtEnd = true;
        m_exit = exit;
        m_runEnd = runEnd;
    }
    // The frame is the last one shown of its run: the functions below wait for the step of an async generator that it belongs to.
    JS_EXPORT_PRIVATE bool isAboveFramesThatWait() const;
    JS_EXPORT_PRIVATE bool isConstructCall();
    // The function object that runs in the frame, where its function records it. Null: it lies in a frame.
    JS_EXPORT_PRIVATE std::optional<JSObject*> recordedCallee();
    void* callerStackPointer() const { return AOT::stackPointerOfCaller(m_cursor.callFrame, m_cursor.bytesAboveReturnAddress); }
    bool isAtFirstReadSite() const { return m_cursor.isAtFirstReadSite && m_callSite.site.firstReadSite; }
    bool calleeWasEntered() const { return m_calleeWasEntered; }
    bool isAmongFramesThatWait() const { return m_cursor.isAmongFramesThatWait; }
    EntryFrame* entryFrame() const { return m_cursor.entryFrame; }
    CallFrame* framePointer() const { return m_cursor.framePointer; }
    void* stackPointer() const { return m_cursor.stackPointer; }
    CallFrame* generatorStubFrameLeft() const { return m_cursor.generatorStubFrame; }
    CallFrame* asyncGeneratorStubFrameLeft() const { return m_cursor.asyncGeneratorStubFrame; }
    // The frame that returns to that stub frame: the body's, or that of the stub that throws because the stack has no room for the body's.
    CallFrame* asyncGeneratorBodyFrameLeft() const { return m_cursor.asyncGeneratorBodyFrame; }
    CallFrame* engineCalleeStubFrameLeft() const { return m_cursor.engineCalleeStubFrame; }

private:
    struct Cursor {
        // What the frame pointer register holds in the frame that rawPC runs in.
        CallFrame* framePointer;
        void* rawPC;
        EntryFrame* entryFrame;
        // The frame that returns to rawPC, where the walk has seen it.
        CallFrame* calleeFrame { nullptr };
        // What the stack pointer is in the frame that rawPC runs in, where the walk has seen the frame that returns to rawPC.
        void* stackPointer { nullptr };
        // The frame that rawPC runs in, once enterFrame() has looked at the return site.
        CallFrame* callFrame { nullptr };
        bool hasFrameRecord { true };
        unsigned bytesAboveReturnAddress { 0 };
        bool isAtFirstReadSite { false };
        CallFrame* generatorStubFrame { nullptr };
        CallFrame* asyncGeneratorStubFrame { nullptr };
        CallFrame* asyncGeneratorBodyFrame { nullptr };
        CallFrame* engineCalleeStubFrame { nullptr };
        CallFrame* plainFunctionConstructFrame { nullptr };
        bool isAmongFramesThatWait { false };
    };

public:
    // Public for std::optional: tryCreate() makes the iterator in the place it returns it in.
    CompiledFrameIterator(VM&, const Cursor&, bool calleeWasEntered);

private:
    static std::optional<CompiledFrameIterator> tryCreate(VM&, const Cursor&, OperationState);
    static void enterFrame(Cursor&, const CallSiteCache::Entry&);
    static void leaveRun(Cursor&, Exit&);
    static bool passOverFrame(Cursor&, Exit&);
    bool passOverMethodReadFrame(Cursor&, Exit&);
    Instance* findEnclosingAdapterInstance(Cursor) const;
    Instance* tryFindInstance() const;
    const CallSiteCache::Entry* findFunctionFrame(Cursor&, Exit&, RunEnd&);
    const CallSiteCache::Entry* findCallerFunctionFrame(Cursor&, Exit&, RunEnd&);
    void enterMachineFrame(const CallSiteCache::Entry*);
    JS_EXPORT_PRIVATE void readFrame() const;

    VM* m_vm;
    const Image* m_image;
    CallSiteCache* m_cache;
    Instance* m_instance { nullptr };
    Cursor m_cursor;
    CallSiteCache::Entry m_callSite;
    mutable CompiledFrame m_frame;
    mutable bool m_isInlinedConstructCall { false };
    mutable bool m_hasReadFrame { false };
    bool m_calleeWasEntered;
    bool m_isAtEnd { false };
    Exit m_exit;
    RunEnd m_runEnd;
    InlineFrameReader m_inlineFrames;
};
static_assert(sizeof(CompiledFrameIterator) <= 1024);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
