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

#include "AOTCompiledCallee.h"
#include "AOTCompiledCaller.h"
#include "AOTCompiledFrame.h"
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
#include <memory>
#include <wtf/text/MakeString.h>

namespace JSC {

StackVisitor::StackVisitor(CallFrame* startFrame, VM& vm, bool skipFirstFrame)
{
    CallFrame* topFrame = nullptr;
    if (startFrame) {
        ASSERT(!vm.topCallFrame || static_cast<void*>(vm.topCallFrame) != vm.topEntryFrame);

        m_frame.m_entryFrame = vm.topEntryFrame;
        topFrame = vm.topCallFrame;
        if (topFrame) {
            m_previousReturnPC = vm.maybeReturnPC;
            if (skipFirstFrame || topFrame->isZombieFrame()) {
                m_previousReturnPC = topFrame->rawReturnPC();
                topFrame = topFrame->callerFrame(m_frame.m_entryFrame);
                m_topEntryFrameIsEmpty = (m_frame.m_entryFrame != vm.topEntryFrame);
                if (startFrame == vm.topCallFrame)
                    startFrame = topFrame;
            }
        }
    }
    readFrame(topFrame);

    // Find the frame the caller wants to start unwinding from.
    while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
        gotoNextFrame();
}

#if ENABLE(AOT)
StackVisitor::WithCompiledFrames::WithCompiledFrames(CallFrame* startFrame, VM& vm, bool skipFirstFrame, CompiledFrameVisit compiledFrameVisit)
    : m_vm(vm)
    , m_compiledFrameVisit(compiledFrameVisit)
{
    bool startsAtTop = startFrame == vm.topCallFrame;
    CallFrame* topFrame = nullptr;
    if (startFrame) {
        ASSERT(!vm.topCallFrame || static_cast<void*>(vm.topCallFrame) != vm.topEntryFrame);

        m_frame.m_entryFrame = vm.topEntryFrame;
        topFrame = vm.topCallFrame;
        if (topFrame) {
            m_previousReturnPC = vm.maybeReturnPC;
            m_nextFrameIsPublished = true;
            if (tryEnterCompiledFrames(topFrame)) {
                startInCompiledFrames(startFrame, skipFirstFrame);
                return;
            }
            if (skipFirstFrame || topFrame->isZombieFrame()) {
                m_previousReturnPC = topFrame->rawReturnPC();
                m_previousCalleeFrame = topFrame;
                topFrame = topFrame->callerFrame(m_frame.m_entryFrame);
                m_topEntryFrameIsEmpty = (m_frame.m_entryFrame != vm.topEntryFrame);
                m_nextFrameIsPublished = m_topEntryFrameIsEmpty;
                if (startFrame == vm.topCallFrame)
                    startFrame = topFrame;
            }
        }
    }
    readFrame(topFrame);
    if (startsAtTop)
        return;

    while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
        gotoNextFrame();
}

StackVisitor::WithCompiledFrames::WithCompiledFrames(VM& vm, const AOT::CompiledFrameExit& exit, CompiledFrameVisit compiledFrameVisit)
    : m_vm(vm)
    , m_shownFrameFilter(true)
    , m_compiledFrameVisit(compiledFrameVisit)
{
    m_frame.m_entryFrame = exit.entryFrame;
    m_previousReturnPC = exit.returnPC;
    m_previousCalleeFrame = exit.calleeFrame;
    m_nextFrameIsPublished = exit.crossedEntryFrame;
    readFrame(exit.callFrame);
}

StackVisitor::WithCompiledFrames::~WithCompiledFrames()
{
    if (m_frame.m_compiledFrames)
        std::destroy_at(m_frame.m_compiledFrames);
}

void StackVisitor::WithCompiledFrames::startInCompiledFrames(CallFrame* startFrame, bool skipFirstFrame)
{
    bool startsAtTop = startFrame == m_vm.topCallFrame;
    if (!showFirstCompiledFrame())
        readFrame(leaveCompiledFrames());
    if (skipFirstFrame && m_frame.callFrame()) {
        gotoNextFrame();
        m_frame.m_index = 0;
    }
    m_topEntryFrameIsEmpty = m_frame.m_entryFrame != m_vm.topEntryFrame;
    if (startsAtTop)
        return;
    while (m_frame.callFrame() && m_frame.callFrame() != startFrame)
        gotoNextFrame();
}

bool StackVisitor::WithCompiledFrames::tryEnterCompiledFrames(CallFrame* callFrame)
{
    using OperationState = AOT::CompiledFrameIterator::OperationState;
    CallFrame* callee = m_frame.m_callFrame;
    if (callee == callFrame)
        return false;
    bool isPublished = callee ? m_frame.m_callerIsEntryFrame : m_nextFrameIsPublished;
    void* returnPC = callee ? callee->rawReturnPC() : m_previousReturnPC;
    CallFrame* calleeFrame = callee ? callee : m_previousCalleeFrame;
    OperationState operationState = OperationState::MayNotHaveCalled;
    if (isPublished) {
        if (!AOT::isOperationFrameRecord(m_vm, callFrame))
            return false;
        if (callFrame != m_vm.topCallFrame)
            operationState = OperationState::HasCalled;
        auto* record = std::bit_cast<CallerFrameAndPC*>(callFrame);
        calleeFrame = callFrame;
        callFrame = record->callerFrame;
        returnPC = record->returnPC;
    }
    auto frames = AOT::CompiledFrameIterator::tryCreate(m_vm, callFrame, removeCodePtrTag(returnPC), m_frame.m_entryFrame, operationState, calleeFrame ? AOT::stackPointerOfCaller(calleeFrame) : nullptr);
    RELEASE_ASSERT(frames || !isPublished);
    if (!frames)
        return false;
    static_assert(sizeof(AOT::CompiledFrameIterator) <= compiledFrameIteratorCapacity && alignof(AOT::CompiledFrameIterator) <= alignof(void*));
    ASSERT(!m_frame.m_compiledFrames);
    m_frame.m_compiledFrames = new (NotNull, m_compiledFramesStorage) AOT::CompiledFrameIterator(WTF::move(*frames));
    setCompiledFrame(callFrame, nullptr);
    return true;
}

bool StackVisitor::WithCompiledFrames::showFirstCompiledFrame()
{
    return m_compiledFrameVisit == CompiledFrameVisit::EachRunOnce || showNextCompiledFrame();
}

bool StackVisitor::WithCompiledFrames::showNextCompiledFrame()
{
    auto& frames = *m_frame.m_compiledFrames;
    bool showsCalledBuiltin = m_compiledFrameVisit == CompiledFrameVisit::ShownFramesAndCalledBuiltins && !std::exchange(m_frame.m_isOfBuiltinCalledByCompiledFrame, false);
    for (; !frames.isAtEnd(); frames.advance(), showsCalledBuiltin = m_compiledFrameVisit == CompiledFrameVisit::ShownFramesAndCalledBuiltins) {
        auto& frame = frames.frame();
        if (showsCalledBuiltin && frame.calledBuiltin) {
            m_frame.m_isOfBuiltinCalledByCompiledFrame = m_shownFrameFilter.shows(AOT::TraceVisibility::Visible, false);
            setCompiledFrame(frame.callFrame, frame.returnAddress);
            return true;
        }
        if (m_shownFrameFilter.shows(frame.visibility, frame.isTailDeleted)) {
            setCompiledFrame(frame.callFrame, frame.returnAddress);
            return true;
        }
    }
    return false;
}

void StackVisitor::WithCompiledFrames::setCompiledFrame(CallFrame* callFrame, const void* returnAddress)
{
    m_frame.m_callFrame = callFrame;
    m_frame.m_returnPC = m_previousReturnPC = const_cast<void*>(returnAddress);
    m_frame.m_argumentCountIncludingThis = 0;
    m_frame.m_isWasmFrame = false;
    m_frame.m_callee = CalleeBits();
    m_frame.m_codeBlock = nullptr;
    m_frame.m_bytecodeIndex = BytecodeIndex();
#if ENABLE(DFG_JIT)
    m_frame.m_inlineDFGCallFrame = nullptr;
#endif
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;
    m_frame.m_callerFrame = callFrame;
    m_frame.m_callerEntryFrame = m_frame.m_entryFrame;
    m_frame.m_callerIsEntryFrame = false;
}

CallFrame* StackVisitor::WithCompiledFrames::leaveCompiledFrames()
{
    auto exit = m_frame.m_compiledFrames->exit();
    std::destroy_at(std::exchange(m_frame.m_compiledFrames, nullptr));
    m_frame.m_callFrame = nullptr;
    m_frame.m_entryFrame = exit.entryFrame;
    m_previousReturnPC = exit.returnPC;
    m_previousCalleeFrame = exit.calleeFrame;
    m_nextFrameIsPublished = exit.crossedEntryFrame;
    return exit.callFrame;
}

void StackVisitor::WithCompiledFrames::gotoNextCompiledFrame()
{
    auto& frames = *m_frame.m_compiledFrames;
    if (m_compiledFrameVisit == CompiledFrameVisit::EachRunOnce) {
        while (!frames.isAtEnd())
            frames.advanceToCallerMachineFrame();
    } else {
        if (!m_frame.m_isOfBuiltinCalledByCompiledFrame)
            frames.advance();
        if (showNextCompiledFrame())
            return;
    }
    readFrame(leaveCompiledFrames());
}

void StackVisitor::WithCompiledFrames::gotoNextFrame()
{
    m_frame.m_index++;
    if (m_frame.m_compiledFrames) {
        gotoNextCompiledFrame();
        return;
    }
    m_shownFrameFilter = AOT::ShownFrameFilter(true);
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
#endif
    m_frame.m_entryFrame = m_frame.m_callerEntryFrame;
    readFrame(m_frame.callerFrame());
}

void StackVisitor::WithCompiledFrames::readFrame(CallFrame* callFrame)
{
    while (callFrame && tryEnterCompiledFrames(callFrame)) {
        if (showFirstCompiledFrame())
            return;
        callFrame = leaveCompiledFrames();
    }
    StackVisitor::readFrame(callFrame);
}
#endif // ENABLE(AOT)

void StackVisitor::gotoNextFrame()
{
    m_frame.m_index++;
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
            m_previousReturnPC = m_frame.m_callFrame->rawReturnPC();
    }
    return callFrame;
}

void StackVisitor::readFrame(CallFrame* callFrame)
{
    if (!callFrame) {
        m_frame.setToEnd();
        return;
    }

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

void StackVisitor::readNonInlinedFrame(CallFrame* callFrame, CodeOrigin* codeOrigin)
{
    m_frame.m_callFrame = updatePreviousReturnPCIfNecessary(callFrame);
    m_frame.m_returnPC = m_previousReturnPC;
    m_frame.m_argumentCountIncludingThis = callFrame->argumentCountIncludingThis();
    m_frame.m_callerEntryFrame = m_frame.m_entryFrame;
    m_frame.m_callerFrame = callFrame->callerFrame(m_frame.m_callerEntryFrame);
    m_frame.m_callerIsEntryFrame = m_frame.m_callerEntryFrame != m_frame.m_entryFrame;
    m_frame.m_isWasmFrame = false;
    m_frame.m_callee = callFrame->callee();
#if ENABLE(DFG_JIT)
    m_frame.m_inlineDFGCallFrame = nullptr;
#endif
    m_frame.m_wasmDistanceFromDeepestInlineFrame = 0;

    m_frame.m_codeBlock = callFrame->isNativeCalleeFrame() ? nullptr : callFrame->codeBlock();
    m_frame.m_bytecodeIndex = !m_frame.codeBlock() ? BytecodeIndex(0)
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
        m_frame.m_callerEntryFrame = m_frame.m_entryFrame;
        m_frame.m_callerFrame = callFrame->callerFrame(m_frame.m_callerEntryFrame);
        m_frame.m_callerIsEntryFrame = m_frame.m_callerEntryFrame != m_frame.m_entryFrame;
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
        if (!omgCallee.hasCallSiteIndexMap())
            return;
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
        m_frame.m_callerEntryFrame = m_frame.m_entryFrame;
        m_frame.m_callerFrame = callFrame->callerFrame(m_frame.m_callerEntryFrame);
        m_frame.m_callerIsEntryFrame = m_frame.m_callerEntryFrame != m_frame.m_entryFrame;
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
static int LEGACY_NODELETE inlinedFrameOffset(CodeOrigin* codeOrigin)
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

#if ENABLE(AOT)
const AOT::CompiledFrame& StackVisitor::Frame::compiledFrame() const
{
    return m_compiledFrames->frame();
}

bool StackVisitor::Frame::isConstructCall() const
{
    return m_compiledFrames->isConstructCall();
}
#endif

JSGlobalObject* StackVisitor::Frame::lexicalGlobalObject(VM& vm) const
{
#if ENABLE(AOT)
    if (isCompiledFrame())
        return compiledFrame().globalObject();
#endif
    return m_callFrame->lexicalGlobalObject(vm);
}

static StackVisitor::Frame::CodeType codeTypeOf(JSC::CodeType codeType)
{
    switch (codeType) {
    case EvalCode:
        return StackVisitor::Frame::CodeType::Eval;
    case ModuleCode:
        return StackVisitor::Frame::CodeType::Module;
    case FunctionCode:
        return StackVisitor::Frame::CodeType::Function;
    case GlobalCode:
        return StackVisitor::Frame::CodeType::Global;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return StackVisitor::Frame::CodeType::Global;
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

#if ENABLE(AOT)
    if (isCompiledFrame())
        return codeTypeOf(AOT::FunctionDescription(compiledFrame().function).codeType());
#endif

    if (!codeBlock())
        return CodeType::Native;

    return codeTypeOf(codeBlock()->codeType());
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
#if ENABLE(AOT)
        if (isCompiledFrame()) {
            traceLine = AOT::FunctionDescription(compiledFrame().function).nameInFrames(compiledFrame().globalObject()->vm());
            break;
        }
#endif
        traceLine = getCalculatedDisplayName(callFrame()->deprecatedVM(), uncheckedDowncast<JSObject>(this->callee().asCell())).impl();
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
#if ENABLE(AOT)
        if (isCompiledFrame()) {
            traceLine = compiledFrame().sourceURL();
            break;
        }
#endif
        String sourceURL = codeBlock()->ownerExecutable()->sourceURL();
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
#if ENABLE(AOT)
        if (isCompiledFrame())
            break;
#endif
        String preRedirectURL = codeBlock()->ownerExecutable()->preRedirectURL();
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
    if (CodeBlock* codeBlock = this->codeBlock())
        return codeBlock->ownerExecutable()->sourceID();
    return noSourceID;
}

ClonedArguments* StackVisitor::Frame::createArguments(VM& vm)
{
    ASSERT(m_callFrame);
    ASSERT(!isCompiledFrame());
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
    return !!codeBlock() || isCompiledFrame();
}

LineColumn StackVisitor::Frame::computeLineAndColumn() const
{
#if ENABLE(AOT)
    if (isCompiledFrame())
        return compiledFrame().lineColumn();
#endif
    CodeBlock* codeBlock = this->codeBlock();
    if (!codeBlock)
        return { };

    auto lineColumn = codeBlock->lineColumnForBytecodeIndex(bytecodeIndex());

    if (std::optional<int> overrideLineNumber = codeBlock->ownerExecutable()->overrideLineNumber(codeBlock->vm()))
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
#if ENABLE(AOT)
    if (isOfBuiltinCalledByCompiledFrame())
        return AOT::isClosureOf(function, AOT::functionInFramesOf(compiledFrame().calledBuiltin));
    if (isCompiledFrame())
        return compiledFrame().isActivationOf(function);
#endif
    return callee().asCell() == function;
}

bool StackVisitor::Frame::isImplementationVisibilityPrivate() const
{
    ImplementationVisibility implementationVisibility = [&] () -> ImplementationVisibility {
        if (auto* codeBlock = this->codeBlock()) {
            if (auto* executable = codeBlock->ownerExecutable())
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
#if ENABLE(AOT)
                // A native frame under a function object of compiled code only enters that code, whatever form the function object has.
                if (AOT::CompiledCallee::isCompiledCallee(callee))
                    return ImplementationVisibility::Private;
#endif
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

#if ENABLE(AOT)
    if (isCompiledFrame()) {
        out.print(indent);
        prefix(out);
        out.print("frame ", RawPointer(this->callFrame()), " { name: ", functionName(), " sourceURL: ", sourceURL(), " ", compiledFrame().callSite.bits(), " }\n");
        return;
    }
#endif

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
