/*
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
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

#include "BytecodeIndex.h"
#include "CodeType.h"
#include "Heap.h"
#include "LineColumn.h"
#include "SlotVisitorMacros.h"
#include "VM.h"
#include "WasmIndexOrName.h"
#include "WriteBarrier.h"
#include <limits.h>
#include <wtf/Variant.h>

#if ENABLE(AOT)
#include "AOTTraceFrame.h"
#endif

namespace JSC {

namespace AOT { struct ReturnAddressTrace; }

class CodeBlock;
class JSObject;
class ScriptExecutable;

class JSFrameData {
public:
    JSFrameData() = default;
    JSFrameData(WriteBarrier<JSCell> callee, WriteBarrier<CodeBlock> codeBlock, BytecodeIndex bytecodeIndex, bool isAsyncFrame = false)
        : m_plain { callee.get(), codeBlock.get() }
        , bytecodeIndex(bytecodeIndex)
        , m_isAsyncFrame(isAsyncFrame)
    {
    }
    JSCell* callee() const { return m_form == Form::Plain ? m_plain.callee : nullptr; }
    CodeBlock* codeBlock() const { return m_form == Form::Plain ? m_plain.codeBlock : nullptr; }
    bool isCompiled() const { return m_form == Form::Compiled; }
#if ENABLE(AOT)
    JSFrameData(const AOT::TraceFrame& frame, bool isAsyncFrame)
        : m_compiled(frame)
        , m_isAsyncFrame(isAsyncFrame)
        , m_form(Form::Compiled)
    {
    }
    const AOT::TraceFrame& compiledFrame() const
    {
        ASSERT(isCompiled());
        return m_compiled;
    }
#endif

private:
    enum class Form : uint8_t { Plain, Compiled };
    struct Plain {
        JSCell* callee;
        CodeBlock* codeBlock;
    };

    union {
        Plain m_plain { nullptr, nullptr };
#if ENABLE(AOT)
        AOT::TraceFrame m_compiled;
#endif
    };

public:
    BytecodeIndex bytecodeIndex;
    bool m_isAsyncFrame { false };

private:
    Form m_form { Form::Plain };
};
static_assert(sizeof(JSFrameData) == 24 && std::is_trivially_copyable_v<JSFrameData>);
#if ENABLE(AOT)
static_assert(sizeof(AOT::TraceFrame) == 16 && std::is_trivially_copyable_v<AOT::TraceFrame>);
#endif

struct WasmFrameData {
    Wasm::IndexOrName functionIndexOrName;
    size_t functionIndex { 0 };
};

enum class AllowURLOverride : bool { No, Yes };

class StackFrame {
public:
    using FrameData = Variant<JSFrameData, WasmFrameData>;

    StackFrame(VM&, JSCell* owner, JSCell* callee);
    StackFrame(VM&, JSCell* owner, JSCell* callee, CodeBlock*, BytecodeIndex);
    StackFrame(VM&, JSCell* owner, JSCell* callee, CodeBlock*, BytecodeIndex, bool isAsyncFrame);
    StackFrame(VM&, JSCell* owner, CodeBlock*, BytecodeIndex);
    StackFrame(VM&, JSCell* owner, JSCell* callee, bool isAsyncFrame);
    StackFrame(Wasm::IndexOrName);
    StackFrame(Wasm::IndexOrName, size_t functionIndex);
    StackFrame() = default;

#if ENABLE(AOT)
    explicit StackFrame(const AOT::TraceFrame& frame, bool isAsyncFrame = false)
        : m_frameData(JSFrameData { frame, isAsyncFrame })
    {
    }
    static void appendCompiledFrames(const AOT::ReturnAddressTrace&, Vector<StackFrame>&);
    const AOT::TraceFrame& compiledFrame() const { return jsFrameData()->compiledFrame(); }
    JS_EXPORT_PRIVATE std::optional<AOT::SourcePosition> compiledPosition() const;
#endif

    bool isCompiledFrame() const
    {
        auto* jsFrame = jsFrameData();
        return jsFrame && jsFrame->isCompiled();
    }

    bool hasLineAndColumnInfo() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->isCompiled() ? !representsHostFunction() : !!jsFrame->codeBlock();
        return false;
    }

    CodeBlock* codeBlock() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->codeBlock();
        return nullptr;
    }

    JS_EXPORT_PRIVATE ScriptExecutable* ownerExecutable() const;
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isConstructor() const;
    JS_EXPORT_PRIVATE bool isBuiltinFunction() const;
    JS_EXPORT_PRIVATE bool representsHostFunction() const;

    JSCell* callee() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->callee();
        return nullptr;
    }

#if USE(BUN_JSC_ADDITIONS)
    bool isWasmFrame() const
    {
        return std::holds_alternative<WasmFrameData>(m_frameData);
    }

    const Wasm::IndexOrName& wasmFunctionIndexOrName() const
    {
        ASSERT(isWasmFrame());
        return std::get<WasmFrameData>(m_frameData).functionIndexOrName;
    }

    bool isAsyncFrame() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->m_isAsyncFrame;
        return false;
    }
#endif

    bool isAsyncFrameWithoutCodeBlock() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->m_isAsyncFrame && !hasLineAndColumnInfo();
        return false;
    }

    LineColumn computeLineAndColumn() const;
    String functionName(VM&) const;
    SourceID sourceID() const;
    JS_EXPORT_PRIVATE String sourceURL(VM&, AllowURLOverride = AllowURLOverride::Yes) const;
    String sourceURLStripped(VM&) const;
    String toString(VM&) const;

    bool hasBytecodeIndex() const;
    BytecodeIndex bytecodeIndex() const;

    template<typename Visitor>
    void visitAggregate(Visitor&);

    bool isMarked(VM&) const;

private:
    const JSFrameData* jsFrameData() const { return std::get_if<JSFrameData>(&m_frameData); }

    FrameData m_frameData { JSFrameData {} };
};
static_assert(sizeof(StackFrame) == 32);

} // namespace JSC
