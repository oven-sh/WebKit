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

#include "AOTFunction.h"
#include "BytecodeIndex.h"
#include "CalleeBits.h"
#include "LineColumn.h"
#include "SourceID.h"
#include "WasmIndexOrName.h"
#include <wtf/Function.h>
#include <wtf/Indenter.h>
#include <wtf/IterationStatus.h>
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
class ClonedArguments;
class Register;
class RegisterAtOffsetList;
class ScriptExecutable;
class StackVisitor;
class UnlinkedCodeBlock;

namespace AOT {
struct Data;
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
        // A frame of AOT code has no CodeBlock until it is requested here, and then one is created. That must not happen during an
        // allocation or a collection. No other accessor of Frame has this side effect.
        CodeBlock* codeBlock() const
        {
            if (m_aotFunction && !m_codeBlock) [[unlikely]]
                return makeCodeBlock();
            return m_codeBlock;
        }
        bool hasCode() const { return m_codeBlock || m_aotFunction; } // !!codeBlock()
        // The realm the frame's code runs in. Unlike callFrame()->lexicalGlobalObject(), this is valid for every kind of frame: a frame of
        // AOT code has no callee slot to find the realm through.
        JS_EXPORT_PRIVATE JSGlobalObject* lexicalGlobalObject(VM&) const;
        // Set if this is a frame of AOT code. Such a frame holds nothing that other code can interpret, so everything about it has
        // to be obtained through these accessors.
        AOT::FunctionRef aotFunction() const { return m_aotFunction; }
        // The adapter frame between an AOT frame and callerFrame(), if the caller is not AOT code. The adapter has saved registers.
        CallFrame* aotAdapterFrame() const { return m_aotAdapterFrame; }
        // Not a physical frame, but a call that was inlined into AOT code. The physical frame is that of the function it was
        // inlined into, which is visited later.
        bool isInlinedAOTFrame() const { return !!m_aotInlineFrame; }
        // The frame's last action was a tail call that was inlined (see above). The physical frame exists, and the unwinder has to
        // process it, but it is not visible to the program.
        bool isTailDeleted() const { return m_isTailDeleted; }
        // The executable of the code that the frame runs, if it is JavaScript.
        JS_EXPORT_PRIVATE ScriptExecutable* ownerExecutable() const;
        JS_EXPORT_PRIVATE bool isBuiltinFunction() const; // hasCode()
        BytecodeIndex bytecodeIndex() const { return m_bytecodeIndex; }
        InlineCallFrame* inlineCallFrame() const {
#if ENABLE(DFG_JIT)
            return m_inlineDFGCallFrame;
#else
            return nullptr;
#endif
        }
        void* returnPC() const { return m_returnPC; }

        bool isNativeFrame() const { return !hasCode() && !isNativeCalleeFrame(); }
        bool isInlinedDFGFrame() const { return !isNativeCalleeFrame() && !!inlineCallFrame(); }
        bool isNativeCalleeFrame() const { return m_callee.isNativeCallee(); }
        Wasm::IndexOrName const wasmFunctionIndexOrName()
        {
            ASSERT(isNativeCalleeFrame());
            return m_wasmFunctionIndexOrName;
        }
        size_t NODELETE wasmFunctionIndex() const;

        CallSiteIndex NODELETE wasmCallSiteIndex() const;

        JS_EXPORT_PRIVATE String functionName() const;
        JS_EXPORT_PRIVATE String sourceURL() const;
        JS_EXPORT_PRIVATE String preRedirectURL() const;
        JS_EXPORT_PRIVATE String toString() const;

        JS_EXPORT_PRIVATE SourceID sourceID();

        CodeType NODELETE codeType() const;
        bool NODELETE hasLineAndColumnInfo() const;
        JS_EXPORT_PRIVATE LineColumn computeLineAndColumn() const;

#if ENABLE(ASSEMBLER)
        const RegisterAtOffsetList* calleeSaveRegistersForUnwinding();
#endif

        ClonedArguments* createArguments(VM&);
        CallFrame* callFrame() const { return m_callFrame; }

        JS_EXPORT_PRIVATE bool isImplementationVisibilityPrivate() const;
        JS_EXPORT_PRIVATE bool isFrameOf(JSCell* function) const; // callee() == function, but for a frame that has none.

        void dump(PrintStream&, Indenter = Indenter()) const;
        void dump(PrintStream&, Indenter, WTF::Function<void(PrintStream&)> prefix) const;

    private:
        Frame() { }
        ~Frame() { }

        void NODELETE setToEnd();
        JS_EXPORT_PRIVATE CodeBlock* makeCodeBlock() const;

#if ENABLE(DFG_JIT)
        InlineCallFrame* m_inlineDFGCallFrame { nullptr };
#endif
        unsigned m_wasmDistanceFromDeepestInlineFrame { 0 };
        CallFrame* m_callFrame { nullptr };
        EntryFrame* m_entryFrame { nullptr };
        EntryFrame* m_callerEntryFrame { nullptr };
        CallFrame* m_callerFrame { nullptr };
        CalleeBits m_callee { };
        mutable CodeBlock* m_codeBlock { nullptr };
        AOT::FunctionRef m_aotFunction;
        CallFrame* m_aotAdapterFrame { nullptr };
        AOT::FunctionRef m_aotFunctionOfFrame; // The function whose machine code runs in the frame.
        unsigned m_aotInlineFrame { 0 };
        bool m_isTailDeleted { false };
        void* m_returnPC { nullptr };
        void* m_callerReturnPC { nullptr }; // The address at which m_callerFrame will resume.
        size_t m_index { 0 };
        size_t m_argumentCountIncludingThis { 0 };
        BytecodeIndex m_bytecodeIndex { };
        bool m_callerIsEntryFrame : 1 { false };
        bool m_isWasmFrame : 1 { false };
        Wasm::IndexOrName m_wasmFunctionIndexOrName { };
        size_t m_wasmFunctionIndex { 0 };
        uint32_t m_wasmCallSiteIndexBits { };

        friend class StackVisitor;
    };

    enum EmptyEntryFrameAction {
        ContinueIfTopEntryFrameIsEmpty,
        TerminateIfTopEntryFrameIsEmpty,
    };

    template <EmptyEntryFrameAction action = ContinueIfTopEntryFrameIsEmpty, StackVisitorFunctor Functor>
    static void visit(CallFrame* startFrame, VM& vm, const Functor& functor, bool skipFirstFrame = false)
    {
        StackVisitor visitor(startFrame, vm, skipFirstFrame);
        if constexpr (requires { functor.didSkipAOTAdapterAtTop(startFrame); }) {
            if (visitor.m_aotAdapterSkippedAtTop) [[unlikely]]
                functor.didSkipAOTAdapterAtTop(visitor.m_aotAdapterSkippedAtTop);
        }
        if (action == TerminateIfTopEntryFrameIsEmpty && visitor.topEntryFrameIsEmpty())
            return;
        while (visitor->callFrame()) {
            IterationStatus status = functor(visitor);
            if (status != IterationStatus::Continue)
                break;
            visitor.gotoNextFrame();
        }
    }

    Frame& operator*() { return m_frame; }
    ALWAYS_INLINE Frame* operator->() { return &m_frame; }
    void unwindToMachineCodeBlockFrame();

    bool topEntryFrameIsEmpty() const { return m_topEntryFrameIsEmpty; }

private:
    JS_EXPORT_PRIVATE StackVisitor(CallFrame* startFrame, VM&, bool skipFirstFrame);

    JS_EXPORT_PRIVATE void gotoNextFrame();

    void readFrame(CallFrame*);
    void readInlinableNativeCalleeFrame(CallFrame*);
    void readNonInlinedFrame(CallFrame*, CodeOrigin* = nullptr);
    void findCaller(CallFrame*);
#if ENABLE(AOT)
    void readAOTFrame(CallFrame*, void* returnPC, uint32_t index);
#endif
#if ENABLE(DFG_JIT)
    void readInlinedFrame(CallFrame*, CodeOrigin*);
#endif
    CallFrame* NODELETE updatePreviousReturnPCIfNecessary(CallFrame*);

    Frame m_frame;
    void* m_previousReturnPC { nullptr };
    AOT::Instance* m_aotInstance { nullptr }; // The Instance of the AOT frames that are being visited, once it has been looked up.
    CallFrame* m_aotAdapterSkippedAtTop { nullptr }; // See the constructor. The unwinder has to restore the registers that it saved.
    bool m_topEntryFrameIsEmpty { false };
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
