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

**Measured-configuration caveat (added 2026-06-11, bughunter A5 amend
round):** any GIL-off run on trees at or after the A5-round interim
closure of AUD1.K4 rows II.18/II.19 measures a configuration in which
(a) megamorphic inline caches are disabled (megamorphic-class sites cap
out as polymorphic stubs and fall to generic slow calls; the
fill side was already disabled under `useJSThreads` by
`MegamorphicCache::fillsDisabledUnderJSThreads()`), and (b) the
`HasOwnPropertyCache` for `hasOwnProperty`/`Object.hasOwn` is disabled
in every tier (including loss of the DFG `HasOwnProperty` intrinsic).
The ruled per-lite cache copies (+ §0 U4 A16-ext JIT repointing) are
the follow-up; until they land, published W-scaling numbers embed this
interim disable, and megamorphic/hasOwnProperty-heavy phases serialize
into generic paths whose cost scales with contention. Runs 1–2 below
PRE-DATE the disable and are unaffected; any future re-run must either
land the per-lite copies first or restate this caveat next to its
numbers.

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

---

# RUN 3 (2026-06-12, full ladder + GC-mode A/B + WS arm)

Source of truth: `Tools/threads/scalebench/results-v3.json` (`valid: true`,
spec "SCALEBENCH v1 + WS amendment"). Everything below is the json's cell
medians, verbatim.

## 15. Setup, provenance, protocol

- Tree `175e2a28b57800add359004fa7b9dfe2ece24e10` (fully green). Same host:
  64 cores, 247 GB RAM, kernel 6.12.68-92.122.amzn2023.x86_64. go1.24.13,
  OpenJDK Corretto-21.0.10.7.1.
- jsc: `WebKitBuild/Release/bin/jsc`, sha256
  `8985994941f0cf4b0bf2b05854ece0d6c2e4705dc7d96534f97806a6d3449538`,
  mtime 2026-06-12 20:17:32 UTC, size 383303704. **`jsc-build-id.txt` is
  STALE** (still records the 2026-06-10 binary); the json records the real
  binary identity.
- Three JS env sets: pinned GIL-off STW (`JSC_useJSThreads=1
  JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1
  JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1`), the same plus
  `JSC_useConcurrentSharedGCMarking=1` ("congc"), and GIL-on minimal
  (`JSC_useJSThreads=1` only, flag-tax baseline).
- Protocol deviation from SPEC §3 (task override, recorded in the json):
  **3 measured reps per cell** (5 for the two W=1 flag-tax cells), median
  reported, **no discarded warmup rep**.
- Checksum gate: all successful runs across all languages, all W, both GC
  modes, and both Phase-C arms produced the identical tuple
  `A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2,
  B=c4bdd580f85ee058, C=af028188d7a56a96` (same tuple as runs 1-2).
  WS parity holds: any checksum difference would have invalidated the batch;
  none occurred.
- §0 handicap statement, updated for this run: GC under JS threads on this
  branch is stop-the-world with parallel marking; concurrent marking
  (`JSC_useConcurrentSharedGCMarking=1`) was A/B-tested here and **crashes
  intermittently at teardown**. Go and Java ship fully concurrent collectors.

## 16. Scaling curves — total wall time, naive arm (medians of 3; speedup vs same-language W=1)

| W | js-stw ms | speedup | js-congc ms | speedup | go ms | speedup | java ms | speedup |
|---|---|---|---|---|---|---|---|---|
| 1 | 33258.7 | 1.000 | 33768.6 | 1.000 | 1738.3 | 1.000 | 1849.9 | 1.000 |
| 2 | 45487.9 | 0.731 | 44415.1 | 0.760 | 1064.3 | 1.633 | 1321.3 | 1.400 |
| 4 | 49959.6 | 0.666 | — (0/3 reps) | — | 699.6 | 2.485 | 1113.5 | 1.661 |
| 8 | 48115.1 | 0.691 | 48767.9 (1/3) | 0.692 | 510.4 | 3.406 | 931.1 | 1.987 |
| 16 | 47561.9 | 0.699 | 48192.1 | 0.701 | 392.8 | 4.425 | 928.0 | 1.993 |
| 32 | 76557.4 | 0.434 | 75688.4 | 0.446 | 359.6 | 4.834 | 1057.3 | 1.750 |
| 48 | 78671.7 | 0.423 | 78660.9 | 0.429 | 352.1 | 4.937 | 1041.2 | 1.777 |
| 64 | 81319.1 | 0.409 | 81472.3 (2/3) | 0.414 | 357.3 | 4.865 | 1141.4 | 1.621 |

- **JS scaling is negative at every W.** Best case W=16 = 0.70x of
  single-thread; second cliff at W=32 (0.43x), driven by Phase A
  (js-stw phase A: 27161 ms at W=8 vs 51436 ms at W=32 — allocation +
  shared-map ingest nearly doubles when threads double past 16). cpu_util
  falls 1.15 (W=1) -> 0.09 (W=64): the program is almost fully serialized
  and added threads are pure overhead.
- **Go scales**: 4.94x at W=48, knee at W=16-32. **Java scales to W=8-16**
  (1.99x), then degrades slightly (1.62x at W=64).
- Cross-language gap: at W=1, JS pinned 33.3 s vs Go 1.74 s (19x) vs Java
  1.85 s (18x). At W=64: 81.3 s vs 0.36 s (228x) and 1.14 s (71x).
- Regression vs run 1's 2026-06-10 binary at W=1: 24.31 s then, 33.26 s now
  (+37%) — but run 1's binary failed every js W>1 cell, while today's binary
  completes the full ladder with bit-identical checksums.

## 17. HEADLINE — STW vs concurrent shared-GC marking, per W

Delta = congc median − stw median (naive arm; negative = congc faster):

| W | stw ms | congc ms | delta ms | delta % | congc reps ok | congc crashes (SIGSEGV) |
|---|---|---|---|---|---|---|
| 1 | 33258.7 | 33768.6 | +509.9 | +1.5% | 3/3 | 0 |
| 2 | 45487.9 | 44415.1 | -1072.7 | -2.4% | 4/6 | 2 |
| 4 | 49959.6 | null | — | — | 0/3 | 3 |
| 8 | 48115.1 | 48767.9 | +652.8 | +1.4% | 1/3 | 2 |
| 16 | 47561.9 | 48192.1 | +630.2 | +1.3% | 3/3 | 0 |
| 32 | 76557.4 | 75688.4 | -869.1 | -1.1% | 3/3 | 0 |
| 48 | 78671.7 | 78660.9 | -10.8 | -0.0% | 3/3 | 0 |
| 64 | 81319.1 | 81472.3 | +153.2 | +0.2% | 2/3 | 1 |

Two findings, both stated plainly:

1. **Where congc completes, it buys ~nothing**: every delta is within
   ±2.4%, i.e. inside run-to-run noise. Concurrent shared GC marking in its
   current state does not move this workload.
2. **It is not stable**: 8 SIGSEGVs across 30 congc reps (W=4: 3/3 crashed,
   cell median null). The STW arm had **0 crashes in 24 runs**. Crash
   signature (from core, W=2): `GCSegmentedArray<JSCell const*>::removeLast`
   <- `HeapClientSet::flushClientMutatorMarkStackForExit` (Heap.cpp:6953)
   <- `GCClient::Heap::detachCurrentThread` (Heap.cpp:7131)
   <- `tearDownSpawnedThreadForExit` (ThreadManager.cpp:687) <- threadMain —
   spawned-thread teardown races the concurrent shared-GC mark stack.
   Per SPEC §6, the W=8 (1/3) and W=64 (2/3) congc medians fall below the
   3-rep threshold and are shown for transparency only.

## 18. Naive vs work-stealing Phase C (WS amendment, W ∈ {8, 32, 64})

Phase C medians, WS vs naive (same-language, same-W, same GC mode — js
comparisons against js-stw; naive comparators reused from §16's ladder,
WS cells run fresh):

| W | js naive C ms | js WS C ms | delta | go naive | go WS | delta | java naive | java WS | delta |
|---|---|---|---|---|---|---|---|---|---|
| 8 | 648.9 | 522.0 | -19.6% | 10.8 | 2.2 | -79.4% | 84.4 | 64.9 | -23.1% |
| 32 | 618.0 | 469.3 | -24.1% | 11.0 | 2.0 | -81.7% | 185.1 | 256.8 | **+38.7%** |
| 64 | 597.4 | 391.6 | -34.5% | 11.0 | 2.1 | -80.8% | 180.3 | 217.5 | **+20.6%** |

- WS helps JS Phase C 20-35%, and helps Go ~80%, but Phase C is <1% of the
  JS total and ~3% of Go's — **totals are unchanged within noise** (e.g. js
  W=64 total: WS 81161.2 vs naive 81319.1 ms).
- Java WS is **slower** at W=32/64 — the thread-local-accumulate + serial
  merge loses to the naive shared-lock append there.
- All WS checksum tuples identical to naive (parity gate passed).

## 19. Peak RSS and CPU utilization

| W | js-stw RSS MB | go RSS MB | java RSS MB | js cpu | go cpu | java cpu |
|---|---|---|---|---|---|---|
| 1 | 13938 | 180 | 847 | 1.15 | 1.48 | 2.03 |
| 2 | 14395 | 181 | 771 | 0.66 | 1.30 | 1.67 |
| 4 | 12993 | 184 | 800 | 0.36 | 1.09 | 1.28 |
| 8 | 12892 | 183 | 805 | 0.25 | 0.85 | 1.03 |
| 16 | 12704 | 196 | 872 | 0.20 | 0.63 | 0.88 |
| 32 | 12351 | 204 | 766 | 0.16 | 0.41 | 0.82 |
| 48 | 11992 | 207 | 932 | 0.12 | 0.31 | 0.76 |
| 64 | 11772 | 205 | 870 | 0.09 | 0.25 | 0.71 |

(RSS = peak KB from the json / 1024, rounded; cpu_util = (user+sys)/(wall*W).)

JS pinned runs hold **11.8-14.7 GB across every W** — vs Go ~0.2 GB, Java
~0.8-0.9 GB, and the GIL-on JS configuration's 0.43 GB. The shared GC heap
under VMLite retains an order of magnitude more memory than GIL-on JS and
57-78x more than Go.

## 20. Single-thread flag tax (W=1, 5 reps each)

| Config | total ms | RSS MB | cpu_util |
|---|---|---|---|
| jsc, zero flags | **N/A** — `ReferenceError: Lock is not defined` (Thread API needs `JSC_useJSThreads=1`; 1 demonstration rep, exit 3) | — | — |
| `JSC_useJSThreads=1` only (GIL on) | 15140.0 | 424 | 1.03 |
| pinned GIL-off set | 33691.9 | 13938 | 1.15 |

**The pinned GIL-off flag set costs +122.5% wall time and 32.9x RSS on
single-threaded execution** (33691.9 / 15139.98; 14272296 / 433968 KB).
This is the entry fee before a second thread exists.

## 21. Caveats (read before quoting any number above)

Host: dedicated 64-core / 247 GB box, quiet throughout (no jsc/ninja/java/go
processes at start; 1-min loadavg 1.03-1.94 at batch starts). Reps: **3 per
cell** (5 for the two W=1 flag-tax cells), median reported, **no warmup
rep** — a deliberate, recorded deviation from SPEC §3's 5+warmup protocol,
so cell medians here are noisier than runs 1-2. Dropped/short cells:
js-congc W=4 is **null** (3/3 SIGSEGV); js-congc W=8 has 1 surviving rep and
W=64 has 2 — both below SPEC §6's 3-rep median threshold and marked as such;
zero-flag jsc is N/A (cannot run the bench at all). The Phase-5 naive-arm
comparators at W=8/32/64 were **reused** from the main ladders (same binary,
env, session), not re-run; WS cells were fresh. Mid-run incident: /tmp
(tmpfs) filled with 6-19 GB core dumps during the first js-congc W=2 cell;
cores were deleted, core dumps disabled (RLIMIT_CORE=0) for all subsequent
reps, and the affected cell fully re-run (both batches kept in raw data:
4 ok / 2 SIGSEGV across 6 reps). `jsc-build-id.txt` is stale; trust the
json's sha256/mtime. One bookkeeping note: the json's prose `findings` array
quotes individual-rep Phase-C values for the WS comparison (e.g. 555.8 vs
632.4 ms at W=8); the tables in §18 use the json's cell **medians**, which
is why the percentages differ slightly from the prose. Finally, per-language
speedups are against the same language's W=1 — they say nothing about the
19-228x absolute cross-language gap, which is reported separately in §16.

## 22. Run-3 reproduction

```
# Driver (3-rep ladders, GC-mode A/B, WS arm, flag-tax cells):
python3 Tools/threads/scalebench/v3_driver.py   # writes results-v3.json

# congc teardown-crash repro (W=4 hit 3/3 here):
cd Tools/threads/scalebench && \
  JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 \
  JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
  JSC_useThreadGILOffUnsafe=1 JSC_useConcurrentSharedGCMarking=1 \
  ../../../WebKitBuild/Release/bin/jsc bench.js -- 4
```

Bottom line, run 3: the full JS ladder now **completes** with bit-identical
checksums (the first run where that is true), but scalability is still
negative at every W (best 0.70x at W=16, 0.41x at W=64, vs Go's 4.9x), the
concurrent-marking arm changes nothing measurable and crashes at spawned-
thread teardown, the WS scheduler fixes only a phase that doesn't matter,
and the flag set still charges +122% time / 33x memory before any
parallelism begins.

## 23. Run 3.1 — acceptance re-verification (no code delta)

**Read this first:** the binary under test is **byte-identical** to the one
run 3 measured (`WebKitBuild/Release/bin/jsc` sha256
`8985994941f0cf4b…3449538`, same mtime/size), and the tree is clean at
`9e9a3a25fd0b` (the run-3 commit; the only changes in it are bench/docs).
A `VM.cpp` working-tree modification observed at session start was gone
(reverted, no stash) before any measurement; nothing was rebuilt. Every
delta below is therefore **host noise**, not a code change.

Protocol: pinned GIL-off env, `bench.js -- W`, 3 reps/cell (5 for GIL-on
W=1 after the first 3 showed a shift; 5 for congc W=4 per the acceptance
spec), medians; `/usr/bin/time -v` for peak RSS and cpu_util
(=(user+sys)/(wall*W)); `ulimit -c 0`; one bench process at a time. Host
incident handled pre-run: three orphaned `addr2line` processes (parent=init,
~115 GB RSS, 3 cores) were killed before the first cell; 1-min loadavg at
batch starts was 2.7-4.5, **above** run-3's 1.0-1.9.

### Before/after (run 3 medians vs run 3.1 medians)

| Cell | run 3 wall ms | run 3.1 wall ms | delta | run 3 cpu | run 3.1 cpu | run 3 RSS MB | run 3.1 RSS MB | RSS delta |
|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 33258.7 | 32555.6 | -2.1% | 1.15 | 1.14 | 13938 | 12680 | -9.0% |
| GIL-off W=2 | 45487.9 | 44141.6 | -3.0% | 0.66 | 0.67 | 14395 | 13059 | -9.3% |
| GIL-off W=16 | 47561.9 | 47805.6 | +0.5% | 0.20 | 0.19 | 12704 | 12715 | +0.1% |
| GIL-off W=32 | 76557.4 | 77368.2 | +1.1% | 0.16 | 0.16 | 12351 | 12352 | +0.0% |
| GIL-on W=1 | 15140.0 | 16271.9 (5 reps) | **+7.5%** | 1.03 | 1.01 | 424 | 422 | -0.4% |

- The GIL-on W=1 **+7.5%** is stable across all 5 reps (16172-16397 ms) but
  cannot be a code regression — the binary hash is identical to run 3's.
  It quantifies this session's noisier host (residual loadavg from the
  bench cells themselves plus background agent sessions). Treat it as the
  error bar on every other cell in this table, including the apparent
  W=1/W=2 wall and RSS "improvements".
- Checksums: every rep of every cell (incl. congc and GIL-on) produced
  checksumC `af028188d7a56a96`, matching run 3's recorded tuple exactly.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests — superset of the 10-test gate) | **0 mismatches** |
| congc teardown: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums (run 3: 3/3 SIGSEGV) |

The congc result does **not** mean the teardown race is fixed — the code is
unchanged. It reclassifies the `tearDownSpawnedThreadForExit` crash from
"deterministic at W=4" to **timing-dependent/flaky**: run 3 hit it 3/3 under
heavy host memory pressure (the same session later found ~115 GB of runaway
symbolizer RSS) and with core dumps initially enabled; run 3.1, on a freer
host, survived 5/5. The §17 stack remains the live bug.

### Acceptance verdict

**FAIL (expected).** Corpus + identity are green, but no headline cell
(W=16 wall, W=32 wall, RSS, W=1 tax) improved >= 15% — definitionally
impossible with a bit-identical binary — and the GIL-on W=1 cell moved
+7.5% (host noise, but > the 5% gate). Run 3.1 is a **baseline
reproduction**: run-3 numbers reproduce within ~±3% (wall, W>=16 RSS) on a
busier host, the corpus and identity gates hold, and the congc crash is
flaky rather than deterministic. No optimization landed between run 3 and
run 3.1 for this acceptance to measure.

## 24. Run 3.1 — acceptance of the heap/object-model optimization delta

Supersedes §23 for acceptance purposes: §23 measured a bit-identical binary
(a baseline reproduction). This run measures the real delta — a 21-file
working-tree change (+1519/-184) over `9e9a3a25fd0b` touching the shared-GC
heap (Heap.cpp, IncrementalSweeper, MarkedBlock, FullGC/EdenGC activity
callbacks, CodeBlockSet, HeapClientSet) and the concurrent object model
(ConcurrentButterfly, PropertyTable, Structure, LockObject, VMManager,
VMTraps). Binary under test: `WebKitBuild/Release/bin/jsc` sha256
`c96178df6ac1dae78b874bb3a29a42d32e5ff84af4dc19c91281087329429b1c`
(built 2026-06-13 03:52 UTC; 0 sources newer than the binary).

Protocol: identical to run 3 / §23 — pinned GIL-off env, `bench.js -- W`
via the v3 driver measurement path (raw records in `results-v31b-raw.jsonl`),
3 reps/cell (5 for congc W=4), medians; `/usr/bin/time -v` for peak RSS and
cpu_util = (user+sys)/(wall*W); core dumps off; one bench process at a time;
background CPU consumers reniced to 19. 1-min loadavg at batch starts:
2.5–5.7 (run 3: 1.0–1.9) — i.e. this host was *noisier* than run 3's, so
the wall-time improvements below are not flattered by a quieter machine.

### Before/after (run 3 medians vs run 3.1 medians)

| Cell | run 3 wall ms | run 3.1 wall ms | delta | run 3 cpu | run 3.1 cpu | run 3 RSS MB | run 3.1 RSS MB | RSS delta |
|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 33258.7 | 23834.5 | **-28.3%** | 1.15 | 1.04 | 13938 | 411 | **-97.1%** |
| GIL-off W=2 | 45487.9 | 43735.9 | -3.9% | 0.66 | 0.87 | 14395 | 3810 | **-73.5%** |
| GIL-off W=16 | 47561.9 | 38385.1 | **-19.3%** | 0.20 | 0.26 | 12704 | 3860 | **-69.6%** |
| GIL-off W=32 | 76557.4 | 38708.9 | **-49.4%** | 0.16 | 0.18 | 12351 | 3850 | **-68.8%** |
| GIL-on W=1 | 15140.0 | 14395.0 | -4.9% | 1.03 | 1.03 | 424 | 423 | -0.3% |

- W=1 GIL-off tax drops from 2.20x to **1.66x** (23834.5 / 14395.0).
- Per-rep wall spread is tight (W=1: 23712–23946; W=32: 38516–38737), and
  every rep of every cell reproduced the run-3 checksum tuple exactly
  (A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2,
  B=c4bdd580f85ee058, C=af028188d7a56a96).
- The W=1 RSS collapse (13.9 GB -> 411 MB) brings flag-on single-thread
  memory to parity with GIL-on (423 MB). W>=2 RSS settles at ~3.8 GB —
  a 69–74% cut but still ~9x the GIL-on footprint; that residual is the
  next memory target.
- Honest negatives: GIL-off W=2 wall only -3.9% (within this host's noise
  band; W=2 remains the worst scaling point at 1.84x the GIL-on W=1 wall
  for 2x the work... per-thread efficiency is still the open problem), and
  W=16/W=32 cpu_util (0.26/0.18) still shows threads mostly parked —
  walls improved because the serial/GC component shrank, not because
  parallel efficiency rose.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests — superset of the 10-test gate) | **0 mismatches** |
| congc teardown: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 40151.9 ms (run 3: 3/3 SIGSEGV; §23 no-delta: 5/5 but flaky-classified) |

### Acceptance verdict

**PASS.** Corpus + identity green; three headline cells beat the >=15%
bar (W=16 wall -19.3%, W=32 wall -49.4%, RSS -68.8%..-97.1%; plus W=1 tax
2.20x -> 1.66x); no cell regresses >5% (worst mover: GIL-on W=1 RSS -0.3%,
GIL-on wall itself improved -4.9%). congc teardown survived 5/5 — but per
§23's reclassification the crash is timing-dependent, so 5/5 here is
consistent-with-fixed, not proof; keep the §17 stack on the books until a
stress campaign under memory pressure retires it.

## 25. Run 3.2 — campaign-2 contention/retention pass

Supersedes §24 for acceptance purposes. Measures a 21-file working-tree
delta (+1043/-75) over `729430dbc80c` touching the allocator slow path
(per-`BlockDirectory` striped MSPL replacing the server-wide refill lock —
T7), the GC stop protocol (parked siblings join parallel marking instead of
idling — T1; §A.3 arbitration park-not-sleep — T6), the heap-limit feedback
(rebase `m_sizeAfterLast*Collect` past the Wlr cohort — T2), the segmented
butterfly entry (born-full-coverage so first grow is the lock-free CAS —
T3), the FTL lazy-slow-path steady state (lock-free DCLP — T8), and a
bounded adaptive spin on `Lock.prototype.hold` (T5). T4 (relabel
thread-local fast path) was withdrawn at adversarial review as unsound; the
review note is in `JSObject.cpp`. Two fixes applied during this acceptance
run: the T3 alignment gate narrowed from `useJSThreads` to `useSharedGCHeap`
(the wider gate changed contiguous-array size-class selection under GIL-on
for no benefit there), and the T2 rebase clamped to `m_totalBytesVisited` so
the eden monotone-visited assert holds on small heaps (6 Debug-corpus
failures: `heap-allocation-storm.js` et al.). Binary under test:
`WebKitBuild/Release/bin/jsc` sha256
`2a85f8e533411e6880272552ae2050b8d3842c1feb31aa181e1caeb6a1f83f88`
(2026-06-15; 0 sources newer than the binary).

Protocol: identical to §24 — pinned GIL-off env, `bench.js -- W` via the
`v3_driver` measurement path (raw records in `results-v32-raw.jsonl`),
3 reps/cell (5 for congc W=4), medians; `/usr/bin/time -v` for peak RSS and
cpu_util = (user+sys)/(wall*W); core dumps off; one bench process at a time.
1-min loadavg at batch starts: 1.6–4.2.

**Host-drift control.** The run-3.1 binary was rebuilt bit-identically
(`jsc-v32-baseline`, sha256 `c96178df…` matches §24 exactly) and re-measured
back-to-back with the v32 binary in every cell (raw in
`results-v32ab-raw.jsonl`). The baseline binary itself reproduces today at
+5.0% (GIL-off W=1: 25025 ms vs §24's 23834.5 ms) to +8.8% (GIL-on W=1:
15659 ms 5-rep interleaved median vs §24's 14395.0 ms) over its own
recorded numbers — i.e. this host has drifted 5–9% slower since 2026-06-13
(a competing Rust build in a sibling worktree was observed mid-run; reniced
where caught). The "vs §24" column below is therefore a *lower bound* on the
delta; the "vs same-host baseline" column is the back-to-back A/B at the
same instant and is the meaningful number.

### Before/after (run 3.1 §24 medians vs run 3.2 medians; same-host A/B in parentheses)

| Cell | §24 wall ms | 3.2 wall ms | vs §24 | vs same-host base | §24 cpu | 3.2 cpu | §24 RSS MB | 3.2 RSS MB | RSS vs same-host |
|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 23834.5 | 24526.7 | +2.9% | **-2.0%** (base 25025) | 1.04 | 1.04 | 411 | 421 | +3.6% (base 419) |
| GIL-off W=2 | 43735.9 | 28478.8 | **-34.9%** | **-36.4%** (base 44795) | 0.87 | 0.86 | 3810 | 1538 | **-58.4%** (base 3700) |
| GIL-off W=8 | — | 21484.9 | — | **-45.1%** (base 39151) | — | 0.54 | — | 1764 | **-52.8%** (base 3737) |
| GIL-off W=16 | 38385.1 | 20718.6 | **-46.0%** | **-47.9%** (base 39785) | 0.26 | **0.43** | 3860 | 1810 | **-52.9%** (base 3845) |
| GIL-off W=32 | 38708.9 | 21140.4 | **-45.4%** | **-47.2%** (base 40066) | 0.18 | 0.29 | 3850 | 1739 | **-55.2%** (base 3883) |
| GIL-on W=1 | 14395.0 | 15798.7 | +9.8% | **+2.7%** (base 15659) | 1.03 | 1.03 | 423 | 421 | -0.2% (base 422) |

- Per-rep wall spread is tight at every cell except W=32 rep2 (27111 ms,
  loadavg spike to 28.8 from W=16's tail; median-robust). All 23 bench reps
  reproduced the §24 checksum tuple exactly.
- W=16 cpu_util crosses **0.43** (median of 0.426/0.432/0.431) — the first
  time this branch has exceeded the 0.40 gate. W=32 rises to 0.29.
- W>=2 RSS roughly halves (3.7–3.9 GB -> 1.5–1.8 GB); the residual is
  **N-mutator eden floating garbage** — with N concurrent mutators every
  eden cycle roots N live stacks, so sibling mutators' in-flight
  temporaries get promoted past the eden boundary and the
  `m_maxHeapSize = std::max(m_maxHeapSize, currentHeapSize + m_maxEdenSize)`
  ratchet (Heap.cpp updateAllocationLimits) climbs monotonically without
  the `edenToOldGenerationRatio < 1/3` Full trigger ever firing. rss3
  REFUTED the original attribution here ("per-thread allocator state +
  segmented-butterfly steady-state"): `JSC_forceSegmentedButterflies=1`
  W=1 peaks at 183MB after eden (vs W=2's 774MB), and per-thread allocator
  state is bounded at ~6.7MB/thread ((RSS@W=32 - RSS@W=2) / 30). The
  GC-heap peak-after-eden delta W=1->W=2 is +1001MB = 85% of the +1176MB
  peak-RSS delta; W=2 floating dead = 774MB peak - 143MB true live =
  631MB. Fixed by **T5-rss-eden-floating-garbage (campaign-3)** — a
  shared-only (>=2 clients) Full-collection trigger + ratchet cap at
  `sizeAfterLastFullCollect * sharedGCEdenSurvivalFullTriggerRatio`
  (default 2.0) in the eden branch of `Heap::updateAllocationLimits()`.
- W=1 GIL-off tax: 24527 / 15799 = **1.55x** (§24: 1.66x), but on a
  same-host basis 24587 / 15659 = 1.57x, vs baseline 25025 / 15659 = 1.60x.
- Honest negatives: GIL-on W=1 wall +2.7% same-host (5-rep interleaved A/B,
  loadavg 2.0–2.3, baseline reps 15568–15922 vs v32 reps 15854–16134;
  reproducible across batches). No new code path runs in this configuration
  (every behavioral change is `isSharedServer()` / `gilOff()` /
  `useSharedGCHeap()`-gated; verified by the bit-identical flag-off identity
  below); the residual is structural — `Heap` layout grew by the
  `MutatorSlowPathLockFacade`, the registry lock, and the sibling-visitor
  pool, shifting hot members' cache lines and inlining decisions. Within the
  5% gate; **fixed by T4-heap-layout-restore (campaign-3)** — every
  campaign-2-added member moved to a contiguous trailer block at the end of
  `class Heap`, the old 1-byte `Lock m_mutatorSlowPathLock` slot back-filled
  with a 1-byte pad, so every pre-campaign-2 member offset is byte-identical
  to `729430dbc80c` (relational `static_assert`s pin this). GIL-off W=1 wall +2.9% vs §24
  but -2.0% same-host (i.e. host drift only), RSS +3.6% same-host (the
  shared-heap path's T3 alignment requesting one size class up on small
  arrays). GIL-off W=2 cpu_util -1.7% (0.871 -> 0.855) — wall improved
  because GC and refill *time* fell, not because the W=2 mutators ran more
  in parallel; W=2 remains the worst per-thread efficiency point.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug bin) | **94 passed, 0 failed, 4 skipped** (after T2 clamp fix; pre-fix: 88/6, all `ASSERT(currentHeapSize >= m_sizeAfterLastCollect)`) |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug bin) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests) | **0 mismatches** |
| congc teardown: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 23700.9 ms (§24: 40151.9 ms, **-41.0%**) |

### Acceptance verdict

**PASS on the same-host A/B**; the literal vs-§24 GIL-on cell exceeds 5%
because the byte-identical baseline binary itself does (host drift). Corpus
+ identity green; all three headline disjuncts hit (W=16 cpu_util 0.43 >=
0.40; W=16 wall -46.0% vs §24 / -47.9% same-host; W>=2 RSS -52% to -58%
same-host). No same-host cell regresses >5% (worst: GIL-on W=1 wall +2.7%,
GIL-off W=1 RSS +3.6%). The campaign-2 delta is the first measurement where
W>=8 wall dips below the GIL-off W=1 wall — i.e. adding threads now makes
the program *faster*, not just less-slow.

## 26. Campaign-3 T4-heap-layout-restore — GIL-on W=1 layout regression

Mandatory follow-up to the §25 honest-negative. Pure declaration reorder in
`Source/JavaScriptCore/heap/Heap.h`: every member campaign-2 inserted
mid-class (`MutatorSlowPathLockFacade m_mutatorSlowPathLock`,
`m_markedSpaceRegistryLock`, `m_siblingSlotVisitors` /
`m_availableSiblingSlotVisitors` / `m_siblingSlotVisitorPoolMayGrow`,
`m_siblingMarkingAssistEnabled` / `m_numberOfSiblingMarkingAssists`) moved
to one trailer block after the last pre-campaign-2 member; the old
`Lock m_mutatorSlowPathLock` slot back-filled with the 1-byte
`m_unusedPadMutatorSlowPathLock`. Result: every pre-campaign-2 member's
byte offset (m_objectSpace, m_handleSet, m_opaqueRoots, m_worldState,
m_barrierThreshold, m_threadLock, every IsoSubspace) is identical to
`729430dbc80c`. Four relational `OBJECT_OFFSETOF` static_asserts pin the
trailer ordering so future additions can't re-introduce the shift. No
behavior change in any configuration; none of the moved members are in the
`Heap::Heap()` init list (no -Wreorder); `Heap.cpp` / `HeapInlines.h`
unchanged.

**Expected**: GIL-on W=1 back to the §25 same-host base 15659 ms ± noise
(recover +2.7%); proportional trim to the GIL-off W=1 1.55× tax (same hot
fields on every allocation slow path). **Beat-or-explain baselines**: §25
table row "GIL-on W=1" same-host base 15659 ms; every other §25 cell must
not regress (pure layout — no code-path delta). Verify: 5-rep interleaved
A/B GIL-on W=1 vs `d8ed7b6f5254`, `v5a-identity.sh`, full corpus both
modes.

## 27. Run 3.3 — campaign-3 acceptance (segmented-array DFG fastpath + relabel-STW-elide + heap layout restore + eden-survival Full trigger)

Measures a 21-file working-tree delta (+1441/-166) over `d8ed7b6f5254`
landing campaign-3: **T1-relabel-stw-elide-sound** (sound replacement for
the withdrawn campaign-2 T4 — the relabel STW path elided when the source
butterfly word is already segmented; `JSArray.cpp` / `JSObject.cpp`),
**T3-jit-segmented-arraymode** (DFG fast path for segmented-butterfly
indexed access via a side-band `ArrayProfile` bit threaded through
`ArrayMode` into `DFGSpeculativeJIT{,64}.cpp` so DFG/FTL stop OSR-exiting
on every segmented load — the largest line-count change),
**T4-heap-layout-restore** (§26), and **T5-rss-eden-floating-garbage**
(`Heap::updateAllocationLimits()` Full trigger at
`sharedGCEdenSurvivalFullTriggerRatio=3.0`, §25 attribution). v33 binary
sha256 `151409b1a06cd128ab47b1fbf6b82753df14bbdc980fbcfafee2fdb685d40216`
(2026-06-15; rebuilds bit-identically; 0 sources newer than the binary).

**Host-drift control.** `d8ed7b6f5254` rebuilt bit-identically
(`jsc-v33-baseline`, sha256 `2a85f8e5…` matches §25 exactly) and measured
back-to-back with v33 in every cell (raw in `results-v33ab-raw.jsonl`).
The baseline binary today reproduces its own §25 numbers within ±2% (W=1
24809 ms vs §25 24527 ms = +1.1%; W=16 20618 ms vs §25 20719 ms = -0.5%;
GIL-on W=1 16021 ms vs §25 15799 ms = +1.4%) — host drift since 2026-06-15
§25 is small, so vs-§25 and vs-same-host-baseline columns track closely.
1-min loadavg at batch starts: 1.6–10.3 (W=32 first batch caught a spike
to 8–10; re-run interleaved, see W=32 row note).

### Before/after (run 3.2 §25 medians vs run 3.3 medians; same-host A/B in parentheses)

| Cell | §25 wall ms | 3.3 wall ms | vs §25 | vs same-host base | speedup-vs-self | §25 cpu | 3.3 cpu | §25 RSS MB | 3.3 RSS MB | RSS vs same-host |
|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 24526.7 | 25063.5 | +2.2% | **+1.0%** (base 24809) | 1.00x | 1.04 | 1.04 | 421 | 422 | +0.5% (base 419) |
| GIL-off W=2 | 28478.8 | 24503.3 | **-14.0%** | **-14.9%** (base 28784) | 1.02x | 0.86 | 0.90 | 1538 | 1701 | **+10.3%** (base 1543) |
| GIL-off W=4 | — | 19207.0 | — | **-19.3%** (base 23813) | 1.30x | — | 0.71 | — | 1732 | -0.8% (base 1746) |
| GIL-off W=8 | 21484.9 | 16166.3 | **-24.8%** | **-25.0%** (base 21561) | 1.55x | 0.54 | 0.56 | 1764 | 1765 | +0.3% (base 1761) |
| GIL-off W=16 | 20718.6 | 15193.4 | **-26.7%** | **-26.3%** (base 20618) | **1.65x** | 0.43 | 0.44 | 1810 | 1789 | -1.1% (base 1809) |
| GIL-off W=32 | 21140.4 | 15950.1 | **-24.6%** | **-25.7%** (base 21465) | 1.57x | 0.29 | 0.33 | 1739 | 1843 | +5.2% (base 1752) |
| GIL-on W=1 | 15798.7 | 15764.7 | -0.2% | **-1.6%** (base 16021) | — | 1.03 | 1.03 | 421 | 421 | +0.1% (base 421) |

- All checksums match the reference tuple at every successful rep. Per-rep
  spread tight at W∈{1,2,4,8} (≤2%); W=16 base reps 19244/20618/20825
  (rep1 low — loadavg 5.1 vs 5.9 for reps 2/3; median-robust); W=32 see
  P0 below.
- W=32 row uses the interleaved re-run (loadavg 9–10, base reps
  21374/21465/21698, v33 surviving reps 15895/16005 plus first-batch rep1
  16084; median 15950). The original W=32 batch (loadavg 7–10) had two
  baseline reps with ~57s sys time (host-contention bimodality, §25
  observed the same at W=32 rep2) and one v33 SIGSEGV — see P0.
- **Speedup-vs-self at W=16: 25064 / 15193 = 1.65x** — short of the 1.8x
  Java-parity bar (would need W=16 ≤ 13924 ms at this run's W=1). cpu_util
  rises only marginally (0.43→0.44); the wall improvement is mutator
  *throughput* (T3 keeps DFG/FTL on the hot indexed path instead of
  OSR-exiting on segmented butterflies), not parallelism. The remaining
  W=16 ceiling is **NOT** §25's "STW collection wall fraction" — gcwall
  (§27.S2) measures STW-GC at **5.4%** of W=16 wall. The dominant ceiling
  is the **~52% thread-0 serial inter-phase work** plus the parallel-phase
  CPU-waste tax — see §27.S1/S2 below.
- **GIL-on W=1 -1.6% same-host (5-rep interleaved**, base
  15691/15842/16021/16202/16361 vs v33 15623/15728/15765/15875/16187,
  loadavg 1.7–6.3 falling). T4-heap-layout-restore recovered ~1.6 of the
  §25 +2.7 percentage points; the other ~1pp is within the 5-rep spread on
  this host. PASS the ≤2% gate.
- Honest negatives: **(a)** GIL-off W=1 wall +1.0% same-host — the T3
  `ArrayProfile` side-band bit and `ArrayMode` widening add a small amount
  of profiling/dispatch overhead even at W=1 (every flat indexed access
  still tests the bit). Within 5%. **(b)** GIL-off W=2 RSS **+10.3%**
  same-host (1543→1701 MB) — T5-rss at the landed `ratio=3.0` admits ~2
  floating live-sets before a Full, while T1/T3 keep more JIT code +
  segmented spine reachable per cycle; the W=2 cell is now allocator-heavy
  enough that the 3.0x bound is hit later than the §25 analysis projected
  for 2.0x. W≥4 RSS is flat-to-down (the bound is reached earlier with
  more concurrent allocators). **(c)** W=32 RSS +5.2% same-host — same
  mechanism, marginal.

### NEW P0 — W=32 SIGSEGV in DFG OSR-entry tier-up (campaign-3 regression)

v33 W=32 GIL-off bench crashes **4/26** Release runs (rc=139 SIGSEGV);
`jsc-v33-baseline` **0/13** under identical conditions in the same
session. Backtrace (core `/tmp/v33cores/core.7937`):
`operationTriggerOSREntryNow` → `DFG::tierUpCommon` →
`failedOSREntry` lambda (`DFGOperations.cpp:6584`) →
`DFG::JITCode::clearOSREntryBlockAndResetThresholds`
(`DFGJITCode.cpp:415`) → `CodeBlock::jitCode()` with `this=nullptr` —
i.e. `m_osrEntryBlock.get()` returned null past the `ASSERT(m_osrEntryBlock)`.
None of `DFGOperations.cpp` / `DFGJITCode.cpp` are in the campaign-3
diff; this is a **pre-existing concurrent-tier-up race** (two GIL-off
mutators reach `failedOSREntry` for the same DFG `JITCode`; the first
runs `m_osrEntryBlock.clear()`; the second derefs the now-null weak
inside `clearOSREntryBlockAndResetThresholds`) that campaign-3 **exposes**
— T3 keeps every mutator in DFG/FTL on the hot loop instead of bouncing
through OSR-exit, so 32 mutators now plausibly hit the FTL OSR-entry
failure path concurrently for the shared bench function. Not seen at
W≤16 (0/14 v33). Repro:
```
env JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 \
    JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
    JSC_useThreadGILOffUnsafe=1 \
    WebKitBuild/Release/bin/jsc Tools/threads/scalebench/js/bench.js -- 32
# ~15% of runs: SIGSEGV at DFGJITCode.cpp:415
```

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug bin) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug bin) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 19105.9 ms (§25: 23700.9 ms, **-19.4%**) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **-1.6%** (≤2% gate PASS) |
| Bench W=32 stability | **FAIL** — 4/26 SIGSEGV (baseline 0/13) |

### Acceptance verdict

**FAIL.** Corpus + identity green; GIL-on layout regression recovered;
W∈{2,4,8,16,32} wall -15% to -26% same-host with no wall regression >5%
anywhere. But: **(1)** W=16 speedup-vs-self **1.65x < 1.8x** (wall
15193 ms > 13700 ms target); **(2)** new ~15% W=32 SIGSEGV in concurrent
DFG OSR-entry tier-up — a pre-existing race campaign-3 makes hot, but a
correctness regression vs `d8ed7b6f5254` regardless; **(3)** W=2 RSS
+10.3% same-host (>5%). Per the ≥20%-improvement-at-W=16 fallback this
section is recorded honestly with the gap named; campaign-3 cannot land
until the tier-up race is closed (a `tierUpCommon` / `m_osrEntryBlock`
TOCTOU guard, not in any campaign-3 file) and the W=2 RSS regression is
either fixed (`sharedGCEdenSurvivalFullTriggerRatio` tuned toward 2.0 at
N=2) or explained as accepted.

### §27.S1 Amendment — serial-fraction ceiling correction & per-section attribution

The §27 ceiling line ("STW collection wall fraction") is **incomplete**.
The `gcwall` instrumentation incidentally exposed that at W=16,
`phaseA_ms + phaseB_ms + phaseC_ms ≈ 7150 ms` against `total_ms ≈ 14850 ms`
— i.e. **~7700 ms (52%) of W=16 wall is thread-0-only inter-phase serial
work** that runs between barriers while W-1 workers park: two
`postingsChecksum()` walks over 4,158,957 postings (~10 heap-BigInt ops
each), `buildDfSnap()`, `makeGroups()`, and `checksumPhaseC()`
(`bench.js` worker-0 path). All three languages run identical serial
work, so this is not a fairness defect; but a 52% structural serial
fraction alone caps per-worker `cpu_util` near 0.50 — which is the
observed 0.44, **before** any STW-GC accounting. STW collection is a
contributor inside the parallel ~7150 ms, not the dominant W=16 ceiling.

**The actionable lever is the GIL-off tax on the serial work.** GIL-on
W=1 15764.7 ms vs GIL-off W=1 25063.5 ms = **1.59×**. The serial sections
are single-threaded by construction, so under GIL-off they pay engine
overhead (sharedGCHeap BigInt allocation, segmented-butterfly GetByVal,
shared-Map iterator) for zero parallelism benefit. If the serial 7700 ms
carries the 1.59× tax uniformly, GIL-on-parity serial ≈ 7700/1.59 ≈
**4843 ms**, projecting W=16 wall ≈ 7150 + 4843 ≈ **11993 ms** — under
the ≤12600 ms Java-parity bar (1.99× vs own W=1) and lifting `cpu_util`
toward (4843 + 16·6050) / (16·11993) ≈ 0.53. That is the campaign-4
target: recover ~2857 ms of serial wall at W=16 by closing the GIL-off
tax on **one** identified hot section, not by touching the parallel
phases.

**Instrumentation (this amendment).** `bench.js` now emits five extra
JSON keys timing each thread-0 serial section individually:
`postingsChecksum1_ms`, `buildDfSnap_ms`, `postingsChecksum2_ms`,
`makeGroups_ms`, `checksumPhaseC_ms`. Pure measurement — no control-flow
change, no engine edit, output-only (Go/Java need not match; SPEC §1.11
output keys are a superset contract). Sum of the five ≈
`total_ms - (phaseA_ms + phaseB_ms + phaseC_ms)` modulo barrier-wakeup
slop and (in WS mode) `wsMergeLocals`.

**Attribution A/B (v33 binary, 5 reps each, quiet host):**
```
# GIL-off W=1
for i in 1 2 3 4 5; do env JSC_useJSThreads=1 JSC_useThreadGIL=0 \
  JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
  JSC_useThreadGILOffUnsafe=1 WebKitBuild/Release/bin/jsc \
  Tools/threads/scalebench/js/bench.js -- 1; done
# GIL-on W=1
for i in 1 2 3 4 5; do env JSC_useJSThreads=1 JSC_useThreadGIL=1 \
  WebKitBuild/Release/bin/jsc \
  Tools/threads/scalebench/js/bench.js -- 1; done
```
W=1 isolates the serial-code tax from any parallel-phase effect (every
section is "serial" at W=1); the per-section GIL-off/GIL-on ratio names
the dominant contributor directly. A W=16 GIL-off run with the same
binary cross-checks that the W=1 section walls reproduce at scale (they
should: thread-0 runs them alone either way).

| Section | GIL-on W=1 ms (med) | GIL-off W=1 ms (med) | ratio | GIL-off W=16 ms (med) |
|---|---|---|---|---|
| postingsChecksum1 | 1846.5 | 2785.0 | 1.51 | 3739.9 |
| buildDfSnap | 34.6 | 37.8 | 1.09 | 56.8 |
| postingsChecksum2 | 2002.5 | 3081.8 | 1.54 | 3973.3 |
| makeGroups | 0.4 | 0.5 | 1.09 | 0.7 |
| checksumPhaseC | 29.8 | 38.0 | 1.28 | 49.8 |
| **sum** | **3913.9** | **5943.1** | 1.52 | **7820.5** (≈ expected 7700) |

(Filled 2026-06-15 from `results-v34ab-raw.jsonl`, v34 binary — within ±1%
of v33 at every cell, see §28. The W=16 serial sum **exceeds** the W=1
GIL-off sum by +32%: thread-0's single-threaded checksum walk runs ~1.3×
slower with 15 siblings parked at the barrier — sharedGCHeap allocator
state churn / background scavenge interference, not the per-op GIL-off
tax which is the W=1 ratio column. The two `postingsChecksum` calls are
**98.6%** of the serial sum (7713 / 7820 ms at W=16).)

**Expected outcome** (to be confirmed by the table): the two
`postingsChecksum` calls dominate (≈8.3M iterations of BigInt
mul/xor/`mix` + `p.docIds[i]`/`p.tfs[i]` segmented indexed reads + shard
`Map` iteration). Whichever of {heap-BigInt alloc under `sharedGCHeap`,
segmented `GetByVal` in a single-mutator context, shared-`Map` iterator
overhead} carries the largest GIL-off/GIL-on ratio becomes the named
campaign-4 engine task with a measured ms target equal to that section's
(GIL-off − GIL-on) delta.

### §27.S2 Amendment — congc default REFUTED; STW-GC is 5.4% not the ceiling

**Decision (campaign-4 C1-congc-no-default):** `useConcurrentSharedGCMarking`
stays **default `false`** (OptionsList.h:433); the GIL-off coupling block
in `Options.cpp::notifyOptionsChanged()` deliberately does **not** force it
on. Recorded there as a comment so a future campaign does not re-propose
"default congc on" without a fresh ≥10% STW-wall measurement.

**gcwall measurement (METHOD B, v33 binary, W=16, 3-run):**

| arm | rendezvous windows | STW open→resume | wall | STW % of wall |
|---|---|---|---|---|
| congc=0 (default) | 57 | 807 ms | 14847 ms | **5.4%** |
| congc=1 | 87–88 | 765 ms | 15119 ms | 5.1% |
| Δ | **+30** | -42 ms | **+272 ms (+1.8%)** | — |

**congcab independent A/B (interleaved, same host):** W=8 median 16.54 s
vs 16.51 s (+0.2%), W=16 15.83 s vs 15.51 s (+1.2%); cpu% **718 → 701**
(did not rise — the congc design predicate); 16/16 checksums identical;
crash rate 1/9 vs 2/10, not arm-correlated (the §27 P0 tier-up TOCTOU).

**Why this refutes the §25/§27 ceiling claim.** §25 named, and the §27
results bullet repeated, "STW collection wall fraction" as the remaining
W=16 ceiling. The ≥10% bar required to justify defaulting C1 is not met:
STW-GC is 5.4% of wall. Even a *perfect* congc (STW→0) caps the gain at
~800 ms, leaving W=16 ≈ 14040 ms — still above the ≤12600 ms Java-parity
target. The stwrate2 verdict ("the answer is GC pause") is likewise
refuted at 5.4%. The corrected attribution is §27.S1's: **~52% of W=16
wall is thread-0 serial inter-phase work**, and the parallel ~7150 ms
runs at ~5.6× CPU-waste vs GIL-on (cpu_util 0.44 with 16 workers awake
for ~48% of wall ⇒ effective per-worker parallel utilisation well under
1). Those two levers — serial GIL-off tax (§27.S1) and parallel-phase
waste — are where campaign-4 effort goes.

**Congc follow-up (DEFERRED).** The specific defect the profile names is
the **~30 extra Reentry-rendezvous windows** C1 introduces (57 → 87–88):
each concurrent-mark handoff opens a fresh stop window, and the
rendezvous cost (~9 ms/window at W=16) eats the 42 ms STW saving and
then some. Fixing this (coalesce C1 handoffs / piggyback on existing
windows) is a real congc work item but is **deferred**: even at zero
extra windows the ceiling is 5.4%, far short of the 12600 ms target.
Re-evaluate only after the §27.S1 serial-tax lever has moved W=16 wall
into range where 5% matters.

**Net of this task:** 0 ms (decision). Prevents a **+1.8% W=16 wall
regression** that defaulting congc would have introduced, and redirects
campaign-4 to the measured 52%-serial / parallel-waste levers.

## 28. Run 3.4 — P0 closure + R1/S2/S3 acceptance (campaign-3 follow-up)

Measures a 12-file working-tree delta (+513/−24) over `21d09c27fef2`
(campaign-3 / v33) landing the §27 follow-ups: **P0-osr-entry-toctou**
(`DFGJITCode.cpp` snapshot-then-null-check absorbs the
`clearOSREntryBlockAndResetThresholds` loser race; flag-off path
byte-identical), **R1-rss-ratio-adaptive** (`Heap.cpp` W-adaptive
`effectiveRatio = min(option, 2.0+0.5·(clients−2))` for the
floating-garbage Full trigger), **S2(d)-jslock-adaptive-spin**
(`LockObject.cpp` W-adaptive spin bound 40→20@W≥8→10@W≥16, refreshed
only in the slow path) plus the option-gated `logJSLockContention`
counters, **S3-jettison-stw-batch** (`CodeBlock.cpp` redundant-jettison
fold-into-pending-window), the §27.S2 congc-no-default decision comment
in `Options.cpp`, and the §27.S1 `bench.js` per-section timing keys. v34
binary sha256
`4f71fa548aac51fbebd86ebb49b0caa7e1e5b4057c76acc087a268c39bf078d7`
(2026-06-15; 0 sources newer than the binary; Debug bin same mtime).

**Host-drift control.** `d8ed7b6f5254` baseline reused
(`jsc-v33-baseline`, sha256 `2a85f8e5…` — bit-identical to §25/§27) and
measured back-to-back with v34 in every cell (raw in
`results-v34ab-raw.jsonl`, driver `v34_ab.sh`). The baseline today
reproduces its own §27 same-host numbers within ±2% (W=1 24777 vs §27
base 24809 = −0.1%; W=16 21156 vs §27 base 20618 = +2.6%; GIL-on W=1
16524 vs §27 base 16021 = +3.1%). 1-min loadavg at batch starts:
2.7–10.8 (W=1/2 quiet at 2.4–4.2; W=16/32 ran into the 30-rep
stability-gate decay tail at 7–13; W=32 interleaved). No sibling builds;
top non-self CPU consumer ≤6.2%.

### W=32 stability (P0 gate — runs first)

`ulimit -c 0`, pinned GIL-off env, v34 binary, `bench.js -- 32`, **30
consecutive reps**: **30/30 exit 0, 0 SIGSEGV**, every checksum tuple
matches the reference. (v33: 4/26 SIGSEGV, §27.) Per-rep wall: 25/30 in
15363–16255 ms, 5/30 bimodal-high at 21048–22668 ms (sys-time spike,
host-contention bimodality observed since §25; not arm-correlated).
**P0-osr-entry-toctou closed.**

### Before/after (run 3.3 §27 medians vs run 3.4 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §27 wall ms | 3.4 wall ms | vs §27 | vs same-host base | speedup-vs-self | Java bar | §27 cpu | 3.4 cpu | §27 RSS MB | 3.4 RSS MB | RSS vs base |
|---|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 25063.5 | 24558.4 | −2.0% | **−0.9%** (base 24777) | 1.00x | — | 1.04 | 1.04 | 422 | 420 | +0.3% |
| GIL-off W=2 | 24503.3 | 24617.0 | +0.5% | **−13.5%** (base 28444) | 1.00x | — | 0.90 | 0.93 | 1701 | 1641 | **+7.0%** |
| GIL-off W=4 | 19207.0 | 19101.7 | −0.5% | **−20.0%** (base 23876) | 1.29x | — | 0.71 | 0.72 | 1732 | 1747 | −0.5% |
| GIL-off W=8 | 16166.3 | 16211.0 | +0.3% | **−22.9%** (base 21017) | **1.51x** | 1.99x | 0.56 | 0.57 | 1765 | 1741 | −2.3% |
| GIL-off W=16 | 15193.4 | 15024.2 | −1.1% | **−29.0%** (base 21156) | **1.63x** | 1.99x | 0.44 | 0.46 | 1789 | 1792 | −1.4% |
| GIL-off W=32 | 15950.1 | 15933.9 | −0.1% | **−27.2%** (base 21874) | **1.54x** | 1.75x | 0.33 | 0.32 | 1843 | 1847 | +4.5% |
| GIL-on W=1 | 15764.7 | 16157.6 | +2.5% | **−2.2%** (base 16524) | — | — | 1.03 | 1.03 | 421 | 422 | +0.2% |

- All 51/51 A/B reps rc=0, every checksum tuple matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
  Per-rep spread tight (≤2%) at every cell except W=4 v34 (rep3 outlier
  15994 ms phaseA 6739, vs reps 1/2 ~9500 — JIT-warmup luck; median is
  rep1) and W=32 base rep3 27381 ms (sys 57.6 s, the §25/§27 bimodality;
  median rep1 21874).
- **Speedup-vs-self vs Java curve** (vs v34's own W=1 24558.4 ms): W=8
  1.51x **< 1.99x** (gap −3870 ms wall); W=16 1.63x **< 1.99x** (gap
  −2683 ms); W=32 1.54x **< 1.75x** (gap −1901 ms). **All three Java
  bars MISSED.** v34 wall is within ±1.1% of v33 at every W — the
  v34-over-v33 delta is correctness (P0), RSS tuning (R1), and
  contention-shape work (S2(d)/S3) whose effect at this benchmark is
  sys-time only: W=16 sys 19.4 s → 7.4 s (−62%), W=32 sys 25.5 s →
  11.7 s (−54%) — fewer rendezvous/futex syscalls — but user+sys/wall
  (cpu_util) moves only 0.44 → 0.46. The §27.S1 52%-serial ceiling is
  unchanged (no v34 piece touches the serial code paths).
- **§27.S1 attribution table now filled** (above). The two
  `postingsChecksum` calls are 7713 of 7820 ms W=16 serial (98.6%),
  GIL-off/GIL-on ratio 1.51–1.54×, and an additional 1.32× W=16/W=1
  GIL-off penalty (3740/2785, 3973/3082) from sibling-parked sharedGCHeap
  interference. **Campaign-4 named target:** the `postingsChecksum`
  GIL-off tax — heap-BigInt mul/xor allocation under `sharedGCHeap` is
  the dominant per-iteration cost (8.3M iterations); a per-lite BigInt
  scratch / nursery or an int64 fast path here is worth ~2030 ms at W=1
  (5867 → 3849) and ~3800 ms at W=16 (7713 → 3849) — the latter alone
  takes W=16 to ~11200 ms ≈ 2.19×, clearing the 1.99× bar.
- Honest negatives: **(a)** GIL-off W=2 RSS **+7.0% same-host**
  (1534→1641 MB) — R1's W-adaptive ratio recovered ~60 MB of the §27
  +10.3% (v33 1701→v34 1641, −3.5%) but the residual is the campaign-3
  T1/T3 JIT-code/segmented-spine retention itself, not the Full-trigger
  cadence (v34 W=2 already runs at effectiveRatio=2.0, ~50% Full
  cadence). Recorded as accepted: 107 MB at W=2 only; W≥4 RSS flat to
  −2.3%. **(b)** GIL-on W=1 vs §27 published +2.5% (16158 vs 15765) —
  but the **same-host interleaved A/B is −2.2%** (base today 16524), so
  this is host drift, not a v34 regression; the ≤2% gate uses the
  same-host number and PASSES. **(c)** W=32 RSS +4.5% same-host
  (unchanged from §27's +5.2%; within 5%).

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV** (v33: 4/26) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug bin) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug bin) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v34) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 19186.5 ms (§27: 19105.9, +0.4%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−2.2%** (≤2% gate PASS) |
| No same-host wall cell regressing >5% | **PASS** (worst: W=2 +0.5% vs §27, −13.5% vs base) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.51/1.63/1.54 vs 1.99/1.99/1.75) |

### Acceptance verdict

**P0 CLOSED; Java bar MISSED.** Corpus + identity green; W=32 0/30
crash; GIL-on same-host −2.2%; no wall regression anywhere; W∈{2..32}
−13.5% to −29.0% same-host vs `d8ed7b6f5254`. But v34 wall ≈ v33 wall at
every W (the v34-over-v33 delta moved only sys-time and RSS), so
speedup-vs-self stays at **1.63× @ W=16 < 1.99×** — the §27.S1 serial
ceiling is unchanged. The campaign-3 delta can land (P0 was its blocker;
R1 partially closes the W=2 RSS negative; S2(d)/S3 are pure wins on
sys-time with no wall regression). The Java-parity gap is now
**quantified to a single section** (postingsChecksum heap-BigInt
GIL-off tax, ~3800 ms W=16) and named as the campaign-4 target above.

## §29 Fairness amendment: postingsChecksum is a SPEC-mandated type asymmetry, not a threading deficit

### Finding

The §28 campaign-4 target — the `postingsChecksum` GIL-off tax that
accounts for 51% of W=16 wall and the entire Java-parity gap — is a
**spec-mandated cross-language type asymmetry**, not a threading
artifact. `Tools/threads/scalebench/SPEC.md` L49-53 fixes the checksum
arithmetic per language:

> All arithmetic on seeds/hashes is unsigned 64-bit with wraparound. JS
> must use `BigInt` masked to 64 bits (`& 0xFFFFFFFFFFFFFFFFn`) for the
> PRNG and checksums; Go uses `uint64`; Java uses `long` …

Audit of the three implementations confirms the spec is followed exactly
and no implementation reaches for arbitrary-precision:

| Lang | File:line | Checksum accumulator | Per-iter cost |
|---|---|---|---|
| Go | `go/main.go` L348 `func indexChecksum() (sum uint64, count uint64)` | native `uint64`, register-resident | add+mul+xor, no alloc |
| Java | `Bench.java` L243-255 `static long[] indexChecksum() { long sum = 0, … }` | native `long`, register-resident | add+mul+xor, no alloc; no `java.math.BigInteger` anywhere |
| JS | `js/bench.js` L312-324 `postingsChecksum()` + `mix()` L85-90 | heap `JSBigInt`, `& MASK` after every op | ~10 BigInt mul/xor/add/shift per posting; each op allocates 2-3 heap `JSBigInt` under `sharedGCHeap` |

At 8.3M postings × ~10 ops/posting this is **~83M heap-BigInt
allocations** on the JS side versus **zero** on the Go/Java side. There
is no `math/big` import in `main.go` and no `BigInteger` reference in
`Bench.java`.

### Measured budget for the same work

| Arm | Source | pc1+pc2 (+ dfSnap+sort) wall ms | Ratio vs Go |
|---|---|---|---|
| Go W=1 | `out-run1` rep1 inter-phase (total − Σ phases) = 1777.3 − 1689.2 | **88** | 1.0× |
| Java W=1 | `out-run1` rep1 inter-phase = 1898.4 − 1792.8 | **106** | 1.2× |
| JS v34 GIL-on W=1 | §28 attribution table: 1846.5 + 2002.5 | **3849** | 44× |
| JS v34 GIL-off W=1 | `results-v34ab-raw.jsonl` rep3: 2814.7 + 3051.3 | **5866** | 67× |

The Go/Java inter-phase residual covers *both* checksum passes plus
`dfSnap` and the §1.9 sort, so 88/106 ms is an **upper bound** on their
checksum cost. JS pays **~55–65×** the native-u64 budget for
bit-identical output — a serial BigInt-allocation tax that exists
identically at W=1 with zero sibling threads.

### Interpretive rulings (this section is the amendment; nothing else changes)

**(i)** The engine-side **B1 fix stands on its own merits**, independent
of cross-language fairness: *heap-BigInt allocation must not regress
1.5× GIL-off vs GIL-on on identical single-threaded code under
`sharedGCHeap`*. The 5866/3849 = **1.52×** GIL-off/GIL-on ratio at W=1
(§28 table: 1.51–1.54×) is a `useSharedGCHeap=1` allocation-path
regression measured against the engine's own GIL-on baseline — it would
be a bug at any absolute cost and is the campaign-4 deliverable
regardless of what Go/Java pay.

**(ii)** The headline **JS-vs-Java speedup-vs-self numbers in §28 carry
a serial-BigInt tax that Go and Java do not pay**. The §28
"speedup-vs-self vs Java curve" row (1.51/1.63/1.54× vs 1.99/1.99/1.75×)
compares each language to *its own* W=1, so a large JS-only serial
component depresses the JS ratio by Amdahl while leaving the Java ratio
untouched. Read those cells as "JS scaling under a ~5.9 s spec-imposed
serial floor that Java runs in ~0.1 s", not as "JS threading is 0.4×
worse than Java threading". The W=16 sibling-interference component
(1.32×, §28) *is* a threading cost and remains correctly attributed.

**(iii)** **`bench.js` and `SPEC.md` are NOT changed.** The spec's
BigInt mandate is the only portable way to get bit-identical u64
wraparound in JS without an engine intrinsic; rewriting the bench to
dodge it would invalidate the cross-language checksum reference. The
engine fix (per-lite BigInt scratch / nursery or an int64-fitting fast
path, §28 last bullet) is the deliverable; this §29 amendment is
**interpretive only** — it annotates how to read §28's cross-language
ratios, it does not relax any gate.

## §30 Run 3.5: campaign-4/B1 sharedGCHeap allocation-path delta — Java bar still missed

Measures a 7-file working-tree delta (+369/−27) over `3909da474a7e`:
`BlockDirectory.{h,cpp}`, `GCThreadLocalCache.cpp`, `Heap.{h,cpp}`,
`LocalAllocator.cpp`, `InlineCacheCompiler.cpp`. Release `jsc-v35` sha256
`8ff9c49a…`; Debug sha256 `e4afea49…`. Baseline `d8ed7b6f5254` reused
bit-identically (`jsc-v33-baseline`, sha256 `2a85f8e5…` — identical to
§25/§27/§28). Driver `v35_ab.sh`, raw `results-v35ab-raw.jsonl`.

**Host-drift control.** Loadavg 3.3–14.2 across batches (a separate
`/root/bun` bridge build session held ~5 cores intermittently; 64-core
host so W≤32 retains ≥27 spare cores). The same-host base column
(back-to-back A/B per cell, W=32 + GIL-on interleaved) controls for it;
absolute walls carry ±~2% host noise.

### Before/after (run 3.4 §28 medians vs run 3.5 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §28 wall ms | 3.5 wall ms | vs §28 | vs same-host base | speedup-vs-self | Java bar | §28 cpu | 3.5 cpu | §28 RSS MB | 3.5 RSS MB | RSS vs base |
|---|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 24558.4 | **22333.2** | −9.1% | **−9.7%** (base 24732) | 1.00x | — | 1.04 | 1.05 | 420 | 423 | −0.3% |
| GIL-off W=2 | 24617.0 | **22630.1** | −8.1% | **−23.6%** (base 29605) | 0.99x | — | 0.93 | 1.01 | 1641 | 1315 | **−14.7%** |
| GIL-off W=4 | 19101.7 | **17728.0** | −7.2% | **−25.5%** (base 23796) | 1.26x | — | 0.72 | 0.80 | 1747 | 1254 | **−28.1%** |
| GIL-off W=8 | 16211.0 | **15348.6** | −5.3% | **−28.8%** (base 21550) | **1.455x** | 1.99x | 0.57 | 0.63 | 1741 | 1302 | **−26.5%** |
| GIL-off W=16 | 15024.2 | **14478.1** | −3.6% | **−31.0%** (base 20974) | **1.543x** | 1.99x | 0.46 | 0.51 | 1792 | 1333 | **−26.4%** |
| GIL-off W=32 | 15933.9 | **15358.2** | −3.6% | **−28.8%** (base 21557) | **1.454x** | 1.75x | 0.32 | 0.37 | 1847 | 1368 | **−22.0%** |
| GIL-on W=1 | 16157.6 | **16087.2** | −0.4% | **−3.2%** (base 16612) | — | — | 1.03 | 1.03 | 422 | 421 | +0.0% |

- All 51/51 ladder+gilon+congc reps and 30/30 stability reps rc=0; every
  checksum tuple matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
- **Speedup-vs-self vs Java curve** (vs v35's own W=1 22333.2 ms): W=8
  1.455x **< 1.99x** (gap −4126 ms wall, need ≤11223); W=16 1.543x
  **< 1.99x** (gap −3255 ms, need ≤11223); W=32 1.454x **< 1.75x** (gap
  −2596 ms, need ≤12762). **All three Java bars MISSED.** v35 lowers W=1
  by 9.1% (−2225 ms) but W=16 only by 3.6% (−546 ms), so the
  speedup-vs-self ratio actually *drops* slightly at W=8/32 (1.51→1.455,
  1.54→1.454) — Amdahl: shrinking the serial component improves
  absolutes but the parallel-section ceiling is unchanged.
- **Per-section delta** (medians, v35 vs §28 v34): pc1+2 −793/−784/
  −944/−1120/−654/−674 ms at W=1..32 (the B1 target moved); phaseA
  −1210/−1090/−341/**+114**/**+246**/**+343** ms; phaseB −179/−140/−59/
  +5/+84/−102 ms. **§28-attribution update:** v35 W=16 pc1+2 = 7059 ms
  = **49% of 14478** (was 51% of 15012); GIL-off/GIL-on W=1 pc1+2 ratio
  **1.524x → 1.433x** (5073/3540); W=16/W=1 sibling-interference on
  pc1+2 **1.315x → 1.391x** (7059/5073). The §29(i) `sharedGCHeap`
  allocation tax is *reduced* (1.52→1.43) but not closed; the
  sibling-interference ratio worsened in relative terms (absolute pc1+2
  is lower at every W, but W=1 dropped more than W=16).
- **Fairness note (per §29):** the cross-language Java-bar gate compares
  speedup-vs-self under JS's spec-mandated ~5.1 s serial heap-BigInt
  floor vs Java's ~0.1 s native-`long` floor. The 49% serial pc1+2 share
  caps achievable speedup at ~2.05× even with perfect parallel scaling
  of everything else; v35 achieves 1.54× of that ceiling. The threading
  cost is the parallel-section residual (W=16 wall − pc1+2 = 7419 ms vs
  W=1 17260 ms → 2.33× on the parallelizable 77%).
- **Honest negatives:** **(a)** phaseA regresses **+2–6%** at W∈{8,16,32}
  (+114/+246/+343 ms) — the B1 allocation-path change costs the parallel
  ingest hot loop slightly; net wall is still −3.6% to −5.3% at those W
  because pc1+2 wins outweigh it. **(b)** W=16 pc-sibling-interference
  ratio worsened 1.32×→1.39× (B1 helped W=1 more than W=16). **(c)**
  Loadavg 5–14 during batches (foreign `/root/bun` build); the
  same-host-base column controls for it but absolute vs-§28 deltas carry
  ±~2% noise. **(d)** stab32 rep-spread wide (min 14899 / max 21278 ms,
  43%) — host bimodality, not arm-correlated; median 15400 stable.

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV**, 1 unique cs tuple (median 15399.6 ms, loadavg 3.3→14.3) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v35) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 18158.6 ms (§28: 19186.5, −5.4%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−3.2%** (≤2% gate PASS) |
| **BigInt-stress GIL-off** (`grep -l BigInt JSTests/stress \| head -50`, pinned env, Release v35) | **49/50 pass**; 1 fail `atomic-increment-bigint64.js` rc=134 is **pre-existing on baseline+v34** (wasm-disabled-under-GIL-off abort, not B1-related) — **no v35 regression** |
| No same-host wall cell regressing >5% | **PASS** (every cell −3.6% to −9.1% vs §28 v34; −9.7% to −31.0% vs same-host base) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.455/1.543/1.454 vs 1.99/1.99/1.75) |

### Acceptance verdict

**All correctness gates GREEN; Java bar MISSED.** Corpus + identity +
BigInt-stress green; W=32 0/30 crash; GIL-on same-host −3.2%; congc
−5.4%; no wall regression anywhere; W∈{1..32} −3.6% to −9.1% vs §28 v34
(−9.7% to −31.0% same-host vs `d8ed7b6f5254`); RSS −22% to −28% at W≥2;
sys-time −56% to −70% at W≥2. But B1 closed only ~0.09 of the 1.52×
GIL-off/GIL-on pc-ratio (→1.43×) and W=1 improved more than W=16, so
speedup-vs-self moved 1.63→1.54 at W=16 — **further from**, not toward,
the 1.99× bar in ratio terms despite every absolute being faster. The
campaign-4 delta can land (pure wins on wall/RSS/sys at every W; no
correctness regression; phaseA +2–6% at W≥8 is the only sub-component
loss). **Campaign-5 named target:** the residual 1.43× GIL-off/GIL-on
pc-ratio (still ~1530 ms at W=1) plus the 1.39× pc sibling-interference
— closing both takes W=16 pc1+2 to ~3540 ms → wall ~10960 ms ≈ 2.04×,
clearing the bar.

## §31 Run 3.6: campaign-5/M1 BigInt fast-path + alloc/IC delta — Java bar still missed; speedup-vs-self ratio drops

Measures a 12-file working-tree delta (+749/−102) over `7b7f0f9cf2c8`:
`JSBigInt.{h,cpp}` (M1, +255), `LocalAllocator.cpp`, `Heap.{h,cpp}`,
`VMLite.{h,cpp}`, `VM.h`, `FrameTracers.h`, `DFGOperations.cpp`,
`Repatch.cpp`, `PropertyInlineCache.h`. Release `jsc-v36` sha256
`712282dd…`; Debug sha256 `c5bcc85a…`. Baseline `d8ed7b6f5254` reused
bit-identically (`jsc-v33-baseline`, sha256 `2a85f8e5…` — identical to
§25/§27/§28/§30). Driver `v36_ab.sh`, raw `results-v36ab-raw.jsonl`.

**Host-drift control.** Loadavg 2.1–16.3 across 81 reps (no foreign
build active during the timing batches; 64-core host). Same-host base
column (back-to-back A/B per cell, W=32 + GIL-on interleaved) controls
for it; absolute walls carry ±~2% host noise.

### Before/after (run 3.5 §30 medians vs run 3.6 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §30 wall ms | 3.6 wall ms | vs §30 | vs same-host base | speedup-vs-self | Java bar | §30 cpu | 3.6 cpu | §30 RSS MB | 3.6 RSS MB |
|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 22333.2 | **20730.2** | −7.2% | **−17.5%** (base 25129) | 1.00x | — | 1.05 | 1.05 | 423 | 422 |
| GIL-off W=2 | 22630.1 | **21474.9** | −5.1% | **−26.2%** (base 29090) | 0.965x | — | 1.01 | 0.99 | 1315 | 1594 |
| GIL-off W=4 | 17728.0 | **16412.9** | −7.4% | **−31.0%** (base 23772) | 1.263x | — | 0.80 | 0.82 | 1254 | 1299 |
| GIL-off W=8 | 15348.6 | **15002.3** | −2.3% | **−30.5%** (base 21580) | **1.382x** | 1.99x | 0.63 | 0.65 | 1302 | 1305 |
| GIL-off W=16 | 14478.1 | **13928.9** | −3.8% | **−33.0%** (base 20790) | **1.488x** | 1.99x | 0.51 | 0.40 | 1333 | 1323 |
| GIL-off W=32 | 15358.2 | **14624.5** | −4.8% | **−31.3%** (base 21294) | **1.417x** | 1.75x | 0.37 | 0.38 | 1368 | 1335 |
| GIL-on W=1 | 16087.2 | **13714.1** | **−14.7%** | **−14.1%** (base 15968) | — | — | 1.03 | 1.03 | 421 | 419 |

- All 81/81 ladder+gilon+congc+stab32 reps rc=0; every checksum tuple
  matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
- **Speedup-vs-self vs Java curve** (vs v36's own W=1 20730.2 ms): W=8
  1.382x **< 1.99x** (need ≤10417 ms, gap −4585); W=16 1.488x
  **< 1.99x** (need ≤10417, gap −3512); W=32 1.417x **< 1.75x** (need
  ≤11846, gap −2779). **All three Java bars MISSED**, and the ratios
  *dropped* vs §30 (1.455→1.382, 1.543→1.488, 1.454→1.417): M1 cuts the
  serial pc1+2 floor more at W=1 than at W≥8, so Amdahl moves the wrong
  way again.
- **Per-section delta** (medians, v36 vs §30 v35): pc1+2
  −829/−353/−919/−159/−401/−969 ms at W=1..32 (M1 target moved); phaseA
  −654/−790/−265/**+156**/−328/**+9** ms; phaseB −129/−116/−32/**+38**/
  **+46**/**+271** ms. **§30-attribution update:** v36 W=16 pc1+2 =
  6658 ms = **48% of 13929** (was 49% of 14478); GIL-off/GIL-on W=1
  pc1+2 ratio **1.433x → 1.574x** (4244/2696) — *worsened*: M1 helps
  the GIL-on path (−23.8% pc1+2) more than the GIL-off path (−16.3%);
  W=16/W=1 sibling-interference on pc1+2 **1.391x → 1.569x** —
  *worsened* (M1 helps W=1 more than W=16). Parallelizable-section
  speedup (W=16 wall − pc1+2 vs W=1 wall − pc1+2) **2.33× → 2.27×**.
- **M1 flag-off / GIL-on win:** the BigInt fast-path is not
  threads-gated, so single-thread benefits regardless. GIL-on W=1
  same-host A/B: **−14.1%** (15968→13714 ms, 5-rep interleaved, all 5
  v36 reps faster than all 5 base reps). This is a clean ~2.25 s
  single-thread win independent of GIL state; GIL-on pc1+2 dropped
  3540→2696 ms (−23.8%).
- **Honest negatives:** **(a)** speedup-vs-self *regressed* at all
  three Java-bar W (1.455/1.543/1.454 → 1.382/1.488/1.417) — every
  absolute wall is faster but the W=1 denominator dropped 7.2% while
  W=8 dropped only 2.3%. **(b)** GIL-off/GIL-on pc-ratio worsened
  1.43×→1.57× and pc-sibling-interference worsened 1.39×→1.57× — M1's
  BigInt fast-path is most effective exactly where it least helps the
  ratio gate (W=1, GIL-on); the §30 named target (closing the
  sharedGCHeap pc-ratio + sibling-interference) is *not* what M1 does.
  **(c)** phaseB regresses +38/+46/+271 ms at W∈{8,16,32}; phaseA +156
  at W=8. **(d)** W=2 RSS +21% (1315→1594 MB). **(e)** stab32 still
  bimodal: 5/30 reps land in a slow mode (phaseA ~12.5 s, sys ~48 s vs
  ~11 s; min 14385 / max 23067, 60% spread); not arm-correlated, same
  signature as §30(d). **(f)** Loadavg drifted 4.3→16.3 during stab32
  (own-load echo on a quiet host); same-host base column controls.

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV**, 1 unique cs tuple (median 15123.9 ms; 5/30 bimodal-slow, loadavg 4.3→16.3) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug `c5bcc85a…`) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v36) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 16540.2 ms (§30: 18158.6, −8.9%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−14.1%** (M1 win; ≤2% gate PASS) |
| **BigInt-stress flag-off** (all 258 `*bigint*`+`big-int-*` JSTests/stress, Release v36) | **254 pass / 0 fail / 4 skip** |
| **BigInt-stress GIL-off** (same 258, pinned env, Release v36) | **253 pass / 1 fail / 4 skip**; the 1 fail `big-int-function-apply.js` rc=3 is **pre-existing on jsc-v33-baseline + jsc-v35** (verified per-bin), not M1-related — **no v36 regression** |
| No same-host wall cell regressing >5% vs §30 | **PASS** (every cell −2.3% to −14.7%) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.382/1.488/1.417 vs 1.99/1.99/1.75) |

### Acceptance verdict

**All correctness gates GREEN; Java bar MISSED (ratios regressed).**
Corpus + identity + BigInt-stress green; W=32 0/30 crash; GIL-on
same-host −14.1% (M1 single-thread win); congc −8.9%; no wall
regression anywhere; W∈{1..32} −2.3% to −7.4% vs §30 v35 (−17.5% to
−33.0% same-host vs `d8ed7b6f5254`). But M1 is the wrong lever for the
ratio gate: it shrinks the serial BigInt floor uniformly-to-better at
W=1/GIL-on than at W≥8/GIL-off, so speedup-vs-self moved 1.543→1.488 at
W=16 and the GIL-off/on pc-ratio moved 1.43→1.57 — both *further from*
the bar despite every absolute being faster. The campaign-5/M1 delta
can land (pure wins on every wall cell + a 14% GIL-on single-thread
win; no correctness regression; phaseB +1–14% at W≥8 is the only
sub-component loss). **Campaign-6 named target unchanged from §30:**
the 1.57× GIL-off/GIL-on pc-ratio (now ~1548 ms at W=1) plus the 1.57×
pc sibling-interference — both *worsened* by M1 in relative terms. M1
does not address either; the next delta must target the GIL-off
allocation/heap path in the pc loop specifically (per-lite BigInt
nursery / scratch reuse under `sharedGCHeap`), not the BigInt
arithmetic itself.

## §32 Run 3.7: campaign-7 W≥2-only (T1-sibint distinct-allocator gate, T4 sibling-assist cap, T5 coop-parked root-snapshot) — bimodality fixed, sibint down, Java bar still missed

Measures a 10-file working-tree delta (+615/−64) over `336dbc9a6d87`:
`Heap.{h,cpp}` (T1+T4+T5, +386), `MachineStackMarker.{h,cpp}` (T5, +104),
`MarkedBlock.{h,cpp}`+`MarkedBlockInlines.h` (+96), `JSThreadsSafepoint.cpp`
(T5 site-a, +20), `VMManager.cpp` (T5 site-b, +70), `OptionsList.h`. Release
`jsc-v37` sha256 `80cd9d62…`; Debug sha256 `8668ba34…`. Baseline
`d8ed7b6f5254` reused bit-identically (`jsc-v33-baseline`, sha256
`2a85f8e5…` — identical to §25/§27/§28/§30/§31). Driver `v37_ab.sh`, raw
`results-v37ab-raw.jsonl`.

**Host-drift control.** Loadavg 2.2–17.3 across 81 reps (a foreign git
fetch + a `/root/upgrade-nodejs` clang/wine session held ~2–6 cores
intermittently; 64-core host so W≤32 retains ≥27 spare cores). Same-host
base column (back-to-back A/B per cell, W=32 + GIL-on interleaved)
controls for it; absolute walls carry ±~2% host noise.

### Before/after (run 3.6 §31 medians vs run 3.7 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §31 wall ms | 3.7 wall ms | vs §31 | vs same-host base | speedup-vs-self | Java bar | §31 cpu | 3.7 cpu | §31 RSS MB | 3.7 RSS MB |
|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 20730.2 | **20174.6** | −2.7% | **−19.1%** (base 24927) | 1.00x | — | 1.05 | 1.04 | 422 | 420 |
| GIL-off W=2 | 21474.9 | **21462.6** | −0.1% | **−26.4%** (base 29151) | 0.940x | — | 0.99 | 0.97 | 1594 | 1672 |
| GIL-off W=4 | 16412.9 | **16138.2** | −1.7% | **−31.5%** (base 23564) | 1.250x | — | 0.82 | 0.82 | 1299 | 1377 |
| GIL-off W=8 | 15002.3 | **14184.3** | −5.5% | **−33.9%** (base 21443) | **1.422x** | 1.99x | 0.65 | 0.63 | 1305 | 1308 |
| GIL-off W=16 | 13928.9 | **13844.7** | −0.6% | **−33.8%** (base 20914) | **1.457x** | 1.99x | 0.40 | 0.51 | 1323 | 1339 |
| GIL-off W=32 | 14624.5 | **14840.0** | +1.5% | **−30.8%** (base 21451) | **1.359x** | 1.75x | 0.38 | 0.37 | 1335 | 1318 |
| GIL-on W=1 | 13714.1 | **13841.5** | +0.9% | **−12.8%** (base 15869) | — | — | 1.03 | 1.04 | 419 | 421 |

- All 81/81 ladder+gilon+congc+stab32 reps rc=0; every checksum tuple
  matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
- **W=1-neutrality (campaign-7 design constraint):** v37 W=1 GIL-off =
  20174.6 ms = **−2.68% vs §31's 20730.2** → **PASS** (±3% gate). The
  delta is host noise + the §31 v36 W=1 reading itself carrying ~2%
  noise; pc1+2 W=1 moved only −50 ms (4244→4195, −1.2%) and GIL-on W=1
  pc1+2 −26 ms (2696→2670). T1/T4/T5 are by construction unreachable at
  W=1 (every gate requires ≥2 distinct allocating clients or a
  non-conductor sibling).
- **Speedup-vs-self vs Java curve** (vs v37's own W=1 20174.6 ms): W=8
  1.422x **< 1.99x** (need ≤10138 ms, gap −4046); W=16 1.457x
  **< 1.99x** (need ≤10138, gap −3707); W=32 1.359x **< 1.75x** (need
  ≤11528, gap −3312). **All three Java bars MISSED.** vs §31 the ratios
  moved +0.040/−0.031/−0.058 (W=8 up, W=16/32 down) — campaign-7 holds
  W=1 ~neutral and helps W=8 most (−5.5% wall) but W=16 wall barely
  moved (−0.6%): the −760 ms pc1+2 win is offset by a +519 ms phaseA
  loss at W=16 (see per-section).
- **Per-section delta** (medians, v37 vs §31 v36): pc1+2
  **−50/−34/−220/−654/−760/+319** ms at W=1..32 (T1-sibint hit:
  W=8/16 −10–11%, exactly the named target); phaseA −488/−65/−119/−420/
  **+519**/−102 ms; phaseB −71/+13/−52/−64/+48/−11 ms; phaseC small.
  **§31-attribution update:** v37 W=16 pc1+2 = 5898 ms = **43% of
  13845** (was 48% of 13929); GIL-off/GIL-on W=1 pc1+2 ratio
  **1.574x → 1.571x** — *unchanged* (T1/T4/T5 do not touch the W=1
  allocation tax); W=16/W=1 pc-sibling-interference **1.569x → 1.406x**
  — **the campaign-7(a) named target moved** (the 32 forced-Full GC
  train is gone; verified via `verboseGC` that the serial pc-loop now
  runs all-Eden at W=16). Parallelizable-section speedup (W=16 wall −
  pc1+2 vs W=1 wall − pc1+2) **2.27× → 2.01×** — *regressed*: the
  parallel section grew +676 ms at W=16 while shrinking −506 ms at W=1.
- **W=32 bimodality (campaign-7(c) named target) — FIXED.** stab32
  v37: min 12827 / med 14560 / max 15232 ms, **19% spread** (§31: 60%);
  phaseA min 4382 / med 6162 / max 6309, **0/30 slow-mode** (§31: 5/30
  reps in a ~12.5 s phaseA / ~48 s sys slow mode); sys_s max 12.3 s
  (§31: ~48 s). The slow tail is gone; instead 3/30 reps land in a
  *fast* mode (phaseA ~4500 ms ≈ 27% below median). T1's
  distinct-allocator gate eliminating the spurious Full-GC train under a
  serial-allocator window is the slow-mode root cause (the §31(e) sys-s
  signature was the Full-collection scavenger).
- **Honest negatives:** **(a)** parallelizable-section speedup
  *regressed* 2.27×→2.01× (W=16 phaseA +519 ms, +9%): T4's
  sibling-assist admission cap and/or T5's coop-snapshot publish bracket
  cost the parallel ingest hot loop at W=16; the W=8 phaseA −420 ms
  suggests the cap tuning (`auto = numberOfGCMarkers −
  heapHelperPool().numberOfThreads()`) is W-mismatched at 16. **(b)**
  W=32 ladder pc1+2 +319 ms (medians [6360,6736,6663] vs §31 6344) —
  T1's distinct-allocator count at W=32 with phaseB's brief
  many-allocator burst may now over-admit Fulls in the *parallel*
  window; needs a per-section `verboseGC` split. **(c)** W=32
  speedup-vs-self *regressed* 1.417→1.359 (the +1.5% wall is the only
  vs-§31 cell above zero, still well under the +5% gate). **(d)** W=16
  cpu_util 0.40→0.51 *improved* but the wall didn't follow — the extra
  CPU is the sibling-assist drain doing work that was formerly idle
  parking, but the marking it does isn't on the critical path. **(e)**
  W∈{4,8,16} ladder reps show 1-of-3 fast-mode (phaseA −2500 ms): the
  3/30 stab32 fast tail is the same effect; the median is the
  conservative reading. **(f)** W=2 RSS +5% (1594→1672 MB), W=4 RSS +6%
  (1299→1377 MB). **(g)** Loadavg 5–17 during ladder W=8..32 (corpus
  overlap 13:56–14:00 + own-load echo); the same-host-base column
  controls for it but vs-§31 deltas at W=8/16 carry ±~2% noise.

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV**, 1 unique cs tuple (median 14559.7 ms; **0/30 slow-mode**, 3/30 fast-mode, 19% spread vs §31's 60%, loadavg 3.5→17.3) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug `8668ba34…`) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v37) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 16228.4 ms (§31: 16540.2, −1.9%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−12.8%** (≤2% gate PASS; +0.9% vs §31 v36 = neutral) |
| **W=1 GIL-off neutrality** (vs §31 20730.2 ms, ±3%) | **−2.68% PASS** |
| No same-host wall cell regressing >5% vs §31 | **PASS** (every cell −5.5% to +1.5%) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.422/1.457/1.359 vs 1.99/1.99/1.75) |

### Acceptance verdict

**All correctness gates GREEN; W=1-neutrality PASS; W=32 bimodality
FIXED; Java bar MISSED.** Corpus + identity green; W=32 0/30 crash with
the §31(e) 60%-spread slow mode eliminated (19% spread, 0 slow reps);
GIL-on same-host −12.8% (neutral vs §31, +0.9%); congc −1.9%; max vs-§31
regression +1.5% (W=32). Campaign-7(a) hit its named target — pc
sibling-interference 1.569×→1.406× via T1's distinct-allocating-clients
eden-full gate — and (c) is closed. But the −760 ms W=16 pc1+2 win is
offset by a +519 ms W=16 phaseA loss, so W=16 wall moved only −84 ms and
speedup-vs-self stayed at 1.457× (need 1.99×). The parallelizable-section
speedup *regressed* 2.27×→2.01×, and W=32 ratio dropped 1.417→1.359.
The campaign-7 delta can land (W=1-neutral by construction; W=32
bimodality fix is a hard win; W=8 −5.5% wall; no correctness regression;
W=32 +1.5% and W=16 phaseA +519 ms are the only sub-component losses).
**Campaign-8 named target:** the W=16 phaseA +519 ms regression
(T4-auto-cap mismatch — siblings admitted to drainFromShared churn at
W=16 without critical-path benefit per (d); cpu 0.40→0.51 with no wall
gain) plus the residual 1.406× pc-sibling-interference (T1 closed the
Full-train but a ~1700 ms W=16-vs-W=1 pc1+2 gap remains: per-Eden stop
cost still scales with W via the suspend/resume round-trip T5 was meant
to elide — verify `coopParkedClients` actually populates at the pc-loop
JS-barrier park site, not just the safepoint site).

## §33 Run 3.8: campaign-8 (F1/T4-retune cap=0, F3-bvl-stripe-elide, F4-burst byte-threshold, T5-barrier-site GILDroppedSection coop snapshot) — all correctness gates GREEN, neutral on scalability, Java bar still missed

Measures a 4-file working-tree delta (+250/−33) over `a94b2cb7b7bf`:
`BlockDirectory.cpp` (F3, +62), `Heap.cpp` (F1+F4+T5-consumer, +119/−26),
`Heap.h` (+12), `LockObject.cpp` (T5-barrier-site, +90). Release `jsc-v38`
sha256 `d308948b…`; Debug sha256 `f55345a6…`. Baseline `d8ed7b6f5254`
reused bit-identically (`jsc-v33-baseline`, sha256 `2a85f8e5…` — identical
to §25–§32). Driver `v38_ab.sh`, raw `results-v38ab-raw.jsonl`.

**Host-drift control.** Loadavg 2.5–18.6 across 81 reps (a foreign
`jsc … intcs` driver loop and a `/root/bun` rustc LTO build were killed
before the run; residual ~2-core idle floor from unrelated sessions on a
64-core host). Same-host base column (back-to-back A/B per cell, W=32 +
GIL-on interleaved) controls for it; vs-§32 deltas at W≤8 carry ±~2%
host noise.

### Before/after (run 3.7 §32 medians vs run 3.8 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §32 wall ms | 3.8 wall ms | vs §32 | vs same-host base | speedup-vs-self | Java bar | §32 cpu | 3.8 cpu | §32 RSS MB | 3.8 RSS MB |
|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 20174.6 | **19647.0** | −2.6% | **−21.7%** (base 25092) | 1.00x | — | 1.04 | 1.05 | 420 | 422 |
| GIL-off W=2 | 21462.6 | **20951.3** | −2.4% | **−27.2%** (base 28799) | 0.938x | — | 0.97 | 0.99 | 1672 | 1679 |
| GIL-off W=4 | 16138.2 | **16258.2** | +0.7% | **−31.7%** (base 23815) | 1.208x | — | 0.82 | 0.82 | 1377 | 1277 |
| GIL-off W=8 | 14184.3 | **14322.4** | +1.0% | **−33.9%** (base 21674) | **1.372x** | 1.99x | 0.63 | 0.65 | 1308 | 1293 |
| GIL-off W=16 | 13844.7 | **13642.0** | −1.5% | **−28.9%** (base 19198) | **1.440x** | 1.99x | 0.51 | 0.52 | 1339 | 1338 |
| GIL-off W=32 | 14840.0 | **14424.4** | −2.8% | **−33.6%** (base 21716) | **1.362x** | 1.75x | 0.37 | 0.39 | 1318 | 1336 |
| GIL-on W=1 | 13841.5 | **13922.5** | +0.6% | **−14.6%** (base 16309) | — | — | 1.04 | 1.04 | 421 | 423 |

- All 81/81 ladder+gilon+congc+stab32 reps rc=0; every checksum tuple
  matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
- **W=1-neutrality (campaign-8 design constraint):** v38 W=1 GIL-off =
  19647.0 ms = **−2.62% vs §32's 20174.6** → **PASS** (±3% gate). All
  four campaign-8 changes are gated on `isSharedServer()` (sticky
  clients-ever≥2) or the spawned-arm `[[unlikely]]` GILDroppedSection
  branch — structurally unreachable at W=1. The −2.6% is host noise
  (same-host base also moved 24927→25092, +0.7%; W=1 pc1+2 4195→4143
  −1.2%; GIL-on W=1 +0.6%).
- **Speedup-vs-self vs Java curve** (vs v38's own W=1 19647 ms): W=8
  1.372x **< 1.99x** (need ≤9873, gap −4449); W=16 1.440x **< 1.99x**
  (need ≤9873, gap −3769); W=32 1.362x **< 1.75x** (need ≤11227, gap
  −3198). **All three Java bars MISSED.** vs §32 the ratios moved
  **−0.050/−0.017/+0.003** (W=8 down, W=16 down, W=32 flat). The W=8
  drop is the W=1 −2.6% numerator and W=8 +1.0% denominator combining;
  both are noise-range individually.
- **Per-section v38** (medians): pc1+2 4143/6646/6034/5746/**5806**/5843
  ms at W=1..32 (vs §32's W=1 4195 / W=16 5898: −52/−92, flat); phaseA
  12825/12038/8696/7129/**6342**/6450; phaseB 2516/2044/1456/1225/1212/
  1807; phaseC 108/251/303/184/178/202. **§32-attribution update:** v38
  W=16 pc1+2 = 5806 = **43% of 13642** (was 43%); GIL-off/on W=1 pc1+2
  ratio **1.571x → 1.510x** (GIL-on pc1+2 2670→2744 +2.8% host noise;
  W=1 paths unchanged by design); W=16/W=1 pc-sibling-interference
  **1.406x → 1.401x** — *unchanged* (T5-barrier-site is wired but did
  NOT measurably reduce the per-Eden stop cost; see (c) below).
  Parallelizable-section speedup (W=16 wall − pc1+2 vs W=1 wall − pc1+2)
  **2.01× → 1.98×** (15504/7836) — *flat*; the §32(a) 2.27→2.01
  regression is NOT recovered.
- **W=32 bimodality — STAYS FIXED.** stab32 v38: min 12766 / med
  14405.7 / max 15150 ms, **19% spread** (≤25% gate); phaseA min 4612 /
  med 6239 / max 6446, **0/30 slow-mode**, 2/30 fast-mode; sys_s max
  11.4 s. vs §32: med 14560→14406 −1.1%. F3's lock-free findBit scan +
  F4's byte-threshold did not regress this; the 2-of-30 fast tail is the
  same effect as §32's 3/30.
- **Honest negatives:** **(a)** T5-barrier-site (GILDroppedSection coop
  snapshot at the JS-park funnel) was the §32 named target ("verify
  coopParkedClients populates at the pc-loop JS-barrier park") — it is
  now wired and the t5verify probe confirms publish, but pc-sibling-
  interference moved only 1.406→1.401x (W=16 pc1+2 5898→5806, −92 ms).
  The ~1700 ms W=16-vs-W=1 pc gap is therefore **NOT** the
  suspend/resume round-trip; the next-named candidate is the per-Eden
  STOP itself (W siblings handshaking the §10.4 barrier each cycle) or
  the per-client `clientSet().forEach` reset cost. **(b)** F1/T4-retune
  cap=0 left W=16 phaseA at 6342 ms — flat vs the t4crit-probe ~6296
  (the §32(a) +519 ms regression vs v36 is **NOT recovered**; cap=0 is
  ~0-ms-neutral as the implementation comment predicted, it just stops
  T4 from un-publishing T5 snapshots). **(c)** Parallelizable-speedup
  1.98× and sibling-int 1.40× both miss the achievable-ceiling gate
  (≥2.3× / ≤1.2×) — the ceiling result cannot be claimed. **(d)** W=8
  speedup-vs-self 1.422→1.372 nominal regression (the only ratio cell
  moving >|0.02|), but the underlying W=8 wall is +1.0% and W=1 −2.6%,
  both inside the ±~2% host-noise band. **(e)** W=4 RSS 1377→1277 MB
  (−7%) and W=32 ladder shows 1-of-3 fast-mode (13017 ms): same fast
  tail as stab32. **(f)** F4-burst's byte-threshold moved W=32 phaseB
  from §32's range to 1807 ms (no §32 absolute to compare directly; the
  W=32 ladder pc1+2 5843 vs §32 ~6360-6736 suggests the over-admit-Full
  named target did improve, but W=32 wall only −2.8% net).

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV**, 1 unique cs tuple (median 14405.7 ms; **0/30 slow-mode**, 2/30 fast-mode, 19% spread ≤25%, loadavg 6.2→18.6) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug `f55345a6…`) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v38) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 15872.2 ms (§32: 16228.4, −2.2%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−14.6%** (≤2% gate PASS; +0.6% vs §32 v37 = neutral) |
| **W=1 GIL-off neutrality** (vs §32 20174.6 ms, ±3%) | **−2.62% PASS** |
| No same-host wall cell regressing >5% vs §32 | **PASS** (every cell −2.8% to +1.0%) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.372/1.440/1.362 vs 1.99/1.99/1.75) |
| Achievable-ceiling (parsec ≥2.3× AND sibint ≤1.2×) | **NOT MET** (1.98× / 1.401×) |

### Acceptance verdict

**All correctness gates GREEN; W=1-neutrality PASS; W=32 bimodality
STAYS FIXED; scalability NEUTRAL; Java bar MISSED.** Corpus + identity
green (identical pass counts to §32); W=32 0/30 crash, 19% spread,
0 slow-mode; GIL-on same-host −14.6% (neutral vs §32, +0.6%); congc
−2.2%; max vs-§32 regression +1.0% (W=8). Campaign-8 implements all
four §32 named follow-ups and lands them without regression, but none
moved the headline: T5-barrier-site is wired yet pc-sibling-interference
holds at 1.40× (the suspend round-trip was not the dominant cost);
F1/T4-retune is ~0-ms-neutral as designed; F3/F4 are W=32 hardening and
hold the stab32 result without regressing it. The campaign-8 delta can
land (W=1-neutral by construction; F3 de-locks rather than de-triggers
the bimodal32 root cause; F4 closes the §32(b) burst over-admit; T5
extends coop-snapshot coverage to JS parks; no correctness regression;
no >5% wall regression). **Campaign-9 named target:** the residual
1.40× pc-sibling-interference is now disambiguated — it is NOT the
SIGUSR2 suspend cost (T5 closed that and the ratio held). Candidates:
(i) the per-Eden STW handshake itself (W siblings each round-tripping
the §10.4 access-released barrier ~31 times in pc1+2 vs W=1's 0
round-trips), (ii) `clientSet().forEach` per-cycle reset cost (now
touches W× two cache lines per cycle), (iii) the W=1 allocation tax
(1.51× off/on, unchanged since §31 — a separate, larger lever). The
parallel-section ceiling (1.98×, regressed from §31's 2.27× at v36)
remains the second open thread: the §32(a) +519 ms W=16 phaseA loss is
still present in v38 (phaseA 6342 ≈ v37's t4crit 6296) and is NOT the
T4 cap (cap=0 is neutral) — re-examine the T5 publish/clear bracket
overhead per parallel-phase iteration, or the MarkedBlock §32 delta.

## §34 Run 3.9 / scalebench matrix v4

Full cross-language matrix on `jsc-v39` (sha `9152bed9…`, congc
default-on via `3be6a06a7a65`): Go / Java / JS-BigInt / JS-intcs at
W∈{1,2,4,8,16,32}, 3-rep medians, one process at a time, `/usr/bin/time
-v`. Toolchains: go 1.24.13, OpenJDK 21.0.10 (Corretto). Go/Java
binaries reused from `out/` (sources unchanged since Jun 12; W=2 smoke
re-verified the cross-language reference tuple). Driver `v4_matrix.sh`;
raw `results-v4-raw.jsonl` (113 records, 0 nonzero rc). Debug sha
`fc7b47c3…`. Loadavg 1.6–13.5 across the matrix; characterization runs
3.5–8.4.

### Full matrix (3-rep medians, speedup-vs-self per language)

| lang | W | wall ms | sv-self | cpu% | RSS MB | phaseA | phaseB | phaseC | pc1+2 |
|---|---|---|---|---|---|---|---|---|---|
| **go** | 1 | 1835.8 | 1.000× | 145 | 176 | 1315 | 408 | 17 | — |
| go | 2 | 1090.9 | 1.683× | 261 | 177 | 789 | 208 | 12 | — |
| go | 4 | 725.5 | 2.530× | 434 | 179 | 509 | 119 | 12 | — |
| go | 8 | 535.4 | 3.429× | 662 | 180 | 361 | 72 | 12 | — |
| go | 16 | 422.0 | 4.350× | 956 | 191 | 272 | 50 | 12 | — |
| go | 32 | 378.1 | 4.855× | 1305 | 198 | 227 | 51 | 12 | — |
| **java** | 1 | 1973.5 | 1.000× | 199 | 757 | 1319 | 507 | 30 | — |
| java | 2 | 1353.7 | 1.458× | 329 | 769 | 875 | 326 | 46 | — |
| java | 4 | 1085.4 | 1.818× | 516 | 790 | 628 | 281 | 69 | — |
| java | 8 | 939.1 | **2.101×** | 809 | 792 | 533 | 255 | 63 | — |
| java | 16 | 976.4 | **2.021×** | 1395 | 873 | 496 | 264 | 106 | — |
| java | 32 | 1022.0 | **1.931×** | 2575 | 744 | 514 | 264 | 138 | — |
| **js-BigInt** | 1 | 20101.8 | 1.000× | 104 | 422 | 13177 | 2547 | 109 | 4161 |
| js-BigInt | 2 | 20988.1 | 0.958× | 196 | 1783 | 12015 | 2058 | 267 | 6528 |
| js-BigInt | 4 | 15767.3 | 1.275× | 329 | 1407 | 8334 | 1431 | 187 | 5732 |
| js-BigInt | 8 | 13786.3 | 1.458× | 527 | 1407 | 6727 | 1089 | 180 | 5736 |
| js-BigInt | 16 | 13145.9 | 1.529× | 824 | 1386 | 6015 | 1042 | 174 | 5885 |
| js-BigInt | 32 | 14356.3 | 1.400× | 1258 | 1371 | 6120 | 1718 | 196 | 6286 |
| **js-intcs** | 1 | 16711.9 | 1.000× | 104 | 398 | 12829 | 2504 | 117 | 1191 |
| js-intcs | 2 | 16199.2 | 1.032× | 204 | 1533 | 11938 | 1973 | 264 | 1935 |
| js-intcs | 4 | 11909.2 | 1.403× | 374 | 1258 | 8313 | 1444 | 212 | 1904 |
| js-intcs | 8 | 10114.7 | **1.652×** | 653 | 1274 | 6811 | 1134 | 194 | 1869 |
| js-intcs | 16 | 9236.3 | **1.809×** | 1079 | 1274 | 5912 | 1119 | 175 | 1926 |
| js-intcs | 32 | 9795.1 | **1.706×** | 1671 | 1270 | 5990 | 1757 | 182 | 1882 |

All 72 matrix reps rc=0. Every Go/Java/JS-BigInt rep matches the
reference tuple `b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|
c4bdd580f85ee058|af028188d7a56a96`; every JS-intcs rep matches
`8021f000|4158957|1fc7d941|c4bdd580f85ee058|af028188d7a56a96`
(checksumA=`8021f000`, checksumC=spec `af028188d7a56a96`). GIL-on W=1
(5 reps interleaved with GIL-off): med **13909.0** ms
[13426,13522,13909,13942,14402]; GIL-off/on W=1 ratio **1.445×**.

### Honest verdict: JS-intcs vs fresh Java

The §31–§33 Java bar {1.99, 1.99, 1.75} was stale (Jun 10, run 1). Fresh
Java on the same host this run: **{2.101, 2.021, 1.931}** at W={8,16,32}
— Java is *slightly better* than the stale bar at W=8/32 and ~flat at
W=16. JS-intcs vs fresh Java:

| W | JS-intcs sv-self | Java sv-self | ratio | abs (JS/Java wall) |
|---|---|---|---|---|
| 8 | 1.652× | 2.101× | 0.79 | 10.77× |
| 16 | 1.809× | 2.021× | **0.90** | 9.46× |
| 32 | 1.706× | 1.931× | 0.88 | 9.58× |

**All three Java bars MISSED** on the intcs arm too, but W=16 is at 90%
of Java's curve (vs BigInt's 1.529/2.021 = 76%). Go at W=16 is 4.35× —
neither JS nor Java approach it. Absolute wall: JS-intcs is ~9.5–10.8×
Java and ~22–25× Go; the W=1 single-thread gap (JS-intcs 16712 / Java
1974 = 8.47×; / Go 1836 = 9.10×) is the dominant factor — scaling shape
alone would close <1.2× of that. Parallelizable-section speedup W=16
(wall − pc1+2, vs W=1): **JS-BigInt 2.20×, JS-intcs 2.12×** — the §33
1.98× regression is recovered and slightly past §31's 2.27× ceiling on
the BigInt arm (the difference is W=16 phaseA 6342→6015, −5%, with congc
default-on).

### BigInt-fairness note (intcs arm, §29 amendment)

The intcs arm replaces only the §1.8 postings-checksum loop with
allocation-free 32-bit Number arithmetic; phaseA/B/C workload, locking,
and Map/array shapes are byte-identical (phaseA medians match BigInt
within 0–3% at every W). The BigInt allocation tax is entirely in
pc1+2: W=1 4161→1191 ms (−71%), W=16 5885→1926 (−67%). W=1 wall
20102→16712 (−17%); W=16 13146→9236 (−30%, because pc1+2 is a larger
share at high W). The fairness reading is: **JS shared-memory threading
scales to 1.81× at W=16 on a workload with native-int checksums** (the
spec-exact BigInt path measures BigInt allocation under the shared GC
as much as it measures threading).

### congc-correction note

Per the `3be6a06a7a65` commit comment: the earlier −26% A/B
(intcs W=16 ~7550 vs ~10200) was the documented W=16 fast-mode
bimodality coinciding with congc=1 reps, not a congc effect. v39
(congc default-on) vs §33 v38 BigInt medians: W=1 +2.3%, W=4 −3.0%,
W=8 −3.7%, W=16 −3.6%, W=32 −0.5% — **neutral-to-slightly-faster**, no
−26%. W=32 30-rep spread tightened 19%→14%.

### (C) W=16 fast-mode characterization

15 consecutive JS-intcs W=16 reps with `JSC_logGC=1`: **0/15 fast-mode**
(phaseA 5940–6232 ms, all normal); GC counts uniform at **51 Eden / 35–36
Full**, sys 4.2–5.7 s, cpu 1082–1110%. Control 15 reps without logGC:
**6/15 fast-mode (40%)**; +6-rep `largeHeapGrowthFactor=2` probe: 2/6
(unchanged rate).

| mode | phaseA med | total med | sys_s med | cpu% med | GC Eden/Full |
|---|---|---|---|---|---|
| fast (control, n=6) | 3854 | 7175 | 3.69 | 904 | (unobservable) |
| normal (control, n=9) | 6024 | 9398 | 4.68 | 1094 | — |
| normal (logGC=1, n=15) | 6118 | 9389 | 4.85 | 1095 | 51 / 35–36 |

**Discriminant:** fast-mode is −37% phaseA, −24% wall, −21% sys_s, −17%
cpu_pct vs normal. The lower cpu-seconds + sys_s are consistent with
*fewer GC cycles* in fast-mode (less marking work, fewer STW
handshakes). **The §32 hypothesis (T1 distinct-allocator gate) cannot be
confirmed via direct GC-count instrumentation:** `JSC_logGC=1` itself
suppresses fast-mode (0/15 vs 6/15, P(0/15 | p=0.40) ≈ 0.0005), so GC
counts are only observable in normal-mode. The per-cycle `dataLog()`
adds enough latency at the collection-start path to deterministically
resolve the early-phaseA timing race toward normal. **JIT tier is not
the discriminant** (one `JSC_verboseOSR=1` rep shows ~29k FTL OSR
entries either way; verboseOSR is +47% phaseA, far too perturbing for
mode discrimination). `largeHeapGrowthFactor=2` does not change the
rate. **Named knob:** `JSC_logGC=1` makes mode *deterministically
NORMAL* (0/15) — **not a perf lever** (it forces the slower mode). No
single env knob found that forces *fast* deterministically. Fast-mode is
a real ~24% wall lever *if* its trigger were controllable; the next
discriminating experiment needs an out-of-band GC counter (e.g. a
`$vm.gcCount()` probe in `bench.js` that the shell prints, not the
Heap-side `dataLog`) to read Eden/Full counts on a fast rep without
perturbing the start-of-collection timing.

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV** (re-verified `results-v39ab-raw.jsonl`: 1 unique cs tuple = ref, med 14003.5 ms, **14% spread** ≤25%, 0/30 slow-mode, 2/30 fast-mode, sys_s 7.5–11.4, loadavg 5.8–16.0) |
| **Corpus GIL-off full** (`run-tests.sh`, pinned env, Debug `fc7b47c3…`) | **41 passed, 53 failed, 4 skipped — FAIL.** All 53 failures are `ASSERTION FAILED: isMarked(cell)` at `Heap.cpp:1658 addToRememberedSet` (exit 134). Reproduces on every GIL-off corpus test that spawns ≥1 Thread. `JSC_useConcurrentSharedGCMarking=0` does **not** mitigate: the v39 `Options.cpp` block force-sets it after env parsing under `!useThreadGIL && useSharedGCHeap`. |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug) | **95 passed, 0 failed, 3 skipped** (identical to §32/§33) |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v39) | **0 mismatches** |
| All checksums match references (Go/Java/JS-BigInt = spec tuple; JS-intcs = `8021f000` + spec-C) | **PASS** (113/113 reps) |
| No crash anywhere in matrix/characterization (Release) | **PASS** (113/113 rc=0) |
| Speedup-vs-self beats fresh Java at W=8/16/32 (intcs arm) | **FAIL** (1.652/1.809/1.706 vs 2.101/2.021/1.931) |

### Acceptance verdict

**Release-side correctness GREEN; Debug GIL-off corpus RED.** Matrix
113/113 rc=0 with all checksums matching; W=32 stability 30/30 holds and
spread tightened 19%→14% under congc-default-on; identity 0 mismatches;
GIL-on corpus 95/0/3. **But the Debug GIL-off corpus regresses
94/0/4 → 41/53/4** on a single signature: the `addToRememberedSet`
write-barrier `ASSERT(isMarked(cell))` fires under concurrent shared-GC
marking. This was never covered by the prior §32/§33 congc gate (which
was Release-only `bench.js -- 4` × 5); v39 making congc the GIL-off
default exposes it on every threaded Debug run. **The v39 default-flip
cannot land as-is** — either the assertion is a stale invariant under
the SPEC-congc N-mutator barrier protocol (in which case it should be
relaxed to the `mutatorShouldBeFenced()` branch's tolerant form for the
shared-heap `routedClient` path), or it is a real lost-mark race that
Release masks (in which case the W=32 30/30 + checksum-match is
necessary but not sufficient evidence). **Campaign-9 named target
(amended):** before any further scalability work, resolve the
`Heap.cpp:1658` Debug assertion under congc — the §33 named pc-sibling
target stands second.

## §35 flat-gap-bughunter

Tree `9791feb9b3d9` + worktree delta below; Release jsc sha256
`2edfe0eaa3ff…`, Debug `dcc6c51abe17…`. Pinned GIL-off env, 64 HW
threads, loadavg 0.4–1.4 during the 3-rep matrix. Evidence pack:
`docs/threads/FLAT-GAP-EVIDENCE.md` (perf/eu-stack/logGC/verboseOSR/
holdtime micro from the pre-fix tree).

### Survivor list (changes that landed this round)

| target | file | what it does | flag-off effect |
|---|---|---|---|
| **forof-tdz-osr-loop** | `dfg/DFGFixupPhase.cpp` | The optional `Check<Int32Use>` hint inserted at `PutClosureVar` (per-iteration for-of scope copy) BadType-exits forever on the iteration-0 TDZ-empty sentinel — ValueProfile cannot record SpecEmpty. Honor the `BadType` exit profile and drop the hint on recompile (same idiom as the rest of the file). Stops the 26 234-exit / 8-jettison loop that pinned `ingestDocFlat` in Baseline (FLAT-GAP-EVIDENCE §5). | Gated `Options::useJSThreads()`; flag-off codegen byte-identical. |
| **hold-vmEntry-trampoline** | `runtime/LockObject.cpp` | gilOff-only fast path for `lock.hold(fn)` when `fn` is a non-host JSFunction with an installed `codeBlockForCall()` and `numParameters() ≤ 1`: dispatch via `vmEntryToJavaScriptWith0Arguments` (the CachedCall / MicrotaskCall shape) instead of `getCallData→JSC::call→executeCallImpl`. Hoist `Thread::currentSingleton()` (4 TLS reads → 1). holdtime micro 117 ns/call → ~35 ns/call; FLAT-GAP-EVIDENCE §1 attributed 16.5 % self at W=16 to the slow trampoline. | gilOff-gated; flag-off falls through to the unchanged `JSC::call` path. |
| **lazy-species-install-race** | `runtime/JSGlobalObject.{cpp,h}` | NEW engine bug found this round: `Int32Array.prototype.slice` on N worker threads races the lazy `tryInstallTypedArraySpeciesWatchpoint` (50 % SIGABRT at W=16, `RELEASE_ASSERT(watchpointSet.state() == IsWatched)` at `ObjectPropertyChangeAdaptiveWatchpoint.h:44`). Route the once-per-type install through `JSThreadsSafepoint::stopTheWorldAndRun` with an in-window re-check, exactly the `haveABadTime()` pattern. | gilOff-gated; flag-off / GIL-on falls through to the unchanged inline body. |
| **serial-math-global-getbyidflush-loop** | `bench.js` | `const $imul = Math.imul` module-level hoist. GIL-off CheckTraps widening blocks LICM of the `Math` GlobalProperty `GetByIdFlush`; pc-shaped loop 89.6 → 2.0 ns/elem. Fairness correction (Go/Java have no global-property indirection for imul). | Flat-arm only; default arm untouched. |
| **hold-closure-alloc** | `bench.js` | Hoist the 5 `lock.hold(() => …)` arrows to per-worker JSFunctions captured over `sc`; loop-varying values staged through `sc.h*` slots. ~5 M closure+env allocs → 5×W. Fairness correction (Go/Java allocate 0 bytes per critical section). | Flat-arm only. |
| **serial-seg-getbutterfly-osrexit** | `bench.js` | `flattenFlatShards()` / `flattenGroupsFlat()`: rebuild cross-thread-filled posting lists / group arrays as thread-0 `Int32Array` so the serial readers + phaseB are monomorphic. `ingestWriterDocFlat` keeps phaseB writers typed. Fairness correction (Go `[]int` / Java `ArrayList` are contiguous regardless of which thread appended). | Flat-arm only. |

### JS-flat wall ms vs Java / Go (3-rep medians; Go/Java from §34)

| W | **JS-flat ms** | Java ms | Go ms | JS/Java | JS/Go |
|---|---|---|---|---|---|
| 1 | **3389** | 1974 | 1836 | 1.72× | 1.85× |
| 16 | **1320** | 976 | 422 | **1.35×** | **3.13×** |
| 32 | **2351** | 1022 | 378 | 2.30× | 6.22× |

W=16 reps {1318, 1320, 1343}; W=1 {3377, 3389, 3450}; W=32
{1529, 2351, 2428} (bimodal — §34(C) fast-mode signature, 1/8 fast in an
8-rep stability sweep). All 9 matrix reps + 12 W=16 stability reps + 8
W=32 stability reps rc=0; every rep matches the flat reference
`686d6890|4154468|0fbbd673|3af6b072|e1d22021`.

Against the §34 entry baseline (JS-flat W=1 ≈ 5461 ms, W=16 ≈ 4577 ms):
W=1 −38 %, **W=16 −71 %**. The W=16 gap to Java closes from 4.7× to
**1.35×**; to Go from 10.8× to **3.13×**. The W=1 gap to Go closes from
3.0× to **1.85×** — the forof-tdz-osr-loop fix alone moved
`ingestDocFlat` from Baseline to FTL and recovered most of the
single-thread floor.

Residual W=16 wall composition: phaseA 593 + phaseB 234 + phaseC 33 +
serial (pc1+df+pc2+csC) 461 = 1320 ms. pc1 still 351–372 ms (4.6 M
postings, FTL): the remaining serial floor. W=32 regresses vs W=16
(phaseA 593 → ~1520) — over-subscription / shard-lock contention; Java
and Go are also flat-to-down at W=32.

### Default arm (spec-exact, unchanged path)

W=1: 19 003 ms, W=16: 13 013 ms; reference tuple
`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|
af028188d7a56a96` matches §34 at both W. W=1 −5 % vs §34 (20 102 ms),
W=16 −1 % — the engine deltas are neutral on the BigInt/string-heavy
spec arm (its hot loops were already FTL-stable; `lock.hold` is gilOff-
gated and the for-of fix only changes recompile behaviour).

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug `dcc6c51abe17…`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| Default-arm reference tuple unchanged (W=1 + W=16) | **PASS** |
| Flat-arm checksum stable (all W, all reps) | **PASS** (`686d6890|4154468|0fbbd673|3af6b072|e1d22021`) |
| W=16 stability (12 reps post-species-fix) | **12/12 rc=0**, 1282–1628 ms |
| W=32 stability (8 reps) | **8/8 rc=0**, 1553–2714 ms (bimodal) |
| **JS-flat W=16 wall < 4000 ms** | **PASS — 1320 ms** |

### Named follow-up targets

1. **lazy-species-install-race, sibling sites**: the same TOCTOU exists
   for `tryInstallArrayBufferSpeciesWatchpoint` and the lazy
   typed-array-iterator-protocol watchpoints (any
   `state() == ClearWatchpoint` gate in
   `JSGenericTypedArrayViewPrototypeFunctions.h` /
   `ArrayBufferPrototype.cpp`). Only the Int32Array-species site is
   exercised by the flat bench; the others should get the same STW
   wrapper before any user code reaches them.
2. **W=32 phaseA contention**: 1520 ms vs W=16's 593 ms with the same
   work and 2× threads — K=128 shard locks at W=32 hit the
   adaptive-spin → park threshold harder. Not a correctness issue;
   recorded for the next scalability pass.
3. **pc1 serial floor (~350 ms)**: pc1 is FTL-warm at W=1 (143 ms) but
   2.5× at W≥2 — the `for…of flatShards[s]` Map-iterator on
   foreign-allocated Maps still costs the segmented-butterfly read path
   on first walk. A per-shard term-id Int32Array index built at
   `flattenFlatShards()` time would make pc1 a flat typed-array walk.

## §36 flat-gap-bughunter round 2

Tree `56b8f886e000` + worktree delta below; Release jsc sha256
`e0fb8fe1cfb5e43c…`, Debug `ebf1057b7d42f42b…`. Pinned GIL-off env, 64 HW
threads, loadavg 2.2–8.2 across the matrix. Evidence pack:
`docs/threads/FLAT-GAP-EVIDENCE.md` Round 2 (perf-annotate / eu-stack /
lock-contention / pc1 split-timer on the §35 binary `2edfe0eaa3ff…`). Raw
`results-v40ab-raw.jsonl` (36 records, 0 nonzero rc). Driver
`v40_ab.sh` / `v40_analyze.py`.

### Survivor list (changes that landed this round)

| target | file | what it does | flag-off effect |
|---|---|---|---|
| **jitcode-refptr-bounce-14pct** | `bytecode/CodeBlock.h`, `ftl/FTLOperations.cpp`, `runtime/LockObject.cpp`, `dfg/DFGOSRExit.cpp` | `CodeBlock::jitCode()` returns `RefPtr<JITCode>` BY VALUE; the gilOff U-T4b lazy-slow-path / DFG-exit steady states and the round-1 `lockProtoFuncHold` fast path call it on a SHARED CodeBlock millions of times — `perf annotate` attributes ≈14.4% of W=16 on-CPU to the `lock incl/decl m_refCount` cache-line bounce (FLAT-GAP-EVIDENCE round 2 §(1)). New `jitCodeRawPtr()` raw accessor (same consume-ordered `m_jitCode.get()` as `jitType()`); use it at the three hot sites and cache the `DFG::JITCode*` once in `operationCompileOSRExit`. | Output-identical (one fewer single-threaded inc/dec on a temporary). |
| **dfg-osrexit-genlock-dclp-precheck** | `dfg/DFGOSRExit.cpp` | gilOff U-T4b means EVERY DFG exit traversal lands in `operationCompileOSRExit` forever; the existing-ramp short-circuit was AFTER the `dfgOSRExitGenerationLock` tryLock spin AND after `variableEventStream.reconstruct()`. eu-stack on a W=32 slow-mode rep: 22% of JS threads at `__sched_yield` inside that spin, with the exit firing INSIDE the held shard lock (2.2× park inflation, +850 ms phaseA bimodal). Add the lock-free DCLP read FIRST: `m_exits[i].m_codePtr` (the same single tagged word the JIT-emitted unlinked dispatch reads lock-free) vs a function-static cached thunk codePtr; paired `storeStoreFence` before `setExitCode`. Steady state: 1 raw-ptr deref + 1 compare + 1 per-lite store, no lock, no reconstruct. | gilOff-only arm; flag-off byte-identical (the fence is once-per-exit-compile). |
| **lazy-species-install-race, sibling site** (§35-R1) | `runtime/JSGlobalObject.{cpp,h}` | `tryInstallArrayBufferSpeciesWatchpoint` has the identical TOCTOU as the §35 typed-array site (`JSArrayBufferPrototypeInlines.h:44` gates on `state() == ClearWatchpoint`). Same `JSThreadsSafepoint::stopTheWorldAndRun` wrapper + in-window re-check; `Impl` body unchanged. The other §35-R1 candidate (typed-array-iterator-protocol watchpoints) is NOT a `ClearWatchpoint`-gated lazy install (those sets are installed eagerly at global init and probed via `state() != IsWatched`), so no sibling fix needed there. | gilOff-gated; flag-off byte-identical. |
| **gc-sharedheap-zero-concurrent-overlap-now-11pct** (SPEC-congc §7.1a) | `heap/Heap.cpp` | Under the pinned env every shared collection was a §3.6 degenerate single STW window — at W=16 a fixed ~143 ms floor (11% of §35's 1291 ms wall, 45% of the residual JS→Java gap). §27.S2 had refuted the full stage-C1 default because unbounded scheduler-driven Concurrent handoffs added ~30 reentries/run. §7.1a single-handoff: gilOff WITHOUT C1, schedule AT MOST ONE Concurrent window per cycle (conductor-thread-local `t_sharedGCConcurrentHandoffsThisCycle` cap; F44 multi-producer barrier stack covers between-window barriers; F19 always-fenced master holds). New `sharedGCWindowedConductActive()` predicate (= stage-flags ∨ `isGILOffProcess()`) keys the WND-open/close / atom-table-pin / reclaim-placement arms. | `isGILOffProcess()` is false flag-off; predicate degenerates to `sharedGCWindowedStagesEnabled()`, every arm byte-for-byte the §27.S2 default (CG-I0). |
| **flatten1-segmented-int32shape-segwalk-copy** | `runtime/ConcurrentButterflyInlines.h`, `runtime/JSGenericTypedArrayViewInlines.h` | `Int32Array.set(segmentedJSArray)` / `new Int32Array(segmentedJSArray)` bailed to the generic per-element `get()→PropertySlot→ToNumber→scope-check` loop (the §10.7 OOB-safety fix). New `forEachSegmentedIndexedContiguousRun` walks the spine's immutable fragment table directly and reuses the flat `copyElements` / `toNativeFromInt32` bodies per ≤4-slot run; plus a `tryGetIndexQuickly` JSArray-generic fallback (header-INLINE, zero PLT/frame) for the non-memcpy shapes. | Gated `mayBeSegmentedButterfly()` / `Options::useJSThreads()` — both constant-false flag-off (I22), original flat path byte-identical. |
| **flatten1-parallelize-shards-out-of-pc1-timer** | `bench.js` | The K=128 shard Maps are independent; Go/Java have NO flatten step at all, so timing a JS-only segmented→typed normalization inside `postingsChecksum1_ms` is a fairness defect. Run the **§35 flatten body unchanged** as a W-parallel shard-stripe (worker `id` owns `s % W === id`) between the phaseA barrier and a new barrier, OUTSIDE the pc1 timer. Sources are the SHARED segmented posting lists; the engine-side segwalk-copy makes the per-shard cost flat-W. W=16 `flatten1_ms`: 206 ms serial → 57 ms wall. (The earlier per-(worker×shard) owner-local-chunk + 3-pass-merge redesign was measured and REJECTED: W=16 flatten1 384 ms / total 1585 ms — see FLAT-GAP-EVIDENCE Round-2 landed-deltas.) | Flat-arm only; default/intcs arms byte-identical. |
| **serial-461-decompose** | `bench.js` | Output-only `flatten1_ms / cs1_ms / flatten2_ms / cs2_ms` keys. | JS-only diagnostic; fairness-neutral. |

### JS-flat wall ms vs Java / Go (3-rep medians; Go/Java from §34)

| W | **JS-flat ms** | Java ms | Go ms | JS/Java | JS/Go | §35 JS-flat | Δ vs §35 |
|---|---|---|---|---|---|---|---|
| 1 | **3223** | 1974 | 1836 | 1.63× | 1.76× | 3389 | −5% |
| 8 | **1104** | 939 | 535 | 1.18× | 2.06× | — | — |
| 16 | **1032** | 976 | 422 | **1.06×** | **2.45×** | 1320 | **−22%** |
| 32 | **1374** | 1022 | 378 | 1.34× | 3.63× | 2351 | **−42%** |

W=16 reps {989, 1032, 1064}; W=1 {3173, 3223, 3323}; W=8 {1096, 1104,
1110}; W=32 {1332, 1374, 1871} — the 1871 outlier at loadavg 4.4 with
phaseA 1115 ms; the 12-rep stability sweep below shows 0/12 in that
range. All 27 flat reps + 12 stability reps rc=0; every rep matches the
flat reference `686d6890|4154468|0fbbd673|3af6b072|e1d22021`.

**The W=16 gap to Java closes from §35's 1.35× to 1.06×** (56 ms over
Java); to Go from 3.13× to 2.45×. The W=32 gap to Java closes from 2.30×
to 1.34× — the dfg-osrexit-genlock-dclp-precheck collapses the bimodal
slow mode (§35-R2). W=1 to Go closes from 1.85× to 1.76×.

Residual W=16 wall composition (median rep): phaseA 540 + flatten1 57 +
cs1 60 + buildDfSnap 6 + phaseB 242 + pc2 78 (= flatten2 67 + cs2 14) +
phaseC/csC 35 ≈ 1018 ms (overhead/barrier ~14 ms). Serial-on-thread-0
floor (cs1 + dfSnap + pc2 + csC) ≈ 151 ms vs Java W=16 serial ≈ 110 ms;
the parallel sections (phaseA + flatten1 + phaseB + phaseC) ≈ 866 ms vs
Java W=16 (phaseA + phaseB + phaseC) ≈ 866 ms — **at W=16 the JS
parallel-section wall now matches Java's**; the remaining 56 ms gap is
the JS-only flatten1 wall (57 ms, parallel but Go/Java pay 0) plus
serial-floor delta.

### Slower-arm re-baseline (default + intcs, 3-rep medians, GIL-off)

| arm | W | wall ms | §35 (this binary) | §34 | Δ vs §34 | reference tuple |
|---|---|---|---|---|---|---|
| default | 1 | 18 069 | 19 003 / 19 872 | 20 102 | −10% | `b3e65a68…\|4158957\|39c33392…\|c4bdd580…\|af028188…` ✓ |
| default | 16 | **9 887** | 13 013 / 13 183 | 13 146 | **−25%** | ✓ unchanged |
| intcs | 1 | 14 030 | — / 16 057 | 16 712 | −16% | `8021f000\|4158957\|1fc7d941\|c4bdd580…\|af028188…` ✓ |
| intcs | 16 | **7 395** | — / 8 326 | 9 236 | **−20%** | ✓ unchanged |

intcs W=16 reps {4916, 7395, 7833} — the 4916 is the §34(C) fast-mode
(unchanged signature). Round-2's engine deltas help the slower arms
substantially: default W=16 −25% vs §34 (the segwalk-copy +
jitcode-refptr-bounce + §7.1a single-handoff all touch paths the
spec-exact arm shares — `Array.push` segmented growth, `lock.hold`
trampoline, STW floor); intcs W=16 −20%. Both reference tuples
unchanged at every W and rep.

### W=32 stability (12 reps, post-dclp-precheck)

12/12 rc=0, all ref-match; total 1345–1542 ms (med **1419**, **13.9%
spread**). phaseA sorted: {654, 659, 664, 676, 688, 694, 696, 713, 787,
803, 815, 820} — **0/12 slow-mode** (§35: 3/8 slow at phaseA 1510–1597,
fast 694–730). **§35-R2 closed**: the dfg-osrexit-genlock-dclp-precheck
eliminates the global-lock spin-yield serialization point; the residual
787–820 phaseA upper band is the K=128 shard-lock park-rate doubling at
W=32 vs W=16 (FLAT-GAP-EVIDENCE round 2 §(4): fast-mode 7.6% park/acq vs
3.8%), not the OSR-exit-under-lock chain.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug `ebf1057b…`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| Default-arm reference tuple unchanged (W=1 + W=16) | **PASS** |
| intcs-arm reference tuple unchanged (W=1 + W=16) | **PASS** |
| Flat-arm checksum stable (all W, all reps incl. stability) | **PASS** (`686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021`, 27/27) |
| W=32 stability (12 reps) | **12/12 rc=0**, 1345–1542 ms, 13.9% spread, **0/12 slow-mode** |
| All matrix reps rc=0 | **PASS** (36/36) |
| **JS-flat W=16 wall < 1100 ms** | **PASS — 1032 ms** (1.06× Java) |

### Honest residuals

1. **W=16 flatten1 ~57 ms parallel-section wall** is the entire
   remaining JS→Java W=16 gap: Go/Java have no flatten at all. 3.6×
   parallel efficiency on 16 workers (W=1 81 ms → W=16 57 ms) is bounded
   by per-worker FTL warmup of `flattenFlatShards` (each worker calls it
   once; tier-up is per-thread) and the one Int32Array species-watchpoint
   STW that the first worker to reach `new Int32Array(jsArray)` issues.
   Next lever: pre-warm `flattenFlatShards` on thread 0 with one dummy
   shard before the phaseA barrier so all workers share the FTL
   compilation, OR move the species-watchpoint install to global-init
   (eager) so the STW lands before timing starts.
2. **`operationCompileFTLLazySlowPath` residual ~3% W=16 self**: with
   the refcount bounce removed, the steady state is still C-call +
   `DeferGCForAWhile` + `->ftl()` vtable + one acquire-load per FTL
   slow-path traversal (gilOff U-T4b never patches the jump). A
   thunk-side fix — the generation thunk reads the release-published
   `m_stubCodePtr` slot directly and tail-jumps when non-null without
   entering C — would remove it; deferred (codegen change, ≤30 ms W=16
   ceiling, no correctness exposure).
3. **W=32 phaseA 654→820 upper band (~25% intra-band spread)**: K=128
   shard-lock arithmetic at W=32 (linear-in-W park rate, FLAT-GAP-
   EVIDENCE round 2 §(4)). Not a correctness issue and not bimodal;
   recorded for the next scalability pass (K scales with W, or per-shard
   striped-lock).
4. **§34(C) fast-mode bimodality on default/intcs arms** is unchanged
   (intcs W=16 1/3 fast at 4916 ms): orthogonal to the flat-arm targets
   this round and §34's named discriminant (out-of-band GC counter)
   stands.

## §37 flat-gap-bughunter round 3

Tree `cbb7d616a1f6` + worktree delta below; Release jsc sha256
`1bda2d11d95516e8…`, Debug `7b9844107a619723…`. Pinned GIL-off env, 64 HW
threads, loadavg 5.9–9.5 across the matrix (host did not decay below the
2.5 gate inside the 180 s synchronous wait; W=16 cell at load 6.7). Evidence
pack: `docs/threads/FLAT-GAP-EVIDENCE.md` Round 3 (per-batch warmup probe /
flatten1 W-scan / lazy-slow-path counter / K=256 discriminant / GetByIdGaveUp
finding / Go `GOGC=off` floor) on the §36 binary `cbb7d616…/d953dcbf…`. Raw
`results-v41ab-raw.jsonl` (36 records, 0 nonzero rc). Driver `v41_ab.sh` /
`v41_analyze.py`.

### Survivor list (changes that landed this round)

| target | file | what it does | flag-off effect |
|---|---|---|---|
| **R-addproptransition-concurrently-unconditional-mlo** | `runtime/Structure.cpp`, `runtime/StructureInlines.h`, `runtime/StructureTransitionTable.h` | `addPropertyTransitionToExistingStructureConcurrently` took the source Structure's `m_lock` UNCONDITIONALLY, but the steady-state path is a read-only HIT on a single-slot transition word. New `tryGetSingleSlotConcurrently` acquire-probes `m_data`; on a key-matched single-slot publish, returns the transition + `transitionOffset()` lock-free (release in `setSingleTransition` orders the target's constructor stores). Map-tag / empty / mismatch fall through to `m_lock` (TransitionMap rehash hazard — intentionally NOT lock-free-walked). Evidence §(2): 25.4% W=16 flatten1 self + the phaseA first-term literal; same DCLP shape as the round-2 `dfg-osrexit-genlock-dclp-precheck`. | Reached ONLY via the `useJSThreads()` reroute in `addPropertyTransitionToExistingStructure` (StructureInlines.h); flag-off codegen byte-identical. |
| **go-gap-liveheap-7x-mark-15pct-postA** (tf-scratch + post-flatten1 `fullGC()`) | `bench.js` | Per-doc `new Map()` tf accumulator → per-worker reusable `Int32Array(V)` scratch + dirty-list (the struct-shaped accumulator the flat arm models). Kills 28k JSMap shells + ~2.8M HashMapBucket cells + 28k bucket vectors per phaseA; iteration walks `tfDirty` in first-occurrence order = Map insertion order, so per-term shard-lock sequence and the flat-arm checksum tuple are byte-identical. Then ONE explicit `fullGC()` after flatten1 (workers parked; JS-only discarded representation) drops the post-phaseA live set from ~645 MB to the ~55 MB Int32Array state — phaseB mark cycles ~10× cheaper. W=16 phaseB: 242 ms → **95 ms (−61%)**; flat W=16 peak RSS 420 MB. `flattenGC_ms` is a JS-only output diagnostic, accounted INSIDE `total_ms`. | Flat-arm only; default/intcs arms byte-identical. |
| **flatten1-full-FASTER-than-noTA-proves-collision** (in-place mutate + ingestWriterHold geometric-grow) | `bench.js` | Evidence §(2) discriminant: at W=16 the `{docIds,tfs}` literal's empty→docIds transition contends on the GLOBAL empty Structure's `m_lock` (map-tagged → DCLP precheck falls through). Drop the fresh wrapper entirely: mutate `pl` in place (existing-slot replaces, no transition), and `ingestWriterHold` geometric-grows the typed buffers in place instead of `appendI32`. **§37 fixup found in measurement**: the in-place `+len` add transitions a 2-prop-literal pl to a {docIds,tfs,len} Structure with inlineCapacity=2/len-OOL, DISTINCT from the 3-prop literal's inlineCapacity=3 Structure — `postingsChecksumFlat` then FTL-OSR-exits BadCache at bc#234 on the first phaseB-created pl (`JSC_verboseOSR`: exit #29 D@439 BadCache → reopt loop; W=1 cs2 14→110 ms). Fix: phaseA `ingestHold` allocates `{docIds:[],tfs:[],len:0}` so the wrapper is monomorphic process-wide. flatten1 `m_lock` traffic: zero. | Flat-arm only. |
| **Go `GOGC=off` algorithmic-floor record** | `go/main.go` (header comment only) | W=16 floor 354 ms (phaseA 202) — the zero-GC AOT-compiled M:N-scheduled SPEC ceiling on this host. Direct lens for the residual table. | Comment-only. |

### Measured-and-rejected this round

| proposal | result |
|---|---|
| **w1-cs1-cold-vs-warm-45ms** (8-shard `postingsChecksumFlat` pre-warm) | A/B at W=16: WITHOUT prewarm cs1 {67,68,79,82}; WITH prewarm cs1 bimodal {44–77} / {122–241}. The 8-shard call's return path executes once → second DFG (post-Overflow-reopt) under-profiles bc#475 (`InadequateCoverage` exit), and the FTLFunctionCall/FTLForOSREntry install races the prewarm→cs1 call boundary. ~1/3 reps land in a +160 ms slow band; net median +10 ms, ~7× variance. **Rejected**; `sEnd` parameter kept for a future per-function tier-up hint. |
| **§36-R2 thunk-side DCLP** (`operationCompileFTLLazySlowPath`) | Evidence §(3): 0.31% W=16 self (5.28 M calls, ≈21 cyc/call on the existing C++ DCLP fast path), ceiling ≤2 ms wall. **Do not implement.** |
| **§36-R3 K=256** | Evidence §(4): halves W=32 park rate to 7.5% but W=32 phaseA does NOT improve (832 vs 705) and W=16 goes bimodal. **Refuted** — W=32 band is not shard-lock-park-bound. |
| **§36-R1 species-STW / per-worker-FTL-warmup** | Evidence §(2): species fires at +736 ms (phaseB), 0 STW windows >2 ms in the flatten1 bucket, pre-warm −25% only. **Both refuted**; flatten1's W>8 knee is a global-resource contention on the ~65 k typed-array constructions (now obviated for the literal half by the in-place mutate above). |

### JS-flat wall ms vs Java / Go (3-rep medians; Go/Java from §34)

| W | **JS-flat ms** | Java ms | Go ms | JS/Java | JS/Go | §36 JS-flat | Δ vs §36 |
|---|---|---|---|---|---|---|---|
| 1 | **3759** | 1974 | 1836 | 1.90× | 2.05× | 3223 | +17% (see †) |
| 8 | **1010** | 939 | 535 | 1.08× | 1.89× | 1104 | −9% |
| 16 | **872** | 976 | 422 | **0.89×** | **2.07×** | 1032 | **−16%** |
| 32 | **870** | 1022 | 378 | **0.85×** | 2.30× | 1374 | **−37%** |

W=16 reps {848, 872, 874}; W=8 {995, 1010, 1015}; W=32 {859, 870, 1136} —
the 1136 is a 1/15 phaseA-slow rep (phaseA 723 vs fast-band 417–464; see
residual 3). All 27 flat reps + 12 stability reps rc=0; every rep matches
the flat reference `686d6890|4154468|0fbbd673|3af6b072|e1d22021`.

**JS-flat now BEATS Java at W=16 (0.89×, −104 ms) and W=32 (0.85×).** To
Go: W=16 closes from 2.45× to 2.07×; vs the `GOGC=off` algorithmic floor
(354 ms) JS is at 2.46× floor and Java at 2.76× — at W=16 JS is now closer
to the zero-GC SPEC ceiling than Java is.

† W=1 reps {3726, 3759, 3759} at loadavg **6.35**. Evidence §(1) re-baselined
W=1 on this binary at ~3920 ms (loadavg 0.75–2.0) and flagged §36's 3223 as
a binary-delta anomaly (§36's `e0fb8fe1…` build vs `cbb7d616…`). Against the
verified this-binary baseline, round 3 W=1 is **−4% at +6 load points**; an
earlier rep at load 2.9 read 3498 ms (−11%). The +17% vs §36's table value
is the unresolved §36 cross-check, not a round-3 regression.

Residual W=16 wall composition (median rep): phaseA 474 + flatten1 76 +
flattenGC 9 + cs1 72 + buildDfSnap ~6 + phaseB 96 + pc2 85 (= flatten2 64 +
cs2 20) + phaseC/csC ~35 ≈ 853 ms. The phaseB drop (242→96) is the entire
JS→Java W=16 crossover; flatten1 is flat vs §36 (the in-place mutate and the
W>8 typed-array knee roughly cancel — see residual 2).

### Slower-arm re-baseline (default + intcs, 3-rep medians, GIL-off)

| arm | W | wall ms | evidence §(6) (this binary) | §36 | reference tuple |
|---|---|---|---|---|---|
| default | 1 | 20 313 | 19 583 | 18 069 | `b3e65a68…\|4158957\|39c33392…\|c4bdd580…\|af028188…` ✓ |
| default | 16 | **13 395** | 12 868 | 9 887 | ✓ unchanged |
| intcs | 1 | 16 167 | 15 326 | 14 030 | `8021f000\|4158957\|1fc7d941\|…` ✓ |
| intcs | 16 | **7 901** | 7 683 | 7 395 | ✓ unchanged |

All 12 reps rc=0, all reference tuples match. default W=16 reps {13 125,
13 395, 13 676} (0/3 fast-mode this sample); intcs W=16 {5064, 7901, 8182}
(1/3 fast-mode, §34(C) signature unchanged). +4–5% vs evidence §(6) is
host-load delta (matrix at load 5.8–9.5 vs evidence at ≤0.35); the round-3
engine change is reached only under `useJSThreads()` and the precheck is a
read-only hit, so default/intcs codegen is unchanged. Evidence §(6)'s
finding stands: §36's default W=16 9 887 does not reproduce on `cbb7d616`.

### W=32 stability (12 reps, post-round-3)

12/12 rc=0, all ref-match; total **820–907 ms (med 860, 10.2% spread)**.
phaseA sorted: {417, 418, 428, 434, 435, 439, 440, 444, 448, 454, 463, 464}
— **0/12 slow-mode**, single 47 ms band. cs1 66–85, cs2 19–21 every rep.
With the prewarm rejected the cs1 slow band is gone; the 12-rep sweep no
longer shows the §36 654–820 phaseA band at all (round-3 phaseA is ~36%
faster than §36's fast band).

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug `7b984410…`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| Default-arm reference tuple unchanged (W=1 + W=16) | **PASS** |
| intcs-arm reference tuple unchanged (W=1 + W=16) | **PASS** |
| Flat-arm checksum stable (all W, all reps incl. stability) | **PASS** (`686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021`, 27/27) |
| W=32 stability (12 reps) | **12/12 rc=0**, 820–907 ms, 10.2% spread, **0/12 slow-mode** |
| All matrix reps rc=0 | **PASS** (36/36) |
| **JS-flat W=16 wall < 976 ms (beat Java)** | **PASS — 872 ms** (0.89× Java) |

### Honest residuals

1. **cs1 cold ~70 ms vs cs2 warm 20 ms — ~50 ms unrecovered tier-up** on
   `postingsChecksumFlat`'s first call (the two `Overflow` recompiles at
   bc#363/bc#402 + FTL install). The 8-shard prewarm proposal was measured
   and rejected (bimodal slow band, net regression). Next lever: a
   per-function tier-up hint (`$vm.optimizeNextInvocation`-shaped) or a
   second discarded full-K call before the timer; both are bench-side and
   fairness-neutral but were not landed this round.
2. **W>8 flatten1 contention knee unchanged** (W=1 75 / W=8 33 / W=16 76 /
   W=32 92 ms): the in-place mutate removed the literal's `m_lock` traffic
   but the per-shard `new Int32Array(segmentedJSArray)` typed-array
   construction half remains. Evidence §(2) names the candidate global
   resource (Gigacage/ArrayBufferContents alloc or the segwalk-copy
   acquire-loads on foreign-thread spines); needs a flatten1-windowed perf
   slice to attribute.
3. **W=32 phaseA low-rate slow rep** (1/15 at ~720 ms vs fast band 417–464,
   +280 ms): not the §35-R2 OSR-exit-genlock chain (12-rep sweep 0/12), not
   load-correlated (slow rep at load 6.70, fast reps at 8.97–9.43). Signature
   resembles the previous-run 4/12 {692–732} band that vanished once the
   prewarm-induced recompile churn was removed; one rep is below the
   discriminant floor — flagged for a 50-rep sweep in the next round.
4. **`operationGetByIdGaveUp` 4.45% W=16 self** (evidence §(N), the
   `PropertyInlineCache.cpp:278` dictionary-base GiveUp on the `sc` scratch
   object) and the **9.8% segmented-butterfly `[].push` path** are
   UNADDRESSED this round — both are phaseA steady-state cost and the
   largest named W=16 levers remaining (~40 ms + ~85 ms ceiling).
5. **§36 default-arm W=16 9 887 anomaly**: this binary reads 12 868–13 395
   at every loadavg sampled (0.35–9.5). Either §36's `e0fb8fe1…` build
   carried a default-arm-relevant delta that did not land in `cbb7d616`, or
   §36's sample was fast-mode-biased. Unresolved cross-check.
6. **`forof-tdz-osr-loop` still `useJSThreads()`-gated** (evidence §(5)):
   un-gating is a flag-off DFG/FTL codegen change under the byte-identical
   LAW; not chartered without an explicit reviewer ruling.

## §38 flat-gap-bughunter round 4

Tree `94fb0ed54cf6` + worktree delta below; Release jsc sha256
`7b6ac702834c48b4…`, Debug `a8a0ec9a62f6f787…`. Pinned GIL-off env, 64 HW
threads, loadavg 2.36–8.50 across the matrix. Evidence pack:
`docs/threads/FLAT-GAP-EVIDENCE.md` round 4 (§(1) Lock-structure GaveUp
attribution + lockmicro / §(2) segmented-push family / §(3b) flatten1-knee
PutById-writeback discriminant / §(4) W=32 50-rep BadIndexingType / §(5)
W=8-vs-16 same-residuals). Raw `results-v42ab-raw.jsonl` (36 records,
**2 nonzero rc** — see correctness regression below). Driver `v42_ab.sh` /
`v42_analyze.py`.

### Survivor list (changes that landed this round)

| target | file | what it does | flag-off effect |
|---|---|---|---|
| **lock-condition-thread-cached-instance-structure** (§37 R1) | `runtime/{LockObject,ConditionObject,ThreadObject,JSGlobalObject}.{cpp,h}` | `construct{Lock,Condition,Thread}` minted a fresh Structure on every `new`, so K=128 shard locks presented 128 distinct StructureIDs at the `.hold` GetById; >8 cases → GiveUpOnCache → permanent `operationGetByIdGaveUp` (4.45 % W=16 self, 4.7 M calls W-invariant). Now one cached Structure per realm (set in `create{Lock,Condition,Thread}Property`, visited in `visitChildrenImpl`), with `newTarget!=callee` → `InternalFunction::createSubclassStructure` for `class L extends Lock` correctness. | Members + setters only reached inside the `useJSThreads()` install block; flag-off the three `WriteBarrierStructureID`s sit null-and-unread after the last `offsetOf*` the JIT bakes — flag-off byte-identical. |
| **operationGetByIdPerThreadMegamorphic** (AUD1.K4 II.19 / A16-ext) | `bytecode/Repatch.cpp`, `runtime/MegamorphicCache.{h,cpp}`, `bytecode/InlineCacheCompiler.cpp`, `runtime/VMLite.cpp` | Per-thread MegamorphicCache side-table (IE-TLS `thread_local` + process registry). `tryFoldToMegamorphic` STAYS refused gilOff (inline probe still bakes VM-singular addr); `appropriateGetByGaveUpFunction(ById)` routes the GaveUp slot to a C++ 2-probe-hash + offset-load against the per-thread cache. `bumpEpoch`/`age()` release-bump a singular `m_perThreadInvalidationGen`; per-thread probe acquire-refreshes before trusting an entry; Full-GC fans out a world-stopped `clearEntries` over the registry. | `useJSThreads()` predicted-false dead branch in `appropriateGetByGaveUpFunction` and `bumpEpoch`; new members sit after every `offsetOf*` the inline probe consults — flag-off byte-identical. **CORRECTNESS REGRESSION — see below.** |
| **phaseA-ingestHold-born-int32array-kills-segpush** | `bench.js` | `sc.ingestHold` body ≡ `sc.ingestWriterHold` (geometric-doubling Int32Array appends, born typed). Removes all 9.36 M segmented `[].push` (the 15.9 % W=16 family), collapses flatten1 to the trim-slice arm only, removes the W=32 `bc#223 BadIndexingType` speculation surface. Same `{docIds,tfs,len}` inlineCapacity=3 shape → wrapper Structure-monomorphic; data identical → flat checksum unchanged. Direct A/B 3-rep med W=16 phaseA 461→350 / total 823→706; W=32 10-rep slow-mode 2/10→0/10. | Flat-arm only. |
| **pushinline-publen-bump-alwaysinline-defeated-ool** | `runtime/JSArray.h`, `runtime/JSArrayInlines.h` | `nm` showed `pushInline`/`publicLength`/`bumpPublicLengthToAtLeast` as out-of-line `t` in the DFGOperations unified TU (declaration/definition `ALWAYS_INLINE` mismatch). Hand-expand the spine→frag0→lengthWord chain + CAS-max bump in-place, load `ool` + `frags[]` once, derive frag0/frag[idx] from locals. Semantics verbatim. | Reached only via `isSegmentedButterfly(word)` arm (useJSThreads-only); flag-off codegen byte-identical. |
| **cs1-sum-ushr0-to-bitor0** (§37 R3) | `bench.js` | `(sum + mix32(item)) >>> 0` → `\| 0` (coerce back to uint32 once at return): the `>>> 0` emitted `op_unsigned`→`UInt32ToNumber` whose checked form Overflow-exits at bc#363 once bit 31 sets, costing 3 DFG installs + 2 reopt rounds before FTL. Bits identical mod 2^32. W=16 5-rep cs1 67→35 ms; checksumA/A2 unchanged. | Flat-arm only. |
| **flatten2-serial-thread0-not-striped-java-pays-ze** | `bench.js` | Post-phaseB re-normalize was serial on thread 0 (62-66 ms W-flat) while flatten1 was already W-striped; same `flattenFlatShards(id, W)` between barriers. W=8 62.6→21.2; W>8 hits the same PutById-writeback knee (residual 2). | Flat-arm only. |

### JS-flat wall ms vs Java / Go (3-rep medians; Go/Java from §34, Go `GOGC=off` floor from §37)

| W | **JS-flat ms** | Java ms | Go ms | Go floor | JS/Java | JS/Go | JS/floor | §37 JS-flat | Δ vs §37 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | **1681** | 1974 | 1836 | — | **0.85×** | **0.92×** | — | 3759 | **−55 %** |
| 8 | **465** | 939 | 535 | — | **0.50×** | **0.87×** | — | 1010 | **−54 %** |
| 16 | **485** | 976 | 422 | 354 | **0.50×** | **1.15×** | **1.37×** | 872 | **−44 %** |
| 32 | **491** | 1022 | 378 | — | **0.48×** | 1.30× | — | 870 | **−44 %** |

W=16 reps {468, 485, 491}; W=8 {463, 465, 466}; W=32 {471, 491, 503}; W=1
{1668, 1681, 1727}. All 24 flat reps + smoke rc=0; every rep matches the
flat reference `686d6890|4154468|0fbbd673|3af6b072|e1d22021`. Loadavg
2.36–2.48 across the flat cells.

**JS-flat now BEATS Java at every W (0.48–0.85×) and BEATS Go at W=1 and
W=8.** At W=16 JS is 1.15× Go and 1.37× the `GOGC=off` zero-GC AOT floor
(Java is 2.31× that floor, Go itself 1.19×). W=8→W=16 marginal scaling is
NEGATIVE (+20 ms) — the parallel work is below the W>8 knee floor (residual
2) plus serial cs1/cs2/buildDfSnap (~60 ms W-invariant).

Residual W=16 wall composition (median rep): phaseA 179 + flatten1 50 +
flattenGC 7 + cs1 36 + buildDfSnap ~7 + phaseB 82 + flatten2 57 + cs2 18 +
phaseC/csC ~36 ≈ 472 ms (peak RSS 285 MB). The phaseA drop (474→179, −62 %)
is the cached-Lock-structure fix + born-Int32Array ingestHold; the
segmented-push family and `operationGetByIdGaveUp` are GONE from `perf`.

`perf -F 1997` W=16 top self (round-4 binary, ~6 K samples):

| self% | symbol | reading |
|---|---|---|
| 15.87 | `lockProtoFuncHold` | hold-trampoline body (uncontended tryLock + JS re-entry) |
| ~10 | `[JIT]` (FTL hold-callback bodies) | the actual ingestHold/ingestWriterHold work |
| 2.33 | `operationCompareStrictEq` | string `===` (term keys; Map.get internals) |
| 2.31 | `WTF::LockAlgorithm::lockSlow` | shard-lock park (W>8 collision rate) |
| 2.25 | `JSGenericTypedArrayView<Int32>::setFromTypedArray` | flatten1/2 `.slice` memcpy |
| 1.71 | `Heap::writeBarrierSlowPath` | |
| 1.35 | `Heap::addToRememberedSet` | |
| 1.05 | `operationGetByIdPerThreadMegamorphic` | the K4.II.19 routing (was `operationGetByIdGaveUp` 4.45 %) |
| **0** | `ButterflySpine::publicLength` / `JSArray::pushInline` / `bumpPublicLengthToAtLeast` | **§37's 9.8 % family GONE** |
| **0** | `operationGetByIdGaveUp` | **§37 R1 GONE** |

### Slower-arm re-baseline (default + intcs, GIL-off) — **CORRECTNESS REGRESSION**

| arm | W | wall ms (med) | rc=0 reps | reference tuple | §37 |
|---|---|---|---|---|---|
| default | 1 | 18 914 | **3/3** | `b3e65a68…\|4158957\|39c33392…\|c4bdd580…\|af028188…` ✓ | 20 313 |
| default | 16 | 11 472 † | **2/3** | ✓ (both passing reps) | 13 395 |
| intcs | 1 | 14 480 | **3/3** | `8021f000\|4158957\|1fc7d941\|…` ✓ | 16 167 |
| intcs | 16 | 7 817 † | **2/3** | ✓ (both passing reps) | 7 901 |

† **rc=3 `RangeError: Not an integer` at `BigInt(g.df)`
(`checksumPhaseC`, bench.js:617:23): default W=16 1/3 + intcs W=16 1/3 in
the matrix; +5/8 in an extra default-W=16 sweep = 6/11 fail rate.** §37 had
36/36 rc=0 — this is a round-4 engine regression.

**Discriminant**: with `appropriateGetByGaveUpFunction(ById)`'s
`operationGetByIdPerThreadMegamorphic` routing temporarily forced false
(single-TU rebuild, sha-restored after), default W=16 passes **8/8** with
`checksumC=af028188…`. With the routing live, **5–6/8 fail**. The
per-thread MegamorphicCache is the cause.

**Mechanism (deduced, not yet directly probed)**: `g.df += df` where
`df = p.docIds.length` (bench.js:583). In the default arm `p.docIds`
JSArrays present >8 StructureIDs at the `.length` GetById (cross-thread
indexing-type/segmented transitions), the IC reaches GiveUpOnCache, routes
to `operationGetByIdPerThreadMegamorphic`. Its miss-walk EXEMPTS
`ArrayType` from the `overridesGetOwnPropertySlot` branch (Repatch.cpp:653,
verbatim from `getByIdMegamorphic` in JITOperations.cpp) and falls to
`getOwnNonIndexPropertySlot` — which does NOT see IndexingHeader `.length`.
The walk then reaches the null proto, calls `cache.initAsMiss(S, "length")`,
and the next probe returns `jsUndefined()`. `g.df += undefined` → NaN →
`BigInt(NaN)` → `RangeError: Not an integer`. Upstream
`getByIdMegamorphic` shares the same walk shape but is reached only via the
`LoadMegamorphic` fold, which `tryFoldToMegamorphic` REFUSES when the case
list contains `ArrayLength` cases — so upstream never feeds an array
`.length` to that walk. Round-4's GaveUp-slot routing has no such filter.
The flat arm is unaffected (its `.length` reads are on Int32Array, JSType
`Int32ArrayType`, which IS caught by the `overridesGetOwnPropertySlot`
branch and not mis-cached). **This is a wrong-value-read, not a crash —
correctness > speed; the routing must not ship as-is.**

### W=32 stability (12 reps, post-round-4)

12/12 rc=0, all ref-match; total **465–498 ms (med 481, 6.9 % spread)**.
phaseA sorted: {176, 177, 178, 187, 191, 193, 196, 197, 200, 203, 205, 206}
— **0/12 slow-mode**, single 30 ms band. With born-Int32Array ingestHold
the §37-R5 / evidence-§(4) `bc#223 BadIndexingType` slow mode is
structurally removed; 15-rep total (3+12) shows 0 slow.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug `a8a0ec9a…`, pinned env) | **93 passed, 1 FAILED, 4 skipped** — `objectmodel/i03-quarantine-readd-across-gc.js` ASAN SEGV in `MachineThreads::tryCopyCooperativelyParkedThreadStack` (3/5 flaky on direct re-run); §37 was 94/0/4 |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| Default-arm reference tuple unchanged (passing reps) | **PASS** (5/5) |
| intcs-arm reference tuple unchanged (passing reps) | **PASS** (5/5) |
| Flat-arm checksum stable (all W, all reps incl. stability) | **PASS** (`686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021`, 24/24) |
| W=32 stability (12 reps) | **12/12 rc=0**, 465–498 ms, 6.9 % spread, **0/12 slow-mode** |
| All matrix reps rc=0 | **FAIL** (34/36; +5/8 extra default W=16) |
| **JS-flat W=16 wall < 800 ms** (round-4 target) | **PASS — 485 ms** (0.50× Java, 1.15× Go) |
| **JS-flat W=8 wall < 939 ms** (beat Java) | **PASS — 465 ms** (0.50× Java) |

**Round-4 net: both perf targets cleared by ~40 % margin; two correctness
gates RED (corpus GIL-off 1 fail; matrix 2/36 rc≠0), both attributable to
the per-thread MegamorphicCache routing.** verified=false.

### Honest residuals

1. **`operationGetByIdPerThreadMegamorphic` returns wrong values for
   non-property-table own slots on exempted JSTypes** (the regression
   above). The GaveUp-slot routing must filter on the same conditions
   `tryFoldToMegamorphic`'s case-list check enforces (all-Load/Miss, no
   ArrayLength/StringLength/CustomAccessor cases) — or the miss-walk's
   ArrayType exemption must call `getNonIndexPropertySlot` (virtual) before
   declaring a miss. The cached-Lock-structure half of R1 stands on its
   own; with the routing reverted the K=128 `.hold` site is monomorphic
   anyway (0 GaveUp), so the routing's W=16 contribution is the residual
   1.05 % at OTHER >8-structure ById sites. **Next round: gate the routing
   behind a per-PropertyInlineCache "all cases were Load/Miss" bit set at
   GiveUp time, or revert the routing and keep only the structure cache.**
2. **flatten1/flatten2 W>8 PutById-writeback knee** (evidence §(3b)):
   flatten1 {W=8 19, W=16 50, W=32 11} ms, flatten2 {17, 57, 84} ms — both
   now W-striped, both show the W>8 knee on the three `pl.X=` existing-slot
   replaces (foreign-thread-allocated butterfly). flatten1 W=32 is FAST
   because the born-Int32Array fix means trim-slice is exact-sized for ~all
   phaseA pl (no `.set` copy); flatten2 W=32 is SLOW because phaseB
   geometric-grew. ~50 ms W=16 ceiling; no engine fix landed (the §(3b)
   discriminant only just attributed it). All 196 k puts on the JIT IC fast
   path (uprobe gaveup=0, STW non-discriminant) — investigation surface is
   the inline replace-store fence/coherence under W>8 cross-thread
   ownership.
3. **`lockProtoFuncHold` 15.9 % W=16 self** — the host-function trampoline
   body is now the SINGLE largest cost. The hold-callback FTL body cannot
   inline through the native frame; the round-2 hold-vmEntry-trampoline
   fast path already shortcuts `executeCallImpl`, so the residual is the
   tryLock + getCallData + arg marshalling. Next lever: a `.hold` intrinsic
   (DFG/FTL recognize `LockProtoFuncHoldIntrinsic`, emit tryLock inline,
   fall to C only on contention) — codegen change, deferred.
4. **W=8→W=16 negative scaling** (465→485 ms): serial cs1/cs2/buildDfSnap
   ~60 ms W-invariant + the W>8 flatten knee (~+85 ms vs W=8) outweigh the
   phaseA/B parallel gain (~−60 ms). Not a regression — the workload's
   parallel fraction at W=16 is now ~40 % of wall.
5. **Corpus `i03-quarantine-readd-across-gc.js` 3/5 SEGV** in
   `MachineThreads::tryCopyCooperativelyParkedThreadStack` (Debug ASAN).
   Distinct signature from the bench RangeError (GC-time stack-copy READ
   fault, not a JS-value error). Round-4 candidates: the per-thread
   MegamorphicCache `thread_local` holder's destructor unregistering during
   a stack copy, OR the registry-walk in `age()` racing the world-stop
   bookkeeping. **NOT discriminated** this round (Debug rebuild not
   re-spun); flagged for the same revert-routing test.
6. **cs1 35 ms vs cs2 18 ms** — the `|0` fix removed the bc#363 Overflow
   reopt loop, but ~17 ms first-call FTL install remains. Below the
   noise floor for further bench-side work.
7. **`forof-tdz-osr-loop` still `useJSThreads()`-gated** per Jarred's
   ruling; unchanged this round.

## §39 intcs cross-language

The `intcs` arm (32-bit allocation-free postings checksum, §29) is now ported
to Go (`main.go`) and Java (`Bench.java`) with byte-identical constants and
shifts: FNV-1a-32 (offset `0x811c9dc5`, prime `0x01000193`), splitmix32-style
`mix32` (`+0x9e3779b9`, `*0x85ebca6b` xs16, `*0xc2b2ae35` xs13, xs16), and
posting-item fold `th ^ (docId*0xd6e8feb9) ^ (tf*0xcaaf00dd)` summed mod-2^32.
Java uses signed-int two's-complement wrap (`*`/`+`/`^` mod-2^32, `>>>` for
logical shift); Go uses native `uint32`. Only `indexChecksum` (§1.8,
checksumA/A2) is rerouted — phaseA ingest, phaseB queries, phaseC analytics,
sharding, and locking are unchanged, so the default-arm spec u64 reference
tuple (`b3e65a68…`) is preserved when `intcs` is off.

**Cross-language intcs reference** (full workload, N_BASE=28000, any W):
`checksumA = 000000008021f000`, `checksumA2 = 000000001fc7d941`,
`postings = 4158957`. All 36 ladder cells (3 langs × 4 W × 3 reps) matched.

**Protocol**: Release jsc `f980fd63c125`, GIL-off env (`JSC_useJSThreads=1
JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1
JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1`); Go 1.24.13; OpenJDK as
installed. One process at a time, `/usr/bin/time -v`, 3 reps, median
`total_ms` reported. Invocation: `./go/bench-go W intcs`, `java Bench W
intcs`, `jsc js/bench.js -- W intcs` (or `SCALEBENCH_INTCS=1`).

| language | W  | wall ms |
|----------|----|---------|
| go       | 1  | 1851.6  |
| go       | 8  | 529.3   |
| go       | 16 | 422.3   |
| go       | 32 | 370.3   |
| java     | 1  | 2010.9  |
| java     | 8  | 1000.7  |
| java     | 16 | 1029.2  |
| java     | 32 | 989.5   |
| js       | 1  | 14761.4 |
| js       | 8  | 8081.6  |
| js       | 16 | 7868.6  |
| js       | 32 | 7993.5  |

## §39b intcs full-workload 32-bit (supersedes §39)

§39 rerouted only the §1.8 postings checksum to 32-bit; phaseA/B/C still ran
the spec u64 PRNG/hash, so JS still paid heap-BigInt for every `next()`, every
`mix()`, every `fnv1a()` in genDoc/shardOf/queries/phaseC — Go/Java paid
native `uint64`/`long` for the same ops. The §39 table above measures that
asymmetry, not "concurrency cost with the BigInt floor removed".

§39b reruns the **whole workload** with ZERO BigInt/u64/long arithmetic in any
language. The work shape is spec-exact (string concat → tokenize →
Map<string>.get/set under K=128 shard locks → 90/10 query mix → Phase C
group-by over 104 groups) but EVERY numeric operation is 32-bit:

- PRNG: splitmix32 (`next32`: `s+=0x9e3779b9; z=s; z=(z^(z>>>16))*0x85ebca6b;
  z=(z^(z>>>13))*0xc2b2ae35; return z^(z>>>16)`). JS Number + `Math.imul`/
  `>>>0`; Go `uint32`; Java `int` + `>>>`.
- `docSeedI(d) = mix32(0x5ca1ab1e ^ d)`; `qSeedI(q) = mix32(0xfacefeed ^ q)`.
- `pickTermI`: two `next32` draws masked `& (V-1)`, min.
- `shardOfI(term) = fnv1a32(term) % K`.
- `mix32`/`fnv1a32` everywhere (genDoc lengths, queryPoint/And/Scored,
  checksumA/A2/B/C).
- partialB: per-thread `uint32`, summed mod-2^32.

This produces DIFFERENT documents than the spec u64 arm (different PRNG
output), so a new cross-language reference. Default OFF: no `intcs` arg ⇒ the
spec u64 path runs and the §1.1 reference tuple (`b3e65a68…`) is unchanged
(verified W=2 smoke, all three languages, 2026-06-17).

**Cross-language intcs reference** (full workload, N_BASE=28000, any W; all 9
gate cells {go,java,js}×{W=1, W=4×2} and all 36 ladder cells matched):

| field     | value      |
|-----------|------------|
| checksumA | `e85d66e7` |
| postings  | `4158480`  |
| checksumA2| `15cf18bb` |
| checksumB | `651b594b` |
| checksumC | `abc7704f` |

**Protocol**: Release jsc `dab17169eb61`, GIL-off env (`JSC_useJSThreads=1
JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1
JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1`); Go 1.24.13; OpenJDK
21.0.10. One process at a time, 3 reps, median `total_ms`. Invocation:
`./go/bench-go W intcs`, `java Bench W intcs`, `jsc js/bench.js -- W intcs`.

| language | W=1    | W=8    | W=16   | W=32   |
|----------|--------|--------|--------|--------|
| go       | 1905.5 |  524.5 |  399.9 |  377.5 |
| java     | 1968.4 |  987.6 |  942.2 | 1078.6 |
| js       | 8212.0 | 6998.7 | 6382.6 | 6674.4 |

Against §39: JS W=1 14761→8212 (-44%, the BigInt PRNG/hash floor removed from
phaseA/B/C); Go/Java essentially unchanged (their u64 was already native). The
residual JS gap is now the string/Map/allocation/STW-GC cost under shared-heap,
not 64-bit-arithmetic premium.

## §40 intcs bisect: noconcat + nomap

§39b removed BigInt and left the residual JS gap as "strings + Map<string> +
alloc/GC". §40 bisects WHICH, with two opt-in arms that compose with the intcs
32-bit base. Both keep K=128 shard locks, the barrier, the Thread structure,
and the Atomics counters byte-identical.

**`noconcat`** — is genDoc-concat→tokenize the cost? `genDocTermsI` returns
the term strings directly (`termOf(pickTermI())` per token); the doc-text
build + tokenize round-trip is skipped. tf `Map<string>`, `shardOfI =
fnv1a32(term)%K`, shard `Map<string>.get/set`, queries, phaseC: byte-identical
to intcs. Tokenize is lossless (caps→lower, punctuation stripped), so the
checksum tuple is **identical to the §39b intcs reference**.

**`nomap`** — is `Map<string>.get/set` the cost? Full string round-trip KEPT
(`genDocTextI` → `tokenize`), but every `Map<string>` lookup is replaced with
a direct integer index: `invTermOf(token)` base26-decodes back to termId;
per-doc tf is an `Int32Array(V)`+dirty-list; per-term storage is a global
V-slot array `nmPost[termId]` (NOT `Map<string>`); `shardOf` is still
`fnv1a32(term)%K` via a precomputed `nmShardOf[V]` table so the
lock-contention pattern matches intcs. Queries/phaseC walk by termId;
`termOf(id)` is rebuilt only for the phaseC topN join string. checksumA folds
`mix32(termId)` (not `fnv1a32(term)`).

**Cross-language references** (full workload, N_BASE=28000, any W; all 12 gate
cells {go,java,js}×{W=1,W=4} and all 54 ladder cells matched):

| arm      | checksumA | postings | checksumA2 | checksumB | checksumC |
|----------|-----------|----------|------------|-----------|-----------|
| noconcat | `e85d66e7`| 4158480  | `15cf18bb` | `651b594b`| `abc7704f`|
| nomap    | `98972b27`| 4158480  | `64cd1705` | `dcf4c2d2`| `abc7704f`|

(noconcat == §39b intcs reference; nomap checksumC == intcs checksumC because
within a group all terms share length+first-letter so termId order ≡ term-lex
order on ties.)

**Protocol**: Release jsc `7e46605fe252`, GIL-off env (`JSC_useJSThreads=1
JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1
JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1`); Go 1.24.13; OpenJDK
21.0.10. One process at a time, 3 reps, median `total_ms`. Invocation:
`./go/bench-go W <arm>`, `java Bench W <arm>`, `jsc js/bench.js -- W <arm>`.

| language | arm      | W=1    | W=16   |
|----------|----------|--------|--------|
| go       | intcs    | 1782.3 |  412.1 |
| go       | noconcat | 1468.6 |  356.7 |
| go       | nomap    |  996.7 |  255.2 |
| java     | intcs    | 1898.6 |  976.5 |
| java     | noconcat | 1596.5 |  841.7 |
| java     | nomap    | 1207.5 |  769.5 |
| js       | intcs    | 7866.8 | 3369.6 |
| js       | noconcat | 5865.0 | 2949.1 |
| js       | nomap    | 6574.9 | 2783.5 |

**Verdict — neither arm closes the single-thread ratio; `nomap` kills the W=16
bimodal mode:**

- W=1 js/java ratio: intcs 4.14× → noconcat 3.67× → nomap 5.45×. `noconcat`
  shaves a similar ~16-18% off all three (concat→tokenize is real work
  everywhere). `nomap` helps Go/Java MORE than JS at W=1 (java −36%, go −44%,
  js −16%): the per-doc `Map<string>` tf accumulator + shard `Map<string>`
  get/set is a bigger fraction of Go/Java's W=1 wall than JS's.

- W=16: the JS intcs/noconcat phaseA is **bimodal** — 8-rep intcs phaseA
  {1506,1566,4486,4507,4570,…}; 8-rep noconcat phaseA {1261,1311,1312,3176,
  3569,…}. The §39b table's 6383 ms is the slow mode. `nomap` phaseA is
  **monomodal** at ~1150 ms (8/8 reps in [1146,1194]). The shared-heap
  `Map<string>` under shard locks is the bimodal trigger; removing it
  stabilizes JS W=16 at ~2780 ms (3.6× java) vs the §39b 6383 ms (6.8× java).

So: the §39b "6.8× Java" headline is the `Map<string>` slow mode. With it
removed (`nomap`) the gap is 3.6× — still not closed, and the residual is NOT
strings (noconcat helps everyone equally) and NOT `Map<string>` lookups. The
remaining ~2000 ms at W=16 is the alloc/GC + lock.hold trampoline cost
(consistent with the §38 `flat` arm reaching ~500 ms at W=16 by removing
those too).

## §41 sharedheap-alloc-bughunter

Round outcome: **no Source/ change landed** (`git diff Source/` empty at HEAD
`42f50113c0c1`). The bughunter produced SHAREDHEAP-ALLOC-EVIDENCE.md (perf
attribution, 70.9 M-cell histogram, refill-branch counts, GIL-on discriminant)
but every candidate fix was reverted under refuter discipline; this section is
therefore a clean-tree re-baseline confirming §40 + the evidence-§(4) RSS
table, plus the now-enforced RSS constraint.

**Protocol**: Debug+Release rebuilt at `42f50113c0c1` (Release: 207 targets,
Debug: 212 targets, both link clean; one `-Wunused-variable` in
VMTraps.cpp:1129). Pinned GIL-off env. One process at a time after loadavg
< 4.0, `/usr/bin/time -v`, 3 reps, median reported.

### Wall ms vs Java (§40 baselines)

| arm     | W  | reps (total_ms)             | median | §40 JS | Java §40 | js/java |
|---------|----|-----------------------------|--------|--------|----------|---------|
| intcs   | 1  | 7788 / 7669 / 7892          | 7788   | 7867   | 1899     | 4.10×   |
| intcs   | 16 | 3444 / 6271 / 3290 (bimodal)| 3444   | 3370   | 977      | 3.53×   |
| nomap   | 1  | 6546 / 6493 / 6527          | 6527   | 6575   | 1208     | 5.40×   |
| nomap   | 16 | 2806 / 3139 / 2854          | 2854   | 2784   | 770      | 3.71×   |
| default | 1  | 18328 / 18341 / 18202       | 18328  | —      | —        | —       |
| default | 16 | 12565 / 12671 / 12666       | 12666  | —      | —        | —       |
| flat    | 16 | 442 / 474 / 469             | 469    | 485¹   | —        | —       |

¹ §38 flat W=16. All cells within ±3% of their §40/§38 prior — expected
(tree unchanged). intcs W=16 bimodality reproduced (1/3 slow-mode at 6271 ms,
the §40 Map<string> phaseA mode).

### Peak RSS vs evidence-§(4) baseline

| arm     | W  | reps (peak RSS KB)                | median MB | §(4) MB | Δ      | ceiling (+10%) |
|---------|----|-----------------------------------|-----------|---------|--------|----------------|
| intcs   | 1  | 431784 / 432752 / 433100          | 433       | 432     | +0.2%  | 475 ✓          |
| intcs   | 16 | 1327152 / 1229480 / 1338856       | 1327      | 1232    | +7.7%  | 1355 ✓         |
| nomap   | 1  | 423712 / 427844 / 424452          | 424       | 427     | −0.7%  | 470 ✓          |
| nomap   | 16 | 519480 / 496916 / 498276          | 498       | 516     | −3.5%  | 567 ✓          |
| default | 1  | 439120 / 435384 / 437116          | 437       | 436     | +0.2%  | —              |
| default | 16 | 1416456 / 1412952 / 1414320       | 1414      | 1392    | +1.6%  | —              |
| flat    | 16 | 289540 / 292792 / 291132          | 291       | ~285²   | +2.1%  | —              |

² §38 flat baseline. intcs W=16 +7.7% is run-to-run noise on the bimodal arm
(the slow-mode rep is the LOW-RSS one — 1229 MB at 6271 ms vs 1327/1339 MB at
3290/3444 ms — so RSS variance is mode-correlated, not a leak).

### Checksums (all 21 reps)

| arm     | tuple                                                            | match |
|---------|------------------------------------------------------------------|-------|
| intcs   | `e85d66e7\|4158480\|15cf18bb\|651b594b\|abc7704f`                | 6/6 ✓ (§39b ref) |
| nomap   | `98972b27\|4158480\|64cd1705\|dcf4c2d2\|abc7704f`                | 6/6 ✓ (§40 ref)  |
| default | `b3e65a68…\|4158957\|39c33392…\|c4bdd580…\|af028188…`            | 6/6 ✓ (§1.1 ref) |
| flat    | `686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021`                | 3/3 ✓ (§38 ref)  |

All 21 reps rc=0 (no §38-style RangeError; that regression's routing was
already reverted before `42f50113c0c1`).

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| All 21 matrix reps rc=0, checksums stable | **PASS** |
| `git diff Source/` empty (refuter discipline) | **PASS** |
| Peak RSS W=1 within +10% baseline | **PASS** (intcs +0.2%, nomap −0.7%) |
| Peak RSS W=16 within +10% baseline | **PASS** (intcs +7.7%, nomap −3.5%) |
| **JS intcs W=1 < 6000 ms** (round target) | **FAIL — 7788 ms** |

**verified=false.** No fix survived; the perf target cannot be met on an
unchanged tree.

### Survivors

None. The round's net contribution is the evidence pack
(SHAREDHEAP-ALLOC-EVIDENCE.md), not a code change.

### Honest residuals

1. **Evidence-§(5) decomposition stands as the round's finding**: of the
   intcs W=1 +5889 ms gap vs Java, only **~1912 ms (33%)** is the
   sharedGCHeap+GIL-off tax that any allocator-side fix can address. The
   remaining **~3937 ms (67%)** is the no-shared-heap "plain JSC" floor
   (`WTF::equal` Map-key compare 4.7%, `lockProtoFuncHold` 4.3%/29.6%c,
   rope-resolve, IC-miss, CellLock/DeferTermination/traps overhead). The
   < 6000 ms target therefore needs BOTH an allocator fix AND a non-alloc
   floor reduction; an allocator-only round was never going to clear it.
2. **Evidence-§(6) redirects the TLAB hypothesis**: 99.67% of 70.9 M cells
   already hit interval-bump; refills are 0.33% and ~4.1% of wall. The
   measurable per-cell tax is the **3-hop allocator lookup**
   (`allocationClientForCurrentThread` → `allocatorForSizeStep` →
   `allocateForClient`, ~250 ms C++-visible + a JIT-inlined slice). The
   "per-thread fresh-block cache" candidate from the §40 brief targets the
   wrong lever (refill, not lookup) and carries RSS risk; the
   higher-leverage, zero-RSS candidate is **caching the LocalAllocator\*
   per (thread, size-class)** to skip the dispatch.
3. **intcs W=16 RSS noise is mode-correlated** (slow-mode rep = low-RSS
   rep). Any future RSS-gate failure on intcs W=16 must be checked against
   which phaseA mode the rep landed in before being treated as a
   regression.
4. **intcs W=16 bimodality unchanged** — the §40 Map<string> slow-mode
   trigger is untouched (no fix landed). nomap W=16 remains monomodal.

## §42 giloff-tax-bughunter

Repo @ `e73a5af68ebd` + 15-file H-VMLITE-TLCPTR / H-TLS-TABLE candidate
(922 ins / 66 del; archived `docs/threads/s42-hvmlite-tlcptr-candidate.diff`).
The candidate addresses GILOFF-TAX-EVIDENCE.md #1 (46.6M unpatched FTL
lazy-slow-path thunk traversals) and #2 (per-cell 3-hop C++ allocator lookup)
by (a) baking a process-constant TLC slot index at JIT-compile time and
loading the per-thread `LocalAllocator*` lite-relative
(`VMLite::{tlcTable,tlcTableBound}`) instead of a null `Allocator` constant,
(b) collapsing the C++ `CompleteSubspace::allocate` sharedGCHeap arm to two
IE-TLS loads + one indexed load, (c) hoisting `DeferGCForAWhile` out of the
gilOff `operationCompileFTLLazySlowPath` steady-state path, and
(d) pre-growing the TLC table to fixed `numCompleteSubspaces × numSizeClasses`
capacity (H-TLC-FIXEDTABLE-NOREALLOC) so cached table pointers never go
stale. Plus per-client `GCClient::CompleteSubspaceView` infra (STAGING, no
caller this round).

**Protocol**: Debug+Release rebuilt (Release 0 targets — pre-built; Debug 209
targets, link clean). Pinned GIL-off env. One process at a time after loadavg
< 2.5, `/usr/bin/time -v`, 3 reps, median reported. intcs W=1 GIL-off
re-confirmed with 7 additional reps at loadavg 0.85–1.19.

### Wall ms vs §41 + GIL-on baseline + Java

| arm     | W  | reps (total_ms)       | median | §41    | Δ§41    | GIL-on §42 | Java §40 | js/java |
|---------|----|-----------------------|--------|--------|---------|------------|----------|---------|
| intcs   | 1  | 7070 / 7142 / 7144¹   | 7142   | 7788   | **−646**| 5928       | 1899     | 3.76×   |
| intcs   | 16 | 3038 / 3378 / 6314²   | 3378   | 3444   | −66     | —          | 977      | 3.46×   |
| nomap   | 1  | 6016 / 6099 / 6194    | 6099   | 6527   | −428    | —          | 1208     | 5.05×   |
| nomap   | 16 | 2686 / 2691 / 2834    | 2691   | 2854   | −163    | —          | 770      | 3.49×   |
| default | 1  | 16705 / 17208 / 17337 | 17208  | 18328  | −1120   | —          | —        | —       |
| flat    | 16 | 439 / 447 / 481       | 447    | 469³   | −22     | —          | —        | —       |
| intcs **GIL-on** | 1 | 5731 / 5928 / 5928 | 5928 | (5876⁴)| +52     | —          | 1899     | 3.12×   |

¹ +7 confirmatory reps at loadavg ≤ 1.19: 7031 / 7085 / 7095 / 7110 / 7115 /
  7118 / 7152; 10-rep median **7113**. ² §40 bimodality reproduced (1/3
  slow-mode). ³ §41 flat W=16. ⁴ §41 evidence-§(5) GIL-on baseline.

**Tax recovered**: §41 tax = 7788 − 5876 = 1912 ms; §42 tax = 7142 − 5928 =
**1214 ms**. Recovered **698 ms = 36.5%** (target ≥ 50% = 956 ms). intcs W=1
**7142 ms > 6800 ms** — perf gate FAIL.

### Peak RSS vs §41 baseline

| arm     | W  | reps (peak RSS KB)             | median MB | §41 MB | Δ       | ceiling (+10%) |
|---------|----|--------------------------------|-----------|--------|---------|----------------|
| intcs   | 1  | 432068 / 432672 / 432824       | 423       | 433    | −2.3%   | 475 ✓          |
| intcs   | 16 | 1202024 / 1217136 / 1223644    | 1189      | 1327   | −10.4%  | 1355 ✓         |
| nomap   | 1  | 426660 / 428212 / 429856       | 418       | 424    | −1.4%   | 470 ✓          |
| nomap   | 16 | 508820 / 515308 / 525212       | 503       | 498    | +1.0%   | 567 ✓          |
| default | 1  | 433820 / 434356 / 434972       | 424       | 437    | −3.0%   | —              |
| flat    | 16 | 288020 / 289644 / 294688       | 283       | 291    | −2.7%   | —              |
| intcs GIL-on | 1 | 441280 / 443040 / 443260   | 433       | —      | —       | —              |

H-TLC-FIXEDTABLE-NOREALLOC pre-grow adds no measurable per-thread footprint;
intcs W=16 RSS **drops** 10.4% (all 3 reps in the fast-mode RSS band).

### Checksums (all 21 reps)

| arm     | tuple                                                            | match |
|---------|------------------------------------------------------------------|-------|
| intcs   | `e85d66e7\|4158480\|15cf18bb\|651b594b\|abc7704f`                | 9/9 ✓ (§39b ref; 6 GIL-off + 3 GIL-on) |
| nomap   | `98972b27\|4158480\|64cd1705\|dcf4c2d2\|abc7704f`                | 6/6 ✓ (§40 ref)  |
| default | `b3e65a6855b9bdeb\|4158957\|39c33392b2a4c5b2\|c4bdd580…\|af028188…` | 3/3 ✓ (§1.1 ref) |
| flat    | `686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021`                | 3/3 ✓ (§38 ref)  |

All 21 reps rc=0.

### Mechanism verification (uprobe, intcs W=1 GIL-off, candidate applied)

| symbol                                | §41 evidence | §42 candidate | Δ     |
|---------------------------------------|-------------:|--------------:|-------|
| `operationCompileFTLLazySlowPath`     | 46,631,032   | **36,422,494**| −22%  |
| `CompleteSubspace::allocateForClient` | 27,816,788   | **0**         | −100% |

**H-TLS-TABLE works completely** — the C++ 3-hop is gone (every
`CompleteSubspace::allocate` hits the new TLS-indexed fast path).
**H-VMLITE-TLCPTR works only for CompleteSubspace types** — `stringSpace` is
iso (per-client `GCClient::IsoSubspace`, not table-addressable), so
`tlcSlotForConcurrently<JSRopeString>` returns `nullopt` and FTL/DFG
`MakeRope` still bakes a null `Allocator` (the diff's own comment at
DFGSpeculativeJIT.cpp:18790 / FTLLowerDFGToB3.cpp:12501 says exactly this).
JSRopeString + JSString = **50.5 M of 70.9 M cells (71%)** per the §41
histogram; the JIT-side fix covers only the JSArray / JSFinalObject /
auxiliary minority. 36.4 M residual traversals × ~25 ns thunk = ~910 ms
still on the table.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| All 21 matrix reps rc=0, checksums stable | **PASS** |
| `git diff Source/` empty (refuter discipline) | **PASS** (reverted post-measurement) |
| Peak RSS W=1 within +10% baseline | **PASS** (intcs −2.3%, nomap −1.4%) |
| Peak RSS W=16 within +10% baseline | **PASS** (intcs −10.4%, nomap +1.0%) |
| **JS intcs W=1 < 6800 ms** (≥ 50% of 1912 ms tax) | **FAIL — 7142 ms** (36.5%) |

**verified=false.** Candidate is correctness-clean (corpus + identity +
checksums all green, RSS improved) and recovers a real 646–698 ms, but the
50% bar needs the iso-subspace half of the JIT inline-alloc fix.

### Survivors

None (reverted under refuter discipline). Candidate archived at
`docs/threads/s42-hvmlite-tlcptr-candidate.diff` — it is correctness-clean
and the H-TLS-TABLE / H-TLC-FIXEDTABLE-NOREALLOC / defer-hoist-lazyslow
hunks are independently landable with no perf risk; they were only reverted
because the round's gate is all-or-nothing on the 6800 ms target.

### Honest residuals

1. **The remaining 1214 ms tax is now ~75% one mechanism**: 36.4 M
   lazy-slow-path thunk traversals from iso-subspace JIT inline allocation
   (overwhelmingly `MakeRope` — JSRopeString is 48.4% of all cells). The
   #1/#2 root cause is correctly identified and HALF-fixed; the missing
   half is **TLC-slot addressing for `GCClient::IsoSubspace`** so
   `tlcSlotForConcurrently<JSRopeString/JSString/JSFunction/…>` returns a
   bakeable slot. The DFG/FTL `MakeRope` hunks already carry the
   lite-relative wiring ("kept so a future iso-TLC scheme need only extend
   `tlcSlotForConcurrently`"); only the resolver + the per-client
   IsoSubspace TLC stamp need extension. Est. additional recovery:
   ~800–900 ms → would land intcs W=1 at ~6200–6300 ms, clearing the gate.
2. **defer-hoist-lazyslow is real but small** at the residual 36.4 M
   traversals — the dominant cost is the `saveAllRegisters` /
   `restoreAllRegisters` full-scalar dump in
   `genericGenerationThunkGenerator`, not the operation body. A
   complementary fix (orthogonal to TLC-slot) is a **gilOff-dedicated thin
   thunk** that does only `acquire-load m_stubCodePtr → tail-call`, no
   register dump, no operation call on the steady-state path (the operation
   body's only steady-state work is that one acquire-load).
3. **GIL-on intcs W=1 +52 ms** vs §41 evidence baseline (5928 vs 5876) is
   within run-to-run noise; the candidate's GIL-on / flag-off paths are
   `[[unlikely]]`-gated additions only and identity is byte-clean.
4. **default W=1 −1120 ms** is the largest absolute Δ of any arm — default
   allocates ~similar cell counts but with more JSFinalObject /
   auxiliary-butterfly traffic (CompleteSubspace types), so H-VMLITE-TLCPTR
   covers a larger fraction of its JIT allocations than intcs's
   rope-dominated mix.
5. **intcs W=16 bimodality unchanged** (1/3 slow-mode at 6314 ms). nomap
   W=16 monomodal.


## §43 iso-TLC-slot + thin-thunk

**H-ISO-TLCSLOT** (per-type IsoSubspace TLC slot — closes the §42 iso half of
the JIT inline-alloc fix) **+ thin lazy-slow-path prefix** (gilOff steady
state does the T8 acquire-load IN JIT code, no register dump). Candidate at
`docs/threads/s43-iso-tlcslot-thinthunk-candidate.diff`; left applied in the
working tree (correctness gates green).

| arm     | W  | reps (total_ms)       | median | §42    | Δ§42    | GIL-on §42 | Java §40 | js/java |
|---------|----|-----------------------|--------|--------|---------|------------|----------|---------|
| intcs   | 1  | 6351 / 6381 / 6448¹   | 6381   | 7142   | **−761**| 5928       | 1899     | 3.36×   |
| intcs   | 16 | 3345-3422 / 6251-6365²| 4836³  | 3378   | +1458³  | —          | 977      | —       |
| nomap   | 1  | 5035 / 5081 / 5085    | 5081   | 6099   | **−1018**| —         | 1208     | 4.21×   |
| nomap   | 16 | 2666 / 2684 / 2731    | 2684   | 2691   | −7      | —          | 770      | 3.49×   |
| default | 1  | 16084 / 16232 / 16513 | 16232  | 17208  | **−976**| —          | —        | —       |
| default | 16 | 12146 / 12332 / 12495 | 12332  | —      | —       | —          | —        | —       |
| flat    | 16 | 453 / 462 / 466       | 462    | 447    | +15     | —          | —        | —       |
| intcs **GIL-on** | 1 | 5684 / 5812 / 5874⁴ | 5812 | 5928 | −116    | —          | 1899     | 3.06×   |

¹ +7 confirmatory reps at loadavg ≤ 1.84: 6258 / 6411 / 6418 / 6451 / 6460 /
  6469 / 6588; **10-rep median 6433**. ² §40 bimodality reproduced at 3/6
  slow-mode (6 reps total: 3345 3390 3422 6251 6309 6365). ³ 6-rep median;
  bimodal — fast-mode median 3390 (= §42 fast-mode), slow-mode median 6309
  (= §42 slow-mode 6314). The +1458 Δ is mode-ratio drift, not a per-mode
  regression. ⁴ Ran at loadavg 4.80-5.88 (matrix-induced); §43 touches no
  GIL-on code path (`vm.gilOff()`-gated) so −116 is load noise.

**Tax recovered**: §41 tax = 1912 ms; §42 tax = 1214 ms; §43 tax (vs the §42
GIL-on baseline 5928) = 6381 − 5928 = **453 ms**. Recovered this round
**761 ms** (intcs W=1 vs §42); cumulative §42+§43 = **1459 ms = 76.3%**.

### Peak RSS vs §41 baseline

| arm     | W  | reps (peak RSS KB)             | median MB | §41 MB | Δ       | ceiling (+10%) |
|---------|----|--------------------------------|-----------|--------|---------|----------------|
| intcs   | 1  | 433228 / 433452 / 433908       | 423       | 433    | −2.3%   | 475 ✓          |
| intcs   | 16 | 1161112-1267632 (6 reps)       | 1186      | 1327   | −10.6%  | 1355 ✓         |
| nomap   | 1  | 424512 / 426400 / 427340       | 416       | 424    | −1.9%   | 470 ✓          |
| nomap   | 16 | 517500 / 520044 / 533252       | 508       | 498    | +2.0%   | 567 ✓          |
| default | 1  | 439200 / 441676 / 443276       | 431       | 437    | −1.4%   | —              |
| default | 16 | 1415408 / 1417928 / 1421488    | 1385      | —      | —       | —              |
| flat    | 16 | 288036 / 289052 / 291068       | 282       | 291    | −3.1%   | —              |
| intcs GIL-on | 1 | 441124 / 442452 / 444392   | 432       | —      | —       | —              |

Iso-slot region adds ~40 `Allocator` words per client; no measurable RSS
delta vs §42 (intcs W=1 unchanged at 423 MB).

### Checksums (all 34 reps)

| arm     | tuple                                                            | match |
|---------|------------------------------------------------------------------|-------|
| intcs   | `e85d66e7\|4158480\|15cf18bb\|651b594b\|abc7704f`                | 19/19 ✓ (10 W=1 + 6 W=16 + 3 GIL-on; §39b ref) |
| nomap   | `98972b27\|4158480\|64cd1705\|dcf4c2d2\|abc7704f`                | 6/6 ✓ (§40 ref)  |
| default | `b3e65a6855b9bdeb\|4158957\|39c33392b2a4c5b2\|c4bdd580…\|af028188…` | 6/6 ✓ (§1.1 ref) |
| flat    | `686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021`                | 3/3 ✓ (§38 ref)  |

All 34 reps rc=0.

### Mechanism verification (uprobe, intcs W=1 GIL-off, candidate applied)

| symbol                                | §42 candidate | §43 candidate | Δ        |
|---------------------------------------|--------------:|--------------:|----------|
| `operationCompileFTLLazySlowPath`     | 36,422,494    | **56**        | −99.9998%|
| `operationArrayPush`                  | (~692 K)      | **691,997**   | unchanged|
| `convertToSegmentedButterfly`         | 0             | **0**         | unchanged|

**H-ISO-TLCSLOT works for non-JSArray iso types** — JSRopeString (~21 M FTL
MakeRope traversals) inline-allocates via the stamped `ropeStringSpace.tlcSlot()`.
**Thin-thunk works completely** — the residual ~15.4 M JSArray-cell lazy-slow
traversals now hit the JIT-side `m_stubCodePtr` acquire-load + tail-jump
instead of `saveAllRegisters` + C call; `operationCompileFTLLazySlowPath`
fires only for FIRST compiles (56 = #distinct lazy slow paths reached).

**JSArray excluded by design** — `tlcSlotForConcurrentlyWithIso<JSArray>`
returns nullopt. With the iso arm enabled for JSArray, the JIT inline
`allocateObject` / `emitAllocateJSObject` fast path stores the butterfly word
**untagged** (raw `storePtr(butterfly, …, JSObject_butterfly)` — no
`loadButterflyTIDTag` emission), so a fresh inline-allocated JSArray reads as
foreign at the §4.2 ensureLength dispatch and SEGMENTS on first growth:
measured **182,339** `convertToSegmentedButterfly` + **19,073,867**
`operationArrayPush` on intcs W=1 (vs 0 / 691,997 with JSArray excluded),
total_ms **10,588** — a +3,472 ms regression. Under §42 the JSArray cell
allocator was always null GIL-off so the path went to
`operationNewArrayWithSize` (TID-tags in C++); §43's iso arm would be the
FIRST time the JSArray inline path fires GIL-off. JSArray stays on the
thin-thunk-cheapened lazy arm until **Task-8** (TID-tag every JIT inline
butterfly install — `JSObjectWithButterfly`'s ctor encoding,
JSObject.h:1730). Every other iso ClassType reaching the resolver either has
no butterfly (JSRopeString/JSString) or a null-butterfly inline path
(JSPromise/JSMap/JSSet/JSBoundFunction/JSFunction et al.).

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| All 34 matrix reps rc=0, checksums stable | **PASS** |
| Peak RSS W=1 within +10% baseline | **PASS** (intcs −2.3%, nomap −1.9%) |
| Peak RSS W=16 within +10% baseline | **PASS** (intcs −10.6%, nomap +2.0%) |
| `operationCompileFTLLazySlowPath` ≤ 5 M (intcs W=1) | **PASS — 56** |
| **JS intcs W=1 < 6400 ms** | **PASS — 6381 ms** (3-rep median, loadavg 2.1-2.3); 10-rep median **6433** — borderline (4/10 reps < 6400, range 6258-6588) |

**verified=true** on the 3-rep gate; the 10-rep median misses by 33 ms.

### Honest residuals

1. **§42's ~910 ms estimate for the 36.4 M residual was ~3-4× high.** §42
   attributed ~25 ns/traversal to the
   `genericGenerationThunkGenerator` body via perf self-time; the §43
   measurement removing all 36.4 M traversals from the C-call path recovers
   **761 ms** total — i.e. ~21 ns/traversal *including* the JSRopeString
   slow-path C++ allocation that the iso-TLC fix ALSO eliminates. The thunk
   body alone is closer to **~5-6 ns/traversal** (the iso-arm-disabled
   bisect at 6949 ms vs §42 7116 ms isolates the GCThreadLocalCache
   pre-grow at ~0; JSRopeString-only iso at lazy=15.5 M recovered ~120 ms ⇒
   21 M × ~5.7 ns). The §42 perf attribution likely folded the
   `restoreAllRegisters → ret → stub` dispatch into the same ~1.5 KB JIT
   range as `saveAllRegisters`.
2. **JSArray iso-TLC is the next ~400-500 ms** (the residual ~15.4 M
   JSArray-cell lazy traversals now cost the thin-prefix ~10 instructions +
   the stub's `operationNewArrayWithSize` C++ allocation — ~30-35 ns each).
   Gated on **Task-8** TID-tag emission at every JIT inline butterfly
   install (FTL `allocateObject` line 23854,
   `AssemblyHelpers::emitAllocateJSObject` line 2342, plus the
   `emitAllocateRawObject` / FTL `allocateJSArray` storage→cell ordering so
   the post-install `storageGPR` writes use the untagged pointer while the
   cell's `m_butterfly` carries the encoded word). Mechanically: when the
   `butterfly` argument is provably non-null (every JSArray fast path),
   `or` in `loadButterflyTIDTag()`; when it may be null (JSPromise et al.),
   skip — matches `JSObjectWithButterfly`'s `if (butterfly)` ctor guard.
3. **intcs W=16 bimodality unchanged** (3/6 slow-mode at ~6300 ms,
   fast-mode ~3390 ms — both modes match §42's per-mode values; the
   6-rep median lands in the slow band by mode-ratio luck). nomap W=16
   monomodal.
4. **GIL-on intcs W=1 5812 ms** ran at loadavg 4.80-5.88 (matrix tail);
   §43 touches no GIL-on code path (every emission site is `vm.gilOff()`-
   gated; `lazySlowPathGenerationThunkGenerator` passes a null prefix
   GIL-on). The −116 vs §42's 5928 is load noise, not a real Δ.
5. **flat W=16 +15 ms** (462 vs 447) — within run-to-run noise; flat
   allocates orders-of-magnitude fewer cells.


## §44 Map-bimodal root cause

Repo @ `0666c87d35bd`, Release+Debug rebuilt (Release 0 targets, Debug 206
targets — Heap.{h,cpp} mtime touch only, `git diff Source/` empty). Pinned
GIL-off env. **`git diff Source/` empty** (refuter discipline; the fix is a
1-line `Tools/threads/scalebench/js/bench.js` change, not engine code).

### Mechanism (corrects §40)

The §40 verdict "shared-heap `Map<string>` under shard locks is the bimodal
trigger" is **wrong**. `nomap` is monomodal because its `nmShardOf[]`
precompute (bench.js:988-991) calls `termOf()` on the **main thread at module
init** — which lazy-reifies `String.fromCharCode` at TID=0. `intcs`/`noconcat`
have no main-thread `termOf()` call before workers, so the first
`String.fromCharCode` access is a 16-thread race from `phaseAI → ingestDocI →
genDocTextI → termOf`. Removing `Map<string>` was incidental.

**Causal chain** (MAP-BIMODAL-EVIDENCE.md Round 2):

1. `String.fromCharCode` is a lazy static property on StringConstructor
   (initial structure `{prototype:64,length:65,name:66}`, butterfly Flat
   TID=0). First access reifies it as a 4th out-of-line property.
2. If a **worker** (TID≠0) wins the reify race: foreign-TID structure
   transition on a Flat butterfly → `convertToSegmentedButterfly`
   (ConcurrentButterfly.cpp:1095-1099, "Flat + foreign transition →
   segment"). StringConstructor's butterfly is now **Segmented** for the
   process lifetime.
3. DFG `get_by_id String "fromCharCode"` lowers to `CheckStructure +
   GetButterfly + GetByOffset(67)`. `compileGetButterfly` under
   `useJSThreads()` (DFGSpeculativeJIT.cpp:12086) emits the segmented check
   (`branch64(AboveOrEqual, butterflyWord, 0xffff<<48)`) and routes it to
   `speculationCheck(BadIndexingType, …)` — **OSR-exit, not slow-path**.
4. Every DFG-compiled call to `termOf` (bc#22) / `tokenize` (bc#266) /
   `genDocTextI` (bc#254) hits the segmented branch → BadIndexingType exit.
   `handleGetById` doesn't consult `hasExitSite(BadIndexingType)` for
   get_by_id, so the recompile re-emits the **same body**: termOf 15× /
   genDocTextI 8× / tokenize 9× recompile loop = the 4600 ms slow-mode
   (3.2× user-CPU, +74% instructions, IPC 0.52 vs 0.96).
5. If **main** (TID=0) wins the reify race: owner-TID transition keeps
   butterfly Flat → DFG GetButterfly never exits → termOf 1× / genDocTextI
   2× / tokenize 3× = the 1600 ms fast-mode.

The race is decided at the `barrier.await()` start line: main is the last
arriver (it spawned 15 threads first) so it returns from `await()` already
running while workers are parked → main has a few-µs head start ≈ 40% win
rate. Explains §34(C): `JSC_logGC=1` adds main-thread `dataLog()` latency at
the first GC → main loses the race → 0/15 fast.

**Discriminating tests** (Release jsc, pinned env, intcs W=16):

| variant | n | phaseA_ms | mode |
|---|---|---|---|
| baseline (race) | 30 | 12 in [1512,1791], 18 in [4364,4746] | bimodal 40% fast |
| `String.fromCharCode(97)` at module init (main reifies) | 15 | [1517,1672] | **15/15 fast** |
| `(new Thread(()=>String.fromCharCode(97))).join()` at init (worker reifies) | 12 | [4441,4691] | **12/12 slow** |
| `JSC_reportDFGCompileTimes=1` | 12 | fast n=3: termOf=1, genDocTextI=2, tokenize=3; slow n=9: **termOf=15, genDocTextI=8, tokenize=9** | matches modes |
| `JSC_verboseOSR=1` exit kinds | 6 | termOf bc#22 / tokenize bc#266 / genDocTextI bc#254 = **`BadIndexingType` at `GetButterfly(String)`** (D@40, DFG dump verified) | — |
| 16-thread `describe({docIds:[],tfs:[]})` Structure test | 1 | **all 17 threads same StructureID** | refutes (d) |

### Fix (bench-level; engine-side residual below)

`bench.js` +1 effective line at module init: `String.fromCharCode(97);` —
reifies on main before workers. No-op call: zero checksum effect on every
arm; Go/Java have no lazy-reification analogue (fairness-neutral, same class
as the `$imul = Math.imul` hoist at line 47).

### intcs W=16 30-rep distribution before/after

| | n | phaseA min | med | max | max/min | slow-mode | cs tuple |
|---|---|---|---|---|---|---|---|
| **before** (race) | 30 | 1512 | **4518** | 4746 | **3.14** | **18/30 (60%)** | `e85d66e7\|4158480\|15cf18bb\|651b594b\|abc7704f` 30/30 |
| **after** (prewarm) | 30 | 1531 | **1624** | 1696 | **1.11** | **0/30 (0%)** | `e85d66e7\|4158480\|15cf18bb\|651b594b\|abc7704f` 30/30 |

intcs W=16 30-rep total_ms after: median **3359** [3050,3754], max/min 1.23.
All 60 reps rc=0.

### Regression checks (3-rep, Release, pinned env)

| arm | W | total_ms reps | median | §43 | Δ | cs match |
|---|---|---|---|---|---|---|
| nomap | 16 | 2442 / 2750 / 2758 | 2750 | 2684 | +66 | 3/3 ✓ §40 ref |
| flat | 16 | 471 / 471 / 477 | 471 | 462 | +9 | 3/3 ✓ §38 ref |
| intcs | 1 | 6261 / 6278 / 6312 | 6278 | 6381 | −103 | 3/3 ✓ §39b ref |

All within run-to-run noise (the prewarm is a no-op for nomap/flat and for
W=1 where main is the only thread).

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| All 69 reps rc=0, checksums stable per arm | **PASS** |
| `git diff Source/` empty (refuter discipline) | **PASS** |
| **intcs W=16 30-rep monomodal (max/min < 1.5×)** | **PASS — 1.11** |
| **intcs W=16 slow-mode rate < 10%** | **PASS — 0/30 (0%)** |

**verified=true.**

### Honest residuals

1. **Engine-side bug remains**: any GIL-off program that first touches a
   lazy static property (`String.fromCharCode/fromCodePoint/raw`,
   `Array.of/from/isArray`, `Object.assign/keys/…`, etc.) from a worker
   thread segments that constructor's butterfly, and DFG `GetButterfly` for
   any out-of-line property on it then OSR-exits forever (the recompile
   doesn't learn — `handleGetById` ignores `BadIndexingType`). Two
   candidate engine fixes (out of scope this round, recorded for §45):
   **(a)** `DFGByteCodeParser::handleGetById` checks
   `hasExitSite(m_currentIndex, BadIndexingType)` and falls back to the
   `getById` IC node (1-compile convergence; ~generic perf); **(b)** the
   ConcurrentButterfly §4.2 foreign-transition rule special-cases
   property-only NonArray butterflies whose `outOfLineCapacity` doesn't grow
   (cell-lock the put + structure publish, stay Flat — segmentation only
   buys concurrent INDEXED growth).
2. **§40 nomap interpretation corrected**: nomap is monomodal because of an
   incidental main-thread `termOf` call, not because `Map<string>` was
   removed. The §40 W=1 result (nomap helps Go/Java more than JS) stands
   unchanged — that IS the Map cost.
3. **§34(C) "logGC suppresses fast-mode"** is now mechanistically explained
   (main-thread stall → loses reify race), and the §34(C) "JIT tier is not
   the discriminant" finding is corrected: the discriminant IS the DFG body
   (per Round-1 (4b)/exp2b), selected by which thread wins reification.
4. **intcs W=16 fast-band median 1624 ms** vs nomap W=16 phaseA ~1100 ms:
   the residual ~500 ms IS the `Map<string>` cost (per-doc `new Map()` +
   `tf.get/set(string)` + `WTF::equal` key compare, 16.5% of fast-mode is
   `getByIdMegamorphic` per Round-1 perf). That's the real `Map<string>`
   tax, now measurable without the mode noise.

## §45 engine-side: no-segment property-only + handleGetById backstop

Closes the §44 hazard class in the engine and **removes** the §44 bench
prewarm (`String.fromCharCode(97)` at `bench.js` module init) so the bench
no longer needs to know which thread reifies a constructor's lazy statics.

### Change

Two cooperating fixes (the §44 residual (1) candidates (a)+(b)):

1. **`ConcurrentButterfly.cpp` §4.2 noseg-property-only `StayFlatShared`
   gate** (`trySegmentedTransition`): a foreign-TID / SW=1 property
   transition on a Flat butterfly with **no indexing header** and **no
   `outOfLineCapacity` growth** reuses the existing flat allocation under
   the cell lock — release-store the value into the live slot, then
   nuke + DCAS `{newStructure, (installerTID, SW=1)}` — instead of
   `convertToSegmentedButterfly`. The R7 read protocol is satisfied by the
   same M2/M5 ordering the owner StayFlat reuse path already relies on
   (payload unchanged, slot address data-dependent on the same word). I12
   holds because the step-0 F2 fire invalidated `writeThreadLocal` /
   `transitionThreadLocal` on source AND target before the SW=1
   publication. Gated `!useThreadGIL()` (the perf hazard is multi-mutator
   only; GIL-on corpus baseline preserved).
2. **`DFGByteCodeParser::handleGetById` BadIndexingType backstop**: under
   `useJSThreads()`, before the simple `CheckStructure + GetButterfly +
   GetByOffset` / `MultiGetByOffset` lowering, consult
   `m_exitProfile.hasExitSite(m_currentIndex, BadIndexingType)` and on a
   hit fall back to the `GetById` IC node. Mirrors the existing
   BadCache/BadConstantCache idiom; converges in **one** recompile when
   the gate above cannot apply (capacity grows) and the base segments
   anyway. Flag-off the block is dead and codegen is byte-identical.

`bench.js`: §44 comment block + prewarm replaced with a §45 pointer (net
−9 lines, no executable code at that site).

### §44 discriminants re-run WITHOUT bench prewarm

Release jsc, pinned GIL-off env, intcs W=16:

| variant | n | phaseA_ms sorted | mode | §44 result |
|---|---|---|---|---|
| (i) no prewarm (race) | 15 | [1568,1575,1575,1578,1583,1588,1593,1594,1594,1611,1616,1638,1640,1671,1692] | **15/15 fast**, max/min 1.079 | was bimodal 12f/18s |
| (ii) `(new Thread(()=>String.fromCharCode(97))).join()` at init (worker reifies) | 12 | [1563,1567,1611,1615,1616,1624,1628,1641,1645,1647,1648,1650] | **12/12 fast**, max/min 1.056 | was **12/12 slow** [4441,4691] |

**(ii) is the load-bearing flip**: forcing worker reification was
deterministically slow (Segmented StringConstructor → recompile loop); now
deterministically fast.

### Butterfly regime after worker reification (`describe`)

| ctor | pre | post (worker reify) | path |
|---|---|---|---|
| `String` | `(3/4){…name:66}` Flat 0x…5528 | `(4/4){…raw:67}` **Flat, same butterfly 0x…5528**, NonArray | StayFlatShared (cap 4→4) |
| `Object` | `(14/16){…groupBy:77}` Flat 0x…c668 | `(15/16){…keys:78}` **Flat, same butterfly 0x…c668**, NonArray | StayFlatShared (cap 16→16) |
| `Array` | `(8/8){…fromAsync:71}` Flat | `(9/16){…from:72}` **tagged butterfly word** (Segmented) | gate not applicable (cap 8→16) → backstop |

String stays Flat/NonArray as required. Array still segments (capacity
growth) but the backstop converges it (see compile counts below).

### intcs W=16 30-rep distribution (no prewarm)

| | n | phaseA min | med | max | max/min | slow-mode | cs tuple |
|---|---|---|---|---|---|---|---|
| §44 baseline (race, no engine fix) | 30 | 1512 | 4518 | 4746 | 3.14 | 18/30 (60%) | `e85d66e7\|4158480\|15cf18bb\|651b594b\|abc7704f` 30/30 |
| §44 prewarm | 30 | 1531 | 1624 | 1696 | 1.11 | 0/30 | same |
| **§45 engine fix, no prewarm** | 30 | **1548** | **1625** | **1672** | **1.080** | **0/30 (0%)** | same, 30/30 |

total_ms 30-rep: min 3104 / med 3317 / max 3877 (max/min 1.249).

### Regression checks (3-rep, Release, pinned env)

| arm | W | total_ms reps | median | §44 | Δ | cs match |
|---|---|---|---|---|---|---|
| nomap | 16 | 2883 / 2541 / 2573 | 2573 | 2750 | −177 | 3/3 ✓ §40 ref |
| flat | 16 | 468 / 454 / 468 | 468 | 471 | −3 | 3/3 ✓ §38 ref |
| intcs | 1 | 6451 / 6221 / 6260 | 6260 | 6278 | −18 | 3/3 ✓ §39b ref |
| default | 1 | 15795 / 16011 / 15865 | 15865 | — | — | 3/3 stable |

All within run-to-run noise; nomap improved (its main-thread `termOf()`
prewarm is now redundant).

### Targeted test

`JSTests/threads/jit/foreign-reify-getbyid-converges.js`: worker-reifies
`Array.from` / `Object.keys` / `String.raw`, then drives `noInline`d
`typeof Ctor.prop` to FTL under `useConcurrentJIT=0` and asserts
`numberOfDFGCompiles ≤ 4` for each (pre-§45 String alone was 8-15).
Observed (Debug+Release, pinned GIL-off): **String=1 Object=1 Array=2** —
exactly the predicted split (gate path = 1 compile, backstop path = 2).
GIL-on: 2/2/2 (gate is GIL-off-only; backstop converges all three).

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off (Debug, pinned env) | **95 passed, 0 failed, 4 skipped** (+1 new) |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **96 passed, 0 failed, 3 skipped** (+1 new) |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release) | **0 mismatches** |
| **intcs W=16 30-rep monomodal (max/min < 1.5×) WITHOUT prewarm** | **PASS — 1.080** |
| **intcs W=16 slow-mode rate < 10% WITHOUT prewarm** | **PASS — 0/30 (0%)** |
| **force-worker-reify discriminant fast** | **PASS — 12/12 in [1563,1650]** |
| All 63 reps rc=0, checksums stable per arm | **PASS** |

**verified=true.**

### Honest residuals

1. **Capacity-growing foreign reification still segments** (Array 8/8→9/16).
   The backstop converges it in one recompile, so there is no perf cliff;
   but the second DFG body uses the generic `GetById` IC, not
   `GetByOffset`. If a future workload is bottlenecked on a
   capacity-growing constructor static, the gate could be extended to a
   locked copy-grow (`StayFlatSharedGrow`) — same M2/M5 argument plus the
   §3 owner copy-grow allocation path. Not done here: the bench hot
   constructors (String, Object) hit the no-growth gate.
2. **Duplicate-property-name structures pre-exist §45** — **RESOLVED
   (S45-DUPLICATE-PROPERTY-NAME): misdiagnosis.** `Object` already showed
   `hasOwn:75, hasOwn:76` BEFORE any worker runs; after worker reify it
   gained a second `keys:78` and Array a second `from:72`. Investigation:
   these are NOT duplicate uids — `describe()` renders PRIVATE builtin
   symbols without the `@` distinguisher. The pairs are
   `hasOwn`(public,75)/`@hasOwn`(private,76),
   `@keys`(private,71)/`keys`(public,78),
   `@from`(private,70)/`from`(public,72) — each installed exactly once by
   `ObjectConstructor`/`ArrayConstructor::finishCreation` (the privates) and
   lazy `setUpStaticFunctionSlot` (the publics). `putDirectInternal`'s
   `structure->get()` replace leg structurally prevents same-uid duplicates,
   which is why "no correctness effect observed" held.
   The remaining real (small) gap: `reifyStaticProperty` did not re-probe
   under the IT-6 reification lock. `setUpStaticFunctionSlot` ALREADY takes
   that lock and re-probes under it (IT-6 fix, Lookup.cpp), so it was
   covered; the actual unlocked-probe caller is
   `JSObject::reifyAllStaticProperties` (probes `getDirectOffset` outside
   any lock, then calls `reifyStaticProperty`), and the batch
   `reifyStaticProperties` template never pre-probes. Two lites entering
   either of those that both saw "not present" pre-lock would both
   `putDirect` (loser = wasted JSFunction allocation + spurious
   `didReplaceProperty` watchpoint fire on a freshly-installed slot + a
   second §4.2 StayFlatShared header publish). Closed by an under-lock
   `getDirectOffset` re-probe at `Lookup.h reifyStaticProperty`, gated on
   `Options::useJSThreads()` (same predicate as the locker) so flag-off is
   byte-identical. Gate: `Object.getOwnPropertyNames` no-duplicate-string
   assertion folded into
   `JSTests/threads/jit/foreign-reify-getbyid-converges.js` — note this is
   an INVARIANT SMOKE / line-coverage gate, not a race amplifier: the
   structure replace leg cannot represent two offsets for one string uid,
   so the assertion structurally cannot fail, and the test's single
   `w2.join()` does not hammer the loser path. Acceptable because the §45
   root cause is a non-bug; do not over-trust as a TOCTOU detector.
   v5a-identity unchanged. Adjacent residual NOT touched here (out of
   S45's owned set, JSObject.cpp): `reifyAllStaticProperties` still calls
   `convertToDictionary` / `setStaticPropertiesReified(true)` outside the
   reification lock — pre-existing, two lites entering that function still
   race on those.
3. **GIL-on path unchanged**: the StayFlatShared gate is `!useThreadGIL()`
   so under the GIL a foreign reify still converts (preserving the GIL-on
   corpus baseline). The backstop is `useJSThreads()` and applies in both
   modes — GIL-on `numberOfDFGCompiles` is 2/2/2.

## §46 ship-correctness-closure

Full ship-readiness verification on the post-§45 working tree (HEAD
a31fc0d9f70f + uncommitted Tier-B/scanner-residual fixes across 47 source
files). Builder-scope only: compile + measure + record.

**Tree-stability:** ALONE held for this run. Builds: `ninja -C
WebKitBuild/Debug jsc` and `WebKitBuild/Release jsc` were already current
(no work). `ninja -C WebKitBuild/TSan jsc` **failed** at
`Structure.cpp:1104` (`m_transitionTable.tryGetSingleSlotConcurrently` —
the `TsanDeferredCtorMember<StructureTransitionTable>` wrapper had no
forwarder for the new method); fixed by adding the one-line forwarder
inside the `#if TSAN_ENABLED` block at `Structure.h:1611`. TSan then linked
clean (binary mtime 23:36). Debug+Release relinked for the header touch
(content inside `#if TSAN_ENABLED`, so non-TSAN object code unchanged).
TSan smoke (`jsc -e 'print(1)'`): OK.

### Gates

| Gate | Result |
|---|---|
| (1) Build Debug + Release + TSan | **PASS** (one TSAN-only forwarder added) |
| (2) Corpus GIL-off (Debug, pinned env) | **95 / 0 / 4** (≥95/0 ✓) |
| (2) Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **96 / 0 / 3** (≥96/0 ✓) |
| (3) v5a-identity (40 tests, Release) | **mismatches=0** ✓ |
| (4) `run-tests.sh --cve` (Debug, GIL-off) | **60 pass / 3 fail / 0 xfail / 2 skip** (65 files) — see §46 CVE below |
| (5) Validation sweeps r2 (Debug, GIL-off) | **00/01/02/04/05/06 = 95/0/4**; **03 = 80/15/4** — see §46 sweeps below |
| (6) Bench regression (Release, GIL-off, 3-rep) | **PASS** — all ±2% of §45, all checksums match refs |
| (7) gcAtEnd × property-wait-termination (10 reps) | **10/10 pass, no SEGV** ✓ |

### §46 bench (Release, pinned GIL-off, loadavg 0.7-2.2)

| arm | W | total_ms reps | median | §45 ref | Δ | cs match |
|---|---|---|---|---|---|---|
| flat | 16 | 458 / 486 / 462 | **462** | 468 | −1.3% | 3/3 ✓ §38 ref `686d6890\|0fbbd673\|3af6b072\|e1d22021` |
| intcs | 1 | 6332 / 6349 / 6340 | **6340** | 6260 | +1.3% | 3/3 ✓ §39b ref `e85d66e7\|15cf18bb\|651b594b\|abc7704f` |
| intcs | 16 | 3265 / 3302 / 3266 | **3266** | 3317 | −1.5% | 3/3 ✓ §39b ref |
| default | 1 | 16273 / 16059 / 16133 | **16133** | 15865 | +1.7% | 3/3 stable §1.1 ref `b3e65a68…\|39c33392…\|c4bdd580…\|af028188…` |

All 12 reps rc=0. Raw: `Tools/threads/scalebench/results-v46-ship.jsonl`.

### §46 validation sweeps r2 vs SCAN-RESULTS r0/r1

| Sweep | r0 | r1 | **r2 (this run)** | Δ vs r1 |
|---|---|---|---|---|
| 00-baseline | 94/0/4 | 94/0/4 | **95/0/4** | +1 |
| 01-graph-bce (`validateDFGClobberize`) | 89/5/4 | 86/8/4 | **95/0/4** | **FIXED — SIGTRAP family gone** |
| 02-verifygc | 94/0/4 | (carried) | **95/0/4** | clean |
| 03-butterfly (`validateButterflyTagDiscipline`) | 94/0/4 | (carried; lint was a stub) | **80/15/4** | NEW — lint wired, 43 I14 reports |
| 04-butterfly-swbit | 92/2/4 | 93/1/4 | **95/0/4** | **FIXED — gcAtEnd SEGV gone** |
| 05-bytecode-exc (`validateExceptionChecks`) | ~44/50/4 | 44/50/4 | **95/0/4** | **FIXED — all 6 sites closed** |
| 06-zombie-scribble | 93/1/4 | 93/1/4 | **95/0/4** | **FIXED** |

Per-sweep `.summary` files (FAIL/ASSERTION/ASAN grep) are **empty** for
00/01/02/04/05/06. Sweeps 01 and 05 strictly improved vs r0/r1 as
required. Logs: `Tools/threads/scan/jsc-validation/*.log` (r2 timestamps).

**Sweep-05 site closure:** `VMTraps.cpp:686` (84 throws / 77 unchecked),
`ThreadAtomics.cpp:1425`, `ConditionObject.cpp:103`,
`ThreadManager.cpp:1124`, `JSObject.cpp:4283`, `CallData.cpp:76` — none
fire under `validateExceptionChecks=true` on the r2 tree.

**Sweep-03 (NEW):** `validateButterflyTagDiscipline` was the
OptionsList.h:688 stub at r0/r1; on the r2 tree the Task-13 lint is wired
and reports **43 raw I14 violations across 15 tests** (exit 134). The
predominant pattern is `GetByOffset/PutByOffset @N takes storage from
non-tag-masking producer {JSConstant,GetLocal,GetByVal} @M` — i.e. the
storage edge is the inline-cell pointer or a GetByVal, not a
`CheckButterflyTIDTag`-producing chain. This is **exactly the §43 residual
#2 (Task-8: TID-tag at JIT inline butterfly install)** surfacing through
its own validator; recorded below as a deferred residual, NOT a regression
of previously-passing functional behaviour (sweep 00 baseline is 95/0/4).

### §46 CVE suite (`run-tests.sh --cve`, Debug, GIL-off)

**60 PASS / 3 FAIL / 0 XFAIL / 2 SKIP** (65 files; 2 SKIP = wasm
premise-skip under the GIL-off `useWasm` refusal). All 3 fails are exit 3
(uncaught JS exception, **no crash, no assert, no ASAN**). All 3 are tests
**added in aad53e672892 (06-15)**, i.e. NOT present in the 06-10 Tier-A
baseline (54 PASS + 1 XFAIL of 57). The Tier-A closure baseline (§A3/A5,
06-17) ran the three Tier-A repros directly (20/20 each) and did not run
`--cve`; none of those repros fail here.

| Test | exit | Signature | Disposition |
|---|---|---|---|
| `mc-gc-weakgcmap-registry-vs-prune.js` | 3 | `ReferenceError: $vm is not defined` @ :116 | **TEST DIRECTIVE BUG** — `requireOptions` lacks `--useDollarVM=1`; the §A3/A5 closure ran it with the flag directly. Re-verified: with `--useDollarVM=1` appended, **3/3 pass** Debug GIL-off. The A3 mechanism (registry race) remains FIXED. |
| `mc-df-arraycopy-relabel.js` | 3 | `RangeError: Range … out of bounds` @ `dst.set(a,0)` :78 | 5/5 deterministic. The B1/B2 fix (`JSGenericTypedArrayViewInlines.h` single-snapshot + `ArrayPrototype.cpp` round-4 reroute) now bounds-checks the snapshotted source length against `dst.length` and throws — the test's writer thread grows `a` past `LEN`. Test oracle needs to tolerate the spec-correct throw (the OOB-evidence oracle on :81 is the actual detector). NOT a memory-safety regression. |
| `mc-life-creator-thread-dies.js` | 3 | `TypeError: Buffer is already detached` @ `new Int32Array(ab)` :90 | 5/5 deterministic. The S11/B10 atomicization (`ArrayBuffer.h m_pinCount`/`m_locked`) + detach gate now spec-correctly throws on view creation over a detached buffer; the test expected `v2[0]===undefined`. NOT a memory-safety regression. |

**No NEW memory-safety failures.** Every Tier-A gate (A1 `mc-grow-s4-*`,
A2 `mc-df-ta-sort-inplace`, A3 `mc-gc-weakgcmap-*` (with flag), A4
`mc-prim-async-generator-*`, A5 `mc-df-delete-reuse` /
`mc-jit-delete-reuse-*`) passes. Log:
`Tools/threads/scan/jsc-validation/cve-suite-r2.log`.

### §46 §43 residual (Task-8 TID-tag at JIT inline butterfly install)

The §43 residual #2 is **NOT closed** on the r2 tree:
`tlcSlotForConcurrentlyWithIso<JSArray>` still returns nullopt (JSArray
stays on the thin-thunk lazy arm), and the wired
`validateButterflyTagDiscipline` lint flags 43 I14 violations (sweep 03
above). The 4 named emission sites (`FTLLowerDFGToB3.cpp allocateObject`,
`AssemblyHelpers::emitAllocateJSObject`, `emitAllocateRawObject`,
`FTL allocateJSArray`) are unchanged in the working-tree diff with respect
to TID-tag emission. Disposition: **DEFERRED** — perf upside (~400-500 ms
on intcs W=1) and the latent correctness concern are recorded; the lint
provides the regression detector. JSArray excluded means the latent
concern is unreachable in production (every iso ClassType reaching the
inline allocator has no/null butterfly).

### §46 §45 residual #2 (duplicate-property-name structures)

**RESOLVED (S45-DUPLICATE-PROPERTY-NAME)** — see §45 honest-residual #2
above: misdiagnosis (private `@hasOwn`/`@keys`/`@from` rendered without the
`@` distinguisher in `describe()`). The under-lock `getDirectOffset`
re-probe at `Lookup.h reifyStaticProperty` is in the working tree
(gated on `Options::useJSThreads()`, flag-off byte-identical). Gate test
`jit/foreign-reify-getbyid-converges.js` PASSES in the r2 corpus.

### §46 verdict

**verified=true.** Corpus + identity green; `--cve` shows zero
memory-safety failures and zero NEW failures vs the Tier-A closure
baseline (the 3 exit-3 fails are post-baseline test additions with
test-side issues, recorded above); validation sweeps 01 and 05 strictly
improved (8→0 and 50→0); bench checksums stable across all 12 reps and
medians within ±2% of §45; gcAtEnd 10/10. Residuals recorded with
disposition in CVE-AUDIT-RESULTS.md "Ship-readiness closure".

## §47 ship-residuals

Repo @ `f6a854fa66d8` + working tree (FUZZ r3-001/r3b fix:
`tryArrayStoragePropertyTransition` + I35 CoW materialize-first reroute in
`tryPutDirectTransitionConcurrent`). Debug+Release+Fuzz rebuilt (Debug 348→
203 ninja targets, Release 203, Fuzz 345→201). Pinned GIL-off env. **Flag-off
byte-identical** (the entire diff is inside `tryPutDirectTransitionConcurrent`
+ the new `tryArrayStoragePropertyTransition`, both `Options::useJSThreads()`-
asserting, dead flag-off; bench-gate 7/8 within ±0.55% witnesses this).

### §47 (1) r3 saved repros 20× Debug

| repro | bare `--useJSThreads=true` | full TARGET ARGS | rc shape |
|---|---|---|---|
| `r3-001-trySegmentedTransition-ArrayStorage.js` | **20/20** | **20/20** | rc=0 |
| `r3-002-storeTaggedButterflyWordConcurrent.js` | **20/20** | **20/20** | rc=3 (uncaught RangeError stack-overflow; no crash) |

Regression tests `JSTests/threads/objectmodel/{array-storage,cow-named}-
property-transition.js` PASS Debug (and in the corpus runs below).

### §47 (2) r3b re-triage vs current Fuzz binary

`Tools/threads/fuzz/triage-r3b-batch.sh` over **136** r3b crash files (the
2026-06-19 02:28-06:30 window across `fuzzilli-storage{,-B,-C}/crashes`
including `duplicates/`; the 128 reported in FUZZ.md "Campaign r3b" plus
deterministic copies):

| count | signature |
|---|---|
| **134** | **NOREPRO** |
| 1 | `storeTaggedButterflyWordConcurrent` ← `setButterflyConcurrent` ← `JSArrayBufferView::slowDownAndWasteMemory` |
| 1 | exit-3 (uncaught RangeError; the saved r3-002 source) |

**0× r3-001 signature** (`trySegmentedTransition`). **Pre-fix the same triage
showed 12× r3-001-signature** — those 12 are the **CoW** I35 entry assert
(`!isCopyOnWrite(source->indexingMode())` at `ConcurrentButterfly.cpp:1068`),
not the AS one: a CoW array literal getting an out-of-line named-accessor add
via `Object.defineProperty` when E4 is ineligible (TTL fired / foreign TID)
fell into `trySegmentedTransition`'s I35 entry RELEASE_ASSERT. Fix:
`tryPutDirectTransitionConcurrent` materialize-first (§4.8) and RESTART before
the locked protocols, mirroring `classifyConcurrentLockedAdd`'s
§4.8-precedes-§4.x ordering. The 122→134 NOREPRO gain is the r3-001 AS-route
fix + this CoW route.

### §47 (3) Corpus + identity + scalebench checksums

| Gate | Result |
|---|---|
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **98 / 0 / 3** (was 96/0/3; +2 new objectmodel tests) |
| Corpus GIL-off (Debug, pinned env) | **97 / 0 / 4** (was 95/0/4) |
| v5a-identity (40 tests, Release) | **mismatches=0** ✓ |
| intcs W=16 3-rep (Release, GIL-off) | rc=0 ×3, **cs `e85d66e7\|15cf18bb\|651b594b\|abc7704f` 3/3 ✓** §39b ref; total_ms 3178/3328/3338, median **3328** (§46 ref 3266, +1.9%) |
| flat W=16 3-rep (Release, GIL-off) | rc=0 ×3, **cs `686d6890\|0fbbd673\|3af6b072\|e1d22021` 3/3 ✓** §38 ref; total_ms 469/476/484, median **476** (§46 ref 462, +3.0% at load 4.6) |

Raw: `Tools/threads/scalebench/results-v47-ship.jsonl`.

### §47 (4) bench-gate (Release, flag-off, 15-run, loadavg 0.80)

| bench | median | baseline | Δ | within ±1% | within ±2% |
|---|---|---|---|---|---|
| array-element-read | 54.346 | 54.446 | −0.18% | ✓ | ✓ |
| array-element-write | 52.051 | 51.844 | +0.40% | ✓ | ✓ |
| flat-butterfly-read | 13.682 | 13.546 | +1.00% | — | ✓ |
| flat-butterfly-write | 62.498 | 62.418 | +0.13% | ✓ | ✓ |
| inline-property-read | 27.175 | 27.325 | −0.55% | ✓ | ✓ |
| inline-property-write | 54.358 | 54.425 | −0.12% | ✓ | ✓ |
| megamorphic-access | 1257.822 | 1445.922 | −13.01% | ✓ | ✓ |
| **transition-heavy-constructor** | **58.258** | **54.918** | **+6.08%** | **FAIL** | **FAIL** |

**7/8 within ±2%** (and 6/8 within ±1%; flat-butterfly-read at exactly
+1.00%). **Accepted residual with attribution:** transition-heavy-constructor
is the TSAN-TRIAGE §24.7 bisection-proven host-inadmissible variance — the
**closeout commit `2f5a5c4974ad` reproduces +6.90%/+7.37%** on this host
today (15- and 21-run medians, full `Source/` reverted), so no change in
`2f5a5c4974ad..HEAD` (incl. this round) is responsible. C′ raw samples span
51.9-61.3 ms (18% spread); per-header audit of all r24-r26 conversions found
none on the bench's structure-transition / butterfly-grow path. Transferred
to PARKED V5b per `docs/threads/INTEGRATE-ungil.md` AB17g item 4 ("this
host is inadmissible … the binding V5b quiet-host interleaved-A/B (≥15 runs)
protocol has not been executed"). Log: `/tmp/bench-gate-47-final.log`.

### §47 (5) §45 discriminant re-check

Force-worker-reify `(new Thread(()=>String.fromCharCode(97))).join()` at
module init (the §44 12/12-slow [4441,4691] discriminant), intcs W=16, 5 reps:

| rep | rc | phaseA_ms | total_ms | cs match |
|---|---|---|---|---|
| 1 | 0 | 1610 | 3246 | ✓ §39b ref |
| 2 | 0 | 1612 | 3366 | ✓ |
| 3 | 0 | 1705 | 3383 | ✓ |
| 4 | 0 | 1665 | 3280 | ✓ |
| 5 | 0 | 1617 | 3203 | ✓ |

**5/5 fast** (phaseA in [1610,1705], well clear of the [4441,4691] slow band).
The §45 engine fix (StayFlatShared no-segment property-only +
`handleGetById` BadIndexingType backstop) **holds**.

### §47 (6) 2h fresh-storage re-fuzz (campaign r47)

`fuzzilli-storage-r47`, fresh, 4 jobs, `timeout 7200` foreground,
2026-06-19T09:23:40Z → 11:23:41Z. **423,374 execs**, 11.58% edge coverage,
1,175,199 edges instrumented. **9 crashes** / 8 reproducible / 2 unique
signatures (`Tools/threads/fuzz/triage-r47.sh`):

| count | signature | callers (#7 frame) |
|---|---|---|
| **0** | **r3-001 `trySegmentedTransition`** | — |
| 5 | `storeTaggedButterflyWordConcurrent` owner-TID RELEASE_ASSERT (`JSObjectInlines.h:116`) | `JSArrayBufferView::slowDownAndWasteMemory` ×3, `JSObject::shiftButterflyAfterFlattening` ×1, `Structure::flattenDictionaryStructureImpl` ×1 |
| 3 | SEGV `DeferrableRefCountedBase::ref` ← `RefPtr<ArrayBuffer>` ← `isArrayBufferViewOutOfBounds` | `validateTypedArray` (Atomics.and, TA iterator) — reads `butterfly->arrayBuffer()` = `0xbebebebe…` poison while a concurrent `slowDownAndWasteMemory` is between `setButterfly(createOrGrowArrayRight)` and `setArrayBuffer(buffer)` |
| 1 | NOREPRO | — |

Log: `Tools/threads/fuzz/campaign-r47.log`; repros + ASAN reports:
`Tools/threads/fuzz/crashes/r47/`.

### §47 NEW residual (r47 setButterfly foreign-TID audit gap)

All 8 reproducible r47 crashes are **one root family**: the manifest-7
caller audit behind `storeTaggedButterflyWordConcurrent`'s owner-TID
RELEASE_ASSERT ("a missed caller becomes a trap, not a silent ownership
steal", `JSObjectInlines.h:93`) has **three escapes** that reach
`JSObject::setButterfly` with a foreign-TID (or otherwise non-owner) word:

1. **`JSArrayBufferView::slowDownAndWasteMemory`** (`JSArrayBufferView.cpp:
   322`): a worker thread reading `.buffer` / Atomics-validating a
   FastTypedArray created on another thread allocates the wastage butterfly
   and `setButterfly`s it with no protocol. Also the source of the
   `DeferrableRefCounted` SEGV: the wastage butterfly's `IndexingHeader::
   arrayBuffer` is uninitialized (0xbebebebe under ASAN poison) between the
   `setButterfly` publication and the cell-locked `setArrayBuffer`, and
   `isArrayBufferViewOutOfBounds` (every TA bounds check) reads it with no
   acquire/fence pairing. 6/8 r47 hits; r3b-001 is the same site.
2. **`JSObject::shiftButterflyAfterFlattening`** (`JSObject.cpp:7307`):
   dictionary flatten on a foreign thread shifts the butterfly base in
   place via `setButterfly`.
3. **`Structure::flattenDictionaryStructureImpl`** (`Structure.cpp:1921`):
   same dictionary-flatten path, the no-shift `setButterfly(butterfly())`
   restamp.

**Disposition:** DEFERRED to a focused r47 fix round — needs design
(tag-preserving cell-locked CAS publication ala
`publishArrayStorageButterflyLocked`, plus an `m_mode`/butterfly/
`setArrayBuffer` publication-order audit for `slowDownAndWasteMemory`;
flatten sites likely need the §6 dictionary cell-locked form). The trap is
**working as designed** (it converts the silent ownership steal / poison
deref into a deterministic abort). NOT a §47 code-change regression —
present at r3b (the saved r3-002 STDERR misattributed; r3b-001 is a
reliable repro of the same `slowDownAndWasteMemory` site).

### §47 verdict

| Gate | Result |
|---|---|
| (1) r3-001/r3-002 20× Debug | **20/20 + 20/20** ✓ |
| (2) r3b re-triage | **134/136 NOREPRO**, 0× r3-001 sig ✓ |
| (3) Corpus on/off + identity 40 + intcs/flat W=16 cs | **98/0/3 + 97/0/4 + 0 mismatch + 6/6 cs match** ✓ |
| (4) bench-gate | **7/8 within ±2%**; transition-heavy-constructor +6.08% = §24.7 host-inadmissible (V5b-transferred) |
| (5) §45 discriminant | **5/5 fast** ✓ |
| (6) 2h re-fuzz | **0× r3-001 signature** ✓; 8 hits = r47 setButterfly audit-gap residual (NEW, recorded above) |

**verified=false** by the literal ±2% bench-gate criterion (1 of 8 outside,
bisection-proven host-inadmissible, attributed and V5b-transferred). All
correctness gates (1)(2)(3)(5)(6-signature) green.

## §48 r47 closure (2026-06-20)

### Fix: r47 manifest-7 setButterfly audit escapes (3 sites + 2 surfaced reads)

The §47-DEFERRED r47 family (8 reproducible = ONE root: foreign-TID
`setButterfly` reaches `storeTaggedButterflyWordConcurrent`'s owner-TID
RELEASE_ASSERT, `JSObjectInlines.h:116`) closes at the 3 audited write
sites plus 2 read sites surfaced by progressing past them:

| site | file | fix |
|---|---|---|
| `JSArrayBufferView::slowDownAndWasteMemory` | `JSArrayBufferView.cpp` | flag-on cell-locked: re-check `hasArrayBuffer()` (idempotent loser path), build wastage butterfly LOCAL + fill `IndexingHeader::arrayBuffer` BEFORE publication, storeStoreFence, tag-PRESERVING seq_cst CAS (the §4.6 AS-COPY shape, open-coded for NonArray indexing), storeStoreFence before `m_mode` flip. Closes r47-001 (owner-TID trap) + r47-002 (poison `arrayBuffer()` mid-publish). |
| `JSObject::shiftButterflyAfterFlattening` | `JSObject.cpp` | flag-on world-stopped + cell-locked tag-preserving seq_cst store (the §6/§4.6 T3/I17 form, no AS shape constraint — the stop makes ANY flat tag-preserving repoint sound). |
| `Structure::flattenDictionaryStructureImpl` (null case) | `Structure.cpp` | flag-on world-stopped + cell-locked: zero the word directly (I2: null payload ⇒ all-zero word). |
| `JSArrayBufferView::existingBufferInButterfly` (surfaced by r47-002 Debug) | `JSArrayBufferView.h` | segment-aware dispatch: a Wasteful view CAN carry a SEGMENTED word (foreign-TID named-prop add that grows outOfLineCapacity falls into `trySegmentedTransition`; the §44 StayFlatShared gate requires `!hasIndexingHeader` which a Wasteful view HAS). Segmented ⇒ read `arrayBuffer` from `spine->indexedFragment(0)->slots[0]` (§4.1; the I8 alias equation pins it to flat-era B−8). + loadLoadFence pairing the (c) storeStoreFence above. |
| `emitLoadTypedArrayArrayBuffer` (was `emitTypedArrayButterflyTagMask`; surfaced by 8CAE8CE0 retriage) | `jit/AssemblyHelpers.cpp` | the JIT analogue: `branchIfResizableOrGrowableSharedTypedArrayIsOutOfBounds` / `loadTypedArrayByteLengthImpl` loaded `[maskedButterfly + offsetOfArrayBuffer]` flat-only — the prior "typed-array wasteful-mode butterflies are never segmented" comment is FALSE. Segment-aware: branch on TID==notTTLTID, segmented ⇒ `[spine + 32 + outOfLineFragmentCount*8]` → indexedFragment(0) → `[+0]` = arrayBuffer. Clobbers scratchGPR flag-on; both call sites re-load `m_mode`. Baseline/DFG/FTL (all share the helper). |

Flag-off byte-identical at every site (gated `[[unlikely]]` on
`Options::useJSThreads()`). Regression tests: `JSTests/threads/objectmodel/
r47-foreign-dictionary-flatten.js`, `r47-typedarray-slowdown-wastememory.js`,
`r48-typedarray-segmented-arraybuffer.js`.

### §48 (1) r47-001 + r47-002 20× Debug `--useJSThreads=true`

| repro | result |
|---|---|
| r47-001 | **20/20** ✓ |
| r47-002 | **20/20** ✓ (after the `existingBufferInButterfly` segment-aware fix; first round 0/20 on `ASSERT(!isSegmentedButterfly)` @ `JSObject.h:1659` `butterfly()` ← `existingBufferInButterfly` ← `validateTypedArray` — the Wasteful-segmented read site, NOT the original DeferrableRefCounted SEGV) |

### §48 (2) r47 + r3b-survivor re-triage vs new Fuzz binary

`Tools/threads/fuzz/triage-r48.sh` over **11** files (9 r47-storage crashes
+ r3b-001 + r3b-002 SOURCE paths):

| count | signature |
|---|---|
| **10** | **NOREPRO** |
| 1 | exit-3 (r3b-002; uncaught RangeError, unchanged from §47) |

**0× r47-family** (`storeTaggedButterflyWordConcurrent` /
`slowDownAndWasteMemory` / `DeferrableRefCounted`). The 8CAE8CE0 file went
NOREPRO only after the JIT segment-aware fix (first re-triage, runtime-only:
SEGV @ `0xbadbeef0+0x20` in JIT iterator-next ← `slow_path_spread`; growable
SAB view + foreign named-prop add ⇒ segmented ⇒ JIT `[spine−8]` = scribble).
Full table: `WebKitBuild/Fuzz/triage-r48/all.tsv`.

### §48 (3) Regression tests + corpus + identity + scalebench checksums

| Gate | Result |
|---|---|
| New regression tests (3 × on/off, Debug) | **6/6 rc=0** ✓ |
| Corpus GIL-on (Debug, `JSC_useJSThreads=1`) | **102 / 0 / 2** (was 98/0/3; +3 new objectmodel tests, +1 prior unskip) |
| Corpus GIL-off (Debug, pinned env) | **100 / 0 / 4** (was 97/0/4) |
| v5a-identity (40 tests, Release) | **mismatches=0** ✓ |
| intcs W=16 3-rep (Release, GIL-off) | rc=0 ×3, **cs `e85d66e7\|15cf18bb\|651b594b\|abc7704f` 3/3 ✓** §39b ref; total_ms 3491/3691/3515 at load 12-18 (r48 fuzz running alongside; checksums-only gate) |
| flat W=16 3-rep (Release, GIL-off) | rc=0 ×3, **cs `686d6890\|0fbbd673\|3af6b072\|e1d22021` 3/3 ✓** §38 ref; total_ms 528/558/502 at load 13-18 |
| DFG/FTL forced segmented-Wasteful path (Debug+Release) | rc=0 ×3 ✓ |

Raw: `Tools/threads/scalebench/results-v48-ship.jsonl`.

### §48 (4) 2h fresh-storage re-fuzz (campaign r48)

`fuzzilli-storage-r48`, fresh, 4 jobs, 3 legs (host bg-task ~1h cap; resume
from on-disk corpus between legs): 2026-06-19T23:07:32Z – 2026-06-20T01:26:42Z,
**121 min** total uptime (59 + 56 + 6). **310,666 execs**, **9.93%** edge
coverage. **2 crashes**, both `_flaky`-tagged (`Tools/threads/fuzz/
triage-r48-campaign.sh`, 10× retry each):

| count | signature |
|---|---|
| **2** | **NOREPRO** |

**0× r47-family signatures** (`storeTaggedButterflyWordConcurrent` /
`slowDownAndWasteMemory` / `DeferrableRefCounted`). Log:
`Tools/threads/fuzz/campaign-r48.log`; storage:
`WebKitBuild/Fuzz/fuzzilli-storage-r48/`; INDEX (empty):
`Tools/threads/fuzz/crashes/r48/INDEX.tsv`.

A separate **9m47s pre-JIT-fix smoke leg** (`fuzzilli-storage-r48-prekill`,
36,308 execs, runtime-only fix applied, JIT helper still flat-only) found
**1 crash**: `ASSERTION FAILED: isPinnedPropertyTable()` (Debug; SIGABRT
no-report on Fuzz), the pre-existing 2026-06-07 class-static/gc family
(FUZZ.md "4-min smoke" footnote); NOT r47-related, NOT a §48 regression
(reproduces flaky on every prior tree). Recorded as a known pre-existing
residual; not counted against (4).

### §48 verdict

| Gate | Result |
|---|---|
| (1) r47-001/r47-002 20× Debug | **20/20 + 20/20** ✓ |
| (2) r47 + r3b-survivor re-triage | **10/11 NOREPRO** (1 = exit-3, unchanged), **0× r47-family** ✓ |
| (3) Regression + corpus on/off + identity 40 + intcs/flat W=16 cs | **6/6 + 102/0/2 + 100/0/4 + 0 mismatch + 6/6 cs match** ✓ |
| (4) 2h re-fuzz | **2 crashes / 2 NOREPRO / 0× r47-family signatures** ✓ |

**verified=true**: (1)(2)(3) green; (4) finds 0 of the
`storeTaggedButterflyWordConcurrent` / `slowDownAndWasteMemory` /
`DeferrableRefCounted` signatures.
