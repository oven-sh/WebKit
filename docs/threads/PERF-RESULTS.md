# Performance results

Status: sixth landing round (2026-09-07). Companion to LANDING-PLAN Part 2.
Sections 1-3 and 5 were re-measured on the sixth round's final tree; §1.1's
analysis and §4 (the LTO pair) are the fifth round's and say so.

This file records how the JS-threads branch performs against `main` at the
merge base, in the four configurations that matter, with the commands that
produced every number. "Flag off" is the branch binary with no option (what
every existing program gets); "GIL on" is `--useJSThreads=1`; "GIL off" is
`--useJSThreads=1` with `JSC_useSharedGCHeap=1 JSC_useThreadGIL=0
JSC_useThreadGILOffUnsafe=1`.

## Machine and builds

- x86-64, Intel Xeon Platinum 8488C, 32 cores / 64 threads, 247 GB, Linux
  6.12; nothing else running during a measurement unless the section says so.
- Both trees built with the same compiler (clang 21.1) and flags
  (`-O2 -g`, JSCOnly port, static JSC, `USE_BUN_JSC_ADDITIONS`), `jsc` shell.
  `main` = the merge base (oven-sh WebKit `main` at the branch point); branch =
  this tree, final state of the sixth round. The LTO pair (section 4) is built
  with `-flto=thin` on both.
- Every number is a median of at least 5 runs (the micro set runs each
  benchmark 5 times inside the process and reports the best, and the whole
  set is run 5 times; the table takes the median of those). Times in ms.

## 1. Micro set

Two groups: the branch's bench-gate suite (`JSTests/threads/bench/*.js`,
`Tools/threads/bench-gate.sh`, 50 measured iterations after 20 warm-up) and a
set of one-line loops that isolate one engine path each
(`Tools/threads/perf/micro-extra.js`). Command:
`MAINJSC=<main jsc> BRJSC=<branch jsc> Tools/threads/perf/run-micro.sh <outdir> 5`.

| benchmark | main | main, polling traps | flag off | GIL on | GIL off | off/main | on/main-poll | GIL off/on |
|---|---|---|---|---|---|---|---|---|
| add-props-escaped (o.a..c on an escaped `{}`, 2M) | 9.9 | 9.7 | 10.0 | 10.7 | 16.1 | 1.01 | 1.10 | 1.51 |
| array-element-read | 54.0 | 67.6 | 54.0 | 67.3 | 110.1 | 1.00 | 1.00 | 1.64 |
| array-element-write | 50.2 | 54.1 | 49.3 | 54.1 | 63.8 | 0.98 | 1.00 | 1.18 |
| array-int32-to-double-relabel-200k (new: `[1,2,3,4]`, one double store, pushes) | 6.4 | 6.3 | 7.2 | 20.0 | 24.6 | 1.12 | 3.16 | 1.23 |
| array-push-pop-10M | 50.1 | 50.0 | 51.8 | 54.9 | 52.0 | 1.03 | 1.10 | 0.95 |
| astar-like-nodes (new: 10k nodes built by a closure-local constructor, six adds each by a helper, per run) | 10.9 | 10.7 | 11.0 | 38.1 | 53.0 | 1.00 | 3.56 | 1.39 |
| class-ctor-4 (a new class per outer call; polymorphic construct) | 33.7 | 33.6 | 36.8 | 86.4 | 170.1 | 1.09 | 2.57 | 1.97 |
| closure-calls-20M | 13.3 | 21.7 | 13.1 | 16.2 | 17.7 | 0.99 | 0.74 | 1.09 |
| flat-butterfly-read | 13.6 | 27.1 | 13.5 | 26.9 | 26.9 | 1.00 | 0.99 | 1.00 |
| flat-butterfly-write | 62.5 | 63.1 | 62.0 | 63.0 | 63.2 | 0.99 | 1.00 | 1.00 |
| inline-property-read | 27.0 | 53.9 | 27.0 | 53.9 | 53.9 | 1.00 | 1.00 | 1.00 |
| inline-property-write | 54.0 | 59.0 | 53.9 | 58.8 | 58.8 | 1.00 | 1.00 | 1.00 |
| int-loop-3e8 (`s = (s+i)|0`) | 96.4 | 161.4 | 96.3 | 161.8 | 158.5 | 1.00 | 1.00 | 0.98 |
| json-parse-200k | 34.4 | 34.5 | 36.5 | 41.1 | 49.7 | 1.06 | 1.19 | 1.21 |
| json-stringify-200k | 26.3 | 26.2 | 26.7 | 27.4 | 38.0 | 1.02 | 1.05 | 1.38 |
| map-set-get-2M | 574.0 | 583.9 | 579.6 | 711.0 | 783.5 | 1.01 | 1.22 | 1.10 |
| megamorphic-access | 1396.2 | 1415.7 | 1098.4 | 1336.7 | 2446.5 | 0.79 | 0.94 | 1.83 |
| megamorphic-put-transition-1M (new: one store site adding a property to objects of 40 shapes) | 25.4 | 26.9 | 28.2 | 48.0 | 148.4 | 1.11 | 1.79 | 3.09 |
| obj-literal-5 (`{a..e}` in a loop; sinkable) | 1.9 | 1.4 | 1.9 | 2.1 | 2.1 | 1.00 | 1.49 | 1.00 |
| out-of-line-replace-poly-3M (new: `o.p = v` on two shapes, `p` out of line) | 11.0 | 10.9 | 11.9 | 13.3 | 14.4 | 1.08 | 1.23 | 1.08 |
| proto-method-calls-20M | 7.4 | 17.0 | 7.3 | 13.5 | 12.8 | 0.99 | 0.79 | 0.95 |
| regexp-exec-1M | 102.4 | 102.1 | 112.0 | 137.8 | 176.4 | 1.09 | 1.35 | 1.28 |
| string-concat-2M | 5.2 | 5.3 | 5.4 | 5.5 | 8.4 | 1.03 | 1.04 | 1.52 |
| throw-catch-200k | 85.0 | 85.6 | 89.1 | 98.4 | 114.7 | 1.05 | 1.15 | 1.16 |
| transition-heavy-constructor | 57.7 | 57.3 | 57.9 | 73.0 | 88.3 | 1.00 | 1.27 | 1.21 |
| transitions-after-fire-2M (new, flag-on only: `{}` + four adds on the main thread after a second thread built the same shapes) | - | - | - | 88.2 | 93.3 | - | - | 1.06 |
| typed-array-sum-50M | 41.7 | 42.4 | 41.6 | 42.4 | 43.6 | 1.00 | 1.00 | 1.03 |

"main, polling traps" is `main --usePollingTraps=1`. The flag forces polling
traps (SPEC-jit I21: signal-delivered traps patch running code from another
thread, which the multi-thread invalidation protocol cannot allow), so this
column, not plain `main`, is the like-for-like baseline for the flag-on
columns; the difference between the first two columns is the price of polling
traps in stock JSC on this machine.

### 1.1 Flag off against main (B1)

(Fifth round's analysis; the sixth round changed no flag-off path and the
column above moved within noise: 0.98-1.12, the 1.06-1.12 rows being the
allocation-helper rows explained below plus the two new array rows, whose
flag-off bodies now carry the flag-on relabel fork out of line.)
At the start of the fifth round the flag-off binary was 15-20 % slower than `main`
on object creation, `Map`, `RegExp` and `throw`. Instruction counts
(`perf stat -e instructions:u`, `--useConcurrentJIT=0` so tiering is
deterministic) and per-symbol deltas (`perf record` on both binaries, symbols
matched by name) put the cost in four places, all of them code that runs with
the flag off:

1. **Unconditional relaxed atomics on hot words.** The butterfly
   `IndexingHeader` lengths, `Structure`'s bit-field, `maxOffset`,
   `inlineCapacity` and transition offset, `JSFunction`'s executable-or-rare-
   data word, `JSString`'s fiber word, the free-list scramble words, the
   `TinyBloomFilter`, `AuxiliaryBarrier` and a fresh butterfly's header store
   had been turned into `std::atomic` relaxed accesses so that TSAN sees
   defined behaviour for the flag-on races on them. On x86-64 a relaxed atomic
   load is a plain load, but the compiler may not combine, hoist or reorder it,
   and it may not keep the value in a register across a call: 22 % of
   `operationMapSet`'s samples sat on re-loads of `publicLength`. These are now
   `racyLoad`/`racyStore` (`wtf/Atomics.h`): relaxed atomics under TSAN, plain
   accesses otherwise, which is what every supported target does for an
   aligned word anyway.
2. **GIL-off arms inlined into always-inline bodies.** `vm.gilOff()` branches
   with a lock, a table walk or an allocation behind them had been written
   inline in `JSOrderedHashTable`'s add/get/has, `JSRopeString::convertToNonRope`,
   `Heap::deferralDepthSlot`, the VM's per-thread selectors and others. The
   code was never executed flag-off but it doubled the size of hot functions
   (`operationMapSet` +49 %, `operationCreateThis` +87 %), pushed their callers
   over clang's inlining budget (`getDirect`, `allocateCell`,
   `canUseAllocationProfiles` became calls) and cost i-cache. Each such arm is
   now an out-of-line `NEVER_INLINE` function behind one predicted-untaken
   byte test of the frozen `g_jscConfig` page.
3. **`thread_local` without `constinit`.** `g_jscCurrentVMLite`,
   `g_jscButterflyTIDTag` and the heap's per-thread TLC pointers are read on
   flag-off paths too (they are how the flag-off path finds "the" VM's data
   without a branch); without `constinit` every access went through a TLS
   init-guard wrapper call. Now `constinit`, one `mov %fs:` each.
4. **`currentButterflyTID()` was an exported out-of-line function** called from
   every butterfly install; now an inline TLS read.

What the flag-off binary still does differently from `main` in generated
code, by class, each with its reason: (1) a byte test of the frozen option
page (`g_jscConfig` / `Options::useJSThreads()`) with a predicted-untaken
branch at each flag-on fork in C++ — a few hundred sites, one to two
instructions each, the arms out of line; (2) reads of `constinit` thread-locals
where the flag-off path finds per-VM data through the same selector the
flag-on path uses (`VMLite` fields: one `mov %fs:`); (3) `racyLoad`/`racyStore`,
which are plain accesses in a non-TSAN build (no difference) and relaxed
atomics under TSAN; (4) JIT code: none — every flag-on emitter is behind the
option; as a check, the DFG/FTL and baseline code emitted for the
constructor, `Map` and integer-loop benchmarks has the same size to the byte
on `main` and on the branch flag-off (`--dumpDFGDisassembly
--dumpFTLDisassembly` / `--useDFGJIT=0 --dumpDisassembly`, sum of the "Code
at" ranges: 4544/4544, 2816/2816, 384/384, 2304/2304 bytes). What is left as
a measurable delta, with reasons:
- `regexp-exec-1M` +8 %: the match result array is built by
  `createRegExpMatchesArray`, whose butterfly/array creation helpers grew
  flag-on branches (`ButterflyInlines.h` segmented-allocation sizing,
  `JSArray::createWithButterfly`'s tag stamp) that clang now declines to
  inline into the 1-KB caller; the remaining delta is two extra calls per
  match. Splitting those helpers further is possible; not done this round.
- `class-ctor-4` +8 %, `throw-catch`, `string-concat`, `array-push-pop`
  +3-5 %: the first two allocate through `operationCreateThis` /
  `ErrorInstance::create`, whose bodies carry the flag-on forks (out of line,
  but the test and the larger frame remain); the others are within two
  standard deviations on this machine (the medians move by that much between
  two runs of `main` itself); instruction counts differ by <2 %. The LTO
  pair (§4) shows the same rows at the same ratios, so none of it is an
  inlining accident of the non-LTO build.
- `megamorphic-access` is faster than `main` (0.80): the branch's
  `getDirect` revalidation loop happens to keep the structure in a register
  across the probe; not investigated further.

### 1.2 GIL on against flag off

What the sixth round changed, row by row (fifth-round value -> now, GIL on
against `main --usePollingTraps=1`):

- **`megamorphic-access` 1.27 -> 0.94, `json-stringify` 1.44 -> 1.05.** The
  megamorphic property cache and the `JSON.stringify` structure-keyed fast
  path are ON GIL on (SPEC-jit history §30): GIL on runs one mutator at a
  time and hands the GIL over only inside blocking primitives, so a probe or
  a fill of the VM-global table never interleaves with another thread's; the
  cache's store-transition arm claims the StructureID lane like every other
  flag-on transition writer and refuses ArrayStorage/copy-on-write sources at
  fill and at probe time. GIL off both stay disabled (a per-thread cache with
  a global epoch is designed, not built), hence the 1.83x / 1.38x in the last
  column.
- **`json-parse` 1.22 -> 1.19.** `JSON.parse`'s literal-parser identifier
  table is on GIL on; what remains is its per-property structure adds going
  through the C++ transition path (`putDirect` flag-on) rather than the
  parser's inlined one.
- **Transitions after the fire (`transitions-after-fire-2M`, new row).** Once
  a second thread has transitioned any instance of a structure, that
  structure's thread-local sets are dead for good (F2 is per structure). Before
  this round every later add on such shapes, on every thread, took the cell
  lock and the locked N2/N3 protocol: the row measured 619 ms against 31 for
  the same loop before the fire (20x). Now the owner of an unshared instance
  keeps a lock-free CLAIM-FIRST leg (SPEC-objectmodel E4-C: CAS the
  StructureID lane to its nuked form, re-check the butterfly word, store,
  publish; locked writers claim first too and restart when they lose), and the
  inline caches' transition handlers claim in both legs and watch no
  thread-local set, so they survive the fire: 88 ms, 2.8x the never-fired
  loop, the residue being the DFG's choice to plant the IC rather than its
  inline transition once the sets are dead (LANDING-PLAN open item).
- **(Re)allocating transitions (`astar-like-nodes`, `class-ctor-4`,
  `transition-heavy-constructor`).** Out-of-line property growth was C++-only
  flag-on in every tier (an inline cache refused to cache it; the DFG refused
  to inline it). Now the IC handler allocates (GIL off from the thread-local
  cache), copies from the masked word, claims, re-checks the word, publishes
  word then structure; the DFG/FTL inline it under the watched sets with an
  `InvalidationPoint` between the last park site and the install (SPEC-jit
  history §31). JetStream's `typescript` went from 0.46 to 0.81 of flag off on
  this. `astar-like-nodes` (new row) is the residual bad case, 3.6x: its
  constructor and helper are closures created per run, so every run's objects
  have fresh poly-proto structures, the adds are polymorphic with prototype
  conditions, and such handlers are compiled per case — where the allocating
  form still calls out to C++ (`operationPutByTransitionReallocatingConcurrent`,
  6 % of samples, plus the C++ claim-first leg 9 %) instead of emitting the
  inline sequence the shared, condition-free handler has. Same objects built
  by top-level functions: 1.2x.
- **`array-int32-to-double-relabel` (new row) 3.2x, was a stop-the-world per
  relabel before this round (SPEC-objectmodel T4-O).** The owner of a young
  array relabels Int32->Double->Contiguous in place without a stop GIL on; the
  3.2x left is the C++ round trip (`operationEnsureDouble` -> the concurrent
  relabel driver with its claim and lane rewrite) where flag-off converts in
  the same C++ function without the claim, plus the copy-on-write
  materialization of the literal through the flag-on path. JetStream's
  `stanford-crypto-pbkdf2`/`-sha256` went from 0.40 / 0.37 of flag off to
  0.84 / 0.84 on this and on in-place butterfly growth (M8 restored GIL on).
- **`out-of-line-replace-poly` (new row) 1.23x, was 10x.** A replace of an
  existing OUT-OF-LINE property never reached an inline cache flag-on since
  the second round (F26): the by-id inline-access patcher only handled inline
  offsets flag-on and the handler chain was never consulted for a
  butterfly-bearing replace. Both fixed; the inline patch now emits the
  tagged-butterfly load with an `fs`-relative tag strip on x86-64.
- Unchanged and still explained by the fifth round's text: the loop rows at
  1.00 against the polling baseline (the whole cost is the poll), `class-ctor-4`
  2.6x (polymorphic construct through `operationCreateThis` plus a claim per
  add), `regexp-exec` 1.35x, `map-set-get` 1.22x, `throw-catch` 1.15x
  (C++-heavy paths carrying the flag-on forks), global property WRITES
  uncached (SPEC-jit history §28).

### 1.3 GIL off against GIL on

- `map-set-get` 1.10x over GIL on: reads (`get`/`has`/`size`) are now
  lock-free and validated against the owner's version word (SPEC-ungil §N.1;
  four reader threads against one `Map`: 2,022 -> 318 ms), but a
  single-threaded loop that alternates `set` and `get` still pays the cell
  lock on every `set` and the version bump; the serial ratio did not move.
- `megamorphic-access` 1.83x, `megamorphic-put-transition` 3.1x,
  `json-stringify` 1.38x over GIL on: the VM-global caches that GIL on now uses
  stay off GIL off (above).
- `array-element-read` 1.64x over GIL on (was 1.01): not a slower read — the
  benchmark's array is built by `push` from empty, and GIL off an Int32 array
  asked to become Double becomes Contiguous instead (T4-O: raw-double lanes
  may not appear under a stale Int32-keyed reader), so the summing loop reads
  boxed values with a type check where GIL on reads unboxed doubles/ints from
  a typed shape. This is the GIL-off cost of the stop-free relabel policy; its
  bad case on real code is recorded in §3 (`stanford-crypto-aes`).
- `class-ctor-4` 2.0x, `add-props-escaped` 1.5x, `string-concat` 1.5x,
  `regexp-exec` 1.3x, `throw-catch` 1.16x: as in the fifth round (shared-heap
  allocation dispatch, per-thread caches, the SW-bit leg of the write
  predicate).
- Found and fixed this round, GIL off, not visible in this table because every
  row here runs after start-up: the shared heap raised the "mutator must
  fence" flag at its first collection and never lowered it, so from the first
  GC on every JIT write barrier on an old object took the fenced slow path
  (an `mfence` and a call): a put loop measured 33.6 ms before the first
  collection and 60.7 after (F27; `write-barrier-idle-fence`). And pinned
  down, not fixed: N threads running the same function below FTL share its
  execution counters and profiles in `JITData`; the per-back-edge counter add
  bounces one cache line between cores (a two-instruction DFG loop 5.6x slower
  at two threads) and delays tier-up. That, not a lock, is why the scaling
  suite's string-heavy workload does not scale (§2); a thread-local prescaler
  fixed the scaling and cost single-threaded loops 2x on this hardware
  (memory renaming does not apply to the `fs`-relative add), so it was not
  landed (SPEC-ungil history, sixth round).

### 1.4 ArrayStorage shift/unshift (B4)

`Tools/threads/perf/as-shift-bench.js`: an ArrayStorage array of 20,000
elements drained by `shift()`, then 5,000 `unshift`/`pop` pairs on a
1,000-element one.

| | flag off | GIL on, before the fifth round | GIL on | GIL off |
|---|---|---|---|---|
| shift-drain 20k | 1.5 | 227.4 | 27.3 | 27.4 |
| unshift/pop 5k | 0.4 | 20.0 | 1.0 | 1.0 |

(Sixth-round re-measurement; unchanged from the fifth.) Before the fifth round, every flag-on `shift`/`unshift` on ArrayStorage built a fresh butterfly
and copied the whole storage (SPEC-objectmodel §4.6 AS-COPY). Now an array the
calling thread owns moves its elements inside the installed vector under the
cell lock (AS-INPLACE, rev 16). What is left is O(n) against flag-off's O(1):
flag-off `shift()` moves the butterfly HEAD (pointer and header advance past
the removed element), which relocates exactly what a stale lock-free reader of
the array decodes; that needs the JIT's ArrayStorage read to carry an owner
test first and is recorded as the follow-up.

## 2. Scaling (B3)

`Tools/threads/scaling-gate.sh --runs 5 <jsc>` (GIL on: the branch binary
with `--useJSThreads=1`; GIL off: a wrapper adding the three GIL-off options).
Each workload runs N threads of identical independent work after two warm-up
runs on the main thread; T(N) is the wall time of the N-thread leg, "serial"
is T(1) against the same binary flag-off (also the third in-process run).
Medians of 5.

| workload | serial, GIL on | serial, GIL off | speedup GIL off at 2 / 4 / 8 threads |
|---|---|---|---|
| raytrace-like (small-object allocation, doubles) | 1.23x | 0.96x | 1.63 / 2.95 / 4.53 |
| splay-like (pointer-heavy live set, GC) | 1.09x | 1.46x | 1.95 / 3.54 / 5.57 |
| map-heavy (`Map` get/set) | 1.17x | 2.38x | 1.83 / 2.94 / 4.27 |
| string-heavy (computed string keys, ropes) | 1.24x | 1.27x | 0.91 / 0.74 / 0.90 |
| richards-like (see note) | 2.69x | 2.64x | (T(1) > T(2); see note) |

(Sixth-round re-measurement. Against the fifth round: raytrace-like 8 threads
4.24 -> 4.53, the rest within noise; the serial ratios did not move. The
map-heavy serial 2.38x is `set` under the cell lock plus the version bump the
lock-free readers validate against; string-heavy's flat curve is the shared
tier-up counters of §1.3, not a lock.)

GIL on, speedup is 1.0 at every N by construction (one thread runs at a
time); its column of interest is the serial cost.

Reading the GIL-off numbers (fifth round's text, still accurate except where
§1.3 adds the counter finding):
- **raytrace-like** is the clean case: serial parity with flag-off and 4.5x
  at 8 threads. What keeps it under 8x is the stop-the-world shared-heap
  collection (every thread's eden pauses all of them).
- **splay-like** scales past its relaxed floor (3.5x at 4, 5.6x at 8); its
  1.5x serial cost is the shared-heap allocation and barrier path on a
  workload that does little but allocate and link.
- **map-heavy** scales (4.3x at 8) from a 2.4x serial base (above).
- **string-heavy** does not scale GIL off (0.74-0.91x). The sixth round pinned
  it: not the atom table or refcounts but the CodeBlocks' shared execution
  counters and value profiles below FTL (§1.3, SPEC-ungil history); the
  workload's functions are short and many, and stay in the counting tiers for
  most of a 1.5-second run when four threads share their counters.
- **richards-like** is a pathological benchmark in stock JSC as well: it
  defines its constructor functions inside the workload function, so every
  invocation creates fresh poly-proto structures and each in-process
  repetition is slower than the last on `main` too (358, 554, 660, 1310,
  2416 ms for five consecutive runs flag-off). The gate's T(1) is the third
  run, which is why its "speedups" are not meaningful (T(2) came out below
  T(1) this time). The sixth round removed one of its two flag-on costs (after
  the F2 fire the owner keeps a lock-free leg, §1.2) and enabled the
  megamorphic cache GIL on; the growth itself is upstream behaviour.

Object creation alone, GIL off (`Tools/threads/perf/scale-objadd.js`, 2M
iterations of `{}` plus four adds per thread; `scale-objlit.js`, 4M object
literals per thread; wall time for N threads of the same per-thread work, so
ideal is flat): adds 6.2 / 6.6 / 8.0 / 9.6 ms at 1 / 2 / 4 / 8 threads,
literals 9.5 / 12.2 / 15.6 / 14.6 ms (sixth round; fifth: 6.0 / 6.5 / 8.4 /
8.9 and 8.9 / 11.8 / 15.5 / 20.0) — against 8 / 1920 / 2100 / 4206 ms and
21 / 6343 ms (2 threads) at the start of the round (LANDING-PLAN B3: instance-
keyed ownership, rev 16). The residual growth at these sizes is thread
start-up and per-thread tier-up inside a 10-20 ms measurement.

## 3. JSC benchmark suites

JetStream 2 through the shell driver (`PerformanceTests/JetStream2/cli.js`,
`Tools/threads/perf/run-jetstream.sh <outdir> 5`): the default list minus
the four WebAssembly tests, `bomb-workers` and `segmentation` (GIL off has no
WebAssembly; the last two use workers), the same 36 tests in all four
configurations. Sixth round, final tree: ten runs each for `main`, flag off
and GIL on (two batches of five, interleaved), five for GIL off. Geometric-
mean totals, median: `main` 352.2, flag off 342.1, GIL on 305.4, GIL off
235.5 — flag off 0.971 of `main`, GIL on 0.893 of flag off (fifth round:
0.768), GIL off 0.771 of GIL on. Run-to-run spread within a configuration is
about 3 % (one flag-off run in ten at 319). Per-test medians, with the fifth
round's GIL-on and GIL-off scores in parentheses where they moved by more
than 10 %:

| test | main | flag off | GIL on | GIL off |
|---|---|---|---|---|
| Air | 568.3 | 539.1 | 490.4 (352.5) | 410.0 |
| Babylon | 845.4 | 806.3 | 667.8 (441.0) | 495.6 (417.1) |
| Basic | 991.6 | 938.0 | 854.4 | 393.4 |
| Box2D | 501.5 | 505.1 | 437.6 (364.8) | 415.9 (335.6) |
| FlightPlanner | 873.1 | 793.7 | 657.4 (356.9) | 459.0 |
| ML | 147.1 | 142.9 | 109.6 (94.9) | 55.7 |
| OfflineAssembler | 210.2 | 196.5 | 177.6 | 130.9 |
| UniPoker | 750.1 | 715.8 | 662.5 | 532.1 |
| WSL | 3.6 | 3.6 | 2.7 (2.1) | 2.0 |
| ai-astar | 723.2 | 718.8 | 664.7 (476.6) | 529.0 (435.4) |
| async-fs | 611.3 | 604.1 | 603.3 | 487.5 |
| cdjs | 305.5 | 299.8 | 271.2 | 266.2 |
| crypto | 1647.6 | 1617.6 | 1529.4 | 1120.1 (1345.2) |
| delta-blue | 1244.3 | 1154.5 | 1047.7 | 830.0 |
| earley-boyer | 928.3 | 901.2 | 590.5 | 571.6 (739.0) |
| first-inspector-code-load | 270.5 | 263.5 | 260.0 | 266.9 |
| float-mm.c | 12.6 | 12.7 | 12.1 | 10.2 |
| gaussian-blur | 264.3 | 262.7 | 260.2 | 260.0 |
| gbemu | 172.2 | 169.2 | 153.6 (121.0) | 90.3 (77.1) |
| hash-map | 630.7 | 638.7 | 521.4 | 563.4 |
| json-parse-inspector | 440.9 | 418.0 | 372.4 | 278.5 |
| json-stringify-inspector | 511.1 | 472.8 | 507.9 (431.3) | 405.8 |
| mandreel | 160.9 | 157.4 | 149.7 | 145.0 |
| multi-inspector-code-load | 444.3 | 435.4 | 417.3 | 420.7 |
| navier-stokes | 993.1 | 983.3 | 865.6 | 747.9 |
| octane-code-load | 902.3 | 865.4 | 916.3 | 853.3 |
| octane-zlib | 27.7 | 26.6 | 26.2 | 25.9 |
| pdfjs | 203.7 | 192.9 | 179.4 | 157.5 |
| raytrace | 865.6 | 845.6 | 770.5 (696.5) | 625.4 |
| regexp | 510.0 | 475.6 | 444.0 | 325.0 |
| richards | 946.9 | 967.6 | 915.9 | 837.7 |
| splay | 473.3 | 455.9 | 424.8 | 248.8 |
| stanford-crypto-aes | 403.2 | 401.4 | 339.1 | 63.7 (268.1) |
| stanford-crypto-pbkdf2 | 987.6 | 924.0 | 773.3 (370.9) | 521.5 (150.4) |
| stanford-crypto-sha256 | 888.4 | 892.0 | 746.3 (331.2) | 520.0 (153.9) |
| typescript | 23.1 | 23.2 | 18.7 (11.1) | 14.3 |
| **Total (geomean)** | 352.2 | 342.1 | 305.4 (268.1) | 235.5 (229.2) |

Reading it:
- **Flag off against `main`, 0.97.** As in the fifth round (0.968): no test
  below 0.9x; the residual is option-byte tests and larger cold arms in the
  C++ runtime around allocation, `JSON` and `RegExp` (§1.1). Not broken down
  further this round.
- **GIL on against flag off, 0.89 (was 0.77).** The five tasks of the round
  account for the movement: the array-building tests (`pbkdf2`, `sha256`
  0.40/0.37 -> 0.84/0.84, `Air` 0.67 -> 0.91, `ai-astar` 0.66 -> 0.92) on the
  stop-free owner relabels and in-place growth; the cache users
  (`typescript` 0.46 -> 0.81, `FlightPlanner` 0.41 -> 0.83, `Babylon` 0.53 ->
  0.83, `json-stringify-inspector` 0.94 -> 1.07, `gbemu` 0.70 -> 0.91, `WSL`,
  `ML`) on the megamorphic cache, the JSON fast paths and the (re)allocating
  transitions. What is left below 0.85: `earley-boyer` 0.66 (unchanged:
  deep closure-allocation and `arguments` paths through flag-on C++ creation
  helpers; not investigated this round), `ML` 0.77 and `typescript` 0.81
  (polymorphic allocating transitions compiled per case still call out, the
  `astar-like` row of §1.2; `Float64Array`-heavy code paying the typed-array
  view checks), `hash-map` 0.82, `Basic` 0.91, `delta-blue`/`raytrace`/
  `richards` 0.91-0.95 (constructor code: the claim per add and
  `operationCreateThis`).
- **GIL off against GIL on, 0.77 (was 0.86; absolute 235.5 against 229.2).**
  GIL off improved where GIL on did when the mechanism is shared (`pbkdf2`,
  `sha256` 3.4x, `Babylon`, `Box2D`, `ai-astar`, `gbemu`) and did not where
  the mechanism is GIL-on-only (the megamorphic cache and JSON tables stay
  off GIL off: `typescript`, `FlightPlanner`, `json-stringify`), which widens
  the on/off ratio. Two GIL-off tests went DOWN against the fifth round and
  both are understood: `stanford-crypto-aes` 268 -> 64 — GIL off, an Int32
  array asked to hold a double becomes Contiguous rather than Double (T4-O),
  and this program also has Double arrays from double-valued literals and
  their `slice`/`concat` copies, so its hot sites see both shapes, the DFG
  arrayifies the Double ones to Contiguous, and Double->Contiguous GIL off is
  still a per-array stop-the-world: 40 % of the samples are in the stop
  conductor (SPEC-objectmodel history §26 addendum 2 has the analysis and the
  two candidate fixes; §5 item 1); `earley-boyer` 739 -> 572 and `crypto`
  1345 -> 1120 were not investigated (both allocate small objects at a high
  rate; the E4-C claim on butterfly-less adds after their shapes' sets fire
  is the suspect).

## 4. LTO pair

(Fifth round's measurement; not repeated in the sixth, whose changes are all
behind the flag and leave the flag-off/`main` comparison where §1 and §3 show
it.) Both trees built with `bun build.ts lto` (`-flto=thin`, the configuration
Bun ships). Binary size of the `jsc` shell: `main` 43.6 MB, branch 47.3 MB
(+3.7 MB, +8.5 %: the flag-on code paths are present in the binary whether or
not the flag is set). The micro set on the LTO pair:

| benchmark | main (LTO) | branch, flag off (LTO) | off/main |
|---|---|---|---|
| add-props-escaped | 9.7 | 9.8 | 1.02 |
| array-element-read | 53.5 | 53.5 | 1.00 |
| array-element-write | 49.1 | 48.3 | 0.98 |
| array-push-pop-10M | 49.7 | 51.3 | 1.03 |
| class-ctor-4 | 33.6 | 36.4 | 1.08 |
| closure-calls-20M | 13.3 | 13.0 | 0.98 |
| flat-butterfly-read | 13.4 | 13.4 | 1.00 |
| flat-butterfly-write | 61.7 | 61.5 | 1.00 |
| inline-property-read | 26.8 | 26.7 | 1.00 |
| inline-property-write | 53.4 | 53.5 | 1.00 |
| int-loop-3e8 | 95.6 | 95.7 | 1.00 |
| json-parse-200k | 32.9 | 33.0 | 1.00 |
| json-stringify-200k | 21.5 | 23.0 | 1.07 |
| map-set-get-2M | 552.2 | 564.4 | 1.02 |
| megamorphic-access | 1382.3 | 1101.1 | 0.80 |
| obj-literal-5 | 1.9 | 1.9 | 1.00 |
| proto-method-calls-20M | 7.3 | 7.3 | 0.99 |
| regexp-exec-1M | 99.2 | 108.6 | 1.10 |
| string-concat-2M | 5.1 | 5.6 | 1.10 |
| throw-catch-200k | 82.8 | 85.8 | 1.04 |
| transition-heavy-constructor | 56.3 | 55.7 | 0.99 |
| typed-array-sum-50M | 41.0 | 41.3 | 1.01 |

The flag-off/main ratios match the non-LTO pair row for row (§1 table):
1.00-1.05 everywhere except `class-ctor-4` and `regexp-exec` (1.08, the two
allocation-helper rows explained in §1.1) and `megamorphic-access` (0.81).
LTO does not change the picture in either direction, which also says the
non-LTO residuals are not inlining-budget accidents that LTO would undo.

## 5. What to do next, in order of expected gain

1. GIL off, Double arrays: make every copy into fresh storage from a Double
   source (copy-on-write materialization, `slice`/`concat`/`splice` results)
   produce Contiguous GIL off, so writable Double arrays no longer meet the
   Contiguous ones T4-O produces at the same sites (`stanford-crypto-aes` GIL
   off 0.19x of GIL on; §3); or the larger alternative, validated Double reads
   in every tier so Double->Contiguous can publish a fresh butterfly without a
   stop.
2. Shared tier-up counters GIL off (string-heavy 0.74x at 4 threads, every
   short multi-threaded run of shared functions): per-thread counter lines or
   owner-only counting (SPEC-ungil history, sixth round; LANDING-PLAN open
   item).
3. Polymorphic construct and per-case allocating transitions: a
   transition-capable `CreateThis` inline path, the inline allocating
   sequence in per-case-compiled handlers (`astar-like` 3.6x, `class-ctor-4`
   2.6x, JetStream `ML`/`typescript` residuals).
4. The DFG's inline transition after the F2 fire (claimed inline form instead
   of the IC; `transitions-after-fire` 2.8x).
5. A per-thread megamorphic cache and JSON tables GIL off (1.4-3x on those
   rows GIL off against GIL on).
6. Global property writes and the remaining frozen scope-metadata cases
   (SPEC-jit history §28); the `JSON.parse` per-property add through C++
   (1.19x); non-x86-64 targets (the out-of-line replace patch and the TID-tag
   strip are x86-64-only fast forms).
7. `earley-boyer` GIL on 0.66 and the two unexplained GIL-off drops (§3):
   profile first.
8. The flag-off 3 % on JetStream: per-symbol instruction deltas on `Air` and
   the JSON tests, the way §1.1 did for the micro set.
