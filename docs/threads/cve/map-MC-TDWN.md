# MC-TDWN — teardown vs in-flight work: mapping to our threads surface

Mechanism class (from the catalog; web-derived exemplars treated as data):
*"Agent/object teardown vs in-flight work: shutdown frees state still
targeted by queued or executing work, or one thread introspects another's
stack/state while that thread is exiting."* Exemplars: CVE-2020-12387
(Firefox: worker shutdown races in-flight runnables, UAF), CVE-2024-35264
(.NET HTTP/3: stream disposed while request body still writing), JDK-6805108
(java.util.Timer: cancel vs executing task), Erlang ETS fixation-vs-delete,
Go sync.Pool cross-request reuse-after-handoff.

Audited against the tree at jarred/threads (phase-1 GIL'd API landed,
UNGIL §A/§B/§E/EXIT1/D.1 machinery landed, GIL-off bring-up in progress).
Specs of record: SPEC-api.md (frozen), SPEC-ungil.md rev 32 + BINDING
annexes (UNGIL-HANDOUT.md), SPEC-vmstate.md §6.4.4.

Verdict legend: **immune** = immune-by-construction (protocol cited, with
the adversarial argument), **needs-test** = susceptibility test written
under `JSTests/threads/cve/` (run post-ungil), **suspected** =
susceptible-suspected with the precise hole.

---

## S1. ~VM vs in-flight spawned-thread teardown tails (the CVE-2020-12387 analog)

The mechanism: the VM (the "agent host") is destroyed while a spawned
thread's exit tail still touches VM-owned state — the server `Heap`
(`delete client` runs lastChanceToFinalize against it,
`runtime/ThreadManager.cpp:650-656`), the DWT, the registry.

Defenses, two-layer:

1. **Spawn-time `Ref<VM>`** — `runtime/ThreadObject.cpp:391-397`: the
   thread lambda captures `protectedVM = Ref { vm }`, so ~VM cannot begin
   while any `threadMain` is anywhere between entry and return. This also
   covers the EXIT1.9 *residual tail* (the `lite = nullptr` free at
   `ThreadObject.cpp:330`, whose M12 default-queue removal touches
   `vm.m_microtaskQueues`): it runs inside `threadMain`, strictly before
   the lambda's Ref drops.
2. **EXIT1.9 normative fence** — `runtime/VM.cpp:978-1004` +
   `VM.cpp:1035-1050` (SPEC-ungil §B.2, EXIT1.3/1.9 as amended r31/r32):
   ~VM blocks under the registry lock until no registered lite other than
   `m_mainVMLite` points at this VM. The T5 tail
   (`ThreadManager.cpp:600-663`) keeps the lite *physically registered*
   through the entire server-touching sequence (TEARDOWN mark at step 2,
   `delete client` at step 4) and unregisters LAST (step 5, the notifying
   wrapper, U20 r31). So a joiner that observed completion at the F5 settle
   (`ThreadManager.cpp:818`) and immediately destroys the VM parks at the
   fence until the joinee's step 5. Progress argument at `VM.cpp:983-989`
   (exit is un-gated, acquires only the leaf registry lock).

Adversarial probes:

- *Fence vs walk ordering*: the A36 foreign-carrier walk (step 2 of ~VM,
  `VM.cpp:882-952`) wholly precedes the wait, and TEARDOWN lites are
  skipped by the walk (`VM.cpp:896-897`) but **counted** by the fence
  predicate (`VM.cpp:995-1001` counts every registered lite) — no gap.
- *Notify-after-unlock*: `unregisterVMLiteAndNotifyTeardown` notifies after
  the internal lock hold drops; sound because both waiters are predicate
  loops under the registry lock and `Condition::wait` enqueues before
  releasing (banner at `runtime/ThreadManager.h:686-712`).
- **Residual suspected sub-case (last VM deref on the spawned thread)**:
  if the embedder drops its last external ref while a spawned thread runs,
  the thread lambda's `Ref<VM>` is the final reference and ~VM executes on
  the spawned thread, after `threadMain` returned — with **no API lock
  held and no entry token**. `VM.cpp:1032`
  `ASSERT(currentThreadIsHoldingAPILock())` is debug-only; in release,
  `m_apiLock->uninstallVMLiteForVMDestruction()` (step 1) then runs on a
  thread that never had a carrier installed, and the §F.2 "destroying
  thread's token survives teardown" premise (`VM.cpp:1026-1031`) is false.
  GIL-on the same shape reaches `deferredWorkTimer->stopRunningTasks()`
  etc. without the lock. This is exactly the JDK-6805108/12387 shape:
  the *destructor inherits a context the protocol assumed was the
  owner's*. Not JS-reachable through a healthy embedder (the documented
  contract is join-then-destroy under the lock), and the jsc shell holds
  its VM ref until process exit — but nothing fail-stops the bad shape in
  release builds.
  **Verdict: suspected** (embedder-API surface; recommend a RELEASE_ASSERT
  or a documented re-dispatch in ~VM when
  `!currentThreadIsHoldingAPILock()`), plus **needs-test** for the
  JS-reachable neighbor: unjoined threads mid-exit at shell teardown —
  `JSTests/threads/cve/mc-tdwn-vm-teardown-unjoined.js`.

Overall: **immune** for join-then-destroy and drop-while-running (fence +
Ref), **suspected** for the last-deref-on-spawned-thread placement above.

## S2. E2A inbox close vs concurrent cross-thread settles (queued work targeting a dying queue)

The mechanism instance: thread B settles a ticket whose registrant A is
concurrently exiting; the settle targets A's `taskQueue`, which A's close
block is about to harvest and abandon. The freed-state variant would be a
`ThreadTask` appended to a queue that is never drained (lost settle) or
drained after the owner freed per-thread state (UAF).

Governing protocol: SPEC-ungil §E.4 routing + §E.5 close + §E.3 rule 3
(ANNEX E2A, BINDING). Implementation:

- `AsyncTicket::settleViaRegistrantRouting`,
  `runtime/ThreadManager.cpp:139-172`: decide-under-`inboxLock` /
  act-after-drop. Open ⇒ append + rule-1 keepalive decrement + wake,
  all atomic under the same lock the closer and the E2A exit predicate
  use (`ThreadManager.cpp:885-906`). Closed ⇒ CAS-claim then the landed
  `scheduleWorkSoon` main fallback. `inboxOpen` is **monotone**
  (open exactly once pre-fn at `ThreadManager.cpp:671-683`, false forever
  at close `ThreadManager.cpp:755`), so the post-drop fallback can never
  race a reopen.
- Close residue: the harvest swaps the queue out under `inboxLock`
  (`ThreadManager.cpp:751-758`) and routes every residue task to the main
  fallback (`ThreadManager.cpp:779-782` →
  `routeQueuedTaskToMainFallback`, which re-checks `isCancelled`).
  Nothing is dropped; tickets already won their settle CAS at enqueue.
- Lifetime: a queued `ThreadTask` holds `Ref<AsyncTicket>`
  (`ThreadManager.h:252-257`); the ticket holds `Ref<ThreadState>`
  registrant (`ThreadManager.h:101-108` — "tickets are process-owned and
  outlive their registering thread") and the `Strong<JSPromise>`. No
  per-thread state referenced by a settle is freed before the last ticket
  ref drops, and `~ThreadState` RELEASE_ASSERTs the inbox is closed and
  empty (`ThreadManager.h:291-321`).

Adversarial probes: settle appends in the same `inboxLock` hold in which
it observed open, and close flips + swaps in one hold — no interleaving
where an append lands after the swap but before the flip. Keepalive
counter death-after-close (§E.3 rule 3) prevents the symmetric bug
(decrement against a dead counter / uint64 wrap): never-armed tickets lose
the `m_keepaliveReleased` CAS by construction (constructed-released,
`ThreadManager.h:159-204`).

Caveat recorded: gate **U-T9-INT1** is open (`ThreadManager.h:184-203`) —
the four `countsKeepalive=true` call-site edits have not landed, so today
*every* ticket is never-armed: a spawned thread's loop exits at fn-return
+ queue-empty and late settles take the main fallback. Declared safe
(api 4.6.2 class — no hang, no wrap) but the §E.3 liveness semantics are
not yet in force; the test below must pass in both regimes.

**Verdict: immune** (protocol), **needs-test** for the end-to-end
exactly-once/no-loss observable —
`JSTests/threads/cve/mc-tdwn-exit-vs-settle.js` (exit-vs-notify storm over
asyncHold grants, finite-timeout property waitAsync, and asyncJoin).

## S3. join()/asyncJoin introspecting an exiting thread's state

The "one thread introspects another's state while it is exiting" arm,
benign-introspection variant. `runtime/ThreadObject.cpp:404-567`:

- Phase/joiner-list atomicity: asyncJoin's phase check + append are one
  `joinLock` hold (`ThreadObject.cpp:553-563`); the completion sequence's
  Phase release-store + `asyncJoiners` swap are one `joinLock` hold
  (GIL-on `ThreadObject.cpp:290-296`; GIL-off close block
  `ThreadManager.cpp:815-821`). A ticket is either swapped (settled by the
  completer) or observes a final phase — no lost settle.
- `TS::result` read: joiners load-acquire Phase first (F1 release-store
  pairs); the Strong is cleared **only** by the 5.10 finalizer hook
  (`ThreadObject.cpp:117-146`), which cannot run while the joiner holds
  the JSThread cell that the hook is keyed on.
- The joinee's native handle is detached (`ThreadObject.cpp:387-397`);
  join synchronizes purely through `joinCondition` on the refcounted
  ThreadState — no pthread-handle UAF class at all.
- GIL-off joiner-park D9 quanta + W1 watchdog episode
  (`ThreadObject.cpp:428-472`) hold `joinLock` only between sleeps and
  re-check phase under the re-taken lock, so completion racing a
  termination/watchdog episode resolves under the lock.

**Verdict: immune.** (Exercised incidentally by both tests.)

## S4. ThreadState last-ref destruction off the JSLock vs still-set GC roots

Exiting *embedder* threads drop their TLS `RefPtr<ThreadState>` at an
unbounded future time, possibly after VM death — the classic "shutdown
frees state (Strongs) still registered with the collector". Defense:

- `~ThreadState` RELEASE_ASSERTs every Strong cleared, inbox closed+empty,
  deadlines harvested, joiners drained (`ThreadManager.h:291-321`) —
  fail-stop, not silent UAF.
- The 5.10 finalizer hook (`ThreadObject.cpp:117-146`) is the sole clearer
  of `result` and drains abandoned asyncJoiners' promise Strongs **under
  the JSLock** (GC finalization / lastChanceToFinalize) — including the
  never-completing lazy tid-0 ThreadStates from `Thread.current` on
  embedder threads, the exact shape where the last ref drops off-lock.
- Tickets drained there were never passed to a settle path (comment at
  `ThreadObject.cpp:130-137`), so no settle task later reads the cleared
  Strong; their DWT pending work falls to VM-shutdown cancelPendingWork.

**Verdict: immune** (assert-backed; the asserts make any future regression
deterministic rather than exploitable).

## S5. STW conductor / ~VM walk introspecting an exiting thread's lite + client heap

The direct "introspect another's state while it exits" arm. The exiting
T5 tail leaves `lite.clientHeap` **dangling** after step 4
(`ThreadManager.cpp:650-656`, EXIT1.4(b): never nulled while registered)
— so every registry walker is one missing state-check away from a UAF.

Defense (SPEC-ungil EXIT1.1/1.2/1.4, U20): conductors reach lites only
through `forEachEnteredThread` (`runtime/VMManager.cpp:315-346`), which
under the registry lock skips `state != Live` **before** any client deref
(`VMManager.cpp:339-342`). Ordering argument: the exiting thread's
TEARDOWN mark (step 2, `ThreadManager.cpp:626-630`) is under the registry
lock and strictly precedes `delete client` (step 4); a walker holding the
lock either runs before the mark (client still alive — the delete cannot
have happened) or after it (lite skipped). Audited every
`->clientHeap` reader in the tree:

- `VMManager.cpp:341,373,398` — guarded by `state == Live` under the lock.
- `VM.cpp:903-926` (A36 walk) — Teardown skipped at `VM.cpp:896`,
  spawned lites excluded by the U-T6 carrier-TID range check at
  `VM.cpp:894` (the whole point of the TID-space partition).
- `JSLock.cpp` sites — all current-thread-own-carrier paths (the owner
  cannot race its own exit).
- `VMLite.cpp:458,508-525` — own-thread paths.

Deleted-state discrimination after the walk: the lite's `state` byte is
read only under the registry lock and the lite is freed only by the party
that observed DETACHED (`VM.cpp:928-949` r32 walk-free vs owner-dtor
split) — "the byte is never read after free".

**Verdict: immune.** Residual obligation (not a hole): U20 is a
*convention* — any future walker bypassing `forEachEnteredThread`
reintroduces the class. Flagged for the scanners phase (grep-able
invariant: no `lites` iteration outside the audited files).

Adjacent known defect (recorded, owned elsewhere): the
`JSThreadsSafepoint.cpp:412` watchdog 30s STW-timeout on
jettison-requested stops (thread-ab17b root cause B) is a
teardown/stop *liveness* failure in this neighborhood, already triaged
with a chartered fix; not re-audited here.

## S6. Cross-thread stack/scope-chain introspection of a dead or foreign stack

The "introspects another's **stack** while that thread is exiting" arm —
the closest direct hit of MC-TDWN in our tree.

- **GIL-on: defended.** A fresh GIL holder may inherit VM-wide fields
  pointing into a previous (possibly dead) holder's stack — the
  EXCEPTION_SCOPE_VERIFICATION scope chain.
  `GILParkSavedExecutionState::resetForFreshThread`
  (`ThreadObject.cpp:206-214`, LockObject.h) scrubs per-thread execution
  state at every spawned-thread entry. **Immune** GIL-on.
- **GIL-off: susceptible — KNOWN.** Spawned threads walk an exception
  scope chain anchored in the *carrier's* stack:
  `ExceptionScope::stackPosition` stack-use-after-return on spawned
  threads (thread-ab17b root cause A; the per-lite exception-state /
  scope-chain reroute of §A.1.4 is not finished). This is precisely
  "thread A consumes pointers into thread B's stack after B's frame is
  gone".

**Verdict: suspected (confirmed, fix chartered via thread-ab17b).** No new
test written — the ab17b verify ladder pins the reproducer; duplicating
it here would drift.

## S7. DWT shutdown / cancellation vs queued or executing settle tasks (the JDK-6805108 analog)

- VM-shutdown ordering: `deferredWorkTimer->stopRunningTasks()` and the
  §E.7.3 purge run in ~VM (`VM.cpp:1074`, `VM.cpp:1112-1119`) — i.e.
  strictly after the S1 fence, so no spawned inbox or E2A loop can still
  reference a DWT being shut down. Cancelled tickets are checked at every
  dispatch site (`DeferredWorkTimer.cpp:412-432`,
  `cancelPendingWorkSafe`'s gilOff decide/act split at
  `DeferredWorkTimer.cpp:572-599`), and `AsyncTicket::settle` re-checks
  `isCancelled` after winning its CAS (`ThreadManager.cpp:122`).
- One asymmetry, defended by rooting rather than by a check:
  `AsyncTicket::runQueuedSettleTaskOnRegistrant`
  (`ThreadManager.cpp:174-185`) runs the task body **without** an
  `isCancelled` guard (its sibling `routeQueuedTaskToMainFallback` has
  one, `ThreadManager.cpp:193`). Mid-life cancellation sources
  (`JSGlobalObject::clearWeakTickets` → `cancelPendingWorkSafe`,
  `JSGlobalObject.cpp:4437-4445`; GC-End `cancelPendingWork(VM&)`) can
  only cancel a ticket whose target/realm died — impossible while the
  ticket's own `Strong<JSPromise>` (cleared only at settle/finalizer,
  `ThreadManager.h:110-116`) pins the promise and hence its global. The
  raw `thread`/dependency cells captured by settle lambdas
  (`ThreadObject.cpp:154-169`) are rooted by the DWT ticket's dependency
  vector, dropped only at cancel — same argument.
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
  (`ThreadManager.h:608-617`, `ThreadManager.cpp:945-954`,
  `ThreadAffinityWeakHandleOwner`), so a stale finalizer cannot evict a
  successor restriction installed at a reused address — the exact
  deleted-slot-reuse hazard (THREAD.md regime 3). **Immune.**
- Owner thread exits while foreign threads consult/violate the
  restriction: `ThreadAffinityEntry` holds `Ref<ThreadState>` owner
  (`ThreadManager.h:405-416`) — no UAF; ownership checks compare
  `owner->nativeThread` against the current thread, so after owner death
  every thread is Foreign ⇒ fail-closed (ConcurrentAccessError), never
  fail-open. **Immune.**

## S9. Finite-timeout wait deadlines vs exiting registrant (Timer-cancel analog)

`ThreadWaitDeadline` expiry (`ThreadManager.cpp:705-727`) and the §E.5
close harvest (`ThreadManager.cpp:770-773`) both funnel through
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

Design defense (SPEC-ungil §D.1, ANNEXES D1+D1R; `ThreadManager.h:486-573`,
`ThreadManager.cpp:439-563`, Heap.cpp `conductSharedCollection`): retired
TIDs are only reissued after a **full shared-GC stop** restamps every live
tag to 0 and fires `fireTransitionThreadLocal` (jettisoning every baked
immediate) — phase 3's free-list release is ordered after the in-stop
restamp by the Sealed→Restamped→Idle state machine; late retires wait a
cycle; the per-range partition keeps carrier/spawned reissue disjoint.
The exiting thread's residual tail "never installs new tagged state"
(soundness paragraph, `ThreadManager.h:525-531`).

But: the three chartered verification arms are **recorded-deferred**
(U-T12 deferral, `ThreadManager.h:554-572`) — the protocol has never been
executed end-to-end. **Verdict: needs-test** —
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
| S5 | conductor walk vs dangling clientHeap | immune | scanners-phase grep invariant (U20); ab17b owns the adjacent watchdog defect |
| S6 | cross-thread stack/scope-chain | GIL-on immune / **suspected GIL-off (known)** | owned by thread-ab17b; do not duplicate |
| S7 | DWT cancel vs queued settle | immune (rooting) | hardening: add isCancelled guard in `runQueuedSettleTaskOnRegistrant` |
| S8 | affinity prune / dead owner | immune | — |
| S9 | deadline expiry vs exit vs notify | immune | covered by S2 test |
| S10 | TID reuse after thread death | design-immune, **unverified** | test `mc-tdwn-tid-recycle-storm.js` (U-T12 arm 1 shape) |

Tests are written for post-ungil execution (do not run against the
mid-bring-up tree); flag requirements are in each test's `//@` header.
