/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnwinding.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTGenerator.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTSavedFrame.h"
#include "AOTStackTrace.h"
#include "AOTState.h"
#include "AOTStubEntrypoints.h"
#include "AOTStubFrames.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "DeferGCInlines.h"
#include "ErrorInstance.h"
#include "HandlerInfo.h"
#include "Interpreter.h"
#include "JITExceptions.h"
#include "LLIntExceptions.h"
#include "OwedOutOfMemoryError.h"
#include "Reg.h"
#include "RegisterSet.h"
#include "TopExceptionScope.h"
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
    bool liesBelowSpilledArguments = frame.liesBelowSpilledArguments();
    void* code = Image::current()->frequentStub(liesBelowSpilledArguments ? Image::FrequentStub::RejectInFrameBelowSpilledArguments : Image::FrequentStub::RejectInFrame, [&] {
        return addressOfStubInCurrentImage(rejectInFrame(liesBelowSpilledArguments));
    });
    return finishAtHandler(instance, { callFrame, code, CompiledHandlerKind::RejectPromise, callFrame }, frame.frameSizeInBytes());
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
static bool findCatchInRun(CompiledFrameIterator& frames)
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

bool isCaughtByCompiledCode(VM&, CompiledFrameIterator frames)
{
    return findCatchInRun(frames);
}

enum class HandlerSearch : bool { Nearest, NearestThatEndsThrow };

// Looks at the callers of `callee` in its run of compiled frames. A frame that turns a throw into a rejection counts as a `catch`. Nothing is no answer: the run may go on in ways that this walk does not follow.
template<HandlerSearch search>
static ALWAYS_INLINE std::optional<HandlerType> findHandlerInRun(VM& vm, CallFrame* callee, void* stackPointer, void* framePointer, EntryFrame* entryFrame)
{
    const VMState& state = vm.aotState();
    CallSiteCache& cache = *state.callSiteCache;
    CallFrame* first = callee;
    unsigned remainingFrames = search == HandlerSearch::NearestThatEndsThrow ? Options::exceptionStackTraceLimit() : 0;
    while (framePointer != entryFrame) {
        const CallSiteCache::Entry* site = cache.resolve(*state.image, removeCodePtrTag(callee->rawReturnPC()));
        if (!site) {
            if (callee != first)
                checkReturnAddressInRun(*state.image, removeCodePtrTag(callee->rawReturnPC()), callee);
            return std::nullopt;
        }
        if (site->isStubReturnSite()) {
            if (!letsThrowPass(site->stubFrameKind))
                return std::nullopt;
        } else {
            const HandlerRecord* handler = site->exceptionHandler();
            if (handler && (search == HandlerSearch::Nearest || handler->type != HandlerType::Finally))
                return handler->type;
            if (site->rejectsOnThrow)
                return HandlerType::Catch;
            if (search == HandlerSearch::NearestThatEndsThrow && !--remainingFrames)
                return std::nullopt;
        }
        callee = frameAtReturnSite(site->frame, stackPointer, static_cast<CallFrame*>(framePointer));
        stackPointer = stackPointerOfCaller(callee, bytesAboveReturnAddressOf(*site));
        if (site->frame.hasFrameRecord())
            framePointer = callee->callerFrameOrEntryFrame();
    }
    return std::nullopt;
}

std::optional<HandlerType> findNearestHandler(VM& vm, CallFrame* stubFrame)
{
    return findHandlerInRun<HandlerSearch::Nearest>(vm, stubFrame, stackPointerOfCaller(stubFrame), stubFrame->callerFrameOrEntryFrame(), vm.topEntryFrame);
}

// Asked before every error of a throw site is created.
bool nearestHandlerIgnoresValue(VM& vm, const Instance&, CallFrame* stubFrame)
{
    return findNearestHandler(vm, stubFrame) == HandlerType::SynthesizedCatch;
}

bool isCaughtAboveCleanup(VM& vm, const CompiledFrameIterator& framesAtCleanup)
{
    CallFrame* frame = framesAtCleanup.machineFrame();
    void* framePointer = framesAtCleanup.callSite().frame.hasFrameRecord() ? frame->callerFrameOrEntryFrame() : framesAtCleanup.framePointer();
    if (findHandlerInRun<HandlerSearch::NearestThatEndsThrow>(vm, frame, framesAtCleanup.callerStackPointer(), framePointer, framesAtCleanup.entryFrame()))
        return true;
    return isCaughtByCompiledCode(vm, framesAtCleanup);
}

bool beginsInTopCompiledFrame(VM& vm, const ReturnAddressTrace& trace)
{
    if (!vm.topCallFrame || vm.topCallFrame->callerFrameOrEntryFrame() == vm.topEntryFrame)
        return true;
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
    return !frames || frames->isAtEnd() || beginsAt(trace, frames->callSite().address);
}

// Native code has called the run that the throw leaves, and hands the exception on to its own caller unless it reports it.
static bool isCaughtByCallersOfNativeCode(VM& vm, CompiledFrameExit exit)
{
    for (unsigned remainingRuns = Options::exceptionStackTraceLimit(); remainingRuns && exit.callFrame; --remainingRuns) {
        if (exit.callFrame->callerFrameOrEntryFrame() == exit.entryFrame)
            return false;
        if (void* taker = vm.aotState().stackPointerOfExceptionTaker; taker && taker < static_cast<void*>(exit.callFrame))
            return false;
        if (findHandlerInRun<HandlerSearch::NearestThatEndsThrow>(vm, exit.callFrame, stackPointerOfCaller(exit.callFrame), exit.callFrame->callerFrameOrEntryFrame(), exit.entryFrame))
            return true;
        std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, exit.callFrame, exit.entryFrame);
        if (!frames || frames->isAtEnd())
            return false;
        if (findCatchInRun(*frames))
            return true;
        if (!frames->isAtEnd())
            return false;
        exit = frames->exit();
    }
    return false;
}

static bool isAddressInCompiledCode(uint64_t word)
{
    std::span<const uint8_t> code = Image::current()->code();
    return word - std::bit_cast<uintptr_t>(code.data()) < code.size();
}

Exception* exceptionKeptByCleanup(uint64_t kept, JSValue thrown)
{
    if (isAddressInCompiledCode(kept))
        return nullptr;
    auto* exception = std::bit_cast<Exception*>(kept);
    return exception->value() == thrown ? exception : nullptr;
}

NEVER_INLINE Exception::StackCaptureAction stackCaptureActionOfThrow(VM& vm, const Instance& instance, JSValue thrown, const CallerFrameAndPC* frameRecordOfOperation, const CompiledFrameIterator* framesAtEndOfRun)
{
    using Action = Exception::StackCaptureAction;
    if (removeCodePtrTag(frameRecordOfOperation->callerFrame->rawReturnPC()) == instance.state->returnAddressOfThrowAfterCleanup) {
        uint64_t kept = instance.state->keptByCleanup;
        // The program threw the value somewhere else, and an Error keeps the stack that it holds.
        if (!isAddressInCompiledCode(kept))
            return Action::CaptureStack;
        // The `throw` stands in this frame, and nothing returns to the end of the cleanup.
        frameRecordOfOperation->callerFrame->setReturnPC(std::bit_cast<void*>(kept));
    }
    if (!framesAtEndOfRun || needsExceptionForEveryThrow(instance))
        return Action::CaptureStackOfThrow;
    // The promise of the handler is rejected with the value.
    if (framesAtEndOfRun->runEnd().hasCalledHandlerOfJob && instance.coroutines.promiseOfEnteredHandler)
        return Action::DoNotCaptureStack;
    // For what holds no stack of its own the frames are walked either way.
    auto* error = dynamicDowncast<ErrorInstance>(thrown);
    if (!error || !error->returnAddresses())
        return Action::CaptureStackOfThrow;
    return isCaughtByCallersOfNativeCode(vm, framesAtEndOfRun->exit()) ? Action::CaptureStack : Action::CaptureStackOfThrow;
}

// A caller that has handlers, and none at its call, plans no error there: the call may stand in a try. Then the error waits for the next poll.
static bool callersPlanErrors(VM& vm, const Image& image, CallFrame* frameThatReturns)
{
    if (frameThatReturns->callerFrameOrEntryFrame() == vm.topEntryFrame)
        return true;
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, frameThatReturns, vm.topEntryFrame);
    if (!frames)
        return true;
    for (; !frames->isAtEnd(); frames->advanceToCallerMachineFrame()) {
        if (frames->asyncGeneratorStubFrameLeft())
            return true;
        const auto& site = frames->callSite();
        if (site.exceptionHandler() || site.rejectsOnThrow)
            return true;
        if (catchesExceptions(image, site.code.index()))
            return false;
    }
    return true;
}

CallFrame* findReturnIntoFunctionWithoutHandlers(VM& vm, CallFrame* operationFrame)
{
    const Image& image = *vm.aotState().image;
    if (vm.exceptionForInspection() || vm.heap.cage().errorWaitsForPoll() || !image.stubAddress(handleException(false)))
        return nullptr;
    CallFrame* frame = operationFrame;
    CodeLocation location = image.locate(removeCodePtrTag(frame->rawReturnPC()));
    if (location.kind == CodeKind::Stub) {
        if (location.stubFrameKind != StubFrameKind::Plain)
            return nullptr;
        frame = static_cast<CallFrame*>(frame->callerFrameOrEntryFrame());
        location = image.locate(removeCodePtrTag(frame->rawReturnPC()));
    }
    if (location.kind != CodeKind::Function || catchesExceptions(image, location.function) || !callersPlanErrors(vm, image, frame))
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
        auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        {
            auto scope = DECLARE_THROW_SCOPE(vm);
            bool hasThrown = throwOutOfMemoryErrorIfOwed(vm, instance.globalObject, scope);
            RELEASE_ASSERT(hasThrown);
            scope.release();
        }
        EXCEPTION_ASSERT_UNUSED(topScope, topScope.exception());
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
