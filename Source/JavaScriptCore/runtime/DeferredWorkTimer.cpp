/*
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
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
#include "DeferredWorkTimer.h"

#include "GlobalObjectMethodTable.h"
#include "JSGlobalObject.h"
#include "JSLock.h"        // UNGIL §E.7.3 (U-T9): the runloop-dispatch flush fallback locks m_apiLock and re-resolves the VM through it.
#include "ThreadManager.h" // UNGIL §E.7 (U-T9): registrant routing + the cross-thread handoff API.
#include "TopExceptionScope.h"
#include "VM.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DeferredWorkTimer::Ticket);

namespace DeferredWorkTimerInternal {
static constexpr bool verbose = false;
}

// =============================================================================
// UNGIL §E.7 (ANNEX E7 + r17 F3 + r18 F2, BINDING; U-T9) — DWT under N threads.
//
// §E.7.1 NAME EQUATION (K4 VII.4 / AUD1.K5): the spec's m_pendingLock IS the
// in-tree m_taskLock, EXTENDED to m_pendingTickets — whose three-condition
// access comment loses the GIL leg. GIL-off, every m_pendingTickets touch in
// this file is either under m_taskLock or on a path the §F.2 token already
// serializes per registrant; the gilOff-gated Locker additions below close
// the remaining cross-thread holes (addPendingWork from a spawned thread vs
// a carrier doWork). One §LK.7 leaf lock; no second lock for the member set.
//
// PLACEMENT DEVIATION (recorded; DeferredWorkTimer.h is OUTSIDE U-T9's owned
// file set, so no members can be added): the §E.7.3 cross-thread state —
// hookManaged classification (kept as its complement, the INTERNAL-ARM
// ticket set: hooks never see a ticket that skipped onAddPendingWork), the
// m_pendingLock-guarded handoff queue, and the FOURTH hook
// onCrossThreadWorkEnqueued — lives in the per-DWT side table below, guarded
// by its own §LK.7 leaf lock with exactly m_pendingLock's discipline (never
// across user JS; the wake fires strictly AFTER the drop, r17 F3). Folding
// it into the class is a mechanical follow-up once the header is editable.
// Entries are created only for gilOff VMs and purged by ~VM
// (jsThreadsPurgeCrossThreadDeferredWorkAtVMDestruction), so a recycled
// DeferredWorkTimer address cannot alias stale state. Flag-off/GIL-on: no
// entry is ever created; every hot path below costs one vm->gilOff() test.
// =============================================================================

namespace {

struct CrossThreadDWTState {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CrossThreadDWTState);
    Lock lock; // §LK.7 leaf (the side-table half of m_pendingLock — see banner).
    Deque<std::tuple<Ref<DeferredWorkTimer::Ticket>, DeferredWorkTimer::Task>> handoffQueue WTF_GUARDED_BY_LOCK(lock);
    UncheckedKeyHashSet<DeferredWorkTimer::Ticket*> internalArmTickets WTF_GUARDED_BY_LOCK(lock);
    // Installed once at embedder boot (§F.6), before any spawned thread can
    // exist; read thereafter without the lock only on the post-drop wake
    // edge (monotonic install, never uninstalled before the ~VM purge).
    Function<void()> onCrossThreadWorkEnqueued;
    bool wakeHookInstalled WTF_GUARDED_BY_LOCK(lock) { false };
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(CrossThreadDWTState);

} // anonymous namespace

static Lock s_crossThreadDWTRegistryLock;

static Vector<std::pair<DeferredWorkTimer*, std::unique_ptr<CrossThreadDWTState>>>& crossThreadDWTRegistry() WTF_REQUIRES_LOCK(s_crossThreadDWTRegistryLock)
{
    static NeverDestroyed<Vector<std::pair<DeferredWorkTimer*, std::unique_ptr<CrossThreadDWTState>>>> registry;
    return registry.get();
}

static CrossThreadDWTState* crossThreadDWTStateFor(DeferredWorkTimer* timer, bool createIfMissing)
{
    Locker locker { s_crossThreadDWTRegistryLock };
    auto& registry = crossThreadDWTRegistry();
    for (auto& entry : registry) {
        if (entry.first == timer)
            return entry.second.get();
    }
    if (!createIfMissing)
        return nullptr;
    registry.append({ timer, makeUnique<CrossThreadDWTState>() });
    return registry.last().second.get();
}

// m_pendingTickets removal piggyback: an internal-arm mark must die with its
// pending-set membership, or a recycled Ticket address could misroute a
// future hookManaged ticket into the internal arm. Called (gilOff only) at
// every removal/purge point below.
static void crossThreadDWTForgetTicket(DeferredWorkTimer* timer, DeferredWorkTimer::Ticket* ticket)
{
    if (auto* state = crossThreadDWTStateFor(timer, /* createIfMissing */ false)) {
        Locker locker { state->lock };
        state->internalArmTickets.remove(ticket);
    }
}

static bool crossThreadDWTIsInternalArm(DeferredWorkTimer* timer, DeferredWorkTimer::Ticket* ticket)
{
    auto* state = crossThreadDWTStateFor(timer, /* createIfMissing */ false);
    if (!state)
        return false;
    Locker locker { state->lock };
    return state->internalArmTickets.contains(ticket);
}

inline DeferredWorkTimer::Ticket::Ticket(WorkType type, JSObject* scriptExecutionOwner, Vector<JSCell*>&& dependencies)
    : m_type(type)
    , m_dependencies(WTF::move(dependencies))
    , m_scriptExecutionOwner(scriptExecutionOwner)
{
    ASSERT_WITH_MESSAGE(!m_dependencies.isEmpty(), "dependencies shouldn't be empty since it should contain the target");
    ASSERT_WITH_MESSAGE(isTargetObject(), "target must be a JSObject");
    target()->realm()->addWeakTicket(*this);
}

inline Ref<DeferredWorkTimer::Ticket> DeferredWorkTimer::Ticket::create(WorkType type, JSObject* scriptExecutionOwner, Vector<JSCell*>&& dependencies)
{
    return adoptRef(*new Ticket(type, scriptExecutionOwner, WTF::move(dependencies)));
}

inline VM& DeferredWorkTimer::Ticket::vm()
{
    ASSERT(!isCancelled());
    return target()->vm();
}

inline void DeferredWorkTimer::Ticket::cancel()
{
    dataLogLnIf(DeferredWorkTimerInternal::verbose, "Canceling ticket: ", RawPointer(this));
    m_isCancelled.store(true, std::memory_order_relaxed);
}

inline void DeferredWorkTimer::Ticket::cancelAndClear()
{
    cancel();
    m_dependencies.clear();
    m_scriptExecutionOwner = nullptr;
}

bool DeferredWorkTimer::Ticket::isTargetObject()
{
    return m_dependencies.last()->isObject();
}

DeferredWorkTimer::DeferredWorkTimer(VM& vm)
    : Base(vm)
{
}

void DeferredWorkTimer::doWork(VM& vm)
{
    ASSERT(vm.currentThreadIsHoldingAPILock());
    // UNGIL I12 enforcement (I4 review outcome): settle/task execution is a
    // CARRIER drain point ONLY. The two reach paths — the run-loop timer
    // (fires on the VM's run-loop thread, i.e. the carrier that created the
    // VM) and jsThreadsFlushCrossThreadDeferredWork (carrier-asserted) —
    // satisfy this structurally today; a spawned mutator draining m_tasks
    // would run a settle task concurrently with other registrants'
    // in-progress synchronous turns with no happens-before edge (the I12
    // violation class behind the I4 hang triage). Fail-stop instead of
    // silently violating the ordering. GIL-on/flag-off: gilOff() is false,
    // no behavior change.
    if (vm.gilOff()) [[unlikely]]
        RELEASE_ASSERT(!ThreadManager::isJSThreadCurrent());
    Locker locker { m_taskLock };
    cancelTimer();
    if (!m_runTasks)
        return;

    // UNGIL §E.7.3 (U-T9): doWork is a §F.1 carrier drain point — splice the
    // cross-thread handoff queue into the task queue first (re-checked under
    // the state lock after every wake; lock order m_taskLock -> state lock,
    // both leaves, never reversed in this file). GIL-on/flag-off: no state
    // entry exists; one gilOff() test.
    if (vm.gilOff() && !ThreadManager::isJSThreadCurrent()) [[unlikely]] {
        if (auto* state = crossThreadDWTStateFor(this, /* createIfMissing */ false)) {
            Locker stateLocker { state->lock };
            while (!state->handoffQueue.isEmpty()) {
                auto [handoffTicket, handoffTask] = state->handoffQueue.takeFirst();
                m_tasks.append(std::make_tuple(handoffTicket, WTF::move(handoffTask)));
            }
        }
    }

    SUPPRESS_UNCOUNTED_LAMBDA_CAPTURE auto stopRunLoopIfNecessary = makeScopeExit([&] {
        locker.assertIsHolding(m_taskLock);
        if (vm.hasPendingTerminationException()) {
            vm.setExecutionForbidden();
            if (m_shouldStopRunLoopWhenAllTicketsFinish)
                RunLoop::currentSingleton().stop();
            return;
        }

        if (m_shouldStopRunLoopWhenAllTicketsFinish && m_pendingTickets.isEmpty()) {
            ASSERT(m_tasks.isEmpty());
            RunLoop::currentSingleton().stop();
        }
    });

    Vector<std::tuple<Ref<Ticket>, Task>> suspendedTasks;

    while (!m_tasks.isEmpty()) {
        auto [ticket, task] = m_tasks.takeFirst();
        // I4 triage instrumentation (review-demanded evidence): record WHICH
        // thread executes each ticket task, so a settle observed "too early"
        // by a registrant (e.g. a grantDelivered flip inside the registering
        // turn) can be attributed to its executing thread/turn.
        dataLogLnIf(DeferredWorkTimerInternal::verbose, "Doing work on: ", RawPointer(ticket.ptr()), " executing on thread ", RawPointer(&Thread::currentSingleton()), " (spawned JS thread: ", ThreadManager::isJSThreadCurrent(), ")");

        auto pendingTicket = m_pendingTickets.find(ticket.ptr());
        // We may have already canceled this task or its owner may have been canceled.
        if (pendingTicket == m_pendingTickets.end())
            continue;
        ASSERT(ticket.ptr() == pendingTicket->ptr());

        if (ticket->isCancelled()) {
            m_pendingTickets.remove(pendingTicket);
            if (vm.gilOff()) [[unlikely]]
                crossThreadDWTForgetTicket(this, ticket.ptr());
            continue;
        }

        // We shouldn't access the Ticket to get this globalObject until
        // after we confirm that the ticket is still valid (which we did above).
        auto globalObject = ticket->target()->realm();
        switch (globalObject->globalObjectMethodTable()->scriptExecutionStatus(globalObject, ticket->scriptExecutionOwner())) {
        case ScriptExecutionStatus::Suspended:
            suspendedTasks.append(std::make_tuple(WTF::move(ticket), WTF::move(task)));
            continue;
        case ScriptExecutionStatus::Stopped:
            m_pendingTickets.remove(pendingTicket);
            if (vm.gilOff()) [[unlikely]]
                crossThreadDWTForgetTicket(this, ticket.ptr());
            continue;
        case ScriptExecutionStatus::Running:
            break;
        }

        // Remove ticket from m_pendingTickets since we are going to run it.
        // But we want to keep the ticket alive while running task since its globalObject ensures dependencies are strongly held.
        // (m_tasks's Ref<Ticket> already keeps it alive, but we remove from m_pendingTickets to match existing semantics.)
        m_pendingTickets.remove(pendingTicket);
        if (vm.gilOff()) [[unlikely]]
            crossThreadDWTForgetTicket(this, ticket.ptr());

        {
            // Allow tasks we are about to run to schedule work.
            SetForScope<bool> runningTask(m_currentlyRunningTask, true);
            auto dropper = DropLockForScope(locker);

            // This is the start of a runloop turn, we can release any weakrefs here.
            vm.finalizeSynchronousJSExecution();

            auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            task(ticket.get());
            if (Exception* exception = scope.exception()) {
                if (scope.clearExceptionExceptTermination())
                    globalObject->globalObjectMethodTable()->reportUncaughtExceptionAtEventLoop(globalObject, exception);
                else if (vm.hasPendingTerminationException()) [[unlikely]]
                    return;
            }

            vm.drainMicrotasks();
            if (vm.hasPendingTerminationException()) [[unlikely]]
                return;

            scope.assertNoException();
        }
    }

    while (!suspendedTasks.isEmpty())
        m_tasks.prepend(suspendedTasks.takeLast());

    // It is theoretically possible that a client may cancel a pending ticket and
    // never call scheduleWorkSoon() on it. As such, it would not be found when
    // we iterated m_tasks above. We'll need to make sure to purge them here.
    if (vm.gilOff()) [[unlikely]] {
        m_pendingTickets.removeIf([&] (auto& ticket) {
            if (!ticket->isCancelled())
                return false;
            crossThreadDWTForgetTicket(this, ticket.ptr());
            return true;
        });
    } else {
        m_pendingTickets.removeIf([] (auto& ticket) {
            return ticket->isCancelled();
        });
    }
}

void DeferredWorkTimer::runRunLoop()
{
    ASSERT(!m_apiLock->vm()->currentThreadIsHoldingAPILock());
    ASSERT(&RunLoop::currentSingleton() == &m_apiLock->vm()->runLoop());

    // UNGIL §E.7.1/§E.7.4 (U-T9): GIL-off, both the flag write and the
    // emptiness read take the extended m_pendingLock (=m_taskLock) — spawned
    // threads concurrently insert (addPendingWork) and read the flag
    // (cancelPendingWork's recheck arm). Purging already-cancelled tickets
    // under the same hold closes the missed-stop window: a spawned-side
    // cancel that ran BEFORE the flag was set saw it false and dispatched no
    // recheck, but cancel() does not remove from m_pendingTickets — without
    // the purge a set of all-cancelled tickets would park the shell forever.
    // The lock serializes the two cases: a cancel ordered before this
    // critical section is swept by the purge; one ordered after sees the
    // flag true and dispatches the on-loop recheck, which RunLoop::run()
    // then services. The non-gilOff arm below also takes m_taskLock (cold,
    // uncontended), without the purge.
    if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]] {
        bool hasLiveTickets;
        {
            Locker locker { m_taskLock };
            m_shouldStopRunLoopWhenAllTicketsFinish = true;
            m_pendingTickets.removeIf([&](auto& ticket) {
                if (!ticket->isCancelled())
                    return false;
                crossThreadDWTForgetTicket(this, ticket.ptr());
                return true;
            });
            hasLiveTickets = !m_pendingTickets.isEmpty();
        }
        if (hasLiveTickets)
            RunLoop::run();
        return;
    }

    // GIL-on threads: we do NOT hold the API lock here (asserted above), so a
    // spawned thread holding the GIL can be in addPendingWork concurrently.
    // m_taskLock is the only common lock for m_pendingTickets — take it for
    // the flag write and the emptiness probe even when gilOff() is false.
    // Flag-off: uncontended; this path is cold (one acquire per shell run).
    bool hasLiveTickets;
    {
        Locker locker { m_taskLock };
        m_shouldStopRunLoopWhenAllTicketsFinish = true;
        hasLiveTickets = !m_pendingTickets.isEmpty();
    }
    if (hasLiveTickets)
        RunLoop::run();
}

DeferredWorkTimer::WeakTicket DeferredWorkTimer::addPendingWork(WorkType type, VM& vm, JSObject* target, Vector<JSCell*>&& dependencies)
{
    // SharedGC (CVE-audit B8, MC-GC S6): a shared-mode conducted collection
    // runs JSFinalizationRegistry::finalizeUnconditionally on the CONDUCTOR
    // inside the §10 stop window and that conductor is a mutator
    // (GCConductor::Mutator, SPEC-heap §10B.2) — GIL-off it holds no API
    // lock and is not Thread::mayBeGCThread(). Admit that caller via the
    // WSAC disjunct (Heap F7: conductor-written under GBL, every other
    // client NoAccess behind the §10.4 barrier => single-writer here). The
    // ticket then takes the §E.7 internal arm below. Flag-off / non-shared:
    // WSAC is never set, so the disjunct is dead and the legacy two-arm
    // contract is enforced unchanged.
    ASSERT_UNUSED(vm, vm.currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && vm.heap.worldIsStopped()) || vm.heap.worldIsStoppedForAllClients());
    for (unsigned i = 0; i < dependencies.size(); ++i)
        ASSERT(dependencies[i] != target && dependencies[i]);

    auto* globalObject = target->realm();
    JSObject* scriptExecutionOwner = globalObject->globalObjectMethodTable()->currentScriptExecutionOwner(globalObject);
    dependencies.append(target);

    auto ticket = Ticket::create(type, scriptExecutionOwner, WTF::move(dependencies));
    WeakTicket weakTicket { ticket.get() };

    dataLogLnIf(DeferredWorkTimerInternal::verbose, "Adding new pending ticket: ", RawPointer(ticket.ptr()));
    // UNGIL §E.7.3 (U-T9): hookManaged is decided HERE — iff hooks are
    // installed AND the registrant is main/embedder. A SPAWNED registrant's
    // ticket takes the INTERNAL arm on any thread (hooks never see a ticket
    // that skipped onAddPendingWork), recorded in the internal-arm set so
    // every later dispatch site (scheduleWorkSoon, cancelPendingWork) checks
    // it BEFORE the installed-hook branch. GIL-on/flag-off: gilOff() is
    // false and the landed branch shape runs byte-identically.
    if (onAddPendingWork) {
        if (vm.gilOff() && ThreadManager::isJSThreadCurrent()) [[unlikely]] {
            auto* state = crossThreadDWTStateFor(this, /* createIfMissing */ true);
            {
                Locker locker { state->lock };
                state->internalArmTickets.add(ticket.ptr());
            }
            // §E.7.1: m_pendingTickets is m_pendingLock(=m_taskLock)-guarded
            // GIL-off — this add can race a carrier's doWork.
            Locker locker { m_taskLock };
            auto result = m_pendingTickets.add(WTF::move(ticket));
            RELEASE_ASSERT(result.isNewEntry);
        } else
            onAddPendingWork(WTF::move(ticket), type);
    } else if (Options::useJSThreads()) [[unlikely]] {
        // GIL-on threads too: the carrier may be in runRunLoop() WITHOUT the
        // API lock (asserted there), so the GIL does not serialize this add
        // against its emptiness probe. m_taskLock is the only common lock.
        Locker locker { m_taskLock };
        auto result = m_pendingTickets.add(WTF::move(ticket));
        RELEASE_ASSERT(result.isNewEntry);
    } else {
        auto result = m_pendingTickets.add(WTF::move(ticket));
        RELEASE_ASSERT(result.isNewEntry);
    }

    return weakTicket;
}

bool DeferredWorkTimer::hasPendingWork(Ticket& ticket)
{
    // UNGIL §E.7.1 (U-T9): GIL-off, m_pendingTickets is m_pendingLock
    // (=m_taskLock)-guarded — the membership probe can race a spawned
    // thread's addPendingWork. GIL-on/flag-off: landed lock-free shape.
    if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]] {
        Locker locker { m_taskLock };
        auto result = m_pendingTickets.find(&ticket);
        return !(result == m_pendingTickets.end() || ticket.isCancelled());
    }
    auto result = m_pendingTickets.find(&ticket);
    if (result == m_pendingTickets.end() || ticket.isCancelled())
        return false;
    ASSERT(ticket.vm().currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && ticket.vm().heap.worldIsStopped()));
    return true;
}

bool DeferredWorkTimer::hasDependencyInPendingWork(Ticket& ticket, JSCell* dependency)
{
    // UNGIL §E.7.1 (U-T9): same lock extension as hasPendingWork.
    if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]] {
        Locker locker { m_taskLock };
        auto result = m_pendingTickets.find(&ticket);
        if (result == m_pendingTickets.end() || ticket.isCancelled())
            return false;
        return (*result)->dependencies().contains(dependency);
    }
    auto result = m_pendingTickets.find(&ticket);
    if (result == m_pendingTickets.end() || ticket.isCancelled())
        return false;
    ASSERT(ticket.vm().currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && ticket.vm().heap.worldIsStopped()));
    return (*result)->dependencies().contains(dependency);
}

bool DeferredWorkTimer::scheduleWorkSoonIfActive(const WeakTicket& weakTicket, Task&& task)
{
    RefPtr ticket = weakTicket.get();
    if (!ticket || ticket->isCancelled())
        return false;
    // UNGIL §E.7.3 (U-T9): the hookManaged check runs BEFORE the
    // installed-hook branch — an internal-arm ticket (spawned registrant at
    // addPendingWork) never reaches the embedder hooks, on ANY calling
    // thread, including on-carrier. With hooks installed the embedder does
    // NOT pump DWT's timer, so internal-arm work goes to the
    // m_pendingLock-guarded handoff queue, flushed + EXECUTED at the §F.1
    // carrier drain points under the carrier's token; the wake is the FOURTH
    // hook onCrossThreadWorkEnqueued (fallback: vm.runLoop().dispatch of the
    // flush — else a parked-main settle deadlocks). Wake-edge lock contract
    // (r17 F3): append under the state lock; the wake fires strictly AFTER
    // dropping it (and with no api rank-1..3 lock held, r18 F2).
    if (onScheduleWorkSoon) {
        VM* vm = m_apiLock->vm();
        if (vm && vm->gilOff() && crossThreadDWTIsInternalArm(this, ticket.get())) [[unlikely]] {
            auto* state = crossThreadDWTStateFor(this, /* createIfMissing */ true);
            bool wakeViaHook;
            {
                Locker locker { state->lock };
                state->handoffQueue.append(std::make_tuple(ticket.releaseNonNull(), WTF::move(task)));
                wakeViaHook = state->wakeHookInstalled;
            }
            // Post-drop wake. A wake-side race (a carrier drain between the
            // drop and the wake) is benign — spurious wake; the drain
            // re-checks queue-nonempty under the lock.
            if (wakeViaHook)
                state->onCrossThreadWorkEnqueued();
            else {
                // Boot-check fallback (the FOURTH hook is REQUIRED with the
                // other three; absent => drive completion through the VM
                // run loop so nothing strands). The dispatch can outlive the
                // VM — ~VM reaches this site through WaiterListManager::
                // unregister — so it keeps the timer alive, not the VM, and
                // re-resolves the VM under the API lock exactly as
                // JSRunLoopTimer::timerDidFire does (willDestroyVM nulls it).
                vm->runLoop().dispatch([protectedThis = Ref { *this }] {
                    Locker locker { protectedThis->m_apiLock.get() };
                    RefPtr<VM> vm = protectedThis->m_apiLock->vm();
                    if (!vm)
                        return;
                    jsThreadsFlushCrossThreadDeferredWork(*vm);
                });
            }
            return true;
        }
        onScheduleWorkSoon(ticket.releaseNonNull(), WTF::move(task));
        return true;
    }

    // I4 triage instrumentation (review-demanded evidence): record WHICH
    // thread enqueues each settle task on the hookless arm — together with
    // doWork's executing-thread log this localizes any settle that runs
    // concurrently with (or inside) the registering thread's synchronous
    // turn (the I12 ordering edge).
    dataLogLnIf(DeferredWorkTimerInternal::verbose, "Scheduling work on: ", RawPointer(ticket.get()), " from thread ", RawPointer(&Thread::currentSingleton()), " (spawned JS thread: ", ThreadManager::isJSThreadCurrent(), ")");
    Locker locker { m_taskLock };
    m_tasks.append(std::make_tuple(ticket.releaseNonNull(), WTF::move(task)));
    if (!isScheduled() && !m_currentlyRunningTask)
        setTimeUntilFire(0_s);
    return true;
}

// Since Ticket is ThreadSafeWeakPtr now, we should optimize the DeferredWorkTimer's
// workflow, e.g. directly clear the Ticket from cancelPendingWork.
// https://bugs.webkit.org/show_bug.cgi?id=276538
bool DeferredWorkTimer::cancelPendingWork(Ticket& ticket)
{
#if ASSERT_ENABLED
    // UNGIL §E.7.1 (U-T9): GIL-off this membership probe is SKIPPED — the
    // unlocked contains() would race a spawned addPendingWork's locked
    // insert (HashSet rehash vs read), and membership is not even a stable
    // cross-thread invariant: a carrier doWork take()s the ticket out of
    // m_pendingTickets before running its task while a spawned settle path
    // can still cancel it. (Taking m_taskLock here is not an option either:
    // GIL-on cancelPendingWorkSafe calls in already holding it.)
    if (!onCancelPendingWork) {
        VM* assertVM = m_apiLock->vm();
        if (!assertVM || !assertVM->gilOff())
            ASSERT(m_pendingTickets.contains(&ticket));
    }
#endif

    ASSERT(ticket.isCancelled() || ticket.vm().currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && ticket.vm().heap.worldIsStopped()));

    bool result = false;
    if (!ticket.isCancelled()) {
        ticket.cancel();
        result = true;

        // UNGIL §E.7.3/§E.7.4 (U-T9): internal-arm tickets never reach the
        // embedder hooks (hookManaged check before the installed-hook
        // branch); an internal cancel/retire while a shell is parked in
        // runRunLoop (m_shouldStopRunLoopWhenAllTicketsFinish) dispatches an
        // ON-loop emptiness re-check AFTER dropping m_pendingLock (r17 F3) —
        // RunLoop::stop otherwise fires only in DWT's own timer callback and
        // an off-carrier retire would strand the parked shell.
        VM* gilOffVM = nullptr;
        if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]]
            gilOffVM = vm;
        bool internalArm = gilOffVM && crossThreadDWTIsInternalArm(this, &ticket);

        // Script execution context is cleared in ->cancel().
        // But, onCancelPendingWork may dereference the ticket.
        // So your WTF::Function has to be careful about the ticket.
        if (onCancelPendingWork && !internalArm) {
            onCancelPendingWork(ticket);
        }

        if (gilOffVM && !onCancelPendingWork) [[unlikely]] {
            bool needsRunLoopRecheck;
            {
                Locker locker { m_taskLock };
                needsRunLoopRecheck = m_shouldStopRunLoopWhenAllTicketsFinish;
            }
            if (needsRunLoopRecheck) {
                // ~VM reaches this site (WaiterListManager::unregister ->
                // Waiter::cancelAndClear) and the dispatch may run after the
                // VM is gone, so it holds the timer, not the VM; m_runTasks
                // is already false once stopRunningTasks() ran.
                gilOffVM->runLoop().dispatch([protectedThis = Ref { *this }] {
                    DeferredWorkTimer& timer = protectedThis.get();
                    // Refs, not pointers: a freed address could be reused by a new
                    // internal-arm ticket before the forget below runs.
                    Vector<Ref<Ticket>> removed;
                    {
                        Locker locker { timer.m_taskLock };
                        if (timer.m_runTasks && timer.m_shouldStopRunLoopWhenAllTicketsFinish) {
                            timer.m_pendingTickets.removeIf([&](auto& pending) {
                                if (!pending->isCancelled())
                                    return false;
                                removed.append(pending.copyRef());
                                return true;
                            });
                            if (timer.m_pendingTickets.isEmpty() && timer.m_tasks.isEmpty())
                                RunLoop::currentSingleton().stop();
                        }
                    }
                    for (auto& removedTicket : removed)
                        crossThreadDWTForgetTicket(&timer, removedTicket.ptr());
                });
            }
        }
    }

    return result;
}

void DeferredWorkTimer::cancelPendingWorkSafe(JSGlobalObject* globalObject)
{
    dataLogLnIf(DeferredWorkTimerInternal::verbose, "Cancel pending work for globalObject ", RawPointer(globalObject));

    // UNGIL §E.7.4 (U-T9): GIL-off, cancelPendingWork's hookless arm takes
    // m_taskLock for the parked-run-loop recheck, and WTF::Lock is
    // non-recursive — so the walk must NOT hold m_taskLock across the
    // per-ticket cancels. Decide-under-iteration / act-after (the r18 F2
    // shape): snapshot the live tickets as strong refs (the weak set hands
    // out Refs; no DWT lock guards it), cancel with no DWT lock held, then
    // re-take the lock for the timer arm. GIL-on/flag-off: the landed
    // single-lock shape, byte-for-byte (the recheck arm is unreachable —
    // gilOffVM is null — so no re-entry exists there).
    if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]] {
        Vector<Ref<Ticket>> liveTickets;
        for (Ref<Ticket> ticket : *globalObject->m_weakTickets) {
            if (!ticket->isCancelled())
                liveTickets.append(WTF::move(ticket));
        }
        for (auto& ticket : liveTickets)
            cancelPendingWork(ticket.get());
        Locker locker { m_taskLock };
        if (!isScheduled() && !m_currentlyRunningTask)
            setTimeUntilFire(0_s);
        return;
    }

    Locker locker { m_taskLock };
    for (Ref<Ticket> ticket : *globalObject->m_weakTickets) {
        if (!ticket->isCancelled())
            cancelPendingWork(ticket.get());
    }
    if (!isScheduled() && !m_currentlyRunningTask)
        setTimeUntilFire(0_s);
}

void DeferredWorkTimer::cancelPendingWork(VM& vm)
{
    ASSERT(vm.heap.isInPhase(CollectorPhase::End));
    Locker locker { m_taskLock };

    dataLogLnIf(DeferredWorkTimerInternal::verbose, "Cancel pending work for vm ", RawPointer(&vm));
    auto isValid = [&](auto& ticket) {
        bool isTargetGlobalObjectLive = vm.heap.isMarked(ticket->target()->realm());
#if ASSERT_ENABLED
        if (isTargetGlobalObjectLive) {
            for (JSCell* dependency : ticket->dependencies())
                ASSERT(vm.heap.isMarked(dependency));
        }
#endif
        return isTargetGlobalObjectLive && vm.heap.isMarked(ticket->scriptExecutionOwner());
    };

    bool needToFire = false;
    for (auto& ticket : m_pendingTickets) {
        if (ticket->isCancelled() || !isValid(ticket)) {
            // At this point, no one can visit or need the dependencies.
            // So, they are safe to clear here for better debugging and testing.
            ticket->cancelAndClear();
            needToFire = true;

            if (onCancelPendingWork) {
                onCancelPendingWork(ticket.get());
            }
        }
    }

    if (onCancelPendingWork) {
        return;
    }

    // GC can be triggered before an invalid and scheduled ticket is fired. In that case,
    // we also need to remove the corresponding pending task. Since doWork handles all cases
    // for removal, we should let it handle that for consistency.
    if (needToFire && !isScheduled() && !m_currentlyRunningTask)
        setTimeUntilFire(0_s);
}

void DeferredWorkTimer::didResumeScriptExecutionOwner()
{
    ASSERT(!m_currentlyRunningTask);
    Locker locker { m_taskLock };
    if (!isScheduled() && m_tasks.size())
        setTimeUntilFire(0_s);
}

bool DeferredWorkTimer::hasAnyPendingWork() const
{
    ASSERT(m_apiLock->vm()->currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && m_apiLock->vm()->heap.worldIsStopped()));
    // UNGIL §E.7.1 (U-T9): GIL-off the emptiness read takes the extended
    // m_pendingLock (const_cast: the header — where `mutable` would go — is
    // outside U-T9's owned set; recorded with the placement deviation).
    if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]] {
        Locker locker { const_cast<Lock&>(m_taskLock) };
        return !m_pendingTickets.isEmpty();
    }
    return !m_pendingTickets.isEmpty();
}

bool DeferredWorkTimer::hasImminentlyScheduledWork() const
{
    ASSERT(m_apiLock->vm()->currentThreadIsHoldingAPILock() || (Thread::mayBeGCThread() && m_apiLock->vm()->heap.worldIsStopped()));
    // UNGIL §E.7.1 (U-T9): GIL-off the iteration takes the extended
    // m_pendingLock — the precondition above no longer excludes concurrent
    // mutation (the §F.2 token is per-thread; a spawned addPendingWork's
    // locked insert can rehash under this probe). Same const_cast note as
    // hasAnyPendingWork (the header is outside U-T9's owned set).
    if (VM* vm = m_apiLock->vm(); vm && vm->gilOff()) [[unlikely]] {
        Locker locker { const_cast<Lock&>(m_taskLock) };
        for (auto& ticket : m_pendingTickets) {
            if (ticket->isCancelled())
                continue;
            if (ticket->type() == WorkType::ImminentlyScheduled)
                return true;
        }
        return false;
    }
    for (auto& ticket : m_pendingTickets) {
        if (ticket->isCancelled())
            continue;
        if (ticket->type() == WorkType::ImminentlyScheduled)
            return true;
    }
    return false;
}

// ---------------- UNGIL §E.7.3 exported seams (U-T9; declared in
// ThreadManager.h — recorded placement deviation, see the banner). ----------

void jsThreadsSetOnCrossThreadWorkEnqueued(VM& vm, Function<void()>&& wake)
{
    // Embedder boot install (§F.6; with the other three DWT hooks, before
    // any spawned thread exists). Contract: invoked with NO JSC lock held;
    // never reenters JSC; never runs JS.
    RELEASE_ASSERT(vm.gilOff());
    RELEASE_ASSERT(wake);
    auto* state = crossThreadDWTStateFor(vm.deferredWorkTimer.ptr(), /* createIfMissing */ true);
    Locker locker { state->lock };
    RELEASE_ASSERT(!state->wakeHookInstalled); // install-once (monotonic; read lock-free on the wake edge).
    state->onCrossThreadWorkEnqueued = WTF::move(wake);
    state->wakeHookInstalled = true;
}

void jsThreadsFlushCrossThreadDeferredWork(VM& vm)
{
    if (!vm.gilOff())
        return;
    ASSERT(vm.currentThreadIsHoldingAPILock());
    ASSERT(!ThreadManager::isJSThreadCurrent()); // §F.1 carrier drain points only.
    // doWork splices the handoff queue under m_taskLock (its gilOff carrier
    // arm) and executes everything under this carrier's token — incl. the
    // E.4(b) retire + m_promise clear inside each task body. The drain
    // re-checks queue-nonempty after every wake (r17 F3): a spurious wake
    // finds the queues empty and returns.
    vm.deferredWorkTimer->doWork(vm);
}

void jsThreadsPurgeCrossThreadDeferredWorkAtVMDestruction(VM& vm)
{
    std::unique_ptr<CrossThreadDWTState> state;
    {
        Locker locker { s_crossThreadDWTRegistryLock };
        auto& registry = crossThreadDWTRegistry();
        for (size_t i = 0; i < registry.size(); ++i) {
            if (registry[i].first == vm.deferredWorkTimer.ptr()) {
                state = WTF::move(registry[i].second);
                registry.removeAt(i);
                break;
            }
        }
    }
    if (!state)
        return;
    // Queued-but-never-drained cross-thread work is dropped — the same
    // declared class as SD15's exit-before-drain reports (the tickets'
    // pending-work registrations die with DWT's own shutdown
    // cancelPendingWork). Task lambdas (and their Ref<AsyncTicket>/
    // Ref<Ticket> captures) are destroyed here, outside any lock, on the
    // destroying thread.
    Deque<std::tuple<Ref<DeferredWorkTimer::Ticket>, DeferredWorkTimer::Task>> doomed;
    {
        Locker locker { state->lock };
        doomed = std::exchange(state->handoffQueue, { });
        state->internalArmTickets.clear();
    }
}

} // namespace JSC
