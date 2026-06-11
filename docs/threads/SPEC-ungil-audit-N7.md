# SPEC-ungil Annex N7 (BINDING, audit executed)

Executes SPEC-ungil §N.7 (task U-T8c, gates U-T9, beside U-T8b).
Scope: every shareable JSCell subclass under
Source/JavaScriptCore/runtime/ carrying NON-PROPERTY multi-word
mutable state (C++ members / internal fields / aux allocations
mutated on JS-reachable paths after publication). Property slots,
butterflies, Structures, PropertyTable = OM spec by definition;
VM/JSGlobalObject members = §K/U-T8b (cross-refs only). Method:
header sweep of runtime/*.h for mutable members on cell classes +
cellLock()-user census (runtime/*.cpp) + cross-check against
SPEC-ungil §N.1-8, §E.1b, §K, §H, §I, OM §4.6/§9.5/I21/annex 15,
jit §5.7.2, annexes N6/CBI/E1B (history). All paths below relative
to Source/JavaScriptCore/.

Disposition vocabulary (§N.7): CELL-LOCK | CAS-PUBLISH |
RACY-TOLERATED | GIL-OFF TypeError | COVERED(section) |
PHASE-1-IN-TREE (already locked in this branch) | UNRESOLVED.

---

## Residue dispositions — ALL RESOLVED at spec rev 26

Former UNRESOLVED items 1-7, each now BINDING in SPEC-ungil §N.9
(§K.6 for item 7) with FULL text in history ANNEX AUD1. The
original analyses are kept below for the implementation record;
the RULING line at the head of each item is the disposition.
No item blocks U-T9.

### RESOLVED-1 (AUD1.N1). AbstractModuleRecord::m_resolutionCache — UNLOCKED HashMap, cross-thread UAF
RULING: CELL-LOCK (§N default) — all access under the record's
JSCellLock, the sibling-map lock; §E.1b alloc-outside shape; no
tier-inlined access. PRIORITY (UAF today). Amplifier owed (U28).
- State: `Resolutions m_resolutionCache` (WTF HashMap),
  runtime/AbstractModuleRecord.h:297-298.
- Mutation: `cacheResolution()` runtime/AbstractModuleRecord.cpp:342-345
  (`m_resolutionCache.add` — rehash frees bucket array); read:
  `tryGetCachedResolution()` :334-340. NEITHER takes any lock,
  while the SIBLING maps on the same cell ARE cell-locked in-tree
  (m_dependencies AbstractModuleRecord.cpp:1465; m_asyncParentModules
  :1561; visitChildren :100, :237).
- Reachability: any thread touching a shared module namespace object —
  JSModuleNamespaceObject::getOwnPropertySlot -> resolveExport ->
  resolveExportImpl -> cacheResolution (AbstractModuleRecord.cpp:667-669,
  :722, :771, :795, :933). Two threads reading `ns.x` race a rehash
  against a bucket walk = exactly the OM annex 15.7 SparseArrayValueMap
  UAF class.
- Proposed ruling: §N default — all access under the record's
  JSCellLock (10a, §E.1b alloc-outside shape), same lock already used
  for the sibling maps. No tier-inlined access exists (namespace loads
  are IC'd on the namespace object, not this cache) so no JIT work.

### RESOLVED-2 (AUD1.N2). RegExp::m_ovector — shared per-match scratch, unlocked on the mutator path
RULING: per-lite match buffer (annex K4 §I regexp row) — scratch
moves OFF the cell GIL-off; ovectorSpan consumers + DFG/FTL exec
thunks take the lite buffer; cell keeps compile state only (R13).
PRIORITY (UAF today). Amplifier owed (U28).
- State: `Vector<int> m_ovector` runtime/RegExp.h:232; resized per
  match at runtime/RegExp.cpp:183; handed out raw via `ovectorSpan()`
  RegExp.h:103.
- Compile state IS phase-1-in-tree cell-locked (RegExp.cpp:227, :251,
  :306, :319 compile*/compileIfNecessary*; deleteCode :384;
  matchConcurrently :373 — compiler-thread side only). But the JS-thread
  match path (RegExpInlines.h matchInline, called from RegExp::match
  RegExp.cpp:365) writes m_ovector with NO lock. Two threads exec()ing
  the SAME shared RegExp cell concurrently: racing resize = realloc UAF +
  torn capture reads.
- Proposed ruling: move per-match scratch off the cell (per-lite scratch
  buffer, §K.1 class; matches THREAD.md "lazy regexp stack" per-thread
  split) — preferred over cell-locking the whole match (would serialize
  hot regexp workloads and §N forbids parking under the lock for
  long-running Yarr JIT execution). Tier-inlined: DFG/FTL RegExpExec
  thunks land in matchInline — they inherit whatever the ruling picks;
  re-point to the per-lite buffer.

### RESOLVED-3 (AUD1.N3). DirectArguments lazy override storage (tier-inlined)
RULING: CAS-PUBLISH as proposed — alloc+fill complete, release-CAS
the pointer, losers discard; readers load-acquire; tier-inlined
null-check stays (address-dependent load, jit F2).
- State: `MappedArguments m_mappedArguments` (CagedBarrierPtr, lazy
  alloc on overrideThings) runtime/DirectArguments.h:183 (offsets baked
  for JIT :153-154); plus GenericArgumentsImpl
  `m_modifiedArgumentsDescriptor` lazy bitmap
  (offsetOfModifiedArgumentsDescriptor DirectArguments.h:154,
  init in GenericArgumentsImplInlines.h).
- Race: foreign read of arguments[i] (DFG GetFromArguments /
  inlined offsetOfMappedArguments null-check) vs owner's first
  `delete arguments.length`-class override: bitmap alloc + flag-flip +
  property materialization is a multi-word publication with no rule.
  OM annex 15.6 audited GenericArgumentsImplInlines only for butterfly()
  callers — explicitly NOT this state.
- Proposed ruling: m_mappedArguments/-Descriptor become
  CAS-PUBLISH (allocate+fill bitmap, release-CAS the pointer; losers
  discard); the materialize-properties half follows OM property rules;
  readers load-acquire. Tier-inlined null-check stays (address-dependent
  load, jit F2 shape).

### RESOLVED-4 (AUD1.N3). ScopedArguments::overrideThings flag + ClonedArguments::materializeSpecials publish order
RULING: as proposed — flag words release-stored AFTER the OM puts;
foreign slow-path readers acquire; no lost properties.
- ScopedArguments: `bool m_overrodeThings` runtime/ScopedArguments.h:170
  (JIT offset :156) flipped after length/callee/caller materialization;
  same family as UNRESOLVED-3 (flag must be release-published AFTER the
  OM puts; foreign tier-inlined readers acquire).
- ClonedArguments: `m_callee` doubles as the not-yet-materialized flag
  (runtime/ClonedArguments.h:100-104, JIT offset :78); materializeSpecials
  does OM puts then clears m_callee — single-word flag but publication
  ORDER is unruled; a foreign reader seeing the cleared flag before the
  puts misses callee/length entirely (lost-property class, violates
  THREAD.md "no lost properties").
- Proposed ruling: both = release-store of the flag word ordered after
  the OM puts; readers acquire on the slow path; tier-inlined fast paths
  re-pointed or fenced per jit item 7 audit.

### RESOLVED-5 (AUD1.N4). StructureRareData runtime caches (tier-inlined flag word)
RULING: installs under Structure::m_lock; each JIT-read word
single-word release-published LAST (watchpoint vector filled
first, immutable after); m_specialPropertyCache = §K.3. OM-annex
cross-pointer recorded in AUD1.
- State: `uintptr_t m_cachedPropertyNameEnumeratorAndFlag`
  runtime/StructureRareData.h:165 (+ FixedVector
  m_cachedPropertyNameEnumeratorWatchpoints :166, installed together);
  `WriteBarrier<JSCellButterfly> m_cachedPropertyNames[...]` :167
  (JIT offsets :110, :115); lazy
  `std::unique_ptr<SpecialPropertyCache> m_specialPropertyCache` :175
  (ensureSpecialPropertyCacheSlow :157, cacheSpecialPropertySlow :154).
- Race: for-in / Object.keys / toString caching mutates these on ANY
  thread iterating a shared structure; baseline/DFG read the
  enumerator+flag word and cachedPropertyNames directly. Multi-word
  install {enumerator, watchpoint vector, flag} has no rule; not covered
  by OM (not property storage), not §K (cell, not VM/global member),
  not §N.1-8. Structure::m_lock is not documented to guard rare-data
  cache installs on read paths.
- Proposed ruling: installs under Structure::m_lock (the structure
  already owns its rare data lifecycle, OM GT order); the
  JIT-read words each single-word release-published; watchpoint vector
  immutable post-publication (publish pointer last). m_specialPropertyCache
  = §K.3-class lazy publication. Needs an OM-annex cross-amendment
  because watchpoint-fire sites are jit-spec territory.

### RESOLVED-6 (AUD1.N5). Intl cell family — lazy mutable members + ICU reentrancy unproven
RULING: post-construction-mutable members = CELL-LOCK (lazy
Strings computed outside, published under it); construction-frozen
ICU handles used concurrently ONLY via call sites verified
const/thread-safe (AUD1 checklist), else clone-per-use under the
lock. NO TypeError, NO SD.
- IntlNumberFormat: `mutable String m_numberingSystem`
  runtime/IntlNumberFormat.h:232 (lazy compute on read; String = two
  words refcounted — torn publish + non-atomic ref).
- IntlLocale / IntlSegmenter / IntlSegmentIterator and kin: lazily
  computed String/UObject members of the same shape; IntlSegmentIterator
  advances a UBreakIterator (inherently mutating) per next().
- Cross-cutting: even immutable-after-init ICU handles (UCollator at
  IntlCollator, UNumberFormatter at IntlNumberFormat.h:225, initialized
  in initialize* during construction) are only safe for CONCURRENT use
  via const ICU APIs — unverified per call site.
- Proposed ruling: per-class audit row; default = cell-lock around any
  member that mutates post-construction (segment iterators, lazy
  strings); ICU const-use proof or per-thread clone for format/compare
  hot paths. No tier-inlined accesses (all host calls). Until ruled:
  candidates for GIL-OFF TypeError on foreign-thread use (SD entry
  required if taken).

### RESOLVED-7 (AUD1.K2, SD19; cross-ref §K.4/U-T8b scope). RegExpGlobalData / RegExpCachedResult — tier-inlined multi-word global cache
RULING: per-lite (§K.1) with per-thread RegExp.$1-$9 semantics =
SD19; DFG/FTL RecordRegExpCachedResult re-pointed via the lite
(AUD1.K4 A16 ext). Annex K4 §0 U2 row owns it.
- State: per-JSGlobalObject `RegExpCachedResult m_cachedResult`
  runtime/RegExpGlobalData.h:64 — multi-word {m_result(2 words),
  m_lastInput, m_lastRegExp} updated on EVERY global-flag match, plus
  lazy reification flip {m_reified + 4 reified barriers}
  (runtime/RegExpCachedResult.h:75-82).
- Listed HERE because DFG/FTL write m_result/m_lastInput/m_lastRegExp
  inline (offsetOfResult/offsetOfLastInput RegExpCachedResult.h:66-70 —
  RecordRegExpCachedResult): §N.7's "tier-inlined accesses disabled or
  re-pointed" clause applies even though the carrier is a global member.
  U-T8b must rule it (per-lite copy = §K.1, matching RegExp.$1 semantics
  per thread = SD entry; or locked = kills inlining). Ruled per-lite
  + SD19 at rev 26 (annex K4 §0 U2; AUD1.K2).

---

## Resolved inventory (IU table)

Dispositions cite the governing frozen text. "PHASE-1-IN-TREE" =
the serialization already exists in this branch's source (census:
cellLock() users in runtime/), satisfying §N default shape; GIL-off
keeps it as-is.

| # | Cell class (file:line) | Non-property mutable state | Disposition |
|---|---|---|---|
| R1 | JSMap/JSSet (runtime/JSMap.h:32, JSSet.h:32, JSOrderedHashTable storage) | hash table buffer, load factors | COVERED §N.1 — ALL ops (reads too) cell-locked; DFG/FTL map intrinsics DISABLED GIL-off (tier-inlined accesses disabled), locked native bodies |
| R2 | JSWeakMap/JSWeakSet (runtime/WeakMapImpl.h:209) | m_buffer, m_keyCount, m_deleteCount | COVERED §N.1 (WeakMapImpl named) |
| R3 | JSMapIterator/JSSetIterator (runtime/JSMapIterator.h:36, JSSetIterator.h:36) | internal fields (entry cursor) + table traversal | COVERED §N.5 (internal-field claim/publish) + §N.1 (storage reads under cell lock); transparent-to-GC bucket hopping inherits N.1 |
| R4 | JSString rope/atomization (runtime/JSString.h:637-682) | fiber0/flags publication | COVERED §N.2 — lock-free release-CAS publish, losers discard; resolveRopeToAtomString vs shared table per U0; JIT rope slow calls land here |
| R5 | DateInstance (runtime/DateInstance.h:62-75) | GregorianDateTime cache m_data | COVERED §N.3 — cache BYPASSED GIL-off; m_data lazy alloc CAS-published; vm.dateCache per §K.1/2 |
| R6 | JSFunction/FunctionRareData (runtime/JSFunction.h:136-144; FunctionRareData.h:44, profiles :72-99) | rare-data materialize; allocation profiles; cached structures | COVERED §N.4 — materialize per §K.3; internals under function's cell lock; profiling fields RACY-TOLERATED (jit item 7); cached Structures per I34 |
| R7 | JSGenerator (runtime/JSGenerator.h:33), JSAsyncGenerator (JSAsyncGenerator.h:36), async function frames | resume state internal fields | COVERED §N.5 — single-word resume-claim CAS SuspendedX->Running; @atomicInternalFieldClaim/Publish twin intrinsics, mode-keyed lowering; interior stores plain while claimed |
| R8 | JSArrayIterator (.h:32), JSStringIterator (:33), JSIteratorHelper (:32), JSRegExpStringIterator (:34), JSWrapForValidIterator (:34), JSAsyncFromSyncIterator (:34), InternalFieldTuple (Bun ALS) | internal fields | COVERED §N.5 (iterator helpers named; InternalFieldTuple per §E ALS1.3 + history r25 ext) |
| R9 | JSPromise + reactions | flags/reactions internal fields | COVERED §E.1b/annex E1B + §E.7 (settle CAS; out of §N by charter, listed for closure; U-T9 settle-site IU table owns call sites) |
| R10 | ArrayBuffer (runtime/ArrayBuffer.h:199, :298) | detach/transfer/resize/grow {base,length} pairs | COVERED §N.6 + annex N6 torn-pair table — detach length=0 seq_cst + quarantine to heap §10 stop; grow base-immutable commit-then-release-length; wasm grow ditto (wasm cells otherwise §I REFUSED v1) |
| R11 | JSArrayBufferView (runtime/JSArrayBufferView.cpp:265, :327 cell-locked wasteful/oversize paths) | m_vector/m_length/m_mode | COVERED §N.6/annex N6 (hoisted vectors jettison) + PHASE-1-IN-TREE for mode transitions |
| R12 | ScriptExecutable/FunctionExecutable/EvalExecutable/ProgramExecutable/ModuleProgramExecutable | first CodeBlock install; m_jitCodeFor*; unlinked generation | COVERED §N.8/annex CBI — compile outside locks, release-CAS m_codeBlockFor{Call,Construct}, loser discards; adjacent fields per-field ruled; UnlinkedCodeBlock = §K.3-class. visitChildren already cell-locked in tree (FunctionExecutable.cpp:91, EvalExecutable.cpp:61, ScriptExecutable.cpp:444, ProgramExecutable.cpp, ModuleProgramExecutable.cpp) |
| R13 | RegExp compile state (runtime/RegExp.h:222-231: m_state, m_regExpBytecode, m_regExpJITCode, m_rareData) | lazy compile/deleteCode | PHASE-1-IN-TREE — cell-locked at RegExp.cpp:227, :251, :306, :319, :373, :384; conforms to §N default. (m_ovector -> RESOLVED-2: per-lite buffer) |
| R14 | RegExpObject (runtime/RegExpObject.h:165 m_lastIndex) | lastIndex word + writability bit | RACY-TOLERATED — single WriteBarrier word, SAB-grade staleness; property-equivalent semantics (spec'd as a property); tier-inlined offsetOfLastIndex stays |
| R15 | ErrorInstance (runtime/ErrorInstance.h:170-171 m_stackTrace, m_errorInfoMaterialized; m_sourceAppender :170) | lazy stack/errorInfo materialization | PHASE-1-IN-TREE — cell-locked at ErrorInstance.cpp:117, :128, :141, :177, :209, :229, :393, :418, :451; m_sourceAppender single-word. Conforms to §N default |
| R16 | AbstractModuleRecord maps EXCEPT resolution cache (runtime/AbstractModuleRecord.h:48; m_dependencies, m_asyncParentModules) + module loader pipeline (ModuleGraphLoadingState.cpp:64, :79; JSModuleLoader.cpp:269, :758, :945, :1072, :1087) | link/evaluate bookkeeping | PHASE-1-IN-TREE — cell-locked (AbstractModuleRecord.cpp:100, :237, :1465, :1561). Resolution cache -> RESOLVED-1 (cell lock) |
| R17 | JSModuleNamespaceObject (runtime/JSModuleNamespaceObject.h:95 m_exports) | export map | IMMUTABLE post-finishCreation — no entry needed; its getOwnPropertySlot path inherits RESOLVED-1 |
| R18 | JSFinalizationRegistry (runtime/JSFinalizationRegistry.h:116-117; lock-taking API :88-96) | live/dead registration maps | PHASE-1-IN-TREE — all access via Locker<JSCellLock> parameters (JSFinalizationRegistry.cpp); GC-side sweep under heap spec stops |
| R19 | JSWeakObjectRef (runtime/JSWeakObjectRef.h:49-55, :75) | m_lastAccessVersion + m_value | RACY-TOLERATED with one amendment: m_lastAccessVersion load/store made relaxed-atomic (single word); m_value single WriteBarrier; deref'd cell kept live by conservative scan (heap I7) regardless of version race |
| R20 | ProxyObject (runtime/ProxyObject.h:138-142) | target/handler internal fields; m_handlerStructureID/-PrototypeStructureID caches; m_isCallable bits | RULED HERE: internal fields single-word (revoke = seq_cst null store, foreign readers re-validate — TypeError path already exists); structure-ID caches each single-word, independently re-validated => RACY-TOLERATED; m_isCallable/m_isConstructible immutable post-construction |
| R21 | JSBoundFunction (runtime/JSBoundFunction.h:94-99) | m_boundArgs (immutable); m_nameMayBeNull lazy; m_length NaN-sentinel double | RULED HERE: m_boundArgs/boundThis immutable post-construction; m_nameMayBeNull = idempotent single-word release-publish (CAS-PUBLISH, losers' value identical); m_length = idempotent 8-byte store, RACY-TOLERATED |
| R22 | GetterSetter | m_getter/m_setter words | RACY-TOLERATED — two independent single words; pair-tearing = SAB-grade staleness, each word always a valid callee; OM accessor-slot rules own the slot itself |
| R23 | JSPropertyNameEnumerator (runtime/JSPropertyNameEnumerator.h:115-116) | names buffer + cached StructureID | IMMUTABLE post-creation (computeNext mutates caller-owned cursor only) — no entry |
| R24 | SparseArrayValueMap (runtime/SparseArrayValueMap.cpp cell-locked) | hash map innards | COVERED OM §4.6 + annex 15.7 — AS family fully cell-locked both sides, AS-COPY; jit never fast-paths sparse |
| R25 | SymbolTable (runtime/SymbolTable.h:799 ConcurrentJSLock m_lock), JSSegmentedVariableObject (own m_lock, JSSegmentedVariableObject.cpp) | symbol map / variable spine | PHASE-1-IN-TREE (pre-existing concurrent locks; jit spec already consumes them) |
| R26 | Structure/StructureRareData TRANSITION state, PropertyTable, butterflies, indexing storage | — | OUT OF §N SCOPE — OM spec (§2-§10, I-series). StructureRareData runtime CACHES -> RESOLVED-5 |
| R27 | JSGlobalObject lazy properties, VM caches (numericStrings, dateCache, RegExpCache — already locked RegExpCache.h:79) | — | OUT OF §N SCOPE — §K.1-5 + U-T8b inventory. RegExpGlobalData cross-ref -> RESOLVED-7 (per-lite, SD19) |
| R28 | Wasm cells (JSWebAssembly*, WebAssemblyModuleRecord) | all | COVERED §I — REFUSED in v1 (GIL-OFF TypeError on spawned threads) |
| R29 | Symbol, BigInt, StringObject/NumberObject/BooleanObject internals, Temporal* ISO fields, ShadowRealmObject, JSGlobalProxy/JSProxy target word, Exception, JSNativeStdFunction, JSCustomGetterFunction/JSRemoteFunction targets, JSSourceCode, JSTemplateObjectDescriptor descriptor ref, JSScriptFetcher/JSScriptFetchParameters | construction-time-only or single-word state | IMMUTABLE post-publication / single-word — no entry. (JSRemoteFunction lazy name word: same idempotent shape as R21.) |
| R30 | API cells (API/JSCallbackObject* private properties, JSAPIWrapperObject) | callback data maps | OUT OF runtime/ SWEEP — owned by SPEC-api §F/U-T8 (api lock ranks); named here so U-T8c closure is explicit |
| R31 | DebuggerScope + inspector-reachable cells | scope cursor | COVERED §A.2.7 — debugger walks only inside §A.3 stops |

## Gate disposition

- U-T8c result: 31 ruled/covered rows + 7 residue items, ALL
  RESOLVED at spec rev 26 (§N.9/§K.6; history ANNEX AUD1). The U-T9
  audit gate is SATISFIED on this annex's side; implementation
  CONSUMES this table verbatim.
- Severity note: RESOLVED-1 and -2 are memory-unsafe TODAY under any
  GIL-off interleaving (HashMap-rehash UAF; scratch-vector realloc
  UAF) — same defect class OM annex 15.7 fixed for ArrayStorage.
  Implement first.
- Amplifier arms owed (U28): two-thread shared-namespace property
  storm (RESOLVED-1); two-thread exec() on one shared RegExp
  (RESOLVED-2); foreign-reader vs owner override on Direct/Scoped/
  ClonedArguments (RESOLVED-3/4); two-thread for-in over one shared
  structure (RESOLVED-5); regexp legacy-statics SD19 variants
  (RESOLVED-7). TSAN + arm64 per §N.5 precedent.
- Census note: cellLock() users found in runtime/ (this branch):
  AbstractModuleRecord, ErrorInstance, Eval/Function/Program/
  ModuleProgram/ScriptExecutable, JSArray(+Inlines), JSArrayBufferView,
  JSFinalizationRegistry, JSGlobalObject, JSModuleLoader,
  ModuleGraphLoadingState, JSObject(+Inlines), JSSegmentedVariableObject,
  RegExp, SparseArrayValueMap, Structure, ConcurrentButterfly,
  JSGenericTypedArrayViewInlines — i.e. phase-1 already landed the §N
  default shape for most multi-word cases; this annex's residue list
  is exactly what the census exposed as unlocked or unruled (all now
  resolved, §0 above).
