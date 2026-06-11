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
    }

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

    // UNGIL §A.2.1 interim (single shared trap word — see
    // perThreadTrapsIfExists below): these three are PER-THREAD-BY-DESIGN
    // scalars (DeferTermination / DeferTraps scopes are properties of one
    // thread's stack), but until the per-lite VMTraps split lands, N GIL-off
    // mutators reach this ONE instance through vm.traps()/the alias, and the
    // DeferTermination/DeferTraps scopes run on spawned-reachable hot paths
    // (LLIntSlowPaths, JITOperations, DFGOperations, JSObject, ...).
    // std::atomic makes the counter ++/-- lost-update-free (no underflow /
    // stuck-nonzero) and every access TSAN-clean (std::atomic, not
    // WTF::Atomic, so the untouched VMTrapsInlines.h ++/-- forms keep
    // compiling). The remaining SEMANTIC cross-talk — one thread's defer
    // scope masking another thread's NeedTermination service, or two
    // interleaved DeferTraps scopes restoring each other's saved
    // m_trapsDeferred — is a bounded DELAY or one early/late poll, never a
    // lost trap (the bits stay set and are serviced at the next unmasked
    // poll site), and disappears with the §A.2.1 per-lite split.
    std::atomic<unsigned> m_deferTerminationCount { 0 };
    std::atomic<bool> m_suspendedTerminationException { false };
    std::atomic<bool> m_trapsDeferred { false };

    // UNGIL TERM1.2 interim (single shared trap word): set when a GIL-off
    // CARRIER consumed a VM-wide NeedTermination but left the bit set in the
    // shared word because other entered lites of this VM still had to observe
    // it (sibling visibility). While set, handleTraps suppresses
    // NeedTermination on carrier threads; once no OTHER lite of this VM is
    // entered, the consumed raise is retired (bit + flag cleared, under the
    // registry lock). A FRESH fireTrapVMWide(NeedTermination) clears the flag
    // (also under the registry lock), so a new raise is never swallowed.
    // Dead weight once the §A.2.1 per-lite words land.
    std::atomic<bool> m_carrierTookSharedTermination { false };

    bool m_needToInvalidateCodeBlocks { false };
    bool m_isShuttingDown { false };
    bool m_threadStopRequested { false };

    // Protects against a race between VMManager::requestResumeAll() and VMManager::notifyVMActivation()
    // to increment their m_numberOfActiveVMs.
    bool m_hasBeenCountedAsActive { false };

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

// UNGIL §A.2.1 seam (U-T2; defined in VMLite.cpp): the per-thread VMTraps the
// rule-3 fan-out and the token-acquisition OR write for `lite`. The §A.2.1
// contract is a VMLite L2 append `VMThreadContext threadContext` (after
// Group 6) whose traps generated code reaches via the chained offset
// lite->threadContext.traps().m_trapBits — that append is LANDED (AB-17
// item 1), and this seam now returns &lite.threadContext.traps() for gilOff
// lites (GIL-on lites keep the VM-word alias; U0b intact). Fan-outs and the
// token-acquisition OR are pointer-identity-keyed and de-alias automatically.
// Returns null for an unregistered lite.
//
// ACTIVATION CHECKLIST — STATUS (AB-17 §A.2.2 reroute change): ALL items
// below are LANDED in the single AB-17 §A.2.2 diff (recorded with the
// orchestrator / INTEGRATE-ungil.md): (2) registration backfill in
// VMLiteRegistry::registerLite (setLiteOwnerVM + OR + per-lite stop-request
// derivation, under the registry lock); (3) every generated-code and C++
// soft-limit read rerouted (LLInt chained offsets now referenced;
// AssemblyHelpers::branchPtrAgainstSoftStackLimit at all JIT-tier sites,
// INCLUDING the LOL tier's prologue check — LOL is additionally forced off
// under GIL-off at option canonicalization pending its §A.1.3 audit;
// softStackLimitForCurrentThread[Slow] for C++); (3c) the single-controller
// stop fan (VM-level updateThreadStopRequestIfNeeded fans each lite's OWN
// update; per-lite shouldStop = lite word | VM word with the carrier-only
// trim; cancel restores the per-lite saved value), with the (h) lock
// re-rank (registry -> per-lite signaling -> per-lite mirror) asserted via
// assertNoPerLiteTrapSignalingLockHeldOnCurrentThread; (3b) VMTraps::vm()
// consults m_liteOwnerVM, and every poll site dispatches through
// handleTrapsForCurrentThreadIfNeeded (lite first, then the VM-level
// instance); (4) the W1/D9 park-site split in LockObject/ConditionObject/
// ThreadObject. The VMEntryScope::setUpSlow gate flipped
// (perLiteSoftStackLimitRerouteLanded = true); VM::updateStackLimits'
// N-entered refusal walk is deleted and the VM word is carrier-published
// only.
//
// ACCEPTANCE AMENDMENT (review round; the items are LANDED but AB-17 is
// NOT verification-complete): the pinned GIL-off acceptance command
// (smoke.js under the six GIL-off flags) still fails intermittently on
// downstream N-entry legs — (i) Debug/ASAN deterministic
// stack-use-after-return in ThrowScope::~ThrowScope on a spawned thread
// (VM-level exception-scope chain shared across lites; per-lite
// exception-state split leg) and (ii) Release intermittent SIGSEGV in
// Baseline-JIT'd code / tier-up "CompilationInvalidated" RELEASE_ASSERT
// (concurrent compilation legs). The W1 parked-carrier watchdog livelock
// (item 4's verdict keyed the parked carrier to its non-advancing CPU
// budget; condition-wait/property-wait termination hung GIL-off) is FIXED
// via Watchdog CallerState::ParkedCarrier. See the AB-17 ACCEPTANCE
// STATUS block in VMEntryScope.cpp for the full failing-rung record. The
// original checklist text is preserved below as the contract of record:
//   (1) LANDED (AB-17): VMLite.h `VMThreadContext threadContext` L2 append +
//       the perThreadTrapsIfExists flip (VMLite.cpp, keyed on lite.gilOff).
//   (2) JSLock.cpp / VMLiteShared (§F.1): the GIL-off token-acquisition edges
//       DO call vm.traps().orVMWideTrapBitsIntoLite(lite)
//       (spawnedThreadEntryTokenLock and the carrier arm — landed at
//       U-T8/U-T11; now real work post-flip), but lite REGISTRATION must
//       additionally backfill the VM word into the fresh per-lite word (and
//       call setLiteOwnerVM on it), or VM-wide bits raised before a
//       late-joining lite registers would be silently lost until its first
//       token acquisition.
//   (3) PARTIAL (AB-17 item 3): VM::updateStackLimits now DUAL-PUBLISHES
//       GIL-off — the entering thread's lite StackManager (its own
//       StackBounds; what the future per-lite generated-code checks read)
//       AND the VM-level word (what all tiers' generated-code stack checks
//       STILL read today). The LLInt per-lite chained offsets
//       (VMLiteTrapAwareSoftStackLimitOffset / VMLiteSoftStackLimitOffset)
//       and the T2 loader/discriminator wrapper are STAGED in
//       LowLevelInterpreter.asm but referenced by no check site. The
//       VM-level publish + its no-other-entered RELEASE_ASSERT tripwire can
//       be deleted ONLY in the same change that reroutes every
//       generated-code soft-limit read (LLInt shared prologue and the
//       64/32_64 doVMEntry/CLoop sites, the Baseline/DFG/FTL emission
//       sites — JIT.cpp, AssemblyHelpers.cpp, ThunkGenerators.cpp,
//       SetupVarargsFrame.cpp, DFGSpeculativeJIT.cpp, FTLLowerDFGToB3.cpp,
//       YarrJIT.cpp — under the §A.1.3 COMPILED-FOR-VM-mode rule) and every
//       C++ VM::softStackLimit() reader (VMInlines.h isSafeToRecurse /
//       ensureStackCapacityFor, LLIntSlowPaths stack_check slow-path
//       re-confirm, JSString rope resolution, JSONObject, LiteralParser,
//       Yarr) through the per-thread lite chain, AND lands item (3c).
//       LANDED SUBSET (I1-AB17 R3): the C++-reader leg only —
//       softStackLimitForCurrentThread() (VMInlines.h) reroutes
//       isSafeToRecurseSoft / ensureJSStackCapacityFor and the
//       LLIntSlowPaths re-confirm through the per-lite PLAIN limit (null
//       per-lite limit falls back to the VM word). This subset is inert
//       w.r.t. 3c/3b: it never reads the trap-aware word, so trap
//       observability is unchanged. The remaining C++ readers (JSString
//       rope, JSONObject, LiteralParser, Yarr interpreter) should adopt
//       the same helper in their own files. Everything else in (3) —
//       LLInt asm reroute included — remains UNLANDED; see (3d).
//   (3c) STOP FAN (REQUIRED BEFORE any (3) read-site reroute lands;
//       memory-safety/liveness grade): once any check site reads the
//       per-lite trap-aware word, VMTraps::requestThreadStopIfNeeded() /
//       cancelThreadStopIfNeeded() on the VM-level instance must fan the
//       m_trapAwareSoftStackLimit poke to EVERY entered lite of that VM —
//       today they poke only the VM word, which a rerouted site no longer
//       reads, so termination / GC-safepoint delivery would be lost or
//       late even single-entered GIL-off. Contract the fan must satisfy:
//       (a) the requester atomically swaps each entered lite's
//       m_trapAwareSoftStackLimit under a lock order stated against the
//       entry-scope publication in VM::updateStackLimits (the
//       fan-vs-concurrent-updateStackLimits write ordering — CAS/exchange
//       discipline on the Atomic word — must be explicit, not implied);
//       (b) a delivery argument: the target mutator is guaranteed to
//       re-execute a rerouted check site within bounded work after the
//       swap; (c) cancel restores the PER-LITE saved value (never the VM
//       word). Pin with a GIL-off test requesting a VMTraps stop while a
//       lite is mid-LLInt.
//       REVIEW FINDINGS (I1-AB17 R2, proposal rejected 0/3 — a naive fan
//       that calls liteTraps->m_stack.requestStop()/cancelStop() directly
//       from the VM-level instance fails all three of the following; the
//       fan contract above is NECESSARY but NOT SUFFICIENT):
//       (d) SINGLE CONTROLLER: each lite's m_trapAwareSoftStackLimit marker
//       must have exactly ONE controlling traps instance. The VM-level
//       request/cancel must drive each lite via that lite's OWN traps
//       bookkeeping (per-lite updateThreadStopRequestIfNeeded recomputing
//       liteTraps->needHandling, with an explicit VM-instance-lock-before-
//       lite-instance-lock rank for m_trapSignalingLock). Otherwise: the
//       VM-level cancel fan erases a lite's marker while that lite's OWN
//       m_trapBits are still pending (StackManager::cancelStop overwrites
//       unconditionally; the hasStopRequest() guard protects only against
//       limit publishes) — lost delivery, the lite spins forever; and the
//       request fan leaves the per-lite m_threadStopRequested stale
//       (false), so the lite's own updateThreadStopRequestIfNeeded sees
//       shouldStop==false==m_threadStopRequested and never cancels — a
//       permanently stuck marker forcing every JS call onto the slow path.
//       (e) LATE-JOINER LEG: a fan that snapshots the registry at request
//       time misses lites registering afterwards. Registration (item 2's
//       backfill) must ALSO derive the fresh lite's own stop request from
//       the backfilled VM-wide bits, under the registry lock, with a
//       stated delivery bound — else the new lite's first
//       updateStackLimits publishes a plain limit and its rerouted sites
//       pass forever.
//       (f) Additional pin test: VM-level cancel while a sibling lite's
//       per-lite trap bits are still pending must NOT clear that lite's
//       marker (interleaving (d) above), alongside the mid-LLInt stop
//       test.
//       REVIEW FINDINGS (I1-AB17 R3, proposal rejected 1/3 — a
//       single-controller fan driving each lite's OWN
//       updateThreadStopRequestIfNeeded fixes (d)-(f), but is STILL not
//       landable ahead of (3b)):
//       (g) UNCANCELLABLE MARKER: with the LLInt read rerouted and the fan
//       arming per-lite markers, but trap SERVICING still running through
//       the VM-level instance (LLIntSlowPaths `vm.traps()
//       .handleTrapsIfNeeded()`), servicing clears only the VM-level bits;
//       the lite's own m_trapBits stay set and its marker stays armed
//       forever — failure mode (d) reproduced one layer up, livelock-grade
//       for recurring async events (GC safepoints). Therefore the LLInt
//       read reroute and the stop fan are NOT inert (a lone spawned lite
//       passes the no-other-entered tripwire) and may land ONLY atomically
//       with the (3b) servicing reroute (VMTrapsInlines.h liteAwareVM-based
//       vm() for instance paths, plus every handleTrapsIfNeeded poll site
//       dispatching on the CURRENT lite's traps instance GIL-off).
//       (h) ATOMIC LANDING UNIT: LLInt asm reroute + all generated-code
//       emission sites listed in (3) + stop fan (3c, single-controller
//       form) + servicing reroute (3b) + tripwire deletion + VM-level
//       dual-publish removal are ONE indivisible diff. The fan's lock
//       re-rank (VMLiteRegistry.lock -> per-lite m_trapSignalingLock ->
//       per-lite StackManager m_mirrorLock) demotes the registry lock from
//       leaf rank and MUST land with a runtime lock-rank assertion plus a
//       grep-audit that no path takes the registry lock while holding any
//       per-lite m_trapSignalingLock.
//       (i) Third pin test, alongside (f) and the mid-LLInt stop test:
//       GIL-off spawned lite receives a fanned async event, services it,
//       then assert the lite's marker is cancelled (trap-aware word !=
//       stop-request marker) and subsequent calls take the fast path —
//       regression test for (g).
//   (3b) VMTrapsInlines.h: VMTraps::vm() must consult m_liteOwnerVM before
//       the `this - VM::offsetOfTraps()` arithmetic — that arithmetic is
//       valid ONLY for the VM-embedded instance and is garbage on a
//       per-lite instance.
//   (4) The §J.3/D9 park sites (LockObject.cpp/ThreadObject.cpp/
//       ConditionObject.cpp): split NeedWatchdogCheck out of
//       jsThreadParkTerminationRequested GIL-off and drive annex W W1 via
//       Watchdog::serviceCheckFromReacquiredParkedCarrier (Watchdog.h),
//       including the r15 F2 old-node disposition; and re-point the D9
//       predicate at the polling thread's PARK lite (TERM1.4/U31). Under the
//       alias the re-point is a semantic no-op (park lite word == VM word),
//       but the W1 split is NOT: today a parked thread treats the watchdog
//       CHECK bit as termination (see Watchdog.h).
JS_EXPORT_PRIVATE VMTraps* perThreadTrapsIfExists(VMLite&);

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
