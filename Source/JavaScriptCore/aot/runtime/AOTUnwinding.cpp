/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnwinding.h"

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubFrames.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "HandlerInfo.h"
#include "Interpreter.h"
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
    case HandlerType::SynthesizedFinally:
        return CompiledHandlerKind::Finally;
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
    const CPURegister* slot = reinterpret_cast<const CPURegister*>(callFrame->registers()) - imageFrame.calleeSavesStart;
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
    instance.json.didUnwindTo(reinterpret_cast<char*>(handler.callFrame) - frameSize);
    return { CompiledUnwindStatus::FoundHandler, handler };
}

bool needsExceptionForEveryThrow(const Instance& instance)
{
    return instance.globalObject->debugger() || Options::breakOnThrow();
}

std::optional<CompiledFrameIterator> findCompiledCallers(VM& vm, const Instance& instance, CallFrame* stubFrame)
{
    if (instance.runtime.exceptions.callSiteOverrides || stubFrame->callerFrameOrEntryFrame() == vm.topEntryFrame)
        return std::nullopt;
    return CompiledFrameIterator::tryCreate(vm, stubFrame->callerFrame(), removeCodePtrTag(stubFrame->rawReturnPC()), vm.topEntryFrame);
}

CompiledUnwindResult unwindCompiledFrames(VM& vm, CompiledFrameIterator& frames, VMEntryRecord& record, bool isTermination)
{
    const Image& image = *vm.aotState().image;
    void* rejectInFrameAddress = const_cast<void*>(image.stubAddress(rejectInFrame()));
    auto bufferSlots = RegisterSet::vmCalleeSaveBufferSlotsByRegIndex();
    CallFrame* leftFrame = nullptr;
    unsigned leftFrameSize = 0;
    Instance* instance = nullptr;

    unwindStubFrames(record, bufferSlots, frames);
    while (!frames.isAtEnd()) {
        const auto& site = frames.callSite();
        const CompiledFrame& frame = frames.frame();
        instance = frame.instance;
        unsigned frameSize = site.frame.frameSizeInBytes();

        if (!isTermination && site.handler) {
            void* code = const_cast<uint8_t*>(image.codeOf(frame.code.index())) + site.handler->codeOffset;
            return finishAtHandler(*instance, { frame.callFrame, code, kindOf(site.handler->type) }, frameSize);
        }

        copyFrameSavedRegisters(record, bufferSlots, frame.callFrame, site.frame);

        if (!isTermination && site.rejectsOnThrow)
            return finishAtHandler(*instance, { frame.callFrame, rejectInFrameAddress, CompiledHandlerKind::RejectPromise }, frameSize);

        leftFrame = frame.callFrame;
        leftFrameSize = frameSize;
        frames.advanceToCallerMachineFrame();
        unwindStubFrames(record, bufferSlots, frames);
    }

    const CompiledFrameIterator::RunEnd& end = frames.runEnd();
    if (end.resumesSavedFrame && leftFrame && !isTermination)
        return finishAtHandler(*instance, { leftFrame, rejectInFrameAddress, CompiledHandlerKind::RejectPromise }, leftFrameSize);

    if (end.adapterFrame)
        copyStubSavedRegisters(record, bufferSlots, end.adapterFrame, AdapterFrame::savedRegisters);

    return { frames.exit().crossedEntryFrame ? CompiledUnwindStatus::ReachedEntryFrame : CompiledUnwindStatus::LeftCompiledCode, { } };
}

bool isCaughtByCompiledCode(VM&, CompiledFrameIterator frames)
{
    Instance* instance = nullptr;
    CallFrame* lastFrame = nullptr;
    for (; !frames.isAtEnd(); frames.advanceToCallerMachineFrame()) {
        const auto& site = frames.callSite();
        if (site.handler && kindOf(site.handler->type) == CompiledHandlerKind::Catch)
            return true;
        if (site.rejectsOnThrow)
            return true;
        instance = frames.frame().instance;
        lastFrame = frames.frame().callFrame;
    }
    if (!lastFrame || !frames.runEnd().resumesSavedFrame)
        return false;
    // The body of a generator throws on to whoever resumed it.
    SavedFrame* saved = SavedFrame::forFrame(*instance, std::bit_cast<const uint64_t*>(lastFrame));
    return !saved || saved->kind() == CoroutineKind::AsyncFunction;
}

// Asked before every error of a throw site is created, so it reads nothing but the call site cache. It looks at the frames in the order of unwindCompiledFrames, which stops at the same one.
// False is always right: that is the answer at a return address the cache does not hold, which may lie in a stub or outside compiled code.
bool nearestHandlerIgnoresValue(VM& vm, const Instance& instance, CallFrame* stubFrame)
{
    if (instance.runtime.exceptions.callSiteOverrides)
        return false;
    const CallSiteCache& cache = *vm.aotState().callSiteCache;
    for (CallFrame* callee = stubFrame; callee->callerFrameOrEntryFrame() != vm.topEntryFrame; callee = callee->callerFrame()) {
        const CallSiteCache::Entry* site = cache.find(removeCodePtrTag(callee->rawReturnPC()));
        if (!site)
            return false;
        if (site->handler)
            return site->handler->type == HandlerType::SynthesizedCatch;
        if (site->rejectsOnThrow)
            return false;
    }
    return false;
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
