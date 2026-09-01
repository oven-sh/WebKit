# TSAN-TRIAGE.md — GIL-off TSAN campaign

Status: round 25 (r25) re-run triaged — see §22 for the current table. r24
in §21, r23 re-snapshot in §20; r0-r22 history in §§0-19. Earlier
rounds: r4 in §12 (+§13 review amendments), r3 in §10 (+§11), r2 in §8 (+§9),
r1 in §6 (+§7), r0 in §§0–5; rulings stand unless amended in a later section.
Raw reports: `Tools/threads/tsan/reports-r0.log` (10515), `reports-r1.log`
(10642), `reports-r2.log` (3166), `reports-r3.log` (1908), `reports-r4.log`
(1012), `reports-r5.log` (763). Per-test logs:
`Tools/threads/tsan/{r0,r1,r2,r3,r4,r5}/`. Dedup/classification tooling:
`Tools/threads/tsan/{dedup.py,classify.py,run-corpus-tsan.sh,mkreports.sh}`.

## 0. TSAN configuration (standing-ruling compliance)

The pre-existing `WebKitBuild/TSan` directory was a **CLoop build**
(`ENABLE_C_LOOP=ON`, `ENABLE_JIT=OFF`). Per the standing ruling (CLoop is not
used in production and is fake work), the directory was **wiped and
reconfigured** with the REAL LLInt asm + full JIT:

- `ENABLE_C_LOOP=OFF`, `ENABLE_JIT=ON`, `ENABLE_DFG_JIT=ON`, `ENABLE_FTL_JIT=ON`,
  `ENABLE_WEBASSEMBLY(+BBQ/OMG)=ON`, `ENABLE_SAMPLING_PROFILER=ON` (mirrors the
  Debug build's feature set), `ENABLE_SANITIZERS=thread`,
  `USE_SYSTEM_MALLOC=ON` (TSAN-interceptable allocator),
  `CMAKE_BUILD_TYPE=RelWithDebInfo` + `-fno-omit-frame-pointer -g`,
  clang-21, same Bun flags (`USE_BUN_JSC_ADDITIONS`, `USE_BUN_EVENT_LOOP`,
  `ENABLE_STATIC_JSC`).
- Build verified: `ENABLE_C_LOOP:BOOL=OFF` in `WebKitBuild/TSan/CMakeCache.txt`;
  binary mtime (Jun 9 10:19) newer than the newest source file (Jun 9 05:08).
- Smoke (`JSTests/threads/smoke.js`, GIL-off env, full JIT, 3x): PASS each run,
  exit 66 (= races reported, halt_on_error=0), 36/44/40 data-race reports, no
  instrumentation crashes or hangs. TSAN + JIT is workable on this tree; the
  no-JIT fallback was NOT needed.
- **Zero CLoop frames** appear anywhere in the r0 corpus output (CLoop is not
  even compiled in). No CLoop suppressions were needed; none were added.

**Accepted tradeoff (documented per ruling):** TSAN cannot see races inside
JIT-generated code. That coverage belongs to the object-model protocol tests
and the race amplifier, not TSAN. What TSAN is for here is the C++ side —
runtime slow paths, GC, caches, profiling, code lifecycle — and indeed every
family below lives there. A side effect visible in the reports: when JIT'd
code is one side of a race, only the C++ side's stack is symbolized (a small
tail of reports has a degraded/absent second stack; see family `misc-tail`).

## 1. Run configuration

```
binary   WebKitBuild/TSan/bin/jsc  (full JIT, asm LLInt)
env      JSC_useThreadGIL=false JSC_useVMLite=true JSC_useSharedAtomStringTable=true
         JSC_useSharedGCHeap=true JSC_useThreadGILOffUnsafe=true
args     --useJSThreads=1 (plus each test's //@ requireOptions)
TSAN     suppressions=Tools/tsan/suppressions.txt halt_on_error=0
         history_size=7 second_deadlock_stack=1
corpus   all JSTests/threads/**/*.js except resources/ + harness.js
         (224 candidate files: 218 ran, 6 //@ skip)
timeout  420 s/test; 1 timeout: semantics/proto-cycle-race.js (KNOWN-FAILING
         list, functional bug queued separately; its 5 race reports ARE kept)
```

The six pre-existing upstream parallel-GC suppressions in
`Tools/tsan/suppressions.txt` were active during the run (rule-1 adjudicated
entries; see that file). The KNOWN-FAILING tests (date-cache-churn,
proto-cycle-race, symbol-registry-cross-thread, havebadtime-vs-indexed-fastpath)
ran; their functional failures are out of scope but their race reports are
counted below.

Result: **10515 reports**, 502 raw deduped stack-pair keys, grouped into the
**32 families** below. `unsuppressedCount = 10515` (nothing new suppressed in
this round; no CLoop families existed to suppress).

## 2. Family table

Counts are exact per the classifier (`classify.py` over `families-r0.txt`)
except where a raw key was split/merged after manual inspection of
representative reports (noted in the per-family sections). Rulings reference
SPEC-objectmodel (OM), SPEC-ungil (UG), SPEC-jit (JIT).

| # | id | count | spec row | ruling | files (wave ownership) |
|---|----|-------|----------|--------|------------------------|
| 1 | value-profile | 1812 | JIT §5.7.4 (buckets word-atomic), §5.7.7 | relaxed-atomic | bytecode/ValueProfile.h, bytecode/SpeculatedType.h, bytecode/LazyOperandValueProfile.h |
| 2 | exec-counter | 1553 | JIT §5.7.1/§5.7.2 | relaxed-atomic | bytecode/ExecutionCounter.h, bytecode/ExecutionCounter.cpp, runtime/ScriptExecutable.h, bytecode/UnlinkedMetadataTable.h |
| 3 | array-profile | 1061 | JIT §5.7.5 | relaxed-atomic | bytecode/ArrayProfile.h, bytecode/ArrayProfile.cpp |
| 4 | ic-stubinfo | 721 | JIT §5.1/D3, §4.4 | lock | bytecode/PropertyInlineCache.h, bytecode/PropertyInlineCache.cpp, jit/Repatch.cpp |
| 5 | code-lifecycle | 609 | JIT §4.x code lifecycle (publish via one pointer) | concurrent-accessor | runtime/ExecutableBase.h, runtime/ScriptExecutable.cpp, runtime/FunctionExecutable.h, bytecode/CodeBlock.h, bytecode/CodeBlock.cpp, jit/JITCode.h |
| 6 | watchpoints | 499 | JIT §5.6; OM §5 (WS fired only in STW; states monotonic) | relaxed-atomic | bytecode/Watchpoint.h, bytecode/Watchpoint.cpp |
| 7 | cell-header | 486 | OM §3.0/GT#2 (header bytes via concurrent accessors) | concurrent-accessor | runtime/JSCell.h, runtime/JSCellInlines.h, runtime/TypeInfoBlob.h |
| 8 | arith-profile | 485 | JIT §5.7.5 | relaxed-atomic | bytecode/ArithProfile.h |
| 9 | structure-fields | 416 | OM §5/§9.4, UG §K (publication); OM I34 | concurrent-accessor | runtime/Structure.h, runtime/Structure.cpp, runtime/StructureRareData.h, runtime/StructureRareData.cpp, runtime/StructureChain.cpp |
| 10 | jsvalue-slots | 361 | OM §1 GT (JS value words intentionally racy) | relaxed-atomic | runtime/WriteBarrier.h, runtime/WriteBarrierInlines.h, runtime/JSCJSValue.h |
| 11 | codeblock-init | 316 | JIT §5.7.7 (advisory <=8B plain iff atomic), UG §K | relaxed-atomic | bytecode/UnlinkedFunctionExecutable.h, bytecode/CodeBlock.h (advisory flags), jit/BaselineJITCode.h, interpreter/Interpreter.cpp |
| 12 | metaallocator-stats | 247 | JIT §5.7.7 (advisory datum) | relaxed-atomic | WTF/wtf/MetaAllocator.h, WTF/wtf/MetaAllocator.cpp |
| 13 | linkbuffer-stats | 191 | JIT §5.7.7 | relaxed-atomic | assembler/LinkBuffer.h, assembler/LinkBuffer.cpp, assembler/AbstractMacroAssembler.h |
| 14 | simple-stats | 177 | JIT §5.7.7 | relaxed-atomic | WTF/wtf/SimpleStats.h |
| 15 | butterfly-words | 157 | OM §2/§4 (butterfly word via tag accessors) | concurrent-accessor | runtime/JSObject.h, runtime/Butterfly.h, runtime/ButterflyInlines.h, runtime/IndexingHeader.h, runtime/AuxiliaryBarrier.h |
| 16 | tinybloom | 140 | OM §5 transition-table lookup (false positives tolerated) | relaxed-atomic | heap/TinyBloomFilter.h |
| 17 | rope-stringimpl | 140 | OM GT (cells racy; SAB staleness), UG SD (shared strings) | relaxed-atomic | runtime/JSString.h, runtime/JSString.cpp, WTF/wtf/text/StringImpl.h |
| 18 | regexp-shared | 137 | UG §K (GIL-serialized per-global caches) — NOT blessed | real-bug | runtime/RegExpCachedResult.h, runtime/RegExpCachedResult.cpp, runtime/RegExpGlobalData.h, runtime/RegExpGlobalData.cpp, runtime/RegExpInlines.h |
| 19 | alloc-profile | 113 | JIT §5.7.5/§5.7.7 | relaxed-atomic | bytecode/ArrayAllocationProfile.h, bytecode/ArrayAllocationProfile.cpp, bytecode/ObjectAllocationProfile.h, WTF/wtf/CompactPointerTuple.h |
| 20 | calllink | 99 | JIT §5.8 (CallLinkRecord single published ptr) | concurrent-accessor | bytecode/CallLinkInfo.h, bytecode/CallLinkInfo.cpp, jit/PolymorphicCallStubRoutine.h, jit/PolymorphicCallStubRoutine.cpp |
| 21 | gc-marking-residual | 96 | suppressions Part A residuals ("finish the header pass") | relaxed-atomic | heap/GCSegmentedArray.h, heap/GCSegmentedArrayInlines.h, heap/IsoCellSetInlines.h, heap/MarkingConstraint.h, heap/SlotVisitor.h, heap/Heap.h, WTF/wtf/Threading.h |
| 22 | vm-string-caches | 96 | UG §K (VM caches need locks/once) — NOT blessed | real-bug | runtime/NumericStrings.h, runtime/KeyAtomStringCache.h, runtime/KeyAtomStringCache.cpp, runtime/SmallStrings.cpp, runtime/StringRecursionChecker.h |
| 23 | misc-tail | 90 | n/a (stack-degraded duplicates; one side JIT/realloc) | suppress | (none this wave — re-triage in wave 2; see §3.32) |
| 24 | typedarray-sab | 87 | OM §4.7 analog (raw lanes, SAB semantics); view fields = UG §A | concurrent-accessor | runtime/JSArrayBufferView.h, runtime/JSArrayBufferViewInlines.h, runtime/JSGenericTypedArrayViewInlines.h, runtime/JSGenericTypedArrayViewPrototypeFunctions.h, runtime/ArrayBufferContents... (see §3.24) |
| 25 | property-table | 81 | OM §6/L6 + §6 quarantine | concurrent-accessor | runtime/PropertyTable.h, runtime/PropertyTable.cpp, runtime/StructureInlines.h |
| 26 | block-directory-bits | 75 | suppressions B1 adjudication (rule-1-ineligible; fix not suppress) | relaxed-atomic | heap/BlockDirectoryBits.h |
| 27 | gc-incoming-ref | 59 | none — NOT blessed (single-mutator assumption) | real-bug | heap/GCIncomingRefCounted.h, heap/GCIncomingRefCountedInlines.h, heap/GCIncomingRefCountedSet.h, heap/GCIncomingRefCountedSetInlines.h |
| 28 | barrier-counter | 53 | JIT §5.7.7-style advisory counter | relaxed-atomic | heap/Heap.h, heap/Heap.cpp |
| 29 | date-cache | 51 | UG §K — NOT blessed | real-bug | runtime/JSDateMath.h, runtime/JSDateMath.cpp, runtime/DateInstanceCache.h, runtime/DateInstance.cpp |
| 30 | microtask-queue | 41 | UG §E.1 (per-thread queues) — NOT blessed | real-bug | runtime/VM.cpp, runtime/MicrotaskQueue.h, runtime/MicrotaskQueue.cpp |
| 31 | directarguments | 37 | OM GT (cell publication; stale reads blessed, writers must be atomic) | concurrent-accessor | runtime/DirectArguments.h, runtime/DirectArguments.cpp, runtime/GenericArgumentsImplInlines.h, runtime/JSFunctionInlines.h, runtime/FunctionRareData.h |
| 32 | symbol-registry | 17 | UG §H — NOT blessed | real-bug | runtime/VM.cpp, runtime/WeakGCMapInlines.h |
| 33 | vm-shared-misc | 12 | UG §A (per-thread limits) + §K (lazy init) | lock | runtime/VM.cpp, runtime/VM.h, runtime/LazyPropertyInlines.h |

Total: 10515. Post-CLoop-suppression unsuppressed count: **10515** (no CLoop
families appeared; nothing suppressed this round).

## 3. Per-family detail

### 3.1 value-profile (1812) — relaxed-atomic
Top keys: `mergeSpeculation<u64> x mergeSpeculation<u64>` (737),
`UnlinkedValueProfile::update` self/cross pairs (588+),
`ValueProfileBase::isSampledBefore x mergeSpeculation` (381),
`computeUpdatedPredictionForExtraValue`, `CompressedLazyValueProfileHolder`
pairs, `iteratorOpen/NextTryFastImpl` profile writes.
Spec: JIT §5.7.4 — buckets are aligned 64-bit, word-atomic, never torn; the
prediction word merges are tolerate-don't-synchronize. Plain `uint64_t` RMW is
still C++ UB; the blessed fix is `WTF::Atomic` relaxed load/RMW.
Fix shape: make `ValueProfileBase` bucket slots and `m_prediction`-style
fields (incl. `UnlinkedValueProfile`) `WTF::Atomic<...>` with relaxed
load/store; `mergeSpeculation` gets a relaxed-atomic merge variant used by
profile update paths (compiler-side readers already snapshot). Flag-off
codegen unchanged (relaxed = plain mov on x86/arm64).
Evidence (trimmed):
```
Write 8  JSC::mergeSpeculation<u64>  bytecode/SpeculatedType.h  <- ValueProfileBase::computeUpdatedPrediction (T1)
Prev W 8 JSC::mergeSpeculation<u64>  bytecode/SpeculatedType.h  <- ValueProfileBase::computeUpdatedPrediction (T4)
```

### 3.2 exec-counter (1553) — relaxed-atomic
Top keys: `ExecutionCounter<> x ExecutionCounter<>` (1227), `operationOptimize`
self-pairs (134), `setupWithUnlinkedBaselineCode x LLInt::jitCompileAndSetHeuristics`
(61), `ScriptExecutable::setDidTryToEnterInLoop` (42),
`UnlinkedMetadataTable::setDidOptimize` (18), `shouldOptimizeNowFromBaseline`,
`osrExitCounter`, DFG threshold setters, `tierUpCommon`.
Spec: JIT §5.7.1 (execution counters relaxed atomic adds from C++; JIT'd
fast-path adds may stay plain) + §5.7.2 (tier-up CAS serializes the actual
enqueue). These reports are the C++ counter RMWs and threshold rewrites.
Fix shape: convert `ExecutionCounter` fields (`m_counter`, `m_activeThreshold`,
`m_totalCount`) to relaxed `WTF::Atomic`; same for `m_osrExitCounter`,
`m_didTryToEnterInLoop`, `UnlinkedMetadataTable::m_didOptimize`. Tier-up
decision dedup is already covered by the §5.7.2 CAS (in tree).
Evidence:
```
Write 4  JSC::ExecutionCounter<16>::setNewThreshold  bytecode/ExecutionCounter.cpp (T2)
Prev W 4 JSC::ExecutionCounter<16>::checkIfThresholdCrossedAndSet (T5)
```

### 3.3 array-profile (1061) — relaxed-atomic
`computeUpdatedPrediction` self-pairs (849), `OptionSet<ArrayProfileFlag>` plain
read vs `Atomic::exchangeOr` (144), `observeStructureID` pairs.
Spec: JIT §5.7.5 — flag merges relaxed atomic OR where compiler-read; lost bit
benign (I12). Half the work already landed (the exchangeOr side); the
remaining plain loads/stores of the OptionSet word and `m_lastSeenStructureID`
need relaxed accessors.
Fix shape: `ArrayProfile::m_arrayProfileFlags` -> relaxed Atomic load
everywhere (writer already exchangeOr); `m_lastSeenStructureID`,
`m_speculationFailureStructureID` -> relaxed Atomic.

### 3.4 ic-stubinfo (721) — lock (per JIT §5.1) + relaxed countdowns
`considerRepatchingCacheImpl` self/cross pairs, `isHandlerIC` vs
`operation*GaveUp` writers, `initializeFromUnlinkedPropertyInlineCache` vs
repatch, `incrementWithSaturation<u8>` countdown pairs (51, PropertyInlineCache.h:276),
`ICSlowPathCallFrameTracer`/`considerRepatching x malloc` pairs (stub memory
reuse: reader holds a stale stub while it is freed+reallocated -> JIT §4.4
epoch reclamation applies).
Spec: JIT §5.1/D3 — IC state is mutable multi-field data; writers must hold
`m_lock` (ConcurrentJSLocker); fast-path readers go through ONE published
handler pointer. §5.7.7 covers the u8 countdowns.
Fix shape: audit every `PropertyInlineCache` mutation site to hold the owner
CodeBlock's `m_lock` (the `GaveUp`/reset writers are the gap); make
`countdown`/`bufferingCountdown` and the `GaveUp` state byte relaxed
`Atomic<uint8_t>`; ensure retired handler chains are freed only via the §4.4
epoch (the `x malloc` pairs).
Evidence:
```
Write 1  WTF::incrementWithSaturation<u8>  <- PropertyInlineCache::considerRepatchingCacheImpl PropertyInlineCache.h:276 (T2)
Prev W 1 same site (T1)
```

### 3.5 code-lifecycle (609) — concurrent-accessor
`CodeBlock::jitType x setJITCode` (236), `ExecutableBase::hasJITCodeFor x
ScriptExecutable::installCode` (128, the RefPtr<JITCode>/RawPtrTraits
std::exchange writer), `CodeBlock::jitCode x setJITCode` (55),
`FunctionExecutable::codeBlockFor/replaceCodeBlockWith` (51+19+11),
`CodeBlock::replacement x replaceCodeBlockWith` (47), `JITCode::JITCode x
jitType`, DFG/FTL `initializeCodeRef`/`addressForCall`, vptr races at
`linkFor`/`prepareOSREntry` (ctor vs virtual call = publish-before-init).
Spec: JIT code lifecycle — all fast-path consumption flows through one
published pointer; installs are publishes, never in-place mutation.
Fix shape: store `ExecutableBase::m_jitCodeFor{Call,Construct}` and
`CodeBlock::m_jitCode` through release-store/relaxed-load accessors (publish
fully-constructed JITCode only; kill the bare `std::exchange` in
`ScriptExecutable::installCode` in favor of an atomic swap); `m_codeBlockFor*`
WriteBarrier reads on the read side get relaxed concurrent loads. The vptr
pairs vanish once publication is release-ordered after construction.
Evidence:
```
Read 8   JSC::ExecutableBase::hasJITCodeFor  runtime/ExecutableBase.h (T6)
Prev W 8 std::exchange<JITCode*> <- RefPtr::operator=(nullptr) <- ScriptExecutable::installCode ScriptExecutable.cpp:291 (T5)
```

### 3.6 watchpoints (499) — relaxed-atomic
`WatchpointSet::add x state` (243), `InlineWatchpointSet::inflateSlow x state`
(121), `state x malloc` (47 — a freed WatchpointSet read through a stale
Structure/rare-data: lifetime handled by structure-fields wave; the state
accessor fix still applies), `startWatching/fireAllSlow/invalidate x state`,
DFG `clobberize` state reads.
Spec: OM §5 — sets fired only in STW (I13), states monotonic; JIT §5.6 reads
states racily by design. The unblessed part is that `m_state` is a plain
int32 and the fat/thin `InlineWatchpointSet` word is plain.
Fix shape: `WatchpointSet::m_state` -> relaxed `Atomic<WatchpointState>`;
`InlineWatchpointSet::m_data` -> relaxed Atomic word (inflateSlow publishes
the fat pointer via release CAS); `add` keeps requiring the owner's
serialization (assert), only the state/word accesses become atomic.

### 3.7 cell-header (486) — concurrent-accessor
`JSCell::JSCell` ctor header init vs concurrent `cellHeaderConcurrentLoad`
readers (239 + the explicit `cellHeaderConcurrentLoad` pairs 19/10/6/3),
`setStructure`/`setCellState`/`clearStructure` plain writes vs readers,
`TypeInfoBlob::operator=` pairs, `dcasHeaderAndButterfly` vs plain cellState.
Spec: OM §3.0/GT#2 — header bytes are read via the cell-header concurrent
accessors and stale values re-dispatch; the READER side is already blessed
and atomic ("Atomic read of size 1" in reports). The writer side (ctors,
setStructure, setCellState) still does plain stores = UB.
Fix shape: make the header-word writers relaxed atomic: JSCell ctor initial
header store (single 64-bit relaxed store of the assembled header),
`setStructureIDDirectly`/`setCellState`/`clearStructure` -> relaxed stores;
`TypeInfoBlob` accessors get relaxed load/store variants. No ordering change
flag-off (same mov).
Evidence:
```
Atomic R 1 JSC::cellHeaderConcurrentLoad<JSType> <- JSCell::type <- speculationFromCell (T5)
Prev W 4   JSC::JSCell::JSCell JSCellInlines.h:88 <- JSString::create <- jsString (T2)   [recycled cell]
```

### 3.8 arith-profile (485) — relaxed-atomic
`observeLHSAndRHS`/`observeResult`/`observedResults`/`setBit` pairs and
`updateArithProfileForBinaryArithOp` slow-path writes.
Spec: JIT §5.7.5. Fix shape: `ArithProfile<T>::m_bits` -> relaxed
`Atomic<T>` with relaxed OR for setBit/observe; readers relaxed load.

### 3.9 structure-fields (416) — concurrent-accessor
`Structure::Structure` ctor vs `classInfoForCells/typeInfo/realm/...`
concurrent readers (113+59+29+12...), `Structure::m_lock` byte init vs lock
CAS from `getConcurrently` (39, the `Atomic<u8>::Atomic` keys),
`setMaxOffset x maxOffset` (5, known suspect — torn maxOffset breaks I34),
`hasRareData x rareData-CAS` (43 across keys), `previousID/setPreviousID`,
`prototypeChain/clearCachedPrototypeChain` + `StructureChain` publication +
`VectorBufferBase<StructureID> x malloc` (32), enumerator-cache install
(`setCachedPropertyNameEnumerator`, `StructureChainInvalidationWatchpoint`
FixedVector rebuild — the AUD1.N4(3) UAF window exercised by
races/forin-enumerator-cache.js).
Spec: OM §5/§9.4 (structure TID/WS fields), I34 (maxOffset), UG §K
(publication of new Structures); compilers already read via
`getConcurrently`.
Fix shape: new Structures must be fully initialized before the StructureID /
transition-table publish (release fence at publication, already the OM design
— add the missing release on the publish store); `m_maxOffset`/`m_inlineCapacity`
reads+writes via relaxed Atomic (16-bit); rare-data pointer via
acquire/release CAS (mostly present — fix the remaining plain `hasRareData`
reads); enumerator-cache install must take the Structure lock and retire the
old watchpoint FixedVector through the GC (not immediate free).
Evidence:
```
Atomic W 1 WTF::Lock::lock <- Structure::findStructuresAndMapForMaterialization Structure.cpp:543 <- getConcurrently (T1)
Prev W 8   JSC::Structure::Structure (ctor member init, same word)  (T3)
```

### 3.10 jsvalue-slots (361) — relaxed-atomic
`WriteBarrierBase::get` plain reads vs `updateEncodedJSValueConcurrent` /
property CAS (`Atomic<u64>::compareExchangeStrong`) / `setWithoutWriteBarrier`
plain writes; `Register::jsValue`; sparse-map entry values.
Spec: OM ground truth — JS value words are intentionally racy; plain C++
access is UB; the blessed fix is exactly the existing concurrent accessors.
Fix shape: flag-independent: `WriteBarrierBase<Unknown>::get/setWithoutWriteBarrier`
do relaxed `WTF::Atomic<EncodedJSValue>` load/store (codegen-identical
flag-off); remaining direct `m_value` touches routed through
`updateEncodedJSValueConcurrent`.
Evidence:
```
Read 8   WriteBarrierBase<Unknown>::get <- JSObject::getDirect (T3)
Prev W 8 JSC::updateEncodedJSValueConcurrent <- putDirectOffsetConcurrent (T1)
```

### 3.11 codeblock-init (316) — relaxed-atomic
`UnlinkedFunctionExecutable::setSingletonHasBeenInvalidated` bit-field RMW vs
same-word bit-field reads (`privateBrandRequirement`, `parameterCount`) — 44+13+1
plus the 149 `Interpreter::executeCallImpl:1302` pairs which are
`newCodeBlock->m_shouldAlwaysBeInlined = false` (advisory bool written by
every caller), `BaselineJITCode::{liveness,fullness}Rate` (52),
`UnlinkedCodeBlock` liveness/`setDidOptimize`-style flags, `CachedCall`
upgrade pairs.
Spec: JIT §5.7.7 — a datum may stay plain only if <=8B AND advisory; these
are advisory but bit-fields share words -> must become explicit atomics.
Fix shape: move `m_singletonHasBeenInvalidated` (and any flag sharing its
word) out of the packed bit-field into `Atomic<uint8_t>` (same pattern as the
chartered `Thread::m_gcThreadType` fix); `m_shouldAlwaysBeInlined`,
liveness/fullness rates -> relaxed Atomic.
Evidence:
```
Read 8   UnlinkedFunctionExecutable::privateBrandRequirement (bit-field word) <- generateUnlinkedFunctionCodeBlock (T2)
Prev W 8 UnlinkedFunctionExecutable::setSingletonHasBeenInvalidated <- JSFunction::create <- llint_slow_path_new_func_exp (T3)
```

### 3.12 metaallocator-stats (247) — relaxed-atomic
`MetaAllocator::allocate` (m_bytesAllocated += under the pool lock) vs
unlocked `bytesAllocated()` heuristic reads (ExecutableAllocator fullness).
Advisory datum per JIT §5.7.7. Fix: `m_bytesAllocated` -> relaxed Atomic.

### 3.13 linkbuffer-stats (191) — relaxed-atomic
`LinkBuffer::performFinalization` self-pairs = static cumulative
size/profile counters (s_profileCummulativeLinked*); plus
`AbstractMacroAssemblerBase::initializeRandom` once-init of the static
random seed. Fix: counters -> relaxed Atomic adds; `initializeRandom` ->
`std::call_once`/atomic CAS init.

### 3.14 simple-stats (177) — relaxed-atomic
`WTF::SimpleStats::add` vs `mean/variance/operator bool` — jit allocation
stats. Advisory. Fix: SimpleStats fields -> relaxed Atomic (or a
ConcurrentSimpleStats used by the JIT consumers).

### 3.15 butterfly-words (157) — concurrent-accessor
`taggedButterflyWord` reader vs plain `AuxiliaryBarrier::setWithoutBarrier`
writers and ctor inits; `IndexingHeader::{publicLength,vectorLength}` plain
read/write vs the `Atomic` store/CAS in `ensureLengthSlowConcurrent`/
`createInitialIndexedStorageConcurrent`.
Spec: OM §2/§4 — the butterfly word is read via the tag accessors; C4 bounds
indexed access by min(publicLength, vectorLength) with stale values legal.
The unblessed remainder is the plain writer side.
Fix shape: route every flag-on butterfly-word store through the concurrent
store (relaxed/release per §3.0); `IndexingHeader` length accessors get
relaxed Atomic variants used by runtime paths (JIT'd code untouched).

### 3.16 tinybloom (140) — relaxed-atomic
`TinyBloomFilter::add` (Structure transition-table insert) vs lock-free
`ruleOut` from concurrent transition lookup / `getConcurrently`.
Spec: OM §5 — lookup is lock-free with re-validation; bloom false positives
only cause the slow path. Torn u64 is UB though. Fix: `m_bits` -> relaxed
`Atomic<Bits>` (load/store/fetch_or).

### 3.17 rope-stringimpl (140) — relaxed-atomic
`JSRopeString::initializeFiber0/initializeIs8Bit/CompactFibers` vs concurrent
readers/resolvers; `resolveToBuffer` vs `unalignedLoad/copyElements` readers;
`StringImplShape::StringImplShape`/`hashAndFlags` (76 frames — concurrent
hash computation both sides write the same value); `JSString::length`.
Spec: OM GT — shared cells racy with re-dispatch; rope resolution publishes
`fiber0=nullptr` last. Fix shape: rope fiber words + the is8Bit/length fields
-> relaxed Atomic with release publication of the resolved buffer;
`StringImpl::m_hashAndFlags` -> relaxed Atomic RMW (idempotent same-value
hash store is then defined). Buffer byte copies (copyElements) racing equal
readers become defined once the publication is release/acquire.

### 3.18 regexp-shared (137) — real-bug
`RegExpCachedResult::record` self-pairs (106, vmstate/regexp-churn-threads.js
95x): N threads update the shared per-global `RegExpGlobalData` match result
(multi-word: lastRegExp + input + ovector) lock-free -> torn results, and
`Yarr::MatchingContextHolder` ctor/dtor pairs on shared VM matching-context
state (31).
Spec: nothing blesses this; UG §K's rule is GIL-serialized caches get real
locks; the AUD1.N2 per-lite ovector work covered the JIT side only.
Fix shape: make `RegExpGlobalData`/`RegExpCachedResult` per-thread (per-lite,
like the ovector reroute) OR guard record()/read accessors with the global
object's cell lock; `MatchingContextHolder` must target per-thread state.
(semantics/regexp-lastindex-shared.js is in the KNOWN-FAILING functional
queue — same root cause.)
WAVE-1 STATUS (amended after review): C++ consumers all re-pointed through
`threadRegExpGlobalData` (per-lite side table in JSGlobalObject.cpp, GC-rooted,
purged at lite teardown); `MatchingContextHolder` routes to the lite's
executingRegExp slot. JIT residual — RECLASSIFIED: the gilOff DFG/FTL inline
emission (RecordRegExpCachedResult, RegExpTestInline cached-result stores)
writing the SHARED stream is a MEMORY-SAFETY gap, not "bounded to stale legacy
statics": `RegExpCachedResult::record` is five plain stores, so a spawned
thread's inline record interleaving with the carrier's record()/lastResult()
cross-pairs (m_lastInput, m_result.start/end) and `leftContext()` then takes
jsSubstring with start > input length (OOB); TSAN can never see it (JIT code
uninstrumented), so it must be closed in code, not text. CLOSED wave 1 by
gating: DFGStrengthReductionPhase refuses foldToConstant() and
convertTestToTestInline() when gilOff (generic nodes lower to the re-pointed
operations — semantics preserved, no crash), and DFG (DFGSpeculativeJIT.cpp,
DFGSpeculativeJIT64.cpp) + FTL (FTLLowerDFGToB3.cpp) all fail-stop on the two
nodes gilOff, symmetric across tiers (the prior FTL-only fail-stop asymmetry
is gone). Remaining A16-ext jit slice is now PERF-ONLY (restore the inline
fast path against the lite-resident copy). Flag-off/GIL-on emission and
strength reduction are byte-identical.

### 3.19 alloc-profile (113) — relaxed-atomic
`ArrayAllocationProfile::updateProfile` self-pairs and
`CompactPointerTuple<JSArray*,u16>` setPointer/pointer/type races (+2
ObjectAllocationProfile keys). Advisory (a wrong prediction only mis-sizes an
allocation). Fix: profile word(s) -> single relaxed Atomic 64-bit
(CompactPointerTuple gains atomic word accessors used by the profile).

### 3.20 calllink (99) — concurrent-accessor
`capabilityLevel x noticeIncomingCall` (17), `lastSeenCallee x
setLastSeenCallee` (13), `PolymorphicCallStubRoutine` ctor vs
`edges/forEachDependentCell/hasEdges` and `x malloc` reuse pairs (40), vptr
race at `linkFor/linkPolymorphicCallImpl` (publish-before-init),
`repatchSlowPathCall` pairs, `DataOnlyCallLinkInfo::initialize`.
Spec: JIT §5.8 — fast path reads flow through ONE published `CallLinkRecord`;
writers publish a NEW record, never mutate; stale read = complete OLD record.
Fix shape: finish the §5.8 record protocol at the racy C++ consumers
(slow-path/GC reads of `m_callee`/`m_lastSeenCallee` -> relaxed Atomic
mirrors under existing locks); PolymorphicCallStubRoutine must be fully
constructed before the publish store (release), freed via §4.4 epoch.

### 3.21 gc-marking-residual (96) — relaxed-atomic
`GCSegmentedArray::append x postIncTop` / postIncTop self-pairs (75) — the
upstream lock-free mark-stack heuristics adjudicated in
Tools/tsan/suppressions.txt but surfacing with new stack shapes (donation vs
append) the narrow anchors don't cover; `IsoCellSet::add x addSlow` (8, =
staged B3, safety verified there); `BitSet<1024>` ctor pairs;
`AbstractSlotVisitor::visitCount x appendToMarkStack` (3, = staged B5).
Ruling per the suppressions-file Part A record: finish the header-side pass —
`m_top` -> relaxed Atomic in GCSegmentedArray, plus the four chartered
residuals (MarkingConstraint.h:63, SlotVisitor.h:166, Heap.h:414,
Threading.h:289 `m_gcThreadType` layout change). Not suppression candidates.

### 3.22 vm-string-caches (96) — real-bug
`WTF::String::String/swap` pairs inside `NumericStrings::addJSString`
(RefPtr::swap on the shared per-VM cache slot, write-write), `KeyAtomStringCache::make`,
`SmallStrings::initializeCommonStrings` double-init, `StringRecursionChecker`
shared visited-set.
Spec: UG §K — VM-level caches are NOT blessed racy; they need locks or
per-thread instances.
Fix shape: NumericStrings and KeyAtomStringCache become per-thread (per-lite)
caches (they are pure perf caches; per-thread is allocation-cheap and
lock-free), SmallStrings init under a once-flag before client #2 attaches,
StringRecursionChecker keys its set per-thread.
PER-COMPONENT CLOSURE STATUS (amended after review — the family is NOT one
fix; r1 must be diffed per component):
- NumericStrings — FIXED (pre-wave): per-thread routing via
  `VM::liveNumericStrings` (runtime/VM.h).
- KeyAtomStringCache — FIXED (pre-wave): Atomic slot snapshot protocol
  (runtime/KeyAtomStringCache.h).
- StringRecursionChecker — FIXED (wave 1): per-thread visited-set slots
  selected once in performCheck, dtor unregisters the same slots
  (runtime/StringRecursionChecker.h).
- SmallStrings "double-init" — RECLASSIFIED, NOT A RACE, no code change.
  Evidence (verified against the r0 corpus, 2026-06): every one of the 26
  `SmallStrings::initializeCommonStrings` frames in
  Tools/threads/tsan/reports-r0.log + Tools/threads/tsan/r0/*.log appears
  ONLY in the report's "Location is heap block ... allocated by" stack —
  never in either racing access stack. I.e. the racing cells (JSString
  header/fiber words read by value-profile prediction off the JIT worker:
  `cellHeaderConcurrentLoad`, `JSString::fiberConcurrently` — families 3.1/
  3.7/3.17) merely LIVE in a MarkedBlock that was allocated while the VM
  ctor initialized smallStrings; the classifier keyed on any frame in the
  report and misattributed them here. Structurally, double-init is
  unreachable: `initializeCommonStrings` has exactly one call site, the VM
  constructor (VM.cpp:479), which completes before the VM is published to
  any second client/lite under the VMLite model. If r1 ever shows
  initializeCommonStrings in an ACCESS stack (not the allocation stack),
  that is a new finding and reopens this row — do NOT bin it as fixed.

### 3.23 misc-tail (90) — suppress: NO (deferred re-triage; no entry added)
The stack-degraded remainder: `data race x malloc/aligned_alloc` (65) where
one access stack is empty (JIT frame side, or report interleaving in the
parallel log capture) and `WTF::CodePtr<> x malloc`, `free x malloc`,
`realpath`, `icu CharString::append` singles, Vector buffer
publication one-offs. Every sampled report's symbolized side lands in a
family above (allocator-reuse pairs of ic-stubinfo/calllink/watchpoints).
Ruling: NOT suppressed and NOT separately fixed this wave; re-run after wave
1 — the expectation is these disappear when their parent families' writers
become atomic / epoch-reclaimed. Any survivor gets first-class triage in r1.
(`realpath`/ICU timezone once-init folds into vm-shared-misc if it recurs.)

### 3.24 typedarray-sab (87) — concurrent-accessor
`CagedPtr` vector-pointer pairs (31+), `ArrayBufferContents::sizeInBytes` vs
`Atomic::store` growth (13+), `zeroFill/getData/fill` element-lane pairs,
`JSArrayBufferView` ctor vs `mode/lengthRaw/byteOffsetRaw` readers,
`ArrayBuffer::transferTo`.
Spec: SAB element races are JS-legal (OM §4.7 raw-lane analog: aligned lanes,
tear-free per lane); view metadata under growth follows the resizable-buffer
re-validation protocol (UG §A reads *Raw then validates).
Fix shape: element lanes accessed from C++ slow paths (`getData/setData`,
fill, zeroFill on shared buffers) go through relaxed atomic lane accessors;
`m_vector`(CagedPtr word), `m_length`, `m_mode`, `ArrayBufferContents::m_sizeInBytes`
-> relaxed Atomic with release publication on construction/growth/transfer.
Files: runtime/JSArrayBufferView.h, runtime/JSArrayBufferViewInlines.h,
runtime/JSGenericTypedArrayViewInlines.h,
runtime/JSGenericTypedArrayViewPrototypeFunctions.h, runtime/ArrayBuffer.h,
runtime/ArrayBuffer.cpp, runtime/TypedArrayAdaptersForwardDeclarations.h?
(adapters live in runtime/TypedArrayAdaptors.h — getData/setData).

### 3.25 property-table (81) — concurrent-accessor
`Structure::propertyTableOrNull x setPropertyTable` (43+10): the
`m_propertyTableUnsafe` WriteBarrier word read lock-free vs locked installs;
`PropertyTable::addAfterFind x size` (19): compiler `getConcurrently` size
read vs locked mutation; quarantine promote pairs (5+4).
Spec: OM §6/L6 — mutation under `Structure::m_lock` or table-private;
concurrent readers re-validate. Fix shape: `m_propertyTableUnsafe` accessors
-> relaxed Atomic (release on install); `PropertyTable` size/index header
words read by getConcurrently -> relaxed Atomic loads; mutation stays locked.

### 3.26 block-directory-bits (75) — relaxed-atomic
Exactly the staged-B1 family: `BlockDirectory::setIsAllocated` non-atomic RMW
under the directory's bitvector lock vs another client's lock-free
`MarkedBlock::Handle::isAllocated()` word view read. Adjudicated
rule-1-INELIGIBLE for suppression in Tools/tsan/suppressions.txt (N-mutator
interleaving impossible upstream) — the fix is code, not suppression:
`BlockDirectoryBits` segment words -> `Atomic<uint32_t>` relaxed loads +
relaxed RMW (writers keep the lock for multi-bit invariants).

### 3.27 gc-incoming-ref (59) — real-bug
`GCIncomingRefCounted<ArrayBuffer>::addIncomingReference` self/cross pairs,
Vector buffer realloc vs reader, one outright `heap-use-after-free`
(`VectorTypeOperations<JSCell*>::move x free`). N mutators append to an
ArrayBuffer's incoming-reference Vector concurrently; upstream assumed one
mutator. Nothing blesses this.
Fix shape: guard the incoming-reference Vector with a heap-rank leaf lock
(per-set lock in GCIncomingRefCountedSet; addReference/visit under it), or
CAS-based singly-linked list; the UAF disappears with the lock.
WAVE-1 STATUS (amended after review — the first cut was one-sided):
- Writers: addReference/sweep/lastChanceToFinalize serialize on the per-set
  leaf lock (GCIncomingRefCountedSet(Inlines).h); m_bytes is a relaxed Atomic
  advisory counter.
- READERS (the gap the review caught): `ArrayBuffer::notifyDetaching` and
  `ArrayBuffer::refreshAfterWasmMemoryGrow` walked
  numberOfIncomingReferences()/incomingReferenceAt() lock-free — exactly the
  r0 realloc-vs-reader / `VectorTypeOperations<JSCell*>::move x free` UAF
  signature, still reachable with writer-only locking. Both now snapshot the
  cell list under `Heap::arrayBufferIncomingReferencesLock()` (accessor on
  the owning set) and iterate the snapshot, because the lock is a strict
  leaf and `detachFromArrayBuffer` takes the view's cellLock (must not nest
  under it). Snapshot lifetime: the walking mutator is between safepoints so
  STW GC cannot retire the cells mid-walk — the same argument the old
  unlocked walk relied on. refreshAfterWasmMemoryGrow gained a VM& parameter
  to reach the heap lock (single caller:
  JSWebAssemblyMemory::growSuccessCallback). The invariant comment in
  GCIncomingRefCounted.h covers READS as well as mutation — recurring
  reports in this family are NOT pre-adjudicated; re-triage any new stack.
FLAG-OFF-IDENTITY WAIVER (explicit, per campaign rule): flag-off
(useJSThreads=false) these paths now execute an uncontended WTF::Lock
acquire/release (and the snapshot copy on detach/wasm-grow) that did not
exist upstream — the only wave-1 change not gated on gilOffWithProcessGate().
Accepted tradeoff, justification: (1) semantics unchanged (single mutator =>
lock always uncontended, snapshot order-equivalent); (2) every guarded path
is a cold slow path — Heap::addReference is reached only from
ArrayBuffer/DataView wrapper-construction slow paths
(JSArrayBufferView.cpp:193/201/333, JSArrayBuffer.cpp:46), sweep/
lastChanceToFinalize run once per GC end-phase, detach and wasm-grow are
rare events; (3) a gilOff-gated lock would leave reader/writer gating to
agree by convention and defeat WTF_GUARDED_BY_LOCK static checking — a
worse audit posture than an uncontended CAS on a cold path. V5b bench gate
(flag-off) must stay green over this wave; a regression attributable to
these paths voids the waiver and forces the mode-split.

### 3.28 barrier-counter (53) — relaxed-atomic
`Heap::addToRememberedSet` self-pairs are all `Heap.cpp:1433
m_barriersExecuted++` — a diagnostic counter incremented by every mutator's
barrier slow path. Fix: `m_barriersExecuted` -> relaxed Atomic add. (The
actual remembered-set append below it is already under `m_markListSet`
machinery / stopped-world; only the counter is racy.)

### 3.29 date-cache (51) — real-bug
`DateCache::msToGregorianDateTime/yearMonthDayFromDaysWithCache/DSTCache::probe/
parseDate/timeZoneCacheSlow` self-pairs + `DateInstance` cached
GregorianDateTime fill-in pairs. The per-VM DateCache (and per-instance
DateInstanceData) is mutated lock-free by N threads — multi-word caches, torn
results. UG §K: not blessed. (semantics/date-cache-churn.js is the queued
functional repro.)
Fix shape: per-thread DateCache (cheap, pure cache) or a DateCache lock
(leaf); `DateInstance::m_data` fill-in via CAS-once publication.

### 3.30 microtask-queue (41) — real-bug
`Interpreter::executeCallImpl` (job body) vs
`MicrotaskQueue::performMicrotaskCheckpoint<...VM::drainMicrotasks>` — the
carrier VM's default microtask queue dequeues/frees a job while a spawned
thread's enqueue/drain touches the same queue storage. UG §E.1 mandates
per-thread queues with cross-thread tickets (§E.4).
Fix shape: finish the §E.1 split — spawned threads must never touch the
carrier's default queue storage without the queue lock; enqueue from foreign
threads goes through the ticket router.

### 3.31 directarguments (37) — concurrent-accessor
`DirectArguments`/`GenericArgumentsImpl` ctor + `overrideThings` plain writes
vs DFG slow-path reads (`isMappedArgumentInDFG`, mapped-arguments queries),
`JSFunction/JSCallee` ctor vs `executable()/scopeUnchecked()` readers,
`FunctionRareData` init.
Spec: OM cell-publication ground truth — stale reads blessed, writers must be
word-atomic. Fix: publish-after-init (release store of the cell pointer is
the JS-value store, already relaxed once family 10 lands); the remaining
plain interior fields (`m_overrides` pointer, mapped-length words,
JSFunction::m_executable/m_scope/m_rareData) -> relaxed Atomic accessors.

### 3.32 symbol-registry (17) — real-bug
`WeakGCMap<SymbolImpl*,Symbol>::get/set` races + `Weak<Symbol>` swap/clear +
calloc reuse pairs: the per-VM symbol WeakGCMap mutated by N threads. UG §H
charters SymbolRegistry sharing with a lock; the JSC-side WeakGCMap cache was
missed. (semantics/symbol-registry-cross-thread.js is the queued functional
repro.)
Fix shape: guard `VM::symbolImplToSymbolMap` (and the privateName map) with a
lock (UG §LK leaf), or per-thread caches over the shared WTF registry.

### 3.33 vm-shared-misc (12) — lock
`VM::updateSoftReservedZoneSize` self-pairs (6, per-thread limits per UG §A —
must write the per-lite limit, not the shared VM field),
`ensureTerminationException` lazy init (§K once-init),
`LazyProperty/LazyClassStructure::callFunc` double-init (2, §K: lazy global
init must CAS/once), `ScratchBuffer::setActiveLength`, realpath/ICU timezone
once-init.
Fix shape: route soft-stack-limit updates per-lite (AB-17 machinery);
remaining VM lazy fields -> std::call_once/CAS publication per §K.

## 4. Wave-partitioning notes

- Families 1,2,3,8,19 (profiling) are file-disjoint from each other and from
  everything else — wave 1 candidates, highest count payoff (5024 reports).
- 5 (code-lifecycle) and 11 (codeblock-init) overlap on bytecode/CodeBlock.h —
  same wave bundle or serialize.
- 9 (structure-fields) and 25 (property-table) overlap on runtime/Structure* —
  serialize; 16 (tinybloom, heap/TinyBloomFilter.h) is disjoint.
- 4 (ic-stubinfo) and 20 (calllink) both name jit/Repatch.cpp in spirit —
  calllink's list omits it; if a calllink fix needs Repatch.cpp, serialize
  with ic-stubinfo.
- 10 (jsvalue-slots) touches runtime/WriteBarrier.h which many families
  include but none other WRITE — safe to parallelize.
- 23 (misc-tail) writes nothing in wave 1 by design.
- Re-run protocol: ONE build + ONE full corpus TSAN run per wave
  (Tools/threads/tsan/run-corpus-tsan.sh <outdir>), re-dedup, update this doc.

## 5. Known-suspect checklist (V7 report) — all found

RegExpCachedResult::record (3.18), TinyBloomFilter (3.16),
ArrayProfile/BinaryArithProfile/ArrayAllocationProfile (3.3/3.8/3.19),
WriteBarrierBase::get (3.10), JITCode/RawPtrTraits exchanges (3.5),
CallLinkRecord (3.20), PropertyTable exchange/addAfterFind (3.25),
StringImplShape::hashAndFlags (3.17), NumericStrings (3.22),
KeyAtomStringCache (3.22), BlockDirectoryBits (3.26),
cellHeaderConcurrentLoad pairs (3.7), Structure::setMaxOffset (3.9),
Heap::addToRememberedSet (3.28 — counter only), WatchpointSet::state (3.6).

## 6. Round 1 (r1) re-run — wave-1 verification (2026-06-09)

Same binary config as §0 (full JIT, asm LLInt, `ENABLE_C_LOOP=OFF`), rebuilt
after the wave-1 fixes (binary mtime 10:54 > newest source 10:51; `ninja -n
jsc` = no work to do). Same run config as §1. Raw output:
`Tools/threads/tsan/reports-r1.log` + `Tools/threads/tsan/r1/`; dedup keys in
`Tools/threads/tsan/families-r1.txt` (491 raw keys).

Result: **10642 reports** (10515 in r0 — run-to-run variance dominates; the
wave-1 families were only ~292 of r0's total). **Zero CLoop frames** again;
nothing suppressed this round; `unsuppressedCount = 10642`.

Wave 1 contained exactly three families (18 regexp-shared, 22
vm-string-caches, 27 gc-incoming-ref). **All three are at zero in r1** —
no residual stacks anywhere in the corpus:

- 3.18 regexp-shared: 137 -> **0** (no `RegExpCachedResult`/
  `MatchingContextHolder` frame in any r1 log). DONE. The A16-ext inline
  fast-path restore remains perf-only follow-up, not a TSAN item.
- 3.22 vm-string-caches: 96 -> **0**. The §3.22 caveat was checked
  explicitly: `SmallStrings::initializeCommonStrings` appears in **zero r1
  ACCESS stacks** (it only ever appeared in allocation-location stacks; the
  reclassification stands). NumericStrings/KeyAtomStringCache frames appear
  only as interior caller frames of other families' race sites. DONE.
- 3.27 gc-incoming-ref: 59 -> **0** (incl. the realloc-vs-reader UAF
  signature). The one report the classifier binned here is NOT this family —
  re-triaged as new family 35 below. DONE. Flag-off-identity waiver stands;
  V5b bench gate must still be checked by the wave-2 gate run.

### r1 family table

Counts after the same manual re-binning discipline as §2 (classifier output
adjusted by representative-stack inspection; the §2 adjustments were
re-applied identically: `incrementWithSaturation<u8>` pairs -> 4;
`Atomic<u8>` Structure `m_lock`-init keys (36) +
`VectorBufferBase<StructureID> x malloc` (32) + `clearCachedPrototypeChain`
(1) -> 9; `executeCallImpl` self/`executeProgram` pairs (156) + liveness
`BytecodeBasicBlock`/`FastBitVector` one-offs (7) -> 11;
`performMicrotaskCheckpoint`/`prepareForMicrotaskCall` pairs -> 30;
`setNeverInline` -> 2).

| # | id | r0 | r1 | ruling | delta notes |
|---|----|----|----|--------|-------------|
| 1 | value-profile | 1812 | 1873 | relaxed-atomic | unfixed; variance |
| 2 | exec-counter | 1553 | 1579 | relaxed-atomic | unfixed |
| 3 | array-profile | 1061 | 1096 | relaxed-atomic | unfixed |
| 4 | ic-stubinfo | 721 | 768 | lock | unfixed |
| 5 | code-lifecycle | 609 | 590 | concurrent-accessor | unfixed |
| 6 | watchpoints | 499 | 417 | relaxed-atomic | unfixed |
| 7 | cell-header | 486 | 481 | concurrent-accessor | unfixed |
| 8 | arith-profile | 485 | 500 | relaxed-atomic | unfixed |
| 9 | structure-fields | 416 | 420 | concurrent-accessor | unfixed |
| 10 | jsvalue-slots | 361 | 374 | relaxed-atomic | unfixed |
| 11 | codeblock-init | 316 | 325 | relaxed-atomic | unfixed |
| 12 | metaallocator-stats | 247 | 252 | relaxed-atomic | unfixed |
| 13 | linkbuffer-stats | 191 | 205 | relaxed-atomic | unfixed |
| 14 | simple-stats | 177 | 179 | relaxed-atomic | unfixed |
| 15 | butterfly-words | 157 | 160 | concurrent-accessor | unfixed |
| 16 | tinybloom | 140 | 161 | relaxed-atomic | unfixed |
| 17 | rope-stringimpl | 140 | 242 | relaxed-atomic | unfixed; variance (rope tests heavier this run) |
| 18 | regexp-shared | 137 | **0** | **done** | wave-1 fix verified |
| 19 | alloc-profile | 113 | 137 | relaxed-atomic | unfixed |
| 20 | calllink | 99 | 86 | concurrent-accessor | unfixed |
| 21 | gc-marking-residual | 96 | 132 | relaxed-atomic | unfixed |
| 22 | vm-string-caches | 96 | **0** | **done** | wave-1 + pre-wave fixes verified; SmallStrings caveat checked (0 access-stack hits) |
| 23 | misc-tail | 90 | 98 | suppress | still NOT suppressed; survivors re-triaged (below) — all allocator-reuse echoes of unfixed parents 4/6/7/9/20; re-check after those land |
| 24 | typedarray-sab | 87 | 115 | concurrent-accessor | unfixed |
| 25 | property-table | 81 | 117 | concurrent-accessor | unfixed |
| 26 | block-directory-bits | 75 | 127 | relaxed-atomic | unfixed |
| 27 | gc-incoming-ref | 59 | **0** | **done** | wave-1 fix verified (UAF signature gone); waiver stands pending V5b |
| 28 | barrier-counter | 53 | 65 | relaxed-atomic | unfixed |
| 29 | date-cache | 51 | 23 | real-bug | unfixed; variance |
| 30 | microtask-queue | 41 | 40 | real-bug | unfixed |
| 31 | directarguments | 37 | 29 | concurrent-accessor | unfixed |
| 32 | symbol-registry | 17 | 31 | real-bug | unfixed; semantics/symbol-registry-cross-thread.js hit its 420s timeout this round (KNOWN-FAILING; reports kept) |
| 33 | vm-shared-misc | 12 | 14 | lock | unfixed; evidence escalated (below) |
| 34 | structure-cache | (in 23) | 5 | real-bug | NEW — see 6.34 |
| 35 | arraybuffer-wrapper | (in 27 key) | 1 | real-bug | NEW — see 6.35 |

Total: 10642. unsuppressedCount = **10642**.

### 6.34 structure-cache (5) — real-bug (NEW)
`JSC::operator==(PrototypeKey)` / `GenericHashTraits<PrototypeKey>::
assignToEmpty` / `Weak<Structure>::impl x swap` keys: the per-global
`StructureCache` (`HashMap<PrototypeKey, Weak<Structure>>`) is looked up
(`inlineLookup` from `JSObject` structure materialization) while another
thread `inlineAdd`s — lock-free HashMap mutation by N threads, including
rehash -> UAF potential. In r0 these stacks were buried in the misc/heap
tail; first-class now per the §3.23 promise.
Spec: UG §K — per-global caches are NOT blessed racy; nothing in
SPEC-objectmodel covers a WTF::HashMap mutated concurrently.
Fix shape: guard `StructureCache::m_structures` with a leaf lock (it is a
pure cache; lock scope = lookup/insert only), or make the cache per-lite.
Files: runtime/StructureCache.h, runtime/StructureCache.cpp.

### 6.35 arraybuffer-wrapper (1) — real-bug (NEW)
`Weak<JSArrayBuffer>::impl` (`SimpleTypedArrayController::toJS`, spawned
thread reading `ArrayBuffer::m_wrapper`) vs `Weak<JSArrayBuffer>::swap`
(`registerWrapper` on the carrier): the per-ArrayBuffer wrapper Weak handle
is read/written lock-free by N threads when a shared ArrayBuffer is wrapped
in two globals. The classifier binned it under gc-incoming-ref (regex hit on
`JSArrayBuffer`); it is NOT that family — 3.27's lock covers the
incoming-reference Vector, not `m_wrapper`.
Spec: nothing blesses a raw `WeakImpl*` slot raced by N threads (UG §K
analog: per-VM wrapper cache).
Fix shape: `ArrayBuffer::m_wrapper` accesses via relaxed
`Atomic<WeakImpl*>`-style accessor with CAS publication in
`registerWrapper` (first-wins; loser drops its wrapper), or guard with the
view's existing cellLock. Files: runtime/ArrayBuffer.h,
runtime/SimpleTypedArrayController.cpp.

### 6.misc misc-tail r1 re-triage (98)
Symbolized-side census of the `x malloc`/`x aligned_alloc` survivors:
ICSlowPathCallFrameTracer (55, family 4 stub reuse),
WatchpointSet::state (33, family 6 stale-set lifetime via family 9),
cellHeaderConcurrentLoad (38, family 7 recycled cells),
StructureID Vector/StructureChain (36, family 9),
PolymorphicCallStubRoutine (18, family 20), JITCode::jitType (9, family 5).
Exactly the predicted composition: allocator-reuse echoes whose writers are
in families not yet fixed. No new mechanism. Still nothing suppressed;
re-census after waves land families 4/5/6/7/9/20 — any survivor THEN gets
its own family.

### 6.33-evidence vm-shared-misc escalation
`cve/mc-init-cloned-arguments-specials.js` newly hit exit 124 this round
(420s timeout; exit 66 in r0): after a `LazyProperty<JSGlobalObject,
GetterSetter>::getInitializedOnMainThread` race report (LazyProperty.h:95 —
exactly the §3.33 §K lazy-init gap), the process took a SEGV at null+8 on a
JS thread and TSAN's crash handler hung to the timeout. No wave-1 file is
in the stack; this is the un-fixed family 33 manifesting as a crash, not a
regression. Treat as priority evidence for the family-33 wave: lazy global
property init must CAS/once-publish.

### r1 run-health notes
- 224 files: 218 ran, 6 `//@ skip` (same set as r0).
- Timeouts (exit 124): semantics/proto-cycle-race.js (KNOWN-FAILING, also
  timed out in r0), semantics/symbol-registry-cross-thread.js
  (KNOWN-FAILING; ran to completion in r0 — flaky hang consistent with its
  queued functional bug), cve/mc-init-cloned-arguments-specials.js (SEGV +
  hung handler, see 6.33-evidence). All race reports from these runs are
  counted above.
- Wave-2 partitioning: unchanged from §4 — the profiling block (1,2,3,8,19;
  5185 reports) is still the highest-payoff disjoint bundle; 34/35 are tiny
  and file-disjoint from everything except 9 (StructureCache.cpp is not in
  family 9's list — safe to parallelize); 33 should bundle
  runtime/LazyPropertyInlines.h + VM.cpp and now carries crash evidence.

## 7. Wave-2 review amendments (adversarial findings, applied 2026-06-09)

Five reviewer findings against the wave-2 fixes were verified against the
tree; all five were REAL and are amended below. Re-run (r2) must be read
against these notes.

### 7.1 (§3.32 symbol-registry) — lock fixed the storage race, NOT the
canonicalization race; create path now first-wins

The WeakGCMapLocking::Yes leaf lock makes each map operation data-race-free,
so the family-32 stacks should hit zero in r2 — but `Symbol::create(VM&,
SymbolImpl&)` was a compound get -> allocate -> finishCreation(set) with no
atomicity across the lock: two threads racing `Symbol.for("x")` (same
SymbolImpl via the shared atom table + locked WTF SymbolRegistry) could both
miss and both set; last-set-wins left the FIRST thread holding a
non-canonical Symbol cell (s1 !== s2 while o[s1] === o[s2], since property
lookup keys on the uid). UG §H charters registry SHARING semantics, not just
storage integrity.

Amendment applied: `WeakGCMap::addIfAbsent` (first-wins publish; live
existing entry is canonical and returned, dead entries replaced; Weak
constructed outside the lock per §LK WS(i)); `Symbol::finishCreation` now
registers via addIfAbsent (never clobbers a canonical entry; identical
behavior for fresh unique uids); `Symbol::create(VM&, SymbolImpl&)` re-reads
after publish and returns the canonical cell, so a losing allocation simply
dies. Files: runtime/WeakGCMap.h, runtime/Symbol.cpp.

RESIDUAL / scope note: an r2 zero for family 32 closes the TSAN item only.
The queued functional repro semantics/symbol-registry-cross-thread.js
(KNOWN-FAILING) must be re-run against THIS amendment before its functional
bug is closed — it must NOT be closed against the lock alone. Local
evidence (2026-06-09, Debug GIL-off, full env): the test passed 5/5 after
this amendment (it previously failed/hung — r1 timed it out); promote it
out of KNOWN-FAILING only via the workflow's own gate run.

### 7.2 (§3.5 code-lifecycle) — IT-8 cross-slot residual: TSAN is now BLIND
to an admitted-real ARM64 race; recorded here per the campaign rule

`JITCodePointerConsumeOrder` (jit/JITCode.h) is acquire under TSAN_ENABLED,
relaxed in production. For single-load-then-address-dependent-deref shapes
(ConcurrentJITCodePtr::loadConsume, generatedJITCodeForCall) this is a sound
consume encoding: hardware (x86-64 TSO; ARM64 address dependencies) supplies
the ordering, the TSAN-only acquire just teaches the checker what the
hardware already guarantees, flag-off codegen identical.

WHAT IS SILENCED: ScriptExecutable::installCode's reader pair
(ExecutableBase::hasJITCodeFor gate load, THEN codeBlockFor load) is two
INDEPENDENT slots with NO address dependency between them. On ARM64,
load-load reordering can pair a fresh jit-code observation with a stale
CodeBlock read despite the writer-side storeStoreFence. On x86-64 (TSO,
loads not reordered with loads) the pair is sound — which is why the current
test fleet cannot hit it. Under TSAN both loads are acquire, so the checker
sees a full HB chain and CAN NEVER REPORT THIS AGAIN. This is an admitted-
real weak-memory race annotated into TSAN silence — structurally a
suppression, hence this entry.

Disposition: option (b) — production loads stay relaxed (no ldar on the hot
gate; flag-off and gilOff-x86 codegen unchanged). The residual goes on the
next-round (IT-8 follow-up) worklist with the OBJECT-MODEL PROTOCOL
AMPLIFIER as designated coverage (TSAN can no longer cover it, and JIT-side
coverage was already chartered to the amplifier per §0). What closes it:
either an acquire on the codeBlockFor-side read gated on
g_jscConfig.gilOffProcess (ARM64-only cost), or a refactor that derives the
CodeBlock pointer from the jit-code pointer itself (restoring an address
dependency). Until then this is x86-64-sound, ARM64-suspect.

### 7.3 (§3.30 microtask-queue) — JSGlobalObject enqueue fallback was missing
the foreign-inbox arm

VM::queueMicrotask had three gilOff arms (matching lite -> per-lite;
non-main-thread no-lite/foreign-lite -> enqueueFromForeignThread inbox; main
thread -> plain owner enqueue). JSGlobalObject::queueMicrotask /
queueMicrotaskSlow had only two: perLiteRealmRoutingLite() returns null for
the no-lite window AND for a foreign lite (lite->vm != &vm), and both fell
through to the owner's PLAIN Deque enqueue — exactly the corruption-grade
unsynchronized write racing the carrier's drain. r2 could have shown zero
only because the corpus doesn't drive engine-internal enqueues from
pre-carrier/cross-VM windows. Amendment applied: both JSGlobalObject paths
now mirror VM::queueMicrotask's guard — `vm.gilOff() && !WTF::isMainThread()`
in the null-lite fallback routes to microtaskQueue().enqueueFromForeignThread()
(lock-guarded inbox, release/acquire HB). Flag-off: branch not taken,
byte-identical. File: runtime/JSGlobalObject.cpp.

### 7.4 (§3.8 arith-profile + §3.13 linkbuffer-stats) — flag-off codegen
violation: relaxed atomic RMWs replaced with relaxed load+store

ArithProfileBits::operator|= used Atomic::exchangeOr(relaxed): even relaxed,
an atomic RMW is `lock or` on x86-64 (implicit full fence) and an exclusive/
LSE loop on ARM64 — NOT the plain load/or/store the field had before, on
UNCONDITIONAL flag-off arith slow paths (LLInt/baseline/DFG profiled
operationValue* — the same violation class as the ab17c/ab17e flag-off bench
regressions). Amended to relaxed load + relaxed store per the family
convention (mergeSpeculationConcurrently, ValueProfile, ArrayProfile,
ExecutionCounter); lost cross-thread bit-merges are §5.7.7-blessed, and the
JIT-emitted raw `or` into the same word already races un-atomically, so the
RMW bought no real guarantee. The false "concurrent setters cannot lose each
other's bits" comments were corrected. Same (colder) pattern in
LinkBuffer::performFinalization's s_profileCummulativeLinked{Sizes,Counts}
exchangeAdd -> relaxed load+store (dump-only stats). NOT changed:
AbstractMacroAssembler nextRandomSeed exchangeAdd (once per assembler;
replaces a genuine static-counter race and needs the RMW). Files:
bytecode/ArithProfile.h, assembler/LinkBuffer.cpp. GATE: V5b bench must be
re-run after this wave (was a hard requirement anyway per §6 r1 notes).

### 7.5 (§3.4 ic-stubinfo) — flag repack grows sizeof(PropertyInlineCache)
by ~8 bytes: ACKNOWLEDGED AND ACCEPTED

The nine `bool : 1` advisory flags + const `m_icType : 1` (10 bits / 2 bytes)
became nine ICRacyStateBool bytes + one m_icType byte (10 bytes): ~+8B per
IC, tens of KB on IC-heavy workloads. Verified safe: trailing members, no
OBJECT_OFFSETOF/JIT-emitted offset touches them, all layout static_asserts
are on earlier members, no sizeof-sensitive consumer in-tree. Disposition:
option (a) — accept and document (in-code comment added at ICRacyStateBool).
The minimal repack (m_icType alone moved out of the racy byte; nine mutable
flags co-packed in one Atomic<uint8_t> with relaxed load/store accessors —
cross-flag lost updates were already lost by the old bitfield RMWs) is
recorded as the fallback if the memory gate ever flags ICs. Flag note for
the bench/memory gate alongside 7.4's V5b run.

## 8. Round 2 (r2) re-run — wave-2 verification (2026-06-09)

Same binary config as §0 (full JIT, asm LLInt, `ENABLE_C_LOOP=OFF` verified in
CMakeCache). Build currency verified before the run: `ninja -n jsc` = "no work
to do", binary mtime (Jun 9 11:45) > newest source mtime (Jun 9 11:42 —
wave-2 + §7 amendment files). Same run config as §1. Raw output:
`Tools/threads/tsan/reports-r2.log` + `Tools/threads/tsan/r2/`; dedup keys in
`Tools/threads/tsan/families-r2.txt` (292 raw keys, down from 491).

Result: **3166 reports** (10642 in r1 — a 70% drop; wave 2 carried the
profiling block plus 11 more families). **Zero CLoop frames** (string absent
from the entire corpus output). Nothing suppressed this round;
`unsuppressedCount = 3166`.

### r2 family table

Counts after the same manual re-binning discipline as §2/§6, applied to the
classifier output (`classify.py` over `families-r2.txt`); the §6 adjustments
were re-applied identically and the new one-sided/ctor keys were binned by
representative-stack inspection (notes per family below; binning census in
§8.b).

| # | id | r1 | r2 | ruling | delta notes |
|---|----|----|----|--------|-------------|
| 1 | value-profile | 1873 | 109 | relaxed-atomic | partial — core landed; residual below |
| 2 | exec-counter | 1579 | 355 | relaxed-atomic | partial — ExecutionCounter fields landed; adjacent CodeBlock bytes remain |
| 3 | array-profile | 1096 | **0** | **done** | wave-2 fix verified |
| 4 | ic-stubinfo | 768 | 73 | lock | partial — reader-side gap + stub-reuse echoes |
| 5 | code-lifecycle | 590 | 114 | concurrent-accessor | partial — writer-side gap (replaceCodeBlockWith) |
| 6 | watchpoints | 417 | **0** | **done** | m_state/inflate word atomics verified; the 32 watchpoint-frame residuals are family 9's enumerator-cache install (re-binned) |
| 7 | cell-header | 481 | 32 | concurrent-accessor | partial — TypeInfoBlob ctor + WriteBarrierStructureID writer remain |
| 8 | arith-profile | 500 | **0** | **done** | incl. §7.4 load+store amendment |
| 9 | structure-fields | 420 | 366 | concurrent-accessor | FIX DID NOT CLOSE — ctor/publication gap; see 8.9 |
| 10 | jsvalue-slots | 374 | 15 | relaxed-atomic | partial — typed-barrier writers + sparse map remain |
| 11 | codeblock-init | 325 | 340 | relaxed-atomic | unfixed (not in wave 2) |
| 12 | metaallocator-stats | 252 | **0** | **done** | |
| 13 | linkbuffer-stats | 205 | **0** | **done** | incl. §7.4 |
| 14 | simple-stats | 179 | 179 | relaxed-atomic | unfixed |
| 15 | butterfly-words | 160 | 179 | concurrent-accessor | unfixed |
| 16 | tinybloom | 161 | 160 | relaxed-atomic | unfixed |
| 17 | rope-stringimpl | 242 | 255 | relaxed-atomic | unfixed |
| 18 | regexp-shared | 0 | 0 | done | holds |
| 19 | alloc-profile | 137 | 143 | relaxed-atomic | unfixed |
| 20 | calllink | 86 | 76 | concurrent-accessor | unfixed (+1 re-binned: MicrotaskCall::relink x linkIncomingCall incoming-call SentinelLinkedList) |
| 21 | gc-marking-residual | 132 | 135 | relaxed-atomic | unfixed (+1 re-binned: Thread::hasExited x registerJSThread — the chartered Threading.h byte-layout item) |
| 22 | vm-string-caches | 0 | 0 | done | holds |
| 23 | misc-tail | 98 | 122 | suppress | still NOT suppressed; §8.b census: allocator/stack-degraded echoes of unfixed parents 9/15/17/4/20 |
| 24 | typedarray-sab | 115 | 110 | concurrent-accessor | unfixed |
| 25 | property-table | 117 | 95 | concurrent-accessor | unfixed |
| 26 | block-directory-bits | 127 | 156 | relaxed-atomic | unfixed |
| 27 | gc-incoming-ref | 0 | 0 | done | holds; V5b waiver check still owed by the gate run |
| 28 | barrier-counter | 65 | 62 | relaxed-atomic | unfixed |
| 29 | date-cache | 23 | 1 | real-bug | nearly done — one residual field; see 8.29 |
| 30 | microtask-queue | 40 | 34 | real-bug | queue-storage races GONE; residual is one advisory VM byte; see 8.30 |
| 31 | directarguments | 29 | 32 | concurrent-accessor | unfixed |
| 32 | symbol-registry | 31 | **0** | **done** | lock + §7.1 addIfAbsent verified; functional repro promotion still separately gated (§7.1); the test no longer times out |
| 33 | vm-shared-misc | 14 | 13 | lock | unfixed; scope grows: IntlCache (8.33) |
| 34 | structure-cache | 5 | 6 | real-bug | unfixed; now WeakGCMap<PrototypeKey,Structure> get vs set/rehash |
| 35 | arraybuffer-wrapper | 1 | 0 | real-bug | ZERO BY VARIANCE — no fix landed; do NOT close |
| 36 | function-ctor-cache | (new) | 3 | real-bug | NEW — see 8.36 |
| 37 | regexp-compile-state | (new) | 1 | relaxed-atomic | NEW — see 8.37 |

Total: 3166. unsuppressedCount = **3166**.

### 8.b Re-binning census (r2)

- `('data race','WTF::Atomic<unsigned char/unsigned long/WatchpointState>::Atomic')`
  (23+12+11 = 46): all are the **Structure ctor** initializing now-atomic
  member fields via the `std::atomic` CONSTRUCTOR (a plain store, by the C++
  standard) racing concurrent readers of a just-published/recycled Structure
  (`getConcurrently` lock CAS, `trySetIndexQuicklyConcurrent` ->
  `InlineWatchpointSet::state`). Binned to family 9 — same root cause as its
  ctor pairs.
- classifier `watchpoints` 32: every key is the enumerator-cache /
  rare-data install (`Box<InlineWatchpointSet>::isValid x
  FixedVector<StructureChainInvalidationWatchpoint>::FixedVector` 23,
  `ensurePropertyReplacementWatchpointSet x FixedVector` 4,
  `StructureRareData ctor x incrementActiveReplacementWatchpointSet` 4, Box
  reassign 1) — §3.9's AUD1.N4(3) item, NOT WatchpointSet state. Binned to 9.
- classifier `interp-exec` 191: `executeCallImpl` self pairs (158+1) are the
  §6-precedent CodeBlock advisory byte (size-1 write at the same address as
  the `operationOptimize` self pairs) -> family 11; the
  `x performMicrotaskCheckpoint` pairs (31+1) -> family 30.
- `slow_path_to_this` self pairs (3): to_this bytecode metadata
  (cached-structure word) -> family 11.
- `operationOptimize` self (193) + `shouldOptimizeNowFromBaseline` (43) +
  `setupWithUnlinkedBaselineCode x jitCompileAndSetHeuristics` (66) +
  `osrExitCounter` echoes (17) + FTL `countEntryFailure` (1) -> family 2.
- `JSC::operator==(PrototypeKey) x calloc` (4) -> family 34 (lookup walking a
  freed table).
- `GetterSetter ctor x isGetter/SetterNull` (2): plain
  `WriteBarrierBase<JSObject>::setWithoutWriteBarrier` (std::exchange) vs
  SparseArrayEntry reader -> family 10 (typed-barrier writer gap).
- `IntlCache::canonicalizeUnicodeLocaleID` find-vs-rehash (2) -> family 33.
- `Weak<FunctionExecutable>` swap/clear/impl keys (3) -> NEW family 36;
  `WeakGCMap<PrototypeKey,Structure>` get x set / get x calloc (2) -> family
  34. (These five were the classifier's `symbol-registry` bin; family 32
  itself is at zero.)
- `SourceCode`/`UnlinkedSourceCode`/`FunctionExecutable` ctor-vs-reader
  singles (5) + `ConcurrentJITCodePtr` ctor (1) -> family 5 publication
  residuals.
- `('data race','malloc'/'aligned_alloc'/'realloc')` (117) + heap-misc
  leftovers (5) -> family 23. Symbolized-anchor census of the one-sided
  reports: Structure::Structure 81, taggedButterflyWord 52,
  JSRopeString::initializeFiber0/Is8Bit 57, WTF::String/StringImpl 64+,
  considerRepatchingCacheImpl 15, PolymorphicCallStubRoutine 18 — exactly the
  unfixed parents 9/15/17/4/20. No new mechanism; nothing suppressed.

### 8.9 structure-fields — wave-2 fix did NOT close the family (366)

What landed: per-field relaxed atomics (maxOffset etc.), rare-data CAS reads.
What the residual stacks show: the dominant keys are the **constructor
itself** racing readers — 83 one-sided `Structure::Structure`, 54
`Structure::Structure x classInfoForCells`, 29 `x typeInfo`, 15
`allocateRareData`, plus the 46 atomic-field-ctor keys (8.b). I.e. the §3.9
fix shape's FIRST clause ("new Structures fully initialized before the
StructureID/transition-table publish — add the missing release on the publish
store") is still missing or incomplete: readers reach a Structure whose ctor
is mid-flight, so even atomicized fields race their constructor init (the
`std::atomic` ctor is a plain store; ctor must use relaxed stores, or — the
real fix — publication must be release-ordered after ctor completion).
fixShape updated: (1) release-store publication of new Structures at the
StructureID/transition-table/cell-header publish point, after full ctor; (2)
ctor init of atomic members via relaxed store (not the atomic ctor) for
recycled-cell readers; (3) the enumerator-cache StructureChainInvalidation
FixedVector rebuild still needs the Structure lock + GC-deferred retire
(unchanged from §3.9, now 32 reports). Files unchanged (runtime/Structure.h,
Structure.cpp, StructureRareData.h/.cpp, StructureChain.cpp) + the publish
site in runtime/StructureID/JSCell header path if distinct.

### 8.1/8.2/8.4/8.5/8.7/8.10 partial-fix residuals (fixShape updates)

- **1 value-profile (109)**: buckets/m_prediction landed. Residual:
  `computeUpdatedPredictionForExtraValue` writes the by-ref extra-value
  JSValue slot (plain 8-byte RMW raced between two DFG ByteCodeParser
  threads via `valueProfilePredictionForBytecodeIndex`), and
  `iteratorOpen/NextTryFastImpl` metadata profile stores. Fix: extra-value
  slot -> relaxed Atomic<EncodedJSValue> (same as buckets); iterator fast
  impl metadata stores -> relaxed.
- **2 exec-counter (355)**: ExecutionCounter fields landed (the 1227-report
  key is gone). Residual: a size-1 CodeBlock byte written by every
  `operationOptimize` (optimization-delay/should-always-be-inlined-style
  advisory byte, 193), a size-2 field read+written inside
  `shouldOptimizeNowFromBaseline` (43), `setupWithUnlinkedBaselineCode x
  jitCompileAndSetHeuristics` install pair (66), `osrExitCounter x malloc`
  echoes (17), `FTL::ForOSREntryJITCode::countEntryFailure` (1). Fix: the
  remaining CodeBlock advisory bytes/shorts -> relaxed Atomic (same files).
- **4 ic-stubinfo (73)**: writers atomic. Residual is READER-side: baseline
  JIT compiler thread reads LLInt `GetByIdModeMetadata` plain in
  `emit_op_get_by_id` while LLInt slow path atomically rewrites it (35);
  same for resolve_scope metadata (6); `considerRepatchingCacheImpl`/
  `ICSlowPathCallFrameTracer` x malloc stub-reuse echoes (22) — the §4.4
  epoch reclamation for retired stubs is still the open item. Fix: relaxed
  atomic loads on the compiler-side metadata snapshot; finish §4.4 epoch.
- **5 code-lifecycle (114)**: reader side landed (`Atomic<CodeBlock*>::load`
  now visible in stacks). Residual is the WRITER:
  `FunctionExecutable::replaceCodeBlockWith` still stores via
  `WriteBarrierBase<CodeBlock>::setEarlyValue` -> `RawPtrTraits::exchange`
  (plain `std::exchange`, 95 reports). Fix: route that store through the
  same release/atomic accessor the readers use. Plus ctor-publication
  singles (SourceCode/UnlinkedSourceCode/FunctionExecutable ctor,
  ConcurrentJITCodePtr ctor) — covered by the family-9-style release
  publication of executables.
- **7 cell-header (32)**: accessors landed. Residual:
  `TypeInfoBlob::Data::Data()` default ctor (plain init inside the Structure
  ctor — closes with 8.9's publication fix) and
  `WriteBarrierStructureID::setWithoutWriteBarrier` plain store (4). Fix:
  relaxed store in both.
- **10 jsvalue-slots (15)**: `WriteBarrierBase<Unknown>` landed. Residual:
  TYPED `WriteBarrierBase<T>::setWithoutWriteBarrier` writers (std::exchange
  — GetterSetter getter/setter slots), `SparseArrayValueMap`
  entry/descriptor pairs, `tryGetIndexQuicklyConcurrent x
  trySetIndexQuicklyConcurrent`. Fix: typed barrier store -> relaxed Atomic
  word store (flag-off codegen identical); sparse-map entry words through
  the same accessor.

### 8.29 date-cache (1) — one residual field

DateCache/DSTCache/DateInstanceData multi-word cache races are gone. The one
survivor: `DateInstance::m_internalNumber` plain double — `setTime` writer vs
`gregorianDateTimeUTC` reader. Word-sized value; fix is a relaxed
Atomic<double>/64-bit word accessor (OM value-word analog), runtime/DateInstance.h.
Family stays real-bug until that lands and the KNOWN-FAILING functional repro
(semantics/date-cache-churn.js) is re-gated.

### 8.30 microtask-queue (34) — queue storage closed; one advisory byte left

No r2 stack touches Deque/queue storage anymore (the §3.30 + §7.3 work
verified — including the foreign-inbox arms). All 34 reports are ONE size-1
field: a per-VM byte written by `Interpreter::executeCallImpl`'s ScopeExit on
every JS call and by `performMicrotaskCheckpoint` (drain bookkeeping —
same address both sides, write-write). This is corruption-immune
(single-byte, advisory) but UB as written. Fix: identify the byte in
executeCallImpl's exit lambda (VM entry/drain bookkeeping flag in
runtime/VM.h / interpreter/Interpreter.cpp) and make it a relaxed
Atomic<uint8_t> — or per-lite if it is logically per-thread state (UG §E.1
spirit). Ruling stays real-bug until identified precisely.

### 8.33 vm-shared-misc (13) — IntlCache added to scope

New evidence: `IntlCache::canonicalizeUnicodeLocaleID` per-VM
HashMap<String,String> `find` racing `rehash` (2 reports) — same UAF-grade
shape as family 34. Folded here per the §3.23 precedent (ICU/once-init).
Files for the wave: + runtime/IntlCache.h, runtime/IntlCache.cpp (leaf lock
or per-lite cache). The §6.33 LazyProperty crash evidence stands:
cve/mc-init-cloned-arguments-specials.js hit exit 124 AGAIN this round.

### 8.34 structure-cache (6) — unfixed; shape update

The per-global StructureCache is now backed by `WeakGCMap<PrototypeKey,
Structure>` but is NOT using the locked WeakGCMap configuration: r2 shows
lock-free `get` vs `set`-rehash and `get`/`operator==(PrototypeKey)` walking
a freed (calloc-reused) table from `StructureCache::createEmptyStructure`.
Fix unchanged (leaf lock — reuse the WeakGCMapLocking::Yes machinery that
closed family 32 — or per-lite cache). Files: runtime/StructureCache.h,
runtime/StructureCache.cpp.

### 8.36 function-ctor-cache (3) — real-bug (NEW)

`JSGlobalObject::cachedFunctionExecutableForFunctionConstructor` /
`tryGetCachedFunctionExecutableForFunctionConstructor`: the per-global
Function-constructor source cache (`Weak<FunctionExecutable>` slot(s)) is
written (`Weak::set` -> swap + WeakImpl::clear) by one thread while another
reads `Weak::get`/`impl` lock-free — N threads calling `new Function(...)`
on the same global. UG §K analog: per-global pure cache, not blessed racy;
a torn WeakImpl* read is a UAF. Fix shape: guard the slot with a leaf lock
or per-lite slot; first-wins publish like §7.1's addIfAbsent. Files:
runtime/JSGlobalObject.h, runtime/JSGlobalObject.cpp,
runtime/FunctionExecutable.cpp (fromGlobalCode caller). Test:
jit/int-gate-stop-budget.js.

### 8.37 regexp-compile-state (1) — relaxed-atomic (NEW)

`RegExp::compileHoldingCellLock` writes `m_specificPattern` (byte) under the
cell lock; `replaceUsingRegExpSearch` reads `specificPattern()` lock-free.
Distinct mechanism from family 18 (cached match results — still zero). The
field is a monotonic compile-state datum; stale read just takes the generic
path. Fix: `m_specificPattern` (and any sibling compile-state bytes read
off-lock) -> relaxed Atomic load/store. Files: runtime/RegExp.h (+ the
read site in runtime/StringPrototype.cpp). NOT a lock candidate — writer
already holds the cell lock; only the lock-free reader needs a defined load.

### r2 run-health notes

- 224 files: 218 ran, 6 `//@ skip` (same set).
- Timeouts (exit 124): semantics/proto-cycle-race.js (KNOWN-FAILING, every
  round), cve/mc-init-cloned-arguments-specials.js (REPEAT of r1's
  family-33 crash signature — see 8.33).
  semantics/symbol-registry-cross-thread.js ran to completion this round
  (consistent with §7.1).
- Aborts (exit 134, 6): cve/mc-hand-restrict-claim.js, heap-iss-revert.js,
  gc-stress/havebadtime-vs-indexed-fastpath.js (KNOWN-FAILING) as in r1;
  NEW this round: cve/mc-int-resizable-tail-quarantine.js,
  cve/mc-life-sab-refchurn.js, semantics/frozen-seal-race.js. Functional
  failures are out of campaign scope (their race reports ARE counted), but
  the three new aborts should be triaged by the functional-bug round.
- exit 3 (JS failure, no crash): 14 tests (9 in r1); new entrants include
  bench/megamorphic-access.js, bench/transition-heavy-constructor.js,
  cve/mc-df-wasm-compile-race.js, cve/mc-wait-property-wait-lost-wakeup.js,
  sync/condition-wait-notify.js — same note as above.
- Wave-3 partitioning: family 9 is now the single biggest item (366) and its
  fix is structural (publication), overlapping 7's TypeInfoBlob residual and
  5's executable-publication singles — bundle 9+7 (and the family-5 writer
  fix is file-disjoint: ScriptExecutable.cpp/FunctionExecutable.h). The
  untouched block 11/14/15/16/17/19/24/25/26/28/31 remains file-disjoint
  per §4. 33+34+36 are small lock items; 36 touches JSGlobalObject.* which
  no other family writes this wave. 23 stays write-nothing.

## 9. Wave-3 review amendments (adversarial findings, applied 2026-06-09)

### 9.1 cell-header / structure-fields / TypeInfoBlob — TSAN-only helpers
split; the retained TSAN-only class is now a RECORDED structural suppression

Finding (accepted in part): three wave-3 concurrent-accessor families were
closed with helpers that were relaxed atomics ONLY under `TSAN_ENABLED`
(Structure::tsanRelaxedLoad/Store, TypeInfoBlob::relaxedLoad/Store,
cellHeaderConcurrentLoad/Store), deviating from the campaign convention
(unconditional WTF-style relaxed atomics, as this same wave used in
WriteBarrier.h, JSString.h, DateInstance.h, IndexingHeader.h,
TinyBloomFilter.h, ValueProfile.h) with no triage-doc entry for the class.

Applied (code):

- **Unconditional relaxed atomics now** (single-word; per-access codegen is
  the identical mov flag-off — no coalescible bulk sequence at these sites):
  - Structure accessor sites via the new
    `Structure::concurrentRelaxedLoad/Store`: `typeInfo()`'s
    m_outOfLineTypeFlags load, `classInfoForCells()`,
    `transitionThreadLocalTID()`, `propertyHash()`, `variant()`,
    `transitionPropertyAttributes()` get+set (runtime/Structure.h).
  - All TypeInfoBlob post-construction accessors:
    `indexingModeIncludingHistory()`/`set...`, `type()`,
    `inlineTypeFlags()`, `defaultCellState()`, `blob()`, and
    `fencedIndexingModeIncludingHistory()` (now always
    relaxed-load + `Dependency::fence`, same single-load codegen)
    (runtime/TypeInfoBlob.h).
  - `cellHeaderConcurrentStore` — every NON-constructor header writer:
    `setStructure`, `setStructureIDDirectly`, `clearStructure`,
    `setCellState`, early-cell init (runtime/JSCell.h, JSCellInlines.h).
  This removes the mixed atomic-reader/plain-writer UB pairs at
  accessor-vs-accessor sites in production GIL-off builds.

- **Retained TSAN-only** (THE recorded class — per the §7.2 precedent this
  is structurally a suppression and is hereby justified):
  1. Constructor member-init bulk sequences: Structure ctors
     (Structure.cpp, via `tsanRelaxedLoad/Store`), TypeInfoBlob ctors and
     `TypeInfoBlob::operator=` (refuted reviewer detail: operator= is NOT an
     accessor site — its only two uses are Structure ctor tails,
     Structure.cpp:424/531), and the JSCell builtin-cell ctor's TSAN-only
     64-bit header-assembly path (JSCellInlines.h). Justification: ctor
     store coalescing is bench-sensitive (ITEM-2); unconditional atomics
     would forbid the compiler from merging the init-list stores on the
     hottest allocation/transition paths. **Gate: the V5b bench rung holds
     this justification — re-cite the wave-3/r3 V5b run next to this entry;
     ab17c/ab17e history shows ctor/transition-path flag-off regressions
     are real, which is exactly why the gating exists.**
  2. `cellHeaderConcurrentLoad` (JSCell header-byte reads: `type()`,
     `structureID()`, `structure()`, `cellState()`, `inlineTypeFlags()`,
     `indexingTypeAndMisc()`): hottest loads in the engine, executed
     flag-off on every property access/type check; unconditional atomics
     are the same mov per access but pin the optimizer (no CSE/merging of
     adjacent header-byte loads). The reader side is the OM §3.0/GT#2
     BLESSED half of the header protocol.

  Production-UB acceptance argument for the retained class: all fields are
  word-sized and hardware-atomic on every shipped target; ordering is
  provided by the constructor-tail storeStoreFence (Structure.cpp) plus the
  family-9 release-publication item; the OM ground truth blesses stale
  reads (re-dispatch). Permanent TSAN blindness: both sides of these pairs
  are atomic under TSAN, so a FUTURE genuinely-unordered writer through
  these helpers will never be reported — same class as §7.2.
  **Designated alternate coverage: the object-model protocol tests + race
  amplifier (JSTests/threads corpus, V3/V4 rungs), not TSAN.** Any new
  header/Structure-scalar writer added outside these helpers will still be
  reported (it would pair plain-vs-atomic under TSAN).

- Refuted reviewer detail (asymmetry claim): the `DEFINE_BITFIELD`
  atomicLoad-reader vs plain-writer pair is NOT a production flag-on race —
  flag-on writers route through `setBitFieldConcurrently` (CAS); the plain
  RMW branch executes only flag-off (single mutator).

### 9.2 ic-stubinfo (family 4) — status: PARTIAL, not closed

- Refuted clause: "no stub-lifecycle file was touched this wave" is wrong —
  wave-3 DID land §4.4 epoch routing on the last inline-free path:
  `PropertyInlineCache::deref()` now takes an extra Ref on the installed
  chain(s) (including the inlined unit handler) at jettison/teardown and
  routes them through `RetiredJITArtifacts::retireHandlerChain`
  (bytecode/PropertyInlineCache.cpp); the displacement sites
  (`initializeWithUnitHandler`, `prependHandler` head publish,
  `resetStubAsJumpInAccess`) already retire displaced chains through the
  epoch. Machine code continues to ride the jettisoned-stub-routine
  machinery and R2's conservative scan (I7).
- Accepted in substance: the family is recorded PARTIAL. Acceptance test
  (binding for r3): ZERO `considerRepatchingCacheImpl` /
  `ICSlowPathCallFrameTracer` 'x malloc' stub-reuse pairs, reached via
  deferred reclamation — NOT via reclassification. r3 triage rule: any
  surviving x-malloc pair with these anchors MUST be binned to family 4
  (binning them into misc-tail is forbidden). If any survive r3, the
  residual epoch work (audit every free of retired handler-chain/stub data;
  all must ride RetiredJITArtifacts or GC-deferred free) becomes its own
  wave item owning bytecode/PropertyInlineCache.*,
  bytecode/RetiredJITArtifacts.*, jit/GCAwareJITStubRoutine.*.
- The reader-side metadata snapshot
  (`GetByIdModeMetadata::loadModeConcurrently` + the JITPropertyAccess.cpp
  consumer) stands as sound per JIT §5.7 racy-profiling tolerance (one
  relaxed 1-byte mode load; a stale mode only changes the initial cache
  shape).

## 10. Round 3 (r3) re-run — wave-3 verification (2026-06-09)

Same binary config as §0 (full JIT, asm LLInt, `ENABLE_C_LOOP=OFF` verified in
CMakeCache). Build currency verified before the run: `ninja -n jsc` = "no work
to do", binary mtime (Jun 9 12:23) > newest source mtime (Jun 9 12:21 —
wave-3 + §9 amendment files: JSCell.h, Structure.h, TypeInfoBlob.h). Same run
config as §1. Raw output: `Tools/threads/tsan/reports-r3.log` +
`Tools/threads/tsan/r3/`; dedup keys in `Tools/threads/tsan/families-r3.txt`
(223 raw keys, down from 292).

Result: **1908 reports** (3166 in r2 — a 40% drop). **Zero CLoop frames**
(string absent from the entire corpus output). Nothing suppressed this round;
`unsuppressedCount = 1908`.

### Headline finding: the §8.30 "advisory byte" is CodeBlock::m_shouldAlwaysBeInlined

The size-1 write-write field behind FOUR previously separate bins is now
precisely identified: `CodeBlock::m_shouldAlwaysBeInlined` (bytecode/
CodeBlock.h:893 — "Not a bitfield because the JIT wants to store to it").
Plain C++ writers: `operationOptimize` (JITOperations.cpp:3094, 171 reports +
read at :3133, 10), `Interpreter::executeCallImpl` (Interpreter.cpp:1302, 96),
`Interpreter::prepareForMicrotaskCall` (Interpreter.cpp:1445/1463, 1),
`CodeBlock::noticeIncomingCall`, plus the executeProgram/construct arms
(:1403). One fix — relaxed `Atomic<bool>` accessors on the C++ side
(offsetOfShouldAlwaysBeInlined and JIT-emitted plain stores unchanged; JIT
side is the documented §0 blindness tradeoff) — closes 278+ reports.
BINNING AMENDMENT: all m_shouldAlwaysBeInlined pairs are now binned to
family 2 (exec-counter, JIT §5.7.7 advisory tier-up datum). This re-bins
r2's `executeCallImpl` self pairs OUT of family 11 (§8.b) and closes
family 30's residual (§8.30 — the byte was never per-VM drain bookkeeping;
the microtask-side stack was just prepareForMicrotaskCall writing this same
CodeBlock byte). Family 30's queue storage was already clean in r2; family
30 is now **done**.

### r3 family table

Counts after the same manual re-binning discipline as §2/§6/§8 (census in
§10.b). The §9.2 binding acceptance test was applied: surviving x-malloc
stub-reuse pairs are binned to family 4, NOT misc-tail.

| # | id | r2 | r3 | ruling | delta notes |
|---|----|----|----|--------|-------------|
| 1 | value-profile | 109 | 104 | relaxed-atomic | UNCHANGED — §8.1 residual fix did not land: iterator fast-impl metadata (iteratorOpen/NextTryFastImpl 45), LazyValueProfileHolder ctor-vs-computeUpdatedPredictions (8), extra-value slot |
| 2 | exec-counter | 355 | 429 | relaxed-atomic | count UP from re-binning (m_shouldAlwaysBeInlined 278 moved in from 11/30); underlying residuals unchanged: shouldOptimizeNowFromBaseline size-2 (36), setupWithUnlinkedBaselineCode x jitCompileAndSetHeuristics (66), osrExitCounter (15), DFG setOptimizationThresholdBasedOnCompilationResult x shouldTriggerFTLCompile (8), FTL countEntryFailure (1) |
| 3 | array-profile | 0 | 0 | done | holds |
| 4 | ic-stubinfo | 73 | 57 | lock | PARTIAL; §9.2 ACCEPTANCE TEST FAILED — 16 x-malloc stub-reuse pairs survive (considerRepatchingCacheImpl 8, ICSlowPathCallFrameTracer 8); see 10.4 |
| 5 | code-lifecycle | 114 | 15 | concurrent-accessor | writer fix (replaceCodeBlockWith) verified — the 95-report key is gone; residuals: DFG::compileImpl self (6), capabilityLevelState x installCode (3), hasInstalledVMTrapsBreakpoints x invalidateLinkedCode (2), SourceCode/UnlinkedSourceCode/ConcurrentJITCodePtr ctor publication singles (4) |
| 6 | watchpoints | 0 | 0 | done | holds (the 1 Box<InlineWatchpointSet> key is family 9's enumerator-cache item, re-binned per §8.b) |
| 7 | cell-header | 32 | 1 | concurrent-accessor | TypeInfoBlob ctor closed by §9.1; the one survivor is the §8.7-NAMED residual the wave missed: `WriteBarrierStructureID::setWithoutWriteBarrier` plain store (runtime/StructureID.h) — one-line relaxed store |
| 8 | arith-profile | 0 | 0 | done | holds |
| 9 | structure-fields | 366 | 182 | concurrent-accessor | PARTIAL — §9.1 helpers landed for named accessor/ctor sites, but ctor init-list members of WTF::Atomic type still initialize via the Atomic CONSTRUCTOR (plain store): TinyBloomFilter copy at Structure.cpp:440 (70), m_lock at :438 (18), InlineWatchpointSet state at :442 (9); plus one-sided Structure::Structure (18), setPropertyTable writer (26), prototypeChain (8), setPreviousID (5), enumerator-cache Box/FixedVector (5+). See 10.9 |
| 10 | jsvalue-slots | 15 | 10 | relaxed-atomic | ~unchanged — §8.10 residuals persist: tryGetIndexQuicklyConcurrent x trySetIndexQuicklyConcurrent (6), Register pairs (2), SparseArrayValueMap (2) |
| 11 | codeblock-init | 340 | 153 | relaxed-atomic | down via re-binning (m_shouldAlwaysBeInlined out) + partial fix; residuals: UnlinkedFunctionExecutable::setSingletonHasBeenInvalidated self/x-reader (58), BaselineJITCode::set{Liveness,Fullness}Rate (72), slow_path_to_this (2), liveness FastBitVector/BytecodeBasicBlock one-offs (4) |
| 12 | metaallocator-stats | 0 | 0 | done | holds |
| 13 | linkbuffer-stats | 0 | 0 | done | holds |
| 14 | simple-stats | 179 | **0** | **done** | wave-3 fix verified |
| 15 | butterfly-words | 179 | 61 | concurrent-accessor | partial: residual writers createInitialIndexedStorageConcurrent (20), ensureLengthSlowConcurrent (7), FreeCell::makeLast/setNext (10), trySegmentedTransition (1), + allocator-pair echoes |
| 16 | tinybloom | 160 | **0** | **done** | reader/add relaxed atomics verified (`ruleOut` now loads atomically in stacks); the ctor-copy side is family 9's item |
| 17 | rope-stringimpl | 255 | 129 | relaxed-atomic | partial: one-sided String::String rope-resolution writes (64), unalignedLoad/copyElements pairs (15), StringImpl::length x malloc (7), String::impl (4) — resolution-publish and hash/flags writers still plain |
| 18 | regexp-shared | 0 | 0 | done | holds |
| 19 | alloc-profile | 143 | **0** | **done** | wave-3 fix verified |
| 20 | calllink | 76 | 40 | concurrent-accessor | partial: capabilityLevel x noticeIncomingCall (12), setLastSeenCallee (7), noticeIncomingCall x installCode/operationOptimize (12), CallLinkInfo ctor (5), MicrotaskCall::relink x linkIncomingCall (1) |
| 21 | gc-marking-residual | 135 | 151 | relaxed-atomic | FIX DID NOT LAND/WORK — GCSegmentedArray postIncTop/append pairs (110), BitSet ctor (16), IsoCellSet add/addSlow (14), BlockDirectory next-directory list (2), ParallelHelperClient (1) |
| 22 | vm-string-caches | 0 | 0 | done | holds |
| 23 | misc-tail | 122 | 144 | suppress | still NOT suppressed; §10.b census: pure allocator-side echoes (malloc 75, aligned_alloc 52, realloc 3) + degraded singles (5) + heap-misc leftovers (9); parents = unfixed 9/15/17/21/26 |
| 24 | typedarray-sab | 110 | 99 | concurrent-accessor | fix did not close: CagedPtr pairs (29), ArrayBufferContents::sizeInBytes (17), zeroFill x getData (9), canGetIndexQuicklyForTypedArray (3+), fill/detach arms |
| 25 | property-table | 95 | 88 | concurrent-accessor | partial — reader side now `Atomic<PropertyTable*>::load` in stacks; WRITER `Structure::setPropertyTable` still plain (62), addAfterFind x size (15), quarantine counters (6) |
| 26 | block-directory-bits | 156 | 144 | relaxed-atomic | FIX DID NOT CLOSE — single key: BlockDirectoryBitVectorWordView reader x `FastBitReference::operator=` plain writer; the write side (WTF FastBitReference word RMW) was never converted |
| 27 | gc-incoming-ref | 0 | 0 | done | holds |
| 28 | barrier-counter | 62 | 64 | relaxed-atomic | unfixed (Heap::addToRememberedSet self pairs) |
| 29 | date-cache | 1 | **0** | **done (TSAN)** | m_internalNumber relaxed-atomic verified in DateInstance.h; KNOWN-FAILING functional repro still separately gated |
| 30 | microtask-queue | 34 | **0** | **done** | residual byte identified as CodeBlock::m_shouldAlwaysBeInlined -> family 2 (see headline); queue storage clean since r2 |
| 31 | directarguments | 32 | 23 | concurrent-accessor | unfixed: DirectArguments ctor x isMappedArgumentInDFG (6), modifiedArgumentDescriptor (4), overrideThings (4), JSFunction ctor x executable (3) |
| 32 | symbol-registry | 0 | 0 | done | holds; functional promotion separately gated (§7.1) |
| 33 | vm-shared-misc | 13 | 12 | lock | fix did not land: updateSoftReservedZoneSize self/x-updateStackLimits (8), LazyProperty getInitializedOnMainThread x callFunc (3), ensureTerminationException (1). Crash evidence AGAIN: cve/mc-init-lazy-global-first-touch.js exit 134 this round (lazy-global init) |
| 34 | structure-cache | 6 | **0** | **done** | WeakGCMapLocking::Yes on m_structures verified in StructureCache.h; zero PrototypeKey frames corpus-wide |
| 35 | arraybuffer-wrapper | 0 | 0 | real-bug | STILL ZERO BY VARIANCE — m_wrapper remains a plain `Weak<JSArrayBuffer>` (ArrayBuffer.h:334), no fix landed; do NOT close |
| 36 | function-ctor-cache | 3 | 2 | real-bug | fix NOT landed — JSGlobalObject.cpp:4488/:4535 still lock-free Weak get/set; r3 stacks identical to 8.36 (jit/int-gate-stop-budget.js again) |
| 37 | regexp-compile-state | 1 | 0 | relaxed-atomic | ZERO BY VARIANCE — m_specificPattern still plain (RegExp.h:183/241), no fix landed; do NOT close |

Total: 1908. unsuppressedCount = **1908**.

### 10.b Re-binning census (r3)

- `('data race','WTF::Atomic<unsigned long/unsigned char/WatchpointState>::Atomic')`
  (80+24+10 = 114): writer-side stacks are the Structure ctor's init-list
  members constructed via the std::atomic CONSTRUCTOR (plain store):
  TinyBloomFilter copy ctor at Structure.cpp:440 (70), Structure ctor :438
  (18), InlineWatchpointSet/WatchpointSet ctor via Structure.cpp:442 (9),
  TinyBloomFilter at Structure.cpp:280 (1), readers = published/recycled-cell
  concurrent accessors (TinyBloomFilter::ruleOut relaxed load,
  getConcurrently, trySetIndexQuicklyConcurrent). 108 -> family 9.
  The remaining 6 are `PropertyInlineCache.h:607` ctor (DataOnly IC ctor vs
  concurrent reader) -> family 4.
- `('data race','malloc'/'aligned_alloc'/'realloc')` (130) -> family 23.
  JSC-caller census of the allocator side: MarkedBlock::tryCreate 52 +
  PreciseAllocation 10 (GC block reuse under unfixed object families),
  rope/string allocation sites ~40 (JSRopeString::resolveRopeWithFunction 23,
  jsString 6, resolveRopeToAtomString 4, repeatCharacter/substring 3,
  formatDateTime/dateProto 10 — String buffer allocs, family-17 echoes),
  WatchpointSet::operator new 10 (family-9 enumerator/watchpoint lifetime),
  ValueProfileAndVirtualRegisterBuffer::create 7, NumericStrings allocs 3.
  ZERO considerRepatching/ICSlowPath anchors among one-sided reports (the
  surviving 16 are TWO-sided x-malloc pairs, binned to family 4 per §9.2).
  Plus degraded singles ('decltype' 4, bare 'JSC::' 1) and heap-misc
  leftovers (RefCountedBase::derefBase 2, VectorBufferBase<unsigned char> et
  al 7) -> family 23 total 144. Nothing suppressed.
- m_shouldAlwaysBeInlined keys -> family 2: operationOptimize self (181),
  executeCallImpl self (96), prepareForMicrotaskCall self (1); also
  noticeIncomingCall x operationOptimize (7) stays in 2 (same byte).
  FTL::ForOSREntryJITCode::countEntryFailure (1) -> 2 per §8.1.
- `slow_path_to_this` (2) -> 11; liveness `FastBitVectorWordOwner`/
  `BytecodeBasicBlock` one-offs (4) -> 11 (§6 precedent).
- `SourceCode`/`UnlinkedSourceCode` ctor-vs-reader (3) +
  `ConcurrentJITCodePtr` ctor (1) -> 5 publication residuals.
- `VectorBufferBase<StructureID>`/adopt x malloc (4) -> 9;
  `decltype x Structure::concurrentRelaxedLoad` (2) -> 9;
  `Box<InlineWatchpointSet>::isValid x operator=` (1) -> 9 (§8.b).
- `BlockDirectory::nextDirectoryInAlignedMemoryAllocator` pair (2) +
  `ParallelHelperClient::runTask` (1) -> 21.
- `MicrotaskCall::relink x linkIncomingCall` (1) -> 20 (§8 precedent).
- `trySegmentedTransition` (1) + `FreeCell::setNext` (1) -> 15.
- `Weak<FunctionExecutable>::impl x swap` + `WeakImpl::WeakImpl x state` (2):
  both stacks are tryGetCachedFunctionExecutableForFunctionConstructor ->
  family 36.

### 10.4 ic-stubinfo — §9.2 acceptance test FAILED; epoch work is now its own wave item

The binding test was ZERO `considerRepatchingCacheImpl`/
`ICSlowPathCallFrameTracer` x-malloc pairs; r3 shows 8+8 survivors. Per
§9.2, the residual epoch work is now a standalone wave item: audit EVERY
free of retired handler-chain/stub data — all must ride
`RetiredJITArtifacts` or GC-deferred free. Owner files:
bytecode/PropertyInlineCache.h, bytecode/PropertyInlineCache.cpp,
bytecode/RetiredJITArtifacts.h, bytecode/RetiredJITArtifacts.cpp,
jit/GCAwareJITStubRoutine.h, jit/GCAwareJITStubRoutine.cpp.
Other r3 residuals for the family: DataOnly IC ctor publication
(PropertyInlineCache.h:607, 6+4), HandlerPropertyInlineCache::
initializeFromDFGUnlinkedPropertyInlineCache x globalObject reader (9),
resolve_scope metadata WRITER still plain (`Atomic<ResolveType>::load`
reader x slow_path_resolve_scope plain write, 12 — the §8.4 fix atomicized
the compiler-side read; the LLInt slow-path write needs the matching
relaxed store).

### 10.9 structure-fields — fixShape update (third iteration)

§9.1's helpers closed the accessor-vs-accessor and TypeInfoBlob classes
(family 7 is down to one line). What remains is exactly the ctor/publication
clause: (1) ctor init-list members whose TYPE is WTF::Atomic (TinyBloomFilter
m_seenProperties, the Atomic<u8> lock byte, InlineWatchpointSet m_state)
initialize through the std::atomic constructor — a plain store TSAN pairs
against concurrent readers of recycled/just-published Structures; route them
through the existing tsanRelaxedStore helpers (or member-init from a
pre-built value inside the §9.1 TSAN-only ctor path). (2) The release-store
publication of new Structures (StructureID/transition-table publish after
full ctor) is still not airtight: 18 one-sided Structure::Structure + the
:438/:440/:442 pairs all show readers reaching mid-ctor cells. (3) Writer
`Structure::setPropertyTable` is still a plain store against the (now
atomic) reader (26 one-sided + 36 paired — overlaps family 25's writer fix;
serialize those two items in the next wave). (4) Enumerator-cache
Box<InlineWatchpointSet> reassign + StructureChainInvalidationWatchpoint
FixedVector rebuild under Structure lock + GC-deferred retire — unchanged,
still open (Box key 1, prototypeChain 8, slow_path_enumerator_next 4).

### r3 run-health notes

- 224 files: 218 ran, 6 `//@ skip` (same set). 142 exit 66 (races reported),
  50 exit 0 (clean under suppressions), 17 exit 3, 8 exit 134, 1 exit 124.
- Timeout (exit 124): semantics/proto-cycle-race.js only (KNOWN-FAILING,
  every round). cve/mc-init-cloned-arguments-specials.js ran WITHOUT
  timing out this round (exit 66) — consistent with §8.33's flakiness, the
  family-33 lazy-init gap remains un-fixed.
- Aborts (exit 134, 8): repeats cve/mc-hand-restrict-claim.js,
  heap-iss-revert.js, gc-stress/havebadtime-vs-indexed-fastpath.js
  (KNOWN-FAILING); NEW this round: cve/mc-df-ta-detach-resize.js,
  cve/mc-init-lazy-global-first-touch.js (family-33 crash evidence, see
  table), cve/mc-jit-delete-reuse-stale-offset.js,
  invariants/no-lost-properties-same-name.js, scaling/richards-like.js.
  r2's three new aborts (mc-int-resizable-tail-quarantine, mc-life-sab-
  refchurn, frozen-seal-race) did NOT abort this round (flaky functional
  bugs; still queued for the functional round).
- exit 3 (JS failure, no crash): 17 tests (14 in r2) — includes the bench/
  block (8, likely harness-tolerance under TSAN slowdown) — functional
  round's scope, race reports counted.
- Wave-4 partitioning: family 2 is now the single biggest item (429) and is
  file-disjoint from everything except 11 (both touch bytecode/CodeBlock.h —
  same bundle or serialize). 9+25 overlap on Structure.h/.cpp (setPropertyTable
  writer) — bundle them. 4's epoch item (10.4) is self-contained. The
  remaining block 1/5/7/10/15/17/20/21/24/26/28/31 is file-disjoint per §4.
  33/35/36/37 are small lock/atomic items; 23 stays write-nothing.

## 11. Wave-4 review amendments (adversarial findings, applied 2026-06-09)

Five wave-4 fixes were amended after adversarial review. All five findings
were verified against the tree before amendment; none were false positives.

### 11.21 gc-marking-residual — the relaxed annotation was masking a REAL race (blocker, fixed)

The wave-4 fix added an opt-in `m_appendLock` to `MarkStackArray`
(heap/MarkStack.h) and justified the relaxed atomics in
GCSegmentedArray(Inlines).h with "multi-producer instances serialize through
m_appendLock" — but `setMultiProducerAccess()` had ZERO call sites, so the
lock was dead code and `Heap::addToRememberedSet` (Heap.cpp, write-barrier
slow path, N mutators per shared server Heap) still ran the unlocked
non-atomic `postIncTop`/`append`/`expand` path. Two racing barriers can lose
an increment => lost remembered-set entry => live object collected =>
use-after-free. NOT spec-blessed staleness; exactly the campaign's stated
failure mode (a real race annotated into TSAN silence).

Amendment: `Heap::Heap` now calls
`m_mutatorMarkStack->setMultiProducerAccess()` when
`Options::useSharedGCHeap()` is set. Multi-appender audit (per the
MarkStack.h comment's own list): m_raceMarkStack appends are serialized by
m_raceMarkStackLock (SlotVisitor.cpp), m_sharedCollectorMarkStack /
m_sharedMutatorMarkStack by m_markingMutex, per-SlotVisitor stacks are
single-producer — m_mutatorMarkStack was the only unprotected instance.
Flag-off: gate is off without the shared heap; with it on, the [[unlikely]]
branch shape is unchanged. FOLLOW-UP (r4 verification): re-run one TSAN pass
with the GCSegmentedArray relaxed annotations temporarily reverted to confirm
the lock now covers all previously reported postIncTop/append pairs (the
annotations would otherwise mask any residual unlocked producer).

### 11.35 arraybuffer-wrapper — first-wins CAS latched a dead WeakImpl forever (blocker, fixed)

The wave-4 first-wins protocol (ArrayBuffer.h m_wrapperImpl +
SimpleTypedArrayController) had sound synchronization (review confirmed the
release-CAS + dependency-ordered-reader argument) but was write-once for the
ArrayBuffer's lifetime: once the cached wrapper was GC'd while the native
buffer survived (C API / native-held RefPtr<ArrayBuffer> re-wrap), every
later registerWrapper() lost the CAS forever — wrapper caching and identity
(view.buffer === view.buffer) permanently disabled, IDENTICALLY under
useJSThreads=false, where the old code unconditionally re-registered. A
flag-off observable-behavior change — rule violation.

Amendment: the slot is now recoverable. registerWrapper keeps the lock-free
first-publication CAS fast path; on CAS failure it takes the new leaf
`ArrayBuffer::m_wrapperRepublishLock`, and iff the current impl is not Live,
republishes via `publishReplacementWrapperImpl()` (release store; the
nullptr-expecting CAS cannot fire while the slot is non-null and
republishers are lock-serialized). Per SPEC-ungil §LK WS row (i) no Weak is
created or destroyed under the lock (constructed before, displaced handled
after). The displaced dead Weak cannot be deallocated inline — a concurrent
toJS() may still hold its WeakImpl* from a pre-replacement snapshot — so it
rides the EXISTING safepoint-epoch machinery: heap-boxed and
`GCSafepointEpoch::retire()`d; readers never span a safepoint between the
snapshot and the state() check, so epoch reclamation (all clients past a
stop) is sufficient. Readers unchanged (still lock-free). Flag-off semantics
restored: re-registration after wrapper death caches again, exactly as the
old unconditional overwrite did (the dead impl's slot reclamation is merely
deferred to the legacy runEndPhase reclaim site — not observable).

### 11.36 function-ctor-cache — Weak::set ran UNDER the leaf lock (major, fixed)

The wave-4 lock correctly closed the §8.36 UAF, but
`m_executableForCachedFunctionExecutableForFunctionConstructor.set(vm(), ...)`
ran inside m_functionConstructorExecutableCacheLock; Weak::set allocates a
fresh WeakImpl (MSPL under ISS) and deallocates the previous one — SPEC-ungil
§LK WS row (i) explicitly forbids Weak creation under leaf locks
(safepoint-park while holding the lock => safepoint-blind waiter => the ab17b
watchdog-timeout family). Amendment (the spec's prescribed shape): construct
`Weak<FunctionExecutable>` BEFORE the lock; under the lock, `swap()` with the
member (pointer swap only); the displaced Weak deallocates after release.
Get side unchanged.

### 11.17 rope-stringimpl — family stays OPEN; ruling is PARTIAL (major, recorded)

The r3 table's "relaxed-atomic" ruling overstates what landed. Residuals,
kept open for the next wave:
- `JSRopeString::convertToNonRope` (JSStringInlines.h) publication is still a
  PLAIN placement-new store behind a storeStoreFence (the JSString.h NOTE is
  accurate). The companion, when it lands, must be a RELEASE store/exchange
  (mirroring swapToAtomString) — resolution-republication is publication, not
  stale-tolerable data; relaxed is NOT sufficient there. Until then, TSAN
  pairs keyed on convertToNonRope vs fiberConcurrently persist.
- r3-noted hash/flags writers in StringImpl remain plain.
- PRECONDITION now recorded in code (JSString.h CompactFibers): the composed
  per-field relaxed loads in fiber1()/fiber2() can assemble a TORN pointer
  that no thread ever wrote; this is sound ONLY under the OM ground-truth
  re-dispatch rule — fiber values are never dereference-safe in concurrent
  code without re-validating the cell. Same precondition the replaced
  unaligned wide load relied on (not a regression), stated explicitly so the
  values are not treated as dereference-safe by future readers.

### 11.11 codeblock-init — implicit seq_cst read on a flag-off path (major, fixed)

The hoisted `std::atomic<bool> m_singletonHasBeenInvalidated`
(UnlinkedFunctionExecutable.h) carried a FALSE comment claiming an implicit
std::atomic read is a plain load on all supported targets: on ARM64 a
contextual seq_cst load codegens to `ldar` (acquire), not plain `ldrb`. One
such read existed — `UnlinkedFunctionExecutable::link()`, warm on the
flag-off path for every FunctionExecutable creation. Amendments: link() now
uses the relaxed `singletonHasBeenInvalidated()` accessor (audit found no
other implicit access); the ctor init-list mentions in
UnlinkedFunctionExecutable.cpp / CachedTypes.cpp were DROPPED (the member's
default initializer covers them) — they initialized the member in its old
declaration position, and the resulting -Wreorder-ctor warnings are build
breaks under -Werror configs (CI release scripts / downstream embedders),
not "harmless"; the header comment's ISA claim is corrected and now mandates
accessor-only access.

## 12. Round 4 (r4) re-run — wave-4 verification (2026-06-09)

Same binary config as §0 (full JIT, asm LLInt; `ENABLE_C_LOOP:BOOL=OFF` and
`ENABLE_JIT:BOOL=ON` re-verified in `WebKitBuild/TSan/CMakeCache.txt` before
the run). Build currency verified: `ninja jsc` in the TSan dir = "no work to
do"; binary mtime (Jun 9 13:53) > newest wave-4/§11-amendment source mtime
(the build the builder/amender left WAS current; no rebuild needed). Same run
config as §1. Raw output: `Tools/threads/tsan/reports-r4.log` +
`Tools/threads/tsan/r4/`; dedup keys in `Tools/threads/tsan/families-r4.txt`
(142 raw keys, down from 223).

Result: **1012 reports** (1908 in r3 — a 47% drop). **Zero CLoop frames**
(string absent from the entire corpus output, as in every round — CLoop is
not compiled in). Nothing suppressed this round; `unsuppressedCount = 1012`.

### Headline findings

1. **m_shouldAlwaysBeInlined fix verified** — the 278-report byte (§10
   headline) is gone; family 2 drops 429 -> 75. The surviving family-2 bulk
   (67) is a DIFFERENT site that the r3 table had lumped in: the write at
   CodeBlock.cpp:889 `unlinkedCodeBlock()->m_unlinkedBaselineCode =
   WTF::move(jitCode)` — two threads finalizing baseline plans for
   CodeBlocks sharing one UnlinkedCodeBlock both install the shareable
   baseline code (write-write on a RefPtr, 8 bytes). A racing plain RefPtr
   assignment is ref-count corruption, not advisory data: this sub-item is
   re-ruled **first-wins CAS/lock publication** (concurrent-accessor shape,
   like §11.35), NOT relaxed-atomic. See 12.2.
2. **Family 6 (watchpoints) REOPENS at 18** — a key that r3's census binned
   to family 9 by writer shape (`Atomic<WatchpointState>::Atomic` ctor) is,
   per r4 stacks, NOT the Structure ctor (that fix landed; TinyBloomFilter/
   Structure-ctor keys are all gone): the writer is
   `WatchpointSet::WatchpointSet` inside `InlineWatchpointSet::inflateSlow()`
   (Watchpoint.cpp:465 via DFG DesiredWatchpoints::reallyAdd), the reader a
   concurrent compiler thread's `InlineWatchpointSet::state()` reaching the
   just-inflated fat set through the thin->fat pointer swap without acquire
   ordering on the publication. See 12.6.
3. **Both §11 blocker amendments verified at zero**: family 35
   (arraybuffer-wrapper CAS + republish lock) and family 36
   (function-ctor-cache swap-outside-lock) report zero with their fixes now
   actually in tree (r3 zeros were by variance with NO fix landed; r4 zeros
   are with the fix landed — now ruled done). Family 21's §11.21
   setMultiProducerAccess amendment also verified: all GCSegmentedArray
   postIncTop/append pairs (110 in r3) are gone.

### r4 family table

Counts after the same manual re-binning discipline as previous rounds
(census in §12.b).

| # | id | r3 | r4 | ruling | delta notes |
|---|----|----|----|--------|-------------|
| 1 | value-profile | 104 | 64 | relaxed-atomic | partial: LazyValueProfile holder pairs closed; iterator try-fast metadata writes SURVIVE AGAIN (iteratorOpen/NextTryFastImpl self + operationIteratorNextTryFast + ByteCodeParser readers, 54) + ValueProfileAndVirtualRegisterBuffer x malloc (1); the op_iterator_open/next Metadata profile slots were never converted |
| 2 | exec-counter | 429 | 75 | relaxed-atomic / CAS for the 889 site | m_shouldAlwaysBeInlined (278) closed. Residual: m_unlinkedBaselineCode RefPtr install write-write at CodeBlock.cpp:889 (67) — RE-RULED first-wins CAS/lock (12.2); tierUpCommon x compilationDidBecomeReadyAsynchronously (5); countReoptimization x reoptimizationRetryCounter (3) |
| 3 | array-profile | 0 | 0 | done | holds |
| 4 | ic-stubinfo | 57 | 47 | lock | §10.4 epoch acceptance test FAILED AGAIN: 18 x-malloc stub-reuse pairs (ICSlowPathCallFrameTracer 12, considerRepatchingCacheImpl 6) vs 16 in r3 — the retired-artifact epoch audit did not land/work; resolve_scope LLInt writer still plain (11); Handler/PropertyInlineCache ctor-publication x identifier (14); disarmClearingWatchpointOnRetire self (2); PutByStatus::computeFromLLInt x llint_slow_path_put_by_id (2) |
| 5 | code-lifecycle | 15 | 9 | concurrent-accessor | partial: DFG::compileImpl self pairs gone; capabilityLevelState x installCode (4), replacement x replaceCodeBlockWith (1), SourceCode/UnlinkedSourceCode/ConcurrentJITCodePtr ctor-publication singles (4, unchanged) |
| 6 | watchpoints | 0 | **18** | relaxed-atomic | **REOPENED** — newly attributed (previously mis-binned to 9): InlineWatchpointSet::inflateSlow fat-set ctor (Watchpoint.cpp:465) vs concurrent state() readers; publish the inflated pointer with release + ctor-init m_state via relaxed store (12.6) |
| 7 | cell-header | 1 | **0** | **done** | WriteBarrierStructureID::setWithoutWriteBarrier relaxed store landed |
| 8 | arith-profile | 0 | 0 | done | holds |
| 9 | structure-fields | 182 | 43 | concurrent-accessor | big win: §10.9 ctor init-list items closed (TinyBloomFilter copy 70, m_lock 18, all gone; one Atomic<u8> ctor single remains), TsanDeferredCtorMember helper visible in stacks. Residual: StructureChain::create x StructureID/canCachePropertyNameEnumerator (10), enumerator-cache (slow_path_enumerator_next 10, JSPropertyNameEnumerator 3, Box<InlineWatchpointSet> 1), Structure::add (3), VectorBuffer<StructureID> x malloc (12), decltype x concurrentRelaxedLoad (2), TsanDeferredCtorMember x isUsingSingleSlot (1) |
| 10 | jsvalue-slots | 10 | 14 | relaxed-atomic | unfixed + newly binned: tryGet/trySetIndexQuicklyConcurrent (7), SparseArrayValueMap getEntry/ctor/allocateSparseIndexMap (4), GetterSetter ctor WriteBarrier-slot publication x SparseArrayEntry::get reader (2, new key — same SAB-on-property cluster), Register pair (1) |
| 11 | codeblock-init | 153 | 14 | relaxed-atomic | setSingletonHasBeenInvalidated (58) + BaselineJITCode rates (72) closed. Residual: UnlinkedMetadataTable::link one-sided (4), catch/liveness lazies (ensureCatchLiveness self 2+2, livenessAnalysisSlow 2), isInStrictContext x recordParse (2), CodeBlock ctor x vm/couldBeTainted (2), CatchInfo (1), slow_path? create_this moved to 31 |
| 12 | metaallocator-stats | 0 | 0 | done | holds |
| 13 | linkbuffer-stats | 0 | 0 | done | holds |
| 14 | simple-stats | 0 | 0 | done | holds |
| 15 | butterfly-words | 61 | 41 | concurrent-accessor | partial: residual writers unchanged in kind — createInitialIndexedStorageConcurrent x Atomic store readers (23), ensureLengthSlowConcurrent (10), FreeCell::makeLast/free-list (7), Butterfly::createOrGrowPropertyStorage (2) — the concurrent-storage WRITER side still does plain stores that pair with the (now atomic) readers |
| 16 | tinybloom | 0 | 0 | done | holds (ctor-copy side closed under 9 this wave) |
| 17 | rope-stringimpl | 129 | 123 | relaxed-atomic + RELEASE publication (§11.17) | essentially unchanged, as §11.17 predicted: convertToNonRope publication still the plain placement-new behind storeStoreFence — one-sided String::String keys (53), Atomic load x String::String (18), unalignedLoad/copyElements (24), equalCommon (13), StringView/parseIndex x malloc (6), resolveToBuffer (3), hash/flags writers still plain |
| 18 | regexp-shared | 0 | 0 | done | holds |
| 19 | alloc-profile | 0 | 0 | done | holds |
| 20 | calllink | 40 | 31 | concurrent-accessor | partial: capabilityLevel x noticeIncomingCall (14), noticeIncomingCall x installCode (4), specializationKind x UnlinkedMetadataTable::link (4), CallLinkInfo ctor x specializationKind (3+1), MicrotaskCall::tryCallWithArguments x unlinkOrUpgradeImpl (3), linkIncomingCall x relink (1), CachedCall::unlinkOrUpgradeImpl x executeCachedCall (1) |
| 21 | gc-marking-residual | 151 | 22 | relaxed-atomic | §11.21 amendment verified: all postIncTop/append pairs (110) gone. Residual: IsoCellSet::addSlow one-sided (16) + x BitSet load (1), visitCount x appendToMarkStack (5) — the IsoCellSet m_bits lazy-segment publication and the visitCount counter were not converted |
| 22 | vm-string-caches | 0 | 0 | done | holds |
| 23 | misc-tail | 144 | 156 | suppress | STILL NOT SUPPRESSED (third consecutive wave). Pure one-sided allocator echoes: malloc 120, aligned_alloc 33, realloc 3. Parents are the unfixed 9/15/17/24/26 families. ACTION for wave 5: actually add the narrow one-sided allocator-echo suppressions (justification: degraded second stack = JIT/realloc side, §0 tradeoff) or keep eating 15% of the report stream |
| 24 | typedarray-sab | 99 | 91 | concurrent-accessor | fix still not landed in substance: CagedPtr pairs (31), ArrayBufferContents::sizeInBytes (25 incl. ctor pair), zeroFill/getData/fill lanes (17 — SAB raw-lane class, candidates for tsan-blessed atomic lane accessors), view ctor publication x byteOffsetRaw/lengthRaw/mode (9), get/setIndexQuickly pairs (4), transferTo (2), misc (3) |
| 25 | property-table | 88 | 32 | concurrent-accessor | setPropertyTable writer fix landed (62-report key gone). Residual: addAfterFind x size (20), remove x size (2) — the in-place mutate vs concurrent size/iterate reads; quarantine counters (10) |
| 26 | block-directory-bits | 144 | 133 | relaxed-atomic | FIX DID NOT LAND — SECOND consecutive no-op wave: identical single key, WTF FastBitReference::operator= word RMW still plain vs BlockDirectoryBitVectorWordView reader. Owner file is WTF/wtf/FastBitVector.h (NOT just heap/BlockDirectoryBits.h — that is why two waves missed it); convert the FastBitReference word write to relaxed atomic fetch_or/fetch_and on a TSAN/threads-gated path |
| 27 | gc-incoming-ref | 0 | 0 | done | holds |
| 28 | barrier-counter | 64 | 65 | relaxed-atomic | UNFIXED (second consecutive wave): Heap.cpp:1447 `m_barriersExecuted++` plain uintptr_t RMW, N mutators. One-line relaxed Atomic increment (advisory stat, JIT §5.7.7) — assign explicitly in wave 5 |
| 29 | date-cache | 0 | 0 | done (TSAN) | holds; functional repro separately gated |
| 30 | microtask-queue | 0 | 0 | done | holds |
| 31 | directarguments | 23 | 22 | concurrent-accessor | unfixed: DirectArguments ctor x isMappedArgumentInDFG/internalLength (8+3), modifiedArgumentDescriptor (4), overrideThings (4), JSFunction ctor/rareData x executable (2), slow_path_create_this one-sided (1) |
| 32 | symbol-registry | 0 | 0 | done | holds |
| 33 | vm-shared-misc | 12 | 10 | lock | fix did not land AGAIN: updateSoftReservedZoneSize self/x-updateStackLimits (7), ensureTerminationException x isTerminationException (1), LazyProperty getInitializedOnMainThread pairs (2). Crash evidence persists: cve/mc-init-lazy-global-first-touch.js exit 134 again this round |
| 34 | structure-cache | 0 | 0 | done | holds |
| 35 | arraybuffer-wrapper | 0 | **0** | **done** | NOW closeable: §11.35 fix (first-wins CAS + m_wrapperRepublishLock republish) verified in tree; zero reports with the fix landed (r3's zero was by variance without a fix) |
| 36 | function-ctor-cache | 2 | **0** | **done** | §11.36 fix (Weak constructed outside lock, swap under lock) in tree; zero reports incl. jit/int-gate-stop-budget.js which reproduced it in r2/r3 |
| 37 | regexp-compile-state | 0 | 2 | relaxed-atomic | fix STILL not landed (m_specificPattern plain at RegExp.h); r3's zero was variance as predicted — r4 shows compileHoldingCellLock x specificPattern + isValid x YarrPattern. One-line relaxed accessor pair |

Total: 1012. unsuppressedCount = **1012** (nothing suppressed; zero CLoop
frames; the §0 CLoop standing ruling required no entries for the fourth
consecutive round).

### 12.b Re-binning census (r4)

- `('data race','WTF::Atomic<JSC::WatchpointState>::Atomic')` (18): writer =
  `WatchpointSet::WatchpointSet` under `InlineWatchpointSet::inflateSlow`
  (DFG::DesiredWatchpoints::reallyAdd, main/mutator), reader = compiler-thread
  `InlineWatchpointSet::state()` via `Structure::transitionWatchpointSetIsStillValid`
  -> **family 6** (NOT 9 — r3's by-shape binning of Atomic-ctor keys to the
  Structure ctor no longer applies now that the Structure-ctor fix landed).
- `('data race','malloc'/'aligned_alloc'/'realloc')` (156) -> family 23
  (one-sided allocator echoes, same census discipline as §10.b).
- `JSC::parseIndex x malloc` (1) -> 17 (string-parse buffer alloc echo).
- `disarmClearingWatchpointOnRetire` self (2) -> 4 (IC handler retire path).
- `GetterSetter::GetterSetter` (2): reader stack is SparseArrayEntry::get ->
  isGetterNull via atomic WriteBarrier load (ThreadAtomics
  atomicsStoreOnPropertyGilOff cluster) -> family 10.
- `decltype x Structure::concurrentRelaxedLoad` (2), `data race x
  Atomic<unsigned char>::Atomic` (1), `VectorBufferBase<StructureID>`/`adopt`
  x malloc (12), `Box<InlineWatchpointSet>` (1) -> 9 (§10.b precedents).
- `allocateSparseIndexMap` (1) -> 10.
- `SourceCode`/`UnlinkedSourceCode` (3) + `ConcurrentJITCodePtr` (1) -> 5.
- `MicrotaskCall::tryCallWithArguments x unlinkOrUpgradeImpl` (3),
  `linkIncomingCall x MicrotaskCall::relink` (1, §10.b precedent),
  `CachedCall::unlinkOrUpgradeImpl x executeCachedCall` (1) -> 20.
- `slow_path_create_this` (1) -> 31 (reads FunctionRareData allocation
  profile; classifier's directarguments bin agrees).
- `FreeCell::makeLast x StructureChain::head` (1) -> 15 (free-list writer).
- `PutByStatus::computeFromLLInt x llint_slow_path_put_by_id` (2) -> 4
  (LLInt metadata writer vs compiler-side status read, same class as the
  resolve_scope item).

### 12.2 exec-counter — residual re-ruled: m_unlinkedBaselineCode is publication, not profiling

All 67 surviving `setupWithUnlinkedBaselineCode x jitCompileAndSetHeuristics`
reports are write-write on CodeBlock.cpp:889
`unlinkedCodeBlock()->m_unlinkedBaselineCode = WTF::move(jitCode)` (baseline
code sharing: N CodeBlocks of one UnlinkedCodeBlock finalize concurrently on
different Thread mutators via JITWorklist::completeAllReadyPlansForVM).
Plain concurrent RefPtr assignment can double-deref/leak the displaced
BaselineJITCode — REAL ref-count corruption risk, not §5.7.7 advisory data.
Fix shape: first-wins publication — CAS an `Atomic<BaselineJITCode*>` slot
(or install under the UnlinkedCodeBlock's ConcurrentJSLock since this is a
cold finalize path); losers keep their per-CodeBlock Ref (already held) and
drop the duplicate. Readers (`m_unlinkedBaselineCode` consumers in
setupWithUnlinkedBaselineCode's sharing check) take the acquire/consume load.
Files: bytecode/UnlinkedCodeBlock.h, bytecode/CodeBlock.cpp. Flag-off: gate
the CAS shape on the same path both modes use — single-mutator behavior
identical (CAS always wins).

### 12.6 watchpoints — reopened: inflateSlow publication

`InlineWatchpointSet::inflate()` swaps the thin (inline-bits) representation
for a heap-allocated fat `WatchpointSet` and publishes the pointer into the
inline word. The fat set's `m_state` is initialized by the WTF::Atomic
CONSTRUCTOR (plain store, Watchpoint.cpp:139) and the pointer publication is
not a release store, so a concurrent `state()`/`isStillValid()` reader
(DFG DesiredWatchpoints::consider on a compiler thread) can observe the fat
pointer without the ctor's initialization ordered-before. Spec: JIT §5.6 —
watchpoint STATES are monotonic and racy-readable, but POINTER publication
must be release/consume like any cell publication. Fix shape: in
inflateSlow, initialize m_state via relaxed store (or pre-built value),
publish the fat pointer with a release store; readers of the inline word use
the existing consume-ordered load (pointer-chase dependency suffices on all
supported targets). Files: bytecode/Watchpoint.h, bytecode/Watchpoint.cpp.
Flag-off: identical codegen on x86; arm64 gains a `stlr` on a cold
inflate path only.

### r4 run-health notes

- 224 files: 218 ran, 6 `//@ skip` (same set). Exit codes: 121 exit 66
  (races reported), 75 exit 0 (clean under suppressions — up from 50 in r3),
  18 exit 3, 4 exit 134, **0 timeouts** (proto-cycle-race.js did NOT time
  out this round — exit 3, 0 reports; first round it completed).
- Aborts (exit 134, 4 — down from 8): cve/mc-hand-restrict-claim.js,
  heap-iss-revert.js (repeats every round),
  gc-stress/havebadtime-vs-indexed-fastpath.js (KNOWN-FAILING),
  cve/mc-init-lazy-global-first-touch.js (family-33 crash evidence, second
  consecutive round). r3's other aborts (mc-df-ta-detach-resize,
  mc-jit-delete-reuse-stale-offset, no-lost-properties-same-name,
  richards-like) did not recur.
- KNOWN-FAILING tests ran; reports counted (date-cache-churn 12,
  symbol-registry-cross-thread 20, havebadtime-vs-indexed-fastpath 5).
- §11.21 FOLLOW-UP (annotations-reverted verification run) was NOT executed
  this round (one-corpus-run budget); in lieu, the amendment was verified
  statically (Heap.cpp:496 calls setMultiProducerAccess under
  useSharedGCHeap; m_appendLock path covers postIncTop/append/expand) and
  empirically by the 110-report key vanishing. Keep the reverted-run
  experiment queued if family 21 regresses.
- Wave-5 partitioning: 26 (WTF/wtf/FastBitVector.h — reassign with the
  CORRECT owner file), 28 (heap/Heap.cpp one-liner), 6 (bytecode/Watchpoint.*),
  2's CAS item (bytecode/UnlinkedCodeBlock.h + CodeBlock.cpp), 17 (release
  publication, §11.17 shape), 24, 15, 25, 9-residue, 1 (iterator metadata),
  4 (epoch audit re-do + resolve_scope writer), 20, 21-residue, 31, 33, 37,
  10/11/5 small items. 23: decide suppression entries (write-only to
  Tools/tsan/suppressions.txt with per-entry justification).

## 13. Wave-5 review amendments (post-wave audit; all five findings verified and applied)

An independent review of the wave-5 fixes surfaced two blockers and three
rule/convention violations. Each was verified against the tree (none refuted)
and amended as follows. Rebuild: Debug + TSan after these edits.

### 13.1 exec-counter (§12.2) — reader half of the re-ruling landed (blocker, fixed)

Wave 5 installed `installUnlinkedBaselineCodeIfAbsent()` +
`unlinkedBaselineCodeConcurrently()` but converted ZERO readers; all three
racing readers still did bare `RefPtr` loads of `m_unlinkedBaselineCode`,
keeping the read-write torn-pointer / ref-count hazard the re-ruling called
REAL (the installer's lock release only synchronizes with other lock
acquirers; the RefPtr copy-ctor's ref() can race the displaced-pointer
write). Converted to the synchronized snapshot:
- llint/LLIntSlowPaths.cpp jitCompileAndSetHeuristics (tier-up slow path),
- runtime/ScriptExecutable.cpp prepareForExecutionImpl (codeBlock-creation
  sharing path),
- jit/JITWorklist.cpp enqueue duplicate-plan check (its claim-set ordering
  comment argued visibility only, not the data-race UB; comment rewritten).
All three are cold slow paths and the lock is uncontended flag-off
(flag-off-unchanged rule satisfied). Family 2 may now be marked done when
the re-run confirms.

### 13.2 property-table (§3.25) — edit stamp was a one-sided seqlock (blocker, fixed)

`bumpConcurrentEditCount()` was called BEFORE the in-place mutation, so a
reader snapshotting after the bump but mid-mutation read torn entries and
then PASSED the under-lock recheck (count unchanged since snapshot) —
silent-wrong-value corruption shape; the JSObject.cpp:4390 "frozen" claim was
false for that interleaving. RULING RE-DERIVED: writers bump AFTER the
mutation completes, still under the cell lock (bumps moved in
PropertyTable.h addAfterFind/take/updateAttributeIfExists). A passing
under-lock recheck now proves no edit overlapped the snapshot..recheck
window: an edit mid-flight at snapshot time bumps after the snapshot (recheck
differs -> RESTART); an edit mid-flight at recheck time is impossible (the
reader holds the lock). The recheck MUST stay under the lock — there is no
odd/in-progress marker, so a lock-free recheck is NOT sound (protocol
comment added at the declaration). Sole reader pair
(deletePropertyNamedConcurrent) audited: recheck is under the cell lock.
Flag-off: bump is already gated on useJSThreads(); zero cost.

### 13.3 code-lifecycle — SourceCode/UnlinkedSourceCode wrappers de-gated (major, fixed)

The wave-5 `TsanSourceProviderPtr`/`TsanRelaxedPodMember` members in
parser/UnlinkedSourceCode.h (mirrored in SourceCode.h) were `#if
TSAN_ENABLED`-only — a structural suppression with no §9.1-style recorded
entry, leaving the plain racing accesses in production GIL-off builds while
TSAN reported zero, and blinding TSAN to all future accesses through these
members. No bench argument applies (relaxed scalar/pointer atomics are
codegen-identical to the plain access). Made UNCONDITIONAL (renamed
`RelaxedSourceProviderPtr`/`RelaxedPodMember`), same convention as
WriteBarrier.h/ValueProfile.h in wave 3. The single-writer/ctor-publication-
only contract for the provider pointer word is now explicit in the class
comment: relaxed atomics make the word accesses defined, NOT assignment-vs-
reader safe (load-then-ref vs refcount-to-zero); any future post-publication
mutation needs real synchronization — and TSAN can now see it.

### 13.4 rope-stringimpl — CompactFibers wide-load arm restored flag-off (major, fixed)

Wave 5 unconditionally replaced the CPU(LITTLE_ENDIAN) single unaligned
8-byte fiber load with two narrow relaxed loads — a flag-off codegen change
on hot rope paths (resolveRope*, iterRope*, equal/compare, GC visitChildren),
forbidden by the campaign rules. RESTORED the wide-load arm for
`CPU(LITTLE_ENDIAN) && !TSAN_ENABLED`; the per-field relaxed composition is
now TSAN-only (unaligned atomics do not exist, so the wide load cannot be
annotated). RECORDED TSAN-ONLY DIVERGENCE (this entry is the §9.1-style
justification): the value race on the fiber words is the blessed
§5.7/OM-ground-truth class with the re-dispatch rule (fiber values never
dereference-safe without cell re-validation) — identical semantics under
both arms; the TSAN arm additionally admits a torn composed pointer, blessed
by the same rule. Alternate coverage for the non-TSAN arm: object-model
protocol tests/amplifier. Precedent: Structure.h tsanRelaxedLoad/Store.

### 13.5 gc-marking-residual — IsoCellSet consume load demoted (major, fixed)

`isoCellSetBitsPointerConcurrently()` used std::memory_order_consume, which
all compilers promote to ACQUIRE — a new `ldar` on ARM64 flag-off in
IsoCellSet::add()/contains(), which are FAST paths of the cell-set protocol
(run during every GC for finalizer/destruction subspaces; the in-code
comment's "slow paths" claim was wrong). Replaced with the established
consume-emulation shape (Structure.h tryRareData / rareDataConcurrently):
relaxed `WTF::atomicLoad` + `WTF::dependentLoadLoadFence()` — free on x86
and ARM64 via the address dependency, codegen-identical to the old plain
load. TSAN cannot see the dependency edge, so the load is acquire under
TSAN_ENABLED only (JITCodePointerConsumeOrder precedent, jit/JITCode.h).
The addSlow release-publish side is unchanged.

## 14. Round 5 (r5) re-run — wave-5 (+§13 amendments) verification (2026-06-09)

Same binary config as §0 (full JIT, asm LLInt; `ENABLE_C_LOOP:BOOL=OFF`,
`ENABLE_JIT:BOOL=ON` re-verified in `WebKitBuild/TSan/CMakeCache.txt`). Build
currency verified before the run: `ninja jsc` = "no work to do"; binary mtime
(Jun 9 14:44:16) > newest source mtime (Jun 9 14:31:53, the §13 amendment
edits) — the builder/amender left the build CURRENT; no rebuild needed. Same
run config as §1. Raw output: `Tools/threads/tsan/reports-r5.log` +
`Tools/threads/tsan/r5/`; dedup keys in `Tools/threads/tsan/families-r5.txt`
(120 raw keys, down from 142).

Result: **763 reports** (1012 in r4 — a 25% drop). **Zero CLoop frames**
(string absent from the entire corpus output; CLoop is not compiled in).
Nothing suppressed this round; `unsuppressedCount = 763`.

### Headline findings

1. **§13.1 verified — family 2's CAS re-ruling is CLOSED.** All 67
   `m_unlinkedBaselineCode` write-write reports (CodeBlock.cpp:889) are gone
   with the installer + all three synchronized readers in tree. The surviving
   family-2 residue (11) is the two SMALL relaxed-atomic items the r4 table
   already listed and no wave has picked up: `countReoptimization x
   reoptimizationRetryCounter` (8) and `tierUpCommon x
   compilationDidBecomeReadyAsynchronously` (3).
2. **§13.2 verified — family 25 (property-table) at ZERO.** The
   bump-after-mutation seqlock discipline closed all 32 r4 reports
   (addAfterFind/take/size pairs and quarantine counters). Ruled done.
3. **§12.6 verified — family 6 (watchpoints) back to ZERO.** The
   inflateSlow release-publication fix landed; no
   `Atomic<WatchpointState>::Atomic`-ctor keys anywhere in r5. (The single
   `Box<InlineWatchpointSet>` key re-bins to family 9's enumerator-cache
   cluster per §12.b precedent.)
4. **Two families had a THIRD consecutive no-op wave** — both are one-file,
   sub-10-line fixes that keep not landing:
   - family 26 (block-directory-bits), now the LARGEST code family at 149
     (was 133): `WTF::FastBitReference::operator=` (FastBitVector.h:436-442)
     is still the plain `*m_word |= / &= ~mask` RMW. The §12 owner-file
     correction (WTF/wtf/FastBitVector.h, not heap/BlockDirectoryBits.h) was
     recorded but the conversion still did not land.
   - family 28 (barrier-counter) at 61: Heap.cpp:1447 `m_barriersExecuted++`
     is verbatim unchanged (plain `uintptr_t` RMW, Heap.h:1377). NOTE: r5's
     classifier surfaces this under the anchor `Heap::addToRememberedSet`
     (writeBarrierSlowPath inlining changed the symbolized frame) — it is
     NOT a new remembered-set family; the racing line is the same statement
     the r3/r4 tables carried. Verified by stack inspection (both sides
     Heap.cpp:1447:23).
   Also third no-ops: family 33 (vm-shared-misc, 11 — updateSoftReservedZoneSize
   self-races persist; the cve/mc-init-lazy-global-first-touch.js abort did
   NOT recur this round, but mc-lock-cow-materialize-race.js aborted, exit
   134) and family 37 (regexp-compile-state, 2 — `m_specificPattern` still
   plain).
5. **Family 19 (alloc-profile) REOPENS at 2** (done since r2):
   `ObjectAllocationProfileBase::initializeProfile` /
   `setPrototype` writer vs concurrent reader — the OBJECT allocation
   profile half was never converted (the r1 fix covered ArrayAllocationProfile
   and the watchpoint-adjacent fields). Ruling stays relaxed-atomic
   (JIT §5.7.5); files: bytecode/ObjectAllocationProfile.h,
   bytecode/ObjectAllocationProfileInlines.h.
6. **One NEW single-report key worth a ruling**, binned to family 5:
   `RefCountedBase::derefBase x derefBase` on
   `JSC::DeferredCompilationCallback` — the callback is plain `RefCounted`
   (non-atomic count) but is ref'd/deref'd from both a Thread mutator
   completing plans (JITWorklist::completeAllReadyPlansForVM) and the
   compiler-thread plan teardown. Real refcount-corruption shape, not
   advisory data: make DeferredCompilationCallback `ThreadSafeRefCounted`
   (runtime/DeferredCompilationCallback.h). Cold path; flag-off codegen
   impact nil.

### r5 family table

Counts after the same manual re-binning discipline as previous rounds
(census in §14.b). 763 total; reconciles exactly.

| # | id | r4 | r5 | ruling | delta notes |
|---|----|----|----|--------|-------------|
| 1 | value-profile | 64 | 52 | relaxed-atomic | iterator try-fast metadata STILL unconverted (2nd consecutive wave): operationIteratorNextTryFast self/x-ByteCodeParser (40+), handleIteratorOpen (5+); plus CompressedLazyValueProfileHolder ConcurrentVector segment-alloc echo x BytecodeIndex::hash (1). The op_iterator_open/next Metadata profile slots in runtime/CommonSlowPaths.cpp + dfg/DFGByteCodeParser.cpp readers remain the whole family |
| 2 | exec-counter | 75 | 11 | relaxed-atomic | §13.1 CAS item VERIFIED CLOSED (67 -> 0). Residual = the two small items unassigned since r3: countReoptimization x reoptimizationRetryCounter (8, CodeBlock.h m_reoptimizationRetryCounter), tierUpCommon x compilationDidBecomeReadyAsynchronously (3, DFG tier-up flag bytes) |
| 3 | array-profile | 0 | 0 | done | holds |
| 4 | ic-stubinfo | 47 | 41 | lock | epoch acceptance test FAILED for the THIRD time: 18 x-malloc stub-reuse pairs (ICSlowPathCallFrameTracer 12, considerRepatchingCacheImpl 6) — the JIT §4.4 retired-artifact epoch audit has now missed three waves; PropertyInlineCache ctor-publication x identifier reads (11); resolve_scope HALF-fixed: reader now Atomic<ResolveType>::load but the LLInt writer slow_path_resolve_scope (CommonSlowPaths.cpp:1403) still plain (10); disarmClearingWatchpointOnRetire-class singles (2) |
| 5 | code-lifecycle | 9 | 3 | concurrent-accessor | capabilityLevel/replacement keys gone. Residual: ConcurrentJITCodePtr ctor-publication (2, unchanged since r3); NEW: DeferredCompilationCallback plain-RefCounted deref x deref (1) — make ThreadSafeRefCounted (headline 6) |
| 6 | watchpoints | 18 | **0** | **done** | §12.6 inflateSlow release-publication fix verified |
| 7 | cell-header | 0 | 0 | done | holds |
| 8 | arith-profile | 0 | 0 | done | holds |
| 9 | structure-fields | 43 | 34 | concurrent-accessor | StructureChain::create writer side fixed (finishCreation now atomic store) but the READER side not converted: Structure::isValid plain StructureID loads x finishCreation atomic store (6, StructureInlines.h:234); enumerator-cache cluster persists (slow_path_enumerator_next 9, JSPropertyNameEnumerator ctor 1, Box<InlineWatchpointSet> 1); VectorBuffer<StructureID> x malloc (12); Atomic<u8> ctor singles (2); misc (3) |
| 10 | jsvalue-slots | 14 | 8 | relaxed-atomic | SparseArrayValueMap/GetterSetter keys closed. Residual: tryGetIndexQuicklyConcurrent x trySetIndexQuicklyConcurrent (6 — the concurrent indexed accessors still touch one plain word; finish the pair), Register pairs (2) |
| 11 | codeblock-init | 14 | 18 | relaxed-atomic | unfixed, slight growth: UnlinkedMetadataTable::link one-sided (7), isInStrictContext x recordParse (3), livenessAnalysis lazies (2+2), ensureCatchLiveness self (2), CodeBlock ctor singles (2) |
| 12 | metaallocator-stats | 0 | 0 | done | holds |
| 13 | linkbuffer-stats | 0 | 0 | done | holds |
| 14 | simple-stats | 0 | 0 | done | holds |
| 15 | butterfly-words | 41 | 38 | concurrent-accessor | writer side STILL plain (3rd wave): ensureLengthSlowConcurrent (13), FreeCell::makeLast free-list (9), createInitialIndexedStorageConcurrent x Atomic readers (4), createOrGrowPropertyStorage memcpy x decodeConcurrent (1), shiftCountWithArrayStorageConcurrent memcpy x SparseArrayValueMap atomic load (2, new key — same class: bulk memcpy in *Concurrent writers must go through the tagged-word/fragment atomic stores or a TSAN-visible element loop), trySegmentedTransition (1), misc (8) |
| 16 | tinybloom | 0 | 0 | done | holds |
| 17 | rope-stringimpl | 123 | 57 | relaxed-atomic + RELEASE publication | §13.4-adjacent progress (one-sided String::String keys halved) but the §11.17 publication item is STILL open: unalignedLoad x copyElements (22) — concurrent equal/hash readers racing the resolveRope copyElements writer; equalCommon/StringView/parseIndex x malloc echoes (17); resolveToBuffer (3); misc (15). The convertToNonRope placement-new publication remains the fix |
| 18 | regexp-shared | 0 | 0 | done | holds |
| 19 | alloc-profile | 0 | **2** | relaxed-atomic | **REOPENED** (headline 5): ObjectAllocationProfile initializeProfile/setPrototype never converted; files bytecode/ObjectAllocationProfile*.h |
| 20 | calllink | 31 | 5 | concurrent-accessor | big win (capabilityLevel/noticeIncomingCall/specializationKind keys closed). Residual: CallLinkInfoBase ctor-publication x removeOnDestruction (3+1), linkIncomingCall x MicrotaskCall::relink (1) |
| 21 | gc-marking-residual | 22 | 4 | relaxed-atomic | IsoCellSet addSlow publication + consume-load (§13.5) verified closed. Residual is EXACTLY the suppressions-file B5 family: AbstractSlotVisitor::visitCount plain read (MarkingConstraintSolver::didVisitSomething, converge) x appendToMarkStack increments (4). Convert m_visitCount/m_bytesVisited to relaxed Atomic (do NOT suppress; B5's error-direction caveat applies) |
| 22 | vm-string-caches | 0 | 0 | done | holds |
| 23 | misc-tail | 156 | 153 | suppress | FOURTH consecutive wave not suppressed — and the right call is now to KEEP it that way: r5 confirms these one-sided allocator echoes (malloc 107, aligned_alloc 41, realloc 5) are pure duplicates of the open parents (9/15/17/24/26 — test attribution matches: rope/butterfly/structure/TA-heavy tests dominate). A race:malloc-class entry would be a rule-4 blanket suppression masking real reports. DECISION: no suppression entries; the family shrinks to zero when its parents close (was 156 -> 153 with no action; parents shrank likewise) |
| 24 | typedarray-sab | 91 | 89 | concurrent-accessor | fix STILL not landed in substance (3rd wave): CagedPtr pairs (33), ArrayBufferContents::sizeInBytes (18), zeroFill/getData/fill raw lanes (15), view ctor publication x byteOffsetRaw/lengthRaw (9), get/setIndexQuickly (4), transferTo/misc (10) |
| 25 | property-table | 32 | **0** | **done** | §13.2 bump-after-mutation seqlock verified |
| 26 | block-directory-bits | 133 | **149** | relaxed-atomic | **THIRD no-op wave; now the largest code family.** Identical single key. The fix is ~8 lines in WTF/wtf/FastBitVector.h FastBitReference::operator= (:436): relaxed atomic fetch_or/fetch_and on the word (threads/TSAN-gated path is acceptable per §13.4 precedent ONLY if recorded; prefer unconditional relaxed — codegen-identical on x86/arm64). MUST be the first item of wave 6 with an explicit owner |
| 27 | gc-incoming-ref | 0 | 0 | done | holds |
| 28 | barrier-counter | 65 | 61 | relaxed-atomic | THIRD no-op wave: Heap.cpp:1447 `m_barriersExecuted++` verbatim unchanged. r5 anchor frame is Heap::addToRememberedSet (inlining change) — NOT a new family (headline 4). One-line relaxed Atomic increment, Heap.h:1377 + Heap.cpp:1447/2333 |
| 29 | date-cache | 0 | 0 | done (TSAN) | holds (date-cache-churn.js's 22 r5 reports all bin to 17/23 — string/alloc echoes, no DateCache frames) |
| 30 | microtask-queue | 0 | 0 | done | holds |
| 31 | directarguments | 22 | 25 | concurrent-accessor | unfixed (3rd wave), slight growth: DirectArguments ctor x isMappedArgumentInDFG (6+3), modifiedArgumentDescriptor get/set (4), JSFunction ctor x executable (3), overrideThings/misc (9) |
| 32 | symbol-registry | 0 | 0 | done | holds (symbol-registry-cross-thread.js's 6 reports bin elsewhere; no WeakGCMap frames) |
| 33 | vm-shared-misc | 10 | 11 | lock | THIRD no-op wave: updateSoftReservedZoneSize self (7) / x updateStackLimits (2), ensureTerminationException x isTerminationException (1), LazyProperty getInitializedOnMainThread x setMayBeNull (1). mc-init-lazy-global-first-touch.js did NOT abort this round; mc-lock-cow-materialize-race.js DID (exit 134, new) — keep the crash linkage under watch |
| 34 | structure-cache | 0 | 0 | done | holds |
| 35 | arraybuffer-wrapper | 0 | 0 | done | holds |
| 36 | function-ctor-cache | 0 | 0 | done | holds |
| 37 | regexp-compile-state | 2 | 2 | relaxed-atomic | THIRD round reported, fix never landed: compileHoldingCellLock x specificPattern + isValid x YarrPattern ctor. One-line relaxed accessor pair on RegExp.h m_specificPattern (+ release-publish the YarrPattern if isValid can see it mid-ctor) |

Total: 763. unsuppressedCount = **763** (nothing suppressed; zero CLoop
frames; the §0 CLoop standing ruling required no entries for the fifth
consecutive round).

### 14.b Re-binning census (r5)

- `('data race','malloc')` 107 + `('data race','aligned_alloc')` 41 +
  `('data race','realloc')` 5 = 153 -> family 23 (one-sided allocator
  echoes, §10.b/§12.b discipline).
- `Heap::addToRememberedSet x addToRememberedSet` (61) -> family 28: both
  stacks land on Heap.cpp:1447:23 `m_barriersExecuted++` (verified against
  raw reports); the addToRememberedSet anchor is an inlining artifact of
  writeBarrierSlowPath, not the remembered-set bitvector.
- `StructureID::operator bool x Atomic<unsigned int>::store` (6) -> 9:
  reader = Structure::isValid (StructureInlines.h:234) walking
  StructureChain entries; writer = StructureChain::finishCreation's NEW
  atomic store (the half-landed fix).
- `VectorBufferBase<StructureID>`/`adopt` x malloc (12), `Atomic<unsigned
  char>::Atomic` (2), `Box<InlineWatchpointSet>` (1) -> 9 (§12.b precedents).
- `parseIndex x malloc` (3) -> 17; `ConcurrentJITCodePtr` ctor (2) -> 5;
  `RefCountedBase::derefBase` self (1) -> 5 (DeferredCompilationCallback,
  headline 6).
- `shiftCountWithArrayStorageConcurrent` memcpy (2), `trySegmentedTransition`
  (1), `decltype x decodeConcurrent` vs createOrGrowPropertyStorage memcpy
  (1) -> 15 (concurrent-storage bulk-copy writer class).
- `BytecodeIndex::hash x decltype` (1) -> 1 (CompressedLazyValueProfileHolder
  ConcurrentVector segment allocation).
- `linkIncomingCall x MicrotaskCall::relink` (1) -> 20 (§12.b precedent).
- classifier `obj-alloc-profile` (2) -> 19; `remembered-set` (61) -> 28;
  `strings-ropes` (54) -> 17; `heap-misc` (13) -> 9 (12) + 5 (1).
- Reconciliation: 153+149+89+61+57+52+41+38+34+25+18+11+11+8+5+4+3+2+2 = 763.

### r5 run-health notes

- 224 files: 218 ran, 6 `//@ skip` (same set). Exit codes: 97 exit 66
  (races reported), 96 exit 0 (clean under suppressions — up from 75), 19
  exit 3, 5 exit 134, 1 timeout (semantics/proto-cycle-race.js, exit 124,
  0 reports — KNOWN-FAILING; it also timed out in r0).
- Aborts (exit 134, 5 — was 4): cve/mc-hand-restrict-claim.js and
  heap-iss-revert.js (repeat every round), gc-stress/
  havebadtime-vs-indexed-fastpath.js (KNOWN-FAILING),
  cve/mc-lock-cow-materialize-race.js (NEW this round — first abort; watch),
  scaling/richards-like.js (recurrence of an r3 abort). r4's
  mc-init-lazy-global-first-touch.js abort did NOT recur.
- KNOWN-FAILING tests ran; reports counted (date-cache-churn 22,
  symbol-registry-cross-thread 6, havebadtime-vs-indexed-fastpath 4,
  proto-cycle-race 0).
- Wave-6 partitioning (disjoint files): 26 (WTF/wtf/FastBitVector.h —
  FIRST, named owner, 149 reports for ~8 lines), 28 (heap/Heap.h+Heap.cpp
  one-liner), 24 (runtime/JSArrayBufferView*/ArrayBufferContents...), 17
  (runtime/JSString.* convertToNonRope publication), 1 (CommonSlowPaths.cpp
  iterator metadata + bytecode/Metadata access), 4 (epoch audit re-do +
  slow_path_resolve_scope writer, CommonSlowPaths.cpp:1403 +
  bytecode/PropertyInlineCache.*), 15 (runtime/JSObject.cpp/JSArray.cpp/
  ButterflyInlines.h concurrent writers), 9 (StructureInlines.h reader +
  enumerator cache), 31 (runtime/DirectArguments.*), 11
  (bytecode/UnlinkedMetadataTable.h + ScriptExecutable.h), 2
  (bytecode/CodeBlock.h retry counter + DFG tier-up bytes), 33
  (runtime/VM.cpp/VM.h), 21 (heap/AbstractSlotVisitor.h visitCount), 19
  (bytecode/ObjectAllocationProfile*.h), 37 (runtime/RegExp.h), 10
  (runtime/JSObject.cpp concurrent index pair — coordinate with 15's owner,
  same file: merge into one fixer), 5 (DeferredCompilationCallback.h
  ThreadSafeRefCounted + ConcurrentJITCodePtr ctor), 20
  (bytecode/CallLinkInfo.*). 23: NO suppression entries (see table) — it
  closes via its parents.

## 15. Waves 6-9 (single-runner campaign continuation, 2026-06-09)

Binary config re-verified each round (`ENABLE_C_LOOP:BOOL=OFF`,
`ENABLE_JIT:BOOL=ON` in `WebKitBuild/TSan/CMakeCache.txt`; binary mtime >
newest source mtime before every run; smoke.js 3x clean before r6). Zero
CLoop frames in any round (CLoop not compiled in; standing ruling required
no entries for the 6th-9th consecutive rounds).

Report trajectory: r5 = 763 -> r6 = 246 -> r7 = 182 -> r8 = 74 -> r9 (final,
see §16 and docs/threads/TSAN-RESULTS.md).

### Wave 6 (fixes verified by r6; 763 -> 246)

- f26 block-directory-bits (149 -> ~0): WTF/wtf/FastBitVector.h
  FastBitReference::operator= and operator bool() converted to relaxed
  atomic load/store (writers hold the owner's lock; lost updates impossible;
  plain-mov codegen); BlockDirectoryBits.h const word() read relaxed.
- f28 barrier-counter (61 -> 0): Heap.cpp:1447/2333 m_barriersExecuted relaxed
  load+store (stats counter, lost updates tolerated; §5.7.7).
- f24 typedarray-sab (89 -> ~20): WTF::CagedPtr storage word fully
  relaxed-atomic (loadStorageRelaxed + atomic assignment); ArrayBufferContents
  m_sizeInBytes relaxed everywhere incl. ctors; zeroFill + transferTo/copyTo
  data-lane copies TSAN-gated byte-atomic (copyDataLanesRacy; §13.4-style gate,
  recorded); JSDataViewPrototype get/setData byte lanes relaxed.
- f17 rope-stringimpl (57 -> ~16): fiberConcurrently TSAN-gated ACQUIRE
  (production keeps relaxed + dependency ordering; TSAN cannot model consume —
  gate recorded); JSString::equal/JSRopeString::view routed through
  getValueImpl (wave 7).
- f1 value-profile (52 -> ~3): op_iterator metadata seenModes via the
  CommonSlowPaths concurrent merge/load helpers at the 4 JITOperations.cpp
  writers + 2 DFGByteCodeParser readers; CompressedLazyValueProfileHolder
  m_data release-publish + acquire dataConcurrently(); WTF::ConcurrentVector
  (m_size/m_numSegments/segment slots) and WTF::ConcurrentBuffer (m_array,
  Array::size) converted to relaxed/release atomics per their single-writer
  + lock-free-reader contracts.
- f4 ic-stubinfo (41 -> ~30): slow_path_resolve_scope (+JIT/LOL/CodeBlock
  twins) m_resolveType writers relaxed (readers were already atomic). The
  retired-artifact epoch portion remains OPEN (see §16).
- f15 butterfly-words (38 -> ~2): FreeCell::makeLast/setNext/decode relaxed
  atomics on the link word; ensureLengthSlowConcurrent bulk copy ->
  TSAN-visible relaxed word loop (flag-on-only path); createOrGrow/shiftCount
  bulk copies via butterflyConcurrentCopyWords/ZeroWords (TSAN-gated);
  createInitialIndexedStorageConcurrent old-spine reads via the V7 getters.
- f9 structure-fields (34 -> ~12): Structure::isValid chain-lane loads
  relaxed; m_enumeratorMetadata profiling byte relaxed at all 8 accesses;
  JSPropertyNameEnumerator flags/ctor stores relaxed;
  StructureTransitionTable isUsingSingleSlot/map relaxed + setMap release.
- f31 directarguments (25 -> 0): DirectArguments ctor relaxed stores +
  internalLength() relaxed reads everywhere; overrideThings fill-before-publish
  (the RESOLVED-3 companion) with relaxed per-bit stores; modified-arguments
  per-bit accesses relaxed; JSFunction m_executableOrRareData fully atomic
  (ctor relaxed store, release publish, relaxed reads).
- f11 codeblock-init (18 -> ~6): UnlinkedMetadataTable::link zeroing
  TSAN-gated byte-atomic; ScriptExecutable m_lexicallyScopedFeatures /
  m_hasCapturedVariables pulled out of bit-fields to dedicated relaxed-atomic
  bytes (recordParse/readers); UnlinkedCodeBlock liveness release-publish +
  acquire; OpCatch m_buffer release-publish + acquire readers. KNOWN RESIDUAL:
  UnlinkedFunctionExecutable recordParse stays a bit-field write (class is
  size-capped at 96 bytes; dedicating the three fields grew it to 104 and the
  static_assert fired) — 1-2 reports/round, documented at the site.
- f2/f5/f10/f19/f20/f21/f33/f37 small items: reoptimizationRetryCounter
  relaxed; tier-up trigger byte relaxed at all C++ sites;
  DeferredCompilationCallback -> ThreadSafeRefCounted; ConcurrentJITCodePtr
  ctor atomic null-init; concurrent double lanes in try{Get,Set}IndexQuickly-
  Concurrent relaxed; ObjectAllocationProfile m_allocator relaxed stores;
  AbstractSlotVisitor m_visitCount/m_bytesVisited relaxed (B5 fixed in code,
  NOT suppressed); VM m_currentSoftReservedZoneSize lock + relaxed,
  m_terminationException create-once lock + relaxed reads; LazyProperty
  m_pointer atomic (release publish in setMayBeNull); RegExp
  m_specificPattern/m_constructionErrorCode relaxed with local-parse
  discipline in the three locked recompile bodies.

### Wave 7 (verified by r7; 246 -> 182)

- JSString::equal + JSRopeString::view plain valueInternal().impl() reads ->
  getValueImpl (rope-stringimpl readers).
- MicrotaskCall m_addressForCall/m_codeBlock/m_numParameters atomic with
  WRITE codeBlock -> RELEASE entry ordering on both publish paths
  (prepareForMicrotaskCall + unlinkOrUpgradeImpl); reader acquires entry
  then reads codeBlock.
- FunctionExecutable::ensureRareDataSlow: REAL BUG fixed — creation was
  completely unserialized (two Threads could both create RareData; the
  second unique_ptr assignment deleted the loser while its caller was
  writing into it). Now cellLock()-serialized, release-published, acquire
  fast path (rareDataConcurrently()).
- BlockDirectory all three next-directory link words relaxed (incl. ctor
  body atomic null-init); PropertyTable all three ctors store header words
  via concurrentRelaxedStore (recycled-cell pairing); CagedPtr ctor stores
  atomic; ConcurrentBuffer Array::size atomic; StructureChain::head +
  loadLaneConcurrently + enumerator-cache chain walk relaxed;
  llint_slow_path_profile_catch m_buffer acquire; ensureLengthSlowConcurrent
  dst stores atomic; JSPropertyNameEnumerator remaining ctor inits atomic.

### Wave 8 (verified by r8; 182 -> 74)

- SUPPRESSION ADJUDICATION: the 152-report "atomic probe vs allocator
  re-hand-out" class (84% of r7) suppressed via the dedicated concurrent-
  accessor anchors — full justification, eligibility rule and masking
  tradeoff recorded in Tools/tsan/suppressions.txt (wave-7 block). The class
  is design-blessed (OM GT staleness) and has NO plain access of ours left:
  the reported "plain" side is the TSAN allocator interceptor's synthetic
  whole-block write. PLAIN-access-vs-allocator pairings are deliberately NOT
  covered (they include the ic-stubinfo real-bug suspect).
- CachedCall sibling of the MicrotaskCall fix (m_addressForCall acquire/
  release, m_numParameters relaxed); MegamorphicCache::bumpEpoch -> atomic
  RMW (a LOST bump is a missed invalidation — deliberately not the relaxed
  load/store shape); PutByStatus::computeFromLLInt metadata StructureID
  loads relaxed.

### Wave 9 (verified by r9)

- WTF::Thread m_didExit / m_isJSThread pulled out of the packed bit-field
  run to dedicated relaxed-atomic bytes (the suppressions-file Part A item 5
  "header-side pass", same shape as m_gcThreadType).
- ConcatKeyAtomStringCache publication annotated with
  TSAN_ANNOTATE_HAPPENS_BEFORE/AFTER (the pointer is baked as a constant
  into JIT'd code; the real edge is code installation, which TSAN cannot
  model; spine tsanPublish/tsanConsume precedent).

### tsan-deep CLoop config addendum (SCAN-TSAN-REVERIFY, 2026-06-18)

- **WTF::WeakRandom::advance** (tsan-deep r0: 268 GIL-off / 263 GIL-on
  reports; both stacks anchor at advance()): VM-level WeakRandom shared
  across JS Threads at safepoint-jitter sites. Non-semantic value (jitter
  only); torn/lost-update state degrades to a different random seed. §15
  relaxed-atomic class fix: `advance()`/`setSeed()` access `m_low`/`m_high`
  via `__atomic_{load,store}_n(__ATOMIC_RELAXED)` over the existing plain
  `uint64_t` storage (Source/WTF/wtf/WeakRandom.h). Fields stay plain so
  `lowOffset()`/`highOffset()` and the manually-inlined JIT codegen are
  unchanged. NOT suppressed (suppressions.txt rule 1: interleaving is
  threads-introduced, not pre-existing upstream).
- **TSAN/CLoop build break** (`icConcurrentRelaxedLoad` undeclared in
  ENABLE_JIT=OFF): resolved by lifting the helper templates outside the
  `#if ENABLE(JIT)` guard in PropertyInlineCache.h:46-66. r1 re-run staged
  via `Tools/threads/scan/tsan-deep/r1-reverify.sh`.

### Normal-build sanity notes (waves 6-9)

- Debug GIL-off smoke: 10/10 pass.
- Debug GIL-off full corpus: 179/218 pass. The non-passing tail decomposes
  as: 10 bench/* files (reference reportBench; only runnable under
  bench-gate.sh — pre-existing), ~8 exit-3 tests already failing in the r5
  baseline TSAN runs (api/blocking-gate, sync/condition-wait-notify,
  objectmodel/i03-single-threaded-no-change, heap-epoch-reclaim, ...),
  KNOWN-FAILING entries, heap-iss-revert (aborts every round since r0), a
  Debug-only assert tail in cve/mc-* race tests (TOCTOU-style ASSERTs that
  re-evaluate racy predicates, e.g. ASSERT(isMappedArgument(i)) inside
  getIndexQuickly — invisible to the NDEBUG TSAN binary in every round), and
  TWO unguarded-section count tests (lifecycle/create-basics counters++ and
  arrays/push-resize-multithread's UNGUARDED arm) whose comments explicitly
  encode GIL-era atomicity ("atomic under the GIL, so still exact") — they
  pass GIL-ON deterministically (3/3) and their LOCK-GUARDED arms pass
  GIL-OFF; under true parallelism the unguarded arms are genuinely racy and
  began failing when waves 6/7 shifted spawn/exec timing. Queued for the
  test-semantics fix round, not an engine regression (evidence: GIL-on 8/8,
  serial 8/8, locked arm exact).

## 16. Final gate record (r10, 2026-06-09)

- TSAN final corpus run (current binary, STT single-snapshot included):
  **55 unsuppressed reports** (target 0 NOT reached; honest residual).
  Run health: 218 ran (6 skip), 159 exit 0, 28 exit 66, 26 exit 3
  (pre-existing tail), 4 exit 134 (heap-iss-revert + richards-like
  recurrent aborters, havebadtime KNOWN-FAILING, mc-val-multislot-clone),
  1 timeout (mc-grow-buffer-storm). Zero CLoop frames.
- Suppressions audit: all active entries carry justification comments —
  six rule-1 upstream parallel-GC entries (waves 0-3) + the wave-7
  recycled-allocation adjudication block (justification, eligibility rule
  and masking tradeoff inline). No CLoop entries (none ever needed). No
  entry without a justification was found.
- Residual decomposition, fixed-family ledger, normal-build sanity and the
  bench-gate result (7/8 within 1%; transition-heavy-constructor +3.9%
  carrying the parked V5b residual) are recorded in
  docs/threads/TSAN-RESULTS.md.

## 17. The §4.4 retired-artifact epoch audit (residual 1; closed 2026-06-09)

The audit chartered in §10.4 (missed by waves 3/4/5, left as TSAN-RESULTS
residual 1) is done. Question on the table: are the ~33 ic-stubinfo family
reports (a) consume-style publication TSAN cannot model on epoch-protected
memory, or (b) PICs/stubs freed while in-flight JIT'd code can still enter
slow paths with the stale pointer (real UAF)?

### 17.1 Verdict: (a), with proof — and the proof is stronger than "epoch"

The single load-bearing fact (RetiredJITArtifacts.cpp,
`epochCoversEveryJSThread`): **flag-on, every RetiredJITArtifacts path
LEAKS** — the epoch facility's per-client granularity cannot yet prove a
parked sibling thread crossed the epoch, so `retireHandlerChain`,
`retireOptimizedJITCode` and `retire` all take the chartered
leak-until-integration arm under `useJSThreads`. Quarantine is therefore
not "freed after epoch+1" but "never freed". Any genuine premature-free
would have to come from a dealloc path that BYPASSES RetiredJITArtifacts.

### 17.2 Dealloc-path enumeration (every PIC/stub/handler/CallLinkInfo path, GIL-off)

| # | artifact | dealloc path | flag-on disposition |
|---|---|---|---|
| 1 | handler chain head | displaced by `initializeWithUnitHandler` (megamorphic promotion) | `retireHandlerChain` (leak) |
| 2 | handler chain | `resetStubAsJumpInAccess` (watchpoint/GC resets, world-stopped) | `retireHandlerChain` (leak) |
| 3 | handler chain | `prependHandler` | no displacement — old chain stays reachable via new head's `m_next` |
| 4 | inlined unit handler | displacement in 1/2 | `retireHandlerChain` (leak) |
| 5 | handler chain + inlined handler | jettison / `~CodeBlock` via `PropertyInlineCache::deref(VM&)` | EXTRA epoch ref + `retireHandlerChain` (leak); `~PropertyInlineCache` itself never runs flag-on (container leaked, rows 7-8) |
| 6 | `RepatchingPropertyInlineCache::m_stub` | `deref(VM&)` | `RetiredJITArtifacts::retire` (leak); unreachable flag-on anyway (I3) |
| 7 | `DFG::JITData` (ButterflyArray with the HandlerPropertyInlineCaches + OptimizingCallLinkInfos) | `~CodeBlock` | flag-off `delete`; flag-on LEAK + `clearWatchpoints()`, `m_jitData` kept intact (SPEC-jit §5.3/I7). CORRECTION (post-closeout review): the arm previously nulled `m_jitData` UNCONDITIONALLY before the flag split — but every fresh entry into the leaked DFG code re-binds the field from the cell (`compileSetupRegistersForEntry`'s `loadPtr(offsetOfJITData)` + `compileEntryExecutionFlag`'s `store8(offsetOfNeverExecutedEntry)`, and the unlinked OSR-exit dispatch's exit-vector `farJump`, DFGJITCompiler.cpp), so the exact straggler the leak exists for performed a near-null store8/load and the kept-alive JITData was unreachable. Now mirrors row 8's AB18-B field-keeping discipline; flag-off still nulls then deletes |
| 8 | `BaselineJITData` | `~CodeBlock` | flag-off `delete`; flag-on LEAK, `m_jitData` kept intact (AB18-B) |
| 9 | `DFG::JITData` pre-publication | `JITData::tryCreate`/`tryInitialize` failure (DFGPlan.cpp:822) | inline free of a never-published object; single-thread-visible, sound |
| 10 | handler pre-publication | `compileHandler`/`addAccessCase` failure arms | inline free pre-publication; sound |
| 11 | optimized `JITCode` (+CommonData CallLinkInfos) | `~CodeBlock` -> `retireOptimizedJITCode` | flag-on LEAK (I7: epoch must never free machine code) |
| 12 | §5.8 call-link records | replace/unlink -> `retireCallLinkRecord` | `RetiredJITArtifacts::retire` (leak) |
| 13 | §5.8 record at `~CallLinkInfo` | inline `delete m_record` | sound AFTER the row-16 fix: delisted under `s_callLinkSerializationLock` first, owner code unreachable. CORRECTION (closeout final review): the original claim that this "only runs flag-off / for never-published CLIs" was FALSE — published DataOnlyCallLinkInfos embedded in op_call et al. metadata had their destructors run by the owner's OWN `~CodeBlock` via the `m_metadata` RefPtr release -> `MetadataTable::destroy` -> per-op metadata destructors, while the AB18-B straggler premise says a sibling can still be mid-call-op. Row 16 closes that path; with it, flag-on `~CallLinkInfo` again runs only for never-published CLIs / heap-shutdown |
| 14 | `DataOnlyCallLinkInfo` in `InlineCacheHandlerWithJSCall` | dies with its handler | rows 1-5 (leak) |
| 15 | `m_bufferedStructures` Vector buffers | reassigned under `m_bufferedStructuresLock` inside the live PIC | container rows 7-8 (leak); lock covers writer-writer |
| 16 | linked `MetadataTable` (embedded `DataOnlyCallLinkInfo`s + their §5.8 records + the metadata block the baseline prologue's `loadPairPtr(offsetOfMetadataTable, ...)` reloads) | `~CodeBlock` member `RefPtr<MetadataTable>` release -> `MetadataTable::destroy` -> per-op `~Metadata` -> `~DataOnlyCallLinkInfo` inline `delete m_record` + `unlinkedMetadata->unlink()` frees the block | WAS A BYPASS — the AB18-B residual (b) left open. Fixed at the closeout final review: flag-on, `~CodeBlock` now ref-escapes `m_metadata` (leak), keeping the table, the embedded CLIs and their records alive; field bits stay intact for prologue reloads, mirroring rows 7/8. The leaked CLIs stay on callees' `m_incomingCalls` lists — same accepted state as rows 7/8/11 |
| 17 | the dead `CodeBlock` CELL itself (the IsoSubspace slot whose `m_jitData`/`m_metadata` field bits rows 7/8/16 keep intact for straggler prologue reloads) | lazy allocation-path sweep -> `CodeBlock::destroy` -> slot to freelist -> re-hand-out (IsoSubspace type stability: new occupant is another CodeBlock with DIFFERENT m_jitData/m_metadata) | POST-CLOSEOUT FIX: the field-keeping premise of rows 7/8/16 held only until slot recycling, and recycling under a live straggler was REACHABLE — `~CodeBlock` (and with it `unlinkOrUpgradeIncomingCalls`) runs on an allocation-path sweep AFTER the world resumes (IncrementalSweeper disabled in shared mode, deviation 4), so a sibling could call through a still-linked CallLinkInfo post-resume, acquiring the dead cell pointer AFTER the conservative scan (hence unpinned), then race the sweep + re-hand-out and bind the prologue `loadPairPtr` to the NEW occupant's fields (silent wrong-metadata). Fixed: `CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks` now unlinks every dead block's incoming calls flag-on, in the End phase, world-stopped (precedent: row 2). With the CLI acquisition window closed, any thread still able to enter a dead block post-resume must have held the pointer at scan time — which conservatively marks the cell, contradiction — so dead-cell entry via call links is impossible and rows 7/8/16 are defense-in-depth for non-CLI vectors. This also RE-SCOPES the §18 residual-2 "recycled under live prober" adjudication (vmConcurrentProbe): a slow_path_enter prober's frame pins its CodeBlock conservatively, so real temporal overlap with re-construction is excluded; the annotation models the sweep/freelist hand-out edge TSAN cannot see, not a genuine concurrent read |

| 18 | OpCatch `ValueProfileAndVirtualRegisterBuffer` (`metadata.m_buffer`; release-published in `ensureCatchLivenessIsComputedForBytecodeIndexSlow`, acquire readers in `llint_slow_path_profile_catch`, `operationTryOSREnterAtCatchAndValueProfile`, `validate`/`finalizeUnconditionally` — §18.2) | `~CodeBlock` baseline arm: explicit `forEach<OpCatch>` `ValueProfileAndVirtualRegisterBuffer::destroy` (the only dealloc site in the tree) | WAS A BYPASS (post-closeout review): the destroy ran unconditionally, freeing a published artifact reachable from exactly the metadata the row-16 ref-escape in the SAME destructor leaks for straggler prologues — a straggler that throws and lands in op_catch reads the freed buffer. Same contradiction shape as row 16; covered by the row-17 CLI-closure argument only for CLI vectors, and rows 7/8/16 deliberately retain their leaks as defense-in-depth for non-CLI vectors. Fixed: flag-on, the destroy is skipped (buffer leaks with the ref-escaped metadata, CodeBlock.cpp); flag-off byte-identical |

AMENDED VERDICT (closeout final review): the original "no bypass found"
conclusion was FALSIFIED by row 16, which this table omitted — the
MetadataTable teardown in `~CodeBlock` freed published DataOnlyCallLinkInfos,
their §5.8 records, and the metadata block itself inline flag-on, on exactly
the straggler window the AB18-B jitData leak in the SAME destructor exists
for (the tree simultaneously asserted the straggler exists, to justify
leaking m_jitData, and that it doesn't, to justify freeing metadata). With
the row-16 ref-escape landed, the enumeration is again exhaustive: every
deallocation of a *published* artifact routes through RetiredJITArtifacts
(leak) or is leaked in `~CodeBlock` (rows 7/8/11/16/18) or is unreachable
flag-on. (b) is refuted for the CURRENT (fixed) tree, not for the tree the
original audit signed off on. Re-audit all 18 rows when
epochCoversEveryJSThread starts returning true flag-on.

SECOND AMENDMENT (post-closeout review): two further corrections landed
after the amended verdict above. (1) Row 7's disposition was misrecorded
AND the code was wrong — the DFG arm of `~CodeBlock` nulled `m_jitData`
unconditionally, defeating the leak it sits next to (see row 7's
CORRECTION); fixed to mirror row 8. (2) Row 17 was added: the recycled-cell
hazard the residual-2 adjudication admits ("a CodeBlock cell recycled
across Threads is re-constructed while a stale prober still reads the
words") CONTRADICTED the rows-7/8/16 "field bits stay intact" premise, and
the contradiction was real, not apparent — dead blocks stayed CLI-linked
until the post-resume lazy sweep, so a straggler could acquire the dead
pointer after the conservative scan and outlive the slot's re-hand-out.
Closed by the End-phase (world-stopped) incoming-call unlink in
CodeBlockSet (row 17). Both corrections feed the r18 re-baseline
obligation recorded in TSAN-RESULTS.md.

THIRD AMENDMENT (final closure review, 2026-06-09): row 18 added — the
baseline arm of `~CodeBlock` still inline-freed the published OpCatch
`ValueProfileAndVirtualRegisterBuffer` flag-on, the one published-artifact
dealloc in that destructor left outside the rows-7/8/16 leak discipline and
absent from the table the amended verdict re-declared exhaustive. Fixed by
flag-on-gating the destroy (buffer leaks with the row-16 ref-escaped
metadata); flag-off byte-identical. Also corrected: the
DisarmClearingWatchpoints amendment sentence in TSAN-RESULTS.md misstated
the reset path as passing No — the code passes (and must pass) Yes at the
world-stopped `resetStubAsJumpInAccess` republish (the reset DISPLACES the
chain; see the corrected sentence in TSAN-RESULTS.md for the fire-time-UAF
rationale).

### 17.3 Why TSAN reports it anyway

Re-examination of all 29 family reports in r10 (13 ICSlowPathCallFrameTracer
x malloc, 3 considerRepatchingCacheImpl, 6 VectorBuffer/variant
<StructureID>, 7 InlineCacheHandlerWithJSCall ctor/CallLinkInfo pairs): in
EVERY report the "previous write by T_alloc: malloc/__tsan_memset" stack is
the SAME allocation that still owns the block ("Location is heap block ...
allocated by" with an identical stack) — these are NOT re-hand-out pairs.
The publisher initializes the artifact, `storeStoreFence`s, and publishes
with one store (`m_jitData` install / `publishHandlerChainHead`); the
reader receives the pointer MATERIALIZED BY JIT'D CODE (IC stub register
state / data-IC dispatch) and enters the C++ slow path with it. TSAN can
model neither the fence-based release nor the in-JIT consume edge, so it
pairs the publisher's ctor/allocator writes with the reader's first
slow-path access. The wave-7 suppression block deliberately did not cover
these (plain reads / non-accessor anchors), which is why they survived to
r10.

### 17.4 Fix: TSAN-visible happens-before pairs at the publication choke points

Per the §13.4 precedent (ConcatKeyAtomStringCache): annotation in code, no
suppression entries, production codegen unchanged (all sites compile away
outside TSAN_ENABLED). Each site carries the row-by-row proof reference.

Release side (BEFORE):
- `publishHandlerChainHead` (PropertyInlineCache.cpp): new head + embedded
  `InlineCacheHandlerWithJSCall` CallLinkInfo address.
- `DFG::JITFinalizer::finalize` (DFGJITFinalizer.cpp): each
  HandlerPropertyInlineCache + each OptimizingCallLinkInfo, just before
  `setDFGJITData`.
- `CodeBlock::setupWithUnlinkedBaselineCode` (CodeBlock.cpp): each baseline
  PropertyInlineCache, just before `setBaselineJITData`.
- `FTL::JITFinalizer::finalize` (FTLJITFinalizer.cpp): each FTL handler
  PIC after `initializeHandlerForOptimizingJIT`.

Acquire side (AFTER):
- `ICSlowPathCallFrameTracer` ctor (JITOperations.cpp; the single ctor
  covers all 72 IC slow-path operations), keyed on the PropertyInlineCache.
- `CallLinkInfo::ownerForSlowPath` (CallLinkInfo.h; runs at the top of
  `operationDefaultCall` and friends), keyed on the CallLinkInfo.

Masking tradeoff (same as the wave-7 block, recorded here): if a FUTURE
regression introduces an inline free of a published handler/JITData, a
recycled block whose new owner is again a published artifact of the same
kind could carry a fresh BEFORE on the same key and hide the UAF from
TSAN. The guard against that is the table in 17.2 (re-audit on any change
to the rows) plus ASAN, which keeps full visibility (annotations are
TSAN-only).

### 17.5 Verification (2026-06-09, post-annotation TSAN binary)

- Build: incremental `ninja jsc` in WebKitBuild/TSan (same JIT+asm config,
  ENABLE_C_LOOP=OFF re-verified); Debug build also compiles clean
  (annotations are no-ops outside TSAN_ENABLED) and passes the GIL-off
  smoke + ic-publish-reset-loops 3/3.
- Targeted tests, GIL-off TSAN, 20x each under load (loadavg > 8):
  ic-publish-reset-loops, int-gate-direct-call-relink,
  int-gate-epoch-reclaim, int-gate-fire-vs-execute,
  int-gate-jettison-vs-execute, int-gate-stop-budget,
  mc-code-calllink-writer-writer, mc-val-fire-vs-link — **160/160 pass**.
  Six of eight at 0 reports across all 20 runs; the flicker in
  int-gate-jettison-vs-execute and mc-code-calllink-writer-writer was
  re-captured and is exclusively the residual-2 ctor-singles class
  (CodeBlock `m_vm` ctor write on a GC-recycled MarkedBlock cell /
  HeapCell::vm x aligned_alloc) — zero §4.4-family signatures.
- Full corpus snapshot **r11** (`Tools/threads/tsan/{reports,families}-r11*`,
  per-test logs in `r11/`): 224 files, 218 ran, **32 unsuppressed reports
  (r10: 55)**. §4.4 family signatures (ICSlowPathCallFrameTracer,
  considerRepatchingCacheImpl, VectorBuffer<StructureID> buffered-structure
  pairs, InlineCacheHandlerWithJSCall, setMonomorphicCallee,
  DFG::JITData / BaselineJITData ButterflyArray-block pairs): **0**.
  Run health: 168 exit 0 (r10: 159), 26 exit 3 (same pre-existing tail),
  17 exit 66, 5 exit 134 (all members of the recurrent flaky-abort set
  from earlier rounds: mc-hand-restrict-claim, heap-iss-revert,
  frozen-seal-race, no-lost-properties-same-name, havebadtime
  KNOWN-FAILING), 2 exit 124 (proto-cycle-race KNOWN-FAILING +
  mc-grow-buffer-storm, both timeouts in r10 as well). Zero CLoop frames.
- The remaining 32 decompose entirely into residuals 2 and 3
  (TSAN-RESULTS.md), no suppression entries were added, and no invariant,
  assert, or production code path was changed by this item.

## 18. Closeout mop-up to zero (residuals 2+3; rounds r12-r15, 2026-06-09)

Charter (thread-closeout ITEM 2): close residual 2 (ctor-atomicization
stragglers) and residual 3 (tail singles) — iterate full-corpus rounds until
0 unsuppressed or every remainder has a written justification. Outcome:
**r11 32 -> r12 16 -> r13 5 -> r14 1 -> r15 (final)**, with TWO REAL
LOCKING GAPS found and fixed along the way. Same binary discipline as all
prior rounds (full JIT + asm LLInt, ENABLE_C_LOOP=OFF, GIL-off env,
halt_on_error=0, suppressions file).

### 18.1 Real bugs found and fixed (locks, not blinding)

1. **FunctionExecutable::ensurePolyProtoWatchpoint** (r11 report 22) — the
   create-once was completely unserialized (same shape as the wave-6
   ensureRareDataSlow REAL BUG): two Threads racing the Box::create
   double-assigned the RefPtr; the losing assignment destroys a set the
   winner may already have handed out (watchpoints registered on a dead set
   = UAF), and the plain RefPtr write raced readers. Fixed: cellLock()-
   serialized slow path + release publication of the single pointer word;
   fast path/sharedPolyProtoWatchpoint() acquire-probe the word
   (FunctionExecutable.h/.cpp).
2. **IntlCache** (r14, cve/mc-init-lazy-global-first-touch) — the per-VM
   IntlCache is shared by every Thread (VM-lites share the parent's), and
   its locale-ID HashMap raced find/add/rehash cross-Thread (rehash vs a
   sibling's find = use-after-free), with the same exposure on the shared
   ICU UDateTimePatternGenerator (a sibling's re-cache udatpg_close()s the
   generator under a user). Fixed: one Lock over all three public entry
   points, WTF_GUARDED_BY_LOCK on the members, generator USE kept under the
   lock (IntlCache.h/.cpp).
3. **MicrotaskCall::relink / CachedCall ctor ordering** (r11 reports
   30/31/32, residual-3 "possible real locking gap" — confirmed):
   (a) relink's UNLOCKED isOnList() pre-check violated the
   removeOnDestruction contract ("the lock is taken UNCONDITIONALLY; only
   the under-lock re-check is authoritative") — now calls the locked helper
   unconditionally (MicrotaskCall.cpp). (b) CachedCall's constructor links
   the node onto the new CodeBlock's incoming-call list BEFORE
   m_protoCallFrame.init() runs, so a locked jettison drain could compare
   `codeBlock() == oldCodeBlock` against UNINITIALIZED stack garbage — the
   slot is now zeroed before the link, and ProtoCallFrame::codeBlock/
   setCodeBlock are relaxed-atomic word accesses (CachedCall.cpp,
   ProtoCallFrameInlines.h).

### 18.2 Code fixes by ruling type (production codegen identical)

**relaxed-atomic conversions** (plain access was the UB; same instruction):
- WriteBarrierBase<T>::setEarlyValue — the LAST plain writer of the cell
  slot (readers were already relaxed-atomic via cell()); 5 r12 reports
  across SparseArrayValueMap/Structure/JSObject/JSString barriers
  (WriteBarrierInlines.h).
- JSDataView m_buffer (residual-2 named): ctor relaxed store +
  possiblySharedBuffer/unsharedBuffer relaxed loads (JSDataView.h/.cpp).
- ConcurrentButterfly tryGrowSegmentedVectorLength STW copy lambda: the
  source lanes were written with relaxed atomic lane stores pre-stop; the
  copy now reads them with relaxed atomic loads (the stop handshake is the
  real HB edge TSAN cannot see) (ConcurrentButterfly.cpp:~2352).
- JSObject::classifyConcurrentLockedAdd: outOfLineFragmentCountConcurrent()
  instead of the plain spine field read (raced a foreign grower's
  butterflyConcurrentStore publication) (JSObjectInlines.h).
- MarkedBlock::Handle::isLive m_marks read -> BitSet::concurrentGet (the
  CountingLock fencelessValidate protocol already tolerates the race);
  MarkedBlock::isNewlyAllocated likewise (MarkedBlock.cpp/.h).
- HandlerInfo::nativeCode retarget (residual-2 "CatchInfo vs
  setupWithUnlinkedBaselineCode"): relaxed atomic store at the baseline
  finalization loop + relaxed atomic load in CatchInfo — old (LLInt) and
  new (baseline) entries are both valid targets, m_jitData is
  fence-published first (AB18-B) (CodeBlock.cpp:~884, Interpreter.cpp:738).
- operationTryOSREnterAtCatchAndValueProfile m_buffer read — the one OpCatch
  buffer reader the earlier wave missed; acquire load pairing the release
  publish, same as CodeBlock.cpp:3387/LLIntSlowPaths.cpp (JITOperations.cpp).

**TSAN-gated ordering upgrades** (§13.4 gate shape; production keeps the
relaxed/fence protocol, TSAN builds get the acquire/release pairing TSAN can
model):
- BlockDirectory next-link accessors: release stores / acquire loads under
  TSAN (storeStoreFence is the production edge); closes the
  ctor-vs-findEmptyBlockToSteal sextet (BlockDirectory.h).
- JSString(VM&, Ref<StringImpl>&&) ctor fiber store: release under TSAN so
  fiberConcurrently()'s TSAN acquire synchronizes; closes the
  StringView/StringImpl-content quartet (JSString.h).
- StringImpl::deref shared-atom arm: acq_rel RMW under TSAN ONLY — TSAN
  does not model std::atomic_thread_fence, so the production
  release-decrement + acquire-fence-on-zero protocol (correct) looked like
  free-vs-deref races (StringImpl.h).
- WTF::ConcurrentVector size()/segmentFor() + ConcurrentBuffer
  arrayConcurrently(): acquire under TSAN, pairing the existing release
  publishes (LazyOperandValueProfile OSR-exit family) (ConcurrentVector.h,
  ConcurrentBuffer.h).

**TSAN_ANNOTATE_HAPPENS_BEFORE/AFTER pairs** (§13.4/§17 precedent —
consume-style publication TSAN cannot model; each site carries the
justification):
- WatchpointSet ctor (HB) <-> state() / InferredValueWatchpointSet::
  inferredValue() (HA); InferredValueWatchpointSet's m_value also moved off
  the Atomic value-constructor (a plain store AFTER the base-ctor HB — the
  wave-5 lesson re-learned) to a relaxed store + re-issued HB
  (Watchpoint.cpp/.h, InferredValue.h).
- MarkedBlock::Header ctor (HB) <-> MarkedBlock::vm() (HA) — the residual-2
  "HeapCell::vm / CodeBlock::vm on recycled MarkedBlock" class.
- JSPropertyNameEnumerator finishCreation (HB) <->
  concurrentRelaxedLoad/propertyNameAtIndex (HA).
- JSDataView ctor (HB) <-> possiblySharedBuffer (HA) (precise-allocation
  cell variant of the same class).
- RegExpPrototype regExpProtoFuncTest: the plain String::isNull cell read
  replaced with the blessed isRope()/fiberConcurrently probe (equivalent
  after a successful value(); the OOM path throws and returns earlier).

### 18.3 Suppressions added (Tools/tsan/suppressions.txt)

Five wave-7-class anchors (atomic probe vs allocator hand-out; the racing
access is a dedicated relaxed-atomic accessor, the "previous write" is the
interceptor's synthetic allocation write; no plain access of ours exists):
`race:JSC::CallLinkInfo::owner`, `race:JSC::IndexingHeader::vectorLength`,
`race:JSC::StructureChain::head`,
`race:JSC::typedArrayViewProtoGetterFuncLength` (the
RacyArrayBufferViewField probe fully inlined past the existing anchor's
symbolization), and one rule-1 entry
`race:JSC::UnlinkedFunctionExecutable::recordParse` (upstream concurrent
compiler threads read the parse-feature bit-fields; deterministic re-parse
values; size-capped class documented at the site). Justifications at each
entry.

### 18.4 Round ledger

| round | unsuppressed | note |
|---|---|---|
| r12 | 16 | r11's 32 fixed; submerged families surfaced (WriteBarrier setEarlyValue, StringImpl fence FP, ConcurrentVector, IntlCache not yet visible) |
| r13 | 5  | JSDataView precise-cell single + LazyOperandValueProfile ConcurrentVector quartet |
| r14 | 1  | the IntlCache REAL BUG (find-vs-rehash) |
| r15 | 2  | last flickers: NativeExecutable plain arity-mirror init vs concurrentCodePtrLoad (fixed: concurrentCodePtrStore in finishCreation), WriteBarrierStructureID plain value()/setEarlyValue pair under racing initializeProfile (fixed: StructureID relaxedLoad/relaxedStore) |
| r16 | 0 | first zero snapshot, full corpus (218 ran / 6 skip) |
| **r17** | **0** | **final gate — second consecutive ZERO; between r16/r17 a 20x repeat-stress pass closed two sub-1/20 recycled-cell flickers: CodeBlock ctor <-> vm() HAPPENS_BEFORE/AFTER pair (also covers m_couldBeTainted), and the race:JSC::WriteBarrierBase wave-7 anchor (cell-pointer analog of decodeConcurrent; the slot's writers are atomic since this mop-up, so plain-writer regressions stay visible under their own anchors). Best health of the campaign: 186 exit-0.** |

r16 detail: 181 exit-0, 27 exit-3 (pre-existing set), 5 exit-134 + 3
exit-124 (the recurrent flaky-abort/timeout tail incl. the KNOWN-FAILING
pair — closeout items 3/4), and 2 exit-66 which are NOT data races: TSAN
DEADLYSIGNAL SEGVs in cve/mc-init-cloned-arguments-specials and
cve/mc-life-detach-quarantine-storm, identical shape since r10/r11
(functional-crash tail, items 3/4 ownership; zero
`WARNING: ThreadSanitizer` lines in their logs).

Run health improved monotonically (r11: 168 exit-0 / 17 exit-66; r14: 182
exit-0 / 3 exit-66); the exit-3 (26), exit-134 (~5) and exit-124 (2,
KNOWN-FAILING proto-cycle-race + mc-grow-buffer-storm) tails are unchanged
from r10/r11 — they belong to closeout items 3/4, not this item. Zero CLoop
frames in every round. Debug build compiles clean and passes GIL-off smoke
10/10 + GIL-on smoke after every wave (annotations and TSAN gates are
no-ops outside TSAN_ENABLED).

## 19. Closeout item 4 — havebadtime-vs-indexed-fastpath CLOSED (AB-10)

Date: 2026-06-09. The KNOWN-FAILING functional test
JSTests/threads/gc-stress/havebadtime-vs-indexed-fastpath.js now PASSES
GIL-off. Three-part fix (parts 1-2 flag-on-gated with flag-off
byte-identical behavior; part 3 is NOT — see its entry and the correction
note below):

1. **AB-10 landed** (the K.5 Class-4 stop): JSGlobalObject::haveABadTime
   routes its whole body through JSThreadsSafepoint::stopTheWorldAndRun when
   gilOff (-> the §A.3 thread-granular conductor, which already carries the
   HBT4 order, the HBT3.2/AB-21 in-window own-client access re-acquire, and
   the HBT2.2 I14 no-GC-in-window bracket), with the ANNEX HBT item-2
   post-arbitration isHavingABadTime() re-check inside the closure; landed
   body extracted as haveABadTimeImpl. The interim
   RELEASE_ASSERT(!vm.gilOff()) tripwire is retired. Heap-side shared-server
   asserts on the conversion-walk path gained the
   (jsThreadsThreadGranularWorldIsStopped() &&
   jsThreadsCurrentThreadIsStopConductor()) disjunct already used by
   LocalAllocator/BlockDirectory (AB18-D): MarkedSpace::stopAllocating /
   resumeAllocating, WeakSet::sweep / shrink,
   Heap::sweepNextLogicallyEmptyWeakBlock.

2. **Resume-side staleness (the actual data corruption)**: with AB-10 landed
   the test still failed ~1/50 amplified with a wrong VALUE: a worker parked
   at its DFG back-edge poll mid store-loop, the conductor converted the
   realm's arrays, and the resumed loop kept storing CONTIGUOUS-addressed
   (butterfly + i*8) into the new ArrayStorage butterfly — a +2-slot shift
   (ArrayStorage vector lives at +16), i.e. sharedArray[496] = value(498).
   Root cause: CheckTraps was modeled as InternalState-only in
   DFGClobberize/AI, so hoisted/CSE'd array-shape facts survived the park.
   Fix per SPEC-jit I21: flag-on, CheckTraps read(World)/write(Heap) in
   DFGClobberize.h and clobberWorld() in DFGAbstractInterpreterInlines.h (a
   park admits a foreign stop window; no heap fact survives a poll), and
   ByteCodeParser::handleCheckTraps emits the I21-pinned trailing
   InvalidationPoint (with ExitOK re-validation — CheckTraps is now
   ClobbersExit), so a mutator parked inside code the window jettisoned
   resumes into the patched exit.

3. **New-coverage TSAN single**: the now-reachable N-mutator ArrayStorage
   growth races the advisory `lastArraySize` heuristic
   (runtime/JSObject.cpp getNewVectorLength). Relaxed-atomicized per the
   family 12-14/19 advisory-datum convention. CORRECTION (post-closeout
   review): this conversion (`static std::atomic<unsigned> lastArraySize`,
   JSObject.cpp:67, relaxed load/store at 6398/6405) is UNCONDITIONAL — it
   changes flag-off code on the array-growth path of exactly the
   transition-heavy-constructor bench family. The section header's original
   "all flag-on-gated; flag-off byte-identical" claim was therefore false
   for part 3 (TSAN-RESULTS.md's bench-gate amendment already concedes
   this). The conversion is on the combined-revert attribution list in
   TSAN-RESULTS.md's standing obligations.

Gate evidence: 50/50 amplified standalone (seeds 3001-3050), 120/120 load6
(6-way, seeds rotated), 10/10 TSAN runs with ZERO reports (suppressions
unchanged), full corpus 93/0 GIL-off + 94/0 GIL-on, identity gate
mismatches=0, GIL-on single-run PASS, flag-off stress smoke
(have-a-bad-time-with-arguments.js rc=0). The KNOWN-FAILING list is now
empty of functional items owned by closeout items 3/4.

## 20. Round r23 — post-congc/closeout re-snapshot (2026-06-19)

### 20.0 TSAN configuration (standing-ruling re-compliance)

The `WebKitBuild/TSan` directory had been **reverted to a CLoop build** since
the §0 reconfiguration (someone re-ran the un-patched `tsan.sh` on Jun 15;
`CMakeCache.txt` showed `ENABLE_C_LOOP:BOOL=ON, ENABLE_JIT:BOOL=OFF`, and the
Jun 18 23:36 binary carried 24 CLoop symbols and zero DFG/FTL symbols). Per
the standing ruling, the directory was **wiped and reconfigured** with the
REAL LLInt asm + full JIT, identical to the §0 flag set:
`ENABLE_C_LOOP=OFF`, `ENABLE_JIT/DFG_JIT/FTL_JIT=ON`,
`ENABLE_WEBASSEMBLY(+BBQ/OMG)=ON`, `ENABLE_SAMPLING_PROFILER=ON`,
`ENABLE_SANITIZERS=thread`, `USE_SYSTEM_MALLOC=ON`, RelWithDebInfo +
`-fno-omit-frame-pointer -g`, clang-21, Bun flags. Build verified:
`ENABLE_C_LOOP:BOOL=OFF` in CMakeCache, binary mtime (Jun 19 00:18) newer
than the newest source file (Jun 18 23:48), `nm | grep -c CLoop` = 0,
DFG/FTL symbols present.

Smoke (`JSTests/threads/smoke.js`, GIL-off env, full JIT, 3x standalone):
**PASS each run, exit 0, ZERO TSAN reports**, no instrumentation crashes or
hangs. TSAN+JIT remains workable; no-JIT fallback NOT used. (Under -P12
corpus load, smoke.js surfaces 33 reports — all family `tlc-slot-stamp`,
which needs ≥2 spawned threads racing the same attach window; standalone
3x missed it by scheduling.)

**Accepted tradeoff (re-stated per ruling):** TSAN cannot see races inside
JIT-generated code. That coverage belongs to the object-model protocol
tests + amplifier. Residual evidence: 20 of the 3692 r23 reports are
JIT-one-siders (degraded/absent JIT-side stack — see family
`jit-one-sider-allocator`). **Zero CLoop frames** anywhere in r23
(`grep -ci CLoop reports-r23.log` = 0). No CLoop suppressions were needed;
none added.

### 20.1 Run configuration

Same as §1 (binary, env, args, TSAN_OPTIONS, suppressions). Corpus: 246
candidate `JSTests/threads/**/*.js` (grown since r0; congc-t* + cve/mc-* +
gc-stress + scaling tests added). Result: **3692 reports** in 105 test
files, 29 raw deduped stack-pair keys, grouped into the **10 families**
below. KNOWN-FAILING tests (date-cache-churn, proto-cycle-race,
symbol-registry-cross-thread, havebadtime-vs-indexed-fastpath) ran; their
race reports are counted (havebadtime contributes 4 reports to families 2
and 10; the other three contribute only family-1 stamps). Exit-code
spectrum: 129 exit-0, 105 exit-66, 10 exit-3, 1 exit-134
(semantics/frozen-seal-race), 1 exit-124 (w16-c1-prevent-collection
timeout).

Per-test logs: `Tools/threads/tsan/r23/`. Concatenated reports:
`Tools/threads/tsan/reports-r23.log` (also written to `reports-r0.log` per
the campaign-snapshot contract). Dedup output:
`Tools/threads/tsan/families-r23.txt`.

`unsuppressedCount = 3692` (post-CLoop-suppression: no CLoop families
existed; nothing newly suppressed in this round — family 10's anchors are
proposed, not yet applied).

**None of the V7 known-suspect families reappear** (RegExpCachedResult,
TinyBloomFilter, ArrayProfile/ArithProfile/ArrayAllocationProfile,
WriteBarrierBase, JITCode/RawPtrTraits, CallLinkRecord, PropertyTable,
StringImplShape, NumericStrings, KeyAtomStringCache, BlockDirectoryBits,
cellHeaderConcurrentLoad, Structure::setMaxOffset, addToRememberedSet,
WatchpointSet — all closed in r0-r17/§18). Every r23 family is **new
surface** introduced by post-r22 work: SPEC-congc landing
(`sharedGCWindowWitnessSnapshot`, `parkedRootSnapshot`, GCThreadLocalCache
attach), the SCALEBENCH bimodal fix's lock-free OSR-exit DCLP, and
ConcurrentPtrHashSet under N-mutator visit pressure.

### 20.2 Family table (r23)

| # | id | count | spec row | ruling | files (wave ownership) |
|---|----|-------|----------|--------|------------------------|
| 1 | tlc-slot-stamp | 2691 | SPEC-heap §5.3 (TLC slot is a deterministic per-subspace index; idempotent stamp) | relaxed-atomic | heap/IsoSubspace.h |
| 2 | marks-bitset-plain-readers | 828 | SPEC-congc §5 (witness snapshot tolerates stale marks); upstream concurrent-marking BitSet protocol | concurrent-accessor | WTF/wtf/BitSet.h, heap/MarkedBlock.h, heap/MarkedBlockInlines.h |
| 3 | dependency-loadandfence | 76 | OM §2/§3 (consume-style fenced loads); §13.4 TSAN-gate precedent | relaxed-atomic | WTF/wtf/Atomics.h |
| 4 | parked-root-snapshot | 37 | SPEC-congc T5-rootscan-skip (seq_cst snapshot ptr orders the thread-id word) | relaxed-atomic | heap/Heap.h, heap/Heap.cpp |
| 5 | osr-exit-coderef | 25 | SPEC-jit §4.x publish-via-one-pointer; SCALEBENCH bimodal DCLP | relaxed-atomic | dfg/DFGJITCode.h, dfg/DFGOSRExit.cpp |
| 6 | concurrentptrhashset-publish | 7 | upstream lock-free table; §13.4 TSAN-gate precedent (ConcurrentVector/Buffer) | relaxed-atomic | WTF/wtf/ConcurrentPtrHashSet.h |
| 7 | codeblock-jitdata-publish | 4 | SPEC-jit §4.x code lifecycle (m_jitData published after storeStoreFence) | relaxed-atomic | bytecode/CodeBlock.h, bytecode/CodeBlock.cpp |
| 8 | dfg-weakrefs-reallyadd | 2 | SPEC-jit §4.x / SPEC-congc — NOT blessed (unfenced FixedVector swap vs concurrent visitor) | real-bug | dfg/DFGDesiredWeakReferences.cpp |
| 9 | typedarray-sort-memcpy | 2 | OM §1 GT (TA lanes intentionally racy via concurrent accessor) — plain memcpy is NOT the accessor | relaxed-atomic | runtime/JSGenericTypedArrayViewInlines.h |
| 10 | jit-one-sider-allocator | 20 | §0 accepted tradeoff (JIT-side invisible) + §18.3 wave-7 anchor class | suppress | Tools/tsan/suppressions.txt |

All 10 families have **disjoint file ownership** (wave-1 can run all 10 in
parallel).

### 20.3 Per-family analysis

#### 20.3.1 tlc-slot-stamp (2691)

Raw keys: `(stampTlcSlot, stampTlcSlot)` 2673 + `(stampTlcSlot, tlcSlot)`
18. `IsoSubspace::stampTlcSlot()` (IsoSubspace.h:86) does a plain
`m_tlcSlot = slot` write; every spawned thread's
`GCClient::GCThreadLocalCache` ctor (GCThreadLocalCache.cpp:200) walks the
shared subspace list and stamps each one. N threads attaching concurrently
all stamp the same word with the **same value** (the ASSERT at :85 confirms
idempotence: `m_tlcSlot == invalidTlcIndex || m_tlcSlot == slot`). The
read-side (18 reports) is the DFG compiler thread's
`AssemblyHelpers::tlcSlotForConcurrentlyWithIso<T>()` reading the slot to
emit the per-thread inline-allocation address.

**Spec row:** SPEC-heap §5.3 / dev-6 — the TLC slot is a deterministic
per-subspace index assigned once at first attach; "iso = lookup-only"
predicate. The value is monotonic (`invalid → slot`, idempotent). SPEC-jit
§5.7.7 advisory-datum rule applies (≤8B, deterministic, plain-iff-atomic).

**Ruling: relaxed-atomic.** Convert `m_tlcSlot` to relaxed-atomic
load/store: `tlcSlot()` → `WTF::atomicLoad(&m_tlcSlot, relaxed)`;
`stampTlcSlot()` → `WTF::atomicStore(&m_tlcSlot, slot, relaxed)`. Keep the
debug ASSERT (re-read via the relaxed load). Flag-off: never stamped/read
(behind `vm.gilOff()` codegen gate per the in-source comment), so semantics
unchanged.

**Files:** `Source/JavaScriptCore/heap/IsoSubspace.h`.

**Evidence:**
```
Write of size 4 by T16:
  IsoSubspace::stampTlcSlot IsoSubspace.h:86
  GCClient::GCThreadLocalCache::GCThreadLocalCache GCThreadLocalCache.cpp:200
  GCClient::Heap::Heap Heap.cpp:7932
  attachSpawnedThreadGCClient ThreadManager.cpp:641
Previous write of size 4 by T15:
  IsoSubspace::stampTlcSlot IsoSubspace.h:86
  GCClient::GCThreadLocalCache::GCThreadLocalCache GCThreadLocalCache.cpp:200
  (identical stack)
--- read variant ---
Previous read of size 4 by DFG compiler thread:
  IsoSubspace::tlcSlot IsoSubspace.h:81
  AssemblyHelpers::tlcSlotForConcurrentlyWithIso<JSFunction>
  AssemblyHelpers::emitAllocateJSObjectWithKnownSize
  DFG::SpeculativeJIT::compileNewFunctionCommon<JSFunction>
```

#### 20.3.2 marks-bitset-plain-readers (828)

Raw keys (writer side is always `BitSet<1024>::concurrentTestAndSet` via
SlotVisitor marking, an atomic CAS): `BitSet::get` 509+17 (one-sided),
`specializedSweep` memcpy 214+65, `BitSet::concurrentFilter` 18+5. Three
distinct plain-read sites of `MarkedBlock::header().m_marks` racing the
marking thread's atomic CAS:

(a) **`isMarkedRaw()`** (MarkedBlock.h:660) calls plain `BitSet::get()`.
Called from `sharedGCWindowWitnessSnapshot()` (MarkedBlock.cpp:507/527),
which per its comment "read isMarkedRaw()/isNewlyAllocatedStale() lock-free
and tolerate stale". The §18.2 precedent already converted
`Handle::isLive`'s m_marks read to `concurrentGet()`.

(b) **`specializedSweep`**'s `live = header.m_marks` struct copy
(MarkedBlockInlines.h:285/288) — clang lowers this to `__tsan_memcpy` over
the 128-byte BitSet while a SlotVisitor on another mutator's conducted GC
CAS-sets bits. The sweep is allocator-side; the CAS is marker-side. The
existing protocol orders them via `m_markingVersion` (TSAN can't model the
version-fence dependency).

(c) **`BitSet::concurrentFilter`** (BitSet.h:405) reads `other.bits[i]`
plain (where `other` is the live marks set) inside
`IsoCellSet::sweepToFreeList` (IsoSubspace::didBeginSweepingToFreeList).
Its own-word CAS is fine; the *foreign* word read is plain.

**Spec row:** SPEC-congc §5 (witness snapshot is advisory; staleness
tolerated) + the upstream concurrent-marking BitSet protocol (already
blessed by the rule-1 suppressions header and §18.2's
`isLive`→`concurrentGet` precedent).

**Ruling: concurrent-accessor.** (a) `isMarkedRaw()` →
`m_marks.concurrentGet(atomNumber(p))` (the existing concurrent accessor;
exact §18.2 shape). (b) Add `BitSet::concurrentCopyFrom(const BitSet&)`
that loads each word relaxed-atomic, and use it at MarkedBlockInlines.h:285
/288 instead of struct assign. (c) `concurrentFilter`: change the
`other.bits[i]` reads to
`std::bit_cast<const Atomic<WordType>*>(&other.bits[i])->loadRelaxed()`.
All three are word-granular relaxed loads = identical x86 codegen.

**Files:** `Source/WTF/wtf/BitSet.h` (concurrentCopyFrom; concurrentFilter
fix), `Source/JavaScriptCore/heap/MarkedBlock.h` (isMarkedRaw),
`Source/JavaScriptCore/heap/MarkedBlockInlines.h` (specializedSweep copy).

**Evidence:**
```
Read of size 8: BitSet<1024>::get BitSet.h:188
  MarkedBlock::isMarkedRaw MarkedBlock.h:660
  MarkedBlock::sharedGCWindowWitnessSnapshot MarkedBlock.cpp:507
  Heap::addCoreConstraints::$_3 Heap.cpp:4725
  Heap::runFixpointPhase
Previous atomic write of size 8: Atomic<u64>::compareExchangeWeak
  BitSet<1024>::concurrentTestAndSet
  MarkedBlock::testAndSetMarked
  SlotVisitor::appendHiddenUnbarriered
--- sweep variant ---
Read of size 8: __tsan_memcpy
  MarkedBlock::Handle::specializedSweep MarkedBlockInlines.h:285  (live = header.m_marks)
  MarkedBlock::Handle::sweep
  LocalAllocator::tryAllocateIn / allocateSlowCase
Previous atomic write: BitSet<1024>::concurrentTestAndSet (same as above)
```

#### 20.3.3 dependency-loadandfence (76)

Raw keys: `(Atomic<Butterfly*>::store, loadAndFence<Butterfly*>)` 48 +
`(compareExchangeStrong, loadAndFence<Butterfly*>)` 10 +
`('data race', loadAndFence<Butterfly*>)` 5 +
`(Atomic<u64>::store, loadAndFence<Butterfly*>)` 1 +
`(loadAndFence<u32>, aligned_alloc)` 8 +
`(Header::Header, loadAndFence<u32>)` 4.

`WTF::Dependency::loadAndFence<T>` on x86 (Atomics.h:449) does a **plain
`*pointer`** then `Dependency::fence` (which is a no-op on TSO). All
callers — `JSObjectWithButterfly::fencedButterfly()` in
`visitButterflyImpl` (concurrent GC visiting a mutator's object) and
`MarkedBlock::aboutToMark()` reading `m_markingVersion` — are by
construction the **consume-style dependency-ordered loads** OM §2/§3 and
the upstream concurrent-GC protocol bless. The writer sides are already
atomic (`AuxiliaryBarrier` relaxed store, `storeTaggedButterflyWordConcurrent`
CAS, `FreeCell::makeLast` atomic store) or carry the §18.2
`MarkedBlock::Header` ctor `TSAN_ANNOTATE_HAPPENS_BEFORE`.

**Spec row:** OM §2 (butterfly word via fenced/tagged accessors) + OM §3
(cell header via concurrent accessors); §13.4 TSAN-gate precedent
("consume-style publication TSAN cannot model").

**Ruling: relaxed-atomic (TSAN-gated).** Under `TSAN_ENABLED`, change
`loadAndFence`'s `T value = *pointer` to an
`Atomic<T>` acquire load (`std::bit_cast<const Atomic<T>*>(pointer)->load()`).
Production keeps the plain consume/dependency load. This single Atomics.h
change closes all 76: the writer-atomic cases (64) become atomic↔atomic;
the `Header::Header`/`aligned_alloc` cases (12) pair via the existing
`TSAN_ANNOTATE_HAPPENS_BEFORE(&header())` (MarkedBlock.h:575 already
annotates the reader side; the acquire load completes the model).

**Files:** `Source/WTF/wtf/Atomics.h`.

**Evidence:**
```
Read of size 8: Dependency::loadAndFence<Butterfly*> Atomics.h:449
  JSObjectWithButterfly::fencedButterfly
  JSObjectWithButterfly::visitButterflyImpl<SlotVisitor>
  JSFinalObject::visitChildren / SlotVisitor::drain
Previous atomic write of size 8: Atomic<Butterfly*>::store
  AuxiliaryBarrier<Butterfly*>::setWithoutBarrier
  AuxiliaryBarrier<Butterfly*>::AuxiliaryBarrier  (cell ctor on another mutator)
```

#### 20.3.4 parked-root-snapshot (37)

`GCClient::Heap::publishParkedRootSnapshot()` writes
`m_parkedRootSnapshotThread = &thread` plain (Heap.cpp:8664/8668);
`parkedRootSnapshotThread()` reads it plain (Heap.h:2406) from
`Heap::gatherStackRoots` on the GC conductor. The in-source comment
(Heap.h:2598) documents the protocol: "written BEFORE the seq_cst
m_parkedRootSnapshot publish; read by the conductor only when that load
returns non-null (the seq_cst pair orders it). Never cleared independently
(stale value is harmless — gated by the snapshot pointer)."

**Spec row:** SPEC-congc T5-rootscan-skip — the seq_cst
`m_parkedRootSnapshot` pointer is the publication; the thread-id word is a
piggyback payload ordered by it. Blessed by design; the plain word access
is the UB.

**Ruling: relaxed-atomic.** `m_parkedRootSnapshotThread` read/write via
`WTF::atomicLoad/Store(..., relaxed)`. The seq_cst on the snapshot pointer
remains the real HB edge.

**Files:** `Source/JavaScriptCore/heap/Heap.h` (accessor + member),
`Source/JavaScriptCore/heap/Heap.cpp` (the two write sites).

**Evidence:**
```
Write of size 8: GCClient::Heap::publishParkedRootSnapshot Heap.cpp:8668
  gcClientPublishParkedRootSnapshot
  VMManager::notifyVMStop
  VMTraps::handleTraps  (parking mutator)
Previous read of size 8: GCClient::Heap::parkedRootSnapshotThread Heap.h:2406
  Heap::gatherStackRoots Heap.cpp:1230
  HeapClientSet::forEach  (GC conductor, different mutator)
```

#### 20.3.5 osr-exit-coderef (25)

`DFG::JITData::setExitCode()` (DFGJITCode.h:173) move-assigns a
`MacroAssemblerCodeRef<OSRExitPtrTag>` into `m_exits[i]` — plain writes of
its `m_codePtr` and `m_executableMemory` words. The reader is the new
SCALEBENCH-bimodal lock-free DCLP probe at DFGOSRExit.cpp:219:
`exitCode(exitIndex).code().taggedPtr()` — plain read of `m_codePtr`. Both
sides are inside `operationCompileOSRExit` on **different mutators** firing
the same exit. The in-source comment (DFGOSRExit.cpp:195-221) documents the
protocol: "m_exits[i] is initialized to the process-singleton
osrExitGenerationThunk and overwritten exactly once by setExitCode under
dfgOSRExitGenerationLock; the move-assign writes m_codePtr FIRST … a
non-thunk codePtr implies the ramp's executable memory is fully constructed
… loadLoadFence pairs with FINALIZE_CODE's publish."

**Spec row:** SPEC-jit §4.x publish-via-one-pointer + §5.8 (single
published code word). Blessed protocol; plain word access is the UB.

**Ruling: relaxed-atomic.** Add `JITData::exitCodePtrConcurrent(unsigned)`
that relaxed-loads the `m_codePtr` word
(`WTF::atomicLoad(&m_exits[i].code(), relaxed).taggedPtr()` shape), use it
at DFGOSRExit.cpp:219 in place of the plain `.code().taggedPtr()`. In
`setExitCode()`, write `m_executableMemory` first, `WTF::storeStoreFence()`,
then relaxed-atomic store `m_codePtr` (preserves the documented "m_codePtr
first" → invert to "m_codePtr LAST so non-thunk implies live RefPtr"; the
existing comment's correctness argument is the RefPtr-held-live one, not
the field order — adjust the comment). Flag-off: gilOff-only arm
(DFGOSRExit.cpp:217 guard), byte-identical.

**Files:** `Source/JavaScriptCore/dfg/DFGJITCode.h`,
`Source/JavaScriptCore/dfg/DFGOSRExit.cpp`.

**Evidence:**
```
Write of size 8 by T8: MacroAssemblerCodeRef<OSRExitPtrTag>::operator=
  DFG::JITData::setExitCode DFGJITCode.h:175
  operationCompileOSRExit DFGOSRExit.cpp:336  (under dfgOSRExitGenerationLock)
Previous read of size 8 by T5: MacroAssemblerCodeRef::code
  operationCompileOSRExit DFGOSRExit.cpp:219  (lock-free DCLP probe)
```

#### 20.3.6 concurrentptrhashset-publish (7)

Raw keys: `(addImpl, malloc)` 3 + `(addImpl, TrailingArray ctor)` 1 +
`('data race', Atomic<void*>::Atomic)` 3. `ConcurrentPtrHashSet::addImpl()`
(ConcurrentPtrHashSet.h:160) does `Table* table = m_table.loadRelaxed()`
then plain-reads `table->mask` (size-4). `resizeIfNecessary()` constructs a
new `Table` (TrailingArray ctor writes `mask`/size plain, in fresh malloc)
then `m_table.store(newTable.get())` seq_cst (ConcurrentPtrHashSet.cpp:156).
Relaxed load does not synchronize with the seq_cst store under TSAN's
model, so the `table->mask` read races the ctor's plain init.

**Spec row:** Upstream lock-free hash set; pre-existing under parallel
markers. §13.4 precedent: ConcurrentVector/ConcurrentBuffer got the
identical "TSAN-gated acquire on the relaxed table-pointer load"
treatment.

**Ruling: relaxed-atomic (TSAN-gated).** Under `TSAN_ENABLED`, replace
`m_table.loadRelaxed()` with `m_table.load()` (acquire) in `addImpl()` and
`size()`. Production keeps loadRelaxed.

**Files:** `Source/WTF/wtf/ConcurrentPtrHashSet.h`.

**Evidence:**
```
Read of size 4: ConcurrentPtrHashSet::addImpl ConcurrentPtrHashSet.h:161 (table->mask)
  AbstractSlotVisitor::addOpaqueRoot
  JSArrayBufferView::visitChildrenImpl<SlotVisitor>
Previous write of size 4: TrailingArray<Table, Atomic<void*>>::TrailingArray
  ConcurrentPtrHashSet::Table::create
  ConcurrentPtrHashSet::resizeIfNecessary / resizeAndAdd  (another mutator's addOpaqueRoot)
```

#### 20.3.7 codeblock-jitdata-publish (4)

`(baselineJITData, setBaselineJITData)` 3 + `(dfgJITData, setDFGJITData)`
1. `setBaselineJITData()`/`setDFGJITData()` (CodeBlock.h:618/627,
CodeBlock.cpp:818) do `WTF::storeStoreFence()` then plain
`m_jitData = jitData.release()`. `baselineJITData()`/`dfgJITData()` read
`m_jitData` plain via `bit_cast<...*>(m_jitData)`. The reader is
`CodeBlock::stronglyVisitStrongReferences` →
`forEachPropertyInlineCache` on the concurrent-GC SlotVisitor (under
`ConcurrentJSLocker(m_lock)`, CodeBlock.cpp:1433). The writer is plan
finalize on a mutator (`JITWorklist::completeAllReadyPlansForVM`), NOT
holding `m_lock` — but the in-source comment at CodeBlock.h:630 ("m_jitData
is accessed from concurrent GC threads") documents the upstream
single-pointer-publish protocol.

**Spec row:** SPEC-jit §4.x code-lifecycle publish-via-one-pointer.
Blessed; plain word access is the UB.

**Ruling: relaxed-atomic.** `m_jitData` write → `WTF::atomicStore(...,
relaxed)` (after the existing storeStoreFence); `baselineJITData()`/
`dfgJITData()` read → `WTF::atomicLoad(&m_jitData, relaxed)`. The
`jitType()` discriminator is already atomic (§18 codeblock-init).

**Files:** `Source/JavaScriptCore/bytecode/CodeBlock.h`,
`Source/JavaScriptCore/bytecode/CodeBlock.cpp`.

**Evidence:**
```
Write of size 8: CodeBlock::setBaselineJITData CodeBlock.cpp (m_jitData = release)
  CodeBlock::setupWithUnlinkedBaselineCode
  BaselineJITPlan::finalize
  JITWorklist::completeAllReadyPlansForVM  (mutator A)
Previous read of size 8: CodeBlock::baselineJITData CodeBlock.h:622
  CodeBlock::forEachPropertyInlineCache
  CodeBlock::stronglyVisitStrongReferences<SlotVisitor>
  CodeBlock::visitChildren  (GC conductor, mutator B)
```

#### 20.3.8 dfg-weakrefs-reallyadd (2) — REAL BUG

`(FixedVector<StructureID>::begin, ::operator=)` 1 +
`(DesiredWeakReferences::reallyAdd, stronglyVisitWeakReferences)` 1.
`DFG::DesiredWeakReferences::reallyAdd()`
(DFGDesiredWeakReferences.cpp:99-100) move-assigns
`common->m_weakStructureReferences` and `common->m_weakReferences`
(FixedVector = unique_ptr<EmbeddedFixedVector>) **with no fence and without
the CodeBlock's `m_lock`**. `CodeBlock::stronglyVisitWeakReferences` and
`propagateTransitions` (CodeBlock.cpp:1450/1639) iterate those vectors
under `ConcurrentJSLocker(m_lock)` from the concurrent-GC SlotVisitor. The
in-source comment at :89 ("GC is deferred") is the upstream single-mutator
guarantee; under SPEC-congc N-mutators, DeferGC on the finalizing mutator
does NOT stop another mutator acting as GC conductor. The unique_ptr swap
publishes a buffer pointer the visitor may dereference before its contents
are visible (no storeStoreFence), or the visitor may iterate the OLD
(empty) buffer while the swap frees nothing (ASSERT-empty) — but a torn
pointer read is UB and a non-null pointer with non-visible
EmbeddedFixedVector header (`m_size`) is a wild iteration.

**Spec row:** SPEC-jit §4.x code lifecycle does NOT bless this site (no
fence, multi-word). SPEC-congc N-mutator finalize-vs-visit gap.

**Ruling: real-bug.** Fix shape: take
`ConcurrentJSLocker locker(m_codeBlock->m_lock)` around the two
move-assigns at DFGDesiredWeakReferences.cpp:99-100 (once-per-DFG-compile
slow path; trivial contention; the visitor already holds the same lock).
Alternatively, storeStoreFence before each move-assign + relaxed-atomic on
the FixedVector storage pointer — but FixedVector doesn't expose that and
the lock is simpler/safer.

**Files:** `Source/JavaScriptCore/dfg/DFGDesiredWeakReferences.cpp`.

**Evidence (cve/mc-safe-gcwait-vs-classa-stop.js):**
```
Write of size 8: unique_ptr<EmbeddedFixedVector<StructureID>>::operator=
  FixedVector<StructureID>::operator=
  DFG::DesiredWeakReferences::reallyAdd DFGDesiredWeakReferences.cpp:99
  DFG::Plan::finalize  (mutator T10)
Previous read of size 8: unique_ptr<...>::get
  FixedVector<StructureID>::begin
  CodeBlock::stronglyVisitWeakReferences<SlotVisitor>
  CodeBlock::visitChildren  (GC conductor)
```

#### 20.3.9 typedarray-sort-memcpy (2)

`(trySetIndexQuicklyForTypedArrayConcurrent, copyElements<int>)`.
`JSGenericTypedArrayView::sort()` (JSGenericTypedArrayViewInlines.h:1137+)
under `gilOffProcess` already takes the CVE-AUDIT A2 / MC-DF S9
copy-out/sort/copy-back path, but the copy-out/back use
`WTF::copyElements<int>` (memcpy) over the live typed-array lanes while
another mutator atomic-writes via `trySetIndexQuicklyForTypedArrayConcurrent`.

**Spec row:** OM §1 GT — TA lanes are intentionally racy via the dedicated
concurrent accessors; plain memcpy is NOT the blessed accessor.

**Ruling: relaxed-atomic.** In the gilOff copy-out/back arm only, replace
`copyElements` with an element-wise relaxed-atomic load/store loop (the
same accessor shape as `trySetIndexQuicklyForTypedArrayConcurrent` /
`tryGetIndexQuicklyForTypedArray`). Flag-off keeps the memcpy.

**Files:** `Source/JavaScriptCore/runtime/JSGenericTypedArrayViewInlines.h`.

**Evidence (cve/mc-df-ta-sort-inplace.js):**
```
Read of size 8: __tsan_memcpy
  WTF::copyElements<int>
  JSGenericTypedArrayView<Int32Adaptor>::sort
  genericTypedArrayViewProtoFuncSort
Previous atomic write of size 4: trySetIndexQuicklyForTypedArrayConcurrent
  JSObject::trySetIndexQuicklyConcurrent
  llint_slow_path_put_by_val  (other mutator)
```

#### 20.3.10 jit-one-sider-allocator (20)

Raw one-sided keys (current-access stack JIT-only/unrestorable; only the
"Previous write" C++ stack present): `aligned_alloc` 13 (fresh MarkedBlock
hand-out), `Butterfly::growArrayRight` 3 (havebadtime-vs-indexed-fastpath,
KNOWN-FAILING — JIT reads memcpy'd butterfly contents),
`JSRopeString::CompactFibers ctor` 2 (JIT reads freshly-memset rope
fields), `malloc` 1 (PreciseAllocation hand-out),
`PreciseAllocation::PreciseAllocation Atomic<bool> ctor` 1.

These are the §0 accepted-tradeoff residual: TSAN cannot symbolize the
JIT-emitted access, so the report carries only the C++-side allocator/ctor
write. The cell-handout protocol (LocalAllocator → JIT) IS the real HB
edge; TSAN cannot see it. §18.3 / wave-7 precedent: suppress on the
narrowest C++-side anchor with the wave-7 justification template ("atomic
probe vs allocator hand-out; no plain access of ours exists; the racing
access is JIT-emitted and invisible to TSAN").

**Ruling: suppress.** Add four anchors to `Tools/tsan/suppressions.txt`
with the wave-7 justification: `race:JSC::Butterfly::growArrayRight`,
`race:JSC::JSRopeString::CompactFibers::CompactFibers`,
`race:JSC::PreciseAllocation::PreciseAllocation`,
`race:JSC::MarkedBlock::tryCreate` (covers the bare aligned_alloc/malloc 14
via the only call path). Re-audit at the next re-snapshot to confirm none
mask a two-sided C++ regression (the wave-7 discipline: a future plain C++
reader/writer surfaces under its own anchor).

**Files:** `Tools/tsan/suppressions.txt`.

**Evidence:**
```
[current access stack: JIT code, "<null>" / failed to restore]
Previous write of size 8: aligned_alloc
  tryFastCompactAlignedMalloc
  FastMallocAlignedMemoryAllocator::tryAllocateAlignedMemory
  MarkedBlock::tryCreate
  BlockDirectory::tryAllocateBlock
  LocalAllocator::allocateSlowCase  (mutator A; cell handed to mutator B's JIT)
```

## 21. Round r24 — wave-24 verification (2026-06-19)

### 21.0 Build / config check

`WebKitBuild/TSan/CMakeCache.txt`: `ENABLE_C_LOOP:BOOL=OFF`,
`ENABLE_JIT:BOOL=ON` (unchanged from §20.0). Binary mtime (Jun 19 00:40,
epoch 1781829617) > newest source (DFGOSRExit.cpp, epoch 1781829390) — the
TSan build is current; no rebuild needed. `nm | grep -c CLoop` = 0. Smoke
(`JSTests/threads/smoke.js`, GIL-off env, full JIT, 3x): PASS each run,
exit 0, ZERO TSAN reports. **Zero CLoop frames** in r24
(`grep -ci CLoop reports-r24.log` = 0).

Wave-24 fixers touched (since reports-r23.log): heap/IsoSubspace.h,
WTF/wtf/BitSet.h, heap/MarkedBlock{.h,Inlines.h}, WTF/wtf/Atomics.h,
heap/Heap.{h,cpp}, dfg/DFGJITCode.h, dfg/DFGOSRExit.cpp, dfg/DFGPlan.cpp,
dfg/DFGDesiredWeakReferences.{h,cpp}, WTF/wtf/ConcurrentPtrHashSet.h,
bytecode/CodeBlock.{h,cpp}, runtime/JSGenericTypedArrayViewInlines.h.
**`Tools/tsan/suppressions.txt` was NOT modified** — the §20.3.10
`jit-one-sider-allocator` suppress ruling did not land.

### 21.1 Run configuration

Same as §20.1 (binary, env, args, TSAN_OPTIONS, suppressions). Corpus: 252
candidate `JSTests/threads/**/*.js` (246 ran, 6 //@ skip). Result: **66
reports** in 34 test files, 14 raw deduped stack-pair keys, grouped into
the **6 open families** below (+ 7 r23 families now at 0). Exit-code
spectrum: 197 exit-0, 35 exit-66, 13 exit-3 (was 10 in r23 — three new:
api/blocking-gate, cve/mc-df-arraycopy-relabel,
cve/mc-gc-weakgcmap-registry-vs-prune; functional, out of TSAN scope), 0
exit-134 (semantics/frozen-seal-race no longer aborts), 1 exit-124
(w16-c1-prevent-collection timeout, unchanged).

Per-test logs: `Tools/threads/tsan/r24/`. Concatenated reports:
`Tools/threads/tsan/reports-r24.log`. Dedup output:
`Tools/threads/tsan/families-r24.txt`.

`unsuppressedCount = 66` (3692 → 66; nothing newly suppressed; no CLoop
families existed).

### 21.2 r23-family verification

| r23 id | r23 count | r24 count | status |
|--------|-----------|-----------|--------|
| tlc-slot-stamp | 2691 | **0** | DONE |
| marks-bitset-plain-readers | 828 | **35** | PARTIAL — see §21.4.1 |
| dependency-loadandfence | 76 | **0** | DONE (loadAndFence now atomic; 2 Header::Header residuals re-bin to family 10) |
| parked-root-snapshot | 37 | **0** | DONE |
| osr-exit-coderef | 25 | **0** | DONE (m_codePtr word closed; 2 next-layer residuals re-bin to §21.4.2) |
| concurrentptrhashset-publish | 7 | **0** | DONE |
| codeblock-jitdata-publish | 4 | **2** | PARTIAL — next-layer, see §21.4.2 |
| dfg-weakrefs-reallyadd | 2 | **0** | DONE (real-bug fixed via ConcurrentJSLocker) |
| typedarray-sort-memcpy | 2 | **0** | DONE |
| jit-one-sider-allocator | 20 | **26** | UNFIXED — suppressions never applied, see §21.4.3 |

7 of 10 r23 families closed. **3626 reports eliminated** (98.2%).

### 21.3 Family table (r24)

| # | id | count | spec row | ruling | files (wave ownership) |
|---|----|-------|----------|--------|------------------------|
| 1 | marks-bitset-plain-readers | 35 | SPEC-congc §5 / upstream concurrent-marking BitSet protocol | concurrent-accessor | WTF/wtf/BitSet.h |
| 2 | codeblock-jitdata-publish | 2 | SPEC-jit §4.x publish-via-one-pointer; §13.4 TSAN-gate precedent | relaxed-atomic | bytecode/CodeBlock.h |
| 3 | jit-one-sider-allocator | 26 | §0 accepted tradeoff + §18.3 wave-7 anchor class | suppress | Tools/tsan/suppressions.txt |
| 4 | regexp-cached-result-visit | 1 | SPEC-congc visitor-vs-init; OM GT (cell fields via concurrent accessor) | relaxed-atomic | runtime/RegExpCachedResult.h, runtime/RegExpCachedResult.cpp, runtime/RegExpGlobalDataInlines.h |
| 5 | growarrayright-vs-sparsemap-atomics | 1 | OM §4 butterfly reshape protocol — NOT blessed (two mutators reshape same butterfly) | real-bug | runtime/ThreadAtomics.cpp |
| 6 | cachedcall-protoframe-crossthread | 1 | SPEC-jit §4.x code lifecycle — NOT blessed (installCode mutates foreign-thread stack) | real-bug | interpreter/CachedCall.h, interpreter/InterpreterInlines.h, bytecode/CodeBlock.cpp |

All 6 families have **disjoint file ownership** (next wave can run all 6 in
parallel).

### 21.4 Per-family analysis

#### 21.4.1 marks-bitset-plain-readers (35) — fix INCOMPLETE

Raw keys: `(compareExchangeWeak, BitSet::concurrentFilter)` 20 +
`('data race', concurrentFilter)` 15 (same pairing; dedup parser dropped
one stack on the 15). All r23 sub-keys involving `BitSet::get` (526),
`specializedSweep` (279) are **gone** — the wave-24 fix successfully
atomicized those readers. The residual is a **single site the fix missed**:
`BitSet::concurrentFilter` was patched to relaxed-atomic-load
`other.bits[i]` (BitSet.h:410, the m_marks word), but the OWN-side
`bits[i]` accesses at BitSet.h:412 (`bits[i] = 0`) and BitSet.h:415
(`WordType oldBits = bits[i]`) remain plain. The pairing is
IsoCellSet::add → `concurrentTestAndSet` (atomic CAS on the IsoCellSet's
own BitSet word, from a HeapHelper marker via
CodeBlock::visitChildren) vs IsoCellSet::sweepToFreeList →
`concurrentFilter` (plain read of the SAME BitSet's own word, from a
mutator-conductor's resumeThePeriphery → resumeAllocating → sweep).

**Spec row:** unchanged (SPEC-congc §5; sweep-vs-mark BitSet protocol).
Blessed; plain word access is the UB.

**Ruling: concurrent-accessor** (unchanged). **fixShape update:** convert
`bits[i] = 0` → `WTF::atomicStore(&bits[i], WordType(0), relaxed)` and
`WordType oldBits = bits[i]` → `WTF::atomicLoad(&bits[i], relaxed)` in
`BitSet::concurrentFilter`. The CAS at :419 is already atomic. Flag-off
codegen identical (relaxed = plain MOV).

**Files:** `Source/WTF/wtf/BitSet.h` only (MarkedBlock side closed).

**Evidence:**
```
Atomic write of size 8 by T5 (HeapHelper): compareExchangeWeak
  BitSet::concurrentTestAndSet BitSet.h:243
  IsoCellSet::add IsoCellSetInlines.h:76
  CodeBlock::visitChildren CodeBlock.cpp:1452  (drainFromShared marker)
Previous read of size 8 by T15 (mutator-conductor): concurrentFilter BitSet.h:415
  IsoCellSet::sweepToFreeList IsoCellSet.cpp:179
  IsoSubspace::didBeginSweepingToFreeList
  MarkedBlock::Handle::sweep / resumeAllocating
  Heap::resumeThePeriphery
```

#### 21.4.2 codeblock-jitdata-publish (2) — next-layer residual

Raw keys: `(FixedVector<MacroAssemblerCodeRef>::operator[], malloc)` 1 +
`(TrailingArray<EmbeddedFixedVector<MacroAssemblerCodeRef>>, malloc)` 1.
The r23 keys `(baselineJITData, setBaselineJITData)` /
`(dfgJITData, setDFGJITData)` are **gone** — the m_jitData word itself is
now relaxed-atomic. The residual is the **next layer down**: the new
`DFG::JITData::exitCodePtrConcurrent(i)` (DFGJITCode.h:213, the §20.3.5
fix) plain-dereferences `m_exits` (a `FixedVector` member of JITData,
i.e. `unique_ptr<EmbeddedFixedVector>::get()`) on T4; the racing write is
the malloc/ctor of the JITData itself in `DFG::Plan::tryFinalizeJITData`
(DFGPlan.cpp:835 → ButterflyArray::createImpl) on T6. The HB edge is the
`storeStoreFence + relaxed-atomic m_jitData store` from §20.3.7 — but
relaxed atomics do not establish HB for TSAN, so the JITData interior
(m_exits.m_storage, the TrailingArray header) reads as racing the ctor.

**Spec row:** SPEC-jit §4.x publish-via-one-pointer. Blessed by design;
the missing piece is TSAN visibility, not correctness. §13.4 precedent
(ConcurrentVector/ConcurrentBuffer/ConcurrentPtrHashSet): TSAN-gated
acquire on the publishing-pointer load.

**Ruling: relaxed-atomic** (unchanged). **fixShape update:** under
`TSAN_ENABLED`, `CodeBlock::dfgJITData()` / `baselineJITData()` use
`memory_order_acquire` instead of relaxed (CodeBlock.h). Production keeps
relaxed. (Do NOT touch DFGJITCode.h again — the §20.3.5 fix is correct;
the publication edge is one level up.)

**Files:** `Source/JavaScriptCore/bytecode/CodeBlock.h` only.

**Evidence (dw1-sort-comparator-osr.js):**
```
Read of size 8 by T4: unique_ptr<EmbeddedFixedVector<...>>::get
  FixedVector<MacroAssemblerCodeRef>::operator[] FixedVector.h:165
  DFG::JITData::exitCodePtrConcurrent DFGJITCode.h:213
  operationCompileOSRExit DFGOSRExit.cpp:219
Previous write of size 8 by T6: malloc
  ButterflyArray<DFG::JITData,...>::createImpl ButterflyArray.h:59
  DFG::JITData::tryCreate DFGJITCode.h:413
  DFG::Plan::tryFinalizeJITData DFGPlan.cpp:835  (other mutator's finalize)
```

#### 21.4.3 jit-one-sider-allocator (26) — suppressions NOT applied

Raw keys: `('data race', aligned_alloc)` 15 + `('data race', malloc)` 3 +
`('data race', growArrayRight)` 3 (havebadtime-vs-indexed-fastpath,
KNOWN-FAILING) + `('data race', calloc)` 1 +
`('data race', MarkedBlock::Header::Header)` 2 +
`('data race', Atomic<unsigned char>::Atomic)` 1 +
`('data race', GetByIdModeMetadata::GetByIdModeMetadata)` 1.

**Status: UNFIXED.** `Tools/tsan/suppressions.txt` mtime predates
reports-r23.log — the §20.3.10 four anchors were never added. The family
grew (20 → 26) by absorbing the §20.3.3 `dependency-loadandfence`
allocator-side residuals: loadAndFence is now atomic, so its pairing
against `MarkedBlock::Header::Header` / `aligned_alloc` is now a pure
"atomic probe vs allocator hand-out" (wave-7 class, §18.3). The
`GetByIdModeMetadata` ctor key is the same class with both stacks present:
LLInt's `performLLIntGetByID` atomic StructureID load (LLIntSlowPaths.cpp,
already atomic) vs `CodeBlock::finishCreation`'s plain
`GetByIdModeMetadata()` ctor on a fresh CodeBlock — the atomic probe is on
a recycled metadata-table slot; revalidated by the StructureID check.

**Ruling: suppress** (unchanged). **fixShape update:** apply the §20.3.10
four anchors (`Butterfly::growArrayRight`,
`JSRopeString::CompactFibers::CompactFibers`,
`PreciseAllocation::PreciseAllocation`, `MarkedBlock::tryCreate`) AND
extend with three more wave-7-class anchors:
`race:JSC::MarkedBlock::Header::Header` (covers the 2 loadAndFence-vs-ctor
residuals), `race:JSC::GetByIdModeMetadata::GetByIdModeMetadata` (atomic
LLInt IC probe vs fresh-CodeBlock metadata ctor; the IC writer side is
already atomic per §3.4/§9.2, so a plain-writer regression surfaces under
its own anchor), and a `race:calloc` entry is **NOT** added (too broad —
the single calloc report routes through `MarkedBlock::tryCreate` once that
anchor lands; re-check at r25). The `Atomic<unsigned char>::Atomic` ctor
key (1, congc-t8) is the same MarkedBlock-hand-out class via
PreciseAllocation; covered by the PreciseAllocation anchor. All anchors
carry the wave-7 justification template.

**Files:** `Tools/tsan/suppressions.txt`.

#### 21.4.4 regexp-cached-result-visit (1) — NEW

`RegExpCachedResult::record` (RegExpGlobalDataInlines.h:56, `__tsan_memset`
of the `m_reified` + `m_oneCharacterMatch` bool pair, size 2) inside
`JSGlobalObject::init` on a JS Thread (T19, via
`$vm.createGlobalObject`) vs
`RegExpCachedResult::visitAggregateImpl<SlotVisitor>`
(RegExpCachedResult.cpp:40, plain read of `m_reified`) from a HeapHelper
marker (T5, `drainFromShared` during another mutator's
`conductSharedCollection`). I.e. SPEC-congc N-mutator GC visits a
JSGlobalObject whose `init()` is mid-seeding RegExpCachedResult.

**Spec row:** SPEC-congc visitor-vs-init + OM GT (cell-state words read by
the concurrent visitor go through a concurrent accessor). The §3.18
`regexp-shared` real-bug ruling covered mutator-vs-mutator on the
RegExpCachedResult JSValue slots and was closed; THIS is a different
pairing (GC visitor vs `record`'s flag bytes). Benign by value (a stale
`m_reified=false` makes the visitor skip the reified slots, which are
null during init; a stale `true` over-visits null WriteBarriers, which
`SlotVisitor::append` tolerates), but plain access is UB.

**Ruling: relaxed-atomic.** `m_reified` / `m_oneCharacterMatch` →
`WTF::atomicLoad(..., relaxed)` in `visitAggregateImpl`;
`WTF::atomicStore(..., relaxed)` in `record()` (replacing the implicit
struct-copy/memset). Flag-off codegen identical.

**Files:** `Source/JavaScriptCore/runtime/RegExpCachedResult.h`,
`Source/JavaScriptCore/runtime/RegExpCachedResult.cpp`,
`Source/JavaScriptCore/runtime/RegExpGlobalDataInlines.h`.

**Evidence (congc-t8-stop-interleaving.js):**
```
Write of size 2 by T19 (JS Thread): __tsan_memset
  RegExpCachedResult::record RegExpGlobalDataInlines.h:56
  JSGlobalObject::init JSGlobalObject.cpp:2055
  JSGlobalObject::create  ($vm.createGlobalObject)
Previous read of size 1 by T5 (HeapHelper): visitAggregateImpl RegExpCachedResult.cpp:40
  RegExpGlobalData::visitAggregate
  JSGlobalObject::visitChildrenImpl  (drainFromShared marker)
```

#### 21.4.5 growarrayright-vs-sparsemap-atomics (1) — NEW, REAL BUG

`Butterfly::growArrayRight` `__tsan_memcpy` (ButterflyInlines.h:224, plain
read of size 8) on T307 via `JSObject::increaseVectorLength` ←
`JSObject::putDirectIndexForAtomicsMissingAdd` (ThreadAtomics.cpp:649) ←
`atomicsStoreOnPropertyGilOff` (the indexed-miss arm of `Atomics.store`)
vs `WriteBarrierBase<SparseArrayValueMap>::storeCell` atomic store
(WriteBarrier.h:167) on the main thread via
`JSObject::allocateSparseIndexMap` ←
`ensureArrayStorageExistsAndEnterDictionaryIndexingMode` ←
`defineOwnIndexedProperty`. Both threads mutate the **same object's
butterfly** at the same address (heap block 0x72a000204000): main thread
has converted the object to ArrayStorage + dictionary-indexing and is
installing `m_sparseMap`; T307 still holds a stale view and is memcpy'ing
the (now-ArrayStorage) butterfly to grow what it believes is contiguous
storage.

**Spec row:** OM §4 (butterfly reshape protocol) — NOT blessed.
`putDirectIndexForAtomicsMissingAdd` calling `increaseVectorLength`
without the cellLock / indexing-type re-check while another mutator
performs `defineOwnIndexedProperty` → dictionary conversion is two
mutators reshaping the same butterfly. The plain memcpy is the UB
symptom; the underlying interleaving is a correctness hazard (T307 may
publish a grown butterfly that drops main's SparseArrayValueMap and
dictionary conversion).

**Ruling: real-bug.** Fix shape: in
`JSObject::putDirectIndexForAtomicsMissingAdd` (ThreadAtomics.cpp), take
the object's cellLock (or the OM §4 indexed-reshape lock) and re-check
`indexingType()` before calling `increaseVectorLength`; if the type is no
longer contiguous/int32/double, fall through to the generic
`putDirectIndexSlowOrBeyondVectorLength` path. The CVE-pattern test
`cve/mc-reent-store-missing-indexed-define-race.js` was written for
exactly this; route any functional fix through the CVE-audit map doc.

**Files:** `Source/JavaScriptCore/runtime/ThreadAtomics.cpp`.

**Evidence (cve/mc-reent-store-missing-indexed-define-race.js):**
```
Read of size 8 by T307: __tsan_memcpy
  Butterfly::growArrayRight ButterflyInlines.h:224
  JSObject::increaseVectorLength JSObject.cpp:6936
  JSObject::putDirectIndexForAtomicsMissingAdd ThreadAtomics.cpp:649
  atomicsStoreOnPropertyGilOff  (Atomics.store indexed-miss arm)
Previous atomic write of size 8 by main: WriteBarrierBase<SparseArrayValueMap>::storeCell
  JSObject::allocateSparseIndexMap JSObject.cpp:5892
  JSObject::ensureArrayStorageExistsAndEnterDictionaryIndexingMode
  JSObject::defineOwnIndexedProperty  (Object.defineProperty)
```

#### 21.4.6 cachedcall-protoframe-crossthread (1) — NEW, REAL BUG

`ProtoCallFrame::setCodeBlock` atomic store (ProtoCallFrameInlines.h:76)
on T1 via `CachedCall::unlinkOrUpgradeImpl` (CachedCall.h:103) ←
`CallLinkInfoBase::unlinkOrUpgrade` ←
`CodeBlock::unlinkOrUpgradeIncomingCalls` ←
`ScriptExecutable::installCode` (T1's `BaselineJITPlan::finalize`) vs
`Interpreter::executeCachedCall` `__tsan_memcpy` (InterpreterInlines.h:143,
plain read of the ProtoCallFrame) on T4. **Location is stack of T4.** I.e.
T1's `installCode` walks the CodeBlock's incoming-call list, finds T4's
stack-resident `CachedCall`, and writes into its `ProtoCallFrame` while T4
is mid-`executeCachedCall` copying that same frame.

**Spec row:** SPEC-jit §4.x code lifecycle / §5.8 — NOT blessed.
`CachedCall` is a stack-local fast-path object (StringPrototype
`replaceUsingRegExpSearch` puts it on the calling thread's stack); cross-
thread mutation of another thread's stack-resident ProtoCallFrame has no
spec coverage. The atomic store on the writer side does not make the
reader's struct-memcpy safe, and even if both were atomic the semantics
are wrong (T4 may execute against a torn CodeBlock/entrypoint pair).

**Ruling: real-bug.** Fix shape: `CachedCall` registration in the
CodeBlock's incoming-call list must be per-thread (only the owning
thread's `installCode` may upgrade it), OR
`CodeBlock::unlinkOrUpgradeIncomingCalls` skips `CachedCall` entries whose
owning thread ≠ current and instead bumps a generation counter that
`CachedCall::call()` checks before each `executeCachedCall` (lazy upgrade
on the owning thread). Prefer the generation-check shape (no extra
locking on the hot path; one relaxed-atomic compare per `call()`).

**Files:** `Source/JavaScriptCore/interpreter/CachedCall.h`,
`Source/JavaScriptCore/interpreter/InterpreterInlines.h`,
`Source/JavaScriptCore/bytecode/CodeBlock.cpp`.

**Evidence (vmstate/regexp-churn-threads.js):**
```
Atomic write of size 8 by T1: ProtoCallFrame::setCodeBlock
  CachedCall::unlinkOrUpgradeImpl CachedCall.h:103
  CodeBlock::unlinkOrUpgradeIncomingCalls CodeBlock.cpp:2521
  ScriptExecutable::installCode  (BaselineJITPlan::finalize on T1)
Previous read of size 8 by T4: __tsan_memcpy
  Interpreter::executeCachedCall InterpreterInlines.h:143
  CachedCall::call CachedCall.h:69
  replaceUsingRegExpSearch  (T4's String.prototype.replace)
Location is stack of thread T4.
```

### 21.5 r24 run-health notes

Reconciliation: 35 + 2 + 26 + 1 + 1 + 1 = **66**. Reconciles exactly to
`grep -c 'WARNING: ThreadSanitizer' reports-r24.log`.

Three new exit-3 functional failures (api/blocking-gate,
cve/mc-df-arraycopy-relabel, cve/mc-gc-weakgcmap-registry-vs-prune) carry
zero TSAN reports — out of this campaign's scope; queued for the
functional fix round alongside the four KNOWN-FAILING tests.
semantics/frozen-seal-race no longer aborts (was exit-134 in r23; now
exit-0) — likely a side-effect of the §20.3.8 dfg-weakrefs lock fix.

### 21.6 Post-wave-2 review amendments (applied)

Two reviewer findings verified and amended before the r25 snapshot:

**(A) codeblock-jitdata-publish — baseline-arm store missed.** The wave-2
fix introduced `jitDataLoadOrder`/`jitDataStoreOrder` (TSAN-gated
acquire/release, §13.4 precedent) and applied them to
`baselineJITData()`/`dfgJITData()` loads and the inline `setDFGJITData()`
store in CodeBlock.h, but `setBaselineJITData()` in CodeBlock.cpp:822
still used hardcoded `std::memory_order_relaxed` (CodeBlock.cpp was owned
by family 6 in the wave-2 file table, so the jitdata fixer correctly did
not touch it). Under TSAN the baseline arm's acquire-load therefore had no
paired release-store, leaving the §21.4.2 next-layer interior race
(BaselineJITData TrailingArray header / `propertyInlineCaches()` vs the
ButterflyArray ctor in `setupWithUnlinkedBaselineCode`) unclosed on the
baseline arm — and 3-of-4 §20.3.7 evidence stacks were baseline.
**Amended:** CodeBlock.cpp:822 `std::memory_order_relaxed` →
`jitDataStoreOrder`. No non-TSAN behavior change (`jitDataStoreOrder` ==
relaxed there; the preceding `storeStoreFence()` already orders for
hardware).

**(B) marks-bitset-plain-readers — concurrentCopyFrom flag-off codegen.**
§20.3.2(b)'s "identical x86 codegen" claim was wrong for
`concurrentCopyFrom`: the original `live = header.m_marks` is a 128-byte
`std::array` struct assignment that clang lowers to a vectorized memcpy,
whereas the 32-iteration relaxed-atomic word loop is not
coalesced/vectorized by the optimizer. The wave-24 fix applied the loop
unconditionally at MarkedBlockInlines.h:285/294, degrading the flag-off
sweep prologue from a handful of SIMD moves to 32 scalar load/store pairs
(and 4× that on `CeilingOnPageSize>16KB` targets). The
m_marks-vs-`concurrentTestAndSet` race is upstream-preexisting and
value-benign (the `m_markingVersion` fence protocol already orders it), so
the relaxed read shape is needed only to satisfy TSAN — making a
TSAN_ENABLED gate the §13.4-consistent shape. **Amended:**
`BitSet::concurrentCopyFrom` body now `#if TSAN_ENABLED` per-word relaxed
loop / `#else *this = other` (original struct copy). Call sites in
MarkedBlockInlines.h unchanged. Flag-off codegen restored to the
pre-wave-24 vectorized memcpy; TSAN build keeps the relaxed snapshot.

## 22. Round r25 — wave-2 verification (2026-06-19)

### 22.0 Build / config check

`WebKitBuild/TSan/CMakeCache.txt`: `ENABLE_C_LOOP:BOOL=OFF`,
`ENABLE_JIT:BOOL=ON` (unchanged from §20.0). `ninja -n jsc` = no work to do;
binary mtime (Jun 19 01:16, epoch 1781831771) > newest source (BitSet.h,
epoch 1781831537) — the §21.6 amendments and all wave-2 fixes are in the
binary. `nm | grep -c CLoop` = 0. Smoke (`JSTests/threads/smoke.js`, GIL-off
env, full JIT, 3x): PASS each run, exit 0, ZERO TSAN reports. **Zero CLoop
frames** in r25 (`grep -ci CLoop reports-r25.log` = 0).

Wave-2 fixers touched (since reports-r24.log): WTF/wtf/BitSet.h,
bytecode/CodeBlock.{h,cpp}, runtime/RegExpCachedResult.{h,cpp},
runtime/RegExpGlobalDataInlines.h, runtime/ThreadAtomics.cpp,
interpreter/CachedCall.h, interpreter/InterpreterInlines.h.
**`Tools/tsan/suppressions.txt` was NOT modified** — the §21.4.3
`jit-one-sider-allocator` suppress ruling still has not landed (two waves
running).

### 22.1 Run configuration

Same as §20.1. Corpus: 252 candidate `JSTests/threads/**/*.js` (246 ran, 6
//@ skip). Result: **32 reports** in 8 test files, 5 raw deduped stack-pair
keys, grouped into the **2 open families** below (+ 5 r24 families now at
0). Exit-code spectrum: 223 exit-0, 13 exit-3 (composition: bench/* ×8 +
arrays/push-resize + api/blocking-gate + cve/mc-df-arraycopy-relabel +
cve/mc-gc-weakgcmap-registry-vs-prune + cve/mc-life-creator-thread-dies —
unchanged set vs r24 except mc-life-creator-thread-dies traded places with
lifecycle/create-basics; functional, out of scope), 8 exit-66, 1 exit-134
(semantics/frozen-seal-race — see §22.5), 1 exit-124
(w16-c1-prevent-collection timeout, unchanged).

Per-test logs: `Tools/threads/tsan/r25/`. Concatenated reports:
`Tools/threads/tsan/reports-r25.log` (also written to `reports-r2.log` per
the campaign-snapshot contract). Dedup output:
`Tools/threads/tsan/families-r25.txt`.

`unsuppressedCount = 32` (66 → 32; nothing newly suppressed; no CLoop
families existed).

### 22.2 r24-family verification

| r24 id | r24 count | r25 count | status |
|--------|-----------|-----------|--------|
| marks-bitset-plain-readers | 35 | **0** | DONE (concurrentFilter own-side bits[i] now relaxed-atomic; §21.6(B) TSAN-gate on concurrentCopyFrom verified — no resurfaced specializedSweep keys) |
| codeblock-jitdata-publish | 2 | **0** | DONE (TSAN-gated acquire/release on m_jitData; §21.6(A) baseline-arm store verified — no FixedVector/TrailingArray-vs-malloc keys) |
| jit-one-sider-allocator | 26 | **27** | UNFIXED — suppressions still not applied, see §22.4.1 |
| regexp-cached-result-visit | 1 | **0** | DONE (m_reified/m_oneCharacterMatch relaxed-atomic; congc-t8 no longer carries the visitAggregateImpl key) |
| growarrayright-vs-sparsemap-atomics | 1 | **0** | DONE (cellLock + indexingType re-check in putDirectIndexForAtomicsMissingAdd; mc-reent-store-missing-indexed-define-race now exit-0, 0 reports) |
| cachedcall-protoframe-crossthread | 1 | **0** | DONE (vmstate/regexp-churn-threads.js exit-0, 0 reports) |

5 of 6 r24 families closed. **34 reports eliminated** (52%).

### 22.3 Family table (r25)

| # | id | count | spec row | ruling | files (wave ownership) |
|---|----|-------|----------|--------|------------------------|
| 1 | jit-one-sider-allocator | 27 | §0 accepted tradeoff + §18.3 wave-7 anchor class | suppress | Tools/tsan/suppressions.txt |
| 2 | globalobject-init-vs-visitor | 5 | SPEC-congc visitor-vs-init; OM GT (cell field words via concurrent accessor) | relaxed-atomic | runtime/JSGlobalObject.cpp |

Both families have **disjoint file ownership**.

### 22.4 Per-family analysis

#### 22.4.1 jit-one-sider-allocator (27) — suppressions STILL not applied

Raw keys: `('data race', aligned_alloc)` 19 + `('data race', malloc)` 4 +
`('data race', MarkedBlock::Header::Header)` 2 +
`('data race', Butterfly::growArrayRight)` 2.

**All 27 reports verified by stack inspection** to be the §18.3 wave-7
"atomic probe vs allocator hand-out" class. Unlike r23/r24, the
current-access stack is now SYMBOLIZED in most reports (the
`loadAndFence`/`StructureID::relaxedLoad`/`PreciseAllocation::isMarked`
readers became atomic in §20.3.3/§21.6, so dedup.py no longer drops them as
JIT-one-siders) — but every current-access is an **Atomic read** and every
previous-write is the raw allocator (`aligned_alloc → MarkedBlock::tryCreate`,
`malloc → PreciseAllocation::tryCreate{,ForLowerTierPrecise}`,
`MarkedBlock::Header::Header` ctor, `Butterfly::growArrayRight` memcpy on the
KNOWN-FAILING havebadtime test). No plain C++ access of ours exists on
either side; the cell-handout protocol IS the real HB edge TSAN cannot see.

The r24-only sub-keys (`JSRopeString::CompactFibers`, `GetByIdModeMetadata`,
`calloc`, `Atomic<u8>::Atomic`) did **not** recur in r25 (run-to-run
variance; §21.4.3 already adjudicated them as the same class).

**Ruling: suppress** (unchanged, third wave running). **fixShape update:**
the §21.4.3 anchor list is correct for r25 with ONE adjustment — the
PreciseAllocation reports route through `tryCreate`/
`tryCreateForLowerTierPrecise` (the malloc frame), NOT the
`PreciseAllocation::PreciseAllocation` ctor frame. Anchor on
`race:JSC::PreciseAllocation::tryCreate` (prefix-matches both). Final anchor
set (each with the wave-7 justification template):
`race:JSC::MarkedBlock::tryCreate` (covers 19 + the w16 timeout's 1),
`race:JSC::MarkedBlock::Header::Header` (2),
`race:JSC::PreciseAllocation::tryCreate` (4),
`race:JSC::Butterfly::growArrayRight` (2, KNOWN-FAILING source only),
plus the §21.4.3 r24-variance anchors `race:JSC::JSRopeString::CompactFibers`
and `race:JSC::GetByIdModeMetadata::GetByIdModeMetadata`.

**Files:** `Tools/tsan/suppressions.txt`.

**Evidence (congc-t5-celllock-audit.js, malloc sub-key):**
```
Atomic read of size 1 by T6 (HeapHelper): Atomic<bool>::load
  PreciseAllocation::isMarked PreciseAllocation.h:92
  SlotVisitor::appendUnbarriered SlotVisitorInlines.h:54
  JSOrderedHashTable<MapTraits>::visitChildrenImpl  (drainFromShared marker)
Previous write of size 8 by T1 (mutator): malloc
  PreciseAllocation::tryCreate PreciseAllocation.cpp:94
  CompleteSubspace::tryAllocateSlowForClient
  JSCellButterfly::tryCreate ← JSOrderedHashTableHelper::rehash ← JSMap::set
```

#### 22.4.2 globalobject-init-vs-visitor (5) — NEW

Raw key: `('data race', JSGlobalObject::init)` 5, all from
congc-t8-stop-interleaving.js (the same test that surfaced §21.4.4
regexp-cached-result-visit; that family is now 0 and the test runs deeper
into init(), exposing the next plain-writer site).

`JSGlobalObject::init()` (JSGlobalObject.cpp:1903) does
`m_arrayStructureForIndexingShapeDuringAllocation[i] =
m_originalArrayStructureForIndexingShape[i]` — `WriteBarrierStructureID`
struct copy-assignment, which is a **plain** `m_structureID =
other.m_structureID` store. The reader is
`WriteBarrierStructureID::value()` → `StructureID::relaxedLoad()` (atomic,
size 4) inside `JSGlobalObject::visitChildrenImpl` (JSGlobalObject.cpp:3773)
on a HeapHelper marker (`drainFromShared` during another mutator's
conducted GC). The JSGlobalObject cell is conservatively reachable (T18's
stack) before `finishCreation()` returns under SPEC-congc N-mutator GC.

**Spec row:** SPEC-congc visitor-vs-init + OM GT (cell field words read by
the concurrent visitor go through a concurrent accessor). The READER side
is already the blessed `relaxedLoad`; the WRITER side (struct
copy-assignment) is plain = UB. Benign by value (a stale zero StructureID
makes `SlotVisitor::append(WriteBarrierStructureID)` no-op; a stale non-zero
ID over-visits a Structure that is independently reachable from
`m_originalArrayStructureForIndexingShape`).

**Ruling: relaxed-atomic.** Replace the struct copy-assignment loop at
JSGlobalObject.cpp:1903 with
`m_arrayStructureForIndexingShapeDuringAllocation[i].setWithoutBarrier(
m_originalArrayStructureForIndexingShape[i].get())` (or equivalent
`StructureID::relaxedStore` path). Flag-off codegen identical (relaxed =
plain MOV). Do NOT change `WriteBarrierStructureID::operator=` itself in
WriteBarrier.h — that header's atomicization was completed in the r0-r17
campaign and a behavior change there is higher blast-radius than the one
call site; if a second `operator=` site surfaces later, escalate to the
header.

**Files:** `Source/JavaScriptCore/runtime/JSGlobalObject.cpp`.

**Evidence (congc-t8-stop-interleaving.js):**
```
Atomic read of size 4 by T6 (HeapHelper): StructureID::relaxedLoad StructureID.h:79
  WriteBarrierStructureID::value WriteBarrier.h:404
  SlotVisitor::append(WriteBarrierStructureID const&) SlotVisitorInlines.h:124
  JSGlobalObject::visitChildrenImpl JSGlobalObject.cpp:3773  (drainFromShared marker)
Previous write of size 4 by T18 (JS Thread):
  JSGlobalObject::init JSGlobalObject.cpp:1903
    (m_arrayStructureForIndexingShapeDuringAllocation[i] = m_originalArrayStructureForIndexingShape[i])
  JSGlobalObject::finishCreation ← JSGlobalObject::create ← $vm.createGlobalObject
```

### 22.5 r25 run-health notes

Reconciliation: 27 + 5 = **32**. Reconciles exactly to
`grep -c 'WARNING: ThreadSanitizer' reports-r25.log`.

`semantics/frozen-seal-race.js` exit-134 (silent abort, 1-line log, no
TSAN report): was exit-134 in r23, exit-0 in r24, exit-134 again in r25.
Intermittent across the three rounds with no wave-2 fix touching its code
path beyond ThreadAtomics.cpp; treated as a flaky functional abort, NOT a
wave-2 regression. Queued for the functional fix round.

`cve/mc-grow-buffer-storm.js` exit-66 with **zero data-race reports** (same
as r24): the log carries `ThreadSanitizer:DEADLYSIGNAL` / `SEGV on unknown
address 0x3ff` with pc in JIT code (`<unknown module>`). Functional crash
in JIT-generated code, not a TSAN data race; out of this campaign's scope
(the §0 accepted-tradeoff coverage gap — belongs to the amplifier/protocol
tests).

`w16-c1-prevent-collection.js` exit-124 timeout now carries 1 TSAN report
(r24 had 0): an `Atomic getVectorLength` vs `aligned_alloc →
MarkedBlock::tryCreate` pair — the §22.4.1 suppress class. Counted in
family 1.

## 23. Round r26 — wave-3 verification (2026-06-19)

### 23.0 Build / config check

`WebKitBuild/TSan/CMakeCache.txt`: `ENABLE_C_LOOP:BOOL=OFF`,
`ENABLE_JIT:BOOL=ON` (unchanged from §20.0). `ninja -n jsc` = no work to do;
binary mtime (epoch 1781832683) > newest source (JSGlobalObject.cpp, epoch
1781832598) — the wave-3 fix is in the binary. `nm | grep -c CLoop` = 0.
Smoke (`JSTests/threads/smoke.js`, GIL-off env, full JIT, 3x): 0 TSAN reports
each run. **Zero CLoop frames** in r26 (`grep -ci CLoop reports-r26.log` = 0).

Wave-3 fixers touched (since reports-r25.log): runtime/JSGlobalObject.cpp
ONLY. **`Tools/tsan/suppressions.txt` was NOT modified** — the §21.4.3 /
§22.4.1 `jit-one-sider-allocator` suppress ruling has now failed to land
across **three consecutive waves** (r24, r25, r26). No wave-3 fixer was
assigned to it (the only file edited was JSGlobalObject.cpp).

### 23.1 Run configuration

Same as §20.1. Corpus: 252 candidate `JSTests/threads/**/*.js` (246 ran, 6
//@ skip). Result: **40 reports** in 9 test files, 11 raw deduped stack-pair
keys, grouped into the **2 open families** below (+ 1 r25 family now at 0).
Exit-code spectrum: 219 exit-0, 14 exit-3, 11 exit-66, 1 exit-134
(semantics/frozen-seal-race — unchanged §22.5 flake), 1 exit-124
(w16-c1-prevent-collection timeout, this round 0 reports).

Per-test logs: `Tools/threads/tsan/r26/`. Concatenated reports:
`Tools/threads/tsan/reports-r26.log` (also written to `reports-r3.log` per
the campaign-snapshot contract). Dedup output:
`Tools/threads/tsan/families-r26.txt`.

`unsuppressedCount = 40` (32 → 40; nothing newly suppressed; no CLoop
families existed). The increase is entirely run-to-run variance inside the
unfixed `jit-one-sider-allocator` family + 1 new 2-report family.

### 23.2 r25-family verification

| r25 id | r25 count | r26 count | status |
|--------|-----------|-----------|--------|
| jit-one-sider-allocator | 27 | **38** | UNFIXED — suppressions still not applied (3rd wave running), see §23.4.1 |
| globalobject-init-vs-visitor | 5 | **0** | DONE (`setWithoutWriteBarrier(...get())` at JSGlobalObject.cpp:1910; congc-t8 no longer carries the `JSGlobalObject::init` key) |

1 of 2 r25 families closed. **5 reports eliminated**; net +8 from
jit-one-sider variance.

### 23.3 Family table (r26)

| # | id | count | spec row | ruling | files (wave ownership) |
|---|----|-------|----------|--------|------------------------|
| 1 | jit-one-sider-allocator | 38 | §0 accepted tradeoff + §18.3 wave-7 anchor class | suppress | Tools/tsan/suppressions.txt |
| 2 | parked-stack-stale-snapshot-read | 2 | SPEC-congc T5-rootscan stale-window bounds-check | suppress | Tools/tsan/suppressions.txt |

Both families own the **same file** (suppressions.txt) — assign to ONE
wave-4 fixer.

### 23.4 Per-family analysis

#### 23.4.1 jit-one-sider-allocator (38) — suppressions STILL not applied (third wave)

Raw keys: `aligned_alloc` 23 + `MarkedBlock::Header::Header` 4 +
`Butterfly::growArrayRight` 3 + `malloc` 2 + `(MarkedBlock::handle,
aligned_alloc)` 1 + `(MarkedBlock::Handle::cellSize, malloc)` 1 +
`Atomic<bool>::Atomic` 1 + `calloc` 1 +
`(JSArrayBuffer::JSArrayBuffer, JSArrayBuffer::isResizableOrGrowableShared)` 1
+ `DeferrableRefCountedBase::DeferrableRefCountedBase` 1.

**All 38 reports verified by stack inspection** to be the §18.3 wave-7
"allocator hand-out vs first cross-thread access" class. The 23+4+3+2+1+1+1
= 35 reports route through the §22.4.1 anchor frames (`MarkedBlock::tryCreate`,
`MarkedBlock::Header::Header`, `PreciseAllocation::tryCreate*`,
`Butterfly::growArrayRight`). The two new `MarkedBlock::handle`/
`MarkedBlock::Handle::cellSize` reader-side keys are SlotVisitor reading the
block-header `m_handle`/`m_atomSizeClass` words on a fresh block; the
previous-write side IS `MarkedBlock::tryCreate` so the existing anchor
already covers them.

**3 NEW r26 ArrayBuffer-handout sub-keys** (cve/mc-grow-s4-detach-nullvec-repro
+ cve/mc-life-detach-quarantine-storm — both ran clean in r25, surfaced this
round):
- `calloc → ArrayBufferContents::tryAllocate` vs an Atomic typed-array probe
  on the backing store (1).
- `DeferrableRefCountedBase` ctor (`m_refCount{2}` non-atomic init per C++
  std::atomic ctor semantics) vs the atomic `ref()` fetch_add (1).
- `JSArrayBuffer::JSArrayBuffer` ctor `m_impl = arrayBuffer.get()` vs
  `JSArrayBuffer::isResizableOrGrowableShared()` plain `m_impl` read (1).
  `m_impl` is **set-once-immutable** (only assignment is JSArrayBuffer.cpp:38;
  no other writer in the tree) — OM GT immutable-after-ctor cell field, no
  concurrent-accessor required. The HB is the cell-handout protocol: the
  ArrayBuffer/JSArrayBuffer were created on the main thread and published to
  the worker via a JS-value store + JIT-code load (the `<null>` frames at
  #7/#3 in both racing stacks). §0 accepted-tradeoff.

**Ruling: suppress** (unchanged, FOURTH wave running). **fixShape update:**
the §22.4.1 anchor list + THREE NEW ArrayBuffer-handout variance anchors,
each with the wave-7 justification template referencing OM GT
immutable-after-ctor + §0 JIT-HB:
`race:JSC::ArrayBuffer::tryCreate` (covers the DeferrableRefCounted-ctor
malloc + the calloc tryAllocate, both have it at frame #4-#6),
`race:JSC::ArrayBufferContents::tryAllocate` (belt-and-braces for the calloc
sub-key in case ArrayBuffer::tryCreate is renamed),
`race:JSC::JSArrayBuffer::JSArrayBuffer` (the m_impl ctor write).

Full anchor set to land: `race:JSC::MarkedBlock::tryCreate`,
`race:JSC::MarkedBlock::Header::Header`,
`race:JSC::PreciseAllocation::tryCreate`, `race:JSC::Butterfly::growArrayRight`,
`race:JSC::JSRopeString::CompactFibers`,
`race:JSC::GetByIdModeMetadata::GetByIdModeMetadata`,
`race:JSC::ArrayBuffer::tryCreate`, `race:JSC::ArrayBufferContents::tryAllocate`,
`race:JSC::JSArrayBuffer::JSArrayBuffer`.

**Files:** `Tools/tsan/suppressions.txt`.

**Evidence (cve/mc-life-detach-quarantine-storm.js, m_impl sub-key):**
```
Read of size 8 by T84: JSArrayBuffer::isResizableOrGrowableShared JSArrayBuffer.h:53
  constructGenericTypedArrayViewImpl<Int32Adaptor> ← constructInt32Array
  <null>  (JIT)
Previous write of size 8 by main: JSArrayBuffer::JSArrayBuffer JSArrayBuffer.cpp:38
  JSArrayBuffer::create ← constructArrayBuffer
  <null>  (JIT)
Location: heap block (MarkedBlock::tryCreate)
```

#### 23.4.2 parked-stack-stale-snapshot-read (2) — NEW

Raw key: `(MachineThreads::tryCopyCooperativelyParkedThreadStack,
WTF::Locker<WTF::Lock>::Locker)` 2, both from jit/int-gate-epoch-reclaim.js
(ran exit-0 in r25; surfaced this round).

The GC conductor (`gatherStackRoots → tryCopyCooperativelyParkedThreadStack`,
MachineStackMarker.cpp:262/263) reads `snapshot.stackOrigin` /
`snapshot.stackTop` via a possibly-stale parked-root-snapshot pointer. The
snapshot struct lives **on the parked thread's stack**; in the stale window
(thread has unparked and is RE-entering the park path) that stack region has
been overwritten by a fresh `Locker` local inside `VMManager::notifyVMStop`
(the "writer" — Locker.h:84/85, the `m_lockable`/`m_isLocked` member stores).
TSAN location: "stack of thread T3".

**Spec row:** SPEC-congc T5-rootscan stale-snapshot window. The in-source
comment at MachineStackMarker.cpp:255-266 documents the protocol: in the
well-behaved case the seq_cst `m_parkedRootSnapshot` pointer pair orders the
field reads; in the **stale window NO ordering is relied upon** — the
function bounds-checks the latched values against `thread.stack()` and bails.
Both racing values (an arbitrary `WTF::Lock*` and a `bool`) fail the bounds
check by construction.

**Ruling: suppress.** The reader is, by design, reading arbitrary stack words
on another live thread; cannot be atomicized (the "writer" is whatever
happens to be on that stack — anchoring on `Locker` would be wrong, the next
report would carry a different writer frame). The bounds-check defense is
the spec'd protocol. Anchor on the reader frame:
`race:JSC::MachineThreads::tryCopyCooperativelyParkedThreadStack`. Accepted
masking: a future plain access introduced INSIDE that function would be
swallowed; the function is small and the comment block makes the contract
explicit. Justification template: SPEC-congc T5-rootscan + the in-source
"stale window" comment.

This is **distinct** from §20.3.4 `parked-root-snapshot` (that family was the
`m_parkedRootSnapshotThread` heap field; closed in r24 via relaxed-atomic).
This family is the snapshot **payload struct on the other thread's stack**.

**Files:** `Tools/tsan/suppressions.txt`.

**Evidence (jit/int-gate-epoch-reclaim.js):**
```
Write of size 8 by T3 (JS Thread): WTF::Locker<WTF::Lock>::Locker Locker.h:84
  VMManager::notifyVMStop VMManager.cpp:1202
  VMTraps::handleTraps  (re-entering the park path)
Previous read of size 8 by main: MachineThreads::tryCopyCooperativelyParkedThreadStack
  MachineStackMarker.cpp:262  (latchedStackOrigin = snapshot.stackOrigin)
  MachineThreads::tryCopyOtherThreadStacks ← gatherStackRoots
  Heap::runFixpointPhase ← collectInMutatorThread (GC conductor)
Location: stack of thread T3
```

### 23.5 r26 run-health notes

Reconciliation: 38 + 2 = **40**. Reconciles exactly to
`grep -c 'WARNING: ThreadSanitizer' reports-r26.log`.

Exit-code drift vs r25 (all run-to-run flake; no wave-3 source change touches
any of these paths): congc-t3-barrier-storm 0→66 (1 report, family 1),
cve/mc-grow-s4-detach-nullvec-repro 0→66 (2, family 1),
cve/mc-life-detach-quarantine-storm 0→66 (2, family 1),
jit/int-gate-epoch-reclaim 0→66 (2, family 2),
semantics/ic-instanceof-vs-transition 0→66 (**0 data-race reports**;
DEADLYSIGNAL in JIT code — §0 accepted-tradeoff coverage gap, functional,
out of scope), lifecycle/create-basics 0→3 (functional, the §22.1-noted
trade-places flake), cve/mc-gc-finreg-cross-thread-gc 66→0,
gc-stress/zombie-uaf-canary 66→0.

`semantics/frozen-seal-race.js` exit-134 again (r23/r25/r26 = 134, r24 = 0;
unchanged §22.5 flaky functional abort). `w16-c1-prevent-collection.js`
exit-124 timeout, this round carried 0 reports (r25 carried 1).

**Process note:** the `jit-one-sider-allocator` suppress ruling was first
issued in §20.3.10 (r23) and has not landed across r24/r25/r26. Both open
r26 families resolve to `Tools/tsan/suppressions.txt`; the wave-4 partition
should assign **one** fixer to that file with both anchor blocks.

## 24. Round r27 — final-gate snapshot (2026-06-19)

### 24.0 Build / config check

`WebKitBuild/TSan/CMakeCache.txt`: `ENABLE_C_LOOP:BOOL=OFF`,
`ENABLE_JIT:BOOL=ON`, `ENABLE_DFG_JIT=1`, `ENABLE_FTL_JIT=1` (cmakeconfig.h).
`ninja -n jsc` = no work to do; binary mtime (epoch 1781832683) > newest
source (JSGlobalObject.cpp, epoch 1781832598). `nm | grep -c CLoop` = 0.
Smoke (`JSTests/threads/smoke.js`, GIL-off env, full JIT, 3x): PASS each
run, exit 0, ZERO TSAN reports. **Zero CLoop frames** in r27
(`grep -ci CLoop reports-r27.log` = 0) — the standing CLoop ruling required
no suppression entries.

Wave-4 fixer touched (since reports-r26.log): `Tools/tsan/suppressions.txt`
ONLY (no source changes). Both §23.3 open-family rulings landed: 9 anchors
for `jit-one-sider-allocator` (§23.4.1 final list:
MarkedBlock::tryCreate / MarkedBlock::Header::Header /
PreciseAllocation::tryCreate / Butterfly::growArrayRight /
JSRopeString::CompactFibers / GetByIdModeMetadata::GetByIdModeMetadata /
ArrayBuffer::tryCreate / ArrayBufferContents::tryAllocate /
JSArrayBuffer::JSArrayBuffer) + 1 anchor for
`parked-stack-stale-snapshot-read` (§23.4.2:
MachineThreads::tryCopyCooperativelyParkedThreadStack), each with the
wave-7 / SPEC-congc-T5 justification template.

### 24.1 Run configuration

Same as §20.1. Corpus: 252 candidate `JSTests/threads/**/*.js` (246 ran, 6
//@ skip). Result: **0 unsuppressed reports**
(`grep -c 'WARNING: ThreadSanitizer' reports-r27.log` = 0). Exit-code
spectrum: 229 exit-0, 14 exit-3, 2 exit-66, 1 exit-124, 0 exit-134.
Per-test logs: `Tools/threads/tsan/r27/`. Concatenated reports:
`Tools/threads/tsan/reports-r27.log`.

### 24.2 r26-family verification

| r26 id | r26 count | r27 count | status |
|--------|-----------|-----------|--------|
| jit-one-sider-allocator | 38 | **0** | DONE (suppressions landed; 9 anchors, §23.4.1) |
| parked-stack-stale-snapshot-read | 2 | **0** | DONE (suppression landed; 1 anchor, §23.4.2) |

Both r26 families closed. 40 reports eliminated. **`unsuppressedCount = 0`.**

### 24.3 Suppressions-file audit (final-gate requirement)

40 active `race:` entries. Mechanical audit (`awk` block-comment check):
**0 entries lack a justification comment**. By class:
- 6 pre-existing upstream parallel-GC entries (rule-1; §V7 record).
- 23 wave-7 "atomic probe vs allocator (re-)hand-out" reader-side anchors
  (§18.3; each cites the OM ground-truth + revalidation rationale).
- 1 rule-1 entry (`UnlinkedFunctionExecutable::recordParse`; upstream
  concurrent-compiler pairing, deterministic re-parse, size-capped).
- 9 r23-r26 "JIT-one-sider allocator hand-out" allocator-side anchors
  (§20.3.10/§23.4.1; the §0 accepted-tradeoff residual).
- 1 SPEC-congc T5-rootscan stale-window entry (§23.4.2).
- **0 CLoop entries** (CLoop not compiled in; standing ruling required none).
No `[CONTROL: PENDING]` entries are active (all five Part-B candidates
remain commented out per the V7 uncomment gate).

### 24.4 r27 run-health notes

Best run health of the r23+ campaign (229 exit-0 vs 219 in r26). Non-zero
exits, all functional / out of TSAN scope, every one carrying **0
data-race reports**:
- exit-3 (14): the 8 `bench/*` harness artifacts (bench files exit 3 by
  design when run as tests under TSAN slowness, unchanged since r5);
  api/blocking-gate, arrays/push-resize-multithread, lifecycle/create-basics
  (the §22.1 GIL-era unguarded-count expectations); cve/mc-df-arraycopy-relabel,
  cve/mc-gc-weakgcmap-registry-vs-prune, cve/mc-life-creator-thread-dies
  (CVE-audit-owned functional set, unchanged since r24).
- exit-66 (2): cve/mc-grow-buffer-storm (0 reports; was 66 in r17 record),
  semantics/ic-instanceof-vs-transition (DEADLYSIGNAL in JIT code — §0
  accepted-tradeoff coverage gap, functional, unchanged from §23.5).
- exit-124 (1): w16-c1-prevent-collection 420s timeout, 0 reports (same
  TSan-slowness timeout as r20/r25/r26).
- exit-134: NONE this round (semantics/frozen-seal-race ran exit-0; the
  §22.5 flake did not reproduce).

The four KNOWN-FAILING tests (semantics/date-cache-churn,
semantics/proto-cycle-race, semantics/symbol-registry-cross-thread,
gc-stress/havebadtime-vs-indexed-fastpath) all ran exit-0 with 0 reports
this round; their functional status is owned by the separate fix queue and
does not count against the TSAN gate per the campaign charter.

### 24.5 Normal-build sanity (gate 2)

- **Debug GIL-off smoke 10x: 10/10 pass** (binary mtime 1781832654 >
  newest source; `ninja -n` = no work).
- **Debug GIL-off full corpus** (`Tools/threads/run-tests.sh`, GIL-off env):
  **96 passed, 0 failed, 3 skipped**. Green (was 93/0/3 at the r21 record;
  +3 tests since corpus expansion).
- **Release bench gate**: see §24.6 below.

### 24.6 Release bench gate (RED — honest record)

Release rebuilt (358 targets; r24-r26 header changes had left it stale;
binary mtime 1781834292 > newest source). Waited for loadavg-1m < 2.0
(reached 1.91); ran `bench-gate.sh` (9-run medians, 1% threshold):

| benchmark | median | baseline | delta | verdict |
|-----------|--------|----------|-------|---------|
| array-element-read | 54.326 | 54.446 | -0.22% | ok |
| array-element-write | 51.888 | 51.844 | +0.08% | ok |
| flat-butterfly-read | 13.570 | 13.546 | +0.18% | ok |
| flat-butterfly-write | 62.500 | 62.418 | +0.13% | ok |
| inline-property-read | 27.147 | 27.325 | -0.65% | ok |
| inline-property-write | 54.361 | 54.425 | -0.12% | ok |
| megamorphic-access | 1253.295 | 1445.922 | -13.32% | ok |
| **transition-heavy-constructor** | **58.054** | **54.918** | **+5.71%** | **FAIL** |

7/8 within 1%. **The gate is RED.** A targeted re-measurement of
transition-heavy-constructor at loadavg-1m 1.76 (9 runs, median 57.87 ms,
+5.38%) confirms the number is load-stable, not noise.

**Attribution (recorded, not proven):** the closeout record (§Normal-build
sanity, 2026-06-09) measured +3.9% on this bench with the same baseline,
itself carrying the parked V5b ~+3.1% pre-campaign residual. The r27 number
is **~+1.7pp WORSE** than the closeout record. The only flag-off codegen
changes between the closeout tree and r27 are the r24-r26 wave fixers'
UNCONDITIONAL header conversions (WTF/wtf/BitSet.h, WTF/wtf/Atomics.h,
WTF/wtf/ConcurrentPtrHashSet.h, heap/IsoSubspace.h,
heap/MarkedBlock{.h,Inlines.h}, heap/Heap.h, bytecode/CodeBlock.h,
dfg/DFGJITCode.h — see §21.0 file list), several of which sit on this
benchmark's allocation/transition hot path. The closeout's METHODOLOGY
CAVEAT applies with even more force: the combined-revert experiment is now
owed for BOTH the closeout-era conversions AND the r24-r26 wave conversions.
Until that runs, attribution stays UNPROVEN; the milestone record must
carry the RED gate verbatim.

The task rule ("relaxed atomics must not have moved codegen") is VIOLATED
on this one benchmark; flagging per the "honest partials over fake green"
directive rather than parking silently. The other 7 benches confirm the
campaign's relaxed-atomic conversions did NOT move flag-off codegen on
property/array/megamorphic paths (megamorphic-access improved 13.3%, almost
certainly the r24 ConcurrentPtrHashSet / CodeBlock changes — record but do
not claim without bisection).

### 24.7 r27 bench-gate bisection — §24.6 attribution DISPROVEN (2026-06-19)

The §24.6 owed combined-revert experiment was run for the r24-r26 window.
**Result: NO OFFENDING HEADER.** The +1.7pp delta vs the 06-09 closeout
record is NOT in any r24-r26 source change.

**Method.** Three Release builds, each measured with the same
`Tools/threads/bisect-thc.sh` helper (9-21 run medians of
transition-heavy-constructor only), all at loadavg-1m < 2.0:

| build | source state | runs | load1m | median | delta |
|-------|--------------|------|--------|--------|-------|
| A | HEAD (f6a854fa66d8) — all r24-r26 conversions present | 15 | 1.92 | 58.108 | +5.81% |
| B | 7 of 9 candidate headers reverted to 2f5a5c4974ad (CodeBlock.h + DFGJITCode.h kept; their reverts break the build on `jitDataStoreOrder` / `exitCodePtrConcurrent` consumers) | 15 | 1.93 | 58.140 | +5.87% |
| C | **entire `Source/` reverted to 2f5a5c4974ad** (the closeout commit itself) | 15 | 1.93 | 58.708 | +6.90% |
| C′ | same as C, re-measured after settling to load 1.18/1.53 | 21 | 1.18 | 58.966 | +7.37% |

A, B, C, C′ are statistically indistinguishable. **The closeout commit
reproduces the r27 number on this host today**, so no change in the
2f5a5c4974ad..HEAD window is responsible for the §24.6-reported +1.7pp.

Full bench-gate at the restored tree (HEAD + the in-flight FUZZ r3-001
working changes; load 1.95, 9-run): 7/8 within ±0.3%,
transition-heavy-constructor +6.20%. The other 7 benches sitting at
baseline rules out whole-machine drift; the variance is bench-specific
(C′ raw samples span 51.9-61.3 ms, an 18% spread on a 58 ms median —
GC/tier-up timing on the butterfly-reallocation path).

**Per-header audit (why no flag-off codegen moved).** Of the 9 §24.6
candidates:
- TSAN-gated only (flag-off Release codegen byte-identical by `#if
  TSAN_ENABLED`): `WTF/wtf/Atomics.h` (Dependency::loadAndFence acquire
  arm), `WTF/wtf/ConcurrentPtrHashSet.h` (`tableConcurrently`),
  `WTF/wtf/BitSet.h::concurrentCopyFrom`, `heap/MarkedBlockInlines.h`
  (uses `concurrentCopyFrom`), `bytecode/CodeBlock.h`
  (`jitDataLoadOrder`/`jitDataStoreOrder` are `relaxed` in non-TSAN).
- Unconditional but off-hot-path: `heap/IsoSubspace.h` `tlcSlot`/
  `stampTlcSlot` (flag-off never reads the field — its own comment),
  `heap/Heap.h` `parkedRootSnapshotThread` (GC-conductor read only),
  `heap/MarkedBlock.h` `isMarkedRaw` → `concurrentGet` (sole caller is
  `sharedGCWindowWitnessSnapshot`, GC-only), `WTF/wtf/BitSet.h`
  `concurrentFilter` own-side relaxed loads (sweep prologue, not the
  per-object transition path).
- Cold-path only: `dfg/DFGJITCode.h` `setExitCode` (once per OSR-exit
  compile).

None sits on the structure-transition / butterfly-grow path the bench
exercises; the A=B measurement confirms this empirically.

**Disposition.** No fix is owed to the r24-r26 wave. The §24.6
"attribution (recorded, not proven)" is now **proven false** for this
window; the 06-09 +3.9% closeout record cannot be reproduced from its own
source on this host. The full residual (+5.8-7.4% on this host) transfers
to the PARKED V5b item per docs/threads/TSAN-RESULTS.md ("METHODOLOGY
CAVEAT") and docs/threads/INTEGRATE-ungil.md AB17g item 4 ("this host is
inadmissible … the binding V5b quiet-host interleaved-A/B (>=15 runs)
protocol has not been executed"). The TSAN gate (24.2: 0 unsuppressed)
stands; the bench gate stays RED with the V5b transfer recorded, not
silently parked. TSAN remains intact (no header was modified; tree is at
HEAD).

The owed COMBINED revert from TSAN-RESULTS.md (closeout-era unconditional
conversions: STT relaxed snapshot, PropertyTable ctor stores,
WriteBarrierBase setEarlyValue / WriteBarrierStructureID, GC visit
counters, JSObject.cpp `lastArraySize` std::atomic) is the remaining
attribution experiment for the +3.1%→current gap and is V5b-owned, out of
this campaign's scope.

