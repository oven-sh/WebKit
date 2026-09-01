# staging-threads — integration instructions (corpus-expansion suites)

Validated 2026-06-07 against `WebKitBuild/Debug/bin/jsc` (Debug + ASAN, GIL-ON
defaults: `--useJSThreads=1` only, one process at a time, `nice -n 19`,
60s/test). This smoke catches syntax/harness/flag/loop mistakes only;
race-catching power is validated post-integration by the ab17d ladder.

## 1. Integration: one `mv` per subtree

```sh
cd /root/WebKit
mv staging-threads/JSTests/threads/* JSTests/threads/
mv staging-threads/Tools/threads/*   Tools/threads/
```

Collision dry-run (performed 2026-06-07): every staged basename was checked
against the live trees — `JSTests/threads/{gc-stress,scaling,semantics}` and
`Tools/threads/{gc-stress-matrix.sh,scaling-gate.sh,INTEGRATE-gc-stress.md,
INTEGRATE-scaling.md}` — **zero collisions**. The two `mv` commands above are
non-clobbering as the tree stands.

Note: `scaling/harness.js` is a `//@ skip` library (loaded by the workloads
via `load("./harness.js")`), and the staged tests resolve
`load("../harness.js")` / `load("../resources/assert.js")` against the
EXISTING `JSTests/threads/harness.js` and `JSTests/threads/resources/` —
correct only after the `mv` lands. (Smoke ran in a /tmp mirror of the
post-integration layout.)

## 2. Which suites join the default run-tests.sh globs

| Suite | Disposition |
|---|---|
| `JSTests/threads/semantics/` | **JOINS the default corpus.** Add `"$JT"/semantics/*.js` to the corpus-collection glob loop in `Tools/threads/run-tests.sh` (line ~237). Self-contained, bounded, default flags (one test also needs `--useDollarVM=1` via its own `requireOptions`: none currently — only gc-stress uses it). |
| `JSTests/threads/gc-stress/` | **JOINS the default corpus** (default-GC-regime leg): add `"$JT"/gc-stress/*.js` to the same glob loop — see `Tools/threads/INTEGRATE-gc-stress.md` for the exact diff. The **matrix** (`Tools/threads/gc-stress-matrix.sh`, scribble/zombie/stress flag sweep) stays **OPT-IN**: hours of saturating load, never in the default run. |
| `JSTests/threads/scaling/` | **Workloads join the corpus at the fractional CORPUS_DEFAULT_SCALE** (correctness/determinism leg only); the **gate sweep** (`Tools/threads/scaling-gate.sh`) and the `SCALING_SELF_TRIPWIRE=1` ratio assertion stay **OPT-IN** — speedup measurement is meaningless while the GIL serializes mutators and the machine is owned by ab17d reruns. See `Tools/threads/INTEGRATE-scaling.md`. |

run-tests.sh edit (one line, suites are no-ops until the `mv` lands because
of the existing `[[ -e "$f" ]]` guard):

```sh
for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js \
         "$JT"/heap-*.js "$JT"/objectmodel/*.js "$JT"/vmstate/*.js \
         "$JT"/gc-stress/*.js "$JT"/semantics/*.js "$JT"/scaling/*.js; do
```

## 3. DEFERRED arm: test262-on-a-Thread (end-stage only, per Jarred)

Charter: run the upstream test262 conformance suite in fixed-size chunks
where each chunk executes INSIDE a `new Thread()` on a shared-realm VM (main
thread spawns, joins, and collects per-test verdicts), then diff the
pass/fail/skip vector against the same chunks run on the main thread of a
fresh VM. Any divergence — a test that passes on main but fails on a spawned
thread, or vice versa — is a per-lite/threads bug by definition (realm
identity, per-thread exception state, stack limits, locale/date caches,
RegExp state, Atomics semantics). It is deliberately the LAST verification
arm: the full suite is tens of thousands of files and hours of wall time
even on a release build, so it runs only once the GIL-off ladder is green
and the machine is no longer owned by statistical reruns. Deliverable when
launched: `Tools/threads/test262-on-thread.sh` (chunker + differ) plus a
divergence report checked against the frozen SPEC semantics sections.

## 4. Smoke results (final state of staging tree)

GIL-ON defaults, Debug+ASAN jsc, one test per process, 60s timeout each.

| Test | Result | Time | Notes |
|---|---|---|---|
| gc-stress/conservative-scan-register.js | PASS | 2s | |
| gc-stress/havebadtime-vs-indexed-fastpath.js | PASS | 5s | |
| gc-stress/watchpoint-storm.js | PASS | 2s | |
| gc-stress/zombie-uaf-canary.js | PASS | 11s | |
| scaling/harness.js | SKIP | — | library, intentional `//@ skip` |
| scaling/lock-fairness.js | PASS | 9s | |
| scaling/map-heavy.js | PASS | 18s | after scale recalibration (was TIMEOUT) |
| scaling/raytrace-like.js | PASS | 27s | after 96x96→48x48 + scale recalibration (was TIMEOUT at any frame count) |
| scaling/richards-like.js | PASS | 2s | |
| scaling/splay-like.js | PASS | 14s | |
| scaling/string-heavy.js | PASS | 24s | after scale recalibration (was TIMEOUT) |
| semantics/atom-rope-torture.js | PASS | 4s | |
| semantics/date-cache-churn.js | PASS | 7s | |
| semantics/frozen-seal-race.js | PASS | 0s | |
| semantics/ic-delete_by_id-vs-transition.js | **TIMEOUT** | 60s | engine-side wedge, see KNOWN-RED below |
| semantics/ic-get_by_id-vs-transition.js | PASS | 1s | |
| semantics/ic-get_by_val-vs-transition.js | PASS | 1s | |
| semantics/ic-in_by_id-vs-transition.js | PASS | 3s | |
| semantics/ic-instanceof-vs-transition.js | PASS | 0s | |
| semantics/ic-put_by_id-vs-transition.js | **TIMEOUT** | 60s | engine-side wedge, see KNOWN-RED below |
| semantics/ic-put_by_val-vs-transition.js | PASS | 2s | |
| semantics/oom-one-thread.js | SKIP | — | `//@ skip` added: heap-cap flags inert in this tree (see below) |
| semantics/private-fields-shared.js | PASS | 1s | |
| semantics/proto-cycle-race.js | PASS | 1s | |
| semantics/regexp-lastindex-shared.js | PASS | 1s | after oracle-count fix |
| semantics/stack-overflow-per-thread.js | PASS | 7s | |
| semantics/symbol-registry-cross-thread.js | PASS | 1s | |
| semantics/termination-storm.js | SKIP | — | intentional `//@ skip`: no termination hook in tree |

Totals: 22 PASS, 3 SKIP (all intentional/documented), 2 KNOWN-RED timeouts.

### KNOWN-RED: ic-put_by_id / ic-delete_by_id — engine livelock, GIL-ON, JIT

100% reproducible on this build. Signature (likely the ic-publish/repatch
family ab17d is hunting):

- `PASSES=120` finishes in ~2s; `PASSES>=135` runs **>12 minutes** without
  completing (killed). Sharp cliff, not gradual slowdown.
- `--useJIT=0` completes the full `PASSES=150` in ~3.5s. `--useConcurrentJIT=0`
  still wedges.
- gdb mid-wedge: main thread BUSY (not blocked) inside
  `JSC::Structure::addNewPropertyTransition` ←
  `JSC::JSObject::putDirectInternal` ← `operationPutByIdSloppyOptimize`
  (JITOperations.cpp:1470), called from baseline JIT frames; the spawned
  mutator thread is parked in `ThreadCondition::wait` (presumably waiting for
  the GIL the main thread never releases). All HeapHelpers idle.
- The wedge is INSIDE a single put/delete operation: a wall-clock tripwire
  added between JS ops (all 7 ic-* tests now carry it, 45s budget) never gets
  a chance to fire on this flavor; it converts only the slow-but-progressing
  flavor into a loud diagnostic failure.

Disposition: the tests are correct and keep full coverage (PASSES=150
intact); they will show as 2 red timeouts in the default corpus on this
build until the engine bug is fixed. Recommend routing this section to the
ab17d triage queue — do NOT water the tests down to green-wash the corpus.

### Fixes applied to the staging tree during smoke

1. `semantics/regexp-lastindex-shared.js` — oracle count corrected 4→5 (the
   subject has five `/ab+/g` matches: ab, abb, abbb, ab, abbbb).
2. `semantics/oom-one-thread.js` — `//@ skip` with explanation: the hard heap
   cap in `CompleteSubspace.cpp` (~218-221, ~284-287) compares against
   `WTF::ramSize()` directly, not `Heap::m_ramSize`, so `--forceRAMSize`
   cannot lower it (smoke-verified: ~1.5GB live hoard under a "64MB cap",
   zero OOM). The test's own vacuity guard correctly fails. Re-enable when
   the engine honors forced RAM size for the cap.
3. `scaling/harness.js` — `CORPUS_DEFAULT_SCALE` 1/32 → 1/128: the assumed
   200x debug slowdown is really 500-1000x on this Debug+ASAN build for
   allocation-heavy workloads (the harness's own "must measure before
   integration" note, now discharged in `INTEGRATE-scaling.md`).
4. `scaling/raytrace-like.js` — frame size 96x96 → 48x48 (a 96x96 frame costs
   ~6.8s under ASAN; per-pixel allocation profile unchanged).
5. `semantics/ic-*.js` (all 7) — added `MAIN_PHASE_BUDGET_MS` wall-clock
   tripwire in the innermost phase-walk loop (loud failure with phase/pass/
   obj progress instead of a silent harness timeout, for the progressing
   flavor of latency collapse; limitation documented in-file).
