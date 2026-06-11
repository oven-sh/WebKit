# SPEC-ungil Annex K4 (BINDING, audit executed)

Status: executed 2026-06-06 against the tree at branch jarred/threads.
This is the §K.4 inventory audit (U-T8b input; gates U-T9): every
VM-/JSGlobalObject-resident or process-global mutable member that N
concurrently-entered threads can reach and whose ONLY serializer today
is the GIL, ruled into the §K scheme. Implementation tasks consume the
tables verbatim; rows are addressed as K4.<table>.<row>.

Classification key (maps to SPEC-ungil §K classes):

| Class | Meaning | Spec authority |
|---|---|---|
| per-lite | duplicate into VMLite; GIL-off accessors route to CURRENT lite; cell-holding copies GC-scanned via registry walk | §K.1 |
| lock | shared hits required; leaf Lock per §LK.7 (or existing lock verified) | §K.2 |
| lazy-publish | first-touch CAS publication, then immutable (immutable-after-init with a publication protocol) | §K.3 / annex LZ1+LZ2 |
| immutable-after-init | written only during VM/global ctor or pre-thread embedder setup; GIL-off mutation asserted absent | §K (degenerate K.3: no protocol needed; add debug assert) |
| main-only | reachable only via debugger/inspector/profiling/test options; GIL-off restricted to the designated thread or no-op'd (SD13/SD14 pattern) | §A.2.7-8, SD13/SD14 |
| requires-stop | class 4: writer iterates/rewrites other threads' state; §A.3 stop conductor | §K.5 |
| already-safe | existing lock/atomic/stop-window discipline verified sufficient | — |

---

## 0. Residue dispositions — ALL RESOLVED at spec rev 26

Former UNRESOLVED items U1-U7. U1-U4 carry spec rulings (SPEC-ungil
§K.6, FULL text history ANNEX AUD1); U5-U7 were purely MECHANICAL and
are reclassified here with rationale. No row blocks U-T9.

- **U1 — SamplingProfiler — RESOLVED (AUD1.K1, SD18).** GIL-off the
  profiler samples ONLY the main/carrier thread (§A.1.7 form (i),
  SUSPEND RULE applies); spawned frames never captured; start/stop/
  report main-only (SD13/SD14 family). Internals keep `m_lock` (:218).
  N-thread capture is post-ungil. Row V.3 re-ruled main-only.
- **U2 — m_regExpGlobalData — RESOLVED (AUD1.K2, SD19).** Per-lite
  (§K.1): each thread owns a private RegExpGlobalData stream;
  RegExp.$1-$9/lastMatch etc are per-thread GIL-off. Tier-inlined
  RecordRegExpCachedResult re-pointed via the lite (AUD1.K4 A16 ext).
  Joins the registry-walk root set + ~VM walk. Regexp corpus arm
  UNBLOCKED (SD19 GILOff variants).
- **U3 — module evaluation state — RESOLVED (AUD1.K3).**
  m_moduleAsyncEvaluationCount = atomic fetch_add, relaxed (row
  III.16 order question discharged: uniqueness+monotonicity suffice).
  m_synchronousModuleQueue = per-lite (§E.1 family) — row V.18
  RE-RULED per-lite (NOT main-only). Cross-thread Evaluate() of one
  record = status claim under the record's cell lock (N7 R16);
  losers adopt the top-level promise (async) or park-capable wait
  access-released (sync).
- **U4 — JIT-baked per-lite cache addresses — RESOLVED (AUD1.K4).**
  ANNEX A16 EXTENDED to MegamorphicCache, HasOwnPropertyCache,
  m_regExpGlobalData and m_weakRandom (VIII.10): gilOff-mode
  compilation indexes the lite-resident copy via loadVMLite;
  flag-off keeps baked addresses. Rows II.18/II.19 JIT arms
  discharged; no locked fallback needed (caches private per lite).
- **U5 — DWT lock-name drift — MECHANICAL.** Spec §E.7.1's
  `m_pendingLock` IS the in-tree `m_taskLock`
  (`DeferredWorkTimer.h:116`) — name equation now noted in spec
  §E.7/§LK.7. Its coverage EXTENDS to `m_pendingTickets` (:121);
  the :125-126 three-condition comment loses the GIL leg. One §LK.7
  leaf lock; row VII.4 final.
- **U6 — m_canFastQueueMicrotask / m_associatedContextIsFullyActive —
  MECHANICAL (SD13 umbrella).** Writes main-only (debugger/context
  attach); reads relaxed-atomic from any entered thread. A stale-true
  window at most skips debugger microtask observation for in-flight
  enqueues = SD13-class degradation; no new SD. Classified main-only
  + relaxed-atomic reads.
- **U7 — SmallStrings lazy tail — MECHANICAL (verification PASSED).**
  `initializeCommonStrings` runs in the VM ctor (VM.cpp:335); the
  `!m_isInitialized` fallback (`SmallStrings.cpp:121-127`) allocates
  a fresh AtomStringImpl and writes NO member; `setIsInitialized
  (false)` is teardown-only (VM.cpp:707). immutable-after-init
  CONFIRMED unconditionally; row VIII.6 condition discharged.

Everything below this line is RULED.

---

## I. VM execution state already ruled elsewhere (listed for closure; no new ruling)

| Member (file:line) | Class | Rationale / authority |
|---|---|---|
| Group-3 set: topCallFrame, exception/unwind words, stack limits (VMLitePrimitives X-macro block, `VM.h` top; comment :1263-1265) | per-lite | §A.1.3 Group-3 storage; vmstate L1-L5 ABI |
| `m_exception`/`m_lastException`/`m_needExceptionCheck`/`m_throwingThread` (`VM.h:1273-1276`, setException comment :1228) | per-lite | vmstate I15 + §A.1.3; throw state is thread-local |
| `entryScope` (`VM.h:939`), `isEntered()` (:298) | per-lite | §A.1.5 per-entry record; moved into lite |
| `m_entryScopeServicesRawBits` (`VM.h:444`) | per-lite | §A.1.5 service routing: VM-wide word + registry fan-out |
| `m_executingRegExp` (`VM.h:911`) + regexp match/stack buffers, `m_regExpAllocator`/`m_regExpAllocatorLock` (`VM.h:951-952`) | per-lite | §A.1.3 Group-3 explicitly lists lazy regexp stack/match buffers; allocator becomes per-lite, lock retired GIL-off |
| Scratch buffers: `m_scratchBufferLock`/`m_scratchBuffers`/`m_sizeOfLastScratchBuffer` (`VM.h:1300-1302`), threading comment :914-916 | per-lite | annex A16 (BINDING): per-lite segmented tables |
| `m_checkpointSideState` (`VM.h:1303`) | per-lite | OSR side state is per-frame, per-thread (§A.1) |
| Microtasks: `m_defaultMicrotaskQueue` (`VM.h:1375`), `queueMicrotask` (:1026) | per-lite | §E / vmstate §6.6 per-lite queue; rerouted |
| `m_microtaskQueues` registry list (`VM.h:1280`) | lock | queue REGISTRATION list (not drain path); §LK.7 leaf; cold |
| `m_aboutToBeNotifiedRejectedPromises` (`VM.h:1321`) | per-lite | SD15 rejection-tracker carrier-queued (§E.1b.4) |
| Termination/traps: `m_terminationException` (:399), `m_hasTerminationRequest` (:1363), `m_executionForbidden{,OnTermination}` (:1364-1365) | already-safe | §A.2 VMTraps fan-out + SD8/§E.5; termination exception = sticky release-publish |
| `m_apiLock` (`VM.h:465`) | already-safe | IS the §LK rank-1 lock; GIL-off protocol per §F |
| `m_syncWaiter` (`VM.h:1376`) | already-safe | WaiterListManager internally locked; §C waits ruled |
| `m_numberOfActiveJITPlans` (`VM.h:1378`) | already-safe | std::atomic, relaxed (:1169) |
| `m_mainVMLite` (`VM.h:1287`) | immutable-after-init | set once in ctor; lites managed by registry |
| `m_gilOff` byte (VM ctor, U0c) | immutable-after-init | U0c BINDING: set ONCE in ctor |

## II. VM string/number/per-op caches — class per-lite (§K.1)

All are hot, mutated on ordinary JS paths, value-cache semantics (a
miss is only a perf event), and hold GC cells => per-lite copy,
GC-scanned via the registry walk (§A.1.3 GC-roots rule).

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.II.1 | `numericStrings` (`VM.h:657`) | named in §K.1; number->string per-op cache |
| K4.II.2 | `lastCachedString` (`VM.h:659`) | single-entry rope/string cache, torn cross-thread reuse unsound |
| K4.II.3 | `lastAtomizedIdentifierStringImpl` / `lastAtomizedIdentifierAtomStringImpl` (`VM.h:660-661`) | last-atomization memo; pure per-thread locality |
| K4.II.4 | `jsonAtomStringCache` (`VM.h:662`) | JSON.parse key atomization scratch |
| K4.II.5 | `keyAtomStringCache` (`VM.h:663`) | property-key atomization scratch |
| K4.II.6 | `stringSplitCache` (`VM.h:664`) + `stringSplitIndice` (`VM.h:665`) | named in §K.1 (as stringSplitIndice); split result cache + index scratch vector |
| K4.II.7 | `stringReplaceCache` (`VM.h:666`) | replace result cache, same shape as split |
| K4.II.8 | `m_cachedSortScratch` / `m_sortScratchSentinel` (`VM.h:637-638`) | Array.prototype.sort scratch butterfly; live across one sort only |
| K4.II.9 | BigInt divisor cache: `m_cachedBigIntDivisor`, `m_nextCachedBigIntDivisor`, `m_bigIntCachedInverse`, `m_bigIntDivisorCount` (`VM.h:680-683`) | multi-word cache mutated on BigInt modulo; not CAS-able as a unit |
| K4.II.10 | `stringRecursionCheckFirstObject` / `stringRecursionCheckVisitedObjects` (`VM.h:941-942`) | recursion guard tracks the CURRENT thread's JS stack; sharing is wrong, not just racy |
| K4.II.11 | `dateCache` (`VM.h:944`, class `runtime/JSDateMath.h:87`) | §N.3 ruling: "vm.dateCache = §K.1/2"; ruled K.1 (timezone/parse caches hot); timezone-change notification fans out via registry |
| K4.II.12 | `m_stringSearcherTables` (`VM.h:1311`) | adaptive search scratch tables; creation = lazy-publish (LazyUniqueRef), contents per-lite |
| K4.II.13 | `m_random` (`VM.h:469`) | WeakRandom state advanced on JS paths; per-lite stream (seeded per-lite) |
| K4.II.14 | `m_integrityRandom` (`VM.h:471`) | Integrity audit sampling on allocation paths |
| K4.II.15 | `m_mightBeExecutingTaintedCode` (`VM.h:547`) | execution-context flag of the CURRENT thread (Group-3-adjacent) |
| K4.II.16 | `m_deletePropertyMode` (`VM.h:1291`, scope RAII :746-758) | scoped mode around the current thread's host call |
| K4.II.17 | `m_doesGC` (`VM.h:1383`) | ASSERT_ENABLED-only expectation state; per-thread by meaning |
| K4.II.18 | `m_hasOwnPropertyCache` (`VM.h:956`) | entry = {structureID, impl, result} multi-word; interleaved writes can pair a key from A with a result from B => per-lite. Creation = K.3 (LazyUniqueRef). JIT path: A16 ext (§0 U4) |
| K4.II.19 | `m_megamorphicCache` (`VM.h:960`) | multi-word epoch'd entries (`MegamorphicCache.h:90`); torn entry can satisfy the wrong key => per-lite. Creation = K.3. JIT path: A16 ext (§0 U4) |

## III. VM shared keyed caches — class lock (§K.2 leaf, §LK.7)

Hits MUST be shared (dedup/memory) or mutation is cold. Leaf Lock;
weak-handle creation inside follows §LK WS1(i): hoist Weak
construction BEFORE the lock, publish under it.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.III.1 | `m_regExpCache` (`VM.h:950`; `RegExpCache.h:79` `m_lock`, `m_weakCache` :80, `m_strongCache` :81) | already-safe — the §K.2 exemplar; verify every mutator path takes :79 (audit found none missing) |
| K4.III.2 | `m_codeCache` (`VM.h:1293`; `CodeCache.h:242 m_sourceCode`) | eval/program/module unlinked-code dedup cache; sharing is the point; parse happens OUTSIDE the lock, publish under it (§N.8 CBI pattern) |
| K4.III.3 | `m_intlCache` (`VM.h:1294`; `IntlCache.h:62-64`) | ICU pattern-generator + canonicalized-locale maps; ICU objects not thread-safe => ALL use under the leaf lock (cold) |
| K4.III.4 | `symbolImplToSymbolMap` (`VM.h:724`) | WeakGCMap, no internal lock; Symbol identity REQUIRES sharing => lock + WS1(i) hoist |
| K4.III.5 | `atomStringToJSStringMap` (`VM.h:725`) | same; with useSharedAtomStringTable the jsString dedup must be shared => lock + WS1(i) |
| K4.III.6 | `wasmGCStructureMap` (`VM.h:727`) | RTT->Structure identity map; identity requires sharing; cold => lock |
| K4.III.7 | `sourceProviderCacheMap` (`VM.h:779`) | parser info cache keyed by SourceProvider; parse-time only => lock |
| K4.III.8 | `m_impurePropertyWatchpointSets` (`VM.h:1296`) | keyed WatchpointSet registry mutated via addImpureProperty (host API) => lock; FIRING any set = requires-stop (K4.VI) |
| K4.III.9 | `m_compactVariableMap` (`VM.h:954`; `parser/VariableEnvironment.h:516 m_map`, no lock) | TDZ environment interning shared across parses => leaf lock |
| K4.III.10 | `m_symbolTableCache` (JSGlobalObject, `JSGlobalObject.h:501`) | WeakGCMap of cloned SymbolTables; codegen-time => lock + WS1(i) |
| K4.III.11 | `m_loopHintExecutionCounts` + `m_loopHintExecutionCountLock` (`VM.h:1372-1373`) | already-safe — existing lock; keep as §LK.7 leaf |
| K4.III.12 | `jitStubs` JITThunks (`VM.h:781`; `jit/JITThunks.h:260` RecursiveLock `m_lock`) | already-safe — internally locked; recursion audited by jit spec |
| K4.III.13 | `m_sharedJITStubs` (`VM.h:784`; `bytecode/SharedJITStubSet.h:134-145`) | already-safe — every accessor takes its own m_lock (landed in thread-implement, R2-2) |
| K4.III.14 | `ftlThunks` (`VM.h:787`; `ftl/FTLThunks.h:98 m_lock`) | already-safe — internally locked |
| K4.III.15 | `m_drainMicrotaskDelayScopeCount` (`VM.h:1318`) | embedder API counter; make atomic (degenerate lock); not on JS fast path |
| K4.III.16 | `m_moduleAsyncEvaluationCount` (`VM.h:1332`, ++ at :1178) | atomic fetch_add, relaxed; order discharged per §0 U3 (AUD1.K3) |
| K4.III.17 | `machineCodeBytesPerBytecodeWordForBaselineJIT` (`VM.h:658`) | stats-only SimpleStats at JIT finalize; leaf lock (or per-lite merge); no correctness payload |
| K4.III.18 | JSGlobalObject `m_installedObjectPropertyChangeAdaptiveWatchpoints` (`JSGlobalObject.h:593`) | append-only Vector of installed watchpoints; install paths race GIL-off => lock; firing = K4.VI |

## IV. VM/JSGlobalObject lazy one-shot members — class lazy-publish (§K.3, annex LZ1/LZ2)

First-touch CAS publication per `runtime/LazyProperty.h` (tags :114-115,
slow path :95-97, isInitialized :91); owner-re-entry/abandonment per
LZ1; conductor precondition per LZ2.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.IV.1 | ALL `LazyClassStructure` members (`JSGlobalObject.h:263-270`, :302-303, :450-452, :461 macro, :493) | the §K.3 named case (initLater) |
| K4.IV.2 | ALL `LazyProperty<JSGlobalObject, T>` members (`JSGlobalObject.h:281-301`, :305-312, :318-328, :340, :342, :359-373, :432-449, :469-472, :489-494) | ditto |
| K4.IV.3 | `m_linkTimeConstants` (`JSGlobalObject.h:498`) | FixedVector of LazyProperty; per-slot §K.3 |
| K4.IV.4 | VM `ensure*` LazyRef/LazyUniqueRef CONTAINERS: `m_watchdog` (:1309), `m_heapProfiler` (:1310), `m_stringSearcherTables` (:1311), `m_shadowChicken` (:1316), `m_hasOwnPropertyCache` (:956), `m_megamorphicCache` (:960) | §K.3 names "VM ensure*"; CONTENTS ruled separately (rows II.12/II.18/II.19, V.1, V.2, V.6) |
| K4.IV.5 | `m_fastCanConstructBoundExecutable` / `m_slowCanConstructBoundExecutable` (`VM.h:640-641`) | lazily created NativeExecutables on bound-function paths; single-word release-CAS publish, loser discards |
| K4.IV.6 | `m_fastRemoteFunctionExecutable` / `m_slowRemoteFunctionExecutable` (`VM.h:643-644`) | same, but Weak<> => creation obeys WS1(i) (hoist MSPL work outside any K-lock) |
| K4.IV.7 | `m_emptyPropertyNameEnumerator` (`VM.h:606`) | created on first empty enumeration; single-word CAS publish |
| K4.IV.8 | `m_exceptionFuzzBuffer` (`VM.h:1308`, alloc :924-925) | fuzz option only; still gets the CAS-publish (cheap) — effectively main-only in practice |
| K4.IV.9 | JSGlobalObject `m_rareData` (`JSGlobalObject.h:524`; struct `JSGlobalObjectInlines.h:52-57`; createRareDataIfNeeded :475-479) | pointer = §K.3 CAS-publish; contents: `profileGroup` immutable-after-init (embedder), `opaqueJSClassData` = JSC C-API class data => leaf lock (C-API entry is rare/cold) |

## V. VM debugger/profiler/tooling members — class main-only (SD13/SD14 family)

GIL-off: feature restricted to the designated (main) thread or
disabled at option validation; spawned-thread interaction = no-op
(SD13 pattern). GIL-on unchanged. Cross-thread WALKS of other lites'
frames use §A.2.7/§A.3 stops.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.V.1 | `m_watchdog` Watchdog (`runtime/Watchdog.h:40`; `m_lock` :68 guards `m_vm` :69; `m_timeLimit`/`m_cpuDeadline`/`m_deadline` :71-73; callbacks :75-77) | already-safe + annex W: internals locked; SD14 + §K.4-routed per-thread CPU deadlines (spec :186); deadline reads on JS slow paths become per-lite mirrors per annex W |
| K4.V.2 | `m_heapProfiler` (`VM.h:1310`) + `m_activeHeapAnalyzer` (`VM.h:1292`, setter :283) | heap snapshot runs inside a heap §10 stop; analyzer pointer written only by the conducting thread |
| K4.V.3 | `m_samplingProfiler` (`VM.h:1313`; `SamplingProfiler.h:218-231`) | main-only capture per §0 U1 (AUD1.K1, SD18); internals keep m_lock |
| K4.V.4 | Debugger list `m_debuggers` (`VM.h:1386`) + Debugger object (`debugger/Debugger.h:54`; `m_vm` :323, `m_globalObjects` :324, parse/blackbox maps :325-326, pause bits :330-337, `m_pauseOnCallFrame`/`m_currentCallFrame` :341-342) | SD13: spawned breakpoints no-op; attach/detach + pause machinery main-thread; cross-thread frame walks = §A.2.7 stops |
| K4.V.5 | `m_isDebuggerHookInjected` (`VM.h:1366`, setter :1176) | sticky monotonic bool; main-only writer, relaxed reads |
| K4.V.6 | `m_shadowChicken` (`VM.h:1316`) | debugger-feature log written from prologues; GIL-off active only on main thread (SD13 umbrella); if ever revived N-thread it must be re-ruled per-lite |
| K4.V.7 | `m_typeProfiler` / `m_typeProfilerLog` / `m_typeProfilerEnabledCount` (`VM.h:1297-1299`) | option-gated; log written from inline JIT paths => GIL-off refused at option validation unless single-threaded |
| K4.V.8 | `m_controlFlowProfiler` / count (`VM.h:1306-1307`) + `m_functionHasExecutedCache` (`VM.h:1305`) | same gating as V.7 |
| K4.V.9 | `m_perBytecodeProfiler` (`VM.h:946`) | Profiler::Database, option-gated tooling |
| K4.V.10 | `m_fuzzerAgent` (`VM.h:1315`) | fuzzing option only |
| K4.V.11 | `m_rtTraceList` (`VM.h:974`, ENABLE(REGEXP_TRACING)) | debug build tooling |
| K4.V.12 | `m_failNextNewCodeBlock` (`VM.h:1288`, :870-875) | test hook |
| K4.V.13 | `m_shouldBuildPCToCodeOriginMapping` (`VM.h:1290`) | sticky bool set by profiler/debugger attach; main-only writer, relaxed reads by compiler threads |
| K4.V.14 | `m_debugState` Wasm::DebugState (`VM.h:1369`) | wasm debugger; main-only (VMManager wasm-debugger stops are §A.3 territory) |
| K4.V.15 | VMInspector (`tools/VMInspector.h:40`) | already-safe — stateless static facade (post-refactor: no instance list in the header; VM enumeration lives in VMManager, row VII.2); dump entry points are debugger/REPL main-only |
| K4.V.16 | JSGlobalObject `m_inspectorController` / `m_inspectorDebuggable` (`JSGlobalObject.h:512-513`) + `m_debugger` (`JSGlobalObject.h:237`) | inspector wiring; SD13 umbrella |
| K4.V.17 | JSGlobalObject `m_globalScopeExtension` (`JSGlobalObject.h:256`) | debugger/embedder scope injection; main-only writes |
| K4.V.18 | `m_synchronousModuleQueue` (`VM.h:1358`) | RE-RULED per-lite (§E.1 family) per §0 U3 (AUD1.K3); listed here for history only |

## VI. Requires-stop — class 4 (§K.5)

Writers that iterate/rewrite OTHER threads' reachable state. One §A.3
stop, conductor = caller, §A.3.5 CLASS-4 variant.

| Row | Member (file:line) | Rationale |
|---|---|---|
| K4.VI.1 | `JSGlobalObject::haveABadTime` (`JSGlobalObject.cpp:2900`; JS-reachable :2460; jettison comment :2854; `m_havingABadTimeWatchpointSet` `JSGlobalObject.h:517`) | THE ruled class-4 case: history annex HBT+HBT2-4 verbatim |
| K4.VI.2 | Watchpoint-set FIRING, all of: `JSGlobalObject.h:516-521` const Ref sets, InlineWatchpointSets :531-572 (incl. `m_structureCacheClearedWatchpointSet` :564), adaptive watchpoints :576-589, VM `m_primitiveGigacageEnabled` (`VM.h:1304`), impure-property sets (row III.8) | invalidation jettisons/deoptimizes code other threads may be RUNNING => fire only inside a §A.3 stop (jit-spec deopt machinery); INSTALL/registration is lock-class (III.18); reads (hasBeenInvalidated, e.g. `JSGlobalObject.h:1192-1194`) are already-safe loads |
| K4.VI.3 | `VM::deleteAllCode` / `deleteAllLinkedCode` (`VM.h:998-999` decls) | rewrites every thread's executable state; route through the same §A.3 stop family (jit spec I2/R1) |
| K4.VI.4 | JSGlobalObject `m_structureCache` CLEAR path (`JSGlobalObject.h:500`; `StructureCache.h:60-61, 67-68`) | per-entry add/get under its existing `m_lock` (already-safe); but bulk clear coupled to :564's watchpoint => clear rides the VI.2 stop |

## VII. Process-global singletons

| Row | Member (file:line) | Class | Rationale |
|---|---|---|---|
| K4.VII.1 | Options / JSCConfig (`gilOffProcess` byte, JSCConfig.h:106 per spec §A.1.3) | immutable-after-init | Config::permanentlyFreeze before threads; U0 option-validation gate |
| K4.VII.2 | VMManager singleton (`runtime/VMManager.h:243`; world-stop state `m_worldMode`/`m_currentStopReason`/`m_pendingStopRequestBits` :142-186 commentary) | already-safe | heap rank 3 m_worldLock + §A.3 machinery; ruled by heap spec §6 |
| K4.VII.3 | AtomString table shards + SymbolRegistry (`VM.h:651-653` pointers; WTF shards) | already-safe | §LK.8 destructor-leaf class; requires useSharedAtomStringTable=1 (U0) |
| K4.VII.4 | DeferredWorkTimer (`VM.h:646`; `DeferredWorkTimer.h:116 m_taskLock`, `m_tasks` :120, `m_currentlyRunningTask` :119, `m_pendingTickets` :121 with the :125-126 safety comment) | lock | §LK.7 leaf (spec name DWT::m_pendingLock); m_taskLock IS the spec's m_pendingLock; m_tasks/m_currentlyRunningTask AND m_pendingTickets under it (§0 U5) |
| K4.VII.5 | `m_runLoop` (`VM.h:467`) | immutable-after-init | const Ref bound at ctor; RunLoop itself WTF-thread-safe for dispatch |
| K4.VII.6 | `m_heapRandom` (`VM.h:470`) | already-safe | advanced only by heap-side allocation paths already serialized by heap spec (GC server / stop windows); NOT touched on JS mutator fast paths |
| K4.VII.7 | `m_currentWeakRefVersion` (`VM.h:1330`) | already-safe | written only inside the GC stop window; mutator reads relaxed (epoch compare) |

## VIII. Immutable-after-init (assert-only; debug assert "no write after first cross-thread entry")

| Row | Members (file:line) | Rationale |
|---|---|---|
| K4.VIII.1 | VM structure roots `structureStructure` ... `bigIntStructure` (`VM.h:552-604`) | written once in VM ctor; thereafter GC-barrier reads only |
| K4.VIII.2 | Promise/native executable roots (`VM.h:607-621`) | ctor-initialized |
| K4.VIII.3 | Sentinels: `m_orderedHashTableDeletedValue`/`m_orderedHashTableSentinel` (:623-624), `m_sentinelStructure` + fast-iterator sentinels (:626-635) | ctor-initialized constants |
| K4.VIII.4 | `heapBigIntConstantOne`/`Zero` (`VM.h:676-677`) | ctor constants |
| K4.VIII.5 | `propertyNames` CommonIdentifiers (:654), `m_emptyList` (:655), `m_bytecodeIntrinsicRegistry` (:1317), `m_builtinExecutables` POINTER (:1295; per-slot fills are §K.3 single-word publishes, `BuiltinExecutables.h:76`) | built in ctor; builtin slots: release-CAS per slot |
| K4.VIII.6 | `smallStrings` (`VM.h:656`; `SmallStrings.h:69-127`) | ctor `initializeCommonStrings`; verification PASSED (§0 U7) — unconditional |
| K4.VIII.7 | `m_identifier` (:464), `m_typedArrayController` (:947), `clientData` (:548), `m_globalConstRedeclarationShouldThrow` (:1289) | set at VM/embedder init before any thread spawn; Bun sets clientData pre-threads |
| K4.VIII.8 | Embedder hooks: `m_onEachMicrotaskTick` (:1323), `m_onComputeErrorInfo{,JSValue}` (:1325-1326), `m_onAppendStackTrace` (:1327), `m_computeLineColumnWithSourcemap` (:1328), `m_didPopListeners` (:1380), `m_crossTaskToken` (:463) | Bun installs once at startup; setters (:1091-1094) get the no-write-after-entry assert; if Bun ever needs dynamic swap, re-rule to lock |
| K4.VIII.9 | JSGlobalObject: `m_vm` (:236), `m_globalThis` (:253), `m_globalLexicalEnvironment` POINTER (:255; var contents = OM territory), `m_name` (:503), `m_isAsyncContextTrackingEnabled` (:506), `m_evalEnabled`/`m_webAssemblyEnabled`/`m_needsSiteSpecificQuirks` (:672-674), disabled-error messages (:677-678; setters :1249-1255) | global-object configuration written by embedder before sharing the global across threads |
| K4.VIII.10 | JSGlobalObject `m_weakRandom` (`JSGlobalObject.h:526`) | **per-lite**, not immutable — listed here only to pin the decision: Math.random state advances per call; per-lite streams (independently seeded) are spec-compliant (no SD: outputs remain uniform); JIT inline fast path re-pointed via lite, same A16-style pattern as II rows |

## IX. Coverage statement

Sweep inputs: `runtime/VM.h` (all 1479 lines; every declared data
member is in a row above or in table I), `runtime/JSGlobalObject.h`
(all member blocks :236-678), `runtime/Watchdog.h`,
`debugger/Debugger.h`, `runtime/SamplingProfiler.h`,
`tools/VMInspector.h`, `runtime/DeferredWorkTimer.h`,
`runtime/RegExpCache.h`, `runtime/VMManager.h`, plus reached-into
helpers (`CodeCache.h`, `IntlCache.h`, `StructureCache.h`,
`JITThunks.h`, `SharedJITStubSet.h`, `FTLThunks.h`,
`RegExpGlobalData.h`, `SmallStrings.h`, `NumericStrings.h`,
`LazyProperty.h`, `JSGlobalObjectInlines.h`, `WeakGCMap.h`,
`VariableEnvironment.h`). Cell-INTERNAL state (JSMap/rope/etc.) is §N,
not this annex. Heap/GC-owned members (`VM::heap` and everything it
roots) are heap-spec territory and intentionally absent.

Binding consequences:

1. Every §F.2 EXCLUSIVITY CONSUMER must cite its row here (spec §K.4).
2. The seven §0 items are RESOLVED at spec rev 26 (§K.6 / history
   ANNEX AUD1); the U-T9 audit gate is SATISFIED on this annex's
   side. U-T8b consumes these tables verbatim.
3. Per-lite rows holding GC cells join the registry-walk root set
   (§A.1.3); the ~VM teardown walk (U-T8) must free all per-lite
   copies.
4. Immutable-after-init rows each get the debug
   assert-no-write-after-first-cross-thread-entry hook (one shared
   macro; U-T8b deliverable).
