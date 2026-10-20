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
#include <wtf/Noncopyable.h>
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

struct MachineFrame {
    CallFrame* callFrame { nullptr };
    const void* pc { nullptr };
    CodeLocation code;
};

struct TrustedStack {
    bool contains(const void*) const { return true; }
};

struct SuspendedThreadStack {
    bool contains(const void* frame) const { return !(std::bit_cast<uintptr_t>(frame) % sizeof(void*)) && bounds.contains(const_cast<void*>(frame)); }

    StackBounds bounds;
};

template<typename Stack>
class MachineFrameIterator {
public:
    MachineFrameIterator(const Image& image, CallFrame* callFrame, const void* pc, Stack stack = { })
        : m_image(&image)
        , m_stack(stack)
    {
        set(callFrame, pc);
    }

    explicit operator bool() const { return m_frame.code.kind != CodeKind::NotInImage; }
    const MachineFrame& operator*() const { return m_frame; }

    SUPPRESS_ASAN MachineFrameIterator& operator++()
    {
        auto* record = reinterpret_cast<const CallerFrameAndPC*>(m_frame.callFrame);
        if (record->callerFrame <= m_frame.callFrame) {
            m_frame = { };
            return *this;
        }
        set(record->callerFrame, removeCodePtrTag(record->returnPC));
        return *this;
    }

private:
    void set(CallFrame* callFrame, const void* pc)
    {
        m_frame = { callFrame, pc, callFrame && m_stack.contains(callFrame) ? m_image->locate(pc) : CodeLocation { } };
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
};

JS_EXPORT_PRIVATE std::optional<SampledTop> findSampledTop(const Image&, const SampledRegisters&, CallFrame* topCallFrame, const StackBounds&);

struct LogicalFrame {
    FunctionID function;
    CallSiteIndex callSite;
    unsigned inlineFrame { 0 };
    bool isInlinedConstructCall { false };
    bool isTailDeleted { false };
};

struct LogicalFrameSite {
    CompiledFunctionID code;
    CallSiteIndex callSite;
    unsigned inlineFrame { 0 };
    bool isTailDeleted { false };
};

LogicalFrame innermostLogicalFrame(const Image&, InlineFrameReader&, const LogicalFrameSite&);
LogicalFrame callerOfInlinedFrame(const Image&, InlineFrameReader&, CompiledFunctionID, const LogicalFrame&);
bool isEntrypointOf(FunctionID candidate, FunctionID sourceFunction);

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
};

class CompiledFrameIterator {
public:
    using Exit = CompiledFrameExit;

    struct RunEnd {
        // After a tail call the outermost frame of the run belongs to another function than the one that was constructed.
        JS_EXPORT_PRIVATE bool constructs(FunctionID outermost) const;

        CallFrame* adapterFrame { nullptr };
        bool isConstruct { false };
        bool resumesSavedFrame { false };
    };

    enum class OperationState : bool { MayNotHaveCalled, HasCalled };
    JS_EXPORT_PRIVATE static std::optional<CompiledFrameIterator> tryCreate(VM&, CallFrame*, const void* returnAddress, EntryFrame*, OperationState = OperationState::MayNotHaveCalled);
    JS_EXPORT_PRIVATE static std::optional<CompiledFrameIterator> tryCreateAtTop(VM&);

    bool isAtEnd() const { return m_isAtEnd; }
    const CompiledFrame& frame() const
    {
        ASSERT(!m_isAtEnd);
        return m_frame;
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
    JS_EXPORT_PRIVATE bool isConstructCall();
    bool isAtFirstReadSite() const { return m_cursor.isAtFirstReadSite && m_callSite.site.firstReadSite; }
    bool calleeWasEntered() const { return m_calleeWasEntered; }
    EntryFrame* entryFrame() const { return m_cursor.entryFrame; }
    CallFrame* generatorStubFrameLeft() const { return m_cursor.generatorStubFrame; }
    CallFrame* engineCalleeStubFrameLeft() const { return m_cursor.engineCalleeStubFrame; }

private:
    struct Cursor {
        CallFrame* callFrame;
        void* rawPC;
        EntryFrame* entryFrame;
        bool isAtFirstReadSite { false };
        CallFrame* generatorStubFrame { nullptr };
        CallFrame* engineCalleeStubFrame { nullptr };
    };

    CompiledFrameIterator(VM&, const Cursor&, bool calleeWasEntered);

    static void leaveRun(Cursor&, Exit&);
    static bool passOverFrame(Cursor&, Exit&);
    const CallSiteCache::Entry* findFunctionFrame(Cursor&, Exit&, RunEnd&);
    const CallSiteCache::Entry* findCallerFunctionFrame(Cursor&, Exit&, RunEnd&);
    void enterMachineFrame(const CallSiteCache::Entry*);
    void readMachineFrame(const CallSiteCache::Entry&);

    VM* m_vm;
    const Image* m_image;
    CallSiteCache* m_cache;
    Cursor m_cursor;
    CallSiteCache::Entry m_callSite;
    CompiledFrame m_frame;
    bool m_isInlinedConstructCall { false };
    bool m_calleeWasEntered;
    bool m_isAtEnd { false };
    Exit m_exit;
    RunEnd m_runEnd;
    InlineFrameReader m_inlineFrames;
};
static_assert(sizeof(CompiledFrameIterator) <= 1024);

class CallSiteOverride {
    WTF_MAKE_NONCOPYABLE(CallSiteOverride);
public:
    JS_EXPORT_PRIVATE CallSiteOverride(Instance&, const void* frame);
    JS_EXPORT_PRIVATE ~CallSiteOverride();

    void setItem(unsigned item) { m_item = item; }
    const CallSiteOverride* previous() const { return m_previous; }
    const void* frame() const { return m_frame; }
    unsigned item() const { return m_item; }

private:
    Instance& m_instance;
    const CallSiteOverride* m_previous;
    const void* m_frame;
    unsigned m_item { 0 };
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
