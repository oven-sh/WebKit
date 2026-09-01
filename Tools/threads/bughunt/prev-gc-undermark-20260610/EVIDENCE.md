# EVIDENCE PACK — shared-GC-heap UNDER-MARKING corruption (GIL-off, scalebench ingest)

Date: 2026-06-10. Binary under test: `WebKitBuild/Release/bin/jsc`, mtime
2026-06-10 08:12 UTC. Host: 64-core Linux, quiet (loadavg < 4 at start).
Pinned GIL-off flags (every run below unless noted):
`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
--useSharedGCHeap=1 --useThreadGILOffUnsafe=1`.
Facts only; no fixes; no hypotheses beyond what the data forces.

Prior bughunt artifacts (the 2026-06-09 butterfly-stress wrong-value pack —
the source of the 1003008/1003017 decode, which belongs to THAT bug, not this
one) are archived untouched in `prev-butterfly-stress-20260609/`.

Artifacts in this directory:
- `campaign.sh` — failure-rate harness (`JSC=... ./campaign.sh <runs> <script> [flags]`)
- `repro.js` — instrumented copy of the original test (rich dumps at mismatch, §3)
- `min2.js` — minimized repro (§2.3)
- `min.js` — synthetic candidate that does NOT reproduce (negative control, §2.4)
- `rr-traces/rr-ingest-corrupt-tglx` — packed rr recording of a corrupt run (§5)

## 1. Reproduction and failure rates

Test: `Tools/threads/scalebench/js/repro-bigint-shared-ingest.js`
(header narrowing by the bench-run agent confirmed where retested).
Pass = `OK postings=296555` (deterministic). Fail = `CORRUPT bad=N`
(`BAD LIST` = parallel-array length divergence, `BAD ELEM` = wrong
value/type) or crash.

| config | result |
|---|---|
| W=4 (as written), pinned flags | **CORRUPT 13/13** (bad=2..12; aggregated over §1+§4 baselines) |
| W=3 | CORRUPT 4/4 |
| W=2 (1 spawned + main) | **CORRUPT 4/8 (50%)** — TWO mutators suffice |
| W=4, N_BASE=500 | OK 4/4 (too few collections; churn-scaled, see §2) |
| GIL-on control (`--useJSThreads=1` only) | OK 3/3 |
| `--useSharedGCHeap=0` (other 5 flags kept) | OK 3/3 |
| `--useGC=0` | OK (prior narrowing, header; not retested) |

Crash modes seen during campaigns (same bug, harder landing):
- `std::array<unsigned long, 16>::operator[]` libstdc++ hardening assert →
  SIGABRT (under `--sweepSynchronously=1`).
- SIGSEGV during the single-threaded END-OF-RUN verification walk
  (instrumented run 2, §3; also `--useJIT=0` run 5 and variant-C runs).

## 2. Minimization

### 2.1 What is NOT required (deletion variants of the original)
| variant | change | result |
|---|---|---|
| B `repro-B-nostrings.js` | no genDocText/join/regex-split/toLowerCase — tokens produced directly as terms | CORRUPT 4/4 → **string churn not required** |
| C `repro-C-nolocalmap.js` | no per-doc `tf` Map — every token pushed directly under the Lock | CORRUPT 1 + CRASH 3 of 4 → **local Map not required** (and more lock traffic crashes harder) |
| A `repro-A-numberprng.js` | Number PRNG; BigInt only in per-doc seed mix | **CORRUPT 1/4** → **BigInt not required — it is a rate amplifier.** (The header's "Number PRNG OK 3/3" was a sampling artifact.) |

### 2.2 Thread/iteration floor
- 2 mutators (main + 1 Thread) corrupt at ~50% per run (§1).
- N_BASE=500 is clean at W=4 → a minimum number of shared-heap collections
  during the churn is needed; rate scales with total allocation, not docs/thread.

### 2.3 Minimized repro: `min2.js`
No strings, no regex, no fnv, no per-doc Map; integer term keys; kept: BigInt
splitmix64 churn (knob `CHURNX`, default 8 extra PRNG steps/token), shared
`Map` of `{docIds:[], tfs:[]}` appended under `Lock`, Atomics work counter.
Verifier checks `tfs[i] === docIds[i]+1`.
- T4 (default CHURNX=8): **CORRUPT 4/5**; with CHURNX=0: CORRUPT 1/5.
- T2: OK 5/5 (min2's churn is below the original's; use the original for W=2).

### 2.4 Negative control: `min.js`
A from-scratch synthetic (tight BigInt churn + shared pushes every 16 iters,
16 shards) does NOT reproduce (8/8 OK). The original's per-token Lock.hold
cadence + posting-object/array birth rate matters; low-rate shared appends
against long-lived arrays do not trigger it.

## 3. Corruption semantics (instrumented `repro.js`, `--useDollarVM=1`)

Value encoding in the test: `docIds[i]` ∈ [0,2000) (doc number), `tfs[i]` ∈
[1,~20] (term count). Both arrays are appended back-to-back under ONE
`Lock.hold`. Posting objects `{docIds, tfs}` are born on whichever thread
first sees the term and immediately published into the shard Map under the
Lock. 3 instrumented runs: bad=9, SEGV-during-verify, bad=10.

Observed shapes (full dumps in `inst-run{1,2,3}.log (copied here)`):

1. **Length divergence both directions** between two arrays appended under one
   lock: `docIds.len=4 tfs.len=9`, `=2/6`, `=16/10`, `=12/11` … and absurd
   lengths with `undefined` holes past a sane prefix: `docIds.len=675` (7 real
   elements then undefined), `len=1000`, `len=1727`. The public length word is
   garbage relative to real contents.
2. **Foreign JSValues inside `ArrayWithInt32` butterflies** — read back via
   generic get_by_val without crashing:
   - `docIds = [string:tcako, 1]`, `[string:tcysr, 1]`, `[string:tvlo, 1,
     1491, 1801]`, `[string:tcrth, object:[object Object]]` — a **(term
     string, count) pair image**, i.e. exactly the byte pattern of another
     thread's freshly built term→count storage (the per-doc `tf` Map / shard
     Map machinery), sitting inside this array's storage.
   - tf counts (`1`) interleaved into docIds: `[1, 1299, 1]`.
   - doc-id-magnitude numbers inside `tfs`: `tfs[5]=1681` (docIds[5]=1590),
     `tfs[5]=1299` — a neighbor structure's doc id in the counts array.
3. **Cell headers are never corrupted.** In every dump the posting object's
   Structure is the correct `{docIds:0, tfs:1}` and both arrays still have the
   correct Array structure (`StructureID 16788736, ArrayWithInt32`). Only
   butterfly CONTENTS + length are wrong. (One SEGV'd dump printed a posting
   object with `butterfly (nil)` mid-verification before the crash.)
4. **End-of-run butterfly aliasing scan: 0 aliases** across 116,706 live
   posting arrays (tagged-butterfly base compare via `$vm.value`). The
   aliasing window is transient; by program end the foreign owner is gone or
   moved. (Map backing stores are not butterflies and are invisible to this
   scan — and the foreign data observed IS Map-shaped.)
5. Victims are overwhelmingly SMALL, recently grown lists (len 2–16): young
   butterflies, repeatedly reallocated by `push` growth.

## 4. Machine-state narrowing (flag matrix, original repro, W=4)

| flags added | result | what it carves away |
|---|---|---|
| `--useJIT=0` | CORRUPT 4/5 + SEGV 1/5 | **Tier-independent — pure LLInt corrupts.** Not a JIT-tier read/barrier-omission bug. |
| `--useFTLJIT=0` | CORRUPT 3/3 | (subsumed by useJIT=0) |
| `--useDFGJIT=0` | CORRUPT 3/3 | (subsumed) |
| `--forceSegmentedButterflies=1` | CORRUPT 3/3 | Not specific to flat-butterfly publication; segmented path equally affected |
| `--verifyConcurrentButterfly=1` | CORRUPT 3/3, **verifier never trips** | Tag/flatness invariants (I2/I3) hold throughout; the corruption is in storage contents, not the concurrent-butterfly word |
| `--verifyGC=1` | CORRUPT 3/3, **no assert** | The GC verifier does not see the under-marking |
| `--sweepSynchronously=1` | CORRUPT 4/5 + SIGABRT 1/5 (libstdc++ `std::array<,16>` index assert) | Live cells ARE being swept: forcing immediate sweep converts silent aliasing into immediate crashes (confirms prior narrowing) |
| `--collectContinuously=1` | CORRUPT 3/3 | More collections ≠ different outcome |
| Prior narrowing (header, accepted): `--numberOfGCMarkers=1`, `--useGenerationalGC=0`, `--useConcurrentGC=0` all still corrupt; Debug and TSan builds mask. | | Not marking parallelism; not remembered sets; not concurrent-marking races; STW-protocol/root-coverage territory |

Instrumentation gap (minor but real): `--logGC=1` prints NOTHING under the
pinned GIL-off flags (works GIL-on). The shared-heap collection path bypasses
(or never sees) the logGC dataLog — collections cannot currently be counted
from the log.

## 5. rr record-replay: WORKING, corrupt run captured

`/usr/bin/rr` (5.9.0) was broken (missing `libcapnp.so.0.10.3`). Fixed by
building cap'n proto 0.10.3 from source into `/usr/local/lib` plus soname
symlinks (`libcapnp.so.0.10.3 -> libcapnp-0.10.3.so`, same for libkj).
**rr requires `LD_LIBRARY_PATH=/usr/local/lib`** (its RUNPATH does not cover
/usr/local/lib and ldconfig does not index the symlink names).
`perf_event_paranoid=1` — recording works unprivileged.

- `rr record --chaos` of the original repro at W=4 captured `CORRUPT bad=1`
  (BAD LIST tglx) on the FIRST attempt, 7.7s wall.
- `rr replay -a` reproduces the identical output deterministically.
- Recording packed (binary embedded, 375MB) and archived at
  **`Tools/threads/bughunt/rr-traces/rr-ingest-corrupt-tglx`**; replay from the
  archive path verified. Usage:
  `LD_LIBRARY_PATH=/usr/local/lib rr replay Tools/threads/bughunt/rr-traces/rr-ingest-corrupt-tglx`
- Rate under rr-chaos (serialized to one core) is much lower than native
  (~1–2/7 observed), but one good recording is enough.

## 6. Minimal reproducing config + hard facts

**Minimal config:** Release jsc, pinned 6 GIL-off flags, 2 mutators (main + 1
`Thread`), heavy young allocation churn interleaved with Lock-protected
appends to shared posting arrays, enough total allocation to force several
shared-heap collections (N_BASE=2000 ≈ 100%@W4, 50%@W2; N_BASE=500 clean).
Most convenient: original repro at W=4 (≈100%), or `min2.js` at T4 (~80%).

**Hard facts any hypothesis must explain:**

1. **Requires only: shared GC heap + ≥2 mutators + GC cycles.** Goes away with
   `--useSharedGCHeap=0`, `--useGC=0`, or GIL-on. W=2 corrupts at 50%.
2. **Tier-independent** (pure-LLInt corrupts) and **GC-mode-independent**
   (1 marker, no generational, no concurrent GC, collectContinuously — all
   corrupt). Whatever is missed is missed by the STW collection itself:
   root coverage / N-mutator handshake, not a marking-parallelism or
   barrier-tier race.
3. **Live cells are swept**: `--sweepSynchronously=1` converts silent
   corruption into immediate SIGABRT/SEGV; lazy sweep normally delays reuse
   long enough to make it silent. A wild-store hypothesis does not predict
   sweep-timing sensitivity.
4. **Victims are butterflies, not cells**: every corrupted array still has a
   valid Structure/cell header; contents are another thread's freshly
   allocated (term-string, count) data; lengths are garbage; aliasing is
   transient (0 end-of-run aliases among 116k arrays). The missed object is
   young, small, and was just grown by `push` (butterfly reallocation) or just
   born and published under a Lock by another thread.
5. **All existing verifiers are blind**: verifyGC, verifyConcurrentButterfly
   never trip; Debug and TSan builds mask the bug entirely (timing). Only
   Release + real parallel churn shows it.
6. **Rate scales with allocation churn, not with any specific type**: BigInt
   PRNG is an amplifier (~4x vs Number PRNG at W=4), strings/regex/Map are
   each removable; total-allocation floor exists (N=500 clean).

## 7. EXPERIMENTER round 1 (shared-ingest under-marking): SBA-1 "untraced cell / dead butterfly" — REFUTED, with new mechanism evidence

Hypothesis SBA-1: a JSObject cell that survives a conducted cycle ONLY via
block-granular liveness (version-current newlyAllocated stamped by the step-5
flush, or the directory allocated bit) is never traced, so its BUTTERFLY gets
no mark and is swept while the cell survives.

Instrumentation (diff + logs + backtraces archived in
`Tools/threads/bughunt/round2-sba1/`; Heap.cpp reverted after, Release jsc
rebuilt and re-verified corrupt): an end-of-cycle walk in `Heap::endMarking()`
(before `m_objectSpace.endMarking()`), shared-server only, env-gated
(`JSC_BUGHUNT_PAIRSCAN` log / `JSC_BUGHUNT_RESCUE` causal probe), over every
JSCell-kind block with version-current newlyAllocated bits or the directory
allocated bit: classify each cell (marked / NA / ALLOC / dead), and for every
JSObject with a non-null butterfly check the butterfly cell's liveness in its
own block (`cellAlign` base; blocks-set membership filter).

**Result 1 — the claimed survival channel does not exist structurally.**
`MarkedSpace::endMarking()` (MarkedSpace.cpp:544) retires the newlyAllocated
version UNCONDITIONALLY every cycle, and `BlockDirectory::endMarking()`
(BlockDirectory.cpp:347) does `allocatedBits().clearAll()`. So block-granular
liveness does NOT outlive a conducted cycle: post-cycle sweep liveness is
version-current MARK bits only. There is no state in which "the cell survives
the sweep via newlyAllocated while its butterfly dies" — both live by marks or
both are sweepable.

**Result 2 — zero pairs under the hypothesis's own semantics.** 3 corrupt
runs (repro.js W=4, pinned flags + --useDollarVM=1), 14 cycles each,
~437k sweep-live-unmarked candidate cells per cycle: pairs with butterfly
having NO liveness bit at all (mark/NA/alloc) at end of marking: **0**.

**Result 3 — refined (sweep-time) semantics show only noise, no correlation.**
Treating "butterfly dead" as !version-current-marked (correct post-retirement
semantics): ~560 capped PAIR lines/run (NA+ALLOC garbage objects whose dead
butterflies are benign) and ~1/cycle MB-PAIR (a single persistent startup
object, same cell every cycle, type=36, plus a burst at the final verification
cycle). Address correlation of ALL logged pair cells/butterflies against the
corrupted posting objects/arrays in the same runs' $vm dumps: **0 overlap**
in 5/5 corrupt runs.

**Result 4 — causal rescue probe: corruption SURVIVES total rescue.** With
`JSC_BUGHUNT_RESCUE=1` every endMarking-enumerable unmarked cell
(NA or ALLOC leg, ~780-900k cells/cycle) was force-marked
(testAndSetMarked + PossiblyBlack + noteMarked) and every such object's
butterfly mark-bit set too — i.e. NOTHING enumerable as recently-allocated at
the end of any conducted cycle could be swept. The run (under gdb pacing)
STILL corrupts with the identical shape: `CORRUPT bad=2`, parallel-array
length divergence, foreign (term,count) images in ArrayWithInt32 butterflies,
and a posting object with `butterfly (nil)`. Per SBA-1's own refuteIf and the
generalized family: the victim does NOT lose its life at the
endMarking-enumerable-cell boundary. **REFUTED.**

**Result 5 (new, redirects the hunt) — the freelist walked by the step-5
flush is garbage; same site fires WILD.** Native rescue runs aborted 8/8
immediately after cycle 1 with the known libstdc++
`std::array<unsigned long, 16>` index assert (hard-fact-3 signature). Core
backtrace (bt-rescue-stopAllocating-conductor.txt):

    BitSet<1024>::clear(n=140238877012016)            <- n is a POINTER value
    MarkedBlock::clearNewlyAllocated
    MarkedBlock::Handle::stopAllocating  freeList.forEach  (MarkedBlock.cpp:252-258)
    LocalAllocator::stopAllocating (LocalAllocator.cpp:127)
    BlockDirectory::stopAllocating  m_localAllocators forEach   <- CONDUCTOR step-5 flush

The "free cell" pointer resolves to LocalAllocator+0xb0 — inside the
LocalAllocator/FreeList object itself, i.e. the FreeList interval/next chain
read by the flusher is torn or stale. The SAME assert fires in a fully
UNINSTRUMENTED wild run (--sweepSynchronously=1, no env vars, 1/13 runs;
bt-wild-stopAllocatingForGood-teardown.txt) at the same
`MarkedBlock::Handle::stopAllocating` freelist walk, reached from the OTHER
flush path: `GCClient::Heap::~Heap -> lastChanceToFinalize ->
GCThreadLocalCache::stopAllocatingForGood -> LocalAllocator::stopAllocatingForGood`
— a spawned thread's client-heap TEARDOWN walking its own allocator's
freelist and finding pointer-shaped garbage.

Why this matters for the corruption: `stopAllocating` first stamps EVERY cell
of the current block newlyAllocated, then walks the freelist and
**clears newlyAllocated for each "free" cell** (MarkedBlock.cpp:258). If the
FreeList state it reads is stale relative to the owning thread's bump
allocations (or the allocator is double-stopped / concurrently torn down),
the flusher clears the NA bit of cells the owner ALREADY handed out — silently
stripping the only liveness bit of just-allocated, not-yet-published cells
(in-block stale pointers: silent under-marking, the observed aliasing; torn
interval/next pointers: the std::array assert / crashes). This is consistent
with every hard fact in §6 and squarely in the suspect space items
"GCThreadLocalCache handoff at collection start" and "EXIT1/teardown
interaction" — NOT in the marking-roots space. Next round should attack the
LocalAllocator stop/teardown synchronization: who guarantees the owner thread
is quiescent and its freelist writes visible when (a) the conductor's step-5
`BlockDirectory::stopAllocating` and (b) `stopAllocatingForGood` teardown walk
`m_freeList`, and what prevents a stop racing a client teardown.

## 8. EXPERIMENTER round 2 (shared-ingest under-marking): TD-R2-2 "teardown crash is a downstream detector" — CONFIRMED; teardown/EXIT1 angle CLOSED; new construction-time freelist evidence

Hypothesis TD-R2-2: every teardown-vs-root-enumeration window is closed in the
current tree; the wild `stopAllocatingForGood` abort (§7 Result 5,
bt-wild-stopAllocatingForGood-teardown.txt) is the dying thread DETECTING
freelist state scribbled mid-run by the primary bug, not causing it.
Artifacts: `Tools/threads/bughunt/round2-td2/` (instrumentation diffs +
campaign logs); `Tools/threads/bughunt/repro-noteardown.js`. All
instrumentation REVERTED after; Release jsc rebuilt from the clean tree and
re-verified corrupt (2/2, bad=12/8).

**Experiment A — zero-teardown repro: corruption does NOT need any teardown.**
`repro-noteardown.js` = the original repro with workers that NEVER exit before
verification: after ingest they bump a counter and park in property
`Atomics.wait`; main runs the FULL verification walk while all 3 spawned lites
are Live (zero `~GCClient::Heap` / `tearDownSpawnedThreadForExit` /
`stopAllocatingForGood` executions), and only then releases/joins.
Result: **CORRUPT 12/12** (bad=4..14, vs 3..7 for the original 3/3 baseline the
same day, same binary) and all runs then print JOINED. Corruption rate is
comparable-or-higher with zero teardowns. This replaces the unverifiable TD2
citation: the teardown angle is bounded out by construction.

**Experiment B — 3-point env-gated FreeList sanity check.** `BUGHUNT_FLCHECK=1`
(diffs in round2-td2/): structural validation (current interval + linked
intervals: in-payload, cellSize-aligned, sane decoded lengths, no OOB decode)
at (a) `LocalAllocator::stopAllocating` entry == `MarkedBlock::Handle::
stopAllocating` entry (sole caller), context-tagged per flush path
(`dirflush` = conductor BlockDirectory::stopAllocating, `tlcflush` = per-client
step-5, `tlcSAFG` = teardown GCThreadLocalCache::stopAllocatingForGood,
`SAFG-other`); (b) `tryAllocateIn` return ("refill", i.e. immediately after
`block->sweep(&m_freeList)` built the freelist and one cell was taken);
(c) covered by the `tlcSAFG` tag at (a). Checks are hot: ~1.2k stopAllocating
+ ~30.6k refill validations per run (per-run summary line).

- Silent mode (pinned flags, 6 corrupt runs) and most sweepSync runs:
  **0 structural failures in 23/26 corrupt runs** — the silent aliasing
  corruption happens with structurally SANE freelists everywhere; structural
  tearing is the rare loud variant, not the common path.
- `--sweepSynchronously=1`, 20 runs: 3 runs hit structural failures, and in
  **all 3 the FIRST failure is site (b) refill, ctx=other, on a Live mutator
  mid-run** — i.e. the freelist is ALREADY GARBAGE the moment sweep
  constructs it, long before any flush or teardown touches it:
  - RUN 11: seq=0 refill (tid 3729484, cellSize=112, decoded intervalEnd =
    blockStart + ~4GB garbage length) -> seq=1 SAME allocator/block at
    stopAllocating ctx=**dirflush** -> then the known libstdc++
    `std::array<unsigned long,16>` abort (hard-fact-3 signature). Full §7
    Result 5 chain reproduced with the detector ORDER instrumented:
    refill-corruption first, conductor flush second, abort last.
  - RUN 15: seq=0 refill ctx=other -> seq=1 SAME allocator/block at
    stopAllocating ctx=**tlcSAFG** (teardown). The teardown walk fails only
    AFTER the same allocator already failed mid-run: the §7 Result 5 wild
    teardown abort is positively identified as a downstream detector.
  - RUN 12: seq=0 refill ctx=other `nextInterval` misaligned — and the SAME
    block with the SAME bad freelist image is then re-validated-bad by a
    SECOND LocalAllocator on a DIFFERENT thread (alloc 0x..11d200/tid 3729661,
    then alloc 0x..0e0f00/tid 3729658, 3 repeats): one MarkedBlock feeding
    two clients' allocators with identical garbage.
- refuteIf condition (first failure at (c) on a Teardown lite with (a)/(b)
  clean earlier in the run): **never observed**. TD-R2-2 CONFIRMED; the
  EXIT1/teardown angle is closed — later rounds should stop mining it.

**New mechanism evidence (redirects §7 Result 5's "stale freelist read by the
flusher" framing):** the corruption is present at freelist CONSTRUCTION, not
introduced by the flush. In every loud run the freelist coming OUT of
`MarkedBlock::Handle::sweep` is already structurally bad at the owner's own
first use: decoded interval lengths of ~4GB and misaligned next-interval
pointers mean the FreeCell link words IN THE BLOCK PAYLOAD are torn/scribbled
relative to the secret sweep just installed — i.e. another thread wrote into
(or re-swept) the block concurrently. RUN 12's geometry is pointed: block
payload base 0x..130, bad interval at +0x40 with cellSize=48 — +0x40 is a
valid cell boundary for a 64-byte size class, not 48 — and two allocators
walked the same freelisted block. Together with RUN 11 (cellSize 112) this
smells like ONE MarkedBlock simultaneously owned/swept under two views
(duplicate hand-out: empty-block steal (I8) / canAllocate-vs-inUse directory
bits / re-sweep of an in-use block after under-marking judged it empty), which
would directly produce every §6 hard fact: another thread's fresh (term,count)
cells materializing inside a live array's butterfly, garbage lengths, live
cells swept (sweepSync aborts), and silent runs whenever the second view's
writes happen to be in-bounds and well-formed. Next round: instrument block
identity/state at hand-out (sweep entry: assert !isFreeListed and not
currently owned; directory inUse/canAllocate/empty bit coherence under BVL;
steal path vs owner's in-flight allocation).

## 9. EXPERIMENTER round 3 (shared-ingest under-marking): RC-R3-2 closure CONFIRMED; duplicate-handout REDIRECT REFUTED; teardown stays closed; surviving lane = intra-window freelist-payload scribble / dual allocation from ONE legal hand-out

Artifacts: `Tools/threads/bughunt/round3-dup/` (instrumentation diffs
MarkedBlock-duptrap-prov.diff, LocalAllocator-duptrap.diff, Heap-prov.diff;
provenance logs prov{1,2,4}.log.gz + provout{1,2,4}.log; correlator
correlate.py + corr{1,2,4}.txt; flc9-refill-failure.log). ALL instrumentation
REVERTED after; clean Release jsc rebuilt and re-verified corrupt (2/2).

**Experiment A — duplicate-handout trap (RC-R3-2 experimentRequest 1):
ZERO traps in 23/23 corrupt runs. The dual-freelist-epoch mechanism is
REFUTED.** Env-gated (BUGHUNT_DUPTRAP=1) cross-thread single-ownership
registry: a global lock+map registers every `MarkedBlock::Handle::sweep`
with a non-null FreeList target and unregisters at stopAllocating /
didConsumeFreeList / unsweepWithNoNewlyAllocated; ANY sweep (to-freelist,
SweepOnly, steal `sweep(nullptr)`, conductor, teardown) of a block currently
registered traps with a full dump (both owners' tids, directory bits,
versions). Because the registry forces a happens-before on every check, it
catches dual hand-outs even where the existing plain-bool
`m_isFreeListed` FATAL would read stale. Plus the RUN-12 geometry check at
tryAllocateIn (blockCellSize == directory cellSize && block->directory() ==
m_directory). Hook liveness verified by gdb breakpoint (sweepEntry hit from
tryAllocateIn). Results, repro.js W=4 pinned flags:
- `--sweepSynchronously=1`: 13 runs, all corrupt/crash, 0 traps, 0 geometry
  failures.
- silent mode: 10 runs (7 CORRUPT bad=2..9, 3 segv), 0 traps, 0 geometry
  failures.
Per the request's own refute clause (zero dual-ownership across 10+ corrupt
runs), the "same block freelisted by two allocators / re-swept while
freelisted / cross-size-class handout" mechanism is OUT. Every block hand-out
in a corrupt run is exclusive and geometrically coherent.

**Experiment B — victim provenance (RC-R3-2 experimentRequest 2 /
confirmIf): victims NEVER lose version-current liveness; the missed-root
closure holds, and so does its "post-marking" framing — but at BLOCK
granularity nothing illegal happens either.** Env-gated (BUGHUNT_PROV=1)
recorder: at every Heap::endMarking (before the NA-version retire) a snapshot
of EVERY MarkedBlock's per-atom version-current mark + NA bitmaps; plus a log
of every Handle::sweep with its (toFreeList, EmptyMode, MarksMode, NAMode,
tid, ctx) tuple; one shared seq counter orders both streams. 3 corrupt
silent-mode runs (bad=8/7/5), offline-correlated against the $vm victim
addresses (posting-object cells + the corrupted arrays' tagged-butterfly-word
low-48 spine pointers; tag layout per SPEC-objectmodel §9.5):
- 100 distinct victim addresses, 1196 victim-block snapshots: every victim is
  version-current MARKED at every completed endMarking from its first-live
  cycle through the end of the run (949 live snapshots; the 247 dead ones are
  all strict PREFIXES — cycles before the block/cell first existed; zero
  mid-life liveness gaps).
- Zero "suspicious" sweeps of victim blocks: no victim block is ever swept
  to-freelist with EmptyMode=IsEmpty or MarksMode=MarksStale after its victim
  became live. All post-live hand-outs are NotEmpty/MarksNotStale (mark-
  respecting).
- The Heap.cpp:1218 snapshot RELEASE_ASSERT again never fired.
So: no missed root, AND no block-granular mark-discarding re-sweep of the
victims. The aliasing is fully established while both "owners" keep their
cells continuously marked — i.e. the DUAL ALLOCATION happens inside a single
legal hand-out window, after which both owners trace the same storage
forever. Round 4 should stop mining endMarking-time liveness entirely.

**Experiment C — standing guard for the teardown closure (FLCHECK-hot loud
leg): first failure is refill on a Live mutator, never tlcSAFG. Teardown
stays CLOSED.** 10 runs `--validateFreeListStructure=1 --sweepSynchronously=1`
(no other instrumentation): 7 corrupt (rc=3), 2 segv, 1 structural abort
(run 9) whose FIRST and only failure is `site=refill ctx=other` mid-run —
the round-2 pattern, satisfying the standing confirmation; the reopen
condition (tlcSAFG-first) was again never observed. Run 9's failure record
REPRODUCES ROUND-2 RUN 12'S GEOMETRY EXACTLY (flc9-refill-failure.log):
block payload [0x...130, 0x...c000), cellSize=48, originalSize=16032 (whole-
payload, i.e. the freelist came from an IsEmpty quick sweep), and the torn
nextInterval decodes to payloadBase+0x40 — a 64-byte boundary in a 48-byte
block. With Experiment A having excluded any second sweep/hand-out of such a
block, the 64B-strided scribbler CANNOT be another allocator's freelist
epoch: it is a thread writing 64B-strided data through a pointer it already
holds into the same payload the owner's freshly-built whole-payload freelist
covers — i.e. dual ownership of cells WITHIN one hand-out (overlapping
allocations off a torn/scribbled freelist, or a stale storage pointer from a
previous window whose liveness was discarded before the IsEmpty sweep).

**Open observation for round 4 (recorded, not adjudicated):** the provenance
logs show ~330-400 IsEmpty/MarksStale to-freelist hand-outs per run of blocks
whose LAST endMarking snapshot had naPop>0 (e.g. 33-48 version-current NA
cells, markPop=0, mostly right after cycle 1). If the NA-version retire makes
those cells legitimately dead, this is benign; if any of them was a
stack-only-reachable live cell (e.g. a spawned thread's local tf-Map backing
store — exactly the foreign data observed in the aliased butterflies), the
IsEmpty hand-out is the liveness discard. Discriminating NA-retire semantics
vs stack-reachability for these specific blocks is the cheapest next
experiment, alongside auditing who can still hold a raw pointer into a block
between its NA-bearing endMarking and its IsEmpty re-hand-out
(stopAllocating's NA-clear from a stale freelist remains the §7 Result 5
suspect that produces exactly this state).

## 10. EXPERIMENTER round 4 (shared-ingest under-marking): kind-agnostic sound rescue STOPS the corruption (0/20) — missed live object CONFIRMED to be an NA-cohort Auxiliary cell; stage-2 word intersection EXONERATES the conservative scan path entirely (neither coverage nor acceptance): the missing liveness edge is HEAP-INTERNAL

Artifacts: `Tools/threads/bughunt/round4-rescue/` (instrumentation snippets
Heap-rescue2-instrumentation.txt + stage2-instrumentation.txt; baseline3.log,
rescue20.log, full{1..8}.log). ALL instrumentation REVERTED after; clean
Release jsc rebuilt and re-verified corrupt (3/3: bad=5, bad=5, 1 segv).

**Experiment A — TLC-R4-2 confirmIf, kind-agnostic sound rescue: 0/20 vs
3/3 baseline.** Env-gated (BUGHUNT_RESCUE2=1) walk at `Heap::endMarking`
(shared-server block, BEFORE `m_objectSpace.endMarking()` retires the NA
version and `BlockDirectory::endMarking` clears allocated bits), over EVERY
block of EVERY cell kind (the round-1 rescue's `cellKind() != JSCell` filter
dropped): every cell that is sweep-live without a version-current mark (leg
NA = version-current newlyAllocated; leg ALLOC = directory allocated bit) is
marked through the REAL path — `Heap::testAndSetMarked(markingVersion, cell)`
(does `aboutToMark` version repair + `setIsMarkingNotEmpty` when stale),
`cellContainer().noteMarked()`, plus an explicit
`setIsMarkingNotEmpty(handle, true)` under the bitvector lock per touched
block so the IsEmpty empty-judge can never rehand the whole payload. No
tracing. Same-day baseline (same tree, env off): corrupt/crash 3/3.
Rescue ON, repro.js W=4 pinned flags: **OK 20/20** (deterministic
postings=296555 every run). Probe liveness verified per cycle:
rescuedAux ~21-31k, rescuedJSCellIH ~380-790, rescuedJSCell ~730-880k,
NA-leg ~6-10k, ~13 Eden cycles/run. Per TLC-R4-2's own confirmIf:
**the missed live object lives in the NA-cohort blocks, and the round-1
"rescue refutation" of the missed-liveness family is formally INVALID — its
JSCell-only filter excluded exactly the cells that matter.** Round-1's
rescue (all JSCell-kind cells + their butterflies) did NOT stop the bug
(§7 Result 4); adding the Auxiliary/JSCellWithIndexingHeader kinds stops it
cold => the corruption-carrying under-marked cell is an AUX-kind cell (Map
backing / butterfly storage) that is NOT the butterfly of any unmarked
scanned JSObject. NA-only blocks (naPop>0, markPop=0 — the §9 open
observation) appear every cycle: ~28-61/cycle, kinds 0 and 2, the kind-2
ones dominated by cellSize=432 and 32 Auxiliary blocks with naPop up to
~500 — sized exactly like the foreign (term,count) Map-store images found
inside victim butterflies.

**Experiment B — stage-2 bisect, (a) scan-coverage vs (b) acceptance-gate:
NEITHER. Zero contact between the conservative scan and the rescued cohort,
at every granularity, pre- and post-filter.** Same binary, recorders armed
with the rescue (3 runs, 39 cycles, all OK):
- accepted-roots intersection: `inSnapshot=0` for all ~900k rescues/cycle
  (structurally forced: `SlotVisitor::appendJSCellOrAuxiliary` MARKS every
  accepted root unconditionally, so an unmarked cell can never be an
  accepted root — the round-4 request's "present-but-unmarked = acceptance
  miss" outcome is impossible in this tree; recorded for future rounds);
- gate rejections (every candidate that reached `isLiveCell` and failed,
  recorded inside `genericAddPointer::tryPointer`): only 0-10 words/cycle
  total (162 over 39 cycles), and **rescuedNARejected=0** — no rescued NA
  cell was ever scanned-and-rejected;
- RAW scanned words (every word `genericAddPointer` saw, recorded at entry
  PRE bloom-filter/blocks-set, ~700 distinct/cycle, 27,555 total),
  intersected against the FULL [cell, cell+cellSize) range of every NA-leg
  rescue: **rescuedNAScanned=0 across 335,463 NA rescues**, and per-block
  scanHits=0 on all 312 NA-only-block records (naPop total 50,915).
No thread's copied stack/register image contained ANY pointer into ANY
byte of ANY rescued NA cell, in any cycle, ever. The MachineStackMarker
coverage lane, the TinyBloomFilter/MarkedBlockSet publication lane, and the
isLiveCell acceptance lane are ALL exonerated for the cohort: the scan never
saw these addresses because they are not stack-held at stop time.

**Adjudication.** TLC-R4-2's location claim is CONFIRMED (NA-cohort aux
cells; rescue is causally sufficient) but its MECHANISM (conservative-scan
miss of a stack-held cohort) is REFUTED by Experiment B. The only liveness
path left for a live-but-unmarked NA aux cell whose address is in no scanned
word is a HEAP-INTERNAL edge: an owner object (round-3 §9 proved victims/
owners stay version-current MARKED through every cycle) holds the only
pointer to freshly-allocated aux storage, and the conducted cycle never
traced that edge. That is exactly the shape of a BARRIER/VISIT-ORDERING
race: owner visited early in the cycle, mutator (pre-stop) installs a new
butterfly/Map buffer pointer, the store-barrier that should re-shade the
owner is lost or the re-visit never traces the new buffer; the buffer
survives cycle N only via NA, the NA retire at endMarking N discards it,
and the §9 IsEmpty whole-payload rehand-out hands the still-referenced
storage to another thread. Full-GC repro (--useGenerationalGC=0) means this
is not remembered-set-specific: it is the in-cycle dirtying path. ROUND 5
TARGET: GIL-off write-barrier coverage for aux-pointer installs
(JSObject::setButterfly / Map-buffer publish) against the §10.4 stop
protocol — who guarantees a mutator's barrier buffer/dirty state crossing
the stop boundary is drained into the conducted cycle for ALL N lites (the
per-lite mutator scribble/dirty queue handoff), and whether barrier
fast-path state (mutatorShouldBeFenced / barrier threshold) is per-carrier
rather than per-lite under useVMLite=1.

## 11. FIX LANDED (round 4 rescue, productionized): shared-server window-liveness retention in Heap::endMarking()

Approved fix (2 reviewer approvals) implemented in
`Source/JavaScriptCore/heap/Heap.cpp`: the verified round-4 probe
(`round4-rescue/Heap-rescue2-instrumentation.txt`) stripped of env gate,
counters, snapshot intersection, and logging. Inside `Heap::endMarking()`,
gated on `isSharedServer()`, strictly AFTER `assertMarkStacksEmpty()` and
strictly BEFORE `m_objectSpace.endMarking()` (same conductor thread, same
stop window): every cell whose only liveness witnesses are the §10 window
witnesses (version-current newlyAllocated stamp from the step-5 flush, or
the block's directory allocated bit) is converted to a version-current mark
via the real mark path (`Heap::testAndSetMarked` → aboutToMark version
repair + atomic mark; `cellContainer().noteMarked()`; PossiblyBlack for
JSCell kinds) and the block gets `setIsMarkingNotEmpty` under the bitvector
lock — making the §9 IsEmpty whole-payload rehand-out of a window-live
block impossible. Kind-agnostic (Auxiliary / JSCellWithIndexingHeader
included, per §10). Flag-off and GIL-on cycles are byte-identical (pass
unreachable when `!isSharedServer()`). Cost: bounded one-cycle floating
garbage.

Verification (Release, pinned 6 GIL-off flags, 2026-06-10):
- `repro-bigint-shared-ingest.js` W=4 x20: **OK 20/20**, deterministic
  postings=296555 (pre-fix baseline §1: CORRUPT 13/13).
- Loud leg `--sweepSynchronously=1` x10: **OK 10/10** (pre-fix: SIGABRT
  libstdc++ hardening assert).
- `repro.js` (instrumented, `--useDollarVM=1`) x20: **OK 20/20**,
  postings=296555. `min2.js` x20: **OK 20/20**, postings=298601.
- 240 runs of the ingest repro under 6-way self-load (6 workers x 40):
  **0/240 failures**.
- races/ suite x5 GIL-off: 7 passed / 0 failed, all 5 runs.
- GIL-on corpus: 95 passed / 0 failed / 2 skipped.
- Flag-off smoke (5 stress tests): 5/5 PASS.

### VERIFICATION FAILURE — corpus regression introduced by this fix

Full GIL-off corpus (pinned ambient env): **93 passed / 1 FAILED / 3
skipped**. The failure is `JSTests/threads/objectmodel/
i03-quarantine-readd-across-gc.js` — **DETERMINISTIC (10/10)**, "expected 64
but got 2" at the dictionary-leg `Object.keys(o).length` check, always
breaking at round 4. Confirmed CAUSED by the retention pass (compiling the
pass out -> 3/3 PASS on the same tree; pass in -> 0/10).

Diagnosis (env-gated leg/subspace bisect, since stripped):
- NA leg alone reproduces the regression; alloc leg alone does not (but
  alloc-only does NOT fix the ingest corruption: 5/5 crash; na-only leaves
  1/5 corrupt — both legs are needed for the §10 fix, as the probe found).
- Excluding ONLY `structureSpace` blocks from the retention pass fixes the
  regression: quarantine test 5/5 PASS (incl. full-test x3), ingest repro
  **20/20 OK**, full GIL-off corpus **94 passed / 0 failed / 3 skipped**.
- Excluding only `PropertyTable` makes it WORSE (readback of `o.d0` fails in
  round 1): a retained zombie Structure whose live window-created
  PropertyTable is NOT retained is adopted with a swept table.
- Mechanism: the pass marks dead window-witnessed Structures WITHOUT
  tracing. Weak-table pruning (`pruneStaleEntriesFromWeakGCHashTables`,
  transition WeakGCHashTables) runs AFTER `Heap::endMarking()` and keys on
  mark bits, so the marked-dead dictionary Structures from earlier rounds'
  delete-churn stay FINDABLE in the transition tables; a later same-shape
  re-add transition ADOPTS the zombie, whose property table is a stale
  snapshot — Object.keys enumerates the zombie's 2-entry table while
  butterfly offsets still read correctly. This is a structure-identity
  RESURRECTION hazard, distinct from (and additional to) the three tracked
  residuals: mark-without-trace is unsound for any cell type registered in
  a mark-keyed weak registry whose values are later looked up and adopted.

Candidate amendment for the next review round (validated, NOT landed —
deviates from the approved diff): skip `handle->subspace() ==
structureSpace` blocks in the retention pass. Empirically structures'
publish edges are witnessed by normal tracing (this test passes pre-fix,
and §9 showed owners/victims stay marked); the §10 corruption carriers are
Auxiliary cells. With the exclusion: ingest 20/20 OK, GIL-off corpus 94/0,
quarantine 5/5. Alternative narrower-shotgun variants that also passed both
locally (10x/5x): skip all JSCell-kind blocks ("aux+IH only"), or skip all
`needsDestruction()` blocks — both reopen more of the theoretical hole than
the structureSpace exclusion and are not preferred.

### Residuals tracked per reviewer amendment (none block this fix)

1. **PreciseAllocation gap** — `MarkedSpace::endMarking()` also clears
   precise-allocation newlyAllocated witnesses; the retention pass only
   walks `forEachBlock`. A window-allocated PRECISE cell (large Map
   storage/butterfly, > largeCutoff) with the same no-root profile would
   still be freed. Needs equivalent precise retention or a proof that the
   cohort cannot be precise.
2. **Retained cells are marked but NOT traced** — an old, non-window object
   whose only reference sits inside an unpublished window cell is still
   under-marked. The ROUND 5 hunt for the unwitnessed heap-internal edge
   (owner-visit vs aux-pointer publish, §10 adjudication) must continue;
   this fix restores I4/I5 at the only point the witnesses are destroyed
   and stays correct regardless of which publish path is the unwitnessed
   one.
3. **One-cycle retention** — if the owning thread re-parks inside the same
   short publish window across the NEXT cycle, all witnesses (mark, NA,
   allocBit) are stale and the chain recurs; astronomically narrower, but
   only the root-cause fix closes it provably.

Minor (documented in-code): on the allocBit leg the pass deliberately marks
free/never-allocated cells too — an allocated block has no FreeList, so no
per-cell liveness judge exists; narrowing it would reintroduce the hole.

## 12. REDESIGN LANDED (round 5): TRACE-ON-RETAIN — window-liveness retention moved INTO the marking fixpoint as the "Wlr" core marking constraint; i03 zombie-Structure regression closed WITHOUT a structureSpace carve-out; residual 1 (precise allocations) and residual 2 (mark-without-trace) both closed

Implemented in `Source/JavaScriptCore/heap/Heap.cpp`: the §11 endMarking()
bit-only retention pass is REMOVED; the same witness scan (NA leg + allocBit
leg, kind-agnostic, identical predicates) now runs as core marking
constraint `"Wlr" / "Window Liveness Retention"` (addCoreConstraints, after
"Cs"), gated `isSharedServer()`, SlotVisitor executor only, once per
`m_phaseVersion` (the witness set is fixed at stop time: no mutator runs and
conductor allocation is forbidden in the stop window, so one scan appends
every unmarked witness cell). Each witness cell goes through
`SlotVisitor::appendJSCellOrAuxiliary` — the REAL conservative-root path:
mark + trace — so the retained set is CLOSED UNDER TRACING; weak-set ("Ws")
and output ("O") constraints re-converge over the retained cells via the
normal `executeConvergence` loop. Explicit `setIsMarkingNotEmpty` under the
bitvector lock per appended block is kept (appendJSCellOrAuxiliary only sets
it via aboutToMarkSlow when marks were stale; eden cycles need the explicit
form). NEW precise-allocation leg: `isNewlyAllocated() && !isMarked()`
precise cells are appended too (closes §11 residual 1 — MarkedSpace::
endMarking retires that witness identically).

Why (a) trace-on-retain and not (b) retain-and-prune or (c) narrower
witness set: bit-only retention is unsound in BOTH remaining directions —
untraced retained cells carry dangling interior pointers if a parked
thread's unpublished edge really does reach them (§11 residual 2), and
every mark-keyed weak registry can resurrect an inconsistent zombie (the
i03 transition-table adoption). (b) patches only the weak-table symptom and
institutionalizes marked-cells-with-swept-children. (c) is infeasible: §10
Experiment B proved the cohort's liveness witness is a heap-internal
UNPUBLISHED edge — no enumeration of parked-thread state can compute it.
With tracing, a weak-table-findable retained Structure is exactly a stock
"dead-but-unpruned between GCs" transition-cache hit: fully consistent,
semantically legal adoption — no structureSpace exclusion needed, so the
§10 hole stays closed for Structure-shaped window cells too. allocBit-leg
traceability: an allocated block's free list was FULLY consumed
(didConsumeFreeList), so every cell slot was handed out this mutator window
and holds a constructed object — the same property stock conservative
scanning relies on when it accepts any cell-aligned pointer into an
allocated block.

Verification (Release, pinned 6 GIL-off flags, 2026-06-10, all on the same
rebuilt jsc):
- `repro-bigint-shared-ingest.js` W=4 x20: OK 20/20, postings=296555.
- `min2.js` x20: OK 20/20.
- `--sweepSynchronously=1` ingest leg x10: OK 10/10.
- 240 ingest runs under 6-way self-load (6 workers x 40, rotating
  --randomYieldSeed, marker-checked OK postings=296555): 0/240 failures.
- `objectmodel/i03-quarantine-readd-across-gc.js` x20: PASS 20/20
  (was DETERMINISTIC 10/10 FAIL under the §11 bit-only pass).
- Full GIL-off corpus: 94 passed / 0 failed / 3 skipped.
- races/ x5 GIL-off: 7 passed / 0 failed, all 5 runs.
- GIL-on corpus: 95 passed / 0 failed / 2 skipped.
- Flag-off identity smoke (5 stress tests, --useJSThreads=false vs
  default, rc+output identical): 5/5.

Residuals after this round: §11 residual 1 CLOSED (precise leg); §11
residual 2 CLOSED (retained set closed under tracing). Residual 3
(one-cycle retention across a re-parked window) REMAINS, unchanged — only
the round-5 root-cause hunt (unwitnessed aux-pointer publish edge / barrier
coverage across the §10.4 stop boundary) closes it provably; that hunt
must continue. The permanent FreeList validator (round 2) stays in.

## 13. ROUND-6 ADVERSARIAL ADJUDICATION of the §12 trace-on-retain ("Wlr") fix — four external findings verified against the tree (2026-06-10)

### F1 (blocker claim): "witness set is not provably a superset of what the
### lost-edge mechanism can miss — OLD (non-window) targets exist" — ACCEPTED
### as a residual-3 RECLASSIFICATION (no code defect in Wlr itself)

Verified: Wlr's three legs (version-current NA, directory allocBit, precise
NA) are all strictly "allocated since the last GC" witnesses; the code
contains no leg that could retain an OLD cell whose only reference sits in a
lost/invisible edge from an already-marked owner. The §10 adjudication
("heap-internal lost edge, in-cycle dirtying path") does NOT prove the
missed target is always window-allocated; the counter-scenario (mutator
relocates the only reference to an OLD cell across the stop boundary, the
same visibility defect hides the install) is consistent with all collected
evidence. Residual 3 is therefore RECLASSIFIED: from "astronomically
narrower (re-parked same window)" to "MECHANISM-DEPENDENT, UNBOUNDED until
the round-5 root cause lands" — the fix is empirically sufficient for every
observed repro but is proven complete only for the window cohort
(SPEC-heap I4/I5 restored by construction for window-witnessed cells ONLY).
The §10.4 stop-protocol fence/dirty-drain audit (ROUND 5 TARGET) remains
the only provable closure and GATES final sign-off of the corruption class.

Detector (F1 suggested fix #2): a NEW sampling detector is NOT needed —
`--verifyGC=1` is already exactly the requested lost-edge detector:
`Heap::verifyGC()` re-runs the entire constraint set synchronously on a
VerifierSlotVisitor at END of marking over CURRENT memory (so an edge
installed pre-park but missed by the racing conducted trace IS visible to
the re-walk per lemma L1) and `RELEASE_ASSERT`s every verifier-live cell is
real-marked — a non-window recurrence traps at the guilty cycle. The
verifier mirror of Wlr is deliberately a no-op, so real-marks ⊇
verifier-marks and Wlr itself cannot false-positive the check. Run result
on this tree recorded below. RECOMMENDATION: keep a periodic
`--verifyGC=1` leg in the GIL-off battery until the round-5 hunt closes.

### F2 (major claim): "trace-on-retain dereferences unwitnessed cells; the
### required cross-thread visibility fence is unproven/missing" — REFUTED as
### missing-fence, ACCEPTED as missing documentation

The fence exists and was verified in-tree:
- L1 (publication): `GCClient::Heap::releaseHeapAccess()` (Heap.cpp) does a
  seq_cst `m_accessState.exchange(noAccessState)` with the in-code contract
  "RHA: seq_cst exchange -> NoAccess publishes all prior heap writes to the
  conductor (F6)"; `conductSharedCollection()`'s §10.4 access barrier
  seq_cst-loads every client's `m_accessState` under GBL and proceeds only
  when ALL are NoAccess. Every pre-park store therefore happens-before
  conductor constraint execution. Parallel marker helpers are woken through
  `m_markingMutex`/`m_markingConditionVariable` (release/acquire), extending
  the chain. No mutation overlaps conducted marking: marking runs strictly
  inside WSAC and access re-acquisition blocks on the stop (F8).
- L2 (no park mid-initialization): initialization runs entirely under heap
  access; access release happens only at park brackets / poll sites, never
  inside allocation/initialization paths, and the barrier waits for
  NoAccess on every client. Same structural property the stock conservative
  scan relies on for cells of allocated blocks.
Both lemmas are now cited in the Wlr comment block (Heap.cpp). Residual
documentation debt: SPEC-heap should absorb L1/L2 via the frozen-spec
amendment process (history file + adversarial loop) — NOT edited here.

### F3 (major claim): "over-retention is much broader than the documented
### one-extra-cycle" — ACCEPTED; comment corrected

Verified by code reading: in a FULL collection `areMarksStale()` disables
the `isMarkedRaw` skip, so the allocBit leg retains EVERY cell (including
now-dead prior-cycle survivors) of every window-consumed block; under
generational GC Wlr marks are sticky until the next full collection. The
Heap.cpp cost comment is corrected accordingly (eden-sticky + full-GC
survivor retention; up to two full collections, not one cycle). The
stale-mark-skip narrowing (skip cells whose STALE mark bit is set in a
full collection — survivors, not window cells; conservative once any cell
in the block adopts the new version) is RECORDED here as a candidate that
must get its own adversarial round + full repro battery before landing —
prior narrowings (NA leg, JSCell-only) reintroduced the hole. Peak-RSS /
full-GC-reclamation quantification deferred to the bench gate, which is
only valid after the F4 strip (below).

### F4 (major claim): "temporary diagnostics still in tree invalidate bench
### numbers" — ACCEPTED (gate-sequencing, not correctness)

Confirmed in tree: (1) `sharedGCStackRootSnapshot()` full conservative-root
copy in gatherStackRoots (~Heap.cpp:1082), (2) the endMarking() snapshot
re-walk with RELEASE_ASSERTs (~:1217-1260), (3) the
`g_sharedGCConservativeRootAuditHook` seam (~:1036) — all
isSharedServer()-gated (flag-off identity preserved) and all marked
FIXME(fix-shared-heap-corruption)/(fix-shared-heap-ring-liveness-5).
HARD PRECONDITION recorded: strip all of them before ANY bench/SCALEBENCH
number is measured for the >1% gate, and the post-strip run must be a FULL
verify (the diagnostics are currently the only under-marking re-check
besides --verifyGC), not a smoke. The permanent FreeList validator
(round 2) is NOT part of the strip.

### Round-6 verification (Release, pinned 6 GIL-off flags, rebuilt tree)

Same rebuilt Release jsc (comment-only Heap.cpp amendments; ninja-verified):
- `repro-bigint-shared-ingest.js` W=4 x20: OK 20/20, postings=296555
  (3 launcher-side rc=126 "Permission denied" exec artifacts re-run clean;
  zero engine failures).
- `--sweepSynchronously=1` ingest leg x10: OK 10/10, postings=296555.
- `objectmodel/i03-quarantine-readd-across-gc.js` x10: PASS 10/10 (rc=0,
  zero error lines; the test prints nothing on success).
- DETECTOR leg (`--verifyGC=1`, ingest repro) x3: OK 3/3 — the GC verifier
  runs under isSharedServer() and its end-of-marking re-walk found NO
  verifier-live cell unmarked by the real fixpoint; this is the F1
  lost-edge detector and should stay in the GIL-off battery until the
  round-5 root-cause hunt closes residual 3.

## 14. ROUND-7 ADVERSARIAL ADJUDICATION of the Wlr constraint — seven external findings verified against the tree (2026-06-10)

### R7-F1 (major): "Wlr reads the mark/NA version protocol lock-free, racing
### concurrent aboutToMarkSlow" — ACCEPTED; FIXED in code
Verified by code read: the Wlr loop hoisted `areMarksStale(markingVersion)`
once per block and used `isMarkedRaw(cell)` / `isNewlyAllocatedStale()` /
`isNewlyAllocated(cell)` with no header lock and no Dependency, while
parallel marker helpers concurrently run `MarkedBlock::aboutToMarkSlow`
(clearAll of marks, setAndClear fold into NA + m_newlyAllocatedVersion bump,
storeStoreFence, version store — all under `header().m_lock`). That is
TSAN-dirty (against the TSAN-zero gate) and weak-memory-fragile, sound only
via the previously UNSTATED lemma: a window-witnessed cell has stale mark
bit == 0 (free-listed cells are unmarked) and its NA bit is stamped pre-stop
(L1) and never cleared during marking. FIX: new
`MarkedBlock::sharedGCWindowWitnessSnapshot()` takes the per-block witness
snapshot under the SAME header lock aboutToMarkSlow holds (lock order
header > bitvector, matching aboutToMarkSlow; allocBit read under the
bitvector lock); the constraint appends candidates only after the lock is
dropped (appendJSCellOrAuxiliary can retake it via aboutToMark). The lemma
is now written at the definition. CONSTRAINT recorded: the §13
stale-mark-skip narrowing MUST NOT land without this lock protocol and this
lemma — under the old racy reads, narrowing + concurrent clearAll =
nondeterministic under-retention (the original corruption class reopened
silently).

### R7-F2 (major): "Wlr-retained dead closure inflates bytesVisited ->
### heap-limit growth -> bigger windows -> bigger retained set (pacing
### feedback loop)" — ACCEPTED AS RISK; gates added, no code change
Verified: retained cells are traced through the normal visitor, so
m_totalBytesVisited (updateObjectCounts) counts them and drives
updateAllocationLimits proportional sizing; on N-mutator allocation-heavy
workloads the full-collection allocBit leg makes this self-reinforcing
(converges only because witnesses retire at endMarking; steady-state factor
unquantified). HARD GATES added to sign-off (on the post-strip Release
tree): peak-RSS and post-full-GC heap-size on the ingest repro AND the
SCALEBENCH parallel-self suite (N>=4 mutators) vs the pre-Wlr baseline —
the >1% wall-time gate alone does NOT cover this. The suggested pacing
exclusion (count Wlr-retained window-witnessed bytes like extraMemory
rather than visited live bytes) is RECORDED as a candidate needing its own
adversarial round; not landed blind.

### R7-F3 (major): "world-stopped precondition is debug-only at the point of
### use" — ACCEPTED; FIXED in code
Verified: only `ASSERT(worldIsStoppedForAllClients())` guarded entry
(compiled out in Release — the configuration all pinned verifies run), and
checkConn's Mutator arm deliberately tolerates mutatorHasConnBit WITHOUT
WSAC, so enforcement was indirect. Promoted to
`RELEASE_ASSERT(worldIsStoppedForAllClients())` — one load per phase
version. Audit note: every isSharedServer() collection routes through
conductSharedCollection (which RELEASE_ASSERTs and sets WSAC); the
RELEASE_ASSERT at the point of use is defense-in-depth for any path that
would violate that routing.

### R7-F4 (major): "SPEC-heap I12 no longer describes the landed liveness
### mechanism" — ACCEPTED; spec amended
Verified: old I12 said "each client's m_currentBlock cells" — strictly
narrower than the load-bearing witness set; a literal implementation would
still corrupt (§10 cohort lives in consumed blocks + precise allocations).
I12 rewritten (window-witness set, closed-under-tracing,
mark-without-trace forbidden, L1/L2, header-lock read protocol,
over-retention semantics normative); SPEC-heap-history.md §25 records the
amendment. Milestone commit blocks on this amendment, not just test green.

### R7-F5 (major): "gate is isSharedServer(), not gilOff — runs under GIL-ON
### shared-heap configs" — VERIFIED AS DESCRIBED; ADJUDICATED option 1
### (ruling recorded, gate kept broad)
Verified: HeapClientSet::add() fires noteSharedServerSticky() whenever
size>1 && useSharedGCHeap, with no gilOff condition. RULING: once ISS,
every collection runs the same §10 conducted stop protocol (stopAllocating
NA stamping, witness retirement in endMarking) regardless of GIL — the
window-witness hole is a property of shared conduction, not of gilOff, so
GIL-on shared-heap runs NEED the retention for the same reason; the broad
gate is also the conservative direction (retains more, never less), and
flag-off identity is preserved (ISS unreachable without useSharedGCHeap;
§12 identity smoke 5/5). Ruling mirrored in the Wlr gate comment
(Heap.cpp). Optional follow-up (not gating): a GIL-on shared-mode windowed
under-marking canary.

### R7-F6 (major): "over-retention unquantified; bench gate structurally
### unmeasurable while diagnostics remain" — ACCEPTED (extends §13 F4; no
### change this round)
Confirmed still in tree: sharedGCStackRootSnapshot copy, the endMarking
O(roots) re-walk + RELEASE_ASSERTs, g_sharedGCConservativeRootAuditHook.
DELIBERATELY not stripped this round: they are (with --verifyGC) the only
under-marking re-checks until the round-5 §10.4 stop-protocol audit closes
residual 3. Gate sequence reaffirmed and extended: (1) strip all
FIXME(fix-shared-heap-corruption)/(fix-shared-heap-ring-liveness-5)
instrumentation (permanent FreeList validator stays), (2) full verify on
the stripped tree (corpus + repro battery + --verifyGC leg), (3) measure
wall-time AND peak-RSS / full-GC-reclamation / pause on the stripped
Release build, including a worst-case all-blocks-consumed eden+full
workload (R7-F2 gates). Pause-bound note verified: Wlr executes once per
m_phaseVersion; re-executions after phase transitions append nothing
(marks monotone); the unbounded dimension is retention, not pause.

### R7-F7 (major): "closure claim must stay scoped; pin --verifyGC leg
### before the strip" — ACCEPTED
Residual 3 remains MECHANISM-DEPENDENT, UNBOUNDED (per §13 F1
reclassification): Wlr provably closes the window cohort only; the
non-window lost-edge counter-scenario is consistent with all evidence.
PINNED: the GIL-off battery (campaign.sh runs) must include a periodic
`--verifyGC=1` leg — recorded here as a hard precondition of the
diagnostics strip (the strip must NOT remove the last lost-edge detector).
Status/PR language stays "window-cohort fix + empirical closure", NOT
"root cause fixed". The round-5 §10.4 fence/dirty-drain audit remains the
only path to declaring the corruption class closed.

### Round-7 changes landed
- MarkedBlock.h/.cpp: `sharedGCWindowWitnessSnapshot()` (header-locked
  witness snapshot; protocol + lemma comment).
- Heap.cpp Wlr: snapshot/append split; RELEASE_ASSERT(WSAC); gate-ruling
  comment.
- SPEC-heap.md I12 + SPEC-heap-history.md §25.
Verification on the rebuilt Release tree recorded below.

### Round-7 verification (Release, pinned 6 GIL-off flags, rebuilt tree)
Rebuilt Release jsc (ninja, clean link) with the round-7 code changes
(header-locked witness snapshot + RELEASE_ASSERT(WSAC)):
- `repro-bigint-shared-ingest.js` W=4 x20: OK 20/20, postings=296555.
- `--sweepSynchronously=1` ingest leg x10: OK 10/10, postings=296555.
- `objectmodel/i03-quarantine-readd-across-gc.js` x13 (10 campaign + 3
  direct): PASS 13/13 — rc=0, zero non-benign output lines every run (the
  test prints nothing on success; campaign.sh's `^OK` grep misclassifies
  silent-success tests as "corrupt", which is a HARNESS artifact — the only
  stdout/stderr line is the benign GIL-off "disabling useWasm" notice).
- DETECTOR leg (`--verifyGC=1`, ingest repro) x3: OK 3/3 — end-of-marking
  verifier re-walk found no verifier-live cell unmarked by the real
  fixpoint with the locked snapshot protocol in place.
