# SPEC-jit history — review-resolution logs (NON-NORMATIVE)

Rev 10: the formerly normative appendices (App. G6, App. G+addendum, App. 5.6,
App. R1) were COPIED VERBATIM to `SPEC-jit-annex.md` (sub-cap, FROZEN NORMATIVE),
which also gained App. R5 (per-platform TID-tag TLS mechanics moved from spec R5).
This entire file is non-normative audit trail; consult the annex.

Moved verbatim from SPEC-jit.md to keep the frozen spec under its size cap.
The frozen spec (docs/threads/SPEC-jit.md) is authoritative; this file is
background only.

## 12. Review-round-1 disposition (Deviations/Notes for re-review)

Each round-1 finding, with what changed (or why not):

1/7. *§4.2 ARM64 ordering unsound* — **accepted**. Protocol replaced with a single
   aligned 64-bit pair load; holder-bearing inlined forms disabled (§4.2, F2 scope
   limit, F3). Verified fields adjacent at `PropertyInlineCache.h:421-422`.
2. *Epoch-only freeing of machine code* — **accepted**. Hard rule in §4.4: epoch
   frees data only; all executable memory is conservative-scan-gated (I7); the §5.3
   "or via RetiredCodeQueue" clause is deleted.
3. *Process-global epoch vs multiple heaps* — **accepted**; resolved by deleting the
   singletons and consuming the per-Heap `GCSafepointEpoch` (§4.4), making epoch
   domain ≡ safepoint domain by construction. M5 emptied accordingly.
4. *valueProfileLock() is NoLockingNecessary on 64-bit* — **accepted**. §1.8/D5
   corrected (cite `CodeBlock.h:817-821`); §5.7 re-derived: word-atomicity for
   buckets, `ConcurrentJSLocker` for multi-word Status objects (cites
   `CallLinkStatus.cpp:59-103`, `GetByStatus.cpp:190-197,467`).
5/9. *TTL elision "no mask / tag bits zero" wrong* — **accepted**. §5.5 adopts
   SPEC-objectmodel E1-E3 verbatim: elide checks, ALWAYS keep the mask; mask folds
   to zero only for proven main-thread (TID=0) tags. D6 explains the THREAD.md:15
   reading. I14 enforces.
6. *TID/SW predicates unspecified* — **accepted**. §5.5 now freezes the exact
   read/write/transition predicates (segmented iff top16 == 0xFFFF; write fast iff
   owner-tag match or SW bit; DCAS slow path otherwise) and the per-thread constant
   comes from R5/CS3.
8. *DFG/FTL direct-call patching unaddressed* — **accepted**. §1.10 ground truth
   (verified `DFGSpeculativeJIT64.cpp:1066`, `FTLLowerDFGToB3.cpp:13980,14025`,
   `CallLinkInfo.cpp:516-611`), new §5.8, D4, Task 7, I2/I3 extended. Indirect calls
   verified data-only in this tree (`CallLinkInfo.cpp:230-289,457-516`).
10. *Epoch crossing ≠ no live references (registers/slow paths)* — **accepted**.
   §4.4(a) cooperative-poll requirement (R1.f) + I16 poll-placement codegen rule for
   JIT'd state; §4.4(b)/I15 Ref-across-safepoint rule for native slow paths (the
   parked-in-allocation scenario is exactly why refcounting, not epoch alone, frees
   nodes).
11. *Task 9 needed runtime/** edits* — **accepted**. §5.6 centralizes classification
   and the stop protocol inside `bytecode/Watchpoint.{h,cpp}` (owned), default
   Class A, with the lock-deferral mechanism specified
   (`DeferredWatchpointFire` extension). No runtime/** fire-site edits or manifest
   entries needed.
12. *Task 2 acceptance gate unrunnable* — **accepted**. M2 split; M2a is a
   precondition landed before Task 2 (escape-hatch option in M1), §5.2.
13. *LLInt metadata "one word" wrong; inventory hand-waved* — **accepted**. §1.9
   corrected (12-byte struct, `GetByIdMetadata.h:41-52`); §4.3 now has the full
   frozen inventory table (verified against `BytecodeList.rb` and
   `LLIntSlowPaths.cpp` slow-path decls at :728/:778/:962/:1084/:1113/:1289/:1407/
   :1507), the put_by_id decision (transition cache disabled, replace cache
   survives), and the alignment mechanism (`UnlinkedMetadataTable.h:70`
   `s_maxMetadataAlignment = 8`).
14. *R1 ThreadSafepoint.h has no provider* — **accepted**. R1 rebuilt directly on
   VMManager (which SPEC-heap already extends and SPEC-objectmodel already asserts
   against); the `JSThreads` stop reason + resume ISB are THIS spec's manifest M4;
   the only thin wrapper (`JSThreadsSafepoint.h`) is an owned header.
15. *Two epoch facilities; JITEpoch never advanced* — **accepted**. JITEpoch/
   RetiredCodeQueue deleted; sole facility is heap's `GCSafepointEpoch` (R4, §4.4);
   CS4 records the bump-at-JSThreads-stop allowance.
16. *R1 missing arbitration/reentrancy* — **accepted** (rev-2 resolution; the
   R1.g wording below was itself found defective in round 2 and superseded —
   §13.1). R1.g (requesters are
   safepoint participants while blocked), R1.h (per-reason nesting via
   `VMManager.h:200-212` request bits; world-stopped fires run inline), §5.6 branch
   1; CS2 asks the heap workstream to mirror the wording.
17. *R3.b wording weaker than what emission relies on* — **superseded**: with the
   mask always emitted (5/9 above), the elision contract needed is exactly
   SPEC-objectmodel E1-E3/I12/I14, which is what R3 now cites; no stronger
   "tag-bits-zero for all threads" property is required of the object model.

## 13. Review-round-2 disposition (Deviations/Notes for re-review)

All round-2 blockers/majors were verified against the tree and accepted unless
noted; sub-claims refuted carry file:line evidence so the next round does not
re-trip on them.

13.1 *World-stop protocol deadlocks / M4 omits the dispatch path / R1.g has no
   referent* (two findings) — **accepted, protocol rebuilt.** Verified:
   `requestStopAllInternal` returns without blocking (`VMManager.cpp:223-276`);
   `Mode::Stopped` requires the requester itself to park
   (`shouldStop()`, `VMManager.cpp:413-430`); stop work runs in the per-reason
   callback on `m_targetVM` (`VMManager.cpp:456-477`); `case StopReason::GC:` is
   `RELEASE_ASSERT_NOT_REACHED` pending SPEC-heap's manifest. R1 is now a
   requester-as-conductor primitive (`stopTheWorldAndRun`: record job →
   requestStopAll → park in notifyVMStop → pinned as targetVM → run closure on own
   stack → Done → resume); M4 now lists the dispatch case + `JSCConfig.h` slot +
   registration hook + requester pinning, matching R1.a-h one-for-one. §5.3/§5.6/
   §4.4 reworded onto the same model; rev-2 R1.g replaced by the park-aware
   requester mutex (R1.g new text). The "StopScope RAII" is deleted.
13.2 *§5.8/F6 guard/payload protocol unsound (null after guard pass; ARM64
   cross-relink type confusion; DirectCall double-load)* (two findings) —
   **accepted, protocol rebuilt** on single-pointer immutable `CallLinkRecord`s
   (reviewers' option 1). Verified: `reset()` nulls all payload words from running
   slow paths (`CallLinkInfo.cpp:230-233`), `setVirtualCall` calls `reset()` first
   (`:278-288`), the data direct fast path loads `offsetOfTarget` twice
   (`:457-516`), DataOnly loads destination and callee as independent words
   (`:338-368`). The "benign stale destination" rationale is deleted; benignity now
   holds per-record (a stale record is a consistent triple for its own callee).
   F6 rewritten; I4 updated.
13.3 *§4.3 "stale mode byte is benign" false; Unset yields wrong JS results* (two
   findings) — **accepted.** Verified: the asm dispatches on the mode byte before
   reading word 1 (`LowLevelInterpreter64.asm:1650`, `.opGetByIdUnset` at
   `:1686-1691` validates structureID only); `setUnsetMode` is installed inside
   `setupGetByIdPrototypeCache` (`LLIntSlowPaths.cpp:887`). Frozen: Default +
   ArrayLength only; `setupGetByIdPrototypeCache` disabled wholesale; explicit
   pairwise mode-coherence argument replaces the deleted claim; new I18.
   **Partial refutation, recorded:** ArrayLength is retained (the suggested
   "disable ArrayLength for uniformity" is unnecessary) — its reader never touches
   word 1 and self-validates via `m_indexingTypeAndMisc` (`asm:1675-1683`), and all
   four Default↔ArrayLength interleavings are individually valid because the
   site's identifier is fixed (§4.3 argument).
13.4 *§5.7 tier-up claims false (no m_lock in operationOptimize; enqueue asserts,
   doesn't dedup; dfg/DFGWorklist doesn't exist)* — **accepted.** Verified:
   no locker in `operationOptimize` (`JITOperations.cpp:3027` ff.);
   `ASSERT(m_plans.find(...) == m_plans.end())` at `JITWorklist.cpp:187`;
   `ls dfg/DFGWorklist*` → no such file. §5.7 now specifies the per-CodeBlock
   tier-up CAS + enqueue dedup (both owned); §7 citation fixed; Task 12 extended.
13.5 *Lock-held Class-A fires have no release-mode remediation; "tracked lock
   counter" unimplementable from owned paths* (two findings) — **accepted.**
   §5.6 deferral rewritten: deferred-overload fires stop at scope exit (lock-free
   by construction); direct fires are required lock-free per existing convention;
   Task 11's caller audit (call sites are grep-enumerable even in non-owned files)
   feeds new manifest slot **M6** (mechanical `DeferredWatchpointFire` conversions
   in `runtime/**`, applied by the integration agent); debug stop-progress
   watchdog replaces the unimplementable lock-rank RELEASE_ASSERT. The reviewers'
   alternative (a) — generic queue-the-fire-in-fireAllSlow with deferred jettison —
   was evaluated and REJECTED as unsound: the firing thread would proceed past the
   guarded mutation while its own speculatively-folded code is still installed,
   breaking same-thread sequential consistency (rationale recorded in §5.6 step 6).
13.6 *Objectmodel M7 consumed-but-dropped* — **accepted.** Verified M7 normative
   text at SPEC-objectmodel.md:820-830 and the E3 clause at :693; `Dependency`
   idiom at `runtime/JSObject.cpp:385-405`. Added R7, F7, §5.5 per-tier emission
   rules, E3 restatement fixed, I14 pass extended.
13.7 *Non-atomic refcounts (RefCounted handler, plain unsigned stub-routine
   count)* — **accepted.** Verified `InlineCacheHandler.h:55` and
   `JITStubRoutine.h:106-118,159`. New §4.5 + I17 + §1.11 ground truth; both files
   owned; unconditional atomicity with an explicit I1 carve-out note.
13.8 *Pinned-base plumbing assigned here but absent from the task list; "existing
   per-thread base register" has no referent* — **accepted.** R5 rewritten with a
   frozen mechanism (initial-exec TLS, constant offset baked as immediate; per-arch
   sequences; no pinned GPR for MVP); new Task 1b; §5.5 instruction-count note
   updated (+1/+2 insns on non-elided write fast paths, hoistable); CS3 downgraded
   to optional.
   **Partial refutation, recorded:** the wider claim that ALL SPEC-vmstate Phase-B
   per-thread plumbing (per-thread `VMTraps`/`VMThreadContext`, VMLite-relative
   scratch buffers) is this spec's unbudgeted burden is wrong for this spec's
   actual dependencies: under the shared-heap design each mutator thread is a
   registered client VM with its OWN existing `VMTraps` (SPEC-heap I4;
   `VMManager.cpp:253-272` iterates per-VM traps), which is all R1's
   cooperative-park protocol needs; the only per-thread datum this spec's
   generated code reads is the butterfly TID tag, now fully specified in R5/Task
   1b. Anything beyond that remains SPEC-vmstate Phase-B scope
   (SPEC-vmstate:206,703,966) and is not consumed by any §11 task.

## Appendix G6 — runtime/** watchpoint-fire files (grep evidence for SPEC-jit G6)

Structure.cpp, JSGlobalObject.cpp, VM.cpp, FunctionRareData.{h,cpp},
ObjectAdaptiveStructureWatchpoint.h, ObjectPropertyChangeAdaptiveWatchpoint.h,
InternalFunction.cpp, RegExpPrototype.cpp, ProgramExecutable.cpp, ArrayBuffer.cpp,
SymbolTable.h, InferredValue.h (plus further hits in bytecode/ and dfg/, owned).


## Appendix G — SPEC-jit §1 ground truth, unabridged (moved verbatim to meet the size cap; the in-spec index is authoritative)

G1. IC split = Handler vs Repatching per IC object; HandlerIC (Baseline+DFG) = pure data dispatch through a prepended-LIFO `RefCounted` `InlineCacheHandler` chain, RepatchingIC = FTL only (`bytecode/PropertyInlineCache.h:100-123,384`; `InlineCacheHandler.h:55,91-94,106,164`). Handler code held via `Ref<GCAwareJITStubRoutine>`, freed only after GC proves off-stack (`jit/GCAwareJITStubRoutine.h:86`; `heap/JITStubRoutineSet.cpp:133` `deleteUnmarkedJettisonedStubRoutines`).
G2. **Safepoint-free handler window** (`PropertyInlineCache.h:555-590`): stubs read handler fields only *before* making any call, never after one returns; shared thunks (`VM::m_sharedJITStubs`) = pure data dispatch. Basis of §4.4.
G3. Handler publish unfenced today (`PropertyInlineCache.cpp:960-963`) under `CodeBlock::m_lock` (`bytecode/CodeBlock.h:813`; a real 1-byte `WTF::Lock`, `runtime/ConcurrentJSLock.h:34-37`); JIT'd readers load `m_handler` raw.
G4. `useHandlerICInFTL`: default false (`runtime/OptionsList.h:638`), FORCE-DISABLED "not completed" (`runtime/Options.cpp:814`); lowering substantially present (`ftl/FTLState.cpp:172-179`; ~20 patchpoints `ftl/FTLLowerDFGToB3.cpp:4750,4774,4906,5050,5295,5668,…`; `dfg/DFGStrengthReductionPhase.cpp:1754-1760`).
G5. Jettison patches code at runtime: `bytecode/CodeBlock.cpp:2294` → `dfg/DFGCommonData.cpp:58` `invalidateLinkedCode` → `dfg/DFGJumpReplacement.cpp:36-42` `replaceWithJump`.
G6. Watchpoint firing single-mutator-shaped (`bytecode/Watchpoint.cpp:129-147`; no cross-thread sync); fire sites span ~20 files incl. non-owned `runtime/**` (12 files grep-enumerated in history App. G6) ⇒ centralize in owned `fireAllSlow` (§5.6).
G7. Stop-the-world is CONDUCTOR-shaped: `requestStopAllInternal` (`runtime/VMManager.cpp:223-276`) is non-blocking; `Mode::Stopped` is reached only when EVERY active VM — incl. the requester's (`:260-272`) — parks in `notifyVMStop` (`:345-458`, `shouldStop()` `:413-430`; a requester spinning without parking deadlocks at active−1); last parker = conductor `m_targetVM` (`:456-457`) dispatching the per-reason callback switch (`:461-477`; `JSCConfig.h:109-114`; `case GC:` asserts, `:462-463` — heap fills it; a new reason needs switch case + config slot); done ⇒ clear bit, resume (`:479-489`). Stops cooperative (poll sites; idle VMs `dispatchStopHandler` `:282-303`), never async suspension. Heap §10 shares this; every client mutator VM registered (its I4), each with its own `VMTraps`.
G8. Profiling: plain `int32_t m_counter` add (`bytecode/ExecutionCounter.h:57,90`); LLInt tier-up counter on shared `UnlinkedCodeBlock` (`CodeBlock.h:612`); value profiles NOT lock-guarded on 64-bit (`CodeBlock.h:817-821`, `NoLockingNecessaryTag` under `USE(JSVALUE64)`); multi-word Status snapshots ARE under `m_lock` (`CallLinkStatus.cpp:59-103`, `GetByStatus.cpp:190-197,467`); tier-up triggering NOT serialized (`operationOptimize`, `jit/JITOperations.cpp:3027` ff.); worklist does NOT dedup (`jit/JITWorklist.cpp:163-193`, ASSERT at `:187` — races corrupt `m_totalLoad`/queues in release); no `dfg/DFGWorklist` exists.
G9. LLInt caches into bytecode metadata directly (`llint/LLIntSlowPaths.cpp`; `try_get_by_id` `:761-768` re-publishes id *before* offset — not a seqlock); `GetByIdModeMetadataDefault` is 12B/4-aligned, NOT one u64 today (`bytecode/GetByIdMetadata.h:41-69`); ProtoLoad 16B incl. `JSObject*`; metadata table supports 8-byte alignment (`bytecode/UnlinkedMetadataTable.h:70` `s_maxMetadataAlignment = 8`).
G10. Indirect calls dispatch through `CallLinkInfo` data (`bytecode/CallLinkInfo.cpp:230-233,279-289`; `CallLinkInfo.h:320,437`); **`DirectCallLinkInfo` with `UseDataIC::No` still patches code** (`CallLinkInfo.cpp:516-541`; `repatchSpeculatively` `:575-611` from `addLateLinkTask`, possibly a compiler thread, `:603`); only `UseDataIC::No` sites: `dfg/DFGSpeculativeJIT64.cpp:1066`, `ftl/FTLLowerDFGToB3.cpp:13980,14025` (owned); data-IC direct fast paths exist with complete `isDataIC()` branches (`CallLinkInfo.cpp:457-516`).
G11. Refcounts non-atomic today: `RefCounted<InlineCacheHandler>` (`bytecode/InlineCacheHandler.h:55`); plain `unsigned` `JITStubRoutine::m_refCount` (`jit/JITStubRoutine.h:106-118,159`).
G12. THREAD.md supporting facts: `heap/LocalAllocator.cpp:138,170-181,249` allocation FIXMEs (heap workstream); 2-bit cell lock `runtime/IndexingType.h:97-98,230`; atom table locker `WTF/wtf/text/AtomStringImpl.cpp:42-63` (vmstate; consumed here only as: uid/`CacheableIdentifier` pointers are stable, pointer-compared).



## Appendix 5.6 — SPEC-jit watchpoint-fire deferral, unabridged (relocated for size cap; the in-spec rule is authoritative)

* **Deferral** (`DeferredWatchpointFire`, `Watchpoint.h:493-508`; overload `Watchpoint.cpp:139-147` invalidates immediately, fires at caller scope exit): (a) Class-A via deferred overload: as today; the scope-exit fire — lock-free by construction — performs steps 2-6. (b) Class-A via DIRECT `fireAll`/`fireAllSlow` (`Watchpoint.h:226-249`, `Watchpoint.cpp:129-137`): REQUIRED lock-free w.r.t. every §7 lock and every cell lock. (c) Task 11 audits every direct caller (grep-enumerable even in non-owned files) → (i) lock-free, (ii) world-already-stopped, (iii) holds a §7/cell lock; bucket (iii) → **manifest M6**; audit table ships in the PR; empty M6 expected, populated M6 = specified fallback. (d) Debug watchdog in `stopTheWorldAndRun`: RELEASE_ASSERT if `Mode::Stopped` is missed within a generous timeout (an escaped bucket-(iii) site deadlocks the stop → crash naming the set). Lock-rank counters NOT specified — uninstrumentable from owned paths.


## 14. Rev 4 — editorial size-cap compression (no normative change)

Rev 3 measured 44358 bytes, over the 40000-byte hard cap. Rev 4 compresses wording only:
§1 ground-truth entries reduced to index lines (full text: App. G); §5.6 deferral bullet
shortened (full text: App. 5.6); R1 mechanics shortened (full rev-3 text: App. R1 below);
§5.7 reflowed as a terse list. Every invariant (I1-I18), fence (F1-F7), lock order, layout,
signature, manifest entry (M1-M6, CS1-CS4), interface (P1-P4, R1-R7), table row, and task
(1-14) is unchanged in content.

## Appendix R1 — SPEC-jit R1 mechanics, unabridged rev-3 text (the in-spec rule is authoritative)

Mechanics (M4 additions + owned veneer):
a. stop reason `v(JSThreads)` in `FOR_EACH_STOP_THE_WORLD_REASON` (`runtime/VMManager.h:200-212`);
b. `notifyVMStop` dispatch `case StopReason::JSThreads:` calling a new `StopTheWorldCallback JSC_CONFIG_METHOD(jsThreadsStopTheWorld)` slot (mirror existing cases) + a `VMManager::setJSThreadsCallback` hook (pattern `VMManager.h:272-277`), registered at first flagged VM creation from owned `bytecode/JSThreadsSafepoint.cpp`;
c. **requester pinning**: `stopTheWorldAndRun` records `{&vm, &work}` in a single-slot pending-job field (owned static, see g.), calls `VMManager::requestStopAll(StopReason::JSThreads)`, then PARKS by entering `notifyVMStop` itself; M4 extends targetVM arbitration so that under reason JSThreads only the recorded requester VM is released from the `shouldStop()` wait — deterministically the conductor; the callback runs the pending `work` on the requester's own stack, world stopped, then `IterationStatus::Done` (loop clears bit, resumes); stack-borrowed state stays valid;
d. resume-path hook: instruction-stream barrier (ISB/equivalent) on every mutator returning from `notifyVMStop` when the serviced stop included `JSThreads` or `GC` (F5);
e. (epoch bump is the heap's, §4.4);
f. **cooperative stops only**: park exclusively at trap-check/VM-entry poll sites, never async suspension — load-bearing for §4.4(a);
g. **requester-vs-requester**: callers serialize on an owned park-aware mutex guarding the pending-job slot (`while (!tryLock()) { if stop pending for this VM, park via notifyVMStop; else yield; }`) — a loser PARKS (counting as stopped) while the winner's stop runs, then retries; no deadlock between two Class-A firing mutators;
h. reasons nest via per-reason request bits + one-reason-at-a-time service loop (`m_currentStopReason`, `VMManager.cpp:391-411`); a Class-A fire reached world-stopped runs inline without re-requesting (§5.6 branch 1) — `stopTheWorldAndRun` checks this first. Heap's GC stop shares the machinery; **CS2** confirms serial multi-reason semantics.

Rev-4 erratum fixed in passing: SPEC-jit §5.7 rule 6 referenced "Task 10" for the
`computeFor*` Status grep audit; the audit is Task 12's (renumbered in an earlier rev).
Corrected to Task 12. No other cross-reference changed.

## 15. Rev 5 — adversarial-review round-1 dispositions (rev 4 -> rev 5)

Each finding verified against THREAD.md, the tree, and sibling specs before disposition. Re-frozen as rev 5. Where rev 4 conflicted with SPEC-heap rev 5 (also frozen), heap's normative dispositions win — both specs are implemented by the same non-coordinating fleet and heap owns the stop/epoch machinery.

1. **R5 TID-tag never initialized on spawned threads + dangling `SPEC-vmstate:703` citation (blocker + 2 dups) — ACCEPTED.** Verified: SPEC-vmstate.md is 669 lines; zero hits for `butterflyTIDTag` in vmstate/api; vmstate §6.7 (`SPEC-vmstate.md:510-530`) is the real TID anchor; objectmodel §9.1 (`SPEC-objectmodel.md:372-373`) pointed at the VMLite-field variant. Zero-initialized TLS = tag 0 = main-thread owner tag ⇒ every spawned thread would take the owner write path with SW=0 — exactly the lost-write corruption the regime split prevents. Resolution: CS3 promoted OPTIONAL → MANDATORY with a concrete contract (api §5.2 spawn / vmstate `VMLite::setCurrent` call jit-exported `initializeButterflyTIDTagForCurrentThread()` after TID assignment, before any JS; clear at detach); new P5 exports; new I19 (VM-entry RELEASE_ASSERT tag == TID<<48; 3-thread test); the `VMLite::butterflyTIDTag` field alternative DROPPED; citation fixed to vmstate §6.7.
2. **R1.h/CS2 vs heap GCL serialization (blockers, 3 overlapping filings) — ACCEPTED.** SPEC-heap rev 5 normatively answers CS2 the other way: `SPEC-heap.md:342` (JSThreads stops hold GCL rank 2 for their stopped window; R1.h overlap unsound vs the access barrier), `:415` (GC never in VMM dispatch; `:461-463` assert stays; M4/heap NVS-tail ordering), `:420b` (CR). Resolution: new G13; R1.h rewritten (GC does NOT share the VMM latch; rev 4 nesting claim withdrawn); new R1.i (STWR releases heap access, takes the heap-exported GCL bracket around the whole stopped window; progress argument per heap §10C); GCL added as the outermost row of §7; CS2 re-purposed to request the exported bracket (`Heap::jsThreadsStopScope()`-shaped; GCL is private), with STWR forbidden under `useSharedGCHeap` until it lands; `worldIsStopped()` redefined = VMM `Mode::Stopped` OR heap `worldIsStoppedForAllClients()` so Class-A fires in GC context (finalizeUnconditionally/visitWeak) classify as branch-1 inline fires and never nest a JSThreads stop inside a GC stop.
3. **CS4 `bumpAndReclaim` MAY-clause (blocker/major, 3 filings) — ACCEPTED (CS4 recorded REFUSED).** Heap I11/§11 (`SPEC-heap.md:199,278-279,353`) RELEASE_ASSERTs GC-conductor-only and shows a non-GC bump reclaims against stale `m_localEpoch`s. §4.4 cadence now: GC world-stops only; a JSThreads stop needing bounded reclamation enqueues a GC request (heap CR 13.10a). Rev 4's "already permitted by its precondition" claim was factually wrong.
4. **R1.c requester-pinning had no VMManager interface (major) — ACCEPTED.** Verified today's arbitration makes the LAST parker the conductor (`VMManager.cpp:455-460`). Resolution: M4 item c frozen as `VMManager::requestStopAllWithConductor(StopReason, VM* conductor)` + JSThreads-conductor arbitration (`m_targetVM = m_jsThreadsConductor` once all parked); pending-job slot stays owned; M4 list restated as exactly R1.a-e.
5. **§5.6 ">1 mutator" gate racy vs thread attach (blocker) — ACCEPTED.** Verified `notifyVMConstruction` parks new VMs only while a stop is in progress (`VMManager.cpp:532-545`); an inline fire under count==1 races an attaching thread (I10 violation). Resolution: fast path DELETED — flag on, Class-A fires ALWAYS take branch 1 or STWR; Task 1's interim degrade is `RELEASE_ASSERT(!Options::useJSThreads()); work();` (no count source needed).
6. **I1 unimplementable for layout repacks + LLInt (major) — ACCEPTED.** Resolution: new D7 (repacks are unconditional compile-time layout changes; flag gates only publication discipline/predicates/disables); I1 rescoped (JIT-emitted sequences identical modulo §4.2/§4.3/§5.8 offset immediates; shapes/counts identical; LLInt differs by one not-taken gate branch); LLInt gate mechanism frozen in §5.4 (M4-added `JSCConfig` byte + `ifJSThreadsBranch` offlineasm macro, once per affected fast path) with a `--useJIT=0` bench-gate assertion in Task 13; §4.3 records per-op metadata size deltas at Task 6.
7. **I14/I16 covered only DFG/B3 (major) — ACCEPTED.** Resolution: §5.5 choke-point rule (LLInt `loadButterflyForRead/ForWrite` macros = only places the butterfly offset may appear in `llint/*.asm` flag-on; `CCallHelpers::loadButterflyForRead/ForWrite` for Baseline/stubs; grep lint + per-site inventory in INTEGRATE-jit.md, Task 8); deterministic runtime check via spawned-thread (TID != 0 ⇒ nonzero tag bits) stress in Task 13 — unmasked dereferences fault. I14/I16 verification clauses updated per tier.
8. **§5.8 missing per-flavor layout, virtual-mode semantics, dangling Task 9b (major) — ACCEPTED.** Verified virtual mode = `polymorphicCalleeMask` (=1) written into `m_callee` + low-bit `branchTestPtr` (`CallLinkInfo.cpp:282,310,342-348`; cells never bit-0, `:136,152`) — an equality-only comparand was wrong. Resolution: frozen fast-path sequence `(c == callee || (c & polymorphicCalleeMask))`; per-flavor `m_record` placement frozen (DataOnly/Optimizing/Direct, +8B per call-op metadata for DataOnly, D7); GC contract frozen (comparand = raw word, never visited; legacy mirror stays the sole GC root/weak ref; visitWeak nulls `m_record` on clear/relink; dead-callee stale records can't match because matching requires the caller to hold the callee live); "Task 9b" folded into Task 7.
9. **Task 2 gated on mid-flight non-owned M2a (major) — ACCEPTED.** Resolution: M2a reclassified PREP-PHASE precondition (lands before implementation starts; behaviorally inert since the unlock option defaults false) + an owned interim smoke path (Options are mutable globals; `:814` runs once at finalize; temporary env-var re-assign from owned FTL init, removed at handoff).
10. **No owned test paths / amplifier doesn't exist (major) — ACCEPTED.** Resolution: owned paths extended with `JSTests/threads/jit/**`; Task 13 rebuilt on owned `$vm`-driven stress loops; `Tools/threads/amplify.sh` confirmed absent (api G15) ⇒ amplifier integration explicitly BEST-EFFORT, not a gate.
11. **"R1.d has no GC reason serviced in NVS to key off" — PARTIAL FALSE POSITIVE (noted in-spec as N1).** GC-parked mutators DO park and exit through `notifyVMStop`: heap's keep-parked GC-bit `shouldStop()` condition and its `gcWillPark`/`gcDidResume` hooks live at NVS entry/exit (`SPEC-heap.md:395-415`), and heap manifest entry 5 explicitly orders "M4's crossModifyingCodeFence first, then our (a) resume hook" (`:415`) — i.e. heap already plans around R1.d's barrier at exactly that location. The valid kernel of the finding (GC not dispatched through the reason switch) is G13/R1.h.

### App. G addendum (rev 5)

G7 (addendum): `notifyVMConstruction` parks a newly constructed VM only when `m_worldMode != Mode::RunAll`, i.e. only while a stop is in progress (`runtime/VMManager.cpp:532-545`); no synchronization with an in-flight inline watchpoint fire — basis for deleting the §5.6 mutator-count gate.
G10 (addendum, virtual mode): `CallLinkInfo::setVirtualCall`/`setStub` write `polymorphicCalleeMask` (= 1, `CallLinkInfo.h:77`) into the `m_callee` slot (`CallLinkInfo.cpp:282,310`); data-IC fast paths treat a low-bit-set comparand as "always call" via `branchTestPtr(NonZero, ..., polymorphicCalleeMask)` (`:342-348`); callee cells never have bit 0 (`RELEASE_ASSERT`s `:136,152`). §5.8's sentinel comparand reproduces exactly this predicate.
G13 (new; full text lives in SPEC-jit §1): heap rev-5 dispositions — GC outside the VMM reason latch (`VMManager.cpp:461-463` assert stays; heap §13.5c), GCL serialization for JSThreads stops, `bumpAndReclaim` GC-conductor-only (`SPEC-heap.md:199,278-279,342,353,415,420`).

### Rev 5 editorial note

Rev 5 also re-compressed for the 40000-byte cap: §1 G1-G12 reduced to an index (App. G above authoritative); §5.8's per-flavor table rendered as a frozen list; deviations D1-D5 merged into one run; headings shortened. Every invariant (I1-I19), fence (F1-F7), lock order, layout, frozen sequence, manifest entry (M1-M6, CS1-CS4), interface (P1-P5, R1-R7), inventory row, and task (1-14, incl. 1b) is present in rev 5; no normative content was dropped — items changed only where this §15 says so.

## §16. Round-2 adversarial-review resolution log (rev 5 -> rev 6)

Ten findings filed (3 of them the same ArrayStorage blocker). All verified against THREAD.md, the tree, SPEC-objectmodel rev 7, and SPEC-heap rev 6. **All were real** — no pure false positives this round; rev 6 resolves every one. Dispositions:

1. **§5.5 omits the SW=1 ∧ ArrayStorage regime-3 dispatch (BLOCKER; filed 3x; = objectmodel manifest entry 8 / I31) — ACCEPTED.** Verified: SPEC-objectmodel rev 7 §2 decode ("SW=1 dispatch ALSO loads the indexing byte: ArrayStorage/SlowPut shape => regime 3, EVERY access locked"), §3 read rule, §4.6 (AS never segments; shift/unshift `JSArray.cpp:1650,1818` mutate innards in place under the cell lock), I31, L5, and manifest entry 8's explicit "recorded JIT blocker for shared ArrayStorage". Rev 5 indeed never mentioned ArrayStorage; its Read predicate ("else mask, proceed as today") and Write branch (3) emitted unlocked masked accesses on SW=1 AS butterflies — racing the runtime's locked in-place mutations => OOB/torn indexing state. Resolution (rev 6): new §5.5 **AS-rule** adopting manifest 8 verbatim — an AS-shape compiled/interpreted fast path is legal only via (a) E2 elision (writeThreadLocal valid+watched+registered, fire => §5.3 jettison, so SW=1 unreachable in that code), (b) an SW-bit test routing SW=1 to new R3 locked operations (`operationSharedArrayStorage*` shims in owned jit/ConcurrentButterflyOperations), or (c) excluding AS array modes; watchpoint-invalid compiles MUST use (b)/(c); generic paths (LLInt asm array paths, generic IC cases) load the indexing byte on the SW=1 branch because the tag alone cannot distinguish AS. Read and Write predicates updated; new invariant **I20** (mirror of objectmodel I31) wired into the I14 validation pass + choke-point lint + a Task 13 shared-AS shift/unshift-vs-readers stress; §3 scope item (4) and Task 8 updated; adoption recorded in §2 N2.

2. **§5.5 Transition predicate dropped E4's transitionThreadLocalTID runtime compare; "owner" undefined for butterfly-less transitions (MAJOR) — ACCEPTED.** Verified objectmodel E4 (§5): owner transition requires currentButterflyTID() == source->transitionThreadLocalTID() AND tag == (currentTID,0) AND both sets valid+watched; F2 fires BOTH sets on the first transition by a thread != S's transition TID. Rev 5's "owner + SW=0 + sets valid&watched" was unsound for shared compiled code: Baseline machine code is executed by every thread, and watchpoint validity does NOT license the executing thread — thread A reaching the compiled transition of a structure whose m_transitionThreadLocalTID is B, while the sets are still valid, would BE the first foreign transition (must fire F2 first). Also "owner" had no meaning for butterfly-less N1/N2 structure-only transitions (ownership = Structure::m_transitionThreadLocalTID; no tag word exists). Resolution: Transition predicate rewritten to E4 EXACTLY with the explicit runtime emission (load g_jscButterflyTIDTag; compare against `tid << 48` immediate when the IC/compile specializes on S, else against Structure::m_transitionThreadLocalTID zero-extended <<48; butterfly-bearing additionally tag == (currentTID,0)); spec states verbatim that watchpoint validity is NOT a substitute for the runtime TID check.

3. **§4.2 repack silently dropped WriteBarrierStructureID barrier semantics + no legal C++ cross-member 64-bit store (MAJOR) — ACCEPTED.** Verified in tree: `PropertyInlineCache.h:421-422` (`PropertyOffset byIdSelfOffset; WriteBarrierStructureID m_inlineAccessBaseStructureID;`) and every publish via `.set(vm, codeBlock, structure)` (`PropertyInlineCache.cpp:50,71,80,894,899,905,910`), clears at `:257,278,927`. An unbarriered raw 64-bit publish under concurrent marking can leave the cached Structure unmarked => freed => StructureID recycled => IC id-compare false-positives => type-confused offset load. Resolution: §4.2 freezes a `union { struct { byIdSelfOffset; m_inlineAccessBaseStructureID; }; std::atomic<uint64_t> m_packedSelfWord; }` overlay (+static_asserts; no strict-aliasing UB — accesses go through the union's atomic member), publish = word store FOLLOWED BY `vm.writeBarrier(codeBlock)`; invalidation to the all-zero word needs no barrier (matches today's `.clear()`); `visitAggregate` keeps reading the id half.

4. **Stale/dangling cross-spec line citations (MAJOR; filed 3x with overlapping lists) — ACCEPTED.** Verified every flagged cite: SPEC-objectmodel.md is 417 lines (the ":820-830" M7 cite was past EOF; M7 is in its §7); SPEC-heap.md is 398 lines after rev-6 compression (":415"/":420b" past EOF; ":218"/":342"/":353" land on unrelated or blank lines; heap manifest 10e itself requests re-anchoring and its recorded map of jit's cites was also stale). Resolution: EVERY cross-spec `SPEC-*.md:<line>` citation replaced with section anchors (heap §9/§10/§10C/§13.5/§13.10/I11/manifest 5a/10b/10e; objectmodel §7 M7/§9/manifest 6/8; vmstate §6.7 — the rev-5 ":510-530" cite was also stale, §6.7 actually sits at :552); header re-scoped: "in-tree file:line cites verified on branch; cross-spec cites = SECTION ANCHORS ONLY"; rev 6 re-frozen against heap rev 6 + objectmodel rev 7.

5. **CS2 phrased as pending while heap rev 6 already provides the bracket (part of the cite findings) — ACCEPTED.** Heap rev 6 §9 declares `class Heap::JSThreadsStopScope` (RAII over GCL, rank 2; pre: caller released heap access; never bumpAndReclaim inside; no-op when !isSharedServer()) and manifest 10b records CS2 RESOLVED-AS-PROVIDED. Resolution: CS2 rewritten as RESOLVED-AS-PROVIDED; R1.i consumes the class by that exact name; the rev-5 "until exported, STWR under useSharedGCHeap = RELEASE_ASSERT" interim clause deleted.

6. **M4 landing time unspecified; Tasks 5/11/13 unexecutable against the rev-5 degraded stub (MAJOR) — ACCEPTED.** Rev 5's stub (`RELEASE_ASSERT(!Options::useJSThreads()); work();`) made every flag-on test crash, while Task 13's flag-on gates required a working stop. Resolution: §10 M4 gains an explicit frozen disposition — **M4 is INTEGRATION-DEFERRED** (unlike M2a, which stays a prep-phase precondition); Task 1's interim stub upgraded to mirror objectmodel manifest 6's: STWR RELEASE_ASSERTs <=1 entered VM (phase-1 GIL), runs the closure inline on the requester's stack, worldIsStopped(vm) true inside; integrator swaps the body to M4+CS2 at the integration gate. Task 13 split into PRE-integration (runnable now: golden disasm diff, --useJIT=0 bench gate, validateButterflyTagDiscipline + poll-placement, spawned-thread butterfly + shared-AS stress GIL-interleaved, IC publish/reset loops) and INTEGRATION-GATE suites (true-concurrent jettison-vs-execute, fire-vs-execute, direct-call-relink, epoch reclamation) that skip while STWR is the stub and re-run unmodified once M4/CS2 land — same pattern as objectmodel Task 12.

7. **R5 constant-offset initial-exec TLS unimplementable on macOS (MAJOR) — ACCEPTED.** Verified: this fork ships macOS x64/arm64 (CLAUDE.md CI matrix, mac-release.bash); Mach-O TLV resolves thread_local through per-variable tlv_get_addr descriptors + lazily allocated storage — no architected constant offset from the thread register, so the rev-5 RELEASE_ASSERT would fire at second-thread startup with both alternatives (pinned GPR, VMLite field) frozen out. Resolution: R5 split per platform (new D8): ELF keeps the constant-offset scheme with the previously unstated `__attribute__((tls_model("initial-exec")))` requirement (shared-library builds otherwise use dynamic TLS models with no constant offset); Darwin freezes a reserved pthread direct key — `#define BUN_JSC_BUTTERFLY_TID_TAG_KEY __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY4` in an owned header (keys 0-1 bmalloc, 2-3 WTF per WTF/wtf/FastTLS.h:39-47), written via `_pthread_setspecific_direct` in P5's init, emitted with the EXISTING `loadFromTLS64(fastTLSOffsetForKey(key))` (MacroAssemblerX86_64.h:7411 / MacroAssemblerARM64.h:6399) and LLInt `tls_loadp` (offlineasm/instructions.rb:348-349) — same shape (one load at a constant offset from the thread register); other targets (Windows) RELEASE_ASSERT at second-thread startup flag-on, MVP-unsupported.

8. **§5.6 worldIsStopped() missed legacy (non-shared-server) GC stops — Class-A fires inside a legacy collection would take the STWR branch, nesting a JSThreads stop inside a GC (MAJOR) — ACCEPTED.** Verified: heap F7 makes WSAC conductor-only (shared protocol §10 step 4); legacy collections set neither WSAC nor VMM Mode::Stopped; objectmodel §6 confirms phase 1 runs the LEGACY heap under useJSThreads (no spec implies useJSThreads => useSharedGCHeap); but the in-tree legacy heap sets `m_worldIsStopped` in stopThePeriphery (Heap.cpp:1896, exposed as `Heap::worldIsStopped()`, Heap.h:386) across all world-suspended collector phases incl. End (CollectorPhase.cpp:33-48 — Begin/Fixpoint/Reloop/End suspended), which covers finalizeUnconditionally/visitWeak fire contexts. Resolution: the frozen predicate becomes `worldIsStopped(VM&)` = VMM Mode::Stopped OR worldIsStoppedForAllClients() OR legacy vm.heap.worldIsStopped(); the R1/JSThreadsSafepoint signature gains the VM& parameter; spec adds a debug assert that fires reached from GC finalization/sweep contexts see the predicate true; CS2's no-op-when-legacy note records that the legacy configuration never reaches STWR from GC contexts via this third disjunct.

### Rev 6 editorial note (size cap)

Rev 6 was re-compressed to <= 40000 bytes: §1's G-topic index reduced to pointers (history App. G stays AUTHORITATIVE/NBR; every relied-on G-fact is restated at its point of use in the body); prose arrows/operators tightened (`->`, `=>`, `=`, `+` unspaced); duplicate restatements removed where a fact is stated normatively elsewhere (F6/F7 now point at §5.8/§5.5; cadence/GCL facts consolidated under G13/§4.4/R1.i/CS2/CS4); verification tails of I1/I14/I20 point at Task 13/§5.5 instead of repeating them. Every layout, signature, frozen sequence, predicate, fence (F1-F7), lock-order row, invariant (I1-I20), interface (P1-P5, R1-R7), manifest entry (M1-M6, CS1-CS4), inventory row, and task (1-14 incl. 1b) present in rev 5 is present in rev 6; no normative content was dropped — content changed only where this §16 says so.

## §17 Round-3 adversarial review: dispositions (rev 6 -> rev 7)

Nine blocker/major findings. Verified against THREAD.md + tree on branch. Spec deltas: D9 added; D6/D8 rewritten; §5.5 AS-rule + elision rewritten; §5.4/§10 M4a split; §5.6/Task 1 interim witness; §7/P1 retire-lock contract aligned; R5 rewritten (ELF LLInt body + Darwin dynamic key); CS5/CS6 added.

1. **E3/E2 write-path elision removes the owner-TID check => undetectable foreign writes / lost-write races vs E4 (BLOCKER) — ACCEPTED (D9, CS5).** Rev 6 §5.5 ("E2 omit SW handling iff writeThreadLocal valid+watched; E3 with both: load+mask+access") let DFG/FTL compile property WRITES with no TID compare and no SW branch. Shared compiled code executes on every thread; a foreign thread B running that elided store (i) does not fire writeThreadLocal — OM F1 requires the FIRST foreign write to fire the set before its SW DCAS completes — and (ii) does not fault, because THREAD.md:15/D6 replaced the blog's subtract-constant+page-fault trap (which TRAPPED on a foreign tag) with an unconditional mask (which silently strips it). Meanwhile owner A's E4 license (both sets valid + runtime TID match) remains intact, so A may run a lock-free copying transition/resize (OM E4/T1: allocate, copy, swap); B's store lands in the superseded butterfly and vanishes — exactly the lost-write THREAD.md's object model forbids. Rev 6 had already applied this reasoning to TRANSITIONS ("watchpoint validity is NOT a substitute for the runtime TID check") but not to plain writes. Resolution: write fast paths ALWAYS retain predicate (2)'s fused TID+SW compare (`tagged & butterflyTagMask == g_jscButterflyTIDTag`) with slow-path fallback to `ensureSharedWriteBit` (fires F1 under per-event STW), in EVERY tier including E1+E2-elided DFG/FTL. Elision scope re-frozen: E1 (segmented-dispatch check) reads+writes; E2 elides ONLY the standalone SW branch (3) and the AS-rule SW test — sound because with writeThreadLocal valid SW is provably 0, and the never-elided compare (2) is the detection point for both first-foreign-write and owner-after-SW (the mask includes the SW bit, so SW=1 fails the owner compare); E3 full mask-only emission is READS-only. Residual write cost = one fused cmp/branch = THREAD.md:15's own stated residual budget, so this is a correction of the elision CLAIM, not a perf regression vs the design's accounting. Recorded as deviation D9. The identical hole exists in OM r8 E2/E3 ("Write fast paths may omit SW handling…", "E3 … access") vs OM F1; routed to the orchestrator as CS5 — jit emits soundly regardless of when OM's text is amended.

2. **R5 frozen TLS contract unimplementable for LLInt on ELF (MAJOR) — ACCEPTED.** Verified: offlineasm's only TLS instruction `tls_loadp` emits Darwin forms exclusively — `%gs:key*8` on x86-64 (offlineasm/x86.rb:1726-1744) and `mrs tpidrro_el0` on ARM64 (offlineasm/arm64.rb:1649-1669); no offlineasm instruction reads `%fs` or `tpidr_el0`, and offlineasm/ is outside jit's owned paths. The rev-6 "baked as immediate" sentence is only realizable by emitters that run after the offset exists (JIT tiers); LLInt asm is assembled at BUILD time. Resolution: R5's ELF bullet now scopes "baked as immediate" to JIT tiers and specifies the LLInt body explicitly: the owned `loadButterflyTIDTag` macro uses raw `emit` (offlineasm/instructions.rb:33) with LINK-time initial-exec TLS relocations against `g_jscButterflyTIDTag` — x86-64 `movq %fs:g_jscButterflyTIDTag@TPOFF, <reg>`; ARM64 `mrs xT, tpidr_el0` + `add xT, xT, :tprel_hi12:g_jscButterflyTIDTag, lsl #12` + `ldr xT, [xT, :tprel_lo12_nc:g_jscButterflyTIDTag]` — hard registers spelled per the documented offlineasm t-register mapping; everything stays inside `llint/**`. This preserves the one-load shape on x86-64 (3 instructions on ARM64, still one load) with zero manifest impact.

3. **§5.5 AS-rule asserted in-place shift/unshift, contradicting OM r8 AS-COPY (MAJOR) — ACCEPTED.** Rev 6's parenthetical ("innards mutate in place under the cell lock: shift/unshift") matched OM r7-era text but contradicts the controlling OM r8 §4.6 AS-COPY (SPEC-objectmodel.md:138): flag-on, AS relayout (shift/unshift JSArray.cpp:1650,1818; any vector move or indexBias/vectorLength change) allocates a FRESH AS butterfly under the cell lock and publishes via casButterfly; superseded storage is never written again; an installed AS butterfly's vectorLength is immutable. AS-COPY is load-bearing: it is what makes residual SW=0 unlocked compiled fast READS sound (OM manifest 8 says so explicitly). Resolution: §5.5 AS-rule rewritten around AS-COPY and cites it as the soundness basis; the in-place claim deleted.

4. **R5 Darwin KEY4 already taken by libpas (BLOCKER) — ACCEPTED.** Verified in tree: `Source/bmalloc/libpas/src/libpas/pas_thread_local_cache.h:87` defines `PAS_THREAD_LOCAL_KEY __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY4` — the live per-thread allocator-cache pointer THREAD.md names as the allocator fast path. Rev 6's audit covered only WTF FastTLS.h. Full key audit: KEY0 = bmalloc + WTF SequesteredImmortalHeap (`SequesteredImmortalHeap.h:448`); KEY1 = bmalloc; KEY2-3 = WTF FastTLS (`FastTLS.h:39-40`, with TSDTests exercising 0-3); KEY4 = libpas. ALL five reserved JSC keys are taken; no in-tree-verifiable free static key exists (a KEY5 is not defined in this tree's usage and squatting an unverified or foreign-framework key repeats the same bug). Resolution: Darwin switches to a `pthread_key_create`'d key at P5 process init — collision-free by construction; Darwin TSD slots are uniform, so `loadFromTLS64(fastTLSOffsetForKey(key))` (MacroAssemblerX86_64.h:7411 / ARM64.h:6399) is valid for dynamic keys and the JIT bakes the offset at emission time (key known long before any code is emitted). LLInt (build-time asm, key not constant) loads the key number from the new M4a `JSCConfig` slot `uint32_t butterflyTIDTagTLSKey` and uses the REGISTER-operand form of `tls_loadp` (x86.rb:1730 BaseIndex form; arm64 register variant) — two loads, Darwin LLInt only; JIT tiers stay one load everywhere.

5. **"AS-rule gates locking on SW=1, but OM requires any-SW locking" (BLOCKER) — PARTIALLY REFUTED; text reworked.** The race scenario offered (SW=0 unlocked read vs owner's cell-locked IN-PLACE shift/unshift memmove) presupposes in-place relayout — which OM r8 AS-COPY forbids (finding 3); under AS-COPY a stale unlocked reader sees a frozen superseded snapshot bounded by that butterfly's immutable vectorLength, kept alive by conservative scan (OM I7). The claim that OM mandates cell-locking ANY-SW for GENERATED code is contradicted by OM r8 manifest 8 (SPEC-objectmodel.md:366), which jit r6 adopted by request: "compiled AS fast paths E2-elided (set fire => jettison), SW-tested -> locked ops, or excluded. Residual SW=0 compiled fast READS made sound by §4.6 AS-COPY, not jit. No open JIT blocker." I31's any-SW locking governs runtime/interpreter slow paths (OM L5), which jit honors via the R3 `operationSharedArrayStorage*` shims locking at any SW. What WAS real: rev 6 never argued why residual SW=0 fast accesses (incl. the LLInt AS put_by_val fast path with its plain m_numValuesInVector RMW) are sound, and its in-place parenthetical actively undermined the argument. Rev 7 states the full basis: SW=0 READS, any thread — AS-COPY snapshots; SW=0 WRITES — owner-only (the never-elided predicate-(2) fused TID+SW compare admits only the owner pre-SW, so plain RMWs are single-writer; foreign compiled writes fail (2), AS makes (3) a locked path, so they land in (4) ensureSharedWriteBit), and the first foreign write/transition runs OM §4.6's per-event STW, which cannot interleave with an owner's compare->store window because that window is poll-free (I16) — the owner's store completes before the world stops, then SW=1 makes the owner compare fail forever after. E2-elided AS code never observes SW=1 (fire => synchronous §5.3 jettison under the same stop). I20 unchanged in substance (no unlocked access reachable by an SW=1 AS butterfly).

6+7. **R1.e gate byte inside INTEGRATION-DEFERRED M4 while Tasks 6/8/13 need it (MAJOR, filed twice) — ACCEPTED.** Real sequencing bug: §5.4's `ifJSThreadsBranch` is a `_g_config` byte-load, JSCConfig.h is non-touchable, M4 was wholly deferred, and unlike §5.2's M2a slip-hatch no interim existed; an owned-global substitute was also forbidden in practice because it changes the emitted gate shape that I1/Task 13 golden-diff. Resolution (option a): R1.e split out as **M4a, a PREP-PHASE precondition like M2a** — the one-byte `useJSThreads` gate + options-finalize store + (finding 4) the Darwin `butterflyTIDTagTLSKey` slot; tiny, inert by default, independent of the VMManager mechanics that justify deferring the rest of M4. Deps line now orders Tasks 6/8 after M4a and Task 13's golden baselines are taken with M4a in place. M4 proper remains INTEGRATION-DEFERRED with items exactly R1.a-d.

8. **§7 lock table lets retire be acquired under the cell lock, "contradicting heap" (MAJOR) — REFUTED as filed; jit's own stale text fixed.** The reviewer quoted heap as "retire: any thread, no rank >= 7 lock" and "leaf-only; never inside 7-10". The heap r6 text in this tree says the OPPOSITE for rank 10: §6 leaf row (SPEC-heap.md:136) — retire lock "below 10: takeable holding rank-10 cell/Structure locks, NEVER 7-9 (§13.10f)"; §9 contract notes (SPEC-heap.md:255) — "retire: any thread, may hold rank-10 cell/Structure locks, must NOT hold ranks 7-9". So the §7 diagram's nesting (retire under Structure/cell lock) is exactly heap's contract and stands. What WAS wrong: jit's own P1 and the §7 row parenthetical still said "no heap lock rank >= 3" — stale rev-5 text inconsistent with both the diagram and heap r6. Rev 7 aligns both to heap's wording (rank-10 holders OK; ranks 7-9 never; not signal-safe). One-line refutation recorded in N3 with `SPEC-heap.md:136,255`.

9. **Interim world-stopped predicates don't compose: OM's §10.6 stub stop invisible to jit worldIsStopped (MAJOR) — ACCEPTED.** Verified OM r8 manifest 6: pre-M4, ALL OM stop sites (§4.2-0, §4.6, §4.7, F1-F3) run through OM's owned `jsThreadsStopTheWorldAndRun`, which sets OM-owned `g_jsThreadsStubWorldStopped` and runs the closure inline; OM's own `butterflyWorldIsStopped` = stub flag || `JSThreadsSafepoint::worldIsStopped(vm)` — defined as a union precisely because the predicates differ. OM TTL fires land in jit's `fireAllSlow` interception, whose rev-6 worldIsStopped (VMM-stopped / WSAC / legacy-heap) saw none of OM's stub state: branch 1 missed => redundant nested jit-stub STWR inside OM's closure, and the frozen "TTL fires assert world-stopped (branch 1)" unsatisfiable for every OM-originated fire — the GIL-interleaved Task 13 / OM Task 12 stress suites would assert-fail pre-M4. Resolution: jit's interim worldIsStopped gains a fourth disjunct reading OM's exported stub witness (pre-M4 ONLY; deleted at the M4 swap); Task 1's stub returns true under it; CS6 records the alternative the orchestrator may prefer once Task 1 lands — OM's veneer delegating to `JSThreadsSafepoint::stopTheWorldAndRun` (one stub, one witness) — either way the integrate doc records the disjunct's deletion at M4.

### Rev 7 editorial note (size cap)

Rev 7 re-compressed to <= 40000 bytes: section-title qualifiers and restated rationale trimmed; N2/N3 adoption lists point here; the RetiredJITArtifacts class collapsed to an inline signature (unchanged); §7 diagram comments tightened; task-list items reduced to pointers where the cited section is the normative source; Darwin key-audit detail, the AS SW=0 soundness proof (finding 5), and the full refutation arguments live in this §17. Every layout, signature, frozen sequence/predicate, fence (F1-F7), lock-order row, invariant (I1-I20), interface (P1-P5, R1-R7), manifest entry (M1-M6 incl. new M4a, CS1-CS6), inventory row, and task (1-14 incl. 1b) is present in rev 7; normative content changed only where this §17 says so.

## §18 Round-4 adversarial review: dispositions (rev 7 -> rev 8)

All seven findings (three duplicates) verified REAL against the tree and on-disk siblings; no refutations this round. Spec edits are minimal-normative; this section carries the full arguments.

1. **Transition predicate omits OM E4's `!isPreciseAllocation(cell)` (BLOCKER, filed 3x) — ACCEPTED, ADOPTED.** Verified: SPEC-objectmodel.md is FROZEN rev 9; its E4 (":164") reads "... AND !isPreciseAllocation(cell)"; I36 (":246") forbids dcasHeaderAndButterfly and E4 on PA cells (8-mod-16 base per `PreciseAllocation.h:68-70`; 16B DCAS faults); ledger 8b (":361") records the open BLOCKER CR against jit r7 verbatim. Rev 7's "(= OM E4 EXACTLY)" parenthetical contradicted its own enumerated predicate — the enumeration is what implementers emit, so the contradiction was load-bearing. Resolution (rev 8 §5.5): the frozen Transition predicate gains the runtime PA exclusion as an explicit conjunct; Emission gains the one-instruction cell-base bit-test (`cell & 8`; MarkedBlock cells are 16B-aligned, PA cells 8-mod-16 — OM line-3 definitions) branching to the R3 slow path; an allowed alternative is compile-time speculation that the cell is MarkedBlock-allocated with slow-path/OSR fallback (e.g. when the IC already proves allocation provenance). Task 13's pre-integration lint list gains "every emitted lock-free transition carries the PA bit-test (or provenance proof)"; N4 records OM ledger 8b as RESOLVED-ADOPTED. Reachability note: JSGlobalObject and other oversize objects are PA — this was not a theoretical hole.

2. **R5 ELF JIT-tier TLS loads unemittable from owned paths (MAJOR) — ACCEPTED; owned paths extended.** Verified in tree: `X86Assembler.h:4115` defines only `gs()` (no `fs()` prefix anywhere); `ARM64Assembler.h:2688` encodes only `mrs_TPIDRRO_EL0` (Darwin read-only register; Linux needs TPIDR_EL0, no encoder); `loadFromTLS64` (`MacroAssemblerX86_64.h:7409`/`MacroAssemblerARM64.h:6392-6428`) sits under `ENABLE(FAST_TLS_JIT)` = `(CPU(X86_64)||CPU(ARM64)) && HAVE(FAST_TLS)` (`PlatformEnable.h:837-839`), and HAVE(FAST_TLS) is Darwin-only (`PlatformHave.h:265`). So rev 7's x86-64 `movq %fs:OFF, r` / ARM64 `mrs Xs, TPIDR_EL0` directives were unimplementable on Linux — Bun's primary deployment target — from `{jit,dfg,ftl,bytecode,llint}/**`. Resolution chosen: extend owned paths (NOT a manifest entry — these are real emitters the jit implementer writes and tests, not integrator-applied config diffs; no other workstream owns or touches assembler/**) to ADDITIVE-ONLY changes in exactly four files: `assembler/{X86Assembler.h,MacroAssemblerX86_64.h,ARM64Assembler.h,MacroAssemblerARM64.h}`. The additions, enumerated in R5: `X86Assembler::fs()` (mirror of `gs()`, 0x64 prefix) plus an fs-prefixed absolute-offset 64-bit load shape; `ARM64Assembler::mrs_TPIDR_EL0(RegisterID)` (encoding identical to `mrs_TPIDRRO_EL0` modulo the op1/CRm/op2 system-register field: TPIDR_EL0 = S3_3_C13_C0_2 vs TPIDRRO_EL0 = S3_3_C13_C0_3); both surfaced as `MacroAssembler{X86_64,ARM64}::loadFromELFTLS64(intptr_t offset, RegisterID dst)`, gated for OS(LINUX) ELF builds, emitted by Task 1b. Existing encodings/encodings tables untouched; golden-diff I1 unaffected (new functions, no edits).

3. **Write predicate (4) unsound for AS: foreign SW=0 write does ensureSharedWriteBit-then-unlocked-store (MAJOR) — ACCEPTED.** Verified against rev 7 text: an AS-shaped object with SW=0 written by a non-owner falls through (1) segmented-no, (2) TID-no, (3) SW=0-no, into (4) "ensureSharedWriteBit, then store" — an UNLOCKED generated ArrayStorage store at a moment when SW has just become 1, violating OM I31 / jit I20 and bypassing OM §4.6's per-event STW + locked-subsequent-access regime for first foreign AS writes. The AS-rule's own "SW=0 WRITES owner-only" claim was contradicted by (4). Resolution (rev 8 §5.5 Write): case (4) forks on AS-shape (the indexing byte is already loaded on generic paths per the generic-path rule; shape-specialized ICs know statically): AS -> tail-call the locked R3 operation (`operationSharedArrayStorage*`), whose contract is now explicitly "fires F1, flips SW, and performs the write ITSELF under the cell lock / per-event STW as OM §4.6 requires, returns done"; non-AS -> `ensureSharedWriteBit` then store (sound: predicate (3) shows non-AS SW=1 stores are legal unlocked). "ensureSharedWriteBit-then-store inline" is now expressly forbidden for AS shapes.

4. **Stale sibling pins + dangling heap line cites (MAJOR) — ACCEPTED.** Verified: on-disk siblings are OM rev 9 (claims supersession of r8 and instructs resolving against on-disk revs, ledger 8f), heap rev 8, vmstate rev 9, api rev 10; rev 7 pinned "OM r8, heap r6". The two `SPEC-heap.md:136,255` cites in N3 now land on unrelated lines (rank-10-retire content lives at heap :125 leaf-row and :239 contract notes). Resolution: line 3 re-pins to on-disk revs and states they are authoritative; N3's heap cites converted to section anchors (heap §6 leaf row / §9 contract notes) per the spec's own cross-spec-cites rule and heap manifest note 10e. OM r9 ledger items folded: 8b (finding 1), 8c — no TID recycling once tagging is compiled+flag-on: consistent with R5/I19 because rev 8's CS3 hook (finding 6) rewrites the tag on every `VMLite::setCurrent`, and vmstate §6.7 already forbids recycling a TID while any installed VMLite carries it; noted in N4.

5. **PREP-PHASE preconditions absent from tree; M2a env-var hatch faults under frozen Config (MAJOR) — ACCEPTED.** Verified: no `useJSThreads` anywhere under Source/ (OptionsList.h, Options.cpp, JSCConfig.h clean); prep artifacts that did land (JSTests/threads/bench) show prep ran without M1/M2a/M4a; and the rev-7 hatch rationale "`Options` are mutable globals" is wrong once config freezing is active — Options live in `g_jscConfig` (`JSCConfig.h:104` OptionsStorage) and `Config::permanentlyFreeze` (`WTFConfig.cpp:196-210`) mprotects the page read-only at finalize, well before FTL init; a late hatch write faults. Resolution (rev 8 §10 header + §5.2): (a) M1/M2a/M4a are declared ORCHESTRATOR-APPLIED to the shared tree BEFORE the implementation fan-out, and Task 1 gains a fail-fast precondition (a compile-time reference to `Options::useJSThreads()`; absence = stop and escalate, do not improvise a different load shape — M4a's golden-diff rationale); (b) explicit fallback: the jit workstream may carry exactly M1+M2a+M4a as a LOCAL patch until integration (the api spec's escape), never other manifest items; (c) the M2a hatch is respecified to run before `Config::finalize`/`permanentlyFreeze` or pair with `Config::disableFreezingForTesting()` (`WTFConfig.cpp:239`).

6. **CS3 ignores vmstate §6.7's `setVMLiteTIDTagHook`; tag goes stale on VMLite install/restore (BLOCKER) — ACCEPTED.** Verified: SPEC-vmstate rev 9 §6.7 (":545-557") deliberately avoids a runtime/->jit/ include by exporting `JS_EXPORT_PRIVATE void setVMLiteTIDTagHook(void(*)(uint16_t))` (null default, no-op for Phase-A standalone builds); `VMLite::setCurrent` calls the hook AFTER the TLS write with `lite ? lite->tid : 0`; vmstate's cross-WS manifest names "jit task 1b" as the registrant. Rev 7's CS3 instead demanded vmstate call P5 directly — a contract vmstate r9 explicitly declined — so following jit verbatim leaves the hook null and `g_jscButterflyTIDTag` stale across lazy embedder-thread installs, §6.4.4 multi-VM didAcquireLock/willReleaseLock switches, and detach (tag left at old TID while currentButterflyTID() changed => §5.5 predicate (2) owner-compare passes for objects the thread does not own; I19 debug assert in debug, silent lock-free foreign writes in release). Resolution (rev 8 CS3/P5/Task 1b): P5 init registers a `void(uint16_t tid)` body that stores `uint64_t(tid) << 48` into the per-platform R5 slot via `JSC::setVMLiteTIDTagHook` (registration guarded for builds where VMLite.h is absent); CS3 now names the hook as the vmstate-side mechanism; api §5.2's direct P5 spawn/detach calls remain as the idempotent belt-and-braces vmstate already endorses.

### Rev 8 editorial note (size cap)

To fund the rev-8 additions under the 40000-byte cap, non-normative rationale was moved here: D9's full unsoundness argument (already in §17), the call-link "stale dead callee can't fire" and cost remarks, the Darwin reserved-key audit detail (already in §17), the offlineasm `tls_loadp` platform-evidence parenthetical, and short "why" clauses in §7/§5.6/R1.i. No layout, signature, frozen sequence/predicate, fence, lock-order row, invariant, interface, manifest entry, or task was cut; §5.5's predicates CHANGED normatively only as §18.1/§18.3 describe.

## §19. Whole-design adversarial review round 1 dispositions (rev 8 -> rev 9)

1. BLOCKER ("parked mutators resume into jettisoned code; stop-delivery mode unspecified"): ACCEPTED, both halves. (a) Verified: invalidateLinkedCode patches only invalidation points; a mutator parked cooperatively at a trap-check poll inside DFG/FTL code resumes at that PC and runs the original stream until the NEXT invalidation point — if an E1/E2-elided butterfly access sits in that window, the fire that just ran under STWR is not yet observed (masked deref of a now-segmented word / unguarded SW=0 write). Today this cannot happen because watchpoints fire synchronously on the sole mutator; N mutators make every park site a potential invalidation site. Fix = new I21(b): flag-on, every DFG/FTL cooperative poll site is immediately followed by an invalidation point (CheckTraps becomes an invalidating node), so the conductor's in-stop patching is observed at resume; Task-13 lint extended from IC windows (I16) to poll->elided-access windows. (b) Trap delivery: with default signal-based VMTraps, CheckTraps emits no poll and VMTraps patches breakpoints into running JIT code from another thread — violating R1.f (cooperative-only) and I2. Fix = I21(a) + M2b: useJSThreads forces Options::usePollingTraps()=true at options-finalize; the async breakpoint/signal patching path is forbidden flag-on.

2. MAJOR ("every Class-A fire is a full STWR; safepoint storm; serializes against GC"): PARTIALLY ACCEPTED. Adopted: fire coalescing — R1.g's pending-job slot MAY be a queue; the winning conductor drains all queued Class-A fire closures inside ONE stop window before resume; losers' STWR returns once their closure has run (they were parked = stopped, satisfying the synchronous-completion requirement of §5.6; jettisons still happen inside the single stop, so "deferred jettison forbidden" is preserved — nothing is deferred past a resume). REFUSED: the >1-mutator inline-fire gate. A registered-client/active-VM count read outside the VMM lock races spawn/attach (the original G7 rationale); and when no OTHER mutator is registered, the STWR stop has no parking waits — its cost is lock acquisitions + the GCL bracket, which the flag-on single-threaded bench (Task 13) now measures rather than assumes. Warm-up fire frequency concerns are additionally bounded by the OM-8h acknowledgement (elision is thread-confined-structure-only; fires on shared-hot structures happen once per set and the sets are per-structure, not per-object).

3. MAJOR ("flag-on LLInt loses proto-load/transition/private-name caching wholesale; no flag-on bench"): PARTIALLY ACCEPTED. The §4.3 disables stand for this freeze: each disabled cache publishes multi-word state (mode byte + structureID + offset + holder) that cannot be made coherent with one aligned u64, and inventing a new pointer-published record form for LLInt asm now would reopen frozen offsets mid-fan-out. Adopted: (a) §4.3 charter — proto-load/transition caches MAY return post-GIL as single-pointer immutable records (the §5.8 CallLinkRecord pattern: publish one pointer to {structureID, offset, holder}; readers address-depend), unowned follow-up; (b) Task 13 gains a flag-on single-threaded bench gate (LLInt/startup-sensitive; budget set at INT) so the regression is measured before the freeze ships rather than discovered after.

4. Cross-cutting items consumed from siblings: heap r10 rank-10a/10b split (our §7 already nested cell < retire-leaf correctly; no jit change); api r11 N8 teardown reorder (no jit change); TID no-recycle rule (no jit change — the R5 tag is rewritten on every setCurrent via CS3 regardless); composed flag-off bar now defined in vmstate R3 (our I1 wording unchanged: it was already scoped "MODULO the unconditional repacks (D7)").

5. Byte-cap edits: N2-N4 round logs compressed to history pointers (full text remains §16-§18); R5/M2a/CS6/D9 citation trims. No normative content removed; I21 added; M2b extended; §5.6 coalescing added; §4.3 charter added.


## §20. Whole-design adversarial review round 2 — resolutions (rev 9 -> rev 10)

1. **Cap evasion via normative >40KB history (blocker, 2 filings) — ACCEPTED.** App. G/G6/5.6/R1 copied verbatim into `SPEC-jit-annex.md` (~12KB, under cap); spec §1/R1 cites repointed; history demoted to non-normative. Spec R5's per-platform TLS bullets also moved there (App. R5) to keep the spec under cap after the r10 additions.
2. **Phase-1 retired-artifact leak (major+major, 2 filings) — ACCEPTED, fixed on the heap side.** Heap r11 runs its §11 reclaim sequence (publish localEpoch -> bumpAndReclaim under the reclaimer's own compiler-thread suspension) at EVERY legacy `runEndPhase` too, so the phase-1 (1-client, legacy-GC, useJSThreads-on) configuration frees everything RetiredJITArtifacts retires. §4.4 cadence and CS4 updated (CS4 stays refused for JSThreads-stop bumps); Task 13's epoch test gains a retire->legacy-GC->free variant runnable PRE-integration.
3. **Watchpoint-fire STW storm (major) — ACCEPTED in part.** §5.6 coalescing upgraded MAY->REQUIRED (winner drains all queued fires in ONE stop); Task-13 flag-on bench now records fires/sec. The dominant fire source (OM F2 on mere shape reuse) is removed by OM r12's per-object E4/F2 keying, which jit §5.5's transition predicate adopts (butterfly-bearing: tag-vs-R5-tag compare only; butterfly-less: structure-TID compare unchanged). Inline-fire count gate stays refused (G7) — the attach race is unchanged.
4. **Flag-on single-thread tax unbounded (blocker, cross-cutting) — ADDRESSED via budgets** (the reviewers' stated alternative to lazy-enable): Task 13's "budget at INT" replaced by a NORMATIVE composite gate — flag-on 1-thread <=5% geomean vs flag-off, cross-spec; a miss promotes §4.3's LLInt proto/transition-cache revival (immutable single-ptr records) and heap's TLC-aware inline allocation emission from charter to REQUIRED pre-ship. Lazy-enable remains open as a charter option recorded in the INTEGRATE doc, not specced (most costs are publication-side but the migration STWs need their own design round).
5. **STWR closure heap-write/allocation contradiction (major, cross-cutting) — ACCEPTED.** R1.i now states: closures allocation-free (OM O4; pre-allocate first) and heap-metadata WRITES without access are sanctioned (heap §10A exemption).
6. Sibling pins refreshed (OM r12, heap r11, vmstate r11, api r12); N2-N5 line compressed; no other normative change.

## §21. Whole-design adversarial review round 3 — resolutions (rev 10 -> rev 11)

1. **No availability shim for heap-owned symbols (major) — ACCEPTED.** Verified: `heap/GCSafepointEpoch.h` and `Heap::JSThreadsStopScope` do not exist in the tree; both are heap-WS deliverables whose build wiring (Sources.txt) ships only via heap's manifest/overlay (heap §14 gating: overlay hunks never committed). vmstate (N7 macro guard) and OM (§9.1 `__has_include` shim) already solved this; jit now has the same pattern — new N6: (a) `RetiredJITArtifacts` bodies compile iff `__has_include("GCSafepointEpoch.h")`, else a no-op leak-until-INT stub (sound under the Task-1 GIL stub: retirement is never concurrent, and phase-1 legacy GC reclamation only matters once heap's site exists anyway); (b) R1.i's `JSThreadsStopScope` bracket is gated on heap's `JSC_HEAP_HAS_STW_FORBIDDEN_SCOPE` macro (defined in heap §9's Heap.h hunk) — note the Task-1 interim STWR stub never had the bracket, so this only affects the real body carried for INT; (c) Task 13's retire->legacy-GC->free epoch test is pre-INT only when the shim is live (heap landed first), else it moves to the integration gate; the heap-before-jit-Task-13 ordering is recorded in INTEGRATE-jit.md; (d) the §10 local-patch allowance gains M3 (build-only Sources.txt/CMake lines for jit's own new files — without which Tasks 1-13 cannot link at all; same class of escape as M1).
2. **Post-GIL safepointing is VM-granularity while the product is N threads in ONE VM (major, cross-cutting) — ACCEPTED as freeze-scope correction.** R1 is NOT final for the post-GIL product shape: VMManager arbitration counts VMs (VMManager.cpp:223-276/413-460), so with N threads entered in one VM a JSThreads conductor could proceed while sibling threads run elided code (voids I2/I8/OM I13). New R1 "Freeze scope" note: VM-counting arbitration is final only for the N-separate-VMs verification config (Task 13's true-concurrent tests run that config by design); thread-granular STW (VMM counts entered THREADS per VM, per-thread NVS tickets) is chartered in vmstate Dev 10 Phase B and listed in api §2; R1.c is re-frozen there. heap's GC stop barrier is NOT affected (per-client access state, heap Dev 5/§3.8 note) — that half of the finding is refuted on heap's side.
3. **Composite perf gate flag matrix undefined / excludes heap §5.5 (major) — ACCEPTED.** Task 13's composite gate is now a normative two-config matrix: {useJSThreads=1} AND {useJSThreads=1, useSharedGCHeap=1}; the latter exercises heap's never-populate allocation slow path, which was previously structurally invisible to the only gate chartered to catch it; miss in EITHER promotes §4.3 LLInt-cache revival+heap TLC-aware inline emission to REQUIRED pre-ship. The useJSThreads=>useSharedGCHeap coupling decision is owned by the orchestrator at GIL removal, recorded in INTEGRATE. heap §3.7 mirrors.
4. **Safepoint frequency unbounded under N threads (major) — ACCEPTED.** Integration gate gains an N-thread warmup stop-budget bench: stop count+total stopped-time ceiling recorded and signed off in INTEGRATE, including a shared-constructor N-thread construction microbench (also OM 8h's Task-14 trigger). Mitigations that bound it: OM r13 F4 chain-fire (one stop per transition chain) composing with §5.6's REQUIRED coalescing.
5. Cap compliance: rev-11 additions paid by compressing justification parentheticals whose full arguments live here or in App./annex (D7 rationale, D8 Darwin key note, D9 page-fault-trap clause, G13's N1 duplication, CS5 detail — all preserved in §13/§16-§19), and the §1 "(verified)" tag. No layout, protocol, invariant, lock-order, manifest, or task content dropped. Sibling pins refreshed (OM r13, heap r12, vmstate r12, api r13).

## §20. Round-4 COMPOSED-design review — rev 12 resolutions

### 20.1 Prep/bootstrap unification (CONFIRMED cross-spec finding)
Three incompatible OptionsList.h conventions existed (jit pre-apply+local-patch fallback; api
"keep local patch until INT" with its own colliding 9.2-1 text; heap overlay; OM/vmstate no
escape at all). §10 now records the single orchestrator decision: ALL FIVE specs'
OptionsList.h entries (api 9.2-1 canonical for useJSThreads; jit M1's other flags; vmstate
M_opts; heap manifest 2; OM entry 1)+M2a+M4a pre-applied to the shared tree BEFORE fan-out;
local OptionsList patches abolished everywhere; non-Options hunks needed for self-checks go
in heap-§14-style private overlay worktrees, never committed. vmstate M_opts2 (Options.cpp
implication hunk) stays an INT item — pre-applying it would be harmless (the implied flags
are inert until their consumers land) but is unnecessary.

### 20.2 Task-13 changes (findings on gate realism and config coverage)
(a) Composite budget aligned with heap deviation-7 r13 SPLIT: <=5% gated only in
{useJSThreads=1, useSharedGCHeap=0}; {1,1} measured+recorded (heap §5.5 slow-path cost),
budget set at GIL-removal chartering; {1,0} miss ⇒ §4.3 LLInt-cache revival required.
(b) Shared-constructor construction microbench MOVED from the INTEGRATION-GATE bucket to
PRE-integration: it runs against the Task-1 GIL stub with TTL sets force-fired, measuring
relative per-op cost of E4 vs post-F2 locked N2/§4.3+L6 — single-threaded relative
measurement needs no true concurrency, so OM Task-14 promotion is DECIDED before the five
workstreams integrate, not after M4/CS2.
(c) INTEGRATION-GATE bucket now states it validates the N-separate-VMs config ONLY; N threads
in ONE VM requires the Phase-B charter (R1 freeze scope, vmstate Dev 10) — a hard
GIL-removal precondition per api §2 — so a green gate cannot be misread as covering the
product config. R1.c's freeze scope already said this; the gate label closes the loop.

### 20.3 OM r14 L6 adoption
No jit emission change: IC fast paths bake target structures and never walk transition/
property tables; table walks happen in runtime slow paths (OM-owned). §5.5 predicates,
choke points, and E1-E3 unchanged. D8/App-R5 prose deduplicated (annex authoritative).

### 21. Section 5.8 data-IC direct-call register discipline (2026-06-10 review round; REAL BUG, fixed)

The flag-on FTL `compileDirectCallOrConstruct` adopted UseDataIC::Yes (Task 7)
on the upstream UseDataIC::No patchpoint shape. Upstream never exercises the
data-IC fast path from FTL direct calls, and the shape lacked two protections
the non-direct data-IC tail path (`compileTailCall`) already had:

1. No `clobberEarly(BaselineJITRegisters::Call::callLinkInfoGPR)` — B3 could
   assign the SomeRegister callee or a WarmAny tail argument to regT2, which
   `emitDirectTailCallFastPath` overwrites with the DirectCallLinkInfo*
   BEFORE the CallFrameShuffler consumes the argument recoveries. The callee
   then receives the boxed link-info pointer as an argument. Observed:
   GeneratorPrototype.js next() FTL-compiled passes the pointer as
   generatorResume's `state`; the generator body BadType-exits and baseline's
   switch_imm dispatches the garbage state to the ENTRY path — completed
   generators silently resurrect from the first yield (GIL-ON, single
   thread, wrong values, no crash).
2. No `shuffleData.registers[callLinkInfoGPR]` liveness recovery — the
   shuffler could clobber the CallLinkRecord* between the record load and the
   post-shuffle `farJump(Address(callLinkInfoGPR, offsetOfTarget()))`:
   the GIL-off "FTL-era wild jump into the JIT pool" signature previously
   misattributed to a §4.4 epoch race (it reproduces with zero threads).

Fix in FTLLowerDFGToB3.cpp mirrors compileTailCall: flag-on early clobber of
callLinkInfoGPR on direct-call patchpoints (tail and non-tail — the non-tail
slow path passes calleeGPR to operationLinkDirectCall after the same stomp)
plus the registers[] recovery on the tail shuffle. DFG was already safe
(GPRTemporary pins regT2; recovery present). Bisect note: the I21(b)
handleCheckTraps ExitOK+InvalidationPoint hunk was disabled and rebuilt to
test causality — NOT causal; it stays as landed. Regression pin:
JSTests/threads/jit/ftl-direct-tailcall-dataic-arg-clobber.js (tier-forced,
single-threaded). Rule reaffirmed for section 5.8: any data-IC fast path
that materializes a pointer into a convention register must declare that
register to BOTH the register allocator (early clobber) and any frame
shuffler that runs inside the fast path (liveness recovery).

## §22. B-relabelrace: deferred fireAllSlow claim CAS (§5.6 Deferral row amended)

Mechanism (discriminated by constructive storm + matched negative control):
the inline original-array `Structure::nonPropertyTransition` fast path
(StructureInlines.h, no `nonPropertyTransitionSlow` frame) fires
`didTransitionFromThisStructure(deferred)` with no m_lock, no allocation and
no safepoint. Two mutators relabeling DISTINCT arrays that share the SAME
original structure (e.g. ArrayWithInt32 with a DFG-fattened transition set)
both pass the relaxed `fireAll` precheck (Watchpoint.h) and reach the
deferred `fireAllSlow` unserialized: the loser lands inside the winner's
takeWatchpointsToFire + storeRelaxed(IsInvalidated) window. Debug: asserts at
the overload entry (`state() == IsWatched`) or inside
`takeWatchpointsToFire` (same race, loser caught pre- vs mid-transfer).
Release: a second take() against the already-claimed source — and a state
copy-back that can leave the loser's deferred set IsWatched/empty
(spurious second scope-exit fire). The slow transition path's incidental
serializers (m_lock/allocation) suppress the window, which is why every
slow-path storm was clean.

Resolution (root cause, not assert relocation): flag-on, the deferred
overload claims the source with a CAS `IsWatched` -> `IsInvalidated` BEFORE
the membership transfer. Exactly one racer wins and performs the transfer
(splice itself already serialized by take()'s membership lock); losers
return with their deferred set untouched (`ClearWatchpoint`, so no
scope-exit fire) — benign because the winner's deferred fire invalidates
everything the loser would have. Because claim precedes splice, `take()`
flag-on installs `IsWatched` (the source's pre-claim state) into the
deferred set instead of copying the now-IsInvalidated source state, and
`takeWatchpointsToFire`'s entry assert is re-scoped to the protocol's exact
invariant per mode (flag-on: source IsInvalidated-by-claimant; flag-off:
IsWatched). Assert protective power is preserved: each arm still asserts
the single state its protocol permits. Flag-off sequence and codegen
path unchanged (assert + take + flip, same order, same stores). This also
makes `firePropertyReplacementWatchpointSet`'s documented reliance on an
"internal IsWatched re-check" in the deferred path actually true.

Audit amendment (same change, B-relabelrace round 2): the flag-on loser arm
itself asserts the one state its protocol permits — the failed CAS captures
the prior state and `ASSERT(prior == IsInvalidated)` before returning.
States are monotonic, so the legitimate lost race can never trip it; a
`ClearWatchpoint` entry (a future direct caller bypassing the `IsWatched`
precheck) is trapped exactly as the pre-claim `ASSERT(state() == IsWatched)`
trapped it pre-amendment. No behavior change in release; flag-off untouched.

## §24. Transition emission (§5.5 Transition row rewritten; fifth landing round)

Until now §5.5's Transition row was a predicate no tier emitted: `tryCachePutBy`
returned GiveUpOnCache for every NewProperty put flag-on, the IC compiler's
Transition case asserted unreachable, and the DFG parser demoted every
Transition variant to a generic PutById — so each property add ran
`putDirectInternal` in C++ (the 40x-1400x object-creation costs recorded in
LANDING-PLAN "Results, fourth round"). The row is now the emitted contract.

What is emitted, and why each clause: (1) compile-time, both source TTL sets
valid, and the emitter WATCHES four sets (source and target, transition- and
write-thread-local): E4/I15 need the source pair; the target pair is F2's "fire
BOTH sets on S and target" mirrored on the consumer side so a fire that names
only the target still retires the emitter (conservative; costs two watchpoint
registrations). (2) Source not ArrayStorage-shaped: OM I31, every AS relayout is
cell-locked and E4 excludes AS in C++ (`mayTransitionLockFreeFromThisStructure`).
(3) Non-reallocating only: a reallocating transition installs a NEW butterfly,
which flag-on is a tagged-word publication with its own protocols (N3 first
install: nuke-CAS + casButterfly; E4 growth: T1 CAS shape); those stay R3 in
this revision. (4) Runtime PA exclusion and ONE tagged-word load feeding both
legs, no second load (a second load could pair a fresh tag with a stale
decision). (5) Butterfly-bearing leg = E4's instance-tag compare and today's
store sequence (the owner of a (currentTID,0) instance is the only lock-free
transitioner; every foreign participant fires F1/F2 in a stop first, and the
window has no poll). (6) Butterfly-less leg = OM N2-LF, never a plain store:
see OM history §23 for the participant table; the compare against
`S->transitionThreadLocalTID()` is an immediate because the stub is specialized
on S. (7) DFG: the runtime legs become an exiting check node so that a foreign
thread running shared optimized code exits (and, via the baseline slow path,
fires F2, after which recompilation sees invalid sets and plants no
transition); `PutStructure` carries the N2-LF claim for the word==0 case
because the DFG keeps value store and structure store as two nodes — sound
because both sit in one poll-free region after the check and the participant
table excludes every concurrent lane writer while the watched sets are valid;
non-escaped local allocations need nothing (no other thread can name the
object before it escapes, and escape materializes the final structure).

Flag-off: nothing changes (the flag-off Transition case is byte-identical; the
new DFG node is never planted).

## §25. CheckTraps GIL-on model = flag-off; CheckTraps cloneable (I21 amended; fifth landing round)

Measured: flag-on GIL-on, a 3e8-iteration integer loop took 1.7x flag-off and
a closure-call loop 2.6x the instructions. Cause: the GIL-on arm of
`CheckTraps` in DFGClobberize/AI was `read(World); write(Heap)` /
`clobberWorld()` ("trivially correct" placeholder from the GIL-off work), so
every loop poll killed constants (callee/scope folding, closure-variable
constants), LICM and B3 loop opts; and `CheckTraps` was not cloneable, so
DFG loop unrolling refused every loop flag-on (`isNodeCloneable`). Neither is
needed GIL-on: the phase-1 GIL is cooperative and handed off only inside the
blocking primitives (LockObject/ConditionObject/ThreadObject parks,
`jsThreadGILHandoffYield`), never from `VMTraps::handleTraps`; a thread at a
poll therefore excludes every other mutator of its VM exactly as a flag-off
thread does, and the flag-off model (InternalState only) is exact. The GIL-off
arm is unchanged (invalidation point + precise jettison + the poll-bounded
plain-data clobbers of AUDIT-checktraps §7.1). Cloning: `CheckTraps` has no
children and no per-node data; a clone is another poll (GIL-off: with its own
invalidation point at codegen), which only adds safepoints.

## §26. Profiled allocation GIL off: TLC-slot allocators, and the pair check (fifth landing round)

Before: GIL off, `ObjectAllocationProfile::initializeProfile` left the
profile's `Allocator` null (a raw `LocalAllocator*` is per client, and the
profile is per structure/function, shared), so `op_new_object`,
`op_create_this` and DFG `CreateThis` took the C++ slow path for every object.
Change: the profile stores `Allocator::encodedTLCSlot(slot)` (low bit set,
slot = `tlcIndexBase + sizeClassIndex`, thread-independent) and the emitters
resolve it through the current thread's lite TLC table
(`emitResolveProfiledAllocator`, FTL `resolveProfiledAllocator`); an
out-of-range slot resolves to null and the existing null test takes the slow
path. Measured: GIL-off constructor loop 490 -> 298 ms at the time.

Consequence found by the amplifier: the `create_this` fast paths read the
profile as two words {allocator, structure}. GIL off another thread can
`clear()` (a `.prototype` store) or refill the profile between the loads; with
a non-null GIL-off allocator the torn pair — old allocator, null structure;
or an allocator sized for a different structure after a refill — reached the
inline allocator (SIGSEGV; a size mismatch would have been silent
corruption). The old comment's claim that a torn pair "just takes the slow
path" was true only of the allocator. Rule (all three tiers, flag-on): read
structure, allocator, structure; slow path unless the two structure reads are
equal and non-null. Writers: `initializeProfile` stores allocator then
structure (store-store fence, as landed); `clear()` now stores structure
(null) then allocator. Under those orders, two equal non-null structure reads
bracket an allocator that belongs to that structure (a refill to a different
structure between them changes the second read; a refill to the same
structure has the same size class). x86: loads are ordered; elsewhere the
emitted `loadFence`s. `op_new_object`'s metadata profile is written once
before the CodeBlock is published and is not subject to this.
`jit/create-this-profile-torn-pair.js`.

## §27. IC property conditions can go stale before generation (fifth landing round)

`PolymorphicAccess` generates a case only after `couldStillSucceed()` has
re-validated its `ObjectPropertyConditionSet`, and the compiler
release-asserted during generation that each non-watchable condition's
structure still ensures it ("This condition is no longer met"). The invariant
is single-threaded: with JS threads another thread can transition the
condition's object — a shared prototype — between the check and generation
(mirror harness, `primitive-poly-proto.js`; `jit/ic-condition-stale-at-generation.js`
reproduces it in 5 of 10 runs). Flag-on rule: a stale condition makes the case
fail softly. `collectConditions` returns false (also for an equivalence
condition that stopped being watchable), `generateWithConditionChecks` emits
an always-miss arm (`m_failAndIgnore`), the handler compiler returns
`GaveUp`; the next repatch derives a fresh case. Nothing already emitted is
wrong: the arm is unreachable and the stub's other cases are unaffected.
Flag-off the assertion stands.

Addendum (measurement). The FTL arm of the pair check first used full fences
(`fence(root, root)`, a locked op on x86) between the three loads; on
`class-ctor-4` that cost 22 ms of 108 GIL on. They are load-load fences now
(B3 `Fence` with a write range and no read range → Air `LoadFence`: nothing
on x86, `dmb ishld` on ARM64), which is all the check needs: the second
structure load must not be folded into the first nor hoisted above the
allocator load. Baseline and DFG used `loadFence()` from the start.


## §28. Global-property scope caching flag-on (fifth landing round)

Review round 1 froze `op_get_from_scope`/`op_put_to_scope` metadata after
linking flag-on: `tryCacheGetFromScopeGlobal` rewrites `{getPutInfo,
structureID, operand}` under `CodeBlock::m_lock`, and the LLInt/Baseline fast
paths read them with no lock, possibly on another thread. The price was every
non-`var` global read — `Math`, `JSON`, `Object`, any property of the global
object — on the slow path in the LLInt and Baseline and, because the DFG takes
its structure from the same metadata, a generic `GetByIdFlush` in the DFG/FTL
too: `Math.sqrt(i)` in a loop 14x flag-off, a global property read 50x, the
scaling suite's ray tracer 2.8x, its Richards 2.7x.

Rule now (x86-64; other targets keep the freeze until their fast paths order
the two loads): the GlobalProperty fill and re-fill publish `operand`, a
store-store fence, then `structureID`. The reader loads `structureID`,
compares it with the scope's, then loads `operand`, then the butterfly. (i) A
reader that sees the new `structureID` sees the operand stored before it. (ii)
A reader that pairs an older matching `structureID` with a newer operand
reads the property's current slot: the writer resolved the operand against
the structure the global object already had, whose value and butterfly were
published before its StructureID (M5), all before the operand store; TSO
makes them visible to the reader before the operand is. A genuinely stale
pair reads a slot that offset quarantine (SPEC-objectmodel §6) keeps from
being reused before a stop. (iii) The two rewrites that change the operand's
meaning together with the resolve type — GlobalProperty to GlobalLexicalVar,
and the put side, whose fast path would also need the butterfly write
predicate — stay frozen flag-on; those sites keep the slow path. The DFG reads
the pair under `m_lock` as before and inlines the load behind a structure
check. `jit/global-property-cache-vs-global-transitions.js` races four
readers (own and shared metadata) against a main thread that keeps
transitioning the global object.

## §29. Int32-mode reads verify the lane GIL off (§5.5 Read row; sixth landing round)

Companion of SPEC-objectmodel rev 17 (history §25 there). Once the owner of
an array may relabel Int32->Contiguous without a stop GIL off, a reader on
another thread whose Int32 shape check is stale (the check is a separate,
earlier load, and the DFG may hoist it across polls) can load a lane that now
holds a non-Int32 JSValue. Flag-off the DFG and FTL type an Int32-mode
`GetByVal`/`ArrayPop` result Int32 with no check (`DataFormatJSInt32`,
`SpecInt32Only`); a cell there would surface its low 32 bits as an integer.
GIL off those two nodes test the loaded value and OSR-exit (BadType) when it
is not an Int32; the abstract type is then true by construction. The inline
`ArraySlice` copies lanes under the same possibly-stale mode, so GIL off an
Int32-mode slice allocates its result with the Contiguous structure (a
mislabelled Int32 result holding a cell would be a marking hole). LLInt and
Baseline indexed loads already return the lane as an untyped JSValue and
need nothing; every conversion itself runs in C++ in all tiers
(`operationEnsure*`, the put-by-val slow paths), where the owner leg lives.
GIL on nothing is emitted: no other mutator of the VM can be between its
check and its load (I21, GIL on). Cost: one compare and branch per Int32-mode
element load in optimized code, GIL off only.

## §30. Megamorphic cache GIL on; claimed transitions in the inline caches (sixth landing round)

Two changes with one mechanism behind them (SPEC-objectmodel history §26).

Megamorphic cache. The VM-global `MegamorphicCache` was inert flag-on: probes
bailed, fills no-op'd (§5.5 Task 8 inventory). The reason is GIL-off only — a
fill is a multi-word entry write with a `RefPtr` uid that N unsynchronized
mutators cannot share. GIL on, one mutator of the VM runs at a time and the
GIL is handed off only inside blocking calls, never inside a probe (straight-
line JIT code) or a fill (straight-line C++), so the cache is exactly as
consistent as flag-off. Rule now: the cache is disabled per PROCESS mode
(`useJSThreads && gilOffProcess`), not per flag. What the probes had to learn
flag-on is the tagged butterfly: loads go through `loadPropertyTagged` (mask;
segmented or shared-written words to the generic path — no register is free
there for the ArrayStorage shape test, the same conservative choice
`loadButterflyForRead` makes without a scratch), replaces through
`storePropertyTagged` (the §5.5 write predicate: owner, or SW=1 on a
non-ArrayStorage shape), and the store cache's TRANSITION arm is the claimed
non-reallocating sequence of the Transition row with runtime refusals for
PreciseAllocation, copy-on-write and ArrayStorage instances (the probe cannot
know the structure at generation time, and needs no watchpoints because it
claims). The reallocating arm's C++ operation completes the transition through
the object-model protocols (`tryCompleteCachedTransitionConcurrent`) instead of
the flag-off nuke-and-set. `Structure::forEachProperty` (the JSON fast
stringifier's walk, `Object.assign` and friends) takes the flag-off lock-free
walk GIL on for the same reason; GIL off it keeps the locked snapshot.
Measured GIL on: the megamorphic-load micro row 63 -> 11 ms (flag-off 10.5),
`JSON.stringify` 36 -> 27 (25), JetStream `Air` 0.67 -> 0.88 of flag-off
before the transition arm; the remaining tables are in PERF-RESULTS. GIL off
is unchanged (a per-thread cache with a global epoch is the recorded design:
32-bit entry epochs so a wrapped epoch cannot validate a stale entry, fills
tagged with the epoch read BEFORE the lookup so a concurrent prototype
mutation cannot produce a persistently stale miss, `age()` over every
thread's cache at collection end).

Claimed transitions. The inline caches' butterfly-bearing transition leg
stored value then StructureID with no claim, so the stub had to watch the
source's and target's thread-local sets and died with them; after the first
cross-thread transition of any object of a shape, no thread cached that
transition again. The leg now claims the lane first (as the butterfly-less
leg has since r13), the four watchpoints are gone, and `tryCachePutBy`
accepts fired sources. Sound because every foreign writer of the lane claims
it first too (OM §4.3 step order, N2 (ii), r17) and restarts when it loses;
the marker treats the nuked lane as a race; the word of an owner-tagged SW=0
instance cannot move while the lane is held, so the leg re-derives the
butterfly after the claim (the two scratch registers were needed for the
CAS). Cost: one `lock cmpxchg` per cached out-of-line-within-capacity add in
IC code; DFG/FTL-inlined adds under watched sets are unchanged.

## §31. (Re)allocating transitions in every tier; the out-of-line Replace fast path (sixth landing round)

Profiles of the JetStream tests still at 0.5-0.7 of flag-off GIL on
(typescript, ai-astar, Babylon) were dominated not by the megamorphic cache
(§30) but by two ordinary things the flag-on JITs refused.

(1) (Re)allocating transitions - the add that grows or first installs the
out-of-line storage - were "R3 until a tagged-butterfly install is
specified": `tryCachePutBy` gave up on them, the DFG/FTL parser sent them to
a generic `PutById`, so every such add ran `operationPutById*` -> the C++ E4
leg (typescript: `operationPutByIdSloppyGaveUp` + `tryPutDirectTransition
Concurrent` + `createOrGrowPropertyStorage` at the top of the profile). The
install is now specified in both places (§5.5 Transition row, (RE)ALLOCATING
form). In the inline caches it is claim-first like the non-reallocating legs
and needs no watchpoints; the one subtlety is the copy: the grown storage is
filled from the old one BEFORE the claim, and a foreign first write that
flipped SW between the owner test and the claim could store into the old
storage after the copy - so the word is re-checked under the claim and a
moved word un-claims and defers to the operation. In the DFG/FTL it is E4's
plain publication under the four watched sets, made sound across the
sequence's park sites (the allocation; the materialization of a sunk object
stored as the value) by an `InvalidationPoint` planted after the value store
and immediately before the install (a fire while parked retires the code
there; after it nothing polls, allocates or exits until `PutStructure`, so
neither a foreign protocol nor a collection can meet the nuked header - the
first cut of this had the install before the value store and a GIL-on
`ftl-eager` stress test's heap verifier found a materialization's collection
looking at a nuked object). A `PutByOffset` whose storage child is the
transition's own allocation node stores through that child, skips the flag-on
re-load + predicate, never exits and does not clobber exit state - it is this
thread's unpublished storage for an object it owns. GIL off the FTL `MultiPutByOffset` keeps refusing
reallocating variants (no InvalidationPoint inside a node) and the inline
caches allocate through the thread's TLC slot (the server allocator is null
under the shared heap, which made the handler's inline allocation always
fail). Measured GIL on: the ai-astar-shaped micro (construct, six adds of
which two allocate, then replaces) 7.0x -> 1.2x of flag-off; typescript 0.46
-> 0.88, ai-astar 0.49 -> 0.64 -> (with (2)) see PERF-RESULTS §3.

(2) Out-of-line Replace never cached in baseline code. The flag-on `put_by_id`
call-site fast path stored inline offsets only ("no register pair for the
write predicate") and sent out-of-line ones to the handler chain; but a
monomorphic Replace handler is installed as the site's INLINED handler
(`setInlinedHandler`), not into the chain - so the chain held only the
slow-path handler, every out-of-line replace called `operationPutByIdOptimize`,
created a Replace case, had it refused as a duplicate of the inlined one
(`MadeNoChanges`), and the site never settled (10x on a loop of out-of-line
replaces to the thread's own objects, in every mode; present since round 2).
Two fixes: the call-site fast path handles out-of-line offsets where the
owner test needs no scratch register (x86-64: `xor %fs:tag, word`; §4.2
bullet), and where it cannot, an out-of-line Replace goes into the chain
instead of the inlined slot. Test `jit/put-by-id-replace-out-of-line-
cached.js` (FTL off so the two-shape site stays on the caches: 7.9x -> 1.0x).

Also this round: `loadPropertyTagged` (the megamorphic load probe's tagged
read, §30) wrote the loaded word into its result register before the
slow-case branch; in the data-IC handlers the result register IS
`handlerGPR`, which the fall-through path dereferences to find the next
handler - a shared-written or segmented base crashed there (four corpus tests
GIL on). The probe now stages the word in the dead entry register.

## §32. Virtual calls GIL off: `installCode` publishes the arity mirror (§5.8; seventh landing round)

Counted in the seventh round's cost ledger (PERF-RESULTS §6): GIL off,
`operationVirtualCall` ran 31 M times in gbemu, 15 M in WSL, 6.7 M in
typescript, 2.4 M in Basic, 272 K in Air - against 0-2,600 GIL on - and its
C++ path (`virtualForWithFunction`, `sanitizeStackForVM`, the entry-token
check, `addressForCall`) was 550 ms of gbemu's 1,400 ms GIL-off deficit.
Cause: the sixth round's answer to the torn (entry, CodeBlock) pair - keep the
executable's arity-check mirror null GIL off so the thunk's fast path never
engages for script functions - is sound but turns every virtual call into a
C++ round trip. What made the mirror untrustworthy was not that it lives in
the executable but WHO wrote it: `entrypointFor`'s lazy refill, a reader-side
write that could land after a later install had retracted the slot. With the
refill gone, `installCode` can publish the mirror itself as the last store of
an install (retract first, CodeBlock slot, fence, mirror), and the thunks'
existing read-mirror / read-CodeBlock / re-read-mirror sequence becomes a
sound pairing GIL off: retirements of code are world-stopped and cannot
interleave a poll-free thunk, tier-up installs bracket their CodeBlock store
with null-then-new mirror stores, so equal non-null mirror reads around the
CodeBlock load exclude a completed install in between. A first version of
this change instead added the entry to the CodeBlock (over its size cap) and
then to the `JITCode` base object (a fourth dependent load into a cold
object: the thunk itself went from 64 to 195 ms of gbemu's samples); the
mirror form touches the same two objects GIL on does. Test:
`jit/virtual-call-fast-path-gil-off.js` (a call site over 64 distinct
function executables, so it settles virtual; counts `operationVirtualCall`
through the diagnostic counters and times the loop against a monomorphic
twin, main and spawned thread): 2.2 M slow calls and 16x before, under a
hundred and 3x after (GIL on: 3x).

## §33. Dictionary flattening requested from under the IC lock, run after it (seventh landing round; WITHDRAWN in the same round, see the end of this section)

GIL off an inline-cache path never flattens a dictionary: flattening is a
stop-the-world (SPEC-objectmodel F3) and the path runs under the CodeBlock's
lock with heap access held, so a stop requested there would wedge the
conductor's quiescence predicate (fifth round, O2/GT11). The fifth round's
rule "report the chain uncacheable instead" turned out to be the largest
single GIL-off cost of the ML benchmark: `prepareChainForCaching` met an
unflattened dictionary PROTOTYPE (a class prototype that had left the
transition chain), returned "uncacheable", the site was repatched to the
GaveUp operation, and every later access through that prototype - 15 M loads
and 9 M stores per run - took the generic C++ path (GIL on flattens the
prototype once, at the first IC attempt, and caches). Now the IC path records
the object in a `DeferredDictionaryFlattenScope` that its lock-free caller
(`repatchGetBy` / `PutBy` / `InBy` / `DeleteBy` / `InstanceOf` / the private-brand
entries) opened before calling it, returns retry-later instead of give-up, and
the scope's close - after `tryCache*` has released the lock, still inside the
operation, the object alive on the caller's stack - flattens through the
existing stop protocol; the site caches on its next execution. One stop per
dictionary object ever (`hasBeenFlattenedBefore`). ML GIL off: 24.4 M GaveUp
calls -> 17 K, score 0.49 -> 0.66 of GIL on. Test:
`jit/dictionary-prototype-flatten-gil-off.js` (a live prototype grown past
`s_maxTransitionLength`, two loads through it per call, 2 M calls; generic
loads counted through the diagnostic counters and the loop timed against an
own-property twin, main and spawned thread): 4 M generic loads and 12x
before, 0 and 1.0x after (GIL on: 0, 1.0x).

WITHDRAWN before the round closed. With the deferred flatten in place the
amplifier (random yields at polls, parks and lock sites) crashed about 1 run
in 50 of `jit/ic-condition-stale-at-generation.js` and 1 in 100 of
`jit/global-property-cache-vs-global-transitions.js` GIL off - one thread
generating inline caches across a prototype chain while another reshapes the
prototypes: a stub read an out-of-line slot through a null butterfly, and a
DFG plan's adaptive watchpoint install found no replacement set at its offset.
Bisection over the round's binaries puts both on this change (0 in 150 on the
binary before it, 3-4 in 150 with it, 0 in 150 with it reverted). The flatten
itself runs world-stopped (Structure::flattenDictionaryStructureUnderStop),
but it keeps the StructureID while renumbering offsets and possibly dropping
the butterfly, so whatever a second thread derived from the pre-flatten layout
of that prototype - a case being generated, a plan being finalized - survives
the stop; flag-off and GIL on never flatten a structure another thread is
mid-way through caching. The refusal of the sixth round is restored (GIL off
never flattens from the IC path; such chains stay generic) and the JetStream
cost with it (`Air`'s 401 k generic loads per run); the diagnostic counter name
`icFlattenSkippedGILOff` stays declared but has no site. A sound version needs the flatten to invalidate like a
transition (fire the structure's transition set inside the stop, or give the
flattened structure a new ID); recorded in LANDING-PLAN Open items. The
install-side null check that the second signature led to
(AdaptiveInferredPropertyValueWatchpointBase::install refusing when the
structure it re-reads has no set at the offset) is kept: it is the documented
flag-on contract of that function (a refused install returns false), and
costs nothing.

## §34. GIL off: the DFG stops minting Double arrays that are converted on arrival (seventh landing round; OM history §27)

Two DFG-side halves of SPEC-objectmodel r18. (1) `ArraySlice` (the inlined
`Array.prototype.slice`) copies lanes word for word into a result labelled
like the source; GIL off a copy of a Double source must be a boxed Contiguous
array (OM T4-C), which that loop cannot produce, so the intrinsic is not
planted on a Double-typed site (the call reaches `JSArray::fastSlice`, which
boxes) and its structure check omits the two Double array structures, so a
Double array at an Int32/Contiguous-typed site exits `BadCache` and the
existing exit-site rule compiles the site as a call next time. (2) `NewArray`
and `NewArrayWithSize` compiled from an allocation profile that recommends
Double GIL off store their result into the profile's last-array word (five
instructions after the inline allocation; the FTL emits a load, mask, or,
store on the absolute address) and the compilation watches the profile's
demotion set (OM T4-P); when the profile sees those arrays converted it fires
the set and the code is jettisoned and recompiled with the new
recommendation. Flag-off and GIL-on: no store, no watchpoint, Double admitted
as before.

## §35. Map/Set `get`/`has` inlined GIL off, validated against the table's seqlock (seventh landing round)

Since the fifth round the DFG refused every hash-table intrinsic GIL off: the
inline `MapGet` probe walks the table with no lock and hands its consumer a
raw slot pointer, and neither survives a concurrent writer. So `map.get(k)`
compiled to a generic call into `mapProtoFuncGet` and the runtime's
validated lock-free reader - measured at 23 M calls per run of JetStream's
Basic (350 ms of its 1.0 s GIL-off deficit), 8 M in WSL. The runtime reader
(SPEC-ungil §N.1, sixth round) already had the right shape: the owner's
version word is odd while a writer is inside, and a reader that sees the same
even version before and after its walk read one consistent state of one
table. The seventh round gives the FTL's inline probe the same discipline and
admits `Map.prototype.get`/`has` and `Set.prototype.has` GIL off (the
mutating and iterating intrinsics stay calls):

- FTL `MapGet` GIL off: load the owner's version (acquire; odd -> slow path),
  then the storage; bound every index it follows by the storage cell's
  immutable vector length (the bucket index from a possibly scribbled
  capacity, each entry index with room for its value and chain slots) and the
  number of links followed by that length; compare keys as before (slots only
  ever hold whole JSValues, so a mis-walked slot is still a genuine value);
  once the answer is formed - found with its slot, or not found - re-read the
  version behind a load-load fence and take the slow path if it moved. The
  slow path (`operationMapGet`/`operationSetGet`) GIL off returns the slot
  from `getKeySlotGILOff`: the runtime's validated walk (which now records
  the slot it found) or the table lock.
- The slot survives to `LoadMapValue`: after validation the entry's value slot
  can only be overwritten whole by a `set` of the same key (old or new value),
  turned into the deleted sentinel by a `delete` (GIL off `LoadMapValue` reads
  the sentinel or an empty slot as `undefined`), or left untouched in a table a
  rehash retired (the value as of the rehash); each is a linearizable answer,
  and the storage cell stays alive while the interior pointer is held
  (conservative root). `IsEmptyStorage` (`has`) needs nothing more.
- DFG tier GIL off: `MapGet` calls the operation directly (no JS call, no
  callee or arity checks); the inline probe is FTL-only.
- Flag-off / GIL on: unchanged code.

Measured: Basic GIL off 467 -> 521 (GIL on 881); WSL, Air, OfflineAssembler
within noise (their tables are hit through `set`, iteration and `size`, still
calls). Test: `jit/map-get-has-inlined-gil-off.js` - FTL readers of a shared
Map and Set race a writer that inserts, overwrites, deletes, clears and forces
rehashes both ways, and every `get` returns a value that key could have held
or `undefined`, never a sentinel, an index or another key's value; and a
single-threaded FTL `get` loop no longer reaches the runtime reader (its
counter: one per call before, a handful after).

Follow-up in the same round (two defects in the first form, both GIL off
only). (a) The three fences were emitted as B3 fences that READ the heap and
write nothing - B3's store-store fence - so B3 was free to hoist the table
loads above the first version load and, worse, to fold the version re-load
into the first load (common-subexpression elimination across an effect that
writes nothing): the compiled probe validated nothing. Confirmed in the B3
dump (no second load of the version word) and fixed by emitting them as
fences that WRITE the heap (B3's load-load fence, no instruction on x86-64).
(b) A key slot can hold the EMPTY value while a writer is inside (an add
reserves the entry before the key store lands, a delete/clear/rehash passes
through it), and a torn walk can index a value or chain slot; empty passes
`isCell()` and the string/BigInt type checks then loaded through a null cell
(SIGSEGV in about 1 run in 15 of `shared-objects/map-lock-free-readers.js`).
The probe now sends an empty entry key to the runtime reader before any type
check. The version re-check alone cannot cover this: it runs after the answer
is formed, and the type check faults before it.

## §36. A Class-A set nobody watches fires without a stop (seventh landing round; §5.6)

§5.6 sends every fire of a code-invalidating (Class-A) watchpoint set through
a stop-the-world, because firing runs the members' callbacks - jettisons,
stub clearing, adaptive re-installs - which patch code other threads may be
running. The classification is static, set at construction; whether anyone
is watching is not. Many Class-A sets are armed `IsWatched` with no member at
all: the property-replacement sets that property ICs and the scope caches arm
on every structure they cache (so that a later compile MAY constant-fold the
property), an inflated set whose watchpoints were all removed. Firing such a
set changes one byte and patches nothing, yet it stopped the world: the
scaling suite's string-heavy workload at four threads took 376 such stops per
run ("Property did get replaced"), each parking three threads and, through the
heap-fact epoch, jettisoning their optimized code on resume; JetStream's
typescript GIL on requested 3,460 stops per run, Babylon 226, nearly all of
this kind.

Rule (r15): in `WatchpointSet::fireAllSlow`, a Class-A fire whose set has no
members takes the membership lock, re-checks emptiness and `IsWatched` under
it, and stores `IsInvalidated` (fenced as `fireAllNow` fences) without a stop.
Atomicity with `add()`: `add()` links members only while holding the same lock
and refuses a set it finds `IsInvalidated` (its compilation is then
invalidated at link), so a member is either visible here - and the fire takes
the stop path - or never added. Compiler threads that read `isStillValid()`
lock-free and register lazily are covered by that refusal, as for every other
fire. Nothing else distinguishes a watched from an unwatched Class-A set, so no
consumer can depend on the stop having happened. Flag-off never reaches this
code (Class-A routing is flag-on). Counted as `watchpointFireWatcherless`.
Measured: string-heavy's Class-A stops 376 -> 2 at four threads; typescript
GIL on 3,460 -> 171 stop requests. Test:
`jit/watcherless-watchpoint-fire-no-stop.js` (five threads run the string-heavy
inner-loop shape; stop requests in the threaded phase 6,445 -> 0 GIL on, values
conserved in every mode).


## §37. Megamorphic cache GIL off: one cache per thread, one epoch per process (eighth landing round; §5.5)

The sixth round left the megamorphic cache inert in a GIL-off process (§30): a
fill writes a multi-word entry with a `RefPtr` uid that N unsynchronized
mutators cannot share, so the inline probes bailed, the fills no-op'd, the
by-id megamorphic access cases were refused, and every access an inline cache
had given up on ran the generic operation - `gbemu` 1.7 M times per JetStream
run, the micro rows `megamorphic-access` at 1.8x and
`megamorphic-put-transition` at 2.7x of GIL on (PERF-RESULTS §6). This is the
design §30 recorded, now built.

Storage. Each JS thread of a GIL-off process owns a `MegamorphicCache`, hung
off its `VMLite` and created by the thread's first fill; the VM's cache stays
what flag-off and GIL-on use. Only the owning thread touches a cache's entries
- probes run in its JIT code, fills in its slow paths - so an entry needs no
atomicity, and the uid references a cache holds are dropped by the owner (a
displaced entry) or with the world stopped (collection end; thread teardown
takes the registry lock the collector's walk holds). Rule G1: no thread reads
or writes another thread's megamorphic cache except the conductor of a
collection inside the stop.

Invalidation. `VM::invalidateStructureChainIntegrity` (prototype changes,
adds/deletes/attribute changes on objects that may be prototypes, freezes,
flattening) runs on whichever thread mutates and must reach every cache. GIL
off the epoch those events bump is one process-wide 32-bit counter; a bump is
an atomic increment. The probe loads the counter's low half where it loaded
the VM cache's epoch before (one absolute load of a read-mostly line) and
compares it with the entry's 16-bit stamp as today. Rule G2 (what a stale
probe may do): a probe that loaded the counter before a concurrent bump can
validate a pre-bump entry and load through it. That is the reader linearized
before the mutation - the outcome a structure-checked IC load racing the same
mutation already has: its slot load returns the old value or `undefined`
(SPEC-objectmodel D1: flag-on deletes release-store `undefined` before the
table edit) and never another property's value (I18: no deleted slot is reused
before a collection, and a collection ages every cache). A reader ordered after
the mutation by any happens-before edge sees the bump: the increment precedes
the mutating thread's release, the probe's load follows the reader's acquire.
Rule G3 (fills): a fill stamps its entry with the counter value read BEFORE the
lookup whose result it caches, and keys it on the base's StructureID read
before that lookup, so a concurrent mutation or transition between lookup and
fill leaves a dead entry (stale stamp, or a StructureID the object no longer
has) rather than a live wrong one. Rule G4 (wrap): entries keep 16-bit stamps;
a cache remembers the counter's high half it last filled under and clears
itself when a fill or a collection finds the high half moved, so a wrapped low
half revalidates nothing unless one thread neither fills nor sees a collection
across 65,536 invalidations.

Collection end, world stopped, bumps the process counter (every collection:
an entry holds a raw holder pointer, as the VM cache's `age()` bump accounts
for) and on a Full collection also clears every thread's cache and drops its
uid references, walking the VMLite registry.

Codegen. GIL off `findMegamorphicCacheEntry` loads the cache pointer from the
current VMLite (the TLS load `loadVMLite` emits) instead of materializing the
VM cache's address, sends a null pointer (a thread that never filled) to the
slow path, and reloads it for the secondary table (the register that held the
base holds the epoch by then); the epoch compare reads the process counter.
The tagged-butterfly legs (`loadPropertyTagged` / `storePropertyTagged`, the
claimed transition arm) are the GIL-on ones of §30 unchanged: they are the
§5.5 read and write rows, which hold GIL off. Flag-off and GIL-on emission is
byte-identical to before. The by-id megamorphic access cases, the `canBeMegamorphic`
indexed forms and the DFG/FTL megamorphic nodes are admitted GIL off again.

Measurements (x86-64, Release, medians of 3). `megamorphic-access` (one get
site, 1000 shapes): GIL off 2410 ms -> 1500 ms (GIL on 1350, flag off 1110);
`megamorphic-put-transition-1M`: 128 ms -> 52.7 ms (GIL on 48); the new
corpus test's single-thread part: 200000 of 200000 gets through the gave-up
operation before, 0 after. JetStream `gbemu` GIL off did not move (104 ->
104, GIL on 158): its generic traffic is not megamorphic-cache misses (the
profile's leaders are JIT code quality, `ensureLengthSlowConcurrent` and the
math-IC slow path; §39, §40 and PERF-RESULTS §6.9 take those).

## §38. Dictionary flattening GIL off is a transition (eighth landing round; SPEC-objectmodel F3)

The problem. Flattening rewrites a dictionary's storage in place - offsets
renumbered by insertion order, slots moved between inline storage and the
butterfly, the butterfly shrunk or dropped - and keeps the StructureID, so a
reader that resolved an offset against the pre-flatten table, or an inline
cache another thread is generating from conditions it sampled before the
flatten, is wrong after it with nothing to tell it so. Flag-on the branch
therefore flattens only inside a stop (F3, "flatten under stop"), and GIL off
the inline-cache paths, which hold `codeBlock->m_lock` with heap access and so
must not request a stop, refuse to flatten at all: a property found through a
dictionary prototype is never cached GIL off and every access takes the
generic path (the fifth round's O2/GT11 refusal; the sixth round's attempt to
flatten after the lock was dropped, P3, kept the in-place form and crashed one
run in five because the StructureID survived the rewrite).

The rule GIL off. Flattening does not touch storage. It allocates a fresh
Structure cloned from the dictionary - the same prototype, type info and
inline capacity, a private copy of the property table with the same offsets
(holes included), the same `maxOffset`, kind `None`, `hasBeenDictionary` and
`hasBeenFlattenedBefore` set, thread-locality sets born fired if either of the
dictionary's had fired (F4/F3, no stop: nothing watches an unpublished
structure) - and publishes it with the structure-only N2 core
(`tryStructureOnlyTransition`: cell lock, StructureID re-check, the dictionary
table's edit count re-check against the value read before the clone, claim,
one header CAS). A reader holding the old structure reads the same slots in
the same butterfly; a stub keyed on the old StructureID misses; conditions can
now be established on the new structure because it is not a dictionary; the
old structure object stays a dictionary nobody's header names. What the
in-place form bought beyond cacheability - compaction of deleted slots and the
butterfly shrink - is forgone GIL off (a flattened prototype keeps its holes
until it dies). Leaving the dictionary fires its transition watchpoint set
(deferred, outside the structure allocation lock); a dictionary is rarely
watched and a watcherless fire takes no stop (§36), a watched one takes the
Class-A stop, which is why the inline-cache paths still do not flatten inline:
they record the object (`Structure::requestDeferredFlattenGILOff`) and return
"retry", and every `repatch*` entry point runs the recorded flatten once
`tryCache*` has returned and the lock is dropped; the retry that follows
caches. The unlocked runtime sites (`JSObject::flattenDictionaryObject`, the
global object's prototype setup, the interpreter's scope flattening) take the
transition form directly GIL off; GIL on and flag off keep the in-place forms
(under the stop, and as upstream) unchanged.

Races. Two threads flattening one object: the loser's publication fails the
StructureID re-check and finds a non-dictionary; its clone is garbage. An
in-place dictionary edit between the clone and the publication: the edit-count
re-check fails, RESTART re-clones. The object's owner transitioning it
claim-first meanwhile (E4-C): the claim CAS in the N2 core loses, RESTART. A
foreign flatten of an object whose shape is still thread-local: the N2 core's
step 0 fires the sets under a stop first, as for any foreign structure change
(legal at every site that runs the flatten).

Follow-up found by the eighth round's amplifier campaign
(`objectmodel/define-property-kind-change-vs-readers.js` GIL off, a hang in
about 1 run in 20 under `--randomYield*`): a flatten publishes a NEW structure
whose table is a clone, so it must not slip between the two halves of an
in-place edit of the source. The uncacheable-dictionary form of a kind-changing
`defineProperty` stored the value under the cell lock and changed the
attributes in the pinned table AFTER releasing it; a flatten that cloned in
between froze the old attributes over the new value in the flattened
structure and the attribute edit landed in the orphaned table - after which
every reader of the property, and the defining thread's own next lookup, spun
on "attributes and value disagree". Two rules close it: `putDirectInternal`'s
in-place attribute change re-checks that the object still has the structure
it edited and RESTARTs otherwise (the replay meets the flattened structure and
takes the transitioning form, publishing value and attributes together); and
the flatten's publication, under the cell lock, compares its clone with the
source's table entry by entry and RESTARTs on any difference (edits made
inside a stop do not bump the edit count the plan was checked against).
0 hangs in 450 amplified runs after (both modes) - but the plain corpus
run of the same test, on a machine loaded by the stress suites, still hung
once on that binary (the same frozen state: the flattened structure's
attributes disagreeing with the slot), so a third interleaving existed. It
is the pair of locks: the flatten compares its clone and publishes inside
one CELL-lock section, while the uncacheable dictionary's in-place attribute
edit took only the STRUCTURE's lock - so the edit could land after the
compare and before the publish, and the defining thread's re-check (second
rule) then still saw the old structure and returned. Third rule: that
in-place attribute edit is made under the cell lock too (as the value store
before it and every dictionary add, replace and delete already are), after
re-checking the structure under it; an edit is then wholly before the
flatten's compare (the compare fails, the flatten restarts) or wholly after
its publish (the re-check fails, the define restarts on the flattened
structure and takes the transitioning form). Lock order is the established
cell lock (10a) before `Structure::m_lock` (10b). With the three rules the
flatten is on by default (`Options::useGILOffDictionaryFlatten` remains as a
switch); measured on the final tree: JetStream GIL off +1.9 % overall with it
on, `ML` +40 %, `Basic`/`regexp`/`raytrace` +6-7 %; the kind-change test's
hang rate before and after the third rule is in LANDING-PLAN "Results,
eighth round" (P2).

## §39. GIL off, the FTL keeps the butterfly across polls; bounds come from the same butterfly's vectorLength (eighth landing round; §5.5, I21)

The problem. GIL off every poll (the `CheckTraps` at a loop head) was modeled
as writing `JSObject_butterfly` and `Butterfly_vectorLength` (AUDIT-checktraps
Tier-B B3): a foreign thread can convert a flat butterfly to segmented storage
and grow it with no stop and no epoch bump (SPEC-objectmodel §4.2), and the
segmented array's publicLength is the old flat header's slot (I9b), so a
hoisted flat base paired with a re-loaded publicLength could index past the
flat allocation. Forcing {base, publicLength, vectorLength} to be re-loaded
after every poll closed that, and cost every GIL-off loop over an array its
loop-invariant code motion: the profile of `crypto`'s inner loops (eighth
round) is the object pointer re-loaded from the frame, the butterfly word
re-loaded and masked, the length re-loaded and, for the store, the ownership
test re-run, on every iteration - about 25 instructions on a 50-instruction
loop body, where GIL on hoists all of it.

The rule (FTL plans, GIL off). The poll no longer writes `JSObject_butterfly`
or `Butterfly_vectorLength`; `NamedProperties`, `IndexedProperties` and
`Butterfly_publicLength` stay poll-bounded (the memory-model interim of
AUDIT-checktraps §7.1 is unchanged: plain values are re-read after a poll).
Instead, every bound the FTL derives from a storage edge - a GetButterfly
result, which LICM may now have hoisted across any number of polls - is the
smaller of that storage's publicLength and its own vectorLength: SSA lowering
adds `CheckInBounds(index, GetVectorLength(storage))` next to the publicLength
check for Int32/Double/Contiguous in-bounds accesses (GetByVal, PutByVal,
HasIndexedProperty, EnumeratorGetByVal, Atomics), and the out-of-bounds legs,
`HasIndexedProperty`, `EnumeratorNextUpdateIndexAndMode`, `ArrayIndexOf` /
`ArrayIncludes` and `ArraySlice` clamp the publicLength they read to the
vectorLength of the same storage (`publicLengthForBounds`); `ArrayPop` and
`ArrayShift` take their runtime path when the two disagree; `ArrayPush` and
`ArrayUnshift` already tested vectorLength. Why this is enough: a flat
butterfly's vectorLength never changes in place GIL off (T1 always allocates
afresh; AS-COPY; the GIL-on in-place forms are GIL-on only), so a hoisted
vectorLength is the extent of the allocation the hoisted base points into for
as long as the frame holds it (the conservative scan keeps a superseded
butterfly alive; I7); lanes below it are either this array's current storage
or, after a foreign segmented conversion, the aliased fragments of it, so a
read there is a current or tardy value and a write there lands where the
segmented readers look; an index at or past it goes to the node's slow path or
exits, which re-derives everything from the object. Everything a stop can
change (haveABadTime, structure retags, debugger) still bumps the conductor
epoch and forces the frame out at the poll (I21's precise-jettison arm), so
structure and indexing-type facts stay hoistable as before and the butterfly
now joins them. The DFG tier has no LICM and keeps the reload (its local CSE
never spans a poll: polls open blocks). Nodes that load the butterfly
themselves (MultiGetByVal, Spread, ArraySortCommit, ...) read a current word
and are unaffected. Flag-off and GIL-on emission is unchanged.

The Int32 lane check (I41) is one compare now: `(lane - 1) < NumberTag - 1`
unsigned passes holes and int32s and fails everything else, replacing the
two-flag form that cost seven instructions per element load.

## §40. GIL off, an exit reaches its compiled ramp without the generation thunk (eighth landing round; §4.4, I2)

Flag-off, the first time a DFG or FTL speculation exit is taken its ramp is
compiled and the exit's patchable jump is repatched to it. GIL off nothing
repatches reachable code outside a stop (I2/P3), so the jump kept pointing at
the generation thunk and every later exit of the same site saved every
register, called `operationCompileOSRExit` / `operationCompileFTLOSRExit`,
found the published ramp and jumped to it - `gbemu` takes about 25,000 exits
a run in either mode, and GIL off each one paid that. Now: DFG code GIL off
dispatches its exits the way unlinked DFG code does, through the JITData exit
vector (`move index; jump` to one shared tail that loads `m_exits[index]`'s
code pointer and far-jumps), whose slot `setExitCode` publishes atomically
after the ramp is finalized, so from the second exit on the site reaches its
ramp in one indirect jump; invalidation points keep their jump replacements.
FTL exit thunks GIL off (x86-64) read the exit's `m_codePtrForConcurrentReaders`
- the pointer `compileStub` publishes with release semantics after bumping
the stop generation - through a borrowed register saved on the stack and
`ret` to it when it is set, falling back to the generation thunk when it is
not; the address of that word is patched in at link time, and the FTL
JITCode's exit vector is no longer shrunk GIL off so the word does not move.
The operations' published-pointer fast paths stay for the first racing exits.
A thread that jumps into a ramp another thread compiled does so through a
pointer published after `FINALIZE_CODE`, the same way every thread already
enters DFG/FTL code a compiler thread produced.

## §41. (considered, not adopted) Exempting Class-A fire windows from the heap-fact epoch

The eighth round considered declaring a watchpoint fire's stop window
code-lifecycle-only (no conductor heap-fact epoch bump, so bystander threads
parked by the fire keep their optimized frames): every `Watchpoint::Type`
handler rewrites code or code-side caches, never a structure, indexing type
or butterfly. It was implemented and then withdrawn in the same round because
no test could show a before/after difference - four optimized worker threads
parked by twelve watched transition fires were not jettisoned on resume with
or without the change (the fires' own jettisons were the only ones) - so the
change had no demonstrated effect to set against the audit surface it
touches (AUDIT-checktraps row CA stays "conservative"). Recorded so the idea
is not re-derived without first explaining that measurement.

## §42. GIL off, a site that meets Double arrays among others is generic, not converting (eighth landing round; §5.5, OM §4.7)

Flag-off, a get/put-by-val site whose profile saw several indexing shapes
speculates the widest (Contiguous, or ArrayStorage) and plants `Arrayify`,
which converts every narrower array that reaches it in place - a few stores.
GIL off a Double array's conversion to Contiguous, and any conversion to
ArrayStorage, is a per-array stop-the-world (OM §4.7 / I28: a stale
Double-keyed reader over boxed storage), so such a site stopped the world once
per Double array that flowed through it: `Basic` builds one-element arrays
that hold a double in some iterations, and a later site that reads them had,
depending on the order its profile filled, either a Contiguous speculation
with `Arrayify` (up to 840,000 stops and 3.7 s of a 5 s run - the test's
bimodal 145 / 420 / 520 scores across runs) or a generic access; the sjcl
tests (`aes`, `pbkdf2`, `sha256`) paid 2,000-4,000 such stops a run at
`bitArray.bitLength`. Rule: GIL off, `ArrayMode::fromObserved` returns
`Array::Generic` instead of a converting Contiguous mode when the observed
shapes include Double, and instead of a converting ArrayStorage mode when they
include anything else; the DFG and FTL then access the site through their
get/put-by-val inline caches, which serve each shape with its own stub and
convert nothing. Int32 and Undecided arrays keep converting to Contiguous
(stop-free relabels for the allocating thread, OM T4-O), and a Double-profiled
put site whose value is not a number keeps its conversion (that array does
change shape, once). Flag-off / GIL on: unchanged.
