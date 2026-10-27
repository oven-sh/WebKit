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
#if ENABLE(AOT)
    class WithCompiledFrames;
#endif

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
        // The frame is that of the built-in function that compiledFrame() calls, and whose steps compiled code runs itself.
        bool isOfBuiltinCalledByCompiledFrame() const { return m_isOfBuiltinCalledByCompiledFrame; }
        // compiledFrame() is the frame above it: CompiledFrameIterator::constructedFunctionThatLeft().
        bool isOfConstructedFunctionThatLeft() const { return m_isOfConstructedFunctionThatLeft; }
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
        ~Frame() { }

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
        AOT::CompiledFrameIterator* m_compiledFrames { nullptr };
        bool m_isOfBuiltinCalledByCompiledFrame { false };
        bool m_isOfConstructedFunctionThatLeft { false };
#endif

        friend class StackVisitor;
#if ENABLE(AOT)
        friend class WithCompiledFrames;
#endif
    };

    enum EmptyEntryFrameAction {
        ContinueIfTopEntryFrameIsEmpty,
        TerminateIfTopEntryFrameIsEmpty,
    };

    enum class CompiledFrameVisit : uint8_t { ShownFrames, ShownFramesAndCalledBuiltins, EachRunOnce };

    template<typename Functor>
    static constexpr CompiledFrameVisit compiledFrameVisitFor = std::is_base_of_v<UnwindFunctorBase, Functor> ? CompiledFrameVisit::EachRunOnce : CompiledFrameVisit::ShownFrames;

    static constexpr bool visitsCompiledFrames = false;

    template <EmptyEntryFrameAction action = ContinueIfTopEntryFrameIsEmpty, StackVisitorFunctor Functor>
    static void visit(CallFrame* startFrame, VM& vm, NOESCAPE const Functor& functor, bool skipFirstFrame = false)
    {
#if ENABLE(AOT)
        if (hasCompiledCode(vm)) [[unlikely]] {
            visitWithCompiledFrames<action>(startFrame, vm, functor, skipFirstFrame);
            return;
        }
#endif
        visitWithoutCompiledFrames<action>(startFrame, vm, functor, skipFirstFrame);
    }

    template <EmptyEntryFrameAction action = ContinueIfTopEntryFrameIsEmpty, StackVisitorFunctor Functor>
    static ALWAYS_INLINE void visitWithoutCompiledFrames(CallFrame* startFrame, VM& vm, NOESCAPE const Functor& functor, bool skipFirstFrame = false)
    {
        StackVisitor visitor(startFrame, vm, skipFirstFrame);
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
    template <EmptyEntryFrameAction action = ContinueIfTopEntryFrameIsEmpty, StackVisitorFunctor Functor>
    static void visitWithCompiledFrames(CallFrame* startFrame, VM&, NOESCAPE const Functor&, bool skipFirstFrame = false, CompiledFrameVisit = compiledFrameVisitFor<Functor>);

    template<StackVisitorFunctor Functor>
    static void visitFrom(VM&, const AOT::CompiledFrameExit&, NOESCAPE const Functor&, CompiledFrameVisit = compiledFrameVisitFor<Functor>);
#endif

    Frame& operator*() { return m_frame; }
    ALWAYS_INLINE Frame* operator->() { return &m_frame; }
#if ENABLE(AOT)
    AOT::CompiledFrameIterator& compiledFrames() { return *m_frame.m_compiledFrames; }
#endif
    void unwindToMachineCodeBlockFrame();

    bool topEntryFrameIsEmpty() const { return m_topEntryFrameIsEmpty; }

private:
    JS_EXPORT_PRIVATE StackVisitor(CallFrame* startFrame, VM&, bool skipFirstFrame);
#if ENABLE(AOT)
    StackVisitor() = default;

    template<typename VMType>
    static ALWAYS_INLINE bool hasCompiledCode(const VMType& vm) { return vm.hasCompiledCode(); }
#endif

    JS_EXPORT_PRIVATE void gotoNextFrame();

    void readFrame(CallFrame*);
    void readInlinableNativeCalleeFrame(CallFrame*);
    void readNonInlinedFrame(CallFrame*, CodeOrigin* = nullptr);
#if ENABLE(DFG_JIT)
    void readInlinedFrame(CallFrame*, CodeOrigin*);
#endif
    CallFrame* LEGACY_NODELETE updatePreviousReturnPCIfNecessary(CallFrame*);

    Frame m_frame;
    void* m_previousReturnPC { nullptr };
    bool m_topEntryFrameIsEmpty { false };
};

#if ENABLE(AOT)
class StackVisitor::WithCompiledFrames final : public StackVisitor {
    WTF_MAKE_NONCOPYABLE(WithCompiledFrames);
public:
    static constexpr bool visitsCompiledFrames = true;

    JS_EXPORT_PRIVATE WithCompiledFrames(CallFrame* startFrame, VM&, bool skipFirstFrame, CompiledFrameVisit);
    JS_EXPORT_PRIVATE WithCompiledFrames(VM&, const AOT::CompiledFrameExit&, CompiledFrameVisit);
    JS_EXPORT_PRIVATE ~WithCompiledFrames();

    JS_EXPORT_PRIVATE void gotoNextFrame();

private:
    void readFrame(CallFrame*);
    bool tryEnterCompiledFrames(CallFrame*);
    bool showFirstCompiledFrame();
    bool showNextCompiledFrame();
    void setCompiledFrame(CallFrame*, const void* returnAddress);
    CallFrame* leaveCompiledFrames();
    void gotoNextCompiledFrame();
    void startInCompiledFrames(CallFrame* startFrame, bool skipFirstFrame);

    static constexpr size_t compiledFrameIteratorCapacity = 1024;

    VM& m_vm;
    AOT::ShownFrameFilter m_shownFrameFilter;
    CompiledFrameVisit m_compiledFrameVisit;
    bool m_nextFrameIsPublished { false };
    // The frame that returns to m_previousReturnPC, while m_frame has no frame.
    CallFrame* m_previousCalleeFrame { nullptr };
    alignas(void*) std::byte m_compiledFramesStorage[compiledFrameIteratorCapacity];
};

template <StackVisitor::EmptyEntryFrameAction action, StackVisitorFunctor Functor>
NEVER_INLINE void StackVisitor::visitWithCompiledFrames(CallFrame* startFrame, VM& vm, NOESCAPE const Functor& functor, bool skipFirstFrame, CompiledFrameVisit compiledFrameVisit)
{
    WithCompiledFrames visitor(startFrame, vm, skipFirstFrame, compiledFrameVisit);
    if (action == TerminateIfTopEntryFrameIsEmpty && visitor.topEntryFrameIsEmpty())
        return;
    while (visitor->callFrame()) {
        IterationStatus status = functor(visitor);
        if (status != IterationStatus::Continue)
            break;
        visitor.gotoNextFrame();
    }
}

template<StackVisitorFunctor Functor>
NEVER_INLINE void StackVisitor::visitFrom(VM& vm, const AOT::CompiledFrameExit& exit, NOESCAPE const Functor& functor, CompiledFrameVisit compiledFrameVisit)
{
    WithCompiledFrames visitor(vm, exit, compiledFrameVisit);
    while (visitor->callFrame()) {
        IterationStatus status = functor(visitor);
        if (status != IterationStatus::Continue)
            break;
        visitor.gotoNextFrame();
    }
}
#endif

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
