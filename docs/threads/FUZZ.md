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
ASAN_OPTIONS="abort_on_error=1:symbolize=1:detect_leaks=0:allocator_may_return_null=1" \
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
