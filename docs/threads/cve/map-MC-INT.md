# MC-INT mapping — trust-boundary integer arithmetic on sizes/limits

Status: surface map + verdicts, 2026-06-07; **re-verified 2026-06-15** against the
post-ab17e/TSAN/closeout tree (line-number drift recorded inline; S10 added for the
ANNEX-A16 per-lite scratch-buffer registry that landed after the original sweep).
Defensive audit artifact for `jarred/threads`
(`--useJSThreads`). Mechanism class per `docs/threads/CVE-AUDIT.md` §MC-INT:
overflow/underflow/unit-confusion computing sizes, lengths, or limits for shared or
growable storage. Feeder class: at a resize site it yields MC-GROW; at a length site it
yields MC-JIT. Exemplars: CVE-2018-6092 (wasm local-count overflow), CVE-2024-2887
(GSAB underflow leg), CVE-2025-13016 (byte/element unit confusion).

Audit method: every NEW size/limit computation introduced by the threads work was read
end-to-end (segmented-butterfly sizing, GIL-off ArrayBuffer resize/quarantine, shared
heap server indices/tables, atom-table shards, TID space). For each: what is the
attacker-influenced input, where is the release-grade clamp, can a second mutator skew
the inputs between clamp and use, and what consumes the result.

Verdict summary:

| # | Surface | Verdict |
|---|---|---|
| S1 | Segmented element growth (T2) fragment math | immune-by-construction |
| S2 | Flat→segmented conversion fragment counts (§4.2) | immune-by-construction |
| S3 | Segmented out-of-line dictionary growth (§6) | immune-by-construction (backstopped; hardening nit) |
| S4 | GIL-off resizable ArrayBuffer resize + tail quarantine (annex N6 arms 3/4) | **test EXECUTED + PASSING (Release bar) — see the S4 executed record**; design verdict stays needs-test-grade (no attributable root cause for the earlier abort) |
| S5 | Wasm-memory-associated resize delegation (stale page-count subtraction) | immune (validity-checked downstream); spec-visible wart noted |
| S6 | Growable SAB grow | immune-by-construction |
| S7 | Shared heap server: TLC indices + allocator table growth | immune-by-construction |
| S8 | Sharded atom table | immune-by-construction |
| S9 | TID partitions / exhaustion | immune-by-construction |
| S10 | Per-lite scratch-buffer registry + size-class index (ANNEX A16) | immune-by-construction |

Susceptibility test (S4, plus S6 belt-and-braces):
`JSTests/threads/cve/mc-int-resizable-tail-quarantine.js` — executed at the
CVE close-out round: 20/20 GIL-off Release, 3/3 GIL-on. One TEST-BROKEN
repair was needed in the phase-3 S6 storm: the original test asserted
TypeError as the only legal failure for a racing-shrink `grow()` request, but
ECMA-262 SharedArrayBuffer.prototype.grow mandates a **RangeError** for
`newByteLength < currentByteLength` (and that is what
`sharedArrayBufferProtoFuncGrow` raises for `GrowFailReason::InvalidGrowSize`,
JSArrayBufferPrototype.cpp). The repaired oracle keeps the discrimination: a
RangeError is legal only when `byteLength >= target` at observation time
(length is monotone, S6); a RangeError with `byteLength < target` is still
flagged as the skewed-arithmetic failure this phase hunts.

---

## S1. Segmented element growth (T2) — immune-by-construction

Where: `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:2405`
(`tryGrowSegmentedVectorLength`; was `:2187` at first sweep — file grew ~220 lines);
spine layout `Source/JavaScriptCore/runtime/Butterfly.h:439` (`allocationSize`).
Governing spec: SPEC-objectmodel §4.4 (T2), §4.1 (C2/C4 sizing equations), I33.

Attacker input: the requested element index / length. Release-grade clamp chain:

- `JSObject::ensureLength` — `RELEASE_ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH)`
  (`runtime/JSObject.h:1756`), where `MAX_STORAGE_VECTOR_LENGTH =
  IndexingHeader::maximumLength = 0x10000000` (`runtime/IndexingHeader.h:44`), i.e.
  2^28 — the same cap as mainline.
- The direct segmented dense-store driver re-checks in release:
  `if (i >= MIN_SPARSE_ARRAY_INDEX || i + 1 > MAX_STORAGE_VECTOR_LENGTH) return false;`
  (`runtime/ConcurrentButterfly.cpp:2877`) before calling
  `ensureLengthSlowConcurrent(vm, this, i + 1)`.

Arithmetic downstream of the clamp:

- `neededIndexedFragments` and the amortized/max variants are computed as
  `(uint64_t(VL) + 1 + 3) / 4` (`ConcurrentButterfly.cpp:2430-2449`) — widened to
  uint64 BEFORE the +1/divide; with VL ≤ 2^28 the count is ≤ 2^26+1, so the
  subsequent uint32 products (`neededIndexedFragments * butterflyFragmentSlots - 1`,
  `outOfLineFragments + neededIndexedFragments`) cannot wrap. The 2026-06 refactor
  added an explicit `maxIndexedFragments` cap derived from
  `MAX_STORAGE_VECTOR_LENGTH` (`:2445`) — strictly tighter than the original sweep.
- `ButterflySpine::allocationSize(totalFragmentCount)` widens the count to `size_t`
  before multiplying by `sizeof(ButterflyFragment*)` (`Butterfly.h:439-442`).
- `publishedVectorLength = min(coveredVectorLength, MAX_STORAGE_VECTOR_LENGTH)`
  (`ConcurrentButterfly.cpp:2451`).

Adversarial second mutator: a racing grower cannot skew the inputs — each attempt
re-loads the spine once, computes from that one snapshot, and publishes a REPLACEMENT
spine via a single `casButterfly` (I6 spine immutability: a reader never sees a
half-updated count/length pair, which is what made CVE-2024-2887's two-location update
exploitable). The cross-thread length word (`publicLength`) is bumped only via the
monotone CAS-max `bumpPublicLengthToAtLeast` (`Butterfly.h:537`), so a losing
racer cannot regress another thread's bound; readers bound every dereference by
`min(publicLength, SAME-loaded-spine vectorLength)` (C4), so a stale-spine reader
degrades to holes, never to an index past allocated storage. The only debug-only
guard in the function
(`ASSERT(newVectorLength <= MAX_STORAGE_VECTOR_LENGTH)`, `:2409`) sits strictly behind
the two release clamps above.

MC-JIT feeder note: with VL ≤ 2^28 and 8-byte slots, every JIT-side scaled index
(`index * 8` + fragment offsets) fits comfortably in 64-bit addressing with 32-bit
displacements — same envelope as mainline. JIT tiers never recompute fragment counts;
segmented words slow-path to the C++ drivers (SPEC-jit §9.4 predicates; no
`outOfLineFragmentCount` arithmetic exists under `dfg/`, `ftl/`, or `llint/`).

## S2. Conversion fragment counts (§4.2) — immune-by-construction

Where: `Source/JavaScriptCore/runtime/ButterflyInlines.h:384-422`;
consumer `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:638-814`.
Governing spec: SPEC-objectmodel §4.2 (steps 1/3 refit protocol), §4.1 C1-C3.

- `aliasedOutOfLineFragmentCountForConversion` RELEASE_ASSERTs C1
  (`outOfLineCapacity % 4 == 0`, `ButterflyInlines.h:386`) so flat out-of-line storage
  splits into whole fragments — no rounding remainder to confuse.
- `aliasedIndexedFragmentCountForConversion` widens `flatVectorLength` to `size_t`
  before `(1 + VL + 3) / 4` (`ButterflyInlines.h:399`); VL ≤ 2^28 (S1 clamp).
- `aliasedAllocationSizeForConversion` (`ButterflyInlines.h:418-422`) delegates to
  mainline `Butterfly::totalSize` — no new arithmetic.
- TOCTOU on the inputs (the MC-INT-under-races leg): the counts are computed from a
  pre-lock plan, but §4.2 step 3 re-reads `flatVectorLength` UNDER the cell lock and
  takes the `refit` escape when `totalOutOfLineFragments + indexedFragments >
  allocatedTotalFragments` (`ConcurrentButterfly.cpp:731-733`) — the planning-time
  size is never trusted at publication time. C3 (`preCapacity == 0`) is
  RELEASE_ASSERTed (`ButterflyInlines.h:410`), so the ArrayStorage pre-capacity unit
  confusion cannot enter the aliasing equations (AS never segments, I31).

## S3. Out-of-line dictionary growth (§6) — immune-by-construction (backstopped)

Where: `Source/JavaScriptCore/runtime/ConcurrentButterfly.cpp:2116-2170`
(`ensureSegmentedOutOfLineCapacity`); caller
`Source/JavaScriptCore/runtime/JSObjectInlines.h:202`; consumer backstop
`Source/JavaScriptCore/runtime/JSObjectInlines.h:292`.
Governing spec: SPEC-objectmodel §6 (coverage monotone across replacement spines), I33.

The one place a size is narrowed BEFORE allocation:

```
uint32_t needFragments = static_cast<uint32_t>((neededCapacitySlots + butterflyFragmentSlots - 1) / butterflyFragmentSlots);   // :2134
```

`neededCapacitySlots` is `size_t`; for the cast to truncate (the classic
allocate-small/index-big shape of CVE-2018-6092) it needs ≥ 2^34 slots, i.e.
`Structure::outOfLineCapacity(maxOffset + 1)` ≥ 2^34 — ≥ 2^34 property-add transitions
and a ≥ 128 GiB out-of-line image; allocation (`AllocationFailureMode::Assert`)
fail-stops orders of magnitude earlier, and the capacity-doubling arithmetic feeding it
is unchanged mainline `Structure` code. Adversarially assuming it anyway: the
truncated-undercount result is NOT trusted by the consumer — before the structure's
`maxOffset` is raised, `growOutOfLineStorageForConcurrentLockedAdd` independently
RELEASE_ASSERTs real coverage in uint64:

```
RELEASE_ASSERT(static_cast<uint64_t>(butterflyFragmentSlots) * butterflySpine(lockedWord)->outOfLineFragmentCountConcurrent() >= newOutOfLineCapacity);   // JSObjectInlines.h:292
```

so an undercounted spine can never be published behind a larger `maxOffset` — the
mechanism's payoff (length site exceeding storage) is structurally cut. Racing element
resizes cannot shrink coverage out from under the check: out-of-line fragment-pointer
prefixes are copied verbatim by every replacement spine (§6 "coverage is MONOTONE",
comment at `:1896-1903`).

Hardening nit (non-blocking): widen `needFragments` to `size_t` or RELEASE_ASSERT
`neededCapacitySlots <= MAX-representable` at `:2134` so the guard is local rather
than relying on the consumer-side assert.

## S4. GIL-off resizable ArrayBuffer resize + tail quarantine — NEEDS-TEST

Where: `Source/JavaScriptCore/runtime/ArrayBuffer.cpp` —
`resizeGILOff` (`:1237`), `deferShrinkTailGILOff` (`:539`),
`consumeQuarantinedTailOnRegrow` (`:574`), `retireArrayBufferQuarantineEntry` (`:432`).
Governing spec: SPEC-ungil annex N6 arms 3/4 (binding torn-pair table; HANDOUT §6 R10).

This is our direct analog of the exemplar class (resizable storage whose limit
arithmetic feeds a protect/decommit): a GIL-off shrink does NOT decommit on the
resizing thread; it quarantines the page tail `[tailOffset, handle.size())` and the
heap §10 stop later runs `OSAllocator::protect(start, entry.tailSize, false, false)` +
`updateSize(entry.tailOffset)`.

What is provably clamped (release-grade, all under the handle lock):

- `maxByteLength < newByteLength` rejected (`:1184`; `m_maxByteLength` is zeroed
  atomically by detach and the detach flag is re-checked under the same lock, `:1180`).
- Page round-up goes through `PageCount::fromBytesWithRoundUp`, which RELEASE_ASSERTs
  page-multiple and validity (`runtime/PageCount.h:83-94`), then
  `RELEASE_ASSERT(desiredSize <= MAX_ARRAY_BUFFER_SIZE)` (`:1206`).
- Grow-leg subtraction guarded: `bytesToAdd = desiredSize - memoryHandle.size()` only
  under `desiredSize > memoryHandle.size()` (`:1216-1217`).
- Shrink-leg call guarded: `deferShrinkTailGILOff` only under
  `desiredSize < memoryHandle.size()` (`:1250-1251`).

What is NOT release-checked — the suspected-hole shape, and why we believe it holds:

`deferShrinkTailGILOff`'s tail-extension subtraction
(`size_t newlyQuarantined = entry.tailOffset - desiredSize;`, `:552`) and
`entry.tailSize = handle.size() - desiredSize` (`:554`/`:560`) are protected only by
debug ASSERTs (`:541`, `:551`). An execution that reaches `:552` with
`desiredSize > entry.tailOffset` underflows `size_t` and, at the next stop, feeds a
~2^64 `tailSize` to `freePhysicalBytes` + `OSAllocator::protect` (`:409-419`) — at
minimum wild un-mapping of the reserved VA range.

The on-paper induction that excludes it (verified by reading every writer):
1. `tailOffset` is always page-aligned and equals the page-round-up of some published
   logical length (creation `:522-525`; extension `:517`; re-grow trim `:556-560`).
2. The published logical length never exceeds `tailOffset` while an entry is pending:
   a shrink publishes `newByteLength ≤ desiredSize = tailOffset`; a grow ALWAYS calls
   `consumeQuarantinedTailOnRegrow` FIRST (`:1213`, before any allocation/GC-capable
   call), which removes or trims the entry up to the new `desiredSize`.
3. A later shrink has `newByteLength < sizeInBytes ≤ tailOffset` with `tailOffset`
   page-aligned ⇒ `desiredSize = roundUp(newByteLength) ≤ tailOffset`. No underflow.
4. All of 1-3 is serialized by the handle lock; the retire hook runs world-stopped and
   takes the same lock, and tail entries exist only for non-wasm resizable buffers
   (the wasm arm rejects `delta < 0`, `:1191-1194`), so no grow path bypasses the
   `consume` in step 2.

Why needs-test rather than immune: the invariant spans three functions, two locks
(quarantine leaf lock nested inside the handle lock — and NOT held across the pair of
operations, only `tailOffset/tailSize` mutations), the §10 stop hook, and an
"enqueue-then-report-outside-the-lock" dance; it is exactly one refactor away from a
silent release-mode underflow, this is brand-new GIL-off-only code with zero soak, and
the failure is invisible until a stop. The test drives deterministic
shrink/regrow-over-pending-tail/shrink-deeper sequences across page boundaries with
forced full collections between steps (retire path), then an amplifier-ready
multi-thread churn of `resize()` on ONE shared buffer object (a second mutator is the
only way to attack the lock-serialization leg of step 4).

Test: `JSTests/threads/cve/mc-int-resizable-tail-quarantine.js`.

**EXECUTED RECORD (added 2026-06-10, review round — this row previously had NO
closure record despite the test being reported closed; recorded here so the
disposition is traceable):**

- *Observed failure signature (historical):* the only recorded failures are
  (a) the thread-cve-close charter listing this test among the 11 failing
  GIL-off, with no captured signature in any doc, and (b) TSAN-TRIAGE.md r2's
  exit-134 abort ("flaky functional bug, queued for the functional round"),
  which did NOT reproduce in r3.
- *Root cause:* **NOT ATTRIBUTABLE from the tree as it stands.** No doc
  records what change (engine or harness) made the test pass. The quarantine
  functions HAVE moved since the map was written — current line numbers:
  `retireArrayBufferQuarantineEntry` `ArrayBuffer.cpp:432` (map said `:396`),
  `deferShrinkTailGILOff` `:539` (map said `:503`),
  `consumeQuarantinedTailOnRegrow` `:574` (map said `:538`), wired at
  `:1305`/`:1347` — i.e. the file was edited between the map and today, but
  whether the edit fixed the abort or the abort was a since-fixed sibling
  composition bug (the MC-SAFE S4 / LazyProperty / DOS S4 rounds all touched
  the same stop machinery this test leans on) cannot be distinguished
  without history access. **Explicitly recorded: the failure stopped
  reproducing; it was not point-fixed under this test's name.**
- *Pass bar achieved (2026-06-10 re-verification, Release):* 5/5 GIL-off
  (`--useJSThreads=1 --useThreadGIL=0` + full GIL-off env), default tiers,
  all three phases (deterministic shrink/regrow matrix, cross-thread resize
  churn, growable-SAB storm). Earlier this round the implementer reported
  20/20.
- *Residual-risk disposition:* because no root cause is attributable, this
  closure is OBSERVATIONAL, not causal. The test stays in the suite as the
  regression gate for the N6 arms-3/4 induction; the TSAN no-JIT and
  Debug/ASAN rungs (audit preamble bars (b)/(d)) should re-run it
  specifically, since the historical signature was an abort, not a JS
  failure.
- *Family-bar re-verification (EXECUTED 2026-06-10, post-review round):*
  the rungs the previous bullet demanded were run on this tree:
  **20/20 GIL-off Release** (default tiers, full GIL-off env +
  `--useJSThreads=1 --useThreadGIL=0`), **5/5 Debug** (same flags — the
  abort oracle class the historical exit-134 signature lives in), and
  **3/3 TSAN no-JIT** (`WebKitBuild/TSan/bin/jsc --useJIT=0`, TSAN.md
  default options, no reports — the exact config of the historical r2
  abort). The exit-134 abort did NOT reappear on any abort-sensitive rung.
  Status remains REGRESSION-WATCH (closure is still observational — no
  root cause was ever attributed — and "stopped reproducing" is not
  "fixed"), but the earlier discrepancy (5/5 Release default-tiers-only vs
  a claimed 20/20) is reconciled: both bars now hold on the recorded tree.

## S5. Wasm-memory-associated resize delegation — immune (validity-checked); wart noted

Where: `Source/JavaScriptCore/runtime/ArrayBuffer.cpp:1400-1460` (GIL-off leg of
`ArrayBuffer::resize`, wasm-memory delegation around `:1412-1417`).
Governing spec: SPEC-ungil annex N6 arm 4 (wasm grow).

The delegation decision runs OUTSIDE the handle lock (deliberately — comment `:1281-1293`),
so `oldPageCount = PageCount::fromBytes(memoryHandle->size())` (`:1305`) and the
relaxed `sizeInBytes` load (`:1296`) can both be stale. A racing grow can therefore
produce `oldPageCount > newPageCount` even though `deltaByteLength ≥ 0` passed, and

```
PageCount(newPageCount.pageCount() - oldPageCount.pageCount())   // :1307
```

underflows uint64 — a textbook MC-INT input skewed by a second mutator. Why it cannot
land: `PageCount(uint64_t)` is unvalidated, but every consumer re-derives under the
wasm memory's own lock — `Memory::growShared` computes `oldPageCount + delta` via
`PageCount::operator+`, which returns the invalid sentinel on uint64 sum overflow OR
`isValid()` failure (`runtime/PageCount.h:108-116`, `maxPageCount` ≤ 2^26), and then
rejects `!newPageCount || !newPageCount.isValid()` plus the `maximum()` bound
(`wasm/WasmMemory.cpp:264-279`). An underflowed delta (≈2^64) always trips one of the
two: the addition wraps (sentinel) or the sum exceeds `maxPageCount`. So the skewed
arithmetic is contained to a failed grow; no length is published, no commit happens.

Residual (not MC-INT, recorded for completeness): `std::ignore = memory->grow(...)`
(`:1307`) swallows that failure and `return deltaByteLength;` reports a successful
positive delta to the caller — a spec-visible correctness wart under races (caller
believes the resize happened). Also the relocating `BoundsChecking` grow's stop
conduction is an explicitly OPEN DEPENDENCY (comment `:279-292`); that hazard is
MC-GROW/MC-TEAR territory and is tracked by the N6 audit, not here.

## S6. Growable SAB grow — immune-by-construction

Where: `Source/JavaScriptCore/runtime/ArrayBuffer.cpp:1547-1595`
(`SharedArrayBufferContents::grow`), monotone reject at `:1559`, commit-subtraction
RELEASE_ASSERT at `:1586`.
Governing spec: annex N6 (grow is base-immutable, commit-then-release-length).

The CVE-2024-2887 underflow leg was "shrink a growable shared buffer's length via
skewed arithmetic". Ours is structurally monotone: under the handle lock,
`if (sizeInBytes > newByteLength || m_maxByteLength < newByteLength) return
InvalidGrowSize` (`:1448`) rejects any non-growth before any arithmetic;
`RELEASE_ASSERT(desiredSize > memoryHandle->size())` (`:1475`) guards the
`extraBytes` subtraction; page math goes through the RELEASE_ASSERTing `PageCount`
constructors; the larger length is release-published only after pages are committed.
Concurrent growers fully serialize on the handle lock, and `m_sizeInBytes` only ever
increases, so no interleaving produces a shrink for cached-length holders (that
residual — JIT code holding an OLD smaller length — is safe by construction and is
MC-GROW/MC-JIT's problem, not an arithmetic one). The susceptibility test includes a
multi-thread `grow()` storm with boundary reads as belt-and-braces.

## S7. Shared heap server: TLC indices + allocator table — immune-by-construction

Where: `Source/JavaScriptCore/heap/MarkedSpace.cpp:758-764`
(`reserveThreadLocalCacheIndices`),
`Source/JavaScriptCore/heap/GCThreadLocalCache.cpp:180-199` (`growTable`).
Governing spec: SPEC-heap §5.3 (T4 index assignment, grow-only table), I2/I5b.

- Index reservation is monotone under `m_directoryLock` with an EXPLICIT wrap guard:
  `RELEASE_ASSERT(next > base); // Overflow would alias TLC slots across subspaces.`
  (`MarkedSpace.cpp:708`). Aliasing TLC slots is the type-confusion payoff this class
  wants; the guard fail-stops it, and reaching it needs ~2^32/numSizeClasses subspace
  creations (not attacker-scalable — subspaces are created per C++ type, not per JS op).
- `growTable`: `m_tableBound * 2` can wrap in principle, but
  `newBound = max(neededBound, max(m_tableBound * 2, 32))` is always ≥ `neededBound`,
  the malloc size is widened (`static_cast<size_t>(newBound) * sizeof(Allocator)`,
  `:186`), and `neededBound = tlcIndex + 1` where `tlcIndex` descends from the guarded
  reservation — so the bound-check + indexed-load contract (contents → pointer → bound
  publication order, `:197-203`) never indexes past the allocation. Table is
  single-owner (I2); no cross-thread writer can skew `m_tableBound` mid-grow.
- Block-level sizing (`tryAllocateBlock`, `addBlock` index resizes) is byte-for-byte
  mainline arithmetic, now serialized under MSPL (SPEC-heap §5.6, I5b) — the threads
  work added serialization, not new size computation.

## S8. Sharded atom table — immune-by-construction

Where: `Source/WTF/wtf/text/SharedAtomStringTable.h:76-119`, `.cpp:37-109`.
Governing spec: SPEC-vmstate §4.2 (shard choice), §7 (leaf locks), I5.

"Shard growth" turns out not to exist as new arithmetic: the shard COUNT is a
compile-time constant (128, `shardCountLog2 = 7`, `:76-77`); the shard pick is a pure
masked shift of the 24-bit string hash (`:116`) — any hash value maps to a valid
shard, so there is no out-of-range computation to corrupt; and per-shard growth is
stock `WTF::HashTable` expansion (the same `StringTableImpl` mainline uses
per-thread), executed entirely under that shard's leaf lock (`Locker` at every
add/remove path; migration `.cpp:90`), so its capacity-doubling arithmetic runs
single-writer exactly as on `main`. No threads-introduced size computation exists on
this surface. (The §4.8 latch-ordering hazards on this table are MC-INIT/MC-LIFE
material, mapped separately.)

## S9. TID partitions / exhaustion — immune-by-construction

Where: `Source/JavaScriptCore/runtime/ThreadManager.cpp:342-360` (spawned,
exhaustion check `:352`), `:446-460` (carrier, `RELEASE_ASSERT(m_nextCarrierTID <
notTTLTID)` `:456`), `:540` (`completeRebiasIfPendingLocked`).
Governing spec: SPEC-ungil §A.3.6/ANNEX A36 (partition), §D.1 (rebias), SD9.

A 16-bit TID that wrapped or got reissued without rebias would alias a live butterfly
TID tag — integer-arithmetic-on-a-limit feeding type confusion, squarely this class.
The code fail-stops instead of wrapping: spawned allocation returns null (→ RangeError)
at `m_nextTID >= carrierTIDBase` (`:312-323`); carrier allocation
`RELEASE_ASSERT(m_nextCarrierTID < notTTLTID)` (`:416`); free-list reuse happens ONLY
after the D1 restamp protocol (`completeRebiasIfPendingLocked`, `:496-520`), and the
75% pressure arithmetic is per-partition uint32 with maxima ≈ 16383·4 — unwrappable
(the per-partition refinement at `:461-471` exists precisely because the naive
75%-of-2^15 threshold was an arithmetic liveness bug; it is documented and bounded).

## S10. Per-lite scratch-buffer registry + size-class index — immune-by-construction

Where: `Source/JavaScriptCore/runtime/VMLite.cpp:508-558`
(`VMLite::scratchBufferForSize`), `:644-654` (`ScratchBufferRegistry::allocateIndex`),
`:668-696` (`VMLite::ensureScratchBufferAtIndex`); table geometry
`Source/JavaScriptCore/runtime/VMLite.h:350-352` (`scratchSegmentSize = 64`,
`maxScratchSegments = 256` ⇒ 16384 entries).
Governing spec: SPEC-ungil ANNEX A16 (baked-index registry + per-lite segmented
table; non-baked size-class arm), SPEC-vmstate §6.6 Group 5.

This is the one NEW threads-introduced size/limit surface since the 2026-06-07 sweep:
a process-wide monotonic index registry feeding a per-lite fixed-geometry segmented
pointer table, with a power-of-two size-class dispatch on top. Three arithmetic
boundaries:

- **Size→class**: `RELEASE_ASSERT(size <= 1<<63)` (`:531`) BEFORE `std::bit_ceil(size)`
  (which is UB above 2^63), then `sizeClass = countr_zero(classSize)` ∈ [0, 63] indexes
  the 64-entry static `s_scratchSizeClassIndices` array — cannot exceed
  `numScratchSizeClasses = sizeof(size_t)*8` (`:313`). No wrap, no unit confusion
  (bytes throughout).
- **Index allocation**: `allocateIndex` is monotone under `m_lock` and
  `RELEASE_ASSERT(index < maxScratchSegments * scratchSegmentSize)` (`:651`) —
  fail-stop at 16384, the table's compile-time capacity. The index is the ONLY value
  that ever reaches the segmented table; the segment lookup
  `scratchSegments[index >> 6]` and entry mask `index & 63` (`:677`/`:686`) therefore
  cannot exceed the 256-segment static array. The `ensureScratchBufferAtIndex` bound
  check at `:670` is debug-only, but every caller (size-class arm `:554`, install fan,
  backfill `:703`) supplies a registry-minted index, so the release guard at `:651` is
  the dominating clamp.
- **Cross-thread skew**: index allocation and the size-class bitmap are each fully
  serialized under their own leaf lock (`m_lock` / `s_scratchSizeClassLock`); the
  per-lite install is single-owner (I11, asserted `:513`) under `scratchBufferLock`.
  No second mutator can skew `index` between the `:651` clamp and the `:677` shift.

Attacker influence: the `size` argument is JIT-compiler-derived (spill-slot counts,
OSR exit scratch), not directly user-JS-controllable; even granting an adversarial
`size`, the `:531` + `:651` RELEASE_ASSERTs close both ends. The deliberately-retired
`sizeOfLastScratchBuffer` high-water comparison (the design note at `:282-296` calls
out exactly the undersized-return heap-overflow this class hunts) is dead by
construction — `scratchBuffers` is now an ownership list only, never a lookup
structure, and `scratchBufferForSize` never consults `.last()`.

---

## Cross-cutting conclusion

The threads implementation's sizing arithmetic is uniformly written in the post-CVE
idiom the exemplars taught: widen-before-multiply (`Butterfly.h:439`,
`ConcurrentButterfly.cpp:2430`), RELEASE_ASSERT at trust boundaries
(`JSObject.h:1756`, `ButterflyInlines.h:386,410`, `MarkedSpace.cpp:764`,
`PageCount.h:85-87`, `VMLite.cpp:531,651`), monotone-CAS for shared length words
(`Butterfly.h:537`), and
single-snapshot immutable publication (I6) so no reader can pair sizes from two
different computations. The one surface where soundness rests on a multi-function
inductive invariant with only debug-grade local checks — the GIL-off resizable-buffer
tail quarantine (S4) — gets the susceptibility test.
