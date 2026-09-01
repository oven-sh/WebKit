# EVIDENCE PACK — STW-watchdog abort under watchpoint storm (GIL-off, scaling suite)

Date: 2026-06-10. Binary under test: `WebKitBuild/Release/bin/jsc` (mtime
2026-06-10 12:20 UTC — includes the GC under-marking fix 25375a997f4f; `ninja jsc`
verified no-work before the campaigns; relinked at 13:33 with the env-gated
§8 instrumentation — inert without the env vars, and post-relink runs
reproduced identically). Host: 64-core Linux, NOT quiet — a
concurrent agent was running Debug-build CVE suites throughout (loadavg 4-6);
the bug is a liveness deadlock, host noise only perturbs interleavings.
Pinned GIL-off flags (every run below unless noted):
`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
--useSharedGCHeap=1 --useThreadGILOffUnsafe=1`.
Facts only; no fixes; no hypotheses beyond what the data forces.

Prior packs archived untouched: `prev-butterfly-stress-20260609/` (wrong-value
IC bug), `prev-gc-undermark-20260610/` (shared-heap under-marking, fixed by
25375a997f4f).

Artifacts (all under `stw-watchdog/`):
- `campaign.sh` — failure-rate harness
- `logs/` — every run's full output (abort reports included)
- `bts/` — all-thread GDB backtraces extracted from abort cores (core-reaper.sh)
- `core-585068-bt.txt` — the first abort's full backtrace (annotated in §2)
- `rr-hunt.sh`, `rr-hunt.out` — rr chaos-mode capture attempt (NEGATIVE, §5)
- `firelog-n4.txt` — per-fire identity log (§4); `repro4.out` + `../repro.js`
  — focused repro (§7)

## 0. THE HEADLINE FACT (from the first abort core, §2)

The watchdog abort is a **deadlock cycle** (4-party in the first core;
2-party at N=2 — see §2.5), not a starved-but-live storm:

```
T_A: holds CodeBlock::m_lock, spins in §A.3.3 arbitration tryLock  ──needs──> T_B's job-slot lock
T_B: §A.3 conductor, holds s_jsThreadsJobSlotLock, waits §A.3.2 predicate ──needs──> T_C/T_D access release
T_C, T_D: hold heap access, blocked in WTF::Lock::lock(CodeBlock::m_lock) ──needs──> T_A
```

T_A's Class-A fire is `Structure::didCachePropertyReplacement` called from
`tryCachePutBy` (Repatch.cpp:1360) **while holding `codeBlock->m_lock`**
(`GCSafeConcurrentJSLocker` taken at the top of tryCachePutBy). This is a
bucket-(iii) lock-holding fire site that the Task-11 direct-fire audit
(docs/threads/INTEGRATE-jit.md) does NOT list: the audit's only
`firePropertyReplacementWatchpointSet` rows are the InlineCacheCompiler/
AccessCase EnsureWatchability paths, and M6.1 (the deferred-fire parameter)
landed but `Structure::didCachePropertyReplacement` (StructureInlines.h:265)
passes NO deferred holder — it fires synchronously under the lock.

## 1. Reproduction and failure rates

Test: `JSTests/threads/scaling/richards-like.js` driven exactly as
`scaling-gate.sh` drives it: `jsc <pinned flags> -e
"globalThis.SCALING_THREADS=N; globalThis.SCALING_WORK_SCALE=S;" richards-like.js`.
Abort = rc 134 + `JSThreads stop-the-world failed to reach a stopped world
within 30.000000s`. PASS runs at scale 1 take ~80 s (N=4) / ~134 s (N=8) wall
on this loaded host — the catastrophic scaling is real, but §4 shows it is
NOT stop-window time; abort runs die ~30 s after the wedge forms (always
early in the run = IC-warmup phase).

| config (workload, N, scale) | runs | watchdog aborts | rate |
|---|---|---|---|
| richards, N=4, scale=1 | 12 | 1 | 8% |
| richards, N=8, scale=1 | 10 | 1 | 10% |
| string-heavy, N=8, scale=1 | 10 | 0 | 0% (SCALEBENCH saw 1; low-rate) |
| richards, N=4, scale=1/8 | 16 | 1 | 6% |
| richards, N=8, scale=1/16 | 40 | 21 | 53% |
| richards, N=4, scale=1/16 | 40 | 20 | 50% |
| richards, N=3, scale=1/16 | 40 | 11 | 28% |
| **richards, N=2, scale=1/16** | 40 | 5 | **12% — TWO spawned threads suffice** |
| richards, N=4, 1/16, `--useJIT=0` | 25 | **0** | LLInt-only: NO repro |
| richards, N=4, 1/16, `--useDFGJIT=0` | 25 | 10 | 40% — Baseline alone reproduces |
| richards, N=4, 1/16, `--useFTLJIT=0` | 25 | 21 | 84% |
| **focused repro.js, N=4** (`../repro.js`) | 15 | 11 | **73%** — distillation validated by core backtrace (bts/repro-core.652059.bt: same 1360 signature) |

No checksum divergence occurred in ANY run of this campaign (the harness's
cross-thread-corruption channel never tripped, and the prior pack's
got/want value-decode question does not apply): this bug is pure liveness.
Butterfly-verifier knobs (forceSegmentedButterflies / verifyConcurrentButterfly)
were not exercised — the 69-core invariant plus the tier knockouts already
pin the wedge to the IC-repatch lock cycle, not the object model.

Tier carving: the culprit fire site is the **Baseline/IC repatch path**
(`tryCachePutBy`), exercised by Baseline+; LLInt's own replace-caching calls
`didCachePropertyReplacement` BEFORE taking `codeBlock->m_lock`
(LLIntSlowPaths.cpp:1384/1398/1705, CommonSlowPathsInlines.h:105 — fire
first, lock only for the metadata publish), which is why `--useJIT=0` never
deadlocks. Higher tiers are irrelevant (no-DFG rate is the highest measured —
more time spent in Baseline IC warmup).

Scale reduction does NOT eliminate the bug; SMALLER scale reproduces BETTER
(1/16 >> 1). The wedge forms during IC warmup (first seconds of every
spawned-thread generation), not steady state: it is a specific interleaving
of warmup-phase fires whose probability rises with concurrent fire frequency,
not with total work.

## 2. The abort anatomy (core.585068, richards N=4 scale=1/8)

Watchdog report (logs/r4-scale8th-run6.log): requester context
`0x7fc8f458c100 (WatchpointSet Class-A fire)`; participant dump: 5 entered
lites; tid=3 `[requester/conductor — exempt]` access=false; tid=4 and
tid=16384 (the carrier, parked in Thread.join) access=false; **tid=1 and
tid=2 access=true NON-QUIESCENT**.

All-thread backtraces (`core-585068-bt.txt`):

- **Aborting thread (T_A, lite tid=3)** — `watchdogAssertStopProgress` called
  from **VMManager.cpp:604 = the §A.3.3 arbitration tryLock loop** (it never
  won tenure; its budget re-arms only on completed windows and NO window
  completed for 30 s). Its stack below the spin:
  `operationPutByIdSloppyOptimize → repatchPutBy → tryCachePutBy
  (Repatch.cpp:1360, INSIDE GCSafeConcurrentJSLocker(codeBlock->m_lock 0x7fc8f20e89d0))
  → Structure::didCachePropertyReplacement(offset=6)
  → firePropertyReplacementWatchpointSet("Did cache property replacement")
  → fireAll → fireAllSlow → fireAllUnderClassAStop(set 0x7fc8f458c100)
  → stopTheWorldAndRun → jsThreadsThreadGranularStopTheWorldAndRun → spin`.
  It released its own heap access (correctly) but NOT codeBlock->m_lock.
- **Conductor (T_B, LWP 585112)** — holds the job slot, parked in the
  **§A.3.2 predicate wait** (VMManager.cpp:664). Its fire is the SAME
  WatchpointSet 0x7fc8f458c100 / SAME Structure 0x7fc70100c580, reached
  lock-free: `operationPutByIdSloppyOptimize → JSObject::putInlineFast →
  putDirectInternal → Structure::didReplaceProperty("Property did get
  replaced") → fireAllUnderClassAStop → ... → predicate wait`. Two
  concurrent Class-A fires of one set: one a replace-path fire, one a
  cache-the-replacement fire — the "fire-while-stopping window" is real but
  benign by itself (coalescing handles it); the deadlock needs the lock.
- **The two NON-QUIESCENT lites (T_C = LWP 585110/tid1, T_D = LWP
  585109/tid2)** — both blocked in `WTF::Lock::lock(0x7fc8f20e89d0)` ==
  **the same CodeBlock's m_lock**, from
  `operationGetByIdOptimize → repatchGetBy → tryCacheGetBy (Repatch.cpp:605)
  → GCSafeConcurrentJSLocker ctor`. They hold heap access; `Lock::lock` is a
  blind futex wait with no stop-the-world poll (FIX-2 mechanism (2): an
  access-holding unbounded native wait with no park-site poll — but here the
  PRIMARY violation is T_A's lock-holding fire; T_C/T_D's wait would be
  bounded if T_A could finish).
- Remaining threads: carrier in `threadProtoFuncJoin` (quiescent), libpas
  scavenger (irrelevant).

All four JS threads are in the SAME CodeBlock (0x7fc8f20e89c0, Baseline-JIT
operation paths) and the SAME Structure — richards' threads run identical
code over identically-shaped objects, so the per-(structure,offset)
property-replacement set and the per-codeBlock lock are shared engine-level
state despite "NO data shared between threads" at JS level.

The cycle: T_A holds m_lock → waits for job-slot (held by T_B) → T_B waits
for T_C/T_D to release access → T_C/T_D wait for m_lock (held by T_A). No
participant can move; both watchdog legs (T_A's arbitration leg at :604,
T_B's predicate leg at :662) reach 30 s; whichever samples first aborts.
The "multiple non-quiescent lites" in the report are the VICTIMS (blocked
lock acquirers), not the culprit — the culprit (T_A) shows access=false.

## 2.5 Invariant across ALL captured aborts

Every abort core reaped (**69/69** at last full count, plus the repro.js
core; `bts/*.bt`, N in {2,3,4,8}, scales 1/16, 1/8, 1, pinned flags and
both knockout configs) contains:
- EXACTLY ONE thread whose Class-A fire is
  `tryCachePutBy (Repatch.cpp:1360) → didCachePropertyReplacement` — i.e. a
  stop request made while holding `codeBlock->m_lock`. Sometimes this thread
  is the conductor (predicate-leg abort, VMManager.cpp:662), sometimes an
  arbitration loser (:604/:605 abort) with a lock-free fire
  (didReplaceProperty / TTL) as conductor — both watchdog legs appear, same
  cycle.
- N-1 down to 1 threads blocked in `WTF::Lock::lock` on that codeBlock's
  m_lock (from tryCacheGetBy Repatch.cpp:605 or tryCachePutBy's :1238 locker
  entry) HOLDING heap access — these are the reported "non-quiescent lites".
- At N=2 the minimal 2-party form: conductor holds m_lock + job slot, waits
  for the one sibling; sibling holds access, waits for m_lock.

Other fire flavors seen QUEUED at arbitration in the same cores (victims,
not culprits — all access-released, lock-free by design):
- `Structure::didReplaceProperty` ("Property did get replaced") from
  putDirectInternal — fires before any codeBlock lock.
- `fireTTLSetsForSharedTransition` ("F2: shared structure-only transition
  (N2)") from tryStructureOnlyTransition (ConcurrentButterfly.cpp:1423) —
  the F2 protocol fires deliberately BEFORE any cell-lock acquisition
  (I10b/I13), i.e. THAT family got the lock discipline right.

## 3. Where the spec/audit and the implementation disagree (facts, not fixes)

- SPEC-jit App. 5.6(c): direct fireAll callers are REQUIRED lock-free w.r.t.
  every §7 lock (CodeBlock::m_lock is §7). The Task-11 audit table found ONE
  bucket-(iii) `firePropertyReplacementWatchpointSet` reacher
  (EnsureWatchability under addAccessCase) and minted M6.1 (deferred-fire
  parameter, landed in Structure.h/.cpp). The call chain that deadlocks here
  — `tryCachePutBy` ExistingProperty branch (Repatch.cpp:1360) →
  `didCachePropertyReplacement` (StructureInlines.h:265, passes no deferred)
  — is ABSENT from the table. (Stock JSC has the same call under the same
  lock; it is only fatal once the fire requests a stop.)
- The §A.3.3/r33-35 fan-in question from the charter: the r33-35 rule-5
  amendments are flag-gated on `useConcurrentSharedGCMarking`, which does NOT
  exist in the tree (grep: zero hits in Source/) — so the frozen rule-5 is
  operative and the landed arbitration (tryLock + progress-re-arm) matches
  it. The deadlock is NOT an arbitration-protocol divergence; arbitration
  behaved as specified. The watchdog message's own bucket-iii hypothesis is
  the correct one.

## 4. Perf facet — fire rate, identities, and window cost (CHARACTERIZE only)

Env-gated instrumentation (`JSC_CLASSA_FIRE_STATS=1` / `JSC_CLASSA_FIRE_LOG=1`)
added to Watchpoint.cpp + VMManager.cpp, clearly marked NOT FOR LANDING, left
in the working tree for the fix stage to reuse or revert. Caveat: counters
dump via atexit, so only PASSING runs report (an aborted run dies in
RELEASE_ASSERT before atexit).

richards-like, pinned flags, instrumented Release, passing runs:

| config | wall (SCALING ms) | stop-fires | inline-fires | §A.3 windows | total request→resume | max window |
|---|---|---|---|---|---|---|
| N=1, scale 1/16 | 229 | 36 | 36 | 50 | 0.24 ms | 0.04 ms |
| N=2, scale 1/16 | 3020 | 36 | 36 | 52 | 1.5 ms | 1.2 ms |
| N=4, scale 1/16 | 4665 | 37 | 36 | 54 | 6.1 ms | 2.2 ms |
| N=8, scale 1/16 | 7835 | ~37 | ~36 | 58 | 24.2 ms | 4.3 ms |
| **N=2, scale 1** | **48835** | **36** | **36** | **54** | **1.6 ms** | 1.1 ms |

Fire identities (N=4 run, `firelog-n4.txt`, 74 fires total):
35x inline "IC has been invalidated" (PropertyInlineCacheClearingWatchpoint,
fired inside drains/stops), 17x stop "Property did get replaced"
(didReplaceProperty), 10x stop "Did cache property replacement"
(didCachePropertyReplacement — the deadlock site), 3x "Allocating a function",
2x poly-proto, 2x Structure-transition sets, 1x put_to_scope link, 1x
op_put_scope, 1x "Allocated a scope".

**The SCALEBENCH §6.2 storm hypothesis is REFUTED by direct measurement**:
at the exact config where T(2) = ~14x T(1) (scale 1, N=2: 48.8 s wall vs
~3.7 s single-thread), the TOTAL time spent in all 54 Class-A/§A.3 stop
windows is **1.6 ms** (~0.003% of wall). Fire COUNT is constant (~36 stop
fires) regardless of N and of work scale — the fires are warmup-phase,
per-(structure,offset)/per-set one-shots, not a steady-state storm. Whatever
serializes richards GIL-off (T(2)=13-14x T(1) reproduced here), it is NOT
stop-the-world frequency or stop latency; mid-run stack samples show JS
threads executing JIT code, not parked. That redirects the TTL-rebias /
de-jank perf charter away from stop-frequency and toward the mutator hot
path itself (out of scope here).

## 5. rr

`/usr/bin/rr` works once `LD_LIBRARY_PATH=/usr/local/lib` is exported
(distro rr links a libcapnp living in /usr/local/lib; bare `rr` fails to
load). Verified recording+`rr ls` on a trivial jsc run. Chaos-mode capture
(`rr-hunt.sh`, richards N=4 scale 1/16 — a config that aborts ~50% natively):
**0 captures in 60 recorded runs**. rr serializes all threads onto one core;
the deadlock needs a real cross-core interleaving (a fire requester holding
the codeBlock lock while ≥1 sibling blocks on it and another fire holds
conductor tenure), which rr's scheduler apparently never produced. No rr
trace exists; the 70 core dumps + the deterministic-ish repro.js are the
substitute evidence.

## 6. Hard facts any hypothesis (and any fix) must explain

1. **69/69 abort cores contain exactly one thread that requested a Class-A
   stop while holding `CodeBlock::m_lock`** — always the same call chain:
   Baseline put-IC slow path `operationPutById*Optimize → repatchPutBy →
   tryCachePutBy` (GCSafeConcurrentJSLocker at Repatch.cpp:1238) →
   ExistingProperty branch → `oldStructure->didCachePropertyReplacement`
   (Repatch.cpp:1360) → `firePropertyReplacementWatchpointSet` →
   `fireAllUnderClassAStop → stopTheWorldAndRun`. No exception across
   N∈{2,3,4,8}, scales {1/16,1/8,1}, and {pinned, no-DFG, no-FTL} configs.
2. The reported NON-QUIESCENT lites are threads blocked in
   `WTF::Lock::lock(codeBlock->m_lock)` from tryCacheGetBy/tryCachePutBy
   locker entry, holding heap access — `Lock::lock` is a blind futex wait
   with no §A.3 poll and no access-release bracket, so the §A.3.2 predicate
   can never count them quiescent while the lock holder is wedged.
3. The cycle closes through §A.3.3 arbitration: the lock-holding requester
   either (a) IS the conductor (holds job slot; predicate-leg watchdog at
   VMManager.cpp:662 — the N=2-capable 2-party form), or (b) spins as an
   arbitration loser (:604/:605) behind a conductor whose own fire is
   lock-free (didReplaceProperty from putDirectInternal, or a TTL
   `fireTTLSetsForSharedTransition`) and whose predicate waits on the
   blocked lock acquirers — the 3-4 party form. Both watchdog legs appear
   in the cores.
4. LLInt-only (`--useJIT=0`, 0/25) never reproduces: the LLInt and
   megamorphic paths call `didCachePropertyReplacement` BEFORE taking
   `codeBlock->m_lock` (LLIntSlowPaths.cpp:1384/1398/1705,
   CommonSlowPathsInlines.h:105, JITOperations.cpp:1277/2135). Only the
   Repatch.cpp:1360 call sits inside the locker. The Task-11 audit table
   lists `firePropertyReplacementWatchpointSet` as bucket-(iii) only via the
   EnsureWatchability/addAccessCase path (M6.1 — the deferred-fire parameter
   that LANDED but is not passed at this call site, deferred=0x0 in every
   core's frame).
5. The deadlock is an IC-WARMUP interleaving, not a steady-state storm:
   total Class-A fires per run ≈ 72 (36 stop + 36 inline), independent of N
   and scale; aborts always land within the first seconds of the spawned
   generation; rate RISES as scale shrinks (50% at 1/16 vs 8% at 1, N=4)
   and as tiers are capped (84% no-FTL), i.e. with the fraction of run time
   spent in Baseline IC warmup.
6. The carrier's two warmup executions do NOT retire the firing sets for
   the spawned generation: spawned threads measurably still produce ~36
   stop-requesting fires in their first seconds (every abort core shows
   spawned-thread fires; the N=4 identity log's 17x didReplaceProperty +
   10x didCachePropertyReplacement are richards object shapes). WHY the
   sets are still IsWatched after warmup (lazily created per-offset
   replacement sets, TTL/per-generation keying, fresh structures per
   spawned thread) is NOT pinned by this pack — measured fact only.
7. §A.3.3/rule-5 arbitration itself behaved as specified: the r33-35
   amendments are gated on `useConcurrentSharedGCMarking`, which does not
   exist in the tree (zero grep hits in Source/), so the frozen rule-5 is
   operative; the watchdog's progress-re-arm correctly identified "no
   completed windows in 30 s". This is not an arbitration livelock — with
   zero contending lock-holding fires the same queue drains fine
   (50-58 windows/run complete in ≤24 ms total).

## 7. Minimal reproducing config

`Tools/threads/bughunt/repro.js` (70 lines, no harness): N threads run one
hot function doing existing-property replace puts + gets on identically
shaped thread-local objects. With the pinned flags:
- N=4: **11/15 watchdog aborts (~75%)**, each in ~30 s; passing runs
  complete in <1 s (bimodal — if warmup's fires win cleanly, steady state
  has zero fires and nothing can wedge).
- Abort core verified identical signature (bts/repro-core.652059.bt:
  tryCachePutBy:1360 fire present, blocked locker siblings).
- N=2 form reproduces via richards r2-s16 (5/40) and the 2-party cores
  (core.593572, core.595818).

Smallest amplifying knob stack: scale 1/16 + `--useFTLJIT=0` on
richards-like N=4 = 84%.

## 8. Artifacts and instrumentation left in tree

- `stw-watchdog/bts/` — 70 all-thread backtrace extractions (cores deleted
  to protect /tmp; reaper script `core-reaper.sh`).
- `stw-watchdog/logs/` — every campaign run's stdout/stderr incl. all abort
  reports; `stw-watchdog/*.out` — per-campaign verdict lines + SUMMARY.
- `stw-watchdog/firelog-n4.txt` — full per-fire identity log.
- `repro.js` — focused repro (above).
- Instrumentation diffs (env-gated, "BUGHUNT ... NOT FOR LANDING"):
  `Source/JavaScriptCore/bytecode/Watchpoint.cpp` (fire counters + identity
  log), `Source/JavaScriptCore/runtime/VMManager.cpp` (window count +
  request→resume latency). Inert without `JSC_CLASSA_FIRE_STATS` /
  `JSC_CLASSA_FIRE_LOG` in env; revert or keep at the fix stage's
  discretion. The Release jsc binary currently includes them.

## 9. EXPERIMENTER round 1 (2026-06-10, post GC-under-marking fix rebuild)

Binary state: tree rebuilt at 13:35 including the GC under-marking fix; the
finder round's env-gated fire-stats instrumentation (Watchpoint.cpp /
VMManager.cpp) retained throughout. Experiment binaries snapshotted at
`WebKitBuild/Release/bin/jsc-bughunt-expA` and `...-expB`; hand-written patch
records in `stw-watchdog/experiments/*.patch`; all Source/ probe edits
REVERTED after the runs and the reverted tree re-verified (see below).
Campaign outputs: `stw-watchdog/exp-*.out`, `expA-*.out`, `expB-*.out`.

### 9.0 Re-confirmation after the GC fix (charter requirement)

`exp-control-repro4.out` runs 1-5 (executed against the pre-experiment,
post-GC-fix binary): **4/5 WATCHDOG-ABORT** on repro.js N=4. The storm bug is
independent of the under-marking fix, as charted. (Runs 6-10 of that file
accidentally exec'd the freshly linked Experiment-A binary — 5/5 PASS — an
unplanned but clean same-session A/B.) End-of-session check on the fully
REVERTED rebuild: `exp-revert-check.out` = 3/6 WATCHDOG-ABORT, tree back in
the reproducing state.

### 9.1 Experiment A — H1/H-arb-1 causality seal (M6.1 deferral at Repatch:1360)

Single-variable change (`experiments/expA-deferred-fire.patch`):
`DeferredStructureTransitionWatchpointFire` holder declared BEFORE the
`GCSafeConcurrentJSLocker` scope in `tryCachePutBy`, threaded through a new
deferred parameter on `didCachePropertyReplacement` into the EXISTING
`firePropertyReplacementWatchpointSet` deferred branch (Structure.cpp:2004).
Destruction order verified: locker dtor (unlockEarly) runs before the holder
dtor, so the Class-A stop is requested strictly after m_lock release.

| campaign | config | baseline (finder round) | Experiment A |
|---|---|---|---|
| expA-repro4 | repro.js N=4 x30 | 11/15 abort (~73%) | **0/30, all PASS <1s** |
| expA-r4-s16-noftl | richards N=4 scale 1/16 --useFTLJIT=0 x25 | ~84% abort | **0/25** |
| expA-r2-s16 | richards N=2 scale 1/16 x40 (2-party form) | 5/40 abort | **0/40** |
| expA-r8-s16 | richards N=8 scale 1/16 x15 | (intermittent) | **0/15** |

**0/110 watchdog aborts, no new failure signature, results/checksums
unchanged** vs 4/5 aborts on the unpatched binary in the same session.
H1 (and its duplicate H-arb-1) CONFIRMED as the load-bearing cause: moving
the one lock-holding Class-A fire out from under codeBlock->m_lock fully
retires the wedge in every reproducing config.

### 9.2 Experiment B — H2 sufficiency probe (stop-aware victim wait, 1360 fire RESTORED)

Single-variable change (`experiments/expB-stop-aware-victim-wait.patch`):
H1 site restored to stock (fire under m_lock); ONLY the locker entries at
tryCacheGetBy (:605) and tryCachePutBy (:1238) replaced with a
tryLock-poll-with-heap-access-released acquire (AdoptLock into the locker).

`expB-repro4.out`, repro.js N=4 x30: **22 PASS / 4 WATCHDOG-ABORT / 4
TIMEOUT (150s silent hang, no watchdog report)**.

Abort core (`stw-watchdog/bts/expB-core-807636.bt`) — the H2 refuter's
predicted outcome (b), precisely:
- Thread 1 (culprit/conductor): restored `tryCachePutBy` ExistingProperty
  fire, `deferred=0x0`, parked in the §A.3.2 predicate wait (VMManager:665)
  HOLDING m_lock.
- Thread 6 (the NON-QUIESCENT lite, hasHeapAccess=true): blocked in a plain
  blind `GCSafeConcurrentJSLocker` on the SAME codeBlock->m_lock at a THIRD
  site the probe did not cover — `fireWatchpointsAndClearStubIfNeeded`
  (Repatch.cpp:518, reached from tryCacheGetBy:963 AFTER the patched entry
  locker was released.
- Thread 4: a second queued Class-A requester (ConcurrentButterfly
  stopTheWorldAndRun) spinning in arbitration (VMManager:608) — the
  "multiple non-quiescent lites" 3-party form, reproduced under the probe.

The 4 silent 150s hangs are a NEW shape introduced by the probe itself (no
stop pending or window already reached, so no watchdog) — consistent with
the probe's acquireHeapAccess-on-exit blocking for a stop window while the
thread already holds m_lock that in-window work needs; not pinned (SIGKILL,
no core). This is the refuter's outcome (c): mechanism diagnostic holds, fix
shape wrong.

Verdict on H2: the MECHANISM is real and load-bearing as the victim-side
half of the cycle (closing two sites dropped aborts ~73% -> 13%), but the
SUFFICIENCY claim is REFUTED: (a) the access-holding blind wait is a CLASS
of §7-lock sites (at minimum :518 also reachable in the same workload), so
no enumerable per-site victim fix closes the bug while a lock-holding fire
exists; (b) the access-release-while-waiting fix shape introduces its own
hangs. H2 stands as defense-in-depth/diagnosis, NOT as a fix path.

### 9.3 Implication for the fix stage

Fix the requester side (H1): route Repatch.cpp:1360 through the landed M6.1
deferral exactly as Experiment A did (plus re-audit the remaining
`didCachePropertyReplacement` callers — JITOperations.cpp:1277/2135 fire
OUTSIDE any locker today, LLIntSlowPaths/CommonSlowPaths fire before locking;
keep SPEC-jit App. 5.6(c) "no fireAll under section-7 locks" enforced, and
add the Repatch:1360 row to the Task-11 audit table). The Experiment-A patch
is the fix candidate; its ordering caveat (fact published before fire — the
M6.1 fireAllSlow ORDERING CAVEAT comment, class (a)/(b)/(c)) must be
classified for this site during fix review: the replacement-set fire guards
constant-folded property values, and the deferral window now spans the
remainder of tryCachePutBy until scope exit.

## 10. EXPERIMENTER round 2 (2026-06-10) — watchdog-angle hypotheses WD-3 / WD-4

Tree state at start: the H1 fix (pre-locker fire in tryCachePutBy,
Repatch.cpp ~1238-1283 + locked-branch refuse-to-publish) had ALREADY LANDED
and the fix campaigns (fix-*.out) were 100/100 PASS, so WD-3's "stock
binary" requirement was met with a scratch build that restores the defect
env-gated (JSC_BUGHUNT_WD3_STOCK=1; record:
`stw-watchdog/experiments/wd3-stock-defect-and-window-logging.patch`;
binary snapshot `WebKitBuild/Release/bin/jsc-bughunt-wd3`; landed-fix binary
snapshot `...-fixed`). The scratch build also logs, from INSIDE the
watchdogAssertStopProgress fail path, the completed-§A.3-window count at the
last requestStart (re-)arm vs at abort ("BUGHUNT-WD3 windowsAtArm=X
windowsAtAbort=Y"), so each abort log itself proves whether ANY window
completed inside the failed 30s budget. All Source/ edits REVERTED after the
campaigns; reverted rebuild re-verified green (wd3-revert-check.out: 6/6
PASS on repro.js N=4).

### 10.1 WD-3 (watchdog true positive, no accounting defect) — CONFIRMED

`wd3-stock.out`, repro.js N=4 x20 with the defect restored: 9 PASS /
11 WATCHDOG-ABORT (~55%, in line with the finder round's ~73%). Control
(same binary, env unset => landed-fix shape): PASS.

Every one of the 11 aborts:
- `windowsAtArm == windowsAtAbort` (14/14 x8, 20/20, 22/22 — count FROZEN
  for the entire 30s budget; budgetElapsed 30.000334-30.001058s). No abort
  showed the count advancing without a re-arm — zero evidence of an
  accounting defect.
- Pending Class-A fire context NON-nil ("WatchpointSet Class-A fire") and
  >= 1 entered lite named NON-QUIESCENT with hasHeapAccess=true (1-3 victims
  per abort — the "multiple non-quiescent lites" wording is victim count,
  not concurrent conductors).
- Core backtrace (bts/wd3-core.*.bt, 11/11): conductor thread parked in
  watchdogAssertStopProgress <- stopTheWorldAndRun <- fireAllUnderClassAStop
  <- fireAllSlow <- Structure::didCachePropertyReplacement <- tryCachePutBy
  (the under-lock fire, i.e. the original Repatch:1360 defect), plus >= 1
  sibling blocked in WTF::Lock::lockSlow at the tryCachePutBy/GetBy
  GCSafeConcurrentJSLocker entry on the same codeBlock->m_lock.

Refuter predictions for refutation (count advancing without re-arm;
abort with no lock-holding fire) observed ZERO times in 11 aborts + the 70
finder-round cores. The 30s abort is a TRUE POSITIVE of the H1 deadlock;
the watchdog's per-leg/progress-re-arm accounting behaved exactly as coded.

### 10.2 WD-4 (silent-hang watchdog coverage gap) — CONFIRMED, in-window-wedge branch pinned

Code inspection: watchdogAssertStopProgress has exactly THREE call sites,
all REQUESTER legs — VMManager.cpp:615 (arbitration spin), VMManager.cpp:675
(§A.3.2 predicate wait), Heap.cpp:5824 (GCL bracket leg). No arm samples a
non-requester wedged thread; no third arm exists.

Experiment-B hang PINNED LIVE this time (`wd4-hangpin.sh`: 60s-alive
=> gdb attach + all-thread dump BEFORE kill). `wd4-hangpin.out` +
`wd4-hangpin2.out`, jsc-bughunt-expB on repro.js N=4: 90 runs total =
76 PASS / 6 WATCHDOG-ABORT / 8 SILENT-HANG (~9%, vs 4/30 in round 1).
All 8 hang dumps (bts/wd4-hang-run*.bt) show ONE identical 3-party shape —
the refuter's predicted CONFIRM branch:
- CONDUCTOR: inside its OPEN §A.3 window (past VMManager.cpp:675; frames:
  tryCacheGetBy/PutBy -> fireWatchpointsAndClearStubIfNeeded(Repatch.cpp:515)
  -> fireAll -> stopTheWorldAndRun -> work closure -> drained fireAllNow ->
  nested fire body) blocked in a blind WTF::Lock::lockSlow on a
  codeBlock->m_lock. Being past the predicate wait, it never calls the
  watchdog again.
- HOLDER: an expB probe thread (bughuntStopAwareLockAcquire,
  Repatch.cpp:509) that won m_lock via tryLock and is parked in gated
  GCClient::Heap::acquireHeapAccess (Heap.cpp:6121) re-acquiring access
  WHILE HOLDING m_lock — gated on the conductor's still-open window.
- Bystanders: remaining mutators parked in gated acquireHeapAccess
  (s_jsThreadsParkCondition) waiting for the window to close.
- watchdogAssertStopProgress frames in the dumps: ZERO (8/8). No requester
  leg is armed; the hang is INFINITE and SILENT (no 30s report, no core).

The alternative no-stop-pending branch was never observed (0/8): every hang
had an open window with a live-but-wedged conductor.

### 10.3 Fix-stage gate consequences (recorded per the refuter's mandatory request)

1. The landed Experiment-A-shape fix is the right shape; any alternative
   that introduces blocking (esp. access-release/re-acquire brackets) inside
   a §7 lock scope lands EXACTLY in the pinned blind spot: it converts the
   debuggable 30s abort into an infinite silent hang that watchdog-based
   gates cannot see.
2. Verification campaigns MUST run under an external wall-clock timeout that
   (a) classifies no-watchdog-output timeout as FAILURE (never flake), and
   (b) captures an all-thread dump at expiry (wd4-hangpin.sh is the
   template: alive-at-60s => gdb attach before kill).
3. Residual (out of this bug's scope, feeds the watchdog hardening charter):
   a third watchdog arm sampling in-window conductor progress (e.g. a
   heartbeat the conductor bumps between fire bodies, checked by any gated
   AHA waiter) would close the blind spot structurally.

## 11. EXPERIMENTER round 3 (2026-06-10) — NPR-2 / ARB3-2 / PWS-NEG-1 / WD-5 closure runs

Binary under test: landed-fix `WebKitBuild/Release/bin/jsc` (mtime 16:04 UTC,
includes GC under-marking fix 25375a997f4f and the H1 pre-locker fire;
`find Source -newer jsc` = empty, no stale objects). Pinned GIL-off flags as §1.
No Source/ edits were made this round; the only new artifact is
`stw-watchdog/experiments/yarr-noPoll-probe.js` (pure JS probe).

### 11.1 Landed-fix campaigns (NPR-2 req 1, PWS-NEG-1 confirmIf 2, WD-5 confirmIf)

| config | runs | result |
|---|---|---|
| string-heavy, N=8, scale=1 (`r3-sh8-fix.out`) | 40 | **40 PASS / 0 abort / 0 timeout** — closes the thin post-fix suite-coverage gap |
| richards, N=4, scale=1/16, `--useFTLJIT=0` (`r3-r4-noftl-fix.out`) | 100 | **100 PASS / 0 abort / 0 timeout** — highest-rate amplifier (84% abort pre-fix), under the external 150s gate (timeout would classify as FAILURE; none occurred) |
| repro.js, N=4 (`logs/r3-repro4-fix-run*.log`) | 30 | **30 PASS / 0 abort** |

Zero aborts means zero opportunity for the NPR-2/PWS-NEG-1 refuters'
"EXECUTING non-quiescent lite" / "access-holding parked lite" core
signatures to appear. Cumulative landed-fix evidence: 0 aborts in 280 runs
(110 + this round's 170) vs 50-84% pre-fix.

### 11.2 ARB3-2 — discriminators re-run, all negative confirmed

- `grep -r "requester queued" stw-watchdog/logs/ *.out`: **0 hits** (no
  starvation breadcrumb in any campaign log, including this round's).
- `grep -rn useConcurrentSharedGCMarking Source/`: **0 hits** — the r33-35
  fan-in/COUNT-bound clauses remain dormant by their own spec gating; the
  implemented (old-rule) arbitration is the operative one, as claimed.
- Insurance re-run: `jsc-bughunt-wd3`, `JSC_BUGHUNT_WD3_STOCK=1`, repro.js
  N=4 x20 (`logs/r3-wd3-insurance-run*.log`): 2 PASS / **18 WATCHDOG-ABORT**.
  **18/18 aborts: windowsAtArm == windowsAtAbort** (14/14 x7, 15/15 x3,
  19/19 x2, 22/22 x4, 28/28, 32/32; budgetElapsed 30.000049-30.001139s).
  3/3 sampled cores (`bts/r3-wd3-insurance-core.1419246.bt` archived) show
  the H1 invariant: conductor in watchdogAssertStopProgress <-
  stopTheWorldAndRun <- Structure::didCachePropertyReplacement <-
  tryCachePutBy, sibling victims in WTF::Lock lockSlow. ZERO
  counterexamples to the resurrection criteria. Arbitration-livelock theory
  stays dead; the abort is the H1 wedge, full stop.

### 11.3 PWS-NEG-1 — park-wake angle: grep audit clean

Across ALL 117 extracted backtraces (`bts/*.bt`):
`PropertyWaiter` 0 hits, `reacquireParkedCarrier` 0 hits,
`GILDroppedSection` 0 hits, `parkForModeStop` 0 hits. The only `waitUntil`
matches are the stop-machinery's own bounded timed parks
(`s_jsThreadsParkCondition` ParkingLot waits and the conductor's 1ms
predicate waits — verified by frame context in core.585068.bt). No core has
an access-holding thread inside a JS-level wait loop or the W1 service
episode. Combined with 11.1 (0 aborts in 170 fresh landed-fix runs), the
park-wake angle is closed.

### 11.4 NPR-2 Yarr-class probe — abort prediction REFUTED; saving path identified; boundedness claim STRENGTHENED

Probe: `experiments/yarr-noPoll-probe.js` — thread A runs
`/(a+)+b/.exec('a'.repeat(40)+'c')`; main thread spins PROBE_SPIN_MS then
runs the repro.js existing-property replace-put warmup (proven Class-A fire
source). Run with pinned flags + `--useRegExpJIT=0` (forces the Yarr
interpreter), 75s external timeout. 8 runs (spin 1500/400/100ms):

- **NO watchdog abort, ever. rc=0 in 8/8 runs**; total wall ~1.82s.
- The stop DID wait on the Yarr thread: with spin=100ms, putWork took
  **1715-1731ms vs 23ms baseline** (putWork alone, same flags, no regexp
  thread) — i.e. the regexp thread IS non-quiescent for the whole match
  (hasHeapAccess, no poll: `grep traps/safepoint/VMTraps yarr/*` = 0 hits,
  confirming the no-poll fact), and the conductor's first Class-A window
  stalled until the match completed.
- The match completes because the Yarr interpreter is **structurally
  bounded**: `yarr/Yarr.h:52` `static constexpr unsigned matchLimit =
  100000000;`, decremented on every `matchDisjunction` entry
  (`YarrInterpreter.cpp:1776`), returning `ErrorHitLimit`
  (`YarrInterpreter.cpp:1777-1778`) after 1e8 steps == **~1.8s release wall
  on this host** (measured single-threaded: 1778ms; subject length does not
  extend it — the limit is per-exec). Default-JIT config: 859ms (YarrJIT
  bails nested quantifiers to the same interpreter). `ErrorHitLimit` has no
  consumer outside the interpreter (repo grep); it surfaces as a silent
  no-match — pre-existing upstream semantics, out of scope.

Verdict: the hypothesis's own refuteIf branch fired for the Yarr sub-claim
— the region is NOT unbounded, so it cannot produce a 30s watchdog abort
even outside the suite. The quiescence path that "saved" it is matchLimit
boundedness, not a hidden poll. This STRENGTHENS NPR-2's load-bearing
claim: every compute-bound no-poll region reachable from the suite (and
even the worst flagged out-of-suite class) is bounded orders of magnitude
below the 30s budget. Residual fact for the de-jank/TTL charter: a Class-A
stop can stall up to ~2s behind a pathological regexp (bounded jank, not a
correctness bug); a CheckTraps-style poll inside matchDisjunction would
close even that.

### 11.5 Round verdict

All four surviving hypotheses CONFIRMED in their negative/closure form;
no hypothesis revived; no new failure signature. The chartered STW-watchdog
abort remains fully and solely explained by the landed H1 fix
(pre-locker Class-A fire in tryCachePutBy). The bug is closed on this
evidence; remaining work items (third watchdog arm for in-window wedges,
Yarr poll for stop-latency, perf-facet mutator hot path) belong to other
charters.

## 12. EXPERIMENTER round 4 (2026-06-10) — consolidated closure soak + synthetic arbitration stressor

Binary under test: stock landed-fix `WebKitBuild/Release/bin/jsc` (mtime
17:02, `find Source -newer jsc` = empty; includes GC under-marking fix
25375a997f4f and the H1 pre-locker fire). NO Source/ edits this round; new
artifacts: `stw-watchdog/soak4.sh` (generalized WD-4 gate: silent timeout =
FAILURE, gdb all-thread dump before SIGKILL, optional NON-fatal mid-run gdb
sampling), `stw-watchdog/round4-driver.sh`,
`stw-watchdog/experiments/arb-lockfree-fires.js`.

### 12.1 The 400-run WD-4-gated soak (NPR4-A req 1+2, ARB-NEG-4 opt, TT3 opt)

All legs on the pinned GIL-off flags, every run individually gated
(hang threshold per leg; alive-past-threshold => gdb dump then kill,
classified FAILURE):

| leg | config | runs | result |
|---|---|---|---|
| 1 (ARB synthetic) | arb-lockfree-fires.js N=4 ARB_BATCHES=800, gate 60s | 50 | 50 PASS / 0 abort / 0 hang |
| 2 | richards-like N=4 scale=1/16 --useFTLJIT=0, gate 60s | 100 | 100 PASS / 0 abort / 0 hang |
| 3 | repro.js N=4, gate 60s | 100 | 100 PASS / 0 abort / 0 hang |
| 4 (TT3 tier-storm) | repro.js N=4 --useDFGJIT=1 --useFTLJIT=1 --thresholdForOptimizeAfterWarmUp=10, gate 90s | 50 | 50 PASS / 0 abort / 0 hang |
| 5 | string-heavy N=8 scale=1, gate 150s, gdb mid-run sample on first 3 | 100 | 100 PASS / 0 abort / 0 hang (98 via driver `round4-driver.out`, final 2 as `r4sh8b` after the background driver was reaped at run 98 — both PASS) |

TOTAL this round: **400 runs, 0 watchdog aborts, 0 silent hangs, 0 other.**
Cumulative landed-fix record: **0 aborts in 680 runs** (280 prior + 400).
`grep -rl "requester queued" logs/r4*.log` = **0** across all 400 logs.

### 12.2 string-heavy mid-run stack sampling (NPR4-A confirmIf clause 2)

`bts/r4sh8-midrun-run{1,2,3}.bt` — first gdb-at-mid-run sampling pass ever
taken on the string-heavy N=8 leg (19 threads each). Observed states, all
three samples: mutators EXECUTING in bounded allocation slow paths
(`GCClient::IsoSubspace::allocate` / `LocalAllocator::allocate` /
`JSRopeString::create` — poll-adjacent, O(allocation)), one HeapHelper in
`SlotVisitor::appendToMarkStack` (bounded marking step), remainder in
`pthread_cond_(timed)wait` service parks. ZERO threads simultaneously
(a) countable non-quiescent by the §A.3.2 predicate and (b) more than ~2s
from a poll or lock release; zero threads blocked on a WTF::Lock whose
holder sits in the stop protocol.

### 12.3 ARB-NEG-4 synthetic lock-free Class-A fire stressor

`experiments/arb-lockfree-fires.js`: per batch each thread builds a fresh
prototype Structure, compiles a UNIQUE-source `Function()` reader (unique
source defeats the code cache — a shared CodeBlock goes megamorphic after
~16 structures and fires plateau at ~15/run; measured), warms the proto
get IC (installs replacement watchpoints), then replaces the watched slot
ONCE via `Object.defineProperty` => `Structure::didReplaceProperty` fire
with NO ConcurrentJS lock held. Measured via the env-gated counters
(`JSC_CLASSA_FIRE_STATS=1` / `JSC_CLASSA_FIRE_LOG=1`, stock binary):

- **3202 stop fires / 3210 completed windows in 1.75s wall (~1.8k
  lock-free Class-A fires/sec), max window 11.3ms**; fire identities 15/17
  sampled = `[Property did get replaced]` (the intended putDirectInternal
  path), 0 `didCachePropertyReplacement`.
- x50 under the gate: 0 aborts, 0 hangs, 0 "requester queued" breadcrumbs.
  Queued concurrent fires coalesce and drain in ms exactly as the
  arbitration design claims, at fire rates ~2 orders above the suite's.

### 12.4 TT4 static characterization (latent in-window jettison locker; WD-4 blind spot)

Read-only verification of the latent-sibling claim (NOT the chartered abort):
- `watchdogAssertStopProgress` has exactly THREE call sites, all requester
  legs: `VMManager.cpp:607`, `VMManager.cpp:665`, `Heap.cpp:5824`. No arm
  samples an in-window conductor => the blind spot is structural.
- `CodeBlock::jettison`'s closure takes `ConcurrentJSLocker locker(m_lock)`
  INSIDE the stop window (CodeBlock.cpp ~2693), under the verbatim M4
  LOCK-ACQUISITION CAVEAT (CodeBlock.cpp:2678-2692).
- The caveat's chosen rule ("flag-on, ConcurrentJSLock critical sections
  must be PARK-FREE — M4's park hook must RELEASE_ASSERT...") is
  DOCUMENTED but NOT ENFORCED: grep for any ConcurrentJSLock-depth check in
  JSThreadsSafepoint.cpp / VMManager.cpp = 0 hits.
- Mitigating fact: `GCSafeConcurrentJSLocker` is DeferGC + lock
  (ConcurrentJSLock.h:71-97) — it DEFERS GC while held rather than parking
  with the lock, so today's only reachable holder-wedge shape was the H1
  EXECUTING-wedge, which is fixed. Dynamically: 0 silent hangs in 680
  gated runs. Verdict: REAL but LATENT; feeds the watchdog-hardening
  charter (third arm + park-hook assert), not this bug.

### 12.5 Round verdict

NPR4-A, ARB-NEG-4, TT3 closure forms all CONFIRMED with the strongest
evidence to date (string-heavy now has a gdb-sampled, fully gated 100-run
leg; the arbitration protocol survived a dedicated 1.8k-fires/sec lock-free
stressor; forced tier-transition traffic at threshold=10 produced nothing).
TT4 CONFIRMED as latent-only. No hypothesis revived; no new failure
signature; zero refuteIf discriminators fired. The chartered STW-watchdog
abort remains solely the H1 lock-holding fire, fixed.
