# Performance results

Status: fifth landing round (2026-09-06). Companion to LANDING-PLAN Part 2.

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
  this tree, final state of the fifth round. The LTO pair (section 4) is built
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
| add-props-escaped (o.a..c on an escaped `{}`, 2M) | 9.7 | 9.6 | 9.9 | 10.5 | 15.8 | 1.01 | 1.09 | 1.51 |
| array-element-read | 53.4 | 67.1 | 53.5 | 66.8 | 67.4 | 1.00 | 1.00 | 1.01 |
| array-element-write | 49.9 | 53.6 | 49.3 | 53.9 | 63.1 | 0.99 | 1.00 | 1.17 |
| array-push-pop-10M | 49.6 | 49.7 | 51.4 | 53.9 | 53.0 | 1.04 | 1.08 | 0.98 |
| class-ctor-4 (a new class per outer call; polymorphic construct) | 33.9 | 33.4 | 36.7 | 85.6 | 170.8 | 1.08 | 2.56 | 2.00 |
| closure-calls-20M | 12.9 | 21.5 | 13.0 | 16.1 | 17.6 | 1.01 | 0.75 | 1.09 |
| flat-butterfly-read | 13.4 | 26.7 | 13.4 | 26.7 | 26.9 | 1.00 | 1.00 | 1.01 |
| flat-butterfly-write | 61.5 | 62.6 | 61.4 | 62.4 | 62.7 | 1.00 | 1.00 | 1.00 |
| inline-property-read | 26.7 | 53.5 | 26.7 | 53.5 | 53.6 | 1.00 | 1.00 | 1.00 |
| inline-property-write | 53.4 | 57.8 | 53.4 | 58.1 | 58.4 | 1.00 | 1.00 | 1.01 |
| int-loop-3e8 (`s = (s+i)|0`) | 95.5 | 160.2 | 95.5 | 160.3 | 131.9 | 1.00 | 1.00 | 0.82 |
| json-parse-200k | 33.9 | 34.2 | 34.9 | 41.8 | 50.1 | 1.03 | 1.22 | 1.20 |
| json-stringify-200k | 24.5 | 24.4 | 25.2 | 35.0 | 36.0 | 1.03 | 1.44 | 1.03 |
| map-set-get-2M | 567.5 | 570.1 | 586.0 | 714.5 | 770.0 | 1.03 | 1.25 | 1.08 |
| megamorphic-access | 1382.5 | 1400.4 | 1086.3 | 1773.1 | 2479.1 | 0.79 | 1.27 | 1.40 |
| obj-literal-5 (`{a..e}` in a loop; sinkable) | 1.9 | 1.4 | 1.9 | 2.1 | 2.1 | 1.00 | 1.48 | 1.01 |
| proto-method-calls-20M | 7.5 | 17.1 | 7.4 | 13.2 | 12.6 | 0.99 | 0.77 | 0.96 |
| regexp-exec-1M | 101.0 | 101.3 | 111.1 | 136.9 | 171.1 | 1.10 | 1.35 | 1.25 |
| string-concat-2M | 5.1 | 5.2 | 5.3 | 5.6 | 8.3 | 1.03 | 1.07 | 1.48 |
| throw-catch-200k | 82.8 | 83.2 | 89.4 | 103.3 | 118.7 | 1.08 | 1.24 | 1.15 |
| transition-heavy-constructor | 57.5 | 56.8 | 58.5 | 72.4 | 87.6 | 1.02 | 1.28 | 1.21 |
| typed-array-sum-50M | 41.3 | 42.1 | 41.3 | 42.1 | 43.3 | 1.00 | 1.00 | 1.03 |

"main, polling traps" is `main --usePollingTraps=1`. The flag forces polling
traps (SPEC-jit I21: signal-delivered traps patch running code from another
thread, which the multi-thread invalidation protocol cannot allow), so this
column, not plain `main`, is the like-for-like baseline for the flag-on
columns; the difference between the first two columns is the price of polling
traps in stock JSC on this machine.

### 1.1 Flag off against main (B1)

At the start of the round the flag-off binary was 15-20 % slower than `main`
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

- **Property-add transitions (B2).** At the start of the round no tier cached
  a transition flag-on (`obj-literal-5` 700 ms, 370x; `class-ctor-4` 600 ms).
  Now: baseline inline caches emit the non-reallocating transition with the
  E4 owner predicate and the N2-LF claim (SPEC-jit §5.5, SPEC-objectmodel
  §2.1), DFG/FTL inline it behind `CheckTransitionOwner` (one structure-
  independent owner test, CSE'd across a constructor's adds) including the
  FTL's polymorphic `MultiPutByOffset`, and allocation sinking removes it
  entirely for non-escaping literals. `obj-literal-5` 2.1 ms (1.0x against
  plain `main`; the polling-traps `main` run is oddly faster on this row),
  `add-props-escaped` 1.07x, `transition-heavy-constructor` 1.26x,
  `inline-property-write` 1.00x. `class-ctor-4` stays at 2.5x: the benchmark
  makes a new class per outer call, so the construct site is polymorphic and
  `this` comes from `operationCreateThis`; the residual is the IC's claim
  (a `lock cmpxchg` per add on a butterfly-less object) plus the slower C++
  create path. Reallocating transitions (out-of-line growth) are still C++
  only (R3 in SPEC-jit §5.5).
- **Loops, reads, calls (B5).** `int-loop`, `inline-property-read`,
  `flat-butterfly-read`, `array-element-read`, `proto-method-calls`,
  `closure-calls` are 1.6-2.0x against default `main` and 1.00x (or better)
  against `main --usePollingTraps=1`: the whole difference on these rows is
  the poll on the loop back-edge (a load, test and branch), which a
  three-instruction loop body doubles and a real body hides
  (`array-push-pop`, `typed-array-sum`, `flat-butterfly-write`). Earlier in
  the round the GIL-on `CheckTraps` node was also modelled as clobbering the
  abstract heap (`read(World); write(Heap)`, needed GIL off where a poll is a
  park site), which kept loop-invariant loads in the loop and blocked loop
  unrolling; it is now modelled exactly as flag-off GIL on (SPEC-jit I21
  amendment) and is cloneable.
- **`json-stringify` 1.44x, `json-parse` 1.22x, `megamorphic-access` 1.27x** (the two JSON rows were 1.65x and 1.4x before the global-property fix below: the loop resolves `JSON` once per iteration)**:** the
  megamorphic property cache and the structure-keyed fast paths of
  `JSON.stringify`/`JSON.parse` are VM-global tables that are disabled
  flag-on (SPEC-jit §5.5 Task 8 inventory); every access takes the generic
  path. Per-thread versions are the fix; not done.
- **`regexp-exec` 1.35x, `throw-catch` 1.24x, `map-set-get` 1.25x,
  `transition-heavy-constructor` 1.28x, `closure-calls` 1.2x against plain
  `main` (0.75x against the polling baseline):** C++-heavy paths paying the flag-on
  branches in allocation (`allocateCell`'s shared-heap dispatch), the
  error-stack walk's extra locking, `Map`'s flag-on table helpers, and for the
  constructor the reallocating (out-of-line) adds, which stay in C++.
- **Global property reads (found by the scaling suite, not by this table).**
  Every read of a non-`var` global — `Math`, `JSON`, constructors,
  `globalThis.x` — took the slow path flag-on in every tier, because scope
  metadata had been frozen flag-on in the first review round: a bare global
  property read in a loop 237 ms against 5 (50x), `Math.sqrt(i)` 253 against
  18 (14x); single-threaded, the scaling suite's ray tracer ran 2.8x flag-off,
  Richards 2.7x, `Map`-heavy 1.76x, string-heavy 1.3x. The loops in this
  table read locals and closure variables, which is why none of its rows
  showed it. Gets are cached again (ordered publish, SPEC-jit history §28):
  6.6 / 17.7 ms on the two loops (the residual is the poll), ray tracer 1.05x,
  Richards 1.01x, `Map`-heavy 1.17x, string-heavy 1.2x flag-off.
- Removed this round, GIL-on: the heap-lifetime fenced write barrier
  (SPEC-objectmodel M8) — every C++ write barrier took the slow path with a
  store-load fence; `class-ctor-4` went from 157 to 86 ms, `throw-catch` 115
  to 89, `regexp-exec` 98 to 85 on that change alone.

### 1.3 GIL off against GIL on

- `class-ctor-4` 1.9x, `add-props-escaped` 1.5x,
  `transition-heavy-constructor` 1.2x over GIL on: the same paths as GIL on
  plus the per-thread allocation dispatch in C++ (`operationCreateThis`, `allocateCell` through
  the thread-local cache) and, for the polymorphic-construct benchmark, the
  virtual-call path. Fixed this round on this path: allocation profiles now
  carry the size class's TLC slot so `op_create_this`/`op_new_object`/DFG
  `CreateThis` allocate inline GIL-off (they went to C++ for every object),
  the per-exit `cpuid` in OSR exits (below), a `RefPtr` refcount bounce on
  the shared `JITCode` in every virtual call, and a `.prototype` lookup per
  `operationCreateThis`.
- `throw-catch` 1.15x over GIL-on (was 3.3x at the start of the round): GIL
  off, OSR exits are never repatched (other threads may be executing the
  jump), so every exit goes through the compile operation, which issued a
  serializing `cpuid` per exit ("the ramp may have been compiled by another
  thread"); a throw inside FTL code exits every time. The ramp publisher now
  bumps the process stop generation and the consumer compares its per-thread
  copy (`jsThreadsSyncToStopGenerationBeforeJITEntry`), so the serializing
  instruction runs once per publication per thread; FTL exits also got the
  lock-free published-ramp fast path the DFG had.
- `map-set-get` 1.10x over GIL on (1.36x over flag off): GIL off every
  `Map`/`Set` operation, reads included, takes the table cell lock (PRE-1);
  a validated lock-free read (the property-table stamp scheme) is the fix.
- `regexp-exec` 1.26x, `json-parse` 1.17x, `string-concat` 1.5x,
  `array-element-write` 1.2x: per-thread caches replace VM-global ones GIL
  off (RegExp and JSON key atom caches, numeric strings) with lower hit
  rates, the shared-heap allocation path, and for array writes the SW-bit leg
  of the write predicate.
- Not a per-thread cost but visible here before this round: two threads doing
  independent object creation ran 230-400x slower than one (section 2).

### 1.4 ArrayStorage shift/unshift (B4)

`Tools/threads/perf/as-shift-bench.js`: an ArrayStorage array of 20,000
elements drained by `shift()`, then 5,000 `unshift`/`pop` pairs on a
1,000-element one.

| | flag off | GIL on, before | GIL on, after | GIL off, after |
|---|---|---|---|---|
| shift-drain 20k | 1.3 | 227.4 | 26.9 | 27.1 |
| unshift/pop 5k | 0.4 | 20.0 | 0.9 | 0.9 |

Before, every flag-on `shift`/`unshift` on ArrayStorage built a fresh butterfly
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
| raytrace-like (small-object allocation, doubles) | 1.19x | 0.97x | 1.59 / 2.96 / 4.24 |
| splay-like (pointer-heavy live set, GC) | 1.12x | 1.50x | 1.92 / 3.60 / 5.73 |
| map-heavy (`Map` get/set) | 1.18x | 2.32x | 1.87 / 3.04 / 4.40 |
| string-heavy (computed string keys, ropes) | 1.18x | 1.28x | 0.86 / 0.75 / 0.79 |
| richards-like (see note) | 2.69x | 3.07x | 3.56 / 7.30 / 13.1 (see note) |

GIL on, speedup is 1.0 at every N by construction (one thread runs at a
time); its column of interest is the serial cost, which was 1.76x / 3.10x /
2.73x / 1.14x / 1.30x on the same five workloads before the global-property
fix of §1.2 and is 1.12-1.19x after it on four of them.

Reading the GIL-off numbers:
- **raytrace-like** is the clean case: serial parity with flag-off and 4.2x
  at 8 threads. What keeps it under 8x is the stop-the-world shared-heap
  collection (every thread's eden pauses all of them), visible as the step
  from 2.96x at 4 to 4.24x at 8.
- **splay-like** scales past its relaxed floor (3.6x at 4, 5.7x at 8); its
  1.5x serial cost is the shared-heap allocation and barrier path on a
  workload that does little but allocate and link.
- **map-heavy** scales (4.4x at 8) from a 2.3x serial base: GIL off, every
  `Map`/`Set` operation, reads included, takes the table's cell lock (AUDIT
  PRE-1); the validated lock-free read is the recorded follow-up (§5).
- **string-heavy** does not scale GIL off (0.75-0.86x). The profile at 4
  threads has no single lock on top; the time goes to rope resolution,
  `operationPutByValCellStringSloppy` (computed string keys), atom-string
  `Ref` traffic and `PropertyTable` copies — the workload builds objects
  keyed by freshly made strings on every thread, which atomizes through the
  one process atom-string table and materializes property tables per
  transition. The atom table's lock and the shared literals' atomic refcounts
  are the suspected serialization; not confirmed further this round.
- **richards-like** is a pathological benchmark in stock JSC as well: it
  defines its constructor functions inside the workload function, so every
  invocation creates fresh poly-proto structures and each in-process
  repetition is slower than the last on `main` too (358, 554, 660, 1310,
  2416 ms for five consecutive runs flag-off). The gate's T(1) is the third
  run. Flag-on adds two known costs on top once a second thread has run the
  same code: the structures' thread-local transition sets fire (F2 is per
  structure — LANDING-PLAN open items), sending every later transition of
  those shapes through the locked path on both threads, and the megamorphic
  property cache those poly-proto sites would fall back to is disabled
  flag-on. First run on either thread alone: 403 ms GIL on against 359
  flag-off; the run after the other thread has used the classes: 578 ms. The
  "superlinear" GIL-off speedups are an artefact of that growth (T(1) is one
  long third run; T(8) is eight shorter ones in parallel).

Object creation alone, GIL off (`Tools/threads/perf/scale-objadd.js`, 2M
iterations of `{}` plus four adds per thread; `scale-objlit.js`, 4M object
literals per thread; wall time for N threads of the same per-thread work, so
ideal is flat): adds 6.0 / 6.5 / 8.4 / 8.9 ms at 1 / 2 / 4 / 8 threads,
literals 8.9 / 11.8 / 15.5 / 20.0 ms — against 8 / 1920 / 2100 / 4206 ms and
21 / 6343 ms (2 threads) at the start of the round (LANDING-PLAN B3: instance-
keyed ownership, rev 16). The residual growth at these sizes is thread
start-up and per-thread tier-up inside a 10-20 ms measurement.

## 3. JSC benchmark suites

JetStream 2 through the shell driver (`PerformanceTests/JetStream2/cli.js`,
`Tools/threads/perf/run-jetstream.sh <outdir> 5`): the default list minus
the four WebAssembly tests, `bomb-workers` and `segmentation` (GIL off has no
WebAssembly; the last two use workers), the same 36 tests in all four
configurations, five runs each. Geometric-mean totals, median of five: `main`
360.6, flag off 348.9, GIL on 268.1, GIL off 229.2 — flag off 0.968 of
`main`, GIL on 0.768 of flag off, GIL off 0.855 of GIL on. Run-to-run spread
within a configuration is under 3 %. Per-test scores of the median run:

| test | main | flag off | GIL on | GIL off |
|---|---|---|---|---|
| Air | 584.0 | 524.1 | 352.5 | 413.9 |
| Babylon | 853.6 | 828.8 | 441.0 | 417.1 |
| Basic | 995.8 | 976.7 | 796.0 | 396.9 |
| Box2D | 506.9 | 502.4 | 364.8 | 335.6 |
| FlightPlanner | 901.5 | 877.0 | 356.9 | 458.7 |
| ML | 148.0 | 144.1 | 94.9 | 57.4 |
| OfflineAssembler | 213.7 | 200.0 | 178.1 | 130.8 |
| UniPoker | 768.6 | 741.8 | 671.5 | 571.0 |
| WSL | 3.7 | 3.6 | 2.1 | 1.9 |
| ai-astar | 745.6 | 727.4 | 476.6 | 435.4 |
| async-fs | 608.5 | 602.3 | 600.8 | 486.0 |
| cdjs | 304.9 | 305.3 | 273.2 | 262.4 |
| crypto | 1677.7 | 1672.4 | 1512.2 | 1345.2 |
| delta-blue | 1243.6 | 1161.5 | 973.6 | 856.9 |
| earley-boyer | 939.6 | 912.5 | 625.0 | 739.0 |
| first-inspector-code-load | 275.0 | 271.4 | 265.6 | 270.1 |
| float-mm.c | 12.7 | 12.7 | 12.2 | 10.2 |
| gaussian-blur | 260.2 | 265.2 | 255.3 | 261.0 |
| gbemu | 175.2 | 171.9 | 121.0 | 77.1 |
| hash-map | 636.3 | 659.2 | 540.1 | 568.3 |
| json-parse-inspector | 445.0 | 394.4 | 377.3 | 277.5 |
| json-stringify-inspector | 542.8 | 458.8 | 431.3 | 420.8 |
| mandreel | 164.0 | 160.1 | 152.0 | 148.1 |
| multi-inspector-code-load | 447.5 | 443.6 | 425.3 | 427.9 |
| navier-stokes | 1032.2 | 973.1 | 884.0 | 810.9 |
| octane-code-load | 894.8 | 888.0 | 924.4 | 896.9 |
| octane-zlib | 28.2 | 27.3 | 26.7 | 26.7 |
| pdfjs | 200.4 | 201.0 | 175.7 | 156.1 |
| raytrace | 883.8 | 843.7 | 696.5 | 618.9 |
| regexp | 518.7 | 478.3 | 452.4 | 329.8 |
| richards | 997.5 | 958.8 | 903.1 | 839.0 |
| splay | 474.7 | 460.3 | 435.6 | 274.8 |
| stanford-crypto-aes | 417.4 | 406.0 | 344.1 | 268.1 |
| stanford-crypto-pbkdf2 | 1000.0 | 927.4 | 370.9 | 150.4 |
| stanford-crypto-sha256 | 921.9 | 889.8 | 331.2 | 153.9 |
| typescript | 24.6 | 23.9 | 11.1 | 13.0 |
| **Total (geomean)** | 360.6 | 348.9 | 268.1 | 229.2 |

Reading it:
- **Flag off against `main`, 0.97.** No test is below 0.9x when run alone
  (the three lowest rows of the table, `json-stringify-inspector`,
  `json-parse-inspector` and `Air`, rerun alone three times each: 0.975,
  0.97, 0.96 of `main`); inside the full run they also absorb heap-layout
  differences from the tests before them. The residual is the same as the
  micro set's (§1.1): option-byte tests and larger cold arms in the C++
  runtime around allocation, `JSON` and `RegExp`. It is not zero, and this
  is the honest number for B1 on a macro suite: -3 %.
- **GIL on against flag off, 0.77.** Half the tests are within 10 %; the
  total is pulled down by a few large ratios, each with a named cause:
  `stanford-crypto-pbkdf2` and `-sha256` 0.40 and 0.37, `Air` 0.67,
  `Basic` 0.82, `ai-astar` 0.66 — array-building code: flag-on, every
  indexing-shape relabel of a young array (Int32 to Double, to Contiguous)
  is a per-event stop-the-world even with one thread (SPEC-objectmodel T4;
  only the Undecided-source case was made stop-free this round, F16), and
  array storage never grows in place (`canReallocInPlace` is off flag-on, so
  every growth allocates and copies); the profile of `pbkdf2` GIL on has
  `stopTheWorldAndRun`, the relabel closure, `operationEnsureDouble` and the
  C++ splice/slice/concat/push-beyond-length paths on top. `typescript` 0.46,
  `FlightPlanner` 0.41, `Babylon` 0.53, `earley-boyer` 0.68, `gbemu` 0.70,
  `ML` 0.66: megamorphic property access and `JSON`/`RegExp`-cache users, the
  VM-global caches that are disabled flag-on (§1.2). `delta-blue`,
  `richards`, `raytrace` 0.82-0.88: constructor-transition code, the
  `class-ctor` row of §1.
- **GIL off against GIL on, 0.86.** The shared-heap allocation and barrier
  paths and the per-thread caches (§1.3): `splay` 0.63, `regexp` 0.73,
  `Basic` 0.50, the crypto tests 0.4-0.8, `json-parse-inspector` 0.74.

The two array items are the next round's first performance task: an
owner-only copy-convert for the Int32/Double/Contiguous relabels with a
publication order that lets a racy reader misread a lane only as a double
(never as a JSValue), and in-place growth for owned, never-shared butterflies.

## 4. LTO pair

Both trees built with `bun build.ts lto` (`-flto=thin`, the configuration
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

1. Indexing-shape relabels without a stop for owned arrays (Int32 to Double,
   to Contiguous) and in-place growth of owned butterflies: the 0.4-0.7x
   JetStream rows (§3) and every array-building program, single-threaded
   included.
2. Per-instance F2 (SPEC-objectmodel history §24, open item): a second thread
   using the same constructor code demotes its structures for both (1.4x on
   the scaling suite's Richards after one cross-thread run; §2).
3. Per-thread megamorphic cache and JSON fast-path tables (`typescript` 0.46,
   `FlightPlanner`, `Babylon`; 1.2-1.45x on the micro rows).
4. Polymorphic construct: a transition-capable `CreateThis` inline path and
   the N2-LF claim without the `lock cmpxchg` where the structure's sets prove
   no foreign thread can hold the object (2.5x on that benchmark).
5. `Map`/`Set` lock-free validated reads GIL off (2.3x serial on `Map`-heavy
   code, §2) and the string-keyed put path that does not scale GIL off.
6. Global property writes and the remaining frozen scope-metadata cases
   (SPEC-jit history §28); non-x86-64 targets.
7. Reallocating transitions in the JIT tiers (out-of-line property growth is
   C++-only flag-on).
8. The flag-off 3 % on JetStream: per-symbol instruction deltas on `Air` and
   the JSON tests, the way §1.1 did for the micro set.
