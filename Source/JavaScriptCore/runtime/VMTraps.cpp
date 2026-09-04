/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
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
#include "VMTraps.h"

#include "CallFrameInlines.h"
#include "CodeBlock.h"
#include "CodeBlockSet.h"
#include "DFGCommonData.h"
#include "ExceptionHelpers.h"
#include "HeapInlines.h"
#include "JSCJSValueInlines.h"
#include "JSThreadsSafepoint.h"
#include "LLIntPCRanges.h"
#include "MachineContext.h"
#include "MacroAssemblerCodeRef.h"
#include "ThreadManager.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include "VMLite.h"
#include "VMLiteShared.h"
#include "VMManager.h"
#include "VMTrapsInlines.h"
#include "WaiterListManager.h"
#include "Watchdog.h"
#include <wtf/Condition.h>
#include <wtf/ProcessID.h>
#include <wtf/Scope.h>
#include <wtf/Vector.h>
#include <wtf/ThreadMessage.h>
#include <wtf/threads/Signals.h>

namespace JSC {

// UNGIL TERM1.2 interim (single shared trap word; see perThreadTrapsIfExists
// in VMTraps.h): "does any OTHER thread of this VM currently have a live
// entry scope?" — the key for whether a serviced VM-wide termination must be
// left visible in the shared word. §F.1 keeps main/embedder carriers mutually
// excluded GIL-off, so any OTHER entered lite observed by an entered servicer
// is either a spawned Thread or a §J.3-parked carrier — both poll the shared
// word (D9 quanta / park predicates) and both are owed the bit (TERM1.2:
// terminating the VM terminates EVERY entered thread).
// GIL-off: does a thread of this VM other than `except` have a targeted
// termination (VMTraps::fireTargetedTermination) that it has not serviced yet?
// Its NeedTermination bit is still in its own word then, and the VM word must
// keep the bit for its call-free loops. Caller holds VMLiteRegistry::lock.
static bool anyOtherUnservicedTargetedTermination(const AbstractLocker&, VM& vm, VMLite* except) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    for (VMLite* lite : VMLiteRegistry::singleton().lites) {
        if (lite->vm != &vm || lite == except || !lite->targetedTerminationRequested.load(std::memory_order_relaxed))
            continue;
        VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
        if (liteTraps && liteTraps != &vm.traps() && liteTraps->needHandling(VMTraps::NeedTermination))
            return true;
    }
    return false;
}

static bool anyOtherLiteOfVMEntered(const AbstractLocker&, VM& vm) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    VMLite* currentLite = VMLite::currentIfExists();
    for (VMLite* lite : VMLiteRegistry::singleton().lites) {
        if (lite->vm == &vm && lite != currentLite && lite->entryScope.load(std::memory_order_relaxed))
            return true;
    }
    return false;
}

static bool anyOtherLiteOfVMEntered(VM& vm)
{
    assertNoPerLiteTrapSignalingLockHeldOnCurrentThread(); // §A.2.2 item 3c (h): registry lock is no longer leaf-ranked.
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock }; // Nothing acquired under it on THIS path (the 3c fan re-rank applies to the per-lite update walks).
    return anyOtherLiteOfVMEntered(locker, vm);
}

#if ASSERT_ENABLED
// §A.2.2 item 3c finding (h): the registry lock is demoted from leaf rank by
// the stop fan (VMLiteRegistry::lock -> per-lite m_trapSignalingLock ->
// per-lite StackManager::m_mirrorLock). This counter is nonzero exactly while
// the current thread holds a PER-LITE m_trapSignalingLock; every registry-
// lock acquisition in the trap machinery asserts it is zero. COVERAGE
// (finding-(h) follow-up): every m_trapSignalingLock acquisition in this
// file that can run on a per-lite instance constructs a
// PerLiteTrapSignalingLockDepthScope, so the counter is a true "any per-lite
// signaling lock held" predicate — not one limited to
// updateThreadStopRequestIfNeeded. The two acquisitions NOT scoped are
// per-lite-unreachable by construction: willDestroyVM's SignalSender drain
// and the SignalSender machinery itself exist only when usePollingTraps is
// off, and useJSThreads forces usePollingTraps=1 (SPEC-jit M2b), so a
// per-lite instance (gilOff only) never has a SignalSender — and the
// condition wait there releases the lock, which would falsify a naive scope.
static thread_local unsigned t_perLiteTrapSignalingLockDepth { 0 };
void assertNoPerLiteTrapSignalingLockHeldOnCurrentThread()
{
    ASSERT(!t_perLiteTrapSignalingLockDepth);
}
#endif

// RAII bump of t_perLiteTrapSignalingLockDepth (no-op on the VM-embedded
// instance, where liteOwnerVM is null, and in release builds). Construct
// immediately after taking m_trapSignalingLock; pass m_liteOwnerVM.
class PerLiteTrapSignalingLockDepthScope {
public:
#if ASSERT_ENABLED
    explicit PerLiteTrapSignalingLockDepthScope(VM* liteOwnerVM)
        : m_isPerLite(!!liteOwnerVM)
    {
        if (m_isPerLite)
            ++t_perLiteTrapSignalingLockDepth;
    }
    ~PerLiteTrapSignalingLockDepthScope()
    {
        if (m_isPerLite)
            --t_perLiteTrapSignalingLockDepth;
    }
private:
    bool m_isPerLite;
#else
    explicit PerLiteTrapSignalingLockDepthScope(VM*) { }
#endif
};

#if ENABLE(SIGNAL_BASED_VM_TRAPS)

struct VMTraps::SignalContext {
private:
    SignalContext(PlatformRegisters& registers, CodePtr<PlatformRegistersPCPtrTag> trapPC)
        : registers(registers)
        , trapPC(trapPC)
        , stackPointer(MachineContext::stackPointer(registers))
        , framePointer(MachineContext::framePointer(registers))
    { }

public:
    static std::optional<SignalContext> NODELETE tryCreate(PlatformRegisters& registers)
    {
        auto instructionPointer = MachineContext::instructionPointer(registers);
        if (!instructionPointer)
            return std::nullopt;
        return SignalContext(registers, *instructionPointer);
    }

    PlatformRegisters& registers;
    CodePtr<PlatformRegistersPCPtrTag> trapPC;
    void* stackPointer;
    void* framePointer;
};

inline static bool NODELETE vmIsInactive(VM& vm)
{
    // UNGIL §A.2.5: GIL-off "inactive" means no registered lite of this VM is
    // entered (per-lite entry records; the VM-member entryScope/ownerThread
    // pair is the GIL-on protocol). Only consulted on the signal-delivery
    // path, which is never started GIL-off, but the predicate is re-pointed
    // per the annex so any future consumer inherits the right meaning.
    if (vm.gilOff()) [[unlikely]]
        return !vm.isAnyThreadEntered();
    return !vm.entryScope && !vm.ownerThread();
}

static bool NODELETE isSaneFrame(CallFrame* frame, CallFrame* calleeFrame, EntryFrame* entryFrame, StackBounds stackBounds)
{
    if (reinterpret_cast<void*>(frame) >= reinterpret_cast<void*>(entryFrame))
        return false;
    if (calleeFrame >= frame)
        return false;
    return stackBounds.contains(frame);
}

void VMTraps::tryInstallTrapBreakpoints(VMTraps::SignalContext& context, StackBounds stackBounds)
{
    // This must be the initial signal to get the mutator thread's attention.
    // Let's get the thread to break at invalidation points if needed.
    VM& vm = this->vm();
    void* trapPC = context.trapPC.untaggedPtr();
    // We must ensure we're in JIT/LLint code. If we are, we know a few things:
    // - The JS thread isn't holding the malloc lock. Therefore, it's safe to malloc below.
    // - The JS thread isn't holding the CodeBlockSet lock.
    // If we're not in JIT/LLInt code, we can't run the C++ code below because it
    // mallocs, and we must prove the JS thread isn't holding the malloc lock
    // to be able to do that without risking a deadlock.
    if (!isJITPC(trapPC) && !LLInt::isLLIntPC(trapPC))
        return;

    CallFrame* callFrame = reinterpret_cast<CallFrame*>(context.framePointer);

    // Even though we know the mutator thread is not in C++ code and therefore, not holding
    // this lock, the sampling profiler may have acquired this lock before acquiring
    // ThreadSuspendLocker and suspending the mutator. Since VMTraps acquires the
    // ThreadSuspendLocker first, we can deadlock with the Sampling Profiler thread, and
    // leave the mutator in a suspended state, or forever blocked on the codeBlockSet lock.
    Lock& codeBlockSetLock = vm.heap.codeBlockSet().getLock();
    if (!codeBlockSetLock.tryLock())
        return;

    Locker codeBlockSetLocker { AdoptLock, codeBlockSetLock };

    CodeBlock* foundCodeBlock = nullptr;
    // UNGIL §A.1.3 mode split (U-T4): GIL-off the raw VM word is inert spare
    // storage. GIL-on group3Primitives() aliases the VM block via
    // mainVMLitePrimitives(), so this is behavior-identical today; GIL-off
    // this path is unreachable anyway (§A.2.5: SignalSender never started),
    // and the reroute keeps the no-raw-reader invariant the U-T8d audit
    // tripwires on.
    EntryFrame* entryFrame = vm.group3Primitives().topEntryFrame;

    // We don't have a callee to start with. So, use the end of the stack to keep the
    // isSaneFrame() checker below happy for the first iteration. It will still check
    // to ensure that the address is in the stackBounds.
    CallFrame* calleeFrame = reinterpret_cast<CallFrame*>(stackBounds.end());

    if (!entryFrame || !callFrame)
        return; // Not running JS code. Let the SignalSender try again later.

    do {
        if (!isSaneFrame(callFrame, calleeFrame, entryFrame, stackBounds))
            return; // Let the SignalSender try again later.

        CodeBlock* candidateCodeBlock = callFrame->unsafeCodeBlock();
        if (candidateCodeBlock && vm.heap.codeBlockSet().contains(codeBlockSetLocker, candidateCodeBlock)) {
            foundCodeBlock = candidateCodeBlock;
            break;
        }

        calleeFrame = callFrame;
        callFrame = callFrame->callerFrame(entryFrame);

    } while (callFrame && entryFrame);

    if (!foundCodeBlock) {
        // We may have just entered the frame and the codeBlock pointer is not
        // initialized yet. Just bail and let the SignalSender try again later.
        return;
    }

    if (foundCodeBlock->canInstallVMTrapBreakpoints()) {
        if (!m_trapSignalingLock->tryLock())
            return; // Let the SignalSender try again later.

        Locker locker { AdoptLock, *m_trapSignalingLock };
        PerLiteTrapSignalingLockDepthScope signalingDepthScope { m_liteOwnerVM }; // Finding (h): keep the depth counter a true predicate.
        if (!needHandling(VMTraps::AsyncEvents)) {
            // Too late. Someone else already handled the trap.
            return;
        }

        if (!foundCodeBlock->hasInstalledVMTrapsBreakpoints()) {
            // T6 (gilOff handleTraps sweep early-out): record the install on
            // the (possibly process-shared) set BEFORE the breakpoints become
            // observable, under the set's lock (codeBlockSetLocker above is
            // still held here), so a sweeper that skipped on `false` provably
            // had nothing to jettison. Sticky; see CodeBlockSet.h.
            vm.heap.codeBlockSet().noteCodeBlockMayHaveInstalledVMTrapBreakpoints(codeBlockSetLocker);
            foundCodeBlock->installVMTrapBreakpoints();
        }
        return;
    }
}

void VMTraps::invalidateCodeBlocksOnStack()
{
    invalidateCodeBlocksOnStack(vm().group3Primitives().topCallFrame); // UNGIL §A.1.3 mode split.
}

void VMTraps::invalidateCodeBlocksOnStack(CallFrame* topCallFrame)
{
    // T6 (gilOff scalability): check the one-shot gate BEFORE taking the
    // process-shared CodeBlockSet lock. m_needToInvalidateCodeBlocks is
    // written under m_trapSignalingLock (requestThreadStopIfNeeded) and was
    // never synchronized by the codeBlockSet lock — its writer does not hold
    // that lock — so this pre-lock read races with a concurrent set exactly
    // as much as the in-lock read did: a raise that lands after this read is
    // delivered by its own trap bits at the next service, unchanged from the
    // pre-change interleaving. GIL-on / flag-off: branch not taken,
    // locked shape byte-identical.
    if (vm().gilOff()) [[unlikely]] {
        if (!m_needToInvalidateCodeBlocks)
            return;
    }
    Locker codeBlockSetLocker { vm().heap.codeBlockSet().getLock() };
    invalidateCodeBlocksOnStack(codeBlockSetLocker, topCallFrame);
}
    
void VMTraps::invalidateCodeBlocksOnStack(Locker<Lock>&, CallFrame* topCallFrame)
{
    if (!m_needToInvalidateCodeBlocks)
        return;

    m_needToInvalidateCodeBlocks = false;

    // UNGIL §A.1.3 mode split (U-T4): trap handling runs on the mutator
    // thread, so group3Primitives() resolves the CURRENT lite's live word
    // GIL-off and aliases the VM block GIL-on.
    EntryFrame* entryFrame = vm().group3Primitives().topEntryFrame;
    CallFrame* callFrame = topCallFrame;

    if (!entryFrame)
        return; // Not running JS code. Nothing to invalidate.

    while (callFrame) {
        CodeBlock* codeBlock = callFrame->isNativeCalleeFrame() ? nullptr : callFrame->codeBlock();
        if (codeBlock && JSC::JITCode::isOptimizingJIT(codeBlock->jitType()))
            codeBlock->jettison(Profiler::JettisonDueToVMTraps);
        callFrame = callFrame->callerFrame(entryFrame);
    }
}

class VMTraps::SignalSender final : public ThreadSafeRefCounted<VMTraps::SignalSender> {
public:
    SignalSender(const AbstractLocker&, VM& vm)
        : m_vm(vm)
        , m_lock(vm.traps().m_trapSignalingLock)
        , m_condition(vm.traps().m_condition)
    {
        activateSignalHandlersFor(Signal::AccessFault);
    }

    static void initializeSignals()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            addSignalHandler(Signal::AccessFault, [] (Signal signal, SigInfo&, PlatformRegisters& registers) -> SignalAction {
                RELEASE_ASSERT(signal == Signal::AccessFault);
                auto signalContext = SignalContext::tryCreate(registers);
                if (!signalContext)
                    return SignalAction::NotHandled;

                void* trapPC = signalContext->trapPC.untaggedPtr();
                if (!isJITPC(trapPC))
                    return SignalAction::NotHandled;

                CodeBlock* currentCodeBlock = DFG::codeBlockForVMTrapPC(trapPC);
                if (!currentCodeBlock) {
                    // Either we trapped for some other reason, e.g. Wasm OOB, or we didn't properly monitor the PC. Regardless, we can't do much now...
                    return SignalAction::NotHandled;
                }
                ASSERT(currentCodeBlock->hasInstalledVMTrapsBreakpoints());
                VM& vm = currentCodeBlock->vm();

                // This signal handler is triggered by the mutator thread due to the installed halt instructions
                // in JIT code (which we already confirmed above). Hence, the current thread (the mutator)
                // cannot be in C++ code, and therefore, cannot be already holding the codeBlockSet lock.
                // The only time the codeBlockSet lock could be in contention is if the Sampling Profiler thread
                // is holding it. In that case, we'll simply wait till the Sampling Profiler is done with it.
                // There are no lock ordering issues w.r.t. the Sampling Profiler on this code path.
                //
                // Note that it is not ok to return SignalAction::NotHandled here if we see contention. Doing
                // so will cause the fault to be handled by the default handler, which will crash. It is also not
                // productive to return SignalAction::Handled on contention. Doing so will simply trigger this
                // fault handler over and over again. We might as well wait for the Sampling Profiler to release
                // the lock, which is what we do here.
                Locker codeBlockSetLocker { vm.heap.codeBlockSet().getLock() };

                bool sawCurrentCodeBlock = false;
                vm.heap.forEachCodeBlockIgnoringJITPlans(codeBlockSetLocker, [&] (CodeBlock* codeBlock) {
                    // We want to jettison all code blocks that have vm traps breakpoints, otherwise we could hit them later.
                    if (codeBlock->hasInstalledVMTrapsBreakpoints()) {
                        if (currentCodeBlock == codeBlock)
                            sawCurrentCodeBlock = true;

                        codeBlock->jettison(Profiler::JettisonDueToVMTraps);
                    }
                });
                RELEASE_ASSERT(sawCurrentCodeBlock);
                
                return SignalAction::Handled; // We've successfully jettisoned the codeBlocks.
            });
        });
    }

    VMTraps& NODELETE traps() { return m_vm.traps(); }


    void notify(AbstractLocker&)
    {
        if (m_scheduled)
            return;
        m_scheduled = true;
        VMTraps::queue().dispatch([protectedThis = Ref { *this }] {
            protectedThis->work();
        });
    }

    bool NODELETE isStopped(AbstractLocker&)
    {
        return !m_scheduled;
    }

private:
    void work()
    {
        VM& vm = m_vm;

        auto workDone = [&](AbstractLocker&) {
            m_scheduled = false;
            m_condition->notifyAll(); // let work queue service next SignalSender if needed.
        };

        {
            Locker locker { *m_lock };
            ASSERT(m_scheduled);
            if (traps().m_isShuttingDown)
                return workDone(locker);

            if (!traps().needHandling(VMTraps::AsyncEvents))
                return workDone(locker);

            // We know that no trap could have been processed and re-added because we are holding the lock.
            if (vmIsInactive(m_vm))
                return workDone(locker);
        }

        auto optionalOwnerThread = vm.ownerThread();
        if (optionalOwnerThread) {
            auto expectedUID = optionalOwnerThread.value()->uid();
            ThreadSuspendLocker locker;
            sendMessage(locker, *optionalOwnerThread.value().get(), [&] (PlatformRegisters& registers) -> void {
                auto signalContext = SignalContext::tryCreate(registers);
                if (!signalContext)
                    return;

                // We can't mess with a thread unless it's the one we suspended.
                // Use ownerThreadUID() instead of ownerThread() to avoid creating a temporary
                // RefPtr<Thread> copy, which would acquire the Thread control block WordLock.
                // If the suspended thread was frozen mid-unlock of that same WordLock,
                // calling ownerThread() here would deadlock.
                auto currentUID = vm.ownerThreadUID();
                if (!currentUID || *currentUID != expectedUID)
                    return;

                Thread& thread = *optionalOwnerThread->get();
                vm.traps().tryInstallTrapBreakpoints(*signalContext, thread.stack());
            });
        }

        if (vm.traps().hasTrapBit(NeedTermination))
            vm.syncWaiter()->notifyOfTerminationRequest();

        {
            Locker locker { *m_lock };
            ASSERT(m_scheduled);
            if (traps().m_isShuttingDown)
                return workDone(locker);
            ASSERT(m_scheduled);
        }

        VMTraps::queue().dispatchAfter(1_ms, [protectedThis = Ref { *this }] {
            protectedThis->work();
        });
    }

    VM& m_vm;
    Box<Lock> m_lock;
    Box<Condition> m_condition;
    bool m_scheduled { false };
};

#endif // ENABLE(SIGNAL_BASED_VM_TRAPS)

void VMTraps::jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite()
{
    // checktraps-dejank-invalidation-point: same walk shape as
    // invalidateCodeBlocksOnStack above, WITHOUT the one-shot
    // m_needToInvalidateCodeBlocks gate — under N mutators every thread whose
    // park overlapped a conductor heap-fact rewrite must jettison its OWN
    // on-stack optimizing-JIT code (jettison fires this code's CheckTraps
    // invalidation points; the §A.3 / GIL-off poll lowering emits one at
    // every poll rejoin, see DFGClobberize.h CheckTraps). Runs on the parked
    // mutator itself after resume, so vm()'s lite-aware resolution and
    // group3Primitives() resolve THIS thread's state (§A.1.3 mode split),
    // exactly as in the NeedDebuggerBreak service path. Both frame words
    // come from the same per-thread primitives: GIL-off the raw VM-block
    // topCallFrame is never written (every writer stores through the current
    // lite), so a walk started from it would visit nothing.
    // CodeBlock::jettison re-enters stopTheWorldAndRun internally (section
    // 5.3 choke point); that nested window is a pure code-lifecycle window
    // (suppressed from the epoch), so this never cascades.
    VM& vm = this->vm();
    VMLitePrimitives& primitives = vm.group3Primitives();
    EntryFrame* entryFrame = primitives.topEntryFrame;
    if (!entryFrame)
        return; // Not running JS code. Nothing to jettison.

    // Amend round (review major fix): COLLECT under the codeBlockSet lock,
    // jettison AFTER dropping it. The legacy invalidateCodeBlocksOnStack
    // shape (jettison while holding the lock) predates jettison-as-stop-
    // window: GIL-off, CodeBlock::jettison re-enters
    // JSThreadsSafepoint::stopTheWorldAndRun, and conducting/queueing a §A.3
    // window while holding this process-shared lock deadlocks against any
    // sibling that must take the same lock on its way TO its park (the
    // handleTraps breakpoint-sweep walk acquires it pre-park) — a thread
    // blocked on a WTF::Lock is not at a stop safepoint, so the nested
    // window's predicate can never converge (section 5.3 lock/stop-progress
    // rules). This path fires on every epoch-overlapped park, potentially on
    // N threads at once, so the inherited shape is not tolerable here the
    // way it is on the rare single-thread debugger path. Dropping the lock
    // is safe: every collected CodeBlock is on THIS thread's own stack, so
    // it is conservatively scanned and cannot be freed; jettison re-checks
    // its own state internally and tolerates an intervening jettison from
    // another path. Deduplicate so a block appearing in multiple frames
    // opens at most one nested stop window.
    // T6 (gilOff scalability): collect WITHOUT the process-shared
    // codeBlockSet lock when this VM is gilOff. The collection below reads
    // only THIS thread's own frames — every collected CodeBlock is on this
    // thread's stack, hence conservatively scanned and unfreeable mid-walk —
    // and never consults CodeBlockSet state. The lock in the legacy
    // invalidateCodeBlocksOnStack shape (which this walk inherited) mutually
    // excludes the SignalSender's CROSS-THREAD stack walk
    // (tryInstallTrapBreakpoints reads this stack's frames under the same
    // lock while the mutator is suspended) — and §A.2.5 never starts a
    // SignalSender for a gilOff VM, so GIL-off there is nothing to exclude.
    // This path fires on up to N threads per overlapped rewrite window;
    // serializing all of them on the one shared lock was part of the
    // measured 8% slow-acquisition share. Non-gilOff callers (none exist
    // today — both call sites are gated on useJSThreads && !useThreadGIL —
    // but keep the conservative shape) still take the lock.
    Vector<CodeBlock*, 8> codeBlocksToJettison;
    {
        std::optional<Locker<Lock>> codeBlockSetLocker;
        if (!vm.gilOff()) [[unlikely]]
            codeBlockSetLocker.emplace(vm.heap.codeBlockSet().getLock());
        CallFrame* callFrame = primitives.topCallFrame;
        while (callFrame) {
            CodeBlock* codeBlock = callFrame->isNativeCalleeFrame() ? nullptr : callFrame->codeBlock();
            if (codeBlock && JSC::JITCode::isOptimizingJIT(codeBlock->jitType())) {
                if (!codeBlocksToJettison.contains(codeBlock))
                    codeBlocksToJettison.append(codeBlock);
            }
            callFrame = callFrame->callerFrame(entryFrame);
        }
    }
    for (CodeBlock* codeBlock : codeBlocksToJettison)
        codeBlock->jettison(Profiler::JettisonDueToVMTraps);
}

WorkQueue& VMTraps::queue()
{
    static LazyNeverDestroyed<Ref<WorkQueue>> workQueue;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        workQueue.construct(WorkQueue::create("JSC VMTraps Signal Sender"_s));
    });
    return workQueue.get();
}

void VMTraps::initializeSignals()
{
#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    if (!Options::usePollingTraps()) {
        ASSERT(Options::useJIT());
        SignalSender::initializeSignals();
    }
#endif
}

void VMTraps::willDestroyVM()
{
    m_isShuttingDown = true;
#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    if (m_signalSender) {
        {
            Locker locker { *m_trapSignalingLock };
            while (!m_signalSender->isStopped(locker))
                m_condition->wait(*m_trapSignalingLock);
        }
        m_signalSender = nullptr;
    }
#endif
}

CONCURRENT_SAFE void VMTraps::cancelThreadStopIfNeeded()
{
    ASSERT(m_threadStopRequested);

    m_stack.cancelStop();
    m_threadStopRequested = false;
}

CONCURRENT_SAFE void VMTraps::requestThreadStopIfNeeded(Locker<Lock>& locker)
{
    ASSERT(!m_threadStopRequested);
    ASSERT(!m_isShuttingDown);

    VM& vm = liteAwareVM(); // §A.2.2 item 3c: callable on a per-lite instance (see VMTraps.h).
    m_stack.requestStop();

    m_needToInvalidateCodeBlocks = true;

#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    // UNGIL §A.2.5: async (signal) delivery is OFF GIL-off. The SignalSender
    // is never started for a gilOff VM: there is no single "ownerThread" to
    // suspend, and trap-breakpoint installation assumes one mutator stack.
    // Delivery GIL-off = the rule-3 bit fan-out (fireTrapVMWide) + the
    // existing poll sites + the D9 park quanta. GIL-on/flag-off unchanged.
    if (!Options::usePollingTraps() && !vm.gilOff()) {
        // sendSignal() can loop until it has confirmation that the mutator thread
        // has received the trap request. We'll call it from another thread so that
        // requestThreadStopIfNeeded() does not block.
        if (!m_signalSender)
            m_signalSender = adoptRef(new SignalSender(locker, vm));
        m_signalSender->notify(locker);
    }
#else
    UNUSED_PARAM(locker);
#endif

    m_threadStopRequested = true;
}

CONCURRENT_SAFE void VMTraps::notifySyncWaiterOfTermination()
{
    vm().syncWaiter()->notifyOfTerminationRequest();
}

CONCURRENT_SAFE void VMTraps::updateThreadStopRequestIfNeeded()
{
    {
        Locker locker { *m_trapSignalingLock };
        PerLiteTrapSignalingLockDepthScope signalingDepthScope { m_liteOwnerVM }; // Finding (h).

        bool shouldStop = needHandling(AsyncEvents);

        // UNGIL §A.2.2 item 3c, SINGLE CONTROLLER (finding (d)): this
        // per-lite instance is the only controller of its own
        // m_trapAwareSoftStackLimit marker, and it must arm it for VM-WIDE
        // pendingness too: VM-level bits that are not mirrored into this
        // word — carrier-only fireTrap() raises (watchdog/debugger/shell)
        // in particular — deliver through the rerouted per-lite check sites,
        // so the marker is derived from BOTH words. Carrier-only bits never
        // arm a spawned lite (W0/SD13). Cancel symmetric: the marker drops
        // only when both words are clear, and restores the PER-LITE saved
        // soft limit (StackManager::cancelStop on THIS instance).
        if (m_liteOwnerVM) [[unlikely]] {
            BitField vmWideMask = AsyncEvents;
            if (m_liteOwnerIsSpawnedThread)
                vmWideMask &= ~CarrierOnlyServicedEvents;
            // The VM word's NeedTermination belongs to another thread unless a VM-wide termination is raised.
            if (!m_liteOwnerVM->traps().vmWideTerminationRaised())
                vmWideMask &= ~NeedTermination;
            shouldStop |= m_liteOwnerVM->traps().needHandling(vmWideMask);
        }

        if (shouldStop != m_threadStopRequested) {
            if (shouldStop)
                requestThreadStopIfNeeded(locker);
            else
                cancelThreadStopIfNeeded();
        }
    }

    // UNGIL §A.2.2 item 3c — the VM-level stop fan (see the declaration
    // comment). Runs AFTER this instance's own signaling lock is released
    // (lock order, finding (h)); each fanned lite recomputes from the
    // CURRENT bit state, so concurrent fans are idempotent and
    // order-insensitive. GIL-on / flag-off: branch not taken.
    if (!m_liteOwnerVM) {
        VM& vm = this->vm();
        if (vm.gilOff()) [[unlikely]]
            updatePerLiteThreadStopRequestsForVMWideChange(vm);
    }
}

CONCURRENT_SAFE void VMTraps::updatePerLiteThreadStopRequestsForVMWideChange(VM& vm)
{
    ASSERT(!m_liteOwnerVM); // VM-level instance only.
    ASSERT(vm.gilOff());
    assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    for (VMLite* lite : registry.lites) {
        if (lite->vm != &vm)
            continue;
        VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
        if (liteTraps && liteTraps != this)
            liteTraps->updateThreadStopRequestIfNeeded();
    }
}

bool VMTraps::handleTraps(VMTraps::BitField mask)
{
    VM& vm = this->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(onlyContainsAsyncEvents(mask));
    // No ASSERT(needHandling(mask)): cancelStop() from resumeTheWorld() can race with this call.

    // Deferred traps are serviced after the deferral ends. This early return
    // parks nowhere, so it must not run the on-stack jettison below either.
    // The epoch also moves at the edges of another thread's request
    // (ClassAStopWatchdogContext), with no park here, and a DeferTraps caller
    // such as linkFor is about to link a call to a CodeBlock that can be on
    // this stack.
    if (vm.trapsForCurrentThread().m_trapsDeferred)
        RELEASE_AND_RETURN(scope, false);

    // checktraps-dejank-invalidation-point (UNGIL §K.5 / SPEC-jit I21):
    // GIL-off, DFG/FTL CheckTraps no longer clobbers the abstract heap — it
    // is an invalidation point, and THIS function is the park whose overlap
    // with a conductor heap-fact rewrite window (haveABadTime, Class-A fire,
    // OM transition stop, debugger JS) must jettison this thread's on-stack
    // optimizing-JIT code so the resumed poll OSR-exits before reusing any
    // hoisted heap fact. Sample the epoch BEFORE any servicing/park below
    // (the NeedStopTheWorld notifyVMStop park, the GIL yield, every quantum
    // park) and compare on EVERY exit path via the scope-exit: an unchanged
    // epoch is one relaxed-ish load + compare. SOUNDNESS DEPENDS ON THE
    // BUMP EDGE (review blocker, amend round): a window that parks this
    // thread bumps IN-WINDOW, pre-resume (the wrapped-work closure in
    // stopTheWorldAndRun's gilOff reroute), so the entry sample here —
    // taken post-publication, because the publication's trap bits are what
    // sent us here — still compares unequal after the park. See the
    // BUMP-EDGE LAW comment in bytecode/JSThreadsSafepoint.cpp. Gate matches
    // the static modeling predicate in DFGClobberize.h (useJSThreads && !useThreadGIL),
    // NOT vm.gilOff(): the compile-time model is per-process, so the runtime
    // check must be at least as broad. Flag-off: zero behavior change.
    bool checkConductorHeapFactRewriteEpoch = Options::useJSThreads() && !Options::useThreadGIL();
    uint64_t heapFactRewriteEpochOnEntry = 0;
    if (checkConductorHeapFactRewriteEpoch) [[unlikely]]
        heapFactRewriteEpochOnEntry = JSThreadsSafepoint::conductorHeapFactRewriteEpoch();
    auto conductorHeapFactRewriteCheck = makeScopeExit([&] {
        if (!checkConductorHeapFactRewriteEpoch) [[likely]]
            return;
        if (JSThreadsSafepoint::conductorHeapFactRewriteEpoch() == heapFactRewriteEpochOnEntry) [[likely]]
            return;
        jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite();
    });

    // UNGIL §A.2.7/§A.2.8 (SD13/SD14/W0): GIL-off, a spawned Thread never
    // services the carrier-only delivery class — spawned breakpoints are
    // defined no-ops and spawned JS is watchdog-unobserved in v1. This mask
    // trim is the servicing-side enforcement of the rule-3 carrier-only
    // exemption (the bits also are not fanned into spawned lites).
    //
    // A carrier polling from outside any entry scope (a bare JSLockHolder at
    // the JSLock off-JS poll sites) cannot service the watchdog check either:
    // shouldTerminate needs the entry global, and Watchdog::timerDidFire is a
    // one-shot dispatch that is re-armed only by an entered carrier, so
    // taking the bit here would drop the check. Leave it in the word for the
    // entered or parked carrier it targets.
    bool isSpawnedGILOff = false;
    if (vm.gilOff()) [[unlikely]] {
        if (ThreadManager::isJSThreadCurrent()) {
            isSpawnedGILOff = true;
            mask &= ~CarrierOnlyServicedEvents;
        } else if (!vm.currentThreadEntryScope())
            mask &= ~NeedWatchdogCheck;
        if (!mask)
            RELEASE_AND_RETURN(scope, false);
    }

    // DeferTraps and DeferTermination scopes are properties of the deferring
    // thread's stack and live in that thread's own instance
    // (trapsForCurrentThread(); see VMTrapsInlines.h and DeferTermination.h),
    // so only THIS thread's deferrals mask servicing here: a sibling's
    // deferral never blinds this thread, and this thread's deferral suppresses
    // exactly the services on this thread. GIL-on / flag-off:
    // trapsForCurrentThread() == vm.traps(), byte-identical.
    VMTraps& vmLevelTraps = vm.traps();

    // GIL-off, NeedTermination in the VM word is another thread's targeted
    // termination unless a VM-wide termination is raised. Leave it for that
    // thread, which retires the bit (fireTargetedTermination).
    if (vm.gilOff() && this == &vmLevelTraps && !m_vmWideTerminationRaised.load(std::memory_order_relaxed)) [[unlikely]]
        mask &= ~NeedTermination;

    if (vm.trapsForCurrentThread().isDeferringTermination())
        mask &= ~NeedTermination;

    // UNGIL TERM1.2 interim (single shared trap word): a GIL-off carrier that
    // already consumed a VM-wide termination left the bit SET for its
    // still-entered siblings (takeTopPriorityTrap / the fan-out below). Until
    // those siblings exit, the bit must not re-terminate this carrier's host
    // clear-and-re-enter; once they are gone, the consumed raise is retired
    // here. Serialized against a FRESH fireTrapVMWide raise by the registry
    // lock (the raise clears the flag under that lock), so a new raise is
    // never swallowed: either it cleared the flag first (we service it
    // normally below) or it re-sets the bit after the retire (serviced at the
    // next poll).
    // The shield flag lives on the VM-LEVEL instance regardless of which
    // instance set it (takeTopPriorityTrap's per-lite carrier arm stores
    // vm.traps().m_carrierTookSharedTermination), so the trim must consult
    // the VM-level flag here too — a per-lite servicing instance's own copy
    // is never set, and without this the per-lite delivery channel bypasses
    // the shield entirely: didAcquireLock's token-acquisition OR
    // (orVMWideTrapBitsIntoLite) re-ORs the still-set VM-word
    // NeedTermination into the carrier's per-lite word after the host
    // cleared and re-entered, and handleTrapsForCurrentThreadIfNeeded
    // dispatches the per-lite instance FIRST — re-terminating the host's
    // re-entry on every clear-and-re-enter until the siblings drain.
    if (vm.gilOff() && !isSpawnedGILOff && vmLevelTraps.m_carrierTookSharedTermination.load()) [[unlikely]] {
        assertNoPerLiteTrapSignalingLockHeldOnCurrentThread(); // §A.2.2 item 3c (h): this trim runs on per-lite servicing instances too; registry lock outranks per-lite signaling locks.
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        if (vmLevelTraps.m_carrierTookSharedTermination.load()) {
            if (this != &vmLevelTraps) {
                // Per-lite instance on the shielded carrier: this lite's
                // NeedTermination bit is an ECHO of the already-consumed
                // raise (re-ORed at token acquisition from the VM word kept
                // set for the siblings). Drop this lite's copy — siblings
                // observe their OWN fanned per-lite bits, so this clear
                // affects no one else; retiring the VM word + flag stays the
                // VM-level instance's job below. Serialized against a FRESH
                // fireTrapVMWide raise by this registry lock (the raise
                // clears the flag under it), so a new raise is never trimmed.
                // A termination that targets this thread is not an echo.
                if (!vm.hasTargetedTerminationRequestForCurrentThread()) {
                    clearTrapWithoutCancellingThreadStop(NeedTermination);
                    mask &= ~NeedTermination;
                }
            } else if (anyOtherLiteOfVMEntered(locker, vm))
                mask &= ~NeedTermination; // Still being delivered to siblings.
            else {
                vmLevelTraps.m_carrierTookSharedTermination.store(false);
                // Retired; the scope exit below re-derives the stop request.
                // A targeted termination still unserviced keeps the bit.
                if (!anyOtherUnservicedTargetedTermination(locker, vm, nullptr))
                    clearTrapWithoutCancellingThreadStop(NeedTermination);
                mask &= ~NeedTermination;
            }
        }
    }

    // T6 (gilOff scalability — codeBlockSet lock on every trap service): this
    // sweep exists to jettison CodeBlocks whose VM-trap BREAKPOINTS were
    // installed by the signal-based delivery path (tryInstallTrapBreakpoints,
    // the only install site). GIL-off that path is structurally unreachable —
    // §A.2.5 never starts a SignalSender for a gilOff VM, and useJSThreads
    // forces usePollingTraps=1 (SPEC-jit M2b) — yet every handleTraps on
    // every thread (~1.6M/run at W=16) serialized on the ONE process-shared
    // CodeBlockSet lock to walk the whole shared set and find nothing
    // (~8% of slow lock acquisitions, scaling with STW-rate x threads).
    // Skip the locked walk when the set's sticky install flag says no
    // CodeBlock can have breakpoints installed; the flag is set under the
    // set's lock BEFORE any install becomes observable and is never cleared,
    // so a skip can never lose a jettison the unconditional sweep would have
    // performed (see CodeBlockSet.h). The skip predicate covers a GIL-on VM
    // sharing the heap, too: its installs flip the shared set's flag, and
    // gilOff sweepers then sweep exactly as before. GIL-on / flag-off: the
    // gilOff() branch is not taken — the unconditional sweep is unchanged.
    bool skipBreakpointJettisonSweep = false;
    if (vm.gilOff()) [[unlikely]]
        skipBreakpointJettisonSweep = !vm.heap.codeBlockSet().mayHaveCodeBlocksWithInstalledVMTrapBreakpoints();
    if (!skipBreakpointJettisonSweep) {
        Locker codeBlockSetLocker { vm.heap.codeBlockSet().getLock() };
        vm.heap.forEachCodeBlockIgnoringJITPlans(codeBlockSetLocker, [&] (CodeBlock* codeBlock) {
            // We want to jettison all code blocks that have vm traps breakpoints, otherwise we could hit them later.
            if (codeBlock->hasInstalledVMTrapsBreakpoints())
                codeBlock->jettison(Profiler::JettisonDueToVMTraps);
        });
    }

    auto takeTopPriorityTrap = [&] (VMTraps::BitField mask) -> Event {
        Locker locker { *m_trapSignalingLock };
        // Finding (h) follow-up: this lock runs on per-lite instances at
        // every handleTrapsForCurrentThreadIfNeeded poll; the scope makes a
        // registry acquisition under it (e.g. a future extension of the
        // TERM1.2 walk below to per-lite instances) trip the rank assert at
        // runtime instead of relying on the grep-audit comment alone. The
        // anyOtherLiteOfVMEntered registry walk below is reachable only on
        // the VM-level instance (`this == &vm.traps()` key), where this
        // scope is a no-op — the lock graph is unchanged.
        PerLiteTrapSignalingLockDepthScope signalingDepthScope { m_liteOwnerVM };

        // Note: the EventBitShift is already sorted in highest to lowest priority
        // i.e. a bit shift of 0 is highest priority, etc.
        for (unsigned i = 0; i < NumberOfEvents; ++i) {
            Event event = static_cast<Event>(1 << i);
            if (hasTrapBit(event, mask)) {
                // UNGIL TERM1.2 interim (single shared trap word; see
                // perThreadTrapsIfExists): termination is VM-WIDE — EVERY
                // entered thread must observe it, but GIL-off all threads
                // poll this ONE word. The take therefore leaves the bit SET
                // whenever any OTHER lite of this VM is still entered
                // (spawned siblings spinning in JS, D9-parked waiters, a
                // §J.3-parked carrier), and clears it only when this
                // servicer is the last observer. A CARRIER that leaves the
                // bit set records the consumption
                // (m_carrierTookSharedTermination) so the host's
                // clear-and-re-enter is not spuriously re-terminated while
                // siblings drain (handleTraps trim above); a SPAWNED
                // servicer needs no flag — it is about to close per §E.5
                // and never re-enters. A bit stranded by the last spawned
                // servicer racing a sibling's exit costs the host at most
                // one extra termination on its next entry — inside the
                // landed NeedTermination envelope (the bit deliberately
                // survives VM exit; see the class comment). Once the
                // §A.2.1 per-lite words land, every thread takes from its
                // OWN word and this collapses to an unconditional clear.
                // ORDERING (GIL-removal round 5): the "entered" predicate
                // here is a live per-lite VMEntryScope record, but the
                // delivery obligation is TOKEN-scoped — a token-holding
                // sibling between entry scopes (teardown -> completion
                // drain, or between drain iterations) re-enters with the
                // bit gone if this clear fires in that window. Nothing
                // refuses a second concurrent entry any more (every gilOff
                // lite has its own trap word), so delivery to a
                // between-entry-scope token holder is guaranteed by the
                // per-lite fanned words instead: fireTrapVMWide fans
                // NeedTermination into every REGISTERED lite's OWN word
                // (entered or not), so a sibling re-entering through a
                // fresh VMEntryScope still observes its own bit regardless
                // of this VM-word clear. §A.2.1 being landed also means
                // this VM-word interim is reachable only through
                // vm.traps() polls (see the this == &vm.traps() key below).
                if (event == NeedTermination && vm.gilOff() && this == &vm.traps()) [[unlikely]] {
                    // Interim-alias shape only: a per-lite word (this !=
                    // &vm.traps(), §A.2.1 landed) is single-observer — rule-3
                    // fan-out already set every sibling's own bit, so take =
                    // unconditional clear; leaving it set would re-terminate
                    // this thread's next entry.
                    if (anyOtherLiteOfVMEntered(vm)) {
                        if (!isSpawnedGILOff)
                            m_carrierTookSharedTermination.store(true);
                        return event; // Bit left set for the siblings.
                    }
                }
                // §A.2.2 item 3b: a CARRIER taking NeedTermination from its
                // own PER-LITE word consumed a VM-wide raise whose VM-word
                // copy (set by fireTrapVMWide for the unrerouted trap-bit
                // polls and late joiners) is still pending — shield this
                // carrier's host clear-and-re-enter from re-consuming it
                // exactly as the VM-word take above does (handleTraps' trim
                // masks it while entered siblings drain, then retires it).
                // A spawned taker needs no shield — it closes per §E.5.
                if (event == NeedTermination && vm.gilOff() && this != &vm.traps() && !isSpawnedGILOff) [[unlikely]]
                    vm.traps().m_carrierTookSharedTermination.store(true);
                clearTrapWithoutCancellingThreadStop(event);
                return event;
            }
        }
        return NoEvent;
    };

    auto cancelThreadStop = makeScopeExit([&] {
        updateThreadStopRequestIfNeeded();
    });

    bool didHandleTrap = false;
    while (needHandling(mask)) {
        auto event = takeTopPriorityTrap(mask);
        switch (event) {
        case NeedDebuggerBreak:
            // checktraps-dejank-invalidation-point: a serviced debugger break
            // leads to debugger JS running while sibling mutators sit parked
            // at polls. GIL-off the Debugger STW walk publishes a
            // ClassAStopWatchdogContext (which bumps the heap-fact rewrite
            // epoch); GIL-on flag-on no context is published, so bump here
            // explicitly — siblings parked across the debugger's GIL tenure
            // then jettison their on-stack optimized code on resume. (GIL-on
            // CheckTraps modeling is still conservative, so this bump is
            // belt-and-braces there; it becomes load-bearing if the GIL-on
            // model is ever de-janked too.) Flag-off: dead branch.
            if (Options::useJSThreads()) [[unlikely]]
                JSThreadsSafepoint::noteConductorHeapFactRewrite();
            invalidateCodeBlocksOnStack(vm.group3Primitives().topCallFrame);
            didHandleTrap = true;
            break;

        case NeedShellTimeoutCheck:
            RELEASE_ASSERT(g_jscConfig.shellTimeoutCheckCallback);
            g_jscConfig.shellTimeoutCheckCallback(vm);
            didHandleTrap = true;
            break;

        case NeedWatchdogCheck: {
            ASSERT(vm.watchdog());
            ASSERT(!isSpawnedGILOff); // Masked above (annex W W0; SD14).
            // The servicing thread's entry scope: per-lite GIL-off, where the
            // no-entry-scope trim above keeps the bit out of the mask, so it
            // is non-null here; the VM member otherwise, non-null whenever the
            // watchdog is active.
            VMEntryScope* entryScope = vm.currentThreadEntryScope();
            ASSERT(entryScope || !vm.watchdog()->isActive());
            if (!vm.watchdog()->isActive() || !vm.watchdog()->shouldTerminate(entryScope->globalObject())) [[likely]]
                continue;
            [[fallthrough]];
        }

        case NeedTermination: {
            // UNGIL TERM1.2: a termination decision is VM-wide. GIL-off,
            // propagate it to every OTHER entered thread's lite (rule-3
            // form, self excluded — this thread is about to throw). Covers
            // both the direct NeedTermination service and the watchdog
            // fall-through on an entered carrier (annex W shape (c)).
            // A termination that targets only this thread (a time limit that
            // it set) is not propagated, and the lite's handled flag stands in
            // for the VM's request. A VM-wide request pending as well wins.
            bool targetsOnlyThisThread = false;
            if (vm.gilOff()) [[unlikely]] {
                targetsOnlyThisThread = vm.hasTargetedTerminationRequestForCurrentThread() && !vmLevelTraps.vmWideTerminationRaised();
                if (targetsOnlyThisThread)
                    vmLevelTraps.retireTargetedTerminationBitInVMWord();
                else
                    fanOutTerminationToSiblingLites();
            }
            if (targetsOnlyThisThread)
                vm.setHandledTargetedTerminationForCurrentThread();
            else
                vm.setHasTerminationRequest();
            scope.release();
            if (!vm.trapsForCurrentThread().isDeferringTermination()) // Per-thread deferral keying (DeferTermination.h).
                vm.throwTerminationException();
            return true;
        }

        case NeedStopTheWorld:
            VMManager::singleton().notifyVMStop(vm, StopTheWorldEvent::VMStopped);
            didHandleTrap = true;
            break;

        // cancelStop() cleared the bit between needHandling() and takeTopPriorityTrap().
        case NoEvent:
            break;

        case NeedExceptionHandling:
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    RELEASE_AND_RETURN(scope, didHandleTrap);
}

bool VMTraps::handleTrapsIfNeeded(VMTraps::BitField mask)
{
    if (!needHandling(mask))
        return false;
    bool handled = handleTraps(mask);
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // SCAN-EXC-CHECKS (sweep 05, validateExceptionChecks family):
    // handleTraps' ThrowScope unconditionally simulates a throw to its
    // caller on destruction (the validator's "this scope can throw at
    // VMTraps.cpp:686" record). GIL-off this wrapper is the callee of the
    // lite-then-VM dispatch shape — handleTrapsForCurrentThreadIfNeeded
    // below, and the two open-coded copies in JSLock.cpp
    // (spawnedDropAllLocksBracketExit, the GILDroppedSection spawned-arm
    // bracket-exit poll; completeDeferredForeignCarrierRestoreAfterUnlock)
    // — which call this twice in a row with no scope of their own, so the
    // SECOND call's ThrowScope ctor at :686 trips on the FIRST call's
    // simulated throw (the dominant 686/686 report); and when only the
    // per-lite call services (VM-level needHandling false), the unsatisfied
    // simulated throw propagates to the park-site epilogue's scope
    // (conditionProtoFuncWait / atomicsWaitOnProperty / the lock.hold
    // callback's CallData.cpp:76 TopExceptionScope), which is the
    // 686/secondary-site tail.
    //
    // This wrapper IS handleTraps' direct caller and so owes the check.
    // Perform it via the canonical exception() read (clears the per-lite
    // m_needExceptionCheck). A REAL pending exception (the NeedTermination
    // throw) is never lost: every dispatch site re-tests
    // hasPendingTerminationException() before the second service, and the
    // GILDroppedSection spawned-arm dtor converts an installed termination
    // back to bits+request before returning to the park site. gilOff-gated:
    // flag-off / GIL-on byte-identical (the wrapper's flag-off callers are
    // the LLInt/JIT slow paths via handleTrapsForCurrentThreadIfNeeded,
    // whose previousScope is past topEntryFrame so handleTraps' dtor never
    // simulates there — no check was ever owed on that path).
    VM& vm = this->vm();
    if (vm.gilOff()) [[unlikely]]
        (void)vm.exception();
#endif
    return handled;
}

CONCURRENT_SAFE void VMTraps::fireTrapVMWide(Event event)
{
    ASSERT(!(event & ~AllEvents));
    ASSERT(onlyContainsAsyncEvents(event));
    VM& vm = this->vm(); // Valid: VM-level instance only (see header contract).

    if (!vm.gilOff()) {
        // GIL-on / flag-off: the VM-level word is the only storage —
        // byte-equivalent to fireTrap().
        fireTrap(event);
        return;
    }

    // Rule-3 exemption (§A.2.7/§A.2.8): carrier-only bits are never raised
    // through the VM-wide form — the debugger bit stays on the landed
    // carrier protocol and the watchdog bit is carrier-delivered (annex W).
    ASSERT(!(event & CarrierOnlyServicedEvents));

    {
        // §A.2.3 rule 3: under the registry lock, set the bit in every lite
        // OF THIS VM (per-VM filter) + the VM word. Each fanned lite's
        // m_trapSignalingLock (and its StackManager::m_mirrorLock) nests
        // under the registry lock here; THIS instance's stop-request update
        // runs after release.
        assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        for (VMLite* lite : registry.lites) {
            if (lite->vm != &vm)
                continue;
            VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
            if (liteTraps && liteTraps != this) {
                liteTraps->m_trapBits.exchangeOr(event);
                // §A.2.2 item 3c single-controller fan: the lite's OWN
                // bookkeeping derives its marker from the bit we just set
                // (rank: registry lock -> per-lite signaling lock).
                liteTraps->updateThreadStopRequestIfNeeded();
            }
        }
        m_trapBits.exchangeOr(event); // The VM word.

        // TERM1.2 interim: a FRESH VM-wide termination supersedes any
        // pending consumed-by-carrier state (handleTraps' retire trim);
        // clearing the flag under the registry lock serializes against the
        // trim so the new raise is never swallowed.
        if (event & NeedTermination) {
            m_carrierTookSharedTermination.store(false);
            m_vmWideTerminationRaised.store(true, std::memory_order_relaxed);
        }
    }

    // Poll sites + D9 park quanta are the delivery vehicle GIL-off (§A.2.5:
    // the SignalSender is never started; requestThreadStopIfNeeded degrades
    // to the stack-limit stop request + the flag-off-only syncWaiter wake).
    if (isAsyncEvent(event))
        updateThreadStopRequestIfNeeded();
}

CONCURRENT_SAFE bool VMTraps::clearTrapVMWide(Event event)
{
    ASSERT(!(event & ~AllEvents));
    ASSERT(onlyContainsAsyncEvents(event));
    VM& vm = this->vm(); // Valid: VM-level instance only (see header contract).

    if (!vm.gilOff())
        return clearTrap(event);

    ASSERT(!(event & CarrierOnlyServicedEvents));

    bool wasSet = false;
    {
        // Same walk and lock ranks as fireTrapVMWide.
        assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        for (VMLite* lite : registry.lites) {
            if (lite->vm != &vm)
                continue;
            VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
            if (liteTraps && liteTraps != this) {
                if (liteTraps->clearTrapWithoutCancellingThreadStop(event) & event)
                    wasSet = true;
                liteTraps->updateThreadStopRequestIfNeeded();
            }
        }
        if (clearTrapWithoutCancellingThreadStop(event) & event) // The VM word.
            wasSet = true;

        // A withdrawn termination leaves no consumed raise to retire.
        if (event & NeedTermination) {
            m_carrierTookSharedTermination.store(false);
            m_vmWideTerminationRaised.store(false, std::memory_order_relaxed);
        }
    }

    updateThreadStopRequestIfNeeded();
    return wasSet;
}

CONCURRENT_SAFE bool VMTraps::fireTargetedTermination(VMLite& target)
{
    VM& vm = this->vm(); // Valid: VM-level instance only (see header contract).
    ASSERT(vm.gilOff());

    {
        // Same walk and lock ranks as fireTrapVMWide.
        assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        if (target.vm != &vm || !registry.lites.contains(&target))
            return false;
        VMTraps* liteTraps = perThreadTrapsIfExists(target);
        if (!liteTraps || liteTraps == this)
            return false;
        target.targetedTerminationRequested.store(true, std::memory_order_relaxed);
        liteTraps->m_trapBits.exchangeOr(NeedTermination);
        liteTraps->updateThreadStopRequestIfNeeded();
        // Call-free loops poll only the VM word. The other threads ignore
        // this bit (handleTraps, updateThreadStopRequestIfNeeded), and the
        // target retires it (retireTargetedTerminationBitInVMWord).
        m_trapBits.exchangeOr(NeedTermination);
    }
    return true;
}

void VMTraps::retireTargetedTerminationBitInVMWord()
{
    VM& vm = this->vm(); // Valid: VM-level instance only.
    ASSERT(vm.gilOff());
    assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };
    if (m_vmWideTerminationRaised.load(std::memory_order_relaxed))
        return;
    if (anyOtherUnservicedTargetedTermination(locker, vm, VMLite::currentIfExists()))
        return;
    clearTrapWithoutCancellingThreadStop(NeedTermination);
}

CONCURRENT_SAFE void VMTraps::fireTerminationAgainForCurrentThread()
{
    VM& vm = this->vm(); // Valid: VM-level instance only.
    if (vm.gilOff() && !vmWideTerminationRaised()) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->vm == &vm && lite->targetedTerminationRequested.load(std::memory_order_relaxed) && fireTargetedTermination(*lite))
            return;
    }
    fireTrapVMWide(NeedTermination);
}

CONCURRENT_SAFE bool VMTraps::clearTrapVMWideKeepingOtherThreadsTargetedTermination()
{
    VM& vm = this->vm(); // Valid: VM-level instance only (see header contract).
    ASSERT(vm.gilOff());
    VMLite* currentLite = VMLite::currentIfExists();

    bool wasSet = false;
    {
        // Same walk and lock ranks as clearTrapVMWide. A lite whose targeted
        // request stands keeps its bit, so nothing is cleared and re-raised.
        assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        for (VMLite* lite : registry.lites) {
            if (lite->vm != &vm)
                continue;
            if (lite != currentLite && lite->targetedTerminationRequested.load(std::memory_order_relaxed))
                continue;
            VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
            if (liteTraps && liteTraps != this) {
                if (liteTraps->clearTrapWithoutCancellingThreadStop(NeedTermination) & NeedTermination)
                    wasSet = true;
                liteTraps->updateThreadStopRequestIfNeeded();
            }
        }
        // The VM word keeps the bit for another thread's unserviced targeted termination.
        if (!anyOtherUnservicedTargetedTermination(locker, vm, currentLite)) {
            if (clearTrapWithoutCancellingThreadStop(NeedTermination) & NeedTermination)
                wasSet = true;
        }
        m_carrierTookSharedTermination.store(false);
        m_vmWideTerminationRaised.store(false, std::memory_order_relaxed);
    }

    updateThreadStopRequestIfNeeded();
    return wasSet;
}

void VMTraps::fanOutTerminationToSiblingLites()
{
    VM& vm = this->vm();
    ASSERT(vm.gilOff());
    // §A.2.2 item 3b: this can now run on a PER-LITE servicing instance —
    // address the VM-level word/flag explicitly, never through `this`.
    VMTraps& vmLevelTraps = vm.traps();
    VMLite* currentLite = VMLite::currentIfExists();
    assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
    auto& registry = VMLiteRegistry::singleton();
    bool anyOtherEnteredSibling = false;
    {
    Locker locker { registry.lock };
    for (VMLite* lite : registry.lites) {
        if (lite->vm != &vm || lite == currentLite)
            continue;
        VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
        if (!liteTraps)
            continue;
        if (lite->entryScope.load(std::memory_order_relaxed))
            anyOtherEnteredSibling = true;
        if (liteTraps != &vmLevelTraps && liteTraps != this) {
            // §A.2.1 per-lite shape: the sibling has its own word — deliver
            // directly (rule 3, self excluded) and drive ITS bookkeeping so
            // its marker arms (item 3c single-controller fan; rank: registry
            // lock -> per-lite signaling lock).
            liteTraps->m_trapBits.exchangeOr(NeedTermination);
            liteTraps->updateThreadStopRequestIfNeeded();
        }
    }
    // TERM1.2: a termination decision born on this servicing thread (the
    // direct NeedTermination service, or the watchdog->termination
    // fall-through on an entered carrier — annex W shape (c)) MUST reach the
    // other entered threads. Their per-lite words were set above, but the
    // unrerouted generated-code trap-bit polls (Baseline/DFG/FTL CheckTraps,
    // LLInt op_check_traps) still read the VM word — a sibling spinning in a
    // call-free loop polls ONLY those — so when any other entered sibling
    // exists, set the VM word too; if this servicer is a carrier, record the
    // consumption so its host's clear-and-re-enter is shielded while the
    // siblings drain (handleTraps trim; a spawned servicer closes per §E.5
    // and needs no shield). With no other entered sibling there is no one
    // owed delivery, and setting the word would only poison the host's next
    // entry.
    if (anyOtherEnteredSibling) {
        if (!ThreadManager::isJSThreadCurrent())
            vmLevelTraps.m_carrierTookSharedTermination.store(true);
        vmLevelTraps.m_vmWideTerminationRaised.store(true, std::memory_order_relaxed);
        vmLevelTraps.m_trapBits.exchangeOr(NeedTermination);
    }
    }
    // Re-derive the VM-level stop request OUTSIDE the registry lock (the
    // VM-level update takes its own signaling lock and then re-runs the fan;
    // rank: VM-level signaling -> registry). No-op when the word was not set.
    if (anyOtherEnteredSibling)
        vmLevelTraps.updateThreadStopRequestIfNeeded();
}

void VMTraps::fireTerminationVMWideAfterParkedCarrierService()
{
    ASSERT(this->vm().gilOff());
    ASSERT(!ThreadManager::isJSThreadCurrent()); // W1 services on carriers only.
    fireTrapVMWide(NeedTermination);
    // Annex W W1: the firing carrier has ALREADY serviced this termination
    // (its §J.3 park is about to fail per SD8/§E.5). Interim alias: shield
    // its host's clear-and-re-enter from re-consuming the shared word —
    // handleTraps' trim masks NeedTermination on this carrier while entered
    // siblings drain and retires the bit once they are gone. fireTrapVMWide
    // just cleared the flag (fresh-raise rule); a genuinely fresh raise
    // racing this store re-clears it under the registry lock and is serviced
    // normally.
    // CAVEAT (r15 F2 disposition (a)): the SD8-fail premise is falsified when
    // a racing notify dequeued the parked waiter DURING the service window —
    // the park then completes "ok" and the carrier has NOT serviced the
    // termination. The park site is responsible for revoking this shield in
    // that disposition by re-raising fireTrapVMWide(NeedTermination) (the
    // fresh-raise rule clears the flag under the registry lock), so the
    // termination is delivered at the caller's next trap poll instead of
    // being trimmed and retired. Current caller-side revokes:
    // waitSyncWithPerWaitNode (WaiterListManager.cpp) and the W1 episode in
    // ConditionObject's wait loop (ConditionObject.cpp, disposition (a)).
    m_carrierTookSharedTermination.store(true);
}

void VMTraps::orVMWideTrapBitsIntoLite(VMLite& lite)
{
    VM& vm = this->vm();
    ASSERT_UNUSED(vm, vm.gilOff());
    ASSERT(lite.vm == &vm);

    // §A.2.3: serialized against a concurrent rule-3 fan-out by the registry
    // lock, so the joiner observes either the pre-raise word (and then the
    // fan-out sets its lite directly) or the post-raise word (ORed here).
    assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
    auto& registry = VMLiteRegistry::singleton();
    Locker locker { registry.lock };

    BitField word = m_trapBits.loadRelaxed() & AsyncEvents;
    // The VM word's NeedTermination is another thread's targeted termination
    // unless a VM-wide termination is raised (fireTargetedTermination).
    if (!m_vmWideTerminationRaised.load(std::memory_order_relaxed))
        word &= ~NeedTermination;
    // W0/SD13: carrier-only bits never reach a spawned thread's lite. The
    // token acquisition runs on the lite's owner thread, so the thread-kind
    // probe identifies the lite's kind (TERM1.4: the C++ gate and the §I
    // byte agree on every reachable path).
    if (ThreadManager::isJSThreadCurrent())
        word &= ~CarrierOnlyServicedEvents;
    if (!word)
        return;
    VMTraps* liteTraps = perThreadTrapsIfExists(lite);
    if (liteTraps && liteTraps != this) {
        liteTraps->m_trapBits.exchangeOr(word);
        // §A.2.2 item 3c single-controller fan: derive the joiner's own stop
        // request from the bits just ORed in (late-joiner leg (e); rank:
        // registry lock -> per-lite signaling lock).
        liteTraps->updateThreadStopRequestIfNeeded();
    }
}

void VMTraps::backfillVMWideTrapBitsAtLiteRegistration(VMLite& lite, const AbstractLocker&)
{
    // §A.2.2 item 2 (§F.1 lite-registration backfill; late-joiner leg (e)):
    // caller is VMLiteRegistry::registerLite, registry lock held, on the
    // lite's owner thread, AFTER setLiteOwnerVM stamped the per-lite
    // instance. VM-level instance only (header contract).
    ASSERT(!m_liteOwnerVM);
    ASSERT(lite.vm == &vm());
    ASSERT(lite.gilOff);

    VMTraps& liteTraps = lite.threadContext.traps();
    ASSERT(&liteTraps != this);

    BitField word = m_trapBits.loadRelaxed() & AsyncEvents;
    // The VM word's NeedTermination is another thread's targeted termination
    // unless a VM-wide termination is raised (fireTargetedTermination).
    if (!m_vmWideTerminationRaised.load(std::memory_order_relaxed))
        word &= ~NeedTermination;
    // W0/SD13: carrier-only bits never reach a spawned thread's lite.
    // Registration runs on the owner thread (all three registration paths),
    // so the thread-kind probe identifies the lite's kind (TERM1.4).
    if (ThreadManager::isJSThreadCurrent())
        word &= ~CarrierOnlyServicedEvents;
    if (word)
        liteTraps.m_trapBits.exchangeOr(word);
    // Derive the fresh lite's own stop request — unconditionally, so the
    // marker also arms from VM-wide bits that are NOT mirrored per-lite
    // (carrier-only raises on a carrier lite). Rank: registry lock ->
    // per-lite signaling lock.
    liteTraps.updateThreadStopRequestIfNeeded();
}

// §A.2.2 item 3b servicing dispatch — see the declaration comment (VMTraps.h).
bool handleTrapsForCurrentThreadIfNeeded(VM& vm, VMTraps::BitField mask)
{
    bool handled = false;
    if (vm.gilOff()) [[unlikely]] {
        VMLite* lite = VMLite::currentIfExists();
        if (lite && lite->gilOff && lite->vm == &vm) {
            VMTraps* liteTraps = perThreadTrapsIfExists(*lite);
            if (liteTraps && liteTraps != &vm.traps()) {
                handled = liteTraps->handleTrapsIfNeeded(mask);
                // A throwing service (termination) is unwinding: skip the
                // VM-level service for this poll (remaining bits are
                // serviced at the next poll site). Non-mutating per-lite
                // read (see the GILDroppedSection dtor rationale,
                // LockObject.cpp).
                if (vm.hasPendingTerminationException()) [[unlikely]]
                    return handled;
            }
        }
    }
    bool vmLevelHandled = vm.traps().handleTrapsIfNeeded(mask);
    return handled || vmLevelHandled;
}

void VMTraps::deferTerminationSlow(DeferAction)
{
    ASSERT(m_deferTerminationCount == 1);

    VM& vm = this->vm();
    if (vm.hasPendingTerminationException()) [[unlikely]] {
        ASSERT(vm.hasTerminationRequest());
        // While we clear the TerminationExeption here, hasTerminationRequest() remains true and
        // is how we remember that we still need a TerminationException when we stop deferring.
        // hasTerminationRequest() will eventually trigger a re-throw of TerminationExeption
        // after we stop deferring.
        vm.clearException();
        m_suspendedTerminationException = true;
    }
}

void VMTraps::undoDeferTerminationSlow(DeferAction deferAction)
{
    ASSERT(m_deferTerminationCount == 0);

    VM& vm = this->vm();
    ASSERT(vm.hasTerminationRequest());
    if (m_suspendedTerminationException || (deferAction == DeferAction::DeferUntilEndOfScope)) {
        vm.throwTerminationException();
        m_suspendedTerminationException = false;
    } else if (deferAction == DeferAction::DeferForAWhile) {
        // A termination that targets only this thread has to reach the VM
        // word too, which call-free loops poll.
        if (vm.gilOff() && vm.hasTargetedTerminationRequestForCurrentThread() && !vm.traps().vmWideTerminationRaised()) [[unlikely]]
            vm.traps().fireTerminationAgainForCurrentThread();
        else
            fireTrap(NeedTermination); // Let the next trap check handle it.
    }
}

VMTraps::VMTraps()
    : m_trapSignalingLock(Box<Lock>::create())
    , m_condition(Box<Condition>::create())
{
    if (Options::forceTrapAwareStackChecks()) [[unlikely]]
        m_stack.requestStop();
}

VMTraps::~VMTraps()
{
#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    ASSERT(!m_signalSender);
#endif
}

// ============================================================================
// UNGIL §A.2.4 rule 4 / TERM1 rule 4 — the D9 park-poll predicates, PARK-LITE
// form (U-T11; U31). The landed jsThreadParkTerminationRequested
// (LockObject.cpp) is the GIL-on D9 predicate; GIL-off it is wrong twice
// over (VMTraps.h activation-checklist item 4):
//   (a) it reads vm.traps() — the VM word — where the §A.2.4 rule-4 clause
//       re-points the poll at the polling thread's PARK lite: spawned = the
//       CURRENT lite; main/embedder park sites = the §J.3-CAPTURED lite
//       (capturedParkLiteOfCurrentThreadIfAny, JSLock.cpp — the release path
//       runs the §A.3.6 LIFO restore, so CURRENT is the prior lite for the
//       whole park). Under the §A.2.1 interim alias (perThreadTrapsIfExists
//       returns the VM word) the re-point is a semantic no-op, but every new
//       park site must consume THESE predicates so the alias flip is a
//       VMLite.cpp-only change;
//   (b) it folds NeedWatchdogCheck into "termination" — GIL-on that is the
//       landed behavior (a parked thread cannot service the check, and the
//       watchdog's next step would be termination anyway), but GIL-off annex
//       W W1 mandates the SPLIT: a parked CARRIER observing the check bit
//       performs the full §J.3 exit reacquisition and SERVICES
//       Watchdog::shouldTerminate (callback consulted, extension honored —
//       the U-T2 corpus shape (a)), terminating only on a terminate verdict.
// Declared in VMTraps.h; the consumers are the park sites in
// WaiterListManager.cpp, LockObject.cpp, ConditionObject.cpp, ThreadObject.cpp
// and ThreadAtomics.cpp.
//
// Quanta poll ONLY lock-free state (U2's bound): both predicates read atomic
// trap words (+ the VM request flag) and take no lock — legal under a
// rank-3 listLock per §J.3.
// ============================================================================

bool parkLitePollTerminationRequested(VM& vm, VMLite* parkLite)
{
    // hasTerminationRequest() is only ever set by trap handling on an
    // executing mutator, so a park must read the trap bits themselves (the
    // landed D9 rationale, LockObject.h).
    if (vm.hasTerminationRequest())
        return true;
    if (!vm.gilOff()) {
        // GIL-on rule-4 VM-WIDE form, landed semantics preserved (U19
        // oracle): the watchdog-check bit is folded into termination —
        // byte-equivalent to jsThreadParkTerminationRequested.
        return vm.traps().needHandling(VMTraps::NeedTermination | VMTraps::NeedWatchdogCheck);
    }
    // GIL-off rule-4 PARK-LITE form, W1 split: termination ONLY. The
    // watchdog-check bit is a carrier-serviced event
    // (parkLitePollWatchdogCheckRequested below), never a termination
    // verdict by itself.
    VMTraps* traps = parkLite ? perThreadTrapsIfExists(*parkLite) : nullptr;
    if (!traps)
        traps = &vm.traps(); // No captured lite (e.g. a caller predating the §J.3 capture): the VM word is the fan-out source and strictly includes the lite's VM-wide bits.
    return traps->needHandling(VMTraps::NeedTermination);
}

bool parkLitePollWatchdogCheckRequested(VM& vm, VMLite* parkLite)
{
    // Matching CLEAR site: Watchdog::serviceCheckFromReacquiredParkedCarrier
    // consumes the bit on BOTH the carrier lite's word (the word this
    // predicate reads — restored as current by the §J.3 reacquisition) and
    // the VM word, so the §A.2.1 alias flip cannot strand the lite bit and
    // livelock the W1 episode — the flip stays a VMLite.cpp-only change.
    if (!vm.gilOff())
        return false; // GIL-on: folded into the termination predicate above (landed shape; W1 is GIL-off-only).
    // W0/SD14: spawned JS is watchdog-unobserved in v1 — the bit is never
    // fanned into spawned lites, but under the interim single-word alias a
    // spawned park lite's word IS the shared VM word, which can carry the
    // carrier-only bit; mask it out on the reader side too.
    if (ThreadManager::isJSThreadCurrent())
        return false;
    // AB-17 §A.2.2 (item 4, post-§A.2.1 de-alias): the watchdog bit is
    // CARRIER-ONLY and raised via fireTrap() on the VM-LEVEL word — it is
    // deliberately NEVER fanned into per-lite words (rule-3 exemption,
    // §A.2.7-8), and a §J.3-PARKED carrier acquires no token, so the
    // token-acquisition OR cannot mirror it either. This poller IS the
    // carrier the bit targets: consult the VM word alongside the park
    // lite's own word (which still matters for the alias/no-capture
    // fallbacks). Without the VM-word read the W1 episode never triggers
    // de-aliased — the parked carrier livelocks and watchdog termination is
    // never delivered (observed: lock-hold-termination / property-wait-
    // termination GIL-off timeouts).
    if (vm.traps().needHandling(VMTraps::NeedWatchdogCheck))
        return true;
    VMTraps* traps = parkLite ? perThreadTrapsIfExists(*parkLite) : nullptr;
    if (!traps)
        return false; // The VM word above was the only remaining source.
    return traps->needHandling(VMTraps::NeedWatchdogCheck);
}

} // namespace JSC
