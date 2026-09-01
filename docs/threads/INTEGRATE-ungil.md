# INTEGRATE-ungil — GIL-removal landing ledger (IU; SPEC-ungil / UNGIL-HANDOUT §IM, TERM1.6)

Created by U-T1 per TERM1.6. IU is the landing ledger for the ungil
milestone, schema per the INTEGRATE-* house pattern. The seven mandated
tables (i)–(vii) are below as skeletons; each names its owning task. Until a
task fills its table, every "IU row" citation in SPEC-ungil / the handout is
an OBLIGATION on that landing task. IU adds call-site enumeration only and
NEVER re-rules a K4/N7/TERM1 disposition (the EXECUTED audit tables in the
handout are consumed verbatim).

Authority: docs/threads/UNGIL-HANDOUT.md (normative handout);
docs/threads/SPEC-ungil.md is the doc of record on conflict.

## Task log

- U-T1 (§A.1.2-7 / §A.3.6 base rerouting, DARK) — LANDED in-tree. All new
  behavior is keyed on `vm.m_gilOff` (per-VM, U0c) or
  `VM::isGILOffProcess()`; no shipping configuration sets either, so
  flag-off AND flag-on+GIL-on behavior is unchanged. Files (all
  U-T1-owned):
  - `runtime/VM.h` / `runtime/VM.cpp`:
    - `m_gilOff` + `gilOff()` (U0c; immutable, computed at the TOP of the VM
      ctor before any entry/codegen/m_mainVMLite registration; winner of
      `Heap::tryDesignateStickySharedServer()` under gilOffProcess, with the
      eager `noteSharedServerSticky()` at clientSet()==1).
    - `VM::isGILOffProcess()` — C++-side equivalent of the §A.1.3 level-(i)
      JSCConfig `gilOffProcess` byte. **OBLIGATION (U-T3): the Config byte
      (JSCConfig.h:106, beside the M4a slot) must stay derivation-identical
      (useJSThreads && !useThreadGIL && useVMLite && useSharedAtomStringTable
      && useSharedGCHeap, latched at Config finalization).**
    - `group3Primitives()` mode-split selector; rerouted same-name accessors:
      `exception/clearException/setException/lastException/clearLastException/
      exceptionForInspection/hasPendingTerminationException`,
      `stackPointerAtVMEntry/setStackPointerAtVMEntry`, `stackLimit`,
      `isSafeToRecurse`, `lastStackTop/setLastStackTop`, `updateStackLimits`.
      GIL-off + no installed same-VM lite falls back to the (inert) VM block
      — ctor tail / ~VM tail / §F.5 nested windows; recorded here as a
      deliberate dark-phase semantics (the activation tasks must not rely on
      VM-block contents GIL-off).
    - `addressOf*` / `*Offset()` emission helpers DELIBERATELY not
      mode-split (GIL-on emission only; gilOff compilations emit
      loadVMLite + VMLitePrimitives offsets — U-T3/U-T4). Comment block in
      VM.h is the in-tree marker.
    - §A.1.5: `isEntered()` mode split (`isAnyThreadEntered()` registry
      walk), `currentThreadEntryScope()`, per-lite service-bit routing +
      `backfillEntryScopeServiceBitsForLiteRegistration()`,
      VM-wide fan-out `requestVMWideEntryScopeService()` (registry-locked).
    - §A.1.6/A16: `allocateBakedScratchBufferIndex()` (registry index +
      install fan); `scratchBufferForSize/clearScratchBuffers/isScratchBuffer`
      gilOff dispatch to the CURRENT lite; `gatherScratchBufferRoots` now
      ALSO walks the VMLiteRegistry (per-VM filter) and scans each lite's
      ownership list (jit R2) — closes VMLite's Phase-A "not visited" GC
      caveat.
    - ANNEX A36: `m_vmEpoch`/`vmEpoch()` (process-monotonic, never 0).
  - `runtime/VMLite.h` / `runtime/VMLite.cpp` (L2 appends only):
    `gilOff` byte (+`offsetOfGilOff()` for U-T3), `State`
    (Live/Teardown/Collected/Detached, under-registry-lock-only),
    `ownerHasNoTlsDtor` (A36 r32), `clientHeap`, `entryScope`,
    `entryScopeServicesRawBits` (atomic; VM owns the packing),
    A16 segmented baked-buffer table (`scratchSegments`,
    `scratchBufferAtIndex` lock-free read, `ensureScratchBufferAtIndex`,
    `backfillBakedScratchBuffers`, `offsetOfScratchSegments()` for U-T4);
    process-wide `ScratchBufferRegistry` (rank OUTSIDE VMLiteRegistry::lock);
    carrier-TID hooks (`setCarrierTIDHooks`/`allocateCarrierTID`/
    `releaseCarrierTIDIfHooked`).
  - `runtime/JSLock.h` / `runtime/JSLock.cpp`: A36/A36C lazy per-(thread,VM)
    carriers — two TLS map slots (main thread: destructor-free plain
    thread_local, leaks accepted; non-main: ThreadSpecific whose dtor is the
    carrier-TLS-death path, U-T1 SKELETON: TEARDOWN-mark ->
    unregisterLite-LAST), epoch-before-carrier staleness check, the
    {lite, TID-tag, §10A.1 client slot} tuple swap at install and LIFO
    restore, `m_didInstallCarrierVMLite`/`m_entryThreadClient`,
    `uninstallVMLiteForVMDestruction` carrier arm. §A.1.4: the L7
    RELEASE_ASSERT now reads through the mode-split accessor (GIL-on: VM
    slot; GIL-off: the carrier lite's slot).
  - `runtime/VMEntryScope.cpp`: §A.1.5 per-lite entry record in
    setUpSlow/tearDownSlow (plus the transitional VM-member shadow, below).
  - `heap/Heap.h` / `heap/Heap.cpp`: `Heap::tryDesignateStickySharedServer()`
    (U0c designation primitive; CAS only, no assert); `friend class VM`
    (eager noteSharedServerSticky from the winner ctor);
    `friend class JSC::JSLock` on GCClient::Heap (A36C client-slot
    re-stamp); the r6-F5 per-VM registry root walk in the Msr/VMExceptions
    constraint (m_terminationException stays VM-global, both modes); the
    §10D `m_isSharedServer=false` arm conditioned on `!gilOffProcess`
    (pollIssRevertIfNeeded early-out + hint disarm).

- U-T1 AMENDMENT (post-review) — (a) JSLock stale-epoch carrier eviction
  rewritten onto the skeleton protocol (evictStaleCarrier; see obligation 4's
  eviction-path ownership rule) — closes a release-build registry UAF + I17
  TID leak; (b) willReleaseLock's depth-0 topCallFrame guard rerouted through
  group3Primitives() (table (iv) row); (c) GIL-off RELEASE_ASSERT tripwires
  on the VMEntryScope transitional shadow (obligation 1 HARD GATE); (d)
  obligation 9a added (exception-scope verification state, K4 table I);
  (e) ledger row 3 + obligation 5 ownership corrected (U-T8 / U-T8b per the
  handout §T task list).

- U-T4b (§A.1.3/6 FTL + OSR emission, DARK) — LANDED in-tree. All gilOff
  arms are codegen-time keyed on the COMPILED-FOR VM's `vm.gilOff()` (per
  §A.1.3; U0c fixes the mode pre-codegen); GIL-on/flag-off emission is
  byte-for-byte unchanged at every touched site (explicit dual arms — no
  shared restructuring of the GIL-on B3/assembly shapes). Files:
  - `ftl/FTLSaveRestore.{h,cpp}`: `materializeBakedScratchBufferPointer` /
    `...DataPointer` (the frozen A16 `loadVMLite -> segment -> [index]`
    emitters; address-dependent loads, clobber only dest), ScopedLambda
    base-materializer forms of save/restoreAllRegisters, and
    `restoreCalleeSavesFromCurrentVMLiteEntryFrameCalleeSavesBuffer`.
  - `ftl/FTLLowerDFGToB3.cpp`: per-site dual emission for ALL nine
    scratchBufferForSize main-path sites (define-fields, 3x ArrayPush,
    splice, unshift, NewArray, NewArrayWithSpread) via a per-site
    patchpoint (`bakedScratchBufferPointer`); JITCode-RESIDENT
    catchOSREntryBuffer set-site + ExtractCatchLocal/ClearCatchLocals +
    ExtractOSREntryLocal read-backs via baked indices; A16-ext gilOff
    fallbacks (see OPEN below) for ArithRandom (operationRandom call) and
    HasOwnProperty (probe skipped).
  - `ftl/FTLForOSREntryJITCode.{h,cpp}` (m_entryBuffer -> baked index),
    `ftl/FTLOSREntry.cpp` + `dfg/DFGOSREntry.cpp` (per-lite fill/readback;
    branch on bakedIndex != UINT_MAX so the same reader serves the future
    U-T4a DFG-tier index), `dfg/DFGCommonData.h`
    (catchOSREntryBufferBakedIndex append).
  - `ftl/FTLOSRExitCompiler.cpp`: full ExitScratchAddressing dual-mode
    compileStub/compileRecovery (every baked absolute — register dump,
    materialization pointer/argument slots, unwind scratch, activeLength,
    checkpoint tmps — becomes lite-resolved base + static offset,
    rematerialized per use); per-lite Group-3 unwind-entry resolution
    (callFrameForCatch/topEntryFrame); gilOff once-only exit compilation
    under a new `ftlOSRExitGenerationLock`. LOCK RANK (corrected — this lock
    is NOT a leaf, it is held across the whole of compileStub): OUTERMOST of
    {codeBlock->m_lock (ConcurrentJSLocker, getArrayProfile),
    ScratchBufferRegistry::m_lock -> VMLiteRegistry lock -> per-lite
    scratchBufferLock (via VM::allocateBakedScratchBufferIndex, the §LK.6
    chain), LinkBuffer/executable-allocator locks}; acquired ONLY from the
    exit-generation thunk's operation call with no other JSC lock held; must
    never be taken while holding any of the above; GIL-on never takes it.
    Sibling with the same rank, never nested with it:
    `ftlLazySlowPathGenerationLock` (FTLOperations.cpp). The U20 lock-order
    lint owner must carry both as an explicit outer-rank row (§LK side is
    frozen; this IU row is the binding rank record until then). gilOff, the
    winner does NOT repatch the exit jump (other mutators may be executing
    it; x86_64 rel32 repatch is not single-copy atomic and ISB1 only covers
    in-stop patching) — the jump stays on the generation thunk and every
    subsequent exit takes the locked fast path, returning the compiled stub
    for the thunk's tail call (data-only protocol; GIL-on repatch unchanged).
  - `ftl/FTLOperations.cpp` + `ftl/FTLLazySlowPath.cpp`: same once-only +
    no-live-repatch treatment for FTL lazy slow paths.
    operationCompileFTLLazySlowPath gilOff takes
    `ftlLazySlowPathGenerationLock`, generates only if `!stub()` (generate()
    RELEASE_ASSERTs !m_stub — N threads racing one unpatched jump would
    otherwise crash or double-publish m_stub/double-repatch), losers return
    the winner's stub; LazySlowPath::generate skips the repatchJump under
    gilOff (thunk round-trip per traversal, data-only). GIL-on both paths
    byte-for-byte unchanged.
  - `ftl/FTLLocation.{h,cpp}`: base-GPR `restoreInto` overload.
  - `ftl/FTLThunks.cpp` + `dfg/DFGOSRExitCompilerCommon.h`
    (adjustFrameAndStackInOSRExitCompilerThunk; signature widened
    MacroAssembler& -> AssemblyHelpers&, both callers are AssemblyHelpers):
    per-lite register-dump buffers + lite-resolved callFrameForCatch in the
    shared generation thunks.
  - `dfg/DFGOSRExitCompilerCommon.cpp` (adjustAndJumpToTarget exception
    tail, shared by DFG+FTL exits): lite-resolved
    targetInterpreterPCForThrow (payload-only variant store, M6-identical
    layout), topEntryFrame callee-save copy, callFrameForCatch.
  - Amplifier: `JSTests/threads/jit/ftl-osr-entry-catch-loop-amplifier.js`
    (concurrent catch + loop OSR entry, one CodeBlock; passes GIL'd).
  U-T4b OPEN items (owners named):
  1. **A16-ext lite-resident copies NOT landed** (no U-T1 L2 lite slots, no
     K.3 publish): ArithRandom gilOff falls back to operationRandom (JIT-side
     tear removed; the HOST-side shared WeakRandom advance remains the
     K4.VIII.10 runtime row — owner of the per-lite stream: the K-rows
     runtime task, with U-T4a/U-T4b re-pointing emission once the slot
     exists); HasOwnProperty gilOff skips the inline probe (conservative);
     RecordRegExpCachedResult gilOff FAIL-STOPS at codegen (DFG_CRASH in the
     gilOff arm of compileRecordRegExpCachedResult — the shared
     m_regExpGlobalData cachedResult is SEMANTIC state with no safe fallback,
     so the activation gate is enforced in code, same precedent as the
     Darwin loadVMLite RELEASE_ASSERT; GIL-on emission byte-for-byte
     unchanged). **gilOff activation (U-T6/U-T9) MUST NOT ship before the
     lite slots + re-pointed emission land** — for RecordRegExpCachedResult
     this is now mechanical (FTL compiles of the node crash under gilOff
     until the K4/U-T8b lite-resident copy lands and the tripwire is
     replaced by the re-pointed emission). Megamorphic FTL
     paths bake no VM cache address in FTLLowerDFGToB3 (they run through the
     shared InlineCacheCompiler machinery — that surface's A16-ext leg
     belongs to its owning task, not U-T4b).
  2. **DFG/Baseline legs untouched** (U-T4a boundary): DFGJITCompiler.cpp:527
     catch-buffer set-site, DFGSpeculativeJIT.cpp:17805/17812 catch
     readbacks, DFG OSR exit compiler scratch buffers, and every
     Baseline/DFG scratch bake still GIL-on-only. The shared read-side
     (DFGOSREntry/DFGCommonData index) is already in place for them.
  3. **Darwin loadVMLite gap inherited** (U-T3 OPEN): gilOff-mode emission
     RELEASE_ASSERTs on Darwin until the JSCConfig vmLiteTLSKey lands.
  4. handleExitCounts' CodeBlock-resident exit counters remain non-atomic
     cross-thread increments under gilOff (profiling-only; CodeBlock/jit
     rows own the ruling).

- U-T10 (§C.1-2 atomic slot accessors) — LANDED in-tree. Files (U-T10-owned
  edits): `runtime/JSObject.h` (AtomicSlotOperation/AtomicSlotStatus/
  AtomicSlotRequest + the four §9.5 entry points on JSObject:
  `atomicSlotReadModifyWrite`/`atomicSlotCompareExchange` + the AtIndex pair),
  `runtime/ConcurrentButterfly.cpp` (ANNEX C1 implementation: lock-free
  inline/flat-OOL/segmented-fragment seq_cst 64-bit slot CAS/RMW loop; the
  flat-path SW discipline — ensureSharedWriteBit FIRST, I34 structureID +
  butterfly re-validation, THEN the slot CAS, Restart on validation failure,
  completed CAS never re-applied; the OM-locked third arm for dictionary/
  AS-shape with the AS PRE-LOCK SW protocol (r8 item 6) and under-lock
  dictionary-ness/offset/D7 re-checks; the indexed-by-shape arm — CoW
  materialize-first §4.8/I35, Int32/Double convert-to-Contiguous on FIRST
  atomic access (owner direct, foreign SW-set DCAS first), Contiguous = flat
  arm, AS/dict-indexed = third arm; write barrier after success),
  `runtime/ThreadAtomics.cpp` (§C.2 re-home: gilOff-only \*GilOff bodies for
  load/store/compareExchange/RMW dispatching through the §9.5 accessors with
  the probe/Restart loop; D3 receiver gates + messages and D7 writability
  carried verbatim; rope SVZ operands resolved OUTSIDE any lock via JSString
  resolution (§N.2) then re-probed; store's Missing arm stays on the generic
  OM add path). GIL-on/flag-off: byte-identical bodies (every new branch is
  `vm.gilOff()`-gated; ThreadAtomics.h signatures untouched per the frozen §7
  list). Corpus: `JSTests/threads/atomics/property-cas-storm-u5-as.js` (ANNEX
  C1 U5 amplifier: owner unlocked AS store storm vs foreign CAS, same index,
  SW initially 0 + exact locked-counter arm),
  `property-cas-storm-u28-flat.js` (U28-class lock-free-arm exactness storm:
  inline/OOL/Int32-converting/Double-converting/RMW + rope-SVZ contention),
  `property-cas-dictionary-delete-u5.js` (U5 dictionary arm: delete/re-add
  storm vs foreign CAS; no quarantined-slot resurrection).
  **ENTRY GATE record (§D.2): NOT SATISFIED — DEFERRED.** The OM Task-14
  PRE-INT bench verdict is NOT recorded anywhere in-tree
  (INTEGRATE-objectmodel §46 still instructs "record the verdict here"; no
  PROMOTE record exists), so the §D.2 HARD precondition of U-T10 ENTRY did
  not hold when this task landed. This task cannot run the jit Task-13
  GIL-stub construction bench (docs+code round, no builds), so U-T10
  proceeded on the no-PROMOTE arm — 8h ships as landed (cell-locked N2), which
  is the only arm consistent with §C's third arm as frozen. This is a GATE
  DEFERRAL, not gate satisfaction. OBLIGATION (orchestrator, HARD, before
  **U-T11 ENTRY** — U-T11 is the first consumer of these accessors (§C.3(a)
  routes the PWT pre-enqueue load through them), so "before U-T14 close" is
  too late: run the §L2.h bench, record the verdict in INTEGRATE-objectmodel
  §46, AND record an explicit gate-deferral ruling in SPEC-ungil-history.md
  (not a U-T10-owned file — the U-T14 close audit must not count this as an
  unrecorded supersession); a PROMOTE verdict retroactively requires landing
  Task 14 and re-reviewing §C's third arm AND the amend-round locked
  undefined-disambiguation arm (this file's locked-arm code is the surface
  to re-review).
  ~~OPEN (owned by U-T11 per the task split)~~ **§C.3(b) LANDED 2026-06-10
  (CVE-close round, MC-WAIT S3a):** atomicsWaitOnProperty and
  atomicsWaitAsyncOnProperty now re-validate SVZ(o[k], expected) via the §9.5
  load UNDER the list lock after the enqueue (ThreadAtomics.cpp:
  sameValueZeroForAtomicsUnderListLock /
  revalidateEnqueuedPropertyWaiterUnderListLock + the per-function
  dequeue-and-restart loops). Mismatch => dequeue + "not-equal"; rope re-read
  or accessor Restart (I34 provenance) => dequeue, re-derive outside any
  lock, FRESH enqueue (the I10 eats-one-notify class, r7 F3). The enqueue
  stays inside findOrCreateList's listLock section (lost-waiter closeout
  fix); the re-validation re-takes listLock — equivalent ordering, argued in
  the §C.3(b) banner in-file. Under-lock compare is alloc-free (rope pairs
  punt to restart; restart's step-1 SVZ resolves both ropes in place outside
  locks, so progress needs an external mutation — the declared
  bounded-adversarial class). waitAsync revoked-registration disposal:
  AsyncTicket::retireUnsettled (win settle CAS, cancelPendingWork, clear
  promise Strong — no settler can have observed the node, RELEASE_ASSERTed).
  GIL-on: one loop iteration, every new arm gilOff-gated — landed body
  byte-equivalent. Regression test: mc-wait-property-wait-lost-wakeup.js
  (20/20 GIL-off Release, Debug, and GIL-on). The §C.3(a) routing was
  already landed (this row's previous state); the 4.5-1a/G11 gate edits were
  landed separately (C.4) — no §C.3 obligation remains OPEN.

  **AMEND round (adversarial review, 2 reviewers) — LANDED:**
  1. **U5 D1-sentinel hardening (blocker, CONFIRMED).** The lock-free
     CAS/RMW loop validated {offset, structureID} provenance only at
     entry/I34 time; flag-on named deletes D1-store jsUndefined into the
     doomed slot BEFORE the structure publication (I30,
     storeUndefinedIntoDoomedSlotConcurrent) and never touch the butterfly
     word, so a loop iteration could read the quarantine sentinel through an
     in-flight delete (flat→dictionary convert + dictionary delete, AND the
     broader case the review missed: a plain non-dictionary delete, whose
     sentinel lands before its header CAS) — a CompareExchangeSVZ with
     expected===undefined would Apply on an ABSENT property (U5) and
     Load/failed-CAS would surface impossible undefined reads. Fix
     (ConcurrentButterfly.cpp): (a) atomicSlotLockFreeLoop re-validates
     structureID every iteration, between the seq_cst slot load and the CAS;
     (b) because the ID check alone cannot close the non-dictionary delete's
     pre-publication window, named-slot jsUndefined reads bounce out as the
     internal LockedRevalidate status and are disambiguated under the cell
     lock (both delete flavors hold it across their whole sentinel-store →
     publication window, §6 L4; the write stays a seq_cst slot CAS — the
     lock excludes deleters, not other CASers); (c) the indexed Contiguous
     call site pins a (nuke-checked) dispatch-time structureID
     (revalidateUndefined=false there — indexed deletes/holes are empty
     JSValue()s, caught by the !current restart, as the review itself
     notes). Corpus: `property-cas-delete-undefined-sentinel-u5.js`.
  2. **Probe accessor-attribute gap (blocker, CONFIRMED).**
     probeOwnPropertyForAtomicsConcurrent decided accessor-ness from the
     methodTable walk (structure S0) but recorded {offset, structureID}
     provenance from a RE-READ structure S1 without re-checking S1's
     attributes — a racing data→accessor reconfiguration between the reads
     handed the lock-free arm a kind=Data probe whose I34 check passes while
     the slot holds a GetterSetter (CAS-a-primitive-over-a-cell type
     confusion). Fix (ThreadAtomics.cpp): the probe now rejects
     Accessor|CustomAccessor|CustomValue against the SAME structure the
     provenance is taken from (kind=Accessor ⇒ existing D3 TypeErrors),
     mirroring the third arm's under-lock re-check; also closes a probe⇄
     third-arm livelock for CustomValue slots that answer slot.isValue().
  3. **Missing-arm TOCTOU (major, CONFIRMED for named adds; both reviewers).**
     probe(Missing) → isExtensible → putDirectMayBeIndex was three steps;
     a racing defineProperty(accessor / non-writable) or preventExtensions
     between probe and put was silently clobbered/overtaken (putDirect's
     define-own attribute-change transition to attributes 0). Fix: named
     adds route through JSObject::putDirectForAtomicsMissingAdd
     (ConcurrentButterfly.cpp; PutModePut through putDirectInternal's
     flag-on §2 loop — existence/extensibility re-derived in the SAME
     iteration whose E4 structureID-CAS publishes the add, so racing
     defines/preventExtensions fail the publication and return a non-null
     error; the GilOff store body then RESTARTS and the fresh probe throws
     the precise D3/D7/non-extensible TypeError; a racing plain writable
     data add is absorbed as a value-only, attribute-preserving replace =
     define-then-store linearization). Corpus:
     `property-store-missing-define-race.js`. **KNOWN RESIDUAL (OPEN):** the
     INDEXED Missing add stays on putDirectIndex verbatim — a racing indexed
     defineProperty forcing a sparse-map/SlowPutAS conversion cannot be made
     conditional without new OM machinery; recorded here so U-T14's close
     audit sees it (the GIL-on body has the identical shape, so flag-off/
     GIL-on behavior is unchanged).
  4. **ENTRY GATE finding (major, PARTIALLY CONFIRMED):** the claim that this
     record "presents the gate as satisfied" was FALSE (the original entry
     already recorded the missing verdict and the no-PROMOTE arm); CONFIRMED
     that the obligation was mis-timed — re-scoped above from "before U-T14
     close" to HARD before U-T11 ENTRY, with the SPEC-ungil-history
     deferral-ruling requirement added.
  GIL-on/flag-off after the amend: still byte-identical — every touched
  body is reached only via the vm.gilOff() dispatch or
  Options::useJSThreads() accessors; putDirectForAtomicsMissingAdd is
  called only from the GilOff store body; AtomicSlotStatus gains the
  accessor-internal LockedRevalidate value (never escapes to callers).

- U-T14 (CLOSE) — LANDED in-tree. Code writes: `runtime/OptionsList.h`,
  `runtime/Options.cpp`; ledger writes: this file. Runs LAST per the DAG.

  1. **DEFAULT FLIP (handout §T U-T14).** `Options::useThreadGIL` default
     true -> false (OptionsList.h), description updated. No shipping default
     configuration changes AT ALL: the option is only consulted under
     `useJSThreads` (every in-tree consumer is the paired
     `useJSThreads() && !useThreadGIL()` / `!useJSThreads() || useThreadGIL()`
     form — ArrayBuffer.cpp:180, JSLock.cpp:1237, SamplingProfiler.h:357,
     VMInspector.cpp:95, Watchdog.h, Debugger.cpp, VM.cpp:2648), and
     `useJSThreads` defaults false. Flag-off codegen and behavior:
     byte-identical. The U19 GIL-on oracle remains reachable via explicit
     `--useThreadGIL=1`.
  2. **U0 option-validation gate (LANDED HERE — it had NOT landed).** The
     VM.cpp:2646 and JSLock.cpp:1235 comments asserted "U0 option validation
     refuses GIL-off without the trio upstream", but no such check existed
     anywhere (grep useThreadGIL: zero hits in Options.cpp pre-close). With
     the default flip that gap would have made plain
     `--useJSThreads=1 --useSharedGCHeap=1`-less configs... still GIL'd only
     by accident of the trio terms in isGILOffProcess(), while the
     SHORT-FORM derivations (ArrayBuffer.cpp:180, VMInspector.cpp:95,
     SamplingProfiler.h:357 — `useJSThreads && !useThreadGIL` WITHOUT trio
     terms) would have DIVERGED from isGILOffProcess() (mixed-mode
     inconsistency). Landed in Options::notifyOptionsChanged immediately
     after the M_opts2 trio normalization: GIL-off without
     {useVMLite, useSharedAtomStringTable, useSharedGCHeap} forces
     useThreadGIL=1 (ANNEX U0C wording: "refused at option validation
     (forced useThreadGIL=1)"). This makes every short-form derivation
     equivalent to VM::isGILOffProcess() again. Flag-off: branch
     unreachable. Explicit useThreadGIL=1 never enters it (U19 unaffected).
  3. **Flag-off delta (a)/(b)/(b2) re-audit — EXECUTED (static).**
     - (a) LLInt Group-3 gilOffProcess branches: **ZERO in tree** (grep
       llint/ + offlineasm/ for gilOff|vmLite|loadVMLite: no matches; the
       only LLInt thread branches are the PRE-ungil R1.e
       useJSThreads/ifJSThreadsBranch set, LowLevelInterpreter64.asm:1609).
       The delta-(a) budget is UNCONSUMED and no LLInt re-baseline was ever
       consumed — flag-off LLInt codegen is trivially byte-identical.
       Consequence recorded as ACTIVATION BLOCKER AB-1 below.
     - (b) atomicsWaitImpl's useJSThreads branch present
       (AtomicsObject.cpp:514ff consumers at :536/:603/:657/:748) —
       sanctioned, unchanged.
     - (b2) §N.5 twin intrinsics
       (@atomicInternalFieldClaim/@atomicInternalFieldPublish): **NOT
       landed** (zero matches repo-wide incl. builtins/ and bytecompiler/).
       Delta (b2) unconsumed; its golden re-baseline never consumed; the
       §N.5 flag-off microbench gate is vacuous. **r17 F5 rule ("no host-op
       call reachable gilOffProcess=false") — VACUOUSLY SATISFIED** (the
       mode-keyed lowering does not exist; nothing emits the gilOff host-op
       arm). Recorded as part of AB-8 (U-T13 §N.5 leg absent).
     - No OTHER flag-off codegen/bytecode-shape delta found: builtins/,
       bytecompiler/, bytecode/ carry zero gilOff tokens; all ungil C++
       branches are vm.gilOff()/VM::isGILOffProcess()-keyed host code (not
       codegen shape); JIT-side emission (AssemblyHelpers loadVMLite, the
       U-T4b FTL dual arms) is reached only from gilOff-mode compilation
       per the U-T4a/U-T4b records. **VERDICT: permitted-delta-list
       compliance PASS statically; byte-identity still requires the
       golden-disasm gate at Build (no builds this round).**
  4. **U0/U0b/U0c gate mechanisms — verified PRESENT in-tree.** U0 = item 2
     + the ctor designation CAS (VM.cpp:392-401 ->
     Heap::tryDesignateStickySharedServer, Heap.cpp:4166) +
     verifyStickySharedServerDesignation (VM.cpp). U0b = loser spawn
     refusal (ThreadManager.cpp:336-358, RangeError; fail-safe
     over-refusal arm recorded there) + the loser main-carrier install
     escape (JSLock.cpp:1237). U0c = ctor-top immutable m_gilOff + eager
     winner noteSharedServerSticky at clientSet()==1. The U0b/U0c CORPUS
     arms (obligations item 8(c): two-VM construction under gilOffProcess,
     loser spawn RangeError, loser embedder entry EXECUTES JS,
     compile-heavy-then-first-spawn) — EXECUTION DEFERRED TO BUILD.
  5. **U19 full oracle / TSAN / amplifier battery — DEFERRED TO BUILD**
     (this round is no-build by orchestration). Exact configs owed: U19 =
     full corpus at {useJSThreads=1, useThreadGIL=1} green and UNCHANGED
     except SD6/SD7; all GIL-off-only SDs via //@ runThreadsGILOff/GILOn
     with OLD expectations GIL-on; TERM1 VM-wide terminate arms. Battery =
     the per-task standing arms (handout PER-TASK GATE LIST + EXIT1.8
     r28-r31 arms, SB1.6/ISB1.6/§N.5 arm64-hardware arms) plus the
     U-T12-recorded deferred amplifiers. These are RECORDED AS DEFERRED,
     NOT AS PASSES.
  6. **U20 lint close.** No automated lock-order lint tool exists in tree
     (Tools/Scripts has none; recorded residual, owner: Build/CI phase).
     Manual close pass over the named rule set: outer-rank sibling rows
     ftlOSRExitGenerationLock / ftlLazySlowPathGenerationLock recorded
     (this file, U-T4b entry — the binding rank record); the §E.4
     settle/wake-under-rank-3 table (JSLock.cpp:925-940) — all frozen rows
     COMPLIANT as landed; job-slot / LZ2.5 / WS1.4 / SB1.6 rule markers
     present at their owning sites (VMLite.cpp, VMManager.cpp normative
     contract blocks). This is a marker-presence pass, not a proof; the
     automated lint remains the discharge vehicle.
  7. **IU disposition completeness — scan executed.** Tables (ii)-(v) were
     EXECUTED IN CODE as doc-of-record comment blocks rather than as rows
     here: (ii) §F.2 predicate split + consumer rulings — VM.cpp:271ff/
     :796/:998 + JSLock.cpp token machinery; (iii) §E.4 settle-site
     lock-context table — JSLock.cpp:925-940; (iv) §A.1.7 — seed rows here
     + SamplingProfiler.h:323-360 carrier-only capture record; (v)
     §E.1b.4 host-hook dispositions — JSGlobalObject.cpp:680ff (+ SD15
     tracker rows :4267/:4289). Those blocks are the binding records; the
     skeleton tables above now POINT at them instead of being unfilled
     obligations.
     **Residual OPEN list at close (activation blockers; each named):**
     - **AB-1 (BLOCKER): LLInt Group-3 emission absent.** No JSCConfig
       gilOffProcess byte (JSCConfig.h unmodified), no LLInt loadVMLite
       emitter, no delta-(a) storage-selection branches (obligation 9b,
       U-T3). Under gilOffProcess LLInt would read/write VM-block Group-3
       state while C++/JIT use per-lite storage — UNSOUND, and unlike the
       JIT tiers there is NO in-LLInt tripwire.
     - AB-2: Darwin loadVMLite vmLiteTLSKey absent — gilOff JIT compilation
       RELEASE_ASSERTs on Darwin (fail-stop, acceptable).
     - AB-3: A16-ext lite-resident copies (U-T4b OPEN 1) —
       RecordRegExpCachedResult gilOff FTL compile DFG_CRASHes (fail-stop);
       ArithRandom/HasOwnProperty conservative fallbacks live.
     - AB-4: DFG/Baseline scratch-bake legs untouched (U-T4a boundary).
     - AB-5: HeapClientSet.cpp:69 RELEASE_ASSERT not wired (ledger row 6;
       I13 inner-CAS interim backstop stands).
     - AB-6: indexed Missing-add residual (U-T10 amend item 3) — GIL-on
       shape identical; acknowledged per its own record.
     - AB-7: OM Task-14 bench verdict + §D.2 gate-deferral ruling still
       unrecorded in INTEGRATE-objectmodel §46 / SPEC-ungil-history.md
       (U-T10 obligation, re-scoped to before U-T11 ENTRY — U-T11 has
       landed, so this is now a RETROACTIVE bookkeeping breach to be
       discharged at Build; the no-PROMOTE arm remains the landed shape).
     - AB-8: §N.5 twin intrinsics + mode-keyed lowering absent (U-T13's
       §N.5 leg) — GIL-off concurrent generator/async resume keeps the
       UNSYNCHRONIZED landed plain sequences: a REAL race GIL-off, hidden
       only while AB-1 blocks activation anyway.
     *(GIL-removal review round — the following rows were verified missing
     from this list even though each is recorded somewhere in a code
     comment; the close ruling's safety argument is re-stated against the
     COMPLETE inventory below.)*
     - ~~AB-9: JSThreadsSafepoint gilOff reroute~~ **CLOSED by the review
       round**: stopTheWorldAndRun now routes gilOff Class-A fires to
       jsThreadsThreadGranularStopTheWorldAndRun (the §A.3.3 licensed
       edit), and JSThreadsSafepoint::worldIsStopped() gained the §J.8
       thread-granular disjunct. This also re-validates the §A.3.4 license
       for the U-T5 M7-tripwire deletion (VMEntryScope.cpp): gilOff stop
       requests now reach the protocol that replaced the premise; the
       gilOn stub keeps its sampled entered-VM tripwire unchanged.
     - ~~AB-10 (BLOCKER): haveABadTime K.5 Class-4 stop absent~~ **CLOSED
       by the closeout round (item 4)**: haveABadTime routes its whole
       body (post-arbitration isHavingABadTime re-check + landed body as
       haveABadTimeImpl) through JSThreadsSafepoint::stopTheWorldAndRun
       when gilOff — the §A.3 conductor already carries the Class-4 shape
       (HBT4 order, AB-21 in-window own-client access re-acquire = HBT3.2,
       HBT2.2 I14 no-GC-in-window bracket; in-window GC initiation defers
       via the IT-4 arms). Heap-side shared-server asserts gained the
       conductor disjunct (MarkedSpace stop/resumeAllocating, WeakSet
       sweep/shrink, Heap::sweepNextLogicallyEmptyWeakBlock — same shape
       as LocalAllocator/BlockDirectory AB18-D). RESUME-SIDE staleness
       (the real corpus failure: a mutator parked at a poll resumed its
       compiled loop and stored contiguous-addressed into the converted
       ArrayStorage butterfly, +2-slot shift): flag-on CheckTraps now
       clobbers the heap in DFGClobberize/AI (a park admits a foreign stop
       window — no heap fact survives a poll), and handleCheckTraps emits
       the SPEC-jit I21 trailing InvalidationPoint (ExitOK re-validated).
       lastArraySize (JSObject.cpp) relaxed-atomicized (N-mutator
       ArrayStorage growth, advisory heuristic). Verified:
       havebadtime-vs-indexed-fastpath.js 50/50 amplified standalone,
       120/120 load6, 10/10 TSAN zero-report, corpus 93/0 GIL-off + 94/0
       GIL-on, identity 40/40.
     - **AB-11: ThreadObject spawn-overload migration.** gilOffProcess
       refuses EVERY spawn including the winner VM's
       (ThreadManager.cpp allocateSpawnedThreadState VM-blind form returns
       null -> RangeError): fail-safe over-refusal, but "N mutators in
       parallel" is structurally unreachable until ThreadObject.cpp
       migrates to the VM-aware overload.
     - **AB-12: E2A drain-loop wiring + U-T9-INT1 keepalive edits.**
       openThreadInbox / runSpawnedThreadDrainLoopAndClose /
       AsyncTicket::armKeepalive have ZERO callers; until the threadMain
       wiring + the four countsKeepalive edits land, GIL-off threads exit
       at fn-return (api 4.6.2 fallback semantics, NOT the chartered
       SPEC-ungil E.2/SD1 close semantics) and the E/SD16/SD17 corpus arms
       cannot be claimed.
     - **AB-13: GILDroppedSection spawned-arm J.3 split + §G consumer
       re-points.** Every spawned GIL-off park site (contended lock.hold,
       cond.wait, property Atomics.wait, join) reaches
       unlockAllForThreadParking's RELEASE_ASSERT(currentThreadIsHolding-
       Lock()) and fail-stops; the mayBlockSynchronously §G predicate has
       zero consumers re-pointed (ThreadAtomics.cpp/LockObject.cpp/
       ConditionObject.cpp/AtomicsObject.cpp owed). The in-code ordering
       constraint stands: the §C.4 lift MUST NOT land before the split.
     - ~~AB-14: VMEntryScopeInlines.h entry-gate re-key; HandleSet Strong
       seam wiring~~ **CLOSED by the review round**: the ctor/dtor fast
       paths are re-keyed on the per-lite record when gilOff (nested and
       sequential re-entry work; tearDownSlow runs), and Strong.h/
       StrongInlines.h now route allocate/free/set-slot through the locked
       strongHandle* seams (HandleSet.h declarations added).
     - **AB-15: SD7 generated-code arm.** The review round landed the §I
       item (1) C++ ctor/compile-surface gate (JSWebAssemblyHelpers.h
       throwIfWebAssemblyRefusedOnSpawnedThread, wired at the Module/
       Memory/Table/Global/Tag/Instance constructors and compile/
       instantiate/validate/streaming/promising entry points, BOTH GIL
       modes per SD7). Item (2) remains open: VMLite::isSpawned L2 append,
       JSToWasm spawned-TS prologue emission, jsCallICEntrypoint()
       nullptr under useJSThreads — without it a spawned thread can still
       WARM-call an exported wasm function created on the carrier.
     - **AB-16: RegExp.h ovector routing** (RegExp.cpp banner OPEN (1)):
       every gilOff global match RELEASE_ASSERTs until ovectorSpan()
       routes to regExpGilOffPerThreadMatchOvector — a HARD U-T9 entry
       gate (fail-stop, not silent).
     - **AB-17: per-lite traps + stack limits (VMTraps.h activation
       checklist items 1-4).** perThreadTrapsIfExists still aliases the VM
       trap word (per-thread termination/defer scopes are phase-1
       semantics; the §A.3 conductor re-fires every sample to compensate),
       and VM::updateStackLimits still writes the single VM-level
       softStackLimit (memory-safety grade under N-parallel entry).
       *GIL-removal review round 3: item 3 is no longer a SILENT hole —
       VM::updateStackLimits now RELEASE_ASSERTs (gilOff arm) that no
       OTHER lite of this VM is entered before publishing the shared soft
       limit. The assert is deleted by the same change that lands the
       §A.2.2 per-lite soft-limit reroute.*
       *CORRECTION (review round 4): the round-3 claim that the
       updateStackLimits walk was "the process-wide interim fail-stop for
       ANY second concurrent entry" was WRONG on two counts: (a) TOCTOU —
       the walk samples sibling entryScopes at lock/token-acquisition
       time, but the sibling-visible entered record is only published
       later in VMEntryScope::setUpSlow, so two concurrent entrants could
       both pass it pre-publication and then both run; (b) re-entry — a
       token-holding thread that tore down its VMEntryScope and re-enters
       JS through a fresh one never re-runs updateStackLimits at all. The
       DETERMINISTIC fail-stop now lives in VMEntryScope::setUpSlow's
       gilOff arm: the no-other-entered walk and the entered-record store
       run under ONE VMLiteRegistry-lock hold, so the second concurrent
       top-level entry aborts at publication regardless of interleaving.
       The updateStackLimits walk is RETAINED as an earlier (advisory)
       trip point only. Both asserts are deleted together by the §A.2.2
       reroute.*
       *ORDERING DEPENDENCY (GIL-removal round 5, BLOCKER-grade if
       violated): the setUpSlow tripwire's deletion trigger (§A.2.2
       soft-stack-limit reroute) and the VMTraps.cpp TERM1.2 interim's
       retirement trigger (§A.2.1 per-lite trap words) are DIFFERENT
       chartered changes, and the TERM1.2 "last observer" clear is only
       sound while the tripwire blocks a second concurrent entry: the
       entered-predicate is a live per-lite VMEntryScope record while the
       delivery obligation is TOKEN-scoped, so a token-holding sibling
       between entry scopes (fn-return teardown -> completion drain,
       between drainMicrotasks iterations) would re-enter with the shared
       NeedTermination bit already cleared — a silently LOST termination.
       Now enforced MECHANICALLY in code, not by comment: the setUpSlow
       walk is keyed on BOTH legs (`perLiteSoftStackLimitRerouteLanded`
       constant flipped by the §A.2.2 change, OR `perThreadTrapsIfExists
       (lite) == &vm.traps()` — the §A.2.1 alias probe), so lifting AB-17
       before §A.2.1 lands leaves the refusal in force instead of going
       live silently.*
     - **AB-18 (review round 3; was recorded ONLY in the
       WaiterListManager.cpp banner and missing from this list): SD6
       second half — the D8 single-flight gate deletion in
       AtomicsObject.cpp.** SPEC-ungil §C.6 (SD6, a BOTH-GIL-MODE delta)
       requires the per-wait-node allocation (LANDED, WaiterListManager.cpp)
       AND deletion of the D8 gate; AtomicsObject.cpp:501ff still carries
       syncTAWaitGateLock/vmsWithSyncTAWaitInFlight and still throws on a
       second concurrent main-thread TA wait. Until the gate is deleted
       (ordered AFTER the AB-13 split per the §C.4 constraint), the SD6
       corpus/U19 expectation ("second waiter parks on its own node") is
       NOT claimable in EITHER GIL mode, and the A26 termination-wake
       bypass coexists with the gate it was paired against.
     - **AB-19 (review round 3; was recorded ONLY in the Heap.cpp
       conductTIDRebiasUnderSharedStop banner and missing from this list):
       multi-VM gilOffProcess rebias tripwire re-key.** The §D.1 rebias
       fire loop routes through fireAllUnderClassAStop, whose run-inline
       branch runs assertAlreadyStoppedEvidenceCoversEveryMutator — a
       phase-1 tripwire whose premise (single entered VM) U0b retires
       (loser VMs may stay entered). In any multi-VM gilOffProcess
       process the FIRST rebias is a deterministic process abort. Owner:
       JSThreadsSafepoint.cpp (exempt entered loser-VM mutators when the
       stopped server is the U0c winner's heap). Until it lands: rebias
       (not just the two-VM amplifier arm) is only sound in SINGLE-VM
       gilOffProcess processes.
     - **AB-20 (review round 3): drainMicrotasksForGlobalObject sibling-
       lite clears.** The Bun-additions VM entry point now clears the VM
       default queue AND the CURRENT lite's queue when gilOff, but
       SIBLING lites' per-thread queues cannot be cleared cross-thread
       without breaking the I11 owner-only queue discipline. Needed: a
       per-lite "clear for global G" request word serviced at each
       owner's next drain (or a global-object liveness check at dequeue).
       Until then, a gilOff embedder clearing a global on one thread can
       still see stale-context microtasks drain on OTHER threads.
     - ~~AB-21 (review round 3; found by the FIRST actual gilOff boot —
       the U-T14 RUN items were DEFERRED-TO-BUILD and had never
       executed): gilOff single-carrier boot dies at the first Class-A
       watchpoint fire~~ **CLOSED by review round 4**: the §A.3
       thread-granular conductor (VMManager.cpp
       jsThreadsThreadGranularStopTheWorldAndRun) now RE-ACQUIRES its own
       client's heap access for the window (after the quiescence
       predicate is satisfied, before work(); released again before
       resume publication) — the conductor is exempt from the AHA §A.3 /
       Mode-stop gates and from allEnteredThreadsAreQuiescent (HBT2.1),
       and GSP cannot be pending under the JSThreadsStopScope GCL
       bracket, so the re-acquire neither parks nor invalidates the
       predicate, and the Heap::deferralDepthSlot
       `hasHeapAccess() || worldIsStoppedForAllClients()` asserts inside
       Class-A fire bodies are satisfied the honest way (real access).
       Debugger.cpp's runDebuggerWalkWithSpawnedThreadsStopped inherits
       the fix. Original record: `--useJSThreads=1 --useSharedGCHeap=1
       --useThreadGILOffUnsafe=1 -e 'print("hi")'` (debug) asserts
       `client->hasHeapAccess() || worldIsStoppedForAllClients()`
       (Heap::deferralDepthSlot, via DeferGCForAWhile) with stack:
       DeferredStructureTransitionWatchpointFire dtor ->
       WatchpointSet::fireAllSlow -> fireAllUnderClassAStop ->
       JSThreadsSafepoint::stopTheWorldAndRun (the AB-9-closure reroute)
       -> drainClassAFireQueue -> fireAllNow -> fireAllWatchpoints ->
       DeferGCForAWhile. The thread-granular stop runs the queued fire
       bodies at a point where the conductor's client has no heap access
       and the world is not stopped-for-all-clients, so any fire body
       that touches the heap trips the access assert. Owner:
       JSThreadsSafepoint.cpp (hold/reacquire the conductor's heap
       access across the fire-queue drain, or extend the stop witness to
       satisfy the deferral-slot predicate). Even SINGLE-carrier gilOff
       smoke runs are blocked until this lands.
     - **AB-22 (review round 4; obligation-1 residue, was MISSING from
       this list): raw vm.entryScope consumers.** The U-T5 shadow drop
       (VMEntryScope.cpp) discharged "shadow dropped" but NOT "every raw
       consumer re-pointed" (IU obligation 1). Round 4 re-pointed the
       crash-grade sites through VM::currentThreadEntryScope() (GIL-on
       byte-identical): CallFrame.cpp convertToZombieFrame (was a
       deterministic null-deref on the gilOff exception-unwind
       no-JS-throw-origin arm — reachable single-carrier, before
       AB-11/12) and CallFrame.cpp globalObjectOfClosestCodeBlock (was a
       silent nullptr GIL-off); SamplingProfiler.cpp noticeVMEntry's
       ASSERT (fired on the first gilOff entry with the profiler on,
       debug); and WIRED the SamplingProfiler.h
       shouldBindCurrentThreadAsJSCExecutionThread consult into
       noticeCurrentThreadAsJSCExecutionThreadWithLock (release-build
       hole: a spawned thread could bind and later be suspend-and-walked
       while free-running). REMAINING OPEN under this row: takeSample's
       `m_vm.entryScope` gate is constantly false gilOff, so the profiler
       is deliberately DORMANT on gilOff VMs (documented at the gate) —
       AUD1.K1's "carrier-only v1" does NOT yet deliver carrier samples
       gilOff; needs the per-lite Group-3 registry-resolve + the
       WhileTargetSuspendedScope takeSample wiring (U-T8d .cpp half).
       The U-T14 close item 7 claim that table (iv) was "executed IN
       CODE" at SamplingProfiler.h:323-360 is corrected accordingly: that
       block records the RULING; the .cpp halves were pending until this
       round (bind consult) / remain pending (suspend scope).
     - **AB-23 (review round 4): gilOff main-carrier identity re-key +
       default-queue drainer residual.** GIL-off, m_mainVMLite is never
       installed (A36), so every `lite != m_mainVMLite.get()` /
       `lite == vm.mainVMLite()` "main carrier" predicate was dead and
       VM::m_defaultMicrotaskQueue was an undrained sink. Round 4
       re-keyed the three predicates (VM::queueMicrotask,
       VM::drainMicrotasks, JSGlobalObject.cpp perLiteRealmRoutingLite)
       on the MAIN THREAD's carrier (lite->ownerHasNoTlsDtor, the A36 r32
       registration-fixed bit; that carrier also borrows &vm.clientHeap,
       F1B), so the main thread's carrier owns the in-object realm stream
       and the VM default queue — restoring the JSGlobalObject.cpp banner
       claim and giving the default queue a drainer. RESIDUAL OPEN: a
       gilOff VM entered ONLY from non-main threads still has no
       default-queue drainer for no-lite-window/off-thread enqueues
       (same service-request shape as AB-20's sibling-clear word; one
       mechanism can serve both).
     - **AB-24 (review round 4; found by the post-AB-21 smoke — the boot
       now gets PAST the Class-A fire and trivial scripts run): gilOff
       debug builds die at the first JIT-path allocation validation.**
       `--useJSThreads=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 -e
       'for (let i=0;i<1e5;i++){({}).x=i}'` (debug) asserts
       `heap.worldIsStoppedForAllClients() ||
       heap.mutatorSlowPathLock().isHeld()`
       (BlockDirectory::assertIsMutatorOrMutatorIsStopped, the
       isSharedServer arm) with stack operationNewObject ->
       JSFinalObject::create -> JSObject::finishCreation ->
       JSCell::classInfo validation -> the I5b lock-free directory-bits
       funnel. Cause: gilOff the server is shared from BOOT (U0c eager
       noteSharedServerSticky at clientSet()==1), so a FREE-RUNNING
       mutator's lock-free bits reads reach the shared-server assert arm,
       which only admits stopped-world or MSPL holders — phase-1 never
       exercised this shape (the GIL kept mutators out of each other's
       addBlock resize window by construction; single-client GIL-on runs
       take the pre-sticky arm). For a SINGLE entered mutator (the only
       shape AB-17's tripwire admits today) the read is benign and the
       assert is over-strict; for N mutators the underlying race
       (lock-free m_bits read vs a sibling addBlock resize, I5b) is REAL
       and needs the heap workstream's ruling (admit an access-holding
       client when the §A.3.1 entered count is 1? take MSPL on the
       validation paths? bits-epoch?). Owner: heap workstream
       (BlockDirectory.cpp is its surface). Until it lands: gilOff DEBUG
       smoke is limited to scripts that stay off the assert-bearing
       allocation validation paths (trivial boots pass post-AB-21);
       release builds do not assert but inherit the open I5b question for
       N>1. Fail-stop, not silent.
     - **AB-25 (GIL-removal round 5): gilOff cross-thread mutation of the
       unlocked VM default microtask queue.** MicrotaskQueue/
       MarkedMicrotaskDeque is a plain unlocked Deque, and under the AB-23
       re-key the VM default queue is OWNED (enqueued/drained/cleared) by
       the MAIN thread's carrier. Two gilOff paths could still touch it
       from arbitrary threads: (1) VM::drainMicrotasksForGlobalObject's
       unconditional `m_defaultMicrotaskQueue->clearForGlobalObject()`
       arm, and (2) VM::queueMicrotask's no-lite/foreign-VM-lite enqueue
       fallthrough (the AB-23 "no-lite-window enqueues" residual) — both
       unsynchronized Deque mutations racing the main carrier's
       performMicrotaskCheckpoint, corruption-grade. INTERIM FAIL-STOP
       LANDED (round 5): both gilOff arms now
       `RELEASE_ASSERT(WTF::isMainThread())` before touching the default
       queue (the main carrier IS the main thread's — ownerHasNoTlsDtor,
       A36 r32), so the racy shape aborts loudly instead of corrupting.
       Unreachable today (AB-11/AB-12 block spawns; AB-17 blocks second
       entry) — same status as the other AB rows. RETIRED by the per-owner
       service-request word already chartered for AB-20's sibling clears
       and AB-23's missing-drainer residual: ONE mechanism (cross-thread
       "clear for global G" + handoff enqueue, serviced at the owner's
       next drain) discharges AB-20, the AB-23 residual, and this row.
       Flag-off/GIL-on: branches not taken, byte-identical.
     - **AB-26 (GIL-removal round 5; was recorded ONLY in the
       ArrayBuffer.cpp banners — the same omission class round 3 corrected
       for AB-18/AB-19): annex N6 arm 4 wasm-grow stop conduction.** A
       relocating MemoryMode::BoundsChecking Wasm::Memory grow REPLACES
       the base word; N6 requires the relocation to run under a heap §10
       stop with the old mapping quarantined to the NEXT stop. Only the
       QUARANTINE half is landed (ArrayBuffer.cpp
       refreshAfterWasmMemoryGrow + quarantineStaleWasmMappingGILOff: the
       torn pair {pre-grow length, pre-grow base} never dereferences an
       unmapped base). The STOP CONDUCTION half — which alone excludes the
       complementary torn pair {post-grow length, PRE-grow base}, an
       out-of-mapping dereference (MEMORY-SAFETY grade) — belongs to
       Wasm::Memory::grow's BoundsChecking arm and is NOT YET ESTABLISHED
       ("OPEN DEPENDENCY, blocks U-T13 sign-off" in the .cpp banner).
       LAUNCH BLOCKER for the full-trio configuration: reachable at
       activation even with the SD7 spawned-wasm-execution refusal
       (AB-15), because spawned-thread JS TypedArray READERS of a wasm
       Memory's buffer race a carrier-side grow — AB-15 covers execution,
       not reads. Owner: U-T13 / wasm workstream; surface:
       Wasm::Memory::grow (BoundsChecking arm) + the
       refreshAfterWasmMemoryGrow publication.
     **CLOSE RULING (re-stated against the complete list):** the default
     flip is safe-by-construction ONLY because the U0 validation now
     REFUSES the gilOff shape outright unless the explicit development
     escape hatch useThreadGILOffUnsafe=1 is ALSO set (landed by the
     review round; previously `--useJSThreads=1 --useSharedGCHeap=1` —
     two flags, since M_opts2 auto-forces the other two — produced a live
     gilOff process against AB-1's silent LLInt split-brain with NO
     in-code fail-stop). Build/Verify MUST treat AB-1, AB-8, AB-11..AB-13
     (AB-10 closed at the closeout round, item 4),
     AB-15..AB-20 (AB-21 closed at round 4), AB-24, AB-25, AB-26, and the
     OPEN residuals of AB-22/AB-23 as LAUNCH BLOCKERS for running the
     full-trio configuration, and the U0 refusal clause (Options.cpp) is
     only deleted when this list is discharged and the §B verification
     ladder (U19 oracle, TSAN, amplifier battery, golden disasm, B.5
     bench) has actually run.
     **§B LADDER ENTRY CRITERIA (GIL-removal round 5, explicit):** AB-13
     (GILDroppedSection spawned-arm split — every spawned park aborts at
     JSLock::unlockAllForThreadParking until it lands), AB-16 (RegExp.h
     ovector reroute — every gilOff global match aborts), and AB-17 (the
     setUpSlow second-entry refusal, now also alias-keyed per the row
     above) are ENTRY CRITERIA for the FIRST verification-ladder rung
     that claims parallel JS execution. Because those three fail-stops
     mean any "gate-green" result obtained on THIS tree can only have
     exercised serialized or single-entered shapes, NO ladder rung, tier
     sign-off, or coverage claim may cite gilOff runs from this tree as
     parallel-mutator coverage, and useThreadGILOffUnsafe stays treated
     as non-functional for coverage purposes. A smoke arm that needs two
     threads in JS requires landing the §A.2.2 per-lite reroute (and per
     the AB-17 coupling, §A.2.1) first — never relaxing the asserts.
     **MILESTONE GATE STATUS (review round 3, explicit):** the GIL-removal
     milestone deliverable — N mutators actually executing JS in parallel
     in one VM — is **NOT MET** by this tree. With AB-11/AB-12 open, every
     spawn under gilOffProcess is refused (RangeError), so the GIL-off
     semantic deltas (SD1-SD5, SD8-SD19), the §E drain loop, §E.3
     keepalive, and the GIL-off corpus are structurally unrunnable; and
     the round-3 updateStackLimits fail-stop deliberately aborts any
     second concurrent ENTRY while AB-17 is open. U-T9/U-T11 rows record
     mechanism LANDED (code present, compiles, flag-off inert), not
     behavior DELIVERED — the milestone must not be reported as met until
     the AB list above is discharged and the §B ladder has run GIL-off.
  8. **§F.6 close items** — see table (vi) below (rewritten this round);
     the in-code row table (JSLock.cpp:941-955) keeps its OPEN markers for
     the Bun-side audits, which CANNOT be executed from this repository.
  **Spec-conflict record (per task instructions):**
  - (i) The orchestrator task card gave U-T14 an EMPTY owned-file list
    while the handout mandates in-tree deliverables (the flip; the U0
    gates). Resolved per the authority clause (handout > prompt) with the
    minimal write set {OptionsList.h, Options.cpp, INTEGRATE-ungil.md}.
  - (ii) VM.cpp/JSLock.cpp comments described U0 validation as already
    landed; it was not. Landing it here makes those comments true — no
    supersession, but the discrepancy is recorded (the comments were
    written against the spec, not the tree).
  - (iii) The handout's U-T14 RUN items (U19, TSAN, amplifiers, golden
    disasm, §B.5 composite, arm64-hardware arms) cannot execute in a
    no-build documentation+code round; recorded as DEFERRED-TO-BUILD
    above, never as passes.

  **Adversarial-review AMEND (round 2; write set per spec-conflict (i)):**
  - Finding R2-1 (LLInt delta-(a) branches + JSCConfig gilOffProcess byte
    absent; "confirms AB-1") — **TRUE FACTS, REFUTED AS A U-T14 DEFECT /
    NO NEW ACTION.** The absence is exactly AB-1, found and recorded BY
    this task's own delta re-audit (item 3) with the same consequence
    analysis (Group-3 VM-block vs per-lite divergence, no in-LLInt
    tripwire), and the close ruling already binds Build/Verify to treat
    AB-1 as a LAUNCH BLOCKER for any full-trio run. Owner is U-T3
    (obligation 9b), per the handout's own re-baseline schedule ("at
    U-T3"); llint/, offlineasm/ and JSCConfig.h are outside U-T14's write
    set, so neither the emission nor the suggested LLInt-entry tripwire
    can land here. No shipping config reaches the unsound state (trio
    defaults false; U0 forces GIL back on), so this stays an activation
    blocker, not a regression introduced by the flip.
  - Finding R2-2 (§N.5 twin intrinsics absent; "confirms AB-8") — **TRUE
    FACTS, REFUTED AS A U-T14 DEFECT / NO NEW ACTION.** Identical
    disposition: recorded as AB-8 by item 3 ((b2) unconsumed) and item 7,
    and the close ruling ALREADY names AB-8 (alongside AB-1) a hard
    LAUNCH BLOCKER for N>1 mutator runs — the reviewer's requested
    treatment is the landed treatment. Owner: U-T13's §N.5 leg
    (builtins/, bytecompiler/ outside this write set). The race is real
    GIL-off but unreachable while AB-1 blocks activation; the U19 GIL-on
    oracle remains the only legal config for the generator corpus until
    (b2) lands.
  - Finding R2-3 (gilOffProcess re-derived live at every call site; no
    process latch; setOptions can flip the derivation mid-process) —
    **CONFIRMED, FIXED HERE (Options.cpp, in-write-set).** Verified: the
    short forms (ArrayBuffer.cpp:180, VMInspector.cpp:95,
    SamplingProfiler.h, JSLock.cpp) and VM::isGILOffProcess() re-read
    Options live while VM::m_gilOff / Watchdog::m_gilOff latch at
    construction; Options::setOptions is callable after
    Options::finalize() (only Config::permanentlyFreeze — embedder
    optional — blocks it) and re-runs notifyOptionsChanged, whose U0 arm
    can itself force useThreadGIL 0 -> 1, silently splitting JSLock arm
    selection / detach-table consultation across consumers. FIX: a
    write-once shadow latch in notifyOptionsChanged — the derivation may
    change freely BEFORE g_jscConfig.options.isFinalized (Options::
    finalize runs at the tail of JSC::initialize, strictly before any VM
    can exist; the jsc shell parses all options pre-initialize), and any
    post-finalization notifyOptionsChanged that would CHANGE the
    derivation RELEASE_ASSERTs (fail-stop, per the reviewer's suggested
    shape). Flag-off + U19: derivation constantly false, assert
    unreachable, host-C++ only — no codegen-shape delta (delta list
    untouched). The JSCConfig gilOffProcess byte (U-T3) SUBSUMES this
    latch when it lands; the in-code comment records the replacement
    obligation. AB-1's JSCConfig leg remains open — this latch is the
    interim immutability backstop ANNEX U0C asks for, not the byte.

  **Adversarial-review AMEND (round 4 — GIL-removal blocker/major sweep;
  write set: CallFrame.cpp, SamplingProfiler.{h,cpp}, VMEntryScope.cpp,
  VMManager.cpp, VM.cpp, JSGlobalObject.cpp, VMLiteInlines.h,
  HandleSet.{h,cpp}, this file, SPEC-ungil-history.md,
  INTEGRATE-objectmodel.md):**
  - R4-1 (raw entryScope consumers; BLOCKER, CONFIRMED) — fixed + new row
    AB-22 (re-points landed; profiler-dormant residual recorded there).
  - R4-2 (updateStackLimits fail-stop TOCTOU + re-entry hole; MAJOR,
    CONFIRMED) — fixed: the deterministic tripwire moved into
    VMEntryScope::setUpSlow's registry-lock hold; AB-17 round-3 text
    corrected in place.
  - R4-3 (milestone structurally unreachable; BLOCKER as a REPORTING rule)
    — TRUE FACTS, NO NEW CODE ACTION: this is exactly the round-3
    MILESTONE GATE STATUS: NOT MET ruling above (AB-11/AB-12/AB-13/AB-17
    open; mechanism-LANDED rows are code-present, not
    function-delivered). The reporting rule stands.
  - R4-4 (conductor runs Class-A fires with no heap access; BLOCKER,
    CONFIRMED = AB-21) — fixed, AB-21 CLOSED (see the row).
  - R4-5 (useThreadGIL default flipped before the milestone gate; MAJOR,
    CONFIRMED as a recorded plan-ordering deviation) — discharged the
    DOCS way: explicit orchestrator ruling appended to
    SPEC-ungil-history.md superseding UNGIL-PLAN §J's flip-at-gate
    ordering and naming the Options.cpp U0 refusal clause +
    useThreadGILOffUnsafe hatch as the binding interim gate (deletable
    only with the AB list, per the close ruling above).
  - R4-6 (mainVMLite-keyed routing predicates dead GIL-off; MAJOR,
    CONFIRMED) — fixed + new row AB-23 (ownerHasNoTlsDtor re-key;
    non-main-only-VM default-queue residual recorded there). The smoke
    run exposed a FOURTH site of the same class: VMLite::setCurrent's K4
    §VIII cross-thread-entry noter keyed on `lite != mainVMLite()`, which
    noted the MAIN THREAD's own first gilOff install and made the
    setGlobalThis/setName immutable-after-init asserts fire on
    single-threaded boot — same re-key applied (VMLite.cpp:259).
  - R4-SMOKE (executed this round; the U-T14 RUN items were never
    executed before round 3's AB-21 boot): debug gilOff single-carrier
    `print("hi")` boot now PASSES end-to-end (AB-21 + the VMLite.cpp
    re-key); uncaught-throw exits cleanly through the re-pointed unwind
    path (R4-1); the GIL-on oracle (`--useJSThreads=1 --useThreadGIL=1`)
    and flag-off smoke are unchanged-green. The first JIT-path-allocating
    script exposes the NEXT pre-existing blocker — recorded as AB-24.
  - R4-7 (AB-7 records absent; MAJOR, CONFIRMED) — discharged the DOCS
    way: §D.2 gate-deferral ruling appended to SPEC-ungil-history.md and
    the verdict-deferral record written into INTEGRATE-objectmodel §46
    (naming U-T10's locked arm + U-T11 §C.3 PWT routing as the re-review
    surfaces on a PROMOTE outcome; bench run itself stays owed at the
    first Build round — AB-7 narrows to "run the bench", no longer
    "records missing").
  - R4-8 (Strong allocate/free/set-slot outlined + per-call gilOff branch
    flag-off; MAJOR, CONFIRMED for the Strong seam) — fixed structurally:
    strongHandle* are now ALWAYS_INLINE HandleSet.h wrappers testing a
    ctor-stamped HandleSet gilOff byte and falling through to the
    pre-ungil inline list ops; only the gilOff arm calls the out-of-line
    locked *Slow entry points (HandleSet.cpp). Flag-off codegen returns
    to the inline shape + one predicted-false byte test. The reviewer's
    aggregate items (currentThreadIsHoldingAPILock de-inline, JSLock
    isGILOffProcess Options loads, retireEntryTokenForLock thread_local,
    group3Primitives branch, JSPromise/RegExp gilOff tests) remain
    individually predicted-false host-C++ branches — ADJUDICATE TOGETHER
    at the §B.5 flag-off bench + golden-disasm gate (already owed at
    Build; the JSCConfig gilOffProcess byte (AB-1/U-T3) is the planned
    replacement for the isGILOffProcess re-derivation).
    **AB17c F4 follow-up (stamp-ordering root-cause fix):** the
    "ctor-stamped HandleSet gilOff byte" above was stamped from
    `vm.gilOff()` inside Heap's construction — which runs in VM's ctor
    INIT LIST, before the ctor body's U0c designation block computes
    `VM::m_gilOff`. The byte therefore read false in every gilOff
    process and ALL Strong allocate/free/barrier traffic took the
    unlocked inline arm (latent §F.3 violation; observed as a
    double-allocated Strong slot under races/counter-lock.js — a
    spawned thread's property-wait Strong clobbered the carrier's
    in-flight UnlinkedCodeBlockGenerator codeBlock handle, crashing
    bytecode generation). Fixed by a one-shot pre-publication re-stamp:
    `HandleSet::noteOwnerVMDesignatedGILOff()` called from the U0c
    winner branch in the VM ctor, immediately after `m_gilOff = true`
    (still single-threaded, no lite registered — the byte remains
    immutable-after-publication). Audited the other two cached gilOff
    bytes (Watchdog.cpp:73, LockObject.cpp:87): both are constructed
    lazily after the VM ctor completes, so they stamp correctly.
    **AB17c F4 second root cause (IT-9 consumer landed, funnel form):**
    residual counter-lock segfault inside JIT inline allocation
    (scrambled FreeList pop, result register 0 past the empty check):
    DFG/FTL bake `JITAllocator::constant` from
    `allocatorForConcurrently<Type>`, which for ISO subspaces returned
    the main client's per-thread LocalAllocator
    (GCClient::IsoSubspace::allocatorFor is unconditionally non-null) —
    the PRE-EXISTING I11 hole the Heap.h FIX-3 carve-out documents
    ("the baked-main-client Allocator is the ... hole that IT-9
    tracks"). With the artifact executed by every lite, N threads
    popped ONE FreeList unlocked (observed in compileNewFunction's
    inline JSFunction allocation). Landed the IT-9 consumer at the
    funnel: `allocatorForConcurrently` (runtime/JSCellInlines.h)
    returns an empty Allocator when `vm.gilOff()`, so every JS-tier
    inline-allocation emitter takes its existing null-constant
    slow-path arm (AssemblyHelpers::emitAllocate appends an
    unconditional jump), and the slow path re-dispatches per-thread via
    allocationClientForCurrentThread — mirroring the §5.5
    CompleteSubspace rule (server arrays never populated) for iso.
    CompleteSubspace bake sites (auxiliarySpace, allocation profiles)
    already produced empty allocators under useSharedGCHeap.
    GIL-on/flag-off: one predicted-false compiler-side branch, baked
    artifacts byte-identical. U-T7 lite-relative TLC/iso emission stays
    the chartered re-enable path (Heap.h IT-9 note updated in place).
    **AB17c F4 third root cause (LLInt data-IC mirror tear — the
    setMonomorphicCallee KNOWN RESIDUAL, discharged):**
    int-gate-stop-budget full-JIT segfault: baseline prologue
    argument-profiling ran against a DFG CodeBlock (null
    m_argumentValueProfiles storage). The LLInt callHelper /
    doCallVarargs fast paths read the mirror triple
    (m_callee/m_codeBlock/m_monomorphicCallDestination) lock-free; a
    live monomorphic tier-up upgrade
    (CallLinkInfo::unlinkOrUpgradeImpl) rewrites codeBlock+destination
    in place with the comparand still matching, so a concurrent LLInt
    caller pairs the NEW codeBlock with the OLD entrypoint — no write
    ordering can fix a 3-load reader. Landed the chartered fix
    (SPEC-jit 5.8 F2): both LLInt sequences
    (LowLevelInterpreter64.asm callHelper + doCallVarargs) now route
    through the immutable published m_record under useJSThreads
    (existing ifJSThreadsBranch gate; null record / comparand miss
    falls to .opCallSlow, which already implements the empty-record
    semantics). Flag-off: one not-taken branch, mirror path
    byte-identical. JIT tiers were already record-routed
    (emitFastPathImpl).
    **AB17c F4 fourth root cause (DFG catch OSR-entry buffer not
    per-lite, A16/U-T4b gap):** ftl-osr-entry-catch-loop-amplifier
    failed 'torn OSR-entry locals' under --useFTLJIT=0: the A16
    per-lite catchOSREntryBufferBakedIndex reroute was landed in FTL
    only; DFG's JITCompiler::makeCatchOSREntryBuffer still baked one
    shared ScratchBuffer and ExtractCatchLocal/ClearCatchLocals baked
    its address — N threads throwing into the same hot catch block
    clobbered each other's reconstructed locals. Landed the DFG
    sibling: gilOff-mode compilations bake the registry index
    (DFGJITCompiler.cpp) and the readback nodes materialize the
    CURRENT lite's buffer via the frozen loadVMLite -> segment ->
    [index] chain (DFGSpeculativeJIT.cpp, local helper mirroring
    DFGOSRExit.cpp's). Fill side (DFGOSREntry::prepareCatchOSREntry)
    already handled the index. Flag-off/GIL-on byte-identical
    (predicted-false compiler-side branches only).
    **AB17c F4 fifth root cause (ANNEX CBI item 3 — executable
    (entrypoint, CodeBlock) mirror pair torn by live tier-up):**
    residual --useFTLJIT=0 segfaults (same baseline-prologue-vs-DFG-
    CodeBlock signature, but the torn pair came from the EXECUTABLE
    mirrors, not the call-link): every virtual-call reader loads
    m_jitCodeFor*WithArityCheck and m_codeBlockFor* as two independent
    racy words; a live installCode between them pairs a stale
    entrypoint with the new CodeBlock. Landed, per CBI item 3 ("derived
    loads go through the codeBlock pointer — address-dependent, jit
    F2") plus a writer-ordering + reader-revalidation pair for the
    baked thunks:
    (1) C++ consumers derive the entrypoint THROUGH the one CodeBlock
    snapshot under gilOff: RepatchInlines.h linkFor +
    virtualForWithFunction, LLIntSlowPaths.cpp (llint virtual/link
    slow path), DFGOperations.cpp (operationLinkDirectCall),
    JITOperations.cpp materializeTargetCode.
    (2) ScriptExecutable::installCode now retracts the gating jit-code
    mirrors FIRST for INSTALLS too (previously clears only),
    storeStoreFence-ordered before the CodeBlock replacement; the
    arity-check mirrors stay null until entrypointFor lazily re-derives
    them from the NEW jit code.
    (3) The pair-reading thunks re-validate the arity-check slot AFTER
    their CodeBlock load (mismatch => slow path / materialize path,
    which produces a matched pair via (1)): LLInt virtualThunkFor
    (LowLevelInterpreter.asm, JSVALUE64 + ifJSThreadsBranch gate), JIT
    virtualThunkFor, boundFunctionCallGenerator,
    remoteFunctionCallGenerator (ThunkGenerators.cpp, useJSThreads
    emission gate — flag-off thunk bytes unchanged).
    (4) PolymorphicCallStubRoutine::upgradeIfPossible refuses the
    in-place (slot.m_codeBlock, slot.m_target) rewrite under gilOff
    (F6: never mutate published dispatch state) — caller falls back to
    full unlink + fresh republish.
    Weak-memory reader-side ordering remains the recorded IT-8 KNOWN
    RESIDUAL (x86-64 TSO sound).
    **AB17c F4 sixth root cause (incoming-calls sentinel list torn by
    unlocked PolymorphicCallNode removals):** residual stop-budget
    segfault in CallLinkInfo::reset()'s remove() (locked linker path)
    on a half-unlinked node: PolymorphicCallNode::unlinkOrUpgradeImpl
    removed itself from the drain list WITHOUT
    s_callLinkSerializationLock, and ~CallLinkInfo (lazy-sweep context,
    unlocked) reached PolymorphicCallNode removals via clearStub ->
    unlinkForcefully. Fixed: the poly node's drain-side remove() now
    takes the link lock under gilOff (scoped before the nested locked
    unlinkOrUpgrade), and ~CallLinkInfo takes the lock around
    clearStub when a stub exists (other clearStub callers are locked
    linker paths; contract recorded at
    PolymorphicCallNode::unlinkForcefully).
    **AB17c F4 CBI consumer sweep (transient-null + matched-pair):**
    the retract-first installCode order makes m_jitCodeFor* transiently
    null; consumers that unconditionally deref'd it or paired it with a
    separately-loaded CodeBlock were converted to through-CB derivation
    under gilOff: Repatch.cpp linkPolymorphicCallImpl (observed crash:
    Ref(null) in generatedJITCodeForCall), Interpreter.cpp all six
    execute* arms (program/call/construct/eval/module; ASSERTs gated),
    JSMicrotask.cpp fast + slow entry. DirectCallLinkInfo::
    repatchSpeculatively is RELEASE_ASSERT-unreachable flag-on.
    **AB17c F4 seventh fix (sentinel-list lock coverage completed):**
    further torn-list crashes showed the lock audit was incomplete:
    (a) PolymorphicCallNode::unlinkOrUpgradeImpl's remove() was
    unlocked (and after locking, needed the isOnList() RE-CHECK under
    the lock — the drain hands nodes from an unlocked begin() read);
    (b) ~CallLinkInfoBase (lazy-sweep mutator) removed unlocked —
    now routed through the out-of-line removeOnDestruction()
    (CallLinkInfoBase.cpp), which re-checks under the lock;
    (c) CachedCall/MicrotaskCall unlinkOrUpgradeImpl + relink removed/
    pushed unlocked — removes via removeOnDestruction(), pushes via
    CodeBlock::linkIncomingCall which now locks its push gilOff;
    (d) CallLinkInfo::setVirtualCall is called UNLOCKED from
    RepatchInlines.h linkFor's mode switch — now self-locks.
    s_callLinkSerializationLock became a RECURSIVE lock: lazy sweep
    (destruction-context removers) can run from allocation inside a
    locked linker (linkPolymorphicCallImpl allocates its stub under
    the lock), so recursion is the only deadlock-free admission.
    Flag-off: all arms gated (gilOff/isGILOffProcess), lock untouched.
    **AB17c F4 residuals (rare, distinct signatures, recorded for the
    next round; the 7 named tests pass the 28-cell tier matrix in most
    runs, per-test residual rate ~1-4%):**
    1. libpas "Alloc bit not set" double-free, seen from two unrelated
    free sites (ParserArena::deallocateObjects under the
    GILOffCompilationLocker; a plain TLC deallocation-log flush) —
    an allocator-level double free of SOME object, i.e. a cross-cutting
    lifetime bug, candidate classes: shared-AtomStringTable StringImpl
    refcount discipline (family 2 adjacency) or a doubly-torn-down
    JIT-side structure. Cores: /tmp/cores/core.591777, core.643552.
    2. --useDFGJIT=0 (baseline-top-tier) jump/return into a
    mid-instruction PC immediately after an unconditional jmp in
    baseline code (int-gate-jettison-vs-execute, ~1/25): a stale
    return/jump target into since-rewritten baseline code — true
    code-lifecycle tail (IC repatch/reset vs execution). Core:
    core.640254-class.
  - R4-9 (per-lite drains skip the per-tick hook; MAJOR, CONFIRMED) —
    fixed: VMLite::drainDefaultMicrotaskQueue now drains with
    performMicrotaskCheckpoint<true>, matching the §E.1b.4 disposition
    row for VM::m_onEachMicrotaskTick ("INLINE on the draining thread,
    spawned drains included"). Flag-off unreachable, unchanged.

  **GIL-removal review round 5 (adversarial findings adjudicated):**
  - R5-1 (TERM1.2 last-observer clear vs AB-17 deletion trigger not
    coupled; MAJOR, CONFIRMED as an ordering hazard) — fixed
    mechanically: the VMEntryScope::setUpSlow AB-17 walk is now keyed on
    BOTH the §A.2.2 constant AND the §A.2.1 `perThreadTrapsIfExists`
    alias probe (see the AB-17 row's ORDERING DEPENDENCY note); the
    VMTraps.cpp take-rule comment cross-references it.
  - R5-2 (gilOff cross-thread mutation of the unlocked VM default
    microtask queue via drainMicrotasksForGlobalObject's default-queue
    arm and queueMicrotask's no-lite fallthrough; MAJOR, CONFIRMED,
    previously UNLISTED) — interim `RELEASE_ASSERT(WTF::isMainThread())`
    fail-stops landed on both gilOff arms; recorded as AB-25.
  - R5-3 (annex N6 arm 4 wasm-grow stop conduction recorded only in the
    ArrayBuffer.cpp banners; MAJOR, CONFIRMED — the AB-18/AB-19 omission
    class) — recorded as AB-26, LAUNCH BLOCKER, owner U-T13/wasm.
  - R5-4 (SPEC-ungil §H deviation: file-static s_symbolRegistryLock
    instead of the spec'd per-registry `Lock m_lock`, taken
    unconditionally flag-off; MAJOR, CONFIRMED as a LEDGER/PROCESS gap,
    code shape RETAINED) — the deviation is now supersession-ledger row
    10 below. The code is deliberately NOT moved to a member lock in this
    round: the file-static's outliving-the-registry property is what
    makes the destructor-walk-vs-straggling-`remove()` ordering use a
    lock that is never destroyed (SymbolRegistry.cpp lifecycle
    paragraph); a member `m_lock` destroyed right after the destructor
    body re-opens a destroyed-lock window for a straggler that loaded its
    back-pointer pre-clear. The flag-off Symbol.for / registered-symbol
    ~StringImpl (sweep-path) lock cost joins the §B.5 flag-off bench
    adjudication list ALONGSIDE the R4-8 aggregate (it escaped the U-T14
    close item 3 re-audit because a WTF lock is neither a gilOff-keyed
    branch nor JSC codegen).
  - R5-5 (per-lite depth-0 release drains bypass DrainMicrotaskDelayScope
    and run-vs-clear executionForbidden semantics; MAJOR, CONFIRMED) —
    fixed in VMLite::drainDefaultMicrotaskQueue itself (covers both
    JSLock call sites: the spawned token unlock and the carrier
    willReleaseLock arm): defer when VM::microtaskDrainIsDelayed() (new
    relaxed cross-thread reader on m_drainMicrotaskDelayScopeCount, per
    that field's own comment), and CLEAR (not run) the per-lite queue
    when executionForbidden() — mirroring VM::drainMicrotasks exactly.
    Flag-off/GIL-on: per-lite drains unreachable, unchanged.
  - R5-6 (AB-17 RELEASE_ASSERT makes the milestone deliverable
    structurally unreachable on this tree; BLOCKER as filed, REFUTED as a
    NEW defect / CONFIRMED as a coverage-accounting rule) — the refusal
    is the deliberate, documented interim (this file already carried the
    MILESTONE GATE STATUS: NOT MET ruling); what was missing is now the
    §B LADDER ENTRY CRITERIA paragraph above (AB-13/AB-16/AB-17 gate the
    first parallel rung; no gilOff run from this tree counts as
    parallel-mutator coverage).

## (i) Supersession ledger (one row per SPEC-ungil SUPERSESSION; spec side already written, IU side written at landing)

| # | Spec side | IU side (landing record) | Task |
|---|-----------|--------------------------|------|
| 1 | r6 F5 — Heap.cpp:3585-class VMExceptions roots via VM accessors vs per-lite registry walk | LANDED: Heap.cpp Msr/VMExceptions constraint branches on vm.gilOff(); per-VM filter; m_terminationException VM-global both modes | U-T1 |
| 2 | §A.1.4 — JSLock.cpp:166 L7 RELEASE_ASSERT GIL-on-only; GIL-off asserts the LITE's slot empty | LANDED: one line serves both via the mode-split accessor (comment at the assert) | U-T1 |
| 3 | A36/§J.7 — JSLock.cpp:151 backstop (`!useJSThreads \|\| useThreadGIL`) | PARTIAL: re-keyed per-VM (`RELEASE_ASSERT(!m_vm->gilOff())` on the main-carrier install path; gilOff VMs take the carrier branch). The full §J.7 U1 replacement (TLS-tag equality + the A36C client check at the backstop) is a **U-T8 deliverable** per the handout §T task list ("J.7 replacement"); U-T5/U-T6 land only its prerequisites (stub deletion; per-thread clients) | U-T1 → U-T8 (prereqs U-T5/U-T6) |
| 4 | A16 vs vmstate:534-539 — VMLite::scratchBufferForSize reserve re-frozen as the GIL-off non-baked path; Group 5 repurposed as ownership list | LANDED: VM::scratchBufferForSize gilOff dispatch; ensureScratchBufferAtIndex appends to the ownership list; gatherScratchBufferRoots registry scan (jit R2) | U-T1 |
| 5 | §LK.6 re-rank vs vmstate §6.5.1/§7 — SBR -> VMLiteRegistry::lock -> scratchBufferLock LEGAL | LANDED: install fan + GC scan take scratchBufferLock under the registry lock; rank note on ScratchBufferRegistry | U-T1 |
| 6 | U0c vs heap §5.1 sticky trigger / §10D — designation CAS primitive; :4755 arm !gilOffProcess; HeapClientSet::add stays idempotent | LANDED except the HeapClientSet.cpp:69 RELEASE_ASSERT(gilOffProcess => server VM m_gilOff==1) — **OBLIGATION (U-T3 per handout task split; file outside U-T1's set)** | U-T1/U-T3 |
| 7 | A36 r9 F4 TID supersessions (vmstate §6.7 tid-0 GIL-on-only; carrier TIDs from the 2^15 TM space) | PARTIAL: carrier creation consumes the hook pair; **OBLIGATION: ThreadManager must register a carrier-TID allocator (I17 accounting incl. carriers) at initialization — until then GIL-off first entry RELEASE_ASSERTs** | U-T1 → api/U-T6 |
| 8 | heap §10A.1 re-stamp clause (SPEC-heap.md:281-283) vs §B.3 + A36C | PARTIAL: GIL-off install/restore re-stamp landed (tuple swap); GIL-on forwarding + re-stamp UNCHANGED; the {client, epoch} staleness upgrade of the §10A.1 slot itself is U-T6's | U-T1 → U-T6 |
| 9 | Phase-1 "useThreadGIL always on" (OptionsList.h text; SPEC-api phase-1 framing) vs handout §T U-T14 default flip + §0 U0 | LANDED: default false (OptionsList.h); U0 validation forces useThreadGIL=1 when {useVMLite, useSharedAtomStringTable, useSharedGCHeap} incomplete (Options.cpp, after the M_opts2 normalization) — restores equivalence of all short-form `useJSThreads && !useThreadGIL` derivations with VM::isGILOffProcess() | U-T14 |
| 10 | SPEC-ungil §H — "WTF::SymbolRegistry's m_table gains Lock m_lock" (per-registry member, destructor-leaf §LK.8) | SUPERSEDED IN SHAPE (round 5, R5-4): landed as ONE file-static `s_symbolRegistryLock` (SymbolRegistry.cpp) serializing all registries, taken unconditionally in every configuration. Strictly MORE serialization (every §H ordering holds; leaf rank unchanged); file-static retained deliberately — it outlives every registry, so the destructor-walk vs straggling ~StringImpl `remove()` ordering never uses a destroyed lock, which a member m_lock cannot guarantee. COSTS: flag-off Symbol.for + registered-symbol ~StringImpl (incl. sweeper-thread sweep paths) now take an uncontended process-global lock where they were lock-free — ON the §B.5 flag-off bench adjudication list with the R4-8 aggregate. Reopen-to-per-registry/sharded on bench evidence (the chartered non-sharding's reopen condition) | U-T13 |
| 11-… | (mid-program supersessions by U-T2…U-T13 were recorded in their task-log entries and in-code doc-of-record blocks rather than as rows here; see the U-T14 close entry item 7 for the pointer map) | — | U-T2…U-T13 |

## (ii) §F.2 predicate-consumer table (~60 rows: assert / BRANCH / EXCLUSIVITY CONSUMER) — U-T8

SKELETON. Columns: consumer site | predicate form consumed | class
(assert/branch/exclusivity) | GIL-off ruling (annex F2 fixed rulings;
~AsyncTicket/finalizer rows) | landed-at.

| Site | Form | Class | Ruling | Landed |
|------|------|-------|--------|--------|
| U-T14 CLOSE: executed IN CODE — the §F.2 predicate split and per-consumer rulings live as the doc-of-record comment blocks in JSLock.cpp (token machinery home) and VM.cpp (:271ff isEntered split, :796 row-21 citation, :998 token-meaning assert). This table intentionally stays a pointer, not a copy. | | | | U-T8 |

## (iii) §E.4 settle-site lock-context table — U-T8

SKELETON. Columns: settle site | lock context at settle | routing
(same-thread / cross-thread ticket) | retirement rule (r17 F2 / r18 F2).

| Settle site | Lock context | Routing | Retirement |
|-------------|--------------|---------|------------|
| U-T14 CLOSE: executed IN CODE — the full settle-site lock-context table (LockObject.cpp:275/:521, ThreadObject.cpp:246/:435, ThreadManager.cpp:78; r17 F2 decide-under-lock/act-after-drop status per row) is the doc-of-record block at JSLock.cpp:925-940. All frozen rows COMPLIANT as landed; the GIL-off inbox routing row landed with U-T9. | | | |

## (iv) §A.1.7 off-thread-reader table (per rerouted Group-3 field) — U-T8d

SKELETON. Columns: rerouted field | off-thread reader | disposition
(i) registry-resolve target lite (suspended, SUSPEND RULE r24) /
(ii) refused GIL-off with defined error / (iii) proven on-thread.

Seed rows from U-T1 (dispositions to be ruled by U-T8d):
| Field | Reader | Disposition |
|-------|--------|-------------|
| topCallFrame | SamplingProfiler.cpp:391-431 (suspends target) | (i) carrier-only v1 (AUD1.K1/SD18) — U-T8d |
| topCallFrame | JSLock.cpp willReleaseLock depth-0 clearLastException guard | RESOLVED at U-T1 amendment: routed through group3Primitives() (on-thread; carrier still installed at the read) — (iii) |
| m_exception / m_lastException | Heap.cpp Msr/VMExceptions (GC visit thread) | RESOLVED at U-T1: registry walk, per-VM filter (r6 F5) — not via accessors |
| exceptionForInspection() | inspector/debugger | U-T8d (currently routes to CURRENT lite when gilOff; off-thread use must go (i)/(ii)) |
| VM-block fallback reads (group3Primitives with no lite) | compiler threads via C++ helpers | U-T8d enumerates; dark-safe today |
| varargsLength / newCallFrameReturnValue | LLIntSlowPaths.cpp size_frame_for_varargs store + paired varargsSetup reload (only C++ consumers; no .asm consumer) | RESOLVED (H1-varargs-shared-vm-scratch-slots fix): both sites routed through group3Primitives() — same-thread per-call scratch, sequenced-before pair within one doCallVarargs dispatch — (iii). Raw vm.varargsLength / vm.newCallFrameReturnValue access retired; do not reintroduce in new slow paths. |
| U-T14 CLOSE: the remaining per-field enumeration was executed IN CODE by U-T8d — the carrier-only capture ruling + SUSPEND-RULE record is the doc-of-record block at SamplingProfiler.h:323-360 (AUD1.K1/SD18, (i) carrier-only v1), with the inspector/debugger exceptionForInspection ruling at Debugger.cpp:54ff. | | |

## (v) §E.1b.4 host-hook disposition table — U-T8e

SKELETON. Columns: globalObjectMethodTable / host-callback slot |
JS-reachable on spawned TS? | disposition {inline, carrier-queued, refused,
unreachable} | SD15 tracker handoff notes.

| Hook slot | Spawned-reachable | Disposition | Notes |
|-----------|-------------------|-------------|-------|
| U-T14 CLOSE: executed IN CODE — the full per-slot enumeration ({inline-safe, carrier-queued, refused-with-error, unreachable-on-spawned(proof)}) is the doc-of-record block at JSGlobalObject.cpp:680ff (baseGlobalObjectMethodTable), with the SD15 promiseRejectionTracker carrier-handoff rows at JSGlobalObject.cpp:4267/:4289. | | | U-T8e |

## (vi) §F.6 embedder checklist (incl. (d) construction-order and (e) spawned-no-foreign-VM audits) — U-T8

CLOSED at U-T14. The binding obligation text is the in-code row table at
JSLock.cpp:941-955 (U-T8 deliverable); this checklist records the CLOSE
dispositions. (a)/(c)/(d) bind OUT-OF-TREE Bun code and cannot be executed
from this repository — at close they are recorded as **BUN-INTEGRATION SHIP
BLOCKERS** (conditional sign-off: the in-tree contract side is complete;
the Bun-side enumerations must be discharged in the Bun repo before any
Bun build enables the full trio), NOT as satisfied audits.

- (a) JSLockHolder exclusivity (m_lock excludes only embedder threads,
  §F.1) — in-tree side COMPLETE (F1B arm + spawned token entry,
  JSLock.cpp; §B.3 supersession landed at U-T6). Bun-side critical-section
  enumeration: SHIP BLOCKER, Bun repo.
- (b) SD10 continuation-affinity sign-off — recorded r21 pre-close (ALS
  slice discharged by ANNEX ALS1); was the U-T9 entry gate; nothing owed
  at U-T14.
- (c) blocking-site enumeration (§F.6 delta (c)/DAL2.5) — in-tree DAL/§J.3
  /RHA-AHA mechanisms landed (U-T8/U-T11); the site CLASSES to audit are
  enumerated in the JSLock.cpp row. Bun-side enumeration: SHIP BLOCKER,
  Bun repo.
- (d) FIRST-VM-WINS construction-order audit row — RECORDED (this row is
  the U-T14 close item): normative requirements per ANNEX EC1 are (1)
  main-VM-first construction, (2) boot-assert vm.gilOff()==true
  immediately after constructing the intended spawner, (3) enumeration of
  EVERY Bun VM-construction site incl. lazy helper VMs, (4) no
  re-designation in v1. In-tree enforcement: U0c CAS + U0b spawn
  RangeError + the ThreadManager.cpp:336 backstop make a violated order
  fail loudly, not silently. Bun-side enumeration: SHIP BLOCKER, Bun repo.
- (e) spawned-no-foreign-VM — ENFORCED in-tree, both arms landed
  (JSLock.cpp:478 spawnedThreadEntryTokenLock scope check +
  JSLock.cpp:1027 lock()-front gate; both process-abort naming §F.6(e)).
  Death-test arm: deferred-to-Build with the U27 harness. Bun-side
  native-code audit (no spawned-thread JSContext creation): SHIP BLOCKER,
  Bun repo.

## (vii) Per-row call-site enumerations deferred by annex K4/N7 rows — U-T8b (+owners named in rows)

SKELETON. One subsection per K4/N7 row that defers an enumeration to IU;
U-T8b (and §N owners U-T13 etc.) fill them. The implementation CONSUMES the
EXECUTED K4/N7 tables verbatim — these subsections add call sites only.

## U-T1 obligations / deferred refinements (tracked; each names its owner)

1. **Raw Group-3 / entry-record C++ consumers.** `vm.topCallFrame`,
   `vm.entryScope` (VMEntryScopeInlines.h ctor/dtor fast path, CallFrame.cpp,
   VMTraps.cpp:80/:497, SamplingProfiler.cpp:375/:786, Debugger.cpp:203,
   JSDollarVM.cpp:4339-4343, Heap.cpp:1343) read members directly and are
   correct GIL-off only because VMEntryScope::setUpSlow/tearDownSlow keeps a
   transitional VM-member SHADOW (see VMEntryScope.cpp comment). The
   activation tasks (U-T5/U-T9 for entry; U-T3/U-T4 emission + U-T8d for
   topCallFrame-class fields) must re-point or rule each site, then drop the
   shadow. GIL-off top-level-entry detection in VMEntryScopeInlines.h MUST
   key on the CURRENT LITE's record before N-mutator entry goes live —
   concurrent first entries would otherwise skip setUpSlow on all but one
   thread. **HARD GATE (U-T1 amendment): "shadow dropped + every raw
   consumer re-pointed" is a precondition of N-mutator entry; until it is
   discharged, RELEASE_ASSERT tripwires in VMEntryScope::setUpSlow/
   tearDownSlow fail-stop any second concurrent GIL-off top-level entry
   instead of letting the shadow race (last-writer-wins / early-null).**
   Rerouted at the U-T1 amendment (no longer raw): JSLock.cpp
   willReleaseLock's depth-0 topCallFrame guard now reads
   group3Primitives() — see table (iv).
2. **Entry-scope service VM-level word retirement.** GIL-off, transient
   VM-wide bits remain set on the VM-level word after every lite serviced
   its copy (the word is the backfill source); a late-registered lite
   re-observes them. Harmless for the current service set
   (FirePrimitiveGigacage re-fire is idempotent; PopListeners list is
   drained under the lock-holder), but U-T2's rule-3 trap fan-out subsumes
   this protocol and must ratify or replace the retirement rule.
3. **Carrier-TID provider.** ThreadManager registers
   `JSC::setCarrierTIDHooks` at initialization (api workstream / U-T6);
   accounting per I17 (carriers count vs 2^15; release on carrier teardown —
   the U-T1 TLS-death skeleton already calls the release hook).
4. **Carrier teardown protocol.** The U-T1 TLS-death path is a SKELETON
   (TEARDOWN mark -> unregisterLite LAST). U-T6 replaces it with the full
   r31/r32 EXIT1.3/EXIT1.9/A36 machinery (COLLECTED/DETACHED, ~VM walk +
   completion fence, deferred degenerate dtor, no-op M12 removal). ~VM's
   existing "no other registered lite points at this VM" assert will fire
   with live carriers until then — acceptable while dark.
   **Eviction-path ownership rule (U-T1 amendment):** the A36 stale-epoch
   eviction arm (JSLock.cpp evictStaleCarrier) runs the SAME skeleton —
   still-registered stale lite: TEARDOWN mark under the registry lock ->
   unregisterLite LAST -> releaseCarrierTIDIfHooked -> owner free; a map
   entry NEVER bare-deletes its lite. Once U-T6's ~VM collection walk
   lands, an UNregistered stale bit-SET (main-thread) lite was walk-freed
   (A36 r32) and the map FORGETS the pointer without touching it — the
   walk and the map can never both free; the unregistered bit-CLEAR arm
   RELEASE_ASSERTs until U-T6 lands the state-keyed deferred degenerate
   dtor there.
5. **Per-lite regexp members.** lite->executingRegExp / regExpAllocator
   exist (Phase A Group 4); the VM-side consumers (VM::m_executingRegExp,
   m_regExpAllocator/m_regExpAllocatorLock; RegExpInlines.h call sites) are
   NOT yet mode-split — owner: **U-T8b** (which consumes the K4 table I
   regexp row and lands AUD1.N2 / RegExp::m_ovector FIRST — both marked
   memory-unsafe-today in the handout), with U-T3/U-T4 covering only any
   emission-side leg. The corresponding r6 F5 root-walk extension for lazy
   regexp match buffers lands with the U-T8b reroute. Enumerated here when
   landed.
6. **m_microtaskQueue Group-3 slice** — §E owns the reroute (U-T9); not
   touched by U-T1.
7. **VMLite facility test gate.** INTEGRATE-vmstate.md's PENDING C++-test
   item (owner-only enqueue/drain, scratch growth, dtor-free-under-ASAN)
   still gates ACTIVATION: U-T1 added dark callers
   (VM::scratchBufferForSize gilOff dispatch, A16 installs) under the
   handout's authority; the test must land before any gate runs GIL-off.
8. **Pending corpus/amplifier arms (U-T1-named, runnable at activation):**
   (a) thrower parked pre-catch survives a forced full collection (per-lite
   exception roots); (b) two-VM root-walk arm (gilOff VM + GIL-on VM
   collected concurrently; per-VM filter); (c) U0b/U0c corpus —
   compile-heavy run THEN first spawn; two VMs constructed under
   gilOffProcess (loser ctor completes, loser spawn RangeErrors, loser
   embedder entry executes JS); (d) A36C two-VM alternating-entry and
   nested re-stamp arms (U-T6/U27 own the harness).
9a. **Exception-scope verification state per-lite (K4 table I).**
   `m_needExceptionCheck` / `m_throwingThread` (+ the rest of the
   ENABLE(EXCEPTION_SCOPE_VERIFICATION) block) are classed PER-LITE by the
   EXECUTED audit (K4 table I row 2: vmstate I15 — throw state is
   thread-local) but remain VM-global: their raw writers live outside
   U-T1's file set (ThrowScope.cpp:56/:95, LockObject.cpp:74-98,
   Interpreter.cpp:968). Owner: **U-T8b**, as a debug-only L2 VMLite tail
   append (NOT part of the frozen VMLitePrimitives ABI) serviced alongside
   its K4 consumption — or an explicit gilOff-debug-only ruling recorded
   here. Until then EXCEPTION_SCOPE_VERIFICATION builds are not
   N-mutator-safe GIL-off (dark today; comment at the VM.h block).
9b. **gilOffProcess Config byte + LLInt level-2 selection** — U-T3
   (JSCConfig.h:106; loadVMLite emitter; VMEntryRecord::m_vmLite). U-T1's
   `VM::isGILOffProcess()` is the C++ twin (see (i) row 6/ledger row 1
   note); U-T14 re-audits the flag-off branch budget against jit I1's
   permitted-delta list.

## AB17d — solo verification round over the AB17c adversarial findings

Each reviewer finding was re-verified against the tree; dispositions below
(FIXED = landed this round; REFUTED = false positive with evidence;
OPEN = confirmed, carried with owner).

1. **entrypointFor lazy refill + unconverted priming consumers (BLOCKER,
   CONFIRMED, FIXED).** ExecutableBase::entrypointFor's unsynchronized
   arity-mirror refill could store an entrypoint derived from the OLD jit
   code after a concurrent installCode retracted the slot — a stable stale
   value the thunk slot-recompare cannot detect — and the unconditional
   generatedJITCodeFor deref could hit installCode's transient null.
   Fix (ExecutableBase.h): under the gilOffProcess Config gate the lazy
   cache is NEVER written and the jit-code read is null-checked. Since
   installCode/clearCode only ever store null to the mirror, script
   executables now have a permanently-null arity slot gilOff: the virtual
   thunks' non-null gate routes every script callee to the slow path, which
   derives a matched (entrypoint, CodeBlock) pair through one CodeBlock
   snapshot. Host executables publish once at construction and never
   retract. The two unconverted priming consumers are gated off under the
   same byte (JSBoundFunction.cpp boundThisNoArgsFunctionCall,
   JSRemoteFunction.cpp); WebAssemblyFunction.h:98 was a FALSE POSITIVE —
   its enclosing function already returns nullptr under
   Options::useJSThreads() before reaching the call (WebAssemblyFunction.h
   ~:91). Deliberate cost: gilOff virtual-mode calls to script functions
   always take the C++ slow path (monomorphic/polymorphic ICs unaffected —
   their pairs are published under the link lock). Flag-off: one
   predicted-false Config-page byte test per site.

2. **Thunk arity-slot revalidation ABA (BLOCKER, CONFIRMED, FIXED by the
   same change).** The value-equality recompare was ABA-vulnerable across
   jettison/reinstall of the same CodeBlock (arity entrypoints are
   addresses in long-lived JITCode objects and recur). With the refill
   gated off, the only values a script executable's slot can ever hold
   gilOff are null — value recurrence is impossible because no non-null
   value is ever (re)published. ThunkGenerators.cpp comment updated; the
   recompare branch is retained as defense-in-depth.

3. **PropertyTable plan-time clone races in-place edits (BLOCKER,
   REFUTED).** The claimed interleaving requires the planner's clone to run
   unlocked. It does not: Structure::takePropertyTableOrCloneIfPinned
   (Structure.cpp:1224, useJSThreads arm) performs the pinned-table
   copy() under GCSafeConcurrentJSLocker on the SOURCE structure's m_lock,
   and every in-place dictionary edit (addAfterFind/take/
   updateAttributeIfExists) runs under the same m_lock via Structure::add's
   l6Locker (StructureInlines.h:449-462) — clone and edit are mutually
   excluded, so the clone can never miss an in-flight add and the rehash
   index-vector free (PropertyTable.h destroyIndexVector) can never run
   concurrently with the clone's read (the claimed UAF). The single
   pre-edit bump is sufficient in this design: with mutual exclusion the
   edit count only needs to detect edits that complete between the locked
   clone and the locked publication validation, and any such edit's bump is
   ordered before the validation read by the cell lock the editors also
   hold (JSObject.cpp:4148 Locker vs putDirectInternal's S6 leg). The
   odd/even seqlock proposal is unnecessary.

4. **unlinkOrUpgradeIncomingCalls unlocked drain iteration (MAJOR,
   CONFIRMED, FIXED).** The drain's isEmpty()/begin() reads ran outside
   s_callLinkSerializationLock while destruction-context removers
   (CallLinkInfoBase::removeOnDestruction) mutate nodes parked in the
   drain's local toBeRemoved list under the lock — torn sentinel reads,
   and begin() could hand out a node freed before the per-node lock
   acquisition. Fix (CodeBlock.cpp): the gilOff drain now holds the
   (recursive) lock across the entire {takeFrom, isEmpty, begin,
   unlinkOrUpgrade} loop; a sweeper either removes a node before the drain
   observes it or blocks until the node is off the local list.

5. **GC-heap allocation under s_callLinkSerializationLock re-creates GT11
   (MAJOR, REFUTED as stated; comment fixed).** linkPolymorphicCall takes
   DeferGCForAWhile BEFORE acquiring the lock (Repatch.cpp:325-341), so
   allocation inside the locked linker cannot initiate a collection or a
   world-stop request while the lock is held — lazy-sweep destructor runs
   (the reason the lock is recursive) are allocation-side sweeping, not
   stop requests, and do not park. The GT11 wedge requires a stop REQUEST
   or safepoint park by the holder; neither is reachable from the locked
   linkers. The stale "non-recursive" AB18-C text was removed with the
   item-4 rewrite. RESIDUAL WATCH: any future locked caller without a
   DeferGC bracket re-opens this — record a rule: no GC-heap allocation
   under this lock without an active DeferGC.

6. **"28-cell matrix ALL GREEN" overclaim (MAJOR, CONFIRMED).** The F4
   verification gate is RE-STATED AS NOT MET. The two recorded residual
   crash classes (libpas double-free cores /tmp/cores/core.591777,
   core.643552; --useDFGJIT=0 mid-instruction-PC core.640254-class) remain
   OPEN named items. The item-1/2/4 fixes this round plausibly bear on the
   second class (stale-entrypoint family) — the 28-cell matrix must be
   re-run after this round before any F4 green claim.

7. **F1 closure unproven (BLOCKER, CONFIRMED — this is the missing AB17c
   F1 entry).** No post-fix transition-heavy-constructor number was ever
   recorded in-tree; the gate is FORMALLY RED until V5b adjudicates with
   the 9-run-median default (or repeated 5-run sessions) on a quiet
   machine against the pinned Release binary, and records the number HERE.
   Given the implementer's own same-binary drift data (+6.12% → +11.36%),
   a single marginal pass is inconclusive; re-run. bench-gate.sh and
   baseline.json were verified untouched this round (no threshold
   tampering).

8. **I3 false-sharing mechanism unsubstantiated flag-off (MAJOR,
   CONFIRMED, comment corrected).** The VM.h I3 comment now carries an
   explicit MECHANISM CAVEAT: no cross-core fetch_or writer is active in
   the flag-off bench configuration; the change stands on strict flag-off
   cost reduction only. perf c2c substantiation remains OWED (V5b, with
   the bench adjudication).

9. **Residual raw m_gilOff/gilOff() loads flag-off-reachable (MAJOR,
   CONFIRMED, FIXED).** Routed through gilOffWithProcessGate():
   softStackLimitForCurrentThreadSlow (VM.h — the comment-documented hot
   one), adaptiveStringSearcherTables, disallowVMEntryCountSlot,
   RegExpInlines.h compileIfNecessary, CodeBlock.cpp linkIncomingCall +
   unlinkOrUpgradeIncomingCalls. Remaining raw gilOff() sites are
   gilOff-arm-only or cold; per-site rulings owed to the U-T14 re-audit.

10. **No layout separation for the service word (MAJOR, CONFIRMED,
    FIXED).** m_entryScopeServicesRawBits is now isolated by 64B
    never-accessed pads on each side (VM.h) — guarantees line exclusivity
    regardless of allocation base alignment (alignas(64) deliberately
    avoided: FastMalloc/TZone do not honor over-alignment). Members are
    outside the frozen X-macro span; no spec revision required.

11. **Flag-off branch-budget accretion (MAJOR, CONFIRMED as process
    debt).** No individual violation found by the reviewer or this round;
    the owed U-T14 golden-disasm re-audit of the put/transition fast path
    against jit I1's permitted-delta list MUST run as part of V5, and now
    also covers the new Config-page byte tests added this round (items 1,
    9) and the priming-site tests.

12. **property-wtr-isolation.js lost wakeup (MAJOR, CONFIRMED, recorded
    EXPECTED-FAIL).** The sole V2/V3 corpus failure matches the unlanded
    MC-WAIT S3a suspect (docs/threads/CVE-AUDIT-STATUS.md CHECK-NOW:
    property-wait pre-enqueue re-validation, handout §C.3, NOT landed =>
    lost wakeup), possibly compounded by numeric-key→uid canonicalization
    on the notify path. NOT introduced this family-round. Until §C.3
    lands (charter as a named family next round, owner TBD), V2/V3 ladder
    accounting carries this test as EXPECTED-FAIL with this citation
    rather than a silent red rung.

13. **Bench gate formally red (MAJOR, CONFIRMED).** Same disposition as
    item 7: V5b adjudication on the pinned binary is the only closure
    path; no drift argument is accepted as a fix.

## AB17e — solo round over the post-AB17d adversarial findings

Dispositions (FIXED = landed this round; REFUTED = false positive with
evidence; OPEN = confirmed, carried). Tree re-built (Release jsc) after the
changes below.

1. **F4 sentinel-lock coverage does not cover OBJECT LIFETIME (BLOCKER,
   CONFIRMED, FIXED).** The AB17c "seventh fix" closed the LIST races but
   not the lifetime race: delisting was the LAST act of destruction
   (~CallLinkInfoBase), and the teardown preceding it was unlocked —
   ~CallLinkInfo's `delete m_record` (and its stub()-gated check-then-act
   lock), ~DirectCallLinkInfo's wholly unlocked teardown, ~CachedCall /
   ~MicrotaskCall's m_addressForCall store. A locked drain
   (CodeBlock::unlinkOrUpgradeIncomingCalls) legitimately begin()'s a
   still-listed node owned by a dying caller and uses it across
   unlinkOrUpgradeImpl (revertCall/publishRecord touch m_record) — UAF /
   double-retire, consistent with the libpas double-free residual class
   (core.591777 / core.643552). Second interleaving: ~CallLinkInfoBase's
   UNLOCKED `if (isOnList())` pre-check reads false in the window after
   the drain's in-loop remove() while the drain is still mid-call on the
   object, skipping the lock entirely. FIX (invariant: precondition 11,
   extended to object lifetime — "delisting is the FIRST act of
   destruction; a destructor either completes its locked delist before the
   drain takes the list, or blocks until the drain loop ends"):
   - ~CallLinkInfoBase (CallLinkInfoBase.h): gilOff arm calls
     removeOnDestruction() UNCONDITIONALLY (no unlocked isOnList gate);
     flag-off keeps the historical inline isOnList()/remove() pair behind
     one g_jscConfig.gilOffProcess byte test.
   - removeOnDestruction (CallLinkInfoBase.cpp): unconditional lock
     acquire gilOff, isOnList() re-checked only under the lock; mode gate
     switched from the out-of-line 5-Options VM::isGILOffProcess() to the
     inline Config-page byte (also closes finding 7's flag-off-work
     objection for this path).
   - ~CallLinkInfo (CallLinkInfo.cpp): gilOff, one lock hold across
     {delist, clearStub, delete m_record}; the unlocked stub() pre-check
     and unlocked record delete are gone.
   - ~DirectCallLinkInfo (CallLinkInfo.h), ~CachedCall (CachedCall.h),
     ~MicrotaskCall (MicrotaskCall.h): locked delist FIRST
     (removeOnDestruction), member teardown only after — sound because a
     node delisted under the lock is unreachable by any subsequent drain.
   Participant enumeration (all CallLinkInfoBase most-derived types):
   CallLinkInfo (+Baseline/Optimizing) FIXED above; DirectCallLinkInfo
   FIXED above; CachedCall FIXED above; MicrotaskCall FIXED above;
   PolymorphicCallNode has NO derived-dtor teardown (POD members), so the
   base-dtor unconditional-lock fix covers it.
   VERIFICATION OWED: the full 28-cell matrix re-run (7 tests x 4 tier
   caps x 5 runs, pinned GIL-off flags) and re-triage of the two residual
   core classes against this fix. F4 gate remains OPEN until then.

2. **F3 GT11 watchdog fix incomplete — sibling flatten sites under
   codeBlock->m_lock (BLOCKER, CONFIRMED, FIXED).** Verified: tryCacheGetBy
   holds GCSafeConcurrentJSLocker(codeBlock->m_lock) across Repatch.cpp
   ~735's direct flattenDictionaryStructure call and all
   prepareChainForCaching calls; Structure::flattenDictionaryStructure
   (Structure.cpp) routes UNCONDITIONALLY to the §10.6 per-event stop under
   useJSThreads — so any gilOff IC-caching attempt meeting a dictionary in
   the chain re-created the watchdogAssertStopProgress wedge. RULE LANDED
   (invariant O2/GT11): gilOff, NEVER flatten from any IC-caching or
   chain-prep path; flattening happens only from unlocked runtime sites.
   COMPLETE site enumeration of flattenDictionaryStructure /
   flattenDictionaryObject callers reachable flag-on, with rulings:
   - Repatch.cpp actionForCell (~415): gated RetryCacheLater (AB17c, was
     the one fixed site).
   - Repatch.cpp tryCacheGetBy unset/proto arm (~738): gated
     RetryCacheLater (THIS ROUND).
   - ObjectPropertyConditionSet.cpp prepareChainForCaching (~596): gated
     std::nullopt => callers GiveUpOnCache (THIS ROUND). Covers every
     tryCache* (get/put/in/instanceof/delete) caller in Repatch.cpp,
     LLIntSlowPaths.cpp:984, and StructureRareData.cpp:212/225.
   - Operations.cpp normalizePrototypeChain (~140): gated
     InvalidPrototypeChain (THIS ROUND). Covers LLIntSlowPaths.cpp:1327
     (flag-off-only via useUnthreadedLLIntPropertyCaches, but gated anyway)
     and :1667 (reachable flag-on under codeBlock->m_lock), and
     JSPropertyNameEnumeratorInlines.h:68.
   - Repatch.cpp delete-IC flatten (~1738): UNREACHABLE flag-on (GiveUp
     gate at ~1725) — no change needed.
   - LLIntSlowPaths.cpp:981-ish flatten: behind
     RELEASE_ASSERT(!Options::useJSThreads()) — no change needed.
   Any future flattenDictionaryStructure caller must be audited against
   this enumeration. VERIFICATION OWED: a proto-chain-dictionary arm for
   transition-vs-write, and the 6 named F3 tests at 5/5.

3. **AB17c F3 ledger entries missing (MAJOR, CONFIRMED, FIXED — this
   section is the record).** Per-fix named invariants for the F3 family:
   - O2/GT11 transition-vs-write stw-watchdog root cause: Repatch.cpp
     actionForCell gate (AB17c) + the three sibling-site gates above
     (AB17e), site enumeration above.
   - S6 L3/L4 cacheable-dictionary staleness guard: JSObject.cpp ~4115.
   - I21 CAS-max: JSObject.cpp ~731 / Butterfly.h ~198.
   Test evidence (honest): 5 of the 6 named tests passed 5/5 GIL-off full
   JIT in the implementer's runs; spawned-thread-butterfly-stress was 4/5 —
   a RED gate. F3 gate is OPEN until 6/6 at 5/5 with the strengthened
   shared-arraystorage-stress oracle (see item 6).

4. **F1 flag-off bench regression — gate red, no code fix landed (BLOCKER,
   CONFIRMED, OPEN; cannot be closed by this solo round).** Re-affirmed:
   no flag-off hot-path work was removed; the I3 layout/Config-gate changes
   are disclaimed as a root-cause fix in VM.h itself. Independent
   measurement recorded by a reviewer this round: 9 fresh runs of the exact
   gate protocol on the just-built Release binary gave median +1.13% vs
   baseline — the +10.59% does NOT reproduce, and the run-to-run spread on
   this host (~9%, and a same-suite megamorphic-access reading 12.2% FASTER
   than baseline) is an order of magnitude above the 1% threshold, so NO
   number from this host/protocol (red or green) is admissible. CLOSURE
   PATH (binding): V5b on a quiet host — pinned CPU/governor, interleaved
   A/B runs of a same-toolchain reference build vs current binary (or
   same-session --record re-baseline per BENCH.md), >= 15 runs — number
   recorded HERE. Note the gate was already red pre-round (+1.78%); the
   regression-to-+10.59% claim and any single-session green are both
   session noise.

5. **gilOffWithProcessGate "strictly cost-reducing" claim wrong on op
   count (MAJOR, CONFIRMED, comment FIXED; latch reorder NOT taken).**
   The VM.h comment now states the real trade: TWO predicted-false
   Config-page byte tests flag-off vs the ONE this-relative member
   load+branch it replaced; the win is the read-only page (no
   concurrently-mutated line), not fewer ops. The latch-before-designation
   reorder that would drop the fallback term is deferred (touches the VM
   ctor / JSLockHolder window — too risky for a solo round) and remains
   the recorded path to a true one-byte-test gate. RegExpInlines.h
   unified: ovectorSpan (per-match hot) and compileIfNecessaryMatchOnly
   now use gilOffWithProcessGate like compileIfNecessary (equivalence
   invariant makes the swap behavior-identical).

6. **F3 stress-test oracle weakened in the round it started passing
   (MAJOR, CONFIRMED, FIXED).** shared-arraystorage-stress.js restored to
   a lost-write-detecting oracle: writer readback is exact equality
   (back === encode(slot, w, k & 7) — sound under the wave barrier: the
   owner relayouts only between waves while all writers are parked, and
   stripes are disjoint since LEN % THREADS == 0), and the owner post-wave
   check requires the exact wave-w encode at the checked stripe index (a
   stale warmup-0 / wave w-1 value now FAILS instead of passing silently).
   The F3 5/5 evidence must be re-collected with this oracle.

7. **New unconditional flag-off work on teardown paths (MAJOR, CONFIRMED,
   FIXED for the dtor paths; LLInt branch recorded as budget debt).**
   ~CallLinkInfoBase and removeOnDestruction now gate on the inline
   g_jscConfig.gilOffProcess byte (no out-of-line cross-DSO call, no
   5-Options re-derivation, flag-off arm inlined in the header) — see item
   1. The ~CallLinkInfo dtor likewise. The AB17c LLInt
   callHelper/doCallVarargs per-call g_config byte test
   (LowLevelInterpreter64.asm ifJSThreadsBranch ~1709) is ADDED to the
   owed U-T14 permitted-delta re-audit list (with AB17d items 1/9/11).

8. **Verification overclaims ("28-cell ALL GREEN", "6 tests 5/5")
   (MAJOR/BLOCKER, CONFIRMED).** Formal gate state, superseding any
   implementer summary: F1 OPEN (item 4), F3 OPEN (items 2/3/6), F4 OPEN
   (item 1). Mechanism LANDED is not behavior DELIVERED; no green claim on
   flaky passes. The AB17d-era 28-cell runs are additionally STALE
   evidence — this round changed the code under test again.

## AB17f — solo verification round over the post-AB17e adversarial findings

Dispositions (FIXED = landed this round; CONFIRMED-OPEN = real, carried with
its ledger-specified closure path; REFUTED-AS-STATED = claim corrected with
file:line evidence; ALREADY-RECORDED = real but the tree already records it).
Tree re-built (Release jsc) after the changes below. This round is
docs+comments+two-narrow-code-fixes only; NO gate state changes: F1, F3, F4
remain OPEN exactly as AB17e item 8 rules.

1. **I21 dense-store length publication relaxed-only (MAJOR, CONFIRMED,
   FIXED).** Verified: both bumpPublicLengthToAtLeast copies (Butterfly.h
   flat ~203, ButterflySpine ~434) CAS-maxed with memory_order_relaxed, and
   every dense-store writer (JSObject.cpp trySetIndexQuicklyConcurrent
   Int32/Contiguous/Double arms ~774-851 and the segmented arms) issues the
   element store plain before the bump — TSO-only publication, unrecorded
   (unlike the IT-8 convention). FIX: the successful CAS is now
   memory_order_release in both copies (free on x86-64: atomic RMW is
   already fully fenced, so zero bench-gate exposure), and the reader-side
   acquire gap (tryGetIndexQuicklyConcurrent's relaxed publicLength load,
   JSObject.cpp ~691/~709) is recorded as a named KNOWN RESIDUAL (I21
   publication, TSO-sound, ARM64 open — benign form: spurious hole =>
   generic-path fallback) at Butterfly.h, ButterflySpine, and
   updatePublicLengthAfterDenseStoreConcurrent (JSObject.cpp ~736).
   Chartered with the IT-8 reader-side residual (same arm64 round).

2. **Stale load-bearing AB18-D comment in setMonomorphicCallee (MAJOR,
   CONFIRMED, FIXED).** Verified the contradiction: the comment claimed the
   LLInt data-IC fast path "is NOT yet routed through m_record", but the
   AB17c third fix landed exactly that reroute
   (LowLevelInterpreter64.asm .opCallThreadedRecord at callHelper :2918 and
   doCallVarargs :3041, behind ifJSThreadsBranch :1709). Comment rewritten:
   mirrors are GC/unlink state flag-on (read under the link lock by
   unlinkOrUpgradeImpl — whose upgrade arm relies on the rewrite being
   invisible to flag-on fast paths — and by visitWeak); the published
   record is the sole flag-on dispatch source; the obsolete ARM64
   mirror-reader KNOWN RESIDUAL is redirected to the IT-8 record. The
   payload-first/fence/comparand-last store order itself is KEPT
   (belt-and-braces; costs nothing on the already-gated arm). clearCallee's
   sibling rationale annotated the same way. No behavior change.

3. **Heap::allocationClientForCurrentThread raw vm.gilOff() on every C++
   allocation (part of the flag-off accretion finding, CONFIRMED, FIXED for
   this site).** Verified at heap/Heap.h ~1833: raw m_gilOff member
   load+branch, conspicuously not routed through the Config-page gate the
   AB17d item-9 ruling applied to its siblings (e.g.
   adaptiveStringSearcherTables, VM.h ~295). Switched to
   vm.gilOffWithProcessGate() — derivation-equivalent (m_gilOff is true
   only in a useJSThreads process, so the gate's false arm coincides;
   VM.h :642-652), flag-off now reads only the frozen read-only Config
   page. Added to the owed U-T14 permitted-delta list with the rest.

4. **F1 bench gate (BLOCKER, CONFIRMED-OPEN — no change).** The reviewer is
   right and the ledger already says so (AB17e item 4): no flag-off fix
   landed, the +1.13%/9-run independent median is still above the 1%
   threshold, this host is inadmissible, and the binding V5b quiet-host
   interleaved-A/B (>=15 runs) protocol has not been executed. Nothing in
   this round claims otherwise. The latch-before-designation reorder
   remains the recorded code-fix path if the adjudicated number is red.

5. **F3 gate (BLOCKER, CONFIRMED-OPEN — no change).** Per AB17e items 3/6:
   spawned-thread-butterfly-stress 4/5 is a red gate; all pre-restoration
   shared-arraystorage-stress 5/5 evidence is void (oracle verified
   restored at JSTests/threads/jit/shared-arraystorage-stress.js:74,102);
   the proto-chain-dictionary arm is owed. Closure: 6/6 at 5/5 GIL-off full
   JIT with the restored oracle, root-causing the 1/5 failure (no
   re-rolls). The item-1 release fix above is in-family for F3's
   lost-element signatures and must be in the binary those runs use.

6. **F4 gate (BLOCKER, CONFIRMED-OPEN — no change).** Per AB17e items 1/8:
   all 28-cell evidence is stale (post-dates the lifetime fix), and two
   residual core classes are open (libpas double-free core.591777/643552 —
   plausibly the lifetime fix, unverified; mid-instruction-PC core.640254
   under --useDFGJIT=0). CONFIRMED in-family per the Repatch finding: no
   landed fix covers live baseline IC repatch/reset (resetGetBy/resetPutBy
   repatchSlowPathCall family, InlineCacheCompiler stub replacement)
   against concurrent execution, and no named invariant exists for that
   surface. CHARTER (next F4 round): triage core.640254-class to the exact
   rewritten range; name the invariant (epoch-retire replaced jump targets,
   or route baseline IC dispatch through an immutable published record like
   the call path); then the fresh 28-cell matrix on the post-fix binary.

7. **Flag-off hot path "not restored to old form" / new branches outside
   the permitted delta (MAJOR, CONFIRMED-OPEN).** Verified reachable
   flag-off accretion: putDirectInternal Options::useJSThreads() +
   RESTART-arm branches (JSObjectInlines.h ~899+),
   addPropertyTransitionToExistingStructure dispatch
   (StructureInlines.h ~707), Structure::add l6Locker arm
   (StructureInlines.h ~450), the AB17c LLInt ifJSThreadsBranch per-op byte
   tests (LowLevelInterpreter64.asm :1709/:1869-2896,
   LowLevelInterpreter.asm virtualThunkFor), RegExpInlines
   ovectorSpan/compileIfNecessaryMatchOnly two-byte-test gates (:53,:280),
   ~CallLinkInfoBase dtor byte test, and this round's item-3 gate. No
   flag-off-reachable TLS reads found (the VMLite::currentIfExists sites
   all sit behind gates false flag-off) — consistent with the reviewer.
   The "strictly cost-reducing" wording was already retracted (AB17e item
   5; VM.h ~605-616). OWED, unchanged: the U-T14 golden-disasm
   permitted-delta audit over the consolidated list (AB17d items 1/9/11 +
   AB17c LLInt + AB17e dtor gates + AB17f item 3), and the
   latch-before-designation reorder as the single change that drops the
   second byte test from every gilOffWithProcessGate site.

8. **Stale baselines V6/V1/V5a (MAJOR, CONFIRMED-OPEN).** Correct: the
   AB17c/d/e LLInt record reroute and virtualThunkFor revalidation execute
   GIL-ON under useJSThreads=1, and no post-edit V6/V1/V5a run is recorded.
   The full pinned ladder (V0, V1 smoke, V5a identity, V6 GIL-on corpus,
   V2/V3) must re-run on the current tree before any family closure;
   property-wtr-isolation.js stays a cited EXPECTED-FAIL (CVE-AUDIT-STATUS
   CHECK-NOW §C.3) in V2/V3 accounting.

9. **installCode reader-side TSO-only (MAJOR, ALREADY-RECORDED).** The
   exact gap the reviewer names is recorded in-file as the IT-8 KNOWN
   RESIDUAL (ScriptExecutable.cpp :286, :347-353 — "writer-side fence
   only; ARM64 load-load reordering... chartered for the next IT-8
   round"). Real, open, chartered; the arm64 CI shipping point stands —
   the IT-8 arm64 round must precede any arm64 GIL-off release artifact.
   No further in-tree action this round.

10. **RegExpInlines "two byte tests where prior code had one" (MAJOR,
    CONFIRMED, already conceded).** Accurate op count; AB17e item 5 already
    corrected the comment and recorded the latch reorder as the path to a
    true one-byte gate. Folded into the item-7 owed audit list. No code
    change (the reorder is explicitly deferred as too risky for a solo
    round — that ruling stands).

Formal gate state after AB17f, unchanged from AB17e item 8: **F1 OPEN, F3
OPEN, F4 OPEN.** This round's code deltas: Butterfly.h release CAS x2 (+
residual records), CallLinkInfo.cpp comment supersession x2 (no behavior),
heap/Heap.h Config-page gate swap, JSObject.cpp residual record. Any
implementer summary claiming a green family supersedes nothing here.

## AB17g — apply round for F1-transition-heavy-flagoff-residual (3/3 approve, with binding amendments)

Scope rule for this round: writes confined to InlineCacheCompiler.cpp,
StructureInlines.h, Structure.cpp, INTEGRATE-ungil.md. No build this round —
the next verify round builds and adjudicates. Gate state is NOT flipped here:
**F1 stays RED/OPEN** (see item 6).

1. **U-T14 golden-disasm adjudication ORDERED FIRST (owed, next verify
   round).** Before any further F1 code churn: dump make()'s
   Baseline/DFG/FTL code and the put IC stubs flag-off
   (JSC_dumpDisassembly=true JSC_dumpDFGDisassembly=true on the bench-gate
   transition-heavy-constructor source, no thread flags) on the current
   Release binary AND on a same-toolchain pre-threads reference build; diff
   against SPEC-jit I1's permitted-delta list (consolidated AB17d items
   1/9/11 + AB17c LLInt + AB17e dtor gates + AB17f item 3). Source-audit
   expectation: BYTE-IDENTICAL — every threads split on the flag-off
   put-transition emission path is an emission-time C++ branch whose
   flag-off arm is verbatim pre-threads (InlineCacheCompiler.cpp
   transitionHandlerImpl ~5865 flag-on-only breakpoint; compiled-per-case
   AccessCase::Transition RELEASE_ASSERT(!Options::useJSThreads()) at
   ~3787/~3945/~3972 then unmodified emission; Replace defer ~3731;
   FTLLowerDFGToB3.cpp compilePutByOffset ~13468, compileMultiPutByOffset
   ~13574/~13597, compileAllocate/ReallocatePropertyStorage ~11957-11979;
   AssemblyHelpers FLAG-OFF IDENTITY block ~656). If the diff is clean, the
   perf-record "hot PCs in [JIT] make()" is NOT evidence of an emitted-code
   delta (it merely localizes the loop) and the residual is the C++
   operation tail (butterfly reallocation per constructed object) plus
   measurement. If NOT clean, the offending site is by construction one of
   the enumerated branches; fix = restore that arm's exact pre-threads
   emission.

2. **Latch-before-designation reorder (PRIMARY code fix) — NOT LANDED this
   round: root cause is outside this round's writable set (VM.cpp, VM.h,
   JSCConfig.h).** Chartered for the next round that owns those files, with
   the reviewers' BINDING amendments folded in:
   (a) MECHANISM IS A SPLIT, NOT A REORDER of Config::finalize(): extract
       the s_gilOffProcessLatchOnce call_once body (JSCConfig.h ~79-100,
       minus WTF::Config::finalize(), keeping the options.isFinalized and
       isPermanentlyFrozen RELEASE_ASSERTs) into a standalone latch-only
       entry (Config::latchGILOffProcess()), invoked strictly BEFORE the
       VM ctor's m_gilOff designation at VM.cpp ~407 (or once in
       JSC::initialize after Options::finalize — simpler to reason about).
       Config::finalize() stays at VM.cpp ~711 unchanged: the spent
       call_once makes its latch a no-op and the forceFencedBarrier options
       store at ~709-710 keeps its required position BEFORE the freeze
       (M8/GT#7). Moving the finalize() call itself is FORBIDDEN — it
       faults every first flag-on VM (V1/V2/V3/V5a/V6 all crash).
   (b) Then drop the second fallback term from VM::gilOffWithProcessGate()
       (VM.h ~644-655: delete the `if (g_jscConfig.options.useJSThreads)
       [[unlikely]] return m_gilOff;` term), halving every flag-off
       gilOffWithProcessGate site — most relevantly
       Heap::allocationClientForCurrentThread (Heap.h ~1833), which runs
       once per constructed object in transition-heavy-constructor via
       operationReallocateButterflyToHavePropertyStorage* — from two
       predicted-false read-only-page byte tests to one.
   (c) HB/proof obligation: rewrite the VM.h ~624-641 comment to the new
       invariant "gilOffProcess==0 implies m_gilOff==0 in every reachable
       state on every thread", with the FULL four-case reader enumeration:
       (i) constructing thread — program order, latch before designation;
       (ii) concurrent embedder second-VM ctors — the call_once return edge
       (JSCConfig.h ~60-63), valid because the latch now precedes every
       gate call in each ctor body; (iii) Thread()-spawned mutators —
       pthread_create publication; (iv) helper threads (compiler/GC/
       watchdog/sampling) and VMLiteRegistry walkers — generalize the
       JSCConfig.h ~64-70 reader-side visibility premise from "LLInt
       readers" to every C++ gilOffWithProcessGate reader (each reaches the
       VM through a lock/queue publication happening-after ctor completion,
       hence after the latch). pthread_create alone is NOT a sufficient HB
       statement. Safety: the only window the dropped fallback covered is
       the first-VM-ctor JSLockHolder before Config finalize; no Thread()
       can exist there (spawn requires a constructed VM), so latching
       earlier creates no state where a second mutator reads
       gilOffProcess==0 while m_gilOff==1.
   (d) Add a debug assert in the VM ctor that the latch ran before
       designation (g_jscConfig.gilOffProcess set in any isGILOffProcess()
       ctor reaching the ~407 designation).
   (e) The AB17e item-5 / AB17f item-10 "too risky for a solo round"
       deferral is superseded ONLY because this lands via the full
       propose/review/apply loop. The gate change is mode-shared: V6 GIL-on
       (92-test corpus) and the V1 GIL-off smoke rung MUST re-run on the
       post-reorder binary before any F1 adjudication (the
       provably-equivalent predicate change is exactly what those rungs
       check).

3. **(3a) Dispatcher do-not-rekey RULING LANDED (StructureInlines.h
   addPropertyTransitionToExistingStructure ~778).** KEEP keyed on
   Options::useJSThreads() and record as within I1's permitted delta — it
   already reads the frozen read-only Config page, i.e. it IS the single
   one-byte-test form the FIX-5 note asked for. DO NOT re-key to
   g_jscConfig.gilOffProcess: GIL-ON useJSThreads (V6) has N mutators and
   StructureTransitionTable::add's single-slot->map inflation allocates
   (can GC => GIL yield) between map construction and the m_data publish;
   keyed on gilOffProcess (==0 GIL-on) the lookup would take the lock-free
   Impl and trySingleTransition would load the half-published m_data.
   Keyed on useJSThreads it routes to the locked Concurrently variant.
   The in-code comment's earlier "re-key when the latched gate exists"
   sentence is superseded in place.

4. **(3b) Flag-on arm outlining LANDED (StructureInlines.h
   addOrReplacePropertyWithoutTransition ~458).** The flag-on
   GCSafeConcurrentJSLocker + steal-retry arm is wrapped in a NEVER_INLINE
   IIFE returning std::tuple<PropertyOffset, unsigned, bool>, so flag-off
   instantiations of every put site carry only the predicted-false byte
   test + a never-taken call. Pure code placement; the m_lock critical
   section is unchanged — no new interleaving. Apply-time check (reviewer
   amendment C) VERIFIED: both control paths in the arm (found-existing
   early return; add tail return) return through the lambda; no
   fall-through into the flag-off tail exists. The proposed file-local
   static-helper fallback is REJECTED as uncompilable (Structure::pin is
   private — Structure.h ~1173 — plus m_propertyHash/m_seenProperties
   access): if any CI toolchain (clang/gcc/MSVC) rejects NEVER_INLINE in
   the lambda position, the outlining is deferred outside-scope and the
   plain arm restored — recorded in the in-code comment. Build risk owned
   by the next verify round (no build this round by charter). Note:
   Structure::add itself (StructureInlines.h ~263) has NO mode split (the
   locker is unconditional, pre-threads form) — the AB17f item-7 phrase
   "Structure::add l6Locker arm" denotes THIS site; no second sibling
   exists to outline.

5. **(3c) No InlineCacheCompiler.cpp or Structure.cpp change.** Audit found
   no flag-off-reachable delta there (the Concurrently variant is already
   out-of-line in Structure.cpp per the dispatcher's icache note). Both
   files verified present and untouched this round.

6. **F1 stays RED — V5b adjudication protocol unchanged and unmet.** The
   two 5-run sessions behind the "+10.59% (was +1.78%)" headline are
   INADMISSIBLE under the binding AB17e item-4 protocol (this host:
   documented ~9% run-to-run spread, a prior independent 9-run median of
   +1.13%, and a same-suite bench reading 12.2% FASTER than baseline);
   equally, those sessions cannot prove the regression GREW. Closure
   requires: quiet host, pinned CPU/governor, interleaved A/B against a
   same-toolchain reference build, >=15 runs, number recorded here. No
   green claim is made for this round's deltas; they are candidates whose
   effect only the adjudicated number can confirm.

This round's code deltas: StructureInlines.h x2 (item 3 comment
supersession; item 4 NEVER_INLINE IIFE outlining). Ledger: this section.
Gate state after AB17g: **F1 OPEN, F3 OPEN, F4 OPEN** (unchanged).

## AB17h — final-binary re-verification round (post-AB17e reviewer findings)

Charter: re-establish every AB17e closure claim against the FINAL tree
(Release jsc rebuilt 08:34 after the 08:32 Structure.cpp/StructureRareData.h
edits; Debug jsc REBUILT this round — `ninja jsc` had real work, confirming
the 08:13 Debug binary used for the AB17e closing evidence was stale), and
either fix or refute the seven reviewer findings. Build proof: Release
`ninja jsc` reports "no work to do" (the 08:34 binary IS the final tree);
Debug relinked clean (206/206). No Source/** edits this round — all deltas
are Tools/threads/load6.sh (new) + this ledger — so the re-verified binaries
remain the binaries of record. Binaries of record (sha256 prefix):
Release jsc 56726e501eac2c1e, Debug jsc d42145b0b88ead2b. Future bench/test
closure claims MUST record the gate output, /proc/loadavg, and the binary
hash together (reviewer T1 exit-criterion amendment, adopted).

1. **T1/F1 flag-off bench (BLOCKER, CONFIRMED-OPEN — not adjudicable on
   this host; no green claim).** First measurements of the FINAL Release
   binary, all with /proc/loadavg recorded:
   - Official gate (9-run medians), three valid runs: +0.62% PASS
     (loadavg 2.0-2.4), +1.32% FAIL (2.32), +2.42% FAIL (2.49). A fourth
     run that overlapped a 64-way ninja rebuild read +63% and is discarded.
   - 31 consecutive unpinned runs: median 55.693 ms (+1.41%), range
     51.757-58.899 ms — a 13% spread, consistent with the AB17f item-6
     record of ~9% run-to-run spread on this host.
   - 31 consecutive runs pinned with `taskset -c 40`: median 51.752 ms
     (**-5.76% vs baseline**), min 48.858 ms (-11.0%).
   - Instructions retired (perf stat, 3 runs): 1.27922e9 / 1.28061e9 /
     1.28093e9 — stable to ±0.07% (~183 instructions per constructed
     object including warmup) while wall time varied 13%. The work the
     binary executes is CONSTANT; the wall-clock deltas are cycle/IPC
     (scheduling, migration, cache) effects, NOT extra instructions.
   Disposition: the reviewer's two FAIL gates (+2.04%/+1.89%) are
   REPRODUCED in kind (2 of my 3 gate runs also fail) but the pinned
   median 5.8% BELOW baseline and the instruction-count stability refute
   "isolated and stable rules out host noise": this bench is the most
   allocation/GC-bound of the suite (perf profile: ~70% identical-codegen
   JIT, remainder MarkedBlock sweep/tryCreate + kernel page alloc), i.e.
   exactly the one most sensitive to host state. Under the BINDING AB17f
   item-6 protocol (quiet host, pinned governor, interleaved A/B against a
   same-toolchain REFERENCE BUILD, >=15 runs) the item cannot be
   adjudicated here: no reference binary exists (baseline.json was
   recorded 2026-06-05 over the same WebKitBuild/Release path, since
   overwritten) and rebuilding one requires repo history access this
   round does not have. F1 stays OPEN; closure path unchanged: build the
   pre-threads (or pre-AB17e) reference jsc, interleave A/B pinned, and
   record the number here. Code-side audit done this round found NO
   ungated flag-off delta on the transition path: putDirectInternal's
   constexpr split (JSObjectInlines.h ~957-1030), the static butterfly
   publish (nukeStructureAndSetButterflyStatic), every DFG/FTL threads
   emission gated at JIT-compile time (DFGSpeculativeJIT.cpp,
   FTLLowerDFGToB3.cpp — all sites are `if (Options::useJSThreads())`
   around EMISSION, so flag-off machine code is unchanged), the
   allocation-client dispatch (Heap.h FIX-V5B-F1, one predicted-false
   Config-page byte test), and operationReallocateButterflyAndTransition's
   gated handler Ref (JITOperations.cpp). JSCell::structure()'s
   unconditional decontaminate() (JSCell.h:203) is the one audited
   flag-off ALU delta: a single register AND between two register ops (no
   memory traffic, no branch); at ~183 instructions/object total and with
   C++ structure() off the per-object steady-state path, it cannot
   account for percent-level deltas — EXONERATED with this note standing
   in for the unpublished AB17e "5-file list". Gating it behind a runtime
   branch would cost more than the AND; revisit only if the interleaved
   A/B protocol ever lands and still shows red.

2. **T2 spawned-thread-butterfly-stress (MAJOR, REFUTED-AS-STATED — the
   mechanism IS in the tree; closure artifact assembled here).** The
   reviewer dismissed ConcatKeyAtomStringCacheInlines.h:80 as "a PRIOR-
   round fix describing a different symptom"; both halves are wrong.
   (1) FAULTING FRAME (on-disk ASAN artifacts,
   /tmp/sbs-load-fail-{7-1,12-2}.log, written 05:34-05:39 DURING AB17e):
   SEGV reading address 0x000000000005 in
   cellHeaderConcurrentLoad<JSType> ← JSCell::isSymbol ←
   CacheableIdentifier::getCacheableIdentifier(JSValue) ←
   operationPutByValSloppyOptimize (JITOperations.cpp:1854) /
   operationGetByValOptimize (:3755): the IC slow path received an EMPTY
   JSValue as the property-key subscript — bits 0, isCell() true, null
   cell + m_type offset 5 = the 0x5 address. (2) THE CHANGE THAT CLOSES
   IT: ConcatKeyAtomStringCacheInlines.h flag-on rewrite (mtime 07:16,
   AFTER the crashes), whose hole-(3) note names this exact signature
   ("key was published BEFORE value, so a pointer-matching reader could
   load a still-null value (observed as the empty-JSValue subscript SEGV
   in operationGetByValOptimize/putByValOptimize)") — the cache is
   graph-owned and shared by N mutators through the shared DFG/FTL
   CodeBlock; a foreign thread's quick-cache key pointer-matched (digits
   0-9 are immortal shared singleCharacterStrings) while the value slot
   was still null, and the null flowed into the get_by_val/put_by_val
   subscript. (3) HAPPENS-BEFORE EDGE: writer publishes
   entry.m_value.set(...) → WTF::storeStoreFence() → entry.m_key.set(...)
   under m_lock with slot-once fills (slot index = locked map size);
   reader orders key-match before value load, so a matching key now
   implies a published value — and defensively, flag-on DFG/FTL no longer
   read the quick entries at all (compileMakeAtomString defers to
   operationMakeAtomString*WithCache). Sibling same-round closers for the
   other three symptoms of this test: NumericStrings per-thread instance
   (06:50), MegamorphicCache fills disabled flag-on + the
   hasMegamorphicProperty bail (06:53/06:56), CompactPointerTuple
   setPointer racy-profiling assert (07:06). The registers in the reports
   also carry ASAN redzone magics (rcx=0xf3f3f300f1f1f1f1, rsi=0xf3) in
   NON-operand registers; the literal stack-use-after/alloca-redzone
   manifestation has not reproduced on any post-rebuild tree (210+ runs)
   and the "alloca-redzone UAF" framing of this item should be retired in
   favor of the empty-JSValue/wrong-atom shared-cache mechanism above.
   What this round adds besides the artifact: the 6-way load harness is
   now committed and re-runnable — **Tools/threads/load6.sh**
   (parameterized version of the /tmp/item2 script; 6 workers x 20 runs,
   rotating --randomYieldSeed, pinned GIL-off flag set) — and the 0/120
   result was re-established on the FINAL Debug binary (see item 4).

3. **T4 GIL-ON put_by_id/delete_by_id livelock (MAJOR, FIXED-AND-NOW-
   DISCHARGED; one prior-record claim CORRECTED).** The Structure.cpp:399
   born-invalid inheritance fix is verified end-to-end (convergence
   argument as recorded there). New evidence, final Debug binary, via an
   instrumented copy of the staged reducer (describe() dictionary scan +
   $vm.getStructureTransitionList chain length, sampled every 5 passes):
   - CLIFF ARITHMETIC DISCHARGED: with JIT, GIL-on, the longest victim
     transition chain crosses s_maxTransitionLength=128 (Structure.h:219;
     the REMOVE path's transitionCountHasOverflowed uses the 128 limit
     regardless of PutById context — Structure.h:295-305, driven here by
     the per-pass `delete o.g` plus thread B's flood deletes) at
     **pass 129 of the mono phase**, immediately followed by the chain
     collapsing to ~1 (the dictionary-pin + flatten fingerprint:
     previousID cleared). That is the observed PASSES=120-fine /
     PASSES>=135-wedge cliff: the first family crosses 128 between those
     counts, goes cachedDictionaryTransition (hasBeenDictionary set), and
     pre-fix the NEXT transition-form put against that family entered the
     unbounded fire->RESTART loop.
   - 10x CONVERGENCE: PASSES=1500 (10x the wedging count) completes in
     34s GIL-on full-JIT Debug, crossing at the same pass 129 — the fix
     removed the non-convergence; it did not move the cliff.
   - Both staged reducers (ic-put_by_id-vs-transition.js,
     ic-delete_by_id-vs-transition.js, default PASSES=150) PASS in ~2.4s
     GIL-on on the final Debug binary (pre-fix: >12 min wedge).
   - NO-JIT CORRECTION (this round REFUTES the prior hypothesis): the
     livelock precondition IS reachable under --useJIT=0. The probe shows
     the chain crossing 128 (mega phase, pass 54) and collapsing no-JIT
     too, and the flatten-back is NOT JIT-only (LLInt flattens at
     LLIntSlowPaths.cpp:981). So "dictionary/flatten onset is dead under
     --useJIT=0" is FALSE and must not be cited as the no-JIT immunity
     mechanism. The pre-fix no-JIT immunity (PASSES=150 in 3.5s) remains
     UNEXPLAINED (plausibly the crossing landing late in the phase walk
     plus interleaving differences; not proven — the pre-fix binary no
     longer exists to instrument). This does NOT weaken the closure: the
     born-invalid convergence argument is tier-independent and does not
     rest on explaining the old immunity.

4. **T5/T6 stale-binary closures (MAJOR, CONFIRMED — now re-verified on
   the final binaries).** The reviewer's premise was correct: Debug ninja
   had real work this round, so every 08:17-08:23 corpus log predated the
   tree's final state. Re-runs on the rebuilt binaries:
   - Corpus GIL-on (plain run-tests.sh): **93 passed / 0 failed / 2
     skipped** (/tmp/tl2/corpus-gilon.log).
   - Corpus GIL-off (pinned env): **92 passed / 0 failed / 3 skipped**
     (/tmp/tl2/corpus-giloff.log).
   - 120x spawned-thread-butterfly-stress under Tools/threads/load6.sh
     (6 workers x 20 runs, rotating seeds, GIL-off pinned flags, final
     Debug binary d42145b0): **120 runs, 0 failures**.
   - Both staged ic reducers: PASS (item 3).
   - V5a flag-off identity (Tools/threads/v5a-identity.sh, 40 stress
     tests, --useJSThreads=false vs no flags, final Release binary):
     40/40 OK, 0 mismatches.

5. **Stuck-process note for future bench rounds.** This host carries a
   wedged `bun test --inspect-wait` process (PID 785900, pegging one core
   since May 26, ~19,000 CPU-minutes). It is one steady core of load in
   every "quiet host" reading on this machine, including the AB17d
   baseline gates. Not killed this round (not ours); kill or migrate
   before any future adjudicated A/B session.

Gate state after AB17h: **F1 OPEN** (item 1; adjudication protocol unmet,
reference build required), **F3/F4 per AB17g (unchanged by this round)**.
T2 is CLOSED with the assembled mechanism artifact (item 2: faulting frame
↔ ConcatKeyAtomStringCacheInlines.h hole-(3) fix ↔ value-before-key
happens-before, harness committed, 0/120 re-run on the final binary).
T3/T4 closures are now VERIFIED on the final binaries (items 3-4).

## AB17i — reviewer-findings verification round (post-AB17h)

All seven external findings verified against the tree; two code fixes
landed, the rest are confirmed-open carries (no gate state was closed).

1. **AUD1.N4(3) enumerator install — FIXED.** Confirmed real: the
   Structure::setCachedPropertyNameEnumerator install (Structure.cpp) ran
   with no m_lock while the StructureRareDataInlines.h setter destroys and
   rebuilds m_cachedPropertyNameEnumeratorWatchpoints — two mutators
   racing for-in caching on a shared Structure GIL-off could free
   watchpoints the other thread had just linked into live transition
   WatchpointSets (UAF on fire) and tear the enumerator/flag publication.
   Landed per the AUD1.N4(2) rule: ConcurrentJSLocker(m_lock) +
   winner-keeps in Structure::setCachedPropertyNameEnumerator;
   storeStoreFence before the JIT-read flag word in the setter (published
   LAST, after the vector it summarizes); the StructureRareData.cpp
   AUD1.N4(3) record flipped OPEN -> RESOLVED. No nested ConcurrentJSLock
   (tryCachePropertyNameEnumeratorViaWatchpoint only reads chain structures
   and calls lock-free InlineWatchpointSet::add); fires stay K4.VI.2.
   Flag-off: uncontended lock + one fence on a cold path; no codegen change
   on bench paths.

2. **amplify.sh divergence check was structurally false-red — FIXED.**
   Confirmed: RaceAmplifier.cpp logs the per-run seed
   ("[RaceAmplifier] enabled: ... seed=<N> ...") onto the compared stream,
   and amplify.sh raw-cmp'd full outputs against a reference run with a
   different seed, so EVERY amplified run diverged by construction (the
   ~99/100 DIVERGENCE findings on the races filter were all banner-only).
   Fix: outputs are compared with '^\[RaceAmplifier\]' lines stripped from
   both sides; exit-status divergence checking and raw replay logs are
   unchanged. CONSEQUENCE: every prior amplified-rung PASS/FAIL produced by
   amplify.sh output-comparison is void as divergence evidence (rc-based
   evidence stands); the amplified races rung must be re-run as real
   evidence in the next verify round.

3. **F1 flag-off bench — CONFIRMED-OPEN (no change, three findings
   agree).** Independent pinned measurement on the final binary still
   reads transition-heavy-constructor above the 1% gate after control
   normalization. Closure path unchanged and binding (AB17f item 6 /
   AB17h amendment): kill/migrate wedged PID 785900; build a
   same-toolchain reference jsc in a separate build dir (requires
   repo-history access — must be chartered explicitly); interleaved
   pinned A/B, >=15 runs/binary, gate output + /proc/loadavg + binary
   sha recorded together. Do NOT re-record baseline.json without
   orchestrator sign-off.

4. **AB17g item 2 (latchGILOffProcess reorder) — CONFIRMED-OPEN, carried
   verbatim.** Verified: no latchGILOffProcess symbol exists;
   VM.h gilOffWithProcessGate() still carries the two-term fallback. The
   bench-hot site remains covered by FIX-V5B-F1 (Heap.h
   allocationClientForCurrentThread single Config-page byte test). After
   landing, re-run V6 GIL-on corpus + V1 GIL-off smoke per amendment (e).

5. **AB17g item 1 (golden-disasm adjudication) — CONFIRMED-OPEN, carried.**
   No disasm artifact exists; the AB17h source-level audit stands but is
   not the ordered empirical proof. Bundle the disasm diff into the same
   chartered session that builds the reference jsc for item 3 (one
   reference build serves both obligations).

Gate state after AB17i: **F1 OPEN** (item 3), F3/F4 per AB17g (unchanged).
AUD1.N4(3) closed in-code (item 1); amplified-races rung evidence reset
(item 2) — re-run owed.
