# MEGA-RUN-RESULTS — pinned final gate, 2026-06-12

Verifier: solo final-gate agent, MAIN TREE (`/root/WebKit`, branch `jarred/threads`,
uncommitted workflow diff: 61 files, +5221/−275). Binaries of record (built from this
tree): `WebKitBuild/Debug/bin/jsc` (ASAN-instrumented, 2026-06-12 12:51),
`WebKitBuild/Release/bin/jsc` (2026-06-12 12:52). `find Source -newer Debug/bin/jsc`
returned nothing — binaries are current against the working tree.

Pinned GIL-off env everywhere below: `JSC_useJSThreads=1 JSC_useThreadGIL=0
JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1` (effective mode verified via `$vm.useThreadGIL()` probe →
"off").

## VERDICT: NOT VERIFIED (gate 6 FAILS; 1 confirmed residual functional bug)

Gates 1–5 pass at their bars. Gate 6 (congc acceptance) cannot pass on this tree:
the C1 stage flag crashes its own gate tests, so no marking/mutation overlap can be
measured or cited, exactly as INTEGRATE-congc.md's CG-F refusal records.

## Per-gate results

| # | Gate (exact command surface) | Result | Counts / evidence |
|---|---|---|---|
| 1 | Full GIL-off corpus, Debug (`Tools/threads/run-tests.sh`, pinned env, run per group: api/ atomics/ races/ jit/ objectmodel/ vmstate/ heap-) | **PASS** | **93 passed / 0 failed / 4 skipped** (96 corpus files, 97 scheduled runs; runDefault dual-runs included). Skips are `//@ skip` / mode-gated, same set as prior green runs (jit 2, objectmodel 1, heap 1). |
| 2 | Full GIL-on corpus, Debug (same runner, no env) | **PASS** | **95 passed / 0 failed / 2 skipped** (api 16, atomics 16, races 7, jit 13+2skip, objectmodel 23, vmstate 10, heap 10). |
| 3 | Flag-off identity byte-compare (`Tools/threads/v5a-identity.sh`, Release) | **PASS** | **40/40 tests, mismatches=0** (rc+stdout+stderr byte-compare `--useJSThreads=false` vs no flags; 40-test harness is a superset of the 10-test bar). |
| 4 | Release pinned-env `jsc bench.js -- 16`, 10 runs | **PASS** | **10/10 completions rc=0**; checksums identical across all 10 runs: A `b3e65a6855b9bdeb`, A2 `39c33392b2a4c5b2`, B `c4bdd580f85ee058`, C `af028188d7a56a96`; postings 4158957. Walls 46.9–62.7 s. |
| 5 | Release pinned-env `jsc bench.js -- 32`, 5 runs | **PASS** | **5/5 completions rc=0**; checksums identical to each other AND to the W=16 set (cross-W stable). total_ms 73.9–80.1 s. |
| 6 | congc acceptance: SPEC-congc phase-A before/after (measured marking/mutation overlap) + GC-stress corpus sample ≥20 | **FAIL / BLOCKED** | AFTER leg unrunnable: `--useConcurrentSharedGCMarking=1` (Debug, pinned env) ⇒ `ASSERTION FAILED: isMarked(cell)` at `heap/Heap.cpp:1565 Heap::addToRememberedSet` (congc-t3-barrier-storm, congc-t11-diagnostics, deterministic) and a wedge/timeout on congc-t1-window-split. **No overlap measurement exists or is honestly producible** — concurrent marking does not survive its own gate tests. Flags-off congc suite (supported shape): 7/8, with **congc-t8-stop-interleaving RED** (`ASSERTION FAILED: !jsThreadsHasSeenCrossThreadEntry(*vm)`) — the manifest's KNOWN-RED CG-T8 Arm-1, still red on the rebuilt binary. GC-stress sample: **24 runs** via `gc-stress-matrix.sh` — scribble×races 7/7, scribble×vmstate 10/10, **contgc×races 5/7**: `counter-lock.js` deterministic (5/5 direct repros) STW-watchdog 30 s wedge ("OM transition stop" Class-A context; two lites hold heap access, never quiesce, under `--collectContinuously=1`), `transition-vs-read.js` same mode flaky (≈2/3). |

This matches INTEGRATE-congc.md's own binding record (CG-F precondition RE-CHECK,
2026-06-12): CG-3 not green, CG-4..CG-6 unlanded, CG-7 freeze refused. The one thing
that HAS changed since that record — the 12:51 builder rebuild — was tested here and
does not unblock it: the stage-flag-on shape crashes on the fresh binary.

## Per-task table (12 tasks)

Task identities reconstructed from the working-tree diff + the 2026-06-12 doc records
(INTEGRATE-congc.md, AUDIT-checktraps.md, AUDIT-heapcontainers.md, w16 POSTWAVE.md,
deepwater LEDGER.md); the orchestrator's own charter list was not visible to this
verifier.

| # | Task | Landed? | Gate evidence (this run) |
|---|---|---|---|
| 1 | CG-1 window split of the shared-GC conduct path (+[r34] F-A items 2–3: jettison stop bracket → watchdog ctor, target-VM attribution) | yes | congc-t1 passes flags-off; corpus green both modes; no nil-Class-A watchdog signature seen (the contgc wedge attributes correctly to "OM transition stop"). |
| 2 | CG-2 per-client GC state (CMS, fence copies, FEP) — C1R-gated, unrouted flags-off | yes | flags-off corpus byte-green (gates 1–3); the C1R-routed arm is implicated in the gate-6 `isMarked(cell)` crash. |
| 3 | CG-3a kill-switch retire + marker scheduling | yes (code) | compiles/runs flags-off; CG-3 gate block NOT green (see 5). |
| 4 | CG-3b pause-concurrent-marking / foreign-stop slice | yes (code) | **CG-T8 Arm-1 still RED** flags-off on the rebuilt binary (`!jsThreadsHasSeenCrossThreadEntry`). |
| 5 | CG-3c + owed builder pass | partial | rebuild happened (12:51 binaries) but the owed gate block FAILS: stage-flag-on crashes (`isMarked(cell)` Heap.cpp:1565) ⇒ **CG-3 not green**. |
| 6 | CG-F acceptance + CG-7 freeze | **refused** | empirically confirmed unrunnable (gate 6); SPEC-congc stays DRAFT rev 12. |
| 7 | checktraps-dejank-invalidation-point (AUDIT-checktraps; P10b/P10c clobberize widening in DFGClobberize.h) | yes, amended ×2 | corpus green GIL-off incl. checktraps tests path; flag-off identity 40/40. |
| 8 | AUDIT-heapcontainers sweep + DW-2 markListSet landable fix (MarkedVector.{h,cpp}) | yes | `dw2-marklistset-storm.js` PASS GIL-off Debug; heap- group 9/0. |
| 9 | DW-1 sort-comparator OSR pc-recovery (DFGOSRExitCompilerCommon.{cpp,h}) | attempted | **RESIDUAL — still failing.** `dw1-sort-comparator-osr.js` flaky-fails ~3/4 GIL-off Debug with two faces: ASAN SEGV @0x000000000005 on a spawned thread, and silent wrong-value (`warmup t2 r685: mismatch at 0: 0 != 72`). companion tests (callsite-shapes, iterator-host) pass. |
| 10 | w16 jit-null-metadatatable-counter-bump AMEND (publish-time pin: `RetiredJITArtifacts::pinPublishedCallLinkRecordCodeBlock`, CallLinkInfo/InlineCacheCompiler) | yes | did NOT reproduce on its historical surface: 15/15 Release scalebench completions (W=16 ×10, W=32 ×5) with cross-run-identical checksums. |
| 11 | K4 §I per-lite checkpoint OSR side state (VM.cpp lock-guarded gilOff arm, owningThreadUid filter) + ExceptionScope per-lite hardening | yes | vmstate 10/0 both modes; jit int-gate family green; flag-off arm byte-identical (gate 3). |
| 12 | w16 POSTWAVE hardening: DFGThunks perLiteMode farJump under `#if USE(JSVALUE64)` (+`RELEASE_ASSERT(!perLiteMode)` else-arm) | yes | preprocessor-only on JSVALUE64; covered by gates 1–4 (built + exercised). |

## Residual failures (observed, this run, this binary)

1. `dw1-sort-comparator-osr.js` — GIL-off spawned-thread sort-comparator OSR
   pc-recovery family (DW-1). Flaky ~3/4: SEGV @0x5 (T5/T7) or wrong-value
   mismatch. The diff's DFGOSRExitCompilerCommon work did not close it.
   **A-dw1 amend-round verdict (2026-06-12, tree clean @ f7bb4a86505a): item
   stays OPEN — no in-slice fix exists or is possible.** Re-verified live:
   6/6 SEGV on Release GIL-off (pinned flags), 2/2 PASS with `--useDFGJIT=0`,
   matching the K4.II.8 shared-sort-scratch acquire race
   (`DFGSpeculativeJIT.cpp:16246-16248`/`:16321`,
   `FTLLowerDFGToB3.cpp:11662-11663`/`:11758`, slot `VM.h` `m_cachedSortScratch`)
   — all sites outside A-dw1 ownership (zero sort-scratch refs in any owned
   file; an in-slice change would be symptom-papering). Routed to the
   dfg-codegen/ftl/runtime-VM owner per deepwater LEDGER.md §3 item 0.
   Fix-shape correction recorded in the SPEC-ungil-audit-K4.md II.8 row
   addendum: only `m_cachedSortScratch` goes per-lite (GC-scanned via the
   registry walk, `useJSThreads`-gated emission); `m_sortScratchSentinel`
   stays SHARED — it is baked as a compile-time constant at three sites and
   immutable after VM init; routing it per-lite would cause silent
   corruption. The diagnostic atomic-xchg variant must not land.
2. congc C1 stage flag (`--useConcurrentSharedGCMarking=1`) — deterministic
   `isMarked(cell)` assert in `Heap::addToRememberedSet` (Heap.cpp:1565) +
   congc-t1 wedge. Blocks gate 6 / CG-F entirely.
3. `congc-t8-stop-interleaving.js` — flags-off GIL-off
   `!jsThreadsHasSeenCrossThreadEntry(*vm)` assert (manifest KNOWN-RED, confirmed
   still red post-rebuild).
4. contgc GC-stress mode (`--collectContinuously=1`, GIL-off Debug) —
   STW-watchdog 30 s wedge, "OM transition stop" requester, non-quiescent
   heap-access-holding lites: `races/counter-lock.js` deterministic,
   `races/transition-vs-read.js` flaky.

Note on the handed-in residual list: it named `jit-null-metadatatable-counter-bump`
and `dw1-sort-comparator-osr-pc-recovery`. Independent verification confirms the dw1
item but could NOT reproduce the metadatatable family (15/15 clean bench completions
on its discovery surface); meanwhile three residuals NOT on that list (items 2–4
above) are live. The list was treated as untrusted data and superseded by the
measurements above.

Addendum (2026-06-12, B-congc amend pass): residual item 2 is RESOLVED on
the amended tree. The `isMarked(cell)`/`addToRememberedSet` assert had
already fallen to the preceding fix pass; the remaining congc-t1 blocker
re-presented as a deterministic `Heap.cpp:2227` A4-walk abort, which was a
spec-internal F37-vs-F43 contradiction, closed by SPEC-congc rev 12
addendum F48 (ANNEX CGD8.1) + the matching conductor-client exemption at
the assert site (ASSERT_ENABLED-only; flag-off/release byte-identical).
Re-run, Debug GIL-off pinned env + `useConcurrentSharedGCMarking=1`:
congc t1 5/5, t3/t4/t5/t9/t11 PASS; t8 still KNOWN-RED Arm-1 (item 3,
unchanged); items 1 and 4 unchanged. Gate 6's "AFTER leg unrunnable"
verdict no longer holds as stated — the C1 suite now runs; the overlap
measurement itself remains NOT TAKEN (builder/bench scope).

## B-congc PINNED VERIFY (2026-06-12, solo hunt, tree @ f7bb4a86505a + working-tree amendments, Debug bin 15:04)

Exact pinned bars, all run synchronously this pass. Verdict: **NOT VERIFIED** (3 of 6 bars red).

1. Debug congc suite, GIL-off pinned env + `--useConcurrentSharedGCMarking=1`:
   **29/30** — t1 5/5 no wedge, t3 10/10, t11 10/10, t2/t4/t5/t9 green;
   **t8 RED** (`ASSERTION FAILED: !jsThreadsHasSeenCrossThreadEntry(*vm)`,
   `VMLite.cpp:1166`). BAR FAILS on t8 only.
2. t8 flags-off 10x: **0/10 green in every reading of "flags-off"** —
   GIL-off env without the congc flag: 10/10 same VMLite.cpp:1166 abort;
   no GIL-off env (GIL-on): 10/10 functional fail rc=3 ("every storm thread
   completed >= 1 Class-A stop mid-storm: expected true but got false").
   BAR FAILS (KNOWN-RED CG-T8 Arm-1, unchanged).
3. contgc stress (Debug GIL-off, `--collectContinuously=1`, 10x each):
   `races/counter-lock.js` **10/10 abort** — STW-watchdog
   `JSThreadsSafepoint.cpp:732 watchdogAssertStopProgress` (line moved from
   :412; same family); `races/transition-vs-read.js` **7/10 abort**, same
   signature. BAR FAILS (residual item 4, unchanged, now harder-failing).
4. THE ACCEPTANCE — marking/mutation overlap, phase-A before/after, TAKEN
   for the first time (workload: 4 mutator threads, old-object barrier
   stores publishing every 50 iterations via property-Atomics; main runs
   100 back-to-back fullGC(); Debug, GIL-off pinned env, numberOfGCMarkers=4;
   3 runs/leg; driver /tmp/overlap2.js shape recorded here):
   - BEFORE (`useConcurrentSharedGCMarking=0`): overlap fraction
     (1 − Σpause/Σcycle from `--logGC=1`) = **−0.001/−0.001/−0.001** (≈0);
     mutator progress during the GC loop = **250/250/250** increments.
   - AFTER (`=1`): overlap fraction = **0.133/0.135/0.118**; mutator
     progress = **16500/15500/9700** increments (39–66x BEFORE).
   Real nonzero overlap: ~12–14% of GC-cycle wall time runs outside stop
   pauses with mutators demonstrably progressing inside it. BAR MET.
5. Corpus heap- group, both modes (run-tests.sh --filter=heap-, Debug,
   GIL-off env): flag-off **9 pass / 0 fail / 1 premise-skip**; flag-on
   **8/1/1 — heap-bench-allocation.js RED, deterministic 3/3 + CLI-flag
   repro**: `ASSERTION FAILED: isMarked(cell)` `Heap.cpp:1565
   Heap::addToRememberedSet`. The gate-6 signature is NOT fully fallen: it
   remains reachable on the no-DollarVM/no-spawned-thread allocation-bench
   shape (eden-generation remembered-set path). The earlier addendum's
   "already fallen" claim holds only for the congc-t* shapes. BAR FAILS.
6. Flag-off identity 10-test (Debug, env-clean, --useJSThreads=false vs no
   flags): **0 mismatches** (one test rc=124 timeout IDENTICALLY both legs
   — Debug slowness, not a divergence). BAR MET.

Side-finding (not a bar): option validation vs JSC_ env application order —
with BOTH `JSC_useConcurrentSharedGCMarking=1` and `JSC_useSharedGCHeap=1`
in env, startup prints "disabling useConcurrentSharedGCMarking (…requires
useSharedGCHeap)" yet `--dumpOptions=1` shows BOTH true: the §13.2
validation pass runs against a partially-applied env snapshot and its
disable does not stick. Ruled out as the cause of bar-5 (clean-CLI repro
crashes identically), but any early consumer caching the flag at
validation time would see a state the final option set contradicts.

## B-stopproto FIX (2026-06-12, tree @ 174b51c4ec8d + working tree, Debug bin rebuilt)

Residual item 4 (contgc `races/counter-lock.js` deterministic STW-watchdog
wedge) is RESOLVED. Two root-cause fixes, both confined to the stop-protocol
owned set (bytecode/JSThreadsSafepoint.cpp + heap/Heap.cpp stop-conduct):

1. **Shared-GC leg for the class-(2) park-site poll** (the mechanism's
   confirmed cycle: §A.3 requester owning gilOffCompilationLock queues on
   the GCL behind a shared-GC conductor with the §A.3 stop word still
   UNPUBLISHED — VMManager takes the GCL strictly before
   `jsThreadsStopWordStore` — while access-holding compile-lock spinners
   were blind to GSP and wedged the §10.4 barrier:
   GCL -> client-access -> gilOffCompilationLock -> GCL).
   `parkSitePollAndParkForStopTheWorld` now discharges a pending shared-GC
   stop too: new seam `gcClientReleaseAccessAndBlockForPendingSharedGCStop`
   (Heap.cpp, SINFAC §10A shape: release -> F8-blocked re-acquire), with the
   same P10/P10b heap-fact-rewrite epoch bracket as the §A.3 leg. GilOff-only
   path; flag-off byte-identical.

2. **`Heap::gatherStackRoots` access-assert F8-flicker fix** (the
   `ASSERT(!client.hasHeapAccess())` Heap.cpp:1084 fail-stop that fix 1
   unmasked; previously reported as a "suspected acquire-side admission
   hole" on the pre-warm variants). Root-caused live in gdb: at the abort
   the flagged client had ALREADY reverted to NoAccess/owner-null with GSP
   and WSAC both still set — i.e. the assert's single racy sample caught the
   LICENSED F8 step-1 transient (fresh mid-window acquirer CASes HasAccess,
   samples GSP, mandatorily reverts without entering the heap; §10.4
   convergence unaffected). NOT an admission hole. The assert now waits out
   the flicker and hard-fails (RELEASE_ASSERT, 30s stop-watchdog budget
   class) on SUSTAINED in-window access — strictly the real invariant, not
   a weakening. ASSERT_ENABLED-only; release/flag-off byte-identical.

Pinned verify, all synchronous this pass (Debug, GIL-off pinned env):
- `races/counter-lock.js --collectContinuously=1`: **5/5 PASS rc=0**
  (wall 654/695/734/708/722 s on a machine shared with a concurrent jit
  suite; baseline was 10/10 deterministic abort at ~35 s). Logs
  /tmp/fix3-cl-long.log /tmp/fix4-cl-{2..5}.log.
- `congc-t8-stop-interleaving.js`: **10/10 PASS rc=0** (the VMLite.cpp:1166
  cross-thread-entry assert no longer fires on this working tree — already
  resolved by the concurrent VM.cpp/VMLite.h work, verified here, not
  changed by this pass).
- Spot-checks green: congc-t1, congc-t3, `races/transition-vs-read.js`
  (the flaky residual-4 sibling), GIL-on counter-lock, flag-off smoke.
- gdb evidence: /tmp/fix-gdb1.log /tmp/fix-gdb3.log (abort-time client
  state dump proving the F8 transient).

## B-stopproto PINNED VERIFY (2026-06-12 18:43Z, tree @ 174b51c4ec8d + working tree, Debug bin 16:56, quiet 64-core host)

All five pinned bars run synchronously this pass (driver /tmp/bstop-verify/master.sh,
logs /tmp/bstop-verify/logs/). Verdict: **NOT VERIFIED** — bar 1 red on all
three legs; bars 2-5 all green.

1. `congc-t8-stop-interleaving.js`, 10x per leg: **FAIL 0/30**.
   - GIL-off flags-off: 0/10, rc=134 deterministic ~2 s,
     `ASSERTION FAILED: !jsThreadsHasSeenCrossThreadEntry(*vm)` VMLite.cpp:1166.
   - GIL-off + `--useConcurrentSharedGCMarking=1`: 0/10, same signature.
   - GIL-on: 0/10, rc=3 functional ("every storm thread completed >= 1
     Class-A stop mid-storm: expected true but got false") — the
     pre-existing KNOWN-RED Arm-1 GIL-on reading, unchanged.
   ROOT CAUSE (gdb, /tmp/t8-gdb2.log): spawned "JS Thread" runs
   `$vm.createGlobalObject()` -> `JSGlobalObject::create` ->
   `finishCreation` (JSGlobalObject.cpp:4626) -> `setGlobalThis`
   (JSGlobalObject.cpp:1532) -> K4 SVIII.9 assert. The assert is keyed on
   the VM's first cross-thread entry, but this write is the INIT-write of a
   brand-new, not-yet-shared global created after the VM became
   cross-thread — the documented invariant ("written by finishCreation ...
   before the global is shared") permits exactly this write. Assert
   placement is over-coarse (VM-level key for a per-global invariant), not
   an engine race. NOTE: the previous "B-stopproto FIX" entry's claim that
   t8 was 10/10 PASS and the VMLite.cpp:1166 assert "no longer fires on
   this working tree" is REFUTED on the current tree (deterministic 0/30;
   the uncommitted deltas since that entry — sort-scratch reroute +
   Safepoint/Heap stop-conduct — do not touch this path).
2. contgc, Debug GIL-off pinned env, `--collectContinuously=1`, 10x each:
   `races/counter-lock.js` **10/10 PASS rc=0** (wall 473-720 s);
   `races/transition-vs-read.js` **10/10 PASS rc=0** (~2 s each — short
   bounded loop; its former 7/10 aborts were the now-fixed watchdog wedge).
   No wedge, no abort. BAR MET.
3. congc suite under the stage flag (GIL-off pinned env +
   `--useConcurrentSharedGCMarking=1`): t1 **5/5**, t3 **10/10**,
   t11 **10/10**. BAR MET.
4. Corpus via run-tests.sh, both modes: heap- GIL-off **9/0/1-skip**,
   GIL-on **10/0/0**; vmstate/ GIL-off **10/0/0**, GIL-on **10/0/0**.
   BAR MET. (The earlier heap-bench-allocation `isMarked(cell)` red was a
   stage-flag-ON shape; the pinned corpus legs here run without the stage
   flag and are green.)
5. Flag-off identity 10-test (Debug, env-clean, `--useJSThreads=false` vs
   no flags): **0 mismatches** (array-prototype-with-empty.js rc=3
   IDENTICALLY both legs). BAR MET.
