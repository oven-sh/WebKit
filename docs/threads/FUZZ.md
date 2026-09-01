# FUZZ.md — Fuzzilli setup for the shared-memory Thread API

Status: rig is set up and smoke-tested (10-minute run; see "Smoke results").
NO long campaigns have been run from this tree yet — campaigns launch via
thread-fuzz once the GIL-off bring-up stabilizes.

Path taken: **real Fuzzilli** (Swift toolchain installed; no fallback fuzzer
needed).

## Components

| Piece | Location |
|---|---|
| Swift toolchain | `/opt/swift` (swift.org 6.3.2-RELEASE for Amazon Linux 2023; not on PATH — use `/opt/swift/usr/bin/swift`) |
| Fuzzilli checkout | `/root/fuzzilli` (clone of google/fuzzilli, `main`) |
| JSCThreads profile | `/root/fuzzilli/Sources/Fuzzilli/Profiles/JSCThreadsProfile.swift`, registered as `jscthreads` in `Profiles/Profile.swift` |
| Fuzzilli binary | `/root/fuzzilli/.build/release/FuzzilliCli` |
| Target jsc | `WebKitBuild/Fuzz/bin/jsc` (REPRL + ASAN; own build dir, never Debug/Release/TSan) |
| Build script | `Tools/threads/fuzz/build-jsc-fuzz.sh` |
| Run script | `Tools/threads/fuzz/run-fuzzilli.sh` |
| In-repo profile copy | `Tools/threads/fuzz/JSCThreadsProfile.swift` + `fuzzilli-profile-registration.patch` (restore into a fresh fuzzilli clone if /root/fuzzilli is lost) |
| Corpus/crashes | `WebKitBuild/Fuzz/fuzzilli-storage/{corpus,crashes,...}` |

## Building

```bash
# jsc (REPRL + ASAN), into WebKitBuild/Fuzz only:
nice -n 10 bash Tools/threads/fuzz/build-jsc-fuzz.sh

# Fuzzilli (after editing the profile):
cd /root/fuzzilli && PATH=/opt/swift/usr/bin:$PATH nice -n 10 swift build -c release
```

The jsc configure line (what build-jsc-fuzz.sh runs):

```bash
cmake -S . -B WebKitBuild/Fuzz -G Ninja \
  -DPORT=JSCOnly -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/opt/llvm-21/bin/clang-21 \
  -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++-21 \
  -DENABLE_STATIC_JSC=ON -DUSE_BUN_JSC_ADDITIONS=ON -DUSE_BUN_EVENT_LOOP=ON \
  -DENABLE_FUZZILLI=ON -DENABLE_SANITIZERS=address -DENABLE_FTL_JIT=ON \
  -DCMAKE_C_FLAGS="-fno-omit-frame-pointer -g -fsanitize-coverage=trace-pc-guard" \
  -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g -fsanitize-coverage=trace-pc-guard"
nice -n 10 ninja -C WebKitBuild/Fuzz jsc
```

## The jscthreads profile

Extends the stock JSC profile with:

- Builtins/typing: `Thread` (join/asyncJoin/id, `Thread.current`,
  `Thread.restrict`), `Lock` (hold/asyncHold/locked), `Condition`
  (wait/asyncWait/notify/notifyAll), `ThreadLocal` (.value),
  `ConcurrentAccessError`.
- Code generators (all join their threads, all cross-thread ops guarded):
  - `ThreadSpawnGenerator` / `ThreadJoinGenerator` — spawn/join/asyncJoin.
  - `SharedObjectPropertyStormGenerator` — 2–3 threads add/read/write/delete
    a small fixed set of property names on one object.
  - `SharedArrayResizeRaceGenerator` — push/pop/length-write/sparse-write
    races against element reads.
  - `DictionaryFlipGenerator` — bulk add+delete to force dictionary
    transitions under cross-thread traffic, optional `Thread.restrict`.
  - `ThreadRestrictGenerator` — restrict from a spawned thread, violate from
    others (must raise ConcurrentAccessError, never corrupt).
  - `PropertyAtomicsGenerator` — Atomics add/sub/and/or/xor/exchange/
    compareExchange/load/store on (obj, propName), mixed with plain racing
    writes.
  - `PropertyAtomicsWaitNotifyGenerator` — Atomics.wait/waitAsync with 5–50ms
    timeouts + notify storms.
  - `LockContentionGenerator` — hold/asyncHold contention guarding shared
    mutation.
  - `ConditionWaitNotifyGenerator` — bounded predicate-loop wait vs notify
    storm.
  - `ThreadLocalGenerator` — per-thread .value divergence.
  - `SharedProxyGetterGenerator` — Proxy traps + self-mutating accessors on
    shared objects.
  - `CrossThreadJITWarmupGenerator` — JIT-warm hot function on a spawned
    thread racing shape changes (transitions, deletes, prototype swaps) from
    the spawner.
- Default flags: stock JSC JIT-threshold lowering + `--useJSThreads=true`.
- Rotating stress flags (per respawn, `--jobs` workers randomize
  independently): JIT tier toggles; threaded-IC kill switches
  (`--useThreadedLLIntICs/BaselineICs/DFG/FTL`); concurrent object-model
  stress (`--forceSegmentedButterflies`, `--forceButterflySWBit`,
  `--verifyConcurrentButterfly`, `--validateButterflyTagDiscipline`,
  `--useStructureAllocationLock`); `--jsThreadStackSizeKB`.
- Startup tests assert the Thread API is exposed and a spawn/join
  round-trips, so a regressed flag wiring fails fast instead of fuzzing
  nothing.
- `timeout=1000ms` (joins/waits are slow), `maxExecsBeforeRespawn=100`
  (threads can leak state across REPRL executions).

NOT enabled by default: the GIL-off configuration. The profile fuzzes
phase-1 semantics (GIL + stress flags exercise the concurrent object model).

## Campaign commands

```bash
# 10-minute, single-worker, timeout-bounded smoke (what was run here):
nice -n 10 bash Tools/threads/fuzz/run-fuzzilli.sh --smoke

# Real campaign (post-ungil / thread-fuzz): 4 workers, resumable storage
nice -n 10 bash Tools/threads/fuzz/run-fuzzilli.sh            # JOBS=4 default
JOBS=16 nice -n 10 bash Tools/threads/fuzz/run-fuzzilli.sh    # bigger box share

# Bare-metal equivalent of what the script runs:
ASAN_OPTIONS="detect_stack_use_after_return=0:abort_on_error=1:symbolize=1:detect_leaks=0:allocator_may_return_null=1" \
nice -n 10 /root/fuzzilli/.build/release/FuzzilliCli \
  --profile=jscthreads \
  --storagePath=/root/WebKit/WebKitBuild/Fuzz/fuzzilli-storage \
  --resume --timeout=1000 --jobs=4 \
  /root/WebKit/WebKitBuild/Fuzz/bin/jsc
```

`--resume` re-imports `fuzzilli-storage/corpus/` so campaigns continue where
the last one stopped. Crashes land in `fuzzilli-storage/crashes/` as
`.js` + `.fzil` pairs (program + protobuf); settings/stats in
`fuzzilli-storage/settings.json` and periodic `stats/` dumps.

### GIL-off variant (post-ungil only)

When the UNGIL ladder is green, add the unsafe-activation flags through
Fuzzilli's pass-through arguments (everything after `--` goes to jsc; check
`FuzzilliCli --help` for the current syntax) or edit the profile's
`processArgs` to append:

```
--useThreadGILOffUnsafe=true --useVMLite=true \
--useSharedAtomStringTable=true --useSharedGCHeap=true
```

Do not bother before then: U0 option validation forces `useThreadGIL=1`
without all four, and the bring-up tree crashes early on known issues —
the campaign would only rediscover the bring-up backlog.

### Triage

```bash
# Reproduce a crash:
WebKitBuild/Fuzz/bin/jsc --useJSThreads=1 <flags from the crash file header> crash.js

# FuzzIL tooling (minimization happens automatically during the campaign;
# lifted .js is already minimized):
cd /root/fuzzilli && swift run FuzzILTool --liftToFuzzIL crash.fzil
```

**ASAN_OPTIONS lane pin (CVE-AUDIT B9 / MC-GC S2a):** every Linux ASAN
threads lane — the fuzz target included — MUST set
`detect_stack_use_after_return=0`. With UAR on (clang's recent default),
address-taken locals live on a heap fake-stack outside `thread.stack()`; the
T5 cooperative parked-root scan captures `&local` as `stackTop` and the
conductor's `copyMemory` walks a span off the mapped OS stack
(`MachineStackMarker.cpp:128` SEGV; reproduced in-tree as
`JSTests/threads/cve/mc-gc-s2a-uar-fakestack.crash.txt`). The engine-side
publish/consumer bounds-checks decline-and-fall-back so this is not a
correctness hole, but UAR-on degrades every cooperative snapshot to a
SIGUSR2 suspend and any new publish site that escapes UAR instrumentation
re-opens the fault — pin it off so the fuzzer exercises the production
shape. `Tools/threads/fuzz/run-fuzzilli.sh`'s `ASAN_OPTIONS` export must
include the same `detect_stack_use_after_return=0` term (add it if absent);
the bare-metal command above already does.

Crash dedup is by ASAN signature + Fuzzilli "crash behaviour is
deterministic/flaky" tagging in the .js header comments. Thread bugs are
often flaky — keep flaky crashes; rerun under
`WebKitBuild/TSan/bin/jsc` (the bring-up tree's TSAN no-JIT build) for a
race report when a crash does not reproduce under ASAN.

## Smoke results (this setup run, 2026-06-07)

10-minute single-worker smoke (`run-fuzzilli.sh --smoke`) against the GIL'd
phase-1 tree:

- Coverage feedback WORKS: 1,200,345 edges instrumented; 5.58% edge coverage
  reached during initial corpus generation.
- Corpus GROWS: 918 total samples, 459 interesting, corpus size 453 at stop;
  82% correctness rate, <1% timeout rate, ~64 execs/s (single worker, shared
  box, nice -n 10).
- Startup tests pass: REPRL handshake, FUZZILLI_CRASH 0/1 detection, Thread
  API exposure, spawn/join round-trip. (FUZZILLI_CRASH 2 = ASSERT(0) is a
  no-op in RelWithDebInfo and is intentionally not tested.)
- 17 crashes (15 unique deterministic files) found already, in
  `WebKitBuild/Fuzz/fuzzilli-storage/crashes/`. NOT triaged here (that is
  thread-fuzz's job). Two example signatures:
  - `Atomics.store([-15132,-1024]);` — abort (SIGABRT) in the
    Atomics-on-properties dispatch when arg0 is a plain JS array and the
    property-key/value args are absent. Reproduces standalone:
    `WebKitBuild/Fuzz/bin/jsc --useJSThreads=1 crash.js` (exit 134).
  - `class C2 extends f0 { static 3188015491 = 4294967296; static #f; }; gc()`
    — SIGABRT (likely pre-existing, not threads-specific; appears with
    --useJSThreads=1 default-on in every execution of this profile).

Caveat: the corpus in `fuzzilli-storage/corpus` was left in place; campaigns
run with `--resume` and will continue from it. A later jsc rebuild changes
edge numbering — Fuzzilli re-evaluates imported programs on resume, so this
is safe, just slower on the first sync.

## Re-verification (2026-06-10)

The Fuzz jsc was rebuilt incrementally against the current bring-up tree
(`build-jsc-fuzz.sh`, exit 0, post-build Thread API check OK) and the
10-minute smoke re-run:

- 1,200,910 edges instrumented; startup tests pass (REPRL handshake, crash
  detection, Thread API exposure, spawn/join round-trip).
- Resume import works: prior corpus re-evaluated against the new binary;
  corpus 503 in-fuzzer at stop, on-disk corpus grew 936 -> 1006 files.
- Coverage feedback works: 6.19% edge coverage at stop (up from 5.58% on
  2026-06-07).
- Correctness 76%, timeout rate 2.6%, ~60 execs/s steady-state (single
  worker, nice -n 10, shared box).
- 9 crashes found this run (crashes/ 45 -> 47 files after dedup). Still
  untriaged — triage is thread-fuzz's job.

Operational note: if a smoke/campaign is interrupted mid corpus import,
Fuzzilli leaves `fuzzilli-storage/old_corpus/` behind and refuses to start.
If it is empty, `rmdir` it; if not, move its contents back into `corpus/`
before relaunching.

## Re-verification (2026-06-19)

The Fuzz jsc was rebuilt incrementally against the post-closeout tree
(`build-jsc-fuzz.sh`, 426 ninja targets, exit 0, post-build Thread API check
OK). `run-fuzzilli.sh` was fixed to export
`detect_stack_use_after_return=0` in `ASAN_OPTIONS` (the lane-pin note above
required it but the script had not been updated). Two smoke runs:

**10-min `--resume` smoke** (against the accumulated 2459-program corpus):

- 1,175,199 edges instrumented; startup tests pass (REPRL handshake, crash
  detection, Thread API exposure, spawn/join round-trip).
- Coverage feedback works: 6.70% -> 10.15% edge coverage during import.
- Entire 10 minutes spent in corpus import (43.92% of 2459 programs at
  SIGINT) — the corpus is now large enough that a rebuild's edge-renumber
  re-evaluation does not finish inside the smoke window. SIGINT-during-import
  left `old_corpus/` behind; merged back into `corpus/` (7314 files on disk;
  Fuzzilli dedups semantically on the next full import).

**4-min fresh-storage smoke** (`STORAGE=/tmp/fuzz-fresh-storage`, no
`--resume`) to confirm the mutation engine actually fuzzes the new binary:

- Fuzzer state reached "Initial corpus generation (GenerativeEngine)";
  on-disk corpus 0 -> 262 files in 4 minutes; ~40 execs/s, 71% correctness.
- 4 unique deterministic crashes written to `crashes/` (TERMSIG 6). NOT
  triaged here — triage is thread-fuzz's job. One example signature:
  `Object.defineProperty(arr, 'acc', {get/set self-mutating}); gc()` ->
  SIGABRT (likely the same pre-existing class-static/gc family seen on
  2026-06-07; appears with `--useJSThreads=1` default-on in every execution
  of this profile, not threads-specific).

Path is unchanged: **real Fuzzilli**. The rig is ready for campaigns
(`thread-fuzz`).

## Campaign r3b (2026-06-19, 4h, post-§46+TSAN tree 8a250c15)

7670-file corpus resume, 4 jobs. **128 new crashes**, 2 unique signatures:

| count | signature | repro |
|---|---|---|
| 125 | `ASSERT !hasAnyArrayStorage(source->indexingType())` at `ConcurrentButterfly.cpp:1064` `trySegmentedTransition` ← `tryPutDirectTransitionConcurrent` ← `putDirectInternal` | `Tools/threads/fuzz/crashes/r3/r3-001-trySegmentedTransition-ArrayStorage.js` |
| 1 | ABRT in `storeTaggedButterflyWordConcurrent` ← `setButterflyConcurrent` ← `setButterfly` | `Tools/threads/fuzz/crashes/r3/r3-002-storeTaggedButterflyWordConcurrent.js` |
| 1 | NOREPRO | — |
| 1 | exit-3 | — |

**r3-001** is a hole in the §45 StayFlatShared gate (or its surrounding
precondition): an ArrayStorage-indexed object reaches `trySegmentedTransition`
on a property add. Single-threaded, `--useJSThreads=true` only. 125 variants
of essentially one repro shape. Debug repros deterministically.

**Prior-campaign re-triage** (same tree): 292/292 NOREPRO — all 06-07/06-10
crashes closed by §46+TSAN.

## r3b closure + campaign r47 (2026-06-19, post-§47 tree)

Fuzz jsc rebuilt against the §47 working tree (`tryArrayStoragePropertyTransition`
+ I35 CoW materialize-first reroute in `tryPutDirectTransitionConcurrent`;
`build-jsc-fuzz.sh` 201 ninja targets, exit 0, Thread API check OK).

**r3b re-triage** (`Tools/threads/fuzz/triage-r3b-batch.sh`, 136 files,
`fuzzilli-storage{,-B,-C}/crashes` 2026-06-19 02:28-06:30 incl.
`duplicates/`):

| count | signature |
|---|---|
| **134** | **NOREPRO** |
| 1 | `storeTaggedButterflyWordConcurrent` ← `JSArrayBufferView::slowDownAndWasteMemory` (= r3b-001; r3-002 family, see r47 below) |
| 1 | exit-3 (uncaught RangeError; the saved r3-002 source — no crash) |

**0× r3-001 signature** (`trySegmentedTransition`). Pre-fix the same triage
showed 12× r3-001-signature: those 12 are the **CoW** I35 entry assert
(`!isCopyOnWrite` at `ConcurrentButterfly.cpp:1068`, NOT the AS one) — a CoW
literal getting an out-of-line `Object.defineProperty` add when E4 is
ineligible. Closed by the I35 materialize-first reroute. INDEX:
`Tools/threads/fuzz/crashes/r3b/INDEX.tsv`; full per-file table:
`WebKitBuild/Fuzz/triage-r3b/all.tsv`.

**Saved r3 repros 20× Debug `--useJSThreads=true`**: r3-001 20/20 (rc=0),
r3-002 20/20 (rc=3 uncaught RangeError stack-overflow, no crash). Regression
tests: `JSTests/threads/objectmodel/array-storage-property-transition.js`,
`JSTests/threads/objectmodel/cow-named-property-transition.js`.

**Campaign r47** (2h fresh-storage, 4 jobs, 09:23:40Z-11:23:41Z):
423,374 execs, 11.58% edge coverage. **9 crashes / 8 reproducible / 2 unique
signatures, 0× r3-001**:

| count | signature | #7 caller |
|---|---|---|
| 5 | `storeTaggedButterflyWordConcurrent` owner-TID assert | `JSArrayBufferView::slowDownAndWasteMemory` ×3, `shiftButterflyAfterFlattening` ×1, `Structure::flattenDictionaryStructureImpl` ×1 |
| 3 | SEGV `DeferrableRefCounted::ref` ← `RefPtr<ArrayBuffer>` ← `isArrayBufferViewOutOfBounds` | `validateTypedArray` (poison `arrayBuffer()` mid-wastage) |
| 1 | NOREPRO | — |

One root family: the manifest-7 `setButterfly` caller audit has three
foreign-TID escapes (slowDownAndWasteMemory; the two dictionary-flatten
sites). DEFERRED — needs a tag-preserving cell-locked publication design
(SCALEBENCH.md §47 "NEW residual"). Repros + ASAN reports:
`Tools/threads/fuzz/crashes/r47/`; log: `Tools/threads/fuzz/campaign-r47.log`.

## r47 closure + campaign r48 (2026-06-20, post-§48 tree)

Fuzz jsc rebuilt against the §48 working tree (r47 manifest-7 setButterfly
audit-escape closure: `slowDownAndWasteMemory` cell-locked tag-preserving
CAS + arrayBuffer-before-butterfly publication order; `shiftButterflyAfter-
Flattening` / `flattenDictionaryStructureImpl` world-stopped tag-preserving
store; `existingBufferInButterfly` + JIT `emitLoadTypedArrayArrayBuffer`
segment-aware — a Wasteful TA view CAN segment via foreign named-prop adds;
the prior "never segmented" comment is false).

**r47 + r3b-survivor re-triage** (`Tools/threads/fuzz/triage-r48.sh`, 11
files = 9 r47-storage + r3b-001 + r3b-002):

| count | signature |
|---|---|
| **10** | **NOREPRO** |
| 1 | exit-3 (r3b-002 RangeError; unchanged) |

**0× r47-family** (`storeTaggedButterflyWordConcurrent` /
`slowDownAndWasteMemory` / `DeferrableRefCounted`). The 8CAE8CE0 file went
NOREPRO only after the JIT segment-aware fix (runtime-only first round: SEGV
@ 0xbadbeef0+0x20 in JIT iterator-next ← `slow_path_spread`).

**Saved r47 repros 20× Debug `--useJSThreads=true`**: r47-001 20/20,
r47-002 20/20 (after `existingBufferInButterfly` segment-aware; first round
0/20 on `ASSERT(!isSegmentedButterfly)` @ `JSObject.h:1659`). Regression
tests: `JSTests/threads/objectmodel/r47-foreign-dictionary-flatten.js`,
`r47-typedarray-slowdown-wastememory.js`,
`r48-typedarray-segmented-arraybuffer.js`.

**Campaign r48** (2h fresh-storage, 4 jobs, 3 legs at host ~1h bg-task cap,
2026-06-19T23:07:32Z – 2026-06-20T01:26:42Z): **310,666 execs**, 9.93% edge
coverage. **2 crashes**, both `_flaky`, **2/2 NOREPRO at 10× retry**, **0×
r47-family signatures**. INDEX (empty):
`Tools/threads/fuzz/crashes/r48/INDEX.tsv`; log:
`Tools/threads/fuzz/campaign-r48.log`.

A 9m47s pre-JIT-fix smoke leg (`fuzzilli-storage-r48-prekill`, 36,308 execs)
found 1× `ASSERTION FAILED: isPinnedPropertyTable()` — the pre-existing
2026-06-07 class-static/gc family (the "4-min smoke" footnote above); not
r47-related.
