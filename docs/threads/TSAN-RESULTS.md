# TSAN Campaign Results (GIL-off, full JIT)

Date: 2026-06-09. Runner: solo continuation of the thread-tsan campaign
(waves 6-10 on top of the wave 1-5 record in `TSAN-TRIAGE.md` §§0-14).
Detailed per-wave fix record: `TSAN-TRIAGE.md` §15. Raw data:
`Tools/threads/tsan/{reports,families}-r{6..10}.log/txt` + per-test logs in
`Tools/threads/tsan/r{6..10}/`.

## Configuration (standing-ruling compliance)

- Binary: `WebKitBuild/TSan/bin/jsc` — **full JIT, real asm LLInt**
  (`ENABLE_C_LOOP:BOOL=OFF`, `ENABLE_JIT/DFG/FTL:BOOL=ON`, re-verified in
  `CMakeCache.txt` before every round; binary mtime > newest source mtime
  confirmed before every run). **Zero CLoop frames in any round** (CLoop is
  not compiled in; the standing ruling required no suppression entries in
  any of rounds r0-r10).
- Accepted tradeoff (per the standing ruling, recorded since §0): TSAN cannot
  see races inside JIT-generated code; that coverage belongs to the
  object-model protocol tests and the race amplifier. TSAN's job here is the
  C++ side — runtime slow paths, GC, caches, profiling, code lifecycle — and
  that is where every family found actually lived.
- Run config: GIL-off env (`JSC_useThreadGIL=false JSC_useVMLite=true
  JSC_useSharedAtomStringTable=true JSC_useSharedGCHeap=true
  JSC_useThreadGILOffUnsafe=true`), `--useJSThreads=1`, full corpus
  (224 files: 218 ran, 6 `//@ skip`), `halt_on_error=0`,
  `suppressions=Tools/tsan/suppressions.txt`.

## Headline

| round | unsuppressed reports |
|---|---|
| r0 (campaign start) | 10515 |
| r5 (end of wave 5)  | 763 |
| r6 (wave 6)         | 246 |
| r7 (wave 7)         | 182 |
| r8 (wave 8 + recycled-memory adjudication) | 74 |
| r9 (wave 9)         | 57 |
| r10 (thread-tsan final gate) | 55 |
| r11 (§4.4 epoch audit landed) | 32 |
| r12 (closeout mop-up wave 1) | 16 |
| r13 (mop-up wave 2) | 5 |
| r14 (mop-up wave 3) | 1 |
| r15 (mop-up wave 4) | 2 |
| r16 (first zero snapshot) | 0 |
| r17 (closeout final gate) | 0 |
| r18-r19 (post-closeout review fixes) | 5 / 2 |
| r20-r21 (post-amendment zeros) | 0 / 0 |
| r23 (r22+ re-baseline, corpus 252) | 3692 |
| r24-r26 (waves r24-r26) | 66 / 32 / 40 |
| **r27 (final-gate snapshot)** | **0** |

r10 closed the thread-tsan campaign at 55 with ~60% of the residual being
ONE family awaiting the JIT §4.4 retired-artifact epoch audit (real-bug
suspect, deliberately NOT suppressed). The audit has now been done
(TSAN-TRIAGE §17): verdict (a) — consume-style publication TSAN cannot
model, on memory that flag-on is never freed (every dealloc path of a
published PIC/handler/stub/CallLinkInfo routes through RetiredJITArtifacts,
which under useJSThreads takes the chartered leak arm). Fixed with
TSAN-only happens-before annotation pairs at the publication choke points
(§13.4 ConcatKeyAtomStringCache precedent; no suppression entries,
production codegen unchanged). r11 verification: ZERO family signatures
(ICSlowPathCallFrameTracer / considerRepatchingCacheImpl /
`VectorBuffer<StructureID>` / InlineCacheHandlerWithJSCall /
setMonomorphicCallee / JITData-BaselineJITData-block pairs) across the
full corpus; ic-publish + int-gate + calllink tests 20x each, 160/160
passes, with the only flicker being residual-2 ctor singles
(CodeBlock::vm-on-recycled-MarkedBlock, HeapCell::vm — pre-existing,
different mechanism).

## Families FIXED in code (waves 6-9, by ruling type)

Full file-level detail in TSAN-TRIAGE.md §15. Summary by ruling:

**relaxed-atomic (SPEC-blessed racy words; plain access was the UB):**
block-directory-bits (FastBitVector/BlockDirectoryBits, 149 reports — the
largest code family), barrier-counter (Heap::m_barriersExecuted, 61),
iterator-metadata + enumerator-metadata profiling bytes (value-profile f1 +
structure-fields enumerator cluster), reoptimizationRetryCounter + DFG
tier-up trigger byte (exec-counter f2), GC visit counters
(AbstractSlotVisitor — the suppressions-file B5 family, fixed in code, NOT
suppressed), ObjectAllocationProfile, RegExp compile state, concurrent
double lanes in the indexed accessors (jsvalue-slots f10), MegamorphicCache
epoch (deliberately a real RMW: a lost bump is a missed invalidation),
PutByStatus LLInt metadata loads, Thread m_didExit/m_isJSThread (pulled out
of the packed bit-field — the long-standing "header-side pass" item),
ScriptExecutable parse-feature fields (dedicated bytes), WTF::CagedPtr
storage word, ArrayBufferContents::m_sizeInBytes, FreeCell link word,
WTF::ConcurrentVector/ConcurrentBuffer internals, BlockDirectory link words,
PropertyTable ctor header stores, StructureTransitionTable word (single
relaxed snapshot in the hot lookups — fewer loads than upstream's plain
code), StructureChain lanes + readers.

**concurrent-accessor / publication (release-publish + acquire/annotated
consume):** rope resolution (fiberConcurrently TSAN-gated acquire +
equal/view via getValueImpl), CompressedLazyValueProfileHolder,
UnlinkedCodeBlock liveness, OpCatch profile buffer, transition-table
setMap, LazyProperty, OSR-entry trigger byte, MicrotaskCall + CachedCall
entry/codeBlock publication (write codeBlock, then RELEASE-publish entry;
reader acquires entry first — closes the new-entry/stale-codeBlock window),
ConcatKeyAtomStringCache (TSAN_ANNOTATE pair — the pointer is baked into
JIT code; the real edge is code installation, which TSAN cannot model).

**lock (unserialized state that the spec does NOT bless):**
- `FunctionExecutable::ensureRareDataSlow` — **REAL BUG FOUND AND FIXED**:
  creation was completely unserialized; two Threads could both create
  RareData and the second unique_ptr assignment deleted the loser's object
  while its caller was still writing into it (use-after-free shape observed
  live in r6 as two-writer reports). Now cellLock()-serialized with
  release/acquire publication.
- `VM::updateSoftReservedZoneSize` (lock around the read-modify-write),
  `VM::ensureTerminationException` (create-once lock — pointer identity is
  semantic).
- `DeferredCompilationCallback` -> ThreadSafeRefCounted (refcount corruption
  shape: ref/deref from mutator plan-completion and compiler-thread
  teardown).

**TSAN-gated paths (recorded per the §13.4 precedent; production codegen
unchanged):** zeroFill/copyDataLanesRacy byte lanes (ArrayBuffer),
butterflyConcurrentCopyWords/ZeroWords bulk copies, UnlinkedMetadataTable
link zeroing, fiberConcurrently acquire.

## Suppressed (every entry justified in Tools/tsan/suppressions.txt)

1. **Six pre-existing upstream parallel-GC entries** (unchanged since wave
   1; rule-1 adjudicated, justifications at each entry).
2. **The wave-7 recycled-allocation adjudication block**: the
   "atomic probe vs allocator re-hand-out" class — 152 of 182 r7 reports.
   Shape: the racing access is an ATOMIC relaxed load through a dedicated
   concurrent accessor (cellHeaderConcurrentLoad, butterflyConcurrentLoad,
   icConcurrentRelaxedLoad, fiberConcurrently, WatchpointSet::state,
   RacyArrayBufferViewField, CagedPtr, decodeConcurrent, ...); the "plain"
   side is the TSAN malloc interceptor's SYNTHETIC whole-block write at
   re-allocation of the same address. Design-blessed (OM ground-truth
   staleness tolerance; probed values are always revalidated), and there is
   NO plain access of ours left to convert — no code change can silence it.
   The full eligibility rule, anchor-by-anchor rationale, and the accepted
   masking tradeoff are recorded in the suppressions file. PLAIN-access-vs-
   allocator pairings are deliberately NOT covered.
3. **No CLoop entries** — zero CLoop frames ever appeared (not compiled in).

## ZERO (r16+r17, closeout mop-up — residuals 2 and 3 CLOSED)

The thread-closeout ITEM 2 mop-up (rounds r12-r17, full record in
TSAN-TRIAGE.md §18) drove the residual 32 to **0 unsuppressed reports** in
TWO consecutive full-corpus snapshots (r16, r17; 218 ran each; r17 also the
best run health of the campaign: 186 exit-0). Between r16 and r17 a
sub-1/20 repeat-stress flicker pass closed the last two members of the
recycled-cell class (CodeBlock ctor <-> vm() annotation pair — also covers
the m_couldBeTainted bit-field pairing — and the
race:JSC::WriteBarrierBase wave-7 anchor, the cell-pointer analog of the
existing decodeConcurrent anchor); the §17-family targeted set plus
calllink-writer-writer then ran 20/20 clean under repeat stress.
Highlights:

- **Two REAL locking gaps found and fixed**:
  `FunctionExecutable::ensurePolyProtoWatchpoint` (unserialized create-once,
  same shape as the wave-6 ensureRareDataSlow bug — now cellLock-serialized
  with acquire/release word publication) and **IntlCache** (per-VM cache
  shared by every Thread; HashMap find-vs-rehash UAF shape + shared ICU
  pattern generator — now fully locked). The residual-3 microtask/CachedCall
  suspicion was also CONFIRMED and fixed (unlocked isOnList() pre-check in
  MicrotaskCall::relink; CachedCall ctor linked the node before its proto
  frame was initialized).
- Last plain writers converted to the existing relaxed accessors
  (WriteBarrierBase::setEarlyValue, WriteBarrierStructureID
  value()/setEarlyValue, JSDataView m_buffer, NativeExecutable arity-check
  mirrors via concurrentCodePtrStore, handler.nativeCode retarget pair,
  classifyConcurrentLockedAdd spine count, MarkedBlock isLive/isNewlyAllocated
  bit reads, the STW segmented-growth copy lambda, the missed OpCatch
  m_buffer reader).
- TSAN-only ordering gates per the §13.4 precedent (production protocol and
  codegen unchanged): BlockDirectory link release/acquire, JSString ctor
  fiber release, StringImpl::deref acq_rel (TSAN cannot model the
  production acquire fence), ConcurrentVector/ConcurrentBuffer consume-side
  acquire.
- TSAN_ANNOTATE_HAPPENS_BEFORE/AFTER pairs (§17 precedent, lifetime
  rationale at every site): WatchpointSet ctor <-> state()/inferredValue(),
  MarkedBlock::Header ctor <-> MarkedBlock::vm() (the "HeapCell::vm /
  CodeBlock::vm recycled-block" class), JSPropertyNameEnumerator
  finishCreation <-> concurrent readers, JSDataView ctor <->
  possiblySharedBuffer.
- Suppressions: five wave-7-class anchors (CallLinkInfo::owner,
  IndexingHeader::vectorLength, StructureChain::head,
  typedArrayViewProtoGetterFuncLength, all atomic-probe-vs-allocator) and
  one rule-1 entry (UnlinkedFunctionExecutable::recordParse — upstream
  concurrent-compiler pairing, deterministic re-parse values, size-capped
  bit-fields documented at the site).

The r16 non-race tails (exit-3 / exit-134 / exit-124 / two DEADLYSIGNAL
SEGVs) are the pre-existing functional set owned by closeout items 3/4 —
unchanged since r10/r11, zero data-race lines.

## Residuals (r11, 32 reports — CLOSED by the mop-up above; kept for history)

1. **ic-stubinfo retired-artifact family — CLOSED (was ~33 of r10's 55).**
   The §4.4 audit (TSAN-TRIAGE §17) refuted explanation (b): re-examination
   of every r10 report in the family showed the "malloc previous-write" is
   the allocation of the SAME still-live block (not a re-hand-out), and the
   dealloc-path enumeration (§17.2, 15 paths) found no flag-on free of a
   published artifact that bypasses RetiredJITArtifacts — which flag-on
   leaks (epochCoversEveryJSThread), i.e. quarantine stronger than the
   epoch contract requires. Explanation (a) held: the release edge runs
   storeStoreFence -> publishing store -> in-JIT consume, invisible to
   TSAN. Closed with TSAN_ANNOTATE_HAPPENS_BEFORE/AFTER pairs at the four
   publication choke points (publishHandlerChainHead, DFG/FTL JITFinalizer,
   setupWithUnlinkedBaselineCode) and the two slow-path consumers
   (ICSlowPathCallFrameTracer ctor, CallLinkInfo::ownerForSlowPath), each
   carrying the lifetime proof in its comment. r11: 0 family reports;
   targeted tests 160/160. NOTE for the epoch-integration milestone: the
   §17.2 table is the proof-of-liveness ledger — when
   epochCoversEveryJSThread starts returning true flag-on (real epoch
   reclamation), the annotations stay sound ONLY while every row keeps
   routing through RetiredJITArtifacts; re-audit rows 1-15 at that change.
2. **Cross-lifetime constructor singles (~14 of the 32)**: plain ctor writes on
   GC/allocator-recycled addresses pairing with stale atomic probes where
   the field cannot be cheaply atomicized (CodeBlock `VM* const m_vm` /
   bit-field `m_couldBeTainted`, JSDataView `m_buffer`, CatchInfo vs
   setupWithUnlinkedBaselineCode, JSPropertyNameEnumerator finishCreation,
   UnlinkedFunctionExecutable recordParse — bit-fields kept because the
   class is size-capped at 96 bytes, documented at the site). r11 adds
   run-to-run flicker members of the same class: BlockDirectory ctor vs
   findEmptyBlockToSteal, InferredValue inflateSlow vs compiler-thread
   clobberize reads.
3. **Tail singles (~18 incl. flicker)**: Register pairs (interpreter stack
   slots), Box<InlineWatchpointSet>, String::isNull / StringView /
   rope-atom bits under i03-as-shift-unshift, StructureChain::head readers
   (forin-enumerator-cache), IndexingHeader::arrayBuffer, JSDataView ctor
   pair, tryGrowSegmentedVectorLength vs trySetIndexQuicklyConcurrent,
   MicrotaskCall::relink vs removeOnDestruction/linkIncomingCall (CachedCall
   CLI on a sibling's stack — possible real locking gap, smallest repro
   vmstate/microtask-ordering.js, queued first for the mop-up). Each 1-2
   reports per run; set membership flickers between rounds (r10 showed 10,
   r11 shows this superset shape with others submerged).

## Normal-build sanity (gate 2)

- **Debug GIL-off smoke 10x: 10/10 pass** (final binary).
- **Debug GIL-off full corpus**: 179/218 pass. The tail is decomposed in
  TSAN-TRIAGE §15 ("Normal-build sanity notes"): bench/* harness artifacts,
  the pre-existing exit-3 set (already failing in the r5 baseline), the
  KNOWN-FAILING list, a Debug-only TOCTOU-assert tail in cve/mc-* race tests
  (ASSERTs that re-evaluate racy predicates; invisible to the NDEBUG TSAN
  binary in every round of this campaign), and two GIL-era unguarded-count
  tests (create-basics `counters++`, push-resize-multithread's UNGUARDED
  arm) that encode "atomic under the GIL" expectations — they pass GIL-ON
  3/3 and their locked arms pass GIL-OFF; queued for the test-semantics fix
  round.
- **Release bench gate** (`bench-gate.sh`, 9-run medians, loadavg < 1.5):
  7/8 benchmarks within 1% (best: -0.8%). **transition-heavy-constructor:
  +3.9% vs the pre-threads baseline — FAIL vs the 1% threshold. The gate is
  RED.** Context: this bench carried the PARKED V5b residual (~+3.1%,
  quiet-host-stable, parked per Jarred before this campaign — see
  thread-bughunter charter). This campaign's attributable share is therefore
  <=~0.8pp; targeted bisection (plain-load reverts of the
  StructureTransitionTable accessors, PropertyTable ctors, and GC visit
  counters, each rebuilt and re-measured) found NO single >1% contributor —
  all candidates were within run noise (+-1.5ms on a 57ms median). The STT
  hot lookups were nonetheless rewritten to a single relaxed snapshot (one
  load where upstream's plain code did two).
  METHODOLOGY CAVEAT (closeout final review): one-at-a-time reverts cannot
  attribute the ~0.8pp delta — several sub-noise contributions from this
  campaign's UNCONDITIONAL flag-off conversions (STT relaxed snapshot,
  PropertyTable ctor stores, WriteBarrierBase setEarlyValue /
  WriteBarrierStructureID, GC visit counters, AND — added at the
  post-closeout review, previously omitted from this list — the
  `lastArraySize` std::atomic conversion in `getNewVectorLength`
  (JSObject.cpp:67/6398/6405), which sits on the array-growth path of
  exactly this benchmark family and means the item-4 closeout work was NOT
  flag-off byte-identical — all on this bench's hot path) can compound
  while each staying inside +-1.5ms. The earlier wording
  ("attribution stays parked with V5b") overstated the exoneration. REQUIRED
  before the gate verdict can transfer to the parked V5b item: ONE combined
  revert (all unconditional flag-off mop-up conversions reverted together)
  on the quiet host against transition-heavy-constructor. If it recovers to
  ~+3.1%, this campaign owns the delta and the cheapest contributors must be
  flag-gated; if it stays at +3.9%, record that and the verdict transfers.
  Until that run lands the milestone ships with a RED flag-off gate and an
  UNPROVEN attribution, recorded as such.

## What would get this to zero — ALL DONE (r16 = 0)

1. ~~The JIT §4.4 retired-artifact epoch audit (residual 1)~~ DONE
   (TSAN-TRIAGE §17; r11 55 -> 32, family at 0).
2. ~~A small ctor-atomicization pass for residual 2~~ DONE (TSAN-TRIAGE §18;
   r12-r16).
3. ~~One mop-up wave for the tail singles~~ DONE (four waves, §18; two real
   locking gaps found en route — ensurePolyProtoWatchpoint and IntlCache).

## Closeout final-review amendments (2026-06-09, post-r17)

The pre-milestone adversarial review found two functional flag-on bugs and
three TSAN-evidence-integrity defects. All five are fixed in code/config;
consequences for the headline:

1. **§17.2 audit verdict corrected (real bug fixed)**: the dealloc-path
   table omitted the MetadataTable teardown row — flag-on, `~CodeBlock`'s
   `m_metadata` release freed published DataOnlyCallLinkInfos, their §5.8
   records, and the metadata block the baseline prologue reloads, on the
   same straggler window the AB18-B jitData leak exists for. Fixed:
   flag-on ref-escape of `m_metadata` in `~CodeBlock` (CodeBlock.cpp);
   TSAN-TRIAGE §17.2 rows 13/16 + verdict amended.
2. **Jettison-time watchpoint disarm (real bug fixed)**:
   `RetiredJITArtifacts::retireHandlerChain` unconditionally disarmed
   clearing watchpoints, including for chains deliberately LEFT INSTALLED by
   the jettison-time `PropertyInlineCache::deref(VM&)` — a post-jettison
   watched-set fire would skip the IC reset and straggler baseline frames
   (no invalidation points) would dispatch stale handlers (silent wrong
   values). Fixed: `DisarmClearingWatchpoints {No, Yes}` parameter; jettison
   passes No; displacement sites, the world-stopped `resetStubAsJumpInAccess`
   reset republish, and `~CodeBlock` pass Yes. (CORRECTION, post-closeout
   review: this sentence previously said "jettison and reset pass No" — the
   code never did that, and must not: reset DISPLACES the chain and
   republishes a slow-path handler, so disarming matches flag-off inline
   destruction. Sites: CodeBlock.cpp:2687 / PropertyInlineCache.cpp:547 = No;
   PropertyInlineCache.cpp:432/1216-1217/1328-1329 + CodeBlock.cpp:1062 = Yes.
   "Correcting" the reset site to No per the old wording would leave armed
   PropertyInlineCacheClearingWatchpoints on displaced chains whose owner
   CodeBlock later dies — a fire-time UAF, the AB18-F class.)
3. **Universal vm() HAPPENS_AFTER narrowed**: the §18 annotation pair lived
   on `MarkedBlock::vm()` / `CodeBlock::vm()` — executed on every C++ slow
   path on every thread — continuously re-synchronizing every mutator with
   every allocating/constructing thread and hiding an unbounded class of
   genuine races from TSAN. Moved to dedicated probe accessors
   (`vmConcurrentProbe()`) used only at the observed residual-2 sites
   (slow_path_enter; the ownerForSlowPath call operations).
4. **Suppression narrowing**: `race:JSC::WriteBarrierBase` (class-wide,
   masked slot()/raw-copy plain-writer regressions engine-wide) ->
   `race:JSC::WriteBarrierBase<*>::cell`; the function-level
   `race:JSC::typedArrayViewProtoGetterFuncLength` entry is DELETED — the
   RacyArrayBufferViewField probe is now NEVER_INLINE under TSAN so the
   existing narrow anchor matches.
5. **Bench-gate verdict corrected** (see the amended Release-bench-gate
   bullet above): RED, attribution unproven pending the combined revert.

**Consequence: the r16/r17 "0 unsuppressed" snapshots are STALE.** Items 3
and 4 change TSAN visibility in the report-reducing-to-report-increasing
direction (annotations narrowed, suppressions narrowed), and items 1-2
change flag-on teardown behavior. A FULL re-baseline snapshot (r18: full
corpus, same pinned config, plus the §17-family + repeat-stress flicker
set) is REQUIRED before the milestone commit may cite a zero claim. Expected
honest outcome: residual-2-class flickers may resurface at vm() sites other
than the two re-annotated ones; triage each new site individually (add a
per-site probe accessor with a lifetime rationale, or fix) rather than
re-widening the annotation.

## Post-closeout review amendments (2026-06-09, second pass)

A second adversarial pass over the closeout state found three more defects;
all are fixed in code/config and ALL widen the r18 obligation:

1. **DFG `~CodeBlock` arm nulled `m_jitData` flag-on (real bug fixed)**:
   the field-null defeated the straggler rationale of the row-7 leak (fresh
   DFG entry/OSR-exit dispatch reloads `offsetOfJITData` from the cell and
   would near-null-store8/farJump). Fixed: flag-on the field stays intact,
   mirroring AB18-B (TSAN-TRIAGE §17.2 row 7 CORRECTION).
2. **Dead-CodeBlock recycled-cell hazard closed (real bug fixed, §17.2
   row 17)**: dead blocks stayed CallLinkInfo-linked until the post-resume
   lazy sweep, so a sibling could acquire the dead cell pointer AFTER the
   conservative scan (unpinned) and race sweep + IsoSubspace re-hand-out
   into another CodeBlock's `m_jitData`/`m_metadata` (silent
   wrong-metadata). Fixed: flag-on End-phase (world-stopped) incoming-call
   unlink in `CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks`.
   This also reconciles the residual-2 "recycled under live prober"
   adjudication with the AB18-B field-keeping premise (see row 17).
3. **Suppression narrowing #2**: `race:JSC::CallLinkInfo::owner` was a
   substring template that also matched every `ownerForSlowPath` frame
   (which carries the §17 HAPPENS_AFTER and the cross-frame plain
   `codeOwnerCell()` read — residual-3 neighborhood). Narrowed to
   `race:JSC::CallLinkInfo::owner()`.
4. **Property-waiter lost-waiter race (real bug found via the re-gate and
   fixed)**: re-running the functional gates on the post-amendment tree
   (the obligation above) surfaced `atomics/property-wtr-isolation.js`
   failing ~8% GIL-off ("expected 1 but got 0" wakes). Root cause, proven
   by instrumentation (register of (cell,"1") immediately followed by
   removeListIfEmpty of the same key; notify-side table empty while the
   waiter stayed parked): `atomicsWaitOnProperty`/`atomicsWaitAsyncOnProperty`
   enqueued the waiter AFTER `PropertyWaiterTable::findOrCreateList`
   released the table lock, so a concurrent notify could dequeue-to-empty
   and `removeListIfEmpty` the just-created entry, orphaning the list the
   waiter then parked on — a permanently lost sync waiter (or async
   ticket). Fixed in ThreadAtomics.cpp: `findOrCreateList` now enqueues the
   waiter before releasing the table lock (list lock nested inside table
   lock, the removeListIfEmpty order). 100/100 clean post-fix (was ~8%).
   The test also gained a GIL-off-correct `notifyOne` spin for its expect-1
   asserts: the parkWaiterOn ready/park handshake only proves the waiter
   parked under the cooperative GIL, so a single right-target notify
   legally returns 0 GIL-off; without the spin the engine bug had
   masqueraded as that benign timing race.

### r18 re-baseline — RUN (2026-06-09, post-amendment tree + fixes 1-4 above)

Full corpus (218 tests, now including the integrated cve/, bench/, arrays/
suites), pinned config (WebKitBuild/TSan JIT+asm jsc, GIL-off env,
halt_on_error=0, suppressions.txt). Result: **5 unsuppressed reports in 2
tests** (artifacts: Tools/threads/tsan/r18/) — the honest outcome the
amendment predicted (narrowed annotations/suppressions resurface real
visibility). Per-site triage, all four sites resolved in code/config:

1. `semantics/ic-in_by_id-vs-transition.js` (1 report): PLAIN read of
   `ButterflySpine::outOfLineFragmentCount` in
   `growOutOfLineStorageForConcurrentLockedAdd`'s segmented-arm
   RELEASE_ASSERT (JSObjectInlines.h) racing a sibling's
   `ensureSegmentedOutOfLineCapacity` butterflyConcurrentStore. The racy
   VALUE was already argued safe (monotone fragment coverage); the access
   was the lone unpaired plain reader. FIXED: now uses
   `outOfLineFragmentCountConcurrent()`.
2. `cve/mc-grow-buffer-storm.js` report 4: bitfield-byte RMW —
   `PolymorphicAccessJITStubRoutine::addedToSharedJITStubSet()` runs on a
   JIT worklist thread post-publication and RMWs the byte that
   `isStillValid()` reads on sibling compiler threads and that GC-side
   sweeps write (`m_mayBeExecuting`/`m_isJettisoned`/`m_ownerIsDead`) —
   compiler threads do not stop for GC, so this was a real byte-level
   lost-update hazard. FIXED: `m_isInSharedJITStubSet` and `m_ownerIsDead`
   moved out of the bitfield to whole `std::atomic<bool>` members
   (GCAwareJITStubRoutine.h).
3. `cve/mc-grow-buffer-storm.js` reports 1-3: stale wasteful-view OOB
   probe (`isArrayBufferViewOutOfBounds` -> `possiblySharedBufferImpl`)
   vs allocator (re-)hand-out — wave-7 dead-cell probe class. FIXED +
   ANCHORED: `IndexingHeader::arrayBuffer()/setArrayBuffer()` converted to
   relaxed atomics (same discipline as the sibling
   vectorLength/publicLength words), and a narrow
   `race:JSC::isArrayBufferViewOutOfBounds` anchor added with the wave-7
   rationale (the racing pair is the malloc/aligned_alloc interceptor's
   synthetic whole-block write; getter bodies stay visible).

### r19 (post r18-triage fixes) — 2 reports, one NEW site; r20 — ZERO

r19 (full corpus, same pinned config, after the three r18 fixes): the five
r18 reports are GONE; **2 new reports** in
`cve/mc-reent-store-missing-indexed-define-race.js`, both the §18 ctor
class at a new site: `SparseArrayValueMap::size()`/`add()` — both already
cellLock()-serialized — pairing with the map CONSTRUCTOR's m_map header
NSDMI stores on the constructing thread (the install into the array
storage is fence + plain store; the cell lock serializes post-publication
accessors against each other but gives TSAN no edge back to the ctor).
Treated per the amendment's per-site rule with a dedicated annotation
pair: `TSAN_ANNOTATE_HAPPENS_BEFORE(this)` at the end of the
SparseArrayValueMap constructor + `tsanAcquireCtorPublication()`
(HAPPENS_AFTER) immediately after every cellLock() acquisition in the
class — narrow by construction (sparse-map cold paths only; does not
touch hot engine-wide accessors, per the vm() narrowing lesson).

**r20 (full corpus, pinned config, post all fixes): 218 tests, 0
unsuppressed reports.** One load-dependent 420s timeout
(cve/mc-gc-thread-shell-finalizer-storm.js, 0 reports before the kill;
passes standalone under Release in <1s; the same storm-family timeouts
appear in r17's record) — pre-existing TSan-slowness variance, not a
hang regression. Artifacts: Tools/threads/tsan/r19/, r20/.

**r21 (confirmation, same pinned config): 218 tests, 0 unsuppressed
reports** (same single storm-test timeout at 0 reports). The milestone may
cite **TWO consecutive zero snapshots on the final tree: r20 + r21**
(satisfying the r16/r17-era convention). r16/r17 are STALE, r18 carried 5
pre-fix reports, r19 carried 2. Artifacts: Tools/threads/tsan/r18..r21/.

**Standing obligations before the milestone commit may cite its gates**:
- **r20 + r21 are the citable zero snapshots** (two consecutive zeros on
  the final tree; r16/r17 STALE; r18 = 5 pre-fix; r19 = 2 — see above).
- **Functional-gate re-run on the FINAL tree (all fixes in)**: DONE —
  GIL-off corpus 93/0 (3 skips), GIL-on 94/0 (2 skips), identity
  mismatches=0 (40 tests), amplified havebadtime-vs-indexed-fastpath
  50/50 (seeds 3001-3050), amplified proto-cycle-race 50/50 (seeds
  4001-4050), property-wtr-isolation 30/30 (and 100/100 on the
  pre-r18-triage build) GIL-off post lost-waiter fix (was ~8% fail/hang).
- **Combined-revert bench attribution** (now INCLUDING `lastArraySize`,
  see the amended candidate list above) — still NOT run; the flag-off gate
  remains RED with unproven attribution and the milestone record must say
  so.
- **§4.4 audit verdict**: any milestone text must carry the AMENDED verdict
  (falsified-then-fixed; (b) refuted only for the current tree) and the
  epochCoversEveryJSThread re-audit trigger — never "verdict (a), proven".
- **Post-r21 row-18 fix (final closure review)**: TSAN-TRIAGE §17.2 row 18
  landed AFTER r21 — `~CodeBlock`'s baseline arm inline-freed the published
  OpCatch ValueProfileAndVirtualRegisterBuffer flag-on; now flag-on-gated
  (leaked with the row-16 ref-escaped metadata), flag-off byte-identical.
  The change strictly REMOVES a flag-on free (no new code runs; TSAN
  exposure cannot increase), so r20+r21 remain the citable zeros for the
  race surface; a confirmation snapshot (r22) on this tree is the cheap
  belt-and-suspenders if the milestone wants snapshot-tree identity. The
  §17.2 table is now 18 rows; the re-audit trigger covers all 18.

---

## r23-r27 campaign (2026-06-19, expanded 252-file corpus)

The r22+ re-baseline (TSAN-TRIAGE §20) ran the EXPANDED corpus (252 files,
up from 218: corpus2 integration added arrays/, gc-stress/, scaling/,
semantics/, lifecycle/, congc-*, dw*-*, cve/mc-* tests) on the
post-row-18 tree and surfaced **3692 reports** in 10 families — entirely
NEW surface introduced by post-r22 work (SPEC-congc landing, SCALEBENCH
bimodal DCLP, ConcurrentPtrHashSet under N-mutator pressure); every
closeout-era family stayed at 0. Waves r24-r26 (TSAN-TRIAGE §§21-23)
surfaced 5 more families as the first 9 were closed; 13 of the 15 distinct
families were closed IN CODE (3652 reports), the remaining 2 adjudicated
suppress-only (40 reports) and landed in r27.

### r27 final gate (this run)

**Gate 1 — TSAN: 0 unsuppressed reports.** Full corpus (246 ran, 6
//@ skip), pinned config (`WebKitBuild/TSan/bin/jsc`, full JIT + asm LLInt,
`ENABLE_C_LOOP=0`, GIL-off env, `halt_on_error=0`,
`suppressions=Tools/tsan/suppressions.txt`); binary mtime > newest source
verified; smoke 3/3 PASS pre-run. `grep -c 'WARNING: ThreadSanitizer'
reports-r27.log` = **0**. `grep -ci CLoop reports-r27.log` = **0** (CLoop
not compiled in; the standing ruling required no entries). Exit-code
spectrum: 229 exit-0 (best of campaign), 14 exit-3 (bench harness +
documented functional set), 2 exit-66 (cve/mc-grow-buffer-storm +
ic-instanceof-vs-transition DEADLYSIGNAL in JIT — §0 accepted-tradeoff,
0 reports each), 1 exit-124 (w16-c1-prevent-collection TSan-slowness
timeout, 0 reports). The four KNOWN-FAILING tests all ran exit-0 with 0
reports this round.

**Suppressions audit: 40 active entries, 0 unjustified.** Mechanical check
(every `race:` line covered by a `#` comment block): PASS. By class:
- 6 pre-existing upstream parallel-GC entries (rule-1).
- 23 wave-7 "atomic probe vs allocator (re-)hand-out" reader-side anchors.
- 1 rule-1 `recordParse` (upstream concurrent-compiler, size-capped).
- **9 r23-r26 "JIT-one-sider allocator hand-out" allocator-side anchors**
  (NEW this run; TSAN-TRIAGE §20.3.10/§23.4.1) — the §0 accepted-tradeoff
  residual: TSAN cannot symbolize the JIT-emitted access, so reports carry
  only the C++-side allocator/ctor write; the real HB is the cell-handout
  protocol (LocalAllocator -> JS-value store -> in-JIT consume).
- **1 SPEC-congc T5-rootscan stale-window entry** (NEW this run;
  TSAN-TRIAGE §23.4.2) — GC conductor reads a snapshot struct on a parked
  thread's stack; in the documented stale window the function bounds-checks
  and bails; cannot be atomicized (the "writer" is arbitrary stack content).
- 0 CLoop entries; 0 `[CONTROL: PENDING]` entries active.

**Gate 2 — normal-build sanity:**
- Debug GIL-off smoke 10x: **10/10 PASS**.
- Debug GIL-off full corpus (`run-tests.sh`): **96 passed, 0 failed,
  3 skipped** — GREEN.
- Release bench-gate (`bench-gate.sh`, 9-run medians, loadavg-1m 1.91 at
  start): **7/8 within 1%; transition-heavy-constructor +5.71% — FAIL.**
  Re-measured at loadavg 1.76 (9 runs, median 57.87 ms = +5.38%): load-
  stable, not noise. **The flag-off gate is RED.**

### Families fixed in code, r23-r26 (by ruling type)

Full per-family record: TSAN-TRIAGE §§20-23. **13 of 15 families closed by
code change**; 2 by adjudicated suppression.

**relaxed-atomic** (SPEC-blessed racy words; plain access was the UB):
`tlc-slot-stamp` (heap/IsoSubspace.h LowerTierPrecise per-subspace slot
index — 2691 reports, the largest r23 family), `marks-bitset-plain-readers`
(WTF::BitSet word loads + MarkedBlock readers — 828),
`dependency-loadandfence` (WTF::Atomics consume-style fenced loads — 76),
`parked-root-snapshot` (Heap m_parkedRootSnapshotThread word — 37),
`osr-exit-coderef` (DFGJITCode publish-via-one-pointer — 25),
`concurrentptrhashset-publish` (7), `codeblock-jitdata-publish` (4),
`typedarray-sort-memcpy` (JSGenericTypedArrayViewInlines lanes — 2),
`regexp-cached-result-visit` (RegExpCachedResult fields — 1),
`globalobject-init-vs-visitor` (JSGlobalObject::init — 5).

**lock / REAL BUG (spec does NOT bless; 3 found and fixed):**
- `dfg-weakrefs-reallyadd` (DFGDesiredWeakReferences.cpp:99-100): unfenced
  FixedVector move-assign of `m_weakStructureReferences` /
  `m_weakReferences` raced concurrent-GC SlotVisitor iteration; the
  upstream "GC is deferred" comment is a single-mutator guarantee that
  SPEC-congc N-mutators voids. Fixed: `ConcurrentJSLocker(m_lock)` around
  the two move-assigns.
- `growarrayright-vs-sparsemap-atomics` (ThreadAtomics.cpp): two mutators
  reshaping the same butterfly via the Atomics-on-property path; OM §4
  reshape protocol does not bless this. Fixed in ThreadAtomics.cpp.
- `cachedcall-protoframe-crossthread` (CachedCall.h /
  InterpreterInlines.h / CodeBlock.cpp): installCode mutated a foreign
  thread's CachedCall protoframe. Fixed.

**concurrent-accessor / publication:** the BitSet `concurrentFilter`
residual (r24 §21.4.1), CachedCall entry acquire re-read.

### Suppressed (r27 additions, every entry justified)

Two adjudication blocks appended to `Tools/tsan/suppressions.txt` (full
rationale at each entry):
1. **JIT-one-sider allocator hand-out** (9 anchors, 38 r26 reports):
   `MarkedBlock::tryCreate`, `MarkedBlock::Header::Header`,
   `PreciseAllocation::tryCreate`, `Butterfly::growArrayRight`,
   `JSRopeString::CompactFibers`, `GetByIdModeMetadata::GetByIdModeMetadata`,
   `ArrayBuffer::tryCreate`, `ArrayBufferContents::tryAllocate`,
   `JSArrayBuffer::JSArrayBuffer`. Wave-7 class, allocator side; the
   "racing" access is JIT-emitted (`<null>` frame), so there is no plain
   C++ access of ours to convert; coverage owned by the protocol tests /
   amplifier per the §0 accepted tradeoff.
2. **parked-stack stale-snapshot read** (1 anchor, 2 r26 reports):
   `MachineThreads::tryCopyCooperativelyParkedThreadStack`. SPEC-congc
   T5-rootscan; in-source comment at MachineStackMarker.cpp:255-266
   documents the bounds-check-and-bail protocol.

### Residuals (honest record)

**TSAN race surface: ZERO.** r27 is the citable zero snapshot on the
final r23-r27 tree (corpus 252, full JIT). r20/r21 remain the citable
zeros for the pre-expansion 218-file corpus.

**Flag-off bench gate: RED, attribution UNPROVEN.**
transition-heavy-constructor +5.71% (was +3.9% at the closeout record,
~+3.1% parked V5b pre-campaign). The ~+1.7pp delta vs closeout aligns with
the r24-r26 wave's UNCONDITIONAL flag-off header conversions (BitSet.h,
Atomics.h, ConcurrentPtrHashSet.h, IsoSubspace.h, MarkedBlock.h/Inlines.h,
Heap.h, CodeBlock.h, DFGJITCode.h — all on this bench's allocation /
transition hot path). The combined-revert experiment (closeout obligation,
NOW EXTENDED to include the r24-r26 conversions) is STILL NOT RUN.
megamorphic-access -13.32% is a large unexplained improvement (do not
claim without bisection). The other 6 benches are within ±0.7%.

**Functional tails (out of TSAN scope, unchanged):** the exit-3/66/124 set
in §24.4; semantics/ic-instanceof-vs-transition DEADLYSIGNAL in JIT code
(amplifier-owned per §0); the four KNOWN-FAILING tests (separate fix
queue).
