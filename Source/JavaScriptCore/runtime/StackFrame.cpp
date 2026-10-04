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

#include "config.h"
#include "StackFrame.h"

#include "AOTRuntime.h"
#include "AOTProgramData.h"
#include "CodeBlock.h"
#include "DebuggerPrimitives.h"
#include "FunctionExecutable.h"
#include "JSCellInlines.h"
#include "JSFunctionInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC {

StackFrame::StackFrame(VM& vm, JSCell* owner, JSCell* callee)
    : m_frameData(JSFrameData {
        WriteBarrier<JSCell>(vm, owner, callee),
        WriteBarrier<CodeBlock>(),
        BytecodeIndex()
    })
{
}

StackFrame::StackFrame(VM& vm, JSCell* owner, JSCell* callee, CodeBlock* codeBlock, BytecodeIndex bytecodeIndex)
    : m_frameData(JSFrameData {
        WriteBarrier<JSCell>(vm, owner, callee),
        WriteBarrier<CodeBlock>(vm, owner, codeBlock),
        bytecodeIndex
    })
{
}

StackFrame::StackFrame(VM& vm, JSCell* owner, JSCell* callee, CodeBlock* codeBlock, BytecodeIndex bytecodeIndex, bool isAsyncFrame)
    : m_frameData(JSFrameData {
        WriteBarrier<JSCell>(vm, owner, callee),
        WriteBarrier<CodeBlock>(vm, owner, codeBlock),
        bytecodeIndex,
        isAsyncFrame
    })
{
}

StackFrame::StackFrame(VM& vm, JSCell* owner, CodeBlock* codeBlock, BytecodeIndex bytecodeIndex)
    : m_frameData(JSFrameData {
        WriteBarrier<JSCell>(),
        WriteBarrier<CodeBlock>(vm, owner, codeBlock),
        bytecodeIndex
    })
{
}

StackFrame::StackFrame(VM& vm, JSCell* owner, JSCell* callee, ScriptExecutable* executable, CodeSpecializationKind kind, JSCell* instanceToken, BytecodeIndex bytecodeIndex, bool isAsyncFrame)
    : m_frameData(JSFrameData {
        callee ? WriteBarrier<JSCell>(vm, owner, callee) : WriteBarrier<JSCell>(),
        WriteBarrier<CodeBlock>(),
        bytecodeIndex,
        isAsyncFrame,
        kind,
        WriteBarrier<ScriptExecutable>(vm, owner, executable),
        WriteBarrier<JSCell>(vm, owner, instanceToken)
    })
{
}

CodeBlock* StackFrame::makeCodeBlock() const
{
#if ENABLE(AOT)
    auto& jsFrame = std::get<JSFrameData>(m_frameData);
    VM& vm = jsFrame.aotExecutable->vm();
    if (vm.heap.mutatorState() != MutatorState::Running)
        return nullptr;
    if (AOT::FunctionRef function = AOT::FunctionRef::of(vm, jsFrame.aotExecutable.get(), jsFrame.aotKind, jsFrame.aotInstanceToken.get()); function && function.instance && function.codeType() == FunctionCode)
        return function.ensureCodeBlock();
#endif
    return nullptr;
}

static ScriptExecutable* ownerExecutableOf(const JSFrameData& jsFrame)
{
    if (jsFrame.aotExecutable)
        return jsFrame.aotExecutable.get();
    return jsFrame.codeBlock ? jsFrame.codeBlock->ownerExecutable() : nullptr;
}

ScriptExecutable* StackFrame::ownerExecutable() const
{
    auto* jsFrame = std::get_if<JSFrameData>(&m_frameData);
    return jsFrame ? ownerExecutableOf(*jsFrame) : nullptr;
}

CodeType StackFrame::codeType() const
{
    auto& jsFrame = std::get<JSFrameData>(m_frameData);
    if (jsFrame.aotExecutable)
        return jsFrame.aotExecutable->isFunctionExecutable() ? FunctionCode : ModuleCode;
    return jsFrame.codeBlock->codeType();
}

bool StackFrame::isConstructor() const
{
    auto& jsFrame = std::get<JSFrameData>(m_frameData);
    return jsFrame.aotExecutable ? jsFrame.aotKind == CodeSpecializationKind::CodeForConstruct : jsFrame.codeBlock->isConstructor();
}

bool StackFrame::isBuiltinFunction() const
{
    auto& jsFrame = std::get<JSFrameData>(m_frameData);
    if (jsFrame.aotExecutable) {
        auto* asFunctionExecutable = dynamicDowncast<FunctionExecutable>(jsFrame.aotExecutable.get());
        return asFunctionExecutable && asFunctionExecutable->isBuiltinFunction();
    }
    return jsFrame.codeBlock->unlinkedCodeBlock()->isBuiltinFunction();
}

JSGlobalObject* StackFrame::codeGlobalObject() const
{
    auto& jsFrame = std::get<JSFrameData>(m_frameData);
#if ENABLE(AOT)
    if (jsFrame.aotExecutable) {
        AOT::FunctionRef function = AOT::FunctionRef::of(jsFrame.aotExecutable->vm(), jsFrame.aotExecutable.get(), jsFrame.aotKind, jsFrame.aotInstanceToken.get());
        return function.instance ? function.instance->globalObject : nullptr;
    }
#endif
    return jsFrame.codeBlock->globalObject();
}

StackFrame::StackFrame(Wasm::IndexOrName indexOrName)
    : m_frameData(WasmFrameData { WTF::move(indexOrName), 0 })
{
}

StackFrame::StackFrame(Wasm::IndexOrName indexOrName, size_t functionIndex)
    : m_frameData(WasmFrameData { WTF::move(indexOrName), functionIndex })
{
}

StackFrame::StackFrame(VM& vm, JSCell* owner, JSCell* callee, bool isAsyncFrame)
    : m_frameData(JSFrameData {
        WriteBarrier<JSCell>(vm, owner, callee),
        WriteBarrier<CodeBlock>(),
        BytecodeIndex(),
        isAsyncFrame
    })
{
}

bool StackFrame::hasBytecodeIndex() const
{
    if (auto* jsFrame = std::get_if<JSFrameData>(&m_frameData))
        return !!jsFrame->bytecodeIndex;
    return false;
}

BytecodeIndex StackFrame::bytecodeIndex() const
{
    ASSERT(hasBytecodeIndex());
    return std::get<JSFrameData>(m_frameData).bytecodeIndex;
}

template<typename Visitor>
void StackFrame::visitAggregate(Visitor& visitor)
{
    WTF::switchOn(m_frameData,
        [&visitor](const JSFrameData& jsFrame) {
            if (jsFrame.callee)
                visitor.append(jsFrame.callee);
            if (jsFrame.codeBlock)
                visitor.append(jsFrame.codeBlock);
            if (jsFrame.aotExecutable)
                visitor.append(jsFrame.aotExecutable);
            if (jsFrame.aotInstanceToken)
                visitor.append(jsFrame.aotInstanceToken);
        },
        [](const WasmFrameData&) { }
    );
}
template void StackFrame::visitAggregate(AbstractSlotVisitor&);
template void StackFrame::visitAggregate(SlotVisitor&);

bool StackFrame::isMarked(VM& vm) const
{
    return WTF::switchOn(m_frameData,
        [&vm](const JSFrameData& jsFrame) {
            return (!jsFrame.callee || vm.heap.isMarked(jsFrame.callee.get())) && (!jsFrame.codeBlock || vm.heap.isMarked(jsFrame.codeBlock.get())) && (!jsFrame.aotExecutable || vm.heap.isMarked(jsFrame.aotExecutable.get())) && (!jsFrame.aotInstanceToken || vm.heap.isMarked(jsFrame.aotInstanceToken.get()));
        },
        [](const WasmFrameData&) { return true; }
    );
}

SourceID StackFrame::sourceID() const
{
    if (auto* jsFrame = std::get_if<JSFrameData>(&m_frameData)) {
        ScriptExecutable* executable = ownerExecutableOf(*jsFrame);
        return executable ? executable->sourceID() : noSourceID;
    }
    return noSourceID;
}

static String processSourceURL(VM& vm, const JSC::StackFrame& frame, const String& sourceURL, AllowURLOverride allowOverride = AllowURLOverride::Yes)
{
    if (allowOverride == AllowURLOverride::Yes && vm.clientData && (!protocolIsInHTTPFamily(sourceURL) && !protocolIs(sourceURL, "blob"_s))) {
        String overrideURL = vm.clientData->overrideSourceURL(frame, sourceURL);
        if (!overrideURL.isNull())
            return overrideURL;
    }

    if (!sourceURL.isNull())
        return sourceURL;
    return emptyString();
}

String StackFrame::sourceURL(VM& vm, AllowURLOverride allowOverride) const
{
    return WTF::switchOn(m_frameData,
        [&vm, allowOverride, this](const JSFrameData& jsFrame) -> String {
            if (isAsyncFrameWithoutCodeBlock()) {
                ASSERT(jsFrame.callee);
                ASSERT(!jsFrame.codeBlock);
                JSFunction* calleeFn = dynamicDowncast<JSFunction>(jsFrame.callee.get());
                return processSourceURL(vm, *this, calleeFn->jsExecutable()->sourceURL(), allowOverride);
            }

            ScriptExecutable* executable = ownerExecutableOf(jsFrame);
            if (!executable)
                return "[native code]"_s;
            if (auto position = reportedPosition(); position && position->source)
                return AOT::ProgramData::get()->sourceName(position->source);
            return processSourceURL(vm, *this, executable->sourceURL(), allowOverride);
        },
        [](const WasmFrameData& wasmFrame) -> String {
            auto moduleName = wasmFrame.functionIndexOrName.moduleName();
            if (moduleName.empty())
                return makeString("wasm-function["_s, wasmFrame.functionIndex, ']');
            return makeString(moduleName, ":wasm-function["_s, wasmFrame.functionIndex, ']');
        }
    );
}

String StackFrame::sourceURLStripped(VM& vm) const
{
    return WTF::switchOn(m_frameData,
        [&vm, this](const JSFrameData& jsFrame) -> String {
            if (isAsyncFrameWithoutCodeBlock()) {
                ASSERT(jsFrame.callee);
                ASSERT(!jsFrame.codeBlock);
                JSFunction* calleeFn = dynamicDowncast<JSFunction>(jsFrame.callee.get());
                return processSourceURL(vm, *this, calleeFn->jsExecutable()->sourceURLStripped());
            }

            ScriptExecutable* executable = ownerExecutableOf(jsFrame);
            if (!executable)
                return "[native code]"_s;
            return processSourceURL(vm, *this, executable->sourceURLStripped());
        },
        [](const WasmFrameData& wasmFrame) -> String {
            auto moduleName = wasmFrame.functionIndexOrName.moduleName();
            if (moduleName.empty())
                return makeString("wasm-function["_s, wasmFrame.functionIndex, ']');
            return makeString(moduleName, ":wasm-function["_s, wasmFrame.functionIndex, ']');
        }
    );
}

String StackFrame::functionName(VM& vm) const
{
    return WTF::switchOn(m_frameData,
        [&vm](const JSFrameData& jsFrame) -> String {
            if (jsFrame.codeBlock) {
                switch (jsFrame.codeBlock->codeType()) {
                case EvalCode:
                    return "eval code"_s;
                case ModuleCode:
                    return "module code"_s;
                case GlobalCode:
                    return "global code"_s;
                case FunctionCode:
                    break;
                }
            } else if (jsFrame.aotExecutable && !jsFrame.aotExecutable->isFunctionExecutable())
                return "module code"_s;
            String name;
            if (jsFrame.callee && jsFrame.callee->isObject())
                name = getCalculatedDisplayName(vm, uncheckedDowncast<JSObject>(jsFrame.callee.get())).impl();
            else if (auto* executable = dynamicDowncast<FunctionExecutable>(ownerExecutableOf(jsFrame)))
                name = executable->ecmaNameWithoutGC();

            if (name.isNull())
                return emptyString();

            if (jsFrame.m_isAsyncFrame)
                return makeString("async "_s, name);

            return name;
        },
        [](const WasmFrameData& wasmFrame) -> String {
            if (wasmFrame.functionIndexOrName.isEmpty() || !wasmFrame.functionIndexOrName.nameSection())
                return "wasm-stub"_s;
            if (wasmFrame.functionIndexOrName.isIndex())
                return WTF::toString(wasmFrame.functionIndexOrName.index());
            return WTF::toString(wasmFrame.functionIndexOrName.name()->span());
        }
    );
}

std::optional<AOT::FunctionRef::ReportedPosition> StackFrame::reportedPosition(AOT::FunctionRef::ConstructPosition constructPosition) const
{
#if ENABLE(AOT)
    auto* jsFrame = std::get_if<JSFrameData>(&m_frameData);
    if (!jsFrame || !AOT::ProgramData::get())
        return std::nullopt;
    AOT::FunctionRef function;
    if (jsFrame->aotExecutable)
        function = AOT::FunctionRef::of(jsFrame->aotExecutable->vm(), jsFrame->aotExecutable.get(), jsFrame->aotKind, jsFrame->aotInstanceToken.get());
    else if (jsFrame->codeBlock)
        function = AOT::FunctionRef::of(jsFrame->codeBlock.get());
    if (function)
        return function.reportedPositionFor(jsFrame->bytecodeIndex, constructPosition);
#else
    UNUSED_PARAM(constructPosition);
#endif
    return std::nullopt;
}

LineColumn StackFrame::computeLineAndColumn() const
{
    if (auto* jsFrame = std::get_if<JSFrameData>(&m_frameData)) {
        ScriptExecutable* executable = ownerExecutableOf(*jsFrame);
        if (!executable)
            return { };
        LineColumn lineColumn;
#if ENABLE(AOT)
        if (jsFrame->aotExecutable) {
            if (AOT::FunctionRef function = AOT::FunctionRef::of(executable->vm(), jsFrame->aotExecutable.get(), jsFrame->aotKind, jsFrame->aotInstanceToken.get()))
                lineColumn = function.lineColumnFor(jsFrame->bytecodeIndex);
        } else
#endif
            lineColumn = jsFrame->codeBlock->lineColumnForBytecodeIndex(jsFrame->bytecodeIndex);

        if (std::optional<int> overrideLineNumber = executable->overrideLineNumber(executable->vm()))
            lineColumn.line = overrideLineNumber.value();

        return lineColumn;
    }
    return { };
}

// A `data:` URL is the whole script, so one frame can be tens of thousands of characters long.
// Sentry caps its own stack lines at 1024, so use the same number.
// FIXME: other schemes, and other places that print a source URL, are not capped.
// https://bugs.webkit.org/show_bug.cgi?id=323716
static constexpr unsigned maximumDataURLLengthInStackTrace = 1024;

static StringView truncateLongDataURL(StringView sourceURL)
{
    if (sourceURL.length() <= maximumDataURLLengthInStackTrace)
        return sourceURL;
    if (!protocolIs(sourceURL, "data"_s))
        return sourceURL;

    // Don't cut in the middle of a %XX escape, or the URL won't decode.
    unsigned length = maximumDataURLLengthInStackTrace;
    size_t lastPercent = sourceURL.reverseFind('%', length - 1);
    if (lastPercent != notFound && length - lastPercent < 3)
        length = lastPercent;
    return sourceURL.left(length);
}

String StackFrame::toString(VM& vm) const
{
    String functionName = this->functionName(vm);
    // sourceURL points into fullSourceURL, so keep fullSourceURL around.
    String fullSourceURL = this->sourceURLStripped(vm);
    auto sourceURL = truncateLongDataURL(fullSourceURL);

    if (sourceURL.isEmpty() || !hasLineAndColumnInfo())
        return makeString(functionName, '@', sourceURL);

    auto lineColumn = computeLineAndColumn();
    return makeString(functionName, '@', sourceURL, ':', lineColumn.line, ':', lineColumn.column);
}

} // namespace JSC
