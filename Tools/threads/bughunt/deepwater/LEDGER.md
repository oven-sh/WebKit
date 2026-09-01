# DEEPWATER LEDGER — what the limp-build dive surfaced beyond the papered sites

Dive: 2026-06-11 → 2026-06-12, worktree `/root/WebKit-limp` @ `5c0e51c2b543`
(jarred/threads), Debug+ASAN limp binary `/root/WebKit-limp/WebKitBuild/Limp/bin/jsc`
(`-fsanitize-recover=address`; guards in `LIMP.md`). 33 sequential runs at the
pinned GIL-off flags (`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
--useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1`),
W up to 64. Raw logs: `/root/WebKit-limp/dive-logs/`; per-run bookkeeping:
`dive-ledger.txt` (this dir). Campaign narrative: `DIVE.md`.

## 1. New-signature table (deduped, beyond the papered sites)

| id | kind | count | trimmed evidence | suspected mechanism | K4/N7 cross-ref | log |
|---|---|---|---|---|---|---|
| DW-1 | correctness — debug ASSERT; **silent control-flow corruption in release** | 1 / 6 leg-b runs (0/15 leg a, 0/12 leg c) | `ASSERTION FAILED: pc->opcodeID() == op_call` `LLIntSlowPaths.cpp(3167) llint_slow_path_array_sort_comparator_return`, SIGABRT @295s, benchbc-w32-r1 | DFG ArraySortIntrinsic OSR-exit comparator-return trampoline recovers `callFrame->bytecodeIndex()` and expects the sort's `op_call`; on a spawned Thread it is NOT. Candidates: per-lite vs carrier mismatch in the CallSiteIndex stashed by `reifyInlinedCallFrames`, or cross-thread CodeBlock replacement between stash and recovery. Needs the 8x query pressure (16000 queries) to tier `entries.sort(comparator)` into DFG. | K4 §I `m_checkpointSideState` ruled per-lite (VM.h:1303 row) — this path is the consumer of exactly that ruling; K4.II.8 (`m_cachedSortScratch`) is the adjacent per-lite sort state. No row covers the OSR-exit pc-recovery contract itself: **partially audited, contract not** | `dive-sortcomparator-crash.log`; full: `dive-logs/benchbc-w32-r1-a1.log` |
| DW-2 | correctness — UAF/SEGV, found during limp verification (then papered with a global Lock) | 1 hard SEGV pre-paper (W=16 full run1); 0 recurrences post-paper in 33 runs | SEGV, zero-page read on freed table (`rdx=0xf5f5...`) in `HashTable::removeIterator`/`add` on `vm.heap.markListSet()`, via `MarkedVector::fill` ← `sortImpl` ← `arrayProtoFuncSort`, spawned Thread T5 | Under `--useSharedGCHeap=1` the per-Heap `m_markListSet` is ONE shared HashSet mutated by every Thread's MarkedVector/MarkedArgumentBuffer spill path with no synchronization. Real GIL-off bug; the limp paper (`MarkedVectorBase::s_limpMarkListSetLock`, MarkedVector.{h,cpp}) is a global lock — landable fix should be per-heap lock or sharded set. | **No K4/N7 row** — Heap-resident, outside both audits' scope (K4 = VM/global members, N7 = cell-class members). Audit-coverage gap: shared-Heap C++ containers under useSharedGCHeap deserve their own sweep. | LIMP.md §guards item 6; `run1.out` |
| DW-3 | performance — Phase C bimodal wall, 17x | 9 slow / 6 fast across leg a (W=32: 5/5 slow 111-135s; W=48: 4 slow 1 fast; W=64: 5/5 fast 6.4-7.9s); checksums identical | Phase C = 128 shards drained via one atomic counter into 104 Lock-guarded group accumulators; slow mode ~17x fast mode | Park/contention pathology on the group Locks (or the counter cacheline) that engages at W=32/48 but not at W=64 on a 64-core box. Reproducible contention-hunt target; informs WTF::Lock parking behavior under GIL-off thread counts below core count. | n/a (perf, not a §K/§N state row) | `dive-ledger.txt` leg-a JSON lines |
| DW-4 | performance — anti-scaling past W=32 on lock-heavy phases | systematic across leg b + leg c | leg b: phaseB 174-176s@32 → 206-218s@64; phaseC worst @64. leg c T64/T32 (equal work/thread, flat=perfect): map-heavy 3.6x, splay 2.8x, raytrace 2.6x, richards/string 1.9x | Allocation-heavy workloads degrade most → shared-GC-heap / allocator contention prime suspect. CAVEAT: N=64 saturates the box on top of ambient load + ASAN overhead, so 2-4x is an UPPER bound conflating engine and host oversubscription. Re-measure on Release, quiet host. | n/a | `dive-ledger.txt` leg-b/c; DIVE.md table |

Not re-listed: the four papered mechanisms themselves (varargs family, libpas
dealloc-fail, STW watchdog, markListSet-as-papered) — frequency below.

## 2. LIMP-tag frequency at W=64 (and campaign-wide)

| tag | W=64 runs (8: 5 leg a + 3 leg b) | campaign total (33 runs) | verification (W=16, pre-campaign) |
|---|---|---|---|
| `LIMP: varargs-scratch-race` | 0 | 0 | 0 (incl. 30k-iter repro.js: 0) |
| `LIMP: forward-args-length-race` | 0 | 0 | 0 |
| `LIMP: varargs-constant-oob` | 0 | 0 | 0 |
| `LIMP: pas-dealloc-fail` | 0 | 0 | 0 |
| `LIMP: stw-watchdog-timeout` | 0 | 0 | 3 lines, 1/3 full runs (stop converged 90s, run completed correct) |

Implication for the fix wave: **none of the papered sites is hot at W≤64 on
this tree** — the U-T1 per-lite varargs scratch routing appears to have removed
the root varargs race (guards stayed silent as tripwires), and the libpas family
needed overload/parallel-run pressure we deliberately avoided. Landed fixes for
these do NOT need to be perf-conscious (cold paths); the markListSet fix is the
exception — it sits on the MarkedVector spill path (every big sort/apply), so
the landable version should be sharded/per-heap, not the dive's global Lock.
The watchdog firing is overload-latency, not a wedge; keep-waiting + rate-limited
dump (the limp shape) is plausibly the right landed shape too.

## 3. Priority order for the next fix wave

0. **K4.II.8 `m_cachedSortScratch` per-lite at the EMISSION sites — FIRST,
   blocks DW-1.** Owner files (outside the dw1 slice):
   `DFGSpeculativeJIT.cpp` `compileArraySortCompact/Commit` (~16224-16299,
   bare non-mode-split non-atomic load + null-store on the baked absolute
   `&vm.m_cachedSortScratch`, baked `m_sortScratchSentinel` at 16251) and
   the FTL twin (`FTLLowerDFGToB3.cpp` ~11662-11758). CONFIRMED GIL-off
   correctness bug (see row 1 disposition: 12/12 SEGV on the regression
   test, DFG-only, value corruption in pure-int32 warmup). Ordering is
   mandatory: until this lands, `dw1-sort-comparator-osr.js` stays red and
   the DW-1 repro loop cannot discriminate the wrong-pc contract.
   **ORCHESTRATOR ACTION REQUIRED: schedule a slice owning
   `DFGSpeculativeJIT.cpp` + `FTLLowerDFGToB3.cpp` to implement this
   ruling. The dw1 slice cannot touch these files; DW-1 (row 1) cannot be
   discriminated or closed — and its regression test stays expected-RED
   GIL-off — until this lands. This is a scheduling dependency, not a
   note.**
1. **DW-1 sort-comparator OSR-exit pc** — only debug-ASSERT today, but release =
   `dispatchToCurrentInstructionDuringExit` at a wrong pc = silent
   miscompilation-grade corruption. Bughunt-grade: instrument the stash side
   (reifyInlinedCallFrames CallSiteIndex) vs recovery side with thread ids +
   CodeBlock pointers; reproduce with the preserved variant
   (`dive-logs/variant/bench-bc.js`, W=32, ~1/6 hit rate).

   **DISPOSITION (2026-06-12, dw1-sort-osr-exit-pc, write-only slice — pending
   builder compile + repro loop):**
   - *Instrumentation landed, both sides, gilOff-gated.* Stash side:
     `operationCompileOSRExit` (traversed on EVERY GIL-off DFG exit since the
     U-T4b repatch suppression) records (thread uid, DFG CodeBlock, expected
     caller baseline CodeBlock, expected CallSiteIndex bits, exit index) into
     a thread-local record when the exit's inline stack carries an
     `ArraySortComparatorCall` (`DFGOSRExitCompilerCommon.{h,cpp}`:
     `sortComparatorOSRExitStashRecord` /
     `recordSortComparatorOSRExitStashIfApplicable`, hooked in
     `DFGOSRExit.cpp`). Recovery side:
     `llint_slow_path_array_sort_comparator_return` cross-checks the record
     and, GIL-off, **hard-stops** (`RELEASE_ASSERT` after a discriminating
     `dataLogLn` dump) on `pc != op_call` — release no longer silently
     dispatches to a wrong pc. The dump discriminates: CodeBlock mismatch =
     cross-thread CodeBlock replacement between stash and recovery; same
     CodeBlock / different bits = CallSiteIndex routing fault
     (per-lite-vs-carrier family); unarmed / foreign uid = carrier mixup or
     FTL-tier exit (FTL stash side NOT instrumented —
     `FTLOSRExitCompiler.cpp` is outside this slice's file ownership).
   - *K4 §I ruling implemented* (`m_checkpointSideState` per-lite): storage
     stays VM-resident (so `scanSideState` reaches every thread's tmps at a
     stopped world), but entries are owner-tagged
     (`CheckpointOSRExitSideState::owningThreadUid`) and the COMPLETE
     accessor set (`push/pop/popAllUntil/has/scan`; readers/writers:
     DFGOSRExit.cpp:310, LLIntSlowPaths.cpp pops x2, Interpreter.cpp:1004,
     VMEntryScope.cpp:424 via `hasCheckpointOSRSideState`, Heap.cpp:1110 via
     `scanSideState`) filters on the current thread under the new
     `VM::m_checkpointSideStateLock` when gilOff. The GIL-on `takeLast()` pop
     and the tail-scan `popAllUntil` WERE cross-thread-unsound GIL-off
     (foreign-entry pop; buried-entry stranding -> stale bytecodeIndex
     consumed by a later checkpoint exit). GIL-on arms run verbatim, asserts
     unweakened; the lock is GIL-off-only.
   - *Adjacent root-cause suspect found, OUTSIDE this slice's files — route
     to owner:* `DFGSpeculativeJIT::compileArraySortCompact/Commit`
     (DFGSpeculativeJIT.cpp ~16224-16299) and the FTL twin
     (FTLLowerDFGToB3.cpp ~11662-11758) emit a NON-mode-split, NON-ATOMIC
     load + null-store pair on the baked absolute `&vm.m_cachedSortScratch`
     (and bake `m_sortScratchSentinel`). GIL-off, two threads can both load
     the same cached scratch JSCellButterfly and concurrently
     insertion-sort into ONE 16-slot scratch; a committing thread
     JSEmpty-fills it under the other's feet. K4.II.8 ruled this per-lite;
     the ruling is NOT implemented at these emission sites. Same inlined-sort
     surface as DW-1; the new instrumentation will say whether it also
     explains the wrong-pc signature.
     **EXPERIMENTALLY CORROBORATED in-slice** on the pre-edit
     `WebKitBuild/Release/bin/jsc` (current-tree build predating this slice's
     edits), pinned GIL-off flags, W=8, using the new regression test:
     3/3 runs FAIL with wrong sorted VALUES (e.g. "warmup t0 r31: mismatch at
     0: 40 != 56"; 2 of 3 also SEGV after the mismatch) — and the mismatches
     occur in the PURE-INT32 WARMUP phase, i.e. with no comparator OSR exits
     at all, which is exactly the two-threads-sharing-one-scratch-butterfly
     prediction and NOT explicable by pc-recovery alone. Discriminator:
     same flags + `--useDFGJIT=0` = 3/3 PASS; GIL'd flag-on = PASS; flag-off
     = PASS. The DFG inlined-sort scratch race is therefore a CONFIRMED
     GIL-off correctness bug on this tree, independent of (and upstream of)
     the wrong-pc assert; whether it fully accounts for DW-1's signature is
     what the landed stash/recovery instrumentation will decide on the
     builder's binary.
   - *Regression test added:* `JSTests/threads/dw1-sort-comparator-osr.js`
     (arrays <= 16 slots so the inlined three-phase pipeline engages, literal
     comparator => ArraySortComparatorCall inlining, int32 warmup then
     mixed-representation flip to force comparator OSR exits, per-round
     reference-sort verification, W=8 spawned + main).
   - *RUNTIME VERIFICATION EXECUTED (2026-06-12 amender pass).* A full
     Release rebuild completed at 03:40 (post-edit; `.ninja_log` shows
     runtime-50/dfg-12/llint TUs + `bin/jsc` relinked; the DW-1 dump string
     is present in the binary), so the earlier "no binary contains these
     edits" deferral is RESOLVED for Release (Debug binary, 03:05, remains
     pre-edit). Gates run synchronously on
     `WebKitBuild/Release/bin/jsc`:
     - **bench-bc repro loop, W=32, pinned GIL-off flags: 20 runs**
       (12 plain + 8 under gdb). 1/20 SEGV (plain run 3; no DW-1 dump, no
       output beyond the wasm banner — crash predates the comparator-return
       check or is the K4.II.8 corruption family); 19/20 completed with
       bit-identical checksums. The wrong-pc RELEASE_ASSERT did NOT fire in
       any run: no recurrence of the DW-1 signature on the instrumented
       binary, but 20 Release runs vs the dive's 1/6-on-Debug+ASAN rate is
       not exhaustion — keep the loop in the next builder pass.
     - **Regression test `JSTests/threads/dw1-sort-comparator-osr.js`,
       pinned GIL-off: 0/12 pass, 12/12 SEGV**, 0 DW-1 dumps.
       Discriminators: +`--useDFGJIT=0` 3/3 PASS; GIL-on flag-on 2/2 PASS;
       flags-off 2/2 PASS. gdb backtrace of the SEGV: JS Thread faulting in
       `slow_path_typeof_is_object` on a garbage cell (sibling threads
       simultaneously faulting in `cellHeaderConcurrentLoad` at ~0x5) — pure
       downstream value corruption, NO frames in this slice's stash/recovery
       code. This is the routed-to-owner K4.II.8 scratch race (sites
       verified still unfixed: DFGSpeculativeJIT.cpp 16224/16226/16299 +
       sentinel 16251; FTLLowerDFGToB3.cpp twin; both files untouched since
       Jun 10). **ORCHESTRATOR NOTE: this test is expected RED GIL-off until
       K4.II.8 lands — a red run is NOT a DW-1 regression and NOT caused by
       this slice.** It is not in the default `run-tests.sh` corpus set
       (root-level file), so it cannot flap the corpus gate.
     - **Corpus (`Tools/threads/run-tests.sh`, default mode): 95 passed,
       0 failed, 2 skipped.**
     - **Flag-off identity (`v5a-identity.sh`, 40 stress tests,
       `--useJSThreads=false` vs no flags): 0 mismatches.**
     - **Bench gate: RED — chartered verify item NOT met. FAIL on
       `transition-heavy-constructor` (+6.73% / +7.57% on two runs, ambient
       load ~5 on 64 cores); all 7 other benchmarks within threshold;
       `megamorphic-access` is -12.3% vs the same baseline.** Baseline was
       recorded 2026-06-05 (15 runs) — a week of landed waves ago — and the
       -12% drift proves it is stale for this tree. Attribution of the
       transition delta to THIS slice is UNRESOLVED (cannot A/B without a
       rebuild; the pre-edit Release binary was overwritten by the 03:40
       build); note the previously parked V5b transition-path delta.
       Environmental attribution is PLAUSIBLE but UNPROVEN: this slice's
       only unconditional flag-off additions are the `owningThreadUid`
       field + ctor uid() call on the rare checkpoint-OSR push path and a
       `gilOff()` branch in `hasCheckpointOSRSideState` (exception unwind)
       — nothing on the transition path — and sibling slices touch
       InlineCacheCompiler/RepatchInlines on this shared tree. None of
       that downgrades the gate: **the verdict stands RED and this row's
       charter verification is INCOMPLETE until the documented builder A/B
       (current vs slice-reverted build) runs on a quiet host and
       attributes the delta.** Re-record the baseline only AFTER
       attribution.
     - *AMENDER FIX (2026-06-12, second write-only pass):* reviewer-caught
       inverted stack-order debug assert in the NEW GIL-off arm of
       `VM::pushCheckpointOSRSideState` (VM.cpp ~2024-2040): the loop
       iterated the vector FORWARD (oldest entry first = shallowest frame =
       highest address) while asserting `previousCallFrame < callFrame`
       seeded from `bounds.end()` — a comparison copied from the GIL-on arm,
       which iterates BACKWARD. Any thread with >=2 live own entries
       (multi-checkpoint inline exit via
       `operationMaterializeOSRExitSideState`, outermost-first) would
       FALSELY assert in GIL-off Debug. Fixed by iterating backward
       (newest-first) over this thread's entries, matching the GIL-on
       contract; GIL-on arm untouched, no assert weakened (the false assert
       is corrected, not removed — the per-thread ordering invariant is
       still enforced). UNVERIFIED AT RUNTIME: the Debug binary (03:05)
       predates ALL of this slice's edits and the amender charter is
       no-builds; builder must rebuild Debug and run the repro loop +
       regression test Debug GIL-off before calling the debug face sound.
     Row 1 status: **OPEN — instrumented + contract-hardened + K4 §I
     dependency landed + runtime gates executed (counts above), with the
     bench gate RED/unattributed and the Debug face unexecuted**, not FIXED
     and not fully verified:
     the fix is K4.II.8 at the owner's emission sites, then re-run this
     row's loops on that build to see whether the wrong-pc signature
     survives. Outstanding before this row's verification can be called
     complete: (a) builder bench A/B on a quiet host (above), (b) Debug
     rebuild + Debug GIL-off runs of the assert paths (above), (c) the
     K4.II.8 owner slice — see §3 item 0 ORCHESTRATOR ACTION.

   **ROOT CAUSE FOUND AND FIXED (2026-06-12, dw1 fixer pass; evidence:
   asan-bench-r3 W32-smoke-2).** The instrumentation paid off exactly as
   designed: the violation dump showed the stash FULLY MATCHING recovery
   (same threadUid=28, same caller baseline CodeBlock, same
   CallSiteBits=835) with recoveryOpcode=26. Opcode 26 is
   **op_call_ignore_result** (OpcodeID enum = FOR_EACH_BYTECODE_ID order:
   op_call=25, op_call_ignore_result=26). NOT cross-thread CodeBlock
   replacement, NOT per-lite CallSiteIndex routing, NOT corruption of any
   kind: a result-discarded sort site (`entries.sort(comparator);` as an
   expression statement — the bench's exact shape) compiles to
   op_call_ignore_result, and the trampoline's pc-recovery contract
   hard-coded op_call. handleArraySort can be hosted at any handleCall
   shape: op_call, op_call_ignore_result, op_tail_call (strict
   `return arr.sort(c)`); varargs (handleVarargsCall never attempts
   intrinsics) and iterator-protocol sites (argc < 2, rejected by
   handleArraySort) cannot reach it. The feared Release "wrong-pc dispatch"
   was in fact CORRECT dispatch (re-executes the ignore-result call —
   legal, the comparator may run any number of times pre-Commit), which is
   why checksums stayed byte-identical in every surviving run.
   *Deterministic single-threaded GIL-ON repro built* (function-literal
   comparator over `{k}` objects, int32 warmup, post-flip double `k` =>
   BadType OSR exit inside the inlined comparator): reproduced the exact
   `ASSERTION FAILED: pc->opcodeID() == op_call` on the pre-fix Debug
   binary on the first post-flip run — the bug is not GIL-off-specific and
   not load-dependent; only the comparator-OSR-exit *trigger* needed W=32
   pressure (global-variable flips don't work as a trigger: they fire the
   var watchpoint and jettison instead of exiting; GetById shape flips
   miss via IC without exiting — value-type flip is the reliable trigger).
   *Fix (root depth):* widened the recovery contract in
   `llint_slow_path_array_sort_comparator_return` (LLIntSlowPaths.cpp) to
   the exact host-opcode set {op_call, op_call_ignore_result, op_tail_call}
   in BOTH the GIL-off DW-1 RELEASE_ASSERT arm and the unconditional debug
   ASSERT (assert corrected, not weakened — any opcode outside the set
   still hard-stops with the dump); stale `callerReturnPC` comment updated
   (DFGOSRExitCompilerCommon.cpp). No JIT emission touched; flag-off
   codegen unaffected. *Verified on rebuilt Debug+ASAN:* repro PASS GIL-on
   and under pinned GIL-off flags; all three host shapes PASS with
   confirmed comparator-frame OSR exits (`bc#14 --> comparator ... BadType`
   in verboseOSR). New deterministic regression test:
   `JSTests/threads/dw1-sort-comparator-callsite-shapes.js` (all three
   shapes, single-threaded; green GIL-on AND GIL-off — unlike
   dw1-sort-comparator-osr.js it does not depend on K4.II.8).
   Row 1 wrong-pc signature: **EXPLAINED + FIXED.** Still open on this row:
   K4.II.8 scratch race (owner slice, §3 item 0) and the bench A/B
   attribution — separate mechanisms.

   **AMEND (2026-06-12, dw1 amend pass — adversarial review refuted the
   "exact host-opcode set" claim):** the widened recovery set
   {op_call, op_call_ignore_result, op_tail_call} was NOT exhaustive.
   handleIntrinsicCall's `isOpcodeShape<OpCallShape>` gate also admits
   op_iterator_open/op_iterator_next (OpcodeInlines.h), and
   BoundFunctionCallIntrinsic expansion grows argumentCountIncludingThis
   with bound args, defeating handleArraySort's argc < 2 rejection:
   `Array.prototype[Symbol.iterator] = Array.prototype.sort.bind(t, cmp)`
   gets ArraySortIntrinsic hosted at op_iterator_open with the comparator
   body-inlined as ArraySortComparatorCall — an OSR exit there recovers to
   an iterator pc outside the set (and a checkpoint-carrying one at that).
   *Amend (parser depth, per review recommendation):* host-opcode guard at
   the top of `ByteCodeParser::handleArraySort` restricting hosting to
   exactly the three plain call opcodes; trampoline left untouched
   (widening it to iterator opcodes would need checkpoint-aware recovery).
   Asserts unchanged — the recovery contract is now actually exact at the
   producer. Stale comments updated (LLIntSlowPaths.cpp,
   DFGOSRExitCompilerCommon.cpp). Guard is unconditional: the hosting bug
   is mode-independent (same as the original wrong-pc bug; debug ASSERT
   fires GIL-on too). New regression test:
   `JSTests/threads/dw1-sort-comparator-iterator-host.js` (bound-sort
   iterator host + post-flip double-k BadType trigger).
   *Also per review:* Release bin/jsc was STALE (12:03 binary vs 12:29 fix
   sources) and still aborted with the pre-fix DW-1 RELEASE_ASSERT under
   GIL-off — both Debug and Release rebuilt with the amend.
   *Verified on rebuilt Debug AND Release:* iterator-host repro PASS
   GIL-on + pinned GIL-off on both builds; verbose parse confirms the
   guard refuses hosting at op_iterator_open (0 ArraySortCompact, 0
   comparator inlines) while dw1-sort-comparator-callsite-shapes.js stays
   PASS on all four build x mode combos with hosting still active for the
   three plain shapes (10 ArraySortCompact, 54 comparator inlines in the
   verbose run). Fuzz/TSan dirs not rebuilt in this pass — rebuild before
   any campaign there (same staleness hazard).
2. **DW-2 markListSet proper fix** — confirmed UAF, currently held closed by a
   recon-only global lock. Land per-heap (or sharded) synchronization; the
   evidence and call path are in LIMP.md item 6.

   **DISPOSITION (2026-06-12, dw2-marklistset-landable-fix, write-only slice —
   pending builder compile + gates):**
   - *Landed shape: SHARDED per-heap set, not the dive's global lock.*
     `Heap::MarkListSetShard` (Heap.h: `Lock` + `UncheckedKeyHashSet`,
     32 shards, `m_markListSetShards`) selected by hashing the vector's
     address (`Heap::markListSetShard()`, `(word>>4)^(word>>12)` fold so
     per-thread stack bases decorrelate frame-offset collisions). Shared
     mode (`Options::useSharedGCHeap()`) registers/unregisters under the
     owning shard's lock only — hot-path cost is one uncontended-by-design
     per-shard lock around a single hash-set add/remove on the SPILL slow
     path only (inline-buffer vectors never touch it, same as before).
     Flag-off: `m_markListSet` + its lock-free accessor are untouched; all
     new code is behind `Options::useSharedGCHeap()` `[[unlikely]]`
     mode-splits, zero new LOCKING flag-off. Flag-off is mode-split-identical
     but NOT literally byte-identical: `MarkedVectorBase` grew a `Lock*`
     member (+8B per stack-resident MarkedVector/MarkedArgumentBuffer, one
     extra nullptr store per construction),
     `removeFromMarkSetAndDeallocateBuffer` gained a null check, and the
     split points load `Options::useSharedGCHeap()`. Per the flag-off LAW,
     the flags-off bench gate (delta 0) is therefore the BINDING proof for
     this change — the builder gate list below carries it as mandatory, not
     advisory.
   - *Complete reader/writer coverage* (repo-grep: no `m_markSet` /
     `markListSet` touch points outside these): MarkedVector.h `fill` +
     `fillWith` registration (the dive's crash path), `adopt` (lock+entry
     move stays in the moved-from vector's shard — shard choice is
     distribution-only, marking walks all shards),
     `removeFromMarkSetAndDeallocateBuffer` (remove under shard lock BEFORE
     the buffer free, so the marker can never iterate onto a freed spill
     buffer); MarkedVector.cpp `addMarkSet(JSValue)` +
     ADDRESS32 `addMarkSet(const void*)` (slowAppend/expandCapacity path),
     new out-of-line `addToSharedMarkSet(Heap&)`; Heap.cpp Msr constraint
     mode-split (walks every shard under its lock; flag-off arm verbatim).
     Vector side carries `MarkedVectorBase::m_markSetLock` (null flag-off =>
     historical lock-free mutations; non-null iff registered in a shard), so
     the destructor needs no Heap back-pointer.
   - *Regression test added:* `JSTests/threads/dw2-marklistset-storm.js` —
     W=16 spawned + main, per-round sort lane (len 257 => MarkedVector::fill
     spill), apply lane (64 args => MarkedArgumentBuffer slowAppend), GC/churn
     lane (shard walks concurrent with registration), per-round order/sum
     self-checks + per-seed reference-checksum comparison so silent
     corruption fails, not just the SEGV. Runs (degraded, single-thread)
     flag-off too.
   - *AUDIT-COVERAGE GAP RECORDED (not fixed here), per §1 row DW-2's K4/N7
     column:* shared-Heap C++ containers under useSharedGCHeap have no
     K4/N7-style sweep. Siblings observed unsynchronized while in Heap.{h,cpp}
     (mutator-reachable mutation, no lock, currently "API-lock predicate"
     or nothing): `m_protectedValues` (protect/unprotect, Heap.cpp:815/831 —
     T9 comment already flags the post-GIL predicate change but the SET
     itself is an unsynchronized HashCountedSet), `m_weakGCHashTables`
     (register/unregisterWeakGCHashTable, Heap.cpp:3690/3695),
     `m_observers` (add/removeObserver, Heap.h:402-403),
     `m_heapFinalizerCallbacks` (add/removeHeapFinalizerCallback). NOT in
     the gap: `m_arrayBuffers` (GCIncomingRefCountedSet has its own
     `m_lock`), `m_handleSet` (HandleSet.cpp `m_strongLock` + documented
     predicate). These need their own ruling rows (per-heap lock vs
     main-VM-only contract) before any of those APIs is reachable from a
     spawned Thread.
   - *Verification executed in-slice (PRE-FIX binary — the edits are not
     compiled yet; charter is write-only):* on the existing
     `WebKitBuild/Release/bin/jsc` (predates this slice), synchronous:
     - flags-off: `dw2-marklistset-storm.js` 1/1 PASS (test mechanics +
       determinism sound);
     - pinned GIL-off flags, W=16: **6/6 FAIL** (runs 2/4/5 SIGSEGV, runs
       1/3/6 180s timeout) — the storm reaches the racy surface at the
       ledger's sizing;
     - discriminator `+--useDFGJIT=0`: **3/3 FAIL** (2 SIGTRAP, 1 timeout)
       => NOT the K4.II.8 DFG sort-scratch family (and SORT_LEN=257 > 16
       avoids the inlined three-phase pipeline by construction);
     - independent re-verification (same slice, second pass, same PRE-FIX
       Release binary, 90s timeout): flags-off 1/1 PASS; pinned GIL-off
       flags W=16: **4/6 SIGSEGV, 2/6 PASS** — run-to-run variance vs the
       first pass's 6/6 (no timeouts hit this time), still a solid red
       baseline at the ledger's sizing;
     - gdb capture attempts: the wedged mode shows the main thread parked in
       `VMManager::notifyVMStop` from `CachedCall::callWithArguments` inside
       `sortStableSort`'s comparator (stop never converges); the SEGV itself
       de-races under gdb (0/3 caught). CAVEAT: this pre-fix binary is
       several landed waves old, so per-run signatures conflate DW-2 with
       whatever else those waves fixed/introduced — the red->green claim for
       THIS fix is decided on the builder's post-edit binary.
     Builder pass owns (ALL mandatory; DW-2 stays OPEN until every gate
     reports green on the post-edit binary):
     1. compile — watch `-Wthread-safety` on the `WTF_GUARDED_BY_LOCK(lock)`
        nested `MarkListSetShard::set` member: the in-class accesses
        (Heap.cpp shard walk, `addToSharedMarkSet`) all sit under
        `Locker { shard.lock }`, but `addToSharedMarkSet` then escapes
        `&shard.set` into the untracked `m_markSet` alias — if the analyzer
        flags the escape or any aliased access, fix at the annotation site
        (e.g. a locked accessor returning the set), never by weakening or
        dropping the GUARDED_BY;
     2. `dw2-marklistset-storm.js` pinned GIL-off flags, W=16 loop —
        demonstrated RED on the pre-fix binary is NOT evidence for this fix
        (see CAVEAT above: that binary conflates DW-2 with other landed-wave
        families, incl. the CachedCall/sortStableSort stop-protocol wedge);
        the claim that closes this row is red->green ON THE POST-EDIT
        BINARY. If still red, triage the residual signature — the test also
        exercises CachedCall/stop-protocol under load, so a residual wedge
        there is a DIFFERENT row, not a DW-2 reopen;
     3. GIL-off corpus run, no new failures;
     4. flags-off bench gate delta 0 — BINDING (flag-off is mode-split-
        identical, not byte-identical: +8B `Lock*` member, ctor nullptr
        store, dtor null check, Options loads at the split points).
        REVIEW RULING (dw2-marklistset-landable-fix amend pass, 1 major,
        conditionally approved on exactly this gate): if the flags-off
        delta is != 0, the major finding ESCALATES TO BLOCKER — the builder
        must NOT close this row, must record the measured delta here, and
        the prescribed remediation is the reviewer-named byte-identical
        variant: drop `MarkedVectorBase::m_markSetLock` entirely and derive
        the shard lock from `m_markSet` via container offset arithmetic
        (the set is embedded in `Heap::MarkListSetShard`, so
        `bitwise_cast<MarkListSetShard*>(reinterpret_cast<uint8_t*>(m_markSet)
        - OBJECT_OFFSETOF(MarkListSetShard, set))->lock`; shared-mode-ness
        is then re-derived from `Options::useSharedGCHeap()` at the
        remove/adopt sites, and `adopt` recomputes nothing — the entry
        stays in the moved-from vector's shard exactly as now). Uglier but
        restores byte-identical flag-off; only reach for it if this gate
        fails;
     5. flag-off identity run.
   Row 2 status: **OPEN — FIX WRITTEN IN-TREE (sharded per-heap), UNCOMPILED
   AND UNVERIFIED; not closeable from this write-only slice.** Every runtime
   number in this disposition is from the PRE-FIX binary and shows only that
   the storm reaches a racy surface, not that this fix removes it. Closure =
   builder gates 1-5 above all green, recorded here with exact counts. The
   recon-only `s_limpMarkListSetLock` paper exists only in the
   `/root/WebKit-limp` worktree and was never on this tree; nothing to
   revert here.
3. **DW-4 shared-heap anti-scaling** — re-measure on Release/quiet host to
   separate engine from oversubscription, then profile the allocator/GC-heap
   contention on map-heavy (worst, 3.6x).
4. **DW-3 Phase C bimodal lock parking** — pure perf, fully reproducible at
   smoke sizing (cheap to bisect: W sweep 32→64 around the modal flip).
5. Watchdog timeout policy — decide landed semantics (keep-waiting + dump vs
   fatal) given DW-3/DW-4 show legitimate >30s stop latency under load.

## 4. Honest summary — how much is hiding behind the current crash front?

Less than feared, with one sharp exception. With the four known crash mechanisms
papered over, 33 sequential runs at up to 64 threads produced zero LIMP firings,
zero ASAN reports, zero hangs, and bit-identical checksums on every completing
run — the papered front is, on this tree, mostly an ALREADY-FIXED front (U-T1
took out the varargs race) rather than a curtain with a monster behind it. What
the deeper water actually held: one genuinely new correctness bug class (DW-1,
OSR-exit pc recovery on spawned threads — debug-assert today, silent control-flow
corruption in release, and a hint that the whole inlined-frame/OSR-exit metadata
contract is under-audited for GIL-off), one shared-Heap container family the K4/N7
audits structurally could not see (DW-2 markListSet, and by extension any other
unsynchronized Heap-resident container under useSharedGCHeap), and two large,
reproducible scalability pathologies (DW-3/DW-4) that are tomorrow's perf work,
not today's crash work. Caveats on the "quiet" result: legs ran at smoke/variant
sizing (postings volume 14x below full), Debug+ASAN timing compresses/shifts
races, and DW-1 needed a specially shaped workload to appear at 1/6 — so the
right read is "the crash front is thin, but workload-shape-sensitive bugs of the
DW-1 kind are likely not alone," not "the tree is clean at W=64."
