/*
 * Copyright (C) 2022-2023 Apple Inc. All rights reserved.
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
#include "WaiterListManager.h"

#include "DeferredWorkTimerInlines.h"
#include "Heap.h" // UNGIL r33: GCClient::Heap::currentThreadClient for the access-released entry assert.
#include "HeapCellInlines.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "JSObjectInlines.h"
#include "ObjectConstructor.h"
#include "Options.h"
#include "ThreadManager.h" // UNGIL §C.6/SD6 (U-T11): useJSThreadsEnabled + the spawned/carrier park-lite split.
#include "VMLite.h"        // UNGIL §J.3 (U-T11): the spawned park lite is the CURRENT lite (TERM1 rule 4).
#include "VMManager.h"
#include "VMTrapsInlines.h"
#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RawPointer.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

namespace WaiterListsManagerInternal {
static constexpr bool verbose = false;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(Waiter);
WTF_MAKE_TZONE_ALLOCATED_IMPL(WaiterList);

Waiter::Waiter(VM* vm)
    : m_vm(vm)
    , m_isAsync(false)
{
}

Waiter::Waiter(JSPromise* promise)
    : m_vm(&promise->vm())
    , m_globalObject(promise->realm())
    , m_ticket(m_vm->deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::AtSomePoint, *m_vm, promise, { }))
    , m_isAsync(true)
{
}

Waiter::~Waiter() = default;

void Waiter::setParkedList(RefPtr<WaiterList>&& list)
{
    ASSERT(!m_isAsync);
    Locker locker { m_parkedListLock };
    m_parkedList = WTF::move(list);
}

void Waiter::notifyOfTerminationRequest()
{
    ASSERT(!m_isAsync);
    RefPtr<WaiterList> parkedList;
    {
        Locker locker { m_parkedListLock };
        parkedList = m_parkedList;
    }
    // Not parked (yet): waitSyncImpl publishes the list before it tests for the request, so a wait
    // that starts after this sees the NeedTermination trap our caller has already fired.
    if (!parkedList)
        return;
    // Notify under the list's lock, which the waiter holds from testing for the request until it is
    // parked, so that the notification cannot fall in between.
    Locker listLocker { parkedList->lock };
    m_condition.notifyOne();
}

// A termination is only ever established (VM::hasTerminationRequest) by the mutator thread itself,
// which cannot do that while it is parked in a wait. Another thread asking this VM to terminate
// (VM::notifyNeedTermination) is visible as the pending NeedTermination trap. Neither interrupts the
// wait while termination is being deferred (DeferTermination); it takes effect when the deferral ends.
static ALWAYS_INLINE bool shouldStopWaitingForTermination(VM& vm)
{
    if (vm.traps().isDeferringTermination()) [[unlikely]]
        return false;
    return vm.hasTerminationRequest() || vm.traps().needHandling(VMTraps::NeedTermination);
}


WaiterListManager& WaiterListManager::singleton()
{
    static LazyNeverDestroyed<WaiterListManager> manager;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        manager.construct();
    });
    return manager;
}

// =============================================================================
// UNGIL §C.6 — D4/D8 lifted together (SD6, one of the TWO recorded both-mode
// deltas; ANNEX A26/§A.2.6; U-T11). Under useJSThreads (BOTH GIL modes) the
// TA sync wait allocates a PER-WAIT node instead of parking the single
// vm.syncWaiter() — per-wait nodes are strictly more correct under the GIL
// too, and they orphan the VMTraps.cpp vm.syncWaiter() termination wakes
// (:364/:465 — BYPASSED, not deleted; flag-off still depends on them), so
// the park polls termination in D9 10ms quanta instead:
//   - GIL-on: the rule-4 VM-WIDE form (parkLitePollTerminationRequested's
//     !gilOff arm — landed jsThreadParkTerminationRequested semantics,
//     watchdog-check folded in; U19 oracle);
//   - GIL-off: the rule-4 PARK LITE's bit (TERM1 rule 4/U31): spawned = the
//     CURRENT lite; main/embedder = the §J.3-CAPTURED lite (the caller's
//     GILDroppedSection released m_lock + token + access via
//     unlockAllForThreadParking, which stashed the carrier lite —
//     capturedParkLiteOfCurrentThreadIfAny). Watchdog-check on a parked
//     CARRIER takes the annex-W W1 early-service episode: full §J.3 exit
//     reacquisition + Watchdog::shouldTerminate under the token
//     (reacquireParkedCarrierAndServiceWatchdogCheck, JSLock.cpp — run with
//     NO rank-3 lock held: listLock is dropped across it), then the r15 F2
//     old-node disposition under listLock:
//       (a) old node already notified/dequeued => the wait completes "ok"
//           immediately — NO re-park, NO fresh node (the consumed notify is
//           honored, never stranded);
//       (b) old node still enqueued un-notified => remove it, tail-enqueue a
//           FRESH node, re-park (FIFO-position loss declared — the existing
//           I10 eats-one-notify class). A notify landing DURING the
//           disposition serializes through listLock and hits exactly one of
//           (a)/(b); at no point are both nodes live past it.
//     A terminate verdict raises VM-wide termination (rule 3) and this park
//     proceeds to its final exit — the wait fails per SD8/§E.5 (unless the
//     node was concurrently notified, in which case the consumed notify is
//     honored as "ok" and the termination is delivered at the caller's next
//     trap poll; because the W1 fire pre-set the consumed-by-servicer shield
//     on the SD8-fail premise, the "ok" disposition re-raises VM-wide to
//     revoke it — see the disposition-(a) comment in the loop — the U-T11
//     "exactly one of ok/timed-out, never both" arm).
// Per-wait nodes make concurrent sync waits on one VM independent, so
// atomicsWaitImpl has no single-flight gate. Flag-off (useJSThreads=false)
// keeps the landed vm.syncWaiter() + waitForSync + central-wake shape
// byte-identical below.
//
// CALLER CONTRACT: every caller parks inside a GILDroppedSection. GIL-off a
// main/embedder caller has m_lock + token + carrier heap access released
// (§J.3); a spawned caller (the 4.5-1a gate is GIL-on only) arrives through
// the section's spawned arm (token kept, heap access released). Parking with
// heap access held would stall the heap §10.4 barrier and the §A.3.2
// conductor predicate for the wait's duration, which the entry assert below
// refuses.
// =============================================================================
template <typename ValueType>
static WaiterListManager::WaitSyncResult waitSyncWithPerWaitNode(VM& vm, Ref<WaiterList> list, ValueType* ptr, ValueType expectedValue, Seconds timeout) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    ASSERT(Options::useJSThreads());

    static constexpr Seconds parkQuantum = Seconds::fromMilliseconds(10); // D9 (§A.2.6; U2's bound).

    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);

    bool gilOff = vm.gilOff();
    bool isSpawned = gilOff && ThreadManager::isJSThreadCurrent();
    VMLite* parkLite = nullptr;
    if (gilOff)
        parkLite = isSpawned ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);

    // GIL-off this site parks in D9 quanta polling termination and carrier
    // watchdog-check only — it never polls the §A.3 stop word — so its
    // stop-convergence argument is entirely that the caller arrives
    // access-released (the GILDroppedSection bracket in AtomicsObject.cpp:
    // carrier and spawned arms alike). A caller parking here with its
    // per-thread client access held would wedge every jettison/Class-A stop
    // into the 30s watchdog fail-stop for up to the full user-specified
    // timeout (potentially Infinity). Fail-stop at entry instead of 30s later
    // with no site named.
    if (gilOff) [[unlikely]] {
        GCClient::Heap* client = GCClient::Heap::currentThreadClient();
        RELEASE_ASSERT(!client || !client->hasHeapAccess());
    }

    Ref<Waiter> waiter = adoptRef(*new Waiter(&vm));

    Locker listLocker { list->lock };
    if (WTF::atomicLoad(ptr) != expectedValue)
        return WaiterListManager::WaitSyncResult::NotEqual;
    list->addLast(listLocker, waiter);
    dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> added a per-wait SyncWaiter=", waiter.get(), " to a waiterList for ptr ", RawPointer(ptr));

    while (waiter->isOnList()) {
        if (parkLitePollTerminationRequested(vm, parkLite))
            break;
        if (MonotonicTime::now() >= deadline)
            break;

        if (gilOff && !isSpawned && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
            // Annex W W1: NO rank-3 lock across the reacquisition — drop
            // listLock for the episode (the waiter stays enqueued; the
            // disposition below resolves every interleaving).
            list->lock.unlock();
            bool terminated = reacquireParkedCarrierAndServiceWatchdogCheck(vm);
            list->lock.lock();
            if (!waiter->isOnList()) {
                // r15 F2 disposition (a): consumed notify honored — completes
                // "ok" via the tail below. If the W1 service ALSO terminated,
                // the shield premise of
                // fireTerminationVMWideAfterParkedCarrierService ("the firing
                // carrier has ALREADY serviced this termination — its park is
                // about to fail") is falsified by this disposition: the park
                // SUCCEEDS, so without correction handleTraps' trim would mask
                // NeedTermination on this carrier and retire it once siblings
                // drain — the watchdog's terminate verdict would be
                // permanently lost on this thread. Re-raise VM-wide: the
                // fresh-raise rule clears m_carrierTookSharedTermination under
                // the registry lock, so the termination is genuinely delivered
                // at the caller's next trap poll (the banner's recorded
                // semantics). Re-setting already-set sibling bits is an
                // idempotent OR; a sibling double-observing termination is
                // inside the landed NeedTermination envelope. Lock context:
                // fireTrapVMWide under this rank-3 listLock takes only the
                // registry lock (leaf) + m_trapSignalingLock, the same
                // held-lock shape as Watchdog::timerDidFire firing under
                // Watchdog::m_lock — no rank-3 lock is ever taken beneath it.
                if (terminated) [[unlikely]]
                    vm.traps().fireTrapVMWide(VMTraps::NeedTermination);
                break;
            }
            if (terminated)
                break; // Final park exit: the wait fails per SD8/§E.5 (the rule-3 bit is set; the tail returns Terminated).
            // r15 F2 disposition (b): remove the un-notified old node and
            // tail-enqueue a FRESH one, then re-park (a NEW acquisition
            // episode — the W1 helper already re-released per §J.3).
            bool removed = list->findAndRemove(listLocker, waiter);
            ASSERT_UNUSED(removed, removed);
            waiter = adoptRef(*new Waiter(&vm));
            list->addLast(listLocker, waiter);
            continue;
        }

#if ENABLE(WEBASSEMBLY_DEBUGGER)
        // Same STW-participation workaround as waitForSync (carrier-only in
        // practice: §I refuses wasm execution on spawned threads).
        if (Options::enableWasmDebugger()) [[unlikely]] {
            if (vm.traps().hasTrapBit(VMTraps::NeedStopTheWorld)) {
                list->lock.unlock();
                VMManager::singleton().notifyVMStop(vm, StopTheWorldEvent::WasmAtomicsWaitBlocked);
                list->lock.lock();
                continue; // Re-check waiter/termination state before any wait.
            }
        }
#endif

        // Quanta poll ONLY lock-free state under the rank-3 lock (U2's
        // bound): waitUntil releases listLock while sleeping.
        waiter->condition().waitUntil(list->lock, std::min(deadline, MonotonicTime::now() + parkQuantum).approximate<WallTime>());
    }

#if ENABLE(WEBASSEMBLY_DEBUGGER)
    if (Options::enableWasmDebugger()) [[unlikely]]
        vm.debugState()->clearStop();
#endif

    // A dequeued waiter was notified: the consumed notify is honored as "ok"
    // even when termination raced the wake (the termination bit stays set
    // and is serviced at the caller's next trap poll).
    if (!waiter->isOnList())
        return WaiterListManager::WaitSyncResult::OK;

    bool removed = list->findAndRemove(listLocker, waiter);
    ASSERT_UNUSED(removed, removed);
    if (parkLitePollTerminationRequested(vm, parkLite)) {
        // Request-then-return: the caller throws via throwTerminationException,
        // which asserts the request flag — a parked thread only ever saw trap
        // BITS while it slept (same shape as the property-wait path,
        // ThreadAtomics.cpp).
        vm.setHasTerminationRequest();
        return WaiterListManager::WaitSyncResult::Terminated;
    }
    return WaiterListManager::WaitSyncResult::TimedOut;
}

template <typename ValueType>
WaiterListManager::WaitSyncResult WaiterListManager::waitSyncImpl(VM& vm, ValueType* ptr, ValueType expectedValue, Seconds timeout)
{
    dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> waitSyncImpl starts totalWaiterCount=", totalWaiterCount());

    // UNGIL §C.6/SD6 (U-T11): per-wait nodes + D9 quanta under useJSThreads,
    // BOTH GIL modes (see the banner above). Flag-off falls through to the
    // landed single-flight vm.syncWaiter() body, byte-identical.
    if (useJSThreadsEnabled()) [[unlikely]]
        return waitSyncWithPerWaitNode(vm, findOrCreateList(ptr), ptr, expectedValue, timeout);

    Ref<Waiter> syncWaiter = vm.syncWaiter();
    Ref<WaiterList> list = findOrCreateList(ptr);
    MonotonicTime time = MonotonicTime::timePointFromNow(timeout);

    {
        VMBlockingScope waitScope(vm, StopTheWorldEvent::AtomicsWaitBlocked);

        Locker listLocker { list->lock };
        if (WTF::atomicLoad(ptr) != expectedValue)
            return WaitSyncResult::NotEqual;

        list->addLast(listLocker, syncWaiter);
        dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> added a new SyncWaiter=", syncWaiter.get(), " to a waiterList for ptr ", RawPointer(ptr));
        syncWaiter->setParkedList(list.copyRef());

        while (syncWaiter->isOnList() && time.now() < time && !shouldStopWaitingForTermination(vm))
            syncWaiter->condition().waitUntil(list->lock, time.approximate<WallTime>());

        syncWaiter->setParkedList(nullptr);

        // At this point, syncWaiter should be either notified (dequeued) or timeout (not dequeued).
        bool didGetDequeued = !syncWaiter->isOnList();
        if (didGetDequeued)
            return WaitSyncResult::OK;

        didGetDequeued = list->findAndRemove(listLocker, syncWaiter);
        ASSERT(didGetDequeued);
        if (!shouldStopWaitingForTermination(vm))
            return WaitSyncResult::TimedOut;
    }

    // The wait was cut short by a termination request: leave with it established and the
    // TerminationException thrown, consuming the trap if another thread's request is what woke us
    // (as VMTraps::handleTraps() would, minus jettisoning the code blocks that have trap breakpoints
    // installed, which throwing from here does not need).
    ASSERT(!vm.traps().isDeferringTermination());
    if (vm.traps().clearTrap(VMTraps::NeedTermination))
        vm.setHasTerminationRequest();
    if (!vm.hasPendingTerminationException())
        vm.throwTerminationException();
    return WaitSyncResult::Terminated;
}

template <typename ValueType>
JSValue WaiterListManager::waitAsyncImpl(JSGlobalObject* globalObject, VM& vm, ValueType* ptr, ValueType expectedValue, Seconds timeout)
{
    dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> waitAsyncImpl starts totalWaiterCount=", totalWaiterCount());

    JSObject* object = constructEmptyObject(globalObject);

    bool isAsync = false;
    JSValue value;

    Ref<WaiterList> list = findOrCreateList(ptr);
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());

    {
        Locker listLocker { list->lock };
        if (WTF::atomicLoad(ptr) != expectedValue)
            value = vm.smallStrings.notEqualString();
        else if (!timeout)
            value = vm.smallStrings.timedOutString();
        else {
            isAsync = true;

            Ref<Waiter> waiter = adoptRef(*new Waiter(promise));
            list->addLast(listLocker, waiter);

            if (timeout != Seconds::infinity()) {
                // Arm the timeout on the VM's run loop, never the calling thread's
                // (SPEC-api 5.6, G28/G26): a spawned JS thread's RunLoop is not pumped
                // and dies with the thread, so a timer armed there would never fire.
                // The VM's run loop is the one DeferredWorkTimer::runRunLoop pumps.
                Ref<RunLoop::DispatchTimer> timer = vm.runLoop().dispatchAfter(timeout, [this, ptr, waiter = waiter.copyRef()]() mutable {
                    timeoutAsyncWaiter(ptr, WTF::move(waiter));
                });
                waiter->setTimer(listLocker, WTF::move(timer));
            }

            dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> added a new AsyncWaiter=", *waiter.ptr(), " to a waiterList for ptr ", RawPointer(ptr));
            value = promise;
        }
    }

    object->putDirect(vm, vm.propertyNames->async, jsBoolean(isAsync));
    object->putDirect(vm, vm.propertyNames->value, value);
    return object;
}

JSValue WaiterListManager::waitAsync(JSGlobalObject* globalObject, VM& vm, int32_t* ptr, int32_t expected, Seconds timeout)
{
    return waitAsyncImpl(globalObject, vm, ptr, expected, timeout);
}

JSValue WaiterListManager::waitAsync(JSGlobalObject* globalObject, VM& vm, int64_t* ptr, int64_t expected, Seconds timeout)
{
    return waitAsyncImpl(globalObject, vm, ptr, expected, timeout);
}

WaiterListManager::WaitSyncResult WaiterListManager::waitSync(VM& vm, int32_t* ptr, int32_t expected, Seconds timeout)
{
    return waitSyncImpl(vm, ptr, expected, timeout);
}

WaiterListManager::WaitSyncResult WaiterListManager::waitSync(VM& vm, int64_t* ptr, int64_t expected, Seconds timeout)
{
    return waitSyncImpl(vm, ptr, expected, timeout);
}

// =============================================================================
// UNGIL §E.7.5/SD11 + §E.4 r17 F2 (U-T9). TA (SharedArrayBuffer)
// Atomics.waitAsync is NOT an AsyncTicket and carries NO §E.3 keepalive: WLM
// settles via DWT scheduleWorkSoon MAIN-side (a spawned registrant's ticket
// took the §E.7.3 internal arm at addPendingWork, so with embedder hooks the
// settle lands in the carrier handoff queue — hooks never see it). The §E.7.5
// registrant re-home is REJECTED v1 (it covers PROPERTY waitAsync only); the
// finite-timeout timer here stays on vm.runLoop() (waitAsyncImpl below) — the
// landed GIL-on-identical shape.
//
// Lock-context fix (gilOffProcess only; flag-off byte-identical): the landed
// notify/timeout paths invoked scheduleWorkSoon while HOLDING list->lock —
// GIL-off that violates the §E.4 settle precondition (r17 F2: no api
// rank-1..3 lock across settle; the §E.7.3 wake must fire with no such lock
// held, r18 F2). The gilOff arms below DECIDE under the lock (dequeue +
// ticket extract + timer clear) and ACT (scheduleWorkSoon) after the drop —
// sound because a dequeued waiter with a cleared ticket is unreachable to
// any racing notifier/timeout (both re-check ticket() under the lock), so
// exactly one settler schedules. The act-after-drop settle is limited to
// waiters whose VM is the settling thread's own (notify: the notifier's VM;
// timeout: the timer fires on the waiter VM's run loop): nothing else keeps a
// waiter's VM alive once list->lock is dropped, and ~VM's unregister(VM*)
// only finds waiters still on a list. A foreign VM's waiter therefore settles
// under list->lock, serialized with that unregister exactly as flag-off.
// =============================================================================

void WaiterListManager::timeoutAsyncWaiter(void* ptr, Ref<Waiter>&& waiter)
{
    dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> timeoutAsyncWaiter ", waiter.get(), ") for ptr ", RawPointer(ptr));
    if (VM::isGILOffProcess()) [[unlikely]] {
        RefPtr<DeferredWorkTimer::Ticket> ticket;
        if (RefPtr<WaiterList> list = findList(ptr)) {
            {
                Locker listLocker { list->lock };
                if (waiter->isOnList()) {
                    bool didGetDequeued = list->findAndRemove(listLocker, waiter);
                    ASSERT_UNUSED(didGetDequeued, didGetDequeued);
                }
                ticket = waiter->ticket(listLocker);
                if (ticket)
                    waiter->clearTicket(listLocker);
                waiter->clearTimer(listLocker);
            }
        } else {
            ASSERT(!waiter->isOnList());
            ticket = waiter->ticket(NoLockingNecessary);
            if (ticket)
                waiter->clearTicket(NoLockingNecessary);
            waiter->clearTimer(NoLockingNecessary);
        }
        if (ticket) {
            VM* waiterVM = waiter->vm();
            waiterVM->deferredWorkTimer->scheduleWorkSoonIfActive(DeferredWorkTimer::WeakTicket { ticket }, [](DeferredWorkTimer::Ticket& dwtTicket) {
                JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket.target());
                JSGlobalObject* globalObject = promise->realm();
                VM& vm = promise->vm();
                promise->resolve(globalObject, vm, vm.smallStrings.timedOutString());
            });
        }
        return;
    }

    if (RefPtr<WaiterList> list = findList(ptr)) {
        Locker listLocker { list->lock };
        if (waiter->isOnList()) {
            bool didGetDequeued = list->findAndRemove(listLocker, waiter);
            ASSERT_UNUSED(didGetDequeued, didGetDequeued);
        }
        notifyWaiterImpl(listLocker,  WTF::move(waiter), ResolveResult::Timeout);
        return;
    }

    ASSERT(!waiter->isOnList());
    notifyWaiterImpl(NoLockingNecessary, WTF::move(waiter), ResolveResult::Timeout);
}

unsigned WaiterListManager::notifyWaiter(VM& vm, void* ptr, unsigned count)
{
    ASSERT(ptr);
    unsigned notified = 0;
    RefPtr<WaiterList> list = findList(ptr);
    if (list) {
        if (VM::isGILOffProcess()) [[unlikely]] {
            // Decide-under-lock / act-after-drop (see banner) for the
            // notifying VM's own async waiters. A waiter of another VM is
            // settled under list->lock: that VM's ~VM runs unregister(VM*)
            // under the same lock, so either the settle reaches its still-live
            // deferredWorkTimer first or the waiter is still listed and is
            // cancelled there. Sync waiters keep the in-lock condition notify
            // (rank-4-class park internals, not a settle).
            Vector<RefPtr<DeferredWorkTimer::Ticket>, 4> pendingSettles;
            {
                Locker listLocker { list->lock };
                while (notified < count && list->size()) {
                    Ref<Waiter> waiter = list->takeFirst(listLocker);
                    if (!waiter->isAsync())
                        waiter->condition().notifyOne();
                    else if (waiter->vm() != &vm)
                        notifyWaiterImpl(listLocker, WTF::move(waiter), ResolveResult::Ok);
                    else {
                        if (auto ticket = waiter->ticket(listLocker)) {
                            pendingSettles.append(WTF::move(ticket));
                            waiter->clearTicket(listLocker);
                        }
                        waiter->clearTimer(listLocker);
                    }
                    notified++;
                }
            }
            for (auto& ticket : pendingSettles) {
                vm.deferredWorkTimer->scheduleWorkSoonIfActive(DeferredWorkTimer::WeakTicket { ticket }, [](DeferredWorkTimer::Ticket& dwtTicket) {
                    JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket.target());
                    JSGlobalObject* globalObject = promise->realm();
                    VM& vm = promise->vm();
                    promise->resolve(globalObject, vm, vm.smallStrings.okString());
                });
            }
        } else {
            Locker listLocker { list->lock };
            while (notified < count && list->size()) {
                notifyWaiterImpl(listLocker, list->takeFirst(listLocker), ResolveResult::Ok);
                notified++;
            }
        }
    }

    dataLogLnIf(WaiterListsManagerInternal::verbose, "<WaiterListManager> <Thread:", Thread::currentSingleton(), "> notified waiters (count ", notified, ") for ptr ", RawPointer(ptr));
    return notified;
}

void WaiterListManager::notifyWaiterImpl(const AbstractLocker& listLocker, Ref<Waiter>&& waiter, const ResolveResult resolveResult)
{
    ASSERT(!waiter->isOnList());

    if (waiter->isAsync()) {
        waiter->scheduleWorkAndClear(listLocker, [resolveResult](DeferredWorkTimer::Ticket& ticket) {
            JSPromise* promise = uncheckedDowncast<JSPromise>(ticket.target());
            JSGlobalObject* globalObject = promise->realm();
            VM& vm = promise->vm();
            JSValue result = resolveResult == ResolveResult::Ok ? vm.smallStrings.okString() : vm.smallStrings.timedOutString();
            promise->resolve(globalObject, vm, result);
        });
        return;
    }

    waiter->condition().notifyOne();
}

size_t WaiterListManager::waiterListSize(void* ptr)
{
    RefPtr<WaiterList> list = findList(ptr);
    size_t size = 0;
    if (list) {
        Locker listLocker { list->lock };
        size = list->size();
    }
    return size;
}

size_t WaiterListManager::totalWaiterCount()
{
    Locker waiterListsLocker { m_waiterListsLock };
    size_t totalCount = 0;
    for (auto& entry : m_waiterLists) {
        Ref<WaiterList> list = entry.value;
        Locker listLocker { list->lock };
        totalCount += list->size();
    }
    return totalCount;
}

void Waiter::scheduleWorkAndClear(const AbstractLocker& listLocker, DeferredWorkTimer::Task&& task)
{
    ASSERT(m_isAsync && m_vm && !isOnList());
    if (m_vm->deferredWorkTimer->scheduleWorkSoonIfActive(m_ticket, WTF::move(task)))
        clearTicket(listLocker);
    clearTimer(listLocker);
}

void Waiter::cancelAndClear(const AbstractLocker& listLocker)
{
    ASSERT(m_isAsync);
    if (auto ticket = this->ticket(listLocker)) {
        m_vm->deferredWorkTimer->cancelPendingWork(*ticket);
        clearTicket(listLocker);
    }
    clearTimer(listLocker);
}

void WaiterListManager::unregister(VM* vm)
{
    Locker waiterListsLocker { m_waiterListsLock };
    for (auto& entry : m_waiterLists) {
        Ref<WaiterList> list = entry.value;
        Locker listLocker { list->lock };
        list->removeIf(listLocker, [&](Waiter* waiter) {
            if (waiter->vm() == vm) {
                dataLogLnIf(WaiterListsManagerInternal::verbose,
                    "<WaiterListManager> <Thread:", Thread::currentSingleton(),
                    "> unregister VM is cancelling waiter=", *waiter,
                    " in WaiterList for ptr ", RawPointer(entry.key));

                // If the vm is about destructing, then it shouldn't
                // been blocked. That means we shouldn't find any SyncWaiter.
                ASSERT(waiter->isAsync());
                waiter->cancelAndClear(listLocker);
                return true;
            }
            return false;
        });
    }
}

void WaiterListManager::unregister(JSGlobalObject* globalObject)
{
    Locker waiterListsLocker { m_waiterListsLock };
    for (auto& entry : m_waiterLists) {
        Ref<WaiterList> list = entry.value;
        Locker listLocker { list->lock };
        list->removeIf(listLocker, [&](Waiter* waiter) {
            if (waiter->isAsync() && waiter->globalObject() == globalObject) {
                dataLogLnIf(WaiterListsManagerInternal::verbose,
                    "<WaiterListManager> <Thread:", Thread::currentSingleton(),
                    "> unregister JSGlobalObject is cancelling waiter=", *waiter,
                    " in WaiterList for ptr ", RawPointer(entry.key));

                waiter->cancelAndClear(listLocker);
                return true;
            }
            return false;
        });
    }
}

void WaiterListManager::unregister(uint8_t* arrayPtr, size_t size)
{
    Locker waiterListsLocker { m_waiterListsLock };
    m_waiterLists.removeIf([&](auto& entry) {
        if (entry.key >= arrayPtr && entry.key < arrayPtr + size) {
            Ref<WaiterList> list = entry.value;
            Locker listLocker { list->lock };
            list->removeIf(listLocker, [&](Waiter* waiter) {
                dataLogLnIf(WaiterListsManagerInternal::verbose,
                    "<WaiterListManager> <Thread:", Thread::currentSingleton(),
                    "> unregister SAB is cancelling waiter=", *waiter,
                    " in WaiterList for ptr ", RawPointer(entry.key));

                // If the SharedArrayBuffer is about destructing, then no VM is
                // referencing the buffer. That means no blocking SyncWaiter
                // on the buffer for any VM.
                ASSERT(waiter->isAsync());
                // If the AsyncWaiter has a valid timer, then let it timeout. Otherwise un-task it.
                // See example, waitasync-timeout-finite-gc.js.
                //
                // OK, let's say if the ticket has a valid timer and its globalObject is about being
                // destructed later but before the timeout. Then, we cannot cancel the work from
                // `unregister(JSGlobalObject* globalObject)` since the waiter is already removed
                // from the lists by this code. So, should we keep it in the list? No, in either
                // case, we have to remove it since all lists associating to the SAB (about destructing)
                // must be removed. This is because there may be a new SAB with a waiter at the same address.
                // Therefore, we will let `clearWeakTickets` to handle this special case.
                if (!waiter->hasTimer(listLocker))
                    waiter->cancelAndClear(listLocker);
                return true;
            });

            ASSERT(!list->size());
            return true;
        }
        return false;
    });
}

Ref<WaiterList> WaiterListManager::findOrCreateList(void* ptr)
{
    Locker waiterListsLocker { m_waiterListsLock };
    return m_waiterLists.ensure(ptr, [] {
        return adoptRef(*new WaiterList());
    }).iterator->value.get();
}

RefPtr<WaiterList> WaiterListManager::findList(void* ptr)
{
    Locker waiterListsLocker { m_waiterListsLock };
    return m_waiterLists.get(ptr);
}

void Waiter::dump(PrintStream& out) const
{
    out.print("[this=");
    out.print(RawPointer(this));
    out.print(", vm=", RawPointer(m_vm));
    out.print(", isAsync=", m_isAsync);
    if (!m_isAsync) {
        out.print("]");
        return;
    }

    auto ticket = this->ticket(NoLockingNecessary);
    out.print(", ticket=", RawPointer(ticket.get()));
    out.print(", globalObject=", RawPointer(m_globalObject));
    if (ticket && !ticket->isCancelled()) {
        out.print(", m_ticket->target=", RawPointer(uncheckedDowncast<JSObject>(ticket->dependencies().last())));
        out.print(", m_ticket->scriptExecutionOwner=", RawPointer(ticket->scriptExecutionOwner()));
    }

    out.print(", m_timer=", RawPointer(m_timer.get()));
    out.print("]");
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
