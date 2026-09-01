# SCALEBENCH SPEC — Concurrent Document Index & Query Engine

Status: FROZEN once calibrated (see §8). One open constant: `N_BASE` (doc count),
to be pinned by the calibration procedure in §8 and recorded in §1.1 before any
reported run.

## 0. Purpose and honesty statement

This benchmark answers the question asked on the threads PR: *how does
scalability hold up on big programs that use the threads* — not microkernels,
not embarrassingly-parallel arithmetic. It is a ~300-600 line concurrent
program per language (JS on this branch's `Thread`/`Lock`/`Atomics`-on-objects
API, Go, Java) doing real string work, real shared hash maps, real allocation
churn, under contended locks.

**Known structural handicap (state this in every doc and every results table):**
GC under JS threads on this branch is currently stop-the-world with parallel
marking; concurrent marking is designed (SPEC-congc.md) but not implemented.
Go and Java ship fully concurrent collectors. Allocation-heavy phases (A
especially) are expected to show STW pauses scaling with heap size and thread
count. Results must be reported with this stated up front, not buried.

## 1. Program shape (identical in all three languages)

A concurrent in-memory inverted index over a synthetic document corpus, in
three phases run back-to-back in one process. All threads are spawned at
process start and reused across phases (rendezvous via a barrier between
phases; implement the barrier with each language's primitive at the same
abstraction level: JS `Lock`+`Condition`, Go `sync.Mutex`+`sync.Cond`, Java
`ReentrantLock`+`Condition` — NOT `sync.WaitGroup`, NOT `CyclicBarrier`,
NOT `Promise` tricks; a hand-rolled counting barrier, ~15 lines, identical
logic in all three).

### 1.1 Constants

| Name | Value | Meaning |
|---|---|---|
| `GLOBAL_SEED` | `0x5CA1AB1E_0BADF00D` (u64) | corpus generator seed |
| `QUERY_SEED` | `0xFACE_FEED_C0FFEE_11` (u64) | query generator seed |
| `GOLDEN` | `0x9E3779B97F4A7C15` (u64) | splitmix64 increment |
| `V` | `65536` | vocabulary size (distinct terms) |
| `K` | `128` | index shard count (fixed, independent of W) |
| `N_BASE` | `28000` — pinned 2026-06-10 by §8 (Release jsc, W=1: Phase A 105.4 s at 200000 → 200000×15/105.41 ≈ 28000; re-measured Phase A 14.70 s / 14.74 s) | base corpus size |
| `N_QUERIES` | `N_BASE` | Phase B operation count |
| `WRITER_MOD` | `10` | op index `q` with `q % 10 == 0` is a writer (10%) |
| `TOPN` | `20` | top-N size in Phases B and C |
| `W` | run parameter | worker thread count |

All arithmetic on seeds/hashes is unsigned 64-bit with wraparound. JS must use
`BigInt` masked to 64 bits (`& 0xFFFFFFFFFFFFFFFFn`) for the PRNG and
checksums; Go uses `uint64`; Java uses `long` (Java's signed overflow is
two's-complement wraparound, bit-identical; comparisons that need unsigned
semantics use `Long.compareUnsigned` / `Long.remainderUnsigned`).

### 1.2 PRNG — splitmix64 (exact)

```
state: u64
next(s):
    s += 0x9E3779B97F4A7C15
    z = s
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB
    z = z ^ (z >> 31)
    return z          // s is the updated state
```

All `>>` are unsigned (logical) shifts. A fresh stream seeded with `x` means
`state = x`; the first output is `next` applied once.

Derived helpers (define exactly, use everywhere):

```
mix(x)        = one splitmix64 output step applied to x (no state):
                z = x + GOLDEN; then the three xor-shift-multiply lines above
randBelow(r, n) = r % n        // u64 modulo; bias is irrelevant and identical
                                // across languages, so it is ALLOWED
```

### 1.3 Hashing — FNV-1a 64 (exact)

Used for all string hashing and all checksums. No language-native `hashCode`,
no `Map` internals — FNV-1a only:

```
fnv1a(bytes): h = 0xCBF29CE484222325
              for each byte b: h = (h ^ b) * 0x100000001B3   (u64 wrap)
              return h
```

Strings are hashed as their UTF-8 bytes (the corpus is pure ASCII, so this is
byte-per-char in all three languages; JS may iterate char codes).

### 1.4 Vocabulary and term skew

Term `i` (0 ≤ i < V) is the string `"t" + base26(i)` where `base26` writes `i`
in lowercase letters `a..z`, most significant first, no padding (`0 → "a"`,
`25 → "z"`, `26 → "ba"`). Term strings are 2-5 chars; real, distinct, ASCII.

Skewed term selection (Zipf-ish, integer-only — NO floating point):

```
pickTerm(prng): a = randBelow(next(prng), V)
                b = randBelow(next(prng), V)
                return min(a, b)        // quadratic skew toward low indices
```

Low-index terms are hot (high df), high-index terms are rare. This creates
realistic shard contention: hot terms hammer a few shards.

### 1.5 Document grammar (deterministic per docId)

Document `d` (docId is a u64, 0-based) is generated from its own PRNG stream,
**independent of which thread generates it**:

```
docSeed(d) = mix(GLOBAL_SEED ^ (d * GOLDEN))
prng = stream seeded with docSeed(d)
titleLen = 5 + randBelow(next(prng), 8)        // 5..12 tokens
bodyLen  = 80 + randBelow(next(prng), 121)     // 80..200 tokens
tokens   = titleLen + bodyLen picks of pickTerm(prng), in order
```

The document **text** is a single string built from the tokens with realistic
casing/punctuation so tokenization is real work (rules keyed on the 0-based
token ordinal `j` within the doc):

- if `j % 7 == 0`: capitalize the token's first letter
- if `j % 11 == 3`: append `","` to the token
- if `j % 13 == 12`: append `"."` to the token
- tokens joined by a single space; title tokens first, then body (one string)

**Pinned abstraction level (binding under §2.2; clarified 2026-06-10, before
any measured results — token sequence and all checksums unchanged):** the text
is assembled builder-style — Go `strings.Builder`, Java `StringBuilder`, JS
parts-array + one final `join("")` (JS's builder idiom) — with capitalization
done by ASCII char-code arithmetic (`c - 32`) in all three. NO per-token
intermediate concatenations (no `tok + ","` temporaries in any language).

### 1.6 Tokenizer (the inverse, run by workers — this is the string churn)

Split the document text on any character that is not `a-z A-Z 0-9`; lowercase
each piece (allocate the lowercased string; do NOT skip lowercasing when
already lowercase — same work in all three); drop empty pieces. The result
must equal the generator's token sequence (lowercased). Term frequency
`tf(term, d)` = occurrences in that sequence.

**Pinned abstraction level (binding under §2.2; clarified 2026-06-10, before
any measured results — token sequence and all checksums unchanged):**
hand-rolled in all three — an explicit per-char scan loop over char codes with
manual ASCII `+32` lowercasing, exactly two allocations per token (the raw
split piece, then an unconditionally allocated lowercased copy). NO regex
split, NO stdlib Unicode case mapping (`String.prototype.toLowerCase`,
`strings.ToLower`, `String.toLowerCase`) in any implementation.

### 1.7 The shared index

- `K = 128` shards. `shardOf(term) = fnv1a(term) % K`.
- Each shard: `{ lock, map }` where `map` is the language's plain hash map
  from term string → posting list, and a posting list is a growable array of
  `(docId: u64-as-number-or-long, tf: u32)` pairs in arbitrary order.
  - JS: shard map is a `Map` if shared `Map` works across `Thread`s on this
    branch; otherwise a plain object with string keys. Document which.
    Posting entries: two parallel arrays (`docIds[]`, `tfs[]`) or an array of
    2-field objects — pick one, use the analogous shape in Go/Java.
  - Go: `map[string]*postingList` + `sync.Mutex` per shard. **NOT `sync.Map`.**
  - Java: `HashMap<String, PostingList>` + `ReentrantLock` per shard.
    **NOT `ConcurrentHashMap`.**
- Shared atomic counters (JS: `Atomics.add` on a shared plain object's
  properties; Go: `atomic.Uint64`; Java: `AtomicLong`):
  `docsIngested`, `tokensProcessed`, `queriesDone`, `writesDone`.
- A read-only **df snapshot** `dfSnap: term → df` frozen at the Phase A / B
  barrier (built single-threaded by thread 0 inside the barrier, then
  published; read-only thereafter). Used for scoring in Phase B so that
  concurrent Phase-B writers cannot make scores timing-dependent.

### 1.8 Phase A — INGEST

Work distribution: a single shared atomic counter `nextDoc` (init 0). Each of
the W workers loops: `d = fetchAdd(nextDoc, 1)`; if `d >= N_BASE` stop.
(This atomic-claim counter IS the shared queue; do not build a buffered
channel/queue — same mechanism in all three.) For each claimed `d`:

1. Generate the document text per §1.5 (local work, deterministic).
2. Tokenize per §1.6, build a local `term → tf` map for the doc.
3. For each distinct term, take the shard lock (JS `lock.hold(fn)`, Go
   `mu.Lock/Unlock`, Java `lock.lock/unlock`) and append `(d, tf)` to the
   posting list, creating it if absent. One lock acquisition per
   (doc, distinct-term) pair — do NOT batch by shard (keeps the lock-traffic
   pattern identical and realistic).
4. `Atomics.add(tokensProcessed, tokenCount)`; `Atomics.add(docsIngested, 1)`.

Phase A checksum (`checksumA`), computed after the barrier by thread 0:

```
checksumA = u64 sum over every shard, term, posting (d, tf) of
            mix( fnv1a(term) ^ (d * 0xD6E8FEB86659FD93) ^ (tf * 0xCAAF00DD) )
```

Order-independent (sum mod 2^64), so it is invariant across W and languages.
Also record `postingsCount` = total number of postings (u64).

### 1.9 Phase B — QUERY (90% read / 10% write)

Shared atomic counter `nextOp` (init 0); workers claim `q = fetchAdd(nextOp,1)`
until `q >= N_QUERIES`. Operation `q` is generated from its own stream:
`qSeed(q) = mix(QUERY_SEED ^ (q * GOLDEN))`, `prng = stream(qSeed(q))`.

**Writers** (`q % WRITER_MOD == 0`): ingest doc with
`docId = N_BASE + q / WRITER_MOD` exactly as in Phase A (same grammar — the
docSeed formula covers any u64 docId). `Atomics.add(writesDone, 1)`.

**Readers** (all other `q`): `kind = randBelow(next(prng), 10)`:

- `kind 0..3` — POINT (40%): `t = pickTerm(prng)`; under the shard lock, read
  `t`'s posting list; result = `(df, sumTf)` over postings with
  `docId < N_BASE` **only** (this filter is what makes results independent of
  concurrent writers — base postings are append-only and never mutated).
  `perQueryHash = mix(fnv1a(term(t)) ^ (df * 0x9E37) ^ sumTf)`.
- `kind 4..7` — AND (40%): `nTerms = 2 + randBelow(next(prng), 2)` (2 or 3)
  terms via `pickTerm` (duplicates allowed; keep them — same everywhere).
  Copy each term's posting list to a local snapshot **under its shard lock**
  (copy = real allocation churn, and it bounds lock hold time); intersect on
  `docId < N_BASE` by building a local hash map docId→count from the first
  list and probing with the rest (this exact algorithm in all three; no
  sorting, no galloping). Result = matching docIds.
  `perQueryHash = mix( u64 sum over matching d of mix(d) ) ^ matchCount`.
- `kind 8..9` — SCORED TOP-N (20%): `nTerms = 2 + randBelow(next(prng), 2)`
  terms via `pickTerm`. Integer scoring, base docs only:
  `idf(t) = (N_BASE * 1000) / dfSnap[t]` (u64 integer division; `dfSnap`
  miss ⇒ idf = 0), `score(d) = Σ_t tf(t,d) * idf(t)`. Select top `TOPN` by
  `(score DESC, docId ASC)` via a local selection over the candidate map
  (build candidates exactly as in AND but union instead of intersection).
  `perQueryHash = fnv1a( the 8-byte little-endian concatenation of the
  ranked docIds, then of their scores )`.

`checksumB` = u64 sum over **reader** ops of `mix(q * GOLDEN ^ perQueryHash)`,
accumulated per-thread locally and summed at the barrier. Order-independent;
the `docId < N_BASE` filter plus frozen `dfSnap` make it invariant across W,
languages, and writer timing. Also recompute the §1.8 postings checksum
**after** Phase B as `checksumA2` (now covering base + the exactly
`N_QUERIES / WRITER_MOD` writer docs — deterministic set).

### 1.10 Phase C — ANALYTICS (parallel group-by / top-N)

Group key of a term = its **string length** (2..5 → 4 groups; small group
count forces real merge contention) crossed with `firstLetter` (`a`..`z`),
i.e. up to 104 groups. Shards are claimed by atomic counter (`nextShard`).
For each claimed shard, for each (term, postings) in the shard map, compute
`totalTf` = Σ tf and `df` over ALL postings (base + writer docs), then update
a **shared** result structure: `groups: groupKey → { lock, totalTf, df,
terms: array of (term, totalTf) }` (group locks pre-created; group map itself
is pre-populated with all 104 keys before Phase C so no lock is needed for
the outer map). Append every term to its group's `terms` array under the
group lock — the deliberately dumb shared-append is the point.

After the barrier, thread 0 sorts each group's `terms` by
`(totalTf DESC, term ASC)` and takes the top `TOPN`.

```
checksumC = u64 sum over groups g of
            mix( fnv1a(groupKeyString(g))           // e.g. "3:a"
                 ^ g.totalTf ^ (g.df * GOLDEN)
                 ^ fnv1a(join of g's topN as "term:totalTf," ) )
```

### 1.10-WS Phase C — WORK-STEALING ARM (additive amendment, 2026-06-12)

A second, mode-selected implementation of Phase C. The §1.10 naive arm is the
**default** and stays byte-for-byte as written (it is the lock-pathology
diagnostic); the WS arm is an alternate scheduler + accumulator for the SAME
work over the SAME inputs and MUST produce bit-identical `checksumC` (and all
other checksums — Phases A/B are untouched). This amendment changes no naive
code path, no constants, and no naive checksums; results recorded before it
remain valid.

**Selection** (naive when unset):

- Go: env `SCALEBENCH_WS=1`
- Java: env `SCALEBENCH_WS=1`
- JS: the literal extra shell argument `ws` (the jsc shell has no env
  accessor — same mechanism as the `smoke` argument):
  `jsc <flags> bench.js -- W [smoke] ws`

In WS mode the output JSON carries an extra field `"mode":"ws"` immediately
after `"threads"`; naive output is unchanged.

**Algorithm (identical in all three languages; fairness rules §2 apply in
full — notably JS uses shared objects + `Lock`/`Atomics`-on-objects, Go/Java
use their idiomatic equivalents, and NO library concurrent maps or lock-free
deque libraries anywhere):**

1. **Per-worker deques over the K=128 shard indices.** W deques, one per
   worker. Deque = a growable array with live region `[head, tail)` protected
   by the one mutex type (JS `Lock`, Go `sync.Mutex`, Java `ReentrantLock`).
   No Chase-Lev / lock-free cleverness in ANY language — the deque is
   mutex-guarded everywhere, so the synchronization abstraction level stays
   identical (§2.1).
2. **Seeding:** shard `s` is pushed to deque `s % W`, in increasing `s` order,
   before the phase starts.
3. **Owner pop:** a worker pops from the TAIL of its own deque (under its
   lock).
4. **Steal-half when empty:** when its deque is empty, worker `w` scans
   victims `w+1, w+2, … w+W-1 (mod W)` in that order; from the first victim
   with `n > 0` items it steals `ceil(n/2)` items from the victim's HEAD
   (victim lock held for the copy only), then appends them to its own tail
   (own lock; locks never nested) and resumes popping.
5. **Termination:** a shared atomic `wsRemaining` (init K) is decremented
   once per popped shard. A worker exits when its deque is empty, a full
   steal scan found nothing, AND `wsRemaining == 0`; if the scan failed but
   `wsRemaining > 0`, items may be transiently in-flight inside a steal —
   re-scan.
6. **Thread-local accumulators:** per-shard work is §1.10's verbatim (same
   `totalTf`/`df` computation over ALL postings, same group key = length ×
   first letter of the base26 part), but folded into a THREAD-LOCAL map
   `groupKey → { totalTf, df, terms[] }` — no group locks are taken during
   the phase. After the Phase C barrier, thread 0 merges the W local maps
   into the §1.10 shared `groups` structure single-threaded (in worker-id
   order), then the naive sort/topN/`checksumC` code runs unchanged.
7. **Equivalence argument (why checksums must match):** each term lives in
   exactly one shard and each shard is popped exactly once (removal happens
   under the holding deque's lock), so every (term, postings) contributes
   exactly once; group `totalTf`/`df` are wrap-around sums (commutative);
   `terms[]` merge order is erased by the §1.10 sort with its full
   `(totalTf DESC, term ASC)` tie-break over unique terms; `checksumC` is an
   order-independent mod-2^64 sum. Any checksum difference is a bug, never
   acceptable variance.

**Parity gate (per language, before any WS results are reported):** at W=4,
3 runs of the naive arm and 3 runs of the WS arm; all 6 runs' full checksum
tuples (`checksumA`, `postings`, `checksumA2`, `checksumB`, `checksumC`)
must be identical to each other and across all three languages. A skipped
cell is reported as skipped, never extrapolated.

### 1.11 Output (the program prints exactly this, one JSON line)

```json
{"impl":"js|go|java","threads":W,
 "phaseA_ms":..., "phaseB_ms":..., "phaseC_ms":..., "total_ms":...,
 "checksumA":"hex16","postings":...,"checksumA2":"hex16",
 "checksumB":"hex16","checksumC":"hex16",
 "docsIngested":...,"tokensProcessed":...,"writesDone":...}
```

Phase times are wall-clock measured inside the program around each phase
(barrier-to-barrier), monotonic clock. Checksums printed as 16-hex-digit
lowercase, zero-padded.

## 2. Fairness rules (BINDING — a violation invalidates the implementation)

1. **Same algorithm, same abstraction level.** Sharded plain hash map +
   explicit per-shard lock in all three. Java MUST NOT use
   `ConcurrentHashMap`, `ConcurrentSkipListMap`, `StampedLock`, or
   `LongAdder`; Go MUST NOT use `sync.Map`, `sync.RWMutex` (plain `Mutex`
   only — JS `Lock` is not a RW lock), channels for the work queue, or
   `sync/atomic` value types beyond plain counters; JS MUST NOT use
   `SharedArrayBuffer` typed-array tricks in place of the object-property
   index. Read locks = write locks = the one mutex type, everywhere.
2. **Idiomatic but unoptimized.** No manual SIMD, no object/array pooling, no
   arena allocation, no string interning beyond what the runtime does by
   itself, no precomputed per-thread caches of shared data (except `dfSnap`,
   which is part of the spec) — unless the identical optimization is applied
   in all three implementations and documented in SCALEBENCH.md.
3. **Identical inputs.** Same `GLOBAL_SEED`/`QUERY_SEED`/`N_BASE`/`V`/`K`/
   `N_QUERIES` across languages and thread counts. Constants live in one
   config block at the top of each implementation; the runner cross-checks
   them via the JSON output.
4. **Checksum gate.** `checksumA`, `postings`, `checksumA2`, `checksumB`,
   `checksumC` MUST be bit-identical across all three languages and ALL
   thread counts. Any mismatch invalidates the entire batch — fix the bug,
   rerun everything. The runner enforces this (§5).
5. **No floating point** anywhere in the measured program (timing excepted).
6. **Defaults only.** Runtimes run with default flags (§4). The JS flag set
   required to enable threads (the pinned `--useJSThreads=1 ...` set) is the
   platform under test, not tuning, and is exempt.
7. **Thread = thread.** W OS-backed threads in each language: JS `new
   Thread(fn)`, Go one goroutine per worker with `runtime.GOMAXPROCS`
   left at default (goroutines are Go's idiom; W goroutines, not a pool),
   Java `new Thread(...)` (platform threads, NOT virtual threads).
8. **Line budget** ~300-600 lines per implementation, shared-constant header
   included. If one implementation needs > 600 lines to satisfy this spec,
   the spec is wrong — fix the spec, not the fairness.

## 3. Matrix

- Threads `W ∈ {1, 2, 4, 8, 16, 32, 48, 64}` (machine has 64 cores; 48 and 64 probe the saturation knee — note the runner itself + OS take some cores, so 64 measures oversubscription-adjacent behavior, which is realistic;
  headroom for GC/JIT helper threads — note in SCALEBENCH.md that JSC's
  compiler/marking threads are part of the platform and are NOT counted in W).
- `N_BASE` sized so **1-thread JS Phase A takes 10-20 s** (§8). All languages
  use that same pinned value.
- 5 measured repetitions per (language, W) cell, **median** reported, min/max
  retained in results.json. 1 additional warmup run per cell, discarded.
- Report per-phase and total: wall ms, and the §4 derived metrics.

## 4. Metrics (per run)

- `wall ms` per phase and total (from the program's JSON).
- `peak RSS` via `/usr/bin/time -v` (`Maximum resident set size`), whole
  process.
- `cpu_util = (user + sys) / (wall * W)` from `/usr/bin/time -v` (User/System
  time vs Elapsed). May exceed 1.0 for JS (JIT/GC helper threads) — report
  it as-is; that IS the honest answer.
- `speedup(W) = median_total(1) / median_total(W)` per language, plus
  per-phase speedups (Phase A speedup is where the STW-GC handicap shows;
  call it out).
- Runtime versions recorded in results.json: `go version`, `java -version`
  (2>&1), and for jsc the build path + the branch/flag set string (the jsc
  binary is `WebKitBuild/Release/bin/jsc`; record
  `Tools/threads/scalebench/jsc-build-id.txt` if present, else the binary's
  mtime+size). **No tuning flags.** One documented exception is allowed per
  language if a default is pathological (e.g., default JVM heap cap causing
  OOM at W=32); the exception, its trigger, and the exact flag go in
  SCALEBENCH.md and results.json (`"exceptions":[...]`). The same machine
  section must state the STW-GC handicap verbatim from §0.

## 5. Runner — `Tools/threads/scalebench/run.sh`

Companion files (the implementer writes these to this spec):

```
Tools/threads/scalebench/
  SPEC.md            (this file)
  bench.js           JS implementation (run via WebKitBuild/Release/bin/jsc
                     --useJSThreads=1 --useThreadGIL=0 --useVMLite=1
                     --useSharedAtomStringTable=1 --useSharedGCHeap=1
                     --useThreadGILOffUnsafe=1 bench.js -- W)
  bench.go           Go implementation (go run or prebuilt ./bench-go W)
  Bench.java         Java implementation (java Bench W)
  run.sh             the runner
  results.json       output (machine-readable)
  RESULTS.md         output (markdown table)
```

`run.sh` behavior (bash, set -euo pipefail):

1. **Preflight:** verify jsc binary, `go`, `java`/`javac` exist; compile
   `Bench.java` and `bench.go` once; record all versions; verify W=1 smoke
   run of each language at a tiny `N_BASE` override (`SCALEBENCH_SMOKE=1`
   env honored by all three implementations: N_BASE=2000) produces matching
   checksums; abort on mismatch.
2. **Load gate:** before EVERY batch (a batch = one (language, W) rep),
   poll 1-minute loadavg from `/proc/loadavg`; if ≥ 4, sleep 15 s and
   re-check (with a 30-min total timeout → abort).
3. **Interleaving:** the rep loop is ordered `for rep in warmup 1..5: for W in
   1 2 4 8 16 32 48 64: for lang in java go js: run` — languages interleaved
   J,G,Js,J,G,Js…, never blocked per-language, so thermal/clock drift hits
   all three equally. The `warmup` rep is executed and discarded.
4. **Invocation:** each run is
   `/usr/bin/time -v <cmd> W 2> time.tmp`, capture the program's JSON line
   from stdout and parse RSS/user/sys/elapsed from time.tmp.
5. **Checksum gate:** after all runs, assert every (checksumA, postings,
   checksumA2, checksumB, checksumC) tuple is identical across the full
   matrix. On mismatch: exit nonzero, print the offending cells, and write
   `results.json` with `"valid": false`. No RESULTS.md is produced from an
   invalid batch.
6. **Emit `results.json`:**

```json
{ "valid": true,
  "spec": "SCALEBENCH v1", "date": "...", "host": {"cores":64, "kernel":"..."},
  "versions": {"jsc":"...", "go":"...", "java":"..."},
  "exceptions": [],
  "checksums": {"A":"...","postings":...,"A2":"...","B":"...","C":"..."},
  "constants": {"N_BASE":..., "V":65536, "K":128, "N_QUERIES":..., "seed":"..."},
  "runs": [ {"lang":"js","threads":4,"rep":2,
             "phaseA_ms":..., "phaseB_ms":..., "phaseC_ms":..., "total_ms":...,
             "rss_kb":..., "user_s":..., "sys_s":..., "elapsed_s":...}, ... ],
  "medians": { "js": { "1": {"total_ms":..., "phaseA_ms":...,
                             "speedup":1.0, "cpu_util":...}, "2": {...} },
               "go": {...}, "java": {...} } }
```

7. **Emit `RESULTS.md`:** one table per phase + total: rows = W, columns =
   `js ms | js speedup | go ms | go speedup | java ms | java speedup`, plus a
   peak-RSS table and a cpu_util table; header includes versions, date, and
   the §0 STW-GC handicap statement verbatim.

`docs/threads/SCALEBENCH.md` (written when results land) = methodology recap,
the fairness rules, the §0 statement, the tables from RESULTS.md, and an
analysis section that names the scaling cliffs honestly (lock contention vs
GC pauses vs allocator, with `perf`/`/usr/bin/time` evidence where claimed).

## 6. Validity rules (run-level)

- Checksum mismatch anywhere ⇒ whole batch invalid (§5.5).
- A run that crashes, OOMs, or deadlocks ⇒ record it in results.json with
  `"failed": true` and the signal/exit code; the cell's median is computed
  only if ≥ 3 measured reps succeeded, else the cell reports `null` and the
  failure is called out in RESULTS.md (a JS crash at W=32 is a finding, not
  something to hide).
- No other processes: the load gate (§5.2) plus a note in RESULTS.md if
  loadavg gating ever delayed a batch by > 5 min.

## 7. Determinism audit (implementer checklist)

Every source of cross-language or cross-W divergence has been closed by
construction; verify each when implementing:

- [ ] PRNG: splitmix64 exact (test vector: seed 0 → first three outputs
      `e220a8397b1dcdaf`, `6e789e6aa1b965f4`, `06c45d188009454f`).
- [ ] All hashes FNV-1a 64 over ASCII bytes.
- [ ] Doc content depends only on docId (not on claiming thread).
- [ ] Query content depends only on q.
- [ ] Reader results filtered to `docId < N_BASE`; scores use frozen `dfSnap`.
- [ ] All checksums are mod-2^64 sums of per-item mixes ⇒ order-independent.
- [ ] Ties broken by docId ASC / term ASC everywhere a sort feeds a checksum.
- [ ] No floats; idf is u64 integer division.
- [ ] JS BigInt masking to 64 bits after every arithmetic op in PRNG/hash
      paths (and ONLY there — index/tf bookkeeping stays in Numbers; docIds
      fit in 2^53).

## 8. Calibration procedure (run once, then freeze)

1. Implement bench.js fully. Run `W=1` with `N_BASE = 200000`.
2. If Phase A wall time ∉ [10 s, 20 s], scale `N_BASE` linearly
   (`N_BASE' = N_BASE * 15 / measured_s`, rounded to the nearest 1000) and
   re-measure; iterate until in range.
3. Pin the final `N_BASE` in §1.1 of this file, in all three implementations,
   and in run.sh; record the calibration measurement in SCALEBENCH.md.
4. From then on this spec is frozen; any change invalidates prior results.
