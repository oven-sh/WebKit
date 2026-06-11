/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LockObject.h"

#include "CustomGetterSetter.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ThreadObject.h"
#include "ThreadManager.h"
#include "VMLite.h"
#include "TopExceptionScope.h"
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>

namespace JSC {

// UNGIL §A.2.4 rule 4 / annex W W1 (AB-17 item 4) — park-lite predicates and
// the W1 carrier service episode. Same-library seams (consumers redeclare;
// the predicates of record live in VMTraps.cpp, the episode helper and the
// captured-lite accessor in JSLock.cpp). GIL-on,
// parkLitePollTerminationRequested(vm, nullptr) is byte-equivalent to the
// landed jsThreadParkTerminationRequested (watchdog-check folded in);
// GIL-off it is termination-ONLY against the PARK lite's word, and the
// watchdog-check bit is serviced by the W1 episode instead of being treated
// as a termination verdict.
bool parkLitePollTerminationRequested(VM&, VMLite* parkLite);
bool parkLitePollWatchdogCheckRequested(VM&, VMLite* parkLite);
VMLite* capturedParkLiteOfCurrentThreadIfAny(VM&);
bool reacquireParkedCarrierAndServiceWatchdogCheck(VM&);

const ClassInfo JSLockObject::s_info = { "Lock"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSLockObject) };

static JSC_DECLARE_HOST_FUNCTION(callLock);
static JSC_DECLARE_HOST_FUNCTION(constructLock);
static JSC_DECLARE_HOST_FUNCTION(lockProtoFuncHold);
static JSC_DECLARE_HOST_FUNCTION(lockProtoFuncAsyncHold);
static JSC_DECLARE_CUSTOM_GETTER(lockLockedGetter);

JSLockObject::JSLockObject(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_state(NativeLockState::create())
{
}

JSLockObject* JSLockObject::create(VM& vm, Structure* structure)
{
    JSLockObject* object = new (NotNull, allocateCell<JSLockObject>(vm)) JSLockObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

void JSLockObject::destroy(JSCell* cell)
{
    static_cast<JSLockObject*>(cell)->JSLockObject::~JSLockObject();
}

GILParkSavedExecutionState::GILParkSavedExecutionState(VM& vm)
    : m_vm(vm)
    , m_gilOff(vm.gilOff())
{
    if (m_gilOff) {
        // UNGIL §J.2 (U-T11): dead GIL-off. Group-3 execution state is
        // per-lite (§A.1.3), and — since obligation 10 landed the
        // exceptionScopeVerificationState() mode split — so is the
        // EXCEPTION_SCOPE_VERIFICATION scope chain, making the "per-lite
        // words" premise of this early return TRUE for every word this
        // class would otherwise save: no other mutator can clobber this
        // thread's topCallFrame/topEntryFrame/exception-scope words while
        // it parks, so there is nothing to save (see the keying rationale
        // on the member declaration, LockObject.h). A save/restore through
        // the raw VM-block members would be a data race shared by every
        // concurrently parking spawned thread. Park-site asserts move to the token
        // meaning (JSLock.cpp IU row 28): a spawned thread holds an entry
        // token; a main/embedder carrier still holds m_lock here, which
        // satisfies the token predicate too.
        ASSERT(vm.currentThreadIsHoldingAPILock());
#if ASSERT_ENABLED
        // Premise check (reviewer amendment): the raw VM-block words must be
        // UNCHANGED across the park — main carrier: its own per-lite live
        // words, untouched while it parks; spawned: GIL-off "inert spare
        // storage" nobody writes (all writers route per-lite). Capture for
        // the dtor's equality assert; reads of never-written (or
        // self-owned) words race with nothing.
        m_topCallFrame = vm.topCallFrame;
        m_topEntryFrame = vm.topEntryFrame;
#endif
        return;
    }
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());
    m_topCallFrame = vm.topCallFrame;
    m_topEntryFrame = vm.topEntryFrame;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: GIL-on arm only (GIL-off took the §J.2 early return
    // above). The accessor resolves the VM copy here — bit-identical to the
    // former raw members.
    m_topExceptionScope = vm.exceptionScopeVerificationState().m_topExceptionScope;
    m_needExceptionCheck = vm.exceptionScopeVerificationState().m_needExceptionCheck;
#endif
}

GILParkSavedExecutionState::~GILParkSavedExecutionState()
{
    if (m_gilOff) {
        // §J.2: nothing was saved; this thread's per-lite state survived the
        // park untouched. Token-meaning assert (runs AFTER ~GILDroppedSection's
        // body re-established the entered state — member dtors run last).
        ASSERT(m_vm.currentThreadIsHoldingAPILock());
#if ASSERT_ENABLED
        // The "unchanged across the park" premise the skip rests on.
        ASSERT(m_vm.topCallFrame == m_topCallFrame);
        ASSERT(m_vm.topEntryFrame == m_topEntryFrame);
#endif
        return;
    }
    ASSERT(m_vm.apiLock().currentThreadIsHoldingLock());
    m_vm.topCallFrame = m_topCallFrame;
    m_vm.topEntryFrame = m_topEntryFrame;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: GIL-on arm only (see ctor).
    m_vm.exceptionScopeVerificationState().m_topExceptionScope = m_topExceptionScope;
    m_vm.exceptionScopeVerificationState().m_needExceptionCheck = m_needExceptionCheck;
#endif
}

void GILParkSavedExecutionState::resetForFreshThread(VM& vm)
{
    if (vm.gilOff()) {
        // IU row 28 (§J.2): token meaning. GIL-off a fresh thread's Group-3
        // execution state is its own lite's, clean at lite creation
        // (§A.1.3); writing the raw VM-block spare storage from a spawned
        // thread would be a cross-thread store into words other parkers
        // concurrently read. Nothing to reset.
        ASSERT(vm.currentThreadIsHoldingAPILock());
        return;
    }
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());
    vm.topCallFrame = nullptr;
    vm.topEntryFrame = nullptr;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    // Obligation 10: GIL-on arm only (GIL-off early-returned above — a
    // fresh GIL-off thread's verification state is its own lite's,
    // zero-initialized at lite creation).
    vm.exceptionScopeVerificationState().m_topExceptionScope = nullptr;
    vm.exceptionScopeVerificationState().m_needExceptionCheck = false;
#endif
}

// UNGIL §J.3 spawned arm (U-T11; the JSLock.cpp unlockAllForThreadParking
// "ORDERING CONSTRAINT" / AB-13 split, now landed): a spawned thread GIL-off
// NEVER owns m_lock (JSLock's token-only entry arm), so a park site has no
// GIL to drop. The §J.3 release is "access release + §A.3 park cooperation +
// §A.3.2b post-wake poll" — which is EXACTLY the §F.4/ANNEX DAL2 heap-access
// bracket: enter releases the client's heap access (a §10.4 GC barrier /
// §A.3.2 conductor never waits on a parked thread); exit re-acquires
// §A.3.2b/§A.3.8-gated (parks across an in-flight stop-the-world) and then
// polls the lite's deferred trap bits before returning to JS. Token, entry
// depth, sp/lastStackTop, m_lockDropDepth are all untouched (per-lite /
// uninvolved), so the D1 LIFO-livelock shape cannot recur (0 locks dropped).
struct GILDroppedSectionSpawnedArm {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(GILDroppedSectionSpawnedArm);
    explicit GILDroppedSectionSpawnedArm(VM& vm)
        : bracket(vm)
    {
    }
    JSLock::DropAllLocks bracket;
};

GILDroppedSection::GILDroppedSection(VM& vm)
    : m_vm(vm)
    , m_savedExecutionState(vm)
    , m_stackPointerAtVMEntry(vm.stackPointerAtVMEntry())
{
    // UNGIL §J.3 (U-T11): GIL-off-by-caller split. The predicate is
    // textually identical to JSLock::DropAllLocks' own GIL-off arm
    // (JSLock.cpp), so the outer split and the inner bracket can never
    // disagree. (m_savedExecutionState keys on gilOff ALONE — see its
    // declaration comment for why that is sound for gilOff MAIN carriers
    // too.)
    if (vm.gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        // Spawned arm: token-only (see GILDroppedSectionSpawnedArm above).
        // Reaching unlockAllForThreadParking here would trip its AB-13
        // RELEASE_ASSERT (a spawned GIL-off thread never holds m_lock).
        m_spawnedArm = makeUnique<GILDroppedSectionSpawnedArm>(vm);
        return;
    }
    // Main/embedder carriers (GIL-off) and every GIL-on caller: m_lock is
    // genuinely held; full §J.3 release (m_lock + token, drain suppressed,
    // park-record push for the W1 watchdog episode happens inside).
    JSLock& apiLock = vm.apiLock();
    ASSERT(apiLock.currentThreadIsHoldingLock());
    // 9.2-9: depth-suppressed release — no microtask drain at park sites
    // (the D11 fix); see JSLock::unlockAllForThreadParking.
    m_lockCount = apiLock.unlockAllForThreadParking();
}

GILDroppedSection::~GILDroppedSection()
{
    if (m_spawnedArm) [[unlikely]] {
        // §J.3 spawned exit: close the DAL2 bracket NOW (the §A.3.2b/§A.3.8-
        // gated heap-access re-acquire — may park across an in-flight
        // stop-the-world — then the deferred lite trap poll), explicitly
        // rather than via the member dtor so the poll's effects can be
        // normalized below before control returns to the park site.
        m_spawnedArm = nullptr;
        // The bracket-exit trap poll (spawnedDropAllLocksBracketExit,
        // JSLock.cpp) runs handleTrapsIfNeeded, which can INSTALL the
        // termination exception inside this host call. Park-site epilogues
        // were written under the recorded premise "only trap BITS were
        // raised while we slept" (request-then-throw), and a value-return
        // with a pending termination exception trips their
        // EXCEPTION_SCOPE_VERIFICATION scopes. Convert the delivery back to
        // the bits+request form: the site's own
        // jsThreadParkTerminationRequested / parkLitePollTerminationRequested
        // check (every park epilogue runs one) or the caller's next trap
        // poll surfaces it — never lost, single canonical throw, no
        // double-throw.
        // (hasPendingTerminationException is the non-mutating per-lite read;
        // vm.exception() would flip this thread's m_needExceptionCheck
        // verification bit.)
        if (m_vm.hasPendingTerminationException()) [[unlikely]] {
            auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
            topScope.clearException();
            m_vm.setHasTerminationRequest();
            m_vm.traps().fireTrapVMWide(VMTraps::NeedTermination); // Idempotent OR; guarantees the next handleTraps re-delivers.
        }
        // Early return BEFORE the sp/lastStackTop restore below: for a
        // spawned thread mid-entry those are LIVE per-lite §A.1.4 slots
        // (token + entry depth untouched across the park); re-stamping
        // lastStackTop with this dtor frame would shrink conservative-scan
        // coverage of the thread's deeper live JS frames for the rest of the
        // entry — a GC-miss UAF. Nothing was unlocked (0 locks dropped), so
        // there is nothing to reacquire or restore.
        return;
    }

    JSLock& apiLock = m_vm.apiLock();
    for (unsigned i = 0; i < m_lockCount; ++i)
        apiLock.lock();

    // Same restoration grabAllLocks performs after its LIFO spin; the
    // intervening holders left THEIR stack-entry bookkeeping in the VM.
    // (m_savedExecutionState's destructor then restores
    // topCallFrame/topEntryFrame, running after this body.)
    m_vm.setStackPointerAtVMEntry(m_stackPointerAtVMEntry);
    m_vm.setLastStackTop(Thread::currentSingleton());
}

void jsThreadGILHandoffYield(VM& vm)
{
    // UNGIL IU row 29 (JSLock.cpp; §J.3/U-T11): the park full-release branch
    // moves to the TOKEN meaning for GIL-off spawned callers — mutex-literal
    // it no-ops (a spawned thread never owns m_lock), and the
    // jsThreadYieldForPendingParkResumptions loop below would busy-spin its
    // full deadline HOLDING heap access, stalling §A.3.2 conductor stops.
    // The spawned GILDroppedSection arm releases heap access for the yield.
    if (vm.gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        if (!vm.currentThreadIsHoldingAPILock())
            return;
    } else if (!vm.apiLock().currentThreadIsHoldingLock())
        return;
    GILDroppedSection droppedSection(vm);
    Thread::yield();
}

// See the declaration comment (LockObject.h). Every increment is paired
// with exactly one decrement on the parker's resume path (all exits of the
// park scopes, including termination), so the counter cannot leak or
// underflow.
static std::atomic<unsigned> s_pendingParkResumptions { 0 };

void jsThreadNoteParkResumptionPending()
{
    s_pendingParkResumptions.fetch_add(1, std::memory_order_relaxed);
}

void jsThreadNoteParkResumptionDone()
{
    s_pendingParkResumptions.fetch_sub(1, std::memory_order_relaxed);
}

void jsThreadYieldForPendingParkResumptions(VM& vm)
{
    // Bounded, monotonic-progress wait: parkers normally resume within a
    // few milliseconds (1ms tryLock quanta + a GIL handoff), so the
    // deadline is pure slack against scheduler stalls. It must NOT be
    // unbounded: a parker can be pinned behind a holder that itself waits
    // on THIS thread's completion (e.g. join under hold), and waiting for
    // it here would deadlock — on deadline expiry we fall back to the
    // pre-existing drain-while-parked timing instead of hanging. Only a
    // DECREASE of the pending count is progress and resets the deadline:
    // the counter is process-global, so an increase (or mere churn) can
    // come from unrelated park traffic and must not extend the wait —
    // otherwise storm workloads (continuous contended-hold/notify churn)
    // could defer this thread's completion sequence unboundedly.
    static constexpr Seconds maxStall = Seconds::fromMilliseconds(500);
    unsigned lastPending = s_pendingParkResumptions.load(std::memory_order_acquire);
    MonotonicTime deadline = MonotonicTime::now() + maxStall;
    while (lastPending) {
        // AB-17 item 4: park-lite form, termination only — this caller is
        // ENTERED (it holds its token between handoff yields), so the
        // watchdog check bit is serviced at its regular trap polls, never
        // treated as termination. GIL-on byte-equivalent (folded form).
        VMLite* yieldParkLite = nullptr;
        if (vm.gilOff())
            yieldParkLite = ThreadManager::isJSThreadCurrent() ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);
        if (parkLitePollTerminationRequested(vm, yieldParkLite))
            return;
        if (MonotonicTime::now() > deadline)
            return;
        jsThreadGILHandoffYield(vm);
        unsigned pending = s_pendingParkResumptions.load(std::memory_order_acquire);
        if (pending < lastPending)
            deadline = MonotonicTime::now() + maxStall;
        lastPending = pending;
    }
}

bool jsThreadParkTerminationRequested(VM& vm)
{
    // See the declaration comment (LockObject.h): hasTerminationRequest()
    // is only ever set by trap handling on an executing mutator, so a park
    // must read the trap bits themselves.
    if (vm.hasTerminationRequest())
        return true;
    return vm.traps().needHandling(VMTraps::NeedTermination | VMTraps::NeedWatchdogCheck);
}

// ---------------- NativeLockState pump machinery (SPEC-api 5.5a) ----------------

void NativeLockState::schedPumpLocked(VM& fallbackVM)
{
    if (m_pumpPending)
        return;
    m_pumpPending = true;
    // SPEC-api 5.5a schedPump: dispatch P on the HEAD ticket's vm.runLoop()
    // (G28), at most one pump per lock (m_pumpPending). In phase 1 there is
    // a single shared VM so this equals the caller's VM; the head-ticket
    // routing is what survives GIL removal (the pump must run on the run
    // loop able to grant the head acquirer). Both callers (R, A-failure)
    // only schedule with m_asyncWaiters non-empty.
    ASSERT(!m_asyncWaiters.isEmpty());
    VM& dispatchVM = m_asyncWaiters.isEmpty() ? fallbackVM : m_asyncWaiters.first()->vm();
    // Capture Ref<VM>, mirroring the D5 waitAsync-timer fix
    // (docs/threads/INTEGRATE-api.md "Landed deviations"): WTF::RunLoop is
    // independently ref-counted and outlives the VM, so this task can
    // otherwise run after embedder VM teardown — pump() -> settleLockGrant
    // -> AsyncTicket::settle() dereferences the ticket's VM
    // (deferredWorkTimer->scheduleWorkSoon). The Ref also forestalls DWT
    // shutdown cancellation racing settle()'s isCancelled() check:
    // cancelPendingWork(VM&) only runs during VM teardown (GC End phase /
    // ~VM), which cannot begin while this task pins the VM; and even a
    // ticket cancelled by other means is tolerated downstream
    // (DeferredWorkTimer::doWork drops cancelled tickets' tasks,
    // DeferredWorkTimer.cpp:121-124, destroying the wrapper — and its
    // promise Strong — under the JSLock).
    dispatchVM.runLoop().dispatch([state = Ref { *this }, protectedVM = Ref { dispatchVM }] {
        state->pump();
    });
}

void NativeLockState::releasePump(VM& vm)
{
    Locker queueLocker { m_queueLock };
    if (!m_asyncWaiters.isEmpty())
        schedPumpLocked(vm);
}

void NativeLockState::enqueueAsyncAcquirer(Ref<AsyncTicket>&& ticket, VM& vm)
{
    Locker queueLocker { m_queueLock };
    m_asyncWaiters.append(WTF::move(ticket));
    schedPumpLocked(vm);
}

void NativeLockState::asyncReleaseInternal(AsyncTicket& ticket, VM& vm)
{
    {
        Locker queueLocker { m_queueLock };
        ASSERT_UNUSED(ticket, m_asyncHolder == &ticket);
        m_asyncHolder = nullptr;
        m_asyncHeld.store(false, std::memory_order_release);
        // The grant is no longer live: a sync hold from the delivered fn's
        // remaining body is legal again (D10; see LockObject.h). Harmless
        // when the runner was never set (no-fn arm, release from another
        // thread).
        m_asyncGrantRunner.store(nullptr, std::memory_order_relaxed);
    }
    m_lock.unlock();
    releasePump(vm);
}

void NativeLockState::pump()
{
    {
        Locker queueLocker { m_queueLock };
        m_pumpPending = false; // clear-before-tryLock is normative (5.5a P)
    }
    if (!m_lock.tryLock())
        return; // holder's release will re-run R with pump-pending false
    RefPtr<AsyncTicket> grant;
    {
        Locker queueLocker { m_queueLock };
        if (m_asyncWaiters.isEmpty()) {
            m_lock.unlock();
            return;
        }
        grant = m_asyncWaiters.takeFirst();
        m_asyncHeld.store(true, std::memory_order_release);
        m_asyncHolder = grant;
    }
    settleLockGrant(*this, *grant);
}

// Settle a granted async acquisition. The settle task runs on a run-loop
// turn holding the JSLock.
void settleLockGrant(NativeLockState& state, AsyncTicket& ticket)
{
    Ref<NativeLockState> protectedState { state };
    Ref<AsyncTicket> protectedTicket { ticket };
    if (ticket.grantWithFunction) {
        JSObject* function = ticket.extraDependency();
        ticket.settle([state = WTF::move(protectedState), ticket = WTF::move(protectedTicket), function](DeferredWorkTimer::Ticket dwtTicket) {
            JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
            JSGlobalObject* globalObject = promise->realm();
            VM& vm = globalObject->vm();
            // Delivery point: from here on the hold is observable by JS, so
            // cond.asyncWait's (b) arm may consume it (the delivered gate in
            // conditionProtoFuncAsyncWait guarantees the ticket could NOT
            // have been consumed before this line — fn therefore always
            // starts with the lock genuinely held; E's consumed-CAS failure
            // below can only mean fn itself gave the hold away, I23).
            ticket->markGrantDelivered();
            // D10 (LockObject.h m_asyncGrantRunner): while fn runs, a sync
            // hold on this lock from this thread must throw "Lock is not
            // recursive" instead of self-deadlocking in m_lock. Cleared by
            // asyncReleaseInternal — via E below, or earlier via
            // cond.asyncWait's 4.3(b) consumption inside fn (after which the
            // lock is free and a sync hold is legal again).
            state->m_asyncGrantRunner.store(&Thread::currentSingleton(), std::memory_order_relaxed);
            auto callData = JSC::getCallData(function);
            MarkedArgumentBuffer args;
            NakedPtr<Exception> exception;
            JSValue result = JSC::call(globalObject, function, callData, jsUndefined(), args, exception);
            // E: implicit post-fn release (unless cond.asyncWait consumed the hold).
            if (ticket->tryConsume())
                state->asyncReleaseInternal(ticket.get(), vm);
            if (exception)
                promise->reject(vm, exception->value());
            else
                promise->resolve(globalObject, vm, result);
        });
        return;
    }
    ticket.settle([state = WTF::move(protectedState), ticket = WTF::move(protectedTicket)](DeferredWorkTimer::Ticket dwtTicket) {
        JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
        JSGlobalObject* globalObject = promise->realm();
        VM& vm = globalObject->vm();
        // Delivery point (see the with-fn arm): the resolved release fn is
        // minted against an unconsumed ticket, so its first call never
        // throws the 4.2 "called more than once" Error spuriously.
        ticket->markGrantDelivered();
        JSNativeStdFunction* releaseFunction = JSNativeStdFunction::create(vm, globalObject, 0, "release"_s, [state, ticket](JSGlobalObject* lexicalGlobalObject, CallFrame*) -> EncodedJSValue {
            VM& innerVM = lexicalGlobalObject->vm();
            auto scope = DECLARE_THROW_SCOPE(innerVM);
            if (!ticket->tryConsume())
                return throwVMError(lexicalGlobalObject, scope, createError(lexicalGlobalObject, "Lock release function called more than once"_s));
            state->asyncReleaseInternal(ticket.get(), innerVM);
            return JSValue::encode(jsUndefined());
        });
        promise->resolve(globalObject, vm, releaseFunction);
    });
}

// ---------------- host functions ----------------

JSC_DEFINE_HOST_FUNCTION(callLock, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Lock constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructLock, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = JSLockObject::createStructure(vm, globalObject, prototype.isObject() ? prototype : jsNull());
    return JSValue::encode(JSLockObject::create(vm, structure));
}

JSC_DEFINE_HOST_FUNCTION(lockProtoFuncHold, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->thisValue());
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.hold called on incompatible receiver"_s);
    JSValue functionValue = callFrame->argument(0);
    auto callData = JSC::getCallData(functionValue);
    if (callData.type == CallData::Type::None)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.hold requires a callable argument"_s);

    NativeLockState& state = lockObject->lockState();
    // Recursion guard: a sync hold (m_holder) OR a live asyncHold(fn) grant
    // being delivered on this very thread (m_asyncGrantRunner, D10 — the
    // sync-in-async self-deadlock; see LockObject.h) both mean "held by the
    // current thread" for 4.2 purposes.
    if (state.heldByCurrentThread() || state.asyncGrantRunByCurrentThread())
        return throwVMError(globalObject, scope, createError(globalObject, "Lock is not recursive"_s));

    if (!state.m_lock.tryLock()) {
        if (!jsThreadsCanBlockOnCurrentThread(vm))
            return throwVMTypeError(globalObject, scope, "Lock.prototype.hold cannot block the current thread"_s);
        // SPEC-api 5.3: contended path. Drop the GIL (depth-free; see
        // GILDroppedSection — never block on m_lock while holding the GIL:
        // the holder needs the GIL to run fn and release), then park in
        // m_lock. The GILDroppedSection destructor reacquires the GIL WITH
        // m_lock held — the one permitted rank-4-leaf shape of 5.9(e).
        // Deadlock-free: every contender blocks on m_lock only after fully
        // releasing the GIL, and the holder releases m_lock under the GIL
        // in its hold epilogue (or pump/release path), so the woken
        // acquirer's GIL reacquisition never waits on a thread that needs
        // m_lock. (Unlike DropAllLocks, GILDroppedSection has no
        // strict-LIFO unwind, so carrying m_lock across the reacquire is
        // safe; see the rationale in LockObject.h.)
        //
        // D9 (docs/threads/INTEGRATE-api.md "Landed deviations"): park in
        // bounded tryLock + sleep quanta, polling
        // jsThreadParkTerminationRequested() between quanta — VMTraps cannot
        // wake a thread blocked in WTF::Lock::lock(), so an unbounded park
        // here is unkillable under the watchdog when the holder can never
        // release (e.g. an asyncHold grant whose release fn is never
        // delivered, or a holder that was itself terminated). Mirrors the
        // mandatory 5.6-4 property-wait termination poll.
        bool acquired = false;
        // Open this parker's park->resumed window (see
        // jsThreadNoteParkResumptionPending, LockObject.h): a completing
        // thread must not run its 4.6.1 microtask drain between the
        // holder's release and this parker's resume
        // (park-no-microtask-drain.js, contended-hold block).
        jsThreadNoteParkResumptionPending();
        {
            GILDroppedSection droppedSection(vm);
            // UNGIL §A.2.4 rule 4 / annex W W1 (AB-17 item 4): GIL-off the
            // D9 poll re-points at the polling thread's PARK lite (spawned =
            // CURRENT lite; carriers = the §J.3-captured lite), and the
            // watchdog-check bit is split out of "termination": a parked
            // CARRIER observing it runs the full W1 reacquire-service-
            // re-release episode (terminating only on a terminate verdict)
            // instead of failing the park. GIL-on: byte-equivalent to the
            // landed jsThreadParkTerminationRequested loop.
            const bool gilOff = vm.gilOff();
            const bool isSpawnedParker = gilOff && ThreadManager::isJSThreadCurrent();
            VMLite* parkLite = nullptr;
            if (gilOff)
                parkLite = isSpawnedParker ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);
            // NOT WTF::Lock::tryLockWithTimeout: that helper is a
            // signal-handler-safe PROBE (for MachineStackMarker) — it sleeps
            // in whole-second quanta and returns isHeld(), i.e. "held by
            // ANYONE", not "acquired by me". Under contention it returns
            // true while another holder (e.g. an undelivered asyncHold
            // grant) still owns m_lock, so this hold would steal the lock
            // and its epilogue would unlock a lock it never acquired — the
            // WTF::Lock double-release abort ("Invalid value for lock: 0")
            // and every corruption downstream of two believed holders. Park
            // in honest tryLock + bounded-sleep quanta instead.
            while (!(acquired = state.m_lock.tryLock())) {
                if (parkLitePollTerminationRequested(vm, parkLite))
                    break;
                if (gilOff && !isSpawnedParker && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
                    // W1 episode: no native lock held here (tryLock failed,
                    // GIL fully released by the dropped section). A
                    // terminate verdict raised VM-wide termination — the
                    // poll above observes it on the next iteration's check;
                    // break directly to the terminated epilogue.
                    if (reacquireParkedCarrierAndServiceWatchdogCheck(vm))
                        break;
                    continue;
                }
                WTF::sleep(Seconds::fromMilliseconds(1));
            }
        }
        // Back under the GIL: close the window on every exit (acquired or
        // terminated) — the pairing invariant keeps the global count exact.
        jsThreadNoteParkResumptionDone();
        if (!acquired) {
            // Termination observed while parked: GIL is reacquired (the
            // dropped section ended), m_lock is NOT held. Same surfacing as
            // the 5.6-7 property-wait path, in the handleTraps shape
            // (request-then-throw: throwTerminationException ASSERTs the
            // request flag, which a parked thread never had set — only trap
            // BITS were raised while we slept).
            vm.setHasTerminationRequest();
            vm.throwTerminationException();
            return { };
        }
    }
    state.m_holder.store(&Thread::currentSingleton(), std::memory_order_relaxed);

    MarkedArgumentBuffer args;
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(globalObject, functionValue, callData, jsUndefined(), args, exception);

    // Epilogue guard: cond.asyncWait may have consumed the hold (4.3(a)).
    if (state.heldByCurrentThread()) {
        state.releaseSyncHold();
        state.releasePump(vm);
    }

    if (exception) {
        throwException(globalObject, scope, exception.get());
        return { };
    }
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(lockProtoFuncAsyncHold, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->thisValue());
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.asyncHold called on incompatible receiver"_s);

    JSValue functionValue = callFrame->argument(0);
    bool hasFunction = !functionValue.isUndefined();
    if (hasFunction) {
        auto callData = JSC::getCallData(functionValue);
        if (callData.type == CallData::Type::None)
            return throwVMTypeError(globalObject, scope, "Lock.prototype.asyncHold requires a callable argument when one is provided"_s);
    }

    NativeLockState& state = lockObject->lockState();
    // Sync-hold check only, deliberately NOT asyncGrantRunByCurrentThread()
    // (D10): per the frozen 4.2 text "async-held is NOT recur (callers
    // queue)" — an asyncHold from inside a delivered fn queues a ticket
    // that the post-fn implicit release (E) lets the pump grant later; no
    // deadlock, so no Error.
    if (state.heldByCurrentThread())
        return throwVMError(globalObject, scope, createError(globalObject, "Lock is not recursive"_s));

    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    Vector<JSCell*> dependencies;
    if (hasFunction)
        dependencies.append(asObject(functionValue));
    dependencies.append(lockObject);
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, WTF::move(dependencies));
    ticket->grantWithFunction = hasFunction;

    // A: try to acquire immediately; otherwise queue FIFO. The immediate
    // grant is only legal when no async acquirer is already queued:
    // m_asyncWaiters can be non-empty while m_lock is transiently free (a
    // release already ran R, but the scheduled pump only runs on a run-loop
    // turn), and a tryLock here would barge an UNDELIVERED grant ahead of
    // the FIFO head (violating the 4.2 FIFO contract — async tickets are
    // FIFO; only SYNC holds may barge, api/lock-async-hold.js test 5; the
    // forcing case is api/condition-async-wait.js's final block). Worse,
    // that undelivered grant pins m_lock for the rest of the synchronous
    // turn, so a thread parked in a contended sync hold can only be freed
    // by run-loop turns the main thread may never reach — the
    // api/condition-async-wait.js sync+async rendezvous deadlock.
    // Emptiness check and tryLock are done under m_queueLock so no acquirer
    // can be enqueued in between (taking rank-4 m_lock under rank-3
    // m_queueLock follows rank order, 5.9; tryLock never blocks, so the
    // pump's m_lock-then-queueLock shape cannot deadlock against this).
    bool acquiredImmediately = false;
    {
        Locker queueLocker { state.m_queueLock };
        if (state.m_asyncWaiters.isEmpty() && state.m_lock.tryLock()) {
            state.m_asyncHeld.store(true, std::memory_order_release);
            state.m_asyncHolder = ticket.ptr();
            acquiredImmediately = true;
        }
    }
    if (acquiredImmediately)
        settleLockGrant(state, ticket);
    else
        state.enqueueAsyncAcquirer(ticket.copyRef(), vm);

    return JSValue::encode(promise);
}

JSC_DEFINE_CUSTOM_GETTER(lockLockedGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(JSValue::decode(thisValue));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.locked called on incompatible receiver"_s);
    NativeLockState& state = lockObject->lockState();
    return JSValue::encode(jsBoolean(state.m_lock.isLocked() || state.m_asyncHeld.load(std::memory_order_acquire)));
}

JSValue createLockProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "hold"_s), 1, lockProtoFuncHold, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncHold"_s), 0, lockProtoFuncAsyncHold, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, "locked"_s), CustomGetterSetter::create(vm, lockLockedGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, "Lock"_s, callLock, ImplementationVisibility::Public, NoIntrinsic, constructLock);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, "Lock"_s), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
    return constructor;
}

} // namespace JSC
