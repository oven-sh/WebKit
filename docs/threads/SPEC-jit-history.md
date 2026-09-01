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
