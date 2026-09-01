# INTEGRATE-congc.md — shared-file manifest for the N-mutator concurrent GC workstream

This file is SPEC-congc.md §13 written as a manifest (the CONGC-HANDOUT.md §2.2
file-ownership ground rules are the charter). The integrator applies the hunks
below exactly as written; the congc workstream itself never edits the
chartered-out files.

Owned surface (spec §13.1): `Source/JavaScriptCore/heap/**` only (the same set
as heap §12, plus `GCThreadLocalCache.*`, `HeapClientSet.h`,
`GCSafepointEpoch.*`); `JSTests/threads/congc-*.js`; this file. CG-1
additionally carries, by explicit task charter (the two [r34] code
obligations, SPEC-ungil-history.md F-A ruling items 2-3, in force now):
`Source/JavaScriptCore/bytecode/JSThreadsSafepoint.cpp` (the jettison
stop-bracket caller only) and `Tools/threads/lint-lockorder-u20.sh` (the
CG-T2 lint authority).

## Status (CG-1: C0 infra — window split of the conduct path)

Landed entirely inside the owned surface; NO shared-file hunks are required to
build or run CG-1. Specifically:

- `conductSharedCollection` (heap/Heap.cpp) is split into
  `openSharedGCStopWindow` (WND-open, §3.1 order with the F15 FIRST-WINDOW
  carve-out, the F28 TicketDrainSuccessor arm, and the F45 re-entry deferral)
  and `closeSharedGCStopWindow` (WND-close, §3.2 order with the F23
  final-close GCL-held carve-out and the F22 phase-store-order pin).
  Flag-off (no §13.2 stage flag on) the conduct is exactly ONE window
  (§3.6 degenerate): FirstWindow open + final close, byte-for-byte today's
  steps 3-4 and 8-9 (CG-I0). The Reentry/TicketDrainSuccessor open arms and
  the non-final close arm compile but are unreachable at C0
  (`sharedGCWindowedStagesEnabled()` is constant `false`, see Options row
  below).
- GCA gains its owner: `Heap::m_gcConductorThread` (Thread*, guarded by
  `*m_threadLock`, stamped where GCA is set in `runSharedGCElection` /
  `tryConductSharedCollectionForPoll`, restamp debug-asserted NotRunning-only).
  The deferred wind-down clears are OWNERSHIP-CHECKED (F20): clear GCA + owner
  only if `m_gcConductorThread == &Thread::currentSingleton()`; `notifyAll`
  unconditional. FLAG-OFF DELTA DISPOSITION: the ownership check changes the
  reachable-today stale-clear race (T1 descheduled between the GCL unlock and
  its deferred clear while T2 conducts) from "followers degrade to the timed
  1ms branch" to "GCA stays true through T2's cycle" — ruled BENIGN-DELTA
  under CG-I0's inspection gate by ANNEX CGD3.1 (BINDING), which this manifest
  cites as that annex itself requires.
- §3.4 between-windows guards (`m_gcConductorActive && m_currentPhase !=
  NotRunning` under `*m_threadLock` after tryLock success => unlock GCL and
  back off) at every landed GCL tryLock site, dispositions per ANNEX CGD6.2:
  election => fall to the follower wait; poll => return false;
  §10D revert poll => RESTRUCTURED per F11/CGD1.2 (back off with the hint
  still armed; the bounded GEC wait runs only while NotRunning — the landed
  unconditional wait deadlocks once a cycle has between-window gaps);
  `JSThreadsStopScope` watchdog-ctor tryLock loop => PROCEED, no back-off
  (F47 row, rev 9). Flag-off the guards never fire: in flag-off shared mode
  every GCL-free point has `m_currentPhase == NotRunning` (ANNEX CGD1.1
  flag-off half), so behavior is unchanged.
- F45 GCL fairness scaffolding: `Heap::m_foreignGCLWaiters` (relaxed
  `Atomic<unsigned>`); BOTH `JSThreadsStopScope` ctors increment before their
  first lock attempt and decrement once the lock is held; a
  `!isSharedServer()` early return never increments; the dtor never touches
  it. The WND-open RE-ENTRY arm defers its blocking acquire while the counter
  is nonzero (access-released 1ms-class sleep + re-check, ANNEX CGD7.2) —
  flag-off the counter is maintained but never consulted (one-window conducts
  never re-enter; CG-I0 byte-for-byte on the GC path; the two extra relaxed
  RMWs per stop scope are the same class as the landed watchdog bookkeeping).
- §3.7 closed-loop conductor scaffolding: `waitBetweenSharedGCWindows()`
  (donateAll + `waitForTermination(m_scheduler->timeToStop())` — condvar-only,
  in NEITHER marker counter, never `drainInParallelPassively`/
  `drainFromShared`; F13/ANNEX CGD1.3) compiled, called only from the
  flag-dead windowed path until CG-3 wires `runConcurrentPhase`'s ISS arm.
- F46 per-window atom-table pin scaffolding: the windowed open/close arms
  install/restore the main VM's AtomStringTable PER WINDOW (install after
  WSAC, restore before that close's GCL release; debug: the conductor's table
  is nulled between windows, CG-I27). Flag-off the landed TENURE-WIDE install
  in `conductSharedCollection` stands byte-for-byte (ANNEX CGD7.3).

## [r34] code obligations carried by CG-1 (SPEC-ungil-history F-A items 2-3)

- F-A item (2): the jettison stop bracket
  (`bytecode/JSThreadsSafepoint.cpp`, the `jsThreadsStopScope.emplace`
  construction) is RE-POINTED at the WATCHDOG ctor instead of the blocking
  ctor: the caller samples `MonotonicTime::now()` strictly BEFORE its
  `ClientHeapAccessReleaseScope` and passes it as `watchdogRequestStart`, so
  the previously-unwatched raw `m_gcConductorLock.lock()` wait is
  watchdog-covered. The blocking ctor remains for the
  `SharedHeapTestHarness.cpp` C-level scenario uses (chartered out of CG-1;
  those waits are bounded by the harness scenarios themselves).
- F-A item (3): the watchdog ctor threads the TARGET VM
  (`&m_heap.vm()` — under U0b every client of one server belongs to that one
  VM, so the server-side vm() IS the requesting VM) into
  `watchdogAssertStopProgress` instead of `nullptr`, so a timeout attributes
  to the requesting VM (kills the nil-Class-A-context misattribution
  signature). NO VMManager.cpp delta (spec §13.3(c)): `VMManager.cpp`'s
  conductor construction already uses the watchdog ctor and is covered by
  this ctor-side fix.

Both are mode-independent plumbing on shared-server-only paths; flag-off
observable behavior unchanged.

## Status (CG-2: C0 infra — per-client GC state, 2026-06-12)

Landed per Part II §4.1-4.2/§5.2-5.3 + ANNEXES CGD2.2, CGD4.4/4.5, CGA1
A4/A21. Everything is C1R-gated (C1R := `Heap::sharedGCBarrierStateIsPerClient()`
= `Options::useConcurrentSharedGCMarking() && isSharedServer()`; F33/CGD4.4) —
with the C1 stage flag off (the only supported shape until CG-3..CG-6) every
landed code path is byte-for-byte and the new per-client state is unrouted,
unread (CGD4.4 ruling, option (b)).

- `GCClient::Heap` gains the CG-2 block (heap/Heap.h): `m_mutatorMarkStack`
  (CMS, lazily created under the lock by the first C1R-routed append; null
  whenever !C1R) + `m_mutatorMarkStackLock` (LK.9c TERMINAL leaf — U20 lint
  R3/R4 enforce; 3 acquisition sites, all marked), `m_mutatorShouldBeFenced` +
  `m_barrierThreshold` + `m_fenceEpochSeen` (§5.3(2) copies; conductor-written
  in-window only), `m_didRunSinceLastWindow` (§4.1; owner-thread relaxed
  writes). Server gains `m_barrierFenceEpoch` (FEP; release-bumped in
  `setMutatorShouldBeFenced` per master mutation).
- `addToRememberedSet` routes via `GCClient::Heap::currentThreadClient()`
  when C1R: client => CMS append under the leaf lock + client fence-copy
  read; NULL client => SERVER stack + server fence master with the
  CGD4.5(a) WSAC debug assert (conductor context); foreign-server client =>
  server arm, no WSAC implication. The server stack KEEPS multi-producer
  mode under C1R (F44 — remaining producers are the conductor-context
  appends); !C1R is the landed 43fd5fb94387 multi-producer shape verbatim.
- WND-open (openSharedGCStopWindow, post-WSAC, C1R-only): didRun fold (OR
  every client byte into the legacy `m_mutatorDidRun` ->
  `m_mutatorExecutionVersion` consumer in stopThePeriphery, clear in-window;
  CG-I9) + the §5.2(i) CMS correctness drain into `m_sharedMutatorMarkStack`
  (per client: HCS rank 6 -> m_markingMutex LK.9d -> CMS LK.9c, the one
  marked LK.9d>LK.9c nesting site). NORMATIVE A21 consequence holds by
  construction: the drain precedes the window's first constraint pass.
- WND-close (closeSharedGCStopWindow, pre-close, every ISS close): §5.3(2)
  fence republish master->all clients + FEP stamp, then the §5.3(4)
  ASSERT-only stamp check. Runs flag-off too (CGD4.4 vacuous arm:
  conductor-only, in-window, copies unread).
- Consumers re-pointed C1R-only: `Heap::mutatorShouldBeFenced()` /
  `barrierThreshold()` are now defined below GCClient::Heap and read the
  CURRENT CLIENT's copy when C1R (covers HeapInlines.h writeBarrier fast
  paths, writeBarrierSlowPath, reportExtraMemoryAllocatedPossiblyFrom-
  AlreadyMarkedCell, and the runtime/* callers). `addressOf*` accessors are
  UNCHANGED (CGD2.2: emitted code bakes the SERVER pair).
- §5.3(3) GIL-off JIT address pin: `setMutatorShouldBeFenced` drops its ISS
  always-fenced forcing ONLY when `useConcurrentSharedGCMarking && !VM::
  isGILOffProcess()` — GIL-off C1+ keeps the server master pinned (F19;
  CGD2.2 reader table is the proof set) until the §13.3(a) jit/ungil
  per-client address reroute lands.
- didRun producers: AHA success tail (GCClient::Heap::acquireHeapAccess) +
  SINFAC hot-poll exit (stopIfNecessaryForAllClients), both C1R-gated
  relaxed stores.
- CGA1 A4: the "all CMS empty" debug walk landed at the existing
  runEndPhase `m_mutatorMarkStack->isEmpty()` site (after
  m_helperClient.finish(), before the first conductor-context writeBarrier
  batch). A21: MarkStackMergingConstraint.cpp carries the binding comment;
  no code change (the constraint covers server + race stacks only — already
  the landed shape).
- Manifest row 1 is APPLIED (this task charter explicitly owns
  `runtime/OptionsList.h` + `runtime/Options.cpp` for the row): the four
  §13.2 stage flags (default false) + validation in notifyOptionsChanged
  enforcing the §7 prefix rule (C4->C3->C2->C1) + useSharedGCHeap for C1,
  evaluated C1-first so violations cascade; plus
  `sharedGCMutatorMarkStackDonationThreshold`. DEFAULT SUPERSESSION: the
  row below said 128; spec §13.2 says "default per §5.2(ii)" = ONE
  MarkStackArray segment (GCSegmentedArray.h CapacityFromSize over the 4KB
  block = (4096 - 16)/8 = 510 cells, release shape) — landed as 510.
- `Heap::sharedGCWindowedStagesEnabled()` is RE-POINTED per row 1's
  instruction (`static constexpr false` -> OR of the four stage flags):
  flags-off identical (returns false at runtime); flags-on activates the
  CG-1 staged window arms. The CG-F precondition-check section's evidence
  items 1-2 below are SUPERSEDED by this landing record (items 3-5 stand:
  CG-3..CG-6 and CG-T3..T11 remain unlanded; CG-F stays blocked).

DEFERRED to CG-3 (per spec §14, recorded so the gap is explicit): the
§5.2(ii) threshold donation trigger at the SINFAC hot-poll tail (the option
exists, nothing consults it yet), the §9.2(1) EXIT1 CMS-flush step, and the
§9.3(1) attach fence-snapshot/FEP-stamp (HeapClientSet.cpp is outside this
task's file charter). Consequence: a stage-flag-ON shape remains
development-only — a C1R thread exit could strand CMS cells until CG-3
lands the exit flush. Flag-off: no exposure (CMS is never populated).

Gate run (CG-T1 re-run, this change): `Tools/threads/lint-lockorder-u20.sh`
clean (R3: 3 CMS sites terminal-leaf-clean; R4 nesting markers present; R1
4/4 + F47 row). Debug-config syntax verification of the three touched TUs
(UnifiedSource-heap-4 = Heap.cpp/Heap.h, heap-6 = MarkStackMergingConstraint,
runtime-36 = Options.cpp) clean. Corpus/bench/identity legs
(`run-tests.sh`, `bench-gate.sh` flags-off delta 0, `v5a-identity.sh`) are
the BUILDER's post-compile gate per the workflow split (this was a
write-only slice; the tree additionally carries ~29 uncommitted edits from a
concurrent workstream, so binaries built now would not attribute cleanly).

AMENDMENT (review round, 2026-06-12): the five row-1 option descriptions as
first landed contained the non-ASCII character U+00A7 ("section sign") inside
their `_s` ASCIILiteral strings. ASCIILiteral's ASSERT_ENABLED ctor asserts
isASCII per character, and the options table is built in a global
initializer, so EVERY Debug-build jsc invocation (even `jsc -e 'print(1)'`,
no flags) aborted at startup with `ASSERTION FAILED: isASCII(...)` at
ASCIILiteral.h:105 — killing the entire Debug verification substrate
including the chartered CG-T1 re-run. -fsyntax-only could not catch this
(runtime assert; compiles clean). AMENDED: all five descriptions now spell
the spec reference in ASCII ("SPEC-congc sec 7.1" etc.); these five rows
were the only non-ASCII bytes inside `_s` literals in the working diff
(the `§` occurrences elsewhere are comments — never reach ASCIILiteral).
Review-round verification already executed by the reviewer (pre-amendment,
Release config, recorded here so the builder pass does not repeat them):
Release incremental build clean; congc-t1-window-split.js and
congc-t2-lockorder-lint.js PASS with `$vm.sharedHeapTest` confirmed live
(not a silent skip); full threads corpus GIL-on 15/15 applicable PASS;
option §7 prefix-rule cascade verified (each illegal flag combination
forced off with the spec-cited message); lint exit 0. STILL OWED to the
builder pass post-amendment: (a) Debug rebuild + Debug CG-T1 re-run green
(the rung this amendment unblocks), (b) `bench-gate.sh` flags-off delta-0
on a quiet host, (c) `v5a-identity.sh`.

## Status (CG-3a: C1 sub-slice — kill-switch retire + marker scheduling, 2026-06-12)

Landed per Part II §7.1/§3.7 + ANNEXES CGD1.3, CGP1, CGD2.1. Gates G1-G4 were
recorded CLOSED by the owner-side entries (SPEC-ungil-history r33-35,
SPEC-nativeaffinity-history r9-11) before this slice was dispatched, so C1
code may land; the C1 stage SHIPS only when all of CG-3 (3a+3b+3c) is green
under the §2.3 gate block. Files touched: `heap/Heap.cpp` (phase-loop region
+ the conductSharedCollection identity stamp), `heap/Heap.h`,
`heap/SlotVisitor.cpp/.h`, the three gate tests, this file.

- Kill-switch retire (§7.1): the `runFixpointPhase` ISS stays-stopped return
  is now conditioned on `!Options::useConcurrentSharedGCMarking() ||
  Options::numberOfGCMarkers() < 2` (the §3.7/CGD1.3 zero-helper rule:
  Concurrent is NEVER scheduled with < 2 markers — a passive conductor would
  park with nobody draining). The `runConcurrentPhase`
  `ASSERT(!isSharedServer())` is replaced by the ISS arm. Flag-off both
  sites behave exactly as before (the retired return still taken; the ISS
  arm unreachable).
- `runConcurrentPhase` ISS arm (F13; ANNEX CGD1.3 GOVERNS): NEITHER legacy
  arm runs when ISS — the conductor RELEASE_ASSERTs the C1 flag, >= 2
  markers, and conn == Mutator (§10B.2: collector thread quiesced until C2),
  then runs `waitBetweenSharedGCWindows()` (the CG-1 §3.7 scaffold:
  donateAll + waitForTermination(timeToStop), condvar-only, in NEITHER
  marker counter) and reloops. Helpers need no new scheduling: the
  runBeginPhase helper tasks already run `drainFromShared(HelperDrain)` for
  the whole cycle, in-window and between windows alike.
- `finishChangingPhase` periphery pairing -> WND-close/open (§7.1/§3):
  the resume edge INTO `CollectorPhase::Concurrent` defers a
  `closeSharedGCStopWindow(false)` until AFTER the `m_currentPhase` store —
  F22 PHASE-STORE ORDER (the phase publication completes before the close's
  GCL release, CG-I4); the suspend edge OUT of Concurrent runs
  `openSharedGCStopWindow(Reentry)` BEFORE `stopThePeriphery()` (whose I5
  assert requires WSAC). Edges key on the Concurrent phase itself, not the
  suspend-state change alone: NotRunning->Begin stays inside the already-open
  window (F15) and End->NotRunning leaves the final close to
  `conductSharedCollection` (F23 carve-out). Concurrent is the only
  non-suspended mid-cycle phase (CollectorPhase.cpp), so the pairing is
  exhaustive. New conductor-identity scaffold: `m_sharedGCConductorClient`
  (Heap.h §3.5 note; stamped/cleared by conductSharedCollection,
  conductor-private) feeds the Reentry open's access-released assert.
- CGP1 pause pair (§9.1(2); mechanism only — the SETTER,
  `pauseConcurrentMarkingForForeignStop`, is CG-3b): `Heap.h` gains
  `m_parallelMarkersShouldPause` + `m_pausedParallelMarkers` (both under
  `m_markingMutex`; writer contract on the bool: WTF::atomicStore under the
  mutex, because checkpoint (b) hint-reads it lock-free with
  WTF::atomicLoad(relaxed)). Checkpoint (a): the helper-wait `isReady`
  lambda gains `|| m_parallelMarkersShouldPause`; a woken waiting helper
  does waiting--, paused++, notifyAll, parks until !ShouldPause, then
  waiting++ and re-arms the work wait (outer while catches a back-to-back
  re-pause). ShouldPause thereby gates a fresh helper's FIRST waiting++
  (transient under the one mutex hold — the §9.1(2) predicate never
  observes it). Checkpoint (b):
  `SlotVisitor::helperDrainPauseCheckpointIfRequested()` at drain()'s
  per-batch safepoint, HelperDrain visitors only
  (`m_isDrainingFromSharedHelper`, F14 participant set): donateAll, active--,
  paused++, notifyAll, park, active++ — granularity one drained batch
  (CG-I12 bound). `didReachTermination` additionally requires
  `m_pausedParallelMarkers == 0` (CG-I22 — false termination structurally
  impossible; waitForTermination stays parked across a foreign stop). Debug
  assert active == waiting == paused == 0 after `m_helperClient.finish()`
  (runEndPhase).
- F17 counter-leave fixes (ANNEX CGD2.1 BINDING — cited here as that annex
  itself requires): all FOUR `drainFromShared` return paths now do
  waiting-- (MainDrain TimedOut/Done; HelperDrain TimedOut/Done-on-exit).
  FLAG-OFF DELTA DISPOSITION: the decrements change, flag-off, ONLY the
  stealSomeCellsFrom denominator (a work-partitioning hint) and the
  Heap.cpp marker-count dataLog — not collection behavior, the bench gates,
  or any assert. Ruled BENIGN-DELTA under CG-I0's inspection gate by
  CGD2.1; the mode-split alternative (gating the decrement on ISS) was
  REJECTED there — divergent counter semantics per mode is the F17 trap
  class itself.
- Gate tests written (the CG-3 gate block runs them once 3a+3b+3c are all
  in): `JSTests/threads/congc-t3-barrier-storm.js` (CGT1.5 incl. the F19
  GIL-off sub-arm note + TSAN/amplifier run-config arms),
  `congc-t4-alloc-steal-storm.js` (CGT1.6; harness + JS-thread arms;
  endMarking liveness walk live-by-construction, see below),
  `congc-t11-diagnostics.js` (CGT1.9; F37 A4-site walk with executing
  CodeBlocks). All three skip-arm gracefully (and PASS) without the
  flags/Thread global.

DEFERRED out of CG-3a (explicit, so the gaps stay visible):
- CG-3b: `pauseConcurrentMarkingForForeignStop` (the CGP1 pair's only
  setter), per-window atom install for stop scopes, F35 rebias pin, F45
  wiring into the stop-scope ctors. UNTIL IT LANDS a foreign §A.3 stop
  during a Concurrent gap does NOT pause helpers — the C1 flag remains a
  development-only shape (same class as the CG-2 deferral note).
- CG-3c: EXIT1 CMS flush, attach fence-snapshot, SINFAC threshold donation,
  CG-A2/CGN1 audit, CG-I18 asserts.
- F28 TicketDrainSuccessor open remains UNWIRED (as at CG-1): under C1,
  back-to-back tickets drain inside the tenure's current window exactly as
  flag-off (the End->NotRunning edge performs no close, so the next cycle
  begins with WSAC still set — sound, conservative; the per-ticket
  close/successor-reopen latency shape is a follow-up wiring item with
  ANNEX CGD4.1 as its charter).
- CGT1.9 re-gating: the §2.4 freelisted-block and endMarking root-liveness
  diagnostics are still the ALWAYS-ON-ISS RELEASE-grade
  fix-shared-heap-corruption instrumentation (stronger than the chartered
  stage-gated debug asserts; both functions are outside CG-3a's chartered
  region). The conversion + strip decision stays with the cleanup pass that
  retires that instrumentation; CG-T11 exercises them as they stand.

Verification (write-only slice; builder owns the compile/run gates):
Debug-config `-fsyntax-only` clean on both touched unified TUs
(UnifiedSource-heap-4 = Heap.cpp/Heap.h consumers, heap-8 = SlotVisitor.cpp);
the three tests parse and PASS their skip arms under the existing (stale)
Release jsc with no flags. STILL OWED to the builder pass: full rebuild;
flags-off corpus + CG-T1 re-run byte-identical (the F17 benign-delta is the
ONLY flag-off behavior delta, per CGD2.1 — review amend: the §9.1(2)
checkpoint-(b) per-batch hint load was originally taken by every HelperDrain
visitor regardless of the stage flag, an unruled flag-off delta; now
m_isDrainingFromSharedHelper is option-byte-gated at the drainFromShared
drain entry (FIX-V5B-F1 pattern), so flag-off the checkpoint reads only the
visitor's own line and the F17 claim holds as written); bench-gate flags-off
delta 0;
v5a-identity; then (post-3b/3c) the CG-3 gate block (CG-T3/T4/T5/T8/T9/T11,
TSAN with the stage flag, GIL-off ladder rungs + stage flag, flags-off
CG-T1 identity).

## Status (CG-3b: C1 sub-slice — stop-protocol composition, 2026-06-12)

Landed per Part II §9.1/§8.3/§3.7 atom pin + ANNEXES CGP1, CGD7.1-7.3,
CGD5.1, amended by the SPEC-ungil-history [r34] F-A ruling items (1)/(4)
(see the PENDING-CONGC-COUNTERPART record below — this section IS the congc
owner's cross-cite). Files touched: `heap/Heap.cpp` (stop-scope region + the
conductSharedCollection loop tail), `heap/Heap.h` (pause/resume declarations
+ the scope's pause-tracking member), `JSTests/threads/
congc-t8-stop-interleaving.js`, this file. Write-only slice; builder owns
the compile/run gates.

- `Heap::pauseConcurrentMarkingForForeignStop(MonotonicTime requestStart)` +
  `resumeConcurrentMarkingAfterForeignStop()` (the CGP1 pair's only setter —
  the CG-3a deferral is closed): set/clear `m_parallelMarkersShouldPause`
  (WTF::atomicStore under `m_markingMutex`, the Heap.h writer contract) +
  notifyAll; the pause waits until
  `m_numberOfActiveParallelMarkers == 0 && m_numberOfWaitingParallelMarkers
  == 0` (paused helpers are in NEITHER counter, F16/F17; the predicate is
  stable because ShouldPause gates counter (re-)entry, CGP1). [r34] F-A
  item (1): the wait is TIMED — per 1ms quantum it samples
  `JSThreadsSafepoint::watchdogAssertStopProgress(requestStart, &vm())`
  (same quantum family as the watchdog ctor's tryLock loop; the item-(3) VM
  threading carries over), so a wedged marker batch fail-stops ON THE
  CONDUCTOR ITSELF instead of hanging unwatched (the conductor's next
  sample otherwise sits in the VMManager predicate loop). CG-I16: only
  `m_markingMutex` is acquired — no api-rank, no heap rank >= 7 lock.
- Call sites (F18/F47 — the heap-OWNED ctors/dtor are the ONLY pause/resume
  sites; §13.3(b) no foreign row): BOTH `JSThreadsStopScope` ctors call the
  pause after the GCL is HELD, when `m_currentPhase != NotRunning` (read
  GCL-ordered, F22). Watchdog ctor: after a SUCCESSFUL tryLock only, never
  per failed iteration; it passes its caller's `watchdogRequestStart` (one
  end-to-end per-requester budget — the F-B/CGS2A.3 [r34] reading).
  Blocking ctor: carries no caller timestamp (its remaining callers are the
  bounded SharedHeapTestHarness scenarios; the jettison bracket was
  re-pointed at the watchdog ctor by CG-1, F-A item (2)), so its pause leg
  samples against a budget opened at the pause call — still a <= 30s
  fail-stop, never an unwatched hang. Flag-off both gates are unreachable
  (every GCL-free point is NotRunning, ANNEX CGD1.1 flag-off half): CG-I0.
- Dtor order (NORMATIVE, §9.1(2)): `~JSThreadsStopScope` resumes the paused
  markers strictly BEFORE releasing GCL — no WND-open with paused markers
  is structural (the conductor's Reentry open blocks on the GCL this scope
  still holds). Tracked by the new scope member
  `m_didPauseConcurrentMarking`; paired resume in the same scope (CG-I16).
- F45 fairness (CG-I26): VERIFIED wired, no new code — both ctors carry the
  CG-1 `m_foreignGCLWaiters` bracket (inc pre-attempt, dec once held) and
  `openSharedGCStopWindow(Reentry)` abstains while nonzero.
- §3.7 atom pin (F46/CG-I27): VERIFIED landed (CG-1 scaffolding, activated
  by the §13.2 flags): per-window install after WSAC in
  `openSharedGCStopWindow`, restore before each close's GCL release,
  ASSERT-build null between windows; the tenure-wide install remains the
  flag-off-only arm. No CG-3b edit was needed.
- F35/CGD5.1 rebias pin: `conductSharedCollection`'s loop tail is
  mode-split. WINDOWED arm (sharedGCWindowedStagesEnabled()), per CYCLE in
  the cycle's still-open FINAL window (the End->NotRunning edge performs no
  close, so WSAC is set and GCL held): `runSafepointHooksAndReclaim()` once
  per cycle (§8.1/CG-I11 — "the cycle's last window"), then the §D.1 rebias
  on the per-cycle predicate (THIS cycle's `m_lastCollectionScope == Full`,
  replacing the per-conduct aggregate, CGD5.1(2)); single-shot per Sealed
  snapshot (`noteRebiasRestampComplete` consumes it); Eden cycles neither
  run nor suppress; post-reclaim, strictly pre-ISB-bump (the NEXT close —
  successor non-final or conduct-tail final — performs the bump and
  WSAC/GSP clears). CG-I23. FLAG-OFF arm: the landed post-loop position +
  `sawFullCollectionThisStop` aggregate, byte-for-byte (one window per
  conduct makes the two positions identical — CG-I0).
- §9.1(8) conductor-as-full-client COMPOSITION FACTS — VERIFIED, not
  edited (CG-I25; ANNEX CGD7.1): AB-21 conductor self-access re-acquire
  inside the GCL bracket — present, `VMManager.cpp:685-710` (windowClient
  acquire `:688` / drop before resume publication `:710`; the rev-9 cite
  `:631-646` drifted with later landings); AB-10 weak-sweep license
  disjunct — present, `WeakSet.cpp:81` (sweep) and `:106` (shrink), and
  `Heap.cpp` `sweepNextLogicallyEmptyWeakBlock` (the spec's `:3339` cite,
  now `:3608`). Soundness composition: the §9.1(2) ctor pause completes
  BEFORE the window's work, so no in-flight visit holds WeakBlock interior
  pointers when the license fires; the dtor's marker resume postdates the
  frees (CGD7.1(d)).
- Gate test written: `JSTests/threads/congc-t8-stop-interleaving.js`
  (CGT1.1): harness stop-scope interleavings re-run under the C1 flag
  (blocking + watchdog ctors vs live conductors, CG-I21 storm); F18/F43/F45
  live arm (sibling-thread Class-A stop storm — fresh-global haveABadTime —
  against back-to-back forced cycles with barrier-heavy OLD-graph churn);
  thread-exit churn vs forced Full cycles (F35 feeder); exact-checksum
  oracle. Run-config arms carried by drivers (t3 convention): F46 logGC
  run, GIL-off F35/F43 re-run under the pinned env + C1 flag, TSAN,
  amplifier. Skip-arms PASS without Thread/$vm. NOTE: "written" is not
  "green" — see the AMEND section directly below for the observed Arm-1
  KNOWN-RED, its CG-2 root cause, the in-tree fix, and the owed re-run.

### AMEND (2026-06-12): CG-T8 Arm-1 KNOWN-RED — CG-2 drain placement, FIXED, builder re-run owed

CG-T8 was observed RED on a freshly built Release jsc (C1 flags on): all
four Arm-1 harness scenarios (jsThreadsStopVsGCRequester,
gcDuringDebuggerPark, debuggerStopDuringSharedGC, syncRequesterStorm)
crashed at the FIRST conducted collection with the `runBeginPhase`
fail-stop "SlotVisitor should think that GC should terminate before
constraint solving". Evidence: a `numberOfGCMarkers=1` rerun dumped
`m_sharedMutatorMarkStack->isEmpty(): false`; the same scenarios PASS with
the C1 flag off; CG-3b's pause/resume is exonerated (paused=0,
ShouldPause=false at crash; syncRequesterStorm takes no stop scopes and still
crashed — see the test header's KNOWN-RED RECORD).

- ROOT CAUSE (a CG-2/CG-3 placement bug, not a CG-3b one): the SPEC-congc
  §5.2(i) per-client CMS drain in `Heap::openSharedGCStopWindow` ran at the
  PRE-CYCLE FirstWindow open (and would have at the F28
  TicketDrainSuccessor open once wired), pre-loading
  `m_sharedMutatorMarkStack` BEFORE `runBeginPhase`'s
  `didReachTermination()` precondition — `SlotVisitor::hasWork` counts both
  shared stacks. With >1 marker the freshly-armed helpers stole the
  pre-cycle cells concurrently, which is why the crash dump recomputed
  `didReachTermination()=true` and the pause counters read clean.
- FIX (this amend, `Heap.cpp` `openSharedGCStopWindow` drain block): the
  drain TARGET is open-kind split. Pre-cycle opens (FirstWindow /
  TicketDrainSuccessor) drain every client CMS into the SERVER legacy
  `m_mutatorMarkStack` — the landed single-VM route for pre-cycle barrier
  appends (cleared at full-GC begin where the mark-version reset makes
  them redundant; retained and constraint-merged on Eden via
  MarkStackMergingConstraint) — which still PRECEDES the window's first
  constraint-solver pass, so §5.2(i)'s normative order holds on this arm
  too. Only the mid-cycle Reentry open (marking in flight, shared-stack
  accounting live) feeds `m_sharedMutatorMarkStack`. The F32/CGA1 A21
  "CMS work accounted via the shared stacks" claim is hereby NARROWED to
  mid-cycle drains; the congc spec owner should fold an amendment marker
  into §5.2(i) at the next rev (this record is the normative content).
- STATUS: FIXED IN TREE, NOT RE-RUN — this amend slice is write-only/no
  builds, and NOTHING in this slice has been compiled or executed (the fix
  is correct by inspection only, on a tree carrying concurrent-workstream
  edits). The builder loop MUST rebuild and re-run CG-T8 under ALL THREE
  configs — (i) the //@ numberOfGCMarkers=4 config, (ii) a
  --numberOfGCMarkers=1 arm, (iii) the pinned GIL-off env arm + C1 flag —
  plus the flags-off byte-identity gates before calling the C1 gate block
  green. Until those runs are recorded here, CG-T8 is carried as
  KNOWN-RED-now-FIX-PENDING, not as a green gate.

### [r34]/[r35] PENDING-CONGC-COUNTERPART — now RECORDED congc-side (F-A items 1 + 4)

SPEC-ungil-history.md [r34] finding F-A amended two congc-owned texts and
left them PENDING-CONGC-COUNTERPART; [r35] finding G-A promoted that marker
to a blocking SHIP GATE ("C1 and ANY useConcurrentSharedGCMarking stage
implementing the §9.1(2) pause MUST NOT ship until the congc owner records
F-A items (1) and (4) — back-cites congc ANNEX CGS2.4(a) + CGT1"). This
section is that record (the §13.5(5) convention, direction reversed;
back-cites: SPEC-ungil-history.md F-A ruling + ANNEX CGS2A AMENDMENT [r34]
+ the REV 35 G-A ruling):

- Item (1) — TIMED, per-quantum-sampled pause: LANDED (CG-3b, above). The
  CGS2.4(a)/CGP1 pause body as implemented samples
  `watchdogAssertStopProgress(requestStart, vm)` per 1ms quantum; the
  untimed CGP1 BLOCK shape is superseded in code. The congc spec owner
  should fold an [r34] marker into ANNEX CGP1's COUNTER PROTOCOL paragraph
  at the next spec rev (text amendment only; the normative content is this
  record + the ungil-side CGS2A.4(a) [r34] text it implements).
- Item (4) — CG-T8 wedged-marker arm (fail-stop ON THE CONDUCTOR ITSELF):
  CHARTER RECORDED in `congc-t8-stop-interleaving.js` (header, PENDING
  arms). Engine leg landed and structurally samples on the conductor; the
  AFFIRMATIVE crash witness needs (a) a SharedHeapTestHarness marker-wedge
  injection hook (park one HelperDrain helper outside its checkpoint while
  a stop scope lands mid-cycle) and (b) an expected-crash run mode —
  `stopTheWorldWatchdogTimeout` is constexpr 30s
  (`JSThreadsSafepoint.cpp:583`) and a marker cannot be wedged from JS.
  The test's live arms witness only the NEGATIVE half (mid-cycle scopes
  with churning markers complete promptly, attribution-only storm reading
  per [r35] G-B — no fan-in cap exists, so no "no-fire-below-cap" claim is
  made or tested).
  DISPOSITION (amend pass, 2026-06-12 — BLOCKING, per the [r35] G-A ship
  gate the charter lists item (4) as PART OF the CG-T8 gate): the
  affirmative witness is OWED and CG-T8 cannot be reported as a delivered
  item-(4) gate without it. Exactly one of the following closes it, and
  the closer must be recorded here:
  (a) land the `SharedHeapTestHarness.cpp/.h` marker-wedge injection hook
      (park one HelperDrain helper outside its checkpoint while a stop
      scope lands mid-cycle — the existing
      `g_sharedGCConservativeRootAuditHook` extern-seam pattern is the
      template) plus an expected-crash run mode/driver arm that asserts
      the fail-stop fires ON THE CONDUCTOR with the item-(3) VM
      attribution. NOT landable from this amend slice: the harness files
      are outside its owned-file set
      (Heap.cpp/Heap.h/congc-t8/INTEGRATE-congc.md) — assign to the
      harness/builder pass; OR
  (b) obtain EXPLICIT charter-owner sign-off on the PENDING disposition
      (name + date recorded in this section), accepting that the
      conductor-side fail-stop leg ships with structural-sampling
      evidence only (`pauseConcurrentMarkingForForeignStop`'s per-1ms
      `watchdogAssertStopProgress` sample) until the hook lands.
  Until (a) or (b) is recorded, the C1 gate block stays NOT-GREEN on this
  item even after the drain-placement fix above turns Arm 1 green.

DEFERRED out of CG-3b (unchanged from the CG-3a list): CG-3c items (EXIT1
CMS flush, attach fence-snapshot, SINFAC threshold donation, CG-A2/CGN1
audit, CG-I18 asserts); the F28 TicketDrainSuccessor open (still unwired —
under C1 back-to-back tickets drain inside the tenure's current window,
which the new per-cycle reclaim/rebias arm handles correctly: each cycle's
tail runs in the still-open window). The CGP1 SWEEPER EXTENSION is C3
(CG-5): `m_sweeperShouldPause`/`m_sweeperInQuantum`/`m_sweeperAcked` land
with the sweeper client (no sweeper exists to gate yet); the marker pause's
phase gate is per spec — the sweeper gate is the NOT-phase-gated one (F29).

STILL OWED to the builder pass: full rebuild; flags-off corpus + CG-T1
re-run byte-identical (CG-3b adds NO flag-off behavior delta: the pause
gates are unreachable flag-off, the rebias/reclaim mode-split keeps the
landed flag-off arm verbatim); bench-gate flags-off delta 0; v5a-identity;
then (post-3c) the CG-3 gate block (CG-T3/T4/T5/T8/T9/T11, TSAN with the
stage flag, GIL-off ladder rungs + stage flag, flags-off CG-T1 identity).

CG-I0 EVIDENCE STATUS (explicit, amend pass 2026-06-12): the flag-off
byte-identity claim for this slice is ASSERTED BY INSPECTION ONLY — the
pause callers sit behind `isSharedServer()` + `m_currentPhase !=
NotRunning` (unreachable flag-off per ANNEX CGD1.1), the atom pin and
rebias relocation sit behind `sharedGCWindowedStagesEnabled()`, and the
F45 counter is maintained-never-consulted flag-off — but NO recorded run
evidence exists yet (no CG-T1 re-run, no bench-gate flags-off delta-0, no
v5a-identity for this tree state). The slice's CG-I0 LAW claim is NOT
CLOSED until the builder records all three. ATTRIBUTION NOTE: the one
unconditional flag-off load on the termination path —
`didReachTermination`'s `!m_heap.m_pausedParallelMarkers` conjunct
(`SlotVisitor.cpp:647`) — is a CG-3a hunk, already ruled BENIGN-DELTA
under CG-I0's inspection gate in the CG-3a status section above; it is
NOT part of CG-3b's edits and is not re-ruled here.

## Status (CG-3c: C1 sub-slice — lifecycle + audits, 2026-06-12)

Landed per Part II §9.2/§9.3/§5.2 drains + ANNEXES CGD3.3, CGD4.1/4.3, CGN1.
Files touched: `heap/Heap.cpp` (GCClient teardown/detach, AHA park legs,
SINFAC entry + hot-poll tail, the two HeapClientSet helper definitions),
`heap/HeapClientSet.h` (helper declarations), `heap/GCThreadLocalCache.h`
(GCCellLockDepth facility), `JSTests/threads/congc-t5-celllock-audit.js`,
`JSTests/threads/congc-t9-attach-exit-churn.js`, this file. Write-only
slice; builder owns the compile/run gates. `heap/GCSafepointEpoch.*` were
chartered but needed NO edit — the epoch=MAX park already lives in
`detachCurrentThread` and the CG-3c work is its ORDERING (below).

- §9.2(1) EXIT1 order (F36): `~GCClient::Heap` now implements, and
  documents in place, the normative order: teardown (the
  `lastChanceToFinalize` MSPL section, access held) -> PERMANENT access
  drop -> CMS final flush under `m_markingMutex`
  (`HeapClientSet::flushClientMutatorMarkStackForExit`, defined in Heap.cpp
  — it needs both heaps' privates via HeapClientSet's existing friendship)
  -> epoch=MAX (`m_localEpoch` store in `detachCurrentThread`) -> HCS
  remove. Both dtor branches honor it: the attached thread routes through
  `detachCurrentThread` (drop -> flush -> epoch=MAX inserted between the
  landed RHA and the landed MAX park); the non-attached branch drops the
  bracket access then flushes (its epoch was parked at MAX by that thread's
  own earlier detach). F36: NO dead-state publication — the flush leaves no
  marker; the GCH simply dies with an empty CMS. The flush completes while
  still registered (rev 1's flush-first order stays REJECTED per F2;
  CG-I17). !C1R the flush is a no-op behind the option-byte-first
  `sharedGCBarrierStateIsPerClient()` gate (F33/CGD4.4; CG-I0 —
  FIX-V5B-F1 class call-site cost only).
- §9.2(1) C4 DELTA (CGD3.3/CGD4.3): NOT YET LIVE BY CONSTRUCTION — no
  assist visitor exists until C4 (CG-7 class), so ACT/DCT have no visitor
  (un)registration to enqueue and the pending-list machinery (apply at
  WND-open pre-walk / CG-I14 quiesced points; exit cancels a pending
  registration, F25) lands WITH `m_assistVisitor`. Recorded here so the
  gap stays visible; CGD4.3's "ACT/DCT never read phase" rule is already
  honored by CG-3c (neither the flush nor the snapshot reads
  `m_currentPhase` — see the AMEND below for what that forced).
- §9.2(4) conductor-exit release-assert: `~GCClient::Heap` entry, shared
  servers only: under `*m_threadLock`,
  `RELEASE_ASSERT(m_gcConductorThread != current || m_currentPhase ==
  NotRunning)` — EXIT1 on the live conductor mid-cycle fail-stops (§3.7).
  The F20 stale-owner case is excluded by construction (a stale owner's
  phase is NotRunning; a restamped owner is the successor, never the
  exiting thread). Phase read under `*m_threadLock` = the landed §3.4
  guard reader shape (F22's enumerated set). Flag-off (!ISS or option
  off): branch skipped, byte-for-byte.
- §9.3(1) ATTACH fence-init handshake:
  `HeapClientSet::snapshotBarrierFenceStateForAttach` (declared
  HeapClientSet.h, defined Heap.cpp) copies the server master
  fence/threshold into the client copies and stamps `m_fenceEpochSeen =
  FEP` (acquire load), asserting {GBL held, !WSAC, client not yet
  published}. !C1R: no-op (copies are unrouted state, CG-I0). The
  CALL-SITE is one line inside `HeapClientSet::add`'s already-shared
  GBL/!WSAC insert section, strictly before the registry append —
  `HeapClientSet.cpp` is OUTSIDE this slice's owned file set, so the call
  is manifest row 3 below (CG-3c-M1; APPLIED 2026-06-12 at
  `HeapClientSet.cpp:97` — the rest of this bullet describes the exposure
  window that existed BEFORE the row landed, kept for the record). Pre-apply, a
  mid-marking attachee's fence copies are zero-init (un-RAISED) — under
  the F19 GIL-off pin this is masked (emitted code reads the SERVER pair,
  which stays tautological), and GIL-on C1 the §5.3(2) WND-close republish
  covers every window after the attach; the uncovered shape is a GIL-on
  C1 attachee's interpreter barriers between its first AHA and the next
  WND-close. CG-3c-M1 is now applied; the CG-T9-with-marking-live
  precondition is met (the test's attach-storm arm is its witness).
- §9.3(2)-(4): verified, no code — first AHA performs the seq_cst GSP load
  (landed F8 step 2); the assist-visitor registration enqueue is the C4
  delta above; didRun=false/CMS=null at ACT are zero-init.
- §5.2(ii) SINFAC hot-poll-tail CMS donation: landed at the normative (and
  only) trigger site — `stopIfNecessaryForAllClients`, after the GSP leg,
  access held (SINFAC I6 legalizes the blocking-class lock acquisitions).
  Shape: cheap CMS-lock-only size probe (terminal leaf, bare) ->
  over `Options::sharedGCMutatorMarkStackDonationThreshold()` ->
  mid-cycle check -> `m_markingMutex` -> CMS lock (the LK.9d>LK.9c lint R4
  edge) -> `transferTo(*m_sharedMutatorMarkStack)` + notifyAll (wakes
  CGD1.3-parked helpers). Mid-cycle check: `m_currentPhase != NotRunning`
  read access-held — NOT an F34 site (F34 covers access-free ACT/DCT
  threads): with access held no window can be open and no in-window phase
  store can be concurrent, and the last store's visibility rides the F8
  seq_cst chain this same poll just executed; debug-asserted to be
  Concurrent (§8.1: the only non-suspended mid-cycle phase). Between
  cycles the donation SKIPS (no marking latency to win; a shared-stack
  append there would re-create the CG-T8 Arm-1 pre-load bug — see the
  AMEND below).
- CG-I18 cell-lock-no-park debug asserts: `GCCellLockDepth`
  (GCThreadLocalCache.h, ASSERT_ENABLED-only inline thread_local; release
  builds compile it away — zero codegen delta) + asserts
  `depth == 0` at SINFAC entry and at ALL THREE AHA park legs (F8 GSP
  revert, §A.3 jsThreadsParkForStopWindow, Mode-machine
  jsThreadsParkForModeStop), each gated on
  `Options::useConcurrentSharedGCMarking()` so flag-off debug behavior is
  unchanged. The DEPTH BOOKKEEPING sites are the `JSCellLock`
  lock/tryLock/unlock inlines in `runtime/JSCellInlines.h` — outside the
  owned set: manifest row 4 below (CG-3c-M2). Until CG-3c-M2 is applied
  the asserts are vacuously green (depth is always 0) — apply it before
  counting the CG-T5 CG-I18 storm as a delivered arm.
- Gate tests written: `congc-t5-celllock-audit.js` (CGT1.7 — per-row arms
  for the CGN1 N1-N5 rows; the N3 map/set storm IS the CG-I18 storm; N6
  coverage is the TSAN run-config arm by design) and
  `congc-t9-attach-exit-churn.js` (CGT1.2 — attach/EXIT1 churn vs forced
  cycles, exit-with-finalizer-side-stores, attach-then-exit-in-one-gap,
  issRevert poll storm, clientChurnVsGC + issRevertChurn harness re-run;
  the §9.2(4) conductor-exit and F36/F34 amplifier arms are engine-assert
  oracles + driver run-config arms, per the CGT1.2 text). Both skip-arm
  PASS without Thread/$vm.

### AMEND (2026-06-12, CG-3c): §9.2(1) exit-flush TARGET narrowed — server legacy stack, not the shared stack

SPEC-congc §9.2(1) says the exit flush goes "into the shared mutator stack
(under m_markingMutex)". Implementing that literally re-creates the CG-T8
Arm-1 KNOWN-RED class for every between-cycles EXIT1: a flush into
`m_sharedMutatorMarkStack` while NotRunning pre-loads the shared-stack
accounting before the next cycle's `runBeginPhase` `didReachTermination()`
precondition (hasWork counts the shared stacks). The discriminator that
could split mid-cycle from between-cycles is a PHASE READ, and §9.2's own
F34/CGD4.3 rule forbids exactly that on the DCT path (access permanently
dropped, no lock ordering vs the in-window store). RESOLUTION (this record
is the normative content; the spec owner should fold an amendment marker
into §9.2(1) at the next rev, the same convention as the CG-3b §5.2(i)
narrowing this extends):

- The exit flush target is the SERVER legacy `m_mutatorMarkStack`,
  unconditionally, via its MULTI-PRODUCER locking `append()` (F44 — legal
  from any thread with no window required), under `m_markingMutex` then
  the CMS leaf lock (the same LK.9d -> LK.9c chain as the WND-open drain;
  the per-cell append adds an acyclic m_appendLock edge under both).
- Soundness, between cycles: pre-cycle barrier cells take the landed
  single-VM route (cleared at full-GC begin, constraint-merged on Eden).
- Soundness, mid-cycle: `MarkStackMergingConstraint` (volatile; covers the
  SERVER + race stacks when C1R, F32) converts the cells to work at the
  next fixpoint window's constraint pass, which precedes termination. The
  flush cannot land cells after the cycle's last constraint pass: cells
  exist in the frozen CMS only if no WND-open drained it since their
  append, and every window whose open postdates the access drop drains
  every registered CMS at open — so a mid-final-window flush is
  structurally an empty no-op and `runEndPhase`'s
  `ASSERT(m_mutatorMarkStack->isEmpty())` cannot trip on flushed cells.
- The same hazard analysis is why the §5.2(ii) SINFAC donation (which CAN
  legally read the phase, access held) donates ONLY mid-cycle and skips
  between cycles rather than falling back to the server stack: between
  cycles there is no latency to win and the WND-open drain is the
  correctness path.

### CG-A2 cell-lock audit execution (CG-3c) — ANNEX CGN1 rows

EXECUTED at C1 entry per SPEC-congc §8.2. Method: walked the
`methodTable()->visitChildren` surface for readers of mutator-mutable
multi-word state (`visitChildrenImpl` definitions + the constraint-solver
visitor inputs), classified each against the CGN1 seed dispositions, and
audited every CELL-LOCKED row's MUTATOR side against CG-I18. Row schema per
CGN1. These rows are the CGN1 table content (the annex says rows are
appended "when the audit runs" — this section is that record; the spec
owner folds them into SPEC-congc-history ANNEX CGN1 at the next rev).

- N1 JSObject butterfly/shape storage — CONFIRMED IN-PROTOCOL. Visitor
  side: `JSObjectWithButterfly::visitChildrenImpl` ->
  `visitButterfly/visitButterflyImpl` (`runtime/JSObject.cpp:1149-1157`,
  `:122-132` helper) performs the om §6/§9 tagged-word decode
  (segmented/flat TID tag) — the frozen om read side. Mutator side:
  butterfly/shape transitions under SAL/cell-lock per om §6; structure-ID
  + butterfly publication order per om §9. No new rule needed.
- N2 JSString ropes — CONFIRMED IN-PROTOCOL. Visitor side:
  `JSString::visitChildrenImpl` and the rope walk read fibers via
  `fiberConcurrently()` (`runtime/JSString.cpp:72/:97/:110/:161`), the
  ungil §N.2 acquire read (TSAN-gated acquire recorded in JSString.h
  `:336-354`; flag-keyed relaxed+address-dependency otherwise); the
  resolver publishes by one release-CAS; the visitor never resolves.
  Mutator side: §N.2 verbatim.
- N3 JSMap/JSSet (JSOrderedHashTable) + JSWeakMap/Set (WeakMapImpl) —
  CLASSIFICATION REFINED vs the seed row (no protocol change): in THIS
  tree the table storage is a SEPARATE cell
  (`JSOrderedHashTable::m_storage`, a `WriteBarrier<Storage>`), so the
  visitChildren read side is a SINGLE-WORD barriered slot read
  (`runtime/JSOrderedHashTable.cpp:35-41`) + the Storage cell's own
  contents visit; `WeakMapImpl::visitChildrenImpl`
  (`runtime/WeakMapImpl.cpp:49`) visits Base + reports extra memory, with
  the bucket harvest in `visitOutputConstraints` (`:82`). The ungil §N.1
  CELL-LOCKED ruling (ALL ops cell-locked including reads, DFG/FTL map
  intrinsics disabled GIL-off) governs the MUTATOR side; the VISITOR side
  in this storage shape never traverses interior multi-word table state
  on the live-mutated cell itself, so the tryLock+defer/re-visit
  machinery (CG-I15) has NO current visitor-side consumer — it becomes
  load-bearing only if a future storage shape moves buckets back in-cell.
  CG-I18 audit of the mutator side: the §N.1 locked native bodies hold
  the cell lock across pure in-cell mutation only — allocation is
  hoisted OUTSIDE the lock (§E.1b shape: allocate outside, re-validate
  under), so no allocation slow path (CIND/SINFAC poll, AHA park) can run
  while the lock is held — CG-I18 holds BY SHAPE; the new
  GCCellLockDepth asserts are the regression fence (CG-T5 N3 storm).
- N4 Structure — CONFIRMED IN-PROTOCOL. Visitor uses the landed
  concurrent-JIT-safe read paths under `Structure::m_lock` (10b) where
  needed; Riptide already races compiler threads here. No delta.
- N5 ArrayBuffer/wasm memory words — CONFIRMED IN-PROTOCOL.
  `JSArrayBufferView::visitChildrenImpl`
  (`runtime/JSArrayBufferView.cpp:208-221`) reads mode then
  `loadLoadFence` then `possiblySharedBuffer()` — the §N.6 read order; the
  buffer is held as an opaque root, never dereferenced for contents by
  the visitor. Detach/transfer/resize per §N.6 torn-pair rules (len=0
  seq_cst + quarantine).
- N6 Profiling-class fields — RACY-TOLERATED rows (each named; TSAN
  suppressions key on THIS list only):
  - `FunctionRareData::m_objectAllocationProfile` /
    `m_internalFunctionAllocationProfile` visitAggregate reads
    (`runtime/FunctionRareData.cpp:67-68`) — allocation-profile
    words mutate under the function's cell lock on the mutator side
    (§N.4) but the watchpoint/profile scalars are jit item 7
    profiling state; a torn read steers heuristics only, never
    pointer-chases unpublished memory (the appended GC references go
    through `visitor.append` on barriered slots).
  - No OTHER racy-tolerated visitor input was found in the walk; any
    future TSAN report on a visitChildren path must either match a row
    here or block the stage (§12 rule). A6 (m_barriersExecuted) stays
    AMENDED-LANDED (relaxed atomics, CGA1 A6 rev 9) — its suppression
    RETIRES per F44.
- RT1 (runtime/-side visitor input, the rev 9/CGD7.4 charter extension):
  `vm.m_terminationException` visited via relaxed `WTF::atomicLoad`
  (`heap/Heap.cpp:4240`, the conservative-root constraint;
  publication once under `m_terminationExceptionLock`, relaxed-atomic
  readers — `runtime/VM.h:431-440/:580-581`). Disposition: N-PROTOCOL
  (atomic single-word lazy-init publish; the visited value is either
  null or a fully-published Exception*). IN-PROTOCOL.
- CG-I18 (NORMATIVE) global check: no mutator-side CELL-LOCKED section in
  the walked surface releases heap access, passes a stop poll, or enters
  a conducting path while holding 10a — enforced going forward by the
  GCCellLockDepth asserts (SINFAC entry + all AHA park legs) once
  CG-3c-M2 is applied; CGN1 N3's termination argument (in-window every
  10a lock free => visitor tryLock retries succeed) therefore holds.

Verification (write-only slice; builder owns the compile/run gates):
Debug-config syntax check clean on UnifiedSource-heap-4 (Heap.cpp +
HeapClientSet.cpp + the edited headers) and UnifiedSource-heap-3
(GCThreadLocalCache.cpp/GCSafepointEpoch.cpp) — PCH bypassed (stale vs the
CG-3a/3b Heap.h edits; the builder rebuild refreshes it). Both new tests
PASS their skip arms on the stale Release jsc bare, and PASS their live
arms GIL-on with `--useSharedGCHeap=1 --useJSThreads=1 --useDollarVM=1
--numberOfGCMarkers=4` (C1 flag OFF — flag-on runs against this stale
binary hit the already-FIXED CG-T8 Arm-1 drain-placement crash and are
meaningless until the rebuild). STILL OWED to the builder pass: full
rebuild; apply manifest rows CG-3c-M1/M2 (M1 since APPLIED 2026-06-12;
M2 still pending); flags-off corpus + CG-T1 re-run
byte-identical (CG-3c adds NO flag-off behavior delta: every new branch is
behind the option-byte-first C1R gate, the conductor-exit assert is
ISS-only, and GCCellLockDepth is ASSERT_ENABLED-only with stage-gated
asserts); bench-gate flags-off delta 0; v5a-identity; then the whole-CG-3
gate block — CG-T3/T4/T5/T8/T9/T11, TSAN
(`Tools/threads/tsan/run-corpus-tsan.sh --useConcurrentSharedGCMarking=1`,
suppressions ONLY the documented RACY-TOLERATED CGA1/CGN1 rows above; A6's
suppression RETIRES per F44), the pinned UNGIL-HANDOUT ladder rungs + the
stage flag, and the flags-off CG-T1 identity re-run. NOTE the CG-T8
item-(4) DISPOSITION above remains open independently of CG-3c.

RE-VERIFICATION (independent, re-dispatch pass, 2026-06-12, no edits
needed): every CG-3c deliverable was independently re-checked against the
live tree, not taken from this record. Confirmed present:
`flushClientMutatorMarkStackForExit` (Heap.cpp:6849; dtor call :6963,
detach call :7038) with the drop->flush->epoch=MAX->remove order in both
branches; the §9.2(4) conductor-exit RELEASE_ASSERT (Heap.cpp:6942, under
`*m_threadLock`); `snapshotBarrierFenceStateForAttach` (Heap.cpp:6803,
declared HeapClientSet.h:109) asserting {GBL held, !WSAC, !isOnList};
the SINFAC mid-cycle-only threshold donation (Heap.cpp:6246-6291, bare
probe + LK.9d>LK.9c donate leg); `GCCellLockDepth`
(GCThreadLocalCache.h:129, ASSERT_ENABLED-only) and the four CG-I18
asserts (SINFAC entry Heap.cpp:6196; AHA GSP-revert :7150; §A.3 park
:7196; Mode-machine park :7236), all stage-gated; GCSafepointEpoch.*
diff-empty (matches the "no edit needed" ruling). Re-run evidence, all
synchronous: `lint-lockorder-u20.sh` exit 0 (R1 4/4 + F47; R3 six CMS
terminal-leaf sites; R4 nesting markers); congc-t5 + congc-t9 skip arms
PASS bare AND live arms PASS GIL-on (`--useSharedGCHeap=1
--useJSThreads=1 --useDollarVM=1 --numberOfGCMarkers=4`, C1 flag OFF) on
the stale Release jsc, exit 0 each; Debug-config `-fsyntax-only` clean on
UnifiedSource-heap-3 AND heap-4 (PCH bypassed — the on-disk Debug PCH is
stale vs the CG-3 Heap.h edits and was excluded from the command line).
The builder-owed list above is UNCHANGED by this pass (manifest rows
CG-3c-M1/M2 still pending AT THE TIME OF THIS RECORD — M1 was
subsequently APPLIED 2026-06-12, see row 3 and the amend record below;
M2 remains pending; CG-T8 Arm-1 still KNOWN-RED-FIX-PENDING;
item-(4) disposition still open).

## Manifest rows (chartered-out files; integrator-applied)

1. `runtime/OptionsList.h` (spec §13.2 shape; required before any C1+ stage
   flag can be exercised — NOT required for CG-1/CG-2, which are flag-dead).
   STATUS: APPLIED at CG-2 (2026-06-12; see the CG-2 status section — the
   CG-2 task charter owned the two runtime/ files for exactly this row).
   Landed with the donation-threshold default corrected to one segment
   (510), per spec §13.2's "default per §5.2(ii)"; the 128 below is the
   stale pre-CG-2 draft value, kept verbatim for the record. WARNING — do
   NOT re-copy this draft into code: its description strings contain the
   non-ASCII `§` character, which is Debug-fatal inside `_s` ASCIILiteral
   (see the AMENDMENT note above); the landed rows use ASCII "sec N":

   ```
   v(Bool, useConcurrentSharedGCMarking, false, Normal, "C1: concurrent marking between shared-GC stop windows (SPEC-congc §7.1)"_s) \
   v(Bool, useSharedGCCollectorThread, false, Normal, "C2: collector-thread conductor for the shared GC (SPEC-congc §7.2)"_s) \
   v(Bool, useSharedGCIncrementalSweep, false, Normal, "C3: incremental + mutator-concurrent sweeping under the shared GC (SPEC-congc §7.3)"_s) \
   v(Bool, useSharedGCMutatorAssist, false, Normal, "C4: incremental mutator assist under the shared GC (SPEC-congc §7.4)"_s) \
   v(Unsigned, sharedGCMutatorMarkStackDonationThreshold, 128, Normal, "CMS donation threshold (SPEC-congc §5.2(ii))"_s) \
   ```

   Option validation (Options.cpp): enforce the §7 prefix rule (a stage flag
   requires every earlier stage's flag) and `useSharedGCHeap` for all four.
   When this row lands, `Heap::sharedGCWindowedStagesEnabled()`
   (heap/Heap.h, currently `static constexpr ... return false`) is re-pointed
   at `Options::useConcurrentSharedGCMarking() || useSharedGCCollectorThread()
   || useSharedGCIncrementalSweep() || useSharedGCMutatorAssist()` — that
   single edit (inside heap/**, congc-owned) activates every windowed arm
   CG-1 staged.

2. Per-client barrier-address JIT emission: jit/ungil owners (spec §13.3(a));
   until it lands, GIL-off C1+ pins the SERVER fence master always-fenced
   (F19/CGD2.2). No row needed for CG-1.

3. CG-3c-M1 — `heap/HeapClientSet.cpp` (§9.3(1) attach handshake call site;
   the file is inside the spec §13.1 owned surface but outside the CG-3c
   task's writable set). In `HeapClientSet::add`, in the ALREADY-SHARED
   insert branch (under GBL with !WSAC, after the `!server.isSharedServer()`
   re-check `continue`), insert ONE line immediately BEFORE
   `m_clients.append(&client);`:

   ```cpp
        snapshotBarrierFenceStateForAttach(client); // SPEC-congc 9.3(1) (CG-3c): fence/FEP snapshot inside the publishing GBL/!WSAC section, BEFORE the insert.
        m_clients.append(&client);
   ```

   Do NOT add it to the first-client/option-off trivial branch (no stop
   protocol exists there; the helper would no-op anyway but the call is
   specified for the publishing shared insert only). The helper is defined
   in Heap.cpp and asserts the bracket invariants; !C1R it returns
   immediately (CG-I0). STATUS: APPLIED (2026-06-12, B-congc amend pass) —
   the hunk above is landed verbatim at `HeapClientSet.cpp:97`, in the
   already-shared insert branch, immediately before `m_clients.append`,
   exactly as specified (the trivial branch untouched). The CG-3c status
   bullet's (masked) exposure window is CLOSED; the CG-T9 attach-storm
   precondition is met (congc-t9 PASSES GIL-off + C1, see the 2026-06-12
   amend record below).

4. CG-3c-M2 — `runtime/JSCellInlines.h` (CG-I18 depth bookkeeping;
   ASSERT_ENABLED-only, release codegen untouched). In the three JSCellLock
   inlines (`JSCellLock::lock/tryLock/unlock`, currently `:379-397`):

   ```cpp
   inline void JSCellLock::lock()
   {
       Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
       if (!IndexingTypeLockAlgorithm::lockFast(*lock)) [[unlikely]]
           lockSlow();
   #if ASSERT_ENABLED
       GCCellLockDepth::increment(); // SPEC-congc 8.2 CG-I18 (CG-3c).
   #endif
   }

   inline bool JSCellLock::tryLock()
   {
       Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
       if (!IndexingTypeLockAlgorithm::tryLock(*lock))
           return false;
   #if ASSERT_ENABLED
       GCCellLockDepth::increment(); // CG-I18: count successful acquisitions only.
   #endif
       return true;
   }

   inline void JSCellLock::unlock()
   {
   #if ASSERT_ENABLED
       GCCellLockDepth::decrement(); // CG-I18: before the release.
   #endif
       Atomic<IndexingType>* lock = std::bit_cast<Atomic<IndexingType>*>(&m_indexingTypeAndMisc);
       if (!IndexingTypeLockAlgorithm::unlockFast(*lock)) [[unlikely]]
           unlockSlow();
   }
   ```

   `GCCellLockDepth` lives in `heap/GCThreadLocalCache.h` (congc-owned),
   visible here via HeapInlines.h -> Heap.h. Same-thread lock/unlock pairing
   is a JSCellLock invariant already (Locker scopes), so the per-thread
   counter is well-formed. The CG-I18 ASSERTS that consume the depth landed
   with CG-3c (Heap.cpp, stage-gated on `useConcurrentSharedGCMarking`);
   until this row is applied they are vacuously green. STATUS: PENDING —
   apply before counting the CG-T5 CG-I18 storm arm as delivered.

3. VMManager deltas: NONE (spec §13.3(c); see the [r34] section above — the
   `VMManager.cpp` watchdog-ctor construction is covered via the heap-owned
   ctor; the F45 waiter counter lives in Heap).

## Gates (CG-T1 / CG-T2)

- CG-T1: `Tools/threads/lint-lockorder-u20.sh` clean (includes the §3.4
  disposition-marker check over every `m_gcConductorLock.tryLock()` site in
  heap/**, the F47 watchdog-ctor row among them); flags-off corpus
  (`Tools/threads/run-tests.sh`) + `$vm.sharedHeapTest` byte-identical;
  `Tools/threads/bench-gate.sh` flags-off delta = 0 (heap I10 bar);
  `Tools/threads/v5a-identity.sh`.
- CG-T2: the U20 lint EXTENDED to LK.9c/9d (ANNEX CGS2.1) encoding the three
  F21 clauses (CG-I10(1)-(3)) + the CGS2.2 chain litmus —
  `Tools/threads/lint-lockorder-u20.sh` IS that extension (the rev-7
  "U20-class" private lint is retired; no second lock-order authority).
  Its ADOPTION as the one lock-order authority is gate §13.5(1), still OPEN.
- Runtime companions: `JSTests/threads/congc-t1-window-split.js`
  (flags-off conduct-path storm over the split: election, poll, stop-scope
  interleavings, revert-poll churn) and
  `JSTests/threads/congc-t2-lockorder-lint.js` (CGS2.2 runtime litmus:
  GCL-vs-marking interleavings flag-off; documents the static authority).

## Adoption gates open at CG-1 (spec §13.5)

All four remain OPEN (rev 12): (1) SPEC-ungil §LK rows LK.9c/9d + U20
extension — the lint is WRITTEN here, adoption pends the SPEC-ungil owner;
(2) §A.3 rule-5/HBT4.5 amendment + CGS2.3 wait bound; (3) HBT4 extended to
window re-entry; (4) nativeaffinity BL1.8 NL-drop. None block CG-1 (C0,
flag-dead); (1)-(2) block C1; (3) blocks exercising the §3.1 re-entry
blocking acquire; (4) blocks C1 in gilOff configs.

## CG-F precondition check (2026-06-12) — BLOCKED, no acceptance run performed

A CG-F/CG-7 acceptance-and-freeze task was dispatched with the stated
dependency "CG-6 green". Live-tree verification (greps, no builds, no git)
shows the dependency is NOT met; no CG-F leg was run and NOTHING below this
file's CG-1 status section changes. Evidence:

1. The §13.2 stage flags do not exist: `useConcurrentSharedGCMarking` /
   `useSharedGCCollectorThread` / `useSharedGCIncrementalSweep` /
   `useSharedGCMutatorAssist` — 0 occurrences in `runtime/OptionsList.h` and
   `runtime/Options.cpp`. The manifest row above remains integrator-pending.
2. `Heap::sharedGCWindowedStagesEnabled()` is still
   `static constexpr ... return false` (heap/Heap.h:1064): every windowed arm
   staged by CG-1 is still flag-dead. C1-C4 (CG-3..CG-6) are NOT landed.
3. Test surface matches: only `congc-t1-window-split.js` and
   `congc-t2-lockorder-lint.js` exist; none of CG-T3..CG-T11.
4. This manifest's only Status section is CG-1 (C0 infra). No CG-2..CG-6
   landing record exists here or in SPEC-congc rev history.
5. The working tree carries ~29 uncommitted Source/JavaScriptCore edits from
   a concurrently running workstream; the existing Release/TSan binaries are
   stale against the tree, so even flag-off legs (Leg 4 identity, bench-gate)
   measured now would not attribute to any congc state.

Consequences (binding until reversed by a landing record):
- Leg 1 is UNRUNNABLE as specified: the AFTER configuration ("landed stage
  flags") cannot be constructed — the flags are not in the binary or the
  source. BEFORE == AFTER by definition.
- CG-7 freeze is REFUSED: flipping §13.5(5) gates CLOSED in spec text and
  regenerating CONGC-HANDOUT.md from a "frozen" rev while CG-3..CG-6 are
  unlanded would falsify the acceptance record. Note the owner-side closure
  entries (SPEC-ungil-history r33-35, SPEC-nativeaffinity-history r9-11) DO
  exist — the gate-closure half of the dispatch is plausible — but gate
  closure unblocks C1 implementation; it does not substitute for CG-3..CG-6
  landing, CG-T3..T11, or the CG-F legs.
- SPEC-congc.md stays DRAFT rev 12; Tools/threads/scalebench/SPEC.md honesty
  note unchanged (its trigger — a stage flag defaulting on — has not
  occurred); BENCH.md gains no stage-gate rows (none were measured).

Unblock path: land CG-2..CG-6 per §14 (stage flags + windowed arms + CG-T3..
T11 + BENCH stage gates), get a quiet host with a clean committed tree, then
re-dispatch CG-F.

## CG-F precondition RE-CHECK (2026-06-12, later same day) — still BLOCKED, no acceptance run performed

A second CG-F/CG-7 dispatch ("depends CG-6 green") was re-verified against
the live tree. The tree has MOVED since the section above — its evidence
items 1-3 are SUPERSEDED — but the dependency is still not met. Current
evidence (greps + manifest state, no builds, no git writes):

Superseded since the prior check:
1. The four §13.2 stage flags NOW EXIST (`runtime/OptionsList.h:433-436`,
   all default false) with the §7 prefix-rule validation in
   `runtime/Options.cpp:922-936`.
2. `Heap::sharedGCWindowedStagesEnabled()` (heap/Heap.h:1168) is now the
   four-flag OR, no longer `constexpr false`.
3. Test surface grew: `congc-t3-barrier-storm.js`,
   `congc-t4-alloc-steal-storm.js`, `congc-t5-celllock-audit.js`,
   `congc-t8-stop-interleaving.js`, `congc-t9-attach-exit-churn.js`,
   `congc-t11-diagnostics.js` exist alongside t1/t2.
4. This manifest now carries CG-2, CG-3a, CG-3b, CG-3c status sections
   (all 2026-06-12).

Still blocking, in dependency order:
1. CG-3 is NOT green. The CG-3c status section's own closing record lists
   the builder pass as STILL OWED: full rebuild; manifest rows
   CG-3c-M1/M2 applied (M1 APPLIED 2026-06-12, row 3; M2 still owed);
   flags-off corpus + CG-T1 byte-identical; bench-gate
   flags-off delta 0; v5a-identity; then the whole-CG-3 gate block
   (CG-T3/T4/T5/T8/T9/T11, TSAN with the stage flag, pinned ladder rungs).
   None of that is recorded as run.
2. CG-T8 Arm-1 remains KNOWN-RED pending the rebuilt-binary re-run (see
   the CG-3b AMEND section), and the CG-T8 item-(4) disposition is open.
3. CG-4..CG-6 (C2/C3/C4 stage behavior) have NO landing record — not in
   this manifest, not in SPEC-congc rev history. The in-tree source agrees:
   the heap/Heap.h:1160-1167 comment block states the C1-C4 stage BEHAVIOR
   is not landed and a stage flag turned on is a development-only shape;
   the CG-3c §9.2(1) C4-DELTA note records that no assist visitor exists
   until C4. CG-T6/T7/T10 do not exist.
4. The working tree carries ~49 uncommitted files from a concurrently
   running workstream (bytecode/dfg/ftl/llint/runtime/heap/wasm). The
   CG-3c section records that stage-flag-on runs against the current
   binaries hit the already-fixed CG-T8 Arm-1 drain-placement crash until
   the builder rebuild — so no CG-F leg measured now (including flags-off
   Leg 4) attributes to a defined congc state.

Consequences (unchanged from the section above, restated as binding):
- Leg 1 is UNRUNNABLE as specified: the AFTER configuration ("landed stage
  flags") now parses but is behaviorally undefined — C2-C4 arms are absent
  and the C1 arms are gated on an un-run builder pass.
- Legs 2-4 are NOT RUN: results against a mid-flight uncommitted tree with
  a known stale-binary crash mode would be unattributable, and recording
  them would falsify the acceptance ledger.
- CG-7 freeze REFUSED again: SPEC-congc.md stays DRAFT rev 12; §13.5(5)
  gates stay as written in the spec text; CONGC-HANDOUT.md is NOT
  regenerated; SPEC-congc-history.md gains no freeze rev; BENCH.md gains
  no rows (nothing was measured); Tools/threads/scalebench/SPEC.md honesty
  note unchanged (no stage flag defaults on).

Unblock path (narrowed from the prior section): (a) builder pass closing
CG-3 (rebuild + owed gate block + CG-T8 re-run), recorded here as a CG-3
gate-green entry; (b) land CG-4..CG-6 per §14 with CG-T6/T7/T10 and their
status sections; (c) quiet host, clean committed tree; then re-dispatch
CG-F + CG-7 as one task.

## 2026-06-12 B-congc AMEND record (review-finding closure)

Scope: the two major findings from this run's adversarial review of the
congc slice. No git operations; working-tree amend only.

1. Manifest truth restored (finding 2): CG-3c-M1 was landed verbatim at
   `HeapClientSet.cpp:97` by the preceding fix pass but row 3 and four
   status bullets still tracked it PENDING. Row 3 now reads APPLIED
   (2026-06-12); the CG-3c exposure-window bullet, the two builder-owed
   lists, and the "Still blocking" item 1 are amended in place to
   M1-applied/M2-still-pending. CG-3c-M2 (`runtime/JSCellInlines.h`,
   CG-I18 depth bookkeeping) was independently re-verified NOT landed
   (no `GCCellLockDepth` reference in the file) and stays PENDING.

2. Face 2 / congc-t1 RED resolved at SPEC level (finding 1): the
   deterministic `Heap.cpp:2227` abort (`runEndPhase` per-client
   CMS-emptiness walk) was confirmed live, root-caused as the
   F37-vs-F43 spec contradiction, and closed by SPEC-congc rev 12
   addendum F48 (full text ANNEX CGD8.1, SPEC-congc-history.md;
   body §5.2 amended byte-neutrally, 953 lines / 49,998 bytes kept —
   anchors preserved; CGA1 A4 row amended in place). Code: the A4-site
   walk now SKIPS the conducting thread's `currentThreadClient()` ONLY
   (its CMS contents are the CGD4.5(b)/CGD7.1(a)-legal NEXT-CYCLE-grey
   class); every non-conductor client still hard-asserts CMS-empty
   under its CMS lock; the server/race-stack asserts are unchanged.
   NOT an assert weakening — the exemption matches exactly the
   F43-legalized appender set, and the 5/5 green runs below prove its
   minimality (no non-conductor CMS was ever non-empty at the site).
   Whole change is `#if ASSERT_ENABLED` + behind
   `sharedGCBarrierStateIsPerClient()`: release codegen and flag-off
   behavior byte-identical.

Verification (synchronous, Debug rebuild of UnifiedSource-heap-4 + link,
this host, 2026-06-12):
- congc-t1 GIL-off pinned env + `JSC_useConcurrentSharedGCMarking=1`:
  PASS 5/5 (was: deterministic abort at `Heap.cpp:2227`).
- congc-t1 flags-off bare (`--useSharedGCHeap=1 --useDollarVM=1`):
  PASS; congc-t1 GIL-on C1 (`--useJSThreads=1
  --useConcurrentSharedGCMarking=1 --numberOfGCMarkers=4`): PASS.
- Full congc suite GIL-off + C1: t1/t3/t4/t5/t9/t11 PASS;
  t8 fails ONLY with the pre-existing manifest KNOWN-RED Arm-1
  signature (`!jsThreadsHasSeenCrossThreadEntry(*vm)`), which is
  flags-off-reproducible and independent of C1 — unchanged status,
  out of this amend's scope.
- The earlier MEGA-RUN-RESULTS residual item 2 surface
  (`isMarked(cell)` at `addToRememberedSet`, congc-t3/t11
  deterministic; congc-t1 wedge) did NOT reproduce on the amended
  tree: t3 and t11 are green and t1 completes. The 2227 abort was the
  remaining Face 2 blocker; with F48 landed the C1 congc gate set is
  green except KNOWN-RED t8 Arm-1.

Builder-owed items NOT discharged here (unchanged): M2; flags-off
corpus + CG-T1 byte-identical diff; bench-gate delta 0; v5a-identity;
TSAN with the stage flag; CG-T8 Arm-1 fix; whole-CG-3 gate block.
