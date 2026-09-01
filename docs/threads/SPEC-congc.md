# SPEC-congc.md - N-MUTATOR CONCURRENT GC (draft rev 13)

Status: DRAFT rev 13 — NOT converged. Rev 13 = §7.1a gilOff
single-handoff (gc-sharedheap-zero-concurrent-overlap-now-11pct;
SCALEBENCH §35 round-2 / §27.S2 re-evaluation on 56b8f886e000):
bounded one-Concurrent-window-per-cycle subset of C1, gilOff-
only, C1R OFF; rev 12 below stands. Rev 12 = directed round
4: gate RE-CONFIRMED UNMET — all four §13.5(5) gates stay
OPEN on the SPEC-ungil/-nativeaffinity owners; CG-7 still owes
ONLY a rev-7/8 delta pass (rev 12 log); cite fix :4628->:4627
(GCL-busy waitFor; CGD7.4 corrected). History/BINDING
annexes = `SPEC-congc-history.md`. Authorities: THREAD.md;
SPEC-{heap,vmstate,objectmodel,jit,api,ungil}.md (+annexes);
UNGIL-HANDOUT.md rev 32. Charter: SPEC-heap Deviation 4
(SPEC-heap.md:23) deferred concurrent GC for N mutators;
shared mode today = synchronous conductor-driven STW; this
spec designs the re-enable. Verified vs 43fd5fb94387; HEAD
drift c8de3791 (rev 10 log); annex cites via CGD7.4 ledger.
heap:/ungil:/om:/jit: = the SPEC files; CG-I* = invariants
(§11); CG-T* = test charters (§12); ANNEX CG* = history
annexes.

Master rule: every stage is a MODE. Flag-off (all §13.2 options
false) = today's shared §10 protocol BYTE-FOR-BYTE (CG-I0, heap
I10 analog); `!useSharedGCHeap` or `!ISS` = legacy Riptide.
Nothing here edits GIL-on/flag-off observable behavior.

Notation: inherits heap §8 (SPEC-heap.md:8): WSAC, MSPL, BVL,
GCL, GBL, GBC, GSP, GCA/GEC, ISS, AHA/RHA, SINFAC, CSAC/RCAC,
CIND, ACT/DCT, BD, LA, TLC, HCS, GCH. New: WND = stop window
(§3); CMS = per-client mutator mark stack (§5.2); FEP =
`m_barrierFenceEpoch` (§5.3); C1R = routing predicate (F33).

## 1. Scope

Deliverables: re-enable, over the shared server heap (heap §4),
the five features Deviation 4 disabled (SPEC-heap.md:23):
concurrent marking, collector continuity, incremental mutator
assist, activity-callback collection, mutator-concurrent
sweeping — staged per §7. Non-goals: the object-model protocol
(om: frozen; consumed), the §A.3 stop machinery (ungil: frozen;
composed in §9), TLC layout/JIT addressing, the epoch
reclamation contract (heap §11), wasm-GC (heap §5.5 stands).

## 2. Ground truth

### 2.1 The legacy one-mutator handshake

`Heap::m_worldState` (`heap/Heap.h`; scribbled at
`Heap.cpp:536`) packs FOUR bits for exactly ONE mutator:
`hasAccessBit`, `stoppedBit`, `mutatorHasConnBit`,
`mutatorWaitingBit` (asserts `:2389-2410`);
its consumers (`Heap.cpp:2385-2790`) and the single-mutator
periphery carry per-line N-ary dispositions in ANNEX CGA1
(BINDING; indexed §4.3) — the audit of record (cites read via
CGD7.4).

### 2.2 The shared-mode machinery (what we generalize INTO)

Landed per heap §10: ticketing `requestCollectionShared()`
(`Heap.cpp:4552`; conn idempotent `:4572`), election
`runSharedGCElection()` (`:4580`; GCL-busy wait `:4627`),
poll-conduct `tryConductSharedCollectionForPoll()` (`:4651`),
`conductSharedCollection()` (`:4830`): GSP store (`:4841`), §10.4
GBL barrier (`:4849-4866`), ticket drain (`:4925-4937`),
`runSafepointHooksAndReclaim()` (call `:4938-4945`, body
`:5034`; empty-cycle fast path `:5059-5060`, §8.1), step-8
resume (`:4991-5019`), WSAC/GSP clear + GBC (`:5020-5022`), VMM
resume (`:5028`), tail re-acquire (`:5031`). Per-client (heap
§10A, cites there): AHA/RHA/SINFAC/park hooks/
`JSThreadsStopScope` (`Heap.cpp:5197-5259`, `:5480-5596`,
`:5820-5908`), HCS, TLC, epoch. Interim N-mutator serialization
(F44, §5.2): `setMultiProducerAccess()` (`:485-497`),
relaxed-atomic `m_barriersExecuted` (`:1448-1452`). Deviation-4 kill switches: no Concurrent phase when ISS
(`Heap.cpp:1988-1989`, assert `:2010`), collector thread
quiesced (`:1662-1676`, `:1715`, `:2385`/`:2427` asserts),
assist off (`:4015-4023`), activity callbacks off (`:808-810`,
reroute `:1626-1632`), always-fenced (`:4001-4014`),
IncrementalSweeper off (`:3002-3020`, `:4905-4907`).

### 2.3 The mechanisms that already carry concurrency

(a) Versioned liveness: `MarkedSpace::m_isMarking`
(`MarkedSpace.h:187, 243`), `isLive`'s marking-aware path
(`MarkedBlock.cpp:59-123`), newlyAllocated stamping in
`specializedSweep` when `isMarking` (MarkedBlockInlines.h,
CGD7.4), freelist->newlyAllocated in `stopAllocating`
(`MarkedBlock.cpp:218-244`). (b) Mark/barrier races: `m_raceMarkStack` under its lock
(`Heap.h:1201, 1216`), `aboutToMarkSlow` (`MarkedBlock.cpp:355`),
the re-whiten CAS protocol (`Heap.cpp:1464-1487`).
(c) Multi-window cycles: `m_currentPhase` persists;
`finishChangingPhase()` (`Heap.cpp:2200-2246`) pairs
`resumeThePeriphery()`/`stopThePeriphery()` (`:2324`, `:2248`);
the rightToRun loop already iterates ALL visitors
(`:2352-2379`). Reused, not redesigned.

### 2.4 Temporary diagnostics

Fix-shared-heap-corruption instrumentation (`Heap.cpp:991-1018`,
`:1217-1260`, `:2295-2320`): stop-mode-only; behind
`verboseSharedGCHeap`-class options; CG-T11 reuses.

## 3. Architecture: the window model

A shared collection becomes ONE GCA TENURE containing a
SEQUENCE of stop windows (WNDs), not one monolithic stop.

1. WND-open = §10 steps 3-4 + flush, ORDER NORMATIVE: the
   conductor (a) is access-RELEASED (at the first WND-open,
   stays released all tenure, §3.7); (b) acquires GCL — BLOCKING
   is legal exactly because access-released (ungil §A.3 rule 2);
   the HBT4 release-before-GCL order EXTENDS to window re-entry;
   the blocking acquire DEFERS to registered foreign GCL waiters
   (F45, §9.1(2a)); (c) seq_cst `GSP=true`;
   (d) `VMManager::requestStopAll(GC)`; (e) GBL barrier until
   every client NoAccess (F8 — `Heap.cpp:4849-4866`); set WSAC;
   per-client flush (§5.2 drain, §6.2 allocator stop, the
   `stopThePeriphery()` route). Rev 1's GCL-before-release order
   REJECTED (F9: §A.3 deadlock); CG-T8 arms it.
   FIRST-WINDOW CARVE-OUT (F15): the first WND-open IS the
   landed entry — tryLock access-HELD (`Heap.cpp:4596`, `:4658`),
   GSP (`:4841`), THEN the step-3 release (`:4843-4844`);
   (a)-(c) + the blocking acquire govern RE-ENTRY only.
   TICKET-DRAIN SUCCESSOR (F28; ANNEX CGD4.1 GOVERNS): a drain
   loop successor (`:4925-4937`) RETAINS GCL from the F23 final
   close; its first WND-open = steps (c)-(e) ONLY. Flags-on
   only; flag-off the drain runs INSIDE the one window (CG-I0).
2. WND-close = §10 steps 8-9: client cache resume pass; ISB
   bump when gilOffProcess (EVERY close — each may
   jettison/patch; ISB1.1, `Heap.cpp:5000-5016`); clear WSAC;
   seq_cst `GSP=false`; GBC broadcast; `requestResumeAll(GC)`;
   phase publication; THEN release GCL (CG-I12) — NON-FINAL
   closes only. FINAL-CLOSE
   CARVE-OUT (F23): the FINAL close (->NotRunning; the drain
   loop's m_requests exit `Heap.cpp:4925-4937` postdates it)
   leaves GCL HELD — released by the landed CALLER (`:4606`
   election, `:4651-4690` poll tail; C2: CGA2 R7) or TRANSFERRED
   to an F28 successor — flag-off = today's caller-bracketed
   hold (CG-I0). PHASE-STORE ORDER (F22, NORMATIVE under any
   §13.2 flag): finishChangingPhase's stores (`:2244`) complete
   BEFORE the close's GCL release — every reader (§3.4 guards,
   §9.1(2) ctor; F34 leaves no other) is GCL-ordered; flag-off
   unaffected (callers hold GCL across it). CG-I4.
   The conductor re-acquires its own access only at the landed
   tail (`Heap.cpp:5031`) after the FINAL WND-close.
   Heap-resume-before-VMM-resume stays normative.
3. Between windows: mutators run; marking helpers may run (§7
   C1); `m_currentPhase` persists (§2.3(c)). WSAC false between
   windows; WSAC-gated asserts (heap I5) stay correct (§8.1).
4. Tenure: the conductor keeps GCA=true and stays the elected
   §10.2 winner across all windows. GCL is RELEASED between
   windows and re-acquired at each WND-open (CG-I12) — what lets
   a JSThreads stop interleave (§9.1). "GCL free && GCA set &&
   phase != NotRunning" becomes a STEADY STATE (today only the
   wind-down instants, `Heap.cpp:4606-4610`). EVERY GCL tryLock
   site — landed AND stage-added (CGA2 R7, F24) — gets a
   between-windows disposition (F10/F12). Guard, under `*m_threadLock` after tryLock success:
   `GCA && m_currentPhase != NotRunning` => unlock GCL and back
   off. Per-site dispositions (`:4596` election, `:4658` poll,
   `:5126` §10D revert poll/F11, CGA2 R7/F24, AND — F47 — the
   stop-scope watchdog-ctor tryLock loop `:5584-5587`, PROCEED,
   no back-off): ANNEX CGD6.2 (BINDING; watchdog row rev 9).
   F28 inter-cycle state {GCL HELD, GCA set, NotRunning, world
   running}: NO guard needed — foreign tryLocks FAIL (election
   falls to the follower wait `:4620-4628`; poll returns false);
   bounded by the loop's m_requests check.
   WIND-DOWN CLEAR (F20; ANNEX CGD3.1 GOVERNS): deferred GCA
   clears postdate the GCL unlock (`:4606/:4609`, poll analog
   `:4651-4690`); clear GCA + owner under `*m_threadLock` ONLY
   if owner == current thread; `notifyAll` unconditional;
   restamp only in NotRunning (debug assert). Flag-off
   byte-for-byte (CG-I0). CG-I21; CG-T8/T9.
5. Conductor identity: stays `GCConductor::Mutator` running the
   `collectInMutatorThread()` phase loop (heap §10B.2) in stages
   C0-C1; C2 adds a collector-thread conductor (§7.2). GCA gains
   an owner: `m_gcConductorThread` (Thread*, under
   `*m_threadLock`, `Heap.h:1323`), stamped/cleared with GCA
   (clear ownership-checked, F20). Consumers: §3.4 guards
   (FOREIGN only), the §9.2 EXIT1 assert, CG-I21.
6. Flag-off degenerate: all §13.2 flags false => exactly ONE
   window per conduct (`Heap.cpp:1988-1989` fixpoint stays
   stopped) — today's `conductSharedCollection`. CG-I0 by
   inspection + §12 gates (heap I10).
7. Conductor tenure contract (NORMATIVE — CG-I19). Conducting is
   a CLOSED LOOP: first WND-open to final WND-close, the
   conductor executes ONLY the phase loop; no heap access all
   tenure (released at WND-open (a), re-acquired at
   `Heap.cpp:5031`); no JS, no RHA/AHA, no EXIT1 (ungil §B.2 on
   `m_gcConductorThread` mid-cycle release-asserts); NL PIN (F40;
   ANNEX CGD6.1): `m_nativeLockDepth == 0` at conducting entry —
   a Locked-native sync requester reaches the conduct OR follower
   path only through the nativeaffinity BL1.8 NL drop scope
   (NA-I13 NARROWED rev 8; both sides).
   BETWEEN windows the conductor's wait is `donateAll()` +
   `waitForTermination(m_scheduler->timeToStop())`
   (`SlotVisitor.cpp:758-771`, `:738-756`) — condvar-only under
   `m_markingMutex`, in NEITHER counter, never visitChildren,
   never `drainInParallelPassively`/`drainFromShared` (F13;
   ANNEX CGD1.3). `numberOfGCMarkers()==1`: Concurrent NEVER
   scheduled; the legacy Mutator-arm (`Heap.cpp:2016-2050`) is
   NOT used when ISS. Wake-ups: helper notifyAll
   (`SlotVisitor.cpp:634, :650`) + scheduler timeout.
   ATOM-TABLE PIN (F46; ANNEX CGD7.3 GOVERNS): the landed
   tenure-wide atom-table install (`Heap.cpp:4885-4889`) becomes
   PER-WINDOW under any §13.2 flag (install at WND-open after
   WSAC; restore pre-GCL-release — consumers are all in-window,
   §8.1); NO AtomString ops between windows (debug: nulled).
   CG-I27; CGA2 R3 amended.

## 4. Handshake generalization (charter item 1)

### 4.1 What replaces each legacy bit

- `hasAccessBit` -> per-client `m_accessState` (landed, §10A).
- `stoppedBit` -> GSP + the GBL barrier + WSAC (landed, F7/F8);
  per-window stop cycle = §3.1-3.2. NO new per-client stop bit:
  the F8 Dekker pair gives each client a sound park/revert path
  (AHA, `Heap.cpp:5820-5908`); GC-park hooks pair
  release/re-acquire per thread (`:5480-5544`, ungil §A.3.8).
- `mutatorWaitingBit` -> the F8 blocking in AHA step 3/SINFAC.
  No ParkingLot on `m_worldState`; clients block on GBC.
- `mutatorHasConnBit` -> GCA + the §10.2 election (landed). C2
  re-splits it (§7.2): "conn = collector thread" becomes GCA
  held by the collector conductor; the bit stays set-idempotent
  (`Heap.cpp:4572`); the `checkConn` assert (`:1715`) keeps its
  `|| WSAC` form.
- `m_mutatorDidRun` -> per-client `GCH::m_didRunSinceLastWindow`
  (plain byte, owner-thread-only — heap I17). Set in AHA's
  success tail + SINFAC's hot-poll exit; conductor ORs over HCS
  at each WND-open into the legacy consumer (`Heap.cpp:2265-2268`
  version bump), clears in-window. Scheduling-only: relaxed; the
  window barrier orders it (CG-I9). C1R-gated (F33).

### 4.2 Per-client states folded into existing machinery

No new per-client state machine. The per-client record:
`m_accessState` (landed), `m_didRunSinceLastWindow` (§4.1), CMS
(§5.2), fence copy + epoch (§5.3), assist visitor + balance
(§7.4), `m_localEpoch` (landed, heap §11). Conductor iterations
over clients run under HCS `m_lock` (rank 6) or while WSAC
(HeapClientSet.h:46-47) — add/remove freeze in-window per heap
I13 (`:54-76`); fold/clear loops sound vs attach/detach (§9.3).

### 4.3 The "the mutator"-singular audit

ANNEX CGA1 (BINDING) enumerates every singular site in
`heap/**` with disposition in {LANDED-N-ARY, WINDOW-CONFINED,
FOLDED, CONDUCTOR-PRIVATE, STAGE-GATED, VM-SINGULAR-DEFERRED};
implementation consumes it verbatim; CG-T1 re-greps, failing on
unclassified matches.

## 5. Write barrier from N threads (charter item 2)

### 5.1 Inline barrier

Unchanged in shape: the HeapInlines.h threshold check +
`writeBarrierSlowPath` (`Heap.cpp:3395`), `mutatorFence()`. What
changes: (a) where the slow path appends (§5.2); (b) where the
threshold lives (§5.3).

### 5.2 Per-client mutator mark stack (CMS)

`addToRememberedSet` (`Heap.cpp:1443`) appends to the single
server `m_mutatorMarkStack` (`:1499`) — N-mutator soundness
ALREADY LANDED via `setMultiProducerAccess()` when
`useSharedGCHeap` (`:485-497`; F44 — rev 2-8's "lock-free, one
mutator" motivation is STALE; superseded both sides, rev 9 log).
CMS is a CONTENTION/SCALING + window-drain-ACCOUNTING design,
not a soundness fix. Rule: when C1R (:= ISS && `useConcurrentSharedGCMarking`;
F33/CGD4.4), GCH gains `m_mutatorMarkStack` + leaf
`Lock m_mutatorMarkStackLock`; `addToRememberedSet` routes via
`currentThreadClient()` (`Heap.h:1684`), appending under the
client's lock.
NORMATIVE under C1R: the SERVER stack KEEPS multi-producer
mode (remaining producers = the F31/F43 conductor-context
appends; narrowing = chartered perf follow-up). Drains: (i) every WND-open transfers every CMS into
`m_sharedMutatorMarkStack` under `m_markingMutex` (donateAll
shape, `SlotVisitor.cpp:758-771`); (ii) out-of-window (C1+), a
client whose CMS exceeds
`sharedGCMutatorMarkStackDonationThreshold` cells (§13.2;
default one segment, `GCSegmentedArray.h:116`) donates under
`m_markingMutex` from its own thread. Trigger SITE normative: ONLY the SINFAC hot poll tail
(`Heap.cpp:5197-5259`, after the GSP leg, access held) — never
inside `addToRememberedSet` (CMS lock TERMINAL leaf, legal under
7-9b — CG-I10/F21; SINFAC I6 `:5216` legalizes the site).
Donation is latency-only (WND-open drains give correctness).
`!C1R`: today's flag-on code byte-for-byte — i.e. the
43fd5fb94387 multi-producer shape, NOT the pre-ungil
single-producer text (CG-I0; F44).
`m_barriersExecuted` (`Heap.cpp:1448-1452`) stays a racy
diagnostic counter (relaxed atomic load/store, landed; CGA1 A6
amended rev 9). The cellState CAS protocol
(`:1464-1487`) is already N-safe (single-word CAS;
mutator-count-independent) — CG-T3. CONDUCTOR-CONTEXT barriers (F31; ANNEX CGD4.5 GOVERNS):
runEndPhase's in-window writeBarrier sites (`Heap.cpp:2067-2069`,
`:2116-2118`) with `currentThreadClient()` NULL (C2) use the
SERVER stack + fence master, in-window only (null => WSAC debug
assert; CGA2 R9); client-conductor in-window CMS appends are
NEXT-CYCLE grey; the `:2063-2064` all-CMS-empty walk stays at
the LANDED site, conductor EXEMPT (F37/F48; CGA1 A4). `MarkStackMergingConstraint` (F32; CGA1 A21) when C1R covers
the SERVER + race stacks only (CMS work terminates via the
WND-open drain, `SlotVisitor.cpp:599-608`); the §3.1(e) drain
PRECEDES the window's first constraint pass (NORMATIVE).

### 5.3 Fence/threshold versioning (per-client mutatorShouldBeFenced)

Today: single `m_mutatorShouldBeFenced` + `m_barrierThreshold`
(`Heap.h:747-751, 1236, 1241`), forced tautological once ISS
(`Heap.cpp:4001-4014`). Re-enable rule:

1. Server master pair stays; mutated ONLY in-window (raise
   `Heap.cpp:1127`; lower `:1263`; `setMutatorShouldBeFenced`
   drops its ISS forcing (`:4001-4014`) once
   `useConcurrentSharedGCMarking` AND NOT GIL-off — F19,
   §5.3(3)) — plus a server `Atomic<uint64_t>
   m_barrierFenceEpoch` (FEP) bumped (release) per in-window
   mutation.
2. GCH gains `m_mutatorShouldBeFenced` + `m_barrierThreshold` +
   `m_fenceEpochSeen`. The conductor republishes master->client
   for EVERY client inside the mutating window (WSAC, pre-close),
   stamping `m_fenceEpochSeen = FEP`. Clients never write these
   fields.
3. Consumers re-point: `mutatorShouldBeFenced()`/
   `barrierThreshold()` read the CURRENT CLIENT's copy when C1R
   (F33; else server, CG-I0). JIT: baked `addressOf*`
   (`Heap.h:748, 751`) become per-client addresses (A16-class
   reroute — CHARTERED to jit/ungil owners, §13.3); until it
   lands, GIL-off C1+ keeps the SERVER MASTER always-fenced
   (F19; emitted code reads ONLY the SERVER pair, CGD2.2).
   Master pinned => copies tautological; FEP stays at the raise
   (CG-I3); CG-T3 arm.
4. Soundness (CG-I3): a RAISE completes for all clients before
   its window closes; a LOWER only in the final window
   (endMarking), post-termination; over-fenced is always sound.
   WND-close debug assert: `m_fenceEpochSeen == FEP` for all
   clients.
5. `reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell`
   (`Heap.cpp:722-756`) and `writeBarrierSlowPath` (`:3395-3406`)
   read the same per-client state; re-whiten unchanged.

## 6. Black allocation during marking (charter item 3)

### 6.1 The mechanism is already allocator-local

Allocate-black in Riptide = versioned newlyAllocated — the
§2.3(a) mechanisms, keyed on `MarkedSpace::isMarking()`
(`MarkedSpace.h:187`).

### 6.2 N-client rules

1. `m_isMarking` flips ONLY inside a window (`beginMarking`
   `Heap.cpp:1119-1128` / `endMarking` `:1206-1264` run
   in-window). The F8 resume edge (seq_cst GSP clear -> client's
   seq_cst GSP load in AHA) publishes the flip to every client
   before it can allocate again — no extra fence (CG-I4).
2. Every sweep-to-freelist and steal stays under MSPL (heap
   §5.2/I8; `assertSharedAllocatorMutationIsSafe`); MSPL holders
   hold access, so no sweep straddles a window and each sweep
   observes a stable `isMarking` (CG-I5).
3. Per-client flush at EVERY WND-open: the `stopAllocating()`
   route reaches every client's LAs via the shared directories'
   `m_localAllocators` lists (`Heap.cpp:2248-2290`, banner
   `:4891-4907`) — N-ary today; per-window cadence is the only
   delta; non-idempotence (`:4895`) preserved by once-per-window
   pairing with the close's resume pass (CG-I6).
4. Steal protocol: a stolen block re-sweeps under MSPL before
   reuse (heap I8); rule 2 makes its newlyAllocated provenance
   correct under marking. CG-T4 storms steal-vs-marking.
5. Out-of-window allocation during marking is live by
   construction: freelist cells (rule 2) are covered at the next
   WND-open by rule 3; conservative re-scan each fixpoint window
   covers `m_currentBlock` cells (heap I12). The `endMarking`
   snapshot diagnostic (`Heap.cpp:1217-1260`) generalizes to
   CG-T11's assert.

## 7. Staged re-enable (charter item 4)

Order NORMATIVE: C1 -> C2 -> C3 -> C4; a stage's flag may be
true only if its predecessors' are (§13.2 validation). Each
stage retires the §2.2 kill switches it names, NOTHING else.

### 7.1 C1 — concurrent marking

Retires: `Heap.cpp:1988-1989` (fixpoint stays-stopped),
`:2010` (Concurrent-phase assert), the always-fenced forcing
(`:4001-4014`, per §5.3). `runFixpointPhase` may schedule `CollectorPhase::Concurrent`
(only when `numberOfGCMarkers() >= 2`, §3.7);
`finishChangingPhase`'s periphery pairing (`:2200-2246`) becomes
WND-close/WND-open (§3). `runConcurrentPhase` gains an ISS arm
(F13): helpers run `drainFromShared(HelperDrain)` between
windows; the conductor runs the §3.7 wait — NEITHER legacy arm
used when ISS (ANNEX CGD1.3). Requires: §5 + §6 complete; §8
audit executed; §9.1 pause protocol. Conductor remains the §10.2
requester.

### 7.1a gilOff single-handoff (rev 13; SCALEBENCH §35 round-2)

A BOUNDED subset of C1, gilOff-process only, NO §13.2 flag: the
§3 windowed conduct machinery (Reentry/non-final close/§3.7
wait/F46/CG-I11 per-cycle reclaim placement) is live whenever
`gilOffProcess` (predicate `sharedGCWindowedConductActive()` =
`sharedGCWindowedStagesEnabled() || gilOffProcess`,
`Heap.cpp:199`); `runFixpointPhase` schedules AT MOST ONE
Concurrent window per cycle (conductor-thread-local cap, reset
per `runBeginPhase`); C1R stays OFF — barriers keep the F44
multi-producer server-stack append (Msm-merged at the Reentry
fixpoint) and the F19 always-fenced master holds, so §5.2's
re-whiten CAS path covers between-window N-mutator barriers
without CMS routing (F44: CMS is contention-only).
Re-evaluation of §27.S2: at v33 STW was 5.4% of W=16 wall and
unbounded handoffs added ~30 reentries (+1.8% net); on
56b8f886e000 round-1's denominator shrink leaves the same
~143 ms STW = 11.0% of 1291 ms (W-invariant 12-16 stops; 45% of
the residual JS->Java gap), and the per-cycle cap bounds extra
rendezvous to #cycles. Correctness = §6.2 + §8.1 verbatim
(Cs/Wlr GreyedByExecution re-run post-Reentry on the bumped
`m_phaseVersion`; CG-I6 LA flush per window). Flag-off/GIL-on:
`gilOffProcess` false, predicate degenerates to
`sharedGCWindowedStagesEnabled()`, byte-for-byte the §27.S2
default (CG-I0).

### 7.2 C2 — collector continuity + activity-callback collection

Retires: `shouldCollectInCollectorThread` stays-false
(`Heap.cpp:1662-1676`), the `:1715` assert, activity gating
(`:808-810`), the async reroute (`:1626-1632`), the `:4571`
collector-quiesced assert (F38; `m_collectorThreadIsRunning`
ruled by CGA2 R10 — `:5136` conjunct KEPT).
Design: the collector thread becomes a STANDALONE-CLASS
conductor — election-equivalent tenure (GCL + GCA), NEVER
holding heap access (not a client; samples the §10.4 barrier,
licensed by §10B.2). `stopTheMutator`/`resumeTheMutator`
(`:2385`, `:2427`) stay DEAD (CG-I7); their unreachable
RELEASE_ASSERTs remain. Refactor surface = ANNEX CGA2 (BINDING):
nullable conductor-client on `conductSharedCollection`
(`Heap.cpp:4830`; per-use rows, F31); collector run loop
(`:335-360`) rewired to ticket waits on `m_threadCondition` (R7
carries the §3.4 guard, F24); election VMTraps poll
(`:4629-4644`) SKIPPED for the collector thread (R6). Activity
callbacks fire RCAC tickets + `m_threadCondition`-class notify;
SINFAC conducting stays as fallback (I15). Continuity = GCA MAY
span back-to-back granted tickets (`:2681-2706` analog); GCA+GCL
MUST drop when no ticket is granted (CG-I12 liveness, §9.1).

### 7.3 C3 — incremental + mutator-concurrent sweeping

Retires: IncrementalSweeper disablement (`:3002-3020`,
`:4905-4907`). Pre-req: the T8 BlockDirectoryBits audit
EXTENSION (stop-mode audit exists: `BlockDirectory.cpp:505-528`
skip; reader rows re-anchored via CGD7.4): re-audit every
accessor for OUT-OF-WINDOW access. Two structural rules replace
per-site reasoning:
1. Bit-vector RESIZE publication: `m_bits` reallocation (heap
   I5b: `addBlock` under BVL+MSPL) additionally RELEASE-publishes
   the new storage; lock-free readers (ANNEX CGB1 rows;
   re-anchored via CGD7.4) acquire-load the descriptor,
   tolerating a stale (smaller) bound — mid-phase blocks are
   clean by construction (§6.2(2)). CG-I8.
2. Sweeper identity: the IncrementalSweeper re-homes to a
   dedicated thread registered as a STANDALONE client
   (`markStandalone()`+ACT, heap §12.1 seam) holding access +
   MSPL per quantum (`sweepSynchronously` discipline,
   `Heap.cpp:1502-1518`); it participates in F8/windows like any
   client (CG-I13). In-lock allocation-path sweeps unchanged.

### 7.4 C4 — incremental mutator assist

Retires: `performIncrement` ISS early-return
(`Heap.cpp:4015-4023`) and the heap §5.4 world-stopped-only
debug-assert. Design: per-client assist visitor — GCH gains
`m_assistVisitor` (heap-allocated; (un)registered per
CG-I14/F34, pending-list only) and a per-client
`m_incrementBalance` (`:2000` reset folds at WND-open).
`performIncrement` routes `didAllocate` bytes (`:3200-3212`) to
the calling client's visitor; `performIncrementOfDraining`
(`SlotVisitor.cpp:532-590`) runs on the client's thread WITH
access, under its visitor's `m_rightToRun` —
`resumeThePeriphery`'s loop (`Heap.cpp:2352-2379`) and
`updateMutatorIsStopped` (`SlotVisitor.cpp:468-486`) already
handle N visitors. Assist visitors take NO §9.1(2)
checkpoint, live in no marker counter (§9.1(6)).
m_opaqueRoots/race-stack interplay is N-ary-safe (locked,
§2.3(b)).

## 8. Marking vs live mutators (charter item 5)

### 8.1 What stays in-window

Constraint solving stays WINDOW-CONFINED: `executeConvergence`
(`Heap.cpp:1948`) and all root constraints (conservative scan
`:1038-1096`; the `:1053` `ASSERT(!ISS || WSAC)` KEEPS) run only
at fixpoint windows (Riptide: Concurrent only drains). Likewise
`beginMarking`/`endMarking`, stack/register gathering (heap
§10.6), `finalize()` (`:2790`), and the §11 reclaim (`runSafepointHooksAndReclaim`, `:5034` — fires once per
cycle in the FINAL window; heap I11's legal-context list reads
"the cycle's last window", CG-I11). CONDITIONAL fast path (rev 9):
nothing-retired cycles early-return (`:5059-5060`) — the hook
pass still runs EVERY cycle; bump/stamp/compiler-suspension only
on retiring cycles (CG-I11 restated).

### 8.2 Cell-lock coverage audit (out-of-window draining)

Stage C1 makes `visitChildren` race mutators. The object-model
protocol is FROZEN and sufficient — om §6 butterfly/structure
rules, om I-series shape storage, ungil §N rulings
(N.1/N.2/N.5/N.6), `m_indexingTypeAndMisc` CAS (heap F5). This
spec adds the AUDIT obligation, not new rules: CG-A2 (executed
at C1 entry, recorded as ANNEX CGN1 rows; scope INCLUDES
runtime/-side visitor inputs, e.g. the atomic
`m_terminationException` visit `Heap.cpp:3765` — rev 9, CGD7.4)
walks every `methodTable()->visitChildren`-reachable reader of
mutator-mutable multi-word state and assigns {IN-WINDOW-ONLY, OM-PROTOCOL (cite), N-PROTOCOL (cite),
CELL-LOCKED (10a; tryLock + defer/re-visit, NEVER block — CGN1
N3; CG-I15), RACY-TOLERATED (justify)}. CELL-LOCK NO-PARK
(CG-I18, NORMATIVE): a JSCellLock (10a) holder must not release
heap access, pass a stop poll, or enter a conducting path.
Grounding + termination: ANNEX CGN1 N3 (SINFAC I6
`Heap.cpp:5216`). CG-A2 classifies every CELL-LOCKED row's
MUTATOR side against CG-I18; debug assert: cell-lock depth == 0
at SINFAC entry and the AHA park leg (CG-T5 arm). SlotVisitor
draining is reused except the §9.1(2) checkpoint/exit deltas.
Visitor-side allocation stays forbidden (heap I4(c)).

### 8.3 TID-rebias pin (F35)

ANNEX CGD5.1 (BINDING) GOVERNS. When gilOffProcess, the ungil
§D.1/D1R rebias block (`Heap.cpp:4946-4978`) runs inside the
FINAL window of a conducted FULL cycle — post-reclaim, strictly
before that window's CG-F4 ISB bump and WSAC/GSP clears, under
WSAC; NEVER after a WND-close. Per-cycle predicate when C1R
(replaces the `:4924-4940` aggregate): the FIRST Full cycle with
a Sealed snapshot runs it, single-shot; Eden cycles neither run
nor suppress (CGD5.1(3)). Satisfies D1/D1R verbatim (CGS1 note);
CG-I23; CG-T8 F35 sub-arm; flag-off/GIL-on dead/identical
(CG-I0).

## 9. Composition with the JSThreads stop protocol (charter item 6)

### 9.1 GC cycle vs §A.3 stop — the ordering pin

Today both serialize on GCL (`Heap::JSThreadsStopScope` — TWO
ctors, F47: blocking `Heap.cpp:5546-5566`, watchdog tryLock-poll
`:5568-5590`; `bytecode/JSThreadsSafepoint.cpp:250-357`);
a concurrent tenure is LONG — untouched it starves §A.3 conductors
into the 30s watchdog (`JSThreadsSafepoint.cpp:512`). PIN
(normative):

1. GCL is held by the GC conductor ONLY in-window or across
   the F28 handoff (§3.1/§3.4); between windows
   `JSThreadsStopScope` may interleave.
2. A foreign GCL holder mid-cycle must not race marking helpers:
   BOTH `JSThreadsStopScope` ctor overloads (F18 AMENDED by F47 —
   blocking `Heap.cpp:5546-5566` AND watchdog `:5568-5590`; both
   share the `!isSharedServer()` early-return), after acquiring
   GCL — watchdog ctor: after a SUCCESSFUL tryLock only, never
   per failed iteration — when `m_currentPhase != NotRunning`,
   call new `Heap::pauseConcurrentMarkingForForeignStop()`
   (MARKER pause only — the rule-7 sweeper gate is NOT
   phase-gated, F29); the shared dtor (`:5592-5596`) resumes
   BEFORE releasing GCL (dtor order NORMATIVE: no WND-open with
   paused markers). CALL SITES (F18/F47): the heap-OWNED
   ctors/dtor are the ONLY pause/resume sites, covering every
   construction (watchdog: `runtime/VMManager.cpp:577`; blocking:
   `bytecode/JSThreadsSafepoint.cpp:445`,
   `SharedHeapTestHarness.cpp:1039/:1073/:1107`); no foreign row
   (§13.3(b)). MECHANISM + COUNTER PROTOCOL:
   ANNEX CGP1 (BINDING) GOVERNS (F6/F14/F16/F17 — one batch =
   the CG-I12 bound; flag-off benign CGD2.1; CG-I16/I22). The
   §A.3 window may then jettison/patch (mutators via its
   fan-out; markers via this hook; C3 sweeper per rule 7); its
   ISB bump (ISB1.1) + rule 4 cover marker-visible frees.
2a. GCL FAIRNESS (F45; ANNEX CGD7.2 GOVERNS): the landed §A.3
   acquisition is an UNQUEUED 1ms tryLock poll (`:5584-5587`);
   the blocking WND-open re-acquire can starve it into the
   watchdog fail-stop (`JSThreadsSafepoint.cpp:512`). NORMATIVE:
   both stop-scope ctors register as foreign waiters
   (`m_foreignGCLWaiters`, relaxed atomic; inc pre-attempt, dec
   once held); WND-open RE-ENTRY defers its blocking acquire
   while nonzero (access-released sleep/re-check; GCL is free
   between windows, so the waiter wins). Election loser's timed
   wait (`:4627`) unchanged. CGS2.3 becomes STRUCTURAL; CG-T8
   VERIFIES. CG-I26.
3. Order pin per cycle edge: WND-open (re-entry) is
   access-released -> GCL -> GSP (§3.1(a)-(c) + carve-outs);
   WND-close clears GSP/WSAC BEFORE releasing GCL. Hence at most
   one of {GC window, §A.3 window} open at any instant —
   single-owner GCL is the proof. The HBT4 order EXTENDS to
   window re-entry (the only blocking GCL acquire; election/poll
   stay tryLock-only); the §10.2 GCL-busy rule (`:4616-4628`)
   unchanged. A mid-cycle GC requester parks as a follower on
   GCA (set whole-cycle, §3.4), not on GCL.
4. No reclamation outside the cycle's final window: heap I11 +
   jit R4/CS4 refusal stand VERBATIM — a mid-cycle §A.3 stop only
   ENQUEUES an RCAC ticket (heap §13.10a; served by the same
   CONDUCT call's ticket drain, `Heap.cpp:4925-4937` — possibly
   an F28 successor cycle). Memory paused markers can see is
   freed only at epoch reclaim (final window), via
   epoch/quarantine-routed jettison paths (om §6 bar; contract
   hooks once-per-CYCLE), OR — F43 carve-out — by the landed
   AB-10 conductor weak-sweep license (`WeakSet.cpp:81/:106`,
   `Heap.cpp:3339`: the §A.3 conductor's in-window allocation
   slow path may free logically-empty WeakBlocks mid-cycle).
   SOUND per CGD7.1(d): markers are fully paused (rule 2) before
   the window's work, so no in-flight visit holds WeakBlock
   interior pointers; such blocks hold no live impls; marker
   resume postdates the frees.
5. GC keep-parked vs §A.3 parking stays disjoint (ungil
   §A.3.8): GC stops set client-visible state (GSP/F8); §A.3
   stops set none. AHA's three gates (GSP `Heap.cpp:5836`, §A.3
   word `:5865`, mode machine `:5886`) compose unchanged — each
   re-loops; a client wakes only when NO window pends.
6. C4 assist visitors vs §A.3 (F14):
   `performIncrementOfDraining` maintains no marker counters; its
   `SlotVisitor.cpp:583`-area safepoint takes NO shouldPause
   checkpoint. An assist mutator is bounded by ONE increment,
   then parked by the §A.3 MUTATOR fan-out at its next stop
   poll. CG-T10 arm.
7. C3 sweeper vs §A.3 (F26/F29/F30; CGD3.2/CGD4.2): ANNEX CGP1
   SWEEPER EXTENSION GOVERNS — flag-keyed
   (`useSharedGCIncrementalSweep` && registered), NO phase gate
   (sweeping runs BETWEEN cycles); quantum ENTRY refused while
   the pause flag is set (`m_sweeperInQuantum` under
   `m_markingMutex`; access/MSPL only after gated entry); ctor
   predicate `!m_sweeperInQuantum || acked`; dtor clears both
   pre-GCL-release; bound <= one quantum. CGS1 row 11; CG-I13;
   CG-T7 arms.
8. The §A.3 conductor is a FULL CLIENT during its window (F43;
   ANNEX CGD7.1 GOVERNS — rev 8's allocation-free closure model
   was FALSE; CGS2.4(a)'s "ALLOCATION-FREE" STRUCK, both sides):
   AB-21 re-acquires its OWN client access inside the GCL
   bracket (`VMManager.cpp:631-646`; Class-A fire bodies take
   DeferGC + write barriers); AB-10 licenses in-window weak
   sweeps (rule 4). Rules (walk CGD7.1): (a) in-window barriers
   -> its OWN CMS under C1R, drained at next WND-open; (b)
   in-window allocation under `m_isMarking` is black per §6.2
   (phase stable under its GCL hold; TLC flushed at next
   WND-open); (c) sweep-to-freelist under MSPL inside the
   window — CG-I5 unaffected (its GCL hold excludes any GC
   WND-open mid-sweep). CG-I25.

### 9.2 EXIT1 teardown mid-cycle

A spawned thread's exit (ungil §B.2: RHA -> TEARDOWN mark -> DCT ->
unregister) may land between windows of a live cycle. Rules (the
CMS/fence steps no-op when !C1R — F33, CG-I0):
1. DCT/`~GCClient::Heap` runs the landed teardown FIRST (access
   re-acquire for `lastChanceToFinalize`'s MSPL section, TLC
   `stopAllocatingForGood`), then PERMANENTLY drops access, THEN
   (strictly after its last possible barrier) flushes its CMS
   into the shared mutator stack (under `m_markingMutex`), then
   epoch=MAX and HCS remove (in the GBL/!WSAC section heap I13
   requires). NO dead-state publication (rev 2's clause
   DELETED — F36; rationale = rev 7 log). C4 DELTA (F25/F34;
   CGD3.3/CGD4.3 GOVERN): assist visitor HEAP-ALLOCATED; ACT/DCT
   never read phase, never mutate the visitor set — ALWAYS
   enqueue (deferred unregistration with ownership transfer;
   exit CANCELS a pending §9.3(3) registration) under
   `m_parallelSlotVisitorLock`; pending list applied at WND-open
   pre-walk, or quiesced per CG-I14. Rev 1's flush-first order
   REJECTED (F2); the pre-remove flush completes while still
   registered — never a registered client with unreachable CMS
   (CG-I17). CG-T9 arm.
2. HCS `remove` blocks inside windows (heap I13); removal
   between windows is LEGAL once rule 1 ran — the next fixpoint
   window re-converges with one fewer conservative root set
   (refs barriered/CMS-drained, RHA-published, or garbage).
3. `~VM` (EXIT1.9 fence) composes: the main client survives all
   spawned exits; §10D ISS reversion requires NotRunning (heap
   §10D step 1), so a cycle never straddles a protocol switch —
   the revert OUTCOME only; the landed PRE-CHECK (`:5126-5140`)
   is restructured per the §3.4 `:5126` row (F11; CGD1.2).
4. The detaching thread is never the live conductor: §3.7
   forbids EXIT1 on `m_gcConductorThread` mid-cycle (release
   assert); CG-T9 attempts it.
5. SERVER TEARDOWN (F39): ANNEX CGD5.2 (BINDING) GOVERNS. Rule
   3 covers the GIL-on revert OUTCOME only (gilOff: `:5103-5110`
   early return). ~VM after the EXIT1.9 fence, BEFORE any
   access-holding teardown: disable elections/collector wakes;
   acquire GCL with the §3.4 back-off (teardown = a §3.4 site)
   until {GCL held, NotRunning, tickets served-or-refused}; join
   the collector; only then the access-holding tail. Teardown =
   the CG-I14 quiesced point. Flag-off byte-for-byte. CG-I24;
   CG-T6 arm.

### 9.3 Mid-cycle client ATTACH

`HeapClientSet::add` blocks only INSIDE windows (I13: insert
under GBL with !WSAC, `HeapClientSet.h:54-68`), so a `Thread()`
spawn's ACT (ungil §B.1) may land BETWEEN windows of a live
cycle (§10B.4-quiescence alternative REJECTED — F1). Rules
(NORMATIVE):
1. Fence init handshake (!C1R: no-op — copies unrouted, CG-I0;
   F33): inside the GBL/!WSAC section that publishes the client
   in HCS, BEFORE the insert, the attacher copies the server
   master fence/threshold into its client
   copies and stamps `m_fenceEpochSeen = FEP`. Happens-before:
   master mutates only in-window (WSAC under GBL); the add runs
   under GBL/!WSAC — snapshot untorn, never stale; a
   live-marking attachee starts RAISED; CG-I3's close assert
   holds; the §5.3(3) pin subsumes the values, the FEP stamp
   stays.
2. `m_isMarking` visibility: the client's first AHA performs
   the seq_cst GSP load (`Heap.cpp:5836`) and the GBL section
   above pairs with the in-window flip (§6.2(1)); allocation
   before ACT completes is impossible (ungil §B.1).
3. C4 assist visitor: ACT ALWAYS ENQUEUES registration to the
   pending list (NO phase read — F34), applied per §9.2(1).
   EXIT1 in the gap CANCELS it (F25). Until applied, the client
   cannot assist (`performIncrement` checks registration); its
   barriers/CMS are live immediately.
4. didRun = false, CMS = empty at ACT (zero-init).
5. CG-T9 attach-storm arm (§12).

## 10. Lock & fence deltas

Lock table (heap §6 master) deltas — additions only, no
re-ranks. BOTH rows are PROPOSED SPEC-ungil §LK amendments (the
ONE merged order, canonical for U20; SPEC-ungil.md:867-925),
SUPERSESSION-PENDING + adoption gate §13.5(1) (F41; FULL rows +
acyclicity walks = ANNEX CGS2.1-2, BINDING):
- LK.9c `GCH::m_mutatorMarkStackLock` — TERMINAL leaf, INSIDE
  `m_markingMutex`; MAY be taken with ranks 7-9b held (§5.2
  append path; CG-I10/F21) — §LK.8 destructor-leaf-class shape,
  superseding heap §6's "never 7-9b" leaf row, BOTH sides.
- LK.9d marking-internal group (`m_markingMutex`,
  `m_parallelSlotVisitorLock`, `m_raceMarkStackLock`, visitor
  `m_rightToRun`): INSIDE GCL/GBL; newly MUTATOR-reachable
  out-of-window (SINFAC-tail donation; pending-list enqueue);
  disjoint from MSPL-9b except landed in-window uses. U20 PROPER
  extends to both rows — CG-T2 IS that extension. The composed
  NL > GCL > `m_markingMutex` > CMS chain is CHARTERED and
  U20-linted — walk = CGS2.2.
Fences (heap §7 deltas):
- F4/F7/F8 unchanged; F8's Dekker proof is per-WINDOW — cadence
  only.
- CG-F1: FEP store(release) in-window; client copies
  conductor-written in-window; clients read own copy plain (the
  window barrier synchronizes).
- CG-F2: `m_isMarking` flips in-window only; published by the F8
  resume edge (§6.2(1)).
- CG-F3: directory-bit storage descriptor release-published on
  resize / acquire-loaded by out-of-window readers (§7.3(1)).
- CG-F4: ISB generation bump at EVERY WND-close when
  gilOffProcess (§3.2), before the GSP clear, mirroring
  `Heap.cpp:5000-5016`.

## 11. Invariants (each needs a test or assert — §12)

- CG-I0: all §13.2 flags off => shared-mode behavior identical
  to today's §10 protocol (one window per cycle, §3.6; "today" =
  43fd5fb94387, F44); `!ISS` => legacy byte-for-byte (heap
  I10).
- CG-I1: at most one GC window OR one §A.3 window open at any
  instant (single-owner GCL, §9.1(3)); heap §10's
  heap-resume-before-VMM-resume holds per window.
- CG-I2: every CMS append happens on its owning client's
  access-holding thread under its CMS lock (exempt: in-window
  conductor-context appends — F31/F43); every drain under
  `m_markingMutex` (conductor in-window or owner
  out-of-window).
- CG-I3: fence RAISES are window-complete (all copies
  republished pre-close); LOWERS only in the final window
  post-termination; `m_fenceEpochSeen == FEP` for every client
  at every WND-close (debug assert).
- CG-I4: `m_isMarking`, `m_collectionScope`, phase transitions,
  marking/newlyAllocated version bumps mutate in-window only —
  phase fields: before the closing GCL release (§3.2 F22).
- CG-I5: no sweep-to-freelist, steal, or `addBlock` straddles a
  window (MSPL implies access, barred from open windows).
- CG-I6: per window, each LA is stop-flushed exactly once,
  resumed exactly once (non-idempotence preserved).
- CG-I7: `stopTheMutator`/`resumeTheMutator`/`m_worldState`
  stop bits stay unreachable when ISS, ALL stages (C2 uses
  windows).
- CG-I8: every BlockDirectoryBits access is in-window, under
  BVL, under MSPL, an acquire-read of release-published storage
  tolerating a stale bound (CGB1 rows), or `!ISS`.
- CG-I9: `m_didRunSinceLastWindow` written only by its owning
  access-holding thread; folded+cleared in-window.
- CG-I10 (F21): (1) `m_markingMutex` never acquired with any
  rank 7-9b lock held; (2) the CMS lock is a TERMINAL leaf and
  MAY be taken with 7-9b held (append path — no cycle); (3)
  `m_markingMutex` > CMS only at the 7-9b-free drain/donation
  sites (WND-open drain; SINFAC tail: I6).
- CG-I11: heap I11 verbatim — epoch bump/reclaim only in the cycle's
  FINAL window (legacy `runEndPhase` when `!ISS`); never from a
  §A.3 stop; the HOOK PASS runs once per cycle; bump/stamp/
  compiler-suspension only on retiring cycles (`:5059-5060`
  fast path, §8.1; rev 9).
- CG-I12: GCL never held by the GC conductor between windows
  (F28 handoff excepted); GCA without a granted ticket is
  dropped (C2 bound) — a §A.3 conductor's GCL wait <= one window
  + one marker-pause batch (§9.1(2)) + (F28 successor) the
  re-stop + its FIRST window; STRUCTURAL via §9.1(2a) (F45),
  CG-T8 VERIFIES. Wait BUDGET vs the watchdog (incl.
  nativeaffinity BL1.6/BL1.8 terms) stated ONCE: ANNEX CGS2.3
  (F42; rev 9).
- CG-I13: every sweeping thread is a registered client holding
  access + MSPL per quantum; quantum entry flag-gated; §A.3
  stops acked per §9.1(7) (phase-independent, F29/F30).
- CG-I14: the `forEachSlotVisitor` set mutates only at WND-open
  pending-list application or quiesced (GCL held + NotRunning),
  under `m_parallelSlotVisitorLock`; ACT/DCT only ENQUEUE (F34);
  every visitor has a live owner (client, conductor, helper
  pool, or pending list).
- CG-I15: marker threads never BLOCK on a JSCellLock (tryLock +
  revisit/race-stack only), never allocate from the shared
  heap.
- CG-I16: `pauseConcurrentMarkingForForeignStop()` terminates
  without acquiring any api-rank or heap rank >=7 lock; paired
  resume in the same `JSThreadsStopScope`.
- CG-I17: a detaching client's CMS is empty when it leaves HCS;
  the final flush postdates its last possible barrier (§9.2(1));
  its assist visitor is out of the set — or pending-list owned —
  before its storage dies (F25).
- CG-I18: no thread releases heap access, passes a stop poll,
  or enters a conducting path holding a JSCellLock (§8.2);
  debug: cell-lock depth == 0 at SINFAC entry / AHA park.
- CG-I19: conductor tenure is a closed loop (§3.7): no JS, no
  RHA/AHA, no EXIT1, access-released throughout; between-window
  waits = donateAll+waitForTermination (condvar, no counter,
  never visitChildren); blocking GCL acquires only
  access-released (§3.1); `m_nativeLockDepth == 0` at conducting
  entry (F40 — nativeaffinity BL1.8, both sides; debug assert).
- CG-I20: a mid-cycle attachee has fence/threshold copies +
  `m_fenceEpochSeen` stamped inside the publishing GBL section,
  before its first allocation (§9.3(1)).
- CG-I21: at most one conductor per cycle: every GCL tryLock
  site (§3.4; F47 watchdog-ctor row excepted as PROCEED) backs
  off when `GCA && m_currentPhase != NotRunning`;
  `m_gcConductorThread` stamped once per cycle (restamp only in
  NotRunning wind-down) and cleared ONLY by its owner (F20).
- CG-I22: a paused/exited helper is in neither counter (F17); a
  paused helper holds no local work (donateAll);
  `didReachTermination` is false while
  `m_pausedParallelMarkers != 0` (§9.1(2)).
- CG-I23: the Sealed->Restamped flip + D1R fires run only
  inside a FULL cycle's FINAL window (post-reclaim,
  pre-ISB-bump/GSP-clear, WSAC); one rebias per Sealed snapshot
  (§8.3).
- CG-I24: server teardown destroys no conduct state and runs no
  access-holding section until {wakes disabled, GCL held,
  NotRunning, tickets quiesced, collector joined} (§9.2(5));
  EXIT1.9 precedes it.
- CG-I25 (F43): a mid-cycle §A.3 conductor is a full client per
  §9.1(8): barriers to its own CMS (C1R), black allocation per
  §6.2, MSPL sweeps/licensed weak sweeps only; no other free of
  marker-visible memory.
- CG-I26 (F45): the WND-open blocking GCL acquire is never
  issued while `m_foreignGCLWaiters != 0`; waiter
  register/deregister brackets every stop-scope lock attempt.
- CG-I27 (F46): the conductor's AtomStringTable is installed
  only in-window (per-window install/restore; debug null between
  windows); no AtomString ops between windows.

## 12. Verification ladder + TSAN charter (charter item 7)

Per-stage rungs; a flag defaults on only after its rungs AND
all earlier stages' re-run green.

- CG-T1 (C0): CGA1 audit executed; grep-lint clean (incl. the
  F47 watchdog-ctor row); CG-I0 gate — flags-off corpus +
  `$vm.sharedHeapTest` byte-identical; BENCH.md flags-off delta
  = 0 (heap I10 bar). F33/F44: routing C1R-gated — flag-off
  keeps the landed multi-producer append (`:1499`) + server
  fence reads.
- CG-T2 (C0): U20 lint EXTENDED to the §10 LK.9c/9d rows (F41;
  gate §13.5(1)), encoding the three F21 clauses; CMS/
  markingMutex order litmus + CGS2.2 chain litmus.
- CG-T3 (C1): barrier storm + TSAN no-JIT + amplifier arms; F19
  sub-arm (FULL charter = ANNEX CGT1.5, MOVED rev 9, verbatim).
- CG-T4 (C1): allocation/steal storm during marking; endMarking
  liveness assert (= ANNEX CGT1.6, MOVED rev 9).
- CG-T5 (C1): CG-A2 audit executed; per-row arms; CG-I18 storm
  (= ANNEX CGT1.7, MOVED rev 9).
- CG-T6 (C2): collector-continuity churn (= ANNEX CGT1.3, MOVED
  rev 8): RCAC storms; activity callbacks; SINFAC fallback;
  R7/F24 guard arm; R10/F38 arm; F39 ~VM-vs-cycle arm.
- CG-T7 (C3): ANNEX CGB1 audit executed (= ANNEX CGT1.4, MOVED
  rev 8): sweep quantum vs window race; sweeper churn;
  F26/F29/F30 arms.
- CG-T8 (all): JSThreads-stop interleaving (= ANNEX CGT1.1,
  MOVED rev 8, + the F40 arm):
  haveABadTime/jettison mid-cycle arms incl. the WND-open GCL
  acquire, a between-windows parked conductor, helpers mid-batch
  (CG-I22); F17/F18/F20/F22/F28/F35 sub-arms; F40 sub-arm
  (BL1.8 NL drop: siblings PROGRESS mid-cycle, depth-restored
  reacquire after the `:5031` tail); GC-requester storm
  (CG-I21); F45 arm (window storm vs registered waiter: CGS2.3
  verified, no watchdog); F43 arm (mid-cycle §A.3 conductor
  allocates/barriers/weak-sweeps, CG-I25); F46 arm (atom-table
  null between windows, CG-I27); `jsThreadsStopVsGCRequester`
  re-run per stage.
- CG-T9 (all): ATTACH/EXIT1 churn during forced concurrent
  cycles (CG-I17/I20; FULL charter = ANNEX CGT1.2, MOVED rev 8):
  attach storm; finalizer-side stores during marking;
  F36/F11/F25/F34 arms; conductor-thread exit release-assert;
  `clientChurnVsGC` + `issRevertChurn` re-run.
- CG-T10 (C4): assist storm; §A.3 stop mid-assist-increment
  (= ANNEX CGT1.8, MOVED rev 9).
- CG-T11 (all): §2.4 diagnostics as stage-gated debug asserts;
  A4-site walk with executing CodeBlocks (F37) (= ANNEX CGT1.9,
  MOVED rev 9).
- TSAN charter: every CG-T3..T10 arm runs under the TSAN no-JIT
  target; suppressions limited to documented RACY-TOLERATED rows
  (CGA1/CGN1); any other report blocks the stage. GIL-off rungs =
  pinned ladder commands (UNGIL-HANDOUT) + the stage flag.

## 13. Files, options, integration

1. Owned: `heap/**` only, same set as heap §12 +
   `GCThreadLocalCache`/`HeapClientSet`/`GCSafepointEpoch`;
   `JSTests/threads/congc-*.js`;
   `docs/threads/INTEGRATE-congc.md` (manifest, written first).
2. Options (`runtime/OptionsList.h` via manifest, heap §13.2
   shape): `useConcurrentSharedGCMarking` (C1),
   `useSharedGCCollectorThread` (C2),
   `useSharedGCIncrementalSweep` (C3), `useSharedGCMutatorAssist`
   (C4) — default false; validation enforces the §7 prefix rule
   + `useSharedGCHeap`. Plus
   `sharedGCMutatorMarkStackDonationThreshold` (Unsigned;
   default per §5.2(ii)).
3. Chartered out (INTEGRATE rows, not owned): (a) per-client
   barrier-address JIT emission — jit/ungil owners (§5.3(3) F19
   server pin until landed); (b) DELETED (F18; §9.1(2));
   (c) VMManager deltas: none — `VMManager.cpp:577` covered via
   the watchdog ctor (F45 counter lives in Heap).
4. Supersession discipline (REWRITTEN rev 8, F42; rev 9 strikes
   CGS2.4(a) ALLOCATION-FREE, F43; FULL text = ANNEX CGS1/CGS2 +
   rev 8 log): kill-switch retires stay flag-gated (heap
   Deviation 4 I10). Touched frozen specs: SPEC-heap (CGS1),
   SPEC-ungil (CGS2, every row SUPERSESSION-PENDING behind a
   §13.5 gate). At freeze each folds into its history, both
   sides.
5. ADOPTION GATES (rev 8; rows NOT in force until the named
   owner lands the cross-cite — nativeaffinity §9 semantics; all
   still OPEN at rev 12): (1) SPEC-ungil §LK rows LK.9c/9d + U20
   extension (CGS2.1-2) — BLOCKS the §5.2 CMS lock and C1.
   (2) the §A.3 rule-5/HBT4.5 amendment (§9.1(2) pause + dtor
   order) + the wait bound vs U32/watchdog (CGS2.3, now
   F45-structural; shared with nativeaffinity BL1.6/BL1.8) —
   BLOCKS C1. (3) the HBT4 order extended to window re-entry
   (CGS2.4) — BLOCKS the §3.1 re-entry blocking acquire.
   (4) nativeaffinity BL1.8 NL-drop (F40; CG-I19 depth==0 =
   this side) — BLOCKS C1 in gilOff configs.

## 14. Ordered task list

- CG-1 (C0 infra): window split of `conductSharedCollection`
  (WND-open/close helpers, §3.1 order incl. carve-outs + F28;
  GCL per-window; GCA tenure + owner; §3.7 closed loop; §3.4
  guards at the landed tryLock sites incl. the `:5126`
  restructure; F45 waiter counter); CG-T1/T2.
- CG-2 (C0 infra): CMS + per-client fence/didRun fields +
  window fold/republish loops; consumers re-pointed (C1R, F33);
  GIL-phase JIT address pin (§5.3(3)).
- CG-3 (C1): retire the three C1 kill switches behind the flag;
  marker-helper between-window scheduling + the §3.7/§7.1 ISS
  conductor wait; §9.1(2)/(2a) pause pair + fairness counter
  (F18/F45/F47: both ctors); per-window atom table (F46); §8.3
  rebias pin (F35); §9.2 exit order + §9.3 attach handshake;
  donation threshold option; CG-A2 audit -> CGN1;
  CG-T3/T4/T5/T8/T9/T11.
- CG-4 (C2): collector-thread conductor; RCAC routing;
  continuity bound; CGA2 R10 ruling (F38); §9.2(5) teardown
  (F39); CG-T6 + re-runs.
- CG-5 (C3): CGB1 audit; bit-storage release/acquire
  publication; sweeper-client re-home + §9.1(7) entry-gated
  pause (F29/F30); CG-T7 + re-runs.
- CG-6 (C4): assist visitors/balances; `performIncrement`
  re-enable; CG-T10 + re-runs; BENCH.md stage gates recorded.
- CG-7: freeze pass (REQUIRES a clean adversarial round over
  the rev-7/8 deltas ONLY — rev 9 log list; the rev-9 deltas
  are COVERED by rev 10's clean directed pass, ruling recorded
  in the rev 11 log) — CGS1 + CGS2 supersessions recorded
  both sides; §13.5 gates: 4/4 CLOSED owner-side
  06-10 (ungil r33-35, na r9-11; verified; C1 unblocked;
  C0 carries the two [r34] code items); size-cap
  check; rev history updated.

Done = every CG-I has a test/assert (§12); flags-off bench +
behavior gates green; all four stage flags green on the ladder;
INTEGRATE-congc.md matches §13.
