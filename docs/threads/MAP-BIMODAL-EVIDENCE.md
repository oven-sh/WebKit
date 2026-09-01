# MAP-BIMODAL-EVIDENCE — intcs W=16 phaseA bimodality

Pinned env: `JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1
JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1`. Bench: `Tools/threads/scalebench/js/bench.js
-- 16 intcs`. Repo @ `0666c87d35bd`, Release `WebKitBuild/Release/bin/jsc`.

## Round 1

### Terminology (resolves §34/§40 ambiguity)

**fast-mode** = phaseA ≈ 1600 ms; **slow-mode** = phaseA ≈ 4600 ms. §34(C)
"logGC suppresses fast-mode (0/15)" means logGC perturbation forces SLOW.
fast-mode is the timing-sensitive favourable state.

### (1) 30-rep base characterization (`exp1`, `/usr/bin/time -v` + `$vm` reads)

Instrumented copy `/tmp/bench-instr.js` adds NON-PERTURBING reads at the
post-phaseA barrier on thread 0 only: `$vm.totalGCTime()`, `gcHeapSize()`,
`heapCapacity()`, `$vm.heapExtraMemorySize()`. Raw: `/tmp/bimodal-r1/exp1.log`.

| mode | n | phaseA_ms | user_s | cpu% | vcsw | icsw | gcTimeA_s | heapSizeA_mb |
|---|---|---|---|---|---|---|---|---|
| **fast** | 14/30 | 1487–1670 (med 1630) | 23–29 (med 25) | 799–867 | 311 660–530 404 (med 352k) | 98–421 | **0.423–0.472** | 444–485 |
| **slow** | 16/30 | 4439–4787 (med 4622) | 78–84 (med 80) | 1271–1308 | 67 970–88 386 (med 76k) | 394–919 | **0.400–0.454** | 459–483 |

Clean bimodal split, **zero intermediate values**, gap = 2.84×. Not
order-correlated (slow reps 1-4, then alternates), not loadavg-correlated
(loadavg 1.9→9.9 across the run; fast/slow interleave at every level).
Checksums identical all 30 reps (`e85d66e7|4158480|15cf18bb|651b594b|abc7704f`).

### (2) GC counter — REFUTES candidate (a)

`$vm.totalGCTime()` measured at post-phaseA barrier (non-perturbing field
read; `JSDollarVM.cpp:3940 vm.heap.totalGCTime()`): **fast 0.423–0.472 s,
slow 0.400–0.454 s — INDISTINGUISHABLE**. heapSize 444–485 MB both modes,
heapCapacity 540–710 MB both modes. ($vm has no `gcCount()`; `totalGCTime`
is the available non-perturbing proxy and is conclusive: identical GC wall
implies identical GC cycle count × cycle cost.) **§32 distinct-allocator
gate hypothesis is dead.** §34(C)'s "fewer GC cycles in fast-mode" inference
from sys_s/cpu% was wrong — the lower sys_s/cpu% in fast-mode has a
different cause (below).

### (3) `perf stat` per-rep (`exp3`, 20 reps)

Raw: `/tmp/bimodal-r1/exp3.log`. perf-stat itself perturbs (fast-rate
4/20; phaseA bands shift to ~3850/~5400).

| mode | n | instructions | cycles | IPC | branch-miss% | ctx-sw |
|---|---|---|---|---|---|---|
| fast | 4 | **77.8–80.4 G** | 80.6–85.5 G | **0.94–0.97** | 0.96–0.97% | 342–369 k |
| slow | 16 | **135.1–140.7 G** | 254–272 G | **0.51–0.53** | 0.60–0.66% | 154–219 k |

**Slow-mode executes 1.74× more instructions at 1.83× worse IPC** (product
= 3.18× cycles, matching user-time 80/25 = 3.2×). This is **executed work,
not wait** — the extra cycles are real computation. The lower IPC is
consistent with heavy indirect-call / branch traffic (C++ slow-path calls
from JIT code), not cache-line spin (cache-refs/misses unsupported on this
host so cannot confirm directly, but a spin-lock would show flat
instructions + low IPC, not +74% instructions). vcsw is **higher in fast
mode** (4.6×): fast-mode threads finish each ingestDoc faster, so the ~140
`lock.hold` acquisitions per doc collide more and park (`lockProtoFuncHold`
→ futex); slow-mode threads spend longer in the un-locked
genDocTextI/tokenize portion so shard-lock contention is lower.

### (4) `perf record -g` mid-phaseA (`exp4`, 10 reps; sampled t∈[0.5s,1.5s])

Raw: `/tmp/bimodal-r1/perf-{slow,fast}-*.data`. 6/10 fast (perf-record
attach perturbs toward fast; opposite of perf-stat). Profiles are
**completely different**:

**slow-mode** (3 reps consistent):
```
 7-8%  slow_path_bitor                       (Baseline arith slow path)
 3.5-5.9%  stringProtoFuncCharCodeAt         (host call — NOT intrinsic-inlined)
 2.9-3.7%  JSRopeString::view
 3.2-3.4%  operationGetByIdOptimize          (Baseline IC repatch)
 2.2-2.9%  CodeBlock::binaryArithProfileForPC
 2.1-2.6%  operationValueAddProfiled
 1.1-1.7%  operationGetFromScope
 0.47%     stringProtoFuncSubstring          (host call)
 0.13%     operationCompileOSRExit
```

**fast-mode**:
```
 16.5%  operationGetByIdGaveUp              (DFG/FTL GetByIdFlush fallback)
  7.9%  JSArray::pushInline
  3.6%  lockProtoFuncHold
  2.2%  WTF::equal(StringImpl,StringImpl)   (Map key compare)
  ~10%  HeapHelper SlotVisitor / visitChildren (concurrent GC mark)
  0.9%  JSRopeString::resolveRope
```

**slow-mode signature = Baseline-tier C++ slow paths** for arith,
charCodeAt, get_by_id, scope. fast-mode signature = DFG/FTL with
`operationGetByIdGaveUp` as the dominant cost (the documented GIL-off
GetByIdFlush from `bench.js:29-46`).

### (4b) Tier-gating probes (pristine `bench.js`, no instrumentation)

| variant | n | phaseA_ms | bimodal? |
|---|---|---|---|
| `JSC_useDFGJIT=0` | 6 | 2528–2821 | **NO — monomodal ~2700** |
| `JSC_useFTLJIT=0` | 12 | {1645–1690}×4 / {4468–4706}×8 | YES (4/12 fast) |
| `JSC_useConcurrentJIT=0` | 12 | {1609–1619}×2 / {4344–4629}×10 | YES (2/12 fast) |
| `nomap` arm | 12 | 1007–1184 | **NO — monomodal ~1100** |

**Slow-mode (4600 ms) is 1.67× SLOWER than Baseline-only (2700 ms).** This
is not "stuck in Baseline" — DFG is making it WORSE. FTL is irrelevant
(bimodal persists with `useFTLJIT=0`). Concurrent-JIT race is not the
mechanism (bimodal persists with `useConcurrentJIT=0`; rate drops to 2/12
but both bands at the same values).

`perf record` of `useDFGJIT=0` (2879 ms) gives a **third distinct
profile**: 17.2% `CallFrame::setCurrentVPC`, 10.7% `slow_path_bitxor`,
6.4% `binaryArithProfileForPC`, 3.8% `slow_path_bitor` — but **no**
`stringProtoFuncCharCodeAt` / `operationGetByIdOptimize` /
`operationValueAddProfiled` / `operationGetFromScope` in the top 25.
Those four are slow-mode-ONLY and are the +1800 ms over pure-Baseline.

### (5) Map-outside-lock variant (`/tmp/bench-nolock.js`, diagnostic)

`ingestDocI` shard loop changed to `shard.lock.hold(noop)` then
`shard.map.get/set` + push **inline in ingestDocI** (no per-term closure).
14 reps: 8 crashed (expected — racy Map.set), 6 survived: {2858, 3283,
5445, 5447, 5545, 5651}. **Still bimodal** (2/6 fast), bands shifted
(+~1000 ms both) — moving Map ops out of the closure does not collapse the
modes. Weak evidence (small n, variant changes IC shape) but rules out
"the lock.hold closure's separate CodeBlock is the bistable function".

### (6) Rope pre-resolve — moot

intcs-phaseA Map keys are tokens from `tokenize()` line 289
`String.fromCharCode(...codes)` → already flat. `termOf()` builds ropes
(`String.fromCharCode(..)+s`) but those feed `parts.push` in `genDocTextI`
then `parts.join("")` (flat result) → `text.substring` (flat backing). The
2.9–3.7% `JSRopeString::view` in slow-mode is from `tok.charCodeAt(0)` /
`tok.substring(1)` in `genDocTextI:688-690` on the termOf rope — that work
is deterministic and present in both modes; in fast-mode it's 0.9%
`resolveRope` (DFG-intrinsic'd charCodeAt resolves once). Not a bimodal
trigger.

### Ranked discriminants

1. **DFG enablement is necessary AND sufficient for bimodality**
   (`useDFGJIT=0` → monomodal 2700; `useFTLJIT=0` still bimodal). The
   bistable state is a DFG compilation outcome, not a GC/heap/lock state.
2. **Slow-mode burns 3.3× user CPU at identical GC time** (gcTimeA 0.42 s
   both modes; user 80 s vs 25 s). Refutes (a) outright.
3. **Slow-mode is 1.67× slower than pure-Baseline** with a profile that
   pure-Baseline does NOT show (`operationGetByIdOptimize`,
   `stringProtoFuncCharCodeAt` host-call, `operationValueAddProfiled`,
   `operationGetFromScope`). Signature of **DFG-compiled code that calls
   generic C++ helpers** (charCodeAt/substring not intrinsic-recognised,
   ValueAdd not int-speculated, get_by_id emitted as
   `operationGetByIdOptimize` repatch path) and/or a sustained
   OSR-exit→Baseline-with-cold-ICs loop. `operationCompileOSRExit` is only
   0.13% so the exits themselves are cheap; the cost is the destination.
4. **Map<string> presence is necessary** (`nomap` monomodal 1100). The
   trigger lives in the `tf.get/set(string)` and/or
   `shard.map.get/set(string)` path's effect on DFG profiling/compilation.
5. **`useConcurrentJIT=0` does NOT resolve it** — the race that selects
   the mode is not "did the concurrent compile finish in time"; it's in
   the **profiling data** the (now-synchronous) DFG compile reads.

### Candidate re-ranking after Round 1

| | before | after | evidence |
|---|---|---|---|
| (a) GC count differs | open | **REFUTED** | gcTimeA identical both modes |
| (b) HashMapImpl rehash storm | open | **unlikely-primary** | rehash count is deterministic; doesn't explain +74% instr or DFG-dependence |
| (c) rope-resolve cascade | open | **REFUTED as trigger** | tokens already flat; rope work deterministic |
| (d) Structure transition convoy | open | **promoted** | `operationGetByIdOptimize` 3.3% slow-only = Baseline IC keeps repatching → polymorphic Structures on `pl`/`shard`; 16 threads each allocating `{docIds:[],tfs:[]}` may yield 16 birth Structures |
| (e) MapImpl::add cellLock | open | **demoted** | no cellLock hot in any profile; vcsw HIGHER in fast mode |
| **(f) NEW — bistable DFG compilation of the ingestDocI hot chain** | — | **PRIMARY** | useDFGJIT=0 monomodal between the two bands; slow-mode profile = generic-C++-helper DFG code; fast-mode profile = intrinsic'd DFG code. The 16-thread profiling race produces two stable profile states (likely at the `tf.get(t)`/`shard.map.get(term)` MapGet site or the `pl.docIds` GetById site) and DFG compiles to either an intrinsic'd fast body or a generic-helper slow body |

### §34(C) correction

§34(C) says "JIT tier is not the discriminant (one `JSC_verboseOSR=1` rep
shows ~29k FTL OSR entries either way)". That measurement was (a) one rep,
(b) +47% perturbed, (c) measured FTL OSR-**entry** count not the
compilation **content**. Round 1 shows the discriminant IS the JIT — not
which **tier** (both modes reach DFG; ingestDocI itself stays Baseline both
modes per `$vm.baselineJITTrue()` sampling) but which **DFG body** is
installed for the leaf functions. The §34(C) logGC-suppresses-fast finding
is consistent: per-GC `dataLog()` adds latency early in phaseA, shifting
which thread's profile sample wins the race toward the slow-body outcome.

### Round-2 discriminants queued

- `JSC_printEachOSRExit=1` on `N_BASE_SMOKE` (small enough to not flood):
  count exits per CodeBlock per mode.
- `JSC_dumpDFGDisassembly=1` filtered to `ingestDocI`/`genDocTextI`/
  `tokenize`/`termOf` + the lock.hold closure: diff fast vs slow DFG IR
  for the MapGet / GetById / StringCharCodeAt nodes.
- 16-thread `{docIds:[],tfs:[]}` Structure-identity test: do worker
  threads' fresh-object allocations get distinct Structures under
  `useVMLite`/`useSharedGCHeap`? (would explain `operationGetByIdOptimize`
  churn on `pl.docIds`).

### diffClean

`git diff Source/` empty (verified). All instrumentation in
`/tmp/bench-instr*.js`, `/tmp/bench-nolock.js`, `/tmp/bimodal-*.sh`;
raw data `/tmp/bimodal-r1/`.

## Round 2

### (7) DFG-compile-count discriminant (`JSC_reportDFGCompileTimes=1`, 12 reps)

| mode | n | phaseA_ms | total DFG compiles | termOf | genDocTextI | tokenize | ingestDocI |
|---|---|---|---|---|---|---|---|
| fast | 3 | 1608–1646 | 74–76 | **1** | 2 | 3 | 2 |
| slow | 9 | 4440–4746 | 107–110 | **15** | **8** | **9** | 2 |

`nomap` W=16 (3 reps, monomodal): termOf=1, genDocTextI=1-2, tokenize=3 ==
fast-mode counts. **Slow-mode = termOf/genDocTextI/tokenize recompile loop**;
all 15 termOf bodies are byte-identical (2048 B). Candidate (f) confirmed as
mechanism shape.

### (8) `JSC_verboseOSR=1` exit-kind census (6 reps; perturbs to 5.8/11 s)

All non-UncountableInvalidation exits in slow-mode reps:

| function | bc# | exit kind | DFG node | bytecode op |
|---|---|---|---|---|
| **termOf** | **22** | **BadIndexingType** ×19 | D@40 GetButterfly | `get_by_id base:String "fromCharCode"` |
| **tokenize** | **266** | **BadIndexingType** ×9 | D@272 GetButterfly | `get_by_id base:String "fromCharCode"` |
| **genDocTextI** | **254** | **BadIndexingType** ×8 | D@265 GetButterfly | `get_by_id base:String "fromCharCode"` |

`JSC_dumpDFGDisassembly=1` (15-compile slow rep): every termOf body has
`D@39 CheckStructure([%DI:Function]) → D@40 GetButterfly(String) →
D@41 GetByOffset(fromCharCode,67)`. D@40 emits the `useJSThreads()`
threaded-butterfly read predicate (DFGSpeculativeJIT.cpp:12086): segmented
check `branch64(AboveOrEqual, butterflyWord, 0xffff<<48)` →
`speculationCheck(BadIndexingType)`. So D@40 exits **iff StringConstructor's
butterfly is Segmented**.

### (9) Per-thread Structure identity test — refutes (d)

16 worker threads + main each `describe({docIds:[],tfs:[]})`, `new Map()`,
`[]`: **all 17 threads same StructureID** (pl=16843904, map=16843520,
arr=16788608). `{docIds,tfs}` is structure-monomorphic across threads.
Candidate (d) per-thread-Structure dead.

### (10) Reify-race discriminant — **PROVES root cause**

`String.fromCharCode` is a lazy static prop on StringConstructor (initial
butterfly: 3/4 props, Flat, TID=0). For `intcs`/`noconcat`, the FIRST access
is from `phaseAI` after the start barrier — a 16-thread race (main is id=0,
TID=0). For `nomap`, the `nmShardOf[]` precompute (bench.js:988-991) calls
`termOf()` at module init → main-thread reification before any worker exists.

| variant (intcs W=16, pinned env) | n | phaseA_ms | mode |
|---|---|---|---|
| baseline | 30 | 12 in [1512,1791] / 18 in [4364,4746] | bimodal 40%/60% |
| `String.fromCharCode(97);` at line 207 (main reifies) | 15 | [1517,1672] | **15/15 fast** |
| `(new Thread(()=>String.fromCharCode(97))).join();` at line 207 (worker reifies) | 12 | [4441,4691] | **12/12 slow** |

**Mechanism**: worker-reify = foreign-TID structure transition on a Flat
butterfly → `convertToSegmentedButterfly` (ConcurrentButterfly.cpp:1095-1099,
"Flat + foreign/SW transition → segment") → StringConstructor butterfly
Segmented for life → every D@40 GetButterfly takes the segmented branch →
BadIndexingType exit storm. main-reify = owner-TID transition → stays Flat
→ D@40 never exits. The recompile doesn't learn because
`DFGByteCodeParser::handleGetById` doesn't check
`hasExitSite(BadIndexingType)` (only array ops do — DFGByteCodeParser.cpp:
552, 612, 2684, …) so it re-emits CheckStructure+GetButterfly+GetByOffset
every time.

### Candidate ranking after Round 2

| | R1 status | R2 status | evidence |
|---|---|---|---|
| (a) GC count | refuted | refuted | — |
| (b) rehash storm | unlikely | **dead** | recompile-loop is the +74% instr |
| (c) rope cascade | refuted | refuted | — |
| (d) per-thread Structure | promoted | **refuted** | (9) all-same StructureID |
| (e) cellLock | demoted | dead | — |
| (f) bistable DFG body | primary | **superseded by (g)** — (f) is the symptom |
| **(g) `String.fromCharCode` reify-race → segmented StringConstructor → GetButterfly BadIndexingType recompile loop** | — | **ROOT CAUSE** | (7)+(8)+(10) |

### §40 correction

`nomap` is monomodal **because of its main-thread `termOf()` precompute**,
not because `Map<string>` was removed. `noconcat` (no precompute) is bimodal
exactly like `intcs` — already in the §40 data ({1261,1311,1312,3176,3569,…}).

### Fix applied

`Tools/threads/scalebench/js/bench.js` +1 effective line at module init
(after `$imul`): `String.fromCharCode(97);`. SCALEBENCH.md §44 has the full
30-rep before/after + gates. Engine-side recompile-loop bug is a named §44
residual.

### diffClean

`git diff Source/` empty (verified). `git diff Tools/` = bench.js +18 lines
(the fix + comment). Raw data `/tmp/bimodal-r2/` (base.jsonl, after.jsonl,
comp-*.txt, vosr-*.txt, dfgdis.txt, struct-test.js, bench-{pre,anti}warm.js).
