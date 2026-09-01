# FLAT-GAP-EVIDENCE — why JS-flat W=16 is 4.7×–10.8× behind Java/Go

Evidence pack for the flat-arm wall-clock gap. All runs on `9791feb9b3d9`,
Release `WebKitBuild/Release/bin/jsc`, pinned GIL-off env
(`JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1
JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1`), host quiet (1-min loadavg 0.7–2.1, 64
HW threads). Raw artifacts: `/tmp/flatgap/{perf{1,16}.data,
perf{1,16}-self.txt, eustack2.raw, gclog16.txt, osr1.txt, holdtime.js}`.

**Baseline (this session):**

| | W=1 flat | W=16 flat | Go W=1 | Go W=16 | Java W=1 | Java W=16 |
|---|---|---|---|---|---|---|
| total_ms | 5874 | 4785 | 1836 | 422 | 1974 | 976 |
| phaseA_ms | 3988 | 2222 | 1315 | 272 | 1319 | 496 |
| phaseB_ms | 603 | 289 | 408 | 50 | 507 | 264 |
| phaseC_ms | 87 | 283 | 17 | 12 | 30 | 106 |
| pc1+pc2+df+csC | 1196 | 1991 | — | — | — | — |

JS-flat / Go @ W=16 = **11.3×**. JS-flat / Java @ W=16 = **4.9×**. JS-flat
W=1 / Go W=1 = **3.2×** — i.e. ~⅔ of the W=16 gap is **single-thread
throughput**, not scaling. phaseA alone (the dominant phase) is 3.03× Go
at W=1 and **8.2× Go at W=16** (Go scales phaseA 4.83×, JS-flat 1.79×).

---

## (1) `perf record -g -F 997` — on-CPU top-20, W=16 vs W=1

W=16 (44 009 samples, ~44.1 cpu-s):

| self% | symbol | reading |
|---|---|---|
| 10.73 | `Interpreter::executeCallImpl` | C++→JS re-entry trampoline (`lock.hold` callback dispatch) |
| 5.78 | `Heap::writeBarrierSlowPath` | write barrier on every closure/env/array store |
| 5.76 | `lockProtoFuncHold` | host fn body: tryLock + getCallData + JSC::call |
| 5.53 | `operationValueSubProfiledOptimize` | **Baseline-JIT** arith slow path (DFG never stable) |
| 5.12 | `operationIteratorNextTryFast` | `for…of tf` Map iterator, **Baseline** slow path |
| 4.94 | `operationGetByValOptimize` | **Baseline** indexed-get slow path |
| 4.00 | `operationGetByIdGaveUp` | IC **gave up** → generic get (megamorphic disabled, §24 caveat) |
| 2.68 | `allocateCell<JSFunction>` | one **arrow-closure alloc per `lock.hold` call** |
| 2.57 | `operationCreateLexicalEnvironmentTDZ` | one env per closure |
| 2.49 | `operationArrayPush` | `pl.docIds.push` / `pl.tfs.push` (segmented butterfly grow) |
| 1.48 | `VM::trapsMaybeNeedHandlingForCurrentThread` | CheckTraps poll |
| 1.03 | `CompleteSubspace::allocateForClient` | shared-heap alloc slow path |
| 0.99 | `Heap::allocationClientForCurrentThread` | per-alloc TLS-ish lookup |
| 0.97 | `arithmeticBinaryOp<jsSub>` | generic sub (Baseline) |
| 0.93 | `JSCellButterfly::visitChildrenImpl` (HeapHelper) | GC mark |
| 0.89 | `iteratorOpenTryFastImpl` | `for…of` open |
| 0.89 | `allocateCell<JSArrayIterator>` | iterator alloc per `for…of` |
| 0.87 | `operationNewArrowFunction` | closure alloc |
| 0.83 | `operationWriteBarrierSlowPath` | barrier (JIT path) |
| 0.65 | `ButterflySpine::publicLength` | segmented array length read |

W=1 (6 363 samples, 6.4 cpu-s) — different shape, **more JIT, less Baseline**:

| self% | symbol |
|---|---|
| 7.99 | `[JIT]` (FTL postingsChecksumFlat hot loop) |
| 6.06 | `Interpreter::executeCallImpl` |
| 3.95 | `PropertyTable::findConcurrently` |
| 3.76 | `JSObject::getOwnNonIndexPropertySlot` |
| 3.71/3.59/3.55 | `[JIT]` (≈14% in JIT bodies) |
| 3.10 | `lockProtoFuncHold` |
| 2.89 | `operationGetByIdGaveUp` |
| 2.11 | `Structure::getConcurrently` |
| 1.88 | `JSObject::getPropertySlot<false>` |
| 1.53 | `Heap::writeBarrierSlowPath` |
| 1.37 | `operationIteratorNextTryFast` |

**Reading.** At W=16 the top-7 self symbols (40.9%) are all
**lock.hold-dispatch + Baseline-JIT operation slow paths**. The flat hot
loop is *not* running compiled code — it's bouncing between JIT stubs and
C++ operations. The W=1 profile shows the same disease (lockProtoFuncHold +
executeCallImpl + getByIdGaveUp + generic property gets ≈ 22%), but a
larger JIT-resident fraction because there are no concurrent jettisons.
Children: `executeCallImpl` is **66.0%** at W=16 — i.e. two-thirds of all
on-CPU samples have the `lock.hold` C++→JS re-entry on their stack; the
closure body is *never* inlined into the surrounding loop because a native
host function sits between them.

---

## (2) `eu-stack` off-CPU, W=16 — where do the 16 threads park?

35 process snapshots → 417 thread-stacks captured. Excluding idle helper
threads (AutomaticThread worklist, bmalloc scavenger):

| site | thread-samples | % of JS-relevant (n≈246) |
|---|---|---|
| ON-CPU (running) | 136 | 55.3% |
| `Condition.wait` (the JS barrier) | 92 | **37.4%** |
| `VMManager::notifyVMStop` (STW stop, via VMTraps) | 8 | 3.3% |
| `Lock.hold` contended park | 5 | 2.0% |
| GC collector running | 4 | 1.6% |
| allocator slow-path park | 1 | 0.4% |

**Reading.** The 16 JS threads spend ~37% of wall **parked at the
barrier** waiting for thread-0's serial sections (pc1+pc2+df+csC =
1991 ms / 4785 ms = 41.6% — matches). **JS `Lock` contention is ~2%** of
off-CPU time — K=128 shard locks vs W=16 means almost no waiting; the
adaptive-spin path absorbs the rest. STW/safepoint is ~5% combined. The
gap is *not* off-CPU; it's the on-CPU work being slow.

The 8 STW-stop stacks all enter through
`operationIteratorNextTryFast`/`operationHandleTraps`/`operationGetByIdGaveUp`
→ `VM::hasExceptionsAfterHandlingTraps` → `VMTraps::handleTraps` →
`VMManager::notifyVMStop` — i.e. the trap poll embedded in the **Baseline
operation slow paths** is the safepoint reach point.

---

## (3) `JSC_logGC=1` at W=16 flat

10 EdenCollection + 8 FullCollection = **18 cycles**. Sum of `p=` pauses =
**164.0 ms**, max 24.2 ms, mean 9.1 ms. Against total_ms 4539 = **3.6% of
wall**. Perturbation: total under logGC = 4539 ms vs 4677–4785 ms
unperturbed (i.e. *faster* under logGC — within run-to-run noise; logGC=1
does not flip the §34 fast-mode here because the flat arm doesn't allocate
the 51 Eden / 35 Full storm the spec arm does).

The "Requesting GC" lines all read `~33 MB allocated this cycle` against a
`33554432` budget; 18 cycles × 33 MB ≈ **600 MB total bytes allocated** —
overwhelmingly transient (closures, lexical envs, Map iterators,
iterator-result arrays, posting-slice copies). **GC pause is not the
ceiling** for the flat arm.

---

## (4) `Lock.hold` calls + per-call cost

`JSC_logJSLockContention=1` (existing instrumentation,
`LockObject.cpp:77-160`):

| | W=1 flat | W=16 flat |
|---|---|---|
| total `hold` acquires | **4 682 780** | 4 682 870 |
| spin-loop iterations | 0 | 1 645 027 |
| ParkingLot parks | 0 | 115 553 (2.5% of acquires) |
| hottest lock | — | lock#90, 987 parks |

Per-call cost micro (`/tmp/flatgap/holdtime.js`, W=1 GIL-off, 2 M iters
each, FTL-warmed):

| pattern | ns/call |
|---|---|
| `lock.hold(() => { x++ })` | **117.0** |
| `callFn(() => { x++ })` (pure-JS, FTL-inlinable) | 12.6 |
| `lock.hold(ingestBody)` (Map.get + 2× push) | 192.4 |
| bare `ingestBody` | 136.6 |

**Reading.** Uncontended `lock.hold` overhead ≈ **104 ns/call** above the
work — composed of (a) host-function call (no DFG inlining of native), (b)
fresh `JSFunction` + `JSLexicalEnvironment` alloc for the arrow closure,
(c) `getCallData` + `JSC::call` → `executeCallImpl` → `vmEntryToJavaScript`
re-entry, (d) `tryLock`. 4.68 M calls × 104 ns = **487 ms of pure
hold-dispatch overhead at W=1** (≈ 8% of W=1 wall). At W=16 the per-call
cost is *higher* (perf attributes 16.5% self ≈ 7.3 cpu-s = 1 559 ns/call to
`lockProtoFuncHold + executeCallImpl` self alone — cache contention on the
shared CodeBlock / VM state in `executeCallImpl`).

Code site: `Source/JavaScriptCore/runtime/LockObject.cpp:800`
(`lockProtoFuncHold`) → `JSC::call` at the epilogue. The closure body
*itself* tier-ups to FTL (the anonymous `#Cue8Xu`/`#Bw8AIg`/`#CTlXvn`
codeblocks all install FTL — see (5)), but the surrounding hot loop cannot
inline through the native trampoline, and every call allocates a fresh
closure (`operationNewArrowFunction` + `operationCreateLexicalEnvironmentTDZ`
= 6.2% self at W=16).

---

## (5) `JSC_verboseOSR=1` — do the flat hot loops reach FTL?

**No.** One W=1 rep (`/tmp/flatgap/osr1.txt`, 15 516 lines):

| function | highest tier installed | reopt count | note |
|---|---|---|---|
| `ingestDocFlat#Dar0Bb` | **DFG only** (8 distinct DFG installs) | 8 jettisons | never FTL; `optimizationDelayCounter` climbs 0→8 (backoff to 655 941) |
| `phaseAFlat#Aii9rM` | DFG | 0 | requests FTL once, never installs |
| `queryPointFlat#DGEVYE` | DFG | 1 | requests FTL, never installs |
| `phaseBFlat#A9sgFT` | DFG | — | 4 OSR-entries to DFG, never FTL |
| `postingsChecksumFlat#D3vRtn` | **FTL** | — | (serial section — does reach FTL) |
| `mix32#CDZV6E` | FTL | 1 | jettisoned once (`HadFTLReplacement`), 201 BadType exits at bc#4 |
| hold-closure `#Cue8Xu` (ingest body) | FTL | 0 | stable |
| hold-closure `#Bw8AIg` (queryPoint body) | FTL | — | exits bc#49 BadType |
| hold-closure `#CTlXvn` (copyPostings body) | FTL | — | exits bc#49 BadType |

`JSC_printEachOSRExit=1` exit-reason histogram (full W=1 run):

```
26 234  exit #39 (bc#368, BadType)   ← ingestDocFlat for-of destructuring
   201  exit #38 (bc#368, BadType)
   201  exit #3  (bc#4,  BadType)    ← mix32
   201  exit #133(bc#851, Overflow)  ← queryScoredFlat
   130  exit #92 (bc#851, Overflow)
   101  exit #25 (bc#293, Overflow)
   101  exit #13 (bc#138, Overflow)
   101  exit #123(bc#716, Overflow)
   100  exit #8  (bc#24,  InadequateCoverage) ← docSeed32 inlined
    …
```

**Reading.** `ingestDocFlat` OSR-exits **26 234 times** at `bc#368
BadType` — the `for (const [t, count] of tf)` Map-iterator destructuring
(bench.js:812). The DFG speculates on the iterator-result array's
IndexingType and misses (segmented/contiguous mismatch under
`useSharedGCHeap`, or the `[t,count]` entry array structure differing from
profile). Each DFG install survives ~100 calls then jettisons; after 8
rounds the exponential backoff pins it in **Baseline for the rest of
phaseA** — which is exactly the `operationValueSubProfiledOptimize` /
`operationIteratorNextTryFast` / `operationGetByValOptimize` /
`operationGetByIdGaveUp` storm in (1). This is **the** dominant
single-thread throughput sink; phaseA is 68% of W=1 wall.

---

## (6) Int32Array allocation under sharedGCHeap, W=16 flat

The flat arm's Int32Array surface is **static**: per-worker scratch
allocated once in `phaseAFlat` (`makeFlatScratch`, bench.js:777) + one
`Int32Array(V)` in `buildDfSnapFlat`. No grow path (the linked-list-in-
Int32Arrays design was withdrawn — bench.js:753-772). Count:

| | per W=16 run |
|---|---|
| `new Int32Array(N_BASE)` | 16 × 3 = 48 (28 000 elem each = 5.38 MB) |
| `new Float64Array(N_BASE)` | 16 × 2 = 32 (28 000 elem each = 7.17 MB) |
| `new Int32Array(V)` | 1 (65 536 elem = 0.26 MB) |
| **total typed-array allocations** | **81** |
| **total typed-array bytes** | **≈ 12.8 MB** |

vs ~600 MB total allocated (logGC). **Typed-array allocation is <2.2% of
bytes and a non-factor.** The allocation pressure is closures + lexical
environments + `JSArrayIterator` + iterator-result 2-tuples + `new Map()`
per doc + `slice()` copies in `copyPostingsFlat`.

---

## Synthesis (the 1-paragraph)

The flat arm is **not running in FTL**. `ingestDocFlat` (68% of W=1 wall)
OSR-exits 26 k times at `bc#368 BadType` on the Map-iterator `[t,count]`
destructure, jettisons its DFG codeblock 8×, and spends the bulk of phaseA
in **Baseline JIT** — perf at W=16 shows 26% self in Baseline `operation*`
slow paths (`ValueSub`, `IteratorNextTryFast`, `GetByValOptimize`,
`GetByIdGaveUp`, `ArrayPush`). On top of that, every one of the **4.68 M
`lock.hold` calls** allocates a fresh arrow `JSFunction` +
`JSLexicalEnvironment` and re-enters JS via the C++ `executeCallImpl`
trampoline (66% children, 11% self at W=16; ≈104 ns/call uncontended,
≈1.5 µs/call observed at W=16) — the closure body cannot inline into the
hot loop because a native host function sits between them. Off-CPU is
**not** the gap: JS-Lock contention is ~2%, STW GC pause is 164 ms (3.6%),
allocator parks <1%; 37% of JS-thread samples are parked at the barrier
waiting for the 1.99 s of thread-0 serial work — but that serial work is
itself the same Baseline-tier disease (pc1/pc2 do reach FTL, so the serial
1.99 s is mostly genuine 4.6 M-posting walk cost). Int32Array allocation is
81 objects / 12.8 MB total — negligible. The two named engine targets the
finders should attack: **(A)** the bc#368 BadType Map-iterator-destructure
speculation miss under `useSharedGCHeap` that pins `ingestDocFlat` in
Baseline (worth ≈ the entire 3× W=1 gap to Go), and **(B)** the
`Lock.prototype.hold` closure-dispatch path — either a DFG-intrinsic /
`lock`/`unlock` pair that lets the body inline, or sinking the arrow
allocation (worth ≈ 0.5 s W=1, more at W=16 from `executeCallImpl` cache
contention).

---

## Round 2

Evidence pack on the **post-round-1 binary** (`56b8f886e000`, Release jsc
sha256 `2edfe0eaa3ff…`), pinned GIL-off env, 64 HW threads, loadavg 1.5–8.6
(the default/intcs re-baseline reps below briefly pushed it to ~8; the
perf/eu-stack/annotate captures were taken at loadavg ≤2). Raw artifacts:
`/tmp/flatgap2/{perf{16,32,32slow,pc1}.data, perf{16,32}-{self,children}.txt,
eustack32-slow.raw, lockcont-w{16,32}.txt, osr16.txt, w32-reps.txt,
rebaseline.jsonl, flat-reps.jsonl}`.

### (1) `perf record -g -F 997` — on-CPU top-20, W=16 vs W=32

W=16 (15 078 samples, ≈15.1 cpu-s; total_ms 1514 under perf):

| self% | symbol | reading |
|---|---|---|
| **13.00** | `operationCompileFTLLazySlowPath` | NEW #1 — gilOff U-T4b leaves the patchable jump unpatched, so **every** FTL slow-path traversal lands here forever |
| **11.91** | `lockProtoFuncHold` | round-1's 5.76 % grew **relatively** (loop is 3× faster); absolute samples 2535→1796 |
| 4.09 | `ButterflySpine::publicLength` | segmented-array length read (`pl.docIds.push` path) |
| 3.89 | `operationGetByIdGaveUp` | IC-gave-up generic get (still no megamorphic) |
| 3.00 | `JSCellButterfly::visitChildrenImpl` (HeapHelper) | GC mark |
| 2.70 | `ButterflySpine::bumpPublicLengthToAtLeast` | segmented push grow |
| 1.57 | `JSArray::pushInline` | |
| 1.26 | `Int32Adaptor::setFromTypedArray` | `flattenFlatShards` / `appendI32` |
| 1.15 | `operationCompareStrictEq` | |
| 0.69 | `Heap::addToRememberedSet` | write barrier |
| 0.55 | `operationArrayPush` | |
| 0.41 | `LockAlgorithm::lockSlow` | ParkingLot slow path |
| 0.41 | `JSOrderedHashTable<Map>::addImpl` | per-doc `tf.set` |

`executeCallImpl` children: **25.1 %** (round-1: 66.0 %) — the
hold-vmEntry-trampoline fix landed; `lockProtoFuncHold` now tail-calls
`vmEntryToJavaScriptWith0Arguments` directly (visible in the disasm at
`+0x50b`). The Baseline `operation*` storm from round-1
(`ValueSub`/`IteratorNextTryFast`/`GetByValOptimize`) is **gone** — the
forof-tdz-osr-loop fix landed and `ingestDocFlat` is FTL-resident.

W=32 (33 509 samples, ≈33.6 cpu-s; total_ms 1870 under perf, **fast-mode**
rep — see §R2 below):

| self% | symbol | vs W=16 |
|---|---|---|
| 13.05 | `lockProtoFuncHold` | flat |
| 12.01 | `operationCompileFTLLazySlowPath` | flat |
| 3.35 | `operationGetByIdGaveUp` | flat |
| 3.04 | `ButterflySpine::publicLength` | flat |
| **1.37** | `LockAlgorithm::lockSlow` | **3.3×** W=16 |
| 0.80 | `[k] futex_wake` | new |
| 0.73 | `[k] native_queued_spin_lock_slowpath` | new |
| 0.64 | `ParkingLot::parkConditionallyImpl` | new |

Children: `do_futex` 8.18 % (W=16: 3.79 %), `__sched_yield` 2.70 % — the
W=32 surcharge is futex traffic + kernel spin, not new on-CPU work.

#### `perf annotate` — what those top-2 symbols actually burn

`operationCompileFTLLazySlowPath` (13.00 % self), instruction-level:

| % of fn | insn | what it is |
|---|---|---|
| **59.10** | `lock incl 0x8(%r15)` | **`RefPtr<JITCode>` copy ctor** — `codeBlock->jitCode()` returns by value (`CodeBlock.h:369`) |
| 17.12 | `mov (%r15),%rax` | (skid: vtable load for `->ftl()`) |
| 12.81 | `je …` after `lock decl` | (skid: `RefPtr` dtor branch) |
| 3.52 | `lock decl 0x8(%r15)` | `RefPtr<JITCode>` dtor |

i.e. **≈76 % of the #1 symbol = 9.9 % of total W=16 CPU** is the
`JITCode::m_refCount` atomic inc/dec pair — 16 threads bouncing **one cache
line** (the shared `ingestHold` FTL `JITCode` object). The "double-checked
publication" steady state at `FTLOperations.cpp:978` is one acquire-load —
but it reaches that load via `jitCode()->ftl()->lazySlowPaths[index]`, and
`CodeBlock::jitCode()` is `RefPtr<JITCode> jitCode() { return m_jitCode.get(); }`.

`lockProtoFuncHold` (11.91 % self), instruction-level:

| % of fn | insn | what it is |
|---|---|---|
| **21.83** | `lock incl 0x8(%rax)` | **same `RefPtr<JITCode>` ref** — `codeBlock->jitCode()->addressForCall()` (`LockObject.cpp:1068`) |
| 18.26 | `jne …` after `lock cmpxchg %cl,0x4(%rdi)` | NativeLockState release |
| 15.33 | `cmp %rax,%r14` | adaptive-spin compare |
| 7.97 / 5.49 / 2.18 | post-`lock incl` skid / `decl` skid / `lock decl` | `RefPtr<JITCode>` dtor |

i.e. **≈37 % of the #2 symbol = 4.5 % of total W=16 CPU** is the **same**
`JITCode::m_refCount` cache line. **Combined ≈14.4 % of W=16 on-CPU time is
`RefPtr<JITCode>` atomic refcount bounce** introduced by the round-1
`hold-vmEntry-trampoline` fast path and the U-T4b lazy-slow-path steady
state. (Neither caller needs ownership: a raw `m_jitCode.get()` read
suffices — the CodeBlock is live for the duration of both calls.)

### (2) `eu-stack` off-CPU, W=32 — where do 32 threads park?

100 process snapshots during a **slow-mode** W=32 rep (phaseA = 1601 ms),
215 thread-stacks captured, 167 JS-relevant after stripping
helpers/scavenger:

| site | thread-samples | % of JS-relevant |
|---|---|---|
| **JS-Lock park** (shard, inside `lockProtoFuncHold`) | 70 | **41.9 %** |
| ON-CPU | 49 | 29.3 % |
| **`operationCompileOSRExit`** at frame#1, frame#0 = `__sched_yield` | 37 | **22.2 %** |
| `CodeBlock::updateAllPredictions` / `VariableEventStream::reconstruct` | 8 | 4.8 % |
| `LocalAllocator::allocateSlowCase` → `enterStripeSlow` park | 3 | 1.8 % |
| `Condition.wait` (barrier) | 0 | 0 % (sampling window ⊂ phaseA) |

Every `operationCompileOSRExit` stack is identical: `__sched_yield` ←
`operationCompileOSRExit` ← `[JIT 0x…0a0188]` ← `llint_call_javascript` ←
`lockProtoFuncHold` — i.e. the `ingestHold` closure body OSR-exits **inside
the held shard lock**, lands in `DFGOSRExit.cpp:215`'s gilOff acquisition
loop on the **process-global `static Lock dfgOSRExitGenerationLock`**, and
spin-yields. 31 threads spin-yield while 1 compiles; the shard-lock holder
is among them, so the other 4.2 M / K ≈ 33 k acquires that target that
shard pile up behind a critical section that just grew from ~100 ns to
~tens of µs.

### (R2) — W=32 phaseA bimodality is **not** shard-lock load (refutes §35-R2's K=128 hypothesis)

8-rep W=32 sweep (no perf): phaseA ∈ {694, 696, 697, 726, 730, **1510, 1588,
1597**} — clean two-mode split, 3/8 slow. Under `perf record` (8 reps):
phaseA ∈ {810…906}, **0/8 slow** — perf's overhead perturbs the
early-phaseA tier-up timing race away from the slow-mode attractor. The
slow-mode discriminant is the OSR-exit storm above, not raw shard-lock
arithmetic: `JSC_logJSLockContention=1` over 6 W=32 reps gives

| mode | phaseA ms | total parks | park/acq | spinIters |
|---|---|---|---|---|
| fast (4 reps) | 668–712 | 286 571–326 387 | 6.1–7.0 % | 3.86 M–4.23 M |
| **slow** | **1570** | **704 098** | **15.0 %** | **6.77 M** |
| fast | 1099 | 142 412 | 3.0 % | 2.14 M |

Slow-mode parks are **2.2×** fast-mode at the same K and same W — the extra
parking is the *consequence* of long critical sections (OSR-exit-under-lock),
not the cause. **`dfgOSRExitGenerationLock` is the W=32 phaseA-bimodal
serialization point**: under gilOff U-T4b every DFG exit traversal acquires
it (the existing-ramp early-return is *after* the `tryLock` spin-yield
loop, `DFGOSRExit.cpp:219-231`), so once a bad speculation in the shared
DFG `ingestHold` codeblock starts firing, 32 threads serialize on a single
`WTF::Lock` via `Thread::yield()`.

A W=16 slow-mode tail also exists (1/3 reps in this round: phaseA
**3629 ms**, pc1 1256 ms, total 6146 ms — outside §35's 12-rep 1282–1628
band) with the same signature.

### (3) pc1 (`postingsChecksumFlat` first call) — why 350 ms for 4.2 M flat-int reads

pc1/pc2 across this round's reps (ms):

| | W=1 | W=16 | W=32 |
|---|---|---|---|
| pc1 | 159 / 328 / 332 | 342–376 (1256 in slow-tail) | 359–411 (530–574 in ~2/10) |
| pc2 | 82 / 173 / 208 | 80–109 | 80–122 (179–224 in ~2/10) |

pc2 is the same function on +3 % data after FTL is warm: **80 ms = 19
ns/posting** is the steady-state inner-loop cost. pc1 − pc2 ≈ **270 ms is
tier-up**, not foreign-Map iteration. `JSC_verboseOSR=1` at W=16
(`/tmp/flatgap2/osr16.txt`): `postingsChecksumFlat#Ckh14h` installs DFG **3
times** (Overflow exits at bc#379 then bc#418 — `(sum + mix32(item)) >>> 0`
int32-overflow speculation, twice), with a 4-iteration "OSR-in succeeded,
OSR failed" loop at bc#280 between DFG#1 and DFG#2, then 1 FTL install,
**0 jettisons**. The W-dependence (W=1 best 159 ms vs W=16 360 ms) is
JIT-worklist pressure: pc1 starts immediately after the phaseA barrier
while the concurrent worklist is still draining 16 workers' phaseA
compilations, so the three serial DFG/FTL compiles for `postingsChecksumFlat`
queue. (pc1 is also bimodal at W=1 — 159 vs 330 — same Overflow-reopt
roulette.) `perf -D 700` main-thread slice (`/tmp/flatgap2/perfpc1.data`)
shows pc1's hot inner loop is JIT-resident (~3.4 % of samples in
`[JIT]`); the named main-thread C++ symbols (`setFromTypedArray`,
`tryGetIndexQuicklyConcurrent`, `segmentedPublicLength`,
`operationNewInt32ArrayWithOneArgument`) are all `flattenFlatShards` /
`appendI32`, not pc1. **§35-R3's "foreign-allocated Map iterator" reading
is not supported by this round's evidence**: post-flatten the inner loop is
typed-monomorphic and FTL-stable; the 270 ms is the Overflow-driven 3× DFG
reopt before FTL.

### (3b) — pc1 direct split-timer: 100 % flatten1, 0 % checksum (corrects §(3) above)

`postingsChecksum1_ms` at `56b8f886e000` wraps **both** `flattenFlatShards()`
and `postingsChecksumFlat()` (bench.js:1287-1296 at that rev). Instrument-
and-revert split-timer (loadavg 3.4, 2-rep mean, pinned gilOff env):

| section | W=1 | W≥2 (16) | Δ W-surcharge |
|---|---|---|---|
| `FLATTEN1` (flatten only) | 79 ms | **205 ms** | **+126 ms** |
| checksum-only (`postingsChecksumFlat`) | 75 ms | **63 ms** | **−12 ms** |

**100 % of pc1's W-surcharge is flatten1; 0 % is the 4.2 M-read checksum
loop.** The checksum-only path is W-*independent* — actually 12 ms *faster*
at W≥2. §(3)'s "270 ms = 3× Overflow reopt + JIT-worklist queueing" reading
is **refuted**: both mechanisms live entirely inside `postingsChecksumFlat`
tier-up and would surface in the checksum-only column, which is flat. The
3× DFG-install / Overflow-exit observation in §(3) is real but costs ≤12 ms
total (75 − 63), not 270 ms; §(3)'s own `perf -D 700` slice already named
the right culprit (every main-thread C++ symbol it lists is
`flattenFlatShards`). **Redirects all pc1 finder budget to the flatten1
path** and away from `pc1-uint32-add-overflow-reopt` / `pc1-ftl-plan-queued`
/ `pc1-dfg-osrentry-stale` (combined ceiling ≤12 ms). Now landed permanently
as `serial-461-decompose` (bench.js `flatten1_ms` / `cs1_ms` / `flatten2_ms`
/ `cs2_ms` output keys; 3-rep median W=16 confirms 206 / 63 / 67 / 16) plus
`flatten1-parallelize-shards-out-of-pc1-timer`, so `postingsChecksum1_ms`
henceforth reports checksum-only.

### (4) Lock contention counter, W=32 vs W=16

`JSC_logJSLockContention=1`, single fast-mode rep each (total acquires
4 682 870 / 4 682 966 — invariant):

| W | spinIters | parks | park/acq | hottest lock |
|---|---|---|---|---|
| 16 | 2 663 604 | 176 012 | **3.76 %** | #90, 1 526 parks |
| 32 | 4 523 313 | 354 077 | **7.56 %** | #90, 2 880 parks |
| 32 slow-mode | 6 766 674 | 704 098 | **15.03 %** | #102, 5 658 parks |

Round-1 W=16 was 1.65 M spin / 115 k parks (2.5 %); the round-1 fixes
**increased** W=16 contention (faster critical-section bodies → more
acquires/ms → higher collision rate at K=128). W=32 fast-mode park rate is
2.01× W=16 — linear in W as expected for K=128 random sharding. Neither
explains the slow-mode +850 ms; only the
OSR-exit-under-lock-serializing-on-a-global-Lock chain does.

### (5) Re-baseline default + intcs arms, this binary (3-rep medians)

| arm | W=1 | W=16 | W=32 | reference tuple |
|---|---|---|---|---|
| default | 19 872 | 13 183 | 14 142 | `b3e65a68…\|4158957\|39c33392…\|c4bdd580…\|af028188…` ✓ unchanged |
| intcs | 16 057 | 8 326 | 8 792 | `8021f000\|…\|1fc7d941\|…` ✓ unchanged |
| flat | 4 405¹ | 1 394 | 1 660 | `686d6890\|4154468\|0fbbd673\|3af6b072\|e1d22021` ✓ unchanged |

¹ W=1 flat reps {4189, 4405, 4451} taken at loadavg ≈8.6 (immediately
after the default/intcs sweep); §35's 3389 was at loadavg ≤1.4 — treat as
host-noise inflated. Default arm vs §35: W=1 19 872 ≈ 19 003 (+5 %), W=16
13 183 ≈ 13 013 (+1 %) — round-1 engine deltas are flat-arm-only as
intended. **intcs pc1 at W=16 = 533 ms vs W=1 = 106 ms** — the intcs arm
walks the **un-flattened** segmented `[]` posting lists, so its 5× pc1
W-surcharge *is* the segmented-butterfly read cost the flat arm's
`flattenFlatShards()` already removes.

### Synthesis (round-2, 1-paragraph)

The two round-1 engine fixes landed and the Baseline-tier disease is gone
(`executeCallImpl` 66 %→25 % children, `operation{ValueSub,IteratorNext,
GetByVal}` no longer in top-20). The new W=16 on-CPU profile is dominated
by **one micro-architectural bug**: `CodeBlock::jitCode()` returns
`RefPtr<JITCode>` by value, and the gilOff steady-state paths of both
`operationCompileFTLLazySlowPath` (`FTLOperations.cpp:974`, hit on **every**
FTL slow-path traversal because U-T4b suppresses the repatch) and the
round-1 `lockProtoFuncHold` fast path (`LockObject.cpp:1068`, hit 4.68 M
times) call it — `perf annotate` attributes **≈14.4 % of W=16 CPU** to the
`lock incl/decl 0x8(jitCode)` pair, i.e. 16 threads bouncing the shared
`ingestHold` `JITCode::m_refCount` cache line. The W=32 phaseA bimodality
(**3/8 slow**, +850 ms) is **not** K=128 shard-lock arithmetic: eu-stack on
a slow-mode rep shows **22 % of JS threads spin-yielding inside
`operationCompileOSRExit`** on the process-global `dfgOSRExitGenerationLock`
(`DFGOSRExit.cpp:209-225`) — every gilOff DFG exit traversal acquires it
*before* the existing-ramp short-circuit, the exit fires inside the held
shard lock, and the 2.2× park inflation (704 k vs 314 k) is the downstream
effect. pc1's ~350 ms is **not** foreign-Map iteration **and not**
checksum-path tier-up: the direct split-timer (§(3b)) shows checksum-only
is 63 ms at W≥2 (≤ its 75 ms at W=1) — **the entire +126 ms W-surcharge is
`flattenFlatShards()`**; the `Overflow`-reopt + worklist-queueing reading
of §(3) is refuted (combined ceiling ≤12 ms). Round-2 named engine targets: **(A)** raw-pointer `jitCode()` reads at the
two gilOff hot-path call sites (zero-risk, ≈14 % CPU); **(B)** lock-free
existing-ramp check in `operationCompileOSRExit` *before* the
generation-lock spin (mirrors the FTL lazy-slow-path DCLP), which collapses
the W=32 slow mode and the rare W=16 6 s tail; **(C)** the
`FTLLazySlowPath` U-T4b steady state still pays a C-call + DeferGC + vtable
dispatch per traversal even after (A) — a generation-thunk that reads the
release-published `m_stubCodePtr` directly and tail-jumps without entering
C would remove the remaining ~3 % self.

### Round-2 landed deltas (post-evidence)

Engine: (A) `jitcode-refptr-bounce-14pct` — `CodeBlock::jitCodeRawPtr()`
raw accessor + use at `FTLOperations.cpp:975`, `LockObject.cpp:1075`, and
the four `dfg()` derefs in `operationCompileOSRExit`;
(B) `dfg-osrexit-genlock-dclp-precheck` — lock-free `m_exits[i].m_codePtr`
read against a function-static cached thunk codePtr BEFORE
`variableEventStream.reconstruct` and the tryLock spin, paired
`storeStoreFence` before `setExitCode`; the §35-R1 sibling
`tryInstallArrayBufferSpeciesWatchpoint` STW wrapper; the SPEC-congc §7.1a
gilOff single-handoff (one Concurrent window per cycle, `t_sharedGCConcurrentHandoffsThisCycle`
cap, `sharedGCWindowedConductActive()`); and
`flatten1-segmented-int32shape-segwalk-copy` (`forEachSegmentedIndexedContiguousRun`
+ the segmented Int32/DoubleShape arm and the `tryGetIndexQuickly`
JSArray-generic fallback in `JSGenericTypedArrayView::setFromArrayLike`).

Bench: `flatten1-parallelize-shards-out-of-pc1-timer` (shard-striped,
SHARED segmented sources — see below) + `serial-461-decompose` output
keys. The earlier `jsarray-push-foreign-thread-segments-despite-jsl`
per-(worker×shard) owner-local chunk redesign + 3-pass merge was
**measured and REJECTED**: at W=16 it inflated `flatten1_ms` to ~384 ms
wall (vs §35's 206 ms serial) and total to 1585 ms — the 3-pass merge
iterates W foreign-thread chunk-Maps per shard (W× more `Map`
objects/entries, W× more small `TypedArray.set` calls) and never reaches
FTL on any worker. The retained design keeps phaseA writing to the SHARED
`flatShards[s]` under the shard lock (unchanged from §35), then runs the
**§35 flatten body unchanged** as a W-parallel shard-stripe between two
barriers OUTSIDE the pc1 timer. The engine-side segwalk-copy makes the
`new Int32Array(segmentedJSArray)` copy a fragment-run walk on every
worker, so the per-shard cost is flat-W and the section parallelizes
cleanly: W=16 `flatten1_ms` 206 ms serial → **57 ms** wall (3.6× on 16
workers; bounded by the Int32Array species-watchpoint STW + per-worker
FTL warmup of `flattenFlatShards`, not by data movement).

(C) is **deferred**: with (A) landed `operationCompileFTLLazySlowPath`'s
remaining self is the DeferGC + vtable + acquire-load triple (≈3 %
W=16), and the post-(A)+(B) W=16 wall of 1032 ms already crosses the
round-2 bar; the JIT-side fix (osrExitGenerationThunk reading the
published slot directly and tail-jumping) is a thunk codegen change with
no perf-evidence-backed need this round.

## Round 3

Tree `cbb7d616a1f6`, Release jsc sha256 `d953dcbf5f0b88c1…`. Pinned
GIL-off env, 64 HW threads. Method: read-only on `Source/` — uprobes via
`perf probe` at `operationCompileFTLLazySlowPath` /
`JSThreadsSafepoint::stopTheWorldAndRun` /
`JSGlobalObject::tryInstall{TypedArray,ArrayBuffer}SpeciesWatchpoint`
(symbols at `0x7fa360` / `0x1de0d0` / `0xcde510` / `0xcde2c0`); per-batch /
per-worker timing via instrumented bench copies under `/tmp/r3/`.
`git diff --stat Source/` clean at exit.

### (1) W=1 flat decomposition — JIT-warmup hypothesis REFUTED

Per-1000-doc phaseA batch wall (`/tmp/r3/bench-w1decomp.js`): 28 batches
99–130 ms, **batch 0 = 99.4 ms** ≈ steady-state. 100-doc resolution on
docs 0–1999: batch 0 = 12.3 ms, batch 7 = 7.9 ms, batches 10–19 ≈ 8.9–
14.4 ms — warmup contributes **≤5 ms of phaseA's ~2800 ms**.

`JSC_verboseOSR=1` W=1 (`/tmp/r3/w1-osr.log`, 6354 lines): 39 LLInt /
42 Baseline / 39 DFG / 17 FTL installs, 28 OSR-exit ramps generated,
**1 jettison total** (queryScoredFlat). `ingestDocFlat` tier history
LLInt→Baseline→DFG→FTL once each — clean. No recompile loops.

`perf record -F 997` W=1 (`/tmp/r3/w1.perf`, 5K samples): **57.4 %
children in `lockProtoFuncHold`** (6.29 % self in the C++ trampoline,
remainder is FTL `ingestHold` body). Top non-JIT self: 6.29 %
`lockProtoFuncHold`, 3.75 % `Int32Adaptor::setFromTypedArray`, ~2.1 %
`JSOrderedHashTable::{addImpl,copyImpl}`, 0.93 % `DeferTermination`
ctor, 0.87 % `ensureLengthSlowConcurrent`, 0.41 %
`operationGetByIdGaveUp`, 0.40 % `operationCompileFTLLazySlowPath`.
4.7 % kernel page-fault.

**Reading**: the W=1→Java gap (~1250 ms+) is **steady-state per-doc
cost**, not tier-up: 28 000 docs × ~50 `lock.hold` calls each at ~35 ns
trampoline + the FTL `ingestHold` body (Map.get/set on per-shard Map +
two `[].push` onto a foreign-thread-contended segmented butterfly).
Running twice in-process is unnecessary — the per-batch curve is flat
from doc ~800 onward.

**W=1 wall on this binary** at loadavg 0.75–2.0: {3825, 3834, 3881,
3945, 3958, 3979} med **~3920 ms** vs §36's 3223 (+21 %). Same commit,
different binary sha (`d953dcbf` vs §36's `e0fb8fe1`); host-variance
and/or rebuild delta. Recorded as an anomaly for §36 cross-check.

### (2) W=16 flatten1 species-STW hypothesis REFUTED; pre-warm: −25 % only

`perf record -c 1 -e probe_jsc:{stw,stw_ret,taspecies,abspecies}` W=16
(`/tmp/r3/w16-stw.perf`, 443 STW windows captured): **`tryInstall
TypedArraySpeciesWatchpoint` fires at +736 ms wall** (2 racing JS
Threads) — that is **inside phaseB** (first `Int32Array.prototype.slice`
in `copyPostingsFlat`/`queryPointFlat`), **NOT in flatten1**. `new
Int32Array(jsArray)` does not consult the species watchpoint (only the
iterator-protocol fast-path), so flatten1 cannot trigger it.
`tryInstallArrayBufferSpeciesWatchpoint`: **0 calls**. STW windows in
the flatten1 bucket (≈+607–653 ms): **5**, max single-window 1.63 ms,
p50 0.11 ms, p90 0.39 ms — negligible vs flatten1's ~53 ms.

Pre-warm (`/tmp/r3/bench-prewarm.js`, 50 thread-0 calls of
`flattenFlatShards` on a dummy shard before the start barrier):
flatten1 {40.3, 35.2, 40.9} vs no-prewarm {55.6, 53.2, 49.6}, both
3-rep, checksums unchanged — **−13 ms / −25 %, does NOT eliminate**.

Per-worker flatten timing (`/tmp/r3/bench-flattime.js`, 3 reps): all 16
workers **uniform 47–54 ms** (min 46.9, max 54.5). No straggler; the
53 ms is the per-worker rate, not a barrier tail.

flatten1 wall vs W (per-worker mean):

| W | flatten1 ms | shards/worker | ms/shard |
|---|---|---|---|
| 1 | 91 | 128 | 0.71 |
| 2 | 69 | 64 | 1.08 |
| 4 | 41–54 | 32 | 1.28–1.69 |
| 8 | 25–26 | 16 | 1.59 |
| **16** | **47–54** | 8 | **6.3** |
| 32 | 99–106 | 4 | **25** |

Clean speedup through W=8 (3.5×), then **doubles at every step
W=8→16→32**. The 57 ms W=16 floor is a **W>8 contention knee** (per-
shard cost 4× from W=8→16, 4× again 16→32) on a global resource hit by
`new Int32Array(segmentedJSArray)` × ~65 k calls — NOT species STW, NOT
per-worker FTL warmup. **§36-R1 both components REFUTED.** Candidate
mechanism for next round: typed-array buffer allocation
(`ArrayBufferContents` / Gigacage) or `forEachSegmentedIndexedContiguousRun`
acquire-loads on foreign-thread spines; needs a flatten1-windowed perf
slice to attribute.

### (3) `operationCompileFTLLazySlowPath` — §36-R2 "≈3 %" REFUTED

`perf stat -e probe_jsc:lazy`: **5 284 162 calls/run at W=16**,
5 469 514 at W=1 (workload-fixed, ~1.2 calls per `lock.hold`). Per-
process `perf record -F 1997` W=16 (`/tmp/r3/w16c.perf`, 29K samples,
35.7 G cycles): self **0.29 % JS Thread + 0.02 % main = 0.31 %** (NOT
~3 %); `FTL::JITCode::ftl()` vtable 0.02 %. ≈111 M cycles / 5.28 M
calls ≈ **21 cycles/call** (≈6.6 ns @ 3.17 GHz) — the existing C++ DCLP
fast path (`stubCodePtrConcurrently()` acquire-load + cmp + ret,
`FTLOperations.cpp:988`) is already near-free post round-2.

Thunk-side DCLP (§36-R2's deferred (C)): the `lazySlowPathGeneration
ThunkGenerator` (`FTLThunks.cpp:160`) would need to load
`callFrame→codeBlock→jitCodeRawPtr→ftl()→lazySlowPaths[index]→
m_stubCodePtr` (`FTLLazySlowPath.h:105`, `std::atomic<void*>`) and
tail-jump on non-null — 4 dependent loads + cmp + jmp in the thunk.
**FEASIBLE** but ceiling is 0.31 % CPU ≈ **≤2 ms W=16 wall**. Recommend
**DO NOT IMPLEMENT** — round-2's estimate was on the §35 binary; on
`cbb7d616` it is 10× smaller than projected.

### (4) W=32 vs W=16 shard-lock; K=256 does NOT help

`JSC_logJSLockContention=1`, single rep each (acquires invariant
4 682 886 / 4 682 998):

| W | spinIters | parks | park/acq | hottest |
|---|---|---|---|---|
| 16 | 2 824 293 | **175 170** | **3.74 %** | #16, 1959 |
| 32 | 6 055 281 | **593 399** | **12.67 %** | #39, 5622 |

W=32/W=16 park ratio **3.39×** (round-2: 2.01×) — the round-2 critical-
section speedups raised the collision rate again. `WTF::Lock` parks
after ~40 spin iters; W=32 spin/acq 1.29 → most acquires still resolve
in spin.

K=256 discriminant (`/tmp/r3/bench-k256.js`, checksums unchanged —
sharding doesn't affect the flat-arm postings tuple):

| K | W | parks | park/acq | phaseA reps |
|---|---|---|---|---|
| 128 | 32 | 593 k–606 k | 12.7 % | {628, 705, 792} |
| **256** | 32 | 349 k–357 k | **7.5 %** | **{817, 832, 836}** |
| 128 | 16 | 175 k | 3.7 % | {537, 547, 554} |
| 256 | 16 | 28 k–170 k | 0.6–3.6 % | {535, 901, 1176} |

K=256 halves W=32 park rate exactly as predicted, but **W=32 phaseA does
NOT improve** (med 832 vs 705) and W=16 phaseA goes bimodal. **REFUTES
"K=256 would help"** — the W=32 phaseA 654–820 band is **NOT shard-lock
park-bound**; per-shard striping is moot (each shard already has its own
lock). The W=32 regression remaining after round-2's genlock-dclp is
something else (candidate: same allocation/coherency knee as §(2)'s
flatten1, plus the §(N) `operationGetByIdGaveUp` cost — both scale with
contending writers).

### (5) `forof-tdz-osr-loop` gating — STILL `useJSThreads()`-gated

`DFGFixupPhase.cpp:5123`: `if (Options::useJSThreads() && m_graph.has
ExitSite(…, BadType)) return;`. The in-tree comment (lines 5109–5122)
already documents that the underlying recompile loop is **generic**
(reproduces with all shared-heap flags off) and that the gate exists
solely "to keep flag-off codegen byte-identical (LAW)". Un-gating would
change flag-off DFG/FTL output on the recompile-after-`BadType` path
(an inserted `Check<…Use>` is dropped) — a flag-off-codegen change.
Under the strict byte-identical LAW this is **not** a chartered un-gate
without an explicit reviewer ruling that exit-profile-driven recompile
deltas are exempt; it is the same idiom already used elsewhere in the
file, which is the argument FOR exemption.

### (6) Re-baseline default + intcs (cbb7d616 / d953dcbf, 3-rep medians)

| arm | W | reps | median | §36 | Δ | reference tuple |
|---|---|---|---|---|---|---|
| default | 1 | {19062, 19583, 20029} | **19 583** | 18 069 | +8 % | `b3e65a68…\|4158957\|…\|c4bdd580…\|af028188…` ✓ |
| default | 16 | {12817, 12868, 12954} | **12 868** | 9 887 | **+30 %** | ✓ unchanged |
| intcs | 1 | {15054, 15326, 15557} | **15 326** | 14 030 | +9 % | `8021f000\|4158957\|1fc7d941\|…` ✓ |
| intcs | 16 | {4907, 7683, 7788} | **7 683** | 7 395 | +4 % | ✓ unchanged |

All 12 reps rc=0, all reference tuples match. The 4907 intcs rep is the
unchanged §34(C) fast-mode signature. **default W=16 +30 % vs §36** is
NOT host noise: a confirmatory rep at loadavg **0.35** gave 12 847 ms.
This binary's default arm matches §35 (13 013 / 13 183), not §36's
9 887; either §36's "+ worktree delta" carried a default-arm-relevant
change that did not land in `cbb7d616`, or §36's default-W=16 sample was
fast-mode-biased. Flagged for §36 cross-check.

### (N) NEW finding — `operationGetByIdGaveUp` 4.45 % W=16 self

`/tmp/r3/w16c.perf`: **4.17 % JS Thread + 0.28 % main = 4.45 % self**
(≈1.6 G cycles ≈ 31 ms wall at W=16); 0.41 % at W=1. Called from FTL
JIT code via the `op_call_ignore_result_return_location` chain (the
phaseA ingest loop). `PropertyInlineCache.cpp:278` returns `GaveUp` for
**any** `useJSThreads()` AccessCase whose base structure
`isDictionary()`; the IC repatches to call `operationGetByIdGaveUp`
forever. Hypothesis: the per-worker `sc` scratch object
(`makeFlatScratch`) — 17+ named slots added imperatively — goes
dictionary, and the hoisted `sc.hT/sc.hCount/sc.hFmap/…` reads inside
the per-worker `ingestHold` closure pay a generic C++ get-by-id every
iteration. At W=16 this is 4.45 % of CPU; named round-3 target
(**bench**: pre-shape `sc` via a single object-literal so it never goes
dictionary; **engine**: dictionary-safe self-load handler under the
structure lock instead of GaveUp).

Also visible in the W=16 profile: `ButterflySpine::publicLength` 4.95 %
+ `bumpPublicLengthToAtLeast` 2.75 % + `JSArray::pushInline` 2.11 %
self = **9.8 %** in the segmented-butterfly foreign-thread `[].push`
path (phaseA `pl.docIds.push(d)` under the shard lock); 1.28 %
`operationCompareStrictEq`; `lockProtoFuncHold` 8.98 % self (vs 6.29 %
W=1 — coherency on the Lock word).

### Synthesis (round-3, 1-paragraph)

All three §36 residual hypotheses are refuted on `cbb7d616`: (R1) the
57 ms W=16 flatten1 floor is **not** species STW (fires in phaseB at
+736 ms, 0 ArrayBuffer-species calls) and **not** per-worker FTL warmup
(pre-warm −25 % only; per-worker times uniform; clean 3.5× scaling
through W=8 then a hard W>8 contention knee, per-shard cost 4× at every
doubling 8→16→32) — it is a global-resource contention on the ~65 k
`new Int32Array(segmentedJSArray)` allocations; (R2)
`operationCompileFTLLazySlowPath` is **0.31 %** W=16 self at 5.28 M
calls (≈21 cyc/call) — the thunk-side DCLP's ceiling is ≤2 ms wall, do
not implement; (R3) K=256 halves W=32 park rate to 7.5 % but **does not
reduce** phaseA wall — the W=32 band is not shard-lock-park-bound. The
forof-tdz fix is still `useJSThreads()`-gated; un-gating is a flag-off
codegen change under the LAW. (1) the W=1 gap is **steady-state**
per-doc cost (≤5 ms warmup of 2800 ms phaseA; clean single tier-up;
57 % children in `lockProtoFuncHold`). New named target this round:
**`operationGetByIdGaveUp` 4.45 % W=16 self** via the
`PropertyInlineCache.cpp:278` dictionary-base GiveUp rule (likely the
`sc` scratch object). Re-baseline anomaly: default W=16 reads
**12 868 ms** on this binary (loadavg-0.35-confirmed), matching §35 not
§36's 9 887 — §36 default-arm number needs re-verification.

## Round 4

Tree `94fb0ed54cf6`, Release jsc, pinned GIL-off env, 64 HW threads,
loadavg 5-15 throughout (host moderately busy — absolute ms below are
upper-bound; ratios and counts are load-independent). Read-only on
`Source/`; uprobes via `perf probe` at `operationGetByIdGaveUp` (capture
`%di`=base, `%si`=propertyCache) and `operationCompileOSRExit`. Raw
artifacts: `/tmp/r4/{gaveup16{,b}.data, w{8,16}.perf, flatperf16.data,
w32-50rep.jsonl, w32cont.raw, osr-{2..6}.log, bench-{describe,describe16,
addr,flatperf,holdhoist}.js, lockmicro*.js}`. `git diff --stat Source/`
clean at exit.

### (1) `operationGetByIdGaveUp` — `sc`-dictionary REFUTED; root cause is **per-instance Lock Structure**

`describe()` probe after phaseA at W=16: **all 16 `sc` objects share ONE
StructureID** (24/24 inline, NonArray, Leaf-Watched, non-dictionary).
Same for `counters`, `pl`, `prng`, `published`, `barrier`, `shards[s]`.
**§37-R1's "sc goes dictionary" hypothesis is REFUTED.**

`perf probe gaveup=%di:%si` at W=16: **4 677 415 calls** (W=1: 4 721 148;
W=8: 4 701 605 — **call count is W-invariant**, ≈1.0×`hold` acquires).
Histogram by `pic` (PropertyInlineCache*): one site = **3.70 M / 4.68 M =
79 %**; its 119 distinct `base` addresses are **exactly the 128
`shards[s].lock` JSLockObject instances** (cross-referenced against an
in-process `describe()` address dump, 119/119 match).

Standalone repro (`/tmp/r4/lockmicro.js`): rotate `locks[i%K].hold(body)`
1 M times — **K≤8 ⇒ 0 gaveup; K=9 ⇒ 110 k; K=16 ⇒ 499 k; K=128 ⇒ 936 k**.
Single-lock micro: 0 gaveup.

`describe(new Lock())` ×128: **128 distinct StructureIDs** (sequential,
+128 each). Same for `new Condition()` and `new Thread()`. Root cause:

| ctor | site | line |
|---|---|---|
| `constructLock` | `JSLockObject::createStructure(vm, …)` per call | `LockObject.cpp:795` |
| `constructCondition` | `JSConditionObject::createStructure(vm, …)` per call | `ConditionObject.cpp:96` |
| `constructThread` | `JSThread::createStructure(vm, …)` per call | `ThreadObject.cpp:442` |

i.e. all three threading primitives allocate a **fresh Structure on every
construction** instead of caching one on `JSGlobalObject` (the standard
`InternalFunction::createSubclassStructure` / `LazyClassStructure`
pattern). With K=128 shard locks, the `.hold` GetById in `ingestDocFlat`
(bench.js:967) sees 128 base structures; after 8 AccessCases the IC would
go megamorphic — but `MegamorphicCache::fillsDisabledUnderJSThreads()`
(InlineCacheCompiler.cpp:4992) makes it return `GaveUp` instead, repatch
to `operationGetByIdGaveUp` forever. Every call then walks
`JSObject::getNonIndexPropertySlot` → `PropertyTable::findConcurrently`
under Lock.prototype's structure lock, which is where the W-dependent
self-% growth comes from (same call count, contended generic-get).

**Quantified bench-side workaround** (`/tmp/r4/bench-holdhoist.js`: hoist
`const __hold = Lock.prototype.hold;` once, replace bench.js:967 with
`__hold.call(shards[s].lock, ingestHold)` — single call site only),
3-rep, checksums unchanged:

| W=16 | baseline | hold-hoisted | Δ |
|---|---|---|---|
| phaseA_ms | {583, 596, 615} med **596** | {417, 440, 446} med **440** | **−156 (−26 %)** |
| total_ms | {956, 963, 979} med **963** | {764, 812, 821} med **812** | **−151** |
| gaveup calls | 4.53–4.68 M | 783–803 k | **−83 %** |

Residual 793 k gaveup is the unpatched sites (phaseB/C `.hold`, barrier,
phaseB `.slice` proto on the writer's Int32Array geometric-grow path,
etc.). **Engine target (round-4 #1): cache one instance Structure per
globalObject for Lock/Condition/Thread** — flag-on-only code, zero
flag-off impact, removes the entire 4.7 M-call gaveup family across every
arm. The `PropertyInlineCache.cpp:278` dictionary rule named in §37-R1 is
**not** the firing path here.

### (2) Segmented `[].push` 9.8 % — arrays + pre-size feasibility

Arrays: `pl.docIds.push(sc.hD)` / `pl.tfs.push(sc.hCount)` at
**bench.js:865-866** inside `ingestHold`, 4.68 M holds × 2 ≈ **9.36 M
pushes**. W=16 perf top (this binary, 23 K samples): `ButterflySpine::
publicLength` 8.17 % + `bumpPublicLengthToAtLeast` 3.40 % +
`JSArray::pushInline` 2.51 % + `operationArrayPush` 0.81 % +
`tryGrowSegmentedVectorLength` 0.62 % + `JSArray::length` 0.40 % =
**15.9 %** (W=8: 18.7 %). Secondary site `g.termIds/termTfs.push`
(bench.js:908-909) is ≤65 k pushes — negligible.

Pre-size: **not knowable** (per-term posting count is the Zipf draw
outcome; range 1..~7 k). The flat arm already HAS the right-shaped
alternative — `ingestWriterHold` (bench.js:868-885) does
geometric-doubling-`Int32Array` appends inside the same shard lock, born
typed. **Switching `ingestHold` to that body** removes `[].push`
entirely, removes the segmented-butterfly read cost from
`publicLength`/`bumpPublicLength`, removes the flatten1
`new Int32Array(segmentedJSArray)` path (flatten1 reduces to the
trim-slice arm at every W), AND removes the §(4) bc#223 BadIndexingType
slow-mode attractor (Int32Array has exactly one DFG ArrayMode). This was
the bench's ORIGINAL phaseA design (bench.js:775-786) — it was withdrawn
for a determinism bug in the *linked-list* variant; the
geometric-doubling variant is what phaseB has been running since round 2
without that bug. **Bench target (round-4 #2): make `sc.ingestHold` ≡
`sc.ingestWriterHold`** (engine-side alternative: a segmented-Int32Shape
`push` fast path is the harder, lower-ceiling fix).

### (3) flatten1 W>8 knee — perf-sliced; **Gigacage/ArrayBufferContents REFUTED**

Knee re-verified on this binary (3-rep median):

| W | flatten1 ms | shards/worker | ms/shard |
|---|---|---|---|
| 1 | 71 | 128 | 0.55 |
| 4 | 34 | 32 | 1.06 |
| 8 | 33 | 16 | 2.06 |
| **16** | **74** | 8 | **9.25** (4.5× W=8) |
| 32 | 94 | 4 | 23.5 (2.5× W=16) |

flatten2 (serial, slice-only) W-flat 62-66 ms.

Perf-sliced via `/tmp/r4/bench-flatperf.js`: a 30-rep
`new Int32Array(pl.docIds)` loop (no store-back) inserted before the real
flatten1, `perf record -F 1997 -D 600` to land inside the ~500 ms loop
window. W=16 top self (14 K samples, ≈22.3 G cycles):

| self% | symbol | reading |
|---|---|---|
| 16.97 | `forEachSegmentedIndexedContiguousRun<Int32 setFromArrayLike>` | the segwalk-copy work itself |
| 6.18 | `segmentedPublicLength` | source `.length` (per `new Int32Array`) |
| **5.89** | `JSObject::setButterflyConcurrent` | **fence/lock on the new TA's butterfly install** |
| 2.20 | `PropertyTable::findConcurrently` | concurrent proto lookup (iterator-protocol fast-path checks in `constructGenericTypedArrayViewWithArguments`) |
| 1.76 | `JSArrayBufferView` ctor | TA cell init |
| 1.50 | `constructGenericTypedArrayViewImpl<Int32>` | |
| 1.10 | `locationForOutOfLineOffsetConcurrent` | concurrent slot read |
| 0.70 | `storeTaggedButterflyWordConcurrent` | |
| **0.28** | `bmalloc_medium_bitfit_…try_allocate` | **only Gigacage/bmalloc symbol** |

Gigacage / ArrayBufferContents / pas / bmalloc symbols **sum to <0.5 %** —
**§37-R4's "Gigacage/ArrayBufferContents alloc?" REFUTED**. The W>8 knee
is `setButterflyConcurrent` + `findConcurrently`/`getConcurrently`/
`locationForOutOfLineOffsetConcurrent` (combined ≈10 %, all
structure-lock or fence-paired) on top of the unavoidable segwalk work.
30-rep-amortized per-shard cost grows W=8→16 **1.66×**, W=16→32 **1.71×**
— vs the single-shot **4.5×**, so ~⅔ of the single-shot knee is
**per-worker FTL warmup of `flattenFlatShards`** (each of W workers
compiles it independently, paying ~13 ms each — matches §37-R1's
"pre-warm −25 % only" partial). The §(2) bench fix obviates the entire
question (no segmented sources → flatten1 is trim-slice ≡ flatten2
≈64 ms serial / W).

### (3b) flatten1 W>8 knee is the **`pl.X=` PutById write-back**, NOT TA construction — REFUTES §(3) above

§(3)'s perf-slice was taken on `/tmp/r4/bench-flatperf.js`, a 30-rep
`new Int32Array(pl.docIds)` loop **with no store-back** — so its 5.89 %
`setButterflyConcurrent` + 2.20 % `findConcurrently` samples were the
synthetic loop's own work, **not** the real `flattenFlatShards` critical
path. Four bench-only discriminants on `94fb0ed5` (pinned GIL-off env,
checksums-don't-care probes) isolate the knee to the three
`pl.docIds=…; pl.tfs=…; pl.len=…` PutByIds at bench.js:1118-1120:

| variant | body | W=4 | W=8 | W=16 | W=32 | knee? |
|---|---|---|---|---|---|---|
| **full** (ref) | `.slice(0,len)` + 3 `pl.X=` puts | — | 29 | 63 | 98 | yes |
| (a) **noTA** | `pl.X` passthrough (no TA ctor), keep 3 puts | 18 | 22 | 64-73 | 90-97 | **FULL KNEE** |
| (b) **sum-sink** | drop 3 puts; `__flatSink += docIds.length+tfs.length+len` | — | 10 | 10-13 | 10 | **GONE, flat** |
| (c) **fixed-64** | `new Int32Array(64)` (FastTypedArray, no segwalk/iter), keep 3 puts | — | 30 | 72 | 88 | **FULL KNEE** |
| (d) iter-only | puts commented, no sink | — | 2-5 | 2-5 | 2-5 | (DCE artifact) |

(a) removes typed-array construction entirely → knee unchanged. (c)
removes the segwalk + iterator-protocol path + variable-size Gigacage
alloc → knee unchanged. (b) removes **only** the three `pl.` PutByIds
(keeps the reads live via a global `put_to_scope` sink) → knee gone,
flat 10 ms at W=8-32. The knee is the write-back.

C++ slow-path counts via uprobe, whole run, W=16:

| probe | count | reading |
|---|---|---|
| `putDirectInternalConcurrentOutOfLine<PutModePut>` | 165-259 | not the ~196 k flatten1 puts |
| `llint_slow_path_put_by_id` | 1.6-5.6 k | warm-up only |
| `operationPutByIdStrictOptimize` | 87-138 | repatch budget |
| `operationPutByIdStrictGaveUp` | **0** | IC never gives up |
| `Structure::getConcurrently` | 5.97 M | W-invariant (not flatten1) |
| `findStructuresAndMapForMaterialization` | ~3.1 k | |
| `didReplacePropertySlow` | ~100 | |
| `stopTheWorldAndRun` | 433 (sum-sink: 491) | **STW NOT discriminant** |

The ~196 k flatten1 PutByIds therefore stay on the **JIT IC fast path**
— no C++ slow-path explosion, no STW delta. The W>8 knee is inside the
inline-cache fast-path machine code for an existing-slot replace on a
shared-heap object whose butterfly was allocated by a foreign thread.
**§(3)'s `setButterflyConcurrent`/`findConcurrently` attribution is the
wrong call-site; the §(2)-predicted "trim-slice ≡ flatten2 ≈64 ms
serial / W" did NOT materialise** (W=16 is 63 ms, not 64/16≈4 ms).
**Redirects all R4 flatten1 finder budget off the TA-ctor / Gigacage /
iterator-protocol surface** and onto the PutById-replace IC fast path
under W>8 cross-thread butterfly ownership. estMs ≈ 53 (W=16 full 63 →
sum-sink 10).

### (4) W=32 50-rep — slow rate 20 %, signature is `ingestHold` bc#223 BadIndexingType (NOT (1))

50-rep sweep (`/tmp/r4/w32-50rep.jsonl`, loadavg 5.8→15.5): **clean
two-mode**. Fast 40/50: phaseA 378-463 (med 430), total 797-898. Slow
10/50: phaseA 682-759 (med 720), total 1089-1177. Across all batches
(50+12+15+12+20 = 109 reps): **28/109 ≈ 26 % slow**.

| mode | phaseA | parks | park/acq | `operationCompileOSRExit` calls |
|---|---|---|---|---|
| fast | 411-454 | 262-344 k | 5.6-7.4 % | **32-34 k** |
| slow | 687-741 | 596-607 k | 12.7-13.0 % | **1.71-1.85 M (54×)** |

`JSC_printEachOSRExit=1` slow-rep histogram (4.98 M lines):
**1 630 899 × `bc#223 BadIndexingType` in `#AwjwZZ` (FTLFunctionCall, 230
bc, reoptimizationRetryCounter=2)**; fast-rep same site = **818**.
`#AwjwZZ` is the `sc.ingestHold` closure body (230 bc, called 4.68 M ×;
bc#223 is the `pl.tfs.push(...)` ArrayPush site at bench.js:866).
Mechanism: when FTL compiles `ingestHold` early (before cross-thread
pushes have driven `pl.docIds`/`pl.tfs` segmented), it speculates a
contiguous IndexingType; once segmented, every call OSR-exits at bc#223
inside the held shard lock; after 2 reopt rounds the backoff pins the bad
FTL for the rest of phaseA (≈35 % of holds × OSR-exit→Baseline body
≈ +280 ms; downstream park/acq doubles).

**Hold-hoist workaround does NOT collapse it** (8/20 slow at W=32 with
`/tmp/r4/bench-holdhoist.js`, same ~1.77 M osrexit signature) — (4) is
**independent of (1)**. The §(2) bench fix (typed-array phaseA storage)
removes the speculation surface entirely; engine-side, the
already-landed `forof-tdz-osr-loop` analog applies (after a
`BadIndexingType` exit on a useJSThreads ArrayPush, recompile with the
segmented arm widened — currently the FTL re-speculates the same thing).

### (5) W=8 vs W=16 — same residuals, no W-specific term

`perf -F 1997` W=8 (12 K samples) vs W=16 top self:

| symbol | W=8 self% | W=16 self% |
|---|---|---|
| `lockProtoFuncHold` | 16.7 | 14.7 |
| `ButterflySpine::publicLength` | 9.9 | 8.2 |
| `operationGetByIdGaveUp` | 7.1 | 5.8 |
| `bumpPublicLengthToAtLeast` | 4.4 | 3.4 |
| `JSArray::pushInline` | 3.3 | 2.5 |
| `[k] native_queued_spin_lock_slowpath` | — | 2.5 |
| `operationCompareStrictEq` | 0.5 | 1.3 |

W=8 gaveup = 4.70 M (W-invariant). **No W=8-specific residual**: the
71 ms gap to Java is the same three families ((1) Lock-structure gaveup,
(2) segmented `[].push`, `lockProtoFuncHold` trampoline) at slightly
*higher* relative weight than W=16 (less futex/idle dilution). The
W=16-only kernel-spin 2.5 % is the shard-lock collision rate growing,
not a new mechanism.

### Synthesis (round-4, 1-paragraph)

The §37 `operationGetByIdGaveUp` 4.45 % residual is **not** `sc` going
dictionary (all 16 `sc` share one non-dictionary Structure): it is
**`constructLock`/`constructCondition`/`constructThread` allocating a
fresh Structure on every `new`** (`LockObject.cpp:795`,
`ConditionObject.cpp:96`, `ThreadObject.cpp:442`), so K=128 shard locks
present 128 distinct base StructureIDs to the `.hold` GetById, the IC
hits the 8-case ceiling, megamorphic is disabled under `useJSThreads`,
and the site repatches to `operationGetByIdGaveUp` permanently
(W-invariant 4.7 M calls/run, 79 % of which the uprobe traces to the
shard-lock instances). A one-line bench workaround (`Lock.prototype.hold`
hoist + `.call`) cuts W=16 phaseA 596→440 ms and total 963→812 ms; the
**engine fix** is to cache the instance Structure on `JSGlobalObject`
(flag-on-only code, zero flag-off risk). The 9.8 % segmented-`[].push`
family is `pl.docIds/pl.tfs` (bench.js:865-866, 9.4 M pushes); it cannot
be pre-sized but **the existing `ingestWriterHold` geometric-Int32Array
body** is the drop-in replacement and simultaneously kills (2), the
flatten1 `new Int32Array(segmented)` path, and the W=32 slow mode. The
flatten1 W>8 knee perf-slice shows **<0.5 %** in Gigacage/
ArrayBufferContents/bmalloc — **REFUTES** §37-R4; **but §(3)'s
`setButterflyConcurrent`/`findConcurrently` attribution is itself
REFUTED by §(3b)**: four discriminants (noTA / sum-sink / fixed-64 /
iter-only) show the knee tracks the three `pl.X=` PutById write-backs
(bench.js:1118-1120) exactly — present with no TA ctor at all, gone
when only the puts are dropped — and uprobe counts place all ~196 k
puts on the JIT IC fast path (gaveup=0, STW non-discriminant). The
§(3) perf-slice had no store-back, so it profiled the wrong loop.
W=32 is bimodal at **26 %** slow (10/50 in
the clean sweep): slow-mode signature is **1.63 M `BadIndexingType` OSR
exits at `ingestHold` bc#223** (the `pl.tfs.push` ArrayPush) vs 818 in
fast mode — early-FTL speculates contiguous, cross-thread pushes go
segmented, reopt backoff pins it; **independent of (1)** (hold-hoist
does not collapse it). W=8's 71 ms-to-Java gap has **no W-specific
residual** — same three families at higher relative weight.
