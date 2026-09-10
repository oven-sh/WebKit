# Performance results

Status: eighth landing round (2026-09-09). Companion to LANDING-PLAN Part 2.
Sections 1-3 and 5 were re-measured on the eighth round's final tree (the
tree is now based on a newer `main`, so `main` in the tables is the new merge
base and the seventh round's rows are not directly comparable in absolute
terms); §6 is the seventh round's GIL-off cost ledger with §6.8 (seventh) and
§6.9 (eighth) saying what the work confirmed, refuted and measured since;
§1.1's analysis and §4 (the LTO pair) are the fifth round's and say so.

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
  `main` = the merge base (oven-sh WebKit `main` at `2e2aa2290fac`, the commit
  Bun pins, onto which the branch was rebased in the eighth round); branch =
  this tree, final state of the eighth round. The LTO pair (section 4) is built
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
| add-props-escaped (o.a..c on an escaped `{}`, 2M) | 9.8 | 9.8 | 10.0 | 10.7 | 11.8 | 1.02 | 1.09 | 1.10 |
| array-element-read | 54.1 | 68.2 | 54.2 | 67.8 | 94.8 | 1.00 | 0.99 | 1.40 |
| array-element-write | 49.6 | 54.3 | 50.8 | 54.3 | 54.8 | 1.02 | 1.00 | 1.01 |
| array-int32-to-double-relabel-200k (new: `[1,2,3,4]`, one double store, pushes) | 6.3 | 6.4 | 6.9 | 18.7 | 22.6 | 1.09 | 2.92 | 1.21 |
| array-push-pop-10M | 50.3 | 50.2 | 51.9 | 54.6 | 51.5 | 1.03 | 1.09 | 0.94 |
| astar-like-nodes (new: 10k nodes built by a closure-local constructor, six adds each by a helper, per run) | 10.7 | 10.6 | 11.2 | 40.6 | 47.3 | 1.05 | 3.83 | 1.17 |
| class-ctor-4 (a new class per outer call; polymorphic construct) | 34.3 | 34.5 | 37.5 | 87.3 | 130.6 | 1.09 | 2.53 | 1.50 |
| closure-calls-20M | 13.2 | 21.7 | 13.1 | 16.3 | 17.9 | 1.00 | 0.75 | 1.10 |
| flat-butterfly-read | 13.6 | 27.1 | 13.6 | 27.0 | 27.0 | 1.00 | 1.00 | 1.00 |
| flat-butterfly-write | 62.2 | 63.3 | 62.1 | 63.2 | 63.6 | 1.00 | 1.00 | 1.01 |
| inline-property-read | 27.1 | 54.1 | 27.1 | 54.2 | 54.1 | 1.00 | 1.00 | 1.00 |
| inline-property-write | 54.2 | 81.0 | 54.2 | 59.5 | 58.9 | 1.00 | 0.73 | 0.99 |
| int-loop-3e8 (`s = (s+i) | 96.7 | 161.8 | 96.6 | 162.0 | 160.3 | 1.00 | 1.00 | 0.99 |
| json-parse-200k | 34.0 | 33.8 | 34.3 | 40.5 | 48.5 | 1.01 | 1.20 | 1.20 |
| json-stringify-200k | 26.2 | 26.3 | 26.3 | 27.7 | 36.2 | 1.00 | 1.05 | 1.30 |
| map-set-get-2M | 571.9 | 584.0 | 581.8 | 715.4 | 785.1 | 1.02 | 1.22 | 1.10 |
| megamorphic-access | 1392.2 | 1414.2 | 1155.5 | 1352.5 | 1534.6 | 0.83 | 0.96 | 1.13 |
| megamorphic-put-transition-1M (new: one store site adding a property to objects of 40 shapes) | 25.7 | 27.0 | 27.9 | 48.3 | 62.5 | 1.08 | 1.79 | 1.30 |
| obj-literal-5 (`{a..e}` in a loop; sinkable) | 1.9 | 2.1 | 1.9 | 2.1 | 2.1 | 1.00 | 1.00 | 1.00 |
| out-of-line-replace-poly-3M (new: `o.p = v` on two shapes, `p` out of line) | 11.9 | 10.7 | 10.8 | 13.4 | 14.4 | 0.91 | 1.25 | 1.08 |
| proto-method-calls-20M | 7.4 | 17.1 | 7.6 | 13.4 | 13.1 | 1.03 | 0.78 | 0.97 |
| regexp-exec-1M | 104.9 | 106.0 | 115.1 | 140.2 | 164.8 | 1.10 | 1.32 | 1.18 |
| string-concat-2M | 5.2 | 5.5 | 5.3 | 5.5 | 6.0 | 1.02 | 1.01 | 1.09 |
| throw-catch-200k | 87.5 | 87.9 | 90.3 | 99.4 | 108.4 | 1.03 | 1.13 | 1.09 |
| transition-heavy-constructor | 57.0 | 57.9 | 58.2 | 74.5 | 74.3 | 1.02 | 1.29 | 1.00 |
| transitions-after-fire-2M (new, flag-on only: `{}` + four adds on the main thread after a second thread built the same shapes) | - | - | - | 88.6 | 89.9 | - | - | 1.01 |
| typed-array-sum-50M | 41.8 | 42.5 | 41.7 | 42.7 | 42.8 | 1.00 | 1.00 | 1.00 |

"main, polling traps" is `main --usePollingTraps=1`. The flag forces polling
traps (SPEC-jit I21: signal-delivered traps patch running code from another
thread, which the multi-thread invalidation protocol cannot allow), so this
column, not plain `main`, is the like-for-like baseline for the flag-on
columns; the difference between the first two columns is the price of polling
traps in stock JSC on this machine.

### 1.1 Flag off against main (B1)

(Fifth round's analysis; the sixth, seventh and eighth rounds changed no
flag-off path and the column above moved within noise: 0.91-1.10 on the
eighth round's tree, the 1.08-1.09 rows being the allocation-helper rows
explained below plus the array relabel row whose flag-off body carries the
flag-on fork out of line, and `regexp-exec` at 1.10 - 104.9 -> 115.1 ms,
where the seventh round's tree measured 110 against the old base's 106; the
rebased tree before the round's changes was not measured quiet on this row,
so base versus round is not separated. Eighth round, GIL off against GIL on
in the last column: `array-element-read` 1.63 -> 1.40 and
`array-element-write` 1.17 -> 1.01 (P3/P4), `megamorphic-access` 1.77 ->
1.13 and `megamorphic-put-transition` 2.72 -> 1.30 (P1, the per-thread
cache), `string-concat` 1.39 -> 1.09, `throw-catch` 1.14 -> 1.09; nothing in
the GIL-on column moved.)
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

The seventh round changed no row here beyond noise (every GIL-on ratio is
within 0.05 of the sixth round's); its GIL-on changes - watcher-less Class-A
fires, profile write-avoidance - act on stop counts and on N-thread profiling,
which no serial micro row exercises. What the sixth round changed, row by row
(fifth-round value -> sixth, GIL on against `main --usePollingTraps=1`):

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

Seventh round, row by row (sixth-round ratio -> now): `add-props-escaped`
1.51 -> 1.12, `transition-heavy-constructor` 1.21 -> 1.01, `class-ctor-4`
1.97 -> 1.48, `astar-like-nodes` 1.39 -> 1.23, `megamorphic-put-transition`
3.09 -> 2.72 - every one a loop of fresh objects taking transitions, which GIL
off used to pay a stop-the-world for on each first replacement of an
IC-armed property (the watcher-less sets, SPEC-jit history §36) and page
faults on freshly minted blocks each eden (SPEC-heap §10E); `regexp-exec`
1.28 -> 1.16 (the cell lock per match, SPEC-ungil history); `string-concat`
1.52 -> 1.39 (block retention; ropes still resolve through the locked slow
path). `map-set-get` did not move (1.10): the row is FTL `get` on Int32 keys
interleaved with `set`, and the `set` side still takes the cell lock and bumps
the version. The sixth round's notes, still current:

- `map-set-get` 1.10x over GIL on: reads (`get`/`has`/`size`) are now
  lock-free and validated against the owner's version word (SPEC-ungil §N.1;
  four reader threads against one `Map`: 2,022 -> 318 ms), but a
  single-threaded loop that alternates `set` and `get` still pays the cell
  lock on every `set` and the version bump; the serial ratio did not move.
- `megamorphic-access` 1.77x, `megamorphic-put-transition` 2.7x,
  `json-stringify` 1.35x over GIL on: the VM-global caches that GIL on now uses
  stay off GIL off (above).
- `array-element-read` 1.63x over GIL on (1.01 in the fifth round): not a slower read — the
  benchmark's array is built by `push` from empty, and GIL off an Int32 array
  asked to become Double becomes Contiguous instead (T4-O: raw-double lanes
  may not appear under a stale Int32-keyed reader), so the summing loop reads
  boxed values with a type check where GIL on reads unboxed doubles/ints from
  a typed shape. This is the GIL-off cost of the stop-free relabel policy; its
  bad case on real code is recorded in §3 (`stanford-crypto-aes`).
- `class-ctor-4` 1.5x, `string-concat` 1.4x, `throw-catch` 1.14x: what is
  left after the seventh round's items above (shared-heap allocation
  dispatch, per-thread caches, the SW-bit leg of the write predicate, rope
  resolution under the cell lock).
- Found and fixed in the sixth round, GIL off, not visible in this table because every
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

(Seventh-round re-measurement: 27.4 / 1.0 ms flag-on in both modes, 1.4 / 0.4 flag-off and `main`; unchanged since the fifth.) Before the fifth round, every flag-on `shift`/`unshift` on ArrayStorage built a fresh butterfly
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
Medians of 5, eighth round, final tree.

| workload | serial, GIL on | serial, GIL off | speedup GIL off at 2 / 4 / 8 threads |
|---|---|---|---|
| raytrace-like (small-object allocation, doubles) | 1.21x | 0.93x | 1.67 / 3.27 / 5.71 |
| splay-like (pointer-heavy live set, GC) | 1.13x | 1.31x | 1.75 / 3.11 / 5.23 |
| map-heavy (`Map` get/set) | 1.24x | 2.31x | 1.88 / 3.34 / 5.13 |
| string-heavy (computed string keys, ropes) | 1.22x | 1.10x | 1.09 / 2.24 / 2.66 (bimodal, see note) |
| richards-like (see note) | 2.73x | 1.88x | (see note) |

(Seventh round's row in the same order: raytrace-like 1.20x / 0.93x / 1.61 /
2.94 / 4.93, splay-like 1.08x / 1.29x / 1.72 / 2.93 / 4.45, map-heavy 1.22x /
2.35x / 1.81 / 3.03 / 4.50, string-heavy 1.26x / 1.33x / 0.85 / 0.82 / 0.85.
An earlier five-run session on the same day and tree - minus the dictionary
flatten, which these workloads do not reach - gave raytrace-like 3.37 / 5.88,
splay-like 3.14 / 5.21, map-heavy 3.23 / 5.17 and string-heavy 0.77 / 1.51 /
3.69, which is the measure of the session-to-session spread and, for
string-heavy, of its two modes.) What moved: raytrace-like 2.94 -> 3.3 at
four and 4.93 -> 5.7-5.9 at eight, map-heavy 3.03 -> 3.2-3.3 and 4.50 ->
5.1-5.2, splay-like 2.93 -> 3.1 and 4.45 -> 5.2 - the eden allowance growing
with the allocating threads (SPEC-heap §10F: its corpus test counts 13 collections for a
four-thread allocation run that took 51) and the cycle-end retention keyed to
the cycle (§10E amendments); string-heavy 0.82 -> 2.2 at four in its fast
mode (P8, loop entry into the superseded DFG code) with a slow mode that the
median lands in about one session in two (§6.9 has the mechanism: an
`Overflow` exit in the FTL loop-entry code that the reoptimization back-off,
inflated by per-thread counted jettisons, never lets recompile). The round's
targets - 3.2x at four threads on the clean workloads, 2x on string-heavy -
are met by raytrace-like and map-heavy in both sessions, by splay-like in
neither (3.11, 3.14; its floor is the relaxed GC one), and by string-heavy in
one of two.

GIL on, speedup is 1.0 at every N by construction (one thread runs at a
time); its column of interest is the serial cost.

Reading the GIL-off numbers:
- **raytrace-like** is the clean case: serial parity with flag-off (0.93x:
  the GIL-off run is a little faster single-threaded than flag off on this
  workload, as in the seventh round) and 5.7x at 8 threads. What keeps it
  under 8x is still the stop-the-world shared-heap collection - fewer of them
  now, each still stopping every thread.
- **splay-like** 3.1x at 4, 5.2x at 8 from a 1.3x serial base (the shared
  heap's allocation and barrier path on a workload that does little but
  allocate and link).
- **map-heavy** 3.3x / 5.1x from a 2.3x serial base (the `Map` paths GIL off,
  §6).
- **string-heavy**: see above and §6.9; T(1) GIL off is now 1.10x of flag off
  (seventh 1.33x: the rope and megamorphic paths of P1/P7).
- **richards-like** is a pathological benchmark in stock JSC as well: it
  defines its constructor functions inside the workload function, so every
  invocation creates fresh poly-proto structures and each in-process
  repetition is slower than the last on `main` too (358, 554, 660, 1310,
  2416 ms for five consecutive runs flag-off). The gate's T(1) is the third
  run, which is why its "speedups" are not meaningful. Its GIL-off serial
  cost fell from 4.42x to 1.88x this round (P1: its megamorphic sites now have
  a cache GIL off).

Object creation alone, GIL off (`Tools/threads/perf/scale-objadd.js`, 2M
iterations of `{}` plus four adds per thread; `scale-objlit.js`, 4M object
literals per thread; wall time for N threads of the same per-thread work, so
ideal is flat): not re-measured this round; seventh round adds 6.0 / 6.6 /
8.2 / 9.6 ms at 1 / 2 / 4 / 8 threads, literals 8.8 / 11.3 / 15.7 / 18.5 ms
(medians of three) - against 8 / 1920 / 2100 / 4206 ms and 21 / 6343 ms (2
threads) before the fifth round's instance-keyed ownership (LANDING-PLAN B3,
rev 16).

## 3. JSC benchmark suites

JetStream 2 through the shell driver (`PerformanceTests/JetStream2/cli.js`,
`Tools/threads/perf/run-jetstream.sh <outdir> 5`): the default list minus
the four WebAssembly tests, `bomb-workers` and `segmentation` (GIL off has no
WebAssembly; the last two use workers), the same 36 tests in all four
configurations. Eighth round, final tree: five runs per configuration,
interleaved by configuration, nothing else running; the rebased tree as it
stood before the round's changes was run in the same session (five runs,
three configurations) so that the round's own effect is separated from the
rebase's. Geometric-mean totals, median (run range): `main` 355.8 (348-360),
flag off 340.9 (340-344), GIL on 306.0 (301-309), GIL off 262.2 (257-267);
rebased tree before the round: flag off 341.7 (339-345), GIL on 305.7
(299-307), GIL off 249.8 (249-252). Ratios: flag off 0.958 of `main` (a
second five-run session an hour earlier on the same binary minus one
Debug-only change gave 0.969, `main` 348.5 and flag off 337.6; over the ten
runs 0.971; seventh round 0.975), GIL on 0.898 of flag off (0.905 in the
earlier session; seventh 0.865 - but the rebased tree before the round
already measured 0.895, so this is the new base and the rebase, not the
round's work), GIL off 0.857 of GIL on (0.849 in the earlier session, which
had the dictionary flatten switched off; seventh 0.830; against the rebased
tree before the round, GIL off is +5.0 % absolute, 249.8 -> 262.2, with GIL
on unchanged). Per-test medians; the last two columns are the rebased tree
before the round's changes:


| test | `main` | flag off | GIL on | GIL off | GIL off / GIL on | rebased tree before the round: GIL on | GIL off |
|---|---|---|---|---|---|---|---|
| Basic | 982.0 | 913.1 | 892.6 | 511.7 | 0.57 | 889.7 | 526.5 |
| ML | 147.4 | 142.6 | 109.9 | 71.8 | 0.65 | 109.4 | 51.5 |
| splay | 489.0 | 450.5 | 418.3 | 276.2 | 0.66 | 422.9 | 269.8 |
| gbemu | 173.9 | 170.4 | 148.9 | 102.7 | 0.69 | 151.4 | 102.5 |
| stanford-crypto-sha256 | 914.6 | 835.2 | 705.2 | 508.4 | 0.72 | 711.1 | 460.7 |
| regexp | 500.5 | 464.7 | 446.9 | 339.6 | 0.76 | 429.5 | 316.8 |
| OfflineAssembler | 198.8 | 199.7 | 177.9 | 138.5 | 0.78 | 176.1 | 142.8 |
| stanford-crypto-aes | 416.1 | 400.2 | 333.7 | 265.9 | 0.80 | 334.1 | 209.6 |
| FlightPlanner | 950.1 | 772.3 | 643.9 | 518.8 | 0.81 | 633.8 | 469.9 |
| async-fs | 616.9 | 591.8 | 600.8 | 491.9 | 0.82 | 604.1 | 482.2 |
| json-stringify-inspector | 524.0 | 517.3 | 510.1 | 420.1 | 0.82 | 507.9 | 408.8 |
| float-mm.c | 12.8 | 12.6 | 12.1 | 10.1 | 0.84 | 12.1 | 10.1 |
| WSL | 3.6 | 3.6 | 2.8 | 2.4 | 0.84 | 2.7 | 2.2 |
| ai-astar | 729.0 | 727.0 | 664.8 | 560.1 | 0.84 | 666.1 | 535.3 |
| UniPoker | 744.8 | 725.1 | 650.2 | 548.4 | 0.84 | 659.2 | 560.9 |
| delta-blue | 1245.3 | 1132.9 | 1048.8 | 885.4 | 0.84 | 1051.2 | 859.8 |
| navier-stokes | 1013.7 | 968.6 | 864.3 | 744.2 | 0.86 | 864.9 | 736.4 |
| pdfjs | 198.0 | 196.4 | 179.8 | 155.1 | 0.86 | 178.7 | 157.6 |
| raytrace | 909.3 | 842.1 | 767.3 | 670.9 | 0.87 | 749.7 | 673.4 |
| Babylon | 836.4 | 808.9 | 669.0 | 590.0 | 0.88 | 658.2 | 515.0 |
| crypto | 1675.0 | 1669.8 | 1527.2 | 1357.7 | 0.89 | 1529.4 | 1177.1 |
| Air | 568.7 | 539.1 | 497.2 | 442.8 | 0.89 | 493.1 | 425.5 |
| hash-map | 611.9 | 613.4 | 493.6 | 441.2 | 0.89 | 531.2 | 481.0 |
| typescript | 22.4 | 22.2 | 18.4 | 16.6 | 0.90 | 18.4 | 14.7 |
| earley-boyer | 941.9 | 905.7 | 798.3 | 753.9 | 0.94 | 579.1 | 694.7 |
| json-parse-inspector | 430.3 | 429.6 | 371.2 | 354.4 | 0.95 | 377.8 | 345.9 |
| richards | 956.3 | 955.4 | 868.2 | 833.5 | 0.96 | 855.5 | 826.0 |
| stanford-crypto-pbkdf2 | 975.6 | 909.5 | 778.5 | 750.0 | 0.96 | 795.1 | 658.1 |
| Box2D | 510.2 | 500.3 | 432.7 | 417.7 | 0.97 | 431.4 | 403.7 |
| multi-inspector-code-load | 436.5 | 445.8 | 428.6 | 414.3 | 0.97 | 419.1 | 411.1 |
| octane-code-load | 911.8 | 858.1 | 921.0 | 895.3 | 0.97 | 900.1 | 870.4 |
| cdjs | 307.7 | 300.1 | 265.3 | 258.5 | 0.97 | 266.9 | 261.1 |
| first-inspector-code-load | 272.7 | 268.3 | 259.6 | 254.1 | 0.98 | 253.8 | 265.7 |
| octane-zlib | 27.6 | 26.9 | 26.2 | 25.8 | 0.98 | 26.2 | 26.3 |
| gaussian-blur | 265.1 | 260.2 | 258.9 | 254.3 | 0.98 | 260.0 | 259.0 |
| mandreel | 160.3 | 158.1 | 146.8 | 145.0 | 0.99 | 148.6 | 143.9 |
| **Total (geomean)** | 355.8 | 340.9 | 306.0 | 262.2 | 0.857 | 305.7 | 249.8 |

Reading it:
- **Flag off against `main`, 0.96-0.97.** The rebased tree before the round
  and the final tree measure the same flag off (341.7 and 340.9 in this
  session, 339.0 and 337.6 in the earlier one), so the round's changes cost
  nothing here; the distance to `main` is the standing flag-off cost (§1.1:
  option-byte tests and larger cold arms around allocation, `JSON`,
  `RegExp`) on a new base. The tests furthest down are `FlightPlanner` 0.81
  (772 against 950; 813 against 888 in the earlier session - `main`'s own
  score moved 7 % between sessions), `Basic` 0.93, `delta-blue` 0.91,
  `regexp` 0.93, `stanford-crypto-sha256`/`pbkdf2` 0.91-0.93; the micro set's
  `regexp-exec` flag off at 1.10 of `main` (§1) is the one direct signal, not
  analysed this round.
- **GIL on against flag off, 0.90.** Unchanged by the round (305.7 before,
  306.0 after; no test moved outside its run-to-run spread except
  `earley-boyer`, whose GIL-on score is bimodal - 579 before, 798 after, 615
  in one of the A/B runs). The A/B the seventh round asked for, GIL on with
  the Baseline profile write-avoidance switched off
  (`--useSharedProfileWriteAvoidance=0`, three runs in each session): totals
  306.9 and 306.1 against 309.3 and 308.0 with it on - within 1 %, on the side
  of keeping it; per test nothing outside the bimodal set moved. Lowest, as
  before: `WSL`/`typescript` 0.78-0.83, `FlightPlanner` 0.83, `ML` 0.77,
  `Babylon` 0.83, `sha256`/`pbkdf2` 0.84-0.86, `hash-map` 0.80.
- **GIL off against GIL on, 0.86 (seventh 0.83); against the tree before the
  round, +5 %.** Up over the round (last column against GIL off):
  `stanford-crypto-aes` 210 -> 266 and `crypto` 1177 -> 1358 (P4, the FTL
  keeps the butterfly across polls), `ML` 51.5 -> 71.8 (P2's flatten: `ML`'s
  matrix classes sit behind dictionary prototypes; with the flatten switched
  off it measures 48), `Babylon` 515 -> 590, `pbkdf2` 658 -> 750, `sha256`
  461 -> 508, `FlightPlanner` 470 -> 519, `earley-boyer` 695 -> 754,
  `typescript` 14.7 -> 16.6, `regexp` 317 -> 340, `Air` 426 -> 443. Down:
  `hash-map` 481 -> 441 (allocation-heavy with Full collections; the §10E
  retention now keeps more blocks through its Fulls and the eden allowance is
  larger - not separated), `Basic` 527 -> 512 (within its spread; the round
  removed its 835,000 relabel stops, P3(b), which had made it trimodal after
  P3(a), and its generator-frame host calls, P7, without moving the median:
  what bounds it is in §6.9). Still lowest: `Basic` 0.57, `ML` 0.65, `splay`
  0.66, `gbemu` 0.69, `sha256` 0.72, `regexp` 0.76 - §6.9 has the mechanism
  for each that is known (`gbemu`: tier-up dwell on a `to_this` exit; `splay`:
  the shared heap's pause count at its allocation rate; `Basic`: array growth
  by copy and the per-event stops that remain).
- **Targets set for the round**: GIL off >= 0.90 of GIL on with no test under
  0.75 - not met (0.86; five tests under 0.75); GIL on >= 0.89 of flag off -
  met at 0.90, by the new base rather than by the round; flag off >= 0.97 of
  `main` - at the line (0.96-0.97 by session, 0.971 over ten runs).

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

Rewritten after the eighth round (items 1, 2, 3's codegen half, parts of 5
and 6 of the previous list were worked; §6.9 says with what result and has
the evidence behind each item below).

1. GIL off, doubles: the storage-then-shape re-check protocol
   (SPEC-objectmodel history §29) - Int32->Double as a real transition,
   Double->Contiguous as a copy, T4-C and the allocation-profile promotion
   retired. `ML` 0.65, `navier-stokes`, `float-mm`, `raytrace`,
   `array-element-read`. Its own round: writer side small, reader side an
   audit of every lane decoder in four tiers and the runtime.
2. GIL off, tiering policy under N threads: the `gbemu` trace (§6.9) - an
   entry-exit taken on every call never trips the loop reoptimization
   trigger, so a bad third compile lives for 400 exits - and string-heavy's
   slow mode (§6.9) - an every-time exit under a back-off inflated by
   per-thread counted jettisons. One decision covers both: how an exit that
   fails on every pass should be weighed against the two multipliers
   (thread count, retry back-off) that exist for the opposite case; the same
   dwell shows in `Basic` and in every test's worst-case component.
3. GIL off, array growth without copying (§6.9): in-place slack raise on
   x86-64 (decision: an architecture-conditional object-model rule) and/or
   segmented growth for owned large arrays (size-gated). `gbemu`, `pdfjs`,
   the sjcl tests; 1-2 GB of copying a run each.
4. `splay`'s three extra Full collections a run GIL off (attribute the
   trigger; §10B), and the worst-case iterations they land in.
5. E1 (LANDING-PLAN Open items): call-link record pins keep linked callees'
   optimized code alive flag on; replace the publish-time pin with GC-end
   unlinking so idle aging works again. Not a throughput item; memory.
6. Rope resolution's cell lock GIL off (`OfflineAssembler` 1.9 M a run, the
   string tests): a CAS publish; `Map` `set`/iteration and generator frame
   materialization in the DFG (`Basic`).
7. GIL on: polymorphic construct and per-case allocating transitions
   (`astar-like` 3.4x, `class-ctor-4` 2.5x), global property writes,
   `JSON.parse` 1.2x, `earley-boyer`'s bimodality (the write-avoidance A/B of
   this round, §3, moved nothing outside it); non-x86-64 targets.
8. The broad object-heavy 10-20 % GIL off and the flag-off 3-4 % on
   JetStream: per-symbol instruction deltas on JIT code, test by test (§6.5's
   method), starting with `delta-blue` and `richards` where no counter moves.

## 6. GIL-off cost ledger (seventh round, taken before any change)

The sixth round left GIL off at 0.77 of GIL on on the JetStream geomean,
single-threaded, with `stanford-crypto-aes` at 0.19 and little above 0.85.
This section is the measurement pass that decides the seventh round's order
of work: where the GIL-off deficit is, per test and per mechanism, measured
on the sixth round's final binary before touching the engine. Everything
below is single-threaded unless it says otherwise; "on" and "off" are GIL on
and GIL off of the same binary.

### 6.1 Method

- Scores: the §3 table (quiet machine) gives the per-test off/on ratio; the
  ten worst plus five more were re-run per configuration for everything
  below (`cli.js` with a one-test `testList`).
- Tiers: the same 15 tests with `--useFTLJIT=0`, `--useDFGJIT=0` and
  `--useJIT=0` in both modes (3 runs each, medians), to separate what the
  optimizing tiers add to the deficit from what the runtime adds.
- Instructions: `perf stat -e instructions:u,cycles:u` with
  `--useConcurrentJIT=0`, both modes.
- Profiles: `perf record -F 4000` flat, both modes, symbols diffed by name
  and converted to CPU-milliseconds (samples / rate); JIT code is one bucket.
- Event counts: a diagnostic option added this round,
  `--reportJSThreadsCounters=1`, counts protocol events in C++ (stop-the-world
  requests by requester, parks, watchpoint fires, F1/F2 fires, structure
  transitions, relabels, collections and their stopped time, synchronous
  sweeps, marked blocks minted and freed, compilations per tier, OSR exits
  reaching the exit operations, jettisons, the IC slow-path operations by
  kind, virtual/polymorphic call slow paths, `operationCreateThis`,
  stop-generation syncs, stop-request updates, the GIL-off compilation lock,
  allocator slow paths, write-barrier slow paths, GIL-off rope resolutions,
  GIL-off `Map`/`Set` C++ reads and adds, RegExp compile checks under the
  cell lock, generator resume claims, exceptions) and prints them at exit.
  Each site is one predicted-untaken option test; nothing reads a counter.
- Knobs: six measurement-only options (`--jsThreadsExp*`, unsound by
  construction, never defaults) that switch one GIL-off mechanism to its
  GIL-on behaviour so its cost can be read as a score difference: the
  `CheckTraps` clobber set (three levels), the parkable-slow-path clobber,
  Int32->Double relabels in place, `Map`/`Set` intrinsic inlining, the
  VM-global megamorphic cache with IC-path use, the RegExp compile check.

### 6.2 Where the deficit is, by tier and by instruction count

Off/on score ratio with tiers removed (both modes lose the same tiers; a
ratio that rises toward 1.0 as tiers are removed says the deficit is in the
code the removed tier generates; one that stays low says it is in the
runtime), and GIL-off/GIL-on retired instructions and cycles for the full
configuration:

| test | full | no FTL | baseline only | LLInt only | instructions off/on | cycles off/on |
|---|---|---|---|---|---|---|
| stanford-crypto-aes | 0.22 | 0.26 | 0.89 | 0.90 | 1.77 | 4.58 |
| Basic | 0.51 | 0.53 | 0.80 | 0.90 | 2.63 | 2.34 |
| ML | 0.48 | 0.63 | 0.85 | 0.93 | 2.04 | 2.13 |
| splay | 0.63 | 0.67 | 0.79 | 0.79 | 1.10 | 1.04 |
| gbemu | 0.61 | 0.65 | 0.91 | 0.95 | 1.51 | 1.45 |
| stanford-crypto-pbkdf2 | 0.83 | 0.85 | 0.84 | 0.86 | 1.20 | 1.27 |
| stanford-crypto-sha256 | 0.69 | 0.74 | 0.72 | 0.87 | 1.27 | 1.28 |
| FlightPlanner | 0.71 | 0.68 | 0.76 | 0.89 | 1.18 | 1.23 |
| regexp | 0.91 | 0.85 | 0.76 | 0.95 | 1.12 | 1.13 |
| crypto | 0.77 | 1.01 | 1.01 | 0.98 | 1.57 | 1.50 |
| OfflineAssembler | 0.71 | 0.76 | 0.82 | 0.87 | 1.30 | 1.31 |
| WSL | 0.72 | 0.73 | 0.89 | 0.87 | 1.33 | 1.26 |
| Babylon | 0.80 | 0.84 | 0.89 | 0.88 | 1.19 | 1.16 |
| json-parse-inspector | 0.90 | 0.85 | 0.84 | 0.84 | 1.09 | 1.13 |
| typescript | 0.77 | 0.75 | 0.88 | 0.91 | 1.21 | 1.19 |

(The tier runs were made eight at a time, so their "full" column is noisier
than §3; the counts include compiler and collector threads.) Reading: GIL off
executes 1.1x-2.6x the instructions of GIL on for the same work, so the
deficit is mostly work, not stalls (`aes` is the exception: 1.8x
instructions, 4.6x cycles — it waits in stops). Only `crypto` is a pure
optimizing-tier case (1.01 without FTL); `ML`, `splay`, `gbemu`, `Babylon`
have an FTL/DFG component on top of a runtime one; the rest keep most of
their deficit down to baseline-only and much of it in LLInt-only, i.e. it is
in C++ paths the interpreter also reaches.

### 6.3 Event counts, GIL on against GIL off

Selected counters for one run of each test (full tiers). "Blocks minted" is
`MarkedBlock::tryCreate`; "shrinks / GCs" is `MarkedSpace::shrink` calls over
collections; "get/put GaveUp" is calls to the `operationGetById*GaveUp` /
`operationPutById*GaveUp` generic operations; STW lists stop-the-world
windows other than watchpoint fires and jettisons (which are 20-500 per test
in both modes and cost under 5 ms in every test).

| test | virtual-call slow path | get/put GaveUp (on / off) | `Map`/`Set` C++ read+add | RegExp check under lock | GIL-off rope resolve | blocks minted (on / off) | shrinks / GCs | other STW (count, total ms) | OSR exits via operation | generator claims |
|---|---|---|---|---|---|---|---|---|---|---|
| stanford-crypto-aes | 0 | 0 / 0 | - | 0 | - | 2,270 / 127,156 | 67 / 68 | 1,496,011 Double->Contiguous relabels (6,717) | 1,196 | 0 |
| Basic | 2,372,414 | 0 / 0 | 22,940,097 | 30,960 | 6,133 | 2,258 / 36,684 | 20 / 21 | - | 13,981 | 2,474,520 |
| ML | 0 | 0 / 24,416,220 | - | 804 | 764 | 2,366 / 121,507 | 73 / 74 | 6,732 Double->Contiguous (30) | 2,707 | 0 |
| splay | 0 | 0 / 0 | - | 0 | - | 16,272 / 69,597 | 11 / 22 | - | 157 | 0 |
| gbemu | 31,457,495 | 0 / 0 | - | 0 | - | 1,033 / 997 | 0 / 43 | - | 24,753 | 0 |
| stanford-crypto-pbkdf2 | 110 | 0 / 0 | - | 0 | 152 | 2,169 / 34,392 | 17 / 17 | - | 3,499 | 0 |
| stanford-crypto-sha256 | 0 | 0 / 0 | - | 0 | 152 | 2,163 / 38,104 | 18 / 18 | 2,051 Double->Contiguous (9) | 3,130 | 0 |
| FlightPlanner | 0 | 0 / 275,691 | - | 1,900,917 | 704,911 | 7,831 / 9,174 | 1 / 5 | - | 1,641 | 0 |
| regexp | 0 | 0 / 0 | - | 7,961,132 | 16,326 | 1,412 / 1,416 | 0 / 40 | - | 102 | 0 |
| crypto | 0 | 0 / 0 | - | 0 | 273 | 650 / 647 | 0 / 0 | - | 402 | 0 |
| OfflineAssembler | 7,778 | 3,502 / 3,475 | 1,898,463 | 26,761,215 | 1,907,958 | 5,918 / 20,026 | 7 / 55 | - | 2,484 | 0 |
| WSL | 14,571,684 | 8,505,858 / 20,428,021 | 8,360,460 | 146,524 | 348,164 | 7,468 / 29,439 | 14 / 108 | - | 127,734 | 912,866 |
| Babylon | 22,353 | 0 / 1,373,983 | - | 12,360 | 487,954 | 2,906 / 2,582 | 0 / 11 | - | 1,928 | 0 |
| json-parse-inspector | 0 | 0 / 0 | - | 0 | - | 6,787 / 9,354 | 1 / 3 | - | 0 | 0 |
| typescript | 6,663,750 | 36 / 14,385,827 | - | 2,820 | 1,440,850 | 16,216 / 62,355 | 5 / 13 | 45 createArrayStorage (6) | 17,918 | 0 |
| earley-boyer | 0 | 0 / 0 | - | 0 | - | 4,779 / 10,796 | 10 / 32 | - | 1,206 | 0 |
| raytrace | 8,301 | 0 / 986 | - | 0 | - | 2,161 / 65,545 | 33 / 34 | - | 302 | 0 |
| delta-blue | 3 | 0 / 0 | - | 0 | - | 2,368 / 22,262 | 11 / 12 | - | 1,493 | 0 |
| pdfjs | 14,331 | 306 / 357 | - | 23,400 | 2,488,502 | 6,603 / 20,148 | 3 / 67 | 240 createArrayStorage (4) | 5,660 | 0 |
| UniPoker | 0 | 0 / 0 | - | 3,137,100 | 641,636 | 2,511 / 18,769 | 10 / 10 | - | 149 | 0 |
| Air | 272,088 | 0 / 835,008 | 313,159 | 0 | 255 | 2,412 / 4,594 | 1 / 10 | - | 3,189 | 0 |
| hash-map | 0 | 0 / 0 | - | 0 | - | 5,966 / 20,801 | 5 / 28 | - | 108 | 0 |

In GIL on every one of the first six columns is zero or near it (the
virtual-call slow path 0-2,600; GaveUp only where a test really is
megamorphic and even then served by the megamorphic cache; `Map` reads
inlined; RegExp checks lock-free; ropes resolved on the flag-off path; blocks
retained). Collections, watchpoint fires, jettisons, structure transitions
and compilations per tier are the same in both modes to within a few
percent in every test; stopped GC time is the same (0-250 ms per test in
both); parks are zero single-threaded; the GIL-off compilation lock is taken
300-12,000 times per test and never contended; `jsThreadsSyncToStopGenerationBeforeJITEntry`
runs 10^2-10^5 times per test (10^6 in `aes`, where it follows the stops) and
`updateThreadStopRequestIfNeeded` 10^2-10^4 (6 M in `aes`, again the stops).

### 6.4 Mechanisms, with their evidence, ranked by share of the deficit

Each entry: what it is, why GIL off pays it and GIL on does not, the
evidence, and the estimated recoverable score. The estimate is the knob
where one exists (score with the mechanism switched to its GIL-on behaviour,
same binary, quiet machine) and otherwise the profile's CPU-ms for the
mechanism's symbols against the test's total.

1. **Marked-block churn: the shared heap frees its empty blocks at the end of
   almost every collection and mints them again next cycle.** `reclaimSharedGCMemoryAtCycleEnd`
   shrinks whenever committed capacity exceeds `m_maxHeapSize`, and after an
   eden cycle that allocated a full eden of fresh blocks capacity sits at
   that bound by construction, so the test fires nearly every cycle (shrinks
   / GCs above: 67/68, 73/74, 33/34, 20/21, ...) and `shrink()` frees every
   empty block, not the excess; the Full-cycle arm additionally runs a
   synchronous whole-heap sweep when capacity is 1.5x the live size (splay:
   11 sweeps, 130 ms). GIL on (non-shared heap) frees blocks only from the
   idle-time incremental sweeper and effectively keeps them. Cost per test:
   blocks minted 5x-56x GIL on's (raytrace 65,545 against 2,161 = 1 GB of
   16 KB blocks through `tryFastCompactAlignedMalloc`, first-touch page
   faults and the warm-up helper thread); in the profiles
   `bmalloc_medium_bitfit_..._try_allocate` + `WarmUpThread::work` +
   `decommitUnusedPages`/`freeBlock` + unattributed fault time is 200 ms of
   raytrace's 416 ms deficit, 230+180 ms in ML, 130+113+50 ms in splay, 79+51
   in `pbkdf2`, 78+45 in `sha256`, 63+40 in Basic, 41+33 in delta-blue. It is
   the one mechanism present in nearly every test (20 of the 24 above).
   Estimated +5-8 % on the geomean.
2. **Virtual calls never take the thunk's fast path.** GIL off, a script
   executable's arity-check entry mirror is kept null (a concurrent
   `installCode` could otherwise pair a stale entry with a new `CodeBlock`),
   so the virtual-call thunk sends every call to a JS function through
   `operationVirtualCall` (C++: `virtualForWithFunction`, `sanitizeStackForVM`,
   `addressForCall`, entry-token check). Counts above: gbemu 31.5 M, WSL
   14.6 M, typescript 6.7 M, Basic 2.4 M, Air 272 K. Profile: gbemu 550 ms of
   its 1,400 ms deficit (`operationVirtualCall` 304, `sanitizeStackForVM`
   138, `addressForCall` 75, `currentThreadHoldsEntryToken` 31), WSL 530 ms,
   typescript 200 ms, Basic 60 ms. Estimated +3-4 % geomean; gbemu 0.60 ->
   ~0.8.
3. **Property ICs on dictionaries and megamorphic sites fall to the generic
   C++ path.** Two rules combine: flag-on, no `AccessCase` is keyed on a
   dictionary structure (an owner adds to a dictionary under an unchanged
   StructureID, so a structure check proves nothing about the butterfly);
   and GIL off, an IC path never flattens a dictionary (flattening is a
   stop, and the IC path holds the CodeBlock lock) and the VM-global
   megamorphic cache is off. GIL on flattens at the IC and then caches, and
   serves the truly megamorphic sites (and dictionary self hits) from the
   megamorphic cache; GIL off every such access is `operation*GaveUp` ->
   `JSObject::get`/`putInlineSlow` -> `findConcurrently`. Counts: ML 24.4 M
   GaveUp (GIL on: 0), WSL 20.4 M (8.5 M), typescript 14.4 M (36), Babylon
   1.37 M (0), Air 835 K (0), FlightPlanner 276 K (0). Knob (megamorphic
   cache and IC megamorphic forms on, GIL off): Babylon 0.73 -> 0.87 of GIL
   on, typescript 0.74 -> 0.84, Air 0.82 -> 0.89, WSL 0.73 -> 0.75; ML does not
   move (0.46 -> 0.47): its accesses are dictionary GaveUps, which the cache
   knob does not touch, so ML needs the dictionary half (flatten outside the
   lock, or a dictionary-capable cache). Estimated +3-4 % geomean.
4. **Double arrays (T4-O).** Known from the sixth round; now counted: `aes`
   0.75-1.5 M Double->Contiguous stops per run (3.3-6.7 s inside the
   conductor), ML 6,732 (30 ms), `sha256` 2,051; and the Int32->Contiguous
   substitution costs the typed-array-style loops their unboxed lanes. Knob
   (relabel in place as GIL on): `aes` 0.23 -> 0.79 of GIL on, `sha256` 0.70 ->
   0.86, ML 0.49 -> 0.51, `pbkdf2` 0.83 -> 0.83. Estimated +4 % geomean, and it
   is the `aes` floor.
5. **`Map`/`Set` operations are calls GIL off.** The DFG refuses the
   hash-table intrinsics GIL off (their inline bucket walk has no
   lock-free validation), so `get`/`has`/`set` go through the generic call
   path into the C++ lock-free reader / locked writer. Counts: Basic 22.9 M
   reads, WSL 8.4 M, OfflineAssembler 1.9 M, Air 313 K. Knob (intrinsics
   inlined): Basic 0.50 -> 0.67, WSL 0.73 -> 0.78, Air +2 %. Estimated +1.5 %.
6. **RegExp compile check under the cell lock.** GIL off every match takes
   the RegExp's cell lock to read "has code" coherently with a racing
   compile (`compileIfNecessary` and its MatchOnly twin). Counts:
   OfflineAssembler 26.8 M, regexp 8.0 M, UniPoker 3.1 M, FlightPlanner
   1.9 M. Profile: `~Locker<JSCellLock>` 8.5 % of OfflineAssembler's samples,
   105 ms in regexp, 39 ms in FlightPlanner. Estimated +1-1.5 %.
7. **GIL-off array codegen in loops (the `CheckTraps` clobber set, the
   pinned write-predicate word, I41).** `crypto` is the clean case (its
   deficit is all FTL, §6.2); §6.5 has the disassembly. Knobs on `crypto`:
   dropping the value-heap writes at the poll changes nothing (1,080 ->
   1,076); also dropping the butterfly-pointer / `vectorLength` writes gives
   1,080 -> 1,282 (0.74 -> 0.84 of GIL on); dropping `MiscFields` too adds
   nothing; the parkable-slow-path clobber knob changes nothing measurable
   on any test. On the other 17 tests the poll knobs are within noise. So
   the LICM cost of the GIL-off poll model is real but confined to
   array-walking FTL loops, and it is the butterfly pointer, not the
   plain-data visibility rule, that matters. Estimated +1 % geomean
   (`crypto` +17 %, `Babylon`/`gbemu`/`splay` +2-4 %).
8. **GIL-off rope resolution** (`convertToNonRopeGILOff`, the locked publish
   of a resolved rope's fiber): pdfjs 2.5 M, OfflineAssembler 1.9 M,
   typescript 1.4 M, FlightPlanner 705 K, UniPoker 642 K, Babylon 488 K;
   17-47 ms per test in the profiles. Estimated +0.5 %.
9. **DFG and FTL OSR exits reach the exit operation on every exit** (the exit
   jump is never repatched GIL off; the DFG thunk saves and restores every
   register around `operationCompileOSRExit`, which then finds the compiled
   ramp; the FTL exit thunk has a thin prefix, the DFG one does not): WSL
   128 K, gbemu 25 K, typescript 18 K, Basic 14 K exits per run against
   50-500 GIL on. About 1 us each: WSL ~100 ms, gbemu ~25 ms. Estimated
   +0.5 %.
10. **Generator resume claims** (`claimGeneratorResume`/`publishGeneratorResume`,
    the GIL-off frame-ownership handshake): Basic 2.5 M (50 ms, 5 % of its
    deficit), WSL 913 K.
11. **Smaller, measured, not ranked**: the megamorphic-cache miss counts
    themselves; `pthread_getspecific` (WTF's `Thread::current()` is not
    fast-TLS on Linux and GIL-off-only paths call it: 42 ms in `aes`, 18 in
    gbemu — goes away with items 2 and 4); `softStackLimitForCurrentThreadGilOffSlow`
    (5-14 ms); the per-allocation TLC dispatch (three dependent loads before
    the free-list pop; not separable in these profiles, `obj-literal` micro
    row 1.00); the write-barrier slow path (fewer calls GIL off than on in
    most tests); the GIL-off compilation lock (uncontended); parks (none).

What is NOT on the list although the prompt for this round expected it near
the top: the parkable-slow-path heap clobber (no measurable cost anywhere),
the `CheckTraps` value-heap writes (none outside array loops), stop
frequency single-threaded (zero stops in 20 of 24 tests apart from
watchpoint fires and jettisons, which GIL on has too; the exceptions are all
item 4), per-thread caches' hit rates (the JSON and numeric-string caches do
not appear), `operationCreateThis` (same count in both modes), exceptions
(same), F1/F2 fires (zero single-threaded).

### 6.5 Codegen audit: `am3` (crypto's inner loop)

`am3` (two Int32-array reads, one Int32-array write and integer arithmetic
per iteration, arrays reached through `this.array` / `w.array`) run alone:
flag off 260 ms, GIL on 290 (the poll), GIL off 527. FTL, the loop block
(Air): GIL on 35 instructions, GIL off 54. The 19 extra, per iteration:
the read-side butterfly is re-loaded and re-masked after the poll instead of
living in a register (3: reload the object from its spill, `and` the mask
into the word) and the bounds check reads `publicLength` through it instead
of a hoisted copy; each Int32 load is followed by the I41 lane check lowered
as two `Compare64` into registers and a `BranchTest32` (3 instructions and
two temporaries per load, where one compare-and-branch after the existing
hole check would do); the write re-loads the target object from its spill,
loads the tagged word through the pinned patchpoint (I16), xors the TID tag
(itself spilled), masks, compares and branches (8); and the extra
temporaries push `m` to the stack (a spill store and two reloads). With the
butterfly writes dropped from the poll's clobber set (knob) the block is 46
instructions and the loop runs in 410 ms: the read side hoists, the write
side stays pinned by I16 and I41 stays. GIL on emits the flag-off loop plus
the poll: its arrays' structures have valid thread-local sets, so E1/E2
elide the predicates, and its `CheckTraps` is modelled as flag-off.

Also confirmed from the code, not separately timed: FTL `MultiPutByOffset`
refuses (re)allocating transition variants GIL off (one InvalidationPoint
per variant would be needed); DFG OSR exit dispatch is the patchable jump to
the generation thunk in every DFG compilation GIL off (item 9); direct calls
are data ICs in both modes; the DFG never inlines `Map`/`Set` intrinsics GIL
off (item 5); allocation sites bake a TLC slot and load lite -> table ->
allocator before the free-list pop (three dependent loads GIL on does not
have).

### 6.6 Multi-threaded, first pass

Counters on the scaling suite's workloads at 1 and 4 threads, GIL off
(scale 1): string-heavy 1,692 -> 7,864 ms wall (0.86x "speedup"), with 456
parks totalling 11 ms, 216 stops (5 ms), stopped GC 197 ms and 18
synchronous full sweeps (362 ms) — together 7 % of the 4-thread wall time,
so neither stops nor GC pauses explain it. What the workload does per
thread: 3.3 M structure transitions (its substring keys are new every
iteration, so nearly every `table[key] = ...` creates a Structure — the same
count flag-off), 5.9 M GIL-off rope resolutions, 2.3 M `PutByVal` GaveUps,
and the block churn of item 1 (72 K blocks at 1 thread, 179 K at 4). A flat
profile at 4 threads against 1 has the shared atom-string table's insert
(`addToStringTable` 7.6x the 1-thread samples for 4x the work),
`equalInternal` 6.4x, `LockAlgorithm::lockSlow` 11x, `materializePropertyTable`
5.6x and Structure marking on top; i.e. structure creation (property-table
materialization and copies, the transition table, structure allocation)
and atomization under contention, plus collecting 13 M structures. The
sixth round's tier-up-counter finding stands for the short-function case;
this workload's 4-thread loss is dominated by the Structure and atom paths.
raytrace-like 222 -> 309 ms at 4 threads (2.9x): no parks, 22 stops, stopped
GC 47 ms; its 4-thread residue is item 1 (119 K blocks minted) and eden
pauses. To be broken down per lock in the multi-thread item of this round.

### 6.7 Order of work that follows from the ledger

By expected geomean gain and floor-raising: (1) block retention in the
shared heap (item 1); (2) the virtual-call fast path GIL off (2); (3)
dictionary flattening off the IC lock plus a per-thread megamorphic cache
(3); (4) Double arrays (4); (5) `Map`/`Set` intrinsics with validated inline
reads (5); (6) the RegExp compile check as an acquire load (6); (7) array
loop codegen: butterfly hoisting under the epoch-jettison argument, a
one-instruction I41 check, an unpinned owner test (7); (8) rope resolution,
DFG exit dispatch through the exit vector, generator claims (8-10); then the
multi-thread items (structure/atom contention, tier-up counters, eden pauses
with N threads) and the carried-over findings.

### 6.8 After the round: what the ledger got right and wrong

Sections 1-3 were re-taken on the final tree; LANDING-PLAN "Results, seventh
round" has the per-item account. Against §6.4's ranking:
- Right, and acted on: block churn (item 1), the RegExp lock (6), virtual
  calls unlinked (2), the Double stop under mixed shapes (4), Map/Set reads as
  calls (5). Each moved the test it was found in by roughly what its count
  predicted; none moved the geometric mean by more than 2 % alone. Item 1's
  final form keeps the sixth round's Full-cycle sweep (§10E; an embedder's
  idle server otherwise kept 3 GB resident), so `splay`, with eighteen Full
  collections per run, keeps about half of the churn the first form removed.
- Built and withdrawn: dictionary flattening GIL off (3). It removed `Air`'s
  401 k generic loads per run and was reverted when the amplifier tied crashes
  to it (SPEC-jit history §33); the megamorphic cache GIL off (also item 3)
  was not attempted.
- Wrong: "gbemu is an OSR-exit storm GIL off (23 k exits against 2 k)". Both
  modes take the same ~21,300 exits from the same 160 sites per run
  (`--printEachOSRExit`); the counter compared counts passes through
  `operationCompileOSRExit`, which GIL on takes once per exit site (the exit
  jump is then repatched to the ramp) and GIL off takes on every exit (the
  jump is never repatched under concurrent execution, SPEC-ungil §A.1.3, so
  each exit runs the generation thunk's register dump and the operation's
  lock-free ramp lookup - about 4 ms of a run, not a storm). gbemu's deficit
  is its generic IC traffic (1 M GetById, 480 k PutByVal, 230 k InstanceOf per
  run) with the megamorphic cache off.
- Wrong in size: the FTL structure-check poison, which §5 had first and §6.4
  at 5-8 %: under 1.5 % by the clobber-model experiment; not pursued.
- Not in the ledger, found by the work: watcher-less Class-A fires were most
  of GIL on's stop requests on the IC-heavy tests (`typescript` 3,460 per run);
  shared-profile write traffic and counter false sharing under N threads
  (§2); the arity/entry mirror was a sixth-round omission, not a design cost.

### 6.9 Eighth round: what was worked, what was measured and left, and the ledger now

Worked (LANDING-PLAN "Results, eighth round" has the per-item account; §1-§3
are re-taken on the final tree): the per-thread megamorphic cache (§5 item 2,
first half), dictionary flattening as a transition (item 2, second half),
the
FTL's butterfly hoisting across polls with vectorLength bounds and the
one-compare I41 check (item 3, codegen half), generator resume claims in the
DFG/FTL (item 6, part), the replace cache and RegExp statics memo (item 6,
part), the eden allowance under N allocating threads (item 5, the allocation
half), allocation-paced Full collections keeping their blocks (the `splay`
remainder of §6.8's item 1), OSR exits reaching their ramps without the
operation (§6.8's "not a storm" - 4 ms was an underestimate of what the
register dump and the operation cost 25,000 times a run, but it stays a
small term), the Double-array stops from converting array modes, and Double +
Contiguous fast copies.

Measured this round and left, with the evidence a next round starts from:

- `gbemu` (GIL off 0.68 of GIL on on the final tree) is NOT slower in its
  optimized code: JIT-annotated profiles (`--useJITDump` + `perf inject`)
  give its two hottest FTL functions the same sample counts in both modes,
  and per-200 ms tier histograms show GIL on FTL-dominant from 0.2 s while
  GIL off spends 0.2-0.6 s of a 2.8 s run in Baseline/DFG code. The trace
  (`--verboseOSR`, `--printEachOSRExit`): the hottest function's third DFG
  compile speculates a `to_this` structure that the emulator object then
  never has again (it gains a property after its first nine calls of each
  iteration), so every call exits at bc#1; GIL on met the same situation one
  compile earlier and reoptimized after 31 exits because the function's
  Baseline execution counter crossed its threshold between exits (the loop
  trigger); GIL off each exit's ramp rewrites that counter and, the exits
  being every call, it never crosses, so replacement waits for the plain
  exit-count threshold - 400 exits at that retry level, 0.7 s. Why the two
  modes' compile sequences diverge one step earlier (GIL off's second DFG
  compile exits ten times at a `CheckIsConstant`, bc#129, in 50 ms and
  reoptimizes; GIL on's runs 200 ms) is the open question; nothing in it is
  a threads protocol cost, and a general fix (count entry exits toward the
  loop trigger, or treat an exit at function entry on every call as
  frequent sooner) would be an upstream tiering change.
- Array growth by `push` GIL off always copies: a published flat
  vectorLength is immutable GIL off (a lock-free foreign reader bounds by
  it and, on a weakly ordered target, could read a lane above the old bound
  before the cleared value), and precise (large) allocations cannot be
  reallocated in place because the old storage may still be read. GIL on
  grows into the size-class slack in place and reallocates precise
  allocations. Counted (new `ensureLength*` counters): `gbemu` copies
  1,129 MB a run GIL off against 30 MB GIL on (13,800 fresh copies against
  6,100 in-place reallocations + 8,300 small copies), `pdfjs` 1,183 MB
  against 264 MB, `stanford-crypto-aes` 1.82 GB against 1.41 GB (750,000
  slack raises GIL on). The cost is the copying and the 2.3x eden count it
  causes (`gbemu` 40 edens against 17), about 5 % of `gbemu`; GC pause time
  itself differs by 23 ms a run. Two ways forward, both decisions rather than
  fixes: allow the in-place slack raise on x86-64 only (TSO orders the
  reader's bound load before its lane load; an ARM64 port would need an
  address dependency in every bounds-checked lane load), or grow OWNED
  large arrays segmented (no copy; every access pays the fragment
  indirection - forcing all arrays segmented halves `gbemu`, so it would
  have to be size-gated).
- `splay` (0.67): GC time is equal (241 ms a run both modes) and block
  minting is now lower GIL off than GIL on; the score difference is the
  worst-case component (206 against 364): eleven Full collections a run
  against eight, each landing in a scored iteration. The Full triggers are
  the shared heap's (§10B); which one fires the extra three is not yet
  attributed.
- `Basic` (0.54 by the five-run medians, single runs 550-600 against 810):
  the trimodal collapse is gone (P3(b)); the remaining distance is generator
  frame materialization and `Map` iteration through host calls (§6.4 item 7,
  unchanged) and the same early-tier dwell as `gbemu`.
- `OfflineAssembler` (0.75): 1.9 M rope resolutions a run take the cell lock
  and the destructible-block notification GIL off (`ropeResolveGILOff`); a
  compare-and-swap publish of the fiber word would halve that, not remove it.
- ML (0.71), `navier-stokes` (0.83), `float-mm` (0.83), `raytrace` (0.84):
  boxed doubles where GIL on has Double arrays (T4-O's substitution);
  SPEC-objectmodel history §29 is the designed way out.
- string-heavy after the loop-entry fix (LANDING-PLAN P8) is bimodal: at
  four threads a run takes 2.3-2.5 s (2.3x) or 3.7-6 s, at eight 3.3 s (3.5x)
  or 7-12 s; the gate's five-run medians (§2) landed on 1.5x and 3.7x. The
  threads now reach the FTL loop-entry code, and that code takes one
  `Overflow` exit (the FNV multiply) on every entry until it is recompiled,
  which happens when its exit count reaches 100 x (1 + live threads) x 2^r -
  r being the function's reoptimization retry counter. The slow runs are the
  ones where r is high: they show 3-5 heap-fact-rewrite jettisons (SPEC-jit
  §5.5, counted toward r on purpose, once per thread whose stack held the
  function) and 9-10 jettisons in all, the fast runs none and 5-6; at r = 9
  with five live threads the threshold is 250,000 exits, so the entry code
  is never replaced and each of the ~2,000 loop entries a run enters it and
  exits. Each exit costs a Baseline stretch, a DFG entry and an FTL entry.
  The same family as `gbemu`'s dwell - the tiering policy treats a
  speculation that fails on every pass like one that fails rarely - compounded
  by two multipliers that were each added for a real case (LANDING-PLAN Open
  items has the policy options).
- The broad 10-20 % on object-heavy tests (`delta-blue`, `richards`,
  `Babylon`, `typescript`, `UniPoker`, `pdfjs`, `ai-astar`): no protocol
  event dominates their counters any more; what remains is straight-line
  cost in the transition and allocation paths (claim CAS, TTL-set loads, the
  shared-write and segmented-word tests on every butterfly access) and the
  early-tier dwell above. Attributing it needs per-symbol instruction
  deltas on JIT code (the §6.5 method) test by test.
