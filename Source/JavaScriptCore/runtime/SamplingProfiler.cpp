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
#include "SamplingProfiler.h"

#if ENABLE(SAMPLING_PROFILER)

#include "AOTRemovableFeature.h"
#include "CodeBlock.h"
#include "CodeBlockSet.h"
#include "HeapIterationScope.h"
#include "HeapUtil.h"
#include "InlineCallFrame.h"
#include "JSCInlines.h"
#include "LLIntPCRanges.h"
#include "MachineContext.h"
#include "MarkedBlockInlines.h"
#include "MarkedBlockSet.h"
#include "NativeCallee.h"
#include "NativeCalleeRegistry.h"
#include "NativeExecutable.h"
#include "TopExceptionScope.h"
#include "VM.h"
#include "VMTrapsInlines.h"
#include "WasmCallee.h"
#include "WasmCapabilities.h"
#include "WasmCodeInImage.h"
#include <wtf/Condition.h>
#include <wtf/FilePrintStream.h>
#include <wtf/HashSet.h>
#include <wtf/JSONValues.h>
#include <wtf/RefPtr.h>
#include <wtf/StackTrace.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>

#if OS(WINDOWS)
#include <wtf/win/Win32Handle.h>
#endif

#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTFunctionDescription.h"
#include "AOTImage.h"
#include "AOTSavedFrame.h"
#include "AOTStackTrace.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

static double sNumTotalStackTraces = 0;
static double sNumTotalWalks = 0;
static double sNumFailedWalks = 0;
static const uint32_t sNumWalkReportingFrequency = 50;
static const double sWalkErrorPercentage = .05;
static constexpr bool sReportStatsOnlyWhenTheyreAboveThreshold = false;
static constexpr bool sReportStats = false;

using FrameType = SamplingProfiler::FrameType;
using UnprocessedStackFrame = SamplingProfiler::UnprocessedStackFrame;

WTF_MAKE_TZONE_ALLOCATED_IMPL(SamplingProfiler);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(SamplingProfiler::SharedStackTrace);

ALWAYS_INLINE static void reportStats()
{
    if (sReportStats && sNumTotalWalks && static_cast<uint64_t>(sNumTotalWalks) % sNumWalkReportingFrequency == 0) {
        if (!sReportStatsOnlyWhenTheyreAboveThreshold || (sNumFailedWalks / sNumTotalWalks > sWalkErrorPercentage)) {
            dataLogF("Num total walks: %llu. Failed walks percent: %lf\n",
                static_cast<unsigned long long>(sNumTotalWalks), sNumFailedWalks / sNumTotalWalks);
        }
    }
}

class FrameWalker {
public:
    FrameWalker(VM& vm, CallFrame* callFrame, EntryFrame* entryFrame, const AbstractLocker& codeBlockSetLocker, const AbstractLocker& machineThreadsLocker)
        : m_vm(vm)
        , m_callFrame(callFrame)
        , m_entryFrame(entryFrame)
        , m_codeBlockSetLocker(codeBlockSetLocker)
        , m_machineThreadsLocker(machineThreadsLocker)
    {
    }

    SUPPRESS_ASAN
    size_t walk(std::span<UnprocessedStackFrame> stackTrace, bool& didRunOutOfSpace)
    {
        if (sReportStats)
            sNumTotalWalks++;
        resetAtMachineFrame();
        size_t maxStackTraceSize = stackTrace.size();
        while (!isAtTop() && !m_bailingOut && m_depth < maxStackTraceSize) {
            recordJITFrame(stackTrace);
            advanceToParentFrame();
            resetAtMachineFrame();
        }
        didRunOutOfSpace = m_depth >= maxStackTraceSize && !isAtTop();
        reportStats();
        return m_depth;
    }

    NODELETE bool wasValidWalk() const
    {
        return !m_bailingOut;
    }

protected:

    SUPPRESS_ASAN
    void recordJITFrame(std::span<UnprocessedStackFrame> stackTrace)
    {
        CallSiteIndex callSiteIndex;
        CalleeBits unsafeCallee = m_callFrame->unsafeCallee();
        CodeBlock* codeBlock = m_callFrame->unsafeCodeBlock();
        if (unsafeCallee.isNativeCallee())
            codeBlock = nullptr;
        if (codeBlock) {
            ASSERT(isValidCodeBlock(codeBlock));
            callSiteIndex = m_callFrame->unsafeCallSiteIndex();
        }
        stackTrace[m_depth] = UnprocessedStackFrame(codeBlock, unsafeCallee, callSiteIndex);
        if (unsafeCallee.isNativeCallee()) {
            assertIsHeld(NativeCalleeRegistry::singleton().getLock());
            auto* nativeCallee = unsafeCallee.asNativeCallee();
            if (NativeCalleeRegistry::singleton().isValidCallee(nativeCallee)) {
                stackTrace[m_depth].nativeCalleeCategory = nativeCallee->category();
                switch (nativeCallee->category()) {
                case NativeCallee::Category::Wasm: {
#if ENABLE(WEBASSEMBLY)
                    // At this point, Wasm::Callee would be dying (ref count is 0), but its fields are still live.
                    // And we can safely copy Wasm::IndexOrName even when any lock is held by suspended threads.
                    auto* wasmCallee = uncheckedDowncast<Wasm::Callee>(nativeCallee);
                    stackTrace[m_depth].wasmCompilationMode = wasmCallee->compilationMode();
                    stackTrace[m_depth].wasmIndexOrName = wasmCallee->indexOrName();
                    stackTrace[m_depth].callSiteIndex = m_callFrame->unsafeCallSiteIndex();
#if ENABLE(WEBASSEMBLY_OMGJIT)
                    // FIXME: We should be able to add all stack traces including inlined ones in SamplingProfiler.
                    if (wasmCallee->compilationMode() == Wasm::CompilationMode::OMGMode) {
                        auto* omgCallee = uncheckedDowncast<const Wasm::OptimizingJITCallee>(wasmCallee);
                        bool isInlined = false;
                        auto origin = omgCallee->getOrigin(stackTrace[m_depth].callSiteIndex.bits(), 0, isInlined);
                        if (isInlined)
                            stackTrace[m_depth].wasmIndexOrName = origin;
                    }
                    stackTrace[m_depth].wasmPCMap = wasmCallee->pcToCodeOriginMap();
#endif
#endif
                    break;
                }
                case NativeCallee::Category::InlineCache: {
                    break;
                }
                }
            }
        }
        m_depth++;
    }

    SUPPRESS_ASAN
    void advanceToParentFrame()
    {
        m_callFrame = m_callFrame->unsafeCallerFrame(m_entryFrame);
    }

    NODELETE bool isAtTop() const
    {
        return !m_callFrame;
    }

    SUPPRESS_ASAN
    void resetAtMachineFrame()
    {
        if (isAtTop())
            return;

        if (!isValidFramePointer(m_callFrame)) {
            // Guard against pausing the process at weird program points.
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
            return;
        }

        CodeBlock* codeBlock = m_callFrame->unsafeCodeBlock();
        if (!codeBlock || m_callFrame->unsafeCallee().isNativeCallee())
            return;

        if (!isValidCodeBlock(codeBlock)) {
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
            return;
        }
    }

    NODELETE bool isValidFramePointer(void* callFrame)
    {
        uint8_t* fpCast = std::bit_cast<uint8_t*>(callFrame);
        for (auto& thread : m_vm.heap.machineThreads().threads(m_machineThreadsLocker)) {
            uint8_t* stackBase = static_cast<uint8_t*>(thread->stack().origin());
            uint8_t* stackLimit = static_cast<uint8_t*>(thread->stack().end());
            RELEASE_ASSERT(stackBase);
            RELEASE_ASSERT(stackLimit);
            RELEASE_ASSERT(stackLimit <= stackBase);
            if (fpCast < stackBase && fpCast >= stackLimit)
                return true;
        }
        return false;
    }

    bool isValidCodeBlock(CodeBlock* codeBlock)
    {
        if (!codeBlock)
            return false;
        bool result = m_vm.heap.codeBlockSet().contains(m_codeBlockSetLocker, codeBlock);
        return result;
    }

    VM& m_vm;
    CallFrame* m_callFrame;
    EntryFrame* m_entryFrame;
    const AbstractLocker& m_codeBlockSetLocker;
    const AbstractLocker& m_machineThreadsLocker;
    bool m_bailingOut { false };
    size_t m_depth { 0 };
};

class CFrameWalker : public FrameWalker {
public:
    typedef FrameWalker Base;

    CFrameWalker(VM& vm, void* machineFrame, CallFrame* callFrame, EntryFrame* entryFrame, const AbstractLocker& codeBlockSetLocker, const AbstractLocker& machineThreadsLocker)
        : Base(vm, callFrame, entryFrame, codeBlockSetLocker, machineThreadsLocker)
        , m_machineFrame(machineFrame)
    {
    }

    size_t walk(std::span<UnprocessedStackFrame> stackTrace, bool& didRunOutOfSpace)
    {
        if (sReportStats)
            sNumTotalWalks++;
        resetAtMachineFrame();
        size_t maxStackTraceSize = stackTrace.size();
        // The way the C walker decides if a frame it is about to trace is C or JS is by
        // ensuring m_callFrame points to some frame above the machineFrame.
        if (!isAtTop() && !m_bailingOut && m_machineFrame == m_callFrame && maxStackTraceSize) {
            recordJITFrame(stackTrace);
            Base::advanceToParentFrame();
            resetAtMachineFrame();
        }

        while (!isAtTop() && !m_bailingOut && m_depth < maxStackTraceSize) {
            if (m_machineFrame >= m_callFrame) {
                // If we get to this state we probably have an invalid trace.
                m_bailingOut = true;
                break;
            }

            if (isCFrame()) {
                RELEASE_ASSERT(!LLInt::isLLIntPC(frame()->callerFrame));
                stackTrace[m_depth] = UnprocessedStackFrame(removeReturnAddressSignature(frame()->returnPC));
                m_depth++;
            } else
                recordJITFrame(stackTrace);
            advanceToParentFrame();
            resetAtMachineFrame();
        }
        didRunOutOfSpace = m_depth >= maxStackTraceSize && !isAtTop();
        reportStats();
        return m_depth;
    }

private:

    NODELETE bool isCFrame()
    {
        return frame()->callerFrame != m_callFrame;
    }

    void advanceToParentFrame()
    {
        if (!isCFrame())
            Base::advanceToParentFrame();
        m_machineFrame = frame()->callerFrame;
    }

    void resetAtMachineFrame()
    {
        if (!isValidFramePointer(m_machineFrame)) {
            // Guard against pausing the process at weird program points.
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
            return;
        }
        Base::resetAtMachineFrame();
    }

    NODELETE CallerFrameAndPC* frame()
    {
        return reinterpret_cast<CallerFrameAndPC*>(m_machineFrame);
    }

    void* m_machineFrame;
};

#if ENABLE(AOT)
class FrameWalkerWithCompiledFrames : public FrameWalker {
public:
    typedef FrameWalker Base;

    FrameWalkerWithCompiledFrames(VM& vm, const AOT::Image& image, const AOT::SampledTop& top, EntryFrame* entryFrame, const StackBounds& stackBounds, const AbstractLocker& codeBlockSetLocker, const AbstractLocker& machineThreadsLocker)
        : Base(vm, top.callFrame, entryFrame, codeBlockSetLocker, machineThreadsLocker)
        , m_image(image)
        , m_stack { stackBounds }
        , m_pc(top.pc)
        , m_framelessTopCallerPC(top.framelessTopCallerPC)
        , m_unrecordedFramePointer(top.framePointer)
    {
    }

    SUPPRESS_ASAN
    size_t walk(std::span<UnprocessedStackFrame> stackTrace, bool& didRunOutOfSpace)
    {
        if (sReportStats)
            sNumTotalWalks++;
        resetAtMachineFrame();
        size_t maxStackTraceSize = stackTrace.size();
        while (!isAtTop() && !m_bailingOut && m_depth < maxStackTraceSize) {
            recordJITFrame(stackTrace);
            advanceToParentFrame();
            resetAtMachineFrame();
        }
        didRunOutOfSpace = m_depth >= maxStackTraceSize && !isAtTop();
        reportStats();
        return m_depth;
    }

protected:

    SUPPRESS_ASAN
    void recordJITFrame(std::span<UnprocessedStackFrame> stackTrace)
    {
        if (!m_compiledFrames) {
            Base::recordJITFrame(stackTrace);
            return;
        }
        const AOT::CodeLocation& code = (**m_compiledFrames).code;
        if (code.kind != AOT::CodeKind::Function)
            return;
        stackTrace[m_depth] = UnprocessedStackFrame();
        stackTrace[m_depth].compiledCode = AOT::CompiledFunctionID(code.function);
        stackTrace[m_depth].compiledCodeOffset = code.offset;
        m_depth++;
    }

    SUPPRESS_ASAN
    void advanceToParentFrame()
    {
        if (const void* callerPC = std::exchange(m_framelessTopCallerPC, nullptr)) {
            m_pc = callerPC;
            m_compiledFrames.reset();
            return;
        }
        if (m_compiledFrames) {
            ++*m_compiledFrames;
            if (*m_compiledFrames) {
                m_callFrame = (**m_compiledFrames).callFrame;
                m_pc = (**m_compiledFrames).pc;
                return;
            }
            m_compiledFrames.reset();
        }
        m_unrecordedFramePointer = nullptr;
        CallFrame* calleeFrame = m_callFrame;
        EntryFrame* calleeEntryFrame = m_entryFrame;
        m_pc = removeReturnAddressSignature(reinterpret_cast<const CallerFrameAndPC*>(calleeFrame)->returnPC);
        Base::advanceToParentFrame();
        if (isAtTop())
            return;
        if (m_entryFrame != calleeEntryFrame) {
            if (auto resumedFrame = AOT::findSampledTop(m_image, { calleeEntryFrame, nullptr, nullptr, calleeEntryFrame }, m_callFrame, m_entryFrame, m_stack.bounds)) {
                m_callFrame = resumedFrame->callFrame;
                m_pc = resumedFrame->pc;
                m_unrecordedFramePointer = resumedFrame->framePointer;
            } else
                m_pc = nullptr;
        }
        // A stub frame records nothing, so m_depth alone does not bound a walk over a cyclic chain of frames.
        if (m_callFrame <= calleeFrame) {
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
        }
    }

    SUPPRESS_ASAN
    void resetAtMachineFrame()
    {
        if (!isAtTop() && isValidFramePointer(m_callFrame)) {
            if (!m_compiledFrames)
                m_compiledFrames.emplace(m_image, m_callFrame, m_pc, m_stack, m_unrecordedFramePointer);
            if (*m_compiledFrames && (**m_compiledFrames).code.runsCompiledCode())
                return;
            m_compiledFrames.reset();
        }
        Base::resetAtMachineFrame();
    }

    const AOT::Image& m_image;
    AOT::SuspendedThreadStack m_stack;
    const void* m_pc;
    const void* m_framelessTopCallerPC;
    CallFrame* m_unrecordedFramePointer;
    std::optional<AOT::MachineFrameIterator<AOT::SuspendedThreadStack>> m_compiledFrames;
};

class CFrameWalkerWithCompiledFrames : public FrameWalkerWithCompiledFrames {
public:
    typedef FrameWalkerWithCompiledFrames Base;

    CFrameWalkerWithCompiledFrames(VM& vm, const AOT::Image& image, void* machineFrame, const AOT::SampledTop& top, EntryFrame* entryFrame, const StackBounds& stackBounds, const AbstractLocker& codeBlockSetLocker, const AbstractLocker& machineThreadsLocker)
        : Base(vm, image, { top.callFrame, top.pc, nullptr, top.framePointer }, entryFrame, stackBounds, codeBlockSetLocker, machineThreadsLocker)
        , m_machineFrame(machineFrame)
    {
    }

    size_t walk(std::span<UnprocessedStackFrame> stackTrace, bool& didRunOutOfSpace)
    {
        if (sReportStats)
            sNumTotalWalks++;
        resetAtMachineFrame();
        size_t maxStackTraceSize = stackTrace.size();
        if (!isAtTop() && !m_bailingOut && m_machineFrame == m_callFrame && maxStackTraceSize) {
            recordJITFrame(stackTrace);
            Base::advanceToParentFrame();
            resetAtMachineFrame();
        }

        while (!isAtTop() && !m_bailingOut && m_depth < maxStackTraceSize) {
            if (m_machineFrame >= m_callFrame) {
                m_bailingOut = true;
                break;
            }

            if (isCFrame()) {
                RELEASE_ASSERT(!LLInt::isLLIntPC(frame()->callerFrame));
                stackTrace[m_depth] = UnprocessedStackFrame(removeReturnAddressSignature(frame()->returnPC));
                m_depth++;
            } else
                recordJITFrame(stackTrace);
            advanceToParentFrame();
            resetAtMachineFrame();
        }
        didRunOutOfSpace = m_depth >= maxStackTraceSize && !isAtTop();
        reportStats();
        return m_depth;
    }

private:

    bool isCFrame()
    {
        return frame()->callerFrame != m_callFrame;
    }

    void advanceToParentFrame()
    {
        if (!isCFrame())
            Base::advanceToParentFrame();
        m_machineFrame = frame()->callerFrame;
    }

    void resetAtMachineFrame()
    {
        if (!isValidFramePointer(m_machineFrame)) {
            m_bailingOut = true;
            if (sReportStats)
                sNumFailedWalks++;
            return;
        }
        Base::resetAtMachineFrame();
    }

    CallerFrameAndPC* frame()
    {
        return reinterpret_cast<CallerFrameAndPC*>(m_machineFrame);
    }

    void* m_machineFrame;
};

static void* linkRegisterOf(const PlatformRegisters& registers)
{
#if USE(PLATFORM_REGISTERS_WITH_PROFILE)
    return removeCodePtrTag(WTF_READ_PLATFORM_REGISTERS_LR_WITH_PROFILE(registers)); // It need not hold a CodePtr.
#elif OS(LINUX) && CPU(ARM64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.machineContext.regs[30]));
#elif OS(WINDOWS) && CPU(ARM64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.Lr));
#elif OS(FREEBSD) && CPU(ARM64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.machineContext.mc_gpregs.gp_lr));
#else
    UNUSED_PARAM(registers);
    return nullptr;
#endif
}

static void* macroScratchRegisterOf(const PlatformRegisters& registers)
{
#if OS(DARWIN) && CPU(X86_64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.__r11));
#elif OS(LINUX) && CPU(X86_64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.machineContext.gregs[REG_R11]));
#elif OS(WINDOWS) && CPU(X86_64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.R11));
#elif OS(FREEBSD) && CPU(X86_64)
    return reinterpret_cast<void*>(static_cast<uintptr_t>(registers.machineContext.mc_r11));
#else
    UNUSED_PARAM(registers);
    return nullptr;
#endif
}
#endif // ENABLE(AOT)

// The profilers of a VM have one thread, which suspends the sampled one once for all of them.
class SamplingProfiler::Sampler final : public ThreadSafeRefCounted<Sampler> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(Sampler);
public:
    static Ref<Sampler> ensure(VM& vm)
    {
        if (!vm.samplingProfilers().isEmpty())
            return vm.samplingProfilers().first()->m_sampler;
        return adoptRef(*new Sampler(vm));
    }

    Lock& lock() LIFETIME_BOUND WTF_RETURNS_LOCK(m_lock) { return m_lock; }
    NODELETE Thread* thread() const { return m_thread.get(); }

    void didCreateProfiler() WTF_REQUIRES_LOCK(m_lock) { m_numberOfProfilers++; }
    void didShutDownProfiler() WTF_REQUIRES_LOCK(m_lock)
    {
        if (!--m_numberOfProfilers)
            wakeUpTimerLoop();

        size_t numberOfFrames = initialNumberOfFrames;
        for (auto& profiler : m_vm.samplingProfilers()) {
            assertIsHeld(profiler->m_lock);
            if (!profiler->m_isShutDown)
                numberOfFrames = std::max(numberOfFrames, profiler->m_maximumStackDepth);
        }
        if (numberOfFrames < m_currentFrames.size())
            m_currentFrames = Vector<UnprocessedStackFrame>(numberOfFrames);
    }

    void add(SamplingProfiler& profiler) WTF_REQUIRES_LOCK(m_lock)
    {
        m_runningProfilers.append(&profiler);
        wakeUpTimerLoop();
        createThreadIfNecessary();
    }
    void remove(SamplingProfiler& profiler) WTF_REQUIRES_LOCK(m_lock) { m_runningProfilers.removeFirst(&profiler); }

    void noticeCurrentThreadAsJSCExecutionThread() WTF_REQUIRES_LOCK(m_lock) { m_jscExecutionThread = &Thread::currentSingleton(); }
    void wakeUpTimerLoop() WTF_REQUIRES_LOCK(m_lock);

private:
    explicit Sampler(VM& vm)
        : m_vm(vm)
        , m_jscExecutionThread(&Thread::currentSingleton())
    {
        m_currentFrames.grow(initialNumberOfFrames);
    }

    static constexpr size_t initialNumberOfFrames = 256;

    void createThreadIfNecessary() WTF_REQUIRES_LOCK(m_lock);
    void timerLoop();
    bool waitWithoutBeingWokenUp(Locker<Lock>&, Seconds delay) WTF_REQUIRES_LOCK(m_lock);
    void takeSample(std::span<SamplingProfiler*>, Seconds timingInterval, Seconds& stackTraceProcessingTime) WTF_REQUIRES_LOCK(m_lock);
    size_t takeStackTrace(std::span<SamplingProfiler*>, MonotonicTime timestamp, Seconds& stackTraceProcessingTime) WTF_REQUIRES_LOCK(m_lock);

    Lock m_lock;
    Condition m_timerCondition;
#if OS(WINDOWS)
    Win32Handle m_highResolutionTimer WTF_GUARDED_BY_LOCK(m_lock);
#endif
    bool m_wasWokenUp WTF_GUARDED_BY_LOCK(m_lock) { false };
    unsigned m_numberOfProfilers WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    VM& m_vm;
    WeakRandom m_weakRandom;
    RefPtr<Thread> m_thread;
    RefPtr<Thread> m_jscExecutionThread WTF_GUARDED_BY_LOCK(m_lock);
    Vector<SamplingProfiler*> m_runningProfilers WTF_GUARDED_BY_LOCK(m_lock);
    Vector<UnprocessedStackFrame> m_currentFrames WTF_GUARDED_BY_LOCK(m_lock);
};

SamplingProfiler::SamplingProfiler(VM& vm, Ref<Stopwatch>&& stopwatch)
    : m_sampler(Sampler::ensure(vm))
    , m_lock(m_sampler->lock())
    , m_vm(vm)
    , m_stopwatch(WTF::move(stopwatch))
    , m_timingInterval(Seconds::fromMicroseconds(Options::sampleInterval()))
{
    if (sReportStats) {
        sNumTotalWalks = 0;
        sNumFailedWalks = 0;
    }

    {
        Locker locker { m_lock };
        setMaximumStackDepth(unlimitedStackDepth);
        assertIsHeld(m_sampler->lock());
        m_sampler->didCreateProfiler();
    }
    if (!vm.heap.objectSpace().preciseAllocationSet())
        vm.heap.objectSpace().enablePreciseAllocationTracking();
}

SamplingProfiler::~SamplingProfiler() = default;

void SamplingProfiler::Sampler::createThreadIfNecessary()
{
    ASSERT(m_lock.isLocked());

    if (m_thread)
        return;

#if OS(WINDOWS)
    // Setting a timer of the other kind does not make it forget that it has expired.
    if (HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE))
        m_highResolutionTimer = Win32Handle::adopt(timer);
#endif

    m_thread = Thread::create("jsc.sampling-profiler.thread"_s, [sampler = Ref { *this }] {
        sampler->timerLoop();
    });
    m_thread->detach();
#if OS(WINDOWS)
    // An expired timer earns a thread no boost. On a busy machine it would get to run when a core is free, like when the sampled thread waits.
    if (m_highResolutionTimer)
        m_thread->changePriority(THREAD_PRIORITY_HIGHEST);
#endif
}

#if OS(WINDOWS)
static bool setTimer(const Win32Handle& timer, Seconds delay)
{
    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -std::max<LONGLONG>(1, delay.nanosecondsAs<LONGLONG>() / 100); // Negative: from now, in units of 100 ns.
    return timer && SetWaitableTimer(timer.get(), &dueTime, 0, nullptr, nullptr, FALSE);
}
#endif

void SamplingProfiler::Sampler::wakeUpTimerLoop()
{
    m_wasWokenUp = true;
    m_timerCondition.notifyOne();
#if OS(WINDOWS)
    setTimer(m_highResolutionTimer, 0_s);
#endif
}

bool SamplingProfiler::Sampler::waitWithoutBeingWokenUp(Locker<Lock>& locker, Seconds delay)
{
    m_wasWokenUp = false;
#if OS(WINDOWS)
    // Any other timed wait is cut off to whole milliseconds: it spins for less than one, and otherwise lasts until the next
    // tick of the system's clock, 15.6 ms apart.
    if (setTimer(m_highResolutionTimer, delay)) {
        HANDLE timer = m_highResolutionTimer.get();
        {
            DropLockForScope unlocker { locker };
            WaitForSingleObject(timer, INFINITE);
        }
        return !m_wasWokenUp;
    }
#else
    UNUSED_PARAM(locker);
#endif
    m_timerCondition.waitFor(m_lock, delay);
    return !m_wasWokenUp;
}

void SamplingProfiler::Sampler::timerLoop()
{
    Locker locker { m_lock };
    Vector<SamplingProfiler*> dueProfilers;
    MonotonicTime endOfLastWait; // By the timer, which need not agree with the clock.
    while (m_numberOfProfilers) {
        if (m_runningProfilers.isEmpty()) {
            m_timerCondition.wait(m_lock);
            continue;
        }

        // The shortest interval sets the pace. One that is ahead of others that have it, as it has just started, takes its sample and falls in step.
        Seconds timingInterval = Seconds::infinity();
        MonotonicTime nextSampleTime;
        MonotonicTime nextTick;
        bool oneHasJustStarted = false;
        for (SamplingProfiler* profiler : m_runningProfilers) {
            assertIsHeld(profiler->m_lock);
            oneHasJustStarted |= profiler->m_hasJustStarted;
            if (profiler->m_timingInterval < timingInterval) {
                timingInterval = profiler->m_timingInterval;
                nextSampleTime = nextTick = profiler->m_nextSampleTime;
            } else if (profiler->m_timingInterval == timingInterval) {
                nextSampleTime = std::min(nextSampleTime, profiler->m_nextSampleTime);
                nextTick = std::max(nextTick, profiler->m_nextSampleTime);
            }
        }

        MonotonicTime now = std::max(MonotonicTime::now(), endOfLastWait);
        if (now < nextSampleTime && !oneHasJustStarted) {
            if (waitWithoutBeingWokenUp(locker, nextSampleTime - now))
                endOfLastWait = nextSampleTime;
            continue;
        }

        // The others take the sample that is nearest to their time.
        bool isTick = now >= nextTick;
        dueProfilers.shrink(0);
        for (SamplingProfiler* profiler : m_runningProfilers) {
            assertIsHeld(profiler->m_lock);
            if (profiler->m_timingInterval == timingInterval ? profiler->m_nextSampleTime <= now : profiler->m_nextSampleTime - now < timingInterval / 2)
                dueProfilers.append(profiler);
        }

        Seconds stackTraceProcessingTime = 0_s;
        takeSample(dueProfilers.mutableSpan(), timingInterval, stackTraceProcessingTime);

        // Read section 6.2 of this paper for more elaboration of why we add a random
        // fluctuation here. The main idea is to prevent our timer from being in sync
        // with some system process such as a scheduled context switch.
        // http://plv.colorado.edu/papers/mytkowicz-pldi10.pdf
        double randomSignedNumber = (m_weakRandom.get() * 2.0) - 1.0; // A random number between [-1, 1).
        Seconds randomFluctuation = timingInterval * 0.2 * randomSignedNumber;
        // A wait that is over before it begins hardly lets go of the lock, and of the thread.
        Seconds delay = std::max(timingInterval - std::min(timingInterval, stackTraceProcessingTime) + randomFluctuation, 10_us);

        now = MonotonicTime::now();
        if (isTick)
            nextTick = now + delay;
        for (SamplingProfiler* profiler : dueProfilers) {
            assertIsHeld(profiler->m_lock);
            profiler->m_hasJustStarted = false;
            if (profiler->m_timingInterval == timingInterval)
                profiler->m_nextSampleTime = nextTick;
            else
                profiler->m_nextSampleTime = std::max(profiler->m_nextSampleTime, now - timingInterval / 2) + profiler->m_timingInterval * (1 + 0.2 * randomSignedNumber);
        }

        if (isTick && waitWithoutBeingWokenUp(locker, delay))
            endOfLastWait = nextTick;
    }
}

SUPPRESS_TSAN
void SamplingProfiler::Sampler::takeSample(std::span<SamplingProfiler*> profilers, Seconds timingInterval, Seconds& stackTraceProcessingTime)
{
    ASSERT(m_lock.isLocked());
    std::span<SamplingProfiler*> recordingProfilers = std::ranges::partition(profilers, [](SamplingProfiler* profiler) {
        assertIsHeld(profiler->m_lock);
        return !profiler->m_recordsSamplesWithoutFrames;
    });

    // What counts is this instant, whatever the thread gets to before it is suspended. Short calls have often
    // returned by then, and their time would go to whatever runs between them.
    MonotonicTime timestamp = MonotonicTime::now();
    ThreadState state = ThreadState::Program;
    bool isInJavaScript = false;
    CodeBlock* outermostCodeBlock = nullptr;
    CalleeBits outermostCallee;
    if (m_vm.heap.isMutatorBusyWithCollectionConcurrently())
        state = ThreadState::GarbageCollector;
    else if (m_vm.topEntryFrame) {
        WTF::loadLoadFence();
        isInJavaScript = true;
        outermostCodeBlock = m_vm.outermostCodeBlock;
        outermostCallee = std::bit_cast<CalleeBits>(m_vm.outermostCallee);
    } else if (m_vm.isIdle())
        state = ThreadState::Idle;

    for (SamplingProfiler* profiler : profilers) {
        assertIsHeld(profiler->m_lock);
        profiler->m_stopwatchTimestamp = profiler->m_stopwatch->elapsedTimeSince(timestamp);
    }

    size_t depthServed = takeStackTrace(isInJavaScript ? profilers : profilers.first(profilers.size() - recordingProfilers.size()), timestamp, stackTraceProcessingTime);
    if (recordingProfilers.empty())
        return;

    // A stack trace takes longer than a sample in another state, and must not stand for more time because of that.
    // However long it takes, the thread gets to run before it is suspended again.
    stackTraceProcessingTime = std::min(MonotonicTime::now() - timestamp, timingInterval / 2);
    if (isInJavaScript) {
        recordingProfilers = std::ranges::partition(recordingProfilers, [&](SamplingProfiler* profiler) {
            assertIsHeld(profiler->m_lock);
            return profiler->m_maximumStackDepth <= depthServed;
        });
        if (recordingProfilers.empty())
            return;
    }

    if (outermostCallee.rawPtr()) {
        // The frame may have left it, by a tail call or an OSR exit. Then it is only safe if the collector is yet to visit the
        // profilers before it removes anything from the set, which is certain as long as it has not stopped the world.
        auto canUseCodeBlock = [&] {
            if (m_vm.heap.worldIsStopped())
                return false;
            Locker codeBlockSetLocker { m_vm.heap.codeBlockSet().getLock() };
            return m_vm.heap.codeBlockSet().contains(codeBlockSetLocker, outermostCodeBlock);
        };
        if (!canUseCodeBlock())
            outermostCodeBlock = nullptr;
        Ref stackTrace = adoptRef(*new SharedStackTrace({ nullptr, false, nullptr, nullptr, { { outermostCodeBlock, outermostCallee, CallSiteIndex() } }, false }));
        for (SamplingProfiler* profiler : recordingProfilers) {
            assertIsHeld(profiler->m_lock);
            profiler->appendStackTrace(timestamp, stackTrace);
        }
        return;
    }

    for (SamplingProfiler* profiler : recordingProfilers) {
        assertIsHeld(profiler->m_lock);
        profiler->appendSampleWithoutFrames(timestamp, state);
    }
}

// Those whose maximum stack depth is no more than the result have got one.
size_t SamplingProfiler::Sampler::takeStackTrace(std::span<SamplingProfiler*> profilers, MonotonicTime timestamp, Seconds& stackTraceProcessingTime)
{
    ASSERT(m_lock.isLocked());
    if (!profilers.empty() && m_vm.entryScope && !Options::forceSamplingProfilerToMissStacks()) {
        size_t maximumStackDepth = 0;
        for (SamplingProfiler* profiler : profilers) {
            assertIsHeld(profiler->m_lock);
            maximumStackDepth = std::max(maximumStackDepth, profiler->m_maximumStackDepth);
        }
        std::span currentFrames = m_currentFrames.mutableSpan().first(std::min(m_currentFrames.size(), maximumStackDepth));

        Locker machineThreadsLocker { m_vm.heap.machineThreads().getLock() };
        if (!m_vm.heap.machineThreads().threads(machineThreadsLocker).contains(m_jscExecutionThread.get()))
            return 0; // It has exited, and would never answer.
        Locker codeBlockSetLocker { m_vm.heap.codeBlockSet().getLock() };
        std::optional<Locker<Lock>> executableAllocatorLocker;
        if (Options::useJIT())
            executableAllocatorLocker.emplace(ExecutableAllocator::singleton().getLock());

        std::optional<Locker<Lock>> wasmCalleesLocker;
#if ENABLE(WEBASSEMBLY)
        if (Wasm::isSupported())
            wasmCalleesLocker.emplace(NativeCalleeRegistry::singleton().getLock());
#endif

        ThreadSuspendLocker threadSuspendLocker;
        auto didSuspend = m_jscExecutionThread->suspend(threadSuspendLocker);
        if (didSuspend) {
            // While the JSC thread is suspended, we can't do things like malloc because the JSC thread
            // may be holding the malloc lock.
            void* machineFrame;
            CallFrame* callFrame;
            EntryFrame* entryFrame = m_vm.topEntryFrame;
            void* machinePC;
            bool topFrameIsLLInt = false;
            RegExp* regExp = nullptr;
            void* llintPC;
#if ENABLE(AOT)
            const AOT::Image* image = m_vm.aotState().image;
            AOT::SampledRegisters sampledRegisters;
            [[maybe_unused]] void* wasmInstance;
#endif
            {
                PlatformRegisters registers;
                m_jscExecutionThread->getRegisters(threadSuspendLocker, registers);
                machineFrame = MachineContext::framePointer(registers);
                callFrame = static_cast<CallFrame*>(machineFrame);
                auto instructionPointer = MachineContext::instructionPointer(registers);
                if (instructionPointer)
                    machinePC = instructionPointer->untaggedPtr();
                else
                    machinePC = nullptr;
                llintPC = removeCodePtrTag(MachineContext::llintInstructionPointer(registers));
                assertIsNotTagged(machinePC);
#if ENABLE(AOT)
                sampledRegisters = { machineFrame, machinePC, linkRegisterOf(registers), MachineContext::stackPointer(registers), macroScratchRegisterOf(registers) };
                wasmInstance = MachineContext::wasmInstancePointer(registers);
#endif
            }

            bool shouldAppendTopFrameAsCCode = false;
            [[maybe_unused]] bool startsAtTopCallFrame = false;
            std::span framesToWalk = currentFrames;
            // FIXME: Lets have a way of detecting when we're parsing code.
            // https://bugs.webkit.org/show_bug.cgi?id=152761
            if (Options::useJIT() && ExecutableAllocator::singleton().isValidExecutableMemory(*executableAllocatorLocker, machinePC)) {
                regExp = m_vm.m_executingRegExp;
                if (regExp) {
                    callFrame = m_vm.topCallFrame; // We need to do this or else we'd fail our backtrace validation b/c this isn't a JS frame.
                    startsAtTopCallFrame = true;
                }
            } else if (LLInt::isLLIntPC(machinePC) && (machineFrame == entryFrame || !LLInt::isVMEntryPC(machinePC))) {
                topFrameIsLLInt = true;
                // We're okay to take a normal stack trace when the PC
                // is in LLInt code.
#if ENABLE(AOT)
                sampledRegisters.linkRegister = nullptr; // A handler is entered with what the code that threw has left there.
            } else if (image && image->locate(machinePC).runsCompiledCode()) {
                // We're okay to take a normal stack trace here too.
#if ENABLE(WEBASSEMBLY_OMGJIT)
            } else if (Wasm::CodeInImage::contains(machinePC)) {
                if (auto* callee = Wasm::CodeInImage::framelessCalleeAt(machinePC, static_cast<JSWebAssemblyInstance*>(wasmInstance))) {
                    UnprocessedStackFrame& frame = consume(framesToWalk);
                    frame = UnprocessedStackFrame(nullptr, callee, CallSiteIndex());
                    frame.nativeCalleeCategory = NativeCallee::Category::Wasm;
                    frame.wasmCompilationMode = callee->compilationMode();
                    frame.wasmIndexOrName = callee->indexOrName();
                    callFrame = AOT::findCallerOfFramelessWebAssembly(sampledRegisters, callee, m_jscExecutionThread->stack());
                }
                sampledRegisters.linkRegister = nullptr;
#endif
#endif
            } else {
                // RegExp evaluation is leaf. So if RegExp evaluation exists, we can say it is RegExp evaluation is the top user-visible frame.
                regExp = m_vm.m_executingRegExp;
                // We resort to topCallFrame to see if we can get anything
                // useful. We usually get here when we're executing C code.
                callFrame = m_vm.topCallFrame;
                startsAtTopCallFrame = true;
                if (Options::collectExtraSamplingProfilerData() && !Options::sampleCCode())
                    shouldAppendTopFrameAsCCode = true;
            }

            if (entryFrame && static_cast<void*>(callFrame) == entryFrame) {
                // What was entered has no frame yet, or no longer.
#if ENABLE(AOT)
                sampledRegisters = { entryFrame, nullptr, nullptr, entryFrame };
#endif
                VMEntryRecord* entryRecord = vmEntryRecord(entryFrame);
                callFrame = entryRecord->unsafePrevTopCallFrame();
                startsAtTopCallFrame = true;
                entryFrame = entryRecord->unsafePrevTopEntryFrame();
                topFrameIsLLInt = false;
            }

            size_t walkSize;
            bool wasValidWalk;
            bool didRunOutOfVectorSpace;
            size_t numberOfFramesWritten = 0;
#if ENABLE(AOT)
            if (image) [[unlikely]] {
                const StackBounds& stackBounds = m_jscExecutionThread->stack();
                auto walkFrom = [&](const AOT::SampledTop& top) {
                    topFrameIsLLInt &= top.callFrame == machineFrame;
                    if (Options::sampleCCode()) {
                        CFrameWalkerWithCompiledFrames walker(m_vm, *image, machineFrame, top, entryFrame, stackBounds, codeBlockSetLocker, machineThreadsLocker);
                        walkSize = walker.walk(framesToWalk, didRunOutOfVectorSpace);
                        wasValidWalk = walker.wasValidWalk();
                    } else {
                        FrameWalkerWithCompiledFrames walker(m_vm, *image, top, entryFrame, stackBounds, codeBlockSetLocker, machineThreadsLocker);
                        walkSize = walker.walk(framesToWalk, didRunOutOfVectorSpace);
                        wasValidWalk = walker.wasValidWalk();
                    }
                };
                walkFrom(AOT::findSampledTop(*image, sampledRegisters, callFrame, entryFrame, stackBounds).value_or(AOT::SampledTop { callFrame, callFrame == machineFrame ? machinePC : nullptr, nullptr }));
                // Not every call from compiled code stores topCallFrame, and a frame of the engine may not have been set up yet.
                bool triesAgain = !wasValidWalk && (startsAtTopCallFrame || !walkSize);
                if (auto top = triesAgain ? AOT::findSampledTop(*image, sampledRegisters, nullptr, entryFrame, stackBounds) : std::nullopt) {
                    numberOfFramesWritten = walkSize;
                    walkFrom(*top);
                }
            } else
#endif
            if (Options::sampleCCode()) {
                CFrameWalker walker(m_vm, machineFrame, callFrame, entryFrame, codeBlockSetLocker, machineThreadsLocker);
                walkSize = walker.walk(framesToWalk, didRunOutOfVectorSpace);
                wasValidWalk = walker.wasValidWalk();
            } else {
                FrameWalker walker(m_vm, callFrame, entryFrame, codeBlockSetLocker, machineThreadsLocker);
                walkSize = walker.walk(framesToWalk, didRunOutOfVectorSpace);
                wasValidWalk = walker.wasValidWalk();
            }

            m_jscExecutionThread->resume(threadSuspendLocker);
            size_t numberOfFramesNotWalked = currentFrames.size() - framesToWalk.size();
            numberOfFramesWritten = std::max(numberOfFramesWritten, walkSize) + numberOfFramesNotWalked;
            walkSize += numberOfFramesNotWalked;

            auto startTime = MonotonicTime::now();
            // We can now use data structures that malloc, and do other interesting things, again.

            // FIXME: It'd be interesting to take data about the program's state when
            // we fail to take a stack trace: https://bugs.webkit.org/show_bug.cgi?id=152758
            if (unsigned depth = Options::forceSamplingProfilerToFailAtStackDepth(); depth && walkSize > depth) [[unlikely]] {
                walkSize = depth;
                wasValidWalk = false;
            }
            // What is wrong with a frame is nothing to a profiler that would not have got as far.
            size_t depthServed = !walkSize ? 0 : wasValidWalk ? unlimitedStackDepth : walkSize - 1;
            if (depthServed) {
                if (sReportStats)
                    sNumTotalStackTraces++;
                Vector<std::pair<size_t, Ref<SharedStackTrace>>, 2> stackTracesByDepth;
                for (SamplingProfiler* profiler : profilers) {
                    assertIsHeld(profiler->m_lock);
                    if (profiler->m_maximumStackDepth > depthServed)
                        continue;
                    size_t depth = std::min(walkSize, profiler->m_maximumStackDepth);
                    size_t index = stackTracesByDepth.findIf([&](auto& entry) {
                        return entry.first == depth;
                    });
                    if (index == notFound) {
                        Vector<UnprocessedStackFrame> stackTrace(ReserveCapacityTag { }, depth + !!shouldAppendTopFrameAsCCode);
                        if (shouldAppendTopFrameAsCCode)
                            stackTrace.append(UnprocessedStackFrame { machinePC });
                        stackTrace.append(currentFrames.first(depth));
                        index = stackTracesByDepth.size();
                        stackTracesByDepth.append({ depth, adoptRef(*new SharedStackTrace({ machinePC, topFrameIsLLInt, llintPC, regExp, WTF::move(stackTrace) })) });
                    }
                    profiler->appendStackTrace(timestamp, stackTracesByDepth[index].second);
                }
            }

            // The next walk must not be what lets go of their references: that may free memory.
            std::ranges::fill(currentFrames.first(numberOfFramesWritten), UnprocessedStackFrame());
            if (wasValidWalk && didRunOutOfVectorSpace && m_currentFrames.size() < maximumStackDepth)
                m_currentFrames.grow(std::min(m_currentFrames.size() + m_currentFrames.size() / 4, maximumStackDepth));

            auto endTime = MonotonicTime::now();
            stackTraceProcessingTime = endTime - startTime;
            return depthServed;
        }
    }
    return 0;
}

void SamplingProfiler::appendStackTrace(MonotonicTime timestamp, SharedStackTrace& stackTrace)
{
    m_stackTraces.append({ timestamp, m_stopwatchTimestamp, { }, stackTrace });
    m_canExtendLastSamplesWithoutFrames = false;
}

void SamplingProfiler::appendSampleWithoutFrames(MonotonicTime timestamp, ThreadState state)
{
    if (!m_canExtendLastSamplesWithoutFrames || m_samplesWithoutFrames.last().state != state) {
        m_samplesWithoutFrames.append({ state, timestamp, timestamp, 0 });
        m_canExtendLastSamplesWithoutFrames = true;
    }
    m_samplesWithoutFrames.last().last = timestamp;
    m_samplesWithoutFrames.last().count++;
}

NODELETE static ALWAYS_INLINE BytecodeIndex tryGetBytecodeIndex(unsigned llintPC, CodeBlock* codeBlock)
{
#if ENABLE(DFG_JIT)
    RELEASE_ASSERT(!codeBlock->hasCodeOrigins());
#endif

    unsigned bytecodeOffset = llintPC;
    if (bytecodeOffset < codeBlock->instructionsSize())
        return BytecodeIndex(bytecodeOffset);
    return BytecodeIndex();
}

#if ENABLE(AOT)
using CompiledSitePositions = UncheckedKeyHashMap<uint64_t, std::optional<AOT::SourcePosition>>;

static void appendCompiledFrames(const AOT::Image& image, const UnprocessedStackFrame& machineFrame, AOT::ShownFrameFilter& shownFrames, CompiledSitePositions& sitePositions, Vector<SamplingProfiler::StackFrame>& frames)
{
    AOT::CompiledFunctionID code = machineFrame.compiledCode;
    auto append = [&](const AOT::TraceFrame& compiledFrame) -> SamplingProfiler::StackFrame& {
        frames.append(SamplingProfiler::StackFrame());
        frames.last().compiledFrame = compiledFrame;
        return frames.last();
    };

    if (auto site = image.callSiteAt(code.index(), machineFrame.compiledCodeOffset)) {
        AOT::forEachShownFrame(code, *site, shownFrames, [&](const AOT::TraceFrame& compiledFrame) {
            SamplingProfiler::StackFrame& frame = append(compiledFrame);
            uint64_t key = (static_cast<uint64_t>(compiledFrame.code.index()) << 32 | compiledFrame.callSite.bits()) + 1;
            auto& position = sitePositions.ensure(key, [&] {
                return AOT::positionOf(compiledFrame);
            }).iterator->value;
            if (!position)
                return;
            frame.semanticLocation.lineColumn = position->lineColumn;
            frame.compiledSiteModule = position->source;
        });
        return;
    }

    AOT::FunctionDescription root(image.rootFunctionOf(code.index()));
    if (shownFrames.shows(root.visibility(), false))
        append({ code, root.sourceFunction(), CallSiteIndex(), false, false });
}
#endif

void SamplingProfiler::processUnverifiedStackTraces()
{
    RELEASE_ASSERT(m_lock.isLocked());

    TinyBloomFilter<uintptr_t> filter = m_vm.heap.objectSpace().blocks().filter();
#if ENABLE(AOT)
    CompiledSitePositions compiledSitePositions;
#endif

    std::span unverifiedStackTraces = m_stackTraces.mutableSpan().subspan(m_numberOfVerifiedStackTraces);
    for (StackTrace& unverifiedStackTrace : unverifiedStackTraces) {
        SharedStackTrace& stackTrace = unverifiedStackTrace.ownerOfFrames;
        if (!stackTrace.unprocessed)
            continue;
        UnprocessedStackTrace& unprocessedStackTrace = *stackTrace.unprocessed;
        stackTrace.frames.reserveInitialCapacity(unprocessedStackTrace.frames.size() + !!unprocessedStackTrace.regExp);

        auto populateCodeLocation = [] (CodeBlock* codeBlock, JITType jitType, BytecodeIndex bytecodeIndex, StackFrame::CodeLocation& location) {
            if (bytecodeIndex.offset() < codeBlock->instructionsSize()) {
                location.lineColumn = codeBlock->lineColumnForBytecodeIndex(bytecodeIndex);
                location.bytecodeIndex = bytecodeIndex;
            }
            location.codeBlockHash = codeBlock->hash();
            location.jitType = jitType;
        };

        auto appendCodeBlock = [&] (CodeBlock* codeBlock, JITType jitType, BytecodeIndex bytecodeIndex) {
            stackTrace.frames.append(StackFrame(codeBlock->ownerExecutable()));
            populateCodeLocation(codeBlock, jitType, bytecodeIndex, stackTrace.frames.last().semanticLocation);
        };

        auto appendEmptyFrame = [&] {
            stackTrace.frames.append(StackFrame());
        };

#if ENABLE(WEBASSEMBLY)
        auto storeWasmCalleeIntoLastFrame = [&] (UnprocessedStackFrame& unprocessedStackFrame, void* pc) {
            CalleeBits calleeBits = unprocessedStackFrame.unverifiedCallee;
            ASSERT_UNUSED(calleeBits, calleeBits.isNativeCallee());
            StackFrame& stackFrame = stackTrace.frames.last();
            stackFrame.frameType = FrameType::Wasm;
            stackFrame.wasmIndexOrName = unprocessedStackFrame.wasmIndexOrName;
            stackFrame.wasmCompilationMode = unprocessedStackFrame.wasmCompilationMode;
#if ENABLE(JIT)
            if (pc && unprocessedStackFrame.wasmPCMap) {
                if (std::optional<CodeOrigin> codeOrigin = unprocessedStackFrame.wasmPCMap->findPC(pc))
                    stackFrame.wasmOffset = codeOrigin->bytecodeIndex();
            }
#else
            UNUSED_PARAM(pc);
#endif
        };
#endif

        auto storeCalleeIntoLastFrame = [&] (UnprocessedStackFrame& unprocessedStackFrame) {
            // Set the callee if it's a valid GC object.
            CalleeBits calleeBits = unprocessedStackFrame.unverifiedCallee;
            StackFrame& stackFrame = stackTrace.frames.last();
            bool alreadyHasExecutable = !!stackFrame.executable;
            if (calleeBits.isNativeCallee()) {
                switch (unprocessedStackFrame.nativeCalleeCategory) {
                case NativeCallee::Category::Wasm: {
#if ENABLE(WEBASSEMBLY)
                    storeWasmCalleeIntoLastFrame(unprocessedStackFrame, nullptr);
#endif
                    return;
                }
                case NativeCallee::Category::InlineCache: {
                    stackFrame.frameType = FrameType::Unknown;
                    return;
                }
                }
                return;
            }

            JSValue callee = calleeBits.asCell();
            if (!HeapUtil::isValueGCObject(m_vm.heap, filter, callee)) {
                if (!alreadyHasExecutable)
                    stackFrame.frameType = FrameType::Unknown;
                return;
            }

            JSCell* calleeCell = callee.asCell();
            auto setFallbackFrameType = [&] {
                ASSERT(!alreadyHasExecutable);
                FrameType result = FrameType::Unknown;
                auto callData = JSC::getCallData(calleeCell);
                if (callData.type == CallData::Type::Native)
                    result = FrameType::Host;

                stackFrame.frameType = result;
            };

            auto addCallee = [&] (JSObject* callee) {
                stackFrame.callee = callee;
            };

#if ENABLE(AOT)
            // The frame exists but the pc is outside the function's code: the sample landed between the call and the callee's first instruction.
            if (std::optional<AOT::FunctionID> function = alreadyHasExecutable ? std::nullopt : AOT::functionOf(calleeCell)) {
                stackFrame.compiledFrame = { { }, AOT::FunctionDescription(*function).sourceFunction(), CallSiteIndex(), false, false };
                return;
            }
            if (auto* savedFrame = unprocessedStackTrace.wasWalked ? nullptr : dynamicDowncast<AOT::SavedFrame>(calleeCell)) {
                const AOT::Image& image = *m_vm.aotState().image;
                if (AOT::CodeLocation code = image.locate(savedFrame->resumeAddress()); code.kind == AOT::CodeKind::Function)
                    stackFrame.compiledFrame = { AOT::CompiledFunctionID(code.function), AOT::FunctionDescription(image.rootFunctionOf(code.function)).sourceFunction(), CallSiteIndex(), false, false };
                return;
            }
#endif

            if (!isJSFunctionType(calleeCell->type())) {
                if (JSObject* object = dynamicDowncast<JSObject>(calleeCell))
                    addCallee(object);

                if (!alreadyHasExecutable)
                    setFallbackFrameType();

                return;
            }

            addCallee(uncheckedDowncast<JSFunction>(calleeCell));

            if (alreadyHasExecutable)
                return;

            ExecutableBase* executable = uncheckedDowncast<JSFunction>(calleeCell)->executable();
            if (!executable) {
                setFallbackFrameType();
                return;
            }

            RELEASE_ASSERT(HeapUtil::isPointerGCObjectJSCell(m_vm.heap, filter, executable));
            stackFrame.frameType = FrameType::Executable;
            stackFrame.executable = executable;
        };

        auto appendCodeOrigin = [&] (CodeBlock* machineCodeBlock, CodeOrigin origin) {
            size_t startIndex = stackTrace.frames.size(); // We want to change stack traces that we're about to append.

            CodeOrigin machineOrigin;
            origin.walkUpInlineStack([&] (const CodeOrigin& codeOrigin) {
                machineOrigin = codeOrigin;
                auto* inlineCallFrame = codeOrigin.inlineCallFrame();
                appendCodeBlock(inlineCallFrame ? inlineCallFrame->baselineCodeBlock.get() : machineCodeBlock, machineCodeBlock->jitType(), codeOrigin.bytecodeIndex());
            });

            if (Options::collectExtraSamplingProfilerData()) {
                RELEASE_ASSERT(machineOrigin.isSet());
                RELEASE_ASSERT(!machineOrigin.inlineCallFrame());

                StackFrame::CodeLocation machineLocation = stackTrace.frames.last().semanticLocation;

                // We want to tell each inlined frame about the machine frame
                // they were inlined into. Currently, we only use this for dumping
                // output on the command line, but we could extend it to the web
                // inspector in the future if we find a need for it there.
                RELEASE_ASSERT(stackTrace.frames.size());
                for (size_t i = startIndex; i < stackTrace.frames.size() - 1; i++)
                    stackTrace.frames[i].machineLocation = std::make_pair(machineLocation, machineCodeBlock);
            }
        };

        if (!unprocessedStackTrace.wasWalked) {
            // The CodeBlock may be of another entry, or a new one at the same address.
            UnprocessedStackFrame& frame = unprocessedStackTrace.frames[0];
            auto isCodeBlockOfCallee = [&] {
                JSCell* callee = frame.unverifiedCallee.asCell();
                if (!HeapUtil::isValueGCObject(m_vm.heap, filter, callee))
                    return false;
                if (frame.verifiedCodeBlock->codeType() != FunctionCode)
                    return callee->type() == JSCalleeType;
                return callee->type() == JSFunctionType && uncheckedDowncast<JSFunction>(callee)->executable() == frame.verifiedCodeBlock->ownerExecutable();
            };
            if (frame.verifiedCodeBlock && !isCodeBlockOfCallee())
                frame.verifiedCodeBlock = nullptr;
        }

        // Prepend the top-most inlined frame if needed and gather
        // location information about where the top frame is executing.
        size_t startIndex = 0;
        if (unprocessedStackTrace.regExp) {
            // If the stack-trace is annotated with RegExp, the top-frame must be RegExp since RegExp evaluation is leaf function.
            appendEmptyFrame();
            stackTrace.frames.last().regExp = unprocessedStackTrace.regExp;
            stackTrace.frames.last().frameType = FrameType::RegExp;
            stackTrace.frames.last().semanticLocation.isRegExp = true;
        } else if (!unprocessedStackTrace.frames.isEmpty() && !!unprocessedStackTrace.frames[0].verifiedCodeBlock) {
            CodeBlock* topCodeBlock = unprocessedStackTrace.frames[0].verifiedCodeBlock;
            if (unprocessedStackTrace.topFrameIsLLInt) {
                // We reuse LLInt CodeBlocks for the baseline JIT, so we need to check for both jit types.
                // This might also be false for various reasons (known and unknown), even though
                // it's super unlikely. One reason that this can be false is when we throw from a DFG frame,
                // and we end up having to unwind past an EntryFrame, we will end up executing
                // inside the LLInt's llint_handle_ucaught_exception. So we just protect against this
                // by ignoring it.
                BytecodeIndex bytecodeIndex = BytecodeIndex(0);
                if (topCodeBlock->jitType() == JITType::InterpreterThunk || topCodeBlock->jitType() == JITType::BaselineJIT) {
                    unsigned bits = static_cast<unsigned>(std::bit_cast<uintptr_t>(unprocessedStackTrace.llintPC));
                    bytecodeIndex = tryGetBytecodeIndex(bits, topCodeBlock);

                    UNUSED_PARAM(bytecodeIndex); // FIXME: do something with this info for the web inspector: https://bugs.webkit.org/show_bug.cgi?id=153455

                    appendCodeBlock(topCodeBlock, topCodeBlock->jitType(), bytecodeIndex);
                    storeCalleeIntoLastFrame(unprocessedStackTrace.frames[0]);
                    startIndex = 1;
                }
            } else {
#if ENABLE(JIT)
                if (std::optional<CodeOrigin> codeOrigin = topCodeBlock->findPC(unprocessedStackTrace.topPC)) {
                    appendCodeOrigin(topCodeBlock, *codeOrigin);
                    storeCalleeIntoLastFrame(unprocessedStackTrace.frames[0]);
                    startIndex = 1;
                }
#endif
                UNUSED_PARAM(appendCodeOrigin);
            }
        }
#if ENABLE(WEBASSEMBLY)
        else if (!unprocessedStackTrace.frames.isEmpty() && unprocessedStackTrace.frames[0].unverifiedCallee.isNativeCallee() && unprocessedStackTrace.frames[0].nativeCalleeCategory == NativeCallee::Category::Wasm) {
            appendEmptyFrame();
            storeWasmCalleeIntoLastFrame(unprocessedStackTrace.frames[0], unprocessedStackTrace.topPC);
            startIndex = 1;
        }
#endif

#if ENABLE(AOT)
        AOT::ShownFrameFilter shownFrames;
#endif
        for (size_t i = startIndex; i < unprocessedStackTrace.frames.size(); i++) {
            UnprocessedStackFrame& unprocessedStackFrame = unprocessedStackTrace.frames[i];
            if (CodeBlock* codeBlock = unprocessedStackFrame.verifiedCodeBlock) {
                CallSiteIndex callSiteIndex = unprocessedStackFrame.callSiteIndex;

                auto appendCodeBlockNoInlining = [&] {
                    appendCodeBlock(codeBlock, codeBlock->jitType(), tryGetBytecodeIndex(callSiteIndex.bits(), codeBlock));
                };

#if ENABLE(DFG_JIT)
                if (codeBlock->hasCodeOrigins()) {
                    if (codeBlock->canGetCodeOrigin(callSiteIndex))
                        appendCodeOrigin(codeBlock, codeBlock->codeOrigin(callSiteIndex));
                    else
                        appendCodeBlock(codeBlock, codeBlock->jitType(), BytecodeIndex());
                } else
                    appendCodeBlockNoInlining();
#else
                appendCodeBlockNoInlining();
#endif
#if ENABLE(AOT)
            } else if (unprocessedStackFrame.compiledCode) {
                appendCompiledFrames(*m_vm.aotState().image, unprocessedStackFrame, shownFrames, compiledSitePositions, stackTrace.frames);
                continue;
#endif
            } else if (unprocessedStackFrame.cCodePC) {
                appendEmptyFrame();
                stackTrace.frames.last().cCodePC = unprocessedStackFrame.cCodePC;
                stackTrace.frames.last().frameType = FrameType::C;
            } else
                appendEmptyFrame();

            // Note that this is okay to do if we walked the inline stack because
            // the machine frame will be at the top of the processed stack trace.
            if (!unprocessedStackFrame.cCodePC)
                storeCalleeIntoLastFrame(unprocessedStackFrame);
        }

        if (!unprocessedStackTrace.wasWalked) {
            StackFrame& frame = stackTrace.frames.last();
            bool isShown = frame.frameType != FrameType::Unknown;
#if ENABLE(AOT)
            if (frame.isCompiledFrame())
                isShown = shownFrames.shows(AOT::FunctionDescription(frame.compiledFrame.function).visibility(), false);
#endif
            if (!isShown)
                stackTrace.frames.clear();
        }

        stackTrace.unprocessed = std::nullopt;
    }

    for (StackTrace& stackTrace : unverifiedStackTraces) {
        stackTrace.frames = stackTrace.ownerOfFrames->frames.mutableSpan();
        for (StackFrame& frame : stackTrace.frames) {
            for (JSCell* cell : std::initializer_list<JSCell*> { frame.executable, frame.callee, frame.regExp, frame.machineLocation ? frame.machineLocation->second : nullptr }) {
                if (cell)
                    m_liveCellPointers.add(cell);
            }
        }
    }
    m_numberOfVerifiedStackTraces = m_stackTraces.size();
}

template<typename Visitor>
void SamplingProfiler::visit(Visitor& visitor)
{
    RELEASE_ASSERT(m_lock.isLocked());
    for (JSCell* cell : m_liveCellPointers)
        visitor.appendUnbarriered(cell);
}

template void SamplingProfiler::visit(AbstractSlotVisitor&);
template void SamplingProfiler::visit(SlotVisitor&);

void SamplingProfiler::shutdown()
{
    {
        Locker locker { m_lock };
        if (std::exchange(m_isShutDown, true))
            return; // The VM may be gone.
        pause();
        clearData();
        assertIsHeld(m_sampler->lock());
        m_sampler->didShutDownProfiler();
    }
    m_vm.didShutDownSamplingProfiler(*this);
}

void SamplingProfiler::setTimingInterval(Seconds interval)
{
    Locker locker { m_lock };
    if (std::exchange(m_timingInterval, interval) == interval)
        return;
    m_canExtendLastSamplesWithoutFrames = false;
    m_nextSampleTime = std::min(m_nextSampleTime, MonotonicTime::now() + interval);
    assertIsHeld(m_sampler->lock());
    m_sampler->wakeUpTimerLoop();
}

void SamplingProfiler::setMaximumStackDepth(size_t depth)
{
    ASSERT(m_lock.isLocked());
    RELEASE_ASSERT(depth);
    if (Options::samplingProfilerMaxStackDepth())
        depth = Options::samplingProfilerMaxStackDepth();
    m_maximumStackDepth = depth;
}

void SamplingProfiler::start()
{
    JSC_BEGIN_REMOVABLE_ENTRY_POINT(Profilers, ReturnDefault, void)
    Locker locker { m_lock };
    startWithLock();
    JSC_END_REMOVABLE_ENTRY_POINT
}

void SamplingProfiler::startWithLock()
{
    ASSERT(m_lock.isLocked());
    if (m_isShutDown || std::exchange(m_isRunning, true))
        return;
    m_nextSampleTime = MonotonicTime::now();
    m_hasJustStarted = true;
    assertIsHeld(m_sampler->lock());
    m_sampler->add(*this);
}

void SamplingProfiler::pause()
{
    ASSERT(m_lock.isLocked());
    m_isRunning = false;
    assertIsHeld(m_sampler->lock());
    m_sampler->remove(*this);
    m_canExtendLastSamplesWithoutFrames = false;
    reportStats();
}

void SamplingProfiler::noticeCurrentThreadAsJSCExecutionThreadWithLock()
{
    assertIsHeld(m_sampler->lock());
    m_sampler->noticeCurrentThreadAsJSCExecutionThread();
}

void SamplingProfiler::noticeCurrentThreadAsJSCExecutionThread()
{
    Locker locker { m_lock };
    noticeCurrentThreadAsJSCExecutionThreadWithLock();
}

void SamplingProfiler::clearData()
{
    ASSERT(m_lock.isLocked());
    m_stackTraces.clear();
    m_numberOfVerifiedStackTraces = 0;
    m_liveCellPointers.clear();
    m_samplesWithoutFrames.clear();
    m_canExtendLastSamplesWithoutFrames = false;
}

String SamplingProfiler::StackFrame::nameFromCallee(VM& vm)
{
    if (!callee) {
        if (regExp)
            return regExp->toSourceString();
        return String();
    }

    DeferTermination deferScope(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSGlobalObject* globalObject = callee->realmMayBeNull();
    if (!globalObject)
        return String();

    auto getPropertyIfPureOperation = [&] (const Identifier& ident) -> String {
        PropertySlot slot(callee, PropertySlot::InternalMethodType::VMInquiry, &vm);
        PropertyName propertyName(ident);
        bool hasProperty = callee->getPropertySlot(globalObject, propertyName, slot);
        scope.assertNoException();
        if (hasProperty) {
            if (slot.isValue()) {
                JSValue nameValue = slot.getValue(globalObject, propertyName);
                if (isJSString(nameValue))
                    return asString(nameValue)->tryGetValue();
            }
        }
        return String();
    };

    String name = getPropertyIfPureOperation(vm.propertyNames->displayName);
    if (!name.isEmpty())
        return name;

    return getPropertyIfPureOperation(vm.propertyNames->name);
}

String SamplingProfiler::StackFrame::displayName(VM& vm)
{
#if ENABLE(AOT)
    if (isCompiledFrame()) {
        AOT::FunctionDescription description(compiledFrame.function);
        if (description.codeType() == ModuleCode)
            return "(module)"_s;
        return description.stackTraceName(vm);
    }
#endif
    {
        String name = nameFromCallee(vm);
        if (!name.isEmpty())
            return name;
    }

    switch (frameType) {
    case FrameType::C:
#if HAVE(DLADDR)
        if (frameType == FrameType::C) {
            auto demangled = StackTraceSymbolResolver::demangle(const_cast<void*>(cCodePC));
            if (demangled)
                return String::fromLatin1(demangled->demangledName() ? demangled->demangledName() : demangled->mangledName());
        }
#endif
        return "(unknown C PC)"_s;

    case FrameType::Unknown:
        return "(unknown)"_s;

    case FrameType::Host:
        return "(host)"_s;

    case FrameType::RegExp:
        return "(regexp)"_s;

    case FrameType::Wasm:
#if ENABLE(WEBASSEMBLY)
        if (wasmIndexOrName)
            return makeString(wasmIndexOrName.value());
#endif
        return "(wasm)"_s;

    case FrameType::Executable:
        if (executable->isHostFunction())
            return static_cast<NativeExecutable*>(executable)->name();

        if (executable->isFunctionExecutable())
            return static_cast<FunctionExecutable*>(executable)->ecmaName().string();
        if (executable->isProgramExecutable() || executable->isEvalExecutable())
            return "(program)"_s;
        if (executable->isModuleProgramExecutable())
            return "(module)"_s;

        RELEASE_ASSERT_NOT_REACHED();
        return String();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return String();
}

int SamplingProfiler::StackFrame::functionStartLine()
{
#if ENABLE(AOT)
    if (isCompiledFrame()) {
        auto definition = AOT::definitionOf(compiledFrame);
        return definition ? static_cast<int>(definition->lineColumn.line) : -1;
    }
#endif
    switch (frameType) {
    case FrameType::Unknown:
    case FrameType::Host:
    case FrameType::RegExp:
    case FrameType::C:
    case FrameType::Wasm:
        return -1;

    case FrameType::Executable:
        if (executable->isHostFunction())
            return -1;
        return static_cast<ScriptExecutable*>(executable)->firstLine();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return -1;
}

unsigned SamplingProfiler::StackFrame::functionStartColumn()
{
#if ENABLE(AOT)
    if (isCompiledFrame()) {
        auto definition = AOT::definitionOf(compiledFrame);
        return definition ? definition->lineColumn.column : std::numeric_limits<unsigned>::max();
    }
#endif
    switch (frameType) {
    case FrameType::Unknown:
    case FrameType::Host:
    case FrameType::RegExp:
    case FrameType::C:
    case FrameType::Wasm:
        return std::numeric_limits<unsigned>::max();

    case FrameType::Executable:
        if (executable->isHostFunction())
            return std::numeric_limits<unsigned>::max();

        return static_cast<ScriptExecutable*>(executable)->startColumn();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return std::numeric_limits<unsigned>::max();
}

#if ENABLE(AOT)
AOT::Typed::ModuleID SamplingProfiler::StackFrame::compiledModule() const
{
    auto definition = AOT::definitionOf(compiledFrame);
    return definition && definition->source ? definition->source : compiledSiteModule;
}
#endif

std::tuple<SourceProvider*, SourceID> SamplingProfiler::StackFrame::sourceProviderAndID()
{
#if ENABLE(AOT)
    if (isCompiledFrame()) {
        AOT::Typed::ModuleID module = compiledModule();
        return { nullptr, module ? firstCompiledCodeSourceID + module.index() : internalSourceID };
    }
#endif
    switch (frameType) {
    case FrameType::Unknown:
    case FrameType::Host:
    case FrameType::RegExp:
    case FrameType::C:
    case FrameType::Wasm:
        return { nullptr, internalSourceID };

    case FrameType::Executable: {
        if (executable->isHostFunction())
            return { nullptr, internalSourceID };

        auto* provider = static_cast<ScriptExecutable*>(executable)->source().provider();
        return { provider, provider->asID() };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { nullptr, internalSourceID };
}

String SamplingProfiler::StackFrame::url()
{
#if ENABLE(AOT)
    if (isCompiledFrame()) {
        AOT::Typed::ModuleID module = compiledModule();
        return module ? AOT::sourceURLOf(module) : emptyString();
    }
#endif
    switch (frameType) {
    case FrameType::Unknown:
    case FrameType::Host:
    case FrameType::RegExp:
    case FrameType::C:
    case FrameType::Wasm:
        return emptyString();
    case FrameType::Executable:
        if (executable->isHostFunction())
            return emptyString();

        String url = static_cast<ScriptExecutable*>(executable)->sourceURL();
        if (url.isEmpty())
            return static_cast<ScriptExecutable*>(executable)->source().provider()->sourceURLDirective(); // Fall back to sourceURL directive.
        return url;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return String();
}

Vector<SamplingProfiler::StackTrace> SamplingProfiler::releaseStackTraces()
{
    return releaseSamples().stackTraces;
}

auto SamplingProfiler::releaseSamples() -> Samples
{
    ASSERT(m_lock.isLocked());
    if (!m_isShutDown) { // The VM may be gone.
        HeapIterationScope heapIterationScope(m_vm.heap);
        processUnverifiedStackTraces();
    }

    Samples result { WTF::move(m_stackTraces), WTF::move(m_samplesWithoutFrames) };
    clearData();
    return result;
}

// What a report does with them allocates. That may finalize the owner of another profiler, which takes the lock to shut it down.
auto SamplingProfiler::stackTracesAndTimingInterval() -> std::pair<Vector<StackTrace>, Seconds>
{
    Locker locker { m_lock };
    {
        HeapIterationScope heapIterationScope(m_vm.heap);
        processUnverifiedStackTraces();
    }
    return { m_stackTraces, m_timingInterval };
}

static String descriptionForLocation(SamplingProfiler::StackFrame::CodeLocation location, std::optional<Wasm::CompilationMode> wasmCompilationMode, BytecodeIndex wasmOffset)
{
    if (wasmCompilationMode) {
        StringPrintStream description;
        description.print(":");
        description.print(wasmCompilationMode.value());
        description.print(":");
        if (wasmOffset) {
            uintptr_t offset = wasmOffset.offset();
            description.print(RawHex(offset));
        } else
            description.print("nil");
        return description.toString();
    }

    String bytecodeIndex;
    String codeBlockHash;
    String jitType;
    if (location.hasBytecodeIndex())
        bytecodeIndex = toString(location.bytecodeIndex);
    else
        bytecodeIndex = "<nil>"_s;

    if (location.hasCodeBlockHash()) {
        StringPrintStream stream;
        location.codeBlockHash.dump(stream);
        codeBlockHash = stream.toString();
    } else
        codeBlockHash = "<nil>"_s;

    if (location.isRegExp)
        jitType = "RegExp"_s;
    else
        jitType = JITCode::typeName(location.jitType);

    return makeString('#', codeBlockHash, ':', jitType, ':', bytecodeIndex);
}

struct Tiers {
    static constexpr ASCIILiteral llint { "LLInt"_s };
    static constexpr ASCIILiteral baseline { "Baseline"_s };
    static constexpr ASCIILiteral dfg { "DFG"_s };
    static constexpr ASCIILiteral ftl { "FTL"_s };
    static constexpr ASCIILiteral compiled { "AOT"_s };
    static constexpr ASCIILiteral builtin { "js builtin"_s };
    static constexpr ASCIILiteral ipint { "IPInt"_s };
    static constexpr ASCIILiteral bbq { "BBQ"_s };
    static constexpr ASCIILiteral omg { "OMG"_s };
    static constexpr ASCIILiteral wasm { "Wasm"_s };
    static constexpr ASCIILiteral host { "Host"_s };
    static constexpr ASCIILiteral regexp { "RegExp"_s };
    static constexpr ASCIILiteral cpp { "C/C++"_s };
    static constexpr ASCIILiteral unknownFrame { "Unknown Frame"_s };
    static constexpr ASCIILiteral unknownExecutable { "Unknown Executable"_s };
};

static String tierName(SamplingProfiler::StackFrame& frame)
{
    if (frame.isCompiledFrame())
        return Tiers::compiled;
    switch (frame.frameType) {
    case SamplingProfiler::FrameType::Executable:
        switch (frame.semanticLocation.jitType) {
        case JITType::HostCallThunk:
            return Tiers::host;
        case JITType::InterpreterThunk:
            return Tiers::llint;
        case JITType::BaselineJIT:
            return Tiers::baseline;
        case JITType::DFGJIT:
            return Tiers::dfg;
        case JITType::FTLJIT:
            return Tiers::ftl;
        default:
            return Tiers::unknownExecutable;
        }
        break;
    case SamplingProfiler::FrameType::Wasm:
        if (frame.wasmCompilationMode) {
            switch (frame.wasmCompilationMode.value()) {
            case Wasm::CompilationMode::IPIntMode:
                return Tiers::ipint;
            case Wasm::CompilationMode::JSToWasmMode:
            case Wasm::CompilationMode::JSToWasmICMode:
            case Wasm::CompilationMode::WasmToJSMode:
            case Wasm::CompilationMode::WasmBuiltinMode:
            case Wasm::CompilationMode::RestoreFrameMode:
                // Just say "Wasm" for now.
                break;
            case Wasm::CompilationMode::BBQMode:
                return Tiers::bbq;
            case Wasm::CompilationMode::OMGMode:
            case Wasm::CompilationMode::OMGForOSREntryMode:
                return Tiers::omg;
            }
        }
        return Tiers::wasm;
    case SamplingProfiler::FrameType::Host:
        return Tiers::host;
    case SamplingProfiler::FrameType::RegExp:
        return Tiers::regexp;
    case SamplingProfiler::FrameType::C:
        return Tiers::cpp;
    case SamplingProfiler::FrameType::Unknown:
        return Tiers::unknownFrame;
    }
    return Tiers::unknownFrame;
}

Ref<JSON::Value> SamplingProfiler::stackTracesAsJSON()
{
    DeferGC deferGC(m_vm);
    auto [stackTraces, timingInterval] = stackTracesAndTimingInterval();

    UncheckedKeyHashMap<SourceID, Ref<SourceProvider>> sources;

    auto stackFrameAsJSON = [&](StackFrame& stackFrame) {
        auto [provider, sourceID] = stackFrame.sourceProviderAndID();
        if (provider)
            sources.add(sourceID, Ref { *provider });

        auto result = JSON::Object::create();
        result->setDouble("sourceID"_s, sourceID);
        result->setString("name"_s, stackFrame.displayName(m_vm));
        result->setString("location"_s, descriptionForLocation(stackFrame.semanticLocation, stackFrame.wasmCompilationMode, stackFrame.wasmOffset));
        LineColumn sourceMappedLineColumn = stackFrame.semanticLocation.lineColumn;
        if (provider) {
#if USE(BUN_JSC_ADDITIONS)
            auto& fn = m_vm.computeLineColumnWithSourcemap();
            if (fn) {
                String remappedURL;
                fn(m_vm, provider, sourceMappedLineColumn, remappedURL);
                if (!remappedURL.isEmpty())
                    result->setString("sourceURL"_s, remappedURL);
            }
#endif
        }
        if (stackFrame.isCompiledFrame())
            result->setString("sourceURL"_s, stackFrame.url());
        result->setDouble("line"_s, sourceMappedLineColumn.line);
        result->setDouble("column"_s, sourceMappedLineColumn.column);
        result->setString("category"_s, tierName(stackFrame));
        uint32_t flags = 0;
        if (stackFrame.frameType == SamplingProfiler::FrameType::Executable && stackFrame.executable) {
            if (auto* executable = dynamicDowncast<FunctionExecutable>(stackFrame.executable); executable && executable->isBuiltinFunction())
                flags = 1;
        }
        result->setDouble("flags"_s, flags);

        if (std::optional<std::pair<StackFrame::CodeLocation, CodeBlock*>> machineLocation = stackFrame.machineLocation) {
            auto inliner = JSON::Object::create();
            inliner->setString("name"_s, String::fromUTF8(machineLocation->second->inferredName().span()));
            inliner->setString("location"_s, descriptionForLocation(machineLocation->first, std::nullopt, BytecodeIndex()));
            LineColumn sourceMappedLineColumn = machineLocation->first.lineColumn;
            if (provider) {
#if USE(BUN_JSC_ADDITIONS)
                auto& fn = m_vm.computeLineColumnWithSourcemap();
                if (fn) {
                    String inlinerRemappedURL;
                    fn(m_vm, provider, sourceMappedLineColumn, inlinerRemappedURL);
                }
#endif
            }
            inliner->setDouble("line"_s, sourceMappedLineColumn.line);
            inliner->setDouble("column"_s, sourceMappedLineColumn.column);
            inliner->setString("category"_s, tierName(stackFrame));
            result->setValue("inliner"_s, WTF::move(inliner));
        }
        return result;
    };

    auto stackTraceAsJSON = [&](StackTrace& stackTrace) {
        auto result = JSON::Object::create();
        result->setDouble("timestamp"_s, stackTrace.timestamp.secondsSinceEpoch().seconds());
        {
            auto frames = JSON::Array::create();
            for (StackFrame& stackFrame : stackTrace.frames)
                frames->pushValue(stackFrameAsJSON(stackFrame));
            result->setValue("frames"_s, WTF::move(frames));
        }
        return result;
    };

    auto sourceAsJSON = [&](SourceProvider& provider) {
        auto result = JSON::Object::create();
        result->setDouble("sourceID"_s, provider.asID());
        if (!provider.sourceURL().isEmpty())
            result->setString("url"_s, provider.sourceURL());
        if (!provider.sourceURLDirective().isEmpty())
            result->setString("sourceURL"_s, provider.sourceURLDirective());
        if (!provider.sourceMappingURLDirective().isEmpty())
            result->setString("sourceMappingURL"_s, provider.sourceMappingURLDirective());
        return result;
    };

    auto result = JSON::Object::create();
    result->setDouble("interval"_s, timingInterval.seconds());
    auto traces = JSON::Array::create();
    for (StackTrace& stackTrace : stackTraces)
        traces->pushValue(stackTraceAsJSON(stackTrace));
    result->setValue("traces"_s, WTF::move(traces));

    auto sourcesArray = JSON::Array::create();
    for (auto& [ sourceID, sourceProvider ] : sources)
        sourcesArray->pushValue(sourceAsJSON(sourceProvider.get()));
    result->setValue("sources"_s, WTF::move(sourcesArray));

    Locker locker { m_lock };
    clearData(); // What has been sampled in the meantime is this function.

    return result;
}

void SamplingProfiler::registerForReportAtExit()
{
    static Lock registrationLock;
    static UncheckedKeyHashSet<RefPtr<SamplingProfiler>>* profilesToReport;

    Locker locker { registrationLock };

    if (!profilesToReport) {
        profilesToReport = new UncheckedKeyHashSet<RefPtr<SamplingProfiler>>();
        atexit([]() {
            for (const auto& profile : *profilesToReport)
                profile->reportDataToOptionFile();
        });
    }

    profilesToReport->add(adoptRef(this));
    m_needsReportAtExit = true;
}

void SamplingProfiler::reportDataToOptionFile()
{
    if (m_needsReportAtExit) {
        m_needsReportAtExit = false;
        JSLockHolder holder(m_vm);
        const char8_t* path = Options::samplingProfilerPath();
        StringPrintStream pathOut;
        pathOut.print(path, "/");
        pathOut.print("JSCSampilingProfile-", reinterpret_cast<uintptr_t>(this), ".txt");
        auto out = FilePrintStream::open(pathOut.toUTF8CString(), "w"_s);
        reportTopFunctions(*out);
        reportTopBytecodes(*out);
    }
}

void SamplingProfiler::reportTopFunctions()
{
    reportTopFunctions(WTF::dataFile());
}

void SamplingProfiler::reportTopFunctions(PrintStream& out)
{
    DeferGCForAWhile deferGC(m_vm);
    auto [stackTraces, timingInterval] = stackTracesAndTimingInterval();

    size_t totalSamples = 0;
    UncheckedKeyHashMap<String, size_t> functionCounts;
    for (StackTrace& stackTrace : stackTraces) {
        if (!stackTrace.frames.size())
            continue;

        StackFrame& frame = stackTrace.frames.front();
        String hash = ""_s;
        if (frame.semanticLocation.hasCodeBlockHash()) {
            StringPrintStream stream;
            frame.semanticLocation.codeBlockHash.dump(stream);
            hash = stream.toString();
        } else
            hash = "<nil>"_s;
        SourceID sourceID = std::get<1>(frame.sourceProviderAndID());
        if (Options::samplingProfilerIgnoreExternalSourceID()) {
            if (sourceID != internalSourceID)
                sourceID = aggregatedExternalSourceID;
        }
        auto frameDescription = makeString(frame.displayName(m_vm), '#', hash, ':', sourceID);
        functionCounts.add(frameDescription, 0).iterator->value++;
        totalSamples++;
    }

    auto takeMax = [&] () -> std::pair<String, size_t> {
        String maxFrameDescription;
        size_t maxFrameCount = 0;
        for (const auto& entry : functionCounts) {
            if (entry.value > maxFrameCount) {
                maxFrameCount = entry.value;
                maxFrameDescription = entry.key;
            }
        }
        if (!maxFrameDescription.isEmpty())
            functionCounts.remove(maxFrameDescription);
        return std::make_pair(maxFrameDescription, maxFrameCount);
    };

    if (Options::samplingProfilerTopFunctionsCount()) {
        out.println("\n\nSampling rate: ", timingInterval.microseconds(), " microseconds. Total samples: ", totalSamples);
        out.println("Top functions as <numSamples  'functionName#hash:sourceID'>");
        for (size_t i = 0; i < Options::samplingProfilerTopFunctionsCount(); i++) {
            auto pair = takeMax();
            if (pair.first.isEmpty())
                break;
            out.printf("%6zu ", pair.second);
            out.println("   '", pair.first, "'");
        }
    }
}

void SamplingProfiler::reportTopBytecodes()
{
    reportTopBytecodes(WTF::dataFile());
}

void SamplingProfiler::reportTopBytecodes(PrintStream& out)
{
    DeferGCForAWhile deferGC(m_vm);
    auto [stackTraces, timingInterval] = stackTracesAndTimingInterval();

    size_t totalSamples = 0;
    UncheckedKeyHashMap<String, size_t> bytecodeCounts;
    UncheckedKeyHashMap<String, size_t> tierCounts;

    auto forEachTier = [&] (auto func) {
        func(Tiers::llint);
        func(Tiers::baseline);
        func(Tiers::dfg);
        func(Tiers::ftl);
        if (m_vm.hasCompiledCode())
            func(Tiers::compiled);
        func(Tiers::builtin);
        func(Tiers::ipint);
        func(Tiers::bbq);
        func(Tiers::omg);
        func(Tiers::wasm);
        func(Tiers::host);
        func(Tiers::regexp);
        func(Tiers::cpp);
        func(Tiers::unknownFrame);
        func(Tiers::unknownExecutable);
    };

    for (StackTrace& stackTrace : stackTraces) {
        if (!stackTrace.frames.size())
            continue;

        StackFrame& frame = stackTrace.frames.front();
        auto frameDescription = makeString(frame.displayName(m_vm), descriptionForLocation(frame.semanticLocation, frame.wasmCompilationMode, frame.wasmOffset));
        if (std::optional<std::pair<StackFrame::CodeLocation, CodeBlock*>> machineLocation = frame.machineLocation) {
            frameDescription = makeString(frameDescription, " <-- "_s,
                machineLocation->second->inferredName(), descriptionForLocation(machineLocation->first, std::nullopt, BytecodeIndex()));
        }
        bytecodeCounts.add(frameDescription, 0).iterator->value++;

        tierCounts.add(tierName(frame), 0).iterator->value++;
        if (frame.frameType == SamplingProfiler::FrameType::Executable && frame.executable) {
            if (auto* executable = dynamicDowncast<FunctionExecutable>(frame.executable)) {
                if (executable->isBuiltinFunction())
                    tierCounts.add(Tiers::builtin, 0).iterator->value++;
            }
        }

        totalSamples++;
    }

    auto takeMax = [&] () -> std::pair<String, size_t> {
        String maxFrameDescription;
        size_t maxFrameCount = 0;
        for (const auto& entry : bytecodeCounts) {
            if (entry.value > maxFrameCount) {
                maxFrameCount = entry.value;
                maxFrameDescription = entry.key;
            }
        }
        if (!maxFrameDescription.isEmpty())
            bytecodeCounts.remove(maxFrameDescription);
        return std::make_pair(maxFrameDescription, maxFrameCount);
    };

    if (Options::samplingProfilerTopBytecodesCount()) {
        out.println("\n\nSampling rate: ", timingInterval.microseconds(), " microseconds. Total samples: ", totalSamples, "\n");

        out.println("Tier breakdown:");
        out.println("-----------------------------------");
        unsigned maxTierNameLength = 0;
        forEachTier([&] (String tier) {
            maxTierNameLength = std::max(maxTierNameLength, tier.length());
        });
        auto printTier = [&] (String tier) {
            size_t count = tierCounts.get(tier);
            if (!count && (tier == Tiers::unknownFrame || tier == Tiers::unknownExecutable))
                return;
            out.print(tier, ": ");
            for (unsigned i = 0; i < maxTierNameLength + 2 - tier.length(); ++i)
                out.print(" ");
            out.printf("%6zu ", count);
            out.println(" (", (static_cast<double>(count) / static_cast<double>(totalSamples)) * 100, "%)");
        };
        forEachTier(printTier);
        out.println("\n");

        out.println("Hottest bytecodes as <numSamples   'functionName#hash:JITType:bytecodeIndex'>");
        for (size_t i = 0; i < Options::samplingProfilerTopBytecodesCount(); i++) {
            auto pair = takeMax();
            if (pair.first.isEmpty())
                break;
            out.printf("%6zu ", pair.second);
            out.println("   '", pair.first, "'");
        }
    }
}

Thread* SamplingProfiler::thread() const
{
    return m_sampler->thread();
}

} // namespace JSC

namespace WTF {

using namespace JSC;

void printInternal(PrintStream& out, SamplingProfiler::FrameType frameType)
{
    switch (frameType) {
    case SamplingProfiler::FrameType::Executable:
        out.print("Executable");
        break;
    case SamplingProfiler::FrameType::Wasm:
        out.print("Wasm");
        break;
    case SamplingProfiler::FrameType::Host:
        out.print("Host");
        break;
    case SamplingProfiler::FrameType::RegExp:
        out.print("RegExp");
        break;
    case SamplingProfiler::FrameType::C:
    case SamplingProfiler::FrameType::Unknown:
        out.print("Unknown");
        break;
    }
}

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(SAMPLING_PROFILER)
