# SPEC-jit-annex — FROZEN NORMATIVE ANNEX (rev 10)

Normative annex of `SPEC-jit.md`: App. G6, App. G (+rev-5 addendum), App. 5.6,
App. R1 (from the history, now NON-NORMATIVE), and App. R5 (from spec R5).
Implementers MUST consume this file with the spec; read-only. Precedence: the
in-spec rules win on conflict (App. R1 is superseded by spec R1.c/h/i; rev 10
changed: §5.5 transition predicate keyed per-object for butterfly-bearing cells,
§5.6 coalescing REQUIRED, §4.4 cadence incl. heap legacy-end reclamation).

---
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

### App. G addendum (rev 5)

G7 (addendum): `notifyVMConstruction` parks a newly constructed VM only when `m_worldMode != Mode::RunAll`, i.e. only while a stop is in progress (`runtime/VMManager.cpp:532-545`); no synchronization with an in-flight inline watchpoint fire — basis for deleting the §5.6 mutator-count gate.
G10 (addendum, virtual mode): `CallLinkInfo::setVirtualCall`/`setStub` write `polymorphicCalleeMask` (= 1, `CallLinkInfo.h:77`) into the `m_callee` slot (`CallLinkInfo.cpp:282,310`); data-IC fast paths treat a low-bit-set comparand as "always call" via `branchTestPtr(NonZero, ..., polymorphicCalleeMask)` (`:342-348`); callee cells never have bit 0 (`RELEASE_ASSERT`s `:136,152`). §5.8's sentinel comparand reproduces exactly this predicate.
G13 (new; full text lives in SPEC-jit §1): heap rev-5 dispositions — GC outside the VMM reason latch (`VMManager.cpp:461-463` assert stays; heap §13.5c), GCL serialization for JSThreads stops, `bumpAndReclaim` GC-conductor-only (`SPEC-heap.md:199,278-279,342,353,415,420`).


## App. R5 — per-platform TID-tag TLS mechanics (moved verbatim from spec R5; FROZEN NORMATIVE)

* **ELF (Linux glibc+musl)**: `extern "C" __attribute__((tls_model("initial-exec"))) thread_local uint64_t g_jscButterflyTIDTag` in owned `jit/ConcurrentButterflyOperations.cpp` (REQUIRED for .so builds). JIT tiers: process init computes the thread-invariant TLS-base offset, RELEASE_ASSERTed constant at second-thread startup, baked as immediate at emission (x86-64 fs-prefixed load; ARM64 TPIDR_EL0+ldr). NEW EMITTERS (owned, line 5; tree has only `gs()`/`mrs_TPIDRRO_EL0`; FAST_TLS_JIT Darwin-only): `X86Assembler::fs()`+fs-prefixed 64-bit absolute-offset load; `ARM64Assembler::mrs_TPIDR_EL0(RegisterID)`; surfaced as `MacroAssembler{X86_64,ARM64}::loadFromELFTLS64(offset, dst)` (OS(LINUX); Task 1b). LLInt (build-time asm; `tls_loadp` Darwin-only): owned `loadButterflyTIDTag` macro=raw `emit` (`instructions.rb:33`)+link-time initial-exec relocations - x86-64 `movq %fs:g_jscButterflyTIDTag@TPOFF, <reg>`; ARM64 `mrs`+`add :tprel_hi12:`+`ldr :tprel_lo12_nc:` (hard regs per t-register mapping).
* **Darwin**: Mach-O TLV has NO constant offset; `thread_local` unusable. ALL reserved direct keys TAKEN (audit history §17). Mechanism: `pthread_key_create`'d key at P5 init (TSD slots uniform=>direct-offset valid for dynamic keys), cached in owned global+M4a `JSCConfig` `uint32_t butterflyTIDTagTLSKey`; per-thread `pthread_setspecific`. JIT tiers: `loadFromTLS64(fastTLSOffsetForKey(key))` (`MacroAssemblerX86_64.h:7411`/`ARM64.h:6399`), offset baked at emission. LLInt: key from `_g_config`+REGISTER-form `tls_loadp` (`x86.rb:1730`/arm64 register form) - two loads, Darwin LLInt only.
* **Other (Windows)**: unsupported flag-on for MVP - P5 init RELEASE_ASSERTs at second-thread startup.
