# SCALEBENCH — scalability of a big threaded program (JS threads vs Go vs Java)

Two runs of the same frozen benchmark on the same host:

- **Run 1** (2026-06-10, pre-fix tree): §0–§8 below, kept verbatim as the
  historical record. Its two release blockers have since landed (GC
  under-marking fix `25375a997f4f`; STW-watchdog fire-under-lock fix
  `6b298a4fdd99`).
- **Run 2** (2026-06-10, fixed tree, rebuilt Release jsc
  sha256 `b5c3a009d514…`): §9–§14 — re-run with the run-1 js quarantine
  accommodations REMOVED from `run.sh` (js cells count for real), plus the
  BEFORE/AFTER accounting of what the two fixes bought and the remaining
  ceilings.

Results JSON: `Tools/threads/scalebench/results.json` — now two sections,
`run1` and `run2` (run-1 copy also at `results-run1.json`; run-2 raw tables
`RESULTS.md`, run-1 at `RESULTS-run1.md`; per-run artifacts `out/` for run 2,
`out-run1/` for run 1).

---

# RUN 1 (historical record — pre-fix tree)

This document answers the question asked on the threads PR: **how does
scalability hold up on big programs that use the threads** — not
microkernels. It also reports the parallel-self criterion (linear scalability
running a program in parallel with itself, no deliberate sharing) from
`Tools/threads/scaling-gate.sh` / `JSTests/threads/scaling/`.

**Known structural handicap:** GC under JS threads on this branch is
currently stop-the-world with parallel marking; concurrent marking is
designed (SPEC-congc.md) but not implemented. Go and Java ship fully
concurrent collectors. Allocation-heavy phases (A especially) are expected to
show STW pauses scaling with heap size and thread count. Results must be
reported with this stated up front, not buried.

## 0. The honest answer, up front

On this tree, **the big-program matrix could not be completed for JS at
W >= 2**: an open shared-GC-heap correctness bug (under-marking during STW
collections with N mutators — live cells swept and re-allocated; §5)
corrupts the benchmark's shared heap and kills 41 of 42 JS runs at
W in {2..64} (uncaught corruption exceptions, SIGSEGVs, SIGABRTs). Go and
Java completed the full matrix with bit-identical checksums; JS completed
W=1 with the same checksums.

On the parallel-self suite (independent work, no deliberate sharing,
GIL-off), the picture at N <= 8 is:

| workload | character | speedup(4) | speedup(8) | notes |
|---|---|---|---|---|
| splay-like | GC-pressure tree churn | 1.47x | 1.59x | best result; its class floors are 2.0/3.0 — still VIOLATION |
| string-heavy | rope/atom churn | 1.15x | 1.14x | flat; one N=8 run died in the STW watchdog |
| map-heavy | Map alloc churn | 1.11x | 1.13x | fully GC-serialized |
| raytrace-like | FP compute + small-object churn | 0.67x | 0.65x | NEGATIVE scaling (N threads slower than 1 doing 1/N the work) |
| richards-like | OO dispatch + small objects | 0.19x | 0.23x | catastrophic negative scaling (T(2) = 15x T(1)) + STW-watchdog SIGABRTs |

So: **the threading infrastructure is functionally there** (locks, atomics,
barriers, threads; checksums at JS W=1 and the smoke runs match Go/Java
exactly; serial identity for plain flag-off code is intact), but **on this
tree the engine does not scale at all yet, on any workload in either
suite**: the best parallel-self result is 1.59x at 8 threads, two workloads
scale NEGATIVELY, and at least two open engine bugs (§5 corruption, §6.3
STW-watchdog deadlock) sit in front of any scalability work. Concurrent
marking (SPEC-congc.md) is the designed fix for the dominant GC
serialization; the negative-scaling workloads additionally point at
per-collection stop/handshake and watchpoint/STW-storm costs that grow with
N (§7).

No spin: by the design's own success criterion ("linear scalability when a
program is run in parallel with itself"), the current tree does not pass on
any of the five workloads, and the big-program benchmark is blocked on a
correctness bug before multi-thread scalability can even be measured for JS.

## 1. Machine, toolchains, binary provenance

- AWS instance, Intel Xeon Platinum 8488C (Sapphire Rapids), 1 socket,
  **64 vCPUs = 32 physical cores x 2 SMT**, 1 NUMA node, 247 GB RAM.
  W in {48, 64} is therefore SMT/oversubscription territory by hardware, not
  just scheduling.
- Kernel 6.12.68-92.122.amzn2023.x86_64, Amazon Linux 2023.10.
- Go: `go version go1.24.13 linux/amd64`.
- Java: OpenJDK 21.0.10 LTS, Corretto-21.0.10.7.1, mixed mode, sharing.
- jsc: `WebKitBuild/Release/bin/jsc` from branch `jarred/threads`, pinned
  flag set (the platform under test, not tuning):
  `--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
  --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1`.
- Build verified current with the tree before the run (`ninja jsc`: no work
  to do). **Disclosure:** a concurrent CVE-close session rebuilt the Release
  binary during the batch window (mtime 05:21:38Z at preflight, 05:29:05Z
  mid-matrix). The six js W=1 runs straddle the swap and agree within 3.7%
  (23.7-24.6 s) with identical checksums; go/java are separate runtimes and
  unaffected. The parallel-self runs (§6) all used the 05:29:05Z binary.
- JSC compiler/GC helper threads are part of the platform and are NOT
  counted in W. `cpu_util` may exceed 1.0; it is reported as-is.

Quiet-host protocol: the run phase started at 1-min loadavg **891.20**
(another workflow's build finishing) and waited until < 4 (reached 04:45Z)
before any measured run; the orphaned fuzzer processes found spinning at
100% CPU (PPID 1, WebKitBuild/Fuzz jsc, 1.5 h old) were killed first. During
the matrix the runner's own load gate added 30 s and post-run settle excess
60 s (total 90 s, under the SPEC §6 5-minute disclosure threshold). A
concurrent session ran bounded single-jsc CVE verification loops (~1 core,
intermittent) during parts of the window; this is disclosed, not hidden —
it cannot explain any of the order-of-magnitude effects reported here.

## 2. What the benchmark is (SPEC summary)

`Tools/threads/scalebench/SPEC.md`, frozen, N_BASE pinned 2026-06-10.
A concurrent in-memory inverted index over a synthetic corpus, ~300-600
lines per language, three phases in one process, all threads spawned once
and reused (hand-rolled counting barrier from Lock+Condition equivalents in
all three languages):

- **Phase A — INGEST**: 28,000 generated documents (~85-212 tokens each),
  claimed via one shared atomic counter; hand-rolled tokenizer (two
  allocations per token); per-document tf map; postings appended to
  **128 shards** of {mutex, plain hash map} — one lock acquisition per
  (doc, distinct term). Real string work, real shared-map writes, real
  contention (Zipf-skewed terms hammer hot shards).
- **Phase B — QUERY**: 28,000 ops, 90% readers (point / 2-3-term AND /
  scored top-20 against a frozen df snapshot) / 10% writers (full ingest of
  a new doc). Reader results filtered to base docs so checksums are
  timing-independent.
- **Phase C — ANALYTICS**: parallel group-by over all shards into 104
  shared groups under group locks, then top-20 per group.

All arithmetic is unsigned 64-bit (JS: BigInt masked to 64 bits — the spec
REQUIRES this; it is also what triggers the §5 engine bug's allocation
churn). splitmix64 PRNG, FNV-1a hashing, no floating point anywhere.
Five checksums (A, postings, A2, B, C) must be bit-identical across all
three languages and ALL thread counts.

### Fairness rules (binding; SPEC §2)

1. Same algorithm, same abstraction level: sharded plain hash map + plain
   mutex everywhere. Java: NO ConcurrentHashMap/StampedLock/LongAdder.
   Go: NO sync.Map, NO RWMutex, NO channels for the queue. JS: NO
   SharedArrayBuffer tricks; `Lock`, `Atomics.*` on plain object properties.
2. Idiomatic but unoptimized; no pooling/arenas/interning beyond runtime
   defaults; pinned builder-style text assembly + hand-rolled tokenizer.
3. Identical inputs/constants; runner cross-checks via JSON output.
4. Checksum gate across the whole matrix.
5. No floating point in measured code.
6. Default runtime flags (the pinned JS thread flag set is the platform
   under test and exempt). One documented exception per language allowed
   for pathological defaults — none was needed for go/java; the single
   recorded exception entry documents the §5 JS engine bug accommodation
   (an engine bug, not a flag change — no JS flags were altered).
7. W OS threads: JS `new Thread`, Go goroutines (GOMAXPROCS default),
   Java platform threads.

Matrix: W in {1, 2, 4, 8, 16, 32, 48, 64}; 1 warmup + 5 measured reps per
cell, medians; languages interleaved java,go,js per rep so drift hits all
three equally; 1-min loadavg < 4 gate before every run.

## 3. Big-program results (medians of 5; speedup vs same language W=1)

Checksums across all 103 successful runs (full go/java matrix, js W=1, plus
one surviving js W=2 rep):
`A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2, B=c4bdd580f85ee058, C=af028188d7a56a96`
— bit-identical in every successful cell, all three languages. The
three-language W=1 and W=4 smoke gates (N_BASE=2000) also matched exactly
(js W=4 smoke failed with the §5 corruption; go/java W=4 matched).

### Total wall time

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 24307 | 1.00x | 1771 | 1.00x | 1898 | 1.00x |
| 2 | FAILED 4/5 | — | 1070 | 1.66x | 1307 | 1.45x |
| 4 | FAILED 5/5 | — | 712 | 2.49x | 1067 | 1.78x |
| 8 | FAILED 5/5 | — | 514 | 3.45x | 956 | 1.99x |
| 16 | FAILED 5/5 | — | 402 | 4.40x | 901 | 2.11x |
| 32 | FAILED 5/5 | — | 370 | 4.79x | 1079 | 1.76x |
| 48 | FAILED 5/5 | — | 362 | 4.89x | 1047 | 1.81x |
| 64 | FAILED 5/5 | — | 386 | 4.59x | 1134 | 1.67x |

### Phase A — INGEST (allocation + shared-map writes; the GC-handicap phase)

| W | js ms | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|
| 1 | 15363 | 1275 | 1.00x | 1271 | 1.00x |
| 8 | — | 354 | 3.60x | 513 | 2.48x |
| 32 | — | 230 | 5.55x | 504 | 2.52x |
| 64 | — | 243 | 5.24x | 558 | 2.28x |

### Phase B — QUERY 90/10 (read-mostly)

| W | js ms | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|
| 1 | 2825 | 395 | 1.00x | 493 | 1.00x |
| 8 | — | 71 | 5.58x | 246 | 2.01x |
| 32 | — | 45 | 8.81x | 268 | 1.84x |
| 64 | — | 49 | 8.13x | 258 | 1.91x |

### Phase C — ANALYTICS (group-merge under shared locks)

| W | js ms | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|
| 1 | 127 | 14 | 1.00x | 30 | 1.00x |
| 8 | — | 11 | 1.31x | 83 | 0.36x |
| 64 | — | 12 | 1.15x | 220 | 0.14x |

(Full 8-row tables for every phase, plus min/max per cell, are in
`Tools/threads/scalebench/RESULTS.md` and `results.json`.)

### Peak RSS (MB, median) and CPU utilization

| W | js RSS | go RSS | java RSS | js cpu | go cpu | java cpu |
|---|---|---|---|---|---|---|
| 1 | 421 | 178 | 837 | 1.02 | 1.48 | 2.05 |
| 8 | — | 182 | 796 | — | 0.84 | 1.02 |
| 64 | — | 206 | 914 | — | 0.23 | 0.69 |

cpu_util = (user+sys)/(wall x W) over the FULL process lifetime (includes
JVM startup; each run's `inprogram_share` in results.json quantifies the
dilution — go ~0.97 at W=64, java ~0.90). Values > 1 at W=1 are the
runtimes' own helper threads (Java's concurrent GC most, 2.05).

### JS failure detail (the §5 bug, per cell)

Failure modes across the 40 failed JS runs (warmup + 5 reps x 7 cells, 1
W=2 rep survived): exit 3 = uncaught corruption exception (BigInt parse /
type error from a corrupted posting list), 139 = SIGSEGV, 134 = SIGABRT
(libstdc++ assertion / RELEASE_ASSERT):

| W | outcomes |
|---|---|
| 2 | 5x exit-3, 1 OK (42.2 s — 1.7x SLOWER than W=1) |
| 4 | 4x exit-3, 2x SIGSEGV |
| 8 | 2x exit-3, 4x SIGSEGV |
| 16 | 2x exit-3, 3x SIGSEGV, 1x SIGABRT |
| 32 | 1x exit-3, 1x SIGSEGV, 4x SIGABRT |
| 48 | 6x SIGABRT |
| 64 | 6x SIGABRT |

No run produced a silently-wrong checksum (the quarantine path recorded
zero hits); every corrupt run died loudly before or at its own checksum.

### What the go/java columns establish (the benchmark is sound)

- The workload scales to ~4.9x (Go) on 32 physical cores by DESIGN: hot
  Zipf terms serialize on shard locks, Phase C is a deliberate shared-append
  merge, and Phase A is allocation-bound. Go's cpu_util at W=64 is 0.23 with
  inprogram_share 0.97 — the idle time is real lock-wait, not startup. This
  is the intended "big program with contended sharing", not an
  embarrassingly-parallel strawman: a JS implementation on a sound engine
  has ~5x headroom to chase, not 64x.
- Java's plateau at ~2x (and Phase C's 0.14x backslide) is the same shape:
  HashMap+ReentrantLock per shard with biased-lock-free JDK21 monitors;
  its concurrent GC keeps Phase A at 2.5x where Go reaches 5.8x.
- JS W=1 is 12.8x slower than Go W=1 end-to-end. ~2/3 of that is the
  BigInt-based u64 arithmetic the spec mandates for checksum identity
  (every PRNG step and hash allocates a heap BigInt; Phase A is 15.4 s of
  predominantly BigInt churn vs Go's native uint64 at 1.3 s). This is a
  single-thread throughput gap, reported as-is, but it is NOT the
  scalability answer — the scalability answer for JS is blocked on §5.

## 4. SPEC §5.5 checksum gate status

- go, java: PASS at every W (all 96 runs, warmups included).
- js: PASS at W=1 (6/6 runs) and the surviving W=2 rep; remaining
  W in {2..64} runs failed before completing (no mismatching checksum was
  ever emitted — corruption is loud on this workload).
- Batch declared `valid: true` with the JS cells recorded `failed` per
  SPEC §6 and nulled medians; the single SPEC §4 "exceptions" entry
  documents the engine-bug accommodation (run.sh would otherwise have
  aborted the whole batch including the sound go/java cells — see the
  comment block above `JS_SHARED_HEAP_BUG` in run.sh).

## 5. The blocking engine bug (gates ALL JS W >= 2 results)

`Tools/threads/scalebench/js/repro-bigint-shared-ingest.js` (found by the
implementation phase; narrowed by this run phase — full status block in the
file header). Shape: 4 threads doing spec-mandated BigInt PRNG churn while
appending to Lock-protected shared posting lists corrupt the shared heap
~100% of runs at W=4 within ~2 s of work.

What this phase established (Release jsc, 2026-06-10 05:29Z binary):

- **GC-dependent**: `--useGC=0` -> clean 4/4. GC on -> corrupt ~100%.
- **Not marking parallelism**: `--numberOfGCMarkers=1` still corrupts.
- **Not generational/remembered sets**: `--useGenerationalGC=0` (full
  collections only) still corrupts; `--useConcurrentGC=0` still corrupts.
- **Live cells are being swept**: `--sweepSynchronously=1` converts the
  silent aliasing into immediate crashes. Under lazy sweep the re-allocation
  is delayed, which is why the default mode shows silent corruption.
- **Shared-heap specific**: `--useSharedGCHeap=0` (other flags kept) ->
  clean; `--useSharedAtomStringTable=0` -> still corrupts.
- **Corruption shape** (diagnostic dump): a shared array's butterfly ALIASES
  another thread's freshly-allocated Map storage — term strings and small
  counts interleaved into a docIds array, parallel arrays diverging in
  length. Consistent with a cell that was in-flight (held only in a
  mutator's registers/stack at the STW handshake) being missed by
  conservative-root/newlyAllocated accounting, swept, and re-handed to
  another thread's allocator.
- **Masked by instrumentation**: Debug build (same tree) 0/2, TSan build
  (rebuilt from this tree) 0/1 with zero TSAN reports, GIL-on 0/2 —
  Release-timing-only. TSAN will not name this one; it needs the
  hypothesis-driven treatment (thread-bughunter) with the repro above,
  which is fast and deterministic enough to bisect instrumentation into.

This is plausibly the same root cause as the parked butterfly-stress
silent-corruption case (Tools/threads/bughunt/EVIDENCE.md) — that case's
old broad signature space (cross-object aliasing) matches this shape — but
that identification is a hypothesis, not a finding.

## 6. Parallel-self suite (Pizlo's original criterion)

`Tools/threads/scaling-gate.sh` (report mode), `JSTests/threads/scaling/`:
each workload runs N threads of identical INDEPENDENT work (no deliberate
sharing); perfect scaling is speedup(N) = N x T(1) / T(N) = N. Floors (for
--gate on a quiet host): speedup(4) >= 2.8 and speedup(8) >= 4.5
(splay-like: 2.0 / 3.0).

### 6.1 As shipped (GIL-ON: the script passes only `--useJSThreads=1`)

The suite predates GIL removal; its stock invocation measures the GIL build:
map-heavy speedup(2..64) = 0.99-1.01x — textbook GIL serialization, as
designed for phase 1. Serial identity on this (noisy-window) run: T(1)
flag-on 1634.9 ms vs flag-off 1418.4 ms = +15.3% (VIOLATION vs the 5%
tolerance; see §6.4). The stock run then aborted at raytrace-like N=16,
which exceeded the default 120 s cell timeout — 16x serialized work does
not fit the budget; report-only artifact, not an engine failure.

### 6.2 GIL-OFF (pinned flag set via wrapper `Tools/threads/scalebench/out/jsc-giloff`; cell timeout 1800 s; N in {1,2,4,8})

From `scaling-gate.sh` (map-heavy, raytrace-like — medians of 3) and manual
cells with the same harness invocation for the rest (richards/string/splay:
harness-reported SCALING times, 2 runs, best shown — best-case for the
engine; the gate itself aborted at richards-like N=4 on a first attempt with
the §6.3 watchdog SIGABRT, so those three workloads' rows could not come
from a single gate run):

| workload | T(1) ms | speedup(2) | speedup(4) | speedup(8) | floors (4/8) |
|---|---|---|---|---|---|
| map-heavy | 2278 | 1.04x | 1.11x | 1.13x | 2.8 / 4.5 — VIOLATION |
| raytrace-like | 10227 | 0.68x | 0.67x | 0.65x | 2.8 / 4.5 — VIOLATION |
| richards-like | 3621 | 0.14x | 0.19x | 0.23x | 2.8 / 4.5 — VIOLATION |
| string-heavy | 2730 | 1.31x | 1.15x | 1.14x | 2.8 / 4.5 — VIOLATION |
| splay-like | 3544 | 1.21x | 1.47x | 1.59x | 2.0 / 3.0 — VIOLATION |

richards-like is the standout pathology: two threads of independent work
take 53-54 s where one takes 3.6 s (T(2) = 14.7x T(1)) — consistent with a
repeating storm of Class-A watchpoint fires each forcing a stop-the-world
(the same path that intermittently trips the §6.3 watchdog on this
workload). raytrace-like's 0.65-0.68x is the same shape at lower intensity.

An exploratory wide sweep (N up to 64) showed map-heavy flat at ~1.0-1.2x
through N=64, and raytrace-like at N=16 running at ~800% CPU while wall time
blew past 600 s — parallel execution is real (8 cores busy), but it is spent
in GC stop/start and allocator slow paths, not progress.

### 6.3 STW-watchdog deadlock (second open engine bug, distinct from §5)

richards-like at N=4 and N=8, and string-heavy at N=8, intermittently
(~1/3 of runs) die in:

```
JSThreads stop-the-world failed to reach a stopped world within 30.000000s.
Pending Class-A fire context: ... (WatchpointSet Class-A fire).
  entered lite ... tid=1 ... hasHeapAccess=true  <== NON-QUIESCENT (blocking the stop)
  entered lite ... tid=2 ... hasHeapAccess=true  <== NON-QUIESCENT (blocking the stop)
  entered lite ... tid=3 ... hasHeapAccess=true  <== NON-QUIESCENT (blocking the stop)
```

then SIGABRT. This is the watchdog family thread-ab17b/ab17e worked in,
recurring under a WatchpointSet Class-A fire with multiple non-quiescent
lites. It is a liveness release-blocker independent of §5.

### 6.4 Serial cost of the flag set (single thread, map-heavy)

Same binary, same host window: flag-off 1418 ms -> `--useJSThreads=1`
(GIL build) 1635 ms (+15.3%) -> full pinned GIL-off set 2278 ms (+60.6%
vs flag-off). The +15.3% GIL-on identity violation and the +60% GIL-off
single-thread tax on this Map-allocation-heavy workload are findings of
this run (host had ~1 intermittent background core; the bench-gate history
records +3.1% on its own workload on a quiet host — the map-heavy number
needs a quiet-host re-measure before being treated as final, but the
ordering flag-off < GIL < GIL-off-set was stable across every run today).

## 7. Analysis — where it stands, why

1. **The dominant scalability cost today is the stop-the-world GC, exactly
   as the §0 handicap statement predicts.** map-heavy (pure Map/alloc churn)
   gets ZERO parallel speedup: N threads allocate N x as fast, every
   collection stops all N, and collection work grows with live set — wall
   time grows ~linearly with N. raytrace-like and richards-like are worse
   than 1.0x: their small-object churn drives constant eden cycles whose
   stop/handshake overhead scales with N, so adding threads adds GC rounds
   faster than it adds compute. Go/Java do not pay this: their collectors
   run concurrently with mutators.
2. **Parallel execution is real but mostly wasted.** The mutator-side
   machinery does run N threads on N cores (raytrace-like N=16 was observed
   at ~800% CPU; the §3 matrix's failed JS W=4 cells ran at ~500% before
   dying), and splay-like/string-heavy show genuine if small wall-time wins
   (1.59x / 1.14x at 8). But no workload in either suite reaches even half
   its floor: the cycles go to GC stop/handshake rounds, allocator slow
   paths, and (for richards-like) what looks like a watchpoint-fire STW
   storm — richards' T(2) = 14.7x T(1) cannot be explained by GC volume
   alone and matches the §6.3 Class-A fire signature.
3. **Correctness gates scalability**: the §5 under-marking corruption makes
   every shared-write-heavy JS program unsafe at W >= 2 on this exact tree
   (the full corpus is green because its tests don't combine sustained
   BigInt-rate allocation with cross-thread shared appends at this
   intensity — this benchmark is precisely the "big program" gap the PR
   question pointed at). The §6.3 watchdog deadlock intermittently kills
   even no-sharing programs at N >= 4.
4. **Lock/atomics overhead differences are visible but second-order.** JS
   `Lock.hold` is a closure call per acquisition and `Atomics.add` on an
   object property is a runtime call; Go inlines a CAS fast path and Java
   biases to a fast monitorenter. At W=1 these show up inside the 12.8x
   single-thread gap (with BigInt churn the dominant term); at W >= 2 they
   are unmeasurable behind the GC wall.
5. **What would change the answer**: (a) fix §5 (deterministic repro in
   hand; TSAN-blind, so instrumentation-bisect or audit the in-flight-cell
   liveness chain: conservative roots of parked lites + newlyAllocated
   accounting per TLC); (b) fix §6.3 (watchdog context already names the
   Class-A fire path); (c) land SPEC-congc concurrent marking — without it,
   no allocation-heavy workload will pass the 2.8x/4.5x floors regardless
   of correctness, and the big-program matrix's Phase A will stay
   GC-bound; (d) re-run this entire document's §3 matrix. The harness,
   corpus, pinned constants and checksums are frozen and reusable as-is.

## 8. Reproduction

```
# Big-program matrix (writes results.json/RESULTS.md):
Tools/threads/scalebench/run.sh

# Engine-bug repro (~100% at W=4, ~2s):
WebKitBuild/Release/bin/jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
  --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
  Tools/threads/scalebench/js/repro-bigint-shared-ingest.js

# Parallel-self, GIL-off:
SCALING_CELL_TIMEOUT_SECS=1800 Tools/threads/scaling-gate.sh \
  --threads "1 2 4 8" Tools/threads/scalebench/out/jsc-giloff
```

---

# RUN 2 (2026-06-10, fixed tree)

## 9. Setup, provenance, protocol

- Same host as run 1 (64 vCPU = 32 physical Sapphire Rapids x 2 SMT, 247 GB),
  same toolchains (go1.24.13, Corretto 21.0.10), same pinned flag set, same
  frozen SPEC/N_BASE (28000) and seed.
- Release jsc rebuilt AFTER both fixes landed: `ninja jsc` reported no work
  to do against the fixed tree; binary mtime 2026-06-10 17:02:33Z, sha256
  `b5c3a009d5142da68002104669896779c6ac3ab95ce11312c1d0119e2b5b7ab0`
  (recorded in `jsc-build-id.txt` and results.json). No binary swap occurred
  during this batch (single session, machine owned exclusively).
- Fix presence verified behaviorally before the matrix:
  `js/repro-bigint-shared-ingest.js` at W=4 — **5/5 clean** (was ~100%
  corrupt); richards-like N=4 (scale 1/16, `-e` injection) — **5/5 clean,
  no watchdog abort** (was ~1/3 SIGABRT).
- Quiet host: 1-min loadavg 0.57 at start (no wait needed); no orphaned
  jsc/fuzzer processes found (checked; none to kill, unlike run 1). Runner
  gate delay 0 s, settle delay 585 s (all own-decay allowance), settle
  excess 0 s — no external-interference disclosure triggered.
- Accommodation removal: the run-1 `JS_SHARED_HEAP_BUG` smoke tolerance and
  the js W>=2 `--expect-tuple` checksum quarantine were deleted from
  `run.sh`. One new, narrower mechanism was added in their place: the
  preflight smoke retries a CRASHED leg up to 3 attempts (any language;
  motivated by the §11 residual's ~8% smoke-rate — a 10% coin flip should
  not abort a multi-hour batch in preflight). Checksum comparison is never
  relaxed, matrix cells never retry, and every crash is logged. The js W=4
  smoke leg passed on attempt 1 in the recorded batch.

## 10. Run-2 big-program results

Checksums across **all 113 successful runs** (warmups included; full go/java
matrix, js W=1 complete, every surviving js W in {2,4,8} run):
`A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2, B=c4bdd580f85ee058, C=af028188d7a56a96`
— bit-identical across all three languages and both runs (same tuple as
run 1). All six smoke legs (3 languages x W in {1,4}) matched. Zero
silently-wrong checksums; batch `valid: true`, `exceptions: []`.

### Total wall time (medians of 5; speedup vs same language W=1)

| W | js ms | js speedup | js ok/5 | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|---|
| 1 | 33373 | 1.00x | 5/5 | 1737 | 1.00x | 1893 | 1.00x |
| 2 | 41913 | 0.80x | 4/5 | 1068 | 1.63x | 1303 | 1.45x |
| 4 | (47.0–47.4 s, 2 survivors) | ~0.71x | 2/5 | 701 | 2.48x | 1041 | 1.82x |
| 8 | (45.0–46.0 s, 2 survivors) | ~0.73x | 2/5 | 512 | 3.39x | 977 | 1.94x |
| 16 | FAILED 5/5 | — | 0/5 | 401 | 4.34x | 931 | 2.03x |
| 32 | FAILED 5/5 | — | 0/5 | 360 | 4.82x | 976 | 1.94x |
| 48 | FAILED 5/5 | — | 0/5 | 370 | 4.70x | 1092 | 1.73x |
| 64 | FAILED 5/5 | — | 0/5 | 357 | 4.87x | 1153 | 1.64x |

js W in {4,8} cells have 2/5 surviving reps — below the 3 needed for a
median, so results.json records them failed-with-null-medians; the surviving
raw runs (correct checksums) are quoted above as ranges. go/java: 80/80
clean, shapes within noise of run 1.

### js phase medians (surviving runs)

| cell | A ingest ms | B query ms | C analytics ms | RSS MB | cpu_util |
|---|---|---|---|---|---|
| W=1 (5/5) | 18680 | 5591 | 138 | 11898 | 1.16 |
| W=2 (4/5) | 22607 (0.83x) | 6167 (0.91x) | 545 (0.25x) | 14389 | 0.72 |
| W=4 (2 raw) | 29127–30019 | 6216–7467 | 615–656 | ~13000 | — |
| W=8 (2 raw) | 26621–26775 | 7584–8668 | 648–702 | ~12885 | — |

### js failure detail (run 2: 31 failed runs of 144 total js incl. warmup)

Exit 133 = SIGTRAP (libpas `pas_deallocation_did_fail` /
RELEASE_ASSERT-class), 134 = SIGABRT, 139 = SIGSEGV:

| W | outcomes (warmup + 5 reps) |
|---|---|
| 1 | 6/6 OK |
| 2 | 5 OK, 1x SIGABRT |
| 4 | 2 OK, 2x 133, 1x 134, 1x 139 |
| 8 | 3 OK, 3x 133 |
| 16 | 1 OK (warmup), 3x 133, 2x 134 |
| 32 | 3x 133, 3x 134 |
| 48 | 6x 133 |
| 64 | 3x 133, 2x 134, 1x 139 |

Every failed run died loudly; the checksum gate over the surviving
population is fully intact.

## 11. BEFORE/AFTER — what the two fixes bought

### Fix 1: GC under-marking (`25375a997f4f`)

| metric | run 1 (before) | run 2 (after) |
|---|---|---|
| `repro-bigint-shared-ingest.js` W=4 | ~100% corrupt | 5/5 clean, deterministic output |
| js W=2 big-program | 41/42 multi-thread js runs died; 1 survivor | **4/5 complete with bit-identical checksums** |
| js W=4/W=8 | 0 survivors | 2/5 survivors each, correct checksums |
| js W>=16 | 0 survivors | 0 survivors (different bug — below) |
| silent wrong checksums | 0 | 0 |

The headline: **JS completion at W=2 is real now** — the under-marking
corruption that killed essentially every multi-thread big-program run is
gone (its dedicated repro is deterministic-clean), and the engine completes
the full shared-write-heavy workload at W=2 with correct results most of
the time. What remains at W>=4 is a DIFFERENT, residual bug (§11.3).

**Cost side (new finding):** run-2 js W=1 is **+37% slower** than run-1 js
W=1 (33.4 s vs 24.3 s) and peak RSS went **421 MB -> 11.9 GB (28x)** on the
identical workload. The quiet-host flag-tax measurement (§13.3) attributes
both entirely to `--useSharedGCHeap=1`. The under-marking fix appears to
have bought correctness partly by severe over-retention/under-collection of
the shared heap. Speedups within run 2 are computed against run 2's own
(slower) W=1 baseline, so the js scaling numbers are not flattered by this.

### Fix 2: STW watchdog fire-under-lock (`6b298a4fdd99`)

| metric | run 1 (before) | run 2 (after) |
|---|---|---|
| richards-like N=4 (scale 1/16) 5x | ~1/3 watchdog SIGABRT | 5/5 clean |
| richards/string-heavy gate cells | intermittent watchdog SIGABRT (~1/3); gate aborted on first attempt | **0 watchdog aborts across the entire suite** (all 60 cells completed) |
| richards-like speedup(2/4/8) | 0.14/0.19/0.23x (best-shown, partial) | 0.15/0.19/0.24x (clean medians of 3) |

The fix bought **abort-free liveness**, not throughput: richards' negative
scaling is unchanged. That matches the watchdog hunt's own perf finding
(docs/threads/STW-WATCHDOG-CLOSURE.md): the fire-rate data REFUTED the
run-1 "Class-A watchpoint storm" frequency hypothesis — at the config where
T(2) ~ 14x T(1), all 54 Class-A/§A.3 stop windows total **1.6 ms** of a
48.8 s wall (~0.003%), and the stop-fire count is constant (~36/run,
warmup-phase one-shots) regardless of N and scale. Whatever serializes
richards GIL-off, it is not stop frequency or stop latency; that residue is
chartered to TTL-rebias/de-jank (§13.2).

### 11.3 NEW P0: residual shared-heap corruption crash (distinct from both fixed bugs)

Run 2's surviving blocker, found at js W=4 smoke in preflight and
characterized standalone (all on the fixed binary):

- **Rate**: ~8% of smoke-size W=4 runs (5/61); at full size it kills 1/5
  W=2, 3/5 W=4/8, and 5/5 W>=16 runs (exposure scales with work x threads).
- **GC-INDEPENDENT** — still fires with `--useGC=0` (3/20). This is NOT the
  under-marking class run 1 hit (that one was 0/4 with `--useGC=0`), and
  not a recurrence of the fixed bug (whose repro stays 5/5 clean).
- **Shared-heap-specific** — `--useSharedGCHeap=0` (other flags kept):
  0/20.
- **Always loud**: SIGTRAP (libpas "Large heap did not find object" on a
  garbage `StringImpl::deref`), SIGABRT, or SIGSEGV. Zero wrong checksums
  in 113 successful runs — the §5.5 gate never saw a silent lie.
- **Signature families** (cores preserved in
  `Tools/threads/scalebench/out/p0-cores/`): (a) LLInt `op_call_varargs` ->
  `setupVarargsFrame`/`JSCellButterfly::copyToArguments` reading a corrupt
  arguments array — the only varargs site in the benchmark is the spread
  `String.fromCharCode(...codes)` at js/bench.js:216 on a THREAD-LOCAL
  array; (b) null-structure cell in `JSObject::toPrimitive`; (c) garbage
  `StringImpl` pointer deref'd into a libpas panic; (d) garbage pc in JIT
  frame. All consistent with cross-thread corruption of freshly-allocated
  cells/strings in the shared heap, on the allocator/string path rather
  than the collector (it fires with GC off).

This is a P0 release blocker. It needs the bughunter treatment
(hypothesis-driven; the smoke-size repro is
`SCALEBENCH_SMOKE=1 jsc <pinned flags> bench.js -- 4 smoke`, ~8%/run,
~7.6 s/run, so a 50-run campaign is ~6 minutes).

## 12. Run-2 parallel-self suite (GIL-off, report mode)

Same command as run 1 (`scaling-gate.sh --threads "1 2 4 8"` through the
pinned-flag wrapper; medians of 3; cell timeout 1800 s). This time the gate
ran to completion in one pass — no watchdog aborts, no manual cells. The
wrapper makes the gate's own serial-identity row self-vs-self (ignore it);
the genuine tax is §13.3.

| workload | T(1) ms (run1 -> run2) | speedup(2) | speedup(4) (run1) | speedup(8) (run1) | floors |
|---|---|---|---|---|---|
| map-heavy | 2278 -> 2711 | 1.19x | 1.32x (1.11) | 1.28x (1.13) | 2.8/4.5 VIOLATION |
| raytrace-like | 10227 -> 10617 | 0.71x | 0.66x (0.67) | 0.61x (0.65) | 2.8/4.5 VIOLATION |
| richards-like | 3621 -> 3626 | 0.15x | 0.19x (0.19) | 0.24x (0.23) | 2.8/4.5 VIOLATION |
| splay-like | 3544 -> 3853 | 1.20x | 1.53x (1.47) | 1.54x (1.59) | 2.0/3.0 VIOLATION |
| string-heavy | 2730 -> 3686 | 1.57x | 1.32x (1.15) | 1.33x (1.14) | 2.8/4.5 VIOLATION |

lock-fairness smoke: ok (ratioW 1.23, no starvation).

Reading: modest, real improvements on the allocation/atom-churn workloads
(map-heavy +0.15-0.19, string-heavy +0.17-0.19 at N>=4 — and string-heavy
no longer dies), no change on the two negative-scaling workloads
(richards, raytrace), and every workload still violates its floor. The
T(1) inflations (map +19%, string +35%, splay +9%) are the §13.3 shared-heap
tax moving, which also means the run-2 speedup ratios are computed against
slower baselines — the absolute T(N) picture is no better than the ratios
suggest.

## 13. Honest remaining-ceilings analysis

Ranked by what they cost today:

1. **The §11.3 residual corruption crash (P0, correctness).** Caps the
   big-program matrix at W~8 for JS and would cap any real shared-heap
   program the same way. GC-independent, shared-heap-allocator/string
   shaped, loud, with a fast repro. Until it is fixed, every other ceiling
   below is academic above W=8.

2. **GC serialization — pending SPEC-congc implementation.** Unchanged from
   run 1 and still the dominant scalability wall for every allocation-heavy
   workload: collections are stop-the-world with parallel marking; N
   mutators allocate N x faster, every cycle stops all N. map-heavy at
   1.28-1.32x and splay-like at 1.53-1.54x against floors of 2.8-4.5/2.0-3.0
   are exactly this. SPEC-congc (concurrent marking for N mutators) is
   designed, frozen-spec'd, and unimplemented; note it did NOT converge in
   its 6 review rounds and needs an ungil-style finalize before
   implementation. richards-like's 0.15-0.24x is NOT explained by stop
   frequency — the watchdog hunt's fire-rate log
   (`Tools/threads/bughunt/stw-watchdog/firelog-n4.txt`, summarized in
   STW-WATCHDOG-CLOSURE.md) measured ~36 constant warmup one-shot fires
   per run and 1.6 ms total stop time against a 14x slowdown, refuting the
   run-1 storm hypothesis. The richards residue (mid-run samples show
   threads executing JIT code, yet 2 threads take 14x one thread's wall) is
   chartered to **TTL-rebias/de-jank**; until that lands, OO-dispatch-heavy
   code must be assumed to scale NEGATIVELY.

3. **The single-thread shared-heap tax — now measured and attributed, no
   longer "undiagnosed", but WORSE.** Run 1 measured +61% on a noisy host
   and called it undiagnosed. Run 2, quiet host, map-heavy T(1), medians
   of 5: flag-off 1373 ms -> GIL-on 1467 ms (+6.8%) -> pinned GIL-off
   2710 ms (**+97% vs flag-off**). Attribution by single-flag ablation:
   pinned-minus-`useSharedGCHeap` runs at GIL-on speed (1465 ms);
   pinned-minus-`useSharedAtomStringTable` is unchanged (2715 ms). RSS
   tells the same story on identical work: 3.72 GB (pinned) vs 155 MB
   (minus sharedGCHeap) vs 152 MB (GIL-on) — and the big-program W=1 cell
   went 421 MB (run 1) -> 11.9 GB (run 2). So the tax IS the shared-GC-heap
   configuration, and the under-marking fix made it materially worse
   (+61% noisy -> +97% quiet; the run-1 vs run-2 W=1 delta of +37% on the
   identical big-program cell is the same effect measured cleanly). The
   mechanism inside useSharedGCHeap (retention policy vs collection
   scheduling vs allocator path) is the next diagnosis step — plausibly the
   same work item as making SPEC-congc's collector actually collect under
   N mutators. The GIL-on +6.8% still violates the 5% identity tolerance
   on this workload (run 1 saw +15.3% noisy) and lands with the parked
   transition-bench adjudication (AB17f item-6 protocol).

4. **Java-shaped plateau is the benchmark's own ceiling, not JSC's.** Go
   tops out at 4.87x and Java at ~2x on 32 physical cores by design
   (Zipf-hot shard locks, shared merge phase). The JS target, once 1-3 are
   fixed, is the ~2-5x band — not 64x.

Bottom line, run 2: **both landed fixes did what they claimed** — the
under-marking corruption is gone (js W=2 completes with bit-identical
checksums; its repro is deterministic-clean) and the watchdog no longer
kills richards/string-heavy (0 aborts in the full suite). But the tree
still does not have a shippable scalability story: a NEW GC-independent
shared-heap crash gates everything above W~8 (P0, fast repro in hand), no
workload in either suite comes near its scaling floor (best: splay 1.54x
at 8 threads; richards still 0.24x), and the shared-heap mode now costs
+97% single-thread and ~28x RSS — a price that, per the §0 criterion, no
big program would accept today.

## 14. Run-2 reproduction

```
# Big-program matrix (writes results.json run2-equivalent + RESULTS.md):
Tools/threads/scalebench/run.sh

# Residual P0 repro (~8%/run at smoke size, ~7.6 s/run):
cd Tools/threads/scalebench && SCALEBENCH_SMOKE=1 \
  ../../../WebKitBuild/Release/bin/jsc --useJSThreads=1 --useThreadGIL=0 \
  --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 \
  --useThreadGILOffUnsafe=1 bench.js -- 4 smoke

# Fixed-bug regression checks (must stay clean):
WebKitBuild/Release/bin/jsc <pinned flags> \
  Tools/threads/scalebench/js/repro-bigint-shared-ingest.js   # 5x
WebKitBuild/Release/bin/jsc <pinned flags> \
  -e "globalThis.SCALING_THREADS=4; globalThis.SCALING_WORK_SCALE=0.0625;" \
  JSTests/threads/scaling/richards-like.js                    # 5x

# Parallel-self, GIL-off:
SCALING_CELL_TIMEOUT_SECS=1800 Tools/threads/scaling-gate.sh \
  --threads "1 2 4 8" Tools/threads/scalebench/out/jsc-giloff

# Single-thread tax + attribution (quiet host):
jsc [no flags | --useJSThreads=1 | pinned set | pinned minus one flag] \
  -e "globalThis.SCALING_THREADS=1; globalThis.SCALING_WORK_SCALE=1;" \
  JSTests/threads/scaling/map-heavy.js
```
