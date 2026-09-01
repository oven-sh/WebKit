# GILOFF-TAX-EVIDENCE

Quantified breakdown of the W=1 intcs GIL-off tax. Baseline: §41 SHAREDHEAP-ALLOC-EVIDENCE.md.

## Round 1

Repo @ e73a5af68ebd, Release `WebKitBuild/Release/bin/jsc`.
Bench: `Tools/threads/scalebench/js/bench.js -- 1 intcs`.
Method: perf record (cycles, fp callgraph) + perf-probe uprobe exact-count of hot
symbols, three configs:

| config                                              | total_ms | Δ vs (a) |
|-----------------------------------------------------|---------:|---------:|
| (a) `JSC_useJSThreads=1` (GIL-on, no sharedGCHeap)  |     5857 |        — |
| (b) (a) + sharedGCHeap=1, GIL still ON              |     6676 |     +819 |
| (c) full pinned GIL-off env                         |     7831 |    +1974 |

So the ~1974ms tax splits cleanly into a **sharedGCHeap-only component (~819ms)**
and a **gilOff-only component (~1155ms)**. The §41 framing (T1-T5) cuts across both.

### (1) perf diff (c) vs (a), self-time-ms, JIT collapsed

Normalised to wall ms (off=8263 perf-run, on=6092 perf-run; Σdiff=1973ms ≈ tax).

| Δms   | OFFms | ONms | symbol |
|------:|------:|-----:|--------|
| +751  | 3009  | 2257 | `[JIT-code]` (collapsed) |
| +145  |  332  |  187 | `lockProtoFuncHold` |
| +143  |  143  |    0 | `operationCompileFTLLazySlowPath` |
| +110  |  346  |  236 | `[kernel]` |
| +103  |  103  |    0 | `CompleteSubspace::allocateForClient` |
|  +82  |   82  |    0 | `~Locker<JSCellLock>` |
|  +80  |   80  |    0 | `JSArray::tryCreate` |
|  +74  |   74  |    0 | `JSRopeString::create` |
|  +68  |   69  |    1 | `operationMakeRope2` |
|  +60  |   60  |    0 | `JSCellButterfly::createFromArray` |
|  +47  |   70  |   24 | `VM::trapsMaybeNeedHandlingForCurrentThread` |
|  +42  |   45  |    3 | `CachedCall::callWithArguments<JSValue,JSValue>` |
|  +38  |   38  |    0 | `GCThreadLocalCache::allocatorForSizeStep` |
|  +38  |   38  |    0 | `allocateCell<JSRopeString>` |
|  +32  |   32  |    0 | `allocateCell<JSArray>` |
|  +31  |   31  |    0 | `Heap::allocationClientForCurrentThread<VM>` |
|  +31  |   41  |    9 | `sizeFrameForVarargs` |
|  +30  |   30  |    0 | `operationNewArrayWithSize` |
|  +29  |   29  |    0 | `JSLexicalEnvironment::create` |
|  +25  |   29  |    4 | `operationSetupVarargsFrame` |
|  +24  |   24  |    0 | `LocalAllocator::tryAllocateIn` |
|  +21  |   69  |   49 | `DeferTermination<0>::DeferTermination` |
|  +20  |   20  |    0 | `FTL::JITCode::ftl()` |
| -274  |    0  |  274 | `Interpreter::executeCallImpl` (call-path swap, not a credit) |

### Ranked breakdown of the ~1974ms

#### #1 — Unpatched FTL lazy-slow-path generation thunk: **~950-1100ms (~50-55% of tax)** — gilOff-only, NEW vs §41

`operationCompileFTLLazySlowPath` uprobe count:

| config | count |
|--------|------:|
| (a) GIL-on no-sharedGCHeap | **53** |
| (b) sharedGCHeap GIL-on    | **60** |
| (c) GIL-off                | **46,631,032** |

Under sharedGCHeap, `m_allocatorForSizeStep` is never populated
(CompleteSubspace.cpp:129-140 §5.5 never-populate rule), so every FTL/DFG inline
allocation bakes/loads a null Allocator and falls to its lazy slow path. Under
GIL-on the lazy-slow-path jump is **patched** on first traversal
(FTLLazySlowPath.cpp:84-85), so the steady state is `inline-miss → jmp stub →
operation*`. Under gilOff the repatch is **skipped** (UNGIL U-T4b cross-modifying-
code safety, FTLLazySlowPath.cpp:73-87), so **every** traversal goes
`inline-miss → jmp generation-thunk → saveAllRegisters → call
operationCompileFTLLazySlowPath → DeferGCForAWhile + acquire-load m_stubCodePtr →
restoreAllRegisters → ret → tail-call stub`.

The thunk does a **full scalar register dump** (`saveAllRegisters`/
`restoreAllRegisters`, FTLThunks.cpp `genericGenerationThunkGenerator`). Perf
attributes **1660ms** of JIT-code self time to a single ~1.5KB JIT address range
(0x…4f900-0x…4ff00 in this run) — the thunk — plus 143ms in the operation body
and 20ms in `FTL::JITCode::ftl()`. The clean A/B is configs (b) vs (c):
**+1155ms ≈ 46.6M × ~25ns/traversal**, i.e. essentially the *entire* gilOff-only
tax is this one mechanism. The §41 T1/T2/T3 components are second-order next to it.

What is being lazy-slow-pathed 46.6M times: every FTL-tier allocation (JSRopeString,
JSArray, JSLexicalEnvironment, JSFunction, JSObject, JSCellButterfly via Spread)
plus FTL write-barrier slow paths. The 70.9M total cells from §41 minus C++-side
allocations (≈ the 27.8M that hit out-of-line `allocateForClient`, see #2) ≈ the
order of magnitude.

#### #2 — JIT-inline-alloc-miss → C++ 3-hop allocator path: **~550-650ms (~30%)** — sharedGCHeap-only (T1)

Out-of-line `CompleteSubspace::allocateForClient` and out-of-line
`Heap::allocationClientForCurrentThread<VM>` each fire **27,816,788** times
(identical count → the same non-inlined call sites). The remaining ~43M of 70.9M
cells go through **inlined** copies of the same chain in `JSRopeString::create` /
`JSArray::tryCreate` / `allocateCell<T>` / `JSLexicalEnvironment::create` /
`JSCellButterfly::createFromArray` / etc., which is where their +74/+80/+38/+29/+60ms
self-time deltas come from.

The 3-hop is **per cell**: `CompleteSubspace::allocate`
(CompleteSubspaceInlines.h:45-46) → `allocationClientForCurrentThread`
(Heap.h:2680: option check + `vm.gilOff()` load + TLS read + server-identity
compare) → `client.threadLocalCache().allocatorForSizeStep`
(GCThreadLocalCache.cpp:117-128: `tlcIndexBase` load + bounds check + indexed
load) → `allocator.allocate` (the §41 99.67% interval-bump). Out-of-line self
time of the three hops alone = 103+38+31 = **172ms / 27.8M = 6.2ns/cell**.

What caching `LocalAllocator*` per (thread, size-class) would skip: hops 1+2
entirely. The TLC already exposes the JIT addressing contract
(`GCThreadLocalCache::offsetOfTable()/offsetOfTableBound()`, marked PROVISIONAL,
GCThreadLocalCache.h:107-108) — JIT could load `m_table[tlcIndexBase+step]`
directly off a thread-relative pointer, matching the GIL-on path's single
`m_allocatorForSizeStep[idx]` load. ~5-6ns × 70.9M ≈ **~400ms**. The much larger
win is that a non-null JIT-baked allocator would also stop the 46.6M lazy-slow-path
traversals (#1), folding both #1 and #2.

Top-5 hot allocateCell sites by self-time delta (each is an inlined 3-hop site):
`JSArray::tryCreate` +80ms, `JSRopeString::create` +74ms, `operationMakeRope2`
+68ms, `JSCellButterfly::createFromArray` +60ms, `allocateCell<JSRopeString>`
+38ms.

#### #3 — `lockProtoFuncHold` gilOff extra work: **~145ms (7%)** — gilOff-only

`Lock.prototype.hold` fires **4,684,903** times (identical all configs;
bench-driven). Self time 187ms→332ms. +31ns/call from the `if (gilOff)` block
(LockObject.cpp:827-902, the hold-vmEntry-trampoline / fast-callee path) which
adds VMLite TLS, a JSCellLock acquire/release on the `Lock` cell, and a
`CachedCall` re-entry instead of `Interpreter::executeCallImpl` (the
−274ms `executeCallImpl` / +42ms `CachedCall::callWithArguments` swap is this
site).

#### #4 — JSCellLock acquires: **~80-120ms (5%)** — gilOff-only (T2)

`~Locker<JSCellLock>` (out-of-line copy, 0x1e9520) fires **9,267,910** times
GIL-off vs **155** GIL-on. ~9.27M ≈ 4.68M (lockProtoFuncHold's per-call cellLock,
#3) + 4.58M (= tokensProcessed; the second hot gilOff-gated cellLock site, on the
varargs/Spread → `String.fromCharCode(...codes)` path). 82ms self / 9.27M =
**8.8ns/release** (acquire CAS is inlined, so true per-acquire-release is higher,
~15-20ns). Partly double-counted in #3.

#### #5 — `String.fromCharCode(...codes)` varargs slow-path: **~115ms (6%)** — sharedGCHeap-only

`JSCellButterfly::createFromArray` fires **4,579,106** times (= tokensProcessed
exactly) under sharedGCHeap (both GIL states) vs **16,743** under (a). The FTL
inline `Spread` fast path allocates the JSCellButterfly inline; with the null
sharedGCHeap allocator it falls to `operationSpreadFastArray` →
`createFromArray`. `mayBeSegmentedButterfly()` is **false** (per-object precise,
JSObject.h:951-958; convertToSegmentedButterfly fires **0** times — see (6)), so
the copy itself takes the flat Int32 loop; the cost is the extra alloc+copy and
the `sizeFrameForVarargs`/`operationSetupVarargsFrame` C++ round-trip
(+31ms/+25ms). bench.js:289 is the sole hot `...spread`.

#### #6 — `trapsMaybeNeedHandlingForCurrentThread` per-call extra: **~47ms (2%)** — gilOff-only (T3, C++ side)

Fires **42,527,683** GIL-off vs **42,527,776** GIL-on — **identical count**. Not
a frequency tax; per-call cost rises from 0.56ns→1.65ns (+1.1ns) because the
gilOff body (VM.h:1723-1735) reads `VMLite::currentIfExists()` TLS + the lite's
trap word in addition to the VM word. 42.5M × 1.1ns ≈ 47ms.

#### #7 — `DeferTermination<0>` ctor: **~21ms (1%)** — gilOff-only (T2)

Fires **6,633,059** GIL-off vs **6,633,067** GIL-on — **identical count**.
69ms/6.63M=10.4ns vs 49ms/6.63M=7.4ns; +3ns/scope from gilOff per-lite trap-word
manipulation. **DeferGCForAWhile** has no out-of-line symbol; its dominant
constructor site is `operationCompileFTLLazySlowPath` (line 964) × 46.6M, already
inside #1.

#### Residual

`operationGetByIdGaveUp` +27ms, `PropertyTable::findConcurrently` +26ms, kernel
+110ms (page faults from extra allocation traffic), `Heap::writeBarrierSlowPath`
~flat (T5: 0.97% off vs 1.39% on — **not** a tax component at W=1).

### (5) CheckTraps clobber widening — **NOT a tax component vs the GIL-on baseline**

DFGClobberize.h:809-905: under `useJSThreads && !useThreadGIL && exitOK`,
CheckTraps writes `Watchpoint_fire | SideState | NamedProperties |
IndexedProperties | Butterfly_publicLength | Absolute | JSMapFields | …` and
defs an InvalidationPointLoc (the "partial de-jank"). Under `useJSThreads &&
useThreadGIL` (config (a) — useThreadGIL is forced ON by U0 validation when only
`useJSThreads=1` is set, OptionsList.h:705) it falls through to **`read(World);
write(Heap);`** — a **strictly wider** clobber. So gilOff CheckTraps is *better*
for LICM/CSE than the GIL-on baseline (shape facts hoist; only data heaps don't),
and the +751ms in `[JIT-code]` is **not** clobber-widening — it is the #1 thunk.
The bench.js `$imul` hoist comment applies under both modes.

JIT-side CheckTraps poll count (the inline back-edge poll) was not directly
counted (no symbol); the inline poll is a single load+branch on the lite trap word
in both modes and contributes negligibly to the diff.

### (6) Segmented at W=1 — **NO** (T4 not a factor)

`convertToSegmentedButterfly` uprobe count = **0** under (c). Every JSArray in
intcs phaseA stays flat at W=1; `mayBeSegmentedButterfly()` (per-object tagged-
word check, JSObject.h:951) returns false. The §41 T4 hypothesis ("arrays go
segmented under sharedGCHeap regardless of W") is refuted for W=1. T4 is a W>1
concern only.

### Summary table

| rank | mechanism | ms (of ~1974) | bucket | §41 tag |
|-----:|-----------|--------------:|--------|---------|
| 1 | unpatched FTL lazy-slow-path thunk × 46.6M (U-T4b repatch-skip) | ~950-1100 | gilOff | — (new) |
| 2 | per-cell 3-hop C++ alloc path × 70.9M (m_allocatorForSizeStep null) | ~550-650 | sharedGCHeap | T1 |
| 3 | `lockProtoFuncHold` gilOff fast-callee path × 4.68M | ~145 | gilOff | — |
| 4 | JSCellLock acquire/release × 9.27M | ~80-120 | gilOff | T2 |
| 5 | `String.fromCharCode(...codes)` Spread C++ round-trip × 4.58M | ~115 | sharedGCHeap | — |
| 6 | `trapsMaybeNeedHandlingForCurrentThread` +1.1ns × 42.5M | ~47 | gilOff | T3 (C++) |
| 7 | `DeferTermination<0>` +3ns × 6.63M | ~21 | gilOff | T2 |
| — | CheckTraps clobber widening | **0** (narrower than GIL-on baseline) | — | T3 (JIT) |
| — | segmented butterfly access | **0** at W=1 (convertToSegmentedButterfly never fires) | — | T4 |
| — | write-barrier slowpath | ~0 (flat) | — | T5 |

#1 and #2 share a root cause (`m_allocatorForSizeStep` stays null under
sharedGCHeap → JIT inline allocation never works); fixing the JIT allocator
baking via the PROVISIONAL `GCThreadLocalCache` table contract addresses both,
worth ~1500-1700ms of the ~1974ms tax.
