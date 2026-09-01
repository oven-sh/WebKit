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
#include "ThreadObject.h"

#include "ArrayBufferSharingMode.h"
#include "ConcurrentButterflyOperations.h"
#include "CustomGetterSetter.h"
#include "ErrorInstance.h"
#include "ErrorInstanceInlines.h"
#include "ExceptionHelpers.h"
#include "InternalFunction.h" // SCALEBENCH §37 R1: createSubclassStructure for the constructThread newTarget!=callee arm.
#include "ClassInfo.h"
#include "JSArray.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "JSTypedArrayViewConstructor.h"
#include "JSTypedArrayViewPrototype.h"
#include "LockObject.h"
#include "JSGlobalProxy.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ProxyObject.h"
#include "TopExceptionScope.h"
#include "TypedArrayController.h"
#include "TypedArrayType.h"
#include "VMLite.h"
#include "VMLiteShared.h"
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/MainThread.h>
#include <wtf/RawPointer.h>
#include <wtf/StackAllocation.h>

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

const ClassInfo JSThread::s_info = { "Thread"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSThread) };

static JSC_DECLARE_HOST_FUNCTION(callThread);
static JSC_DECLARE_HOST_FUNCTION(constructThread);
static JSC_DECLARE_HOST_FUNCTION(threadProtoFuncJoin);
static JSC_DECLARE_HOST_FUNCTION(threadProtoFuncAsyncJoin);
static JSC_DECLARE_HOST_FUNCTION(threadFuncRestrict);
static JSC_DECLARE_CUSTOM_GETTER(threadCurrentGetter);
static JSC_DECLARE_CUSTOM_GETTER(threadIdGetter);
static JSC_DECLARE_HOST_FUNCTION(callConcurrentAccessError);
static JSC_DECLARE_HOST_FUNCTION(constructConcurrentAccessError);

JSThread::JSThread(VM& vm, Structure* structure, Ref<ThreadState>&& state)
    : Base(vm, structure)
    , m_state(WTF::move(state))
{
}

JSThread* JSThread::create(VM& vm, Structure* structure, Ref<ThreadState>&& state)
{
    JSThread* thread = new (NotNull, allocateCell<JSThread>(vm)) JSThread(vm, structure, WTF::move(state));
    thread->finishCreation(vm);
    return thread;
}

void JSThread::destroy(JSCell* cell)
{
    static_cast<JSThread*>(cell)->JSThread::~JSThread();
}

bool jsThreadsCanBlockOnCurrentThread(VM& vm)
{
    return vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread();
}

// ---------------- spawn / run / completion ----------------

// SPEC-api 5.10 finalizer hook: registered exactly once per ThreadState, at
// TS::jsThread creation (spawner under the GIL pre Thread::create, or the
// first lazy-TS Strong). Uses only the public Heap API (no VM.h/.cpp edit).
// The lambda holds a Ref<ThreadState> and is the SOLE clearer of TS::result;
// it also clears any still-set jsThread/threadLocals Strongs (lazy
// main/embedder TSs have no completion sequence — their Strongs die here, at
// cell death or VM teardown via lastChanceToFinalize(), with the JSLock
// held, satisfying the 5.10 rule).
static void registerThreadStateFinalizer(VM& vm, JSThread* thread, ThreadState& state)
{
    vm.heap.addFinalizer(thread, [protectedState = Ref { state }](JSCell*) {
        protectedState->jsThread.clear();
        protectedState->threadLocals.clear();
        protectedState->result.clear();
        // Drain never-settled asyncJoin tickets. asyncJoin on a thread that
        // never runs the completion sequence (lazy main/embedder tid-0
        // ThreadStates; or any receiver at VM teardown) leaves its tickets
        // in asyncJoiners with no other clearing point: the last TS ref can
        // then drop at an embedder thread's TLS teardown, OFF the JSLock and
        // possibly after VM death, destroying still-set Strong<JSPromise>es
        // (5.10 violation / VM UAF; ~ThreadState now asserts emptiness).
        // This hook runs with the JSLock held (GC finalization /
        // lastChanceToFinalize), so clearing the promise Strongs here is the
        // 5.10-legal point. Tickets drained here were never passed to
        // settleJoinTicket (the completion sequence and asyncJoin's
        // settleNow path both operate on tickets already swapped OUT of
        // asyncJoiners), so no settle task can later read the cleared
        // Strong; their DWT pending work falls to the VM-shutdown
        // cancelPendingWork backstop (SPEC-api 5.5).
        Vector<Ref<AsyncTicket>> abandonedJoiners;
        {
            Locker joinLocker { protectedState->joinLock };
            abandonedJoiners = std::exchange(protectedState->asyncJoiners, { });
        }
        for (auto& ticket : abandonedJoiners)
            ticket->promise().clear();
    });
}

// F5 settle path: schedules the ticket's settle task via the 5.5 protocol
// (DeferredWorkTimer::scheduleWorkSoon), so the promise settles on a run-loop
// turn, never synchronously inside asyncJoin() or the completion sequence
// (I12). In the GIL phase the settling thread is whichever one drains the
// single shared VM queue (5.5 relaxation); the registering thread may already
// be dead — the ticket outlives it (4.6.2).
static void settleJoinTicket(AsyncTicket& ticket, JSThread* thread, bool failed)
{
    // `thread` is rooted by the ticket's dependency vector; the promise by
    // the ticket's Strong (and the DWT ticket target), both until settle.
    ticket.settle([protectedTicket = Ref { ticket }, thread, failed](DeferredWorkTimer::Ticket) {
        JSPromise* promise = protectedTicket->promise().get();
        JSGlobalObject* globalObject = promise->realm();
        VM& vm = globalObject->vm();
        // The result Strong was written before the Phase release-store (F1);
        // every settle observes phase != Running, so the read is ordered.
        if (failed)
            promise->reject(vm, thread->result());
        else
            promise->resolve(globalObject, vm, thread->result());
    });
}

static void threadMain(VM& vm, Ref<ThreadState> state)
{
    state->nativeThread = &Thread::currentSingleton();
    setCurrentThreadState(state.copyRef());
    // THREADS-INTEGRATE(api): VMLite + butterfly-TID-tag handshake
    // (SPEC-api 5.2 / vmstate 6.4.4 / jit P5+CS3), before the JSLockHolder.
    const bool gilOff = vm.gilOff();
    auto lite = makeUnique<VMLite>();
    lite->tid = state->tid; // TM is the sole TID allocator; written before setCurrent (vmstate 6.7)
    // UNGIL §A.1.3 level-2 byte: stamped BEFORE registerLite publishes the
    // lite to registry walkers (same write-once-at-registration clause as
    // VM.cpp's m_mainVMLite stamp and JSLock.cpp's carrier registration),
    // and before the first JSLockHolder's spawnedThreadEntryTokenLock
    // consults it (§F.1 U1 backstop, JSLock.cpp:523).
    lite->gilOff = gilOff ? 1 : 0;
    VMLiteRegistry::singleton().registerLite(*lite, vm); // sole writer of lite->vm (vmstate 6.5.1)
    // ANNEX A16 (UNGIL U-T1): registration backfill of the baked-index
    // scratch buffers, AFTER registerLite so a concurrent
    // VM::allocateBakedScratchBufferIndex install fan cannot be lost (both
    // sides are idempotent under the per-lite scratchBufferLock). The JSLock
    // carrier registration (JSLock.cpp) has the same call; without it here, a
    // spawned thread whose VM allocated indices BEFORE the spawn reads a null
    // (lite, index) slot in the shared OSR-exit thunks/ramps and stores
    // through it — the i03-t5-racing-growers / i03-restart-locked-vs-conversion
    // GIL-off DFG-tier SIGSEGV.
    lite->backfillBakedScratchBuffers();
    VMLite::setCurrent(lite.get());
    initializeButterflyTIDTagForCurrentThread(); // jit P5; after setCurrent, before any JS (CS3)
    if (gilOff) {
        // SPEC-ungil E2A integration shape (recorded in the ThreadManager.h
        // banner; AB-12): §B.1 per-thread GC client attach — stamps
        // lite->clientHeap (EXIT1.4(b) release-publish) and the §10A.1
        // currentThreadClient slot, satisfying the §F.1 A36C clause checked
        // at the first JSLockHolder (JSLock.cpp:524) — then the §E.1 inbox
        // open, both BEFORE fn. attachCurrentThread's gated acquire parks
        // across any in-flight stop-the-world, so a conductor mid-stop never
        // observes this thread entering JS without heap access.
        attachSpawnedThreadGCClient(vm, *lite);
        openThreadInbox(state.get());
    }
    ThreadState::Phase phase = ThreadState::Phase::Failed;
    {
        // The GIL: all JS execution is serialized by the shared VM's JSLock
        // (SPEC-api 5.2). Atom table and stack limits migrate on acquisition.
        JSLockHolder locker(vm);
        // Start from clean per-thread execution state: the previous GIL
        // holder may have left pointers into another (possibly dead)
        // thread's stack in the VM (e.g. the EXCEPTION_SCOPE_VERIFICATION
        // scope chain); see GILParkSavedExecutionState (LockObject.h).
        // GIL-on only: GIL-off there is no holder migration, per-thread
        // execution state is per-lite (§A.1.4), and writing VM-wide fields
        // here would race other running mutators.
        if (!gilOff)
            GILParkSavedExecutionState::resetForFreshThread(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

        JSThread* thread = uncheckedDowncast<JSThread>(state->jsThread.get());
        JSObject* function = asObject(state->fnSlot.get());
        JSGlobalObject* globalObject = function->globalObject();

        MarkedArgumentBuffer args;
        for (auto& slot : state->argSlots)
            args.append(slot.get());
        ASSERT(!args.hasOverflowed());

        auto callData = JSC::getCallData(function);
        NakedPtr<Exception> exception;
        JSValue callResult = JSC::call(globalObject, function, callData, jsUndefined(), args, exception);

        // AB18-I publish-side guard (applied per the review round, NOT the
        // original single-domain proposal): JSC::call's NakedPtr capture
        // (CallData.cpp) reads and clears the CURRENT lite's group-3
        // exception word through its own TopExceptionScope. Re-reading that
        // SAME per-lite word here would be dead code — two sequenced
        // same-thread reads of one word are coherent, and no foreign writer
        // of another thread's per-lite word exists (VM::setException asserts
        // the API lock; the only async-flavored clear is the current-thread,
        // termination-only deferral). So the V1 lost-throw signature requires
        // the exception to live in a DIFFERENT storage domain than the one
        // the capture read — the U-T4 class: a not-yet-rerouted GIL-off path
        // storing into (or a lite-selection change redirecting the capture
        // away from) the inert VM-block spare word. Before allowing
        // Phase::Finished, read BOTH domains — this thread's per-lite word
        // AND the VM-block spare word (GIL-off the main carrier never
        // installs m_mainVMLite, so the VM block is inert spare storage; a
        // non-null value there is exactly the unrerouted-path evidence) —
        // and fold a residual NON-TERMINATION exception into the capture so
        // a recorded throw can never be published as Finished. A residual
        // TERMINATION exception is deliberately left in place for the drain
        // loop's vm.hasTerminationRequest() arm, which takes the TERM1.3
        // Failed publication with the fresh "Thread terminated" error.
        //
        // The [AB18-I] log line is the verify round's diagnostic: if the V1
        // loss reproduces WITH a line, the line names the losing domain (and
        // whether the lite changed across the call); if it reproduces with NO
        // line, both domains were already null at fn-return — the consumed-
        // before-capture variant — and the hunt moves to the throw/unwind-
        // side U-T4 audit (Interpreter::unwind / genericUnwind storage
        // selection, DFG/FTL OSR-exit + exception-handler entry checks,
        // vmEntryToJavaScript epilogue, LLInt catch/clear paths).
        //
        // GIL-on (incl. flag-off) this block is never entered, so identity
        // holds byte-for-byte.
        if (gilOff && !exception) {
            Exception* perLiteResidual = WTF::atomicLoad(&lite->primitives.m_exception, std::memory_order_relaxed);
            Exception* vmBlockResidual = WTF::atomicLoad(&vm.mainVMLitePrimitives().m_exception, std::memory_order_relaxed);
            VMLite* currentLite = VMLite::currentIfExists();
            if (perLiteResidual || vmBlockResidual || currentLite != lite.get()) [[unlikely]] {
                dataLogLn("[AB18-I] fn-return residual exception state: perLiteWord=", RawPointer(perLiteResidual),
                    " vmBlockWord=", RawPointer(vmBlockResidual),
                    " currentLite=", RawPointer(currentLite),
                    " entryLite=", RawPointer(lite.get()),
                    " tid=", state->tid);
                Exception* residual = perLiteResidual ? perLiteResidual : vmBlockResidual;
                if (residual && !vm.isTerminationException(residual)) {
                    exception = residual;
                    // Consume the folded word ourselves: the clearException()
                    // in the Failed arm below clears only the CURRENT lite's
                    // word and would leave a VM-block residual set for a
                    // later reader to double-fold.
                    if (perLiteResidual)
                        WTF::atomicStore(&lite->primitives.m_exception, static_cast<Exception*>(nullptr), std::memory_order_relaxed);
                    else
                        WTF::atomicStore(&vm.mainVMLitePrimitives().m_exception, static_cast<Exception*>(nullptr), std::memory_order_relaxed);
                }
            }
        }

        state->fnSlot.clear();
        state->argSlots.clear();

        JSValue resultValue;
        if (exception) {
            resultValue = exception->value();
            phase = ThreadState::Phase::Failed;
            scope.clearException();
        } else {
            resultValue = callResult;
            phase = ThreadState::Phase::Finished;
        }

        if (gilOff) {
            // E2A precondition (ThreadManager.h:806-811): F1 — the result
            // Strong is stored under THIS thread's token, BEFORE the drain
            // loop. Everything else the GIL tail does at fn-return — the
            // Phase release-store + joinCondition.notifyAll, the F5 ticket
            // settles, the 5.10 Strong clears, unregisterThread, and the
            // lite/client teardown — happens at CLOSE (U7/SD1: a thread
            // completes only when its inbox closes, not at fn-return),
            // inside closeThreadInboxAndComplete /
            // tearDownSpawnedThreadForExit, called below after the token
            // drops. Doing them here would settle joins before queued
            // ThreadTasks ran and unregister the lite while the drain loop
            // still needs it.
            state->result.set(vm, resultValue ? resultValue : jsUndefined());
        } else {
            // 5.2 yield-point ordering for the completion drain below: let
            // parked-but-releasable waiters resume first. Without this, this
            // thread goes from its hold() epilogue (m_lock release) straight to
            // the 4.6.1 drain while the waiter it just notified/unblocked is
            // still inside its blocking host call, and the drain runs inside
            // that waiter's park — the D11 violation
            // park-no-microtask-drain.js asserts against. Bounded by the
            // progress-reset deadline in jsThreadYieldForPendingParkResumptions
            // (LockObject.cpp), so a join-under-hold cycle can only delay,
            // never deadlock, completion. NOTE: this wait-before-drain is a
            // GIL-stub deviation from the frozen SPEC-api 4.6.1 ordering of the
            // same kind as the recorded D2 notify-yield deviation (the spec's
            // "never waits" applies to tickets; this waits only on park
            // resumptions, bounded).
            jsThreadYieldForPendingParkResumptions(vm);
            // Completion sequence (SPEC-api 4.6.1), still under the JSLock:
            // drain the shared VM microtask queue once (GIL-phase rule;
            // post-GIL: own queue until empty).
            vm.drainMicrotasks();
            if (scope.exception()) [[unlikely]]
                scope.clearException();

            // F1: the result Strong is written BEFORE the Phase release-store
            // below; join() readers load-acquire Phase first (redundant under
            // the GIL, load-bearing post-GIL). Cleared only by the 5.10
            // finalizer hook.
            state->result.set(vm, resultValue ? resultValue : jsUndefined());

            // F5 completion protocol: under joinLock — Phase release-store,
            // joinCondition.notifyAll(), swap asyncJoiners out; drop joinLock;
            // settle the moved tickets via the 5.5 schedule. Never waits for
            // tickets (4.6.1).
            Vector<Ref<AsyncTicket>> joiners;
            {
                Locker joinLocker { state->joinLock };
                state->phase.store(phase, std::memory_order_release);
                state->joinCondition.notifyAll();
                joiners = std::exchange(state->asyncJoiners, { });
            }
            bool failed = phase == ThreadState::Phase::Failed;
            for (auto& ticket : joiners)
                settleJoinTicket(ticket, thread, failed);

            // Clear owned Strongs (still under the JSLock; SPEC-api 5.10).
            // TS::result is NOT cleared here — the finalizer hook is its sole
            // clearer (it must survive for join()/asyncJoin() readers, who keep
            // the JSThread cell, and hence the hook, alive).
            state->threadLocals.clear();
            state->jsThread.clear();
            ThreadManager::singleton().unregisterThread(state);

            // THREADS-INTEGRATE(api): VMLite teardown (SPEC-api 5.2 / vmstate
            // N8; registry lock is a 5.9-legal leaf), still under the final
            // JSLock. The TID is retired forever (Deviation 10).
            VMLiteRegistry::singleton().unregisterLite(*lite);
            VMLite::setCurrent(nullptr);
            clearButterflyTIDTagForCurrentThread();
        }
    }
    if (gilOff) {
        // ANNEX E2A (AB-12): post-fn drain loop, §E.5 close (termination
        // traps route through the same close block with Phase::Failed), F5
        // completion against state, then the EXIT1.3 teardown via
        // tearDownSpawnedThreadForExit — which ends with
        // VMLite::setCurrent(nullptr) (the butterfly TID tag clears through
        // the setCurrent hook) and unregisterVMLiteAndNotifyTeardown.
        // Preconditions hold here: owning spawned thread, no lock and no
        // token (the JSLockHolder scope above closed — its depth-0 unlock
        // released heap access per §F.1; the loop re-acquires, gated),
        // state->result already stored (F1), lite->clientHeap attached.
        runSpawnedThreadDrainLoopAndClose(vm, *lite, state.get(), phase);
    }
    lite = nullptr; // freed AFTER teardown / the JSLock release (EXIT1.9 residual-tail rule / SPEC-api 4.6.1); ~VMLite asserts uninstalled+unregistered
    setCurrentThreadState(nullptr);
}

JSC_DEFINE_HOST_FUNCTION(callThread, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Thread constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructThread, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue functionValue = callFrame->argument(0);
    auto callData = JSC::getCallData(functionValue);
    if (callData.type == CallData::Type::None)
        return throwVMTypeError(globalObject, scope, "Thread constructor requires a callable argument"_s);

    // SCALEBENCH §37 R1: per-realm cached instance Structure (set in
    // createThreadProperty). Same fresh-Structure-per-new fix as constructLock
    // (LockObject.cpp); the load-bearing case is K shard locks but Thread had
    // the identical bug. The newTarget!=callee arm calls createSubclassStructure
    // which can run JS / throw — keep it BEFORE allocateSpawnedThreadState so a
    // throw cannot leak a forever-Running entry in ThreadManager::m_threads
    // (counted against maxJSThreads, I17). Everything after the allocation is
    // infallible up to Thread::create. Flag-off this function is unreachable.
    Structure* structure = globalObject->threadObjectStructure();
    ASSERT(structure);
    JSValue newTarget = callFrame->newTarget();
    if (newTarget != callFrame->jsCallee()) [[unlikely]] {
        structure = InternalFunction::createSubclassStructure(globalObject, asObject(newTarget), structure);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // UNGIL U0b (AB-11 migration): the VM-aware overload licenses spawns
    // from the m_gilOff winner VM and refuses loser VMs (null -> RangeError
    // below). It also requests a full collection when an exhausted winner
    // spawn left a Sealed rebias snapshot (SD9 liveness), so the RangeError
    // window closes without organic allocation pressure. Flag-off/GIL-on:
    // identical to the VM-blind form (both reduce to
    // allocateSpawnedThreadStateInternal()).
    RefPtr<ThreadState> state = ThreadManager::singleton().allocateSpawnedThreadState(vm);
    if (!state)
        return throwVMRangeError(globalObject, scope, "too many live Threads (or thread-ID space exhausted)"_s);

    JSThread* thread = JSThread::create(vm, structure, Ref { *state });

    // Root the cell, the function, and the arguments across the spawn
    // (SPEC-api 5.10); all created while holding the GIL, BEFORE
    // Thread::create (no spawn->run UAF window). The 5.10 finalizer hook is
    // registered at jsThread creation.
    state->jsThread.set(vm, thread);
    registerThreadStateFinalizer(vm, thread, *state);
    state->fnSlot.set(vm, functionValue);
    for (size_t i = 1; i < callFrame->argumentCount(); ++i)
        state->argSlots.append(Strong<Unknown>(vm, callFrame->uncheckedArgument(i)));

    StackAllocationSpecification stackSpecification;
    if (unsigned stackKB = Options::jsThreadStackSizeKB())
        stackSpecification = StackAllocationSpecification::RequestSize(static_cast<size_t>(stackKB) * 1024);

    // Detach the native handle: join() synchronizes through
    // ThreadState::joinCondition, never through the pthread handle, so
    // keeping it joinable only leaks it (reported by TSAN as a thread leak
    // on every spawn).
    // Capture the VM by Ref: the native handle is detached, so nothing else
    // guarantees the VM outlives this thread. If the embedder drops its last
    // ref while we are running (e.g. main script exits without join()), the
    // VM must stay alive until threadMain returns or this is a use-after-free.
    Thread::create("JS Thread"_s, [state = Ref { *state }, protectedVM = Ref { vm }]() mutable {
        threadMain(protectedVM.get(), WTF::move(state));
    }, ThreadType::JavaScript, Thread::QOS::UserInitiated, Thread::defaultSchedulingPolicy, stackSpecification)->detach();

    return JSValue::encode(thread);
}

// ---------------- join / asyncJoin ----------------

JSC_DEFINE_HOST_FUNCTION(threadProtoFuncJoin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSThread* thread = dynamicDowncast<JSThread>(callFrame->thisValue());
    if (!thread)
        return throwVMTypeError(globalObject, scope, "Thread.prototype.join called on incompatible receiver"_s);

    ThreadState& state = thread->threadState();
    if (currentThreadStateIfExists() == &state)
        return throwVMError(globalObject, scope, createError(globalObject, "Thread cannot join itself"_s));

    if (state.phase.load(std::memory_order_acquire) == ThreadState::Phase::Running) {
        if (!jsThreadsCanBlockOnCurrentThread(vm))
            return throwVMTypeError(globalObject, scope, "Thread.prototype.join cannot block the current thread"_s);
        bool terminated = false;
        {
            // Release the GIL while blocked — depth-free (GILDroppedSection,
            // LockObject.h): concurrent joiners wake in arbitrary order,
            // which DropAllLocks' strict-LIFO unwind protocol livelocks on
            // (observed in the join chains of lifecycle/join-semantics.js).
            GILDroppedSection droppedSection(vm);

            // UNGIL §A.2.4 rule 4 / annex W W1 (AB-17 item 4): park-lite D9
            // polls + the W1 watchdog split (see the contended-hold loop in
            // LockObject.cpp). GIL-on byte-equivalent.
            const bool gilOff = vm.gilOff();
            const bool isSpawnedParker = gilOff && ThreadManager::isJSThreadCurrent();
            VMLite* parkLite = nullptr;
            if (gilOff)
                parkLite = isSpawnedParker ? VMLite::currentIfExists() : capturedParkLiteOfCurrentThreadIfAny(vm);

            Locker joinLocker { state.joinLock };
            // F5 wait, in 10ms waitUntil quanta polling
            // jsThreadParkTerminationRequested() (landed deviation D9,
            // docs/threads/INTEGRATE-api.md): VMTraps cannot wake a thread
            // parked in joinCondition, so an unbounded wait is unkillable
            // under the watchdog if the joinee never completes (e.g. it is
            // wedged in native code the termination machinery cannot
            // reach). Belt-and-braces in practice — every park site the
            // joinee can occupy now polls termination itself, so the joinee
            // normally completes (Failed) and wakes us — but the joiner
            // must not depend on that. The quantum wait holds joinLock only
            // between sleeps (5.9(a3)); a completion observed under the
            // lock takes priority over a concurrent termination request.
            while (state.phase.load(std::memory_order_acquire) == ThreadState::Phase::Running) {
                if (parkLitePollTerminationRequested(vm, parkLite)) {
                    terminated = true;
                    break;
                }
                if (gilOff && !isSpawnedParker && parkLitePollWatchdogCheckRequested(vm, parkLite)) [[unlikely]] {
                    // W1 episode: drop joinLock for the reacquisition (no
                    // native lock may be held across it); the loop re-checks
                    // phase under the re-taken lock, so a completion that
                    // raced the episode wins as before.
                    bool watchdogTerminated;
                    {
                        DropLockForScope episodeUnlocker { joinLocker };
                        watchdogTerminated = reacquireParkedCarrierAndServiceWatchdogCheck(vm);
                    }
                    if (watchdogTerminated) {
                        terminated = true;
                        break;
                    }
                    continue;
                }
                state.joinCondition.waitUntil(state.joinLock, MonotonicTime::now() + Seconds::fromMilliseconds(10));
            }
        }
        if (terminated) {
            // Back under the GIL (the dropped section ended), no native
            // lock held. Same surfacing as the 5.6-7 property-wait path,
            // in the handleTraps shape (request-then-throw:
            // throwTerminationException ASSERTs the request flag, which a
            // parked thread never had set — only trap BITS were raised
            // while we slept).
            vm.setHasTerminationRequest();
            vm.throwTerminationException();
            return { };
        }

        // GIL-off realization of the SPEC-api 4.6.1 / 4.6-4 "who drains"
        // contract: under the GIL the joinee's completion sequence drained
        // the single shared VM queue (ThreadObject.cpp fn-return tail), so a
        // reaction the joiner queued before parking had run by the time
        // join() observed completion. GIL-off that job sits on the
        // joiner-owned queue — per-lite for spawned joiners, the VM default
        // queue under the main-carrier key for a main-thread joiner (E.1b
        // rule 1: settling-thread enqueue; the U-T9 VM::queueMicrotask
        // reroute) — and the joinee may not touch it (I11, SD2
        // own-queues-only, SD17 no adoption). Drain it HERE — after the
        // GILDroppedSection closed (D11 holds: no drain inside the park) and
        // completion was observed, back under this thread's token, before
        // join() returns. The joinee sampled its result at fn-return, so
        // this cannot perturb the value join() reports. drainMicrotasks
        // reroutes to the CURRENT lite's queue, so this is I11-clean for
        // spawned joiners too. A pending termination raised by a drained job
        // propagates like any other host-call exit. If a
        // drainMicrotaskDelayScope is active the drain is a silent no-op,
        // degrading to pre-fix behavior (jobs run at the natural turn
        // boundary instead). The gate mirrors the AB-25 main-carrier
        // condition: a GIL-off non-main embedder joiner with no current lite
        // must NOT fall through into the default-queue body and drain the
        // main carrier's unlocked Deque from a foreign thread (the sibling
        // VM::queueMicrotask fail-stops on that shape; drainMicrotasks has
        // no such guard).
        if (vm.gilOff() && (WTF::isMainThread() || (VMLite::currentIfExists() && VMLite::currentIfExists()->vm == &vm))) {
            vm.drainMicrotasks();
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    // F1 reader shape cleanup (AB18-I review round; NOT credited as part of
    // the AB18-I fix): take ONE acquire snapshot of Phase first and decide
    // Failed from it, then read the result Strong. Phase transitions
    // Running -> Finished|Failed exactly once under joinLock with result.set
    // sequenced before the release-store, so the previous two-load form was
    // observationally equivalent (coherence pins the second load to the same
    // settled value); this is the textbook ordering plus a debug assert,
    // nothing more.
    ThreadState::Phase settledPhase = state.phase.load(std::memory_order_acquire);
    ASSERT(settledPhase != ThreadState::Phase::Running);
    JSValue result = thread->result();
    if (settledPhase == ThreadState::Phase::Failed) {
        throwException(globalObject, scope, result);
        return { };
    }
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(threadProtoFuncAsyncJoin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSThread* thread = dynamicDowncast<JSThread>(callFrame->thisValue());
    if (!thread)
        return throwVMTypeError(globalObject, scope, "Thread.prototype.asyncJoin called on incompatible receiver"_s);

    ThreadState& state = thread->threadState();
    // Repeat calls return distinct promises with the same settlement (4.1):
    // each call mints a fresh promise + ticket. Ticket creation is the I20
    // shell-liveness point (addPendingWork at registration, under the JSLock).
    // The receiver may be a never-completing ThreadState (lazy main/embedder
    // tid-0 cell from Thread.current): such a promise never settles —
    // 4.1/4.6.3 permit that (it pins the shell like an un-notified
    // waitAsync) — and its ticket is drained at teardown by the 5.10
    // finalizer hook (registerThreadStateFinalizer), never by ~ThreadState.
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, { thread });

    // F5 asyncJoin: phase check and append both under joinLock — the
    // completion sequence's Phase store and asyncJoiners swap are likewise
    // both under joinLock, so a ticket is either appended before the swap
    // (settled by the completion sequence) or observes phase != Running here
    // (settled below). No lost wakeup. The phase observed under the lock is
    // final (Running -> Finished|Failed happens exactly once), so deciding
    // resolve-vs-reject from it is sound.
    bool settleNow = false;
    bool failed = false;
    {
        Locker joinLocker { state.joinLock };
        auto phase = state.phase.load(std::memory_order_acquire);
        if (phase != ThreadState::Phase::Running) {
            settleNow = true;
            failed = phase == ThreadState::Phase::Failed;
        } else
            state.asyncJoiners.append(ticket.copyRef());
    }
    if (settleNow)
        settleJoinTicket(ticket, thread, failed);
    return JSValue::encode(promise);
}

// ---------------- Thread.current / id ----------------

JSThread* ensureJSThreadForState(JSGlobalObject* globalObject, ThreadState& state)
{
    if (state.jsThread)
        return uncheckedDowncast<JSThread>(state.jsThread.get());

    VM& vm = globalObject->vm();

    // Resolve the instance Structure without running any JS — this path must
    // be infallible (the Thread.current getter and the 5.10 first-lazy-Strong
    // hook both rely on that). SCALEBENCH §37 R1: read the per-realm cached
    // Structure stamped in createThreadProperty (one StructureID per realm
    // instead of one per call). createThreadProperty is the only installer of
    // the Thread.current getter and of every other caller of this function, so
    // under useJSThreads the cache is always populated by the time we run; the
    // fresh-null-prototype fallback covers the SPEC-api 9.2-2 "global without
    // the constructor installed" defensive case only.
    Structure* structure = globalObject->threadObjectStructure();
    if (!structure) [[unlikely]]
        structure = JSThread::createStructure(vm, globalObject, jsNull());

    // First Strong for a lazy main/embedder ThreadState (tid 0): register the
    // 5.10 finalizer hook here. The Strong pins the cell, so for lazy TSs the
    // hook fires at cell death or VM teardown via lastChanceToFinalize(),
    // with the JSLock held (5.10 "main/embedder: ~VM" clearing point). An
    // embedder thread exiting early only drops its TLS RefPtr — the hook
    // lambda's Ref keeps the ThreadState alive until then.
    JSThread* thread = JSThread::create(vm, structure, Ref { state });
    state.jsThread.set(vm, thread);
    registerThreadStateFinalizer(vm, thread, state);
    return thread;
}

JSC_DEFINE_CUSTOM_GETTER(threadCurrentGetter, (JSGlobalObject* globalObject, EncodedJSValue, PropertyName))
{
    // Thread.current (SPEC-api 4.1): the caller's JSThread. On a spawned
    // thread this returns the cell created at spawn — reference-equal to the
    // parent's `new Thread(...)` result (I5). On the main thread and any
    // embedder thread, the ThreadState (tid 0) and its JSThread cell are
    // created lazily on first access and are stable thereafter for that
    // thread (5.1 currentThreadState). Distinct embedder threads get
    // distinct ThreadStates (identity = the per-thread TLS slot, never the
    // tid). Infallible: never observes the receiver, never runs JS.
    Ref<ThreadState> state = ensureCurrentThreadState();
    return JSValue::encode(ensureJSThreadForState(globalObject, state.get()));
}

JSC_DEFINE_CUSTOM_GETTER(threadIdGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSThread* thread = dynamicDowncast<JSThread>(JSValue::decode(thisValue));
    if (!thread)
        return throwVMTypeError(globalObject, scope, "Thread.prototype.id called on incompatible receiver"_s);
    return JSValue::encode(jsNumber(thread->threadState().tid));
}

// ---------------- Thread.restrict ----------------

// Dev 8: species-protected builtin prototype/constructor pairs, ptr-compared
// against o's OWN global's slots (never the calling realm's): Array, Promise,
// RegExp, ArrayBuffer + SharedArrayBuffer (both sharing modes), and each
// %TypedArray% view pair plus the %TypedArray% super pair (G29). Lazy slots
// are probed via the non-forcing Concurrently accessors first: an
// unmaterialized slot cannot be `object`, and once a LazyClassStructure is
// materialized its prototype()/constructor() accessors no longer run the
// initializer, so nothing here ever forces a lazy slot (G25/G29).
static bool isSpeciesProtectedBuiltin(JSObject* object)
{
    JSGlobalObject* global = object->globalObject();
    const void* pointer = static_cast<const void*>(object);

    // Eager pairs (plain WriteBarrier slots). void* compares: some of these
    // return pointers to classes that are only forward-declared here; JSC
    // cells are single-inheritance so the comparison is exact.
    if (pointer == static_cast<const void*>(global->arrayPrototype()) || pointer == static_cast<const void*>(global->arrayConstructor()))
        return true;
    if (pointer == static_cast<const void*>(global->promisePrototype()) || pointer == static_cast<const void*>(global->promiseConstructor()))
        return true;
    if (pointer == static_cast<const void*>(global->regExpPrototype()) || pointer == static_cast<const void*>(global->regExpConstructor()))
        return true;

    // ArrayBuffer + SharedArrayBuffer (both modes; lazy).
    for (ArrayBufferSharingMode mode : { ArrayBufferSharingMode::Default, ArrayBufferSharingMode::Shared }) {
        if (!global->arrayBufferStructureConcurrently(mode))
            continue; // not materialized => object cannot be it
        if (object == global->arrayBufferPrototype(mode) || object == global->arrayBufferConstructor(mode))
            return true;
    }

    // %TypedArray% super pair: one instance of each dedicated class per
    // global; ClassInfo identity never forces a lazy slot (there is no
    // public non-forcing accessor for these two LazyProperty slots).
    if (object->inherits<JSTypedArrayViewPrototype>() || object->inherits<JSTypedArrayViewConstructor>())
        return true;

    // Each %TypedArray% view pair (lazy; the resizable/growable-shared
    // structure variants share the same prototype/constructor pair).
#define THREADS_CHECK_TYPED_ARRAY_PAIR(name) \
    if (global->typedArrayStructureConcurrently(Type##name, false)) { \
        if (object == global->typedArrayPrototype(Type##name) || object == global->typedArrayConstructor(Type##name)) \
            return true; \
    }
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(THREADS_CHECK_TYPED_ARRAY_PAIR)
#undef THREADS_CHECK_TYPED_ARRAY_PAIR

    return false;
}

// Round-4 tightening (recorded as landed deviation D13 in
// docs/threads/INTEGRATE-api.md): the 5.7.3/9.2-6 enforcement story is sound
// ONLY for receivers whose every enforced operation lands on the hooked
// JSObject generic entry points once the object is pinned on an
// uncacheable-dictionary SlowPut shape. That is false for receivers whose
// ClassInfo method table overrides the enforced entry points with
// NON-delegating implementations: a JSGenericTypedArrayView serves indexed
// reads/writes through TypedArrayType-keyed overrides (and JIT/IC fast paths
// keyed on the same) that never consult the structure's dictionary-ness or
// the shadow ArrayStorage 5.7.1(a) installs — a foreign thread could read
// AND write every element of a "restricted" Float64Array with no
// ConcurrentAccessError. StringObject indexed chars, DirectArguments/
// ScopedArguments mapped indices, and every other getOwnPropertySlot(ByIndex)
// / put(ByIndex) overrider escape the same way. ALLOWLIST, not denylist:
// - any class whose enforced method-table slots are all still the JSObject
//   defaults (plain objects, JSFinalObject/JSNonFinalObject shapes — C++
//   resolves an inherited &Derived::op to &JSObject::op, so the pointer
//   comparison detects "not overridden" exactly);
// - JSArray exactly: its overrides (put / getOwnPropertySlot for "length",
//   deleteProperty, getOwnPropertyNames) delegate to the hooked generic
//   paths for everything except the "length" metadata slot, and arrays are
//   part of the I14-covered surface
//   (JSTests/threads/api/thread-restrict.js indexed cases).
//   Note (closes the D13 audit gap): JSArray::defineOwnProperty does not
//   delegate to the hooked JSObject::defineOwnProperty entry point — its
//   indexed branch routes to JSObject::defineOwnIndexedProperty and its
//   non-index branch calls defineOwnNonIndexProperty directly — so it
//   carries its own threadRestrictCheck gate at the top (JSArray.cpp, same
//   idiom as the JSObject::defineOwnProperty hook), keeping frozen I14
//   (SPEC-api.md: indexed set/delete/define on array =>
//   ConcurrentAccessError) enforced. Do NOT drop JSArray from this
//   allowlist (would break the I14-required restrict acceptance of arrays)
//   and do NOT weaken the test.
// Everything else => the 5.7 TypeError at restrict time, so an object that
// cannot be enforced is never accepted (no silent enforcement hole). This
// also keeps the 9.2-6 note "do not hook getOwnPropertySlotByIndex" valid:
// the SlowPut-pin argument it relies on holds precisely for the receivers
// this allowlist admits.
static bool restrictReceiverStaysOnHookedPaths(JSObject* object)
{
    const ClassInfo* classInfo = object->classInfo();
    if (classInfo == JSArray::info())
        return true;
    const MethodTable& methodTable = classInfo->methodTable;
    const MethodTable& base = JSObject::info()->methodTable;
    return methodTable.getOwnPropertySlot == base.getOwnPropertySlot
        && methodTable.getOwnPropertySlotByIndex == base.getOwnPropertySlotByIndex
        && methodTable.put == base.put
        && methodTable.putByIndex == base.putByIndex
        && methodTable.deleteProperty == base.deleteProperty
        && methodTable.deletePropertyByIndex == base.deletePropertyByIndex
        && methodTable.defineOwnProperty == base.defineOwnProperty
        && methodTable.getOwnPropertyNames == base.getOwnPropertyNames
        && methodTable.getOwnSpecialPropertyNames == base.getOwnSpecialPropertyNames
        && methodTable.preventExtensions == base.preventExtensions
        && methodTable.isExtensible == base.isExtensible
        && methodTable.setPrototype == base.setPrototype;
}

// Dev 8/11 excluded receivers (=> TypeError "cannot restrict this object").
static bool isExcludedRestrictReceiver(JSObject* object)
{
    // Global objects (GlobalObjectType is in the environment range),
    // environment/scope objects.
    if (object->isEnvironment() || object->isWithScope())
        return true;
    // Proxies and global proxies (any realm's).
    if (object->inherits<ProxyObject>() || object->inherits<JSGlobalProxy>())
        return true;
    // Species-protected builtin prototype/constructor pairs (Dev 8).
    if (isSpeciesProtectedBuiltin(object))
        return true;
    // Dev 11: structures that hijack the indexing header cannot take
    // ArrayStorage (5.7.1(a) requires it; JSObject.cpp ensureArrayStorage
    // path would return null / misbehave).
    if (object->structure()->hijacksIndexingHeader())
        return true;
    // D13 (round 4): receivers whose method table overrides any enforced
    // entry point would bypass the 9.2-6 hooks (typed-array views,
    // StringObject, arguments objects, functions' lazy own properties, ...).
    if (!restrictReceiverStaysOnHookedPaths(object))
        return true;
    // DataView (and any other ArrayBufferView the allowlist would admit):
    // its enforced method-table slots are all JSObject defaults and
    // TypeDataView does not hijack the indexing header (isTypedView() is
    // false for it), so neither check above fires — yet its backing
    // ArrayBuffer stays reachable from any thread via the
    // DataView.prototype get*/set* methods, which the 9.2-6 hooks never
    // see. Same D13 rationale, different escape path => restrict-time
    // TypeError (I14 overrider-instance block lists DataView explicitly).
    if (object->inherits<JSArrayBufferView>())
        return true;
    return false;
}

JSC_DEFINE_HOST_FUNCTION(threadFuncRestrict, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue argument = callFrame->argument(0);
    if (!argument.isObject())
        return throwVMTypeError(globalObject, scope, "cannot restrict this object"_s);
    JSObject* object = asObject(argument);
    if (isExcludedRestrictReceiver(object))
        return throwVMTypeError(globalObject, scope, "cannot restrict this object"_s);

    // 5.7.1(0) affinity hit: owner => return o (4.1 idempotency; the 5.7.1
    // conversions already happened on the first restrict), foreign => CAE
    // (re-restrict from another thread, 4.1).
    auto& manager = ThreadManager::singleton();
    switch (manager.objectAffinity(object)) {
    case ThreadManager::Affinity::Owner:
        return JSValue::encode(object);
    case ThreadManager::Affinity::Foreign: {
        throwConcurrentAccessError(globalObject, scope, "Thread.restrict called from a non-owning thread"_s);
        return { };
    }
    case ThreadManager::Affinity::None:
        break;
    }

    // 5.7.1 conversion sequence (side effects are perf-only; defeats + pins
    // the object off every cacheable fast path so each enforced op lands on
    // a hooked generic path, 5.7.3 / 9.2-6):
    //
    // (a) ensureArrayStorage: legal for blank/non-array shapes, no-op on any
    //     ArrayStorage; non-null post-Dev-11 (hijacksIndexingHeader receivers
    //     were excluded above).
    ArrayStorage* arrayStorage = object->ensureArrayStorage(vm);
    ASSERT_UNUSED(arrayStorage, arrayStorage);
    // (b) SlowPut conversion. switchToSlowPutArrayStorage is idempotent on
    //     already-SlowPut shapes (JSObject.cpp grew the SlowPut no-op case):
    //     the guard here is perf-only, NOT correctness — GIL-off, a racing
    //     restrict (both pass step (0) Affinity::None) or a concurrent
    //     haveABadTime conversion can flip the shape to SlowPut between this
    //     check and the call, and the call must then no-op, not CRASH.
    if (!hasSlowPutArrayStorage(object->indexingType()))
        object->switchToSlowPutArrayStorage(vm);
    // (c) Uncacheable dictionary (keeps the indexing mode): the 5.7.3
    //     choke-point hooks gate on isUncacheableDictionary(), and no IC
    //     ever caches this shape.
    if (!object->structure()->isUncacheableDictionary())
        object->convertToUncacheableDictionary(vm);
    // (d) Flatten pin (G25). Mandatory: without it the first cache attempt
    //     re-flattens the dictionary and the object escapes the hooks; the
    //     bit is inherited by later transitions. SlowPut is sticky, so all
    //     later indexed PUTs (including owner-added o[0]) stay on the hooked
    //     generic paths.
    object->structure()->setHasBeenFlattenedBefore(true);
    ASSERT(object->structure()->isUncacheableDictionary());
    ASSERT(hasSlowPutArrayStorage(object->indexingType()));

    // Owner identity = the restricting thread's Ref<ThreadState>, never a
    // TID (5.7.2). GIL-off, two racing restricts can both pass step (0)
    // (Affinity::None) — the claim itself is arbitrated under the affinity
    // lock inside restrictObject, and the loser surfaces the frozen 4.1
    // re-restrict ConcurrentAccessError (MC-HAND S6). The loser's 5.7.1
    // conversion sequence above already ran on the winner's object; that is
    // object-model-safe (M5) and perf-only (the conversions are exactly what
    // the winner's restrict performs/requires anyway).
    if (!manager.restrictObject(object, ensureCurrentThreadState().get())) {
        throwConcurrentAccessError(globalObject, scope, "Thread.restrict called from a non-owning thread"_s);
        return { };
    }
    return JSValue::encode(object);
}

// ---------------- ConcurrentAccessError ----------------

static Structure* concurrentAccessErrorStructure(JSGlobalObject* globalObject, VM& vm)
{
    JSValue constructor = globalObject->getDirect(vm, Identifier::fromString(vm, "ConcurrentAccessError"_s));
    if (constructor && constructor.isObject()) {
        JSValue prototype = asObject(constructor)->getDirect(vm, vm.propertyNames->prototype);
        if (prototype && prototype.isObject())
            return ErrorInstance::createStructure(vm, globalObject, prototype);
    }
    return globalObject->errorStructure();
}

Exception* throwConcurrentAccessError(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral message)
{
    VM& vm = globalObject->vm();
    Structure* structure = concurrentAccessErrorStructure(globalObject, vm);
    ErrorInstance* error = ErrorInstance::create(vm, structure, String { message }, jsUndefined());
    return throwException(globalObject, scope, error);
}

JSC_DEFINE_HOST_FUNCTION(callConcurrentAccessError, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    Structure* structure = concurrentAccessErrorStructure(globalObject, vm);
    return JSValue::encode(ErrorInstance::create(globalObject, structure, callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(constructConcurrentAccessError, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = prototype.isObject() ? ErrorInstance::createStructure(vm, globalObject, prototype) : concurrentAccessErrorStructure(globalObject, vm);
    RELEASE_AND_RETURN(scope, JSValue::encode(ErrorInstance::create(globalObject, structure, callFrame->argument(0), callFrame->argument(1))));
}

// ---------------- property factories ----------------

static void linkConstructorAndPrototype(VM& vm, JSObject* constructor, JSObject* prototype, ASCIILiteral tag)
{
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, String { tag }), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
}

JSValue createThreadProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "join"_s), 0, threadProtoFuncJoin, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncJoin"_s), 0, threadProtoFuncAsyncJoin, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, "id"_s), CustomGetterSetter::create(vm, threadIdGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));

    // SCALEBENCH §37 R1: stamp the per-realm instance Structure once against
    // the canonical prototype (mirrors createLockProperty). Only reached
    // inside the useJSThreads install block.
    globalObject->setThreadObjectStructure(vm, JSThread::createStructure(vm, globalObject, prototype));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 1, "Thread"_s, callThread, ImplementationVisibility::Public, NoIntrinsic, constructThread);
    linkConstructorAndPrototype(vm, constructor, prototype, "Thread"_s);
    constructor->putDirectCustomAccessor(vm, Identifier::fromString(vm, "current"_s), CustomGetterSetter::create(vm, threadCurrentGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));
    constructor->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "restrict"_s), 1, threadFuncRestrict, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    return constructor;
}

JSValue createConcurrentAccessErrorProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject, globalObject->errorPrototype());
    prototype->putDirect(vm, vm.propertyNames->name, jsNontrivialString(vm, "ConcurrentAccessError"_s), static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->message, jsEmptyString(vm), static_cast<unsigned>(PropertyAttribute::DontEnum));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 1, "ConcurrentAccessError"_s, callConcurrentAccessError, ImplementationVisibility::Public, NoIntrinsic, constructConcurrentAccessError);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    return constructor;
}

} // namespace JSC
