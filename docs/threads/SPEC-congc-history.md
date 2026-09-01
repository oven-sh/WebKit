# SPEC-congc-history.md

Companion to `SPEC-congc.md` (draft rev 10). Per the frozen-spec
convention: this file is NON-NORMATIVE EXCEPT the sections marked
ANNEX ... (BINDING), which are part of the binding spec text and
exist here only for the body size cap (50000 bytes).

## Rev log

- rev 1 (2026-06-07): initial draft per the thread-specs2 charter.
  Grounding pass: SPEC-heap rev 13 (Deviation 4, §10/§10A/§10B,
  I1-I17), SPEC-ungil rev 32 (§A.3, §B, EXIT1/HBT4/ISB1/SB1
  annexes), and the live tree (Heap.cpp 5913 lines incl. the landed
  §10 implementation and the fix-shared-heap-corruption
  diagnostics). Not yet adversarially reviewed; freeze pending the
  review loop + the whole-design cross-check.

- rev 2 (2026-06-07): adversarial round 1 — 10 reviewer findings
  received, 9 distinct (the two mid-cycle-attach findings are one
  defect), ALL ACCEPTED as real after verification against the
  tree; none refuted. Dispositions (F-numbers used by the rev 2
  body cites):
  - F1 (blocker, two findings merged): §4.2 cited a nonexistent
    §9.3; mid-cycle ATTACH had no fence-copy/FEP init — a client
    attaching between windows of a live cycle ran with the
    attach-default unfenced copy (every store skips the remembered
    set; live objects collected) and broke the CG-I3 assert.
    VERIFIED: rev 1 §9 had only 9.1/9.2; `HeapClientSet::add`
    blocks only in-window (`HeapClientSet.h:54-68`), so
    between-window attach is reachable. FIX: new §9.3 (GBL-section
    fence/threshold+FEP snapshot before HCS publish; assist-visitor
    registration deferral; CG-I14 amendment; CG-I20; CG-T9 attach
    storm arm). The §10B.4-quiescence alternative was rejected for
    spawn-latency/starvation (recorded in §9.3).
  - F2 (major): §9.2(1) flushed the CMS BEFORE the landed teardown;
    `lastChanceToFinalize` runs with access re-acquired and can
    execute write barriers (`Heap::addToRememberedSet`,
    `Heap.cpp:1427`), whose appends post-flush died with the
    client. Lost-object interleaving (rev 1 full text + reviewer
    interleaving): cycle Concurrent between windows; exit; CMS
    flushed empty; finalizer stores white W into black surviving B;
    barrier appends B to the flushed CMS; HCS remove before next
    WND-open; CMS destroyed; B never re-scanned; W freed live.
    CG-I17's "empty when leaving HCS" assert contradicted the
    rev 1 normative order — proof the order was wrong. FIX:
    teardown -> permanent access drop -> final flush -> epoch=MAX
    -> HCS remove; early flush optional; CG-T9 arm.
  - F3 (major): ANNEX CGN1 N3's progress sentence ("in-window
    convergence guarantees progress since holders are then
    parked") was inverted — a holder parked WHILE holding 10a is
    the livelock, not the proof. FIX: CG-I18 (cell-lock no-park)
    in §8.2 with grounding (SINFAC I6 precondition
    `Heap.cpp:5125-5127`; F8 parking only in access acquisition
    `Heap.cpp:5707-5758`); N3 row amended below; CG-A2 row schema
    now classifies mutator sides against CG-I18; debug asserts +
    CG-T5 arm.
  - F4+F5 (major+blocker, one fix): conductor lifecycle between
    windows was unspecified — no access/poll contract, no GCA
    owner identity, no resume predicate, no wake-up path once the
    legacy conn/parking machinery (`stopIfNecessarySlow`
    `Heap.cpp:2421`, `waitForCollector` `:2497`) is kept dead
    (CG-I7); the §3.4 `:4590` guard as written would also have
    blocked the conductor's own poll, and a conductor blocked in
    native code (or exiting) stalled the cycle with GCA set
    forever. VERIFIED: GCA is a bare bool (`Heap.h:1290`); rev 1
    §7.1 "served by SINFAC polls" named machinery that does not
    exist. FIX: §3.5 `m_gcConductorThread` identity; §3.7 closed
    loop (conductor access-released all tenure; between-window
    waits = the landed MainDrain shape `SlotVisitor.cpp:623-636`;
    wake-ups = helper termination notifyAll `:629, :645` +
    scheduler timeout; no JS/RHA/EXIT1 mid-cycle, release-assert);
    §7.1 rewritten (conductor runs the Collector arm
    `Heap.cpp:1997-2002` under ISS; Mutator arm `!ISS`-only);
    CG-I19; CG-T8/T9 arms. The rev 1 SINFAC-conn sentence is
    superseded INTERNALLY (pre-freeze rev supersession, this
    entry; both-sides discipline N/A — nothing frozen cited it).
  - F6 (major): §9.1(2) cited `m_parallelMarkersShouldExit`-class
    signaling AND the `:2315-2342` rightToRun loop — non-equivalent
    mechanisms (Exit is one-shot cycle-terminate set in runEndPhase
    `Heap.cpp:2027`, consumed `SlotVisitor.cpp:664, :673`).
    FIX: normative new pause pair (`m_parallelMarkersShouldPause` +
    `m_pausedParallelMarkers` under `m_markingMutex`), checkpoints
    at the helper-wait isReady lambda (`SlotVisitor.cpp:661-667`)
    and the per-batch drain safepoints (`:522, :578`), ack count,
    lost-wakeup argument, per-batch pause granularity feeding the
    CG-I12 bound. Resolves old open item 3.
  - F7 (major): §7.2's client-less conductor reused a conduct body
    written against `GCClient::Heap&` with no refactor table.
    FIX: ANNEX CGA2 below (BINDING); §7.2 points at it; old open
    item 1 (VMTraps poll) RESOLVED as CGA2 row R6.
  - F8 (major): §5.2(ii) gated donation on
    `Options::minimumNumberOfCellsToKeep` — VERIFIED nonexistent
    (grep of Source/JavaScriptCore; nearest is
    `minimumNumberOfScansBetweenRebalance`, OptionsList.h:402,
    different semantics) — and named no trigger site. FIX: new
    option `sharedGCMutatorMarkStackDonationThreshold` (§13.2;
    default `s_segmentCapacity`, `GCSegmentedArray.h:62, :116`);
    trigger site pinned to the SINFAC hot-poll tail only
    (rank-7-9b-free by SINFAC's I6 precondition), never inside
    `addToRememberedSet`.
  - F9 (blocker): rev 1 WND-open order (GCL -> GSP -> release
    access) deadlocked vs an interleaved `JSThreadsStopScope`:
    the conductor re-acquired access at each WND-close, then
    block-acquired GCL holding access; the §A.3 fan-out (ungil
    §A.3 rule 2) waits for that access forever; mutual stall to
    the 30s watchdog (`JSThreadsSafepoint.cpp:379, :403`).
    VERIFIED new-path: the landed election/poll only ever tryLock
    GCL (`Heap.cpp:4523, :4585, :5036`), and HBT4 pins
    release-access-before-GCL for all §A.3 conductors. FIX: §3.1
    order = access-released -> GCL -> GSP; non-final WND-closes do
    not re-acquire conductor access (§3.2); HBT4 order extended to
    GC-conductor re-entry (§9.1(3)); CG-T8 arm at the GCL acquire.
  Size cap: body compressed to 49995 bytes (<= 50000); full
  rejection rationales live in this entry per the overflow rule.

- rev 3 (2026-06-07): adversarial round 2 — 10 reviewer findings
  received (the 10th truncated in transmission; its surviving text
  identified the same defect as F10's flag-off half and is folded
  there), 7 distinct defects after merging duplicates
  (findings 1+5+10 = F10; findings 2+6+8 = F13; the rest 1:1).
  ALL ACCEPTED as real after verification against the tree; none
  refuted. Dispositions (F-numbers cited by the rev 3 body):
  - F10 (blocker; 3 findings merged): the §3.4 second-cycle claim
    guarded only `tryConductSharedCollectionForPoll` and cited
    `:4550-4554` — VERIFIED to be the tryLock-FAILURE follower
    arm; the election WINNER arm (`Heap.cpp:4523-4532`) checks
    only `m_lastServedTicket >= ticket` under `*m_threadLock`,
    sets GCA, and conducts unconditionally. Under the window model
    "GCL free && GCA set && unserved tickets" is a steady state
    (today only the wind-down instants `:4534-4537`, where phase
    is already NotRunning), so a between-windows sync requester
    wins tryLock and nests a second `conductSharedCollection`
    (interleaving: ANNEX CGD1.1). Also accepted (the truncated
    finding's half): an UNCONDITIONAL "GCA set => back off" guard
    would change flag-off behavior, because GCA-true-with-GCL-free
    IS reachable flag-off in wind-down — rev 2's "impossible ...
    backs off" sentence was doubly wrong. FIX: §3.4 rewritten as a
    GCL-tryLock SITE ENUMERATION (`:4523`, `:4585`, `:5036`) with
    the PHASE-GATED predicate `GCA && m_currentPhase != NotRunning`
    (unreachable flag-off => CG-I0 holds byte-for-byte); CG-I21;
    CG-T8 requester-storm arm.
  - F11 (blocker): `pollIssRevertIfNeeded` (`Heap.cpp:5010-5105`)
    tryLocks GCL (`:5036`) then loops
    `while (m_gcConductorActive) waitFor(1_ms)` (`:5040-5043`)
    HOLDING GCL and heap access — sound today (GCA-true/GCL-free
    lasts instants), a process-wide deadlock against the
    between-windows steady state (ANNEX CGD1.2). The rev 2 §9.2(3)
    text addressed the revert OUTCOME (phase must be NotRunning)
    but not this landed pre-check's structure. FIX: §3.4 `:5036`
    row + §9.2(3) cross-ref — never wait for GCA under GCL when
    phase != NotRunning; return with the hint armed; bounded
    wind-down wait only when NotRunning. CG-T9 arm (revert-pending
    poll storm mid-cycle).
  - F13 (blocker; 3 findings merged): rev 2 §3.7/§7.1/CG-I19
    pinned the conductor's between-window wait to
    `drainInParallelPassively`'s "MainDrain wait"
    (`SlotVisitor.cpp:623-636`) and §9.1(2) exempted it from pause
    checks. VERIFIED wrong on every arm: (a) the
    `drainInParallelPassively` guard (`:718-731`) falls back to
    ACTIVE `drainInParallel()` (donateAndDrain + counted
    drainFromShared(MainDrain)) on `numberOfGCMarkers()==1`,
    `mutatorWaitingBit` (dead under ISS), `!hasHeapAccess()` —
    which under ISS forwards to mainClientHasHeapAccess()
    (`Heap.h:405-412`), i.e. an UNRELATED thread's state when the
    conductor is access-released or not the main client — or
    `worldIsStopped()`; (b) even the MainDrain wait actively
    steals and drains on hasWork (`:680-688`, `:705`) and is
    counted in active/waiting; (c) the combination with the
    §9.1(2) exemption yields, per branch, either a pause-predicate
    that can never close (watchdog stall at
    `JSThreadsSafepoint.cpp:401`) or a conductor draining cells
    concurrently with a §A.3 window's jettison (ANNEX CGD1.3);
    (d) numberOfGCMarkers()==1 was unruled. FIX: §3.7/§7.1
    rewritten — ISS conductor wait = `donateAll()` +
    `waitForTermination` (`SlotVisitor.cpp:753-758`, `:737-751`),
    no counters, never visitChildren, never
    drainInParallelPassively/drainFromShared; ==1 markers =>
    Concurrent never scheduled (one-window degenerate); CG-I19
    amended; CG-T8 arm kept.
  - F14 (major): §9.1(2) never defined WHICH thread classes the
    pause denominator counts; `performIncrementOfDraining`
    (`SlotVisitor.cpp:527-585`) maintains NO counters, so giving
    the C4 assist path the `:578` checkpoint (as rev 2 did) makes
    a pausing assist mutator overshoot the ack predicate AND park
    holding access where the §A.3 fan-out cannot reach it (it
    passes no stop poll while waiting on
    m_markingConditionVariable) — watchdog stall; omitting it
    races the §A.3 window for up to one increment, which is the
    bounded, fan-out-compatible choice. FIX: participant set
    pinned to drainFromShared(HelperDrain) helpers only (§9.1(2));
    new §9.1(6): assist takes NO checkpoint, bound = one
    increment, parked by the mutator fan-out at its next poll;
    §7.4 cross-ref; CG-T10 arm.
  - F15 (major): §3.1's (a)access-released -> (b)GCL -> (c)GSP
    order, read literally at the FIRST window, double-acquires the
    non-recursive GCL (election tryLocked it while access-held,
    `:4523`) and inverts the landed GSP-before-release order
    (`:4768-4771`) — contradicting CG-I0 (flag-off = one window =
    the first one, byte-for-byte). FIX: first-window carve-out in
    §3.1 (the first WND-open IS the landed entry; tryLock while
    access-held is §A.3-safe because non-blocking); (a)-(c) and
    the blocking acquire pinned to RE-ENTRY; §9.1(3) and CG-I19
    re-phrased to match; the F9 rejection re-grounded as
    re-entry-only.
  - F16 (major): §9.1(2)'s rev 2 ack predicate
    ("paused == active + waiting") composed with
    `didReachTermination` (`SlotVisitor.cpp:594-598`) was
    underdetermined: decrementing active on pause allows
    termination to fire with undonated work parked in paused
    helpers' local stacks (lost marks); not decrementing makes the
    equality unsatisfiable (pauser deadlock). FIX: normative
    counter protocol — pause checkpoint does donateAll (paused
    helpers hold NO local work) + leaves its counter
    (waiting--/active-- as appropriate) + paused++; ShouldPause
    gates counter re-entry; pauser predicate = active==0 &&
    waiting==0 under m_markingMutex (exited helpers count in
    neither — SUPERSEDED rev 4, F17: this was asserted as a landed
    property but is FALSE against the tree; it is now a normative
    EDIT, the §9.1(2)(c) exit delta); didReachTermination
    additionally gated on paused==0 (CG-I22); CG-T8 mid-batch
    sub-arm.
  - F12 (labeling): rev 2's grounding sentence for §3.4 cited the
    follower arm as if it guarded winners — corrected as part of
    the F10 rewrite (kept as a separate number because two
    findings called out the citation error independently of the
    missing guard).
  Body size: compressed to 49989 bytes (<= 50000); the full
  interleavings/derivations moved to ANNEX CGD1 per the overflow
  rule; §2.1's consumer list deduplicated against ANNEX CGA1
  (the annex table is the audit of record).

- rev 4 (2026-06-07): adversarial round 3 — 6 reviewer findings
  received, 3 distinct defects after merging duplicates
  (findings 1+3+5 = F17, two blockers + one blocker restatement;
  findings 2+4+6 = F18, three majors stating the same
  charter/miscite defect; the §5.3 finding = F19, 1:1). ALL
  ACCEPTED as real after verification against the tree; none
  refuted. Dispositions (F-numbers cited by the rev 4 body):
  - F17 (blocker; 3 findings merged): the rev 3 §9.1(2)/F16 pauser
    predicate `active==0 && waiting==0` was grounded on the
    sentence "helpers that exit (TimedOut/Done) leave both
    counters" — VERIFIED FALSE: `drainFromShared` increments
    `m_numberOfWaitingParallelMarkers` unconditionally at the top
    of every loop iteration (SlotVisitor.cpp:621) and decrements
    only on the resume-to-active path (:688); ALL FOUR return
    paths exit with the increment leaked — MainDrain TimedOut
    (:626), MainDrain Done (:629-630), HelperDrain TimedOut
    (:641-642), HelperDrain Done on m_parallelMarkersShouldExit
    (:673-674). Grep-complete: the only writers in the tree are
    :621/:688 plus the zero-init (Heap.h:1261); the only readers
    are the steal denominator (:682) and a diagnostic log
    (Heap.cpp:1867); termination uses ACTIVE only
    (didReachTermination, SlotVisitor.cpp:594-598) — which is why
    the leak is benign tip-of-tree and why the spec's predicate
    turned it into a guaranteed liveness failure: every cycle's
    runEndPhase sets m_parallelMarkersShouldExit
    (Heap.cpp:2026-2031), each helper Done-returns leaking +1, so
    from the second cycle onward every §A.3 pause wait wedges
    forever. Worse: in the GIL-off conductor the stop scope is
    constructed (VMManager.cpp:561) BEFORE `requestStart` is
    sampled (:579), so the wedge is not even watchdog-covered —
    silent process hang with GCL held. FIX (body §9.1(2)(c) EXIT
    DELTA): normative waiting-- on all four return paths (they
    hold m_markingMutex); the flag-off delta (steal-denominator
    heuristic only) is benign-ruled under CG-I0 with the full
    derivation in ANNEX CGD2.1; debug assert
    active==waiting==paused==0 after `m_helperClient.finish()`;
    the gate clause extended to a fresh helper's FIRST :621
    increment (transient — checkpoint (a) moves it to paused under
    the same mutex, so the pauser predicate re-closes); CG-I22
    re-worded ("a property CREATED by the F17 exit delta, not
    landed"); CG-T8 gains the second-cycle sub-arm (one completed
    cycle BEFORE the injected stop — the only way the leak is
    test-reachable).
  - F18 (major; 3 findings merged): §13.3(b) chartered the
    marker-pause call site OUT to "`JSThreadsSafepoint.cpp` ...
    the scope ctor lives there (`:334-337`)" — VERIFIED WRONG both
    halves: the ctor/dtor live at Heap.cpp:5456-5482 (heap-owned
    per §13.1); bytecode/JSThreadsSafepoint.cpp:334-338 is merely
    one USE site (the `std::optional` declaration + `.emplace`).
    The other construction sites — runtime/VMManager.cpp:561 (the
    GIL-off §A.3 thread-granular conductor, the path every GIL-off
    jettison/haveABadTime takes once JSThreadsSafepoint.cpp:239-241
    reroutes gilOff requests) and SharedHeapTestHarness.cpp:1039/
    :1073/:1107 — were never mentioned. Read literally, the
    §13.3(b) row places the pause call at the stub use site and
    leaves the VMManager conductor pausing NOTHING: a §A.3 window
    jettisoning while HelperDrain markers are mid-visitChildren,
    the exact CGD1.3 UAF class rev 3 closed. The §14 freeze gate
    ("INTEGRATE-congc.md matches §13 exactly") would have forced a
    fabricated integration row for a call site that must not
    exist. FIX: §9.1(2) now pins CALL SITES = the ctor/dtor ONLY
    (covering every construction by construction; dtor order made
    normative: resume markers BEFORE releasing GCL so a WND-open
    cannot interleave with paused markers); §13.3(b) DELETED with
    the miscite recorded; §13.3(c) states VMManager.cpp:561 is
    covered with ZERO VMManager edits; the §9.1 intro cite gains
    the `bytecode/` path; §14 CG-3 re-worded (no foreign
    integration row); CG-T8 gains the GIL-off VMManager-conductor
    jettison sub-arm.
  - F19 (blocker): §5.3(3)'s GIL-off fail-safe pinned the CLIENT
    copies always-fenced — but NO emitted code reads the copies:
    Baseline bakes the SERVER `addressOf*` as AbsoluteAddress
    (AssemblyHelpers.h:2045, :2052, :2116; Heap.h:723/:726 return
    &m_mutatorShouldBeFenced/&m_barrierThreshold, the server
    members; branchIfBarriered reads
    VM::offsetOfHeapBarrierThreshold — also server), and DFG/FTL
    load VM_heap_barrierThreshold / VM_heap_mutatorShouldBeFenced
    off the VM (FTLLowerDFGToB3.cpp:27281, :27323, :27355). With
    §5.3(1) dropping the setMutatorShouldBeFenced ISS forcing
    (Heap.cpp:3928-3940 — its own banner says the fence "must hold
    at all times" with N mutators) and the §13.3(a) reroute
    unlanded, GIL-off C1 mutators would run JIT code with
    mutatorShouldBeFenced=false and blackThreshold between cycles
    after the first endMarking lower — eliding mandatory
    store-store fences and skipping barrier slow paths (lost
    remembered-set appends / unfenced butterfly publication). The
    fail-safe failed exactly in the case it existed for. NOT the
    recorded open item 2 (which weighs the COST of a pin vs
    blocking C1 GIL-off): the defect is that the pin attached to
    storage the baked addresses do not read. FIX: §5.3(1)'s
    forcing-drop is additionally gated on NOT GIL-off — GIL-off
    keeps the landed forcing, so the SERVER MASTER (what emitted
    code reads) stays tautological and the copies snapshot
    tautological from it; FEP stays at the raise (CG-I3
    unaffected); §13.3(a) row re-worded; CG-T3 gains the GIL-off
    two-cycle fence-storm sub-arm asserting the server pair;
    derivation: ANNEX CGD2.2. Open item 2 (pin cost) remains open
    and now reads against the server pin.
  Body size: compressed to 49959 bytes (<= 50000); compressions
  moved rationale prose to existing annex pointers (CGD1.1-1.3,
  this entry, ANNEX CGD2); no normative clause was weakened —
  every trimmed sentence survives here or in an annex.

- rev 5 (2026-06-07): adversarial round 4 — 8 reviewer findings
  received, 8 distinct, ALL ACCEPTED as real after verification
  against the tree; none refuted. Dispositions (F-numbers cited by
  the rev 5 body):
  - F20 (blocker): the F10 phase-gated guard left one state
    unhandled — the PREDECESSOR conductor's deferred GCA clear.
    VERIFIED: both landed conduct wrappers release GCL BEFORE
    clearing GCA under `*m_threadLock` (runSharedGCElection:
    unlock `Heap.cpp:4533`, clear `:4536`;
    tryConductSharedCollectionForPoll: unlock `:4600`, clear
    `:4603`). Flags-on interleaving (ANNEX CGD3.1): cycle-1
    conductor T1 finishes the final WND-close (phase NotRunning),
    unlocks GCL, is descheduled before its clear; requester T2
    tryLocks `:4523` — the guard predicate is FALSE (phase
    NotRunning, the legitimate pass-through), T2 re-sets GCA,
    restamps `m_gcConductorThread`, conducts cycle 2; cycle 2
    goes between-windows (GCL free, phase Concurrent); T1 resumes
    and clears GCA + the thread MID-CYCLE-2 — every §3.4 guard
    then evaluates false (GCA false, phase Concurrent), so a
    third requester nests a conductor (the exact CGD1.1
    catastrophe F10 closed); cycle-2 followers' `:4550-4554`
    wait predicate wakes spuriously; the §9.2(4)/CG-I21
    discriminators read a nulled owner. Neither §3.4/§3.5 nor
    CGD1.1 considered the predecessor's clear racing a successor
    (CGD1.1's flag-off half analyzed only the new winner
    conducting during wind-down). FIX (body §3.4 WIND-DOWN
    CLEAR): the clear becomes OWNERSHIP-CHECKED — under
    `*m_threadLock`, clear GCA + `m_gcConductorThread` only if
    `m_gcConductorThread == &Thread::current()`; notifyAll
    unconditional; restamp over a non-null owner legal only when
    phase == NotRunning (debug assert). The alternative
    (clearing inside the final WND-close before the GCL release)
    was NOT taken: it removes the flag-off-reachable
    GCA-true/GCL-free wind-down state entirely, a larger CG-I0
    delta than the ownership check (whose flag-off delta —
    followers keep the untimed GCA wait across a takeover — is
    benign-ruled in CGD3.1). §3.5 amended; CG-I21 extended
    (owner-only clear, both identities debug-asserted); CG-T8
    F20 sub-arm (amplifier-descheduled conduct return +
    second-cycle between-windows requester storm).
  - F21 (major): CG-I10's "neither taken with ranks 7-9b held"
    contradicted §5.2's own normative premise that
    `addToRememberedSet` callers may hold rank 7-9b allocation
    locks while taking the CMS lock — the invariant was
    unsatisfiable on the design's hottest path, and CG-T2's lint
    would fail every barrier append under an allocation lock (or
    be silently weakened). The design is sound only under the
    leaf reading: the CMS lock is TERMINAL (the append does
    setCellState + append only — nothing acquired under it), so
    7-9b -> CMS creates no cycle; the dangerous edge is
    exclusively m_markingMutex-with-7-9b, already prevented by
    the F8 donation-site restriction. Rev-2 regression: F8 pinned
    the donation site with the 7-9b rationale and CG-I10's
    wording was never reconciled with the append side. FIX:
    CG-I10 restated as three separable clauses (m_markingMutex
    never under 7-9b; CMS terminal leaf, MAY be under 7-9b;
    m_markingMutex > CMS only at the 7-9b-free drain/donation
    sites); §5.2 parenthetical and the §10 lock-table row
    re-worded; CG-T2 charter encodes the three clauses.
  - F22 (major): under the window split, `finishChangingPhase`'s
    phase store (`m_currentPhase = m_nextPhase`, Heap.cpp:2213)
    executes AFTER resumeThePeriphery (`:2187`) — i.e. after the
    WND-close released GCL per §3.2 as written — making the store
    race every load-bearing reader the spec itself created: the
    §3.4 guards (read under `*m_threadLock` post-tryLock; the
    writer holds neither lock at the store), the §9.1(2)
    stop-scope ctor check, and CG-I4's "phase transitions mutate
    in-window only". The value race is benign mid-cycle (both
    values != NotRunning) but is an unsuppressed TSAN report —
    a guaranteed §12 C1 ladder blocker — and the §3.4 flag-off
    soundness sentence silently assumed store-before-release.
    Flag-off the race does not exist (callers hold GCL across
    the store); the window split creates it, so ruling it is this
    spec's obligation. FIX (body §3.2 PHASE-STORE ORDER): all
    four phase-field updates complete BEFORE the close's GCL
    release, under any §13.2 flag; CG-I4 cross-cites; CG-T8 F22
    sub-arm (stop-scope ctor injected just after a non-final
    WND-close).
  - F23 (major): §3.2's unconditional "THEN release GCL" had no
    final-window carve-out symmetric to F15's entry carve-out:
    GCL release at cycle boundaries lives in the CALLERS
    (`Heap.cpp:4533`, `:4600` — verified), so a literal final
    WND-close release double-unlocks a WTF::Lock not held, and
    CG-I0 REQUIRES the caller unlocks to remain (flag-off CG-1
    builds the close helper). FIX (body §3.2 FINAL-CLOSE
    CARVE-OUT): only closes preceding another window of the same
    cycle release GCL; the final close (the ->NotRunning edge,
    which the `:4852-4863` m_requests-empty exit postdates)
    leaves GCL held for the landed caller (or the CGA2 R7
    collector loop at C2). CG-I12's wording ("never held BETWEEN
    windows") already matched the intent and stands.
  - F24 (major): ANNEX CGA2 R7 specified the C2 collector-loop
    election-equivalent (tryLock GCL, set GCA + thread, conduct)
    with NO §3.4 guard, while §3.4/CG-I21 enumerated a CLOSED
    three-site list — an implementer satisfying both texts ships
    the CGD1.1 nesting at C2 (mid-cycle RCAC tickets are granted
    and unserved until cycle end, so a between-windows collector
    wake wins tryLock against the steady state). FIX: R7 row
    amended below (guard + re-wait); §3.4 and CG-I21 re-worded
    from the closed list to "landed AND stage-added sites";
    CG-T6 R7-guard arm.
  - F25 (major): the C4 assist-visitor lifecycle was unsatisfiable
    for a mid-cycle EXIT1: §9.2 permits between-windows exit and
    §9.2(2) rules HCS removal legal, but §9.2(1)'s teardown order
    had no visitor step; unregistering at DCT violates CG-I14
    (forEachSlotVisitor mutation in-window/between-cycles only),
    destroying without unregistering violates CG-I17 and leaves
    the `:2315-2342` in-window walks dereferencing a dangling
    visitor (UAF); blocking DCT until the next window was
    specified nowhere. The attach side had the needed mechanism
    (§9.3(3) deferral); the exit side had no analog — and
    §9.3(3) itself had no cancellation rule, so
    attach-then-exit in one gap left the conductor registering a
    visitor for a destroyed GCH. FIX (body §9.2(1) C4 DELTA,
    derivation CGD3.3): heap-allocated visitor; deferred
    UNREGISTRATION with ownership transfer to the pending list;
    cancellation of a pending deferred registration; conductor
    applies pending (un)registrations at each WND-open before
    any walk; CG-I14/CG-I17/§7.4 amended; CG-T9
    attach-then-exit-in-one-gap arm.
  - F26 (major): the C3 sweeper was invisible to the §A.3 stop
    protocol: the fan-out counts ENTERED threads (ungil §A.3
    rule 1 registry walk — a standalone client has no lite), and
    the §9.1(2) pause participant set was pinned (F14) to
    HelperDrain helpers — so a §A.3 window (jettison,
    haveABadTime heap iteration) ran concurrently with active
    sweeping, invalidating the frozen SPEC-heap §10A exemption's
    "world stopped" premise (jit R1.i: conductor may write heap
    without access). CG-I13's "no sweeper-vs-window special
    case" covers GC windows only (the sweeper parks via F8/GSP;
    §A.3 stops set no client-visible state per §9.1(5)). FIX
    (body §9.1(7), derivation CGD3.2): per-quantum sweeper ack
    extending the §9.1(2) ctor pause (option (a) of the
    review's two; option (b) — GCL-subordinate quantum
    bracketing — rejected as a new lock-shape on the sweep hot
    path); flag-gated supersession of the heap §10A premise
    recorded as CGS1 row 11; CG-I13 extended; CG-T7 F26 arm.
  - F27 (major, recording defect): ANNEX CGS1 omitted the
    flag-gated supersessions of SPEC-heap's whole-conduct GCL
    tenure — the §10.2 election pseudocode holds GCL across
    conduct (SPEC-heap.md:254-257) and step 9 says "Conductor
    releases GCL" (SPEC-heap.md:274) — and of the §10D step (1)
    pre-check structure (SPEC-heap.md:296: timed loop until
    "...∧ GCL tryLock succeeds ∧ ..."), both contradicted under
    `useConcurrentSharedGCMarking` by §3.1/§3.2/§3.4 and the
    `:5036` row/CGD1.2. CGS1 row 5 covered only the step-7
    banner — a different clause. FIX: CGS1 rows 9-10 added
    below (recorded two-sided at freeze per §13.4); no body
    change beyond the rows' existence (the §13.4 pointer already
    says "list = ANNEX CGS1").
  - Size cap: body compressed to 49983 bytes (<= 50000). Per the
    overflow rule, the §9.1(2) MECHANISM/COUNTER PROTOCOL full
    text was relocated VERBATIM to ANNEX CGP1 (BINDING) below —
    no clause weakened or changed; the body keeps a normative
    summary. Other trimmed sentences survive in this rev log
    (rev 2 F1/F6/F8, rev 3 F13, rev 4 F17-F19 entries) or the
    CGD annexes; the §8.2 CG-I15 termination argument's full
    text lives in ANNEX CGN1 N3 (where it already was, verbatim).

- rev 6 (2026-06-07): adversarial round 5 — 7 reviewer findings
  received, 7 distinct, ALL ACCEPTED as real after verification
  against the tree; none refuted. NOTE: the rev 6 body trims every
  inline "(rev N, F#)" attribution to "(F#)" for the size cap —
  the F-number -> rev mapping is THIS log (F1-F9 rev 2, F10-F16
  rev 3, F17-F19 rev 4, F20-F27 rev 5, F28-F34 rev 6).
  Dispositions:
  - F28 (blocker): back-to-back ticket-drain cycles inside one
    conduct call had NO legal WND-open arm for the successor
    cycle. VERIFIED: the landed step-7 loop
    (`Heap.cpp:4852-4863`-region: `for(;;){ if (m_requests
    .isEmpty()) break; ... collectInMutatorThread(); }`) runs
    whole cycles back-to-back; under C1, F23 makes each cycle's
    final close (->NotRunning) leave GCL HELD and §3.2 places the
    m_requests exit AFTER it — so a non-empty re-check starts
    cycle 2 with GCL held, the conductor access-released, no
    election entry. The F15 FIRST-WINDOW arm is inapplicable
    (requires the landed access-held tryLock entry); the RE-ENTRY
    arm self-deadlocks (blocking acquire of a non-recursive
    WTF::Lock already held). The stop sequence (GSP/
    requestStopAll/GBL barrier, `:4768-4793`) also executes once
    per conduct call above the loop, so the successor had no
    specified re-stop. Mid-cycle tickets (allocation triggers,
    §9.1(4) RCAC) make the multi-cycle conduct COMMON under C1.
    The alternative reading (intermediate cycle ends release GCL)
    was checked and rejected: it contradicts F23's own
    ->NotRunning definition and creates an unruled steady state
    {GCL free, GCA set, NotRunning, unserved tickets} in which
    the §3.4 guard's NotRunning pass-through admits a foreign
    election winner CONCURRENT with the original conductor's
    loop re-entry — two threads in the conduct machinery (the
    CGD1.1 class, invisible to F10/F20 because phase is
    NotRunning). FIX (body §3.1 TICKET-DRAIN SUCCESSOR arm; full
    text ANNEX CGD4.1): GCL ownership TRANSFERS from the
    predecessor's final close to the successor's first WND-open,
    which runs steps (c)-(e) only; §3.2 F23 re-worded
    (caller-release OR transfer); §3.4 gains the inter-cycle
    state ruling (foreign tryLocks FAIL — no guard needed);
    §9.1(1)/(4) re-worded; CG-I12's §A.3 wait bound restated
    (+re-stop +successor first window); CG-T8 F28 sub-arm.
  - F29 (blocker): the F26 sweeper ack was attached to the
    §9.1(2) ctor pause, which is gated on `m_currentPhase !=
    NotRunning` — but C3 incremental sweeping is a BETWEEN-CYCLES
    activity (phase == NotRunning): `notifyIncrementalSweeper()`
    fires in the end phase (`Heap.cpp:2083`),
    `startSweeping`/`scheduleTimer` (`IncrementalSweeper.cpp:152`,
    `:41`) then run timer-sliced quanta while mutators run.
    VERIFIED against the tree (end-phase call site; timer
    re-arming in `doWork`). Consequence as specified: every §A.3
    stop between cycles — the dominant case — skipped the pause
    entirely; the sweeper flag was never set; the §A.3 window's
    heap writes (jit-R1.i exemption) raced live sweep quanta —
    exactly the CGD3.2 hazard F26 was accepted to close, still
    open everywhere except the narrow mid-cycle case. The phase
    gate is sound for the MARKER pause (helpers exist only while
    a cycle is live) but anti-correlated with sweeper activity.
    FIX (body §9.1(2)/(7); ANNEX CGP1 SWEEPER EXTENSION rewritten
    below): the sweeper flag/ack is keyed on
    `useSharedGCIncrementalSweep` && sweeper-client-registered,
    with NO phase gate; the marker pause keeps its gate; CGS1
    row 11 amended; CG-T7's F26 arm re-pinned to phase ==
    NotRunning (the only config in which the defect was
    test-reachable).
  - F30 (blocker): two further holes in the same CGP1 sweeper
    protocol. (1) IDLE-SWEEPER LIVENESS: the ctor's wait
    predicate was the ack bit unconditionally whenever a sweeper
    client is registered — but an idle sweeper (timer-parked, no
    current quantum; the majority state) executes no quantum
    boundary, never polls the flag, never acks: the ctor wedges
    to the 30s watchdog (`JSThreadsSafepoint.cpp:401`), and in
    the GIL-off conductor the scope is constructed at
    `VMManager.cpp:561` BEFORE `requestStart` is sampled (`:580`
    — re-verified; the CGD2.1 shape), so the wedge is a SILENT
    hang with GCL held. The stated bound ("one quantum") silently
    assumed an in-quantum sweeper. (2) QUANTUM-ENTRY RACE: the
    rev 5 text governed only a CURRENT quantum ("on set it
    finishes/aborts the quantum"); nothing forbade STARTING a new
    quantum after the flag was set (the sweeper is invisible to
    the §A.3 fan-out — F26's own premise — and §A.3 stops set no
    client-visible heap state), so a timer wake mid-§A.3-window
    could acquire access/MSPL and mutate
    freelists/newlyAllocated/directory bits concurrently with the
    window — CGD3.2 re-opened through the entry edge. There was
    no entry-side analog of the helper protocol's "ShouldPause
    gates counter (re-)entry" clause. FIX (ANNEX CGP1 rewritten
    below; body §9.1(7) summary): two-bit state machine under
    `m_markingMutex` — `m_sweeperInQuantum` set at quantum ENTRY
    (entry REFUSED while the pause flag is set; access/MSPL only
    after a gated entry), cleared at exit; ctor predicate
    `!m_sweeperInQuantum || acked` (idle sweeper passes with no
    ack); ack lifetime = cleared by the dtor's resume with the
    flag, before the GCL release; bound = one quantum in-quantum,
    zero otherwise; CG-T7 gains the idle-sweeper and
    delayed-timer-entry shapes.
  - F31 (major): in-window conductor-executed write barriers had
    no CMS/fence disposition. VERIFIED: runEndPhase runs
    `iterateExecutingAndCompilingCodeBlocks -> writeBarrier
    (codeBlock)` (`Heap.cpp:2036-2039`) and
    `m_codeBlocks->iterateCurrentlyExecuting -> writeBarrier`
    (`:2085-2088`), reaching `addToRememberedSet` (`:1427`),
    which §5.2 routes via `currentThreadClient()` and whose
    `m_mutatorShouldBeFenced` read (`:1434`) §5.3(3) re-points to
    the current client's copy — undefined (null deref as written)
    for the §7.2 C2 standalone conductor (explicitly NOT a
    client), and unstated for the C0/C1 client-conductor (its
    in-window CMS appends postdate the WND-open drain). Neither
    CGA2's "conductorClient/vm() use" pattern nor CGA1's grep set
    caught these TLS-routed inline-wrapper calls. FIX (body
    §5.2 CONDUCTOR-CONTEXT clause; full text ANNEX CGD4.5; CGA2
    row R9 below; CGA2 audit pattern extended to TLS-client-
    routed calls): null-client barrier executions fall back to
    the SERVER `m_mutatorMarkStack` (`:1479`) + SERVER fence
    master — sound because in-window only (WSAC, single writer);
    debug assert null-client => WSAC; client-conductor in-window
    CMS appends are NEXT-CYCLE grey, drained at the next cycle's
    WND-open (legacy parallel: the End-phase
    `m_mutatorMarkStack` appends consumed next cycle,
    MarkStackMergingConstraint.cpp:64-68 comment); CG-I2 gains
    the conductor-context exemption.
  - F32 (major): `MarkStackMergingConstraint` — the tree's sole
    consumer of `m_mutatorMarkStack` — had no disposition.
    VERIFIED: `quickWorkEstimate` reads the stack's size
    (`MarkStackMergingConstraint.cpp:47`; also `:54`
    prepareToExecuteImpl) and `executeImplImpl` transfers it
    (`:72-73`); the file is in `heap/**` and matches CGA1's
    `m_mutatorMarkStack` grep pattern, so CG-T1 as chartered
    FAILED on it as unclassified — the audit of record was
    incomplete against its own grep. Convergence semantics under
    CMS were unspecified (walk CMSes under what order, vs
    server-only). FIX (body §5.2; CGA1 row A21 below): when C1R
    the constraint covers the SERVER stack (F31
    conductor-context appends) + `m_raceMarkStack` ONLY; CMS work
    is accounted exclusively through the WND-open drain into the
    shared mutator stack, which hasWork/didReachTermination
    already counts (`SlotVisitor.cpp:600-605`); NORMATIVE: the
    §3.1(e) WND-open drain precedes the window's first
    constraint-solver pass (so the estimate is never stale-low
    for CMS work). `!C1R`: today's code.
  - F33 (major): the C0 routing predicate was ambiguous — §5.2
    ("when ISS"), §5.3(3) ("when ISS"), and the §4.1 didRun fold
    were ISS-keyed and landed at C0 (CG-2), while the master rule
    requires flag-off shared mode = today's protocol
    BYTE-FOR-BYTE (Heap.cpp:1479 lock-free append, server fence
    reads). A per-barrier CMS lock flag-off is a protocol AND
    plausible bench delta; the spec's own convention (F17 ->
    CGD2.1; F20 -> CGD3.1) demands an explicit ruling for any
    flag-off delta, and none existed. FIX (ruling ANNEX CGD4.4):
    option (b) — routing is FLAG-GATED via C1R := ISS &&
    `useConcurrentSharedGCMarking` (body notation + §5.2,
    §5.3(3), §4.1); flag-off keeps `:1479` + the server fence
    pair + landed didRun behavior; §9.2(1)/§9.3(1) gain explicit
    !C1R no-op arms; CG-T1 records the gate. Option (a)
    (benign-rule the reroutes and soften the master rule) was
    REJECTED: it weakens the strongest gate in the spec for zero
    benefit (C0 lands the fields either way).
  - F34 (major): the F25 deferral discrimination ("between
    cycles ... directly" in §9.2(1); "while m_currentPhase !=
    NotRunning DEFERS" in §9.3(3)) keyed on phase reads by
    ACT/DCT threads that hold neither GCL nor `*m_threadLock` —
    outside F22's enumerated GCL-ordered reader set. VERIFIED:
    nothing in rev 5 synchronized those reads; the bare load
    races `finishChangingPhase`'s store (`Heap.cpp:2213`) — an
    unsuppressed TSAN report (§12 ladder blocker) — and a
    stale-NotRunning read lets a DCT/ACT mutate
    `forEachSlotVisitor` directly against a cycle whose first
    WND-open is concurrently storing the phase — the CGD3.3(a)
    skip/UAF shape the pending list exists to prevent. FIX (body
    §9.2(1)/§9.3(3) rewritten; full text ANNEX CGD4.3): ACT/DCT
    NEVER read phase and ALWAYS enqueue; the pending list is
    applied only at WND-open (before any walk) or quiesced (GCL
    held + phase NotRunning under `*m_threadLock`: §10D revert /
    server teardown); §3.2 F22's reader note now states the
    rewrite leaves NO other phase reader; CG-I14 re-worded;
    CG-T9 gains the NotRunning -> first-WND-open edge arm.
  - Size cap: body compressed to 49998 bytes (<= 50000). Per the
    overflow rule the trimmed prose survives here or in annexes:
    the §9.1(2) and §9.1(7) summaries defer to ANNEX CGP1 (which
    GOVERNS); the §6.1 mechanism list deduplicated against
    §2.3(a); the inline rev attributions moved to this entry's
    mapping note; the §2.2 per-client cite list collapsed to its
    heap §10A pointer; no normative clause was weakened — F28-F34
    full texts are ANNEX CGD4 + the CGP1 rewrite.

- rev 7 (2026-06-07): adversarial round 6 — 6 reviewer findings
  received, 5 distinct after merging duplicates (the two TID-rebias
  findings — "no window disposition, restamps after the final
  WND-close" and "D1/D1R in-stop ordering unsatisfiable under
  C1/F28" — are one defect, merged as F35). ALL ACCEPTED as real
  after verification against the tree; none refuted. F-number
  mapping extends the rev 6 note: F35-F39 = rev 7.
  Dispositions:
  - F35 (blocker; 2 findings merged): the landed conduct-tail
    UNGIL §D.1 TID-rebias block (`Heap.cpp:4877-4915`:
    `rebiasSnapshotForConductor` -> `conductTIDRebiasUnderSharedStop`
    -> `noteRebiasRestampComplete`, gated on
    `sawFullCollectionThisStop`, which AGGREGATES across the
    step-7 `m_requests` drain loop, `:4856-4868`) had NO window
    disposition anywhere in the spec — grep-verified: "rebias"
    appeared nowhere in the body; the sole history mention was
    ANNEX CGA2 R3's mislabel ("TID-rebias teardown"), a C2-only
    row nothing at C1 consults. The window split re-homed every
    OTHER tail step (reclaim -> final window §8.1; ISB bump ->
    every close CG-F4; resume/WSAC/GSP -> per close §3.2; the
    m_requests exit postdates the final close per F23/F28), so a
    faithful CG-1/CG-3 implementation leaves the block at its
    landed position — which under `useConcurrentSharedGCMarking`
    is AFTER the final close's resume steps: WSAC false, GSP
    cleared, mutators running JS while the conductor restamps
    live Structures and runs D1R fires (`fireTransitionThreadLocal`
    takes ConcurrentJSLockers/rank-6b locks; the run-inline
    branch's WSAC premise is false), with no covering ISB edge
    for the jettisons, and with the post-resume `m_freeTIDs`
    release licensed by a Sealed->Restamped flip that no longer
    happens-before resume — dead-TID reissue against still-live
    baked `tid<<48` tags, the D1R type-confusion class. Reachable
    in chartered configs (CG-T3's F19 GIL-off sub-arm; CG-T8's
    F18 sub-arm; snapshots arise from ordinary thread exits).
    Constraints also mutually inconsistent under F28: the landed
    once-per-conduct position can be in no window; the
    aggregation has no per-cycle meaning (Full cycle 1 + Eden
    cycle 2 left "which final window restamps" unruled); and
    "strictly BEFORE the ISB1.1 bump" was ambiguous once CG-F4
    bumps at EVERY close. The spec's own gates could not catch
    it: CGA1's grep set had no rebias pattern; CGS1 asserts no
    SPEC-ungil supersession; no CG-T arm exercised rebias vs
    windows. FIX (body §8.3 TID-REBIAS PIN; full text ANNEX
    CGD5.1, GOVERNS): per-CYCLE rule — the rebias executes inside
    the FINAL window of a conducted FULL cycle, after that
    cycle's reclaim, strictly before that window's CG-F4 ISB
    bump and WSAC/GSP clears, under WSAC, never after any
    WND-close; the predicate re-derives per-cycle (first Full
    cycle with a Sealed snapshot runs it; Eden predecessors/
    successors neither run nor suppress); Sealed-stays-Sealed
    between windows/cycles is safe (gate-lift licensed only by
    the flip); the placement satisfies ANNEX D1/D1R VERBATIM
    (CGS1 note added — no supersession); CGA1 grep set extended
    (four rebias symbols); CGA2 R3 corrected; CG-I23; CG-T8 F35
    sub-arm.
  - F36 (major): §9.2(1)'s "publishes its fence/didRun state as
    dead" sat OUTSIDE the GBL/!WSAC section (the parenthetical
    attaches to epoch=MAX + HCS remove only) and under no stated
    lock — the dying thread has dropped access (so it no longer
    gates the §10.4 barrier) but is still an HCS member, so a
    window CAN open concurrently and the conductor's in-window
    fold/republish loops (§5.3(2) republish + `m_fenceEpochSeen
    = FEP` for EVERY client; §4.1 didRun clear) write the SAME
    plain GCH fields (CG-F1 "clients read own copy plain"):
    an unsuppressed TSAN report (the F22/F34 acceptance bar) not
    in any RACY-TOLERATED row, and a dead sentinel stored
    mid-window spuriously fails the CG-I3 WND-close assert (held
    unexempted per §9.3(1)). The clause also named no consumer —
    nothing specified reads dead state (the fold/republish/CG-I3
    domains are bounded by HCS membership; the GBL-ordered
    remove ends them); the attach side (§9.3(1)) was deliberately
    placed INSIDE the publishing GBL section for exactly this
    reason, and the exit side lacked the symmetry. Rev 2's F2
    ruled only the flush ORDER, never the dead-publication's
    synchronization. FIX: option (a) — the clause is DELETED
    (body §9.2(1)); HCS membership defines the domain; conductor
    stamps on a not-yet-removed exiting client are sound (its
    GCH outlives HCS membership; the remove is GBL-ordered and
    barred in-window per heap I13). ANNEX CGD4.4's "exit
    CMS-flush and dead-publication steps" sentence amended to
    match (this entry is the amendment record). CG-T9 F36 arm:
    amplifier-descheduled EXIT1 parked between the CMS flush and
    the GBL acquire across a fence-raising window, CG-I3 assert
    + TSAN enabled.
  - F37 (major): ANNEX CGA1 row A4 relocated the landed
    `ASSERT(m_mutatorMarkStack->isEmpty())` to "'all CMS empty'
    at endMarking (in-window walk)" — irreconcilable with rev 6's
    F31/CGD4.5(b): the tree runs the landed assert at the TOP of
    `runEndPhase` (after `m_helperClient.finish()`, `Heap.cpp`
    `:2032`-region), THEN `iterateExecutingAndCompilingCodeBlocks
    -> writeBarrier(codeBlock)` (`:2036-2039`), THEN
    `updateObjectCounts(); endMarking()` — and F31 routes a C0/C1
    client-conductor's in-window barriers into its OWN CMS
    (NEXT-CYCLE grey). So at every C1 cycle with executing
    CodeBlocks the conductor's CMS is non-empty when endMarking
    runs, and an implementer consuming A4 verbatim (§4.3) ships
    a deterministic debug-assert failure. A4 dates from rev 2;
    F31 never reconciled it. FIX: A4 amended in place (marked
    rev 7) — the relocated all-CMS-empty walk lives at the
    LANDED `:2032` site, strictly BEFORE the first
    conductor-context writeBarrier batch, where it is sound (the
    final window's §3.1(e) WND-open drain emptied every CMS;
    WSAC bars client appends since); the server-stack and
    race-stack asserts stay at the same site (conductor-context
    server appends also postdate it). §5.2 CONDUCTOR-CONTEXT
    cross-cites; CG-T11 gains the C1 executing-CodeBlocks arm.
  - F38 (major): `m_collectorThreadIsRunning` was unruled at C2:
    CGA1 A17 dispatches it to "§7.2 conductor", but neither §7.2
    nor ANNEX CGA2 mentioned the flag, and CGA2's escape clause
    (conductorClient/vm()/TLS-routed patterns) does not force a
    row for it. Four live consumers verified: (1) the
    `ASSERT(!m_collectorThreadIsRunning)` in
    `requestCollectionShared` (`Heap.cpp:4503`) — if the rewired
    R7 loop keeps the landed flag discipline (`:337/:340/:357`),
    the assert fires on the FIRST mid-cycle ticket against a
    conducting collector thread, and mid-cycle tickets are
    COMMON (§9.1(4), F28); the §7 master rule forbade retiring
    it (named nowhere); (2) the `:5051` conjunct in
    `pollIssRevertIfNeeded`'s quiescence predicate — dropping
    the flag instead silently degenerates it to always-true, an
    unruled weakening of the §10D revert; (3)/(4) `:2828`
    (`requestCollection`, `ASSERT(!isSharedServer())` — !ISS
    only) and `:4334` (§10B.4 pre-ISS migration quiescence) are
    unaffected, but nothing said so. Both halves of the
    implementer's forced guess looked spec-conformant. FIX:
    ANNEX CGA2 row R10 added (flag discipline KEPT by the R7
    loop under `*m_threadLock` around each conducted ticket
    batch; the `:4503` assert RETIRED at C2 behind
    `useSharedGCCollectorThread` and NAMED in §7.2's retire list
    so the master rule permits it; the `:5051` conjunct KEPT —
    it is what makes a GIL-on revert refuse while the collector
    conducts between tickets; `:2828`/`:4334` recorded
    out-of-scope); body §7.2 retire list amended; CG-T6 R10 arm
    (mid-cycle sync requester + RCAC storm vs a conducting
    collector thread, debug build).
  - F39 (major): EXIT1.9/server teardown vs a live C2
    collector-conducted cycle was unruled. §9.2(3)'s covering
    argument (§10D requires phase NotRunning) covers the GIL-on
    revert OUTCOME only: under gilOffProcess
    `pollIssRevertIfNeeded` returns immediately
    (`Heap.cpp:5023-5031` — ISS sticky for process lifetime),
    and §10D never gates ~VM itself. C2 removes what made this
    moot: with activity gating (`:790-792`) and the reroute
    (`:1595-1600`) retired, RCAC tickets fire from timers and
    the collector-thread conductor runs multi-window cycles with
    NO mutator participation; the EXIT1.9 completion fence
    bounds ~VM only on registered lites (UNGIL-HANDOUT EXIT1.9:
    "no registered lite other than m_mainVMLite") — the
    collector thread has no lite, and §9.2(4) covers client
    conductors only. Verified shapes: (a) join-vs-barrier cycle
    (~VM's access-holding lastChanceToFinalize sections vs a
    WND-open barrier waiting on the main client while teardown
    joins the collector); (b) a bare teardown GCL acquire
    succeeding between windows with phase != NotRunning
    (teardown is not a §3.4 site — no back-off applied),
    destroying conduct state mid-cycle (UAF). CG-I14/CGD4.3
    already PRESUPPOSED "server teardown" as a quiesced
    application point stated nowhere; CGA2 R7 rewired the run
    loop with no shutdown/join contract. Squarely charter item 6
    at C2. FIX (body §9.2(5); full text ANNEX CGD5.2, GOVERNS):
    ordered teardown-of-conducting — after the EXIT1.9 wait and
    BEFORE any access-holding teardown section: disable
    elections/collector wakes; acquire GCL with the §3.4
    back-off until {GCL held, phase NotRunning under
    `*m_threadLock`, granted tickets quiesced
    (served-or-refused)}; join the collector thread at that
    boundary; only then run the access-holding tail; teardown is
    then the sanctioned CG-I14 quiesced application point
    (presupposition closed). CG-I24; CG-T6 teardown arm (~VM
    injected while an RCAC C2 cycle is between windows, GIL-on
    and GIL-off — cycle completes or is refused, no watchdog, no
    conduct-state UAF).
  - Size cap: body compressed to 49991 bytes (<= 50000). Per the
    overflow rule the trimmed prose survives here or in annexes:
    §8.3 and §9.2(5) keep normative summaries and ANNEX CGD5
    GOVERNS their full texts; the §9.1(2) CALL-SITES sentence,
    the §9.1(7) narrative, the §9.2(1) C4-DELTA attribution
    string, and the §5.3(3)/§3.2/§3.4 grounding parentheticals
    were tightened with their full texts already resident in
    this log (rev 4 F18, rev 6 F29/F30, rev 6 F34, rev 4 F19,
    rev 5 F22, rev 5 F20) — no normative clause weakened.

Open items for the review loop (tracked, not yet ruled):
1. RESOLVED rev 2 (F7): collector-thread conductor needs no
   VMTraps poll — ANNEX CGA2 row R6.
2. §5.3(3) fail-safe pin (GIL-off always-fenced until the JIT
   reroute lands) costs every GIL-off barrier a fence; reviewers
   should weigh pinning per-stage vs blocking C1 GIL-off on the
   jit row. STILL OPEN — rev 4 F19 re-pointed the pin to the
   SERVER master (the storage emitted code actually reads); the
   cost question is unchanged by that fix.
3. RESOLVED rev 2 (F6): §9.1(2) now pins a fresh pause pair; the
   lost-wakeup argument is in the body.
4. CG-I12's wait bound: needs a measured bound on window length
   at C1 (fixpoint windows include constraint solving; heap
   §10.6 stack scans are O(threads)). STILL OPEN (CG-T8
   measures); rev 2 adds the per-batch pause granularity that
   makes the marker-pause half derivable; rev 6 (F28) extends
   the measured quantity by the inter-cycle re-stop + the
   successor's first window when a drain successor starts.

- rev 8 (2026-06-07): CROSS-DOCUMENT pass (SPEC-congc vs
  SPEC-nativeaffinity vs the frozen family) — 3 findings
  received, ALL ACCEPTED after verification; none refuted.
  Dispositions:
  - F41 (major, lock-table fork): rev 7 §10 added process locks/
    edges (the CMS terminal leaf legal under 7-9b — the same edge
    shape that previously required the explicit both-sides §LK.8
    destructor-leaf supersession vs heap §6's "leaf ... never
    7-9b" row — and the marking-internal group newly
    mutator-reachable out-of-window) with NO SPEC-ungil §LK row,
    while declaring a private "U20-class" lint (CG-T2);
    SPEC-ungil §LK is the ONE merged order, canonical for U20
    (SPEC-ungil.md:867-925), and SPEC-nativeaffinity gates every
    addition through §LK (its §3.5/§9.1/§9.7). §LK silently
    stopped being the one merged order, and nativeaffinity
    LK.1c's "OUTER to ... all leaves" claim was written blind to
    these leaves — the NL>GCL>m_markingMutex>CMS chain
    (§9.1(2) + BL1.6) held only ACCIDENTALLY. FIX: §10 rewritten
    as PROPOSED §LK rows LK.9c/LK.9d, SUPERSESSION-PENDING +
    adoption gate §13.5(1); CG-T2 folded into U20 PROPER; chain
    chartered + linted (ANNEX CGS2.1-2); CGS1 trailer corrected;
    CGS2 folds into SPEC-ungil-history at freeze.
  - F40 (major, falsified cross-spec grounding): nativeaffinity
    NA-I13's conductor exemption (ANNEX BL1.6 there) lets a
    Locked native reach a sync collection mid-body WITH NL held —
    derived against the landed SINGLE-window heap §10 conduct.
    SPEC-congc §3 replaces that with an N-window tenure
    (per-window blocking GCL re-acquires, GBL barriers,
    donateAll/waitForTermination waits, the F28 handoff, the
    `:4955` tail), so an NL-holding mutator-conductor would hold
    NL across the ENTIRE cycle — serializing every Locked
    native/custom accessor/JSClassRef callback/handleHostCall
    funnel process-wide for the cycle. Verified NO deadlock (NL
    waiters are §A.3-compliant park sites, F8-reverting at each
    WND-open per NL1/BL1.1; marking termination needs nothing
    from NL waiters) — a liveness/grounding gap, not
    unsoundness; also ZERO cross-references existed between the
    two specs. RULING (option (a), forbid — full walk ANNEX
    CGD6.1; option (b), re-deriving BL1.6 for the windowed
    protocol, REJECTED there): CG-I19 gains
    `m_nativeLockDepth == 0` at conducting entry; the
    sync-collection request path takes the nativeaffinity BL1.8
    NL drop scope (NA-I11-style, depth saved/restored). Recorded
    BOTH SIDES: SPEC-congc §3.7/CG-I19/CGD6.1 + the §13.5(4)
    gate; SPEC-nativeaffinity rev 8 §3.4/NA-I13
    NARROWED/BL1.6 note/BL1.8/NA-T4 multi-window arm. CG-T8 F40
    sub-arm + NA-T4 rev-8 arm compose.
  - F42 (major, recording defect): rev 7 amended the frozen §A.3
    stop bracket — the JSThreadsStopScope ctor (a frozen-cited
    element of §A.3 rule 5/HBT4.5, "allocation-free closure",
    SPEC-ungil.md:256-268) gains a GCL-held BLOCKING marker
    pause; HBT4's release-before-GCL order extends to window
    re-entry; the §A.3 conductor's GCL wait meaning changes vs
    the 30s watchdog (U32/HBT4.5) — while §13.4 claimed "nothing
    frozen is superseded" and CGS1 folded only into
    SPEC-heap-history; the two new specs applied OPPOSITE
    readings of the both-sides convention to the same frozen
    surface. FIX: §13.4 rewritten; §13.5 adoption gates added
    (nativeaffinity §9 convention); ANNEX CGS2.3-4 carry the
    pending rows; the conductor watchdog BUDGET (marker pause +
    nativeaffinity NL terms) is stated ONCE (CGS2.3, cited from
    nativeaffinity BL1.6/BL1.8).
  - Size cap: §3.4 site dispositions -> CGD6.2; CG-T6/T7/T8/T9
    full charters -> ANNEX CGT1 (verbatim); §8.3/F20/F36/F39/
    CGA2/CGP1 body indexes compressed (annexes already GOVERN);
    body 49,992 bytes.

- rev 9 (2026-06-10): DIRECTED REVISION ROUND 1 (triage +
  confirmed findings; reviewed against tree commit 43fd5fb94387,
  "GIL removal complete" — the IMPLEMENTED engine is now ground
  truth). TRIAGE RESULT (recorded as the round's first finding):
  all 6 round-6 findings (F35-F39, 5 distinct) and all 3
  compose-round findings (F40-F42) are REAL-AND-FIXED; zero
  REAL-AND-UNFIXED; zero CHURN (0 refuted across all rounds; the
  14->13->7->8->7->6 plateau decomposes into fix-induced cascades
  — F37<-F31, F28<-F23, F29/F30<-F26, F20<-F10 — plus reviewers
  reaching later-stage surfaces only after earlier text
  stabilized). Engine symbols spot-verified live:
  conductTIDRebiasUnderSharedStop (Heap.cpp:4763),
  rebiasSnapshotForConductor/noteRebiasRestampComplete/
  sawFullCollectionThisStop (:4924-4978); CGA2 R10 consumers
  (:2865, :4402, :4571, :5136). FREEZE GATE: UNMET — the rev-7/8
  fix texts have never survived an adversarial round; given the
  demonstrated fix-induced-finding rate, rev 9 freezes ONLY after
  a clean pass over the rev-7/8/9 deltas (§8.3/CGD5.1, §9.2(1)
  F36 deletion, CGA1 A4, CGA2 R10, §9.2(5)/CGD5.2, CGD6.1,
  LK.9c/9d + §13.5 gates, CGS2, and rev 9's F43-F47). The §13.5
  counterpart edits (SPEC-ungil §LK rows + U20 extension +
  CGS2 fold; SPEC-nativeaffinity NA-I13/BL1.8 rows) remain
  PENDING on their owners — gates stay open. Open items 2
  (§5.3(3) fence-pin cost, perf-only) and 4 (CG-I12 window
  bound, measured by CG-T8) remain open by design.
  New findings, ALL ACCEPTED after verification; none refuted:
  - F43 (blocker): §9.1/CGS2.4(a) modeled the §A.3 conductor as
    an allocation-free, access-less closure; the IMPLEMENTED
    conductor re-acquires its own client heap access inside the
    JSThreadsStopScope/GCL window (AB-21, commit 353a3100fd61 —
    PREDATING rev 8's verification stamp; VMManager.cpp:631-646;
    Class-A fire bodies take DeferGC and run write barriers) and
    is licensed to run WeakSet::sweep/shrink +
    Heap::sweepNextLogicallyEmptyWeakBlock from its in-window
    allocation slow path (AB-10, commit 43fd5fb94387; assert
    disjuncts WeakSet.cpp:81/:106, Heap.cpp:3339) —
    WeakBlock::destroy from a mid-GC-cycle §A.3 window is
    neither epoch reclaim nor quarantine-routed jettison, so
    §9.1(4) was false the moment §9.1(1) lands; and the spec had
    NO row for conductor-client CMS appends with markers paused,
    conductor allocation under m_isMarking, or conductor
    sweep-to-freelist vs CG-I5. FIX: §9.1(4) carve-out + new
    §9.1(8) (conductor = FULL CLIENT in-window) + CG-I25 +
    CG-T8 F43 arm; CGS2.4(a) "ALLOCATION-FREE" STRUCK (both
    sides — the pending ungil-side row text amended here);
    full walk ANNEX CGD7.1.
  - F44 (major): §5.2's motivating claim was STALE — commit
    43fd5fb94387 already serializes the server stack
    (m_mutatorMarkStack->setMultiProducerAccess() when
    useSharedGCHeap, Heap.cpp:485-497, with the exact
    lost-increment-UAF rationale in-tree) and made
    m_barriersExecuted relaxed-atomic (:1448-1452). The CMS
    soundness motivation is solved in-tree by a different
    mechanism; "!C1R byte-for-byte" must mean the LANDED
    multi-producer shape. FIX: §5.2 re-grounded (CMS =
    contention/scaling + window-drain accounting); NORMATIVE:
    the server stack KEEPS multi-producer mode under C1R (its
    remaining producers are the F31/F43 conductor-context
    appends; narrowing is a chartered perf follow-up); CGA1
    A4/A6 amended. Superseded both sides within this spec (the
    rev 2-8 "lock-free, one mutator" text reads through this
    entry).
  - F45 (major): CGS2.3's §A.3 GCL-wait budget had NO enforcing
    mechanism — the landed acquisition is an unqueued 1ms
    tryLock poll (watchdog ctor Heap.cpp:5568-5590, used by the
    real conductor at VMManager.cpp:577) with no ParkingLot
    queue position, racing the spec's BLOCKING per-window
    re-acquire; with back-to-back fixpoint windows nothing
    structural made the §A.3 requester win a gap, and the
    failure mode is watchdogAssertStopProgress's deterministic
    fail-stop (JSThreadsSafepoint.cpp:512 — the spec's :401/:412
    cites were stale). FIX: §9.1(2a) foreign-waiter fairness
    rule (m_foreignGCLWaiters counter; WND-open re-entry defers
    while nonzero); CG-I26; CGS2.3 amended (budget STRUCTURAL;
    CG-T8 verifies, not samples); mechanism ANNEX CGD7.2.
  - F46 (major): conductSharedCollection installs the main VM's
    AtomStringTable on the conductor thread for the WHOLE
    conduct (Heap.cpp:4885-4889) under an "every mutator is
    parked" license — false BETWEEN windows, where the table's
    owner runs JS and mutates it; CGA2 R3 mischaracterized the
    install as an in-window site (it is a tenure-wide TLS
    install). FIX: §3.7 ATOM-TABLE PIN — per-window
    install/restore once any §13.2 flag is on, no AtomString
    ops between windows, debug null-out between windows;
    CG-I27; CGA2 R3 amended; ruling ANNEX CGD7.3.
  - F47 (major): §9.1(2)/F18 said "the ctor" (singular) — the
    tree has TWO JSThreadsStopScope ctors (blocking
    Heap.cpp:5546-5566; watchdog tryLock-poll :5568-5590, the
    one the REAL conductor uses); the pause obligation must
    live in BOTH (watchdog: after tryLock success, never per
    failed iteration), and the watchdog ctor's tryLock loop is
    itself a landed GCL tryLock site with no §3.4/CGD6.2
    disposition (post-spec it succeeds mid-cycle BY DESIGN).
    FIX: §9.1(2) names both overloads + exact placement +
    construction sites (VMManager.cpp:577 watchdog;
    JSThreadsSafepoint.cpp:445 + SharedHeapTestHarness.cpp:1039/
    :1073/:1107 blocking); CGD6.2 gains the watchdog-ctor row
    (PROCEED — §9.1(1) legalizes the foreign mid-cycle hold; no
    back-off; participates in the F45 waiter counter); CG-T1
    lint covers it.
  - Cite refresh (the 6th confirmed finding): rev 8's
    "verified 2026-06-07" stamp predated four Heap-touching
    commits (through 43fd5fb94387); essentially every Heap.cpp
    cite >= :1430 was off by +16..+115, with two SEMANTIC
    drifts: (1) runSafepointHooksAndReclaim gained a
    hasRetiredItems() early-return (Heap.cpp:5059-5060, commit
    de1d22dc) — §8.1/CG-I11 restated (hooks unconditional;
    bump/stamp/suspension only on retiring cycles); (2) the
    core-constraint m_terminationException root visit is now an
    atomic load (Heap.cpp:3765) — the CGN1/CG-A2 charter now
    explicitly includes runtime/-side visitor inputs the
    heap/**-only grep set cannot catch. Body re-verified and
    re-stamped vs 43fd5fb94387; earlier-rev annex cites read
    through the ANNEX CGD7.4 ledger (BINDING).
  Size cap: §9.1(2a)/(4)/(8), §3.7 atom pin and the rev-9 full
  walks live in ANNEX CGD7; body compressed back under 50000.

- rev 10 (2026-06-10): DIRECTED REVISION ROUND 2 — a
  VERIFICATION pass over the rev-9 deltas (F43-F47 + the cite
  refresh). RESULT: all 7 rev-9 items re-verified ACTUALLY-FIXED
  with accurate tree grounding; ZERO new blocker/major findings;
  ZERO churn; NO normative deltas — no ruling moves, so no
  supersession entry moves (CGS1/CGS2 rows UNCHANGED; the
  both-sides convention has nothing to record this round beyond
  this log). Verification anchors (rev-10 body line numbers;
  the rev-10 status edit preserves all line positions <= :759,
  so the rev-9 anchors below remain live):
  - F43 (was blocker): §9.1(8) full-client rule at
    SPEC-congc.md:597-609, §9.1(4) carve-out at :562-576,
    CG-I25, ANNEX CGD7.1. Code: conductor self-access
    re-acquire `VMManager.cpp:631-646` (AB-21); in-window
    weak-sweep license assert disjuncts `WeakSet.cpp:81`/`:106`,
    `Heap.cpp:3339` (AB-10).
  - F44 (was major): §5.2 re-grounding at SPEC-congc.md:259-285;
    CGA1 A4/A6 amended. Code: `setMultiProducerAccess()` under
    `useSharedGCHeap` `Heap.cpp:485-497`; relaxed-atomic
    `m_barriersExecuted` `:1443-1452`; append `:1499`.
  - F45 (was major): §9.1(2a) foreign-waiter fairness + CG-I26,
    ANNEX CGD7.2. Code: watchdog-ctor tryLock poll
    `Heap.cpp:5584-5587`; election hold `:4628`;
    `watchdogAssertStopProgress` `JSThreadsSafepoint.cpp:512`.
  - F46 (was major): §3.7 ATOM-TABLE PIN at SPEC-congc.md:199-204
    + CG-I27, ANNEX CGD7.3. Code: tenure-wide install
    `Heap.cpp:4885-4889`.
  - F47 (was major): both-ctors pause text at
    SPEC-congc.md:521-536; CGD6.2 watchdog row pointer at
    :156-158. Code: blocking ctor `Heap.cpp:5546-5566`, watchdog
    ctor `:5568-5590`.
  - Cite refresh: re-stamp at SPEC-congc.md:12-14 + the CGD7.4
    ledger; ~25 load-bearing cites spot-checked accurate; both
    semantic drifts restated in-body (`:5059-5060` reclaim fast
    path at :466-469 and CG-I11 at :755-759; atomic
    `m_terminationException` at :478-481).
  Cross-checks run, all clean: (a) the F45 waiter counter vs the
  §9.2(5) teardown back-off — livelock-free per CGD7.2 rule 1's
  decrement-on-hold; (b) F43 vs CG-I1/I5/I15/I19 — consistent;
  (c) CGD7.1(e) vs the landed SPEC-ungil.md:256-268 R1.i class-4
  variant — aligned (ungil already licenses in-window alloc for
  class-4 conductors); (d) HEAD drift: c8de3791 touches only
  runtime/DeferredWorkTimer*/SparseArrayValueMap — no cited line
  moved; the 43fd5fb94387 stamp + CGD7.4 ledger remain valid
  (note appended to CGD7.4).
  FREEZE GATE: STILL UNMET, by the spec's own rule. (1) The
  §13.5 adoption gates (body :905-915) are all 4 still OPEN —
  the SPEC-ungil §LK LK.9c/9d rows + U20 lint extension
  (CGS2.1-2), the §A.3 rule-5/HBT4.5 + U32 wait-bound amendment
  (CGS2.3, F45-structural), the HBT4 window-re-entry extension
  (CGS2.4, carrying the F43 ALLOCATION-FREE strike), and the
  nativeaffinity NA-I13/BL1.8 rows are PENDING on their owners;
  they cannot land from this round (write set = the three congc
  files) and they BLOCK C1/freeze per §13.5. (2) CG-7 (body
  :942-948) requires a clean adversarial round over the
  rev-7/8/9 deltas: rev 10 constitutes ONE clean directed pass
  over the rev-9 deltas; the rev-7/8 deltas (rev 9 log list)
  still need theirs. Status stays DRAFT, NOT converged.
  Tracked items 2 (§5.3(3) fence-pin cost, perf-only) and 4
  (CG-I12 window bound, CG-T8-measured) remain open by design.
  CLOSURE PLAN recorded: (i) owners land the §13.5 counterparts
  in a round whose write set includes SPEC-ungil(-history) and
  SPEC-nativeaffinity(-history), then the gates close in §13.5;
  (ii) one final adversarial pass scoped to the rev-7/8/9 delta
  list; clean -> execute CG-7 (CGS1/CGS2 supersessions recorded
  both sides, size-cap check, freeze).
  Size cap: the rev-10 status/§13.5/CG-7 edits are offset by
  compressing §13(3)(b)-(c) and §13(4) — NO content lost: the
  §13(4) full discipline text lives VERBATIM in ANNEX CGS1/CGS2
  + the rev 8 log entry above; the §13(3) deltas dropped only
  prose duplicated at §9.1(2)/F45. Body = 49,983 bytes.

- rev 11 (2026-06-10): DIRECTED REVISION ROUND 3 — one
  confirmed finding (major): the freeze gate REMAINS UNMET, is
  accurately tracked, and is UNCLOSABLE within this round's
  write set (SPEC-congc{,-history}.md + CONGC-HANDOUT.md only);
  the spec correctly stays DRAFT, NOT converged. Disposition:
  (1) GATES: all four §13.5(5) adoption gates re-confirmed OPEN
  (body :905-915, restamped "at rev 11") — CGS2.1-2 (SPEC-ungil
  §LK LK.9c/9d + U20 extension; blocks the §5.2 CMS lock and
  C1), CGS2.3 (§A.3 rule-5/HBT4.5 + U32 wait-bound amendment,
  F45-structural, shared with nativeaffinity BL1.6/BL1.8;
  blocks C1), CGS2.4 (HBT4 order extended to window re-entry,
  carrying the F43 ALLOCATION-FREE strike; blocks the §3.1
  re-entry blocking acquire), nativeaffinity BL1.8 NL-drop
  (F40; blocks C1 gilOff). Closing them requires edits to
  SPEC-ungil{,-history}.md and SPEC-nativeaffinity{,-history}
  .md — outside this round's permitted writes; they stay
  SUPERSESSION-PENDING per §13.5(4).
  (2) CG-7 SCOPE RULING (the one normative delta this round;
  body :942-948 rewritten): the rev-9 deltas (F43-F47 + the
  cite refresh) are ruled COVERED — rev 10 was a clean directed
  pass over exactly that delta set, all 7 items re-verified
  actually-fixed against the tree with 0 new findings (rev 10
  log anchors), which satisfies CG-7's own wording for that
  slice. CG-7's remaining precondition narrows to ONE clean
  adversarial pass over the rev-7 and rev-8 deltas (rev 9 log
  list). This ruling moves NO frozen-spec text: CG-7 is a
  congc-internal task row, so the both-sides supersession
  convention has nothing to record — CGS1/CGS2 rows UNCHANGED.
  (3) NO new contradictions confirmed at rev 10/11: header,
  §13.5(5), CG-7, and this log are mutually consistent; the
  VMManager.cpp:637 "allocation-free" comment is the R1.i
  DEFAULT-conductor rule and SPEC-ungil.md:256-268 carries the
  class-4 supersession (access retained, alloc legal
  in-window), aligned with CGD7.1(e). No new tree-grounding
  claims made this round (docs-only, no git): the 43fd5fb94387
  verify stamp + c8de3791 drift check + CGD7.4 ledger stand as
  recorded at rev 10.
  (4) Tracked items 2 (§5.3(3) fence-pin cost, perf-only) and 4
  (CG-I12 window bound, CG-T8-measured) remain open by design;
  non-blocking.
  CLOSURE PLAN (unchanged shape, rev-10 step now counted):
  (i) a round whose write set includes SPEC-ungil{,-history}.md
  and SPEC-nativeaffinity{,-history}.md lands the four §13.5
  counterpart rows (CGS2.1-4; NA-I13/BL1.8), then §13.5(5)
  flips them CLOSED here; (ii) one adversarial pass scoped to
  the rev-7/8 deltas only; on a clean result execute CG-7
  (CGS1/CGS2 supersessions recorded both sides, size-cap check,
  freeze).
  ANCHOR PRESERVATION: the rev-11 edits keep the body at 952
  lines with every edited region replaced line-for-line (header
  :1-14 within :1-17; §13.5(5) :905-915; CG-7 :942-948), so ALL
  rev-9/rev-10 log anchors remain live at their recorded
  positions. Size cap: header compression (charter prose
  shortened, no content lost — full prior wording in the rev 10
  log above) absorbs the edits; body = 49,962 of 50,000 bytes.
  CONGC-HANDOUT.md: in the write set but NOT created this
  round — no finding touches it and no handout exists to
  restate; deferred to the freeze (CG-7) packaging step.

- rev 12 (2026-06-10): DIRECTED REVISION ROUND 4 — one
  confirmed finding (major): the freeze gate REMAINS UNMET at
  rev 11, accurately tracked, and STILL UNCLOSABLE within this
  round's write set (SPEC-congc{,-history}.md +
  CONGC-HANDOUT.md only); the spec correctly stays DRAFT, NOT
  converged. Disposition:
  (1) GATES: all four §13.5(5) adoption gates re-confirmed
  OPEN, restamped "at rev 12" (body :907; same rows as the
  rev 11 log: CGS2.1-2 blocking the §5.2 CMS lock and C1;
  CGS2.3 F45-structural, shared with nativeaffinity
  BL1.6/BL1.8, blocking C1; CGS2.4 with the F43
  ALLOCATION-FREE strike, blocking the §3.1 re-entry blocking
  acquire; nativeaffinity BL1.8 NL-drop F40, blocking C1
  gilOff). Closing them still requires edits to
  SPEC-ungil{,-history}.md and SPEC-nativeaffinity{,-history}
  .md — outside this round's permitted writes; rows stay
  SUPERSESSION-PENDING per §13.5(4). CG-7's precondition is
  unchanged from the rev 11 narrowing: ONE clean adversarial
  pass over the rev-7/8 deltas only (rev 9 log list); the
  rev-9-delta coverage ruling stands as recorded in the rev 11
  log. No frozen-spec text moves this round, so the both-sides
  supersession convention again has nothing to record —
  CGS1/CGS2 rows UNCHANGED.
  (2) CITE FIX (the one textual delta beyond restamps): the
  GCL-busy/election timed wait — `m_gcElectionCondition
  .waitFor(*m_threadLock, 1_ms)` — is at `Heap.cpp:4627`, not
  `:4628`, both at HEAD and at pinned 43fd5fb94387 (`:4628` is
  the closing brace line). Fixed at BOTH body occurrences:
  §2.2 (:58, "GCL-busy wait") and §9.1(2a) (:551, election
  loser's timed wait). The §9.1(3) §10.2 GCL-busy-rule RANGE
  cite `:4616-4628` is correct as a range (it spans the
  release/lock/wait block) and is unchanged. The CGD7.4 ledger
  row that produced the off-by-one (old `:4554` -> new
  `:4628`, runSharedGCElection internals) is corrected by the
  rev-12 BINDING addendum appended to CGD7.4 below: `:4554` ->
  `:4627`. Ledger correction is congc-internal (CGD7.4 is a
  congc history annex), so both-sides = body + history here.
  (3) ENGINE GROUND-TRUTH RE-CONFIRMED this round (live tree
  reads): watchdog-ctor tryLock poll calling
  watchdogAssertStopProgress at `Heap.cpp:5584-5587`;
  per-window atom-table install at `Heap.cpp:4885-4889`;
  `watchdogAssertStopProgress` definition at
  `JSThreadsSafepoint.cpp:512`; `m_foreignGCLWaiters` absent
  from Source/JavaScriptCore/** (correct — §9.1(2a) is
  NORMATIVE, not landed). No other tree-grounding claims made
  (docs-only, no git): the 43fd5fb94387 verify stamp +
  c8de3791 drift check + CGD7.4 ledger (as corrected) stand.
  (4) Tracked items 2 (§5.3(3) fence-pin cost, perf-only) and
  4 (CG-I12 window bound, CG-T8-measured) remain open by
  design; non-blocking.
  CLOSURE PLAN (unchanged from rev 11): (i) a round whose
  write set includes SPEC-ungil{,-history}.md and
  SPEC-nativeaffinity{,-history}.md lands the four §13.5
  counterpart rows (CGS2.1-4; NA-I13/BL1.8), then §13.5(5)
  flips them CLOSED here; (ii) one adversarial pass scoped to
  the rev-7/8 deltas only; on a clean result execute CG-7
  (CGS1/CGS2 supersessions recorded both sides, size-cap
  check, CONGC-HANDOUT.md created as freeze packaging,
  freeze).
  ANCHOR PRESERVATION: rev-12 edits keep the body at 952 lines
  with every edited line replaced in place (header :1-7 within
  :1-17; §2.2 :58; §9.1(2a) :551; §13.5(5) :907; CG-7 :946),
  so ALL rev-9/10/11 log anchors remain live at their recorded
  positions. The header's rev-9-COVERED parenthetical moved
  out of the header to make room for the cite-fix note; its
  full text remains normative in the CG-7 task row
  (:942-945) and the rev 11 log. Size cap: body = 49,958 of
  50,000 bytes.
  CONGC-HANDOUT.md: in the write set but NOT created this
  round — unchanged ruling from rev 11 (no finding touches it;
  no handout exists to restate); deferred to the freeze (CG-7)
  packaging step.

- rev 12 addendum (2026-06-12): F48 — A4-site conductor-client
  exemption (full text ANNEX CGD8.1 below; GOVERNS). Repro
  ground truth: congc-t1 under the pinned GIL-off env +
  `useConcurrentSharedGCMarking=1` deterministically aborted at
  the LANDED A4 walk (`Heap.cpp:2227`,
  `ASSERTION FAILED: !client.m_mutatorMarkStack ||
  client.m_mutatorMarkStack->isEmpty()`). Root cause is a
  rev-7-vs-rev-9 internal contradiction, NOT an engine bug: F37
  (rev 7) justified the walk's emptiness claim by "the final
  window's §3.1(e) WND-open drain emptied every CMS and WSAC
  bars client appends since" — but F43/CGD7.1(a) (rev 9) made
  the client-conductor a FULL CLIENT whose in-window
  conduct-path write barriers legally append to its OWN CMS at
  ANY in-window point (NEXT-CYCLE grey), and WSAC never barred
  the conductor's own thread. F37's reasoning covered only the
  two post-site end-phase batches (CGD4.5(b)); rev 9 never
  reconciled A4 with F43. RULING: the A4 emptiness claim binds
  every NON-CONDUCTOR client ONLY; the walk exempts the client
  whose `currentThreadClient()` is the conducting thread's.
  Body edit byte-neutral in place (§5.2 :296, "BEFORE these
  batches (F37" -> "conductor EXEMPT (F37/F48"; 36->36 chars,
  body stays 953 lines / 49,998 bytes — all rev-9..12 anchors
  live; the BEFORE-the-batches placement stays normative via
  the A4 row + CGD8.1). A4 row amended in place (marked).
  Empirical confirmation of the exemption's MINIMALITY: with
  the exemption landed (conductor client skipped, every other
  client still hard-asserted under its CMS lock), congc-t1
  passes 5/5 GIL-off + flag-off bare + GIL-on C1 — i.e. no
  non-conductor client's CMS was ever non-empty at the site;
  the only appender was the conductor itself, exactly the
  F43-legalized set. Not an assert weakening: the exempted
  appends are the CGD4.5(b)/CGD7.1(a)-LEGAL class (drained at
  the next WND-open per §5.2 drain (i); exit-flushed via
  §9.2(1) otherwise); any other client's non-empty CMS still
  fail-stops.

---

## ANNEX CGA1 (BINDING) — "the mutator"-singular audit table

Audit patterns (CG-T1 grep set, `heap/**` only):
`m_mutatorDidRun`, `m_mutatorSlotVisitor`, `m_mutatorMarkStack`,
`mutatorShouldBeFenced`, `m_barrierThreshold`, `stopTheMutator`,
`resumeTheMutator`, `mutatorWaitingBit`, `mutatorHasConnBit`,
`hasAccessBit`, `stoppedBit`, `m_worldState`, `mutatorState()`,
`m_mutatorState`, `sanitizeStackForVM`, `shadowChicken`,
`m_mutatorExecutionVersion`, `m_barriersExecuted`,
`m_incrementBalance`, `mutatorIsStopped`, `rightToRun`,
`m_currentThreadState`, `m_machineThreads`; added rev 7 (F35):
`conductTIDRebiasUnderSharedStop`, `rebiasSnapshotForConductor`,
`noteRebiasRestampComplete`, `sawFullCollectionThisStop`
(dispositioned by body §8.3 / ANNEX CGD5.1). Every match must map to
a row below (or a row added by the implementing change with the
same discipline). Dispositions per SPEC-congc §4.3.

| # | Site | Disposition | Rule |
|---|---|---|---|
| A1 | `m_worldState` bit machine: `Heap.cpp:2348` (stopTheMutator), `:2390` (resumeTheMutator), `:2421-2459` (stopIfNecessarySlow), `:2497-2533` (waitForCollector), `:2534-2600` (acquireAccessSlow), `:2601-2670` (releaseAccessSlow), `:2652-2686` (conn relinquish + unpark), `:2688-2714` (handleNeedFinalize), `:2747` (notifyThreadStopping), asserts `:2354-2384` | LANDED-N-ARY (superseded when ISS) | Unreachable/no-op once ISS (RELEASE_ASSERTs `:2352`, `:2393`; reroute `:2421-2427`, `:2545-2616`); CG-I7 keeps them dead in ALL stages. `!ISS`: untouched (CG-I0). |
| A2 | `m_mutatorDidRun` writes `Heap.cpp:2433, 2519, 2594` (legacy paths); consumer `:2234-2237` (`m_mutatorExecutionVersion`) | FOLDED | §4.1: per-client `m_didRunSinceLastWindow` set in AHA success tail + SINFAC hot-poll exit; conductor ORs into the `:2234` consumer at WND-open, clears in-window. Legacy writes stay `!ISS`-only. CG-I9. |
| A3 | `m_mutatorSlotVisitor` (`Heap.h:1182`; ctor `Heap.cpp:378`; `forEachSlotVisitor` `HeapInlines.h:279`; assist use `Heap.cpp:3974`) | STAGE-GATED (C4) | §7.4: per-client assist visitors registered at ACT; the server's `m_mutatorSlotVisitor` remains for `!ISS` and as the conductor's own assist slot. CG-I14. |
| A4 | `m_mutatorMarkStack` (`Heap.h:1215`; append `Heap.cpp:1499` — MULTI-PRODUCER once `useSharedGCHeap`, ctor `:485-497`, F44 rev 9; clear/log/empty-assert sites per CGD7.4; empty assert `:2063-2064`) | FOLDED | §5.2 CMS: per-client stack + leaf lock when C1R; window/threshold drains under `m_markingMutex`. AMENDED rev 7 (F37 — the rev 2 "at endMarking" placement deterministically fired on F31's conductor-context appends, which PRECEDE endMarking at `:2036-2039`): the relocated "all CMS empty" walk stays at the LANDED `:2063-2064` site (top of runEndPhase, after `m_helperClient.finish()`, strictly BEFORE the first conductor-context writeBarrier batch `:2067-2069`), where it is sound — the final window's §3.1(e) WND-open drain emptied every CMS and WSAC bars client appends since; the server/race-stack asserts stay there too (conductor-context server appends also postdate the site). AMENDED rev 12 addendum (F48/CGD8.1 — F37's "WSAC bars client appends since" never held for the conductor's OWN client once F43/CGD7.1(a) made it a full client with in-window own-CMS appends, which can PRECEDE the site): the emptiness claim binds NON-CONDUCTOR clients only; the walk skips the conducting thread's `currentThreadClient()` (its CMS contents are NEXT-CYCLE grey, §5.2 drain (i)); every other client still hard-asserts empty under its CMS lock. CG-T11 C1 executing-CodeBlocks arm. CG-I2/I10. |
| A5 | `m_mutatorShouldBeFenced`/`m_barrierThreshold` (`Heap.h:722-726, 1209`; writes `Heap.cpp:473-474`, `:3928-3940`, raises/lowers `:1111`, `:1247`, init `:4456`; readers `:714`, `:746`, `:1433`, `:3324`; JIT bakes `addressOf*` `Heap.h:723,726`) | FOLDED | §5.3: server master mutated in-window only + FEP; per-client copies republished in the mutating window; consumers read current client's copy when ISS. JIT address: §13.3(a) charter; GIL-off pinned always-fenced until it lands. CG-I3. |
| A6 | `m_barriersExecuted` (`Heap.cpp:1448-1452`; reset per CGD7.4) | RACY-TOLERATED | AMENDED rev 9 (F44): landed as relaxed atomic load/store (43fd5fb94387) — no longer a plain `++`; lost updates documented benign in-tree; the TSAN suppression row RETIRES (no plain-access report remains). |
| A7 | `sanitizeStackForVM(vm())` (`Heap.cpp:1704`, `:2206`, `:2675`) | VM-SINGULAR-DEFERRED | Per-VM, not per-mutator; self-guards on entered state. Post-GIL per-thread stacks are vmstate/ungil territory (lite-owned stacks); conductor calls it only in-window. |
| A8 | shadow chicken + `vm().topCallFrame` (`Heap.cpp:2253-2254`) | VM-SINGULAR-DEFERRED | In-window read of the one main VM's state; ungil §A.1 reroutes topCallFrame per-lite — when that lands this becomes a registry walk (already noted in-tree `:2249-2252`). |
| A9 | `mutatorState()` / `m_mutatorState` (`Heap.cpp:583`, `:3337`, `:3384-3390`; slot dispatch `Heap.h:1080-1095`) | LANDED-N-ARY | Already per-client via `mutatorStateSlot()` routing when ISS (Heap.h:1080-1095). No change. |
| A10 | Per-client deferral (`Heap.h:1061`; heap I17) | LANDED-N-ARY | No change; CIND/assist consult calling client (also gates C4 assist entry). |
| A11 | `SlotVisitor::m_mutatorIsStopped`/`m_rightToRun`/`updateMutatorIsStopped` (`SlotVisitor.h:166-168, 236-239`; `SlotVisitor.cpp:469-486`; resume loop `Heap.cpp:2315-2342`) | WINDOW-CONFINED -> STAGE-GATED (C1) | Semantics is "world is stopped", not "the one mutator": keyed on `m_heap.worldIsStopped()`, flipped by stopThePeriphery/resumeThePeriphery which become per-window (§3). N visitors already handled by the `:2315-2342` loop. C4 adds per-client visitors to the same machinery. |
| A12 | `m_raceMarkStack` + lock (`Heap.h:1169, 1184`; `aboutToMarkSlow` `MarkedBlock.cpp:345+`) | LANDED-N-ARY | Locked; mutator-count-independent. CG-T3 exercises under N. |
| A13 | Re-whiten CAS protocol (`Heap.cpp:1444-1467`) and barrier race comments (`:734-738`, `:1473-1477`) | LANDED-N-ARY | Single-word CAS vs collector; argument independent of mutator count (monotone isMarked). CG-T3. |
| A14 | `m_incrementBalance` (`Heap.cpp:1969` reset; `:3959-3978` assist) | STAGE-GATED (C4) | Per-client balance; reset folds at WND-open. Until C4: ISS early-return stays (`:3950`). |
| A15 | `m_currentThreadState`/machine-threads scan (`Heap.cpp:1879` fixpoint assert; gatherStackRoots `:1024-1080`) | WINDOW-CONFINED | Conservative scan in-window only (assert `:1037` KEEPS); N-thread coverage landed (heap §10.6/T6, AHA registration `Heap.cpp:5678`). |
| A16 | Activity callbacks (`Heap.cpp:790-792`; reroute `:1595-1600`) | STAGE-GATED (C2) | §7.2: RCAC tickets + collector-thread wake; SINFAC fallback. |
| A17 | Collector-thread plumbing (`shouldCollectInCollectorThread` `:1631-1648`, `collectInCollectorThread` `:1650`, run loop `:333-357`, `m_collectorThreadIsRunning`, `:1686` assert) | STAGE-GATED (C2) | §7.2 conductor; legacy stop bits stay dead (CG-I7). |
| A18 | `handleNeedFinalize`/`needFinalizeBit` (`Heap.cpp:2688-2714`; finalize `:2753`) | WINDOW-CONFINED | Shared mode finalizes in-window (conduct loop); heap §10B(5)'s "no JS finalizers in the stop window" + ungil §F.3(b) carve-out unchanged. The legacy bit path is `!ISS`-only. |
| A19 | `requestCollection` legacy asserts (`Heap.cpp:2806-2823`) | LANDED-N-ARY | `!ISS`-only (`:2812` assert); shared path is `requestCollectionShared` `:4479`. |
| A20 | Eden/full activity callback objects themselves (`m_fullActivityCallback` etc.) | STAGE-GATED (C2) | Fire-side only; their timers run on the main run loop; C2 routes their "collect now" into RCAC. |
| A21 | `MarkStackMergingConstraint.cpp:47, :54, :72-73` (estimate/prepare/transfer of `m_mutatorMarkStack`) — added rev 6, F32: the file is in `heap/**`, matches the `m_mutatorMarkStack` pattern, and had no row | FOLDED (§5.2) | When C1R: the constraint covers the SERVER `m_mutatorMarkStack` (F31 conductor-context appends only) + `m_raceMarkStack`; CMS work is accounted exclusively via the §5.2(i) WND-open drain into the shared mutator stack (hasWork/didReachTermination count it, `SlotVisitor.cpp:600-605`); NORMATIVE: the §3.1(e) drain precedes the window's first constraint-solver pass. `!C1R`: today's code (CG-I0). |

## ANNEX CGB1 (BINDING) — BlockDirectoryBits out-of-window audit (T8 extension)

Charter: re-run the T8 audit (heap §14 T8; stop-mode banner
`BlockDirectory.cpp:613`) with the question changed from "is this
in-window/MSPL/!ISS" to "may this run concurrent with marking
helpers (C1) or a sweeping client (C3)". Seed rows from the current
tree (the executed audit at C3 entry must cover every
`BlockDirectoryBits` accessor; rows here are the known
classification anchors):

| # | Site | C1/C3 classification |
|---|---|---|
| B1 | `m_bits.resize` in `addBlock` (`BlockDirectory.cpp:177-181`; BVL+MSPL per heap I5b) | WRITER — gains release-publication of the storage descriptor (CG-F3). |
| B2 | `parallelNotEmptyBlockSource` lock-free `markingNotEmpty().findBit` (`BlockDirectory.cpp:539-559`; in-window assert `:556`) | READER — C1 relaxes the assert for marker helpers; converts to acquire-read of the published descriptor, stale-bound-tolerant (CG-I8(d)). |
| B3 | `IncrementalSweeper` lock-free isDestructible/isEmpty reads (`IncrementalSweeper.cpp:60` banner) | READER — C3 sweeper-client runs them under MSPL per quantum (CG-I13); no lock-free out-of-window directory reads survive C3. |
| B4 | `assertNoUnswept` skip (`BlockDirectory.cpp:495-519`) | DEBUG — keeps its shared-mode skip; C3 narrows the skip to "no MSPL and not in-window". |
| B5 | `assertIsMutatorOrMutatorIsStopped` consumers (e.g. `:511`, `:556`) | DEBUG — predicate re-derived per stage: in-window OR MSPL OR owning-LA-thread OR (C1) registered marker helper on acquire-published storage. |
| B6 | Eden-bit store under BVL (heap §5.2(2), `LocalAllocator.cpp` `:250`-area edits) | WRITER — unchanged (BVL); marking helpers never write eden bits. |
| B7 | `MarkedBlock::Handle::sweep` directory-bit reads under MSPL (`Heap.cpp:1487-1498` banner) | READER — unchanged (MSPL). |
| B8 | TLC teardown bit flips (`GCThreadLocalCache.h:87-91`, MSPL) | WRITER — unchanged (MSPL; heap I5b). |

Executed-audit obligation: the C3 change adds every remaining
accessor as a row with one of {IN-WINDOW, BVL, MSPL,
ACQUIRE-PUBLISHED-READER, DEBUG, !ISS}; CG-T7 fails on any accessor
without a row.

## ANNEX CGN1 (BINDING) — cell-lock coverage audit (CG-A2)

EXECUTED AT C1 ENTRY (gate for CG-T5; rows appended here when the
audit runs — the annex is binding as an OBLIGATION now and as a
TABLE once executed). Row schema: {class / visitChildren site,
mutator-side writer protocol (om §/ungil §N row), visitor-side
read disposition per SPEC-congc §8.2}. Seed dispositions fixed
now (normative):

- N1. JSObject butterfly/shape storage: om §6/§9 frozen protocol;
  visitor follows the om-specified read side (segmented/flat tag
  decode); IN-PROTOCOL.
- N2. JSString ropes: ungil §N.2 release-CAS publish / acquire
  read; visitor acquire-reads fiber words; never resolves;
  IN-PROTOCOL.
- N3. JSMap/JSSet/WeakMap impls: ungil §N.1 cell-locked INCLUDING
  reads — visitor side is CELL-LOCKED with tryLock+revisit
  (CG-I15); a failed tryLock re-queues the cell on the visitor's
  own stack. Termination (rev 2, F3 — the rev 1 sentence here
  stated the inverse and is SUPERSEDED): by CG-I18 (SPEC-congc
  §8.2) no thread parks, passes a stop poll, or releases access
  while holding 10a, so IN-WINDOW every 10a lock is free and each
  retry succeeds; out-of-window a failed tryLock defers to the
  race stack / next window. Each CELL-LOCKED row's mutator side
  is audited against CG-I18 when CG-A2 executes.
- N4. Structure: `Structure::m_lock` (rank 10b) — visitor uses the
  existing concurrent-JIT-safe read paths (Riptide already races
  compiler threads here); IN-PROTOCOL.
- N5. ArrayBuffer/wasm memory words: ungil §N.6 torn-pair rules;
  visitor reads {base,length} per the N6 read order; IN-PROTOCOL.
- N6. Profiling-class fields (FunctionRareData jit item 7 etc.):
  RACY-TOLERATED rows must each name the field and the reason;
  TSAN suppressions key on this list ONLY.

## ANNEX CGA2 (BINDING) — C2 client-less conductor refactor surface

Charter (rev 2, F7; pattern EXTENDED rev 6, F31): SPEC-congc
§7.2's collector-thread conductor is not a client and never holds
access; the landed conduct path is written against one. Every
conductorClient/vm() use AND every TLS-client-routed call
(`currentThreadClient()` consumers reached from the conduct path,
e.g. inline `writeBarrier` wrappers — the F31 class neither the
old pattern nor CGA1's grep caught) in the conduct path gets a
row; the C2 change adds rows for any site this table misses
(CG-T6 fails on an unclassified site).

| # | Site | Disposition |
|---|---|---|
| R1 | `conductSharedCollection(GCClient::Heap&)` signature (`Heap.cpp:4757`) | Parameter becomes nullable (`GCClient::Heap*`); null = standalone conductor (C2). |
| R2 | Step-3 own-access release (`Heap.cpp:4769-4770`) and tail `conductorClient.acquireHeapAccess()` (`:4955`) | SKIP when null — the collector thread has no access to release/re-acquire; the §10.4 barrier then waits on ALL clients (no "every client except the conductor's own" carve-out). |
| R3 | Main-VM in-window work: `sanitizeStackForVM(vm())` (`Heap.cpp:1704, :2206`), shadow chicken (`:2253-2254`), AtomStringTable scope | KEEP, executed by the conductor thread IN-WINDOW — licensed by heap §10B rule 2 (phase-loop vm() asserts gain `|| WSAC`); the heap T9 audit classified sites for a client conductor, and every such site is in-window, where client-ness is irrelevant. AMENDED rev 7 (F35): the rev 2 entry "TID-rebias teardown (`:4880-4915` area)" was a MISLABEL — that block is the §D.1 rebias EXECUTION, not teardown, and its window placement is a stage-C1 body rule (§8.3 / ANNEX CGD5.1, GOVERNS); this row merely INHERITS it for the C2 conductor (the rebias is conductor-executed, in the Full cycle's final window, client-ness irrelevant). AMENDED rev 9 (F46): "every such site is in-window" was FALSE for the AtomStringTable entry — the landed install (`Heap.cpp:4885-4889`) is a TENURE-WIDE TLS install spanning between-window periods, not an in-window execution; superseded by the §3.7 ATOM-TABLE PIN (per-window install/restore, ANNEX CGD7.3); sanitizeStackForVM/shadow-chicken rows stand. |
| R4 | Step-8 resume pass / per-client TLC loops (`Heap.cpp:4923-4925`) | UNCHANGED — already loop over HCS; the conductor's own (nonexistent) client simply contributes no entry. |
| R5 | §10D revert poll context (`pollIssRevertIfNeeded`) | NOT run by the collector conductor (main client's thread only, heap §10D); no change. |
| R6 | VMTraps poll in `runSharedGCElection` (`Heap.cpp:4562-4572`) | SKIPPED for the collector thread: it never enters a VM, so no JSThreads/debugger conductor ever needs IT parked via traps — its §A.3 compatibility is being access-free + condvar-parked (rev 1 open item 1 RESOLVED). |
| R7 | Collector run loop (`Heap.cpp:333-357`; `shouldCollectInCollectorThread` `:1631-1648`) | REWIRED: wait on the `m_threadCondition`-class signal for granted-unserved tickets (RCAC/activity wakes, SPEC-congc §7.2), then run the election-equivalent (tryLock GCL; AMENDED rev 5, F24 — after tryLock success, under `*m_threadLock`, the §3.4 guard applies: `m_gcConductorActive && m_currentPhase != NotRunning` => unlock GCL and re-wait on the ticket signal (a mid-cycle RCAC ticket against a between-windows C1 cycle otherwise nests a conductor, CGD1.1-at-C2); else set GCA + `m_gcConductorThread`, conduct, drop both per the continuity bound CG-I12; the final-close GCL release is THIS loop's per F23). CG-T6 R7-guard arm. |
| R8 | `m_currentThreadState`/conservative-scan registration | N/A — the collector thread is never a mutator root; it contributes no stack to heap §10.6 gathering (it is not in HCS). |
| R9 | (rev 6, F31) TLS-routed in-window barrier executions: runEndPhase `iterateExecutingAndCompilingCodeBlocks -> writeBarrier(codeBlock)` (`Heap.cpp:2036-2039`) and `m_codeBlocks->iterateCurrentlyExecuting -> writeBarrier` (`:2085-2088`), reaching `addToRememberedSet` (`:1427`, fence read `:1434`) | `currentThreadClient()` is NULL for the C2 conductor: null-client executions append to the SERVER `m_mutatorMarkStack` (`:1479`) and read the SERVER fence master — sound in-window only (WSAC, single writer; consumed next cycle per CGA1 A21); debug assert in `addToRememberedSet`: null client => WSAC. Client-conductor (C0/C1): own-CMS appends, NEXT-CYCLE grey (SPEC-congc §5.2; ANNEX CGD4.5). |
| R10 | (rev 7, F38) `m_collectorThreadIsRunning` (`Heap.h:1325`; run-loop writes `Heap.cpp:337/:340/:357`; consumers `:2828`, `:4334`, `:4503`, `:5051`) | RULED: the R7 rewired loop KEEPS the landed discipline — flag set under `*m_threadLock` around each conducted ticket batch, cleared on return to the ticket wait. The `requestCollectionShared` `ASSERT(!m_collectorThreadIsRunning)` (`:4503`) is RETIRED at C2 behind `useSharedGCCollectorThread` (NAMED in body §7.2's retire list per the §7 master rule — a mid-cycle ticket against a conducting collector is licensed and COMMON, §9.1(4)/F28). The `pollIssRevertIfNeeded` conjunct (`:5051`) is KEPT: it is what makes a GIL-on §10D revert refuse while the collector conducts between granted tickets. `:2828` (`requestCollection`, `ASSERT(!isSharedServer())`) and `:4334` (§10B.4 pre-ISS migration quiescence) are !ISS/pre-ISS — out of scope, unchanged. CG-T6 R10 arm. |

## ANNEX CGS1 (BINDING) — supersessions to record at freeze

Recorded BOTH SIDES at freeze time (SPEC-congc rev N + the sibling
history file), per the supersession convention. Flag-gated:
each clause below is superseded only under the named stage flag;
flag-off keeps the frozen text operative (CG-I0).

1. SPEC-heap.md:23 (Deviation 4 disabled-feature list) — vs
   SPEC-congc §7 (C1: concurrent marking; C2: collector
   continuity + activity-callback collection; C3:
   mutator-concurrent sweeping; C4: incremental assist).
2. SPEC-heap §5.4 (`performIncrement` early-return when ISS;
   activity callbacks never fire when shared) — vs §7.2/§7.4.
3. SPEC-heap §10B(3) (collector thread quiesced once shared;
   stopTheMutator/resumeTheMutator unreachable) — PARTIAL: the
   quiescence clause vs §7.2; the unreachable clause is KEPT
   (CG-I7).
4. SPEC-heap §10B(7) (deviation-4 features disabled) — vs §7.
5. SPEC-heap §10 step 7 "full synchronous collection ... world
   suspended for the entire cycle" (conduct banner) — vs §3
   (window model); the step ORDERING per window is kept.
6. SPEC-heap I5 ("shared mode runs marking-start, stop/prepare
   iteration, conservative scan, constraint solving, sweep
   scheduling, precise-vector iteration only on the conductor ...
   while WSAC") — NARROWED, not removed: marking DRAIN moves
   out-of-window at C1 (helpers), sweep execution to the sweeper
   client at C3; the listed phase-control items stay WSAC-only
   (SPEC-congc §8.1).
7. SPEC-heap I11 "legal contexts: §10 step 7" — REINTERPRETED:
   "the cycle's final window" (SPEC-congc CG-I11); no semantic
   change to the I11 conditions themselves.
8. setMutatorShouldBeFenced ISS forcing (`Heap.cpp:3936-3937`,
   specced via heap §10B(5) "always-fenced once ISS") — vs
   SPEC-congc §5.3 under `useConcurrentSharedGCMarking`.
9. (rev 5, F27) SPEC-heap §10.2 election pseudocode + §10 step 9
   whole-conduct GCL tenure (SPEC-heap.md:254-257: `GCA = true;
   conduct(); GCL.unlock()`; SPEC-heap.md:274: "Conductor releases
   GCL, re-checks its ticket") — vs SPEC-congc §3.1/§3.2/§3.4
   (GCL per-window, released between windows, re-acquired at each
   WND-open; final close leaves GCL to the caller per F23; the
   election outer shape, follower waits, and GCL-busy rule are
   KEPT). Flag-gated on `useConcurrentSharedGCMarking`.
10. (rev 5, F27) SPEC-heap §10D step (1) pre-check wait structure
   (SPEC-heap.md:296: timed loop until ticket-quiescent ∧
   `m_currentPhase==NotRunning` ∧ GCL tryLock succeeds ∧
   size()==1) — vs SPEC-congc §3.4 `:5036` row / ANNEX CGD1.2 (no
   GCA wait under GCL when phase != NotRunning; the revert
   OUTCOME conditions are unchanged). Flag-gated on
   `useConcurrentSharedGCMarking`.
11. (rev 5, F26; amended rev 6, F29/F30) SPEC-heap §10A jit-R1.i
   exemption premise ("a JSThreads conductor inside its
   JSThreadsStopScope stopped window may WRITE heap memory
   without access (world stopped, GCL held)") — the "world
   stopped" premise is superseded at C3 by SPEC-congc §9.1(7):
   the standalone sweeper is excluded by the FLAG-KEYED,
   phase-independent pause/ack + quantum-entry gate (ANNEX CGP1
   sweeper extension), not the §A.3 fan-out — in-quantum sweeps
   are acked out AND fresh quantum entry is refused for the
   scope's whole tenure, including BETWEEN GC cycles. The
   exemption itself stands once the gate is in place. Flag-gated
   on `useSharedGCIncrementalSweep`.

SPEC-UNGIL SIDE (REWRITTEN rev 8, F42 — the rev-7 sentence "No
SPEC-ungil clause is superseded: §A.3/EXIT1/HBT4/ISB1 are
composed with unchanged" was FALSE for §A.3/HBT4/U32 and for the
§LK lock order): the SPEC-ungil-side PENDING supersessions live
in ANNEX CGS2 (§LK rows LK.9c/9d + U20 extension, F41; the
§9.1(2) JSThreadsStopScope ctor pause obligation + dtor order vs
§A.3 rule 5/HBT4.5; the conductor in-bracket wait bound vs
U32/HBT4.5 and the 30s watchdog; the HBT4 release-before-GCL
order extension to window re-entry), each SUPERSESSION-PENDING
behind a SPEC-congc §13.5 adoption gate, and CGS1+CGS2 fold into
SPEC-heap-history AND SPEC-ungil-history at freeze, both sides.
Still NOT superseded: EXIT1 (composed unchanged, §9.2); ISB1 —
the ISB1.1 bump cadence change (§3.2) is an extension in the
direction ISB1 already licenses (every window that may jettison
bumps). Rev 7 (F35)
note: the §8.3 per-full-cycle rebias placement also supersedes
NOTHING in ANNEX D1/D1R — it satisfies them VERBATIM: the Full
cycle's final window IS world-stopped inside that full shared
collection (heap §10, WSAC set); D1R item-1 fires complete
before THAT stop window resumes; the per-window order rebias ->
ISB1.1 bump -> GSP clear preserves D1R item 2's resume-side sync
(derivation ANNEX CGD5.1).

## ANNEX CGS2 (BINDING) — SPEC-ungil-side PENDING supersessions (rev 8, F41/F42)

Every row here is SUPERSESSION-PENDING and an ADOPTION GATE
(SPEC-congc §13.5): NOT in force until the SPEC-ungil owner lands
the cross-cite, recorded both sides; the gated stage MUST NOT
ship before that. At freeze this annex folds into
SPEC-ungil-history (CGS1 folds into SPEC-heap-history). All rows
flag-gated (`useConcurrentSharedGCMarking` unless noted);
flag-off keeps the frozen text operative (CG-I0).

CGS2.1 — §LK rows LK.9c/LK.9d (F41; gate §13.5(1)). Proposed
insertions in the SPEC-ungil §LK merged process lock table
(SPEC-ungil.md:867-925), heap-rank-internal additions, no
re-ranks:
- **LK.9c `GCH::m_mutatorMarkStackLock` (CMS lock)** — TERMINAL
  leaf: nothing of any rank acquired while holding it; ordered
  INSIDE `m_markingMutex` (drain/donation sites only); MAY be
  taken with heap ranks 7-9b held (the §5.2
  `addToRememberedSet` append path). This is the §LK.8
  destructor-leaf-class shape and, like §LK.8, supersedes heap
  §6's leaf-row "never 7-9b" for this lock — BOTH SIDES (heap §6
  + SPEC-ungil §LK.8 class list). Soundness: the holder appends
  to a segmented array (may fastMalloc a segment), acquires
  nothing, never waits.
- **LK.9d marking-internal group** (`m_markingMutex`,
  `m_parallelSlotVisitorLock`, `m_raceMarkStackLock`, visitor
  `m_rightToRun`) — ordered INSIDE GCL/GBL; mutually ordered as
  landed (markingMutex > CMS at drains; the others mutually
  unnested vs CMS); DISJOINT from MSPL-9b except landed in-window
  uses. NEW under SPEC-congc: mutator threads reach the group
  OUT-OF-WINDOW at exactly three sites — the §5.2(ii) SINFAC-tail
  CMS donation (`m_markingMutex`; access held, no 7-9b — SINFAC
  I6 `Heap.cpp:5125-5127`), the §9.2(1) DCT final flush
  (`m_markingMutex`; post permanent access drop), and the
  §9.2(1)/§9.3(3) ACT/DCT pending-list enqueue
  (`m_parallelSlotVisitorLock` only). U20 PROPER extends to BOTH
  rows: SPEC-congc CG-T2 IS the U20 extension — the rev-7
  "U20-class" private lint is retired (no second lock-order
  authority exists).

CGS2.2 — composed-chain walk: NL > GCL > `m_markingMutex` > CMS
(chartered; U20-linted via CGS2.1). Edges: NL > GCL — a
nativeaffinity BL1.6 §A.3 conductor (haveABadTime-class) MAY hold
NL on entry through its HBT4 bracket's GCL acquire; GCL >
`m_markingMutex` — the §9.1(2) stop-scope ctor calls
`pauseConcurrentMarkingForForeignStop` while holding GCL;
`m_markingMutex` > CMS — WND-open drain + SINFAC-tail donation
(§5.2). Acyclicity: CMS is TERMINAL (CG-I10(2)); GCL /
`m_markingMutex` holders never ACQUIRE NL (nativeaffinity NA-I10
negative edge); the barrier-append path takes CMS under 7-9b
WITHOUT `m_markingMutex` (CG-I10(1)); no reverse edge exists.
Note: under the rev-8 F40 ruling the GC-CONDUCT NL>GCL edge is
REMOVED (BL1.8 drop) — the chain survives only through the
BOUNDED §A.3 conduct. nativeaffinity LK.1c's "OUTER to ... all
leaves" was written before these leaves existed; it HOLDS (an NL
holder doing a barrier append takes only the CMS terminal leaf;
no reverse edge), and this row makes that consistency CHARTERED
and lint-enforced rather than accidental.

CGS2.3 — conductor in-bracket wait BUDGET vs the 30s watchdog
(`JSThreadsSafepoint.cpp:401`/`:412`); amends the frozen
U32/HBT4.5 stop-progress reading (gate §13.5(2)). Stated ONCE
here; SPEC-nativeaffinity BL1.6/BL1.8 cite this ledger rather
than restating it. A §A.3 conductor's GCL wait is bounded by the
sum of: (1) one GC window (CG-I12); (2) one marker-pause batch
(§9.1(2)/ANNEX CGP1 — donateAll + counter-leave, F16); (3) when
an F28 successor exists: the inter-cycle re-stop + the
successor's FIRST window (GCL next free at its first non-final
close); (4) C3: <= one sweeper quantum (§9.1(7)); (5)
nativeaffinity NL terms: ZERO — conductors never ACQUIRE NL
(NA-I10), the GC-conduct NL hold is removed (F40/BL1.8), and the
BL1.6 conductor-HOLD case adds nothing to a FOREIGN conductor's
wait. The frozen reading under which a conductor's GCL wait could
span one whole synchronous conduct is superseded by this windowed
bound; CG-T8 VERIFIES the sum against the watchdog. AMENDED
rev 9 (F45): the bound is STRUCTURAL only via the §9.1(2a)
fairness rule (ANNEX CGD7.2) — the landed §A.3 acquisition is
an unqueued 1ms tryLock poll (watchdog ctor `Heap.cpp:5568-5590`;
real-conductor use `VMManager.cpp:577`) with no queue position,
so absent §9.1(2a) the budget is probabilistic and the failure
mode is the `watchdogAssertStopProgress` fail-stop
(`JSThreadsSafepoint.cpp:512`; the `:401`/`:412` cites above are
pre-refresh — CGD7.4).

CGS2.4 — §A.3 rule-5/HBT4 amendments (gates §13.5(2)-(3)).
(a) `Heap::JSThreadsStopScope` ctor obligation (§9.1(2), F18):
after acquiring GCL, when `m_currentPhase != NotRunning`, the
ctor BLOCKS in `pauseConcurrentMarkingForForeignStop` (markers
only — the C3 sweeper gate is phase-independent, F29); the dtor
resumes BEFORE releasing GCL. This amends the frozen §A.3 rule
5/HBT4.5 characterization of the conductor bracket
(SPEC-ungil.md:256-268): the "closure stays ALLOCATION-FREE"
clause is STRUCK (rev 9, F43 — the IMPLEMENTED conductor
re-acquires its own client access and allocates in-window:
AB-21 `VMManager.cpp:631-646`, AB-10 weak-sweep license
`WeakSet.cpp:81/:106` + `Heap.cpp:3339`, ungil ANNEX HBT2.1
class-4 allocating body; conductor-as-client rules = SPEC-congc
§9.1(8)/ANNEX CGD7.1 — supersession recorded BOTH sides: this
pending row carries the strike for the ungil fold); the ctor is
also no longer non-blocking past the GCL acquire; the
added wait is bounded per CGS2.3 and acquires no api-rank or
heap >= 7 lock (CG-I16). (b) The HBT4 release-before-GCL order
(ungil §A.3.3) EXTENDS to window RE-ENTRY: the conductor's
per-window blocking GCL acquire is legal exactly because it is
access-released all tenure (§3.1(a)-(b)); first-window carve-out
F15 (tryLock access-held) unchanged. Election/poll stay
tryLock-only.

## ANNEX CGD1 (BINDING) — rev 3 interleavings and the ISS conductor-wait derivation

Referenced by SPEC-congc §3.4, §3.7, §9.2(3). The interleavings
are the normative reachability proofs for the rev 3 rules; the
test charters arm them.

### CGD1.1 — second conductor via the election winner arm (F10)

1. Conductor mid-cycle, BETWEEN windows: GCL free (§3.4), GCA
   true, `m_currentPhase == Concurrent`.
2. A mutator hits an allocation trigger and calls
   `requestCollectionShared` (legal between windows — it holds
   access, `Heap.cpp:4486-4491`): `m_lastGrantedTicket++ >
   m_lastServedTicket` (granted tickets are served only at the
   cycle-end ticket drain, `:4852-4863`).
3. It calls `runSharedGCElection(ticket)`: served < ticket; the
   `:4523` tryLock SUCCEEDS (GCL free); `:4526-4530` sees
   served < ticket, re-sets GCA, calls `conductSharedCollection`.
4. Two threads now run the phase machinery: the second conduct's
   entry asserts pass (GSP false, WSAC false between windows), it
   issues a nested `requestStopAll(GC)`, runs
   `collectInMutatorThread` against phase state owned by the
   parked first conductor; on exit it clears GCA (`:4536`) under
   the live conductor. Double endMarking/finalize on unwind.
   The rev 3 guard (GCA && phase != NotRunning => follower) makes
   step 3 fall to the `:4550-4554` wait instead.

Flag-off half (the truncated 10th finding): GCA-true-with-GCL-free
IS reachable today — election wind-down unlocks GCL (`:4534`)
before clearing GCA (`:4536`); same shape in the poll path
(`:4600-4604`). An unconditional GCA back-off would alter flag-off
behavior at that race (today a tryLock winner with unserved
tickets conducts). Phase-gating excludes it: in flag-off shared
mode, any point where GCL is free has `m_currentPhase ==
NotRunning` (the one window spans the whole cycle), so the guard
never fires flag-off.

### CGD1.2 — revert-poll deadlock (F11)

Config: GIL-on shared, C1 (§13.2 allows: stage flags gate only on
`useSharedGCHeap`).
1. Concurrent cycle live; conductor between windows (GCL free,
   GCA true).
2. A spawned thread EXIT1s mid-cycle (legal, §9.2); HCS shrinks
   to 1; `m_issRevertPending` armed.
3. Main client's next SINFAC poll (`Heap.cpp:5143-5151` region)
   enters `pollIssRevertIfNeeded`; the `:5036` tryLock SUCCEEDS;
   it enters `while (m_gcConductorActive)
   m_gcElectionCondition.waitFor(*m_threadLock, 1_ms)`
   (`:5040-5043`) — holding GCL AND heap access.
4. GCA cannot clear until the cycle ends; the cycle cannot end:
   the next WND-open blocks on GCL (held by the poller), and even
   the §10.4 barrier could never complete (the poller holds
   access). Every §A.3 stop also wedges on GCL. Permanent.
   Today this loop is sound only because GCA-true/GCL-free lasts
   the few wind-down instructions.

### CGD1.3 — why the ISS conductor must not enter the legacy drain arms (F13)

`drainInParallelPassively` (`SlotVisitor.cpp:718-735`) branches to
ACTIVE `drainInParallel()` when `numberOfGCMarkers()==1 ||
(m_worldState & mutatorWaitingBit) || !m_heap.hasHeapAccess() ||
worldIsStopped()`. Under ISS: `mutatorWaitingBit` is never set
(CG-I7, legacy bits dead); `Heap::hasHeapAccess()` forwards to
`mainClientHasHeapAccess()` (`Heap.h:405-412`) — when the
conductor IS the main client's thread (common C0/C1 case) it is
access-released all tenure, so the guard fires and the conductor
takes `drainInParallel()`: `donateAndDrain()` visits cells with NO
counter updates, then `drainFromShared(MainDrain)` enters the
counters and steals/drains on hasWork (`:683-690`, `:705`). When
the conductor is a spawned thread, the branch keys on the MAIN
client's unrelated access state — nondeterministic. Failure
modes vs §9.1(2):
- UAF: a foreign §A.3 stop's pause predicate closes while the
  conductor is mid-visitChildren inside donateAndDrain (in neither
  counter, exempted from checkpoints by rev 2) — the §A.3 window
  jettisons/patches under a live visitor.
- Deadlock: a MainDrain-parked conductor IS counted (waiting++ on
  entry, `:616-620`), but rev 2 gave it no checkpoint and
  exempted it — the ack equality is unsatisfiable; the §A.3
  conductor stalls to the 30s watchdog
  (`JSThreadsSafepoint.cpp:401`).
- hasWork-wakeup race: during a pause, shared stacks are non-empty
  exactly because paused helpers donated — a MainDrain conductor
  wakes on hasWork and drains concurrently with the §A.3 window.
`waitForTermination` (`:737-751`) has none of these: pure condvar
loop, no counters, no stealing; with `didReachTermination` gated
on `m_pausedParallelMarkers == 0` (CG-I22) it also cannot exit
mid-pause. Zero-helper case: a passive conductor with
`numberOfGCMarkers()==1` would wait forever (nobody drains), hence
the §3.7 rule that Concurrent is never scheduled there.

## ANNEX CGD2 (BINDING) — rev 4 counter-leak derivation and the F19 reader table

Referenced by SPEC-congc §9.1(2)(c), §5.3(3), CG-I22, CG-T3, CG-T8.

### CGD2.1 — F17: the waiting-counter leak and the CG-I0 benign ruling

Landed shape (`SlotVisitor::drainFromShared`,
`Source/JavaScriptCore/heap/SlotVisitor.cpp:607-710`): every loop
iteration takes `m_markingMutex` and does
`m_numberOfWaitingParallelMarkers++` (:621); the paired decrement
(:688) runs only when the thread re-enters ACTIVE (steal + drain).
The four return paths all exit COUNTED in waiting:
- MainDrain TimedOut (:626) — every in-window
  `drainInParallel(MainDrain)` slice that times out;
- MainDrain Done (:629-630) — every fixpoint-window drain that
  reaches termination;
- HelperDrain TimedOut (:641-642);
- HelperDrain Done on `m_parallelMarkersShouldExit` (:673-674) —
  taken by EVERY helper at EVERY cycle end (`runEndPhase` sets the
  flag and notifies, Heap.cpp:2026-2031; the helper task wrapper,
  Heap.cpp:1828-1847, just returns the visitor).
Writer/reader census (grep-complete over Source/JavaScriptCore):
writers = :621, :688, zero-init Heap.h:1261; readers = the
stealSomeCellsFrom denominator (:682), a diagnostic dataLog
(Heap.cpp:1867). `didReachTermination` (:594-598) reads ONLY
`m_numberOfActiveParallelMarkers` — hence the leak has no
observable effect tip-of-tree beyond steal granularity, and hence
no landed code ever needed the counter balanced.

Wedge interleaving (why rev 3 was a guaranteed deadlock, not a
race): (1) cycle 1, any fixpoint window — the conductor's
in-window MainDrain returns Done, waiting leaks +1; (2) cycle 1
end — every helper Done-returns, leaking +numberOfGCMarkers()-1;
(3) cycle 2 goes Concurrent; a sibling fires haveABadTime/jettison
-> `JSThreadsStopScope` ctor sees phase != NotRunning, calls the
§9.1(2) pause, waits for active==0 && waiting==0 — waiting > 0
forever with no live thread behind it. In the GIL-off conductor
the scope is constructed at VMManager.cpp:561 BEFORE
`requestStart` is sampled at :579, so the wait precedes watchdog
coverage (`watchdogAssertStopProgress`) — silent wedge with GCL
held, blocking every future WND-open and every other §A.3 stop.

CG-I0 disposition of the exit delta: adding waiting-- on the four
return paths changes, flag-off, ONLY the :682 steal denominator
(a work-partitioning hint) and the :1867 diagnostic value; it is
not observable in collection behavior, the bench gates, or any
assert. Ruled BENIGN-DELTA under CG-I0's inspection gate; the
implementing change cites this annex in INTEGRATE-congc.md. (The
alternative — gating the decrement on ISS — was rejected:
divergent counter semantics per mode is exactly the class of
latent trap F17 itself instantiates.)

### CGD2.2 — F19: emitted-code readers of the fence/threshold pair

The complete reader set of the barrier fence/threshold consulted
by EMITTED code (verified rev 4):
- Baseline/common JIT: `AssemblyHelpers.h:2045`, `:2052` bake
  `AbsoluteAddress(vm.heap.addressOfBarrierThreshold())`;
  `:2116` bakes `addressOfMutatorShouldBeFenced()`. `Heap.h:723`
  and `:726` return the addresses of the SERVER members
  `m_mutatorShouldBeFenced` (Heap.h:1204) / `m_barrierThreshold`
  (Heap.h:1209).
- `branchIfBarriered` (AssemblyHelpers.h:2056-2059) loads
  `VM::offsetOfHeapBarrierThreshold()` off a VM register — the
  same server member through the VM.
- FTL: `FTLLowerDFGToB3.cpp:27281` (VM_heap_barrierThreshold),
  `:27323`/`:27355` (VM_heap_mutatorShouldBeFenced) — server
  members via AbstractHeap offsets.
NONE of these read the §5.3(2) GCH client copies; the copies are
consulted only by the re-pointed C++ runtime readers (§5.3(3)).
Therefore a GIL-off fail-safe must pin the SERVER pair: keeping
the landed `setMutatorShouldBeFenced` forcing (Heap.cpp:3928-3940)
when GIL-off makes master AND copies tautological, so every
reader — baked server address, VM-offset load, or client copy —
remains fenced until the §13.3(a) per-client reroute lands. The
rev 3 copies-only pin left every JIT store under-fenced from the
first GIL-off endMarking lower onward.

## ANNEX CGP1 (BINDING) — §9.1(2) marker-pause mechanism + counter protocol (full text)

Relocated VERBATIM from the rev 4 body §9.1(2) at rev 5 (size
cap); BINDING per the frozen-spec convention. The body's §9.1(2)
summary and this text are one normative unit; on any perceived
divergence THIS text governs.

MECHANISM (NORMATIVE, rev 3): a NEW pair
`bool m_parallelMarkersShouldPause` + `unsigned
m_pausedParallelMarkers`, both guarded by `m_markingMutex`.
NOT reused: `m_parallelMarkersShouldExit` (one-shot
cycle-terminate, `Heap.cpp:2027`; `SlotVisitor.cpp:664, :673`)
and the `:2315-2342` rightToRun loop (history rev 2, F6).
PARTICIPANT SET (rev 3, F14): the pause pair covers EXACTLY the
helpers inside `drainFromShared(HelperDrain)` — the counters'
only maintainers (`SlotVisitor.cpp:620-621, :687-688`). The
conductor is in no counter and needs no checkpoint (§3.7; window
re-open blocks at the GCL acquire, §3.1(b), held by the foreign
scope); C4 assist visitors take NO checkpoint (§9.1 rule 6).
COUNTER PROTOCOL (rev 3, F16): pause = set ShouldPause +
`notifyAll` `m_markingConditionVariable`, then wait (same
mutex/condvar) until `m_numberOfActiveParallelMarkers == 0 &&
m_numberOfWaitingParallelMarkers == 0`. A pausing helper LEAVES
its counter: (a) the helper-wait `isReady` lambda
(`SlotVisitor.cpp:661-667`) gains `|| shouldPause`; a woken
waiting helper does waiting--, paused++, notifyAll, waits for
`!shouldPause`, then waiting++ and re-evaluates; (b) the helper
drain-batch safepoint (`SlotVisitor.cpp:522`, HelperDrain
visitors only) on shouldPause does `donateAll()` (a PAUSED
HELPER HOLDS NO LOCAL WORK), active--, paused++, notifyAll,
waits, then active++ — granularity = one drained batch (the
CG-I12 bound); (c) EXIT DELTA (rev 4, F17 — a NEW normative
edit; rev 3's "exited helpers leave both counters" was FALSE):
every `drainFromShared` return does waiting-- (TimedOut `:626`,
`:642`; Done `:630`, `:674`) — the landed exits ALL leak the
`:621` increment (sole writers `:621`/`:688`), leaving the
predicate permanently unsatisfiable once any helper exited
(every cycle end, `Heap.cpp:2027`). Flag-off delta = the `:682`
steal denominator only; benign-ruled under CG-I0 (derivation:
ANNEX CGD2.1). Debug assert: active==waiting==paused==0 after
`m_helperClient.finish()`. ShouldPause gates counter (re-)entry
INCLUDING a fresh helper's FIRST `:621` waiting++ (transient:
checkpoint (a) moves it to paused under the same mutex), so the
predicate is stable once reached. `didReachTermination`
(`SlotVisitor.cpp:594-598`) additionally requires
`m_pausedParallelMarkers == 0` — false termination becomes
structurally impossible and waitForTermination stays parked
across the foreign stop (CG-I22). No lost wakeup: every
flag/count write and wait shares `m_markingMutex`. Resume:
clear flag + `notifyAll`. Helpers hold no lock a §A.3 window
needs, so the pause terminates (CG-I16).

SWEEPER EXTENSION (rev 5, F26; REWRITTEN rev 6, F29/F30). The
rev 5 text — "the ctor [the phase-gated §9.1(2) pause]
additionally sets a sweeper-pause flag ...; the C3 sweeper polls
it at each quantum boundary; on set it finishes/aborts the
quantum, drops MSPL, RHA-releases access, acks and parks ...;
the ctor's wait predicate gains the sweeper ack (when the C3
flag is on and a sweeper client is registered); bound: one
quantum" — is SUPERSEDED in full: it rode the phase-gated pause
although sweeping runs between cycles (F29), wedged on an idle
sweeper whose ack never comes (F30(1)), and did not gate quantum
ENTRY (F30(2)); derivations ANNEX CGD4.2 + the rev 6 log.
NORMATIVE, current:

- Gate: when `useSharedGCIncrementalSweep` is on AND a sweeper
  client is registered, the `JSThreadsStopScope` ctor sets
  `m_sweeperShouldPause` under `m_markingMutex` UNCONDITIONALLY —
  no `m_currentPhase` condition (the marker pause keeps its
  phase gate; the two are independent).
- Entry gating (F30(2)): the sweeper sets
  `m_sweeperInQuantum = true` under `m_markingMutex` at quantum
  ENTRY, and the entry is REFUSED while `m_sweeperShouldPause`
  is set (the flag gates quantum entry exactly as ShouldPause
  gates marker counter (re-)entry); a refused sweeper re-arms
  its timer or parks on `m_markingConditionVariable`.
  Access/MSPL acquisition happens only AFTER a successful gated
  entry; the bit clears under the same mutex at quantum exit.
- In-quantum pause: the sweeper polls the flag at each quantum
  boundary; on set it finishes/aborts the current quantum, drops
  MSPL, RHA-releases access, clears `m_sweeperInQuantum`, sets
  `m_sweeperAcked` (same mutex), notifyAll, and parks until the
  flag clears.
- Ctor wait predicate (F30(1)):
  `!m_sweeperInQuantum || m_sweeperAcked` — an idle sweeper
  (timer-parked, between quanta or between sweep lists)
  satisfies it IMMEDIATELY with no ack; no quantum boundary is
  required of an idle sweeper.
- Ack lifetime: the dtor's resume clears `m_sweeperShouldPause`
  AND `m_sweeperAcked` together, BEFORE releasing GCL (same
  clause as the marker resume); no ack survives a scope.
- Bound: one sweep quantum when in-quantum at ctor time; ZERO
  otherwise. No lost wakeup: all four states share
  `m_markingMutex`/`m_markingConditionVariable`.

## ANNEX CGD3 (BINDING) — rev 5 interleavings and derivations

Referenced by SPEC-congc §3.4 (F20), §9.1(7) (F26), §9.2(1)
(F25). Normative reachability proofs; CG-T6/T7/T8/T9 arm them.

### CGD3.1 — F20: the predecessor's stale GCA clear vs a successor cycle

Flags-on (`useConcurrentSharedGCMarking`):
1. Cycle-1 conductor T1 completes the final WND-close (phase ->
   NotRunning, GCL held per F23), returns from
   `conductSharedCollection`, executes the caller unlock
   (`Heap.cpp:4533` or `:4600`), and is descheduled BEFORE its
   deferred clear block (`:4535-4538` / `:4601-4604`).
2. Requester T2 (new granted ticket) tryLocks GCL at `:4523` —
   succeeds; the §3.4 guard is FALSE (GCA true but phase ==
   NotRunning — the designed wind-down pass-through, CGD1.1
   flag-off half); T2 re-sets GCA (already true), restamps
   `m_gcConductorThread = T2`, conducts cycle 2.
3. Cycle 2 reaches Concurrent and releases GCL between windows
   (§3.4 steady state: GCL free, GCA true, phase Concurrent).
4. T1 resumes and executes `m_gcConductorActive = false` + clears
   `m_gcConductorThread` + notifyAll — MID-CYCLE-2.
Consequences without the fix: every §3.4 tryLock site now sees
`GCA == false && phase == Concurrent` => guard false => a third
requester T3 between cycle-2 windows wins tryLock and NESTS a
conductor against the live cycle (the ANNEX CGD1.1 interleaving:
nested requestStopAll, double endMarking/finalize); cycle-2
followers parked on `served < ticket && GCA` (`:4550-4554`,
§9.1(3)) wake spuriously and re-contend; the nulled
`m_gcConductorThread` breaks the §3.4 FOREIGN discrimination, the
§9.2(4) EXIT1 release-assert, and CG-I21's premise (clearing is
not stamping — the "stamped once" assert does not catch this).
FIX: ownership-checked clear (body §3.4). T1's clear compares
`m_gcConductorThread == &Thread::current()` — T2 restamped, so
T1 skips the clear (notifyAll still runs); T2's own clear at its
wind-down matches and clears.
Rejected alternative: moving the clear inside the final WND-close
before the GCL release eliminates the wind-down state entirely —
but that state IS reachable flag-off today (`:4534-4537`), so
removing it is a larger CG-I0 surface than gating the clear; it
also still needs the ownership check for the C2 R7 loop unless
that loop is restructured too.
CG-I0 disposition (flag-off delta of the ownership check): the
stale-clear race EXISTS today flag-off (T1 descheduled between
`:4533` and `:4536`, T2 conducts under whole-cycle GCL, T1's
clear lands mid-T2-cycle). Today's effect: followers in the
untimed GCA wait wake, re-loop, and fall to the timed 1ms branch
(GCA false) — correct but degraded; GCA is not otherwise
load-bearing mid-cycle flag-off (WSAC covers the `:1683` assert;
GCL is held whole-cycle). With the fix: GCA stays true through
T2's cycle, followers stay in the untimed wait, woken by T2's
serve/clear notifyAll — no lost wakeup (same condvar), no
behavior change observable in collection results, bench gates, or
asserts. Ruled BENIGN-DELTA under CG-I0's inspection gate (the
CGD2.1 precedent class); the implementing change cites this annex
in INTEGRATE-congc.md.

### CGD3.2 — F26: sweeper invisibility to §A.3 (reachability)

Config: C3 flag on, GIL-off or GIL-on shared. The §7.3(2) sweeper
is a STANDALONE client (`markStandalone()`+ACT): no VM, no lite,
never "entered". The §A.3 quiescence predicate is a
VMLiteRegistry walk over ENTERED threads
(SPEC-ungil §A.3 rules 1-2: "parked/not-entered/access-released"
per EXIT1 sampling) — the sweeper never appears in it; §A.3 stops
set no client-visible heap state (ungil §A.3.8, §9.1(5)), so the
sweeper's F8/GSP polls see nothing either; the §9.1(2) pause
participant set is pinned to HelperDrain helpers (F14). Hence a
JSThreadsStopScope window (haveABadTime heap iteration, jettison
CodeBlock walks/patching, heap writes under the §10A jit-R1.i
exemption) runs concurrently with a live sweep quantum mutating
the same heap (destructor execution, freelist/newlyAllocated
writes, directory-bit writes). The §9.1(7) ack closes it: the
scope ctor (the ONLY pause site, F18) gains the sweeper flag/ack
(ANNEX CGP1 sweeper extension); CGS1 row 11 records the premise
supersession. Option (b) (sweeper brackets each quantum with
GCL-subordinate state so the ctor's GCL hold excludes quanta) was
REJECTED: it puts a GCL-shaped acquisition on the sweep hot path
and serializes sweeping against GC windows (the sweeper must run
BETWEEN windows; GCL is held in-window — wrong exclusion).

### CGD3.3 — F25: why mid-cycle EXIT1 had no legal visitor path

For a C4 client exiting BETWEEN windows (legal per §9.2): (a)
unregister at DCT => forEachSlotVisitor mutation outside
{in-window, between cycles} — CG-I14 violation racing the
`Heap.cpp:2315-2342` in-window walks; (b) destroy without
unregistering => CG-I17 violation + the next WND-open/close walk
dereferences freed storage (UAF); (c) block DCT until the next
window => unspecified, and the exiting thread has already
permanently dropped access and takes no checkpoint — unbounded
and deadlock-prone vs a cycle whose next window may wait on HCS
state. The deferral mirror of §9.3(3) is the unique remaining
shape; ownership transfer (heap-allocated visitor) is what lets
GCH destruction proceed before the next WND-open applies the
unregistration. The cancellation rule closes the
attach-then-exit-in-one-gap leak (pending registration for a
destroyed GCH). CG-T9 arms both shapes.

## ANNEX CGD4 (BINDING) — rev 6 rulings and full texts

Referenced by SPEC-congc §3.1/§3.2/§3.4 (F28), §9.1(7)
(F29/F30 — see also the CGP1 rewrite), §9.2(1)/§9.3(3) (F34),
§5.2 (F31/F33). CG-T7/T8/T9 + CG-T1 arm them.

### CGD4.1 — F28: the ticket-drain successor arm (full text)

Reachability: the landed conduct body's step-7 loop checks
`m_requests` under `*m_threadLock` and re-runs
`collectInMutatorThread()` until empty (the ticket drain,
`Heap.cpp:4852-4863` region). Under C1, any §A.3 stop (§9.1(4):
RCAC enqueue) or allocation trigger landing between windows
leaves a granted-unserved ticket, so a SECOND full cycle inside
the same conduct call is the common case. At that point, per F23,
the predecessor cycle's final close (->NotRunning) has left GCL
HELD; the world is resumed; GSP/WSAC are clear; the conductor is
access-released; GCA + `m_gcConductorThread` remain stamped.
Neither rev 5 §3.1 arm could open the successor's first window
(F15 arm: no landed election entry exists; RE-ENTRY arm: blocking
acquire of the already-held non-recursive GCL = self-deadlock),
and no clause specified the successor's re-stop (the landed
GSP/requestStopAll/GBL sequence runs once, above the loop).

NORMATIVE (the §3.1 third arm): when the conduct loop observes
`m_requests` non-empty and begins a successor cycle,
- GCL ownership TRANSFERS: the predecessor's F23 final close
  leaves GCL held; the successor's first WND-open performs NO
  GCL acquisition and consists of §3.1 steps (c)-(e) only —
  seq_cst GSP=true, `requestStopAll(GC)`, GBL barrier + WSAC,
  per-client flush. The conductor stays access-released; GCA and
  the owner stamp persist (one tenure spans the whole conduct
  call).
- The inter-cycle state {GCL HELD, GCA set, phase NotRunning,
  world running, unserved tickets} needs no §3.4 guard: every
  foreign tryLock site FAILS on the held GCL (election winner
  falls to the `:4550-4554` follower wait; the poll returns
  false; CGA2 R7 re-waits). The state is bounded by the
  m_requests check (the conductor runs no JS between cycles,
  §3.7).
- Flags-on only (`useConcurrentSharedGCMarking`): flag-off the
  drain loop runs INSIDE the single stop window (the landed
  shape — GSP/barrier once, resume once), and the F28 arm is
  dead code (CG-I0 byte-for-byte).
- REJECTED alternative — releasing GCL between cycles and
  re-entering via the blocking RE-ENTRY arm: in the resulting
  {GCL free, GCA set, phase NotRunning, unserved tickets} state
  the §3.4 guard's NotRunning pass-through (designed for F20
  wind-down takeover) admits a foreign election winner that
  restamps `m_gcConductorThread` and conducts the remaining
  tickets CONCURRENTLY with the original conductor's queued
  blocking acquire — two threads in the conduct machinery with
  trampled owner identity (breaks the §9.2(4) release-assert and
  CG-I21's stamped-once premise). Distinguishing "wind-down
  takeover" from "drain gap" would need a new discriminator;
  retaining GCL needs none.
- CG-I12 consequence (accepted, latency-only): a §A.3
  conductor's GCL wait worst case extends from one window + one
  marker-pause to + the inter-cycle re-stop + the successor's
  FIRST window — GCL is next free at the successor's first
  non-final close (or its caller release if the successor is
  single-window). CG-T8's F28 sub-arm measures it against the
  30s watchdog margin.

### CGD4.2 — F29/F30: sweeper pause gate, idle liveness, entry race

F29 reachability: incremental sweeping is by construction a
between-cycles activity — `notifyIncrementalSweeper()` fires at
cycle end (`Heap.cpp:2083`, end phase, after
deleteUnmarkedCompiledCode), `startSweeping` schedules the timer
(`IncrementalSweeper.cpp:152`, `scheduleTimer` `:41`), and quanta
run in timer slices while mutators run, i.e. while
`m_currentPhase == NotRunning` (CGD3.2 itself: the sweeper "must
run BETWEEN windows"). The rev 5 ack rode the §9.1(2) ctor pause,
which fires only when `m_currentPhase != NotRunning` — so the
common-case §A.3 stop (between cycles) set no sweeper flag and
awaited no ack: the CGD3.2 race shipped everywhere except the
narrow mid-cycle case, CG-I13's ack clause was unsatisfiable as
specified, and CGS1 row 11's premise was false between cycles.
The phase gate is correct for the MARKER pause (HelperDrain
helpers exist only while a cycle is live) and stays.

F30(1) idle liveness: between quanta the sweeper is parked on
its scheduler with no access and no MSPL; once the sweep list is
exhausted it schedules nothing until the next cycle. An
ack-bit-only predicate ("when ... a sweeper client is
registered") therefore waits on a thread that will never reach a
quantum boundary — with C3 on, every §A.3 stop taken while the
sweeper is idle (the majority state) wedges to the 30s watchdog
(`JSThreadsSafepoint.cpp:401`), and via the `VMManager.cpp:561`
conductor (scope constructed BEFORE `requestStart` is sampled,
`:580` — the CGD2.1 shape) it is a SILENT process hang with GCL
held. Same guaranteed-liveness-failure class as F17, in the
mechanism F26 added.

F30(2) entry race: the rev 5 text governed only a CURRENT
quantum; the sweeper is invisible to the §A.3 fan-out (no lite —
F26's premise) and §A.3 stops set no client-visible heap state
(§9.1(5)), so nothing stopped a timer-woken sweeper from
acquiring access + MSPL and STARTING a fresh quantum inside an
open §A.3 window — freelist/newlyAllocated/directory-bit
mutation concurrent with the window's heap writes, the CGD3.2
race re-opened through the entry edge. There was no
happens-before from the ctor's flag store to an entry-side poll
without requiring the entry check under `m_markingMutex` — the
sweeper analog of CGP1's "ShouldPause gates counter (re-)entry"
clause, which the rev 5 extension lacked.

FIX: the ANNEX CGP1 SWEEPER EXTENSION rewrite (two-bit state
machine, flag-keyed not phase-keyed, gated entry, idle-pass
predicate, dtor-owned ack lifetime, derived bound). CG-T7's
three shapes pin: between-cycles mid-quantum (phase
NotRunning), fully idle (no stall), delayed timer into an open
window (entry refused).

### CGD4.3 — F34: no raced phase read in the visitor lifecycle

Defect: rev 5's two deferral discriminators — §9.3(3) "ACT while
m_currentPhase != NotRunning DEFERS" and §9.2(1) "between cycles
DCT (un)registers directly" — were phase reads by threads holding
neither GCL nor `*m_threadLock`, absent from F22's enumerated
GCL-ordered reader set (an EXIT1 thread has permanently dropped
access and never conducts, CG-I18/§3.7; the ATTACH side's GBL
section orders nothing against the GCL-ordered phase store).
Consequences: (1) the bare load vs `finishChangingPhase`'s
in-window store (`Heap.cpp:2213`) is an unsuppressed TSAN report
— a §12 ladder blocker; (2) a stale-NotRunning read at a cycle
edge lets ACT/DCT mutate `forEachSlotVisitor` DIRECTLY against a
live cycle's first WND-open — racing the in-window walks
(`Heap.cpp:2315-2342`), the CGD3.3(a) skip/UAF shape. (The
stale-Concurrent direction is benign over-deferral.)

NORMATIVE: the predicate is CLOSED, not synchronized — ACT and
DCT never read the phase and never mutate the visitor set.
Registration and unregistration ALWAYS enqueue to the pending
list under `m_parallelSlotVisitorLock` (exit cancels a pending
registration; ownership transfers per F25). The pending list is
applied, under `m_parallelSlotVisitorLock`, at exactly two kinds
of point: (a) every WND-open, by the conductor, BEFORE any
`forEachSlotVisitor` walk; (b) quiesced application by a thread
holding GCL with `m_currentPhase == NotRunning` verified under
`*m_threadLock` (the §10D revert path and server teardown drain
the remainder — both already hold that guard shape per the §3.4
`:5036` row and heap §10D step 1). A pending registration before
application only delays assist eligibility
(`performIncrement` checks registration); a pending
unregistration only delays storage reclamation (owned by the
list). F22's reader enumeration is unchanged — the rewrite
ADDS no phase reader. CG-T9's F34 arm fires ACT/DCT
amplifier-descheduled across the NotRunning -> first-WND-open
edge.

### CGD4.4 — F33: the C1R routing predicate (flag-off ruling)

Defect: §5.2 (CMS reroute), §5.3(3) (fence/threshold consumer
re-point), and §4.1 (didRun fold) were conditioned on ISS and
landed at C0 (CG-2) — flag-off shared mode would stop executing
the landed lock-free append (`Heap.cpp:1479`) and the landed
server reads (`:1434`, `:3324`), taking a per-slow-path lock
instead: a protocol delta AND a plausible bench delta against
the master rule's BYTE-FOR-BYTE bar and the CG-T1 gate, with no
benign ruling (the convention every other flag-off delta
followed: F17 -> CGD2.1, F20 -> CGD3.1).

RULING (option (b); option (a) — benign-rule the reroutes and
soften the master rule to "protocol-equivalent" — REJECTED: it
weakens the spec's strongest gate to save a predicate): all
three routings are gated on C1R := ISS &&
`useConcurrentSharedGCMarking`. Flag-off (= ISS && !flag): the
C0 change may LAND the GCH fields and helper code, but
`addToRememberedSet` keeps `:1479`, the fence/threshold
consumers keep the server pair, and didRun keeps the landed
shared-mode behavior — byte-for-byte. The C1R-conditional
clauses get explicit flag-off arms: the §9.2(1) exit CMS-flush
step (AMENDED rev 7, F36: the dead-publication step this
sentence also named is DELETED — see the rev 7 log) and the
§9.3(1) attach
fence-snapshot/FEP-stamp are NO-OPS when !C1R (the copies are
unrouted, unread state; CG-I0). The §5.2 WND-open drain and
§5.3(2) republish loops iterate empty/unrouted state flag-off
and are vacuous by construction. CG-T1 verifies: flags-off
corpus byte-identical AND BENCH.md flags-off delta = 0 with the
C0 infra landed.

### CGD4.5 — F31: conductor-context barrier disposition (full text)

The conduct path itself executes write barriers in-window:
runEndPhase's `iterateExecutingAndCompilingCodeBlocks ->
writeBarrier(codeBlock)` (`Heap.cpp:2036-2039`) and the
finalize-side `m_codeBlocks->iterateCurrentlyExecuting ->
writeBarrier` (`:2085-2088`), reaching `addToRememberedSet`
(`:1427`; fence read `m_mutatorShouldBeFenced` `:1434`). Under
§5.2/§5.3(3) as rev 5 wrote them these route via
`currentThreadClient()` — NULL for the §7.2 C2 standalone
conductor (not a client): both the append routing and the fence
read were undefined (null deref as written). For a C0/C1
client-conductor the appends land in its OWN CMS after that
window's WND-open drain already ran — their fate was unstated.
Neither ANNEX CGA2's "conductorClient/vm() use" pattern nor
CGA1's grep set (the call sites name only `writeBarrier`, an
inline wrapper) caught these sites — both audit patterns are
extended (CGA2 charter; CGA1 A21).

NORMATIVE: (a) NULL-CLIENT (conductor-context) barrier
executions append to the SERVER `m_mutatorMarkStack` (`:1479`)
and read the SERVER fence master/threshold. Soundness: they
occur ONLY in-window (WSAC set — single writer, no concurrent
client appends to the server stack; the server fence pair is the
master §5.3(1) already mutates in-window). Debug assert in
`addToRememberedSet`: `currentThreadClient() == nullptr` implies
WSAC. (b) A CLIENT-conductor's in-window appends go to its own
CMS and are NEXT-CYCLE GREY: drained at the NEXT cycle's
WND-open (§5.2 drain (i)). This matches the landed semantics —
End-phase `m_mutatorMarkStack` appends are deliberately left for
the next cycle's merging constraint
(MarkStackMergingConstraint.cpp:64-68 comment; CGA1 A21) — and
GC correctness never depended on same-cycle consumption of
end-phase barrier appends. (c) CG-I2 carries the matching
exemption (in-window conductor-context appends are not
owner-access-thread appends; WSAC single-writer covers them).

## ANNEX CGD5 (BINDING) — rev 7 full texts

Referenced by SPEC-congc §8.3 (F35) and §9.2(5) (F39); both
GOVERN over their body summaries. CG-T6/T8 arm them.

### CGD5.1 — F35: TID-rebias window placement (full text)

Defect/reachability: the landed conduct-tail rebias block
(`Heap.cpp:4877-4915`; conductor function banner `:4615-4690`;
`sawFullCollectionThisStop` aggregation `:4856-4868`) is the
ONLY tail step the window split gave no disposition (rev 7 log).
Left at its landed position it runs, under
`useConcurrentSharedGCMarking`, AFTER the final WND-close's
resume steps — restamping live Structures and firing D1R
watchpoints with mutators running, no covering ISB edge, and a
gate-lift flip that no longer happens-before resume (dead-TID
reissue vs live baked `tid<<48` tags).

NORMATIVE (per-CYCLE rule; option (i) of the review's two —
option (ii), once-per-conduct in the LAST cycle's final window
with the OR'd predicate, REJECTED: it makes a trailing Eden
cycle's final window run Full-collection-scoped work, has no
landed analog for "which window", and delays the restamp across
mutator-visible gaps for no benefit):
1. When gilOffProcess (the block is gilOff-only; flag-off/GIL-on
   it is dead — landed `[[unlikely]]` guard), the §D.1/D1R
   rebias executes inside the FINAL window of a conducted FULL
   cycle: after that cycle's `runSafepointHooksAndReclaim`
   (§8.1/CG-I11 pin it to the same window), strictly BEFORE that
   window's CG-F4 ISB bump, WSAC clear and GSP clear — i.e.
   under WSAC, the landed per-window order preserved. The rebias
   NEVER executes after any WND-close.
2. Per-cycle predicate: `sawFullCollectionThisStop`'s
   per-conduct aggregation is replaced when C1R by a per-cycle
   derivation — THIS cycle's `m_lastCollectionScope ==
   CollectionScope::Full`. Within an F28 multi-cycle conduct the
   rebias runs in the FIRST Full cycle whose snapshot is Sealed;
   `noteRebiasRestampComplete` consumes the snapshot, so later
   cycles of the same conduct find none (single-shot per
   snapshot). An Eden cycle — predecessor or F28 successor —
   neither runs nor suppresses it. Flag-off keeps the landed
   aggregate + post-loop position (one window per conduct makes
   them identical positions — CG-I0 byte-for-byte).
3. Sealed-stays-Sealed safety (the mid-stop seal carve-out,
   `Heap.cpp:4908-4913`, generalized to between-windows AND
   between-cycles carrier exits — the NORMAL case under the
   window model): a snapshot sealed after the running Full
   cycle's rebias point stays Sealed; mutators between
   windows/cycles observe Sealed, and the post-resume
   `m_freeTIDs` release (ThreadManager phase 3, the SD9
   gate-lift) is licensed ONLY by the Sealed->Restamped flip —
   so no dead TID is reissuable until a later Full cycle's
   final-window rebias runs and that window's F8 resume edge
   publishes the flip. The landed `shouldDoFullCollection` probe
   (`Heap.cpp:3219-3232`) arms the next conducted cycle as Full
   unchanged.
4. D1/D1R conformance: no supersession (ANNEX CGS1 closing
   note). D1's "world-stopped INSIDE the next FULL shared
   collection (heap §10)" — the Full cycle's final window is
   exactly that; D1R item 1 ("fire ... BEFORE the stop
   resumes") — fires complete before THAT window's resume, which
   strictly precedes any mutator's post-resume reissue path;
   D1R item 2's resume-side sync (ISB1.1 bump after the rebias,
   before the GSP clear) is the rule-1 per-window order.
5. Audit closures: CGA1 grep set extended (the four rebias
   symbols; matches disposition to §8.3); CGA2 R3 amended (the
   "TID-rebias teardown" mislabel removed; R3 inherits this rule
   for the C2 conductor); CG-I23; CG-T8 F35 sub-arm (GIL-off C1:
   thread-exit-armed rebias snapshot + forced multi-window Full
   cycle + F28 Eden successor — assert the flip and all fires
   happen with WSAC set, before the final window's resume, and
   that no TID reissue precedes the flip's publication).

### CGD5.2 — F39: server/Heap teardown vs a live conducted cycle (full text)

Defect: see the rev 7 log entry (the §9.2(3) §10D argument is
disarmed under gilOffProcess, `Heap.cpp:5023-5031`; the C2
collector conductor has no lite, so the EXIT1.9 fence does not
order ~VM after its cycles; CG-I14/CGD4.3 presupposed a "server
teardown" quiesce point no clause stated; teardown was not a
§3.4 tryLock site, so a bare GCL grab could land mid-cycle).

NORMATIVE (§9.2(5) — ordered teardown-of-conducting):
1. After the EXIT1.9 completion fence and BEFORE any
   access-holding teardown section (`lastChanceToFinalize`'s
   MSPL sections), ~VM/Heap teardown DISABLES conducting: no new
   elections; collector-thread wakes stopped
   (`m_threadShouldStop`-class signal); activity callbacks
   disarmed.
2. It then acquires GCL with the §3.4 back-off discipline
   (tryLock; on `GCA && m_currentPhase != NotRunning` unlock +
   bounded re-wait — teardown is hereby ADDED to the §3.4 site
   enumeration) until: GCL held AND phase == NotRunning under
   `*m_threadLock` AND granted tickets are quiesced —
   served by the last conduct, or explicitly COMPLETED-AS-REFUSED
   under `*m_threadLock` (no follower is left parked on a ticket
   nobody will serve; the notify uses the election condvar).
3. It joins the C2 collector thread AT that boundary (the R7
   loop holds neither GCL nor a ticket there and observes the
   stop signal at its next wait). The join therefore cannot
   deadlock against a WND-open barrier: no window can open (GCL
   held by teardown), and teardown holds no access yet.
4. Only then does the access-holding teardown tail run; no
   WND-open can interleave with it (GCL still held).
5. At that point teardown IS the CG-I14 quiesced application
   point (GCL held + NotRunning under `*m_threadLock` — the
   CGD4.3(b) presupposition closed); it applies the pending
   visitor list before destroying any visitor storage.
6. Configs: !C2 has no collector conductor — steps 1-3 reduce
   to the §3.4-disciplined GCL acquire (still required at C1:
   it is what orders teardown after a live client-conducted
   cycle); flag-off the acquire always succeeds with phase
   NotRunning at the landed teardown point — byte-for-byte
   (CG-I0). CG-I24; CG-T6 teardown arm (GIL-on and GIL-off).

## ANNEX CGT1 (BINDING) — test charters, FULL text (MOVED here rev 8 under the size cap)

Content normative; CGT1.1-1.4 carried VERBATIM from the rev-7
body §12 entries CG-T8/T9/T6/T7 (no semantic change except the
NEW F40 sub-arm in CGT1.1, marked); the body entries are the
indexes.

### CGT1.1 — CG-T8 (all stages): JSThreads-stop interleaving

JSThreads-stop interleaving — haveABadTime/jettison
mid-concurrent-cycle from a sibling, incl. at the WND-open GCL
acquire (§3.1), vs a between-windows parked conductor (§3.7),
and with helpers mid-batch (CG-I22); F17 sub-arm: stop injected
into a SECOND cycle; F18 sub-arm: >=1 jettison arm drives the
GIL-off `VMManager.cpp:561` conductor; F20 sub-arm: conduct
return descheduled between GCL unlock and GCA clear +
second-cycle between-windows requester storm; F22 sub-arm:
stop-scope ctor just after a non-final WND-close; F28 sub-arm:
§A.3 stop vs a forced back-to-back drain cycle — no deadlock at
the successor's WND-open, CG-I12 wait measured; GC-requester
storm (sync election + poll) vs a between-windows cycle — one
`m_gcConductorThread` per cycle (CG-I21); GCL wait vs the 30s
watchdog measured (rev 8: against the CGS2.3 budget ledger);
F35 sub-arm: GIL-off C1, thread-exit-armed rebias snapshot +
forced multi-window Full cycle + F28 Eden successor — flip/fires
under WSAC before the final window's resume, no TID reissue
before publication; NEW rev 8 — F40 sub-arm (composes with
nativeaffinity NA-T4's rev-8 multi-window arm): a Locked native
holding NL on an NL-eligible lite calls a gc()-style sync
collection that conducts a forced multi-window C1 cycle — the
BL1.8 drop scope releases NL BEFORE conducting entry (CG-I19
depth==0 debug assert does not fire), sibling threads' Locked
natives PROGRESS between windows of the cycle (witnessed), NL is
reacquired depth-restored after the `:4955` tail before the
native body resumes; follower variant: the requester loses the
election and parks on its ticket — same drop/reacquire bracket;
control build with the drop scope disabled shows the
cycle-length stall (latency witness, no deadlock);
`jsThreadsStopVsGCRequester` (heap §12.1) re-run per stage.

### CGT1.2 — CG-T9 (all stages): ATTACH/EXIT1 churn

ATTACH/EXIT1 churn during forced concurrent cycles
(CG-I17/I20); attach storm with the §6.2(5) liveness assert
(§9.3(5)); exit with finalizer-side stores during full marking
(§9.2(1)); F36 arm: amplifier-descheduled EXIT1 parked between
the CMS flush and the GBL acquire across a fence-raising
window (CG-I3 assert + TSAN on); conductor-thread exit attempt
(release-assert, §9.2(4)); spawn+exit arming
`m_issRevertPending` mid-cycle + main-client SINFAC poll storm —
assert cycle completion (F11); attach-then-exit inside one
between-windows gap (F25); ACT/DCT amplifier-descheduled across
the NotRunning -> first-WND-open edge (F34); `clientChurnVsGC` +
`issRevertChurn` re-run.

### CGT1.3 — CG-T6 (C2): collector-continuity churn

Collector-continuity churn — RCAC storms, zero mutator polls;
activity-callback collections fire; SINFAC fallback conducts
when the collector thread is disabled; R7 guard arm (F24):
collector wake on an RCAC ticket between windows of a forced C1
cycle; assert one conductor; R10 arm (F38): mid-cycle sync
requester + RCAC storm vs a conducting collector thread (debug);
F39 arm: ~VM vs an RCAC C2 cycle between windows (GIL-on/off) —
completion or refusal, no watchdog, no conduct-state UAF.

### CGT1.4 — CG-T7 (C3): sweep vs windows

T8-extension audit (ANNEX CGB1) executed; sweep quantum vs
window race arm; sweeper-thread attach/detach churn; F26/F29/F30
arms (C3 on): §A.3 jettison/haveABadTime (a) mid-quantum, phase
PINNED NotRunning (between cycles, dominant; ack <= one
quantum); (b) sweeper fully IDLE — no watchdog stall; (c)
sweeper timer amplifier-delayed into the open §A.3 window —
quantum entry refused.

### CGT1.5 — CG-T3 (C1): barrier storm (MOVED rev 9; VERBATIM rev-8 body text, cites refreshed)

CG-T3 (C1): barrier storm — N threads store-heavy during forced
concurrent marking; TSAN no-JIT (TSAN.md target) over the
corpus; amplifier arms (AMPLIFIER.md hooks) at: WND-open barrier
entry, CMS donate, fence republish, `m_isMarking` resume edge,
steal-vs-mark. F19 sub-arm: GIL-off C1, two full cycles, then a
fence storm asserting the SERVER pair stays tautological.

### CGT1.6 — CG-T4 (C1): allocation/steal storm (MOVED rev 9, verbatim)

CG-T4 (C1): allocation/steal storm during marking
(`allocationStorm`+marking variant); endMarking liveness debug
assert (§6.2(5)) enabled.

### CGT1.7 — CG-T5 (C1): cell-lock audit arms (MOVED rev 9, verbatim)

CG-T5 (C1): CG-A2 cell-lock audit executed (ANNEX CGN1 rows
complete); per-row arm for every CELL-LOCKED and N-PROTOCOL row;
CG-I18 storm (map/set mutation vs forced fixpoint windows) with
the cell-lock-depth debug asserts enabled.

### CGT1.8 — CG-T10 (C4): assist storm (MOVED rev 9, verbatim)

CG-T10 (C4): assist storm — per-client balances; assist visitor
vs WND-open rightToRun handoff arm; §A.3 stop injected
mid-assist-increment (§9.1(6)): fan-out completes within one
increment, no paused-count overshoot.

### CGT1.9 — CG-T11 (all): diagnostics-as-asserts (MOVED rev 9, verbatim)

CG-T11 (all): the §2.4 diagnostics as debug asserts gated on the
stage flags (freelisted-block check per WND-open; endMarking
root-liveness check); A4-site all-CMS-empty walk exercised at
C1 with executing CodeBlocks present (F37).

## ANNEX CGD6 (BINDING) — rev 8 rulings and moved full texts

### CGD6.1 — F40: NL across a GC conduct tenure (full walk)

Defect (cross-document round, rev 8 log): SPEC-nativeaffinity
permits a Locked native to reach a sync collection mid-body WITH
NL held — its NA-I13 voluntary-transition assert is exempted for
the conductor bracket via ANNEX BL1.6 (there), and NA-T4 carried
a conductor-holds-NL sync-collection sub-arm. That walk was
derived against the LANDED single-window heap §10 conduct: one
access release, one GCL bracket, one re-acquire. SPEC-congc §3
replaces that with a tenure of N windows — per-window blocking
GCL re-acquires, per-window GBL barriers, between-window
donateAll/waitForTermination condvar waits, the F28 GCL
inter-cycle handoff, the tail access re-acquire at
`Heap.cpp:4955`. An NL-holding mutator-conductor
(GCConductor::Mutator, stages C0-C1) would therefore hold NL
across an ENTIRE concurrent cycle, serializing every Locked
native, custom accessor, JSClassRef callback and handleHostCall
funnel process-wide for the cycle's duration, through transition
shapes BL1.6 never walked. Verified NOT a deadlock: NL waiters
are §A.3-compliant park sites that F8-revert at each WND-open
(nativeaffinity NL1/BL1.1), and marking termination needs
nothing from NL waiters — a liveness/grounding gap. Neither spec
cited the other; CG-I19 was silent on NL; §9 composed GC-vs-§A.3
but never GC-vs-NL.

Option space: (a) FORBID — NL dropped around the sync-collection
conduct path; (b) RE-DERIVE BL1.6 for the windowed protocol
(walk per-window release/re-acquire edges, the F28 handoff, the
tail re-acquire; add composed test arms). (b) REJECTED: even
re-derived sound, every Locked native still stalls for a full
cycle (latency regression with no consumer); the walk would need
re-derivation on every future window-model change; and CG-I19's
closed loop already excludes every other foreign obligation (JS,
heap access, RHA/AHA, EXIT1) — NL is the same class.

RULING (NORMATIVE, recorded BOTH SIDES — here + SPEC-congc
§3.7/CG-I19/§13.5(4); SPEC-nativeaffinity rev 8 §3.4 NA-I13
NARROWED + ANNEX BL1.8 + the BL1.6 head note + the NA-T4 rev-8
arm):
1. `m_nativeLockDepth == 0` at conducting entry (CG-I19; debug
   assert at the §10.2 election win, the poll-conduct grant, and
   `conductSharedCollection` entry).
2. A sync-collection requester on an NL-eligible lite with
   depth != 0 instantiates the nativeaffinity BL1.8 NL DROP
   SCOPE (NA-I11-style: depth saved, FULL release) BEFORE the
   request path's election/follower-park/conduct, reacquiring
   via the park-capable NL1 loop AFTER the conduct's `:4955`
   access-reacquire tail (conductor case) or the follower's
   ticket-served resume (follower case), before the native frame
   resumes. The scope brackets the WHOLE request funnel — the
   loser/follower park is covered, so no NL is held across a
   ticket park either.
3. Effect on the process-wide Locked-native stall: bounded by
   the stop WINDOWS the requester's threads see (F8), not the
   cycle; the BL1.6 conductor-HOLD license remains ONLY for §A.3
   (single-window, haveABadTime-class) conductors and
   arbitration losers, whose bracket is bounded per CGS2.3.
4. GIL-on/flag-off: the drop scope's level-0 gate makes this
   dead code (nativeaffinity NA-I1); CG-I0 unaffected.
Arms: CGT1.1 F40 sub-arm; nativeaffinity NA-T4 rev-8
multi-window arm (drop witnessed, waiters progress, depth
restored).

### CGD6.2 — §3.4 per-site back-off dispositions (MOVED rev 8 under the size cap; VERBATIM rev-7 body text)

Guard, under `*m_threadLock` after tryLock success:
`m_gcConductorActive && m_currentPhase != NotRunning` => unlock
GCL and back off (cites refreshed rev 9 — CGD7.4):
- `:4596` election winner: fall through to the follower wait
  (`:4616-4628`; else CGD1.1 nesting).
- `:4658` poll: return false; retry next poll.
- `:5126` §10D revert poll: return, hint stays armed (F11;
  CGD1.2 — a raw landed-style wait deadlocks); bounded
  wait only when NotRunning.
- CGA2 R7 (C2): unlock GCL, re-wait on the ticket signal (F24;
  CG-T6 arm).
(Rev 8 addition per CGD5.2: the §9.2(5) teardown acquire is a
fifth §3.4 site — unlock + bounded re-wait.)
- (rev 9, F47) `JSThreadsStopScope` watchdog-ctor tryLock loop
  (`Heap.cpp:5584-5587`): disposition PROCEED — NO back-off
  guard; a foreign mid-cycle GCL hold is exactly what §9.1(1)
  legalizes (today the tryLock succeeds only when no conduct is
  in flight; post-spec it succeeds between windows BY DESIGN).
  Obligations on success: the §9.1(2) marker pause (F47
  placement: after tryLock success only) and the F45 waiter
  register/deregister (CGD7.2). CG-T1's grep-lint counts this
  site CLASSIFIED, keeping §3.4's "every site" claim true.

## ANNEX CGD7 (BINDING) — rev 9 rulings, full texts, and the cite-refresh ledger

### CGD7.1 — F43: the §A.3 conductor is a FULL CLIENT during its window (full walk)

GOVERNS SPEC-congc §9.1(4) carve-out and §9.1(8); supersedes
rev 8's allocation-free/access-less conductor model and
CGS2.4(a)'s "closure stays ALLOCATION-FREE" clause (struck both
sides — the strike rides the pending CGS2.4 row into the
SPEC-ungil fold).

Landed facts (ground truth, 43fd5fb94387):
1. AB-21 (commit 353a3100fd61): the thread-granular §A.3
   conductor re-acquires its OWN client heap access inside the
   JSThreadsStopScope/GCL window (`VMManager.cpp:631-646`)
   because Class-A fire bodies take DeferGC and run write
   barriers — the per-VM deferral slots assert
   `hasHeapAccess() || worldIsStoppedForAllClients()`, and the
   §A.3 thread-granular witness is invisible to them. The access
   is dropped again BEFORE resume publication.
2. AB-10 (commit 43fd5fb94387): the conductor's in-window
   allocation slow path (ungil ANNEX HBT2.1 class-4 allocating
   body) may run `WeakSet::sweep`/`shrink` and
   `Heap::sweepNextLogicallyEmptyWeakBlock` — the asserts gained
   the `(jsThreadsThreadGranularWorldIsStopped() &&
   jsThreadsCurrentThreadIsStopConductor())` disjunct
   (`WeakSet.cpp:81`, `:106`; `Heap.cpp:3339`). `WeakBlock::
   destroy` can therefore run from a mid-GC-cycle §A.3 window.

Rules (NORMATIVE; the §6.2-rule classification the body's F43
row points at):
(a) BARRIERS: the conductor's in-window write barriers route per
    §5.2 — under C1R to its OWN CMS (its client is current on
    the thread; the CMS lock is the terminal leaf, legal at any
    depth); the appends are drained at the NEXT WND-open like
    any client's (next-cycle grey is sound: the referenced cells
    are either already grey/black or re-greyed by the drain
    before the next constraint pass, §5.2 NORMATIVE order).
    Markers are PAUSED for the whole window (CGP1), so no
    marker observes the CMS mid-append; !C1R appends go to the
    multi-producer server stack (F44).
(b) ALLOCATION while `m_isMarking`: black by §6.2 — the phase
    fields and `m_isMarking` are stable for the whole §A.3
    window (the conductor holds GCL; flips are in-window-only,
    CG-I4); freelist cells are covered by the next WND-open
    flush of its TLC (§6.2(3)); `m_currentBlock` cells by the
    next fixpoint window's conservative re-scan (§6.2(5) — the
    conductor's stack is scanned: it is a registered client).
(c) SWEEP-TO-FREELIST: conductor-triggered sweeps run under MSPL
    with access held INSIDE the §A.3 window; CG-I5 ("no sweep
    straddles a GC window") is unaffected because the §A.3
    window's GCL hold excludes any GC WND-open until the sweep's
    MSPL section and the window itself complete.
(d) WEAK-SWEEP SOUNDNESS (the §9.1(4) carve-out): at the moment
    the AB-10 license fires, every marker is paused at a batch
    boundary (the §9.1(2) ctor pause completed before the
    window's work ran), so no in-flight visit holds WeakBlock
    interior pointers; logically-empty WeakBlocks contain no
    live impls by definition; the dtor's marker RESUME postdates
    the frees, and the window's ISB1.1 bump covers code paths.
    Epoch/quarantine routing is NOT required for these blocks —
    this is a third, explicitly-licensed free class, named in
    §9.1(4).
(e) CLOSED-LOOP DELTA: ungil's "allocation-free, own client
    only" default-conductor closure rule (HBT2.2/R1.i) is
    NARROWED to gilOff Class-A bodies as implemented — the spec
    no longer claims allocation-freedom anywhere; CG-I25 +
    CG-T8 F43 arm enforce (a)-(d).

### CGD7.2 — F45: foreign-waiter fairness for the WND-open GCL re-acquire (mechanism)

GOVERNS §9.1(2a); amends CGS2.3 (see its rev-9 note).

Problem: the §A.3 acquisition is an unqueued 1ms tryLock poll
(`Heap.cpp:5584-5587`; `JSThreadsSafepoint::
watchdogAssertStopProgress` `JSThreadsSafepoint.cpp:512`); the
spec's WND-open re-entry is a BLOCKING (barging) acquire issued
the moment `m_scheduler->timeToStop()` elapses. With
back-to-back fixpoint windows the inter-window GCL-free gap can
be repeatedly lost to the conductor; nothing structural makes
the poller win, so CGS2.3's bound was sampled, not guaranteed,
and the loss mode is a deterministic 30s fail-stop crash.

Mechanism (NORMATIVE):
1. `Heap::m_foreignGCLWaiters` (relaxed `Atomic<unsigned>`).
   BOTH JSThreadsStopScope ctors increment it BEFORE their first
   lock attempt and decrement it immediately AFTER the lock is
   held (dtor does not touch it; a !isSharedServer() early
   return never increments). The §9.2(5) teardown acquire and
   any stage-added foreign GCL acquirer follow the same
   bracket.
2. The GC conductor's WND-open RE-ENTRY (only — the first-window
   tryLocks and the F28 transfer are untouched): before its
   blocking acquire, load the counter; while nonzero, DEFER —
   access-released timed sleep (1ms class) + re-check. Liveness:
   between windows GCL is FREE (CG-I12) and the conductor is
   abstaining, so a registered waiter's next tryLock succeeds
   within one poll quantum; it then decrements, and the
   conductor proceeds (possibly queueing behind the foreign
   scope's bounded §9.1(2) window — that term is already in
   CGS2.3). Multiple waiters drain in poll order; the conductor
   waits at most the sum of their bounded scopes — all CGS2.3
   terms.
3. No counter check inside the blocking lock itself (no barging
   change to WTF::Lock); fairness is achieved purely by
   conductor abstention. Flag-off: the counter is maintained but
   never consulted (one-window conducts never re-enter) — CG-I0
   byte-for-byte on the GC path; the two extra relaxed RMWs per
   stop scope are the same class as the landed watchdog
   bookkeeping.
4. CG-I26 asserts the abstention; CG-T8's F45 arm storms
   back-to-back fixpoint windows against a registered waiter
   and fails on any watchdog fire.

### CGD7.3 — F46: per-window AtomStringTable install (ruling)

GOVERNS the §3.7 ATOM-TABLE PIN; amends CGA2 R3 (see its rev-9
note). Landed: `conductSharedCollection` installs the main VM's
atom table at the top of the conduct and restores it at function
return (`Heap.cpp:4885-4889`) — a TENURE-WIDE TLS install whose
"every mutator is parked" license is true today (one window)
and FALSE between windows (the owner thread runs JS and mutates
the table). Ruling: once any §13.2 flag is on, the install moves
INTO the window — install at each WND-open after WSAC, restore
before that close's GCL release; the consumers (finalize(),
deleteUnmarkedCompiledCode, the step-7 phase loop) are all
in-window (§8.1), so no consumer loses coverage. Between
windows the §3.7 closed loop performs NO AtomString
create/deref — this was previously unstated; it is now CG-I27,
and debug builds set the conductor thread's table to null
between windows so any violation crashes deterministically
(the dataLog/logGC and scheduler paths in the between-window
wait take no atom operations; CG-T8's F46 arm runs a logGC=true
cycle to prove it). Flag-off: the landed tenure-wide install
stands byte-for-byte (CG-I0).

### CGD7.4 — cite-refresh ledger (BINDING; rev 9)

Body cites were re-verified against commit 43fd5fb94387.
Earlier-rev annex/log text retains pre-refresh numbers as
historical record; any pre-refresh cite in a BINDING annex reads
through this table (old -> new; `Heap.cpp` unless noted):

| old | new (43fd5fb94387) | symbol |
|---|---|---|
| :521 | :536 | m_worldState scribble comment |
| :706-740 | :722-756 | reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell |
| :790-792 | :808-810 | activity-callback ISS gating |
| :993-1020 / :1201-1244 / :2258-2282 | :991-1018 / :1217-1260 / :2295-2320 | fix-shared-heap-corruption diagnostics |
| :1024-1080 / :1037 | :1038-1096 / :1053 | gatherStackRoots / WSAC assert |
| :1103-1112 / :1111 | :1119-1128 / :1127 | beginMarking / fence raise |
| :1190-1248 / :1247 | :1206-1264 / :1263 | endMarking / fence lower |
| :1427 / :1432 / :1444-1467 / :1479 | :1443 / :1448-1452 / :1464-1487 / :1499 | addToRememberedSet / m_barriersExecuted / re-whiten CAS / append |
| :1487-1498 | :1502-1518 | sweepSynchronously |
| :1595-1600 | :1626-1632 | async-trigger reroute |
| :1631-1648 / :1636-1648 | :1662-1676 | shouldCollectInCollectorThread |
| :1683 / :1686 | :1702 / :1715 | checkConn / conn assert |
| :1879 | :1910-area | fixpoint thread-state assert |
| :1917 | :1948 | executeConvergence |
| :1957-1958 / :1979 | :1988-1989 / :2010 | fixpoint stays-stopped / Concurrent assert |
| :1969 | :2000 | m_incrementBalance reset |
| :1984-1996 / :1997-2002 | :2016-2050 | runConcurrentPhase arms |
| :2027 | :2058-area | parallelMarkersShouldExit set |
| :2032 / :2036-2039 / :2085-2088 | :2063-2064 / :2067-2069 / :2116-2118 | runEndPhase empty walk / barrier batches |
| :2169-2209 / :2213 | :2200-2246 / :2244 | finishChangingPhase / phase stores |
| :2217(-2256) / :2287 / :2315-2342 | :2248(-2290) / :2324 / :2352-2379 | stopThePeriphery / resumeThePeriphery / rightToRun loop |
| :2234-2237 | :2265-2268 | m_mutatorExecutionVersion bump |
| :2348 / :2390 / :2354-2384 / :2421 / :2497 | :2385 / :2427 / :2389-2410 / :2456 / :2530-area | stopTheMutator / resumeTheMutator / bit asserts / stopIfNecessarySlow / waitForCollector |
| :2652-2680 / :2688-2714 / :2747 | :2681-2706 / :2725-2750 / :2784 | conn relinquish / handleNeedFinalize / notifyThreadStopping |
| :2753 | :2790 | finalize() |
| :2806-2823 / :2812 / :2828 | :2843-2860 / :2849 / :2865 | requestCollection legacy asserts |
| :3127, :3140 | :3200-3212 | didAllocate -> performIncrement routing |
| :3322(-3333) | :3395(-3406) | writeBarrierSlowPath |
| :3936(-3940) / :3928-3940 | :4001-4014 | setMutatorShouldBeFenced ISS forcing |
| :3950(-3951) / :3959-3978 / :3974 | :4015-4023 / :4024-4043 / :4039-area | performIncrement / assist body |
| :4334 | :4402 | §10B.4 pre-ISS quiescence conjunct |
| :4456 | :4529 | ISS-flip fence init |
| :4479 / :4499 / :4503 | :4552 / :4572 / :4571 | requestCollectionShared / conn idempotent / collector-quiesced assert |
| :4507 / :4523 / :4533-4537 / :4542-4554 / :4550-4554 / :4554 / :4562-4572 | :4580 / :4596 / :4606-4610 / :4616-4628 / :4620-4628 / :4628 / :4629-4644 | runSharedGCElection internals |
| :4578 / :4585 / :4600-4603 | :4651 / :4658 / :4651-4690 | tryConductSharedCollectionForPoll internals |
| :4757 / :4768 / :4769-4771 / :4780-4793 / :4817-4834 / :4822 / :4832-4834 | :4830 / :4841 / :4843-4844 / :4849-4866 / :4891-4907 / :4895 / :4905-4907 | conductSharedCollection: entry/GSP/release/barrier/flush banner |
| :4852-4863 / :4856-4868 / :4870 / :4877-4915 / :4880-4915 | :4925-4937 / :4924-4940 / :4938-4945 / :4946-4978 | ticket drain / rebias aggregate / hooks call / rebias block |
| :4923-4925 / :4927-4943 / :4945-4950 / :4955 | :4991-5019 / :5000-5016 / :5020-5022 / :5028-5031 | resume pass / ISB bump / clears+GBC / VMM resume+tail |
| :4961-5008 | :5034-5099 | runSafepointHooksAndReclaim body (+ NEW :5059-5060 fast path) |
| :5023-5031 / :5036(-5043) / :5040-5043 / :5051 | :5103-5110 / :5126(-5140) / :5130-5136 / :5136 | pollIssRevertIfNeeded internals |
| :5107-5149 / :5125-5127 | :5197-5259 / :5216 | SINFAC / I6 precondition |
| :5390-5452 / :5456-5482 | :5480-5544 / :5546-5596 | park hooks / JSThreadsStopScope (now TWO ctors + dtor) |
| :5656-5838 / :5678 / :5707-5758 / :5723 / :5752 / :5773 | :5820-5908 / :5768-area / :5820-5908 / :5836 / :5865 / :5886 | AHA region / registration / steps 0-3 / three gates |
| Heap.h:722-726 / :723,:726 / :1061,:1095 / :1169,:1184 / :1183 / :1209 / :1290 / :1325 | :747-751 / :748,:751 / :1684 / :1201,:1216 / :1215 / :1241 / :1323 / :1358-area | fence pair / addressOf / currentThreadClient / race stack / mutator stack / threshold / GCA / collectorRunning |
| JSThreadsSafepoint.cpp:231-337 / :334-338 / :379,:401,:403,:412 | :250-357 / :445 / :512 | stopTheWorldAndRun / stop-scope construction / watchdogAssertStopProgress |
| VMManager.cpp:561 | :577 | conductor stop-scope construction (watchdog ctor) |
| SlotVisitor.cpp:469-486 / :522,:578 / :527-585 / :600-605 / :623-636 / :629,:645 / :661-667 / :664,:673 / :737-751 / :753-765 | :468-486 / :527,:583-area / :532-590 / :599-608 / :620-655 / :634,:650 / :666-669 / :669,:678-area / :738-756 / :758-771 | visitor internals |
| MarkedBlock.cpp:59-106 / :201-227 / :345 | :59-123 / :218-244 / :355 | isLive / stopAllocating / aboutToMarkSlow |
| BlockDirectory.cpp:495-519 / :511 / :539-559 / :556 | :505-528 / :521-area / RE-AUDIT at C3 (reader renamed/moved; CGB1 B2 row re-anchors at audit execution) / RE-AUDIT | directory audit anchors |

Semantic drifts called out (not just positional): the
:5059-5060 reclaim fast path (§8.1/CG-I11 restated) and the
atomic `m_terminationException` root visit (`Heap.cpp:3765`) —
the CGN1/CG-A2 audit charter EXPLICITLY extends to runtime/-side
visitor inputs (constraint-visited VM state), which the
`heap/**`-only CGA1 grep set cannot catch.

Rev 10 drift check (BINDING addendum): HEAD advanced to
c8de3791 after the rev-9 stamp; the delta touches only
`runtime/DeferredWorkTimer*` and `runtime/SparseArrayValueMap` —
no line cited by this ledger or by the body moved. The
43fd5fb94387 column above therefore remains the authoritative
read for every cite; no re-ledger needed at rev 10.

Rev 12 correction (BINDING addendum): in the
runSharedGCElection-internals row, the single-line mapping
`old :4554 -> new :4628` was off by one. The election/GCL-busy
timed wait `m_gcElectionCondition.waitFor(*m_threadLock, 1_ms)`
sits at `Heap.cpp:4627` (`:4628` is the closing brace of that
block), both at pinned 43fd5fb94387 and at HEAD c8de3791. Read
that row's `:4554` entry as `:4627`. The range mappings in the
same row (`:4542-4554 -> :4616-4628`, `:4550-4554 ->
:4620-4628`) are UNCHANGED — `:4628` is a correct inclusive
range end for the release/lock/wait block. Body occurrences
fixed at rev 12 (§2.2, §9.1(2a)); rev 12 log records the
disposition.

## ANNEX CGD8 (BINDING) — rev 12 addendum ruling

### CGD8.1 — F48: A4-site conductor-client exemption (full text)

GOVERNS the §5.2 "conductor EXEMPT (F37/F48)" clause and the
CGA1 A4 row as amended; composes with (does not supersede)
F37's placement ruling and CGD4.5(b)'s next-cycle-grey
disposition.

Defect (internal spec contradiction, surfaced by a live
deterministic abort): congc-t1 under the pinned GIL-off env +
`useConcurrentSharedGCMarking=1` aborts at the LANDED A4 walk
(`Heap.cpp:2227`, top of runEndPhase, after
`m_helperClient.finish()`):
`ASSERT(!client.m_mutatorMarkStack ||
client.m_mutatorMarkStack->isEmpty())`. F37 (rev 7) placed the
walk there and called it sound because "the final window's
§3.1(e) WND-open drain emptied every CMS and WSAC bars client
appends since" — reasoning only about the two POST-site
conductor-context end-phase batches CGD4.5 had named. Rev 9's
F43/CGD7.1(a) then made the client-conductor a FULL CLIENT:
its in-window conduct-path write barriers route under C1R to
its OWN CMS (terminal-leaf lock, legal at any depth) at ANY
in-window point — including BEFORE runEndPhase reaches the A4
site. WSAC bars suspended clients; it never barred the
conducting thread itself. F43 was never reconciled with A4, so
every C1 cycle in which the conductor fires a barrier after
the final WND-open drain trips the walk by design.

NORMATIVE:
1. The A4 emptiness invariant binds every NON-CONDUCTOR
   client ONLY. At the A4 site the walk SKIPS the client
   returned by `GCClient::Heap::currentThreadClient()` on the
   conducting thread; all other clients hard-assert
   CMS-empty under their CMS lock (bare terminal-leaf
   acquisition; not a nested LK.9d>LK.9c site — no outer
   `m_markingMutex`).
2. The exempted contents are exactly the CGD4.5(b)/CGD7.1(a)
   LEGAL class: NEXT-CYCLE grey, drained at the next WND-open
   (§5.2 drain (i)); on client exit they ride the §9.2(1)
   `flushClientMutatorMarkStackForExit` path. No same-cycle
   consumption is required (CGD4.5(b)'s landed-semantics
   argument applies unchanged to pre-site appends: end-phase /
   conduct-path barrier appends were always deliberately
   deferred to the next cycle's merging constraint).
3. This is NOT an assert weakening: the invariant the walk
   enforces was always "no client that WSAC suspends can have
   appended since the final drain". The conductor was never in
   that set; asserting it was a spec error (F37/F43
   non-reconciliation), not an engine error. Any non-conductor
   client's non-empty CMS still fail-stops, and the server
   `m_mutatorMarkStack` / race-stack asserts at the same site
   are UNCHANGED.
4. Soundness witness (exemption minimality, 2026-06-12, live
   tree): with the exemption landed verbatim (skip
   conductor-client; every other client still asserted),
   congc-t1 passes 5/5 under the pinned GIL-off env + C1 flag,
   plus flag-off bare and GIL-on C1 arms — i.e. no
   non-conductor CMS was ever non-empty at the site across the
   storm scenarios; the sole appender was the conductor,
   exactly the F43-legalized set. A broader exemption (e.g.
   skipping the walk wholesale) is REJECTED: it would paper
   genuine WSAC violations.
5. CG-T11's A4-site arm (CGT1.9) re-arms against the amended
   shape: the C1 executing-CodeBlocks run must exercise BOTH a
   non-empty conductor CMS surviving the site AND the
   non-conductor hard-assert (a deliberately corrupted
   non-conductor CMS in the harness must still abort).
