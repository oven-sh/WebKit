# EVIDENCE PACK — spawned-thread-butterfly-stress silent value corruption (GIL-off full JIT)

Date: 2026-06-09. Binary under test: `WebKitBuild/Debug/bin/jsc` built
2026-06-08 18:27 UTC (includes the working-tree `runtime/VM.cpp` modification,
mtime 06:55 — binary postdates it). Pinned flags:
`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
--useSharedGCHeap=1 --useThreadGILOffUnsafe=1 --useDollarVM=1
--randomYieldPeriod=64 --randomYieldSeed=<seed>` (RaceAmplifier maxSleepUs=100
unless noted). Facts only; no fixes, no hypotheses beyond what data forces.

## 0. Headline result of this run

The silent corruption did NOT reproduce on the current binary in ~2,560
fresh runs across 13 configurations (details §3) — including an exact
replication of the seed set, worker layout, and 6-way load shape that
produced the two known failures on 2026-06-08 evening (same binary).
The only current-binary corruption evidence remains the two 2026-06-08 logs
(§1). Everything any hypothesis must explain is therefore: (a) the decoded
semantics of those two failures (which are sharp), (b) the layout facts
(§2), and (c) the non-reproduction envelope (§3), which bounds the failure
rate at my load shapes to < ~1/2560 even though the 06-08 round saw ~2/240.
The discriminating variable between 06-08 and today is host load shape
(unknown co-running workloads then), NOT the binary or seeds.

## 1. Corruption semantics (decoded)

Value encoding (test source): `value = tid*1000000 + serial*1000 + p` for
property `"p"+p`, p in [0,24). Each `p*` property is written EXACTLY ONCE,
inside `buildOne()`, BEFORE the object is published into the shared registry.
Nothing ever rewrites them. **"Stale by 9 writes" is impossible** — there is
only one write per slot, ever. The corruption is a WRONG-SLOT (or
wrong-offset) read.

Current-binary failures (both 2026-06-08, plain `Tools/threads/load6.sh`,
default SEED_BASE=1000, Debug jsc of 18:27):

| seed | log | got | decoded | want | decoded | meaning |
|---|---|---|---|---|---|---|
| 3012 | `/tmp/load6-logs/FAIL-rc3-w2-r12-s3012.log` (18:56) | 1003008 | tid=1 serial=3 p=8 | 1003017 | tid=1 serial=3 p=17 | read of `p17` returned `p8`'s value |
| 6014 | `/tmp/load6-logs-2/FAIL-rc3-w5-r14-s6014.log` (19:07) | 2023017 | tid=2 serial=23 p=17 | 2023008 | tid=2 serial=23 p=8 | read of `p8` returned `p17`'s value |

Sharp properties of this pair:
- SAME-OBJECT cross-property reads (tid and serial match got↔want in both).
  Not cross-object, not cross-thread values, not type confusion (both
  well-formed Int32 values of sibling properties).
- Both involve EXACTLY the pair {p8, p17}, once in each direction — a
  swap-shaped signature (Δ = 9 property slots both ways), not a
  one-directional staleness shift.
- Both faulted in `checkOne` (test line 52, the named-property compare)
  called from `threadBody` line 86 — the FOREIGN-read loop
  (`checksum += checkOne(theirs[i])`): the failing reader does NOT own the
  object. The read form is `o["p" + p]` — get_by_val with a freshly
  rope-concatenated, atom-resolved string key.
- The owner of the corrupted object is a spawned thread in both cases
  (tid 1 and tid 2), read by some other thread.
- The object's structure WAS being transitioned concurrently by its owner
  during the failure window (`late*` adds, one per round), but its butterfly
  is NEVER reallocated during the concurrent phase (capacity proof in §2).

Historical context (PRE-current-binary logs, June 7 – June 8 morning,
`/tmp/sb.*`, `/tmp/item2/*`, ~38 distinct "named property corrupt" failures):
the old signature space was much broader — cross-object, cross-tid, and
serial-field corruption (e.g. `got 1004000 want 4000`, `got 2005007 want
10019`). Those binaries predate the 2026-06-08 shared-cache /
VAMP-offset-swap / F3 fixes. On the current binary only the narrow
{p8,p17} same-object swap signature has ever been observed. (Do not mix the
two corpora when reasoning about the current bug.)

## 2. Object layout (measured with $vm.dumpCell, flag-off AND pinned GIL-off flags — identical)

`buildOne` object `{tid, serial, p0..p23, indexed}`:
- inlineCapacity = 4 → inline: `tid`(off 0), `serial`(off 1), `p0`(off 2), `p1`(off 3).
- Out-of-line: `p2`..`p23` at OOL indices 0..21, `indexed` at OOL index 22.
- Final butterfly propertyCapacity = 32, preCapacity 0. Adding a `late*`
  property does NOT move the butterfly (verified: same base/butterfly
  pointer after the transition). OOL usage peaks ≈25 (< 32) for the whole
  test, so during the entire concurrent phase a published registry object's
  butterfly is never reallocated for property growth; only its StructureID
  changes (owner `late*` transitions) and the butterfly word's high tag bits
  (GIL-off tagged word observed: `0x4000_7e82fb250a08`).
- p8 = OOL index 6; p17 = OOL index 15. Δ = 9 slots = 72 bytes. OOL slots
  grow DOWNWARD from the butterfly pointer, so p8's slot address is 72 bytes
  ABOVE p17's.
- PropertyOffsets: p8 = firstOutOfLineOffset+6, p17 = firstOutOfLineOffset+15.

Constraint on any explanation: the two slots live in the same once-written,
never-moved 32-slot OOL block. A wrong value therefore requires the READER
side to have used the other property's offset (off by exactly ±9 OOL slots)
— wrong offset from a (structure, uid)→offset mapping, an IC stub, a
megamorphic cache entry, a property-table lookup, or a butterfly-derived
pointer displaced ±72 bytes. Plain memory staleness of the slot itself
cannot produce these values.

Related same-family hard datum (older binary, 2026-06-08 morning,
`/tmp/load6-logs2/FAIL-rc134-w5-r11-s12011.log`): Debug assert
"Detected offset inconsistency: numberOfSlotsForMaxOffset doesn't match
totalSize!" — `transitionOffset=68 maxOffset=68 m_inlineCapacity=3
numberOfSlotsForMaxOffset=8 totalSize=9 inlineOverflowAccordingToTotalSize=6
numberOfOutOfLineSlotsForMaxOffset=5`: a Structure observed with its
property-table size and maxOffset views disagreeing by one slot. Direct
evidence (albeit pre-fix binary) that structure/property-table publication
was observable torn in this tree's lineage.

## 3. Reproduction campaign (current binary, 2026-06-09)

Test files: original test, plus `Tools/threads/bughunt/repro.js`
(instrumented copy — identical oracle; on mismatch dumps got/want decode,
re-reads via three access forms, all 24 siblings, indexed lane, ownKeys,
indexingMode, $vm.dumpCell, and a 5ms-later re-read) and
`Tools/threads/bughunt/repro-loop.js` (5 outer reps per process, fresh
registries per rep, hot JIT). Campaign driver:
`Tools/threads/bughunt/load6x.sh` (load6.sh + EXTRA_FLAGS support).
Background "corpus load" during most campaigns: 6 looping jsc workers over
ic-publish-reset-loops, shared-arraystorage-stress, transition-vs-write,
transition-vs-read, i03-i37-same-shape-add-storm, i03-t5-racing-growers
(`/tmp/corpusload.sh`).

| config | runs | corruption | other failures |
|---|---|---|---|
| 4× load6 concurrently (24 jsc procs), original test, seeds 1000–9019 | 480 | 0 | 0 |
| Exact replication ×4: plain load6, repro.js, SEED_BASE=1000 (includes the historically failing seeds 3012, 6014), + corpus load | 480 | 0 | 0 |
| Seed 3012 solo, quiet-ish host | 10 | 0 | 0 |
| Seed 3012 solo, `taskset -c 0,1` | 10 | 0 | 0 |
| repro-loop.js (5 reps/proc), seeds 31000+ | ~60×5 reps | 0 | 1× GC mark-stack assert (below); 5× rc124 timeout during stress-ng window |
| maxSleepUs=1000 (A) | 90 | 0 | 0 |
| period=64 dup (B) | 90 | 0 | 0 |
| period=16 + maxSleepUs=500 (B2) | 90 | 0 | 0 |
| all workers pinned to cores 0–7 (D) | 90 | 0 | 0 |
| --collectContinuously=1 (E) | 60 | n/a | 60/60 instant `ASSERTION FAILED: m_requests.isEmpty() \|\| (m_worldState.load() & mutatorHasConnBit)` Heap.cpp:1654 — collectContinuously is structurally incompatible with GIL-off; NOT usable as an amplifier |
| --forceRAMSize=100MB (F) | 60 | 0 | 2× STW-watchdog 30s (stress-ng window) |
| baseline (G) + --forceRAMSize=60MB (H) under `stress-ng --cpu 24 --vm 8` | ~120 | 0 | 3× STW-watchdog 30s (JSThreadsSafepoint.cpp:476/494, "CodeBlock jettison" requester, one lite `hasHeapAccess=true` non-quiescent) — known ab17b/ab17c family, fires under extreme host starvation; pollutes campaigns, so extreme oversubscription is NOT a usable amplifier either |
| --useFTLJIT=0 (N1) | 90 | 0 | 0 |
| --useDFGJIT=0 (N2: LLInt+baseline only) | 90 | 0 | 0 |
| --forceSegmentedButterflies=1 (N3) | 90 | 0 | 0 |
| --verifyConcurrentButterfly=1 (N4) | 90 | 0 | 0 — the verifier never tripped |
| baseline loop, ORIGINAL test, plain load6 + corpus load, rounds 7–9 (seeds 70000/80000/90000 bases) | 360 | 0 | 18× rc124 (120s timeouts in round 8, coinciding with a host load-average spike to ~870 from co-tenant workloads) |

§3.1 TOTALS (current binary, this run): ≈2,560 completed runs across 13
configurations, ZERO occurrences of the silent value corruption. Tier
narrowing is therefore UNANSWERED by direct evidence (no config — including
the pinned full-JIT baseline — fired); what the narrowing campaigns DO
establish at the ~90-run level is that none of no-FTL, no-DFG,
forceSegmentedButterflies, verifyConcurrentButterfly makes the bug EASIER to
hit, and the concurrent-butterfly tag-decode/flatness verifier finds nothing
wrong on this workload at this sample size.

Determinism answer (item 1 of the brief): failing seeds do NOT fail
deterministically. Seed 3012: 0/10 solo, 0/10 under taskset-2-cores, 0/~8
exact-replication re-runs under 6-way load. The failure is a function of
host-load timing, not of the seed's yield schedule alone. The RaceAmplifier
seed does not capture enough of the schedule to make this bug replayable.

## 4. Side findings (real, but NOT the chartered bug)

1. **GC mark-stack race (crash, current binary)**:
   `/tmp/bh-loop/FAIL-rc134-w2-r4-s33004.log` — Debug assert
   `result == m_segments.head()->m_top++` in
   `GCSegmentedArray<const JSCell*>::postIncTop()`
   (GCSegmentedArrayInlines.h:145), i.e. two threads pushing/popping one
   mark stack concurrently, seed 33004, repro-loop.js under corpus load.
   First time this signature is on record per /tmp history. Symbolized
   (llvm-symbolizer) — the full chain is:
   `JSC::trySegmentedTransition` (ConcurrentButterfly.cpp:1357)
   → `JSObject::tryPutDirectTransitionConcurrent` (JSObjectInlines.h:938)
   → `VM::writeBarrier` → `Heap::writeBarrierSlowPath`
   → `Heap::addToRememberedSet` (Heap.cpp:1481)
   → `MarkStackArray::append` → `GCSegmentedArray::postIncTop` assert.
   I.e. during concurrent GC, TWO MUTATORS executing concurrent property-add
   transitions append to the SHARED mutator mark stack (remembered set)
   without mutual exclusion. This is exactly the code path the test's
   `late*`/`fromSlot*` concurrent writes exercise. Recorded as fact; whether
   it relates to the silent corruption is undetermined (this failure mode is
   a crash/assert, and remembered-set corruption affects liveness, not
   property offsets).
2. **STW watchdog under extreme starvation**: see table (F/G/H). Known
   family; the participant dump shows a single lite with
   `hasHeapAccess=true` blocking a CodeBlock-jettison stop for >30s wall
   under a 4×-oversubscribed host. Distinguish from genuine wedges before
   acting on these under load campaigns.
3. `--collectContinuously=1` asserts immediately GIL-off (table row E) —
   worth a flag-validation guard eventually.

## 5. rr record-replay

NOT AVAILABLE on this host:
- no `rr` in AL2023 dnf repos;
- upstream rr-5.9.0 rpm has unresolvable deps here (needs glibc-32bit pieces
  and `libcapnp.so.0.10.3` — no capnproto package in AL2023);
- EC2 PMU is only partially exposed: `perf stat -e r5101c4` (retired
  conditional branches, rr's required counter) warns
  "bits 16,20,22 of config not supported by kernel" — rr would most likely
  refuse or be unreliable even if built from source.
Not pursued further. (Also note: rr serializes to one core; this bug needs
real parallelism + load, so even chaos mode would be a long shot.)

## 6. Hard facts any hypothesis must explain

1. **Wrong-offset, not stale-data**: each `p*` slot is written exactly once,
   pre-publication; the butterfly is never reallocated nor the slots
   rewritten during the race window. Yet a foreign reader got the value of
   the OTHER property of the SAME object, with the {p8,p17} pair hit in BOTH
   directions (Δ = ±9 OOL slots / ±72 bytes / PropertyOffset ±9).
2. **Reader-side, by-val, foreign**: both failures are `o["p"+p]` get_by_val
   reads in `checkOne` on the foreign-read path (`theirs[i]`), with the key
   a freshly concatenated rope resolved through the SHARED atom-string
   table; the same checkOne also reads the owner's objects all test long
   without tripping.
3. **Concurrent owner activity is structure transitions only**: during the
   window the owner adds `late<round>` properties (new Structure, same
   butterfly) to one own object per round, and all threads write the common
   `shared` object's `fromSlot*` properties. No butterfly moves.
4. **It is rare and load-shaped**: 2/240 on 2026-06-08 evening vs 0/~2400
   on the same binary, same seeds, same harness on 2026-06-09 under every
   load shape tried (including exact replication, 8-core pinning, 2-core
   taskset, period/sleep variations, small-heap GC pressure). Whatever
   window exists, the RaceAmplifier's instrumented sites + seed do not
   control it; the discriminator is external host-scheduling state.
5. **The value pair is exact and clean**: got/want are both valid Int32
   sibling values — no torn JSValue, no boxed/unboxed confusion, no tag
   garbage. A 5ms-later re-read result is unknown for the historic failures
   (the instrumented repro will answer HEALED vs STILL-WRONG when it next
   fires — `Tools/threads/bughunt/repro.js` is in place for the next
   campaign).
6. **Lineage**: the pre-fix binaries showed a much broader corruption space
   (cross-object/cross-tid); after the 06-08 fix round only the same-object
   {p8,p17} swap shape has been seen. The landed fixes narrowed, not
   eliminated, the wrong-offset read family.

## 7. Minimal reproducing config (best known)

NOT currently reproducible on demand. Best-known historical config:
Debug jsc (2026-06-08 18:27 build), plain `Tools/threads/load6.sh`
(6 workers, SEED_BASE=1000, period=64, maxSleepUs=100) on the original test,
on a host with substantial UNRELATED concurrent load (exact co-load on
2026-06-08 evening unknown) — observed ~2/240 there; 0/~2400 since.
Recommended next instrument: run `Tools/threads/bughunt/repro.js` as the
standing campaign test (identical oracle, rich state dump at mismatch +
HEALED/STILL-WRONG probe) so the NEXT natural occurrence pins down
persistence, sibling state, structureID and butterfly at the moment of
corruption.

## 8. Artifacts

- `Tools/threads/bughunt/repro.js` — instrumented copy (oracle unchanged;
  rich dump + HEALED probe at mismatch).
- `Tools/threads/bughunt/repro-loop.js` — 5-rep hot-JIT variant.
- `Tools/threads/bughunt/load6x.sh` — load6 with EXTRA_FLAGS support.
- `Tools/threads/bughunt/logs/` — preserved copies of the key failure logs:
  the two corruption hits (s3012, s6014), the torn-Structure assert
  (s12011, older binary), the new GC mark-stack assert (s33004, current
  binary), the collectContinuously incompatibility, and a representative
  STW-watchdog overload log (s53002).
- Campaign log dirs (volatile, /tmp): load6-c1..4, bh-exact-1..4, bh-A/B/B2,
  bh-D/E/F/G/H, bh-N1..N4, bh-loop, bh-base-7..9, det3012.

## 9. EXPERIMENTER round 1 (2026-06-09): GR1-remset-lost-append-premature-reclaim

Verdict in one line: the MECHANISM is real, on-demand reproducible, and the
one-line fix is validated — but the mechanism is UNREACHABLE in the failing
workload (the original test performs ZERO GCs, measured solo and under the
06-08 loaded shape, including the historic failing seeds), so GR1 is REFUTED
as the cause of the chartered {p8,p17} silent corruption. The unwired
`setMultiProducerAccess()` remains a confirmed must-fix defect regardless.

### 9.1 Instrumentation (all reverted; forward patches kept)

Scratch build on top of the current tree (Debug):
- `Heap::addToRememberedSet` (Heap.cpp:1480, the ONLY physical append site to
  `m_mutatorMarkStack`): concurrent-entry detector (atomic in-append counter),
  per-append size-delta check, relaxed shadow append counter.
- Drain-side shadow check `appends - drained == size()` at
  `MarkStackMergingConstraint::executeImplImpl` (world stopped) and the
  full-GC `clear()`; per-process SUMMARY (appends/overlaps/anomalies/shadow
  mismatches/gcVersion/allocThisCycle/maxEdenSize) at Heap teardown
  (`--destroy-vm`).
- ConcatKey vehicle check (request 2B): on every flag-on
  `ConcatKeyAtomStringCache::getOrInsert` cache HIT, re-derive the expected
  concatenation from the three fibers and RELEASE_ASSERT on mismatch.
Patches: `gr1/heap-detector-plus-fix.patch`, `gr1/msmc-detector.patch`,
`gr1/concatkey-detector.patch`. Tree restored to pristine and rebuilt after
the experiments (final smoke: s3012 PASS).

### 9.2 Mechanism arm: lost/corrupted appends reproduce ON DEMAND

New amplifier `gr1/barrier-storm.js` (owner-only stores of young cells into
2048 old objects per thread x 5 threads, eden pressure, run with
`--forceRAMSize=8388608` + pinned GIL-off flags): maximizes concurrent
`addToRememberedSet` traffic. NOTE: a first variant doing FOREIGN writes to
shared old objects wedged the STW watchdog (Class-A WatchpointSet fire,
JSThreadsSafepoint.cpp:494) at any heap size — known overload family,
avoided by owner-only writes.

Unpatched binary, 50 runs (seeds 4001-4050, ~3 s each):
- 50/50 CRASH on `ASSERTION FAILED: result == m_segments.head()->m_top++`
  (GCSegmentedArrayInlines.h:145, postIncTop) — the EXACT s33004 signature,
  now deterministic instead of 1-in-hundreds.
- 49/50 runs logged OVERLAP (two threads inside the remset append at once).
- 42/50 runs logged per-append size anomalies BEFORE crashing: delta=2 (784x,
  interleave), delta=510/511/1019/1020 (66x — exactly +-1/+-2 segments;
  s_segmentCapacity is ~510 for 4 KB blocks => racing `expand()` segment-list
  pushes), and ONE delta=0 (a thread observed its OWN append not reflected:
  a lost m_top increment in vivo).
- 2/50 runs survived to a merge drain and tripped SHADOW-MISMATCH:
  `appends=10245 drained=0 actual-size=11248` and `appends=10246
  actual-size=10744` — the stack's bookkept size diverges from the number of
  real appends by ~+-2 segments (both inflation = drains would read
  stale/garbage slot contents as remembered cells, and the loss direction
  GR1 needs). In a Release build all of this is SILENT.
Logs: `gr1/logs/storm-unpatched-*`.

### 9.3 Fix arm: wiring the dormant lock kills the whole failure class

One-line candidate fix (as predicted by the hypothesis): in Heap's
constructor, `if (Options::useJSThreads()) m_mutatorMarkStack->
setMultiProducerAccess();` (MarkStack.h:53 lock plumbing was fully written
and had ZERO callers — confirmed dead code before this).

Patched binary, same 50 storm seeds: 50/50 PASS, 0 asserts, 0 shadow
mismatches across 1,912,411 appends with ~100k contended entries (overlaps
still logged in 50/50 runs — contention is constant; the lock makes it safe).
Residual size-delta anomalies (delta 2..140, two -507) are artifacts of the
DETECTOR's own unlocked before/after `size()` reads (size() is a non-atomic
two-field read: m_top + 510*(nSegments-1)); the authoritative invariant
appends==drained held EXACTLY in every run. Butterfly stress + repro-loop
also pass on the fix arm.

### 9.4 Linkage arm: the failing workload NEVER GCs — GR1 cannot have done it

GR1's corruption story REQUIRES an eden collection inside the
silenced-barrier window (premature reclaim + slot recycling). Measured on
the instrumented binary, original `spawned-thread-butterfly-stress.js`,
pinned flags:
- Solo, seeds 1000/2001/3012(!!)/6014(!!)/9999/12345: gcVersion=0,
  appends=0 every time.
- Under the 06-08 shape (gr1 variant of load6, 6 workers, SEED_BASE=1000,
  stress-ng --cpu 96 co-tenant on the 64-core host): 34 valid runs, ALL
  gcVersion=0, appends=0, zero detector events of any kind.
- Why it never GCs: total allocation is 11.27-11.31 MB per run (measured
  spread across seeds AND under load: +-0.2%) against maxEdenSize=33.55 MB —
  the run ends at ~34% of the first-GC threshold. Allocation is
  workload-determined; host load cannot triple it. No GC => no remembered
  set, no eden sweep, no reclaim => no GR1 corruption path. (GC activity
  timers were never observed to fire in this shell config either.)
- The ConcatKey vehicle check (9.1) stayed silent in every storm/campaign
  run; it remains armed in the patch file for any future natural hit.

By contrast repro-loop.js (5 reps) does GC (gcVersion=1+, ~180 appends/run):
this is exactly why s33004 (the postIncTop assert) was caught on repro-loop
under corpus load — the side-finding-1 crash family belongs to the
barrier-during-GC population, which the ORIGINAL test is not a member of.

### 9.5 Disposition

- GR1 as THE chartered bug: REFUTED (necessary precondition — a GC during
  the run — does not occur in this workload; measured including the two
  historic failing seeds).
- GR1 as a defect: CONFIRMED with an on-demand reproducer
  (`gr1/barrier-storm.js` + `--forceRAMSize=8388608`, 50/50) and a validated
  one-line fix (wire `setMultiProducerAccess()` on `m_mutatorMarkStack` —
  and audit the other shared `MarkStackArray`s) that must land regardless of
  the chartered bug's verdict. Hand the fix through the normal
  propose -> 2-reviewer -> implement flow.
- Side artifact (pre-existing, not the bug): `--destroy-vm` GIL-off teardown
  intermittently asserts `currentThreadIsHoldingAPILock()` (VM.cpp:1044)
  AFTER PASS — 2/18 in one loaded slice
  (`gr1/logs/destroyvm-teardown-assert-artifact.log`). Only affects the
  instrumented SUMMARY harness; excluded from stats.

## 10. EXPERIMENTER round 2 (2026-06-09): TP2 / GR2-r2 / SK2 — ROOT CAUSE FOUND

One-line verdict: **TP2-keyatomcache-return-reload-swap is CONFIRMED as the
chartered bug**, with an on-demand reproducer, an in-vivo smoking-gun
detector trip causally paired with the exact historical corruption signature,
and a validated fix-shape (snapshot return). GR2-r2 (storage exonerated) and
SK2 (test sound / outcome illegal) are both CONFIRMED by the same data.

### 10.1 Instrumentation (all reverted; diffs kept)

Single scratch edit to `Source/JavaScriptCore/runtime/KeyAtomStringCacheInlines.h`
(`Tools/threads/bughunt/tp2/armA-instrumentation.patch`, `armB-instrumentation.patch`):
- one-shot log of the 512-bucket index of "p8"/"p17" (collision check);
- after the hash+equal verification SUCCEEDS, a GIL-off-gated spin (ARM A:
  2000 volatile iters on every hit; ARM A': 30000 iters gated to 2-3-char
  'p'-keys) to widen the verify->return window;
- explicit re-load of the slot after the spin, with a `KEYATOM-RACE` dataLog
  whenever the reload differs from the verified pointer;
- ARM A/A' return the RELOAD (faithful to the stock `return slot;` — the
  Debug build is -O0, CMAKE_CXX_FLAGS_DEBUG="-g", so every mention of `slot`
  is a distinct memory load: TP2 confirmIf(1) holds statically);
- ARM B returns the VERIFIED snapshot (the deferred V7 Race C fix shape),
  spin and detector unchanged.
Tree restored to pristine afterward and rebuilt; final smoke s3012 PASS.

### 10.2 Collision arithmetic verified in vivo

First run, both keys logged once:
`KEYATOM-IDX p8 hash=15955301 idx=357` / `KEYATOM-IDX p17 hash=14316901 idx=357`
(0xF37565 / 0xDA7565 — identical low 16 bits, both -> slot 357 of 512),
exactly as TP2 computed offline. No other p-pair ever appeared in any race
line across all arms.

### 10.3 ARM A / A' — corruption reproduced ON DEMAND with the smoking gun

All runs SOLO (no external load), pinned GIL-off flags, original test:
| arm | window | runs | corruptions | detector trips |
|---|---|---|---|---|
| A (2000-iter spin, all hits) | ~12 | 1 | 2 (1 harmful, 1 benign same-content) |
| A' (30000-iter spin, p-keys) | 10 | 1 | 2 (both harmful) |

The two corruption runs (preserved: `logs/TP2-armA-CORRUPT-s12000.log`,
`/tmp/tp2-armA2/CORRUPT-r1-s21001.log`):
- s12000: `KEYATOM-RACE idx=357 requested=p17 verified=p17 reloaded=p8`
  + `named property corrupt: got 2000008 want 2000017` in the SAME run,
  thrown from checkOne via the foreign-read frame (test line 86) — the
  byte-identical historical signature (same-object {p8,p17}, foreign reader).
- s21001: two trips `requested=p17 verified=p17 reloaded=p8`,
  corruption `got 23008 want 23017` (tid=0 serial=23).
Natural rate was ~1/120 under 6-way load and 0/2560 solo; the widened window
fires ~1/10 SOLO — orders-of-magnitude amplification as predicted.

### 10.4 GR2 arm — storage intact at the instant of corruption

`repro.js` loop under the ARM A' binary: hit at s40011
(preserved: `logs/TP2-GR2-repro-MISMATCH-s40011.log`):
- `KEYATOM-RACE idx=357 requested=p17 verified=p17 reloaded=p8` immediately
  followed by `MISMATCH prop=p17 got=3030008 want=3030017`;
- same-form re-read, Reflect.get, and GOPD all return 3030017 (correct);
- ALL 24 sibling named props correct; indexed lane correct; ownKeys correct;
- $vm.dumpCell: butterfly base/propertyCapacity 32 intact, slot contents
  correct (p17's slot holds 3030017);
- after-5ms re-read: HEALED.
This is GR2-r2's exact confirm condition: the wrong VALUE was manufactured in
the KEY-PRODUCTION lane (wrong atom returned for the right characters), not
in butterfly storage, growth, relocation, or offset resolution. The §6
"wrong-offset" framing in this pack is hereby corrected: the offset WAS
right — for the WRONG uid.

### 10.5 ARM B — snapshot return kills it with the window still hot

Same widened window + detector, `return verified;` instead of the reload:
36 runs (3x12, including every seed that corrupted under A/A': 12000, 21001,
40000-base repro.js) — **0 corruptions, 0 other failures**, while the
detector tripped TWICE with the harmful shape
`idx=357 requested=p8 verified=p8 reloaded=p17` (the MIRROR direction,
matching historic s6014). I.e. the race window was entered and would have
swapped under the stock return; the snapshot return neutralized it. Both
historic directions (p17->p8 and p8->p17) have now been observed in vivo.

### 10.6 SK2 — test soundness chain (static)

- GIL-off Atomics on plain objects dispatch via `vm.gilOff()`
  (ThreadAtomics.cpp:594/613/653/691) to the concurrent probe +
  `JSObject::atomicSlotReadModifyWrite` (ConcurrentButterfly.cpp:2907),
  whose slot accesses are seq_cst loads/CAS (atomicSlotLockFreeLoop,
  ConcurrentButterfly.cpp:2855/2897; lockedUndefinedArm seq_cst). The
  `ready.count` rendezvous therefore establishes happens-before from every
  thread's buildOne writes to every post-barrier foreign read.
- Each p-slot is written exactly once pre-publication; butterfly never moves
  (§2); decode is forced (base ≡ 0 mod 1000).
- And now demonstrated dynamically: the "got p8's value for p17" outcome is
  produced by an engine defect (cross-lite cache race returning the wrong
  atom), not by any legal racy-but-correct execution. The test is sound; the
  outcome is illegal; a fix is required (no test-side waiver).

### 10.7 Mechanism summary + fix gate

`KeyAtomStringCache::make` (KeyAtomStringCacheInlines.h:49-59): `slot` is a
reference into the shared per-VM 512-entry array; the value is verified by
hash+equal, then `return slot;` RE-LOADS the cell, which a colliding-key miss
on another lite can overwrite between the two — returning a fully valid atom
string for the OTHER key. Downstream lookup is then a perfectly correct read
of the wrong property. Explains every §6 hard fact: same-object sibling pair
{p8,p17} both directions (unique 512-bucket collision among the test's keys),
tier-independence (C++ slow path shared by all tiers), no-crash/no-ASAN
(all values live and well-formed), GIL-on immunity and load-shaping (needs a
preemption inside a ~few-instruction C++ window with no RaceAmplifier site),
survival of the ConcatKey/NumericStrings/MegamorphicCache fixes (different
cache), and storage-verifier silence (object model innocent).

FIX TO PROPOSE (normal propose -> 2-reviewer -> implement flow; NOT landed
here): the KeyAtomStringCache.h:35-52 deferred "UNGIL V7 Race C" change —
`std::array<Atomic<JSString*>, capacity>`, make() loads the slot ONCE
(consume/acquire), null-checks BOTH the cached pointer and
`tryGetValueImpl()` (the stock code derefs a possibly-null impl — a rope
JSString stored by a racing lite would crash), verifies the LOCAL, returns
the LOCAL, publishes with release store; clear() relaxed-stores nullptr.
CRITICAL REVIEW POINT (this round's evidence): the fix MUST return the
verified snapshot — an atomic-slot conversion that still `return slot;`
re-loads would keep the bug while silencing TSAN.

### 10.8 Artifacts

- `tp2/armA-instrumentation.patch`, `tp2/armB-instrumentation.patch`,
  `tp2/KeyAtomStringCacheInlines.h.orig`, `tp2/run-arm.sh`, `tp2/NOTES.md`,
  `tp2/armB-note.md`
- `logs/TP2-armA-CORRUPT-s12000.log` (trip + corruption, same run)
- `logs/TP2-GR2-repro-MISMATCH-s40011.log` (trip + full intact-storage dump)
- volatile: /tmp/tp2-armA*, /tmp/tp2-armA2, /tmp/tp2-gr2, /tmp/tp2-armB1..3

## 11. EXPERIMENTER round 3 (2026-06-09): differential gate — fix-completeness CONFIRMED, transition lane CLOSED, prior "failed verification" explained

Hypotheses under test: `TP-r3-keyatom-snapshot-fix-is-complete-transition-lane-exonerated`,
`TP-r3-no-transition-protocol-carrier-exists`, `GR3-A-fix-landed-verification-gate-artifact`.

### 11.1 Instrumentation (scratch; diff preserved at `r3/r3-instrumentation.patch`, REVERTED after)

On TOP of the landed snapshot-return fix (`KeyAtomStringCache.h` Atomic slots,
`KeyAtomStringCacheInlines.h` verified-snapshot return):
- one-shot provenance canary `KEYATOM-SNAPSHOT-FIX-ACTIVE regressArm=<bool>`
  on first GIL-off `make()` hit — every counted log self-certifies its binary;
- ARM-B'-style widened window: after hash+equal verification succeeds, a
  30000-iter spin gated to 2-3-char 'p' keys, then a RE-LOAD of the slot,
  logging `KEYATOM-RACE idx=... requested=... verified=... reloaded=...` when
  the reload differs from the verified snapshot;
- env `KEYATOM_REGRESS=1` arm: on a detected divergence, RETURN THE RELOAD —
  i.e. byte-faithful restoration of the pre-fix `return slot;` semantics in
  exactly (and only) the harmful case. Default arm returns the verified
  snapshot (shipped semantics);
- `Structure::getConcurrently` canary (Structure.cpp ~:2088): under the same
  held table lock, RE-probe `table->get(uid)` and
  `RELEASE_ASSERT(offset2 == offset && attributes2 == entryAttributes)` —
  any torn/mid-rehash/unstable offset answer in the transition/table lane
  would abort the run with rc 134.

Builds: Debug, instrumented binary 07:01 UTC (canary present, `strings`-verified);
clean post-revert binary 07:26 UTC (canary string absent, `strings`-verified).
NOTE on lab hygiene: a sibling session ran part of the campaign on the same
instrumented tree (its results below, log-attributed by the per-run canary)
and reverted the instrumentation + rebuilt at 07:24-07:26 mid-flight; runs
that hit the relink window failed rc=126 ("Permission denied", binary being
rewritten) and are EXCLUDED; every counted run carries (or provably lacks)
the in-log canary, so binary provenance is unambiguous per run.

### 11.2 ARM-FIXED — shipped semantics, window held hot: 0/30, with the window provably ENTERED 9 times

All solo, pinned GIL-off flags, `--randomYieldPeriod=64`, butterfly-stress:

| batch | seeds | runs | corruptions | harmful KEYATOM-RACE trips |
|---|---|---|---|---|
| /tmp/r3-fixed-12000 | 12000-12011 | 12 | 0 | 7 (4x p17->p8 + 3x p8->p17, runs s12001/s12004/s12006/s12009) |
| /tmp/r3-fixed-21000 | 21000-21011 | 12 | 0 | 1 (p17->p8, s21005) |
| /tmp/r3-fixed-a | 12000-12003 | 4 | 0 | 1 (p17->p8, s12000) |
| /tmp/r3-fixed-b r0, -c r0 | 21000, 3000 | 2 | 0 | 0 |
| **total** | | **30** | **0** | **9 (both historic directions)** |

Plus repro.js (rich-dump oracle): 4 instrumented runs s40000-40003, 0 MISMATCH
(+8 clean-binary runs s40004-40011, 0 MISMATCH).
The detector trips prove the verify->return race window is still physically
entered on the shipped code — and the snapshot return makes it harmless.

### 11.3 ARM-REGRESS — pre-fix reload-return restored: corruption ON DEMAND, causally paired

`KEYATOM_REGRESS=1`, same binary, same seeds, same flags (5 instrumented runs):

| seed | result |
|---|---|
| 12000 | `KEYATOM-RACE idx=357 requested=p17 verified=p17 reloaded=p8` + `named property corrupt: got 4018008 want 4018017` — SAME RUN (preserved: `logs/R3-ARMREGRESS-CORRUPT-s12000.log`) |
| 12001 | trip (p17->p8) + `named property corrupt: got 3014008 want 3014017` — SAME RUN (preserved: `logs/R3-ARMREGRESS-CORRUPT-s12001.log`) |
| 12002 | no trip, PASS |
| 12003 | 1 trip, PASS (divergence consumed off the checked-read path) |
| 21000 | no trip, PASS |

2/5 corrupt under reload-return vs 0/30 under snapshot-return on the SAME
binary, seeds, and load shape (Fisher exact p ~= 0.017; against the combined
30 + round-2 ARM-B 36 = 66 zero-corruption snapshot-return runs, far
stronger). The ONLY code difference between the arms is which pointer
`make()` returns after verification. This is the complete differential the
round-2 pack called for: the in-tree fix closes the chartered bug with the
window held hot, independent of host load.

### 11.4 Transition/offset lane: canary armed in all 39 instrumented runs, SILENT — including inside both corruption runs

The getConcurrently double-probe RELEASE_ASSERT never fired in any run
(no rc 134, no assertion text in any log) — including the two ARM-REGRESS
corruption runs, where the wrong value was being manufactured AT THAT MOMENT
in the key lane. Combined with §10.4 (storage/offset intact at the instant of
corruption) this closes the transition-publication lane: there is no
structure/butterfly mispairing mechanism on this workload (add-only `late*`
transitions keep p8/p17 offsets invariant across the whole chain), and the
locked uid-pointer-compared table probe returns stable answers under
concurrent transition load. `TP-r3-no-transition-protocol-carrier-exists`
is CONFIRMED; the lane is retired.

### 11.5 GR3-A — the prior "implemented-but-verification-failed" status was a gate artifact

Direct inspection of the failed verification round's own logs:
- `/tmp/load6-fix.log`: 240/240 PASS on the fixed binary;
- `/tmp/corpus-giloff.log` / `-giloff2` / `-gilon`: contain ZERO
  `named property corrupt` lines. The 4 corpus-giloff fails are
  `heap-client-churn` (ASSERT deferralContext...), `heap-iss-revert`
  (ASSERT ++spins < 1<<24), `heap-epoch-reclaim` (rc 3) and
  `objectmodel/i03-single-threaded-no-change` (rc 3, premise-self-check) —
  all under ambient `JSC_*` env pollution that the harness itself flags
  (`/tmp/races-1.log` WARNING block). None is the chartered signature.
The verification rung failed for unrelated reasons and was misattributed;
no second carrier was ever in evidence. With the provenance canary now part
of the documented method (any future campaign on an instrumented binary
self-certifies), the stale-binary ambiguity class is closed going forward.

### 11.6 Post-revert clean-tree smoke

Tree restored to landed-fix-only state (KeyAtomStringCacheInlines.h reverted
07:24, Structure.cpp canary reverted 07:25, jsc relinked 07:26; binary
contains no KEYATOM strings). Historic failing seeds s3012, s6014, s12000:
all PASS. Natural-rate clean-binary runs this round (regress-b r4-r11 with
the env arm a no-op, repro r4-r11, smokes): 0 corruptions.

### 11.7 Verdict

- `TP-r3-keyatom-snapshot-fix-is-complete-transition-lane-exonerated`: CONFIRMED.
- `TP-r3-no-transition-protocol-carrier-exists`: CONFIRMED (lane retired).
- `GR3-A-fix-landed-verification-gate-artifact`: CONFIRMED (misattribution
  branch demonstrated from the gate's own logs; fix efficacy proven by the
  §11.2/§11.3 differential, which does not depend on natural reproduction).
No refuteIf condition of any hypothesis was observed: no corruption ever
occurred on snapshot-return semantics; both corruptions occurred under
restored reload semantics with a causally-paired detector trip; no
non-colliding-bucket pair ever appeared; the offset-lane assert never fired.
The chartered bug is the TP2 KeyAtomStringCache race, the landed fix is
complete on the evidence, and the hunt for additional carriers of THIS
signature can stand down (standing soak: keep repro.js in the load6 rotation;
any future hit dumps HEALED/STILL-WRONG state and would reopen).

### 11.8 Artifacts

- `r3/r3-instrumentation.patch`, `r3/run-r3.sh`
- `logs/R3-ARMREGRESS-CORRUPT-s12000.log`, `logs/R3-ARMREGRESS-CORRUPT-s12001.log`,
  `logs/R3-ARMFIXED-TRIP-NOCORRUPT-s12000.log`
- volatile: /tmp/r3-fixed-{12000,21000,a,b,c}, /tmp/r3-fixed-repro,
  /tmp/r3-regress-{a,b}, /tmp/r3-clean-smoke-*.log, /tmp/r3-campaign.log

## 12. EXPERIMENTER round 4 (2026-06-09): closure verification — soak, full-corpus collision table, on-demand differential re-certification

Survivors this round all assert the same negative: the KeyAtomStringCache
verify-then-reload race (TP2) is the SOLE carrier, the landed snapshot-return
fix is complete, and the transition/grow/JIT lanes carry nothing
(`TP-r4-lane-closed-no-residual-transition-carrier`,
`GR4-grow-relocate-lane-closed-no-residual-carrier`,
`jit-r4-angle-closed-keyatom-is-sole-carrier`,
`jit-r4-residual-baked-wrong-uid-ic-audited-out`). Round 4 ran the three
cheapest observations that could still separate them from a hidden second
carrier: (a) binary provenance certification, (b) the full-corpus 512-bucket
collision table (the one prediction nobody had actually computed for ALL keys),
(c) the standing soak under a recreated 06-08-shape co-tenant load, plus an
on-demand re-run of the §11.3 causal differential on a fresh rebuild.

### 12.1 Provenance (clean binary)

`WebKitBuild/Debug/bin/jsc` mtime 2026-06-09 07:26 postdates the §11.6 source
reverts (KeyAtomStringCacheInlines.h 07:24, Structure.cpp 07:25); `strings`
shows 0 occurrences of any KEYATOM instrumentation marker; source contains the
snapshot-return fix (single acquire load, verified snapshot returned,
KeyAtomStringCacheInlines.h:53-70) and `WTF::Atomic<JSString*>` slots
(KeyAtomStringCache.h:56, capacity 512). This clean binary was copied to
/tmp/jsc-clean-r4 and used unmodified for the entire soak.
Note: the r3 revert left the four instrumentation #includes (Options.h,
<atomic>, <stdlib.h>, DataLog.h) in KeyAtomStringCacheInlines.h — harmless,
but it makes `patch r3-instrumentation.patch` report hunk 1 as
already-applied; round 4 hand-applied the logic hunks instead (identical
code, see r4/ notes).

### 12.2 Full-corpus collision table (offline, engine-faithful)

New artifact `r4/hashdump.cpp` compiles against the build's own WTF headers
(clang++-21, RapidHash via StringHasher::computeHashAndMaskTop8Bits — the
exact HashTranslatorCharBuffer path) and dumps hash + bucket(=hash%512) for
every key the workload resolves through KeyAtomStringCache:
p0..p23, late0..late59, fromSlot0..4, tid, serial, indexed, count, hits.
Cross-validation: p8=0x00f37565, p17=0x00da7565, both bucket 357 — bit-exact
match with the in-vivo §10.2 KEYATOM-IDX log lines.

Result (`r4/hashdump.out`): 9 colliding buckets total.
- {p8,p17} @357 is the UNIQUE p-vs-p collision — the only pair where BOTH
  keys are oracle-checked and hot (resolved thousands of times per run by
  checkOne). This is exactly why the natural signature was always p8/p17,
  in both directions.
- Cross-family collisions exist and are a REFINEMENT of §10.2's "unique
  collision among the test's keys" wording: {p3,late45}@172, {p15,late57}@353,
  {late19,late47}@32, {late0,late38}@214, {late37,late51,fromSlot3}@406,
  {late7,late44}@426, {late1,late29}@469, {late15,late54}@475. Each late*/
  fromSlot* key is resolved only ~once per thread per run, so the pre-fix
  window probability for these pairs was negligible — consistent with zero
  observed non-p8/p17 signatures.
- Sharpened future discriminator: a corruption involving any key pair NOT in
  this table cannot be a KeyAtomStringCache bucket race and would prove a
  second carrier immediately.

### 12.3 Standing soak — 240/240 clean on the snapshot-return binary under co-tenant load

Two batches of `load6x.sh` (6 workers x 20 runs) on `repro.js` (rich-dump
oracle), clean binary /tmp/jsc-clean-r4 (certified §12.1), full pinned GIL-off
flags + race amplifier, fresh seeds 60000+/70000+:
- Batch 1 under `stress-ng --cpu 96` (1.5x the 64 cores, the closest replica
  of the 2026-06-08 oversubscribed host shape; load avg >60): 120 runs,
  0 failures.
- Batch 2 with the instrumented differential arms + a full ninja jsc rebuild
  as co-tenant load: 120 runs, 0 failures.
Total this round: 240/240 clean — meeting the >=240-run exposure bar of
`TP-r4-lane-closed-no-residual-transition-carrier` with zero
`named property corrupt`, zero BAD, zero nonzero-rc. Cumulative
snapshot-return record now ~2800+ natural-rate runs with 0 corruptions.

### 12.4 Differential re-certification on a FRESH rebuild — and an environment-sensitivity finding

The r3 instrumentation (provenance canary + widened verify->return window +
KEYATOM-RACE detector + env-gated KEYATOM_REGRESS reload-return) was
re-applied and jsc rebuilt from scratch (so this also re-certifies the §11.3
result is not an artifact of one particular binary).

Phase A — under the stress-ng 96-way oversubscription (both arms, 5 runs
each, seeds 12000-12004): 0 corruptions AND 0 window entries in 10 runs.
The oversubscription deschedules the lites together and the few-instruction
window never gets a cross-thread overwrite. This is a real finding: extreme
CPU oversubscription SUPPRESSES the race rather than amplifying it, which is
consistent with the bug's historically capricious natural rate (~2/240 on one
evening, 0/~2400 since) — the window needs true parallelism plus unlucky
preemption, not just load.

Phase B — stress-ng killed, r3-like host (soak batch 2 as background load),
15 runs per arm, seeds 12000-12014, SAME binary:
- ARM-REGRESS (KEYATOM_REGRESS=1): 4/15 `named property corrupt`
  (s12002 got 4029008 want 4029017; s12006 got 11008 want 11017; s12010 got
  3000008 want 3000017; s12013 got 4019008 want 4019017). Every corrupt log
  contains EXACTLY ONE KEYATOM-RACE line, all
  `idx=357 requested=p17 verified=p17 reloaded=p8` — 1:1 causal pairing,
  signature bit-identical to the chartered s3012 failure (p8 value returned
  for p17, delta 9).
- ARM-FIXED (shipped snapshot semantics): 0/15 corruptions with the window
  provably ENTERED twice, including one harmful-direction trip
  `requested=p8 verified=p8 reloaded=p17` (s12000) — the exact schedule that
  corrupts pre-fix — plus one same-content atom-instance swap.
4/15 vs 0/15 on the same seeds/binary/host (one-sided Fisher p≈0.05 alone;
combined with §11.3's 2/5 vs 0/66 the causal case is overwhelming).

### 12.5 Lane checks riding along

The Structure::getConcurrently locked double-probe RELEASE_ASSERT was armed
in all 30 instrumented Phase-A/B runs, including inside all four corruption
runs: never fired. $-free corroboration of §11.4 — the offset lane returned
stable, correct answers at the instant the wrong value was manufactured in
the key lane. No non-{p8,p17} pair, no STILL-WRONG, no torn JSValue, no
butterfly movement appeared anywhere in round 4 (340 runs total).

### 12.6 Round-4 verdict + tree state

- `TP-r4-lane-closed-no-residual-transition-carrier`: CONFIRMED (240/240
  soak at the prescribed exposure; double-probe canary silent in-vivo; no
  refuteIf event).
- `GR4-grow-relocate-lane-closed-no-residual-carrier`: CONFIRMED (same soak;
  differential re-ran and DID corrupt under KEYATOM_REGRESS=1, defeating its
  own refuteIf clause; no storage/offset displacement ever observed).
- `jit-r4-angle-closed-keyatom-is-sole-carrier`: CONFIRMED (differential
  re-certified on a fresh rebuild; §12.2 collision table proves {p8,p17} is
  the unique hot p-vs-p bucket pair, exactly predicting the signature; any
  future non-table pair = second carrier by construction).
- `jit-r4-residual-baked-wrong-uid-ic-audited-out`: CONFIRMED-CONSISTENT
  (0/340 corruptions on snapshot semantics this round; no IC-shaped residual
  surfaced; nothing contradicted the audit).
Tree restored to landed-fix-only state: both r4 instrumentation edits
reverted, jsc relinked, `strings` shows 0 KEYATOM markers, historic seeds
s3012/s6014/s12000 PASS on the final clean binary. THE BUG IS CLOSED for
this hunt; the standing soak (repro.js in the load6 rotation) remains the
reopen tripwire, with §12.2's collision table as the instant
second-carrier discriminator.

### 12.7 Artifacts

- `r4/hashdump.cpp`, `r4/hashdump.out`, `r4/NOTES.md`
- `logs/R4-ARMREGRESS-CORRUPT-s12002.log`, `logs/R4-ARMREGRESS-CORRUPT-s12006.log`,
  `logs/R4-ARMFIXED-TRIP-NOCORRUPT-s12000.log`
- volatile: /tmp/r4-soak-{1,2}, /tmp/r4-regress{,2}, /tmp/r4-fixed{,2},
  /tmp/jsc-clean-r4, /tmp/r4-stress.log
