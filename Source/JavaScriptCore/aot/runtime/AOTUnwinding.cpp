/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnwinding.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTCheckedFrameCells.h"
#include "AOTGenerator.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTRuntimeValidation.h"
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
    case StubFrameKind::ReturnsAfterOperation:
    case StubFrameKind::EngineCallee:
    case StubFrameKind::HostCall:
    case StubFrameKind::GeneratorBody:
    case StubFrameKind::ArgumentList:
    case StubFrameKind::FirstOfTwoReads:
    case StubFrameKind::MethodRead:
    case StubFrameKind::BelowSpilledArguments:
    case StubFrameKind::PlainFunctionConstruct:
    case StubFrameKind::PlainFunctionConstructMakesThis:
    case StubFrameKind::AtStubEntry:
        return true;
    case StubFrameKind::Adapter:
    case StubFrameKind::ConstructAdapter:
    case StubFrameKind::ConstructAdapterMakesThis:
    case StubFrameKind::SavedFrameAdapter:
    case StubFrameKind::HandlerOfJob:
    case StubFrameKind::AsyncGeneratorBody:
    case StubFrameKind::InHostCallFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const int8_t* bufferSlotsByRegisterIndex()
{
    static const int8_t* const slots = RegisterSet::vmCalleeSaveBufferSlotsByRegIndex().data();
    return slots;
}

static ALWAYS_INLINE void copyFrameSavedRegisters(VMEntryRecord& record, const int8_t* bufferSlots, CallFrame* callFrame, const ImageFrame& imageFrame)
{
    const CPURegister* slot = reinterpret_cast<const CPURegister*>(callFrame->registers()) - imageFrame.startOfCalleeSaves();
    for (uint64_t mask = ImageFrame::unpackRegisters(imageFrame.calleeSaveRegisters); mask; mask &= mask - 1, ++slot) {
        int8_t bufferSlot = bufferSlots[WTF::ctz(mask)];
        ASSERT(bufferSlot >= 0);
        record.calleeSaveRegistersBuffer[bufferSlot] = *slot;
    }
}

static void copyStubSavedRegisters(VMEntryRecord& record, const int8_t* bufferSlots, CallFrame* stubFrame, std::span<const AdapterFrame::SavedRegister> savedRegisters)
{
    for (auto saved : savedRegisters) {
        int8_t bufferSlot = bufferSlots[Reg(saved.reg).index()];
        RELEASE_ASSERT(bufferSlot >= 0);
        record.calleeSaveRegistersBuffer[bufferSlot] = *reinterpret_cast<const CPURegister*>(reinterpret_cast<const char*>(stubFrame) + saved.offset);
    }
}

static ALWAYS_INLINE void didLeaveFramesOfCheckedCellsBelow(Instance& instance, const void* address)
{
    if (!CheckedFrameCells::isInUse()) [[likely]]
        return;
    if (CheckedFrameCells* cells = CheckedFrameCells::find(*instance.vm))
        cells->didLeaveFramesBelow(address);
}

static ALWAYS_INLINE void finishAtHandler(CompiledUnwindResult& result, Instance& instance, const CompiledHandler& handler, unsigned frameSize)
{
    char* stackPointer = reinterpret_cast<char*>(handler.callFrame) - frameSize;
    // A parse is one call of the program, so a handler of the frame that opened a source lies behind that parse.
    instance.json.didLeaveFramesUpTo(handler.callFrame);
    didLeaveFramesOfCheckedCellsBelow(instance, stackPointer);
    instance.runtime.exceptions.stackPointerOfHandler = stackPointer;
    result.status = CompiledUnwindStatus::FoundHandler;
    result.handler = handler;
}

// The stub takes the thrown value, hands it to whoever waits for the coroutine and returns from the frame.
static void finishInRejectingStub(CompiledUnwindResult& result, Instance& instance, CallFrame* callFrame, const ImageFrame& frame)
{
    bool liesBelowSpilledArguments = frame.liesBelowSpilledArguments();
    void* code = Image::current()->frequentStub(liesBelowSpilledArguments ? Image::FrequentStub::RejectInFrameBelowSpilledArguments : Image::FrequentStub::RejectInFrame, [&] {
        return addressOfStubInCurrentImage(rejectInFrame(liesBelowSpilledArguments));
    });
    finishAtHandler(result, instance, { callFrame, code, CompiledHandlerKind::RejectPromise, callFrame }, frame.frameSizeInBytes());
    // The stub reads nothing that the function has kept in the frame.
    didLeaveFramesOfCheckedCellsBelow(instance, callFrame);
}

static CompiledFrameExit leaveRunAt(CallFrame* outermostFrame, EntryFrame* entryFrame)
{
    EntryFrame* entryFrameOfRun = entryFrame;
    void* returnPC = outermostFrame->rawReturnPC();
    CallFrame* callerFrame = outermostFrame->callerFrame(entryFrame);
    if (entryFrame != entryFrameOfRun)
        return { callerFrame, nullptr, entryFrame, true, nullptr };
    return { callerFrame, returnPC, entryFrame, false, outermostFrame };
}

static NEVER_INLINE Instance* findInstanceOfRun(VM& vm, const ReturnSiteInRun& site, EntryFrame* entryFrame)
{
    if (Instance* known = CompiledFrameIterator::knownInstanceOfRun(vm.aotState(), entryFrame, site.framePointer))
        return known;
    auto frames = CompiledFrameIterator::tryCreate(vm, site.framePointer, removeReturnAddressSignature(site.rawPC), entryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, site.stackPointer);
    return frames && !frames->isAtEnd() ? frames->instance() : nullptr;
}

// The frame is the outermost of the run, if the walk has left any.
static void finishAtEndOfRun(VM& vm, CompiledUnwindResult& result, Instance* instance, CallFrame* leftFrame, ImageFrame leftImageFrame, VMEntryRecord& record, const int8_t* bufferSlots, bool isTermination)
{
    if (result.runEnd.resumesSavedFrame && leftFrame && !isTermination) {
        finishInRejectingStub(result, *instance, leftFrame, leftImageFrame);
        return;
    }

    if (result.runEnd.adapterFrame) {
        if (VMState& state = vm.aotState(); state.hasSeveralImages) [[unlikely]] {
            state.didLeaveFramesBelow(result.runEnd.adapterFrame);
            if (state.adapterFrameThatRestoresImage == result.runEnd.adapterFrame)
                state.restoreImageBelowAdapter();
        }
        copyStubSavedRegisters(record, bufferSlots, result.runEnd.adapterFrame, AdapterFrame::savedRegisters);
        // The return label of the adapter, which clears the word, is not run.
        if (leftFrame)
            instance->coroutines.frameThatMayContinue = nullptr;
    }
    if (leftFrame) {
        instance->json.didLeaveFramesUpTo(leftFrame);
        didLeaveFramesOfCheckedCellsBelow(*instance, leftFrame);
    }

    result.hasLeftRun = true;
    result.status = result.exit.crossedEntryFrame ? CompiledUnwindStatus::ReachedEntryFrame : CompiledUnwindStatus::LeftCompiledCode;
}

// The adapter made its image the one that runs, and the address that it returns to lies in the code of the image that ran before.
static bool isCalledByCodeOfOtherImage(const VMState& state, CallFrame* adapterFrame, EntryFrame* entryFrame)
{
    if (state.adapterFrameThatRestoresImage != adapterFrame || adapterFrame->callerFrameOrEntryFrame() == entryFrame)
        return false;
    std::span<const uint8_t> code = state.endOfImagesBelowAdapters[-1].parts->image->code();
    return std::bit_cast<uintptr_t>(removeReturnAddressSignature(adapterFrame->rawReturnPC())) - std::bit_cast<uintptr_t>(code.data()) < code.size();
}

// Compiled code keeps its instance in a register that its callees save.
static Instance* instanceOfCompiledCallerOf(CallFrame* adapterFrame)
{
    constexpr AdapterFrame::SavedRegister saved = AdapterFrame::savedRegisters[2];
    static_assert(saved.reg == instanceGPR);
    return *reinterpret_cast<Instance* const*>(reinterpret_cast<const char*>(adapterFrame) + saved.offset);
}

// Goes over the machine frames of one run as CompiledFrameIterator does. It reads the frame records and the call site cache, and leaves every frame in front of the handler.
// A program of one image pays nothing for the other form: what that adds stands under `if constexpr`.
template<bool goesOnInOtherImages>
static ALWAYS_INLINE CompiledUnwindResult unwindRunOrRuns(VM& vm, const ReturnSiteInRun& start, EntryFrame* entryFrame, VMEntryRecord& record, bool isTermination, CallFrame* asyncGeneratorBodyFrameLeft, Instance*& instance)
{
    CompiledUnwindResult result;
    if (static_cast<void*>(start.framePointer) == entryFrame)
        return result;

    VMState& state = vm.aotState();
    const Image* imageOfRun = state.image;
    CallSiteCache* cacheOfRun = state.callSiteCache.get();
    const int8_t* bufferSlots = bufferSlotsByRegisterIndex();
    if constexpr (!goesOnInOtherImages) {
        if (!instance) {
            instance = state.onlyInstance;
            if (!instance) [[unlikely]]
                instance = findInstanceOfRun(vm, start, entryFrame);
        }
    }
    ReturnSiteInRun place = start;
    CallFrame* leftFrame = nullptr;
    ImageFrame leftImageFrame { };
    // Whether anything compiled has called it is not known yet.
    CallFrame* calleeOfRun = start.callee;

    for (;;) {
        const Image& image = *imageOfRun;
        CallSiteCache& cache = *cacheOfRun;
        const void* pc = removeReturnAddressSignature(place.rawPC);
        const CallSiteCache::Entry* site = cache.find(pc);
        if (!site) [[unlikely]]
            site = place.callee == (goesOnInOtherImages ? calleeOfRun : start.callee) ? cache.resolve(image, pc) : resolveReturnAddressInRun(cache, image, pc, place.callee);
        if (!site || (site->isStubReturnSite() && site->stubFrameKind == StubFrameKind::InHostCallFrame)) [[unlikely]] {
            if (place.callee == start.callee)
                return result;
            result.exit = { place.framePointer, place.rawPC, entryFrame, false, place.callee };
            break;
        }

        CallFrame* callFrame = frameAtReturnSite(site->frame, place.stackPointer, place.framePointer);
        if (!site->isStubReturnSite()) [[likely]] {
            if (!isTermination) [[likely]] {
                // The consumer of an async generator never sees what the body throws: the step ends as a rejection.
                if (asyncGeneratorBodyFrameLeft) [[unlikely]] {
                    finishInRejectingStub(result, *instance, asyncGeneratorBodyFrameLeft, asyncGeneratorBodyFrameLeft == leftFrame ? leftImageFrame : ImageFrame { });
                    return result;
                }
                if (const HandlerRecord* handler = site->exceptionHandler()) {
                    void* code = const_cast<uint8_t*>(image.codeOf(site->code.index())) + handler->codeOffset;
                    finishAtHandler(result, *instance, { callFrame, code, kindOf(handler->type), place.framePointer }, site->frame.frameSizeInBytes());
                    result.siteOfHandler = place;
                    return result;
                }
                if (site->rejectsOnThrow) [[unlikely]] {
                    copyFrameSavedRegisters(record, bufferSlots, callFrame, site->frame);
                    finishInRejectingStub(result, *instance, callFrame, site->frame);
                    return result;
                }
            }
            copyFrameSavedRegisters(record, bufferSlots, callFrame, site->frame);
            leftFrame = callFrame;
            leftImageFrame = site->frame;
        } else {
            switch (site->stubFrameKind) {
            case StubFrameKind::InHostCallFrame:
                RELEASE_ASSERT_NOT_REACHED();
            case StubFrameKind::Plain:
            case StubFrameKind::ReturnsAfterOperation:
            case StubFrameKind::ArgumentList:
            case StubFrameKind::HostCall:
            case StubFrameKind::FirstOfTwoReads:
            case StubFrameKind::MethodRead:
            case StubFrameKind::BelowSpilledArguments:
            case StubFrameKind::PlainFunctionConstruct:
            case StubFrameKind::PlainFunctionConstructMakesThis:
            case StubFrameKind::AtStubEntry:
                break;
            case StubFrameKind::GeneratorBody:
                completeGeneratorLeftByThrow(callFrame);
                break;
            case StubFrameKind::AsyncGeneratorBody:
                asyncGeneratorBodyFrameLeft = place.callee;
                break;
            case StubFrameKind::EngineCallee:
                copyStubSavedRegisters(record, bufferSlots, callFrame, EngineCalleeFrame::savedRegisters);
                break;
            case StubFrameKind::Adapter:
            case StubFrameKind::ConstructAdapter:
            case StubFrameKind::ConstructAdapterMakesThis:
                if constexpr (goesOnInOtherImages) {
                    if (!asyncGeneratorBodyFrameLeft && isCalledByCodeOfOtherImage(state, callFrame, entryFrame)) {
                        // What finishAtEndOfRun() does for a run that ends at an adapter.
                        state.restoreImageBelowAdapter();
                        copyStubSavedRegisters(record, bufferSlots, callFrame, AdapterFrame::savedRegisters);
                        if (leftFrame) {
                            instance->coroutines.frameThatMayContinue = nullptr;
                            instance->json.didLeaveFramesUpTo(leftFrame);
                            didLeaveFramesOfCheckedCellsBelow(*instance, leftFrame);
                        }
                        imageOfRun = state.image;
                        cacheOfRun = state.callSiteCache.get();
                        instance = instanceOfCompiledCallerOf(callFrame);
                        RELEASE_ASSERT(instance->program == imageOfRun);
                        leftFrame = nullptr;
                        leftImageFrame = { };
                        place = { callFrame->rawReturnPC(), stackPointerOfCaller(callFrame), callFrame->callerFrame(), callFrame };
                        calleeOfRun = callFrame;
                        continue;
                    }
                }
                [[fallthrough]];
            case StubFrameKind::HandlerOfJob:
            case StubFrameKind::SavedFrameAdapter:
                result.runEnd = { callFrame, isConstructAdapterFrame(site->stubFrameKind), site->stubFrameKind == StubFrameKind::SavedFrameAdapter, site->stubFrameKind == StubFrameKind::HandlerOfJob };
                result.exit = leaveRunAt(callFrame, entryFrame);
                finishAtEndOfRun(vm, result, instance, leftFrame, leftImageFrame, record, bufferSlots, isTermination);
                return result;
            }
        }

        if (site->frame.hasFrameRecord()) {
            if (callFrame->callerFrameOrEntryFrame() == entryFrame) [[unlikely]] {
                result.exit = leaveRunAt(callFrame, entryFrame);
                break;
            }
            place.framePointer = callFrame->callerFrame();
        }
        place.stackPointer = stackPointerOfCaller(callFrame, bytesAboveReturnAddressOf(*site));
        place.rawPC = callFrame->rawReturnPC();
        place.callee = callFrame;
    }
    finishAtEndOfRun(vm, result, instance, leftFrame, leftImageFrame, record, bufferSlots, isTermination);
    return result;
}

CompiledUnwindResult unwindRun(VM& vm, const ReturnSiteInRun& start, EntryFrame* entryFrame, VMEntryRecord& record, bool isTermination, CallFrame* asyncGeneratorBodyFrameLeft)
{
    Instance* instance = nullptr;
    return unwindRunOrRuns<false>(vm, start, entryFrame, record, isTermination, asyncGeneratorBodyFrameLeft, instance);
}

CompiledUnwindResult unwindRunOfThrow(VM& vm, Instance& instanceThatThrows, const ReturnSiteInRun& start, EntryFrame* entryFrame, VMEntryRecord& record)
{
    Instance* instance = &instanceThatThrows;
    return unwindRunOrRuns<false>(vm, start, entryFrame, record, false, nullptr, instance);
}

CompiledUnwindResult unwindRunsOfImages(VM& vm, const ReturnSiteInRun& start, EntryFrame* entryFrame, VMEntryRecord& record, Instance*& instance)
{
    return unwindRunOrRuns<true>(vm, start, entryFrame, record, false, nullptr, instance);
}

// The iterator has passed the frames of stubs in front of the frame that it is at.
CompiledUnwindResult unwindCompiledFrames(VM& vm, const CompiledFrameIterator& frames, VMEntryRecord& record, bool isTermination)
{
    const int8_t* bufferSlots = bufferSlotsByRegisterIndex();
    if (CallFrame* stubFrame = frames.generatorStubFrameLeft())
        completeGeneratorLeftByThrow(stubFrame);
    if (CallFrame* stubFrame = frames.engineCalleeStubFrameLeft())
        copyStubSavedRegisters(record, bufferSlots, stubFrame, EngineCalleeFrame::savedRegisters);
    if (frames.isAtEnd()) {
        CompiledUnwindResult result;
        result.runEnd = frames.runEnd();
        result.exit = frames.exit();
        finishAtEndOfRun(vm, result, nullptr, nullptr, { }, record, bufferSlots, isTermination);
        return result;
    }
    CallFrame* asyncGeneratorBodyFrameLeft = frames.asyncGeneratorStubFrameLeft() ? frames.asyncGeneratorBodyFrameLeft() : nullptr;
    RELEASE_ASSERT(!frames.asyncGeneratorStubFrameLeft() || asyncGeneratorBodyFrameLeft);
    return unwindRun(vm, { const_cast<void*>(frames.callSite().address), frames.stackPointer(), frames.framePointer(), nullptr }, frames.entryFrame(), record, isTermination, asyncGeneratorBodyFrameLeft);
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
        const void* pc = removeReturnAddressSignature(callee->rawReturnPC());
        const CallSiteCache::Entry* site = cache.find(pc);
        if (!site) [[unlikely]]
            site = cache.resolve(*state.image, pc);
        if (!site) {
            if (callee != first)
                checkReturnAddressInRun(*state.image, removeReturnAddressSignature(callee->rawReturnPC()), callee);
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

void JIT_OPERATION_ATTRIBUTES checkThatThrowingCallHasSite(Instance* instance, CallFrame* stubFrame)
{
    VM& vm = *instance->vm;
    Exception* exception = vm.exceptionForInspection();
    if (exception && vm.isTerminationException(exception))
        return;
    auto* error = exception ? dynamicDowncast<ErrorInstance>(exception->value()) : nullptr;
    bool isForLackOfMemory = error && error->isOutOfMemoryError();
    const VMState& state = vm.aotState();
    CallFrame* callee = stubFrame;
    void* stackPointer = stackPointerOfCaller(stubFrame);
    void* framePointer = stubFrame->callerFrameOrEntryFrame();
    while (framePointer != vm.topEntryFrame) {
        const void* pc = removeReturnAddressSignature(callee->rawReturnPC());
        const CallSiteCache::Entry* site = state.callSiteCache->resolve(*state.image, pc);
        if (!site)
            return;
        if (!site->isStubReturnSite()) {
            // throwOwedOutOfMemoryErrorAtReturnOfOperation().
            if (isForLackOfMemory)
                return;
            if (std::optional<MarkOfCallWithoutSite> mark = MarkOfCallWithoutSite::read(pc))
                crashOnValidationFailure(vm, { ValidationFailureKind::CallThrew, state.image->rootFunctionOf(site->code.index()), mark->value, jsNumber(mark->callee) });
            return;
        }
        if (!letsThrowPass(site->stubFrameKind))
            return;
        callee = frameAtReturnSite(site->frame, stackPointer, static_cast<CallFrame*>(framePointer));
        stackPointer = stackPointerOfCaller(callee, bytesAboveReturnAddressOf(*site));
        if (site->frame.hasFrameRecord())
            framePointer = callee->callerFrameOrEntryFrame();
    }
}

// Asked before every error of a throw site is created.
bool nearestHandlerIgnoresValue(VM& vm, const Instance&, CallFrame* stubFrame)
{
    return findNearestHandler(vm, stubFrame) == HandlerType::SynthesizedCatch;
}

bool isCaughtAboveCleanup(VM& vm, const ReturnSiteInRun& siteOfCleanup)
{
    if (findHandlerInRun<HandlerSearch::NearestThatEndsThrow>(vm, siteOfCleanup.callee, siteOfCleanup.stackPointer, siteOfCleanup.framePointer, vm.topEntryFrame))
        return true;
    auto frames = CompiledFrameIterator::tryCreate(vm, siteOfCleanup.framePointer, removeReturnAddressSignature(siteOfCleanup.rawPC), vm.topEntryFrame, CompiledFrameIterator::OperationState::MayNotHaveCalled, siteOfCleanup.stackPointer);
    return frames && findCatchInRun(*frames);
}

bool beginsInTopCompiledFrame(VM& vm, const ReturnAddressTrace& trace)
{
    if (!vm.topCallFrame || vm.topCallFrame->callerFrameOrEntryFrame() == vm.topEntryFrame)
        return true;
    std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, vm.topCallFrame, vm.topEntryFrame);
    return !frames || frames->isAtEnd() || beginsAt(trace, frames->callSite().address);
}

const void* returnAddressOfInnermostCompiledCall(VM& vm)
{
    EntryFrame* entryFrame = vm.topEntryFrame;
    for (CallFrame* frame = vm.topCallFrame; frame && entryFrame; frame = frame->callerFrame(entryFrame)) {
        if (frame->callerFrameOrEntryFrame() == entryFrame)
            continue;
        std::optional<CompiledFrameIterator> frames = CompiledFrameIterator::tryCreateAtCallerOf(vm, frame, entryFrame);
        if (frames && !frames->isAtEnd())
            return frames->callSite().address;
    }
    return nullptr;
}

static bool isAddressInCompiledCode(uint64_t word)
{
    std::span<const uint8_t> code = Image::current()->code();
    return word - std::bit_cast<uintptr_t>(code.data()) < code.size();
}

// Native code has called the run that the throw leaves, and hands the exception on to its own caller unless it reports it.
static bool isCaughtByCallersOfNativeCode(VM& vm, CompiledFrameExit exit)
{
    // Code of another image may have called the run, with no native code between. The answer only saves a walk.
    if (vm.aotState().hasSeveralImages) [[unlikely]]
        return false;
    for (unsigned remainingRuns = Options::exceptionStackTraceLimit(); remainingRuns && exit.callFrame; --remainingRuns) {
        if (exit.callFrame->callerFrameOrEntryFrame() == exit.entryFrame)
            return false;
        // So may code of another realm that runs the image.
        if (exit.returnPC && isAddressInCompiledCode(std::bit_cast<uintptr_t>(removeReturnAddressSignature(exit.returnPC))))
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

Exception* exceptionKeptByCleanup(uint64_t kept, JSValue thrown)
{
    if (isAddressInCompiledCode(kept))
        return nullptr;
    auto* exception = std::bit_cast<Exception*>(kept);
    return exception->value() == thrown ? exception : nullptr;
}

NEVER_INLINE Exception::StackCaptureAction stackCaptureActionOfThrow(VM& vm, const Instance& instance, JSValue thrown, const CallerFrameAndPC* frameRecordOfOperation, const CompiledUnwindResult* runWithoutHandler)
{
    using Action = Exception::StackCaptureAction;
    if (removeReturnAddressSignature(frameRecordOfOperation->callerFrame->rawReturnPC()) == instance.state->returnAddressOfThrowAfterCleanup) {
        uint64_t kept = instance.state->keptByCleanup;
        // The program threw the value somewhere else, and an Error keeps the stack that it holds.
        if (!isAddressInCompiledCode(kept))
            return Action::CaptureStack;
        // The `throw` stands in this frame, and nothing returns to the end of the cleanup.
        frameRecordOfOperation->callerFrame->setReturnPC(std::bit_cast<void*>(kept));
    }
    if (!runWithoutHandler || needsExceptionForEveryThrow(instance))
        return Action::CaptureStackOfThrow;
    // The promise of the handler is rejected with the value.
    if (runWithoutHandler->runEnd.hasCalledHandlerOfJob && instance.coroutines.promiseOfEnteredHandler)
        return Action::DoNotCaptureStack;
    // For what holds no stack of its own the frames are walked either way.
    auto* error = dynamicDowncast<ErrorInstance>(thrown);
    if (!error || !error->returnAddresses())
        return Action::CaptureStackOfThrow;
    return isCaughtByCallersOfNativeCode(vm, runWithoutHandler->exit) ? Action::CaptureStack : Action::CaptureStackOfThrow;
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
