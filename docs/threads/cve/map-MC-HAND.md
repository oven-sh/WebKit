# MC-HAND — Cancellation / completion / ownership-handoff race: mapping to our threads surface

Mechanism class (from the catalog, CVE-AUDIT.md §MC-HAND): an async cancel,
completion, or ownership transfer races the operation it targets; shared
completion state is observed by the wrong logical owner — a
cross-request/cross-thread data leak even when memory-safe. Exemplars:
CVE-2025-47907 (database/sql cross-query row leak via cancel-vs-Scan),
CVE-2020-15586 / CVE-2021-36221 (net/http cancellation races), ERL-90
(erlang/otp #3471 socket ownership handoff — the transferred resource's
completion observed by the old owner).

Date: 2026-06-07. Tree: jarred/threads (phase 1 GIL'd complete, ungil in
progress). Specs consulted: SPEC-api rev 14 (§4.6/F5/5.5/5.5a/5.6/5.7),
SPEC-ungil + UNGIL-HANDOUT rev 32 (§E.1-E.7, §A.2, annex N6, ANNEX WS1),
SPEC-objectmodel rev 14. Read-only audit; tests under JSTests/threads/cve/
are written but NOT executed (bring-up loop owns the build); they run
post-ungil via thread-cve-audit.

Why this class matters for us: the catalog already names our surface —
"Thread.join/terminate vs running thread; cross-thread promise/microtask
handoff; native-affinity handoff". Unlike MC-DF (where the spec machinery is
visibly anti-double-fetch), the MC-HAND defenses are scattered across many
small exactly-once gates: Phase release-store ordering (F1), the joinLock
append-vs-swap protocol (F5), the AsyncTicket m_settled CAS, the §E.3/E.4
keepalive + inbox-close routing, the PropertyWaiter flip-under-listLock rule,
and the affinity-table identity checks. Each gate is argued adversarially
below. One surface earns **susceptible-suspected**.

---

## S1. Thread.join: completion vs termination request (cancel-vs-complete)

**Surface.** A joiner parked in `Thread.prototype.join` while (a) the joinee
completes and (b) a watchdog/embedder termination request lands — the classic
cancel-races-the-operation-it-targets shape. The joiner must not report
termination state as the joinee's result, must not read a half-published
result, and must not lose its wakeup.
Source/JavaScriptCore/runtime/ThreadObject.cpp:404-523.

**Governing protocol.**
- F1 (SPEC-api 4.6.1): the result Strong is written BEFORE the Phase
  release-store; joiners load-acquire Phase before reading the result
  (ThreadObject.cpp:243-256 GIL-off / :280-284 GIL-on writer side; :417,
  :450, :518 reader side). The pair {Phase, result} is published as a unit,
  and Phase transitions Running -> Finished|Failed exactly once — completion
  state cannot be observed torn or attributed to the wrong outcome arm.
- F5: the joiner's wait loop re-checks Phase under `joinLock` on every 10 ms
  quantum BEFORE polling termination (ThreadObject.cpp:450-472) — "a
  completion observed under the lock takes priority over a concurrent
  termination request" (:449) — and the writer's notifyAll is under the same
  lock (:290-294), so no lost wakeup.
- Termination surfacing on the loser arm is request-then-throw on the
  joiner's own VM/lite state (:474-484), never a write into the joinee's
  ThreadState — the cancel can never corrupt the joinee's completion record.
- AB-17 W1: GIL-off, the park polls termination against the PARK lite's word
  only (`parkLitePollTerminationRequested(vm, parkLite)`, :428-470), and the
  watchdog-check bit is serviced by the W1 reacquisition episode with
  `joinLock` dropped (:455-470) — the loop re-checks Phase under the
  re-taken lock, so a completion that raced the episode still wins.

**Adversarial self-check.**
(a) Completion landing between the Phase check and the termination poll:
the joiner then reports termination even though the joinee finished. That is
the correct priority — termination is a VM-wide cancel (TERM1.2: the shared
NeedTermination trap bit stays visible while any lite is entered;
VM.h:1836-1851), not a per-join cancel, so there IS no "wrong owner": every
thread of the VM is being cancelled. The joinee's result Strong is still
cleared only by the 5.10 finalizer hook (ThreadObject.cpp:117-146), so
nothing dangles.
(b) Wrong-target cancellation (the ERL-90 shape — cancel delivered to the
wrong session) cannot arise because termination has exactly one target
granularity: the whole VM. There is no per-Thread terminate() in the frozen
API (SPEC-api 4.1) — the design refuses the exemplar's surface outright.
(c) The GIL-off joiner-side microtask drain (:486-514) is gated so a
non-main embedder joiner with no current lite cannot drain the main
carrier's queue from a foreign thread — jobs are delayed to their natural
turn, never run on a wrong-owner thread.

**Verdict: immune-by-construction** (single joinLock arbiter + monotonic
exactly-once Phase + F1 ordering + VM-granular cancellation). Covered by the
existing JSTests/threads/lifecycle corpus (join-semantics, async-join);
no MC-HAND-specific oracle exists that S2's and S4's tests don't subsume.

## S2. asyncJoin ticket registration vs completion (lost wakeup / double settle)

**Surface.** `asyncJoin` appends a ticket while the joinee is completing —
the ticket must settle exactly once, with the right outcome arm, regardless
of which side wins. ThreadObject.cpp:525-567 (registration), :286-299
(completion swap), :154-169 (settle).

**Governing protocol.**
- F5: the phase check and the asyncJoiners append are both under `joinLock`
  (:553-563); the completion sequence's Phase store and asyncJoiners swap
  are both under the same lock (:290-295). A ticket is either swapped out
  (settled by the completion sequence) or observes phase != Running and is
  settled by the registering call's settleNow arm — never both, never
  neither. "The phase observed under the lock is final ... so deciding
  resolve-vs-reject from it is sound" (:550-552).
- AsyncTicket::settle is exactly-once by the m_settled CAS
  (ThreadManager.cpp:120); repeat asyncJoin calls mint distinct
  promise+ticket pairs (4.1) — completion state is never pooled across
  requests, which is precisely what made CVE-2025-47907 possible
  (a shared rows object observed by the cancelled query's successor).
- Settlement reads `thread->result()` ordered after the Phase release-store
  (:163-167 with the F1 comment); the ticket's dependency vector roots the
  JSThread cell (and hence, via the 5.10 finalizer registration, the result
  Strong's sole clearer) until settle.
- Abandoned tickets (receiver never completes: lazy main/embedder tid-0
  TSs, or VM teardown) are drained ONLY by the 5.10 finalizer hook
  (:117-146), which runs with the JSLock held; drained tickets "were never
  passed to settleJoinTicket ... so no settle task can later read the
  cleared Strong" (:131-137).

**Adversarial self-check.** Can the finalizer drain race a concurrent
asyncJoin appending to the same list? No: the finalizer runs only when the
JSThread cell is dead, and asyncJoin holds the cell as its receiver — a live
receiver keeps the hook from firing. Can a settleNow ticket double-settle
with a completion-sequence settle? No: settleNow tickets were never appended
(the append and the phase observation are one critical section). Residue is
implementation fidelity of the settle task's realm choice
(`promise->realm()`, :160) — the promise settles in ITS OWN realm regardless
of which thread runs the settle task, the same property the upstream
cross-realm-settle fix (51cc3feb7298) just hardened.

**Verdict: immune-by-construction** for the protocol; the dead-registrant
half of this surface is S4's test (which exercises this path end-to-end).

## S3. Ownership identity vs recycled identifiers (TID reuse; recycled cell addresses)

**Surface.** Every "wrong logical owner" bug needs an identity confusion.
Ours would be: (i) a retired thread's TID reused while TID-tagged butterflies
or lock/owner records still reference it; (ii) a dead restricted object's
affinity-table entry aliased by a new object allocated at the recycled cell
address — the ERL-90 shape, an ownership record outliving its owner.

**Governing protocol.**
- (i) is closed twice over. First: TIDs are retired forever (SPEC-api
  Deviation 10; ThreadObject.cpp:312 "The TID is retired forever") — there
  is no reuse to confuse. Second, belt-and-braces: no protocol uses TID as
  an OWNERSHIP identity anyway — lock-holder identity is the native Thread
  pointer (5.3), restrict-owner identity is the Ref<ThreadState> /
  nativeThread pointer, "never TID" (5.7.2; ThreadManager.cpp:963-975 —
  "compared, never dereferenced"); all lazy main/embedder TSs share tid 0
  precisely because tid is not identity. Butterfly TID tags are an access
  CHECK (jit P5/CS3), not an ownership grant, and a retired TID's tag can
  never match a live thread's.
- (ii) the affinity table treats any entry whose Weak is not
  Live-and-equal-to-the-probed-object as ABSENT (ThreadManager.cpp:976-985);
  restrictObject's stale-replace arm destroys the dead predecessor's entry,
  and the finalizer's identity check (context != entry) makes a racing late
  finalizer a no-op (:1023-1052) — a recycled address can neither make a
  never-restricted object appear restricted nor evict a live successor.

**Verdict: immune-by-construction** (identity = unforgeable pointer + Ref
lifetime, never a recyclable small integer; recycled-address aliasing ruled
by the Weak-identity protocol). Same-address Weak races are additionally
covered at the GC level by the MC-DF S4 quarantine machinery.

## S4. AsyncTicket settle vs registrant death (inbox close; dead-thread handoff)

**Surface.** GIL-off, every asyncHold/asyncWait/property-waitAsync/asyncJoin
ticket is ROUTED to its registrant's inbox for settlement (§E.1/E.4). The
registrant can die (fn-return + queue drain, or termination) while a notify
on another thread is mid-settle: completion handed to an owner that no
longer exists — the purest MC-HAND shape we have.
ThreadManager.h:159-278 (§E.3/E.4 protocol docs), ThreadManager.cpp:120-198
(settle + routing), :742-938 (drain loop + close residue).

**Governing protocol (SPEC-ungil §E.3/E.4/E.5, BINDING).**
- Exactly-once: the m_settled CAS (ThreadManager.cpp:120) is won before any
  routing decision; cancelPendingWork and late timers lose it.
- The open-or-fallback decision is made UNDER the registrant's inboxLock
  (settleViaRegistrantRouting, :139-173): open => append + §E.3 rule-1
  keepalive decrement + wake, all in ONE inboxLock section ("no close can
  interleave between a decrement and its append", :144-149); closed => win
  the keepalive CAS but SKIP the dead counter, drop the lock, and route to
  the main fallback via the landed scheduleWorkSoon path (:165-172). Inbox
  closure is MONOTONIC ("open exactly once pre-fn, false forever at close",
  :168-170), so the post-drop fallback can never race a reopen.
- Close residue: tasks already queued when the inbox closes are harvested
  and re-routed to main (closeThreadInboxAndComplete, :781), each having
  already won its CAS; ~ThreadState RELEASE_ASSERTs both queues empty
  (ThreadManager.h:291-308).
- Keepalive wrap-safety: m_keepaliveReleased is CONSTRUCTED released, so
  never-armed tickets (asyncJoin et al.) lose every decrement CAS
  (ThreadManager.h:160-210).

**Adversarial self-check.**
(a) Cross-ticket leak (the CVE-2025-47907 oracle): impossible by data
layout — each ticket owns its promise Strong and dependency vector; there is
no shared completion buffer two requests could alternate over.
(b) The U-T9-INT1 gate is OPEN in this tree (the four countsKeepalive
call-site edits land with the E2A wiring, ThreadManager.h:184-203): until it
closes, tickets are never armed and ALL late settles take the declared main
fallback — recorded as safe (no hang, no wrap) but it means the open-arm
inbox path has had less soak than the fallback. That asymmetry is an
implementation-fidelity residue on a sound design — exactly what a test
should pin.
(c) The settle task runs on whatever thread services it (main fallback, or
the registrant's E2A loop), but the promise it resolves settles in its own
realm and its reactions run on the SETTLING thread's queue (E.1b rule 1) —
a dead registrant's reactions migrate to main, never to an unrelated
spawned thread's queue (SD2 own-queues-only, SD17 no adoption).

**Verdict: needs-test:**
`JSTests/threads/cve/mc-hand-dead-registrant-settle.js` — registrant
threads asyncJoin two long-running target threads (one resolves, one
rejects), publish their promises, and die; the targets then complete.
Oracle: every promise settles exactly once, on the correct arm, with the
exact result CELL (heap identity) of ITS OWN target — any cross-pair value
bleed, double-settle, or never-settle is an MC-HAND hit. Deterministic
ordering (registrants are joined dead before targets are released).

## S5. Property-waiter wakeup arbitration: notify vs timeout vs sweep vs termination

**Surface.** One PropertyWaiter can be claimed by up to four completers:
Atomics.notify, the finite-timeout expirer, the dead-cell sweep, and
termination — the multi-cancel race that net/http kept getting wrong
(CVE-2020-15586/CVE-2021-36221: two completers, one buffer).

**Governing protocol.** SPEC-api 5.6: waiter state is "flipped exactly once,
under the list lock" (ThreadAtomics.cpp:786); notify dequeues and flips
under LL, then settles OUTSIDE it (5.6-4/5.9); the timeout timer takes LL
and only acts on Waiting (Notified => no-op); the dead-cell sweep flips
under the same LL in the same critical section that dequeues
(ThreadAtomics.cpp:884-920 — "the dequeued <=> flipped invariant stays
intact"). GIL-off, finite timeouts become registrant-local deadlines whose
expirer must win `tryDequeue` under the list lock first — "an
already-notified/dequeued waiter returns false (the in-flight settle wins;
§E.5 harvest rule)" (ThreadManager.h:259-278). Behind all of it, the ticket
m_settled CAS is a second exactly-once belt, and `consumed` (release-fn /
asyncWait arbitration, ThreadManager.h:123-128) makes the lock-hold handoff
single-winner (the loser throws the 4.2 Error; the hold epilogue's
m_holder==current guard, SPEC-api 5.3/G22, prevents the double-unlock).

**Verdict: immune-by-construction** (single-lock arbiter, flip-exactly-once,
dequeue<=>flip invariant, CAS belts). Exercised by the existing
JSTests/threads/atomics + sync corpora (property-waitasync-timeout,
lock-async-hold); a CVE-shaped test would duplicate them.

## S6. Thread.restrict ownership claim: racing restricts (claim-vs-claim)

**Surface.** Two threads race `Thread.restrict(o)` on the same object.
threadFuncRestrict (ThreadObject.cpp:775-835) runs: step (0) affinity check
(:791-799, one m_affinityLock section inside objectAffinity), THEN the
5.7.1 conversion sequence (:801-829, lock-free w.r.t. the affinity table),
THEN restrictObject's table insert (:833 -> ThreadManager.cpp:1011-1030,
a SECOND m_affinityLock section).

**Suspected hole (precise).** The frozen contract is "re-restrict from
another thr => CAE" (SPEC-api 4.1, :51; enforced only by step (0)). GIL-on,
the whole host call is atomic and the contract holds. GIL-off, host calls
run in parallel and the check and the claim are separate lock sections:
1. T_A and T_B both run step (0) and both observe Affinity::None.
2. T_A's restrictObject inserts; T_A is the recorded owner.
3. T_B's restrictObject hits the live-entry arm and returns SILENTLY — the
   comment "(foreign re-restrict was rejected by the caller's affinity step
   (0))" (ThreadManager.cpp:1023) bakes in exactly the atomicity the GIL
   provided and ungil removes.
4. threadFuncRestrict returns o to T_B as success.
T_B now believes o is confined to T_B and stores thread-private data in it;
T_A is the actual owner with full unhindered access — shared ownership
state observed by the wrong logical owner, memory-safe data exposure: the
MC-HAND definition verbatim. Additionally T_B ran the conversion sequence
(structure transitions) on an object T_A already owned — object-model-safe
(M5), but itself a silent I14-semantics violation.

Nothing in SPEC-ungil re-serializes this: ANNEX WS1.2 reshapes
restrictObject's Weak CONSTRUCTION (build outside, publish under the lock)
but explicitly keeps the two-section check/claim shape, and no §E/§A clause
covers restrict-vs-restrict arbitration. The fix is one line of protocol:
the lose arm must compare the live entry's owner against the caller and
surface Foreign (or fold step (0)'s verdict into the same m_affinityLock
section as the ensure) — but as written and as specced, the window is real.

**Verdict: susceptible-suspected** (GIL-off only; phase-1 green). Test:
`JSTests/threads/cve/mc-hand-restrict-claim.js` — two threads race
Thread.restrict on a fresh object per round, then re-probe ownership after
a barrier. Oracle per round: exactly ONE racing restrict succeeds and the
other throws ConcurrentAccessError; the settled table owner must be the
thread whose racing call reported success. Two successes, or
success-reported-but-not-owner, is the hit. Deterministic invariant,
amplifier-ready (the window is two short lock sections apart).

## S7. ArrayBuffer transfer/detach: ownership handoff of the backing store

**Surface.** `transfer()` moves buffer contents to a new owner while racing
readers/Atomics still hold the old {base, length} — the resource-handoff
analog of ERL-90 (old owner observes the transferred resource).

**Governing protocol.** SPEC-ungil annex N6 arm 2: transfer is COPY + source
detach — the handle-move design was explicitly REJECTED (r14 note) for the
transferee-aliasing hole, so old-owner and new-owner bytes are disjoint by
construction; detach publishes length=0 first and quarantines the contents
to a heap §10 stop (ArrayBuffer.cpp:151/:184, writer arms :498/:525), so a
stale old-owner read sees stale-but-mapped SOURCE bytes, never the new
owner's. There is no completion state the wrong owner can observe — the
"completion" (the new buffer) is a fresh allocation.

**Verdict: immune-by-construction**; the racing-reader side is already
hammered by JSTests/threads/cve/mc-prim-arraybuffer-transfer-vs-atomics.js
(P6) and mc-df-ta-detach-resize.js (MC-DF S2) — no third test.

## S8. JIT plan completion vs invalidation; tier-up claim

**Surface.** A compiler thread completes a plan whose compile-time
assumptions a mutator invalidated mid-flight (stale completion installed =
completion observed by an owner whose world moved on); two threads race to
tier up the same CodeBlock (double claim).

**Governing protocol.** Watchpoint sets fire only under STW (SPEC-objectmodel
I13/M6) and installation revalidates set-validity inside the stop, so a
cancelled/invalidated plan's completion is discarded, not installed; the
tier-up trigger is the jit §5.7.2 m_tierUpInFlight CAS — "losers either
[back off]" (UNGIL-HANDOUT:2948-2956) — single-claimant by construction.
This is HotSpot's lesson (the JVM exemplars in jvm.md §6/§7) already
designed in.

**Verdict: immune-by-construction** at the design level; per-tier
verification is owned by the thread-ungil ladder + thread-scanners (no
JS-level deterministic oracle exists below the S2-S6 observables). The
in-language ownership-claim instance — generator/async resume claim — is
already MC-PRIM P5 (mc-prim-generator-resume-claim.js).

---

## Summary table

| # | Surface | Anchor | Governing invariant | Verdict |
|---|---------|--------|---------------------|---------|
| S1 | join: completion vs termination | ThreadObject.cpp:404-523 | F1/F5; AB-17 W1; TERM1.2 (VM-granular cancel) | immune-by-construction |
| S2 | asyncJoin register vs complete | ThreadObject.cpp:525-567, :286-299, :117-146; ThreadManager.cpp:120 | F5 append-vs-swap under joinLock; m_settled CAS; I12/I20; 5.10 | immune-by-construction (S4 test covers end-to-end) |
| S3 | ownership identity vs recycled TID / cell address | ThreadObject.cpp:312; ThreadManager.cpp:963-985, :1023-1052 | Dev 10 (TID retired forever); 5.3/5.7.2 pointer identity; Weak-identity checks | immune-by-construction |
| S4 | ticket settle vs registrant death / inbox close | ThreadManager.cpp:120-198, :742-938; ThreadManager.h:159-278 | SPEC-ungil §E.3/E.4/E.5 (m_settled CAS, inboxLock-arbitrated routing, monotonic close, main fallback) | needs-test → mc-hand-dead-registrant-settle.js |
| S5 | property-waiter notify vs timeout vs sweep | ThreadAtomics.cpp:786, :884-920; ThreadManager.h:259-278 | 5.6 flip-once-under-LL; dequeue<=>flip; §E.5 harvest rule; consumed/G22 | immune-by-construction |
| S6 | Thread.restrict claim vs claim | ThreadObject.cpp:791-833; ThreadManager.cpp:1011-1030 | SPEC-api 4.1 "re-restrict => CAE" — enforced only by a check in a SEPARATE lock section from the claim | **susceptible-suspected** (GIL-off) → mc-hand-restrict-claim.js |
| S7 | ArrayBuffer transfer handoff | ArrayBuffer.cpp:151/:184/:498/:525 | annex N6 arm 2 (copy+detach; handle-move rejected r14) | immune-by-construction (existing P6/S2 tests) |
| S8 | JIT plan completion vs invalidation; tier-up claim | jit §5.6/§5.7.2 sites | I13/M6 fire-under-STW; m_tierUpInFlight CAS | immune-by-construction (ladder-owned) |

## Test manifest (EXECUTED LATER, post-ungil — do not run during bring-up)

- JSTests/threads/cve/mc-hand-restrict-claim.js — `--useJSThreads=1`
- JSTests/threads/cve/mc-hand-dead-registrant-settle.js — `--useJSThreads=1`

Both are deterministic-oracle / nondeterministic-interleaving: trivially
green under the phase-1 GIL, signal-bearing GIL-off, amplifier-ready
(Tools/threads/amplify.sh, TSAN no-JIT target). Both join every thread and
bound every wait (annex T2 conventions). S6's test is the one expected to
FAIL post-ungil until the restrict lose-arm re-checks ownership; its
failure message names the exact frozen clause (SPEC-api 4.1) it asserts.
