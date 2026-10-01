/*
 * Copyright (C) 2013-2025 Apple Inc. All rights reserved.
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
#include "StackVisitor.h"

#include "AOTRuntime.h"

#include "ClonedArguments.h"
#include "DebuggerPrimitives.h"
#include "ExecutableBaseInlines.h"
#include "InlineCallFrame.h"
#include "JSCInlines.h"
#include "NativeCallee.h"
#include "RegisterAtOffsetList.h"
#include "WasmCallee.h"
#include "WasmIndexOrName.h"
#include "WebAssemblyFunction.h"
#include <wtf/text/MakeString.h>

namespace JSC {

// Returns the caller of a frame that uses the engine's standard frame layout, and the address at which the caller will resume.
static CallFrame* callerOf(CallFrame* callFrame, EntryFrame*& entryFrame, void*& returnPC)
{
    EntryFrame* entryFrameOfCallee = entryFrame;
    returnPC = callFrame->rawReturnPC();
    CallFrame* caller = callFrame->callerFrame(entryFrame);
#if ENABLE(AOT)
    // The frame above a VM entry frame is the one that called out of the VM, some distance up the stack.
    if (entryFrame != entryFrameOfCallee && caller && AOT::hasCode())
        returnPC = AOT::returnAddressForFrame(caller, entryFrameOfCallee);
#else
    UNUSED_VARIABLE(entryFrameOfCallee);
#endif
    return caller;
}

// Returns the frame, unless it belongs to an AOT stub. Stub frames are skipped, and the first frame above them is returned.
static CallFrame* skipFramesOfStubs(CallFrame* callFrame, EntryFrame*& entryFrame, void*& returnPC, CallFrame*& adapter)
{
#if ENABLE(AOT)
    while (callFrame) {
        switch (AOT::classifyAddress(removeCodePtrTag(returnPC)).kind) {
        case AOT::ImageAddressInfo::Stub:
            // A stub that was entered from outside the VM (for example Stub::ConstructByCalling, which Reflect.construct() may
            // call) has set up its frame the way the engine does for a native function, and has to be treated as one. It is the
            // only frame between one VM entry and the next, and the unwinder has to stop at each entry.
            if (callFrame->callerFrameOrEntryFrame() == entryFrame) {
                returnPC = nullptr;
                return callFrame;
            }
            returnPC = callFrame->rawReturnPC();
            callFrame = callFrame->callerFrame();
            continue;
        case AOT::ImageAddressInfo::Adapter:
            adapter = callFrame;
            callFrame = callerOf(callFrame, entryFrame, returnPC);
            continue;
        case AOT::ImageAddressInfo::Function:
        case AOT::ImageAddressInfo::NotInImage:
            return callFrame;
        }
    }
#else
    UNUSED_PARAM(entryFrame);
    UNUSED_PARAM(returnPC);
    UNUSED_PARAM(adapter);
#endif
    return callFrame;
}

StackVisitor::StackVisitor(CallFrame* startFrame, VM& vm, bool skipFirstFrame)
{
    CallFrame* topFrame = nullptr;
    if (startFrame) {
        ASSERT(!vm.topCallFrame || static_cast<void*>(vm.topCallFrame) != vm.topEntryFrame);

        m_frame.m_entryFrame = vm.topEntryFrame;
        topFrame = vm.topCallFrame;
        if (topFrame) {
            m_previousReturnPC = vm.maybeReturnPC;
            bool isTheEnginesOwn = true;
#if ENABLE(AOT)
            // What kind of frame it is has to be known before anything is read from it.
            if (AOT::hasCode()) {
                void* returnPC = AOT::returnAddressForFrame(topFrame, __builtin_frame_address(0));
                if (returnPC && AOT::classifyAddress(returnPC).kind != AOT::ImageAddressInfo::NotInImage) {
                    // A function that ran out of stack in its prologue has given up its frame by now (Stub::ThrowStackOverflowAtPrologue). If it was
                    // called from outside AOT code, what is on top is the adapter's frame, with the registers it saved. If it was
                    // called from outside the VM, there is no frame of JavaScript code since then.
                    topFrame = skipFramesOfStubs(topFrame, m_frame.m_entryFrame, returnPC, m_aotAdapterSkippedAtTop);
                    m_topEntryFrameIsEmpty = m_frame.m_entryFrame != vm.topEntryFrame;
                    m_previousReturnPC = returnPC;
                    isTheEnginesOwn = AOT::classifyAddress(removeCodePtrTag(returnPC)).kind == AOT::ImageAddressInfo::NotInImage;
                    if (startFrame == vm.topCallFrame)
                        startFrame = topFrame;
                }
            }
#endif
            if (topFrame && !isTheEnginesOwn && skipFirstFrame) {
                CallFrame* first = topFrame;
                readFrame(first);
                gotoNextFrame();
                m_frame.m_index = 0;
                m_topEntryFrameIsEmpty = (m_frame.m_entryFrame != vm.topEntryFrame);
                if (startFrame == first)
                    startFrame = m_frame.callFrame();
                while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
                    gotoNextFrame();
                return;
            }
            if (topFrame && isTheEnginesOwn && (skipFirstFrame || topFrame->isZombieFrame())) {
                CallFrame* first = topFrame;
                CallFrame* adapter = nullptr;
                topFrame = callerOf(first, m_frame.m_entryFrame, m_previousReturnPC);
                topFrame = skipFramesOfStubs(topFrame, m_frame.m_entryFrame, m_previousReturnPC, adapter);
                m_topEntryFrameIsEmpty = (m_frame.m_entryFrame != vm.topEntryFrame);
                if (startFrame == first)
                    startFrame = topFrame;
            }
        }
    }
    readFrame(topFrame);

    // Find the frame the caller wants to start unwinding from.
    while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
        gotoNextFrame();
}

void StackVisitor::gotoNextFrame()
{
    m_frame.m_index++;
#if ENABLE(AOT)
    if (m_frame.m_aotInlineFrame) {
        auto location = m_frame.m_aotFunctionOfFrame.inlineCallSiteLocation(m_frame.m_aotInlineFrame);
        m_frame.m_aotFunction = location.function;
        m_frame.m_aotInlineFrame = location.inlineFrame;
        m_frame.m_isTailDeleted = location.isTailDeleted;
        m_frame.m_bytecodeIndex = location.bytecodeIndex;
        m_frame.m_codeBlock = m_frame.m_aotFunction.codeBlockIfExists();
        return;
    }
    m_frame.m_isTailDeleted = false;
#endif
#if ENABLE(DFG_JIT)
    if (m_frame.isInlinedDFGFrame()) {
        InlineCallFrame* inlineCallFrame = m_frame.inlineCallFrame();
        CodeOrigin* callerCodeOrigin = inlineCallFrame->getCallerSkippingTailCalls();
        if (!callerCodeOrigin) {
            while (inlineCallFrame) {
                readInlinedFrame(m_frame.callFrame(), &inlineCallFrame->directCaller);
                inlineCallFrame = m_frame.inlineCallFrame();
            }
            m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
            readFrame(m_frame.callerFrame());
        } else
            readInlinedFrame(m_frame.callFrame(), callerCodeOrigin);
        return;
    }
#endif // ENABLE(DFG_JIT)
    m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
    readFrame(m_frame.callerFrame());
}

void StackVisitor::unwindToMachineCodeBlockFrame()
{
#if ENABLE(DFG_JIT)
    if (m_frame.isInlinedDFGFrame()) {
        CodeOrigin codeOrigin = m_frame.inlineCallFrame()->directCaller;
        while (codeOrigin.inlineCallFrame())
            codeOrigin = codeOrigin.inlineCallFrame()->directCaller;
        readNonInlinedFrame(m_frame.callFrame(), &codeOrigin);
    }
#endif
}

inline CallFrame* StackVisitor::updatePreviousReturnPCIfNecessary(CallFrame* callFrame)
{
    if (m_frame.m_callFrame) {
        if (m_frame.m_callFrame != callFrame)
            m_previousReturnPC = m_frame.m_callerReturnPC;
    }
    return callFrame;
}

void StackVisitor::readFrame(CallFrame* callFrame)
{
    if (!callFrame) {
        m_frame.setToEnd();
        return;
    }

#if ENABLE(AOT)
    {
        void* returnPC = m_frame.m_callFrame && m_frame.m_callFrame != callFrame ? m_frame.m_callerReturnPC : m_previousReturnPC;
        if (AOT::ImageAddressInfo what = AOT::classifyAddress(removeCodePtrTag(returnPC)); what.kind == AOT::ImageAddressInfo::Function) {
            readAOTFrame(callFrame, removeCodePtrTag(returnPC), what.index);
            return;
        }
    }
#endif

    if (callFrame->isNativeCalleeFrame()) {
        readInlinableNativeCalleeFrame(callFrame);
        return;
    }

#if !ENABLE(DFG_JIT)
    readNonInlinedFrame(callFrame);

#else // !ENABLE(DFG_JIT)
    // If the frame doesn't have a code block, then it's not a DFG frame.
    // Hence, we're not at an inlined frame.
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (!codeBlock) {
        readNonInlinedFrame(callFrame);
        return;
    }

#if ASSERT_ENABLED
    if (!codeBlock->inherits<CodeBlock>()) {
        dataLogLn("Invalid codeblock type: ", *(JSCell*)codeBlock);
        dataLogLn("Callee: ", RawPointer(callFrame->unsafeCallee().rawPtr()));
        ASSERT_NOT_REACHED();
        readNonInlinedFrame(callFrame);
        return;
    }
#endif

    // If the code block does not have any code origins, then there's no
    // inlining. Hence, we're not at an inlined frame.
    if (!codeBlock->hasCodeOrigins()) {
        readNonInlinedFrame(callFrame);
        return;
    }

    CallSiteIndex index = callFrame->callSiteIndex();
    ASSERT(codeBlock->canGetCodeOrigin(index));
    if (!codeBlock->canGetCodeOrigin(index)) {
        // See assertion above. In release builds, we try to protect ourselves
        // from crashing even though stack walking will be goofed up.
        m_frame.setToEnd();
        return;
    }

    CodeOrigin codeOrigin = codeBlock->codeOrigin(index);
    if (!codeOrigin.inlineCallFrame()) {
        readNonInlinedFrame(callFrame, &codeOrigin);
        return;
    }

    readInlinedFrame(callFrame, &codeOrigin);
#endif // !ENABLE(DFG_JIT)
}

void StackVisitor::findCaller(CallFrame* callFrame)
{
    EntryFrame* entryFrame = m_frame.m_entryFrame;
    void* returnPC = nullptr;
    CallFrame* adapter = nullptr;
    CallFrame* caller;
    if (m_frame.m_aotFunction) {
        returnPC = callFrame->rawReturnPC();
        caller = callFrame->callerFrame();
    } else
        caller = callerOf(callFrame, entryFrame, returnPC);
    m_frame.m_callerFrame = skipFramesOfStubs(caller, entryFrame, returnPC, adapter);
    m_frame.m_callerReturnPC = returnPC;
    m_frame.m_callerEntryFrame = entryFrame;
    m_frame.m_callerIsEntryFrame = entryFrame != m_frame.m_entryFrame;
    m_frame.m_aotAdapterFrame = adapter;
}

#if ENABLE(AOT)
void StackVisitor::readAOTFrame(CallFrame* callFrame, void* returnPC, uint32_t index)
{
    // Whether the frame above is that of a callee that is actually running. (It is not if there is no frame above, or if that frame
    // was being set up for a callee that turned out not to be callable.)
    bool calleeRuns = m_frame.m_callFrame && (m_frame.m_aotFunction || m_frame.m_isWasmFrame || (!m_frame.m_callee.isNativeCallee() && m_frame.m_callee.rawPtr() && m_frame.m_callee.asCell()->isCallable()));
    m_frame.m_callFrame = callFrame;
    m_previousReturnPC = returnPC;
    m_frame.m_returnPC = returnPC;
    m_frame.m_argumentCountIncludingThis = 0;
    m_frame.m_isWasmFrame = false;
    m_frame.m_callee = CalleeBits();
#if ENABLE(DFG_JIT)
    m_frame.m_inlineDFGCallFrame = nullptr;
#endif
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;
    if (!m_aotInstance)
        m_aotInstance = AOT::instanceForFrame(callFrame);
    m_frame.m_aotFunctionOfFrame = { m_aotInstance, index };
    auto location = m_frame.m_aotFunctionOfFrame.locationForReturnAddress(returnPC);
    m_frame.m_aotFunction = location.function;
    m_frame.m_aotInlineFrame = location.inlineFrame;
    m_frame.m_isTailDeleted = location.isTailCall && calleeRuns;
    m_frame.m_bytecodeIndex = location.bytecodeIndex;
    m_frame.m_codeBlock = m_frame.m_aotFunction.codeBlockIfExists();
    findCaller(callFrame);
    // The adapter's caller is not AOT code, so the cached Instance no longer applies.
    if (m_frame.m_aotAdapterFrame)
        m_aotInstance = nullptr;
}
#endif

void StackVisitor::readNonInlinedFrame(CallFrame* callFrame, CodeOrigin* codeOrigin)
{
    m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
    m_frame.m_returnPC = m_previousReturnPC;
    m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
    m_frame.m_aotFunction = { };
    m_frame.m_aotInlineFrame = 0;
    findCaller(callFrame);
    m_frame.m_isWasmFrame = false;
    m_frame.m_callee = callFrame->callee();
#if ENABLE(DFG_JIT)
    m_frame.m_inlineDFGCallFrame = nullptr;
#endif
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

    m_frame.m_codeBlock = callFrame->isNativeCalleeFrame() ? nullptr : callFrame->codeBlock();
    m_frame.m_bytecodeIndex = !m_frame.hasCode() ? BytecodeIndex(0)
        : codeOrigin ? codeOrigin->bytecodeIndex()
        : callFrame->bytecodeIndex();

    RELEASE_ASSERT(!callFrame->isNativeCalleeFrame());
}

void StackVisitor::readInlinableNativeCalleeFrame(CallFrame* callFrame)
{
    RELEASE_ASSERT(callFrame->callee().isNativeCallee());
    auto& callee = *callFrame->callee().asNativeCallee();
    switch (callee.category()) {
    case NativeCallee::Category::Wasm: {
#if ENABLE(WEBASSEMBLY)
        auto& wasmCallee = uncheckedDowncast<Wasm::Callee>(callee);
        auto depth = m_frame.m_wasmDistanceFromDeepestInlineFrame;
        m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
        m_frame.m_returnPC = m_previousReturnPC;
        m_frame.m_isWasmFrame = true;
        m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
        m_frame.m_aotFunction = { };
        findCaller(callFrame);
        m_frame.m_callee = callFrame->callee();
        m_frame.m_codeBlock = nullptr;
        m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;
        m_frame.m_wasmCallSiteIndexBits = callFrame->callSiteIndex().bits();

        m_frame.m_wasmFunctionIndexOrName = wasmCallee.indexOrName();
        m_frame.m_wasmFunctionIndex = wasmCallee.index();

#if ENABLE(WEBASSEMBLY_OMGJIT)
        bool canInline = isAnyOMG(wasmCallee.compilationMode());
        if (!canInline)
            return;

        const auto& omgCallee = uncheckedDowncast<const Wasm::OptimizingJITCallee>(wasmCallee);
        bool isInlined = false;

        // Because PC is just after the call instruction, to query to the origin for the call instruction, we decrease it by 1.
        // While it can be pointing at the broken offset (e.g. all ARM64 instructions are 4-byte aligned), it is still fine since map is controlling pc with range.
        auto callSiteIndexFromPC = omgCallee.tryGetCallSiteIndex(std::bit_cast<void*>(std::bit_cast<uintptr_t>(removeCodePtrTag<void*>(m_frame.m_returnPC)) - 1));
        RELEASE_ASSERT(callSiteIndexFromPC);
        CallSiteIndex callSiteIndex = callSiteIndexFromPC.value();
        m_frame.m_wasmCallSiteIndexBits = callSiteIndex.bits();

        auto codeOrigin = omgCallee.getCodeOrigin(callSiteIndex.bits(), depth, isInlined);
        auto indexOrName = omgCallee.getIndexOrName(codeOrigin);
        if (!isInlined)
            return;

        // The callerFrame just needs to be non-null to indicate that we
        // haven't reached the last frame yet.
        m_frame.m_callerFrame = callFrame;
        m_frame.m_wasmDistanceFromDeepestInlineFrame = depth + 1;
        m_frame.m_wasmFunctionIndexOrName = indexOrName;
        m_frame.m_wasmFunctionIndex = codeOrigin->functionIndex;
#else
        UNUSED_VARIABLE(depth);
#endif
#else
        UNUSED_PARAM(callFrame);
#endif
        break;
    }
    case NativeCallee::Category::InlineCache: {
        m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
        m_frame.m_returnPC = m_previousReturnPC;
        m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
        m_frame.m_aotFunction = { };
        findCaller(callFrame);
        m_frame.m_isWasmFrame = false;
        m_frame.m_callee = callFrame->callee();
#if ENABLE(DFG_JIT)
        m_frame.m_inlineDFGCallFrame = nullptr;
#endif
        m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

        m_frame.m_codeBlock = nullptr;
        m_frame.m_bytecodeIndex = BytecodeIndex(0);
        break;
    }
    }
}

#if ENABLE(DFG_JIT)
static int NODELETE inlinedFrameOffset(CodeOrigin* codeOrigin)
{
    InlineCallFrame* inlineCallFrame = codeOrigin->inlineCallFrame();
    int frameOffset = inlineCallFrame ? inlineCallFrame->stackOffset : 0;
    return frameOffset;
}

void StackVisitor::readInlinedFrame(CallFrame* callFrame, CodeOrigin* codeOrigin)
{
    ASSERT(codeOrigin);
    m_frame.m_isWasmFrame = false;
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

    int frameOffset = inlinedFrameOffset(codeOrigin);
    bool isInlined = !!frameOffset;
    if (isInlined) {
        InlineCallFrame* inlineCallFrame = codeOrigin->inlineCallFrame();

        m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
        m_frame.m_returnPC = m_previousReturnPC;
        m_frame.m_inlineDFGCallFrame = inlineCallFrame;
        if (inlineCallFrame->argumentCountRegister.isValid())
            m_frame.m_argumentCountIncludingThis = callFrame->r(inlineCallFrame->argumentCountRegister).unboxedInt32();
        else
            m_frame.m_argumentCountIncludingThis = inlineCallFrame->argumentCountIncludingThis;
        m_frame.m_codeBlock = inlineCallFrame->baselineCodeBlock.get();
        m_frame.m_aotFunction = { };
        m_frame.m_bytecodeIndex = codeOrigin->bytecodeIndex();

        JSFunction* callee = inlineCallFrame->calleeForCallFrame(callFrame);
        m_frame.m_callee = callee;
        ASSERT(!!m_frame.callee().rawPtr());

        // The callerFrame just needs to be non-null to indicate that we
        // haven't reached the last frame yet. Setting it to the root
        // frame (i.e. the callFrame that this inlined frame is called from)
        // would work just fine.
        m_frame.m_callerFrame = callFrame;
        return;
    }

    readNonInlinedFrame(callFrame, codeOrigin);
}
#endif // ENABLE(DFG_JIT)

CodeBlock* StackVisitor::Frame::makeCodeBlock() const
{
#if ENABLE(AOT)
    m_codeBlock = m_aotFunction.ensureData()->ensureCodeBlock();
#endif
    return m_codeBlock;
}

ScriptExecutable* StackVisitor::Frame::ownerExecutable() const
{
#if ENABLE(AOT)
    if (m_aotFunction)
        return m_aotFunction.executable();
#endif
    return m_codeBlock ? m_codeBlock->ownerExecutable() : nullptr;
}

bool StackVisitor::Frame::isBuiltinFunction() const
{
#if ENABLE(AOT)
    if (m_aotFunction)
        return m_aotFunction.isBuiltinFunction();
#endif
    return m_codeBlock->unlinkedCodeBlock()->isBuiltinFunction();
}

JSGlobalObject* StackVisitor::Frame::lexicalGlobalObject(VM& vm) const
{
#if ENABLE(AOT)
    if (m_aotFunction)
        return m_aotFunction.instance->globalObject;
#endif
    return m_callFrame->lexicalGlobalObject(vm);
}

StackVisitor::Frame::CodeType StackVisitor::Frame::codeType() const
{
    if (isNativeCalleeFrame()) {
        auto* nativeCallee = callee().asNativeCallee();
        switch (nativeCallee->category()) {
        case NativeCallee::Category::Wasm:
            return CodeType::Wasm;
        case NativeCallee::Category::InlineCache:
            return CodeType::Native;
        }
        return CodeType::Native;
    }

    if (!hasCode())
        return CodeType::Native;

#if ENABLE(AOT)
    JSC::CodeType type = m_aotFunction ? m_aotFunction.codeType() : m_codeBlock->codeType();
#else
    JSC::CodeType type = m_codeBlock->codeType();
#endif
    switch (type) {
    case EvalCode:
        return CodeType::Eval;
    case ModuleCode:
        return CodeType::Module;
    case FunctionCode:
        return CodeType::Function;
    case GlobalCode:
        return CodeType::Global;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return CodeType::Global;
}

#if ENABLE(ASSEMBLER)
const RegisterAtOffsetList* StackVisitor::Frame::calleeSaveRegistersForUnwinding()
{
    if (!NUMBER_OF_CALLEE_SAVES_REGISTERS)
        return nullptr;

    if (isInlinedDFGFrame())
        return nullptr;

    if (isNativeCalleeFrame()) {
        auto* nativeCallee = callee().asNativeCallee();
        switch (nativeCallee->category()) {
        case NativeCallee::Category::Wasm: {
#if ENABLE(WEBASSEMBLY)
            auto* wasmCallee = uncheckedDowncast<Wasm::Callee>(nativeCallee);
            if (auto* calleeSaveRegisters = wasmCallee->calleeSaveRegisters())
                return calleeSaveRegisters;
#endif // ENABLE(WEBASSEMBLY)
            break;
        }
        case NativeCallee::Category::InlineCache: {
            break;
        }
        }
        return nullptr;
    }

#if ENABLE(AOT)
    if (m_aotFunction)
        return AOT::calleeSaveRegistersOf(*m_aotFunctionOfFrame.info().function());
#endif
    if (CodeBlock* codeBlock = this->codeBlock())
        return codeBlock->jitCode()->calleeSaveRegisters();

    return nullptr;
}
#endif // ENABLE(ASSEMBLER)

String StackVisitor::Frame::functionName() const
{
    String traceLine;

    switch (codeType()) {
    case CodeType::Wasm:
        traceLine = makeString(m_wasmFunctionIndexOrName);
        break;
    case CodeType::Eval:
        traceLine = "eval code"_s;
        break;
    case CodeType::Module:
        traceLine = "module code"_s;
        break;
    case CodeType::Native: {
        JSCell* callee = this->callee().asCell();
        if (callee)
            traceLine = getCalculatedDisplayName(callFrame()->deprecatedVM(), uncheckedDowncast<JSObject>(callee)).impl();
        break;
    }
    case CodeType::Function:
        if (JSCell* callee = this->callee().asCell())
            traceLine = getCalculatedDisplayName(callFrame()->deprecatedVM(), uncheckedDowncast<JSObject>(callee)).impl();
        else if (auto* executable = dynamicDowncast<FunctionExecutable>(ownerExecutable()))
            traceLine = executable->ecmaNameWithoutGC();
        break;
    case CodeType::Global:
        traceLine = "global code"_s;
        break;
    }
    return traceLine.isNull() ? emptyString() : traceLine;
}

String StackVisitor::Frame::sourceURL() const
{
    String traceLine;

    switch (codeType()) {
    case CodeType::Eval:
    case CodeType::Module:
    case CodeType::Function:
    case CodeType::Global: {
        String sourceURL = ownerExecutable()->sourceURL();
        if (!sourceURL.isEmpty())
            traceLine = sourceURL.impl();
        break;
    }
    case CodeType::Native:
        traceLine = "[native code]"_s;
        break;
    case CodeType::Wasm:
        traceLine = "[wasm code]"_s;
        break;
    }
    return traceLine.isNull() ? emptyString() : traceLine;
}

String StackVisitor::Frame::preRedirectURL() const
{
    String traceLine;

    switch (codeType()) {
    case CodeType::Eval:
    case CodeType::Module:
    case CodeType::Function:
    case CodeType::Global: {
        String preRedirectURL = ownerExecutable()->preRedirectURL();
        if (!preRedirectURL.isEmpty())
            traceLine = preRedirectURL.impl();
        break;
    }
    case CodeType::Native:
    case CodeType::Wasm:
        break;
    }

    return traceLine.isNull() ? emptyString() : traceLine;
}

String StackVisitor::Frame::toString() const
{
    String functionName = this->functionName();
    String sourceURL = this->sourceURL();
    auto separator = !sourceURL.isEmpty() && !functionName.isEmpty() ? "@"_s : ""_s;

    if (sourceURL.isEmpty() || !hasLineAndColumnInfo())
        return makeString(functionName, separator, sourceURL);

    auto lineColumn = computeLineAndColumn();
    return makeString(functionName, separator, sourceURL, ':', lineColumn.line, ':', lineColumn.column);
}

SourceID StackVisitor::Frame::sourceID()
{
    if (ScriptExecutable* executable = ownerExecutable())
        return executable->sourceID();
    return noSourceID;
}

ClonedArguments* StackVisitor::Frame::createArguments(VM& vm)
{
    ASSERT(m_callFrame);
    CallFrame* physicalFrame = m_callFrame;
    // FIXME: Revisit JSGlobalObject.
    // https://bugs.webkit.org/show_bug.cgi?id=203204
    JSGlobalObject* globalObject = physicalFrame->lexicalGlobalObject(vm);
    ClonedArguments* arguments;
    ArgumentsMode mode;
    if (Options::useFunctionDotArguments())
        mode = ArgumentsMode::Cloned;
    else
        mode = ArgumentsMode::FakeValues;
#if ENABLE(DFG_JIT)
    if (isInlinedDFGFrame()) {
        ASSERT(m_inlineDFGCallFrame);
        arguments = ClonedArguments::createWithInlineFrame(globalObject, physicalFrame, m_inlineDFGCallFrame, mode);
    } else 
#endif
        arguments = ClonedArguments::createWithMachineFrame(globalObject, physicalFrame, mode);
    return arguments;
}

bool StackVisitor::Frame::hasLineAndColumnInfo() const
{
    return hasCode();
}

LineColumn StackVisitor::Frame::computeLineAndColumn() const
{
    if (!hasCode())
        return { };

    ScriptExecutable* executable = ownerExecutable();
#if ENABLE(AOT)
    auto lineColumn = m_aotFunction ? m_aotFunction.lineColumnFor(bytecodeIndex()) : m_codeBlock->lineColumnForBytecodeIndex(bytecodeIndex());
#else
    auto lineColumn = m_codeBlock->lineColumnForBytecodeIndex(bytecodeIndex());
#endif

    if (std::optional<int> overrideLineNumber = executable->overrideLineNumber(executable->vm()))
        lineColumn.line = overrideLineNumber.value();

    return lineColumn;
}

void StackVisitor::Frame::setToEnd()
{
    m_callFrame = nullptr;
#if ENABLE(DFG_JIT)
    m_inlineDFGCallFrame = nullptr;
#endif
    m_isWasmFrame = false;
}

bool StackVisitor::Frame::isFrameOf(JSCell* function) const
{
    if (callee().isNativeCallee())
        return false;
    if (JSCell* callee = this->callee().asCell())
        return callee == function;
    // It was called as no object. That is only done to a function of which the realm has one closure (AOT::KnownFunction::isExact).
    auto* jsFunction = dynamicDowncast<JSFunction>(function);
    return jsFunction && hasCode() && jsFunction->executable() == ownerExecutable();
}

bool StackVisitor::Frame::isImplementationVisibilityPrivate() const
{
    ImplementationVisibility implementationVisibility = [&] () -> ImplementationVisibility {
        if (hasCode()) {
            if (auto* executable = ownerExecutable())
                return executable->implementationVisibility();
            return ImplementationVisibility::Public;
        }

#if ENABLE(WEBASSEMBLY)
        if (isNativeCalleeFrame())
            return callee().asNativeCallee()->implementationVisibility();
#endif

        if (callee().isCell()) {
            if (auto* callee = this->callee().asCell()) {
                if (auto* jsFunction = dynamicDowncast<JSFunction>(callee)) {
                    if (auto* executable = jsFunction->executable())
                        return executable->implementationVisibility();
                    return ImplementationVisibility::Public;
                }
            }
        }

        return ImplementationVisibility::Public;
    }();
    switch (implementationVisibility) {
    case ImplementationVisibility::Public:
        return false;

    case ImplementationVisibility::Private:
    case ImplementationVisibility::PrivateRecursive:
        return !Options::showPrivateScriptsInStackTraces();
    }

    ASSERT_NOT_REACHED();
    return false;
}

size_t StackVisitor::Frame::wasmFunctionIndex() const
{
    ASSERT(isNativeCalleeFrame());
    ASSERT(m_isWasmFrame);
    return m_wasmFunctionIndex;
}

CallSiteIndex StackVisitor::Frame::wasmCallSiteIndex() const
{
    return CallSiteIndex::fromBits(m_wasmCallSiteIndexBits);
}

void StackVisitor::Frame::dump(PrintStream& out, Indenter indent) const
{
    dump(out, indent, [] (PrintStream&) { });
}

void StackVisitor::Frame::dump(PrintStream& out, Indenter indent, WTF::Function<void(PrintStream&)> prefix) const
{
    if (!this->callFrame()) {
        out.print(indent, "frame 0x0\n");
        return;
    }

    if (m_aotFunction) {
        out.print(indent);
        prefix(out);
        out.print("frame ", RawPointer(this->callFrame()), " { name: ", functionName(), " sourceURL: ", sourceURL(), " ", bytecodeIndex(), " }\n");
        return;
    }

    CodeBlock* codeBlock = this->codeBlock();
    out.print(indent);
    prefix(out);
    out.print("frame ", RawPointer(this->callFrame()), " {\n");

    {
        indent++;

        CallFrame* callFrame = m_callFrame;
        CallFrame* callerFrame = this->callerFrame();
        const void* returnPC = callFrame->hasReturnPC() ? callFrame->returnPCForInspection() : nullptr;

        out.print(indent, "name: ", functionName(), "\n");
        out.print(indent, "sourceURL: ", sourceURL(), "\n");

        bool isInlined = false;
#if ENABLE(DFG_JIT)
        isInlined = isInlinedDFGFrame();
        out.print(indent, "isInlinedDFGFrame: ", isInlinedDFGFrame(), "\n");
        if (isInlinedDFGFrame())
            out.print(indent, "InlineCallFrame: ", RawPointer(m_inlineDFGCallFrame), "\n");
#endif

        out.print(indent, "callee: ", RawPointer(callee().rawPtr()), "\n");
        out.print(indent, "returnPC: ", RawPointer(returnPC), "\n");
        out.print(indent, "callerFrame: ", RawPointer(callerFrame), "\n");
        uintptr_t locationRawBits = callFrame->callSiteAsRawBits();
        out.print(indent, "rawLocationBits: ", locationRawBits,
            " ", RawHex(locationRawBits), "\n");
        out.print(indent, "codeBlock: ", RawPointer(codeBlock));
        if (codeBlock)
            out.print(" ", *codeBlock);
        out.print("\n");
        if (codeBlock && !isInlined) {
            indent++;

            if (callFrame->callSiteBitsAreBytecodeOffset()) {
                BytecodeIndex bytecodeIndex = callFrame->bytecodeIndex();
                out.print(indent, bytecodeIndex, " of ", codeBlock->instructions().size(), "\n");
#if ENABLE(DFG_JIT)
            } else {
                out.print(indent, "hasCodeOrigins: ", codeBlock->hasCodeOrigins(), "\n");
                if (codeBlock->hasCodeOrigins()) {
                    CallSiteIndex callSiteIndex = callFrame->callSiteIndex();
                    out.print(indent, "callSiteIndex: ", callSiteIndex.bits(), " of ", codeBlock->codeOrigins().size(), "\n");

                    JITType jitType = codeBlock->jitType();
                    if (jitType != JITType::FTLJIT) {
                        RefPtr jitCode = codeBlock->jitCode();
                        out.print(indent, "jitCode: ", RawPointer(jitCode.get()),
                            " start ", RawPointer(jitCode->start()),
                            " end ", RawPointer(jitCode->end()), "\n");
                    }
                }
#endif
            }
            auto lineColumn = computeLineAndColumn();
            out.print(indent, "line: ", lineColumn.line, "\n");
            out.print(indent, "column: ", lineColumn.column, "\n");

            indent--;
        }
        out.print(indent, "EntryFrame: ", RawPointer(m_entryFrame), "\n");
        indent--;
    }
    out.print(indent, "}\n");
}

} // namespace JSC
