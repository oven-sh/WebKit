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
#include <wtf/Scope.h>
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
    : m_frameData(CompiledFrameData::create(
        callee ? WriteBarrier<JSCell>(vm, owner, callee) : WriteBarrier<JSCell>(),
        WriteBarrier<ScriptExecutable>(vm, owner, executable),
        WriteBarrier<JSCell>(vm, owner, instanceToken),
        kind,
        bytecodeIndex,
        isAsyncFrame))
{
}

void StackFrame::appendTypedFrames(VM& vm, JSCell* owner, JSCell* instanceToken, std::span<const AOT::StackTraceFrame> frames, Vector<StackFrame>& results, bool mayBeFollowedByMore)
{
    if (frames.empty())
        return;
    size_t needed = results.size() + frames.size();
    if (needed > results.capacity())
        results.reserveCapacity(mayBeFollowedByMore ? std::max(needed, 2 * results.capacity()) : needed);
    for (auto& frame : frames)
        results.unsafeAppendWithoutCapacityCheck(StackFrame(instanceToken, frame));
    vm.writeBarrier(owner, instanceToken);
}

void StackFrame::appendTypedFrames(const AOT::ReturnAddressTrace& trace, Vector<StackFrame>& results)
{
#if ENABLE(AOT)
    JSCell* instanceToken = AOT::tokenOf(trace.instance);
    bool isAtTheTop = !trace.hasHostFunctionFrameOnTop && !trace.omittedFrameCount;
    unsigned framesToOmit = trace.omittedFrameCount;
    results.reserveCapacity(std::min<uint32_t>(trace.limit, trace.numberOfReturnAddresses + trace.hasHostFunctionFrameOnTop));
    const AOT::Instance::StackTraceInfo* places = trace.instance->stackTraceInfos();
    AOT::InlineFrameReader inlineFramesRead;
    uint32_t lastShown = AOT::FunctionRef::none;
    uintptr_t imageCodeStart = AOT::imageCodeStart();
    std::optional<AOT::CallSiteOverride> atItem;
    if (trace.topIsAtStackTraceItemSite) [[unlikely]]
        atItem.emplace(*trace.instance, &trace);
    for (uint32_t kept : trace.returnAddresses()) {
        const void* topFrame = atItem ? &trace : nullptr;
        auto clearTopFrame = makeScopeExit([&] {
            atItem.reset();
        });
        const void* returnAddress = AOT::ReturnAddressTrace::addressOf(kept, imageCodeStart);
        bool rootIsConstructed = kept & AOT::ReturnAddressTrace::isConstructedRoot;
        const AOT::Instance::CachedAddressInfo* asked = trace.instance->resolveReturnAddress(returnAddress);
        RELEASE_ASSERT(asked);
        if (auto place = places[asked - trace.instance->cachedReturnAddresses].forSite(); !isAtTheTop && !framesToOmit && place.kind == AOT::Instance::StackTraceInfo::IsVisible) {
            if (results.size() == trace.limit)
                return;
            results.constructAndAppend(instanceToken, AOT::StackTraceFrame::create(asked->function, place.typedFunction, CallSiteIndex(place.callSiteBits).bytecodeIndex(), rootIsConstructed));
            lastShown = place.typedFunction;
            continue;
        }
        AOT::FunctionRef function { trace.instance, asked->function };
        for (auto location = function.locationForReturnAddress(returnAddress, topFrame, &inlineFramesRead);; location = function.inlineCallSiteLocation(location.inlineFrame, inlineFramesRead)) {
            bool isHidden = isAtTheTop ? AOT::typedFunctionIsSynthetic(location.typedFunction) : AOT::typedFunctionIsHiddenInStackTraces(location.typedFunction);
            bool isConstructed = location.inlineFrame ? location.isConstructed : rootIsConstructed;
            if (!location.isTailDeleted && !isHidden && framesToOmit) {
                --framesToOmit;
                lastShown = AOT::FunctionRef::none;
            } else if (!location.isTailDeleted && !isHidden) {
                if (results.size() == trace.limit)
                    return;
                isAtTheTop = false;
                results.constructAndAppend(instanceToken, AOT::StackTraceFrame::create(function.index, location.typedFunction, location.bytecodeIndex, isConstructed));
                lastShown = location.typedFunction;
            } else if (isConstructed && lastShown != AOT::FunctionRef::none && AOT::typedFunctionWraps(location.typedFunction, lastShown)) [[unlikely]]
                results.last().setIsConstructed();
            if (!location.inlineFrame)
                break;
        }
    }
#else
    UNUSED_PARAM(trace);
    UNUSED_PARAM(results);
#endif
}

StackFrame::StackFrame(VM& vm, JSCell* owner, JSCell* instanceToken, uint32_t typedCode, uint32_t typedFunction, BytecodeIndex callSite, bool isAsyncFrame)
    : m_frameData(JSFrameData {
        WriteBarrier<JSCell>(vm, owner, instanceToken),
        typedCode,
        typedFunction,
        CodeSpecializationKind::CodeForCall,
        callSite,
        isAsyncFrame
    })
{
}

CodeBlock* StackFrame::createCodeBlock() const
{
#if ENABLE(AOT)
    auto& jsFrame = *jsFrameData();
    VM& vm = jsFrame.aotExecutable()->vm();
    if (vm.heap.mutatorState() != MutatorState::Running)
        return nullptr;
    if (AOT::FunctionRef function = AOT::FunctionRef::of(vm, jsFrame.aotExecutable(), jsFrame.aotInstanceToken()); function && function.instance && function.codeType() == FunctionCode)
        return function.ensureCodeBlock();
#endif
    return nullptr;
}

static ScriptExecutable* ownerExecutableOf(const JSFrameData& jsFrame)
{
    if (jsFrame.aotExecutable())
        return jsFrame.aotExecutable();
    return jsFrame.codeBlock() ? jsFrame.codeBlock()->ownerExecutable() : nullptr;
}

ScriptExecutable* StackFrame::ownerExecutable() const
{
    auto* jsFrame = jsFrameData();
    return jsFrame ? ownerExecutableOf(*jsFrame) : nullptr;
}

CodeType StackFrame::codeType() const
{
    auto& jsFrame = *jsFrameData();
#if ENABLE(AOT)
    if (jsFrame.isTyped())
        return AOT::typedFunctionCodeType(jsFrame.aotTypedFunction());
#endif
    if (jsFrame.aotExecutable())
        return jsFrame.aotExecutable()->isFunctionExecutable() ? FunctionCode : ModuleCode;
    return jsFrame.codeBlock()->codeType();
}

bool StackFrame::isConstructor() const
{
    auto& jsFrame = *jsFrameData();
#if ENABLE(AOT)
    if (jsFrame.isTyped())
        return jsFrame.aotKind == CodeSpecializationKind::CodeForConstruct || AOT::typedFunctionIsConstructor(jsFrame.aotTypedFunction()) || AOT::builtinKindOfTypedFunction(jsFrame.aotTypedFunction()) == AOT::StackTraceBuiltinKind::JSBuiltinConstructor;
#endif
    return jsFrame.aotExecutable() ? jsFrame.aotKind == CodeSpecializationKind::CodeForConstruct : jsFrame.codeBlock()->isConstructor();
}

bool StackFrame::representsHostFunction() const
{
#if ENABLE(AOT)
    auto* jsFrame = jsFrameData();
    return jsFrame && jsFrame->isTyped() && AOT::builtinKindOfTypedFunction(jsFrame->aotTypedFunction()) == AOT::StackTraceBuiltinKind::HostFunction;
#else
    return false;
#endif
}

bool StackFrame::isBuiltinFunction() const
{
    auto& jsFrame = *jsFrameData();
    if (jsFrame.isTyped()) {
#if ENABLE(AOT)
        if (representsHostFunction())
            return false;
        auto position = reportedPosition();
        return position && position->source && AOT::ProgramData::get()->isSourceNameEmpty(position->source);
#else
        return false;
#endif
    }
    if (jsFrame.aotExecutable()) {
        auto* asFunctionExecutable = dynamicDowncast<FunctionExecutable>(jsFrame.aotExecutable());
        return asFunctionExecutable && asFunctionExecutable->isBuiltinFunction();
    }
    return jsFrame.codeBlock()->unlinkedCodeBlock()->isBuiltinFunction();
}

JSGlobalObject* StackFrame::codeGlobalObject() const
{
    auto& jsFrame = *jsFrameData();
#if ENABLE(AOT)
    if (jsFrame.isTyped())
        return AOT::globalObjectForInstanceToken(jsFrame.aotInstanceToken()->vm(), jsFrame.aotInstanceToken());
    if (jsFrame.aotExecutable()) {
        AOT::FunctionRef function = AOT::FunctionRef::of(jsFrame.aotExecutable()->vm(), jsFrame.aotExecutable(), jsFrame.aotInstanceToken());
        return function.instance ? function.instance->globalObject : nullptr;
    }
#endif
    return jsFrame.codeBlock()->globalObject();
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
    if (auto* jsFrame = jsFrameData())
        return !!jsFrame->bytecodeIndex;
    return false;
}

BytecodeIndex StackFrame::bytecodeIndex() const
{
    ASSERT(hasBytecodeIndex());
    return jsFrameData()->bytecodeIndex;
}

template<typename Visitor>
void StackFrame::visitAggregate(Visitor& visitor)
{
    switchOnFrameData(
        [&visitor](const JSFrameData& jsFrame) {
            if (jsFrame.callee())
                visitor.appendUnbarriered(jsFrame.callee());
            if (jsFrame.codeBlock())
                visitor.appendUnbarriered(jsFrame.codeBlock());
            if (jsFrame.aotExecutable())
                visitor.appendUnbarriered(jsFrame.aotExecutable());
            if (jsFrame.aotInstanceToken())
                visitor.appendUnbarriered(jsFrame.aotInstanceToken());
        },
        [](const WasmFrameData&) { }
    );
}
template void StackFrame::visitAggregate(AbstractSlotVisitor&);
template void StackFrame::visitAggregate(SlotVisitor&);

bool StackFrame::isMarked(VM& vm) const
{
    return switchOnFrameData(
        [&vm](const JSFrameData& jsFrame) {
            return (!jsFrame.callee() || vm.heap.isMarked(jsFrame.callee())) && (!jsFrame.codeBlock() || vm.heap.isMarked(jsFrame.codeBlock())) && (!jsFrame.aotExecutable() || vm.heap.isMarked(jsFrame.aotExecutable())) && (!jsFrame.aotInstanceToken() || vm.heap.isMarked(jsFrame.aotInstanceToken()));
        },
        [](const WasmFrameData&) { return true; }
    );
}

SourceID StackFrame::sourceID() const
{
    if (auto* jsFrame = jsFrameData()) {
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
    return switchOnFrameData(
        [&vm, allowOverride, this](const JSFrameData& jsFrame) -> String {
            if (isAsyncFrameWithoutCodeBlock()) {
                ASSERT(jsFrame.callee());
                ASSERT(!jsFrame.codeBlock());
                JSFunction* calleeFn = dynamicDowncast<JSFunction>(jsFrame.callee());
                return processSourceURL(vm, *this, calleeFn->jsExecutable()->sourceURL(), allowOverride);
            }

            ScriptExecutable* executable = ownerExecutableOf(jsFrame);
            if (!executable && !jsFrame.isTyped())
                return "[native code]"_s;
            if (auto position = reportedPosition(); position && position->source)
                return AOT::ProgramData::get()->sourceNameInFrames(position->source);
            if (!executable)
                return emptyString();
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
    return switchOnFrameData(
        [&vm, this](const JSFrameData& jsFrame) -> String {
            if (isAsyncFrameWithoutCodeBlock()) {
                ASSERT(jsFrame.callee());
                ASSERT(!jsFrame.codeBlock());
                JSFunction* calleeFn = dynamicDowncast<JSFunction>(jsFrame.callee());
                return processSourceURL(vm, *this, calleeFn->jsExecutable()->sourceURLStripped());
            }

            if (jsFrame.isTyped())
                return sourceURL(vm);
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
    return switchOnFrameData(
        [&vm](const JSFrameData& jsFrame) -> String {
            if (jsFrame.codeBlock()) {
                switch (jsFrame.codeBlock()->codeType()) {
                case EvalCode:
                    return "eval code"_s;
                case ModuleCode:
                    return "module code"_s;
                case GlobalCode:
                    return "global code"_s;
                case FunctionCode:
                    break;
                }
            } else if (jsFrame.aotExecutable() && !jsFrame.aotExecutable()->isFunctionExecutable())
                return "module code"_s;
#if ENABLE(AOT)
            if (jsFrame.isTyped()) {
                if (AOT::typedFunctionCodeType(jsFrame.aotTypedFunction()) == ModuleCode)
                    return "module code"_str;
                String name = AOT::typedFunctionName(vm, jsFrame.aotTypedFunction(), jsFrame.m_isAsyncFrame);
                if (!jsFrame.m_isAsyncFrame)
                    return name;
                return name.isEmpty() || AOT::typedFunctionHasGivenName(jsFrame.aotTypedFunction()) ? emptyString() : makeString("async "_s, name);
            }
#endif
            String name;
            if (jsFrame.callee() && jsFrame.callee()->isObject())
                name = getCalculatedDisplayName(vm, uncheckedDowncast<JSObject>(jsFrame.callee())).impl();
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
    auto* jsFrame = jsFrameData();
    if (!jsFrame || !AOT::ProgramData::get())
        return std::nullopt;
    AOT::FunctionRef function;
    if (jsFrame->isTyped())
        function = { nullptr, jsFrame->aotTypedCode() };
    else if (jsFrame->aotExecutable())
        function = AOT::FunctionRef::of(jsFrame->aotExecutable()->vm(), jsFrame->aotExecutable(), jsFrame->aotInstanceToken());
    else if (jsFrame->codeBlock())
        function = AOT::FunctionRef::of(jsFrame->codeBlock());
    if (function)
        return function.reportedPositionFor(jsFrame->bytecodeIndex, constructPosition);
#else
    UNUSED_PARAM(constructPosition);
#endif
    return std::nullopt;
}

LineColumn StackFrame::computeLineAndColumn() const
{
    if (auto* jsFrame = jsFrameData()) {
        if (jsFrame->isTyped()) {
            auto position = reportedPosition();
            return position ? position->lineColumn : LineColumn { };
        }
        ScriptExecutable* executable = ownerExecutableOf(*jsFrame);
        if (!executable)
            return { };
        LineColumn lineColumn;
#if ENABLE(AOT)
        if (jsFrame->aotExecutable()) {
            if (AOT::FunctionRef function = AOT::FunctionRef::of(executable->vm(), jsFrame->aotExecutable(), jsFrame->aotInstanceToken()))
                lineColumn = function.lineColumnFor(jsFrame->bytecodeIndex);
        } else
#endif
            lineColumn = jsFrame->codeBlock()->lineColumnForBytecodeIndex(jsFrame->bytecodeIndex);

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
    String fullSourceURL;
    std::optional<AOT::FunctionRef::ReportedPosition> typedPosition;
    bool isTyped = isTypedAOTFrame() && !isAsyncFrameWithoutCodeBlock();
    if (isTyped) {
        typedPosition = reportedPosition();
        fullSourceURL = typedPosition && typedPosition->source ? AOT::ProgramData::get()->sourceNameInFrames(typedPosition->source) : emptyString();
    } else
        fullSourceURL = this->sourceURLStripped(vm);
    auto sourceURL = truncateLongDataURL(fullSourceURL);

    if (sourceURL.isEmpty() && representsHostFunction())
        return makeString(functionName, "@[native code]"_s);
    if (sourceURL.isEmpty() || !hasLineAndColumnInfo())
        return makeString(functionName, '@', sourceURL);

    LineColumn lineColumn;
    if (!isTyped)
        lineColumn = computeLineAndColumn();
    else if (typedPosition)
        lineColumn = typedPosition->lineColumn;
    return makeString(functionName, '@', sourceURL, ':', lineColumn.line, ':', lineColumn.column);
}

} // namespace JSC
