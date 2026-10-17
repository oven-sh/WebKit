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

#include "AOTFunction.h"
#include "BytecodeIndex.h"
#include "Heap.h"
#include "CodeSpecializationKind.h"
#include "CodeType.h"
#include "LineColumn.h"
#include "SlotVisitorMacros.h"
#include "VM.h"
#include "WasmIndexOrName.h"
#include "WriteBarrier.h"
#include <limits.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Variant.h>

namespace JSC {

class CodeBlock;
class FunctionExecutable;
class JSGlobalObject;
class JSObject;
class ScriptExecutable;

class JSFrameData {
public:
    JSFrameData() = default;
    JSFrameData(WriteBarrier<JSCell> callee, WriteBarrier<CodeBlock> codeBlock, BytecodeIndex bytecodeIndex, bool isAsyncFrame = false)
        : m_calleeOrInstanceToken(callee.get())
        , m_codeBlock(codeBlock.get())
        , bytecodeIndex(bytecodeIndex)
        , m_isAsyncFrame(isAsyncFrame)
    {
    }
    struct CompiledCode {
        ScriptExecutable* executable;
        JSCell* instanceToken;
    };
    JSFrameData(WriteBarrier<JSCell> callee, const CompiledCode* compiledCode, CodeSpecializationKind kind, BytecodeIndex bytecodeIndex, bool isAsyncFrame)
        : m_calleeOrInstanceToken(callee.get())
        , m_compiledCode(compiledCode)
        , bytecodeIndex(bytecodeIndex)
        , m_isAsyncFrame(isAsyncFrame)
        , aotKind(kind)
        , m_form(Form::Compiled)
    {
    }
    JSFrameData(WriteBarrier<JSCell> instanceToken, uint32_t typedCode, uint32_t typedFunction, CodeSpecializationKind kind, BytecodeIndex callSite, bool isAsyncFrame)
        : m_calleeOrInstanceToken(typedCode != notTyped ? instanceToken.get() : nullptr)
        , m_typed { typedCode, typedFunction }
        , bytecodeIndex(callSite)
        , m_isAsyncFrame(isAsyncFrame)
        , aotKind(kind)
        , m_form(typedCode != notTyped ? Form::Typed : Form::Plain)
    {
        if (typedCode == notTyped)
            m_codeBlock = nullptr;
    }
    JSCell* callee() const { return m_form == Form::Typed ? nullptr : m_calleeOrInstanceToken; }
    CodeBlock* codeBlock() const { return m_form == Form::Plain ? m_codeBlock : nullptr; }
    ScriptExecutable* aotExecutable() const { return m_form == Form::Compiled ? m_compiledCode->executable : nullptr; }
    JSCell* aotInstanceToken() const
    {
        if (m_form == Form::Typed)
            return m_calleeOrInstanceToken;
        return m_form == Form::Compiled ? m_compiledCode->instanceToken : nullptr;
    }
    static constexpr uint32_t notTyped = UINT32_MAX;
    uint32_t aotTypedCode() const { return isTyped() ? m_typed.code : notTyped; }
    uint32_t aotTypedFunction() const { return isTyped() ? m_typed.function : notTyped; }
    bool isTyped() const { return m_form == Form::Typed; }

private:
    enum class Form : uint8_t { Plain, Compiled, Typed };
    struct Typed {
        uint32_t code;
        uint32_t function;
    };

    JSCell* m_calleeOrInstanceToken { nullptr };
    union {
        CodeBlock* m_codeBlock { nullptr };
        const CompiledCode* m_compiledCode;
        Typed m_typed;
    };

public:
    BytecodeIndex bytecodeIndex;
    bool m_isAsyncFrame { false };
    CodeSpecializationKind aotKind { CodeSpecializationKind::CodeForCall };

private:
    Form m_form { Form::Plain };
};
static_assert(sizeof(JSFrameData) == 24 && std::is_trivially_copyable_v<JSFrameData>);

struct CompiledFrameData : public ThreadSafeRefCounted<CompiledFrameData> {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CompiledFrameData);
    static Ref<CompiledFrameData> create(WriteBarrier<JSCell> callee, WriteBarrier<ScriptExecutable> executable, WriteBarrier<JSCell> instanceToken, CodeSpecializationKind kind, BytecodeIndex bytecodeIndex, bool isAsyncFrame)
    {
        return adoptRef(*new CompiledFrameData(callee, executable, instanceToken, kind, bytecodeIndex, isAsyncFrame));
    }
    CompiledFrameData(WriteBarrier<JSCell> callee, WriteBarrier<ScriptExecutable> executable, WriteBarrier<JSCell> instanceToken, CodeSpecializationKind kind, BytecodeIndex bytecodeIndex, bool isAsyncFrame)
        : code { executable.get(), instanceToken.get() }
        , frame(callee, &code, kind, bytecodeIndex, isAsyncFrame)
    {
    }
    JSFrameData::CompiledCode code;
    JSFrameData frame;
};

struct WasmFrameData {
    Wasm::IndexOrName functionIndexOrName;
    size_t functionIndex { 0 };
};

enum class AllowURLOverride : bool { No, Yes };

class StackFrame {
public:
    using FrameData = Variant<JSFrameData, WasmFrameData, Ref<CompiledFrameData>>;

    StackFrame(VM&, JSCell* owner, JSCell* callee);
    StackFrame(VM&, JSCell* owner, JSCell* callee, CodeBlock*, BytecodeIndex);
    StackFrame(VM&, JSCell* owner, JSCell* callee, CodeBlock*, BytecodeIndex, bool isAsyncFrame);
    StackFrame(VM&, JSCell* owner, CodeBlock*, BytecodeIndex);
    StackFrame(VM&, JSCell* owner, JSCell* calleeOrNull, ScriptExecutable*, CodeSpecializationKind, JSCell* instanceToken, BytecodeIndex, bool isAsyncFrame = false);
    StackFrame(VM&, JSCell* owner, JSCell* instanceToken, uint32_t typedCode, uint32_t typedFunction, BytecodeIndex callSite, bool isAsyncFrame = false);
    StackFrame(JSCell* instanceToken, const AOT::StackTraceFrame& frame)
        : m_frameData(JSFrameData { WriteBarrier<JSCell>(instanceToken, WriteBarrierEarlyInit), frame.function, frame.typedFunction, frame.isConstructed() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall, frame.callSite(), false })
    {
    }
    static void appendTypedFrames(VM&, JSCell* owner, JSCell* instanceToken, std::span<const AOT::StackTraceFrame>, Vector<StackFrame>&, bool mayBeFollowedByMore);
    static void appendTypedFrames(const AOT::ReturnAddressTrace&, Vector<StackFrame>&);
    enum SuspendedTag { Suspended };
    StackFrame(SuspendedTag, JSCell* instanceToken, uint32_t typedCode, uint32_t typedFunction, BytecodeIndex callSite)
        : m_frameData(JSFrameData { WriteBarrier<JSCell>(instanceToken, WriteBarrierEarlyInit), typedCode, typedFunction, CodeSpecializationKind::CodeForCall, callSite, true })
    {
    }
    StackFrame(VM&, JSCell* owner, JSCell* callee, bool isAsyncFrame);
    StackFrame(Wasm::IndexOrName);
    StackFrame(Wasm::IndexOrName, size_t functionIndex);
    StackFrame() = default;

    bool hasLineAndColumnInfo() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->codeBlock() || jsFrame->aotExecutable() || (jsFrame->isTyped() && !representsHostFunction());
        return false;
    }

    bool isTypedAOTFrame() const
    {
        auto* jsFrame = jsFrameData();
        return jsFrame && jsFrame->isTyped();
    }
    uint32_t typedAOTFunction() const { return jsFrameData()->aotTypedFunction(); }

    CodeBlock* codeBlock() const
    {
        if (auto* jsFrame = jsFrameData()) {
            if (jsFrame->aotExecutable()) [[unlikely]]
                return createCodeBlock();
            return jsFrame->codeBlock();
        }
        return nullptr;
    }

    CodeBlock* codeBlockIfExists() const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFrame->codeBlock();
        return nullptr;
    }
    JS_EXPORT_PRIVATE ScriptExecutable* ownerExecutable() const;
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isConstructor() const;
    void setIsConstructed() { jsFrameData()->aotKind = CodeSpecializationKind::CodeForConstruct; }
    JS_EXPORT_PRIVATE bool isBuiltinFunction() const;
    JS_EXPORT_PRIVATE bool representsHostFunction() const;
    JS_EXPORT_PRIVATE JSGlobalObject* codeGlobalObject() const;

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
    JS_EXPORT_PRIVATE std::optional<AOT::FunctionRef::ReportedPosition> reportedPosition(AOT::FunctionRef::ConstructPosition = AOT::FunctionRef::ConstructPosition::AtDivot) const;
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
    const JSFrameData* jsFrameData() const
    {
        if (auto* jsFrame = std::get_if<JSFrameData>(&m_frameData)) [[likely]]
            return jsFrame;
        if (auto* compiledFrame = std::get_if<Ref<CompiledFrameData>>(&m_frameData))
            return &compiledFrame->get().frame;
        return nullptr;
    }
    JSFrameData* jsFrameData() { return const_cast<JSFrameData*>(const_cast<const StackFrame*>(this)->jsFrameData()); }
    template<typename JSFunctor, typename WasmFunctor>
    decltype(auto) switchOnFrameData(const JSFunctor& jsFunctor, const WasmFunctor& wasmFunctor) const
    {
        if (auto* jsFrame = jsFrameData())
            return jsFunctor(*jsFrame);
        return wasmFunctor(std::get<WasmFrameData>(m_frameData));
    }
    JS_EXPORT_PRIVATE CodeBlock* createCodeBlock() const;

    FrameData m_frameData { JSFrameData {} };
};
static_assert(sizeof(StackFrame) == 32);

} // namespace JSC
