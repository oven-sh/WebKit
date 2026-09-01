# SHAREDHEAP-ALLOC-EVIDENCE

Allocation-throughput evidence for the SCALEBENCH §40 residual gap (JS intcs/nomap
vs Java, W=1 floor). Pinned env: `JSC_useJSThreads=1 JSC_useThreadGIL=0
JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1`. Release `WebKitBuild/Release/bin/jsc` @ 42f50113c0c1.

## Round 1

### (4) RSS BASELINE — `/usr/bin/time -v`, 3 reps each

| arm     | W  | total_ms (3 reps)             | median ms | peak RSS KB (3 reps)          | median MB |
|---------|----|-------------------------------|-----------|-------------------------------|-----------|
| intcs   | 1  | 7659 / 7668 / 7917            | 7668      | 435616 / 432084 / 431388      | **432**   |
| nomap   | 1  | 6586 / 6531 / 6549            | 6549      | 426876 / 427520 / 427116      | **427**   |
| default | 1  | 18586 / 18448 / 18370         | 18448     | 433208 / 435692 / 441348      | **436**   |
| intcs   | 16 | 6254 / 3335 / 6527 (bimodal)  | 6254      | 1220800 / 1310136 / 1231752   | **1232**  |
| nomap   | 16 | 3034 / 2797 / 2625            | 2797      | 515584 / 522188 / 512280      | **516**   |
| default | 16 | 10213 / 12876 / 13019         | 12876     | 1426252 / 1391824 / 1375992   | **1392**  |

RSS-CONSTRAINT thresholds (+10% rejection ceiling): intcs W=1 ≤ **475 MB**,
intcs W=16 ≤ **1355 MB**, nomap W=1 ≤ **470 MB**, nomap W=16 ≤ **567 MB**.
Note nomap W=16 RSS is **2.4× lower** than intcs W=16 — Map's
JSOrderedHashTable buffer + bucket churn dominates W=16 footprint.

### (5) GIL-ON DISCRIMINANT — how much W=1 gap is "sharedGCHeap mode tax" vs "plain JSC alloc cost"?

`bench.js -- 1 <arm>` with **only** `JSC_useJSThreads=1` (GIL on, no shared
heap, no VMLite); reps {5886, 5682, 5836, 5943} intcs / {4879, 4733, 4941}
nomap. Further isolation runs (1 rep each): +VMLite+SharedAtom only = 5972;
+sharedGCHeap only (GIL on) = 6852; full-flags-but-GIL-on = 6573.

| config                              | intcs W=1 ms | nomap W=1 ms | vs Java (1899/1208) |
|-------------------------------------|--------------|--------------|---------------------|
| Java (§40)                          | 1899         | 1208         | 1.00×               |
| `useJSThreads` only (no sharedHeap) | **5836**     | **4879**     | 3.07× / 4.04×       |
| pinned GIL-off                      | 7748 (avg)   | 6555 (avg)   | 4.08× / 5.43×       |
| **sharedHeap+GILoff tax**           | **+1912 (25%)** | **+1676 (26%)** |                |

**Decomposition of intcs W=1 gap (5849 ms vs Java):**
- "Plain JSC cost" (no shared heap): **+3937 ms (67%)** — this is the floor a
  TLAB-style fix CANNOT touch. Per perf below it is NOT alloc-dominated; it is
  string-equal + rope-resolve + Map-bucket + IC-miss + lock.hold-callback
  overhead spread thin.
- sharedGCHeap + GIL-off tax: **+1912 ms (33%)** — the part the per-allocateCell
  hypothesis targets. Within this, GIL-on-with-sharedGCHeap = 6573 → ~737 ms is
  shared-heap-mode-with-GIL, ~1175 ms is GIL-off-only (per-cell lock/traps/TLS).

### (1) `perf record -g -F 997` on W=1 intcs (pinned, 8337 ms wall under perf)

Self-time, alloc-path symbols only (children unreliable across JIT frames):

| category                                         | self %  | abs ms | notes |
|--------------------------------------------------|---------|--------|-------|
| **C++ per-cell wrapper** (sharedGCHeap dispatch) | **3.0%** | ~250  | `allocateCell<JSRopeString>` 1.37, `CompleteSubspace::allocateForClient` 1.03, `Heap::allocationClientForCurrentThread` 0.46, `GCThreadLocalCache::allocatorForSizeStep` 0.24, `CompleteSubspace::allocate` 0.28, `allocateCell<JSArray/JSString/JSFunction>` 0.53 |
| **Refill / sweep** (slow-path block handout)     | **4.1%** | ~340  | `MarkedBlock::specializedSweep` (Rope+String+generic) 2.90, `LocalAllocator::tryAllocateIn` 0.31, `MarkedBlock::Handle::sweep` 0.22, `didConsumeFreeList` 0.16, `MarkedBlock::tryCreate` 0.10s/0.73c, `BlockDirectory::*` 0.10, `allocateSlowCase` 0.05 |
| **writeBarrierSlowPath**                         | 1.05%   | ~88   | unchanged GIL-on (82 ms) |
| **Creation wrappers** (around allocateCell)      | ~6.0%   | ~500  | `JSRopeString::create` 0.80, `jsString` 0.54, `JSArray::tryCreate` 0.91, `operationMakeRope2` 0.67, `JSCellButterfly::createFromArray` 0.85, `ensureLengthSlowConcurrent` 1.12, `JSOrderedHashTableHelper::copyImpl` 0.63, `jsSubstringOfResolved` 0.31, `operationNewArrowFunction…` 0.21 |
| **WTF malloc** (StringImpl backing, libpas)      | ~2.1%   | ~175  | `fastCompactMalloc` 0.95, `tryFastCompactMalloc` 0.25, `pas_thread_local_cache_flush_deallocation_log` 0.41, `StringImpl::createUninitialized` 0.26, `~StringImpl` 0.24 |
| **Kernel page-fault** (first-touch fresh blocks) | 2.6%c   | ~217  | `asm_exc_page_fault` children; M2-alloc-tax-residual (c) note in LocalAllocator.cpp:292 already documents this |
| **TOTAL alloc-adjacent**                         | **~18-19%** | **~1500** | of 8337 ms perf-wall |

**Non-alloc top costs (for context — these are the "plain JSC" floor):**
`WTF::equal(StringImpl,StringImpl)` 4.68% (Map key compare),
`lockProtoFuncHold` 4.30% self / 29.6% children (the Lock.hold native wrapping
each shard critical section), `JSRopeString::resolveRope` 1.67%,
`operationCompileFTLLazySlowPath` 1.59%, `tryStructureOnlyTransition` 1.03%,
`getOwnNonIndexPropertySlot` 1.03%, `PropertyTable::findConcurrently` 0.99%,
`JSCellLock` dtor 0.98%, `operationGetByIdGaveUp` 0.90%,
`JSOrderedHashTableHelper::addImpl` 0.90%, `operationMapGet` 0.80%,
`DeferTermination` ctor 0.72%, `trapsMaybeNeedHandlingForCurrentThread` 0.71%.
JIT code (FTL) ≈ 18% across 6 hot addresses.

**GIL-on perf diff** (same arm, `JSC_useJSThreads=1` only, 5943 ms): the four
sharedGCHeap-only dispatch symbols **vanish** —
`CompleteSubspace::allocateForClient` 0%, `allocationClientForCurrentThread` 0%,
`GCThreadLocalCache::allocatorForSizeStep` 0%; `allocateCell<JSRopeString>`
drops 1.37%→0.16%. Absolute: ~248 ms of the +1912 ms tax is visible as **C++
per-cell allocator-lookup indirection**. Sweep/refill absolute time is ~same
(146 vs 133 ms), confirming refill COUNT parity ((3) below). The remaining
~1660 ms tax is in JIT-inlined per-cell code + `JSCellLock`/traps overhead not
attributable to a single C++ symbol.

### (2) Allocation count + size-class histogram (W=1 intcs, instrumented & reverted)

Instrumented `LocalAllocator::allocateSlowCase` to sum
`m_freeList.originalSize()/cellSize` at each refill (= cells consumed since
last refill ≈ total allocs through this LocalAllocator, JIT + C++).

**Total allocateCell calls: ~70.86 million** (W=1 intcs, one run). At 7685 ms
wall → **108 ns/cell average**. Java's TLAB bump is ~2-5 ns/cell; Go ~10 ns.

Top size classes by **cell count**:

| sz (B) | cells (M) | %     | refills | likely type (JSString.h:87 confirms 16/32) |
|--------|-----------|-------|---------|---------------------------------------------|
| 32     | 34.29     | 48.4% | 68 842  | JSRopeString                                |
| 16     | 16.23     | 22.9% | 24 531  | JSString                                    |
| 48     | 12.09     | 17.1% | 36 409  | (HashMapBucket / 3-fiber rope / small obj)  |
| 64     | 7.29      | 10.3% | 37 739  | JSArray / JSFinalObject                     |
| 880    | 0.220     | 0.31% | 17 745  | butterfly / array storage                   |
| 1008   | 0.210     | 0.30% | 33 240  | butterfly / Map buffer (close to 1 KB step) |
| 432    | 0.171     | 0.24% | 8 037   | butterfly                                   |
| 256    | 0.112     | 0.16% | 2 130   |                                             |
| 160    | 0.123     | 0.17% | 1 318   |                                             |
| 224    | 0.116     | 0.16% | 1 759   |                                             |

**98.7% of cells are in 4 size classes (16/32/48/64)**. But by **refills** the
large classes punch above their weight: sz=1008 alone is 14.3% of refills
(33 240 / 232 402) for 0.30% of cells — only ~6.3 cells per refill at that
size. sz≥432 = 25.5% of refills.

nomap W=1: **56.14 M cells** (21% fewer than intcs — the Map bucket/buffer
allocs). GIL-on intcs: **75.58 M cells** (slightly different sz=48/64/80 split
— object-model layout differs under sharedGCHeap, but total ±7%).

### (3) Refill rate (W=1 intcs, instrumented & reverted)

**232 402 refills** for 70.86 M cells → **305 cells/refill avg**, i.e.
**0.33% of allocs hit the slow path**. Per-refill cost from perf self-time:
refill+sweep category ≈ 4.1% × 8337 ms / 232 402 ≈ **~1.5 µs/refill**.

GIL-on (no sharedGCHeap): **235 122 refills**, 75.58 M cells — refill COUNT is
unchanged by sharedGCHeap. The +1912 ms tax is per-cell / per-refill OVERHEAD,
not extra refills.

### (6) Fresh-block bump availability (W=1 intcs, branch-counted in `allocateSlowCase` isSharedServer leg)

| refill outcome              | count   | %      | freelist shape after           |
|-----------------------------|---------|--------|--------------------------------|
| `tryAllocateFromOwnDirectory` (recycled cursor) | 170 980 | **73.6%** | fragmented intervals (free-list pop) |
| `findOwnEmptyBlockForRefill`                    | 0       | 0.0%   | (never hit)                    |
| cross-subspace steal (tryLock won)              | 15 988  | 6.9%   | full-block bump (stolen empty) |
| `tryAllocateBlock` (fresh mint)                 | 45 086  | **19.4%** | full-block bump (fresh page)   |
| legacy server-wide                              | 0       | 0.0%   | (never reached)                |

**26.3% of refills already produce a pure-bump freelist** (fresh-mint + steal).
73.6% are recycled blocks. At the **per-cell** level the FreeList fast path
(FreeListInlines.h:38) is interval-bump for *every* cell within an interval; the
"free-list pop" cost is the `FreeCell::advance` once per interval, not per cell.
So today's allocator is already bump-within-interval for ≥99% of cells; the
"real TLAB" win would be (a) eliminating the per-cell sharedGCHeap allocator
**lookup** (`allocationClientForCurrentThread` → `allocatorForSizeStep` →
`allocateForClient`), and (b) reducing the 232 K refill round-trips through the
stripe lock — NOT changing the bump itself.

nomap W=1 contrast: ownDir=102 030 (53%), steal=119 (0.06%), freshMint=88 587
(46%) — far more fresh-mints, near-zero steals. nomap lacks the Map-buffer
directories whose empties intcs steals from; without that reuse pool nomap
mints ~2× as many fresh pages (matches the M2-alloc-tax-residual page-fault
note at LocalAllocator.cpp:292).

### Synthesis

1. **The §40 "per-allocateCell cost" hypothesis is ~25% of the W=1 gap, not all
   of it.** GIL-on/no-sharedHeap intcs W=1 = 5836 ms is still 3.07× Java. A
   perfect sharedGCHeap-tax fix gets us from 4.08× to 3.07×, no further.
2. **70.9 M allocs, 232 K refills, 305 cells/refill.** Fast-path hit rate is
   already 99.67%. Refill cost ~1.5 µs and ~4.1% of wall — not the bottleneck.
3. **The visible sharedGCHeap per-cell tax is allocator LOOKUP, not bump.**
   `allocationClientForCurrentThread` + `allocatorForSizeStep` +
   `allocateForClient` + the fattened `allocateCell<T>` body = ~250 ms in C++
   alone, all absent GIL-on. The JIT-inlined equivalent is the likely home of
   another large slice (not symbol-attributable).
4. **A "real TLAB" (per-thread fresh-block cache) targets the wrong lever.**
   It would reduce 232 K refills (~340 ms) and the 45 K fresh-mints'
   page-faults (~217 ms) — total ceiling ~550 ms — at the cost of holding more
   blocks per thread (RSS risk). It does NOT touch the per-cell lookup tax.
   **Higher-leverage candidate: cache the LocalAllocator* per (thread,
   size-class) and skip the 3-hop dispatch** — that's where the ~250 ms+ of
   directly-measured sharedGCHeap-only overhead lives, and it costs zero RSS.
5. **RSS budget is tight on nomap W=16 (516 MB → 567 MB ceiling).** Any
   per-thread block cache must be sized so 16 threads × N hot size classes ×
   16 KB stays under ~50 MB; with 4 hot classes that's ≤48 blocks/thread.
6. **25.5% of refills service sz≥432 (butterfly/Map-buffer)** at ≤37
   cells/refill. If a TLAB cache is built, these are the size classes that
   benefit most per-block-held — but they're also the RSS-expensive ones.
