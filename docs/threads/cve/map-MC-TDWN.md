# MC-TDWN — teardown vs in-flight work: mapping to our threads surface

Mechanism class (from the catalog; web-derived exemplars treated as data):
*"Agent/object teardown vs in-flight work: shutdown frees state still
targeted by queued or executing work, or one thread introspects another's
stack/state while that thread is exiting."* Exemplars: CVE-2020-12387
(Firefox: worker shutdown races in-flight runnables, UAF), CVE-2024-35264
(.NET HTTP/3: stream disposed while request body still writing), JDK-6805108
(java.util.Timer: cancel vs executing task), JDK-8211821 (table walk after
last JavaThread detached), Erlang ETS fixation-vs-delete, Go sync.Pool
cross-request reuse-after-handoff.

Audited against the tree at jarred/threads (re-audited post-ab17b/U-T8b:
per-lite exception-scope state landed; T5/EXIT1.9/E2A machinery stable;
TERM1 cooperative termination routed through the same close block). Specs
of record: SPEC-api.md (frozen), SPEC-ungil.md rev 32+ BINDING annexes
(UNGIL-HANDOUT.md), SPEC-vmstate.md §6.4.4.

Verdict legend: **immune** = immune-by-construction (protocol cited, with
the adversarial argument), **needs-test** = susceptibility test written
under `JSTests/threads/cve/` (run post-ungil), **suspected** =
susceptible-suspected with the precise hole.

---

## S1. ~VM vs in-flight spawned-thread teardown tails (the CVE-2020-12387 analog)

The mechanism: the VM (the "agent host") is destroyed while a spawned
thread's exit tail still touches VM-owned state — the server `Heap`
(`delete client` runs lastChanceToFinalize against it,
`runtime/ThreadManager.cpp:696`), the DWT, the registry.

Defenses, two-layer:

1. **Spawn-time `Ref<VM>`** — `runtime/ThreadObject.cpp:463-468`: the
   thread lambda captures `protectedVM = Ref { vm }`, so ~VM cannot begin
   while any `threadMain` is anywhere between entry and return. This also
   covers the EXIT1.9 *residual tail* (the `lite = nullptr` free whose M12
   default-queue removal touches `vm.m_microtaskQueues`): it runs inside
   `threadMain`, strictly before the lambda's Ref drops.
2. **EXIT1.9 normative fence** — `runtime/VM.cpp:1014-1040` +
   `VM.cpp:1042-1096` (SPEC-ungil §B.2, EXIT1.3/1.9 as amended r31/r32):
   ~VM blocks under the registry lock until no registered lite other than
   `m_mainVMLite` points at this VM. The T5 tail
   (`ThreadManager.cpp:640-703`) keeps the lite *physically registered*
   through the entire server-touching sequence (TEARDOWN mark at step 2
   `:669`, `delete client` at step 4 `:696`) and unregisters LAST (step 5,
   the notifying wrapper `:702`, U20 r31). So a joiner that observed
   completion at the F5 settle (`ThreadManager.cpp:858`) and immediately
   destroys the VM parks at the fence until the joinee's step 5. Progress
   argument at `VM.cpp:1019-1025` (exit is un-gated, acquires only the
   leaf registry lock).

Adversarial probes:

- *Fence vs walk ordering*: the A36 foreign-carrier walk (step 2 of ~VM,
  `VM.cpp:851-960`) wholly precedes the wait, and TEARDOWN lites are
  skipped by the walk (`VM.cpp:931`) but **counted** by the fence
  predicate (`VM.cpp:1031-1037` counts every registered lite) — no gap.
- *Notify-after-unlock*: `unregisterVMLiteAndNotifyTeardown`
  (`ThreadManager.cpp:605-609`) notifies after the internal lock hold
  drops; sound because both waiters are predicate loops under the registry
  lock and `Condition::wait` enqueues before releasing.
- **Residual suspected sub-case (last VM deref on the spawned thread)**:
  if the embedder drops its last external ref while a spawned thread runs,
  the thread lambda's `Ref<VM>` is the final reference and ~VM executes on
  the spawned thread, after `threadMain` returned — with **no API lock
  held and no entry token**. `VM.cpp:1068`
  `ASSERT(currentThreadIsHoldingAPILock())` is debug-only; in release,
  `m_apiLock->uninstallVMLiteForVMDestruction()` (step 1) then runs on a
  thread that never had a carrier installed, and the §F.2 "destroying
  thread's token survives teardown" premise (`VM.cpp:1062-1063`) is false.
  GIL-on the same shape reaches `deferredWorkTimer->stopRunningTasks()`
  etc. without the lock. This is exactly the JDK-6805108/12387 shape:
  the *destructor inherits a context the protocol assumed was the
  owner's*. Not JS-reachable through a healthy embedder (the documented
  contract is join-then-destroy under the lock), and the jsc shell holds
  its VM ref until process exit — but nothing fail-stops the bad shape in
  release builds.
  **Verdict: suspected → FAIL-STOPPED (B12 landed)**. The release-build
  fail-stop is in place at the top of the `if (m_mainVMLite)` block in
  `~VM` (`runtime/VM.cpp`, immediately after the §F.2 debug ASSERT):
  threads-on (`Options::useJSThreads()`, `[[unlikely]]`) gates a
  `RELEASE_ASSERT_WITH_MESSAGE(currentThreadIsHoldingAPILock(), …)` naming
  the MC-TDWN S1 protocol violation and the join-then-destroy-under-lock
  embedder contract. Flag-off is byte-identical (the block is unreachable
  — `m_mainVMLite` is null without `useVMLite`; the pre-existing debug
  ASSERT remains the sole check). The bad shape now crashes
  deterministically in release rather than silently running step (1)
  onward without a carrier or token. Plus **needs-test** for the
  JS-reachable neighbor: unjoined threads mid-exit at shell teardown —
  `JSTests/threads/cve/mc-tdwn-vm-teardown-unjoined.js` (gate: 20/20
  clean exit; the shell holds its VM ref to process exit so the
  fail-stop is not on the test's path).

Overall: **immune** for join-then-destroy and drop-while-running (fence +
Ref), **fail-stopped** for the last-deref-on-spawned-thread placement
above (B12).

## S2. E2A inbox close vs concurrent cross-thread settles (queued work targeting a dying queue)

The mechanism instance: thread B settles a ticket whose registrant A is
concurrently exiting; the settle targets A's `taskQueue`, which A's close
block is about to harvest and abandon. The freed-state variant would be a
`ThreadTask` appended to a queue that is never drained (lost settle) or
drained after the owner freed per-thread state (UAF).

Governing protocol: SPEC-ungil §E.4 routing + §E.5 close + §E.3 rule 3
(ANNEX E2A, BINDING). Implementation:

- `AsyncTicket::settleViaRegistrantRouting`,
  `runtime/ThreadManager.cpp:158-191`: decide-under-`inboxLock` /
  act-after-drop. Open ⇒ append + rule-1 keepalive decrement + wake,
  all atomic under the same lock the closer and the E2A exit predicate
  use (`ThreadManager.cpp:897-960`). Closed ⇒ CAS-claim then the landed
  `scheduleWorkSoon` main fallback. `inboxOpen` is **monotone**
  (open exactly once pre-fn, false forever at close
  `ThreadManager.cpp:795`), so the post-drop fallback can never race a
  reopen.
- Close residue: the harvest swaps the queue out under `inboxLock`
  (`ThreadManager.cpp:793-798`) and routes every residue task to the main
  fallback (`ThreadManager.cpp:819-822` →
  `routeQueuedTaskToMainFallback` `:206-215`, which re-checks
  `isCancelled`). Nothing is dropped; tickets already won their settle
  CAS at enqueue.
- Lifetime: a queued `ThreadTask` holds `Ref<AsyncTicket>`; the ticket
  holds `Ref<ThreadState>` registrant and the `Strong<JSPromise>`. No
  per-thread state referenced by a settle is freed before the last ticket
  ref drops, and `~ThreadState` RELEASE_ASSERTs the inbox is closed and
  empty (`ThreadManager.h:303-332`).
- TERM1 cooperative termination (`terminated=true` at
  `ThreadManager.cpp:782/835-853`) routes through the **same** close
  block: identical harvest/route/complete sequence, only the published
  result differs. No new lifetime hazard; trap-delivery races are
  MC-AINT's surface.

Adversarial probes: settle appends in the same `inboxLock` hold in which
it observed open, and close flips + swaps in one hold — no interleaving
where an append lands after the swap but before the flip. Keepalive
counter death-after-close (§E.3 rule 3) prevents the symmetric bug
(decrement against a dead counter / uint64 wrap): never-armed tickets lose
the `m_keepaliveReleased` CAS by construction.

Caveat recorded: gate **U-T9-INT1** is open — the four
`countsKeepalive=true` call-site edits have not landed, so today *every*
ticket is never-armed: a spawned thread's loop exits at fn-return +
queue-empty and late settles take the main fallback. Declared safe
(api 4.6.2 class — no hang, no wrap) but the §E.3 liveness semantics are
not yet in force; the test below must pass in both regimes.

**Verdict: immune** (protocol), **needs-test** for the end-to-end
exactly-once/no-loss observable —
`JSTests/threads/cve/mc-tdwn-exit-vs-settle.js` (exit-vs-notify storm over
asyncHold grants, finite-timeout property waitAsync, and asyncJoin).

## S3. join()/asyncJoin introspecting an exiting thread's state

The "one thread introspects another's state while it is exiting" arm,
benign-introspection variant. `runtime/ThreadObject.cpp:474-640`:

- Phase/joiner-list atomicity: asyncJoin's phase check + append are one
  `joinLock` hold; the completion sequence's Phase release-store +
  `asyncJoiners` swap are one `joinLock` hold (GIL-on
  `ThreadObject.cpp:364-368`; GIL-off close block
  `ThreadManager.cpp:855-861`). A ticket is either swapped (settled by the
  completer) or observes a final phase — no lost settle.
- `TS::result` read: joiners load-acquire Phase first (F1 release-store
  pairs); the Strong is cleared **only** by the 5.10 finalizer hook
  (`ThreadObject.cpp:117-148`), which cannot run while the joiner holds
  the JSThread cell that the hook is keyed on.
- The joinee's native handle is detached (`ThreadObject.cpp:463-468`);
  join synchronizes purely through `joinCondition` on the refcounted
  ThreadState — no pthread-handle UAF class at all.
- GIL-off joiner-park D9 quanta + W1 watchdog episode
  (`ThreadObject.cpp:509-548`) hold `joinLock` only between sleeps and
  re-check phase under the re-taken lock, so completion racing a
  termination/watchdog episode resolves under the lock.

**Verdict: immune.** (Exercised incidentally by both tests.)

## S4. ThreadState last-ref destruction off the JSLock vs still-set GC roots

Exiting *embedder* threads drop their TLS `RefPtr<ThreadState>` at an
unbounded future time, possibly after VM death — the classic "shutdown
frees state (Strongs) still registered with the collector". Defense:

- `~ThreadState` RELEASE_ASSERTs every Strong cleared, inbox closed+empty,
  deadlines harvested, joiners drained (`ThreadManager.h:303-332`) —
  fail-stop, not silent UAF.
- The 5.10 finalizer hook (`ThreadObject.cpp:117-148`) is the sole clearer
  of `result` and drains abandoned asyncJoiners' promise Strongs **under
  the JSLock** (GC finalization / lastChanceToFinalize) — including the
  never-completing lazy tid-0 ThreadStates from `Thread.current` on
  embedder threads, the exact shape where the last ref drops off-lock.
- Tickets drained there were never passed to a settle path (comment at
  `ThreadObject.cpp:126-140`), so no settle task later reads the cleared
  Strong; their DWT pending work falls to VM-shutdown cancelPendingWork.

**Verdict: immune** (assert-backed; the asserts make any future regression
deterministic rather than exploitable).

## S5. STW conductor / ~VM walk introspecting an exiting thread's lite + client heap

The direct "introspect another's state while it exits" arm. The exiting
T5 tail leaves `lite.clientHeap` **dangling** after step 4
(`ThreadManager.cpp:696`, EXIT1.4(b): never nulled while registered)
— so every registry walker is one missing state-check away from a UAF.

Defense (SPEC-ungil EXIT1.1/1.2/1.4, U20): conductors reach lites only
through `forEachEnteredThread` (`runtime/VMManager.cpp:383-412`), which
under the registry lock skips `state != Live` **before** any client deref
(`VMManager.cpp:407`). Ordering argument: the exiting thread's TEARDOWN
mark (step 2, `ThreadManager.cpp:666-670`) is under the registry lock and
strictly precedes `delete client` (step 4); a walker holding the lock
either runs before the mark (client still alive — the delete cannot have
happened) or after it (lite skipped). Audited every `->clientHeap` reader
in the tree:

- `VMManager.cpp:407-412,436,464` — guarded by `state == Live` under the
  lock.
- `VM.cpp:851-960` (A36 walk) — Teardown skipped at `VM.cpp:931`, spawned
  lites excluded by the U-T6 carrier-TID range check.
- `bytecode/JSThreadsSafepoint.cpp:722-736` (watchdog STW-timeout
  participant dump, **new since first audit**) — under the registry lock
  (`:713`) and skips `state != Live` at `:725` before dereferencing
  `clientHeap` at `:731-733`. Conforms to U20.
- `JSLock.cpp` sites — all current-thread-own-carrier paths (the owner
  cannot race its own exit).
- `VMLite.cpp` sites — own-thread paths.
- `vm.clientHeap` (the VM-embedded main client, distinct from
  `lite->clientHeap`) sites in `Heap.cpp`/`CompleteSubspace*`/
  `RetiredJITArtifacts.cpp`/`JSThreadsSafepoint.cpp` — refer to the
  *server-VM-embedded* main client, not a per-lite dangling pointer; not
  this surface.

Deleted-state discrimination after the walk: the lite's `state` byte is
read only under the registry lock and the lite is freed only by the party
that observed DETACHED (the r32 walk-free vs owner-dtor split) — "the
byte is never read after free".

**Verdict: immune.** Residual obligation (not a hole): U20 is a
*convention* — any future walker bypassing `forEachEnteredThread`
reintroduces the class. Flagged for the scanners phase (grep-able
invariant: no `lites` iteration outside the audited files).

Adjacent known defect (recorded, owned elsewhere): the
`JSThreadsSafepoint.cpp` watchdog STW-timeout on jettison-requested stops
(thread-ab17b root cause B) is a teardown/stop *liveness* failure in this
neighborhood, already triaged with a chartered fix; not re-audited here.

## S6. Cross-thread stack/scope-chain introspection of a dead or foreign stack

The "introspects another's **stack** while that thread is exiting" arm —
the closest direct hit of MC-TDWN in our tree.

- **GIL-on: defended.** A fresh GIL holder may inherit VM-wide fields
  pointing into a previous (possibly dead) holder's stack — the
  EXCEPTION_SCOPE_VERIFICATION scope chain.
  `GILParkSavedExecutionState::resetForFreshThread`
  (`ThreadObject.cpp:227`, LockObject.h) scrubs per-thread execution
  state at every spawned-thread entry. **Immune** GIL-on.
- **GIL-off: now defended (fix landed via U-T8b).** The per-lite
  `VMExceptionScopeVerificationState` (`runtime/VMLite.h:363-377`,
  obligation 10) anchors each spawned thread's ExceptionScope chain in
  its own lite, selected through `VM::exceptionScopeVerificationState()`
  — "a spawned thread's scope chain can never link into the carrier's
  stack (the deterministic GIL-off ExceptionScope::stackPosition()
  stack-use-after-return)". The chain only exists in
  ENABLE(EXCEPTION_SCOPE_VERIFICATION) builds; release builds never had
  the cross-stack walk. Analogous per-lite split for the DoesGC
  validation word (`VMLite.h:379-397`, AB18-C) closes the same shape for
  DFG-does-GC bookkeeping. **Immune** GIL-off (was suspected at the
  prior audit; ab17b root cause A is closed in the tree).

**Verdict: immune** (both regimes). The ab17b verify ladder pins the
reproducer; no new test written here.

## S7. DWT shutdown / cancellation vs queued or executing settle tasks (the JDK-6805108 analog)

- VM-shutdown ordering: `deferredWorkTimer->stopRunningTasks()` and the
  §E.7.3 purge run in ~VM — strictly after the S1 fence, so no spawned
  inbox or E2A loop can still reference a DWT being shut down. Cancelled
  tickets are checked at every dispatch site (`DeferredWorkTimer.cpp`
  `cancelPendingWorkSafe` gilOff decide/act split), and
  `AsyncTicket::settle` re-checks `isCancelled` after winning its CAS
  (`ThreadManager.cpp:141`).
- One asymmetry, defended by rooting rather than by a check:
  `AsyncTicket::runQueuedSettleTaskOnRegistrant`
  (`ThreadManager.cpp:193-204`) runs the task body **without** an
  `isCancelled` guard (its sibling `routeQueuedTaskToMainFallback` has
  one, `ThreadManager.cpp:212`). Mid-life cancellation sources
  (`JSGlobalObject::clearWeakTickets` → `cancelPendingWorkSafe`; GC-End
  `cancelPendingWork(VM&)`) can only cancel a ticket whose target/realm
  died — impossible while the ticket's own `Strong<JSPromise>` (cleared
  only at settle/finalizer) pins the promise and hence its global. The
  raw `thread`/dependency cells captured by settle lambdas are rooted by
  the DWT ticket's dependency vector, dropped only at cancel — same
  argument.
  **Immune today**, but brittle: an `if (m_ticket->isCancelled()) return;`
  at the top of `runQueuedSettleTaskOnRegistrant` would make it
  protocol-immune instead of rooting-immune. Recorded as a hardening
  recommendation (one line, no semantic change — cancelled tickets'
  settle tasks are defined no-ops).

**Verdict: immune** (with the hardening note above).

## S8. Affinity table: restricted-object death and owner-thread death (ETS fixation analog)

- Late Weak finalizer vs recycled cell address: `pruneRestrictedObject`
  takes the finalizing Weak's `expectedEntry` context and removes the
  entry only if it is still that entry
  (`ThreadManager.h:630-636`, `ThreadManager.cpp:985-1033,1086`,
  `ThreadAffinityWeakHandleOwner`), so a stale finalizer cannot evict a
  successor restriction installed at a reused address — the exact
  deleted-slot-reuse hazard (THREAD.md regime 3). **Immune.**
- Owner thread exits while foreign threads consult/violate the
  restriction: `ThreadAffinityEntry` holds `Ref<ThreadState>` owner
  (`ThreadManager.h:417-428`) — no UAF; ownership checks compare
  `owner->nativeThread` against the current thread, so after owner death
  every thread is Foreign ⇒ fail-closed (ConcurrentAccessError), never
  fail-open. **Immune.**

## S9. Finite-timeout wait deadlines vs exiting registrant (Timer-cancel analog)

`ThreadWaitDeadline` expiry (`ThreadManager.cpp:745-766`) and the §E.5
close harvest (`ThreadManager.cpp:810-813`) both funnel through
`tryDequeue` under the *waiter list's* rank-3 lock with
already-dequeued ⇒ skip (the in-flight notify wins; §E.5 harvest rule),
and `settleTimedOut` routes through the §E.4 settle whose `m_settled` CAS
is the exactly-once gate. Rank-3 locks never held together
(`inboxLock` dropped before `tryDequeue`, §LK). A waiter can therefore be
notified, timed out locally, and harvested at close concurrently and
still settles exactly once with a single value.

**Verdict: immune** (protocol); the exactly-once observable is asserted by
`mc-tdwn-exit-vs-settle.js` (notify racing registrant exit racing the
timeout).

## S10. TID retire/reissue vs a dead thread's residual tagged state (sync.Pool analog)

Teardown-then-reuse: a dead thread's TID survives in butterfly TID tags,
`Structure::m_transitionThreadLocalTID`, and DFG/FTL/IC bodies with baked
`tid<<48` immediates; reissuing it to a new thread without scrubbing would
hand the new thread the dead thread's thread-local fast paths.

Design defense (SPEC-ungil §D.1, ANNEXES D1+D1R; `ThreadManager.h:486-590`,
`ThreadManager.cpp:484-590`, Heap.cpp `conductSharedCollection`): retired
TIDs are only reissued after a **full shared-GC stop** restamps every live
tag to 0 and fires `fireTransitionThreadLocal` (jettisoning every baked
immediate) — phase 3's free-list release is ordered after the in-stop
restamp by the Sealed→Restamped→Idle state machine; late retires wait a
cycle; the per-range partition keeps carrier/spawned reissue disjoint.
The exiting thread's residual tail "never installs new tagged state".

But: the three chartered verification arms are **recorded-deferred**
(U-T12 deferral) — the protocol has never been executed end-to-end.
**Verdict: needs-test** —
`JSTests/threads/cve/mc-tdwn-tid-recycle-storm.js` lands the arm-(1)
spawn-storm shape (exhaustion → SD9 RangeError → rebias → recovery, with
dead-thread structures still reachable across the reissue). Arms (2)/(3)
remain U-T12 deliverables (multi-VM amplifier / D1R item-5 jettison arm —
need RaceAmplifier + $vm instrumentation, not expressible as a plain
corpus test).

---

## Summary table

| # | Surface | Verdict | Action |
|---|---------|---------|--------|
| S1 | ~VM vs spawned exit tails | immune (fence+Ref) / **suspected** sub-case | last-VM-deref-on-spawned-thread: recommend release-build fail-stop; test `mc-tdwn-vm-teardown-unjoined.js` |
| S2 | inbox close vs cross-thread settle | immune | test `mc-tdwn-exit-vs-settle.js` (exactly-once/no-loss observable; U-T9-INT1 both regimes) |
| S3 | join/asyncJoin vs completion | immune | covered by S2 test |
| S4 | TS last-ref off-lock vs Strongs | immune (assert-backed) | — |
| S5 | conductor walk vs dangling clientHeap | immune | scanners-phase grep invariant (U20); JSThreadsSafepoint.cpp dump path now audited too |
| S6 | cross-thread stack/scope-chain | **immune** (both regimes) | U-T8b per-lite scope state landed (was suspected; closed) |
| S7 | DWT cancel vs queued settle | immune (rooting) | hardening: add isCancelled guard in `runQueuedSettleTaskOnRegistrant` |
| S8 | affinity prune / dead owner | immune | — |
| S9 | deadline expiry vs exit vs notify | immune | covered by S2 test |
| S10 | TID reuse after thread death | design-immune, **unverified** | test `mc-tdwn-tid-recycle-storm.js` (U-T12 arm 1 shape) |

Tests are written for post-ungil execution (do not run against the
mid-bring-up tree); flag requirements are in each test's `//@` header.
