/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnwinding.h"

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTHandlerTable.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTSavedFrame.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubFrames.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "DeferGCInlines.h"
#include "HandlerInfo.h"
#include "Interpreter.h"
#include "JITExceptions.h"
#include "LLIntExceptions.h"
#include "OwedOutOfMemoryError.h"
#include "Reg.h"
#include "RegisterSet.h"
#include "VM.h"
#include "VMEntryRecord.h"
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

static CompiledHandlerKind kindOf(HandlerType type)
{
    switch (type) {
    case HandlerType::Catch:
    case HandlerType::SynthesizedCatch:
        return CompiledHandlerKind::Catch;
    case HandlerType::Finally:
        return CompiledHandlerKind::Finally;
    case landingOfMisfit:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool letsThrowPass(StubFrameKind kind)
{
    switch (kind) {
    case StubFrameKind::Plain:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::AtStubEntry:
        return true;
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::HandlerOfJob:
    case StubFrameKind::AsyncGeneratorBody:
    case StubFrameKind::InHostCallFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void completeExitedGenerator(const CompiledFrameIterator& frames)
{
    if (CallFrame* stubFrame = frames.generatorStubFrameLeft())
        completeGeneratorLeftByThrow(stubFrame);
}

static void copyFrameSavedRegisters(VMEntryRecord& record, std::span<const int8_t> bufferSlots, CallFrame* callFrame, const ImageFrame& imageFrame)
{
    const CPURegister* slot = reinterpret_cast<const CPURegister*>(callFrame->registers()) - imageFrame.startOfCalleeSaves();
    for (uint64_t mask = ImageFrame::unpackRegisters(imageFrame.calleeSaveRegisters); mask; mask &= mask - 1, ++slot) {
        int8_t bufferSlot = bufferSlots[WTF::ctz(mask)];
        ASSERT(bufferSlot >= 0);
        record.calleeSaveRegistersBuffer[bufferSlot] = *slot;
    }
}

static void copyStubSavedRegisters(VMEntryRecord& record, std::span<const int8_t> bufferSlots, CallFrame* stubFrame, std::span<const AdapterFrame::SavedRegister> savedRegisters)
{
    for (auto saved : savedRegisters) {
        int8_t bufferSlot = bufferSlots[Reg(saved.reg).index()];
        RELEASE_ASSERT(bufferSlot >= 0);
        record.calleeSaveRegistersBuffer[bufferSlot] = *reinterpret_cast<const CPURegister*>(reinterpret_cast<const char*>(stubFrame) + saved.offset);
    }
}

static void unwindStubFrames(VMEntryRecord& record, std::span<const int8_t> bufferSlots, const CompiledFrameIterator& frames)
{
    completeExitedGenerator(frames);
    if (CallFrame* stubFrame = frames.engineCalleeStubFrameLeft())
        copyStubSavedRegisters(record, bufferSlots, stubFrame, EngineCalleeFrame::savedRegisters);
}

static CompiledUnwindResult finishAtHandler(Instance& instance, const CompiledHandler& handler, unsigned frameSize)
{
    char* stackPointer = reinterpret_cast<char*>(handler.callFrame) - frameSize;
    instance.json.didUnwindTo(stackPointer);
    instance.runtime.exceptions.stackPointerOfHandler = stackPointer;
    return { CompiledUnwindStatus::FoundHandler, handler };
}

// The stub takes the thrown value, hands it to whoever waits for the coroutine and returns from the frame.
static CompiledUnwindResult finishInRejectingStub(Instance& instance, CallFrame* callFrame, const ImageFrame& frame)
{
    static void* const code[] = { addressOfStubInCurrentImage(rejectInFrame(false)), addressOfStubInCurrentImage(rejectInFrame(true)) };
    return finishAtHandler(instance, { callFrame, code[frame.liesBelowSpilledArguments()], CompiledHandlerKind::RejectPromise, callFrame }, frame.frameSizeInBytes());
}

bool needsExceptionForEveryThrow(const Instance& instance)
{
    return instance.globalObject->debugger() || Options::breakOnThrow();
}

std::optional<CompiledFrameIterator> findCompiledCallers(VM& vm, const Instance&, CallFrame* stubFrame)
{
    if (stubFrame->callerFrameOrEntryFrame() == vm.topEntryFrame)
        return std::nullopt;
    return CompiledFrameIterator::tryCreateAtCallerOf(vm, stubFrame, vm.topEntryFrame);
}

CompiledUnwindResult unwindCompiledFrames(VM& vm, CompiledFrameIterator& frames, VMEntryRecord& record, bool isTermination)
{
    const Image& image = *vm.aotState().image;
    static const std::span<const int8_t> bufferSlots = RegisterSet::vmCalleeSaveBufferSlotsByRegIndex();
    CallFrame* leftFrame = nullptr;
    ImageFrame leftImageFrame { };
    Instance* instance = nullptr;

    unwindStubFrames(record, bufferSlots, frames);
    while (!frames.isAtEnd()) {
        const auto& site = frames.callSite();
        CallFrame* callFrame = frames.machineFrame();
        instance = frames.instance();

        // The consumer of an async generator never sees what the body throws: the step ends as a rejection.
        if (!isTermination && frames.asyncGeneratorStubFrameLeft()) {
            CallFrame* bodyFrame = frames.asyncGeneratorBodyFrameLeft();
            RELEASE_ASSERT(bodyFrame);
            return finishInRejectingStub(*instance, bodyFrame, bodyFrame == leftFrame ? leftImageFrame : ImageFrame { });
        }

        unsigned frameSize = site.frame.frameSizeInBytes();
        const HandlerRecord* handler = site.exceptionHandler();
        if (!isTermination && handler) {
            void* code = const_cast<uint8_t*>(image.codeOf(site.code.index())) + handler->codeOffset;
            return finishAtHandler(*instance, { callFrame, code, kindOf(handler->type), frames.framePointer() }, frameSize);
        }

        copyFrameSavedRegisters(record, bufferSlots, callFrame, site.frame);

        if (!isTermination && site.rejectsOnThrow)
            return finishInRejectingStub(*instance, callFrame, site.frame);

        leftFrame = callFrame;
        leftImageFrame = site.frame;
        frames.advanceToCallerMachineFrame();
        unwindStubFrames(record, bufferSlots, frames);
    }

    const CompiledFrameIterator::RunEnd& end = frames.runEnd();
    if (end.resumesSavedFrame && leftFrame && !isTermination)
        return finishInRejectingStub(*instance, leftFrame, leftImageFrame);

    if (end.adapterFrame) {
        copyStubSavedRegisters(record, bufferSlots, end.adapterFrame, AdapterFrame::savedRegisters);
        // The return label of the adapter, which clears the word, is not run.
        if (instance)
            instance->coroutines.frameThatMayContinue = nullptr;
    }

    return { frames.exit().crossedEntryFrame ? CompiledUnwindStatus::ReachedEntryFrame : CompiledUnwindStatus::LeftCompiledCode, { } };
}

void unwindBeyondCompiledFrames(VM& vm, CallFrame* frameRecord, CompiledUnwindStatus status)
{
    switch (status) {
    case CompiledUnwindStatus::FoundHandler:
        break;
    case CompiledUnwindStatus::LeftCompiledCode:
        genericUnwind(vm, frameRecord);
        return;
    case CompiledUnwindStatus::ReachedEntryFrame:
        // This entry into the VM has no frame left to look at, so the frames are not walked a second time. These are the targets genericUnwind() sets when it finds no handler.
        vm.callFrameForCatch = frameRecord;
        vm.targetMachinePCForThrow = LLInt::handleUncaughtException(vm).code().taggedPtr();
        vm.targetMachinePCAfterCatch = nullptr;
        vm.targetInterpreterPCForThrow = { static_cast<JSInstruction*>(nullptr) };
        vm.targetInterpreterMetadataPCForThrow = 0;
        vm.targetTryDepthForThrow = 0;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// True spares the Exception its stack trace, which looks at no more frames than the limit. A longer search costs more than it can save, and false is always right.
bool isCaughtByCompiledCode(VM&, CompiledFrameIterator frames)
{
    Instance* instance = nullptr;
    CallFrame* lastFrame = nullptr;
    for (unsigned remainingFrames = Options::exceptionStackTraceLimit(); !frames.isAtEnd(); frames.advanceToCallerMachineFrame(), --remainingFrames) {
        if (!remainingFrames)
            return false;
        if (frames.asyncGeneratorStubFrameLeft())
            return true;
        const auto& site = frames.callSite();
        const HandlerRecord* handler = site.exceptionHandler();
        if (handler && kindOf(handler->type) == CompiledHandlerKind::Catch)
            return true;
        if (site.rejectsOnThrow)
            return true;
        instance = frames.instance();
        lastFrame = frames.machineFrame();
    }
    if (!lastFrame || !frames.runEnd().resumesSavedFrame)
        return false;
    // The body of a generator throws on to whoever resumed it.
    SavedFrame* saved = SavedFrame::forFrame(*instance, std::bit_cast<const uint64_t*>(lastFrame));
    return !saved || saved->kind() == CoroutineKind::AsyncFunction;
}

// Asked before every error of a throw site is created.
bool nearestHandlerIgnoresValue(VM& vm, const Instance&, CallFrame* stubFrame)
{
    const VMState& state = vm.aotState();
    CallSiteCache& cache = *state.callSiteCache;
    void* framePointer = stubFrame->callerFrameOrEntryFrame();
    void* stackPointer = stackPointerOfCaller(stubFrame);
    for (CallFrame* callee = stubFrame; framePointer != vm.topEntryFrame;) {
        const CallSiteCache::Entry* site = cache.resolve(*state.image, removeCodePtrTag(callee->rawReturnPC()));
        if (!site) {
            if (callee != stubFrame)
                checkReturnAddressInRun(*state.image, removeCodePtrTag(callee->rawReturnPC()), callee);
            return false;
        }
        if (site->isStubReturnSite()) {
            if (!letsThrowPass(site->stubFrameKind))
                return false;
        } else {
            if (const HandlerRecord* handler = site->exceptionHandler())
                return handler->ignoresValue();
            if (site->rejectsOnThrow)
                return false;
        }
        callee = frameAtReturnSite(site->frame, stackPointer, static_cast<CallFrame*>(framePointer));
        stackPointer = stackPointerOfCaller(callee, bytesAboveReturnAddressOf(*site));
        if (site->frame.hasFrameRecord())
            framePointer = callee->callerFrameOrEntryFrame();
    }
    return false;
}

CallFrame* findReturnIntoFunctionWithoutHandlers(VM& vm, CallFrame* operationFrame)
{
    const Image& image = *vm.aotState().image;
    if (vm.exceptionForInspection() || !image.stubAddress(handleException(false)))
        return nullptr;
    CallFrame* frame = operationFrame;
    CodeLocation location = image.locate(removeCodePtrTag(frame->rawReturnPC()));
    if (location.kind == CodeKind::Stub) {
        if (location.stubFrameKind != StubFrameKind::Plain)
            return nullptr;
        frame = static_cast<CallFrame*>(frame->callerFrameOrEntryFrame());
        location = image.locate(removeCodePtrTag(frame->rawReturnPC()));
    }
    if (location.kind != CodeKind::Function || catchesExceptions(image, location.function))
        return nullptr;
    return frame;
}

void throwOwedOutOfMemoryErrorAtReturnOfOperation(Instance& instance, CallerFrameAndPC* frameRecordOfOperation)
{
    if (CallFrame* frameThatReturns = findReturnIntoFunctionWithoutHandlers(*instance.vm, std::bit_cast<CallFrame*>(frameRecordOfOperation)))
        throwOutOfMemoryErrorAtReturnOf(instance, frameThatReturns);
}

void throwOutOfMemoryErrorAtReturnOf(Instance& instance, CallFrame* frameThatReturns)
{
    VM& vm = *instance.vm;
    {
        DeferGCForAWhile deferGC(vm);
        auto scope = DECLARE_THROW_SCOPE(vm);
        bool hasThrown = throwOutOfMemoryErrorIfOwed(vm, instance.globalObject, scope);
        RELEASE_ASSERT(hasThrown);
        scope.release();
    }
    instance.runtime.exceptions.stackPointerOfHandler = removeCodePtrTag(frameThatReturns->rawReturnPC());
    frameThatReturns->setReturnPC(reinterpret_cast<char*>(&instance) + InstanceLayout::offsetOfLoopPollPage() + offsetOfDivertedReturnInLoopPollPage);
}

} // namespace JSC::AOT

namespace JSC {

CatchInfo::CatchInfo(const AOT::CompiledHandler& handler)
{
    m_valid = true;
    m_type = handler.kind == AOT::CompiledHandlerKind::Finally ? HandlerType::Finally : HandlerType::Catch;
    m_nativeCode = CodePtr<ExceptionHandlerPtrTag>::fromTaggedPtr(tagCodePtr<ExceptionHandlerPtrTag>(const_cast<void*>(AOT::Image::current()->stubAddress(AOT::catchException()))));
    m_nativeCodeForDispatchAndCatch = CodePtr<ExceptionHandlerPtrTag>::fromTaggedPtr(tagCodePtr<ExceptionHandlerPtrTag>(handler.code));
    m_catchPCForInterpreter = { static_cast<JSInstruction*>(nullptr) };
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
