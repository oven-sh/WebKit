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

#pragma once

#include "JSExportMacros.h"
#include "StackManager.h"
#include <atomic>
#include <wtf/AutomaticThread.h>
#include <wtf/Box.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/RefPtr.h>
#include <wtf/StackBounds.h>
#include <wtf/WorkQueue.h>

namespace JSC {

class CallFrame;
class JSGlobalObject;
class VM;
class VMLite;

class VMTraps {
public:
    using BitField = uint32_t;
    static constexpr size_t bitsInBitField = sizeof(BitField) * CHAR_BIT;

    // The following are the type of VMTrap events / signals that can be fired.
    // This list should be sorted in servicing priority order from highest to
    // lowest.
    //
    // The currently imlemented events are (in highest to lowest priority):
    //
    //  NeedShellTimeoutCheck
    //  - Only used by the jsc shell to check if we need to force a hard shutdown.
    //  - This event may fire more than once before the jsc shell forces the
    //    shutdown (see NeedWatchdogCheck's discussion of CPU time for why
    //    this may be).
    //
    //  NeedTermination
    //  - Used to request the termination of execution of the "current" stack.
    //    Note: "Termination" here simply means we terminate whatever is currently
    //    executing on the stack. It does not mean termination of the VM, and hence,
    //    is not permanent. Permanent VM termination mechanisms (like stopping the
    //    request to stop a woker thread) may use this Event to terminate the
    //    "current" stack, but it needs to do some additional work to prevent
    //    re-entry into the VM.
    //
    //  - The mechanism for achieving this stack termination is by throwing the
    //    uncatchable TerminationException that piggy back on the VM's exception
    //    handling machinery to the unwind stack. The TerminationException is
    //    uncatchable in the sense that the VM will refuse to let JS code's
    //    catch handlers catch the exception. C++ code in the VM (that calls into
    //    JS) needs to do exception checks, and make sure to propagate the
    //    exception if it is the TerminationException.
    //
    //  - Again, the termination request is not permanent. Once the VM unwinds out
    //    of the "current" execution state on the stack, the client may choose to
    //    clear the exception, and re-enter the VM to executing JS code again.
    //    See NeedWatchdogCheck below on why the VM watchdog needs this ability
    //    to re-enter the VM after terminating the current stack.
    //
    //  - Many clients enter the VM via APIs that return an uncaught exception
    //    in a NakedPointer<Exception>&. Those APIs would automatically clear
    //    the uncaught TerminationException and return it via the
    //    NakedPointer<Exception>&. Hence, the VM is ready for re-entry upon
    //    returning to the client.
    //
    //  - In the above notes, "current" (as in "current" stack) is in quotes because
    //    NeedTermination needs to guarantee that the TerminationException has
    //    been thrown in response to this event. If the event fires just before
    //    the VM exits and the TerminationException was not thrown yet, then we'll
    //    keep the NeedTermination trap bit set for the next VM entry. In this case,
    //    the termination will actual happen on the next stack of execution.
    //
    //    This behavior is needed because some clients rely on seeing an uncaught
    //    TerminationException to know that a termination has been requested.
    //    Technically, there are better ways for the client to know about the
    //    termination request (after all, the termination is initiated by the
    //    client). However, this is how some current client code works. So, we need
    //    to retain this behavior until we can change all the clients that rely on
    //    it.
    //
    //  NeedWatchdogCheck
    //  - Used to request a check as to whether the watchdog timer has expired.
    //    Note: the watchdog timeout is logically measured in CPU time. However,
    //    the real timer implementation (that fires this NeedWatchdogCheck event)
    //    has to operate on wall clock time. Hence, NeedWatchdogCheck firing does not
    //    necessarily mean that the watchdog timeout has expired, and we can expect
    //    to see NeedWatchdogCheck firing more than once for a single watchdog
    //    timeout.
    //
    //  - The watchdog mechanism has the option to request termination of the
    //    the current execution stack on watchdog timeout (see
    //    Watchdog::shouldTerminate()). If termination is requested, it will
    //    be executed via the same mechanism as NeedTermination (see how the
    //    NeedWatchdogCheck case can fall through to the NeedTermination case in
    //    VMTraps::handleTraps()).
    //
    //  - The watchdog timing out is not permanent i.e. after terminating the
    //    current stack, the client may choose to re-enter the VM to execute more
    //    JS. For example, a client may use the watchdog to ensure that an untrusted
    //    3rd party script (that it runs) does not get trapped in an infinite loop.
    //    If so, the watchdog timeout can terminate that script. After terminating
    //    that bad script, the client may choose to allow other 3rd party scripts
    //    to execute, or even allow more tries on the current one that timed out.
    //    Hence, the timeout and termination must not be permanent.
    //
    //    This is why termination via the NeedTermination event is not permanent,
    //    but only terminates the "current" stack.
    //
    //  NeedDebuggerBreak
    //  - Services asynchronous debugger break requests.
    //
    //  NeedExceptionHandling
    //  - Unlike the other events (which are asynchronous to the mutator thread),
    //    NeedExceptionHandling is set when the mutator thread throws a JS exception
    //    and cleared when the exception is handled / caught.
    //
    //  - The reason why NeedExceptionHandling is a bit on VMTraps as well is so
    //    that we can piggy back on all the RETURN_IF_EXCEPTION checks in C++ code
    //    to service VMTraps as well. Having the NeedExceptionHandling event as
    //    part of VMTraps allows RETURN_IF_EXCEPTION to optimally only do a single
    //    check to determine if the VM possibly has a pending exception to handle,
    //    as well as if there are asynchronous VMTraps events to handle.

// WARNING: Do NOT sort this list. Read comment above for the reason.
#define FOR_EACH_VMTRAPS_EVENTS(v) \
    v(NeedShellTimeoutCheck) \
    v(NeedTermination) \
    v(NeedWatchdogCheck) \
    v(NeedDebuggerBreak) \
    v(NeedStopTheWorld) \
    v(NeedExceptionHandling)

#define DECLARE_VMTRAPS_EVENT_BIT_SHIFT(event__)  event__##BitShift,
    enum EventBitShift {
        FOR_EACH_VMTRAPS_EVENTS(DECLARE_VMTRAPS_EVENT_BIT_SHIFT)
    };
#undef DECLARE_VMTRAPS_EVENT_BIT_SHIFT


#define COUNT_EVENT(event) + 1
    static constexpr BitField NumberOfEvents = FOR_EACH_VMTRAPS_EVENTS(COUNT_EVENT);
#undef COUNT_EVENT

    using Event = BitField;

#define DECLARE_VMTRAPS_EVENT(event__) \
    static_assert(event__##BitShift < bitsInBitField); \
    static constexpr Event event__ = (1 << event__##BitShift);
    FOR_EACH_VMTRAPS_EVENTS(DECLARE_VMTRAPS_EVENT)
#undef DECLARE_VMTRAPS_EVENT

#undef FOR_EACH_VMTRAPS_EVENTS

    static constexpr Event NoEvent = 0;

    static_assert(NumberOfEvents <= bitsInBitField);
    static constexpr BitField AllEvents = (1ull << NumberOfEvents) - 1;
    static constexpr BitField AsyncEvents = AllEvents & ~NeedExceptionHandling;
    static constexpr BitField NonDebuggerEvents = AllEvents & ~NeedDebuggerBreak;
    static constexpr BitField NonDebuggerAsyncEvents = AsyncEvents & ~NeedDebuggerBreak;

    // UNGIL §A.2.3/§A.2.7/§A.2.8 (SPEC-ungil; GIL-off only): the carrier-only
    // delivery class. The debugger bit (SD13) and the watchdog bit (annex W
    // W0/SD14) are EXEMPT from the rule-3 VM-wide fan-out — they are
    // delivered to and serviced by main/embedder CARRIER threads only;
    // spawned Thread() threads never service them (handleTraps masks them
    // out on a spawned thread; Watchdog/Debugger entry hooks additionally
    // early-return — the W0/SD13 enforcement points). NeedShellTimeoutCheck
    // joins the class conservatively: g_jscConfig.shellTimeoutCheckCallback
    // is a shell-main-thread protocol and must not run on a spawned thread.
    // GIL-on/flag-off: never consulted (the masking is gated on
    // vm.gilOff() && ThreadManager::isJSThreadCurrent()).
    static constexpr BitField CarrierOnlyServicedEvents = NeedShellTimeoutCheck | NeedWatchdogCheck | NeedDebuggerBreak;

    static constexpr bool isAsyncEvent(BitField event)
    {
        return AsyncEvents & event;
    }

    static constexpr bool onlyContainsAsyncEvents(BitField events)
    {
        return (AsyncEvents & events) && !(~AsyncEvents & events);
    }

    ~VMTraps();
    VMTraps();

    static void initializeSignals();

    void willDestroyVM();

    ALWAYS_INLINE bool needHandling(BitField mask) const
    {
        return m_trapBits.loadRelaxed() & mask;
    }
    // Designed to be a fast check to rule out if we might need handling, and we need to ensure needHandling on the slow path.
    ALWAYS_INLINE bool maybeNeedHandling() const { return m_trapBits.loadRelaxed(); }
    void* trapBitsAddress() LIFETIME_BOUND { return &m_trapBits; }
    static constexpr ptrdiff_t offsetOfTrapsBits() { return OBJECT_OFFSETOF(VMTraps, m_trapBits); }

    enum class DeferAction {
        DeferForAWhile,
        DeferUntilEndOfScope
    };

    bool isDeferringTermination() const { return m_deferTerminationCount; }
    inline void deferTermination(DeferAction);
    inline void undoDeferTermination(DeferAction);

    inline void notifyGrabAllLocks();

    bool hasTrapBit(Event event)
    {
        return m_trapBits.loadRelaxed() & event;
    }
    bool hasTrapBit(Event event, BitField mask)
    {
        BitField maskedBits = event & mask;
        return m_trapBits.loadRelaxed() & maskedBits;
    }

    bool isInBlockingScope() const { return m_isInBlockingScope; }

    ALWAYS_INLINE CONCURRENT_SAFE bool clearTrap(Event event)
    {
        ASSERT(!(event & ~AllEvents));
        auto oldBits = clearTrapWithoutCancellingThreadStop(event);
        // Trap bit must be cleared before we update the thread stop request.
        if (isAsyncEvent(event))
            updateThreadStopRequestIfNeeded();
        return oldBits & event;
    }
    ALWAYS_INLINE CONCURRENT_SAFE void fireTrap(Event event)
    {
        ASSERT(!(event & ~AllEvents));
        m_trapBits.exchangeOr(event);
        // Trap bit must be set before we update the thread stop request.
        if (isAsyncEvent(event))
            updateThreadStopRequestIfNeeded();
        // A thread parked in Atomics.wait / memory.atomic.wait handles no traps; wake it so it sees this one.
        if (event == NeedTermination)
            notifySyncWaiterOfTermination();
    }

    JS_EXPORT_PRIVATE CONCURRENT_SAFE void notifySyncWaiterOfTermination();
    // UNGIL §A.2.3 rule 3 / ANNEX TERM1 (TERM1.2): the VM-WIDE trap-raising
    // form. Flag-off / GIL-on: byte-equivalent to fireTrap() (the VM-level
    // word is the only storage). GIL-off (vm.gilOff()): under the
    // VMLiteRegistry lock, sets the bit in EVERY registered lite OF THIS VM
    // (§A.1.3 per-VM filter) AND in the VM-level word (this object), then
    // runs the thread-stop machinery. Token acquisition ORs the VM word into
    // the acquiring lite (orVMWideTrapBitsIntoLite below), so lites
    // registered/entered after a raise still observe it. Termination is
    // VM-WIDE ONLY in v1: there is NO mechanism to raise NeedTermination on
    // exactly one lite (TERM1.2); per-lite raising exists only for genuinely
    // per-thread traps (§A.3 stop tickets; carrier-only bits) and never
    // carries the termination bit.
    //
    // MUST be called on the VM-level VMTraps only (vm.traps()) — the
    // implementation uses VMTraps::vm()'s offset arithmetic, which is only
    // valid for the VM-embedded instance.
    JS_EXPORT_PRIVATE CONCURRENT_SAFE void fireTrapVMWide(Event);

    // The withdrawal counterpart of fireTrapVMWide (VM::cancelTermination):
    // clears the bit in every lite of this VM and in the VM word. Returns
    // whether any of them had it. Flag-off / GIL-on: clearTrap(). Same
    // VM-level-instance-only contract.
    JS_EXPORT_PRIVATE CONCURRENT_SAFE bool clearTrapVMWide(Event);

    // UNGIL annex W W1 terminate arm, interim single-shared-word form: raise
    // VM-wide termination (rule 3) on behalf of a carrier that has ALREADY
    // observed/serviced this termination itself (the §J.3-parked W1 servicer,
    // whose park is about to fail per SD8/§E.5). Equivalent to
    // fireTrapVMWide(NeedTermination) plus marking the raise consumed by this
    // carrier, so the host's clear-and-re-enter after the failed park is not
    // spuriously re-terminated by the shared word while spawned siblings are
    // still draining (see m_carrierTookSharedTermination below). Collapses to
    // plain fireTrapVMWide once the §A.2.1 per-lite words land.
    JS_EXPORT_PRIVATE void fireTerminationVMWideAfterParkedCarrierService();

    // UNGIL §A.2.3 "token acquisition ORs it in" (replaces
    // notifyGrabAllLocks() as the late-joiner delivery edge GIL-off): copies
    // the VM-level word's pending async bits into `lite`'s per-thread traps
    // under the registry lock. Carrier-only bits (above) are filtered when
    // the acquiring thread is a spawned Thread (W0/SD13). Caller: the §F.1
    // GIL-off token-acquisition path, on the lite's owner thread, plus
    // GIL-off lite-registration backfill. No-op while the per-lite traps
    // word aliases the VM word (see perThreadTrapsIfExists below).
    JS_EXPORT_PRIVATE void orVMWideTrapBitsIntoLite(VMLite&);

    // The following returns true if a trap was handled.
    bool handleTraps(BitField mask = AsyncEvents);
    bool handleTrapsIfNeeded(BitField mask = AsyncEvents);

    // UNGIL §A.2.2 item 2 / item 3c late-joiner leg (e): called by
    // VMLiteRegistry::registerLite (the sole writer of lite.vm), UNDER the
    // registry lock, on the registering lite's OWNER thread, for gilOff
    // lites of a gilOff VM only. Copies the VM-level word's pending async
    // bits into the fresh lite's own word (carrier-only bits filtered for
    // spawned threads) and derives the lite's own stop request from them —
    // without this, VM-wide bits raised before a late-joining lite registers
    // would be silently lost until its first token acquisition, and the new
    // lite's first updateStackLimits would publish a plain limit its
    // rerouted check sites pass forever. MUST be called on the VM-level
    // instance.
    void backfillVMWideTrapBitsAtLiteRegistration(VMLite&, const AbstractLocker& registryLocker);

#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    struct SignalContext;
    void tryInstallTrapBreakpoints(struct VMTraps::SignalContext&, StackBounds);
#endif

    static WorkQueue& queue();

#if ENABLE(C_LOOP)
    ALWAYS_INLINE CLoopStack& cloopStack() { return m_stack.cloopStack(); }
    ALWAYS_INLINE const CLoopStack& cloopStack() const { return m_stack.cloopStack(); }
    ALWAYS_INLINE void* cloopStackLimit() { return m_stack.cloopStackLimit(); }
    ALWAYS_INLINE void* currentCLoopStackPointer() const { return m_stack.currentCLoopStackPointer(); }
#endif

    // UNGIL §A.2.2: the limits generated code checks live in m_stack
    // (StackManager). GIL-off these are PER-THREAD state — each lite's
    // VMThreadContext carries its own VMTraps/StackManager, set at that
    // thread's VM entry from its own StackBounds (the GIL-on ownerThread
    // handoff migration of limits is GIL-on-only; vmstate §2 rule 3 is
    // preserved GIL-on). The VM-level instance keeps serving the carrier
    // protocol until the §A.2.1 per-lite append activates (see
    // perThreadTrapsIfExists below).
    ALWAYS_INLINE void* softStackLimit() const { return m_stack.softStackLimit(); };
    inline void setStackSoftLimit(void*);

    ALWAYS_INLINE void** addressOfSoftStackLimit() { return m_stack.addressOfSoftStackLimit(); }

    static constexpr ptrdiff_t offsetOfStackManager() { return OBJECT_OFFSETOF(VMTraps, m_stack); }
    static constexpr ptrdiff_t offsetOfSoftStackLimit()
    {
        return offsetOfStackManager() + StackManager::offsetOfSoftStackLimit();
    }

    using Mirror = StackManager::Mirror;
    inline void registerMirror(Mirror&);
    inline void unregisterMirror(Mirror&);

    VM& vm() const;

    inline void requestStop();
    inline void cancelStop();

    // checktraps-dejank-invalidation-point (UNGIL §K.5 / SPEC-jit I21):
    // jettison every optimizing-JIT CodeBlock on the CURRENT thread's stack
    // after a conductor heap-fact rewrite window (haveABadTime / Class-A fire
    // / OM transition stop / debugger) overlapped this thread's park.
    // Jettison fires the CheckTraps invalidation points emitted by the
    // GIL-off DFG/FTL poll lowering, so the resumed mutator OSR-exits at the
    // poll before reusing any hoisted heap fact. Unlike
    // invalidateCodeBlocksOnStack, this is NOT gated on
    // m_needToInvalidateCodeBlocks (that flag is a one-shot consumed by the
    // first servicing thread; under N mutators every overlapped thread must
    // walk its own stack) and is compiled regardless of
    // ENABLE(SIGNAL_BASED_VM_TRAPS) (it is driven by the polling-traps epoch
    // check, not by signals). Lock shape (amend round, review major fix):
    // the stack walk COLLECTS under the codeBlockSet lock and jettisons
    // AFTER dropping it — CodeBlock::jettison re-enters
    // JSThreadsSafepoint::stopTheWorldAndRun (section 5.3 choke point), and
    // conducting a stop while holding that process-shared lock would
    // deadlock against any sibling whose own path to its park needs the same
    // lock (e.g. handleTraps' breakpoint-sweep walk). Walks the CURRENT
    // thread's frames, resolved from vm.group3Primitives() (the per-lite
    // words GIL-off), so it must run on the thread whose stack is walked.
    // Callers: VMTraps::handleTraps' epoch scope exit, and
    // JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld's post-resume
    // epoch check (public for the latter).
    void jettisonOptimizedCodeOnStackAfterConductorHeapFactRewrite();

private:
    ALWAYS_INLINE BitField clearTrapWithoutCancellingThreadStop(Event event)
    {
        return m_trapBits.exchangeAnd(~event);
    }

    // UNGIL TERM1.2 (handleTraps' NeedWatchdogCheck->NeedTermination
    // fall-through and direct NeedTermination service): GIL-off, a
    // termination decision born on the servicing thread is propagated to the
    // OTHER entered threads' lites (rule 3, self excluded — the servicing
    // thread's own bit was already taken, and re-setting it would make the
    // post-unwind re-entry spuriously terminate).
    void fanOutTerminationToSiblingLites();

    CONCURRENT_SAFE void cancelThreadStopIfNeeded() WTF_REQUIRES_LOCK(m_trapSignalingLock);
    CONCURRENT_SAFE void requestThreadStopIfNeeded(Locker<Lock>&) WTF_REQUIRES_LOCK(m_trapSignalingLock);
    JS_EXPORT_PRIVATE CONCURRENT_SAFE void updateThreadStopRequestIfNeeded();

    // UNGIL §A.2.2 item 3c (AB-17) — the VM-level stop fan, single-controller
    // form (review findings (d)-(f)): a VM-level async-bit change GIL-off
    // drives EVERY registered lite of this VM through that lite's OWN
    // updateThreadStopRequestIfNeeded (which recomputes the lite's marker
    // from its own word PLUS the VM-level word), under the registry lock.
    // Never touches a lite's m_trapAwareSoftStackLimit directly — each
    // lite's marker has exactly ONE controlling traps instance (its own),
    // and cancel restores the PER-LITE saved value via the lite's own
    // StackManager. Lock order (finding (h)): VM-level m_trapSignalingLock
    // (released before the fan) -> VMLiteRegistry::lock -> per-lite
    // m_trapSignalingLock -> per-lite StackManager::m_mirrorLock. VM-level
    // instance only.
    CONCURRENT_SAFE void updatePerLiteThreadStopRequestsForVMWideChange(VM&);

    JS_EXPORT_PRIVATE void deferTerminationSlow(DeferAction);
    JS_EXPORT_PRIVATE void undoDeferTerminationSlow(DeferAction);

#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    class SignalSender;
    friend class SignalSender;

    void invalidateCodeBlocksOnStack();
    void invalidateCodeBlocksOnStack(CallFrame* topCallFrame);
    void invalidateCodeBlocksOnStack(Locker<Lock>& codeBlockSetLocker, CallFrame* topCallFrame);

    void addSignalSender(SignalSender*);
    void removeSignalSender(SignalSender*);
#else
    void invalidateCodeBlocksOnStack() { }
    void invalidateCodeBlocksOnStack(CallFrame*) { }
#endif

    StackManager m_stack;
    Atomic<BitField> m_trapBits { 0 };

    // §A.2.1: owner VM for per-lite embedded instances (set once at lite
    // registration, under the registry lock, before the lite is installable);
    // null for the VM-embedded instance. VMTraps::vm()'s `this -
    // VM::offsetOfTraps()` arithmetic is valid ONLY for the VM-embedded
    // instance — VMTrapsInlines.h must consult this first (out-of-scope leg).
    VM* m_liteOwnerVM { nullptr };
    // §A.2.2 item 3c: thread kind of the owning lite's thread, stamped at
    // registration alongside m_liteOwnerVM (registration runs on the owner
    // thread in all three paths: VM ctor main lite, JSLock.cpp carrier lite,
    // ThreadObject.cpp spawned lite). Used by the per-lite
    // updateThreadStopRequestIfNeeded to exclude carrier-only VM-wide bits
    // from a spawned lite's stop-request derivation (W0/SD13).
    bool m_liteOwnerIsSpawnedThread { false };
public:
    // §A.2.1/§A.2.2 registration-time, once, under the registry lock, before
    // the lite is installable (VMLiteRegistry::registerLite is the sole
    // caller).
    void setLiteOwnerVM(VM* vm, bool ownerThreadIsSpawned)
    {
        m_liteOwnerVM = vm;
        m_liteOwnerIsSpawnedThread = ownerThreadIsSpawned;
    }
    // §A.2.2 item 3c accommodation: VM resolution that is valid on BOTH the
    // VM-embedded instance and a per-lite instance. The `this -
    // VM::offsetOfTraps()` arithmetic in VMTraps::vm() is garbage on a
    // per-lite instance (item 3b); Watchdog's clearTrap on a per-lite
    // instance already reaches requestThreadStopIfNeeded via
    // updateThreadStopRequestIfNeeded today, so the request path must
    // resolve through m_liteOwnerVM first. The item-3b servicing reroute is
    // LANDED (AB-17; VMTraps::vm() itself now consults m_liteOwnerVM) — this
    // helper remains the explicit form for paths that predate that change.
    ALWAYS_INLINE VM& liteAwareVM() const { return m_liteOwnerVM ? *m_liteOwnerVM : vm(); }
private:

    // Deferral state is a property of one thread's stack: DeferTermination and
    // DeferTraps scopes resolve vm.trapsForCurrentThread() (the per-lite
    // instance GIL-off), and handleTraps reads the same instance, so these
    // are only ever touched by the owning thread.
    unsigned m_deferTerminationCount { 0 };
    bool m_suspendedTerminationException { false };
    bool m_trapsDeferred { false };

    // UNGIL TERM1.2 interim (single shared trap word): set when a GIL-off
    // CARRIER consumed a VM-wide NeedTermination but left the bit set in the
    // shared word because other entered lites of this VM still had to observe
    // it (sibling visibility). While set, handleTraps suppresses
    // NeedTermination on carrier threads; once no OTHER lite of this VM is
    // entered, the consumed raise is retired (bit + flag cleared, under the
    // registry lock). A FRESH fireTrapVMWide(NeedTermination) clears the flag
    // (also under the registry lock), so a new raise is never swallowed;
    // every VM-wide termination source (VM::notifyNeedTermination, the
    // watchdog, the parked-carrier W1 verdict) raises through that form.
    // Load-bearing as long as generated-code trap polls read the VM word.
    std::atomic<bool> m_carrierTookSharedTermination { false };

    bool m_needToInvalidateCodeBlocks { false };
    bool m_isShuttingDown { false };
    bool m_threadStopRequested { false };

    // Protects against a race between VMManager::requestResumeAll() and VMManager::notifyVMActivation()
    // to increment their m_numberOfActiveVMs.
    bool m_hasBeenCountedAsActive { false };

    bool m_isInBlockingScope { false };

    // Prevents dispatching multiple idle stop handlers for a single stop cycle.
    Atomic<bool> m_hasDispatchedIdleStopHandler { false };

    Box<Lock> m_trapSignalingLock;
    Box<Condition> m_condition;

#if ENABLE(SIGNAL_BASED_VM_TRAPS)
    RefPtr<SignalSender> m_signalSender;
#endif

    friend class LLIntOffsetsExtractor;
    friend class SignalSender;
    friend class DeferTraps;
    friend class VMManager;
};

class DeferTraps {
public:
    DeferTraps(VM&);
    ~DeferTraps();
private:
    VMTraps& m_traps;
    bool m_previousTrapsDeferred;
};

// The per-thread VMTraps that the rule-3 fan-out (fireTrapVMWide), the
// token-acquisition OR (orVMWideTrapBitsIntoLite) and the registration
// backfill write for `lite`; defined in VMLite.cpp. A gilOff lite carries its
// own instance (lite.threadContext.traps(): trap word + StackManager limits,
// reached by generated code through the chained lite offset); a GIL-on lite
// aliases the VM-level word. Returns null for an unregistered lite.
//
// Delivery contract GIL-off: a VM-wide raise sets every same-VM lite's word
// plus the VM word under the registry lock and drives each lite's OWN
// updateThreadStopRequestIfNeeded, the single controller of that lite's
// trap-aware soft-stack-limit marker (shouldStop = lite word | VM word, with
// the carrier-only trim on spawned lites; cancel restores the per-lite saved
// limit). Lites registering after a raise are backfilled at registration
// (backfillVMWideTrapBitsAtLiteRegistration) and at every token acquisition.
// Servicing dispatches lite-first through handleTrapsForCurrentThreadIfNeeded,
// so bits fanned into a lite are cleared on the instance whose marker they
// armed. Lock order: VM-level m_trapSignalingLock (released before any fan)
// -> VMLiteRegistry::lock -> per-lite m_trapSignalingLock -> per-lite
// StackManager mirror lock, asserted by
// assertNoPerLiteTrapSignalingLockHeldOnCurrentThread.
JS_EXPORT_PRIVATE VMTraps* perThreadTrapsIfExists(VMLite&);

// Park-site poll predicates, evaluated between condition-wait quanta with the
// park site's list lock held: both read only atomic trap words and the VM's
// termination-request flag. GIL-on parkLite is null and the watchdog-check
// bit folds into the termination predicate; GIL-off the termination poll
// reads the parked thread's lite (spawned: the current lite; carrier: the
// lite captured at its release, capturedParkLiteOfCurrentThreadIfAny in
// JSLock.h) and a carrier services the watchdog-check bit separately through
// reacquireParkedCarrierAndServiceWatchdogCheck.
bool parkLitePollTerminationRequested(VM&, VMLite* parkLite);
bool parkLitePollWatchdogCheckRequested(VM&, VMLite* parkLite);

// UNGIL §A.2.2 item 3b servicing dispatch (AB-17; review finding (g)): the
// GIL-off poll-site form. Services the CURRENT thread's per-lite traps
// instance FIRST (the words the rule-3 fan-out and the token-acquisition /
// registration ORs write), then the VM-level instance (the word carrier-only
// fireTrap() raisers and the TERM1.2 interim still use). Without the
// per-lite service, bits fanned into a lite's own word would never clear and
// its marker would stay armed forever — failure mode (g), livelock-grade for
// recurring async events. GIL-on / flag-off: exactly
// vm.traps().handleTrapsIfNeeded(mask), byte-identical. If the per-lite
// service throws (termination), the VM-level service is skipped for this
// poll — the caller is about to unwind; remaining VM-level bits are serviced
// at the next poll site.
JS_EXPORT_PRIVATE bool handleTrapsForCurrentThreadIfNeeded(VM&, VMTraps::BitField mask = VMTraps::AsyncEvents);

#if ASSERT_ENABLED
// §A.2.2 item 3c finding (h): runtime lock-rank assertion for the demoted
// registry lock — no path may acquire VMLiteRegistry::lock while holding any
// per-lite m_trapSignalingLock.
JS_EXPORT_PRIVATE void assertNoPerLiteTrapSignalingLockHeldOnCurrentThread();
#else
inline void assertNoPerLiteTrapSignalingLockHeldOnCurrentThread() { }
#endif

} // namespace JSC
