/*
 * Copyright (C) 2013-2024 Apple Inc. All rights reserved.
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

#include "AOTCompiledFrameExit.h"
#include "AOTShownFrameFilter.h"
#include "BytecodeIndex.h"
#include "CalleeBits.h"
#include "LineColumn.h"
#include "SourceID.h"
#include "WasmIndexOrName.h"
#include <cstddef>
#include <wtf/Function.h>
#include <wtf/Indenter.h>
#include <wtf/IterationStatus.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/WTFString.h>

namespace JSC {

struct EntryFrame;
struct InlineCallFrame;

class CallFrame;
class CallSiteIndex;
class CodeBlock;
class CodeOrigin;
class JSCell;
class JSFunction;
class JSGlobalObject;
class ClonedArguments;
class Register;
class RegisterAtOffsetList;
class StackVisitor;
class UnwindFunctorBase;
class VM;

namespace AOT {
class CompiledFrameIterator;
struct CompiledFrame;
}

template<typename T>
concept StackVisitorFunctor = requires(const T t, StackVisitor& visitor) {
    { t.operator()(visitor) } -> std::same_as<IterationStatus>;
    // in other words, requires a method:
    //     IterationStatus operator()(StackVisitor&) const;
};

class StackVisitor {
public:
    class Frame {
    public:
        enum CodeType {
            Global,
            Eval,
            Function,
            Module,
            Native,
            Wasm
        };

        size_t index() const { return m_index; }
        size_t argumentCountIncludingThis() const { return m_argumentCountIncludingThis; }
        bool callerIsEntryFrame() const { return m_callerIsEntryFrame; }
        bool isWasmFrame() const { return m_isWasmFrame; }
        CallFrame* callerFrame() const { return m_callerFrame; }
        EntryFrame* entryFrame() const { return m_entryFrame; }
        CalleeBits callee() const { return m_callee; }
        CodeBlock* codeBlock() const { return m_codeBlock; }
        JS_EXPORT_PRIVATE JSGlobalObject* lexicalGlobalObject(VM&) const;
#if ENABLE(AOT)
        bool isCompiledFrame() const { return !!m_compiledFrames; }
        JS_EXPORT_PRIVATE const AOT::CompiledFrame& compiledFrame() const;
        JS_EXPORT_PRIVATE bool isConstructCall() const;
#else
        static constexpr bool isCompiledFrame() { return false; }
#endif
        BytecodeIndex bytecodeIndex() const { return m_bytecodeIndex; }
        InlineCallFrame* inlineCallFrame() const {
#if ENABLE(DFG_JIT)
            return m_inlineDFGCallFrame;
#else
            return nullptr;
#endif
        }
        void* returnPC() const { return m_returnPC; }

        bool isNativeFrame() const { return !codeBlock() && !isNativeCalleeFrame() && !isCompiledFrame(); }
        bool isInlinedDFGFrame() const { return !isNativeCalleeFrame() && !!inlineCallFrame(); }
        bool isNativeCalleeFrame() const { return m_callee.isNativeCallee(); }
        Wasm::IndexOrName const wasmFunctionIndexOrName()
        {
            ASSERT(isNativeCalleeFrame());
            return m_wasmFunctionIndexOrName;
        }
        size_t LEGACY_NODELETE wasmFunctionIndex() const;

        CallSiteIndex LEGACY_NODELETE wasmCallSiteIndex() const;

        JS_EXPORT_PRIVATE String functionName() const;
        JS_EXPORT_PRIVATE String sourceURL() const;
        JS_EXPORT_PRIVATE String preRedirectURL() const;
        JS_EXPORT_PRIVATE String toString() const;

        JS_EXPORT_PRIVATE SourceID sourceID();

        CodeType LEGACY_NODELETE codeType() const;
        bool LEGACY_NODELETE hasLineAndColumnInfo() const;
        JS_EXPORT_PRIVATE LineColumn computeLineAndColumn() const;

#if ENABLE(ASSEMBLER)
        const RegisterAtOffsetList* calleeSaveRegistersForUnwinding();
#endif

        ClonedArguments* createArguments(VM&);
        CallFrame* callFrame() const { return m_callFrame; }

        JS_EXPORT_PRIVATE bool isImplementationVisibilityPrivate() const;
        JS_EXPORT_PRIVATE bool isFrameOf(JSCell* function) const;

        void dump(PrintStream&, Indenter = Indenter()) const;
        void dump(PrintStream&, Indenter, WTF::Function<void(PrintStream&)> prefix) const;

    private:
        Frame() { }
#if ENABLE(AOT)
        WTF_MAKE_NONCOPYABLE(Frame);
        JS_EXPORT_PRIVATE ~Frame();
#else
        ~Frame() { }
#endif

        void LEGACY_NODELETE setToEnd();

#if ENABLE(DFG_JIT)
        InlineCallFrame* m_inlineDFGCallFrame { nullptr };
#endif
        unsigned m_wasmDistanceFromDeepestInlineFrame { 0 };
        CallFrame* m_callFrame { nullptr };
        EntryFrame* m_entryFrame { nullptr };
        EntryFrame* m_callerEntryFrame { nullptr };
        CallFrame* m_callerFrame { nullptr };
        CalleeBits m_callee { };
        CodeBlock* m_codeBlock { nullptr };
        void* m_returnPC { nullptr };
        size_t m_index { 0 };
        size_t m_argumentCountIncludingThis { 0 };
        BytecodeIndex m_bytecodeIndex { };
        bool m_callerIsEntryFrame : 1 { false };
        bool m_isWasmFrame : 1 { false };
        Wasm::IndexOrName m_wasmFunctionIndexOrName { };
        size_t m_wasmFunctionIndex { 0 };
        uint32_t m_wasmCallSiteIndexBits { };
#if ENABLE(AOT)
        static constexpr size_t compiledFrameIteratorCapacity = 1024;
        AOT::CompiledFrameIterator* m_compiledFrames { nullptr };
        alignas(void*) std::byte m_compiledFramesStorage[compiledFrameIteratorCapacity];
#endif

        friend class StackVisitor;
    };

    enum EmptyEntryFrameAction {
        ContinueIfTopEntryFrameIsEmpty,
        TerminateIfTopEntryFrameIsEmpty,
    };

    enum class CompiledFrameVisit : bool { ShownFrames, EachRunOnce };

    template<typename Functor>
    static constexpr CompiledFrameVisit compiledFrameVisitFor = std::is_base_of_v<UnwindFunctorBase, Functor> ? CompiledFrameVisit::EachRunOnce : CompiledFrameVisit::ShownFrames;

    template <EmptyEntryFrameAction action = ContinueIfTopEntryFrameIsEmpty, StackVisitorFunctor Functor>
    static void visit(CallFrame* startFrame, VM& vm, NOESCAPE const Functor& functor, bool skipFirstFrame = false)
    {
        StackVisitor visitor(startFrame, vm, skipFirstFrame, compiledFrameVisitFor<Functor>);
        if (action == TerminateIfTopEntryFrameIsEmpty && visitor.topEntryFrameIsEmpty())
            return;
        while (visitor->callFrame()) {
            IterationStatus status = functor(visitor);
            if (status != IterationStatus::Continue)
                break;
            visitor.gotoNextFrame();
        }
    }

#if ENABLE(AOT)
    template<StackVisitorFunctor Functor>
    static void visitFrom(VM& vm, const AOT::CompiledFrameExit& exit, NOESCAPE const Functor& functor)
    {
        StackVisitor visitor(vm, exit, compiledFrameVisitFor<Functor>);
        while (visitor->callFrame()) {
            IterationStatus status = functor(visitor);
            if (status != IterationStatus::Continue)
                break;
            visitor.gotoNextFrame();
        }
    }
#endif

    Frame& operator*() { return m_frame; }
    ALWAYS_INLINE Frame* operator->() { return &m_frame; }
#if ENABLE(AOT)
    AOT::CompiledFrameIterator& compiledFrames() { return *m_frame.m_compiledFrames; }
#endif
    void unwindToMachineCodeBlockFrame();

    bool topEntryFrameIsEmpty() const { return m_topEntryFrameIsEmpty; }

private:
    JS_EXPORT_PRIVATE StackVisitor(CallFrame* startFrame, VM&, bool skipFirstFrame, CompiledFrameVisit);
#if ENABLE(AOT)
    JS_EXPORT_PRIVATE StackVisitor(VM&, const AOT::CompiledFrameExit&, CompiledFrameVisit);
#endif

    JS_EXPORT_PRIVATE void gotoNextFrame();

    void readFrame(CallFrame*);
    void readInlinableNativeCalleeFrame(CallFrame*);
    void readNonInlinedFrame(CallFrame*, CodeOrigin* = nullptr);
#if ENABLE(DFG_JIT)
    void readInlinedFrame(CallFrame*, CodeOrigin*);
#endif
    CallFrame* LEGACY_NODELETE updatePreviousReturnPCIfNecessary(CallFrame*);
#if ENABLE(AOT)
    bool tryEnterCompiledFrames(CallFrame*);
    bool showFirstCompiledFrame();
    bool showNextCompiledFrame();
    void setCompiledFrame(CallFrame*, const void* returnAddress);
    CallFrame* leaveCompiledFrames();
    void gotoNextCompiledFrame();
    void startInCompiledFrames(CallFrame* startFrame, bool skipFirstFrame);
#endif

    Frame m_frame;
    void* m_previousReturnPC { nullptr };
    bool m_topEntryFrameIsEmpty { false };
#if ENABLE(AOT)
    VM& m_vm;
    AOT::ShownFrameFilter m_shownFrameFilter;
    CompiledFrameVisit m_compiledFrameVisit;
    bool m_nextFrameIsPublished { false };
#endif
};

class CallerFunctor {
public:
    CallerFunctor()
        : m_hasSkippedFirstFrame(false)
        , m_callerFrame(nullptr)
    {
    }

    CallFrame* callerFrame() const { return m_callerFrame; }

    IterationStatus operator()(StackVisitor& visitor) const
    {
        if (!m_hasSkippedFirstFrame) {
            m_hasSkippedFirstFrame = true;
            return IterationStatus::Continue;
        }

        m_callerFrame = visitor->callFrame();
        return IterationStatus::Done;
    }

private:
    mutable bool m_hasSkippedFirstFrame;
    mutable CallFrame* m_callerFrame;
};

} // namespace JSC
