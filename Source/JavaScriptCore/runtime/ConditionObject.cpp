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
#include "ConditionObject.h"

#include "ExceptionHelpers.h"
#include "InternalFunction.h" // SCALEBENCH §37 R1: createSubclassStructure for the constructCondition newTarget!=callee arm.
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ThreadObject.h"
#include "ThreadManager.h"
#include "VMLite.h"
#include "VMTraps.h"
#include <wtf/ParkingLot.h>

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

const ClassInfo JSConditionObject::s_info = { "Condition"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSConditionObject) };

static JSC_DECLARE_HOST_FUNCTION(callCondition);
static JSC_DECLARE_HOST_FUNCTION(constructCondition);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncWait);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncAsyncWait);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncNotify);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncNotifyAll);

JSConditionObject::JSConditionObject(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_state(NativeConditionState::create())
{
}

JSConditionObject* JSConditionObject::create(VM& vm, Structure* structure)
{
    JSConditionObject* object = new (NotNull, allocateCell<JSConditionObject>(vm)) JSConditionObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

void JSConditionObject::destroy(JSCell* cell)
{
    static_cast<JSConditionObject*>(cell)->JSConditionObject::~JSConditionObject();
}

JSC_DEFINE_HOST_FUNCTION(callCondition, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Condition constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructCondition, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // SCALEBENCH §37 R1: per-realm cached instance Structure (set in
    // createConditionProperty). See the matching comment in constructLock
    // (LockObject.cpp) — same fresh-Structure-per-new bug, same monomorphic-IC
    // fix, same newTarget handling via createSubclassStructure. Flag-off this
    // function is unreachable.
    Structure* structure = globalObject->conditionObjectStructure();
    ASSERT(structure);
    JSValue newTarget = callFrame->newTarget();
    if (newTarget != callFrame->jsCallee()) [[unlikely]] {
        structure = InternalFunction::createSubclassStructure(globalObject, asObject(newTarget), structure);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(JSConditionObject::create(vm, structure));
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncWait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSConditionObject* conditionObject = dynamicDowncast<JSConditionObject>(callFrame->thisValue());
    if (!conditionObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait called on incompatible receiver"_s);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->argument(0));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait requires a Lock argument"_s);

    NativeLockState& lock = lockObject->lockState();
    // Frozen 4.3: sync cond.wait requires a 5.3 SYNC hold (m_holder). Inside
    // an asyncHold(fn) delivered fn the lock is async-held (4.3(b)
    // territory): sync wait correctly throws here — use cond.asyncWait,
    // whose (b) arm consumes the live grant. (The companion sync-in-async
    // hazard, lock.hold inside that fn, throws "Lock is not recursive" via
    // the D10 m_asyncGrantRunner guard in LockObject.cpp.)
    if (!lock.heldByCurrentThread())
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait requires the lock to be held by the caller"_s);
    if (!jsThreadsCanBlockOnCurrentThread(vm))
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait cannot block the current thread"_s);

    NativeConditionState& condition = conditionObject->conditionState();
    Ref<CondWaiter> waiter = CondWaiter::create(CondWaiter::Kind::Sync);

    // Step 1 (SPEC-api 5.4): enqueue Waiting, under queueLock, while the JS
    // Lock's m_lock is still held (5.9(f) against-rank exemption). Together
    // with step 2 this is the lost-wakeup closure (F3, I9): any notify that
    // can observe the state this waiter is waiting on can only run after the
    // JS Lock is released, by which point the waiter is already enqueued.
    {
        Locker queueLocker { condition.queueLock };
        condition.waiters.append(waiter.copyRef());
    }

    // Step 2: release the JS Lock (clear m_holder, unlock, pump 5.5a R).
    lock.releaseSyncHold();
    lock.releasePump(vm);

    bool terminated = false;
    {
        // Step 3: drop the GIL. Depth-free GIL drop (see GILDroppedSection
        // in LockObject.h): waiters park and wake in arbitrary order, which
        // DropAllLocks' strict-LIFO unwind protocol cannot tolerate.
        GILDroppedSection droppedSection(vm);

        // UNGIL §A.2.4 rule 4 / annex W W1 (AB-17 item 4): park-lite D9
        // polls + the W1 watchdog split (see the contended-hold loop in
        // LockObject.cpp). GIL-on byte-equivalent.
        const bool gilOff = vm.gilOff();
        const bool isSpawnedParker = gilOff && ThreadManager::isJSThreadCurrent();
        VMLite* parkLite = nullptr;
        if (gilOff)
            parkLite = isSpawnedParker ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);

        // Step 4: park on &waiter->state, in 10ms quanta that poll
        // jsThreadParkTerminationRequested() between parks (landed deviation
        // D9, docs/threads/INTEGRATE-api.md): VMTraps cannot wake a
        // ParkingLot-parked condition waiter, so an infinite park is
        // unkillable under the watchdog when the would-be notifier was
        // itself terminated and unwound without notifying — the exact
        // failure mode the mandatory 5.6-4 property-wait poll closes. The
        // park-side validation runs under the ParkingLot internal queue
        // lock (5.9(a1)): a notify that flipped state before we park makes
        // validation fail, so we never sleep through it; a notify that
        // flips after we are enqueued unparks us. A quantum timeout simply
        // re-loops (never surfaces as a spurious return), so quantum
        // wakeups are invisible to JS.
        while (waiter->state.load(std::memory_order_acquire) == CondWaiter::waiting
            && !parkLitePollTerminationRequested(vm, parkLite)) {
            if (gilOff && !isSpawnedParker && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
                // W1 episode: no lock held at this poll point (queueLock is
                // only taken in steps 1/5; ParkingLot is not entered).
                bool watchdogTerminated = reacquireParkedCarrierAndServiceWatchdogCheck(vm);
                if (watchdogTerminated) {
                    // r15 F2 disposition (a): if a notify raced the episode
                    // and dequeued us, the consumed notify is honored as a
                    // successful wait — but then the W1 shield's premise
                    // ("the park is about to fail") is falsified: revoke it
                    // by re-raising VM-wide termination (fresh-raise rule),
                    // so the verdict is delivered at the next trap poll
                    // instead of being trimmed and retired.
                    if (waiter->state.load(std::memory_order_acquire) != CondWaiter::waiting)
                        vm.traps().fireTrapVMWide(VMTraps::NeedTermination);
                    break;
                }
                continue; // Re-validate state before re-parking (disposition (b): the node stayed enqueued; no re-enqueue needed for a ParkingLot wait).
            }
            ParkingLot::parkConditionally(
                &waiter->state,
                [&] { return waiter->state.load(std::memory_order_acquire) == CondWaiter::waiting; },
                [] { },
                MonotonicTime::now() + Seconds::fromMilliseconds(10));
        }

        // Step 5: decide under queueLock. Notified => notify already
        // dequeued us. Still Waiting => only termination exits the loop
        // that way: remove ourselves (we must still be present, since only
        // notify dequeues and it flips state before doing so — the
        // dequeued <=> flipped invariant), exactly like the spurious-wakeup
        // arm this used to be. (If the termination request vanished, the
        // removal simply surfaces as a legal spurious wakeup, I9.)
        {
            Locker queueLocker { condition.queueLock };
            if (waiter->state.load(std::memory_order_acquire) == CondWaiter::waiting) {
                bool removed = condition.waiters.removeFirstMatching([&](auto& entry) {
                    return entry.ptr() == waiter.ptr();
                });
                RELEASE_ASSERT(removed);
                terminated = parkLitePollTerminationRequested(vm, parkLite); // AB-17 item 4: park-lite form, W1 split.
            }
        }

        // Still inside step 3's GIL-dropped scope: reacquire the JS Lock,
        // blocking WITHOUT the GIL (no recursion/G11 check here, 5.4-5: the
        // holder needs the GIL to run JS and release, so we must never block
        // on m_lock while holding it). The GILDroppedSection destructor then
        // reacquires the GIL WITH m_lock held — the one permitted
        // rank-4-leaf shape of 5.9(e), same as the contended hold() path.
        // Deadlock-free: every contender blocks on m_lock only after fully
        // releasing the GIL, and the holder releases m_lock under the GIL in
        // its hold epilogue (or pump/release path), so our GIL reacquisition
        // never waits on a thread that needs m_lock. D9 again: the
        // reacquisition is bounded by the same termination-poll quanta
        // (the current holder may never release if it was terminated); on
        // termination we leave WITHOUT the lock — the enclosing hold()'s
        // epilogue guard (heldByCurrentThread) then skips its release, the
        // same consumed-hold shape as 4.3(a).
        if (!terminated) {
            // tryLock + bounded sleep, NOT WTF::Lock::tryLockWithTimeout —
            // see the contended-hold loop in LockObject.cpp: that helper
            // returns isHeld() ("held by anyone", a signal-safe probe), so
            // a contended reacquire would report success while the notifier
            // still holds m_lock. This waiter would then set m_holder and
            // "release" a lock it never owned (the sync-hold-steal /
            // double-unlock corruption seen in condition-async-wait.js).
            while (!lock.m_lock.tryLock()) {
                if (parkLitePollTerminationRequested(vm, parkLite)) {
                    terminated = true;
                    break;
                }
                if (gilOff && !isSpawnedParker && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
                    // W1 episode: no native lock held (tryLock failed; GIL
                    // released by step 3's dropped section).
                    if (reacquireParkedCarrierAndServiceWatchdogCheck(vm)) {
                        terminated = true;
                        break;
                    }
                    continue;
                }
                WTF::sleep(Seconds::fromMilliseconds(1));
            }
            if (!terminated)
                lock.m_holder.store(&Thread::currentSingleton(), std::memory_order_relaxed);
        }
    }
    // Back under the GIL: close the notified->resumed window notifyImpl
    // opened for us. state == notified is exactly "the notifier incremented
    // for this waiter" — a terminated exit that was never notified removed
    // itself from the queue in step 5 under queueLock (dequeued <=> flipped:
    // it can no longer be flipped) and was never counted, so it must not
    // decrement.
    if (waiter->state.load(std::memory_order_acquire) == CondWaiter::notified)
        jsThreadNoteParkResumptionDone();
    if (terminated) {
        // Same surfacing as the 5.6-7 property-wait path; back under the
        // GIL, in the handleTraps shape (request-then-throw — see the
        // contended-hold epilogue in LockObject.cpp).
        vm.setHasTerminationRequest();
        vm.throwTerminationException();
        return { };
    }
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncAsyncWait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSConditionObject* conditionObject = dynamicDowncast<JSConditionObject>(callFrame->thisValue());
    if (!conditionObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.asyncWait called on incompatible receiver"_s);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->argument(0));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.asyncWait requires a Lock argument"_s);

    NativeLockState& lock = lockObject->lockState();

    // SPEC-api 4.3: the lock must be (a) sync-held by the caller (5.3
    // m_holder) or (b) async-held (live m_asyncHolder ticket); else
    // TypeError. Validate before mutating anything.
    bool syncHeld = lock.heldByCurrentThread();
    RefPtr<AsyncTicket> asyncHolder;
    if (!syncHeld) {
        Locker queueLocker { lock.m_queueLock };
        asyncHolder = lock.m_asyncHolder;
        // (b) tightening (recorded as D6 in docs/threads/INTEGRATE-api.md):
        // "live m_asyncHolder ticket" means a DELIVERED grant. The pump /
        // immediate-grant path installs m_asyncHolder before the grant's
        // settle task runs; consuming such a granted-but-unsettled hold here
        // (reachable synchronously: lock.asyncHold(fn); cond.asyncWait(lock)
        // in one turn) would unlock m_lock while the with-fn settle task
        // later runs fn WITHOUT the lock (mutual-exclusion hole, I6) — and,
        // no-fn, would resolve the promise with a release fn that always
        // throws the 4.2 Error for a release the user never performed. An
        // undelivered grant is not observable by JS, so it is "not held"
        // for 4.3 purposes: fall through to the TypeError.
        if (asyncHolder && !asyncHolder->grantDelivered())
            asyncHolder = nullptr;
        // (b) tightening, round 4 (recorded as D12 in
        // docs/threads/INTEGRATE-api.md): a DELIVERED with-fn grant is "held"
        // only FOR THE THREAD CURRENTLY RUNNING fn. While fn is live, the
        // lock's mutual exclusion is embodied by the D10 runner identity
        // (NativeLockState::m_asyncGrantRunner); if fn parks at any
        // GIL-dropping site (property Atomics.wait, join, a contended hold on
        // another lock, the D2 notify yield), a FOREIGN thread reaching here
        // must not consume the grant — asyncReleaseInternal below would
        // unlock m_lock while fn's remaining body runs believing it still
        // holds the lock, and a third thread's lock.hold would then succeed
        // mid-critical-section (I6, the cross-thread variant of the D6
        // hole). Same-thread consumption from inside fn (I23) still passes:
        // the runner IS the current thread. The NO-FN grant's release fn is
        // a transferable capability per the frozen 4.3(b) "unvalidated
        // consumption" text, so cross-thread consumption stays legal for
        // that arm (escalated to the spec owner in the D12 entry).
        if (asyncHolder && asyncHolder->grantWithFunction && !lock.asyncGrantRunByCurrentThread())
            asyncHolder = nullptr;
    }
    if (!syncHeld && !asyncHolder)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.asyncWait requires the lock to be held"_s);

    // Build the waiter and enqueue it on the condition BEFORE releasing the
    // lock, mirroring the sync wait() steps 1-2 ordering (F3): once the lock
    // is released, a notify is already able to find this waiter. The ticket's
    // dependency vector roots the lock cell for the promise's lifetime (5.5);
    // the registration's addPendingWork is what keeps the shell alive (I20).
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    // U-T9-INT1 / UNGIL sec.E.3: cond.asyncWait is a COUNTED registration —
    // a spawned registrant's outstanding asyncWait must hold its E2A drain
    // loop open past fn-return so the notify-side settle is delivered to the
    // registrant's inbox (sec.E.4/U22/I12) instead of falling back to main
    // after the inbox closed. create() arms internally iff gilOff + spawned
    // registrant; main/embedder and GIL-on/flag-off ignore the bit (sec.E.7).
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, { lockObject }, /* countsKeepalive */ true);
    ticket->grantWithFunction = false; // resolves with a fresh release fn on re-grant (no-fn contract, 4.3)

    Ref<CondWaiter> waiter = CondWaiter::create(CondWaiter::Kind::Async);
    waiter->ticket = ticket.ptr();
    waiter->lock = &lock;
    {
        NativeConditionState& condition = conditionObject->conditionState();
        Locker queueLocker { condition.queueLock };
        condition.waiters.append(WTF::move(waiter));
    }

    // BOTH arms consume the hold (4.3), then release now and pump R (5.5a).
    if (syncHeld) {
        // (a): clear m_holder + unlock; the enclosing hold()'s epilogue
        // guard then skips its release (5.3).
        lock.releaseSyncHold();
        lock.releasePump(vm);
    } else {
        // (b) is unvalidated consumption: CAS the holder ticket's consumed
        // flag; an outstanding release fn called later loses the CAS and
        // throws the 4.2 Error. Then run the 5.5a async-release sequence.
        asyncHolder->tryConsume();
        lock.asyncReleaseInternal(*asyncHolder, vm);
    }
    return JSValue::encode(promise);
}

static EncodedJSValue notifyImpl(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned maxCount)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSConditionObject* conditionObject = dynamicDowncast<JSConditionObject>(callFrame->thisValue());
    if (!conditionObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.notify called on incompatible receiver"_s);

    NativeConditionState& condition = conditionObject->conditionState();
    unsigned woken = 0;
    Vector<Ref<CondWaiter>> asyncWoken;
    {
        Locker queueLocker { condition.queueLock };
        while (woken < maxCount && !condition.waiters.isEmpty()) {
            // SPEC-api 5.4 notify: dequeue FIFO (sync + async uniformly,
            // 4.3); flip state Waiting -> Notified (release) STILL under
            // queueLock — dequeued <=> flipped, atomic against the
            // wait()-side step-5 re-check — then unpark. unparkOne only
            // takes the ParkingLot internal lock (rank-4 leaf), legal under
            // a rank-3 lock (5.9); it wakes the parked waiter, or defeats
            // its park-side validation if it has not parked yet (F3).
            Ref<CondWaiter> waiter = condition.waiters.takeFirst();
            if (waiter->kind == CondWaiter::Kind::Sync) {
                // Open the waiter's notified->resumed window BEFORE the flip
                // (jsThreadNoteParkResumptionPending, LockObject.h): the
                // waiter may wake and close it the instant the store lands,
                // so increment-after-flip could underflow. This is what
                // orders a notifier's 4.6.1 completion drain after the woken
                // waiter's cond.wait return (park-no-microtask-drain.js).
                jsThreadNoteParkResumptionPending();
                waiter->state.store(CondWaiter::notified, std::memory_order_release);
                ParkingLot::unparkOne(&waiter->state);
            } else {
                waiter->state.store(CondWaiter::notified, std::memory_order_release);
                asyncWoken.append(WTF::move(waiter));
            }
            woken++;
        }
    }
    // Async waiters re-enter the lock's async-acquirer queue (5.5a A-failure
    // path); never holding two rank-3 locks at once.
    for (auto& waiter : asyncWoken)
        waiter->lock->enqueueAsyncAcquirer(Ref { *waiter->ticket }, vm);

    // Fair-handoff point for the phase-1 GIL stub: woken sync waiters (and
    // waiters woken by EARLIER notify calls that are still unwinding their
    // GIL droppers) need the GIL to finish wait(). Without this, a notifier
    // that loops in JS (e.g. "notifyAll until everyone reports done") holds
    // the GIL forever and those waiters can never run. Unconditional and
    // depth-free on purpose: see jsThreadGILHandoffYield (LockObject.h) for
    // why a DropAllLocks-based handoff livelocks against parked waiters.
    //
    // LANDED DEVIATION from frozen SPEC-api 5.2 (recorded in
    // docs/threads/INTEGRATE-api.md "Landed deviations", queued for the
    // post-GIL re-freeze): this makes notify()/notifyAll() a yield point in
    // addition to 5.2's blocking-park list. 5.2 as written is unimplementable
    // for cond.notify under a cooperative GIL — a JS-looping notifier would
    // starve its own waiters forever (cond.wait can only finish by
    // reacquiring the GIL). Foreign JS can therefore run inside a notify()
    // call, including while the caller still holds the JS Lock's rank-4
    // m_lock (notify-under-hold); that is the same shape as the 5.4 wait-side
    // reacquisition (GIL acquired with m_lock held, 5.9(e) leaf rule), so no
    // new lock-order edge is introduced. Race tests that rendezvous through
    // notify loops (condition-notify-all*, wait-notify-storm) depend on this
    // yield; the GIL-phase semantic oracle includes it deliberately.
    jsThreadGILHandoffYield(vm);

    return JSValue::encode(jsNumber(woken));
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncNotify, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return notifyImpl(globalObject, callFrame, 1);
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncNotifyAll, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return notifyImpl(globalObject, callFrame, std::numeric_limits<unsigned>::max());
}

JSValue createConditionProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "wait"_s), 1, conditionProtoFuncWait, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncWait"_s), 1, conditionProtoFuncAsyncWait, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "notify"_s), 0, conditionProtoFuncNotify, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "notifyAll"_s), 0, conditionProtoFuncNotifyAll, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));

    // SCALEBENCH §37 R1: stamp the per-realm instance Structure once against
    // the canonical prototype (mirrors createLockProperty). Only reached
    // inside the useJSThreads install block.
    globalObject->setConditionObjectStructure(vm, JSConditionObject::createStructure(vm, globalObject, prototype));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, "Condition"_s, callCondition, ImplementationVisibility::Public, NoIntrinsic, constructCondition);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, "Condition"_s), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
    return constructor;
}

} // namespace JSC
