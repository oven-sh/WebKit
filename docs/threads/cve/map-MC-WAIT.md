# MC-WAIT — Waiter-list lifetime and wait-protocol semantics: mapping to our threads surface

Mechanism class (from the catalog, web-derived data): futex/waiter-queue
node lifetime across agent death; timeout-vs-notify races; a sync object
freed while its own slow path or parked waiters still reference it; ordering
strength of wait/notify fast paths. Exemplars: JDK-8153224 (ObjectMonitor
async-deflation family — monitor deflated/freed while a contending thread is
still in the monitor's own slow path), the cross-engine Atomics.wait
not-equal ordering bug (value re-check not serialized against the notifier
=> lost wakeup; no CVE), ERTS signal-queue inconsistency race. No public
JS-engine memory-safety CVE in this class — under-audited, not absent.

Date: 2026-06-07. Tree: jarred/threads (phase 1 GIL'd complete, ungil in
progress; AB-17 landed, AB-17b open). Specs consulted: SPEC-api (5.3, 5.4,
5.5, 5.6, 5.9, 5.10, F3/F4/F5, G11), SPEC-ungil (§A.2.6/D9, §A.3.2, §C.3,
§C.6/SD6/SD8, §E.4, §G, annex W W1, r15 F2), UNGIL-HANDOUT rev 32,
INTEGRATE-api.md (D5, D9, D11), INTEGRATE-ungil.md (U-T11 OPEN list).
Read-only audit; the test under JSTests/threads/cve/ is written but NOT
executed (the bring-up loop owns the build); it runs post-ungil via
thread-cve-audit.

Why this class matters more for us than for SAB-era engines: a SAB engine
has exactly one engine-level wait protocol (Atomics.wait on shared bytes).
We have five: TA SAB waits (per-wait nodes, SD6), property-keyed waits
(SPEC-api 5.6), JS Lock/Condition parks (5.3/5.4), thread join (F5), and the
thread-granular stop-the-world NVS park tickets (§A.3.2). Every one parks
with the GIL dropped, polls termination in D9 quanta, and GIL-off interacts
with the W1 parked-carrier watchdog-service episode — i.e. every one is a
waiter-list whose node lifetime, wake arbitration, and fast-path ordering
must be argued individually.

---

## S1. TA (SharedArrayBuffer) sync Atomics.wait — per-wait nodes

**Surface.** `waitSyncWithPerWaitNode`,
Source/JavaScriptCore/runtime/WaiterListManager.cpp:202-314 (flag-on, both
GIL modes; flag-off keeps the landed single-flight `vm.syncWaiter()` body at
:327-349 byte-identical). Governed by SPEC-ungil §C.6/SD6 (per-wait nodes),
SD8/§E.5 (terminate => wait fails), §A.2.6/D9 (10ms park quanta), annex W W1
+ r15 F2 (parked-carrier watchdog service and old-node disposition).

**Verdict: immune-by-construction.** Sub-mechanism by sub-mechanism:

- *Lost wakeup / not-equal ordering (the cross-engine exemplar).* The
  expected-value check runs UNDER the per-address `list->lock`
  (WaiterListManager.cpp:218-220) in the same critical section as the
  enqueue (:221); `notifyWaiter` serializes through the same lock
  (:496-530). A notifier's store is program-ordered before its
  `Atomics.notify`, and the notify takes `listLock`; the waiter's
  `WTF::atomicLoad` (seq_cst) + enqueue happen atomically w.r.t. that lock,
  so either the waiter sees the new value (NotEqual, :220) or the notifier
  sees the enqueued node. No window.
- *Timeout-vs-notify race.* Single arbitration point: the tail re-checks
  `waiter->isOnList()` under the still-held `listLock` (:300-313). A waiter
  dequeued by a notify returns OK even if the deadline passed or termination
  raced the wake (consumed notify honored; the termination bit stays set and
  is delivered at the next trap poll — the U-T11 "exactly one of
  ok/timed-out, never both" arm). A waiter that timed out removes ITSELF
  under the lock (:303), so a concurrent notify either consumed it first
  (=> OK) or finds it gone and wakes the next node. State can never be
  consumed twice.
- *Node lifetime.* `Waiter` is `ThreadSafeRefCounted`; the parker holds a
  `Ref<Waiter>` (:216) and a `Ref<WaiterList>` for the whole wait, the list
  refs enqueued nodes (`WaiterList::addLast` refs, `removeWithUpdate` derefs,
  WaiterListManager.h:132-137/:192-197), and `m_waiterLists` holds
  `Ref<WaiterList>`. No raw-node handoff exists; a notifier's `takeFirst`
  produces a protected Ref before unlinking (WaiterListManager.h:139-148).
- *W1 episode double-node window (the most adversarial spot).* GIL-off, a
  parked CARRIER that observes the watchdog-check bit drops `listLock`, runs
  the full §J.3 reacquire-service-re-release episode, and re-takes the lock
  (:230-272). While the lock is dropped the old node stays enqueued, so a
  racing notify can consume it. The r15 F2 disposition then resolves every
  interleaving under the re-taken lock: (a) node gone => wait completes OK,
  and if the service ALSO returned a terminate verdict the
  consumed-by-servicer shield premise is falsified, so the code re-raises
  `fireTrapVMWide(NeedTermination)` (:258-259) — the verdict is not lost;
  (b) node still enqueued => removed, FRESH node tail-enqueued, re-park
  (:267-271; FIFO-position loss is the declared I10 eats-one-notify class).
  At no point are two live nodes for one logical wait reachable by a
  notifier, because both dispositions run under `listLock`.
- *Lock-rank check.* `fireTrapVMWide` under the rank-3 `listLock` takes only
  the registry lock (leaf) + `m_trapSignalingLock` — the banner (:253-257)
  matches `Watchdog::timerDidFire`'s held-lock shape; no rank-3 lock is
  taken beneath it. The W1 reacquisition itself runs with NO rank-3 lock
  held (:234-236).

**Residual (recorded, not a hole):** SD6's other half — deleting the D8
single-flight gate in AtomicsObject.cpp — is an OPEN obligation recorded in
the :166-183 banner; until it lands a second concurrent non-spawned sync TA
wait on the same VM throws the gate's TypeError instead of parking. That
masks SD6's user-visible delta but is fail-closed (a throw, not a protocol
break), so it does not create an MC-WAIT instance.

## S2. TA waiter-list lifetime across agent death (VM/SAB teardown)

**Surface.** `WaiterListManager::unregister(VM*)` /
`unregister(JSGlobalObject*)` / `unregister(uint8_t*, size_t)`,
WaiterListManager.cpp:599-681; the `ASSERT(waiter->isAsync())` at :614 and
:660 encodes "no sync waiter can be parked at teardown". Governed by
SPEC-api 5.10 (rooting), SPEC-ungil §E.5/TERM1 (termination delivery to
parked threads).

**Verdict: immune-by-construction** — but only because two independent
mechanisms close the agent-death window, so both must be preserved:

1. *A spawned thread parked in waitSync keeps the VM alive.*
   `constructThread` detaches the native handle and captures
   `protectedVM = Ref { vm }` into the thread main lambda
   (Source/JavaScriptCore/runtime/ThreadObject.cpp:391-396), so
   `unregister(VM*)` (called from VM teardown) cannot run while any spawned
   thread — parked or not — is still inside `threadMain`. The JDK
   "monitor deflated while a thread is in its slow path" shape is therefore
   unreachable for VM-lifetime state: the parked thread itself pins the VM.
2. *Termination wakes every park bounded by one D9 quantum.* The orphaned
   `vm.syncWaiter()` central wakes (VMTraps.cpp) are BYPASSED flag-on (annex
   A26); their replacement is the 10ms `parkLitePollTerminationRequested`
   poll on the park lite (TERM1 rule 4: spawned = current lite, carrier =
   §J.3-captured lite), WaiterListManager.cpp:225/:305. So an embedder
   tearing down goes terminate -> every parked thread unparks within ~10ms
   -> joins complete -> teardown proceeds with empty sync-waiter lists.

Adversarial self-check: a release-build embedder that destroys the VM with a
foreign (non-spawned, non-pinning) thread parked in waitSync would dequeue
that thread's node via `removeIf` and the parked thread would later return
OK against a freed VM. That is the documented embedder protocol violation
the :612-614 comment inherits from upstream; with `--useJSThreads` the only
in-model waiters are the main/embedder carrier (which is running teardown
itself, so cannot be parked) and spawned threads (pinned by 1). SAB-death
(`unregister(uint8_t*,size_t)`) similarly cannot strand a sync waiter: the
waiting thread's frame conservatively roots the typed array/SAB for the
whole wait (Heap.cpp:1022-1045 scans every registered thread), so "SAB
destructing" implies no parked sync waiter on it.

## S3. Property-keyed Atomics.wait/waitAsync/notify (SPEC-api 5.6)

**Surface.** PropertyWaiter / PropertyWaiterList / PropertyWaiterTable and
the three entry points, Source/JavaScriptCore/runtime/ThreadAtomics.cpp:
:778-958 (table), :972-1104 (`atomicsWaitOnProperty`), :1106-1201
(`atomicsWaitAsyncOnProperty`), :1203-1253 (`atomicsNotifyOnProperty`).
Governed by SPEC-api 5.6 (steps + "flipped exactly once, under the list
lock"), 5.5 (ticket states), 5.10 (Strong liveness), F4 (read-then-enqueue
under the JSLock), and GIL-off by SPEC-ungil §C.3 (PWT arming re-home + I10
re-derivation, history annex C3 BINDING).

### S3a. GIL-off lost wakeup — **CLOSED 2026-06-10** (was susceptible-CONFIRMED: 13/20 runs hit the lost wakeup)

**Closure:** the §C.3(b) under-listLock SVZ re-validation landed
(ThreadAtomics.cpp: `revalidateEnqueuedPropertyWaiterUnderListLock` +
`sameValueZeroForAtomicsUnderListLock`, driven by dequeue-and-restart loops
in both `atomicsWaitOnProperty` and `atomicsWaitAsyncOnProperty`; the
waitAsync revoked-registration arm retires its ticket via
`AsyncTicket::retireUnsettled`). mc-wait-property-wait-lost-wakeup.js is the
regression test, 20/20 GIL-off (Release + Debug) and GIL-on. The original
finding follows, kept as the design record:

This is the exact "Atomics.wait not-equal ordering bug" exemplar, on our
new property lane.

The landed code reads the property once in step 1 ("validate + read under
the JSLock; no re-read below", ThreadAtomics.cpp:980-986) and enqueues the
waiter under `listLock` later (:1008-1011) with NO value re-check. The I10
lost-wakeup closure argument in the banner (:1001-1004) is "JSLock held from
the step-1 read through the enqueue" — a GIL-on argument. GIL-off the JSLock
is a token, not a mutual-exclusion of mutators, so this window is real:

  thread A: Atomics.load(o,k) == expected        (lock-free §9.5 read)
  thread B: Atomics.store(o,k,new); Atomics.notify(o,k)   // list empty
  thread A: enqueue under listLock; park                  // forever*

(*bounded only by timeout/termination.) The frozen design closes it —
SPEC-ungil §C.3: "enqueue + SVZ re-validation UNDER listLock; mismatch =>
dequeue 'not-equal'; rope/convert => dequeue (I10), resolve outside, FRESH
enqueue" — and INTEGRATE-ungil.md:241-245 records precisely this as **OPEN,
owned by U-T11**: the pre-enqueue load routes through the §9.5 accessor, but
"the under-listLock SVZ re-validation + dequeue-and-restart arm ... are
U-T11's". So this is a known-unlanded spec obligation, not a design hole;
classified susceptible-suspected because the code as it stands GIL-off loses
wakeups (a hang/liveness break, not memory unsafety — the waiter's node,
refs and arbitration stay sound).

**Test (written, do not run until post-ungil):**
JSTests/threads/cve/mc-wait-property-wait-lost-wakeup.js — handshake-driven:
the waiter publishes "about to wait" immediately before calling
`Atomics.wait(box,'v',0,T)`; the notifier then stores 1 and notifies. Under
the §C.3 contract every round ends "ok" or "not-equal"; any "timed-out"
round means a store+notify pair that landed in the read->enqueue window was
lost. Deterministic-leaning amplifier: the window is hit probabilistically,
but a hit is unambiguous (no legal interleaving yields "timed-out" once the
notify is guaranteed to precede the deadline). GIL-on the test must pass
trivially (JSLock closes the window) — it doubles as the §C.3 regression
test once U-T11 lands the re-validation. Same reasoning covers
`atomicsWaitAsyncOnProperty` (read at :1114, enqueue at :1141-1143, same
missing re-validation); the test includes a waitAsync arm.

### S3b. Timeout-vs-notify-vs-termination arbitration — immune-by-construction

SPEC-api 5.6's "state flipped exactly once, always under the list lock (one
flip arbitrates outcome)" is implemented faithfully:

- notify flips Waiting->Notified and dequeues in one `listLock` section
  (:1224-1239);
- the sync parker, on loop exit, re-checks state under `listLock`: Notified
  wins; otherwise IT dequeues itself and flips TimedOut/Terminated
  (:1072-1081) — dequeued <=> flipped holds in both directions;
- the waitAsync finite-timeout timer task flips TimedOut only after
  observing Waiting under `listLock`, dequeueing in the same section
  (:1168-1183); the notify path settles only tickets it flipped Notified
  under the lock (:1230-1235), and settles OUTSIDE the rank-3 lock per §E.4
  (:1240-1247). Exactly one settler per ticket; double-settle is structurally
  impossible (`wasWaiting` arbitration + DWT tickets are one-shot).
- The W1 episode honors a notify consumed during the watchdog service and
  re-raises termination VM-wide to revoke the consumed-by-servicer shield
  (:1058-1067) — same disposition rules as S1.

### S3c. Waited-on cell / list lifetime — immune-by-construction

The freed-sync-object exemplar is closed by three layers:
`list->cellProtect` Strong + `uidProtect` root the waited-on object and key
while the list is non-empty (created under the JSLock,
:804-806/:843-851; SPEC-api 5.10); a per-cell teardown-sweep finalizer
handles the never-notified infinite waitAsync whose Strong has no other
clearing point (round-4 fix, :852-875, with `m_sweepFinalizerCells`
re-registration for recycled cell addresses :829-833); and every parked sync
waiter holds `Ref<PropertyWaiterList>` + `Ref<PropertyWaiter>` across the
park, so even a swept (unreachable) list is not freed memory — the sweep
deliberately leaves sync waiters enqueued (:884-887) and their threads own
their dequeue. The D5 timer lambda pins `Ref<VM>` and bails on a cancelled
ticket under the JSLock before touching VM state (:1156-1167), closing the
timer-outlives-VM arm the 5.10 `~AsyncTicket` RELEASE_ASSERTs against.

## S4. JS Lock / Condition — sync object freed while parked waiters reference it (JDK-8153224 analog)

**Surface.** NativeLockState
(Source/JavaScriptCore/runtime/LockObject.h:39-108), the contended
`lock.hold` park (LockObject.cpp:542-615), CondWaiter/NativeConditionState
(ConditionObject.h:39-97) and the sync `cond.wait` park
(ConditionObject.cpp:100-275). Governed by SPEC-api 5.3/5.4/5.9 and the F3
lost-wakeup closure (enqueue while still holding the JS Lock being
released).

**Verdict: immune-by-construction.** The ObjectMonitor failure needs the
native sync state to die while a thread is inside its slow path. Here:

- The native states are `ThreadSafeRefCounted`, owned by the JS cell via
  `Ref` (LockObject.h:136, ConditionObject.h:97). They are never deflated,
  reused, or detached from the cell — there is no async-deflation analog at
  all; destruction happens only at last deref.
- A parked holder/waiter always has the JS cell (`lockObject`,
  `conditionObject` locals) on its native stack, and the shared-server GC
  conservatively scans EVERY I4(b)-registered thread — parked and
  access-released included — in one MachineThreads pass
  (Source/JavaScriptCore/heap/Heap.cpp:1022-1045). So the cell outlives the
  park, hence the Ref, hence the native state. The async settle paths don't
  even rely on that: the grant/settle lambdas capture `Ref<NativeLockState>`
  directly (LockObject.cpp:446, :482).
- `cond.wait` parks on `ParkingLot::parkConditionally(&waiter->state, ...)`
  (ConditionObject.cpp:190-196) — the queue node is ParkingLot's own
  thread-owned node; the address key `&waiter->state` lives in a
  `Ref<CondWaiter>` held by both the parker and the queue, so the
  freed-while-parked shape cannot occur. Wake arbitration is the same
  flip-once-under-queueLock protocol as S3b (notify flips BEFORE the parker
  can re-check under queueLock; still-Waiting => self-dequeue, spurious-wake
  legal per I9, ConditionObject.cpp:197-215).
- `lock.hold`'s contended path deliberately parks in honest
  `tryLock()` + 1ms sleep quanta rather than `tryLockWithTimeout`
  (LockObject.cpp:590-598 banner) — the banner documents that the rejected
  helper returns "held by ANYONE", whose acceptance would have been a
  textbook MC-WAIT bug (stolen lock => double-release abort). Recorded here
  as evidence the surface was audited, and as a tripwire: any future
  "optimization" replacing the quanta loop with a timed-lock helper must
  re-prove "acquired by me".

Residual (in-model boundary, recorded): `m_holder` / `m_asyncGrantRunner`
hold `WTF::Thread*` that are compared, never dereferenced (LockObject.h:44,
:64). An ABA on a recycled `WTF::Thread` address would require the previous
holder to have died while holding `m_lock` without unwinding — termination
unwinds through the hold epilogue, and abrupt native thread death without
unwind is outside the model (it leaves `m_lock` held forever, which D9
already converts from "unkillable hang" to "terminable park" for everyone
else). No action.

## S5. Thread join / asyncJoin — waiter lifetime across the agent's OWN death

**Surface.** F5 completion (ThreadObject.cpp:286-295), sync join park
(:404-475, 10ms `joinCondition.waitUntil` quanta :471), asyncJoin ticket
drain at ThreadState destruction (:123-143). Governed by SPEC-api 5.2,
4.1, 5.5, F5.

**Verdict: immune-by-construction.** This surface is the one where agent
death is the PROTOCOL, so it was designed against this class directly:

- The joiner waits on `joinCondition` owned by the Ref-counted ThreadState;
  the JSThread cell (conservatively rooted on the joiner's stack, S4
  argument) holds the Ref, and `threadMain` itself holds `Ref<ThreadState>`
  + `Ref<VM>` (:395-396), so neither side of the rendezvous can be freed
  under the other.
- Completion is Phase release-store + `notifyAll` + asyncJoiners swap-out,
  all under `joinLock`, settles dropped-lock (:286-295) — a joiner cannot
  observe Running after the joinee published its result, and cannot park
  after `notifyAll` without re-checking Phase under the lock (the :450 loop
  re-checks Phase under the re-taken lock after every quantum and after the
  W1 episode).
- asyncJoin tickets registered after death settle immediately; tickets never
  settled because the joinee's TS dies (asyncJoin raced thread teardown's
  last ref) are drained at ~ThreadState (:138-143) — the "node orphaned
  across agent death" arm, explicitly handled.
- A joiner parked when the JOINER's VM is terminated exits via the D9 poll
  (:451-454) — same TERM1 rule-4 shape as every other park.

## S6. Thread-granular stop-the-world vs D9 park sites — **susceptible-suspected** (known open; AB-17b)

**Surface.** `parkSitePollAndParkForStopTheWorld` + FIX-2 banner,
Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp:415-460, and the stop
watchdog `watchdogAssertStopProgress` :375-412. Governed by SPEC-ungil
§A.3.2 (NVS park tickets; "parked implies access-released" conductor
predicate) and UNGIL-HANDOUT §A.3.2.

**Suspected hole, precisely:** the §A.3.2 conductor predicate converges only
if every entered thread either reaches a poll site or is parked with heap
access released. The FIX-2 banner (:416-429) records that the W1/D9
park-site split left the D9 quantum loops (TA per-wait nodes, property wait,
Lock/Condition/Thread parks) neither polling the §A.3 stop word nor (on the
affected paths) releasing heap access — so a waiter parked across a
thread-granular stop request stalls the conductor while the waiter's
NOTIFIER is itself ticket-parked by the same stop fan: a true deadlock that
`watchdogAssertStopProgress` converts into a deliberate 30s fail-stop crash
(:401-412). The remedy helper exists in-tree but is **not wired**: grep
confirms `parkSitePollAndParkForStopTheWorld` has zero call sites outside
its own TU — none of the five D9 loops audited in S1-S5 calls it.

Classification: availability-only (deterministic fail-stop, no silent
corruption — the watchdog exists precisely to keep this class from becoming
a wedge), and it is the exact second signature the thread-ab17b charter
already owns ("STW watchdog timeout on jettison-requested stops,
JSThreadsSafepoint.cpp:412"). No separate susceptibility test is filed: the
AB-17b pinned GIL-off verification ladder is the binding reproducer/verify,
and a JS test today would only re-document the known watchdog crash on a
mid-bring-up tree. MC-WAIT obligation on the fix, recorded for that review:
wiring the poll into each D9 loop must (a) run with NO rank-3 lock held
(FIX-2 already states this, :424-429) and (b) route the post-wake path
through the W1 disposition rules (re-validate the wait predicate before
re-sleeping) so the stop-park cannot strand a consumed notify — i.e. the fix
must not reintroduce S1/S3's solved races while closing the convergence
hole.

---

## Summary table

| # | Surface | Governing spec | Verdict |
|---|---------|----------------|---------|
| S1 | TA sync wait per-wait nodes (WaiterListManager.cpp:202-314) | SPEC-ungil §C.6/SD6/SD8, D9, annex W W1, r15 F2 | immune-by-construction |
| S2 | TA waiter lists vs VM/SAB death (WaiterListManager.cpp:599-681) | SPEC-api 5.10; §E.5/TERM1; ThreadObject.cpp:391-396 VM pin | immune-by-construction (two load-bearing mechanisms recorded) |
| S3a | Property wait GIL-off pre-enqueue re-validation (ThreadAtomics.cpp) | SPEC-ungil §C.3 (annex C3 BINDING) — **LANDED 2026-06-10** | **CLOSED** — regression test mc-wait-property-wait-lost-wakeup.js 20/20 |
| S3b | Property wait timeout/notify/termination arbitration (ThreadAtomics.cpp:1040-1104, :1168-1194, :1224-1247) | SPEC-api 5.6/5.5, §E.4 | immune-by-construction |
| S3c | Property-wait cell/list/timer lifetime (ThreadAtomics.cpp:804-958, :1156-1167) | SPEC-api 5.10, D5 | immune-by-construction |
| S4 | Lock/Condition freed-while-parked (LockObject.h:39-137, LockObject.cpp:542-615, ConditionObject.cpp:100-275) | SPEC-api 5.3/5.4/5.9, F3/I9 | immune-by-construction |
| S5 | join/asyncJoin across joinee death (ThreadObject.cpp:123-143, :286-295, :404-475) | SPEC-api F5/5.5/5.10 | immune-by-construction |
| S6 | Thread-granular STW vs D9 parks (JSThreadsSafepoint.cpp:415-460) | SPEC-ungil §A.3.2; FIX-2 | **susceptible-suspected** — known open, owned by AB-17b (fail-stop, not corruption) |
