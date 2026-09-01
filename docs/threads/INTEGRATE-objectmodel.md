# INTEGRATE-objectmodel — shared-file changes requested by the objectmodel workstream

Per SPEC-objectmodel.md §10 (entry text frozen in annex §M). Entries are append-only;
each is tagged with the task that generated it. Code call sites that depend on these
are marked `// THREADS-INTEGRATE(objectmodel)`.

## From Task 1 (ConcurrentButterfly.h skeleton)

### 1. runtime/OptionsList.h — three remaining §9.6 options (manifest entry 1)

`useJSThreads` (and the api-spec entries) are already landed at OptionsList.h:681-687.
The three objectmodel stress/verify options are NOT yet present. Insert the following
three lines immediately after the existing line
`v(Bool, useJSThreads, false, Normal, "enable shared-memory Thread/Lock/Condition/ThreadLocal API"_s) \`
(keeping the alphabetical-ish JSThreads cluster together):

```cpp
    v(Bool, forceSegmentedButterflies, false, Normal, "stress: every butterfly allocation/transition publishes a segmented butterfly (SPEC-objectmodel §9.6)"_s) \
    v(Bool, forceButterflySWBit, false, Normal, "stress: treat every butterfly write as a foreign shared write (SW DCAS + writeThreadLocal fire; SPEC-objectmodel §9.6)"_s) \
    v(Bool, verifyConcurrentButterfly, false, Normal, "debug-assert every concurrent-butterfly tag decode (I2/I3), the butterfly() flatness contract, and run the ConcurrentButterfly self-tests"_s) \
```

No action is needed in ConcurrentButterfly.h when these land: it probes
`Options::{verifyConcurrentButterfly,forceSegmentedButterflies,forceButterflySWBit}()`
by SFINAE (`JSC::ConcurrentButterflyInternal::OptionProbe_*`) and returns constant
`false` while they are absent, picking the real options up automatically once the
lines above are applied. Lint per manifest entry 1: no occurrence of
`useConcurrentJS` anywhere (the flag was renamed `useJSThreads`).

### 2. Sources.txt (manifest entry 2) — PENDING Task 5

`runtime/ConcurrentButterfly.cpp` does not exist yet (created by Task 5). When it
lands, add to Source/JavaScriptCore/Sources.txt, in the `runtime/` section in
alphabetical order (between `runtime/CommonSlowPathsInlines.cpp`-area entries and
`runtime/ConsoleClient.cpp`-area entries):

```
runtime/ConcurrentButterfly.cpp
```

### 3. CMakeLists.txt (manifest entry 3)

a. Install the new private header: add `runtime/ConcurrentButterfly.h` to the
   JavaScriptCore `PRIVATE_FRAMEWORK_HEADERS` / forwarding-headers list (same list
   that carries `runtime/Butterfly.h`), alphabetical order.

b. x86-64: add `-mcx16` to JavaScriptCore (and any target that may inline
   `JSC::dcasHeaderAndButterfly`) C/C++ flags. Without it the GCC/Clang
   `__sync_bool_compare_and_swap` on `unsigned __int128` used by
   `dcasHeaderAndButterfly` is a link error (`__sync_bool_compare_and_swap_16`)
   — which is intentional per I32 (inline `lock cmpxchg16b` only; a lock-based
   libatomic fallback is forbidden and `-latomic` must NOT be added to fix such
   an error).

c. arm64: LSE (`-march=...+lse`) where the baseline allows, else GCC/Clang
   outlined atomics are acceptable per manifest entry 3 ("arm64 LSE or outlined
   atomics") — the 16-byte `__sync` builtin lowers to `casp` or an ldxp/stxp
   loop either way; both are lock-free.

d. Assert backstop (I32): see VM.cpp entry 4a below; on Clang it evaluates
   `__atomic_is_lock_free(16, sampleCell)`; on GCC the helper returns `true`
   structurally (the GCC `__atomic_is_lock_free` is itself a libatomic libcall,
   which must not be linked; lock-freedom on the GCC path is guaranteed by the
   inline-or-link-error property of the `__sync` builtin plus `-mcx16`).

### 4. runtime/VM.cpp (manifest entry 4a) — §9.2 startup assert + self-test

In `VM::VM(...)` (or `VM::create` common path), after options are finalized,
behind the master flag:

```cpp
#include "ConcurrentButterfly.h" // add to VM.cpp includes

    // SPEC-objectmodel §9.2/I32 + Task 1 self-test (manifest entry 4a).
    if (Options::useJSThreads()) [[unlikely]] {
        alignas(16) static uint64_t sampleCell[2];
        RELEASE_ASSERT(concurrentButterflyAtomicsAreLockFree(&sampleCell));
        concurrentButterflySelfTestIfNeeded(); // runs iff Options::verifyConcurrentButterfly()
    }
```

(Manifest entries 4b — force `heap.m_mutatorShouldBeFenced = true` flag-on (M8) —
and 4c — per-server-heap quarantine-epoch adapter registration — belong to later
tasks (Tasks 6b/9) and will be appended here when their owned code exists.)

### 5. runtime/VMLite.h shim (SPEC-objectmodel §9.1) — auto-swap, record only

`currentButterflyTID()` is shimmed inside ConcurrentButterfly.h
(`ALWAYS_INLINE uint16_t currentButterflyTID() { return 0; }`) under
`#if !__has_include("VMLite.h")`, per §9.1's sanctioned interim pattern. When the
vmstate workstream lands `runtime/VMLite.h` (vmstate §6.7, sole provider,
JS_EXPORT_PRIVATE), the `__has_include` flips and the shim compiles away — no
source change required in this workstream. Integrator check: after VMLite.h
lands, verify a clean build contains no definition of the shim (grep
`interim shim per SPEC-objectmodel` is the marker) being selected, and that all
TUs agree (VMLite.h must be reachable on every include path that reaches
ConcurrentButterfly.h, else ODR skew).

### 6. JSGlobalObject.* (manifest entry 5)

None (frozen: PA-cell global object uses the I36 locked path; no entry needed).

## From Task 2 (JSObject tagged accessors)

No NEW shared-file edits are requested by Task 2. Items below are records and
intra-workstream obligations the integrator should verify.

### 7. M5 placement deviation: nuke masking lives in JSCell.h, not JSCellInlines.h

SPEC-objectmodel M5 says the structure()-only nuke masking is implemented in
"owned JSCellInlines.h", but `JSCell::structure()` is defined inline in
JSCell.h:159 (not JSCellInlines.h); moving the definition into JSCellInlines.h
would break linking for the many TUs that ODR-use `structure()` with only
JSCell.h included. Resolution applied: the masking is implemented at the
existing definition site in JSCell.h (`m_structureID.decontaminate().decode()`,
fully commented), which is in this workstream's owned `JSCell*` path set.
Note: `StructureID::decode()` already decontaminates internally, so this is a
zero-behavior-delta contract-hardening; `structureID()` stays a RAW load as M5
requires. JSCellInlines.h carries the audit annotation.

### 8. §9.3 spine/fragment functions: declared (Task 2), defined by Tasks 4/5

ConcurrentButterfly.h now declares the frozen §9.3 accessors plus four Task-2
bounds-checked variants (`segmentedOutOfLineSlotIfWithinBounds`,
`segmentedIndexedSlotIfReadable`, `segmentedIndexedSlotIfWithinVectorLength`,
`segmentedVectorLength`). Their DEFINITIONS land with Task 4 (ButterflySpine /
ButterflyFragment layout in Butterfly.h/ButterflyInlines.h) and Task 5
(ConcurrentButterfly.cpp). Until those land, a full link of a tree containing
only Tasks 1-2 succeeds ONLY if the linker drops the unreferenced flag-on
branches or Tasks 4/5 are present — in the orchestrated build (all tasks landed
before the single build-fix loop) this is moot, but do not attempt to build/test
the flag-on paths from a Task-2-only snapshot.

### 9. Manifest entries 7/7b status after Task 2

- The getDirect/getDirectOffset/putDirectOffset/putDirectWithoutBarrier family
  is now regime-safe for ALL callers via the internal dispatch in
  `JSObject::locationForOffset` (§Q): NO per-call-site guards are needed for it
  (entry 7's guard list applies to textual `(->|.)butterfly()` callers only).
- The quickly-family + `length()`/`getArrayLength()` dispatch is internal to
  JSObject.h flag-on (history §15.6), so those call sites need no guards either.
- Entry 7b (I34 unowned-window audit: polls between offset-producing structure
  lookup and the deref) remains integrator-applied and is NOT discharged by
  Task 2; the owned-path I34 audit is Task 9.

### 10. Intra-workstream handoffs recorded by Task 2 (for Tasks 3-9 implementers)

- visitButterflyImpl (JSObject.cpp): both visit paths now mask the tag; the
  segmented legs are placeholders (`RELEASE_ASSERT(!isSegmentedButterfly)` in
  the stopped path, didRace in the concurrent path) marked for Task 6b's
  `visitSegmentedButterfly`.
- `setButterflyConcurrent` RELEASE_ASSERTs that the N3 install CAS never loses;
  Tasks 5-8 must route genuinely racy installs/resizes through `casButterfly`
  (§9.3) with caller re-dispatch, and Task 8 converts the two
  `storeTaggedButterflyWordConcurrent` resize sites in JSObject.cpp
  (ensureLengthSlow, reallocateAndShrinkButterfly) to the §4.4 T1/T2 CAS form
  plus the T5 cell-locked in-place branch.
- `setIndexQuicklyConcurrent` RELEASE_ASSERTs on segmented shape-conversion
  stores; Tasks 6-8 route those through putIndexConcurrent/§4.3/§4.7.
- F1 (foreign first write => fire writeThreadLocal + SW DCAS) is NOT yet
  enforced on the write fast paths; Task 7 (`ensureSharedWriteBit`) wires it.
  Until then the GIL stub guarantees a single mutator.

## From Task 3 (Structure TTL watchpoint sets, F3 wiring, E1-E4/I29)

No NEW shared-file edits are requested by Task 3. Items below are records and
intra-workstream obligations.

### 11. §10.6 veneer/witness: declarations landed, definitions owed by Task 5

ConcurrentButterfly.h now DECLARES (manifest entry 6):

```cpp
extern JS_EXPORT_PRIVATE bool g_jsThreadsStubWorldStopped;
JS_EXPORT_PRIVATE void jsThreadsStopTheWorldAndRun(VM&, const ScopedLambda<void()>&);
JS_EXPORT_PRIVATE bool butterflyWorldIsStopped(VM&);
```

Their DEFINITIONS are owned by runtime/ConcurrentButterfly.cpp (Task 5, "§10.6
stub+witness flag"). Until Task 5 lands, Structure.cpp's §9.4 fire functions and
the F3/flatten-under-stop sites reference these symbols, so a link of a tree
containing Tasks 1-3 only will fail on them — moot in the orchestrated build
(all tasks land before the single build-fix loop). Task 5 requirements recorded
here:

- `butterflyWorldIsStopped(VM& vm)` = `g_jsThreadsStubWorldStopped ||
  JSThreadsSafepoint::worldIsStopped(vm)` (jit Task 1 has ALREADY landed
  bytecode/JSThreadsSafepoint.{h,cpp}, so the second disjunct is available now).
- `jsThreadsStopTheWorldAndRun` should preferably DELEGATE to
  `JSThreadsSafepoint::stopTheWorldAndRun` (jit CS6's preferred option, see the
  comment block in bytecode/JSThreadsSafepoint.cpp) while still raising the
  witness, OR define `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS` next to the
  witness definition so jit's worldIsStopped() disjunct 4 picks it up.

### 12. F4 chain-fire interpretation (recorded for review)

Spec §5 F4 says F1/F2 stops "fire still-valid TTL sets along the fired
structure's previousID chain + transition-table successors". Implementation in
`Structure::fireThreadLocalSetsWithChainUnderStop` propagates the SAME set
kind(s) that fired: `fireWriteThreadLocal` (F1) chains only writeThreadLocal
(so a first foreign WRITE does not destroy E1/E4 transition elision for the
whole shape family); `fireTransitionThreadLocal` (F2/F3) chains both (it
implies writeThreadLocal per §5). Monotone either way, so sound under the
stricter reading too; flagged here in case the integrator wants the literal
"both kinds always" behavior (one-line change: pass alsoFireTransitionThreadLocal
= true from fireWriteThreadLocal).

The fire functions are also reached from F3 (flatten/pin) sites, so F3 fires
chain as well — over-invalidation only, monotone, rare paths.

`StructureTransitionTable::forEachTransition` (new, StructureTransitionTable.h
declaration + StructureInlines.h definition) supports the successor walk; the
multi-slot leg requires GC to be deferred (WeakGCMap::forEach contract) — the
fire path holds DeferGC across the walk.

### 13. TTL member initialization policy

`m_transitionThreadLocalWatchpointSet` / `m_writeThreadLocalWatchpointSet` start
`IsWatched` flag-on and `ClearWatchpoint` (inert, never consulted or fired)
flag-off, in all three Structure constructors. `m_transitionThreadLocalTID` =
`currentButterflyTID()` for fresh structures, copied from `previous` in the
transition constructor (N1 "creator's TID, copied to targets"). The TID sits in
former padding between m_maxOffset and m_propertyHash — sizeof(Structure) grows
only by the two InlineWatchpointSets (16 bytes, appended after
m_transitionWatchpointSet; no existing member offset changes).

### 14. F3 wiring sites (all six; GT#8 non-sites untouched)

`fireTTLWatchpointSetsAfterPinning(vm, source)` is called on the RESULT right
after pin()/pinForCaching() at: changePrototypeTransition,
changeGlobalProxyTargetTransition, toDictionaryTransition,
nonPropertyTransitionSlow (both the pinForCaching seal/freeze branch and the
isDictionary pin branch), and setBrandTransition — always AFTER the pin
statement's m_lock Locker temporary is destroyed (O2: never STW holding a
§6-ranked lock). The poly-proto create/materialize/removeTransition helpers
(Structure.cpp:415-480, :598-670 per GT#8) are intentionally NOT wired. New
future callers of pin()/pinForCaching() on transition results must add the same
call; a reminder comment sits inside Structure::pin.

### 15. flatten-under-stop notes (for Tasks 5/6/9 implementers)

- `flattenDictionaryStructure` flag-on dispatches to
  `flattenDictionaryStructureUnderStop` when either TTL set is invalid or the
  object's word is SW/segmented; scratch Vector pre-allocated outside the stop
  (O4) with a refit => RESTART loop; fires both sets (F3) inside the same stop.
- Flag-on, the impl takes the cell lock UNCONDITIONALLY (L3/L4: all
  dictionary-mode storage access is cell-locked) — Task 9's dictionary-mode
  reader/writer locking should align with this.
- The segmented leg of the impl clears now-unused out-of-line fragment slots via
  `segmentedOutOfLineSlot` (defined by Tasks 4/5) and SKIPS the
  butterfly-shrink/shiftButterflyAfterFlattening block (spines are immutable,
  I6/I7; retained capacity accepted). GC value-visits remain bounded by
  outOfLineSize (§4.5 step 4).

### 16. E4/I29 consumers

`Structure::mayTransitionLockFreeFromThisStructure(const JSCell*, uint64_t
taggedButterflyWord)` and `Structure::revalidateLockFreeTransition(...)`
(StructureInlines.h) are the E4/I29 predicates for Tasks 6-8's runtime
transition sites and the basis for SPEC-jit §5.5's emitted predicate (which
mirrors them: sets valid+watched + !isPreciseAllocation + tag/TID compare).
E1/E2 are `transitionThreadLocalIsValidAndWatched()` /
`writeThreadLocalIsValidAndWatched()`; E5 slow paths never consult them.

## From Task 3b (SAL emission — SPEC-vmstate §5.3/N5, spec §6/I20)

### 17. Emission inventory (all in [SAL] files; no shared-file edits needed)

Locker spelled exactly `SharedVMState::StructureAllocationLocker
structureAllocationLocker { vm };` (name differs from the spec's illustrative
`locker` only to avoid shadowing the pre-existing `GCSafeConcurrentJSLocker
locker` at the table-insertion sites).

ID-creating Structure::create:
- `StructureCreateInlines.h` `Structure::create(VM&, JSGlobalObject*, ...)`
  and `Structure::createStructure(VM&)`: SAL taken INTERNALLY, around
  allocateCell + ctor + finishCreation; `locker.deferralContext()` IS threaded
  into `allocateCell<Structure>(vm, ...)` (the only sites where the allocation
  call accepts it today). In the 7-arg create the locker is acquired only
  after `didBecomePrototype()` — that path can recursively create a Structure
  and the SAL is non-recursive.
- `Structure.cpp` call-site brackets around the previous-structure
  `Structure::create(vm, structure, deferred)` in: addNewPropertyTransition,
  removeNewPropertyTransition, changePrototypeTransition,
  changeGlobalProxyTargetTransition, attributeChangeTransition,
  toDictionaryTransition, nonPropertyTransitionSlow, and around
  `BrandedStructure::create` in setBrandTransition.
- `Structure::create(PolyProtoTag, ...)` deliberately takes NO locker
  (delegates to the internally-locked create; nesting self-deadlocks).

Allocating transition-table insertions (`m_transitionTable.add`): all six call
sites (addNewPropertyTransition, removeNewPropertyTransition,
changePrototypeTransition, attributeChangeTransition,
nonPropertyTransitionSlow else-branch, setBrandTransition else-branch) take
the SAL OUTSIDE the m_lock locker per the frozen order SAL < JSCellLock <
Structure::m_lock (spec §6/I20). `StructureTransitionTable::add` itself
ASSERTs `structureAllocationRegionDepth() == 1` when
`Options::useStructureAllocationLock()` is on (it must never re-acquire — the
lock is non-recursive).

### 18. Two flag-gated companions at the Structure.cpp sites (rationale)

Both are `Options::useStructureAllocationLock()`-gated, so flag-off behavior
is bit-identical to today (spec I22):

- `std::optional<DeferGC> salDeferGC` (sites lacking a function-scope DeferGC:
  addNew/removeNew/attributeChange/setBrand). The previous-structure
  `Structure::create` body lives in StructureInlines.h (NOT a [SAL] emission
  file), so its `allocateCell` calls cannot receive `locker.deferralContext()`;
  the pre-lock DeferGC (spec O1's sanctioned form) guarantees no collection or
  STW park starts inside the SAL region (vmstate S1-S3, heap L5). It also
  keeps the `GCSafeConcurrentJSLocker`'s embedded DeferGC destructor (which
  runs before the SAL releases) from collecting under the SAL.
- `std::optional<DeferredStructureTransitionWatchpointFire> salDeferredFire`
  (sites where `deferred` may be null). `Structure::create` ->
  `finishCreation(vm, previous, deferred)` ->
  `fireStructureTransitionWatchpoint`: with null `deferred` this fireAlls the
  previous structure's transition watchpoints INLINE — watchpoint firing can
  take rank-6b CodeBlock/jit locks, which are OUTER to the SAL (spec §6) and
  must never be acquired holding it. Flag-on, a local deferred fire is armed
  so firing happens at function exit, after the SAL released (same semantics
  callers already get whenever they pass their own deferred).

### 19. vmstate M7 audit items (verification checklist — NOT new emission by us)

- `BrandedStructure.cpp:53` and `StructureInlines.h:56/61` allocate
  Structure cells WITHOUT the locker's GCDeferralContext (unowned /
  non-[SAL] files). They are SAL-covered by the Structure.cpp call-site
  brackets and GC-quiesced by salDeferGC/function-scope DeferGC; M7 may later
  thread a `GCDeferralContext*` parameter through
  `Structure::create(VM&, Structure*, Deferred*)` / `BrandedStructure::create`
  if S1 is wanted via deferral-context rather than DeferGC.
- `wasm/js/WebAssemblyGCStructure.cpp:65` allocates a Structure cell with NO
  SAL anywhere on its path (file unowned by this part). M7 must add a locker
  there ("adds lockers ONLY where absent — NEVER nested").
- `Options::useStructureAllocationLock()` OptionsList entry is the one already
  recorded by INTEGRATE-vmstate.md (line ~265); VMLiteShared.cpp and our
  Structure.cpp/StructureTransitionTable.h assertions both depend on it; no
  additional shared-file entry needed from this part.
- `StructureCreateInlines.h` now includes `<JavaScriptCore/VMLiteShared.h>`
  (matching that header's framework-style includes): the forwarding header
  for VMLiteShared.h must exist (it does once VMLiteShared.h is in the
  vmstate Sources/headers manifest; flagged here only as a build check).

## From Task 3c (L6 routing / I37 — Concurrently lookups, m_lock steal/walk/mutation conversions)

### 20. What landed (all in owned runtime/Structure* files; no shared-file edits needed)

All conversions are gated on `Options::useJSThreads()`; flag-off every path is
today's code, bit-identical (spec I22).

L6(i) — mutator transition-table LOOKUPS hold the source's m_lock:
- `Structure::addPropertyTransitionToExistingStructure` (StructureInlines.h)
  routes to `addPropertyTransitionToExistingStructureConcurrently`. Routing in
  the shared inline body covers every mutator caller, including the unowned
  `runtime/LiteralParser.cpp:1451` and the owned-but-other-task
  `JSObject.cpp:4647` / `JSObjectInlines.h:587` — no edits needed there.
- `removePropertyTransitionFromExistingStructure`,
  `attributeChangeTransitionToExistingStructure`, and `setBrandTransition`'s
  existing-structure probe (Structure.cpp) route to their Concurrently
  variants.
- The two direct `m_transitionTable.get` sites without a Concurrently variant
  (`changePrototypeTransition`, `nonPropertyTransitionSlow`) take a scoped
  `ConcurrentJSLocker(useJSThreads() ? &structure->m_lock : nullptr)`.

L6(i) insert side — I37 "no lost/duplicated transitions":
- New `StructureTransitionTable::getMatching(Structure* candidate)`
  (declared StructureTransitionTable.h, defined StructureInlines.h): lookup
  keyed exactly as `add()` keys the candidate (Hash::createKeyFromStructure).
- Every insert site (addNewPropertyTransition, removeNewPropertyTransition,
  attributeChangeTransition, changePrototypeTransition,
  nonPropertyTransitionSlow, setBrandTransition) dual-checks getMatching under
  the already-held m_lock and ADOPTS a racing winner (returns the published
  Structure, with the winner's transitionOffset where there is an offset
  out-param) instead of clobbering it in the table. The loser's candidate
  Structure is discarded unreferenced; if it stole the source's table the
  source rematerializes from its transition chain on demand.
- `StructureTransitionTable::add` ASSERTs flag-on that no matching key exists.

L6(ii) — steal/clone/materialize under the SOURCE's m_lock, O1 DeferGC:
- `takePropertyTableOrCloneIfPinned`: flag-on, steal AND pinned-clone run
  under a `GCSafeConcurrentJSLocker` (m_lock + DeferGC; copy() allocates under
  the lock — O1's sanctioned form).
- `copyPropertyTableForPinning`: flag-on, clone under GCSafe m_lock.
- `materializePropertyTable`: no gate needed — documented as already
  conformant (chain walk locks each structure; source-table copy under the
  found structure's m_lock; rebuilt table private until the locked
  setPropertyTable publication; function-scope DeferGC discharges O1 for every
  allocation under m_lock). Callers must not hold this structure's m_lock.

L6(iii) — mutator uncached table WALKS hold m_lock:
- `Structure::get(VM&, PropertyName[, unsigned&])` (StructureInlinesLight.h —
  this tree's home of the spec's "StructureInlines.h get families") routes
  flag-on to `getConcurrently` after the m_seenProperties fast negative
  (single-word filter; immutable post-publication for non-dictionary
  structures; dictionary readers/writers ordered by the L3 cell lock).
  getConcurrently never allocates and reads any found table under its owning
  structure's m_lock.
- `forEachProperty` (StructureInlines.h), `isSealed`, `isFrozen`,
  `getPropertyNamesFromStructure` (Structure.cpp): flag-on locked walks with a
  steal-retry loop (re-materialize if `propertyTableOrNull() != table` under
  the lock). GCSafe lockers since functors/builders may GC-allocate (O1).
- `addOrReplacePropertyWithoutTransition` (StructureInlines.h): flag-on the
  find WALK joins the mutation in ONE m_lock critical section (a racing
  rehash can no longer invalidate findResult between find and addAfterFind),
  with the same steal-retry.
- The six under-m_lock ASSERTs that called `get(vm, propertyName)` inside
  Structure::add/remove/attributeChange/addOrReplace now query the in-hand
  table directly (`std::get<0>(table->get(uid))`) — flag-on get() acquires
  m_lock and would self-deadlock from under the lock.

### 21. File-list deviation (recorded for review)

Task 3c's spec line names the [SAL] files (Structure.h/.cpp,
StructureCreateInlines.h, StructureTransitionTable.h). Normative L6(iii)
additionally cites the get/find walk families, whose bodies live in
StructureInlines.h / StructureInlinesLight.h (both in this part's owned
`runtime/Structure*` paths, spec §5). Those two files were edited for exactly
that scope plus the routing/assert items above. StructureCreateInlines.h
needed NO 3c change (no transition-table or property-table sites; its SAL
emission landed in Task 3b).

### 22. Audit notes for manifest 7b / Task 9 / unowned callers

- LiteralParser.cpp:1437 reads `structure->trySingleTransition()` lock-free
  flag-on. Analysis: single-slot reads are one aligned word; a mid-inflation
  read sees either the flagged single slot or the (flag-clear) map pointer and
  then returns null -> falls through to the routed, locked
  addPropertyTransitionToExistingStructure. Key fields compared before use, so
  no false positives; stale negatives are benign. No edit made (unowned file);
  flag for the I34/M7-style audit.
- Structure::add/remove/attributeChange<ShouldPin> call ensurePropertyTable
  BEFORE locking. Sound flag-on because their targets are either PRIVATE
  (in-flight transitions, ShouldPin::No) or PINNED (dictionaries,
  ShouldPin::Yes) — pinned tables are cloned, never stolen, so the
  ensure->lock window cannot hand the table to another structure. Task 9
  (dictionary/quarantine) should keep this invariant in view.
- `Structure::get` flag-on contract: callers must not hold the receiving
  structure's m_lock (getConcurrently's chain walk locks it). In-tree
  under-lock callers were converted (see §20); out-of-tree callers that take
  m_lock manually must use getConcurrently-style direct table access.
- getPropertiesConcurrently / forEachPropertyConcurrently: unchanged
  (already locked); compiler-thread readers unchanged per L6.

## From Task 5 (§4.2 conversion, §9.3 accessor definitions, §10.6 stub + witness)

### 23. Sources.txt entry 2 is now DUE (manifest entry 2)

`runtime/ConcurrentButterfly.cpp` now exists. Apply the entry recorded in §2
above verbatim: add the line

```
runtime/ConcurrentButterfly.cpp
```

to Source/JavaScriptCore/Sources.txt in the `runtime/` section, alphabetical
order (after `runtime/CompositeIndexBuffer.cpp`-area entries if present,
otherwise between the `runtime/CommonSlowPaths*`-area and
`runtime/ConsoleClient.cpp`-area entries). Without it, Tasks 3+'s references to
`jsThreadsStopTheWorldAndRun` / `butterflyWorldIsStopped` /
`g_jsThreadsStubWorldStopped` and JSObject.cpp/Structure.cpp's references to
the §9.3 accessors fail to link.

### 24. §10.6 stub + witness: what landed (manifest entry 6 discharge, pre-M4)

- `bool g_jsThreadsStubWorldStopped` (plain bool; unraced under the phase-1
  GIL) and `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS` (defined in
  ConcurrentButterfly.h next to the extern, so bytecode/JSThreadsSafepoint.cpp's
  `#if defined(...)` disjunct-4 read of the witness compiles in — jit CS6).
- `jsThreadsStopTheWorldAndRun(VM&, const ScopedLambda<void()>&)` DELEGATES to
  `JSThreadsSafepoint::stopTheWorldAndRun` (jit CS6's preferred option; that
  stub RELEASE_ASSERTs the APILock + <=1-entered-VM single-mutator witness and
  runs the closure inline). REVISED in adversarial-review round 1: the owned
  witness is raised INSIDE the delegated closure, NOT around the delegated
  call - the delegate's entry begins with `if (worldIsStopped(vm)) run-inline`
  and its worldIsStopped() consults this witness (disjunct 4), so raising it
  before delegating made every outermost OM stop skip the delegate's
  single-mutator RELEASE_ASSERTs. The witness is also now `std::atomic<bool>`
  (relaxed) since cross-thread readers (disjunct 4, compiler/GC threads via
  butterflyWorldIsStopped) consult it - matching the jit side's atomic
  depth-counter discipline. Nested veneer calls save/restore the witness
  inside the closure (R1.h nesting; the delegate's depth counter makes nested
  calls inline).
- `butterflyWorldIsStopped(VM&)` = `g_jsThreadsStubWorldStopped ||
  JSThreadsSafepoint::worldIsStopped(vm)`.
- M4 integration swap (INTEGRATION-DEFERRED): replace the veneer body with the
  real VMManager STWR (+ CS2 GC-conductor bracket), make
  butterflyWorldIsStopped the jit worldIsStopped predicate alone, and delete
  the witness global + macro (JSThreadsSafepoint.cpp's disjunct 4 goes with
  them). All OM stop sites (§4.2-0 in convertToSegmentedButterfly, the F3
  sites in Structure.cpp, later §4.6/§4.7 sites) call only the veneer, so the
  swap is one function body.

### 25. §4.2 conversion: contract notes for Tasks 6-8 implementers

- `convertToSegmentedButterfly(vm, object, newStructureOrNull, offset, value)`
  returns nullptr for RESTART (§4.2): re-enter the WHOLE operation from §2
  dispatch on fresh tag + structureID. nullptr is returned after a step-0 F2
  stop, on any step-3 divergence (structureID changed, racing conversion
  already segmented the object, butterfly vanished), and when the word is not
  flat at planning time. Callers must never blind-retry the conversion itself.
- The §4.3(b2) prohibition is structural here: convertToSegmentedButterfly is
  never called with the cell lock held (it takes the lock itself; step 0 may
  STW). Task 6's stay-flat locked growth must RESTART out to dispatch rather
  than call this function under its lock.
- PA cells: publication is the I36/M8 fenced order (nuke CAS, fence, 64-bit
  butterfly-word CAS — CAS not plain store, per I17, because lock-free §4.4
  element-resize CASes race even under the cell lock — fence, new structureID
  store, with the remaining semantic header bytes written setStructure-style
  before the ID store). Task 8's casButterfly sites compose with this via
  re-dispatch on CAS failure.
- The first spine is published with spineEpoch = 1; replacement spines
  (§4.3-1 / T2 growth, Tasks 6/8) must copy aliasedAllocationBase/Size
  VERBATIM (I7) and increment spineEpoch.
- Fresh out-of-line fragments (trigger grew capacity) are separate 32-byte
  auxiliary allocations (one per fragment) so Task 6b's visitSegmentedButterfly
  can markAuxiliary each non-aliased fragment pointer as an allocation base.
  Fresh fragment slots are cleared before publication.

### 26. VM.cpp manifest entry 4a addendum (self-test re-exercise)

The Task-1 snippet in §4 above already calls
`concurrentButterflySelfTestIfNeeded()`; no additional entry is needed from
Task 5 (the .cpp adds no new startup hook). Entries 4b (M8 mutator fence) and
4c (quarantine-epoch adapter) remain owed by Tasks 6b/9 as recorded in §4.

## From Task 6 (§4.3 protocol, N2 structureOnlyTransition, get/putDirectConcurrent)

### 27. What landed (owned files only; NO new shared-file needs)

- runtime/ConcurrentButterfly.cpp: `trySegmentedTransition` (§4.3 core:
  allocate -> lock -> re-verify -> release-store value -> nuke + 128-bit DCAS
  -> unlock; PA cells use the I36/M8 fenced order with a 64-bit butterfly CAS),
  with the FULL DCAS-failure taxonomy (a) volatile-byte merge, (b1) SW merge
  (spine words / flat reuse, gated on the target's writeThreadLocal being
  invalid - I12), (b2) RESTART for freshly copied flat payloads (I21), (c)
  un-nuke + goto step 3 on butterfly-payload change (I27b), (d) RELEASE_ASSERT.
  Regime dispatch inside the core: None = N3 first install through the §4.3
  DCAS (tag = (currentButterflyTID(), 0)); Flat owner SW=0 = locked stay-flat
  (reuse or copy-grow); Flat foreign/SW=1 = routed to §4.2
  convertToSegmentedButterfly (never converts holding the cell lock); Segmented
  = spine reuse or replacement spine (copy + append; aliasedAllocationBase/Size
  VERBATIM, spineEpoch+1).
- `tryStructureOnlyTransition` (N2 locked path): F2 step-0 firing (butterfly-
  less keying on Structure::transitionThreadLocalTID, butterfly-bearing on the
  instance tag), cell lock, structureID re-check, release-store of the inline
  value FIRST (I9), ONE 64-bit header CAS under the §3.0 merge loop. No nuke
  (butterfly untouched); legal on PA cells (8B-aligned).
- Frozen §9.3 void drivers `segmentedTransition` / `structureOnlyTransition`:
  anchor the source on the settled structure at entry (M5 spin), retry the
  try* core through recoverable RESTARTs, and RELEASE_ASSERT if a racing
  transition changes the SOURCE (callers that can lose that race must call the
  try* forms from their own §2 dispatch; sole tolerated race: the target
  itself was published - value stored SAB last-writer-wins).
- runtime/JSObject.h + ConcurrentButterfly.cpp:
  `JSObjectWithButterfly::getDirectConcurrent(PropertyOffset)` /
  `putDirectConcurrent(VM&, PropertyOffset, JSValue)` (§9.5): full §2
  dispatch, M7(d) loadLoadFence before every tagged-word load, I33
  bounds-checked segmented slots with acquire-re-load on stale spines, I31
  cell-locked AS access (reads included), §3 foreign-SW=0 writes routed
  through `ensureSharedWriteBit` then re-dispatched. Poll-free (I34); never
  decode a possibly-nuked StructureID (M5).

### 28. Intra-part forward declaration (Task 7 ordering note)

ConcurrentButterfly.h now DECLARES `ensureSharedWriteBit(VM&,
JSObjectWithButterfly*)` (frozen §9.3 signature); its DEFINITION is owed by
Task 7. Until Task 7 lands, the only call site is
`putDirectConcurrent`'s foreign-SW=0 branch (flag-on only), so the tree
compiles dark but does not LINK a flag-on shared-write test before Task 7.
No action for the integrator; recorded so a partial-stack build failure is
attributable.

### 29. Index-convention fix in landed Task-5 accessors (recorded for review)

`segmentedOutOfLineSlot`, `segmentedOutOfLineSlotIfWithinBounds` and the §4.2
step-4 store passed `offsetInOutOfLineStorage(offset)` (the NEGATIVE
PropertyStorage index -(k+1), PropertyOffset.h) to `ButterflySpine::
outOfLineSlot(unsigned k)`, which takes the 0-based out-of-line index k
(flat address B-16-8k; §4.1/I8). Task 6 added
`outOfLineButterflyIndex(PropertyOffset)` (= offset - firstOutOfLineOffset) to
ConcurrentButterfly.h and converted all three sites. The I8 alias equations
are unchanged; no other caller passed the negative form to a spine accessor.

### 30. Driver-contract note for the jit/integration consumers

The spec-frozen void signatures cannot report RESTART; E5 slow-path wiring
MUST use `trySegmentedTransition` / `tryStructureOnlyTransition` with their
own §2 re-dispatch loop, passing the source structure the target was derived
from (`expectedSource`). The void forms are for serialized contexts (per-event
STW, GIL-stub phase, single-threaded warmup).

STATUS UPDATE (adversarial-review round 1): the C++ runtime E5 named-property
slow path IS now wired. `JSObject::putDirectInternal` (JSObjectInlines.h)
carries the §2 re-dispatch `while(true)` loop and routes both add-transition
legs (existing-structure and addNewPropertyTransition) plus the
attribute-change publication through
`JSObject::tryPutDirectTransitionConcurrent`, which is the E4 gate:
`mayTransitionLockFreeFromThisStructure` => today's lock-free sequence with
the I29 allocate->revalidate->poll-free publication; otherwise
`trySegmentedTransition` (out-of-line offsets) / `tryStructureOnlyTransition`
(inline / structure-only), false => RESTART. The dictionary leg is cell-locked
(L3/L4, value stored with the table edit - I9), the existing-offset REPLACE
leg fires F1 via `ensureSharedWriteBit` for foreign SW=0 out-of-line stores,
and `putDirectWithoutTransition`/`prepareToPutDirectWithoutTransition` (+ the
two JSObject.cpp WithoutTransition callers) route through the new cell-locked
`putDirectWithoutTransitionConcurrent`. Remaining for jit shims: only the
JIT-emitted transition fast paths (SPEC-jit §5.5).

### 31. Task 6b — §4.5 GC visit landed; I25 barrier audit result

`visitSegmentedButterfly` (frozen §9.3 template signature) is declared in
ConcurrentButterfly.h, defined in ConcurrentButterfly.cpp (inside the
USE(JSVALUE64) region) and explicitly instantiated for `SlotVisitor` and
`AbstractSlotVisitor` — the two instantiations `visitButterflyImpl` needs.
Both segmented branches in owned JSObject.cpp now call it: the
mutatorIsStopped() branch RELEASE_ASSERTs a non-null result (no race is
possible world-stopped); the concurrent double-collect branch surfaces a
nullptr as didRace exactly like the flat path. No shared-file change is
required for this task (the visit is reached only through the existing
visitButterflyImpl template; spines/fragments already allocate from
`vm.auxiliarySpace()`, the same CompleteSubspace as
Butterfly::createUninitialized, satisfying I25's "GC auxiliary, butterfly
subspace" without new subspace registration).

Robustness deviations from a literal §4.5 reading (recorded for review; both
strictly conservative):
- step 4's `outOfLineSize <= 4*outOfLineFragmentCount` is checked and treated
  as didRace rather than asserted: the visit's own raw-structureID snapshot
  can be NEWER than the spine the caller loaded (a §4.3 replacement published
  in between), in which case the publication's vm.writeBarrier(object)
  re-greys the object;
- AS/CoW indexing modes observed on the fresh structure return nullptr
  (didRace) instead of RELEASE_ASSERTing I31/I35: a §4.6 transition INTO AS
  stops mutators, not concurrent markers, so an unpaired {AS structure,
  segmented word} read is a legal stale-spine race. Genuine violations still
  trap: the mutator-stopped revisit re-dispatches on the settled pair and the
  stopped call site RELEASE_ASSERTs a non-null result.

I25 barrier audit (Task 6b deliverable) — every owned segmented
store/publication site was swept; full per-site disposition is recorded in the
comment block above visitSegmentedButterfly in ConcurrentButterfly.cpp. Result:
NO unbarriered site. Summary: mutator fragment-slot stores use
WriteBarrierBase::set on the owner; §4.2-4/§4.3-4 pre-publication release
stores target private (unpublished) storage and are covered by the post-unlock
vm.writeBarrier(object[, value]); every spine/butterfly/header publication
(DCAS, PA fenced order, N3 install, N2 header CAS) emits vm.writeBarrier(object)
(+ (object, newStructure)) after unlock, matching setButterfly's barrier.
Integrator action: none.

## From Task 7 (ensureSharedWriteBit — §3 foreign first write)

### 32. What landed; no shared-file change required

`ensureSharedWriteBit(VM&, JSObjectWithButterfly*)` (frozen §9.3 signature,
declared since Task 6) is now defined in runtime/ConcurrentButterfly.cpp.
It is the §3 "Write existing slot, foreign, SW=0" handler with the full
carve-out set:

- F1 fire-then-DCAS (I12/I13/I10b): writeThreadLocal(S) is fired through the
  §10.6 veneer (chain-fired per F4 inside Structure::fireWriteThreadLocal)
  BEFORE the flip; the flip itself is the 128-bit {header, butterfly} DCAS
  (t,0)->(t,1) under the §3.0 merge loop — volatile header bytes folded
  (I26), any semantic divergence or payload change abandons and re-dispatches
  (lock-free flavor of §3.0 step 4). Pairing the header pins the structureID
  lane to the structure whose set was verified fired, closing the
  fire-vs-racing-transition TOCTOU (same I12 guard idea as the landed
  trySegmentedTransition taxonomy-(b) check).
- §4.6 AS (I31): first foreign write to an ArrayStorage-shape object is a
  per-event STW through the §10.6 veneer — fires writeThreadLocal and
  publishes (installerTID, 1) FLAT inside the SAME stop (closure allocates
  nothing, O4). transitionThreadLocal is NOT fired by a write (I11 untouched);
  shared transitions into/on AS are Task 8's F2 stop sites.
- §4.8 CoW (I35): tryMaterializeCopyOnWriteButterflyForSharedWrite replicates
  today's JSObject::convertFromCopyOnWrite (AllocateInt32/Double/Contiguous
  nonPropertyTransition + private copy) but publishes under F2-fire-first +
  cell lock + nuke + DCAS (PA: I36/M8 fenced order), butterfly side expecting
  exactly the loaded CoW word and installing (currentButterflyTID(), 0).
  The nuke is kept even though shape/m_offset are unaffected because
  visitButterflyImpl dispatches on the structure's CoW-ness (an unpaired
  {CoW structure, auxiliary butterfly} visit would treat the fresh allocation
  as a JSImmutableButterfly CELL).
- I36 PA: the flip is a cell-locked 64-bit CAS (no 16B DCAS at 8-mod-16
  bases); the lock freezes the structure (all PA transitions are cell-locked,
  E4 excludes PA), so the fired-set check cannot go stale under it; lock-free
  §4.4 resize CASes still race the word, handled by CAS-failure re-dispatch.
- R-DOUBLE (§4.7): no rebox, no boxing, no sharing-onset stop — the flip is
  shape-blind; callers do raw 8B element stores on shared Double per §3.

Integrator action: none (no OptionsList/Sources/CMake/VM entries beyond those
already recorded by Tasks 1-6b).

### 33. Intra-part sequencing notes for Tasks 8/6-followups (recorded for review)

- The CoW materializer's "the word is stable under the cell lock"
  RELEASE_ASSERTs are sound once Task 8 routes the flag-on
  butterfly-replacing JSObject.cpp paths (today's plain
  convertFromCopyOnWrite / nukeStructureAndSetButterfly resize sites) through
  casButterfly/the locked protocols per §4.4 and manifest GT10. Until Task 8
  lands, an OWNER thread running today's plain convertFromCopyOnWrite
  concurrently with a foreign materializer is the one §4.8 window not yet
  CAS-serialized; flag-off (I22) is unaffected, and the GIL-stub phase
  (single mutator) cannot reach it.
- The landed putDirectConcurrent AS branch (Task 6) takes the cell lock but
  does not yet fire F1/flip SW for foreign property writes on AS objects;
  Task 8's "§4.6 AS all-access locking + stops" should route its foreign-write
  case through ensureSharedWriteBit (the AS branch above is written to be
  called with no lock held, per the veneer caller contract GT11).

## From Task 8 (§4.4 array CAS, T5, I27/I17, §4.6 AS-COPY + stops)

### 34. What landed; no NEW shared-file change required

All shared-file needs (OptionsList entries, Sources.txt ConcurrentButterfly.cpp,
`-mcx16`/LSE, VM startup assert, m_mutatorShouldBeFenced) were already recorded
by Tasks 1-7; Task 8 adds none.

Landed (owned files only):

- **casButterfly** (frozen §9.3 signature) in runtime/ConcurrentButterfly.{h,cpp}:
  ONE 64-bit seq_cst CAS on the tagged word (M3/I16/I17), with the full
  I27/I4/I2/I3 assert set (assertCasButterflyShape): N3 install form
  (0 -> (currentTID,0)), T1 form (flat payload replacement, tag exactly
  (currentTID,0) both sides, OR cell-locked AS-COPY with tag - incl. SW -
  preserved verbatim), T2 form (segmented -> segmented only; flat->segmented
  stays the §4.2 nuke+DCAS). false => caller re-dispatch, never blind-retry.
- **publishArrayStorageButterflyLocked**: the §4.6 AS-COPY publication form
  (T3/I17) - cell-locked, tag preserved verbatim, RELEASE_ASSERTs the CAS
  (no lock-free racer can target an AS word under I31).
- **tryGrowSegmentedVectorLength** (T2): replacement spine = copy + append
  fresh indexed fragments (aliased base/size VERBATIM - I7; epoch+1;
  shape-keyed hole fill, Double = raw PNaN lanes §4.7, with a pre-CAS shape
  re-check closing the I28 relabel window), published by one casButterfly.
- **ensureLengthSlowConcurrent** / **shrinkButterflyForSetLengthConcurrent**:
  the flag-on GT10 drivers for JSObject::ensureLengthSlow /
  reallocateAndShrinkButterfly. ensureLength: T1 fresh-copy + casButterfly
  (SW flip mid-resize fails the CAS => T2, the copy is DISCARDED - I21/I27);
  foreign/SW=1 flat => §4.2 convert; segmented => T2; CoW => §4.8 (owner
  ensureWritable / foreign ensureSharedWriteBit). NOTE (adversarial-review
  round 1): the former T5 cell-locked in-place vectorLength growth was
  REMOVED - foreign READS of flat words are lock-free and have only a control
  dependency from the vectorLength load to the slot load (no load->load
  ordering on arm64), so an in-place bound raise could pair the new
  vectorLength with pre-hole-fill uninitialized slack. Flag-on, a published
  flat butterfly's vectorLength is now IMMUTABLE: every bound increase
  publishes fresh storage behind the butterfly word (dependency-ordered, like
  T1/T2/conversion).
  Shrink: owner copy+CAS; SW=1/capacity-stale => in-place truncation
  (publicLength only - the capacity shrink is forgone flag-on for shared
  words; weaker post-condition recorded in ConcurrentButterfly.h).
- **GT10 sites**: JSArray.cpp tryGrowAndShiftButterflyRight (:96 site) is the
  T1 CAS with caller re-dispatch (ineligible/failed => the callers'
  ArrayStorage fallback, the §4.6 vector-moving route sanctioned for shared
  objects); unshiftCountSlowCase (:470 site) stays cell-locked with
  casButterfly as publication form and reuse-in-place disabled flag-on;
  shift/unshiftCountWithArrayStorage (:1650/:1818 sites) divert flag-on to
  shiftCountWithArrayStorageConcurrent / unshiftCountWithArrayStorageConcurrent
  - full AS-COPY: everything under the cell lock (pre-lock DeferGC = O1's
  sanctioned back-edge), fresh AS butterfly (indexBias 0) built with the
  shift/gap applied, casButterfly publication, superseded storage never
  written again. JSObject::increaseVectorLength runs flag-on under
  DeferGC + cell lock, with the in-place setVectorLength branch disabled
  (AS-COPY) and both grow publications converted to the locked CAS form.
- **§4.6 stops (I31)**: JSObject::convertToArrayStorageConcurrent - flag-on,
  ALL four convert*ToArrayStorage paths plan + allocate outside a §10.6
  per-event stop (O4) and copy + publish inside it; SHARED triggers (foreign
  tag / SW=1 / segmented source) fire both TTL sets (F2, chain-fire inside)
  and publish FLAT (currentButterflyTID(), 1); owner triggers publish with SW
  preserved and do NOT fire (§5 F2 per-object keying). Publication = M8
  fenced nuke order (PA-legal, I36).
- **I31 all-access locking** in the owned array files: JSArray::pop AS branch,
  JSArray::setLengthWithArrayStorage (sparse-map edits included; locks drop
  before throws - O1), pushInline's AS in-vector fast path (JSArrayInlines.h),
  getIndexConcurrent/putIndexConcurrent AS branches, and (already landed,
  Task 6) get/putDirectConcurrent. putDirectConcurrent's and
  putIndexConcurrent's AS foreign-SW=0 writes now route through
  ensureSharedWriteBit first (resolves item 33, second bullet).
- **§9.5 indexed slow paths**: JSObjectWithButterfly::getIndexConcurrent /
  putIndexConcurrent (frozen §9.5 signatures) in ConcurrentButterfly.cpp.
- **Segmented-word routing in owned array paths** (the flat-only butterfly()
  contract): JSObject::ensureLength dispatches on the word (spine vectorLength
  + shared publicLength slot); JSArray::setLength, pop, pushInline,
  shift/unshiftCountWithAnyIndexingType (incl. post-ensureLength re-checks:
  a mid-call T2 conversion re-routes to the AS path) divert segmented words
  to the §9.5 accessors / ArrayStorage route.
- Item 33, first bullet, resolved: the flag-on butterfly-replacing
  resize paths now publish via casButterfly/locked protocols, so the §4.8
  materializer's "word stable under the cell lock" RELEASE_ASSERTs hold.

### 35. Residuals recorded for Tasks 9/10/12 and the manifest sweeps

- I31 sweep residue (JSObject.cpp): putByIndexBeyondVectorLengthWithArrayStorage
  and putDirectIndexBeyondVectorLengthWithArrayStorage still write
  storage->m_vector / sparse map outside the cell lock AFTER their (now
  locked) increaseVectorLength call; ArrayStorage walks in
  getOwnPropertyNames/defineOwnIndexedProperty and the JSArray.cpp
  sort/fastSlice families bail to generic paths but read AS headers unlocked.
  These are in-place element/scalar accesses (no relayout), to be folded into
  Task 9's L3/quarantine pass + Task 10 stress assertions; the flag-off paths
  are untouched (I22).
- T4/§4.7 residue (UPDATED round 2): the two designated generic byIndex
  fallbacks - JSObject::putByIndex and JSObject::getOwnPropertySlotByIndex -
  and putByIndexBeyondVectorLengthWithoutAttributes now carry flag-on
  prologues that dispatch on the tagged word (§53 item 4); they no longer
  deref the flat-only butterfly() on segmented words, and their AS legs are
  cell-locked. Remaining unconverted owned residue (still reachable only via
  rarer entry points): defineOwnIndexedProperty's AS walks and the
  getOwnPropertyNames AS header reads - still covered by the manifest-7
  §10.7 guard sweep until folded in.
- First install of ArrayStorage on butterfly-less objects
  (createArrayStorage from blank indexing types) is an N3 install, not
  stop-wrapped; a FOREIGN first install is unreachable until api Thread()
  lands and is then a §4.3/N2-routed transition (F2 fires there). Round 2
  NOTE: the DENSE first-install family (createInitialUndecided/Int32/Double/
  Contiguous) is now fully routed through
  JSObject::createInitialIndexedStorageConcurrent (§53 item 2); the AS-flavor
  residue above stands unchanged.
- reallocateAndShrinkButterfly flag-on post-condition is weaker (vectorLength
  may exceed length on shared words); JSArray::setLength is the only runtime
  caller and depends only on publicLength.

### 36. T2 growth of conversion-tailed spines: per-event-stop migration (spec clarification, Task 8)

The frozen spec's T2 ("growth = fresh fragments", §4.1 C2 "last fragment may
cover past B+8*vectorLength, never dereferenced") leaves a latent gap: a
§4.2-aliased spine whose last indexed fragment carries a C2 tail (flat
vectorLengths are odd, so VL % 4 == 1 conversions leave a 2-slot tail) cannot
have its vectorLength raised across that tail - the tail slots alias memory
PAST the flat allocation's precise end (outside aliasedAllocationSize, i.e.
possibly outside the MarkedBlock cell), and pre-publication hole-fills of
shared fragments would also race a concurrent grow+store (lost write, I21).

tryGrowSegmentedVectorLength therefore has two modes:
(a) full-coverage spines (vectorLength == 4*indexedFragmentCount - 1): pure
copy + append + one lock-free casButterfly - never writes a shared fragment;
(b) tailed spines: rebuild ALL indexed fragments fresh under ONE §10.6
per-event stop (pre-allocated, O4; raw 64-bit lane copy of header slot +
elements inside the stop; publication CAS inside the stop). Soundness rests
on I34 (no access holds a slot pointer across a stop), which also licenses
the fragment-identity change for fragment 0's shared publicLength slot: a
stale spine on a reader's stack stays a self-consistent frozen snapshot
(conservative scan, I7). After one migration the spine is full-coverage, so
each object pays at most ONE such stop per conversion - relevant to the jit
Task-13 stop-budget bench alongside the §4.6/§4.7 per-event stops.

This is implementer-resolved under the annex's "spec wins on conflicts" rule
(no frozen text contradicts it; it discharges C2/C4's "never dereferenced"
obligation for grown spines). Flagged here for the next spec re-freeze.

## From Task 9 (§6 dictionary mode + quarantine, D1, hook adapter, I34 audit)

### 37. runtime/VM.cpp — manifest entry 4c: per-server-heap quarantine-epoch adapter registration

The owned code now exists (`registerButterflyQuarantineEpochHook` /
`butterflyQuarantineEpochSlot`, defined in runtime/ConcurrentButterfly.cpp,
declared in runtime/PropertyTable.h). Apply in `VM::VM(...)` (or the
`VM::create` common path), immediately after the existing manifest-4a block
(§4 above) — i.e. after options are finalized and `heap` is constructed, and
in any case BEFORE a second GC client can attach to this VM's server heap
(heap §9):

```cpp
#include "PropertyTable.h" // already in VM.cpp's include set via Structure machinery; add explicitly if not

    // SPEC-objectmodel §6 / §10 manifest entry 4c (Task 9): register the
    // per-server-heap butterfly-quarantine epoch bump. The adapter runs
    // world-stopped once per collection of THIS server heap (legacy AND
    // shared protocols, heap CR §13.10d) and bumps ONLY that heap's slot in
    // the owned ButterflyQuarantineEpochs registry — NEVER a process-global
    // counter (r13). Idempotent per heap; registration must precede client #2.
    if (Options::useJSThreads()) [[unlikely]]
        registerButterflyQuarantineEpochHook(heap);
```

Note `heap` here must be the SERVER `JSC::Heap`. If the VM-construction path
only has the client heap in hand, pass its server reference (the same object
`Heap::heap(cell)` returns for cells allocated by this VM). No unregistration
exists or is needed (the registry retires slots in place; a heap that never
collects after its last quarantine simply never promotes — safe).

If the hook is NOT yet registered, behavior degrades safely: quarantined
deleted out-of-line offsets are never promoted to Reusable, so property
storage for delete/re-add churn grows until the entry is applied. The Task 12
quarantine suite (delete -> re-add reuse across GC, I18/I30/I34) FAILS its
reuse-progress assertion until 4c is applied — that is the intended signal.

### 38. What landed (record) — OWNERSHIP CORRECTION (adversarial-review round 1)

CORRECTION: PropertyTable.h/.cpp are NOT in this part's owned path list
(`Butterfly*, JSObject*, JSCell*, Structure*, StructureTransitionTable*,
JSArray*, ConcurrentButterfly*`); the original §38 heading mislabeled them as
owned. The quarantine edits described below were landed there in an earlier
round. Per the run's write rules this part makes NO FURTHER writes to
PropertyTable.h/.cpp (reverting would itself be an out-of-ownership write).
INTEGRATOR DECISION REQUIRED, one of:
  (a) add `PropertyTable*` to the objectmodel ownership list (the edits are
      internally complete and flag-off byte-identical - I22 - and their sole
      cross-file surface is the two functions declared in PropertyTable.h and
      defined in owned ConcurrentButterfly.cpp:
      `registerButterflyQuarantineEpochHook` / `butterflyQuarantineEpochSlot`);
  or (b) re-assign the diffs to whichever workstream owns PropertyTable and
      have it adopt them verbatim (the full description below is the spec of
      record for that adoption; the registry/hook definitions stay in owned
      ConcurrentButterfly.cpp either way).
No other file outside the owned list carries writes from this part.

- PropertyTable.h/.cpp: §6 flag-gated split of m_deletedOffsets into
  Reusable (existing member) + Quarantined (`m_quarantinedDeletedOffsets`,
  `{offset, epoch}` stamps) with the cached per-heap epoch slot pointer;
  total out-of-line eligibility in addDeletedOffset (inline offsets stay
  Reusable — manifest 7b: inline never quarantined); frozen
  `releaseQuarantinedSlots(uint64_t)`; lazy promotion inside
  hasDeletedOffset() (so nextOffset/takeDeletedOffset draw from Reusable only,
  I18); fresh-offset allocation in nextOffset() skips past quarantined slots
  (storage never shrinks while quarantined, I30); propertyStorageSize() counts
  quarantined slots; clones copy list + stamps + slot. Flag-off: byte-for-byte
  today's single-list behavior (I22).
- ConcurrentButterfly.cpp: ButterflyQuarantineEpochs registry (Lock + stable
  Heap* -> boxed Atomic<uint64_t> map), §10.4c safepoint-hook adapter, and the
  two exported functions above (declared in PropertyTable.h — their sole
  runtime consumer; ConcurrentButterfly.h is intentionally untouched, it is
  the JIT-facing surface).
- JSObject.cpp: flag-on named deletes rerouted to
  deletePropertyNamedConcurrent — §6 L4 cell-lock serialization, L3/I19
  dictionary path (cell lock outer to the m_lock the table edit takes, I20),
  D1/I30 jsUndefined() release-store BEFORE the table edit / structure
  publication at BOTH delete kinds (the spec's JSObject.cpp:2388/:2397 sites,
  now :2954/:2963-era code), F2 step-0 firing for foreign/shared deletes
  (I10b), §4.2 RESTART discipline, and a §3.0 merge-loop 64-bit header CAS
  publication (no nuke — butterfly untouched, maxOffset preserved by
  quarantine, GT#7; legal on PA cells, I36). deletePropertyByIndex: AS branch
  cell-locked (I31/L5) incl. sparse-map find/remove; segmented words store
  the shape's hole encoding through the loaded spine (C4/I33; Double = raw
  PNaN lane, §4.7). putByIndexBeyondVectorLengthWithArrayStorage /
  putDirectIndexBeyondVectorLengthWithArrayStorage: the Task-8-recorded
  in-place AS windows (setLength, post-increaseVectorLength vector store,
  map->vector copy + final store) are now cell-locked flag-on, re-reading
  arrayStorage() under each lock (AS-COPY republication).

### 39. I34 poll-free audit — owned offset-deref windows (Task 9 deliverable)

Audited rule: no poll/allocation/park between obtaining a PropertyOffset/slot
pointer into object storage and the access through it, unless structureID is
re-validated after. Owned windows:

- JSObjectWithButterfly::get/putDirectConcurrent, get/putIndexConcurrent
  (ConcurrentButterfly.cpp): poll-free by construction (recorded at the
  definitions, Task 6/8); the AS legs hold the cell lock across the window.
- JSObject::deletePropertyNamedConcurrent (JSObject.cpp, new): offset produced
  by an m_lock-held walk; the cell-lock acquisition may PARK, so structureID
  is RE-VALIDATED under the lock before the offset is trusted (RESTART on
  mismatch); from there to the D1 store + table edit/header CAS: no poll, no
  GC allocation (PropertyTable edits are fastMalloc), no park.
- deletePropertyByIndex flag-on legs: AS leg re-derives storage under the
  lock; segmented leg derives the slot from the freshly loaded spine and
  stores immediately (bounds-checked, C4/I33).
- locationForOffset()/§Q dispatch and the quickly-family: windows live in
  CALLER code — out of Task 9 scope by the frozen spec: manifest entry 7b is
  the integrator-applied audit of unowned getDirect/getDirectOffset/
  putDirectOffset/locationForOffset callers (out-of-line offsets only).
- PropertyTable promotion/stamping: no object-storage pointers held at all;
  runs entirely under the table's m_lock/private context (L6) with only the
  leaf registry lock inside.

### 40. Task 9 residuals — UPDATED after adversarial-review round 1

- SparseArrayValueMap rehash-vs-locked-reader: PARTIALLY CLOSED in owned
  files; the remainder is now a SHARED-FILE REQUEST (see §51 below). Closed in
  owned JSObject.cpp: (i) every unlocked `map->putEntry` / `map->putDirect`
  call in putByIndexBeyondVectorLengthWithArrayStorage /
  putDirectIndexBeyondVectorLengthWithArrayStorage is followed by a
  cell-locked MAP-IDENTITY revalidation - a racing locked map->vector copy
  that orphans the map forces a full re-run of the operation, closing the I21
  lost-add window; (ii) defineOwnIndexedProperty's structural edits that do
  NOT allocate in the GC heap or run JS (`map->add`, `map->remove`) and its
  AS length bump now run under the cell lock; (iii) the
  getOwnIndexedPropertyNames AS walk (vector scan + sparse-map iteration) is
  cell-locked, with index emission moved outside the lock (O1). REMAINING:
  putEntry/putDirect INTERNAL structural edits (their embedded add/rehash)
  still run unlocked because SparseArrayValueMap.{h,cpp} is outside this
  part's owned paths - the allocate-then-locked-insert split is specified as
  a ready-to-apply request in §51.
- FOREIGN indexed delete on a CopyOnWrite word: FIXED (round 1).
  deletePropertyByIndex's flag-on dispatch now routes foreign CoW deletes
  through ensureSharedWriteBit's §4.8/I35 materialize-first carve-out (and
  fires F1 for foreign SW=0 flat hole stores) before falling through.
- getOwnPropertyNames AS header walk: FIXED (round 1) - cell-locked, see
  above. defineOwnIndexedProperty: the lockable windows are closed (above);
  its long-lived `entryInMap` pointer across JS re-entry remains exposed to
  the putEntry-internal rehash residual and is covered by the §51 request.

### 41. Task 10 (stress modes + §8 assertion ledger) — integrator record

No NEW shared-file entries. Existing entries that Task 10 leans on, unchanged:

- Entry 1 (OptionsList.h): the three §9.6 option lines recorded at the top of
  this document are now CONSUMED — `forceButterflySWBit` via
  `butterflyWriterIsForeign()` (ConcurrentButterfly.h) at every owned write
  dispatch; `forceSegmentedButterflies` via the §3 dispatch/StayFlat
  suppression in `trySegmentedTransition`, the owner-resize rerouting in
  `ensureLengthSlowConcurrent`/`shrinkButterflyForSetLengthConcurrent`, and
  the exported `applyForceSegmentedButterfliesStressIfNeeded()`
  (ConcurrentButterfly.cpp); `verifyConcurrentButterfly` additionally gates
  the new I6/I9/I10b/I11/I12 publication witnesses and the new
  `concurrentButterflyStressSelfTest()`. Until entry 1 is applied, the SFINAE
  probes return constant false and all of the above compiles dark (I22).
- Entry 4a (VM startup): `concurrentButterflySelfTestIfNeeded()` — the call
  recorded earlier in this document now ALSO runs
  `concurrentButterflyStressSelfTest()` behind the same option; no integrator
  change.

Task 10 additions are confined to ConcurrentButterfly.h/.cpp: the §8
invariant→assertion ledger (ConcurrentButterfly.cpp, "Task 10" section) cites
the targeted assertion site for every I1–I37 plus the new O2/O3 cell-lock
depth witness (lockCellChecked/unlockCellChecked/CellLockDepthScope; the
§10.6 veneer entry now RELEASE_ASSERTs no TU-held cell lock). Owned
JSObject.cpp/JSObjectInlines.h flat-INSTALL sites (Task 2 stamping) should
call `applyForceSegmentedButterfliesStressIfNeeded(vm, object)` after
installing flag-on — wiring noted for the Task 12 test pass; the
ConcurrentButterfly.cpp transition/resize choke points already cover every
slow-path allocation routed through this TU.

## From Task 11 (§10 manifest consolidation; entries 1-7b status; §10.7 guard list; 7b audit)

This section is the integrator's single point of entry. It consolidates
manifest entries 1-7b (annex §M, FROZEN NORMATIVE, applied verbatim — the
entry FILES are not implementer-editable; this doc is the implementer-authored
record §10 requires), states each entry's verified application status as of
this writing (checked against the tree, not against git), supplies the one
entry whose ready-to-paste text was still owed (4b), and ships the two
integrator-applied site lists: the §10.7/App.-R8 flatness-guard list with
EXACT FUNCTIONS (manifest entry 7: "exact functions: integrate doc") and the
7b I34 unowned-window audit list ("site list ships in INTEGRATE doc").

### 42. Entry-by-entry status (annex §M text governs; details earlier in this doc)

| # | Annex §M entry (abbreviated; §M text is normative) | Ready-to-paste text | Verified status in tree |
|---|---|---|---|
| 1 | OptionsList.h: four §9.6 options; useJSThreads deduped; lint no `useConcurrentJS` | §1 (top of this doc) | PENDING. `useJSThreads` present (OptionsList.h:681); `forceSegmentedButterflies` / `forceButterflySWBit` / `verifyConcurrentButterfly` NOT yet present (grep clean). SFINAE probes keep owned code compiling dark until applied. Lint PASSES: zero `useConcurrentJS` under runtime/. |
| 2 | Sources.txt: add runtime/ConcurrentButterfly.cpp | §2 + §23 | PENDING (grep of Sources.txt finds no ConcurrentButterfly). The .cpp EXISTS; without this entry nothing links. Apply first. |
| 3 | CMakeLists.txt: install ConcurrentButterfly.h; `-mcx16` (x86-64); arm64 LSE/outlined; I32 backstop | §3 | PENDING (grep of CMakeLists.txt finds no ConcurrentButterfly). Header list = `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS` (CMakeLists.txt:546), alphabetical next to runtime/Butterfly.h. |
| 4a | VM.cpp: §9.2 startup assert behind useJSThreads | §4 + §26 + §41 | PENDING (grep of VM.cpp finds none of our symbols). |
| 4b | VM.cpp: M8 force `heap.m_mutatorShouldBeFenced = true` | §43 below (NEW — was owed) | PENDING. |
| 4c | VM.cpp: per-server-heap quarantine-epoch adapter registration | §37 | PENDING. Degrades safely until applied (§37); Task 12 quarantine suite fails its reuse-progress assertion as the signal. |
| 5 | JSGlobalObject.*: none | §6 | N/A by frozen text (PA-cell global object: I36 locked path). Nothing to do; do NOT add anything there on our behalf. |
| 6 | VMManager STWR = jit M4 (INTEGRATION-DEFERRED); interim §10.6 stub normative+owned | §11 + §24 | DISCHARGED pre-M4: stub + witness live in owned ConcurrentButterfly.{h,cpp}; delegates to `JSThreadsSafepoint::stopTheWorldAndRun` (bytecode/JSThreadsSafepoint.h:88 area, landed) with `JSC_OM_PROVIDES_JSTHREADS_STUB_WITNESS` (jit CS6 disjunct 4). M4 swap recipe = §24 last bullet (one function body + predicate + delete witness). |
| 7 | Flatness guards in unowned butterfly()-callers | §44 below (guard list + exact functions) | INTEGRATOR-APPLIED at integration. Grep re-derived 2026-06-05; matches App. R8 exactly (§44 reconciliation). |
| 7b | I34 unowned-window audit of getDirect/getDirectOffset/putDirectOffset/locationForOffset callers | §45 below (site list + rule) | INTEGRATOR-APPLIED at integration. Out-of-line offsets only (inline never quarantined). |
| 8 | Cross-spec ledger = annex §L, r12/r14 deltas = §L2 | — | RECORD ONLY, plus one PRE-INT gate: §46 below (8h Task-14 promotion decision). |

Application order: 2 → 3 → 1 → 4a → 4b → 4c (4x are one VM::VM block; 2/3
must precede any link); 7/7b at the integration sweep; entry-6 swap only at
jit M4/CS2.

### 43. runtime/VM.cpp — manifest entry 4b (M8): force mutator fencing, heap lifetime

Heap state verified: `m_mutatorShouldBeFenced` (heap/Heap.h:1093, default
false; setter `setMutatorShouldBeFenced` Heap.h:992/Heap.cpp:3653 also raises
m_barrierThreshold); the Heap constructor honors `Options::forceFencedBarrier()`
(Heap.cpp:451); and Heap.cpp:1111 RESTORES the fence from
`Options::forceFencedBarrier()` after legacy GC cycles — so a one-shot setter
call is NOT lifetime-stable; the option must be forced too. Both lines live in
VM.cpp (entry-4 file), in `VM::VM(...)` immediately BEFORE the manifest-4a
block (§4) — i.e. after the `heap` member is constructed and options are
finalized:

```cpp
    // SPEC-objectmodel §10 manifest entry 4b / M8 (GT#7): flag-on, the fenced
    // nuke/publication order must be the ONLY branch and in-place butterfly
    // reallocs must stay disabled for the HEAP LIFETIME. Heap::endMarking
    // restores the fence from Options::forceFencedBarrier() (Heap.cpp:1111),
    // so force the option as well as the live state. THREADS-INTEGRATE(objectmodel)
    if (Options::useJSThreads()) [[unlikely]] {
        Options::forceFencedBarrier() = true;
        heap.setMutatorShouldBeFenced(true);
    }
```

Notes: `Options::forceFencedBarrier()` returns a mutable lvalue (OptionsList.h
:218 accessor pattern); forcing it process-wide is intentional — M8 is a
process-mode statement, and flag-off VMs in the same process merely pay the
already-supported fenced-barrier mode (same as running with the option set on
the command line). If the integrator prefers the options-coherence site
(Options::notifyOptionsChanged) over VM::VM for the option line, that is
equivalent; the `heap.setMutatorShouldBeFenced(true)` line stays in VM::VM
(covers a Heap constructed before the option was forced). Shared-heap note:
Heap.cpp:3661-3663 already pins value=true for shared servers; 4b extends the
same guarantee to flag-on legacy/single-VM heaps, which OM relies on (M8)
independently of useSharedGCHeap.

### 44. Manifest entry 7 — §10.7 flatness-guard list, EXACT FUNCTIONS (App. R8 discharge)

Re-derivation (spec App. R8 command, run 2026-06-05 from Source/JavaScriptCore):
`grep -rln -E '(->|\.)butterfly\(\)' runtime/ tools/ heap/ interpreter/ llint/`
returned exactly: the 14+3 App.-R8 guard files below, PLUS the owned files
(ButterflyInlines.h, JSArray.cpp, JSArrayInlines.h, JSObject.h, JSObject.cpp,
JSObjectInlines.h, Structure.cpp — real §2 dispatch landed, Tasks 2-9; NO
guards), PLUS the exempt pair (JSCellButterfly.h/.cpp — CoW words never
SW=1/segmented, I35/§4.8/GT16), PLUS llint/LLIntSlowPaths.cpp (SPEC-jit
surface, E1-E5/M7 — excluded here per App. R8). heap/ and interpreter/ have
zero textual callers (quickly-family sites there are covered internally per
§9.5/§Q — e.g. interpreter/Interpreter.cpp:351-352). The list therefore
matches App. R8 with NO new offenders. Integrator MUST re-run the same grep at
integration time; any file not classified below is a new offender and gets the
same treatment.

Guard form (manifest entry 7, verbatim pattern), inserted at entry of each
function listed (or immediately after the guarded object becomes known),
tagged `// THREADS-INTEGRATE(objectmodel)`:

```cpp
    if (object->mayBeSegmentedButterfly()) {
        // SPEC-objectmodel §10.7: tagged/segmented word — this fast path
        // derefs butterfly() as flat. Fall to the generic path.
        ... bail (see per-file disposition) ...
    }
```

`mayBeSegmentedButterfly()` is landed (runtime/JSObject.h:793; one load +
compare; constant false flag-off — I22 guarantees the guard is dead code
today). For functions whose butterfly() use sits under `ASSERT`/diagnostics
only, the guard may instead wrap the assert (marked [assert-only] below).
"bail" per site = return the not-fast sentinel the function already has
(false / nullptr / JSValue() / fall-through), so callers take their existing
generic path = E5 dispatch.

Guards — runtime/ (file → exact functions, with the butterfly() lines):

1. ArrayPrototype.cpp — `arrayProtoFuncJoin` (:452; guard the
   `holesMustForwardToPrototype` fast branch, bail to generic join);
   `arrayProtoFuncReverse` (:629/:641/:651; guard before the shape switch,
   bail to the generic reverse loop); static `sortCompact` (:827/:844/:865;
   bail => caller's generic sort); `fastIndexOf` (:1245/:1269/:1314; bail =>
   return the "not fast" sentinel so callers run generic indexOf);
   `arrayProtoFuncIndexOf` (:1356; its fastIndexOf call is then already
   covered — guard only the direct :1356 length/butterfly peek);
   static `tryConcatAppendOneNonArray` (:1480; guard BOTH `first` and the
   result path; bail nullptr); `tryConcatAppendArrayFastWithWatchpoints`
   (:1535/:1536; guard firstArray AND secondArray; bail nullptr).
2. ArrayPrototypeInlines.h — `fastArrayJoin` (:311/:335/:354/:372; single
   guard at entry on thisObject; bail to the generic join slow path; the
   :354 re-load compare `thisObject->butterfly() == &butterfly` is then
   unreachable on tagged words).
3. LiteralParser.cpp — `LiteralParser<CharType, reviverMode>::parseRecursively`
   (:1488 `object->butterfly()`); object is parser-created on THIS thread, so
   the word is (currentTID,0) flat unless forceSegmentedButterflies — guard
   bails to the non-fast putDirect path. (The :1451 transition lookup is
   already L6-routed, §20.)
4. RegExpMatchesArray.h — `createRegExpMatchesArrayForPlainRegExp` (:82
   [assert-only] / :86 gcSafeZeroMemory tail-clear). Array is freshly created
   thread-private (tryCreateUninitializedRestricted) — flat by construction;
   under forceSegmentedButterflies stress the §9.6 stress hook intentionally
   EXEMPTS uninitialized-restricted windows (ObjectInitializationScope), so a
   static-flatness comment + debug guard suffices: wrap :82/:86 in
   `ASSERT(!array->mayBeSegmentedButterfly())`.
5. RegExpMatchesArray.cpp — `createRegExpMatchesArrayWithGroupsOrIndices`
   (:96/:99/:108/:111) and `createRegExpMatchesArrayForPlainRegExpHavingABadTime`
   (:252/:256): same disposition as (4) — fresh private arrays; debug-assert
   guard form.
6. ObjectInitializationScope.cpp — `ObjectInitializationScope::
   verifyPropertiesAreInitialized` (:75) [assert-only, ASSERT_ENABLED block]:
   guard at entry, early-return when mayBeSegmentedButterfly() (verification
   walk derefs flat layout; the object is mid-initialization thread-private,
   so this fires only under stress modes).
7. CommonSlowPaths.h — `allocateNewArrayBuffer` (:248/:249) [assert-only]:
   result is a freshly created CoW-backed array (JSCellButterfly payload,
   never segmented — I35); wrap both ASSERTs in the debug guard form. No
   runtime guard needed (CoW words are flat-decodable by contract).
8. ObjectConstructorInlines.h — `objectCloneFast` (:217) and
   `tryCreateObjectViaCloning` (:280): SOURCE-object butterfly memcpy — a
   REAL spine-as-flat OOB risk. Full runtime guard at entry on `source`
   (`if (source->mayBeSegmentedButterfly()) return false / nullptr;`) —
   callers fall to the generic clone (structure-walk) path.
9. ClonedArguments.cpp — `ClonedArguments::copyToArguments` (:325): guard at
   entry on `this`; bail to the existing generic per-index copy loop.
10. DirectArguments.cpp — `DirectArguments::fastSlice` (:226): butterfly() is
    on the freshly allocated `resultArray` (private, flat); debug-assert
    guard form on resultArray; no runtime bail needed.
11. JSCJSValue.cpp — `JSValue::dumpInContextAssumingStructure` (:319)
    [diagnostic]: runtime guard; on tagged words print the raw tagged word
    (`taggedButterflyWord()`) instead of dereferencing as flat.
12. JSGenericTypedArrayViewInlines.h — `JSGenericTypedArrayView<Adaptor>::
    copyFromInt32ShapeArray` (:378/:382/:386/:390) and
    `copyFromDoubleShapeArray` (:408/:413) — the spec's "six contiguous paths
    :378-413; spine-as-flat deref = OOB" (typedArray.set(sharedArray) is the
    canonical attack). Full runtime guard at entry of BOTH functions; bail =
    return false so setFromArrayLike takes the generic per-element path
    (which lands in §9.5-dispatched accessors).
13. JSONObject.cpp — `FastStringifier<CharType, bufferMode>::appendInt32Array`
    (:1507 dot-call `array.butterfly()`): runtime guard; bail = the
    FastStringifier's existing recordFailure path (falls back to the general
    stringifier). (The getDirect sites :612/:626/:1393 are §9.5-covered — NO
    guards, per §Q.)
14. JSArrayBufferView.cpp — AUDITED, NO GUARD NEEDED (grep false positive,
    recorded as App. R8 requires): the sole dot-call (:171
    `setButterfly(vm, context.butterfly())`) is
    `ConstructionContext::butterfly()` — a context-owned fresh allocation for
    the view under construction, not a JSObject's tagged word. Disposition:
    static-flatness comment at the site; nothing else.

Guards — tools/ (diagnostic; runtime guard form, print-raw-word bail):

15. JSDollarVM.cpp — `functionCpuClflush` (:2504/:2505; on tagged words skip
    the butterfly flush lines) and `functionDeltaBetweenButterflies` (:3864;
    mask via untaggedButterfly() or return jsUndefined() for tagged words).
16. HeapVerifier.cpp — `HeapVerifier::reportCell` (:393; print raw tagged
    word when mayBeSegmentedButterfly()).
17. VMInspector.cpp — `VMInspector::dumpCellMemoryToStream` (:543; same).

Exempt (verbatim App. R8): runtime/JSCellButterfly.h, runtime/JSCellButterfly.cpp
(CoW words never SW=1/segmented — I35/§4.8/GT16; recorded so the next audit
does not flag them). Not here: llint/LLIntSlowPaths.cpp + DFG/FTL/Baseline =
SPEC-jit surface (E1-E5/M7, jit §5.5/CS5). Owned runtime files in the grep
output get real dispatch (landed), never guards.

### 45. Manifest entry 7b — I34 unowned-window audit site list

Rule (annex §M 7b, verbatim obligation): for every UNOWNED caller of
getDirect / getDirectOffset / putDirectOffset / putDirectWithoutBarrier /
locationForOffset, any window where a poll/allocation/park can intervene
between the offset-producing structure lookup (get(vm, ...) /
getDirectOffset(vm, ...)) and the deref through that offset gets (a)
structureID revalidation after (mismatch => re-lookup or generic path) or
(b) conversion to get/putDirectConcurrent (§9.5; poll-free, M5/M7).
OUT-OF-LINE offsets only — inline offsets are never quarantined (PropertyTable
addDeletedOffset keeps inline Reusable, §38) and a stale inline read is a
plain SAB-granularity race, not a UAF. Owned-file windows were audited in
Task 9 (§39); jit-compiled windows are discharged by per-access structure
checks (ICs) per history §15.9.

Survey grep (run 2026-06-05):
`grep -rln -E '(->|\.)(getDirect|getDirectOffset|putDirectOffset|putDirectWithoutBarrier|locationForOffset)\(' runtime/ tools/ interpreter/ heap/ inspector/ API/ wasm/ jsc.cpp`
— unowned files, classified:

CLASS A — offset and deref in one straight-line expression/statement window,
no poll/alloc between (audit = confirm and annotate; NO code change expected):
runtime/Lookup.cpp (:53/:63 getDirectOffset then immediate getDirectOffset
deref — the canonical pattern), runtime/IntlObjectInlines.h,
runtime/IntlPartObject.h, runtime/IntlSegmentDataObject.h,
runtime/MapIteratorPrototypeInlines.h, runtime/SetIteratorPrototypeInlines.h,
runtime/SetPrototypeInlines.h, runtime/RegExpPrototypeInlines.h,
runtime/CommonSlowPathsInlines.h, runtime/ArrayConstructor.cpp,
runtime/IteratorOperations.cpp, runtime/JSGlobalObjectFunctions.cpp,
runtime/FunctionRareData.cpp, runtime/JSLexicalEnvironment.cpp.

CLASS B — getDirect on KNOWN INLINE offsets / internal fields (exempt by the
out-of-line-only rule; annotate only): runtime/JSPromise.cpp,
runtime/JSPromiseConstructor.cpp, runtime/JSPromisePrototype.cpp,
runtime/JSMicrotask.cpp, runtime/JSModuleLoader.cpp,
runtime/ThreadObject.cpp + runtime/ThreadAtomics.cpp (api-part files; their
own spec's audit applies — cross-ref INTEGRATE-api.md).

CLASS C — freshly created thread-private targets (no foreign deletes
constructible before publication; annotate only): runtime/RegExpMatchesArray.h,
runtime/RegExpMatchesArray.cpp, runtime/ClonedArguments.cpp,
runtime/ObjectConstructorInlines.h + runtime/ObjectConstructor.{h,cpp}
(cloning paths; their SOURCE-object reads are behind the §44.8 guard which
bails before any offset wandering), runtime/CommonSlowPaths.{h,cpp}.

CLASS D — REAL audit targets (a throwing/allocating call CAN sit between
lookup and deref; integrator applies fix (a) or (b) per site):
- runtime/JSONObject.cpp (:612/:626/:1393 getDirect inside Stringifier — the
  toJSON/getter calls between holder-offset capture and the next read
  re-enter JS; the Stringifier already re-reads via getDirect each pass, so
  confirm each getDirect's structure check is the SAME statement, else (b)).
- runtime/ProxyObject.cpp (target getDirect around trap calls — re-entrant by
  construction; any cached offset across a trap call must revalidate).
- runtime/JSFunction.cpp (rareData/prototype getDirect around allocation of
  the prototype object; revalidate after allocation).
- runtime/JSGlobalObject.cpp (init-time getDirect/putDirectOffset — single
  threaded at init, but the global object is a PA cell: annotate I36, no
  window fix expected).
- runtime/LiteralParser.cpp (putDirectOffset on freshly transitioned objects
  — private until returned; confirm no GC-allocating call between
  addPropertyTransitionToExistingStructure and the putDirectOffset — there is
  one (JSString allocation for keys) on some paths: revalidate or reorder).
- runtime/ArrayPrototypeInlines.h (getDirectOffset in fastArrayJoin after the
  §44.2 guard; the join loop allocates strings between reads — but each read
  re-derives via the per-iteration butterfly identity re-check :354; confirm
  and annotate).
- interpreter/ — no textual callers (LLInt/jit surface excluded).
- inspector/ScriptCallStackFactory.cpp, jsc.cpp, tools/JSDollarVM.cpp —
  diagnostic/shell code: fix (b) (use getDirectConcurrent) where the deref
  can race a mutator, else annotate.

Discharge record: each Class-D site the integrator touches gets the
`// THREADS-INTEGRATE(objectmodel) I34` tag; Classes A-C get a one-line
audit annotation in the integration commit message only (no code churn).

### 46. Entry 8 record — 8h PRE-INT decision gate (r14)

Per spec §10 row 8 (r14 text): the post-fire fallback survey REJECTED both a
per-structure transition lock and lock-free N2 header-CAS (contention;
same-offset value clobber breaks I9/I21; history §21). The landed cell-locked
N2 (`tryStructureOnlyTransition`, §27) STANDS pending spec Task 14
(per-thread structure splitting, post-GIL charter). The PROMOTION DECISION is
made PRE-INTEGRATION on jit Task-13's GIL-stub shared-constructor construction
bench (§L2.h): if foreign butterfly-less adds dominate that bench beyond the
budget, Task 14 is promoted INTO the integration milestone; otherwise the
cell-locked N2 ships as-is. Integrator action: run the jit Task-13 bench
before the build-fix loop closes and record the verdict here. The other
ledger rows (a-g, i) are closed/record-only per annex §L/§L2; nothing further
from this part.

**VERDICT RECORD (GIL-removal review round 4 — DEFERRAL, not a verdict):**
the §L2.h bench has NOT run (no build executed in any ungil round); the
no-PROMOTE arm (cell-locked N2, §27) is the OPERATIVE INTERIM verdict and is
what U-T10 (ConcurrentButterfly locked third arm) and U-T11 (§C.3 PWT
pre-enqueue routing) landed against. The explicit gate-deferral ruling lives
in SPEC-ungil-history.md ("GIL-removal review round 4", Ruling 2), which
names those two landed surfaces as mandatory re-review on a PROMOTE outcome.
Integrator action (owed at the FIRST Build round, tracked as
INTEGRATE-ungil.md AB-7): run the jit Task-13 §L2.h bench and replace this
deferral record with the actual verdict.

### 47. Task 11 self-checks performed (record)

- App.-R8 grep re-run: list matches frozen annex EXACTLY (no new offenders,
  no vanished files); JSArrayBufferView.cpp resolved as a ConstructionContext
  false positive (§44.14) — the file stays on the list as audited.
- OptionsList lint: zero `useConcurrentJS` occurrences (entry 1 lint clause).
- Entry-6 stub: `JSThreadsSafepoint::{stopTheWorldAndRun,worldIsStopped}`
  confirmed present (bytecode/JSThreadsSafepoint.h) — the §24 delegation and
  CS6 witness macro path is live, not speculative.
- Heap fence restore path confirmed (Heap.cpp:1111) — motivates §43's
  two-line 4b form (option + live state), recorded so nobody "simplifies" it
  to the setter call alone.
- VM.cpp/Sources.txt/CMakeLists.txt verified to contain NONE of this part's
  entries yet (statuses in §42 are tree-verified, not assumed).

## From Task 12 (tests: JSTests/threads/objectmodel/, i03-*.js; C++ self-test legs)

### 48. Test corpus landed (record; no shared-file change beyond manifest entry 1)

JSTests/threads/objectmodel/ (new directory, owned). One named suite per
annex §15.T12 scenario plus the spec-§11.12 extras; all GIL-stub runnable,
deterministic, and written to re-run UNMODIFIED at M4/CS2 (final-state checks
are interleaving-independent; reader threads accept only {old, undefined,
new} value sets):

- i03-selftest.js — drives the C++ self-tests (concurrentButterflySelfTest +
  concurrentButterflyStressSelfTest) via --verifyConcurrentButterfly=1 at VM
  startup (manifest entry 4a), then exercises every tag-decode regime with
  per-decode validation live.
- i03-single-threaded-no-change.js (flag-OFF) and
  i03-single-threaded-flag-on.js (flag-ON, zero Threads) — the I22 twins.
- i03-n2-inline-add-races.js — §15.T12 "N2 inline-add races" (I9/I21).
- i03-n3-first-install-races.js — "N3 racing first installs" (I21).
- i03-t1-vs-sw-flip.js — "T1-vs-SW-flip" (I27).
- i03-t5-racing-growers.js — "T5 racing growers + T5-vs-conversion" (I21).
- i03-b2-stay-flat-growth-vs-sw-flip.js — "§4.3(b2) stay-flat growth vs SW
  flip".
- i03-restart-locked-vs-conversion.js — "locked-transition-vs-planned-
  conversion RESTART" (I10b/I11).
- i03-stale-spine-reader-vs-grow.js — "stale-spine reader vs T2 grow+push"
  (I33, indexed AND out-of-line clauses).
- i03-convert-grow-gc-read.js — "convert -> grow x2 -> GC -> read
  pre-conversion property" (I7/I25), named + indexed flavors.
- i03-as-sparse-holes.js — "owner sparse-insert loop vs foreign hole-reads,
  locked" (I31).
- i03-as-shift-unshift.js — "shift/unshift AS-COPY vs stale reader snapshot"
  (I31).
- i03-cow-materialize-race.js — "CoW: two foreign writers race
  materialization" (I35), disjoint-index + same-index + double-literal legs.
- i03-pa-global-races.js — "oversized/PA-cell transition suite" (I36):
  global-object (PA cell) add storms, foreign write vs owner transition,
  delete/re-add quarantine on the PA table.
- i03-quarantine-readd-across-gc.js — "delete -> re-add quarantine reuse
  across GC" (I18/I30/I34), plain + dictionary-storm flavors.
- i03-visit-range-outofline.js — "§4.5 visit-range cross-check outOfLineSize
  1..9" (I25/I8), out-of-line + indexed fragment-boundary flavors.
- i03-i37-same-shape-add-storm.js — §15.T12 r14 addendum: I37 same-shape add
  storm (distinct instances, one shared transition chain, L6) + a remove-
  transition storm flavor.
- i03-shared-double.js — I28 shared-Double (R-DOUBLE growth, Double->
  Contiguous per-event-STW relabel vs readers, Int32->Double on shared).
- i03-array-resize-cas.js — I16/I17/I10 element-resize CAS storms (foreign
  push storm vs reader; dual disjoint-half growers).
- i03-stress-force-segmented.js / i03-stress-force-sw.js — §9.6 stress modes
  end-to-end (semantics must be identical under forced representation /
  forced-foreign writes; AS+CoW exemptions checked).

### 49. Dependencies and expected pre-integration failure modes

- Manifest entry 1 (OptionsList.h): i03-selftest.js and both i03-stress-*.js
  pass --verifyConcurrentButterfly / --forceSegmentedButterflies /
  --forceButterflySWBit via //@ requireOptions. The tree currently carries
  ONLY useJSThreads (verified at Task 11, §42); until entry 1 lands these
  three tests fail OPTION PARSING — that is the expected pre-integration
  state, not a corpus bug. All other i03 tests need only --useJSThreads=1.
- The GIL-stub Thread/Atomics-on-properties API (SPEC-api; already used by
  the existing JSTests/threads corpus) and the threads harness
  (JSTests/threads/harness.js + resources/assert.js) are consumed unchanged.
- gc() is the jsc-shell global; no $vm dependency anywhere in the corpus
  (the suites must run in production-shaped shells at M4/CS2).
- STW suites run against the §10.6 interim stub pre-M4 (manifest entry 6);
  per spec §11.12 they are re-run UNMODIFIED at M4/CS2 — no test encodes any
  stub-only timing assumption (all rendezvous go through Atomics gates +
  harness sleepMs).

### 50. C++ self-test delta (owned ConcurrentButterfly.cpp)

concurrentButterflyStressSelfTest() gained the Task-12 PA-lane witness
(I32/I36): asserts the 8-mod-16 ("PA-shaped") address is rejected by the
dcasHeaderAndButterfly alignment-gate predicate (checked against the
predicate, not by invoking the crashing gate), then exercises the I36-legal
64-bit butterfly-word CAS at exactly such an address — success flip leaves
the adjacent header lane bit-identical (I16/I36), stale-expected re-flip
fails and leaves memory untouched (the cell-locked PA re-dispatch shape).
Pure memory, no VM/heap; runs behind verifyConcurrentButterfly like the rest
of the self-tests. No new shared-file requirement.

## From adversarial-review round 1 (fixes landed; new shared-file requests)

### 51. SparseArrayValueMap.{h,cpp} — shared-file request: allocate-then-locked-insert split

SparseArrayValueMap.{h,cpp} is OUTSIDE this part's owned paths, so the
remaining putEntry/putDirect internal-rehash residual (§40) is recorded here
as a request for the integrator / owning workstream. Requested change,
mirroring the §4.3 allocate->lock->verify->publish discipline:

- In `SparseArrayValueMap::putEntry` and `SparseArrayValueMap::putDirect`
  (SparseArrayValueMap.cpp), flag-on (`Options::useJSThreads()`), perform the
  `m_map.add(...)` STRUCTURAL step under the OBJECT's cell lock
  (`Locker { object->cellLock() }`): `add` of a default SparseArrayEntry only
  fastMallocs (no GC allocation, no JS), so it is wrappable; pre-reserve
  capacity (`m_map.reserveInitialCapacity`-style grow) BEFORE the lock when a
  rehash would be needed, so the locked section never rehashes. The
  subsequent `entry.put(...)` (which can run JS for accessor entries and can
  throw) stays OUTSIDE the lock, but must re-find the entry under the cell
  lock after any JS re-entry before writing through it (iterator/pointer may
  be stale once adds are concurrent).
- `SparseArrayValueMap::remove` similarly takes the object's cell lock around
  the structural erase (callers in owned JSObject.cpp already hold it -
  deletePropertyByIndex, defineOwnIndexedProperty - so the internal lock must
  be conditional or the call sites adjusted; the simplest form is an
  `AbstractLocker`-taking overload asserted held).
- Lock order: JSCellLock < Structure::m_lock (I20); the map edit takes no
  m_lock, so the only constraint is no GC allocation inside (O1 - satisfied
  by the pre-reserve).

Until applied, the residual window is: a cell-locked reader iterating the map
(all owned readers now lock - §40) racing the UNLOCKED internal add/rehash
inside putEntry/putDirect. The owned-file mitigations (map-identity
revalidation, locked add/remove at defineOwnIndexedProperty) close the
lost-add and walk-vs-walk legs; only putEntry's internal rehash remains.

### 52. Round-1 fix ledger (what changed in owned files; for re-review)

1. putDirectInternal (JSObjectInlines.h): now the §2 re-dispatch loop with
   E4 gating - see §30 STATUS UPDATE. New helpers:
   `JSObject::tryPutDirectTransitionConcurrent` (E4/I29 lock-free form or
   trySegmentedTransition/tryStructureOnlyTransition),
   `JSObject::putDirectWithoutTransitionConcurrent` (cell-locked
   without-transition add, value stored with the table edit - I9), and a
   cell-locked dictionary add/replace leg (L3/L4).
   putDirectToDictionaryWithoutExtensibility's replace store is cell-locked
   with structure revalidation (flatten renumber race).
2. In-place indexing-shape relabels (JSObject.cpp): convertUndecidedTo* /
   convertInt32ToDouble / convertInt32ToContiguous / convertDoubleToContiguous
   now dispatch flag-on to `JSObject::relabelIndexingShapeConcurrent`
   (per-event §10.6 stop, F2 fired for shared triggers, flat AND segmented
   lanes rewritten under the stop - I28/§4.7). Their flag-on return values
   are empty Contiguous* handles for segmented words (callers fall to generic
   §9.5-routed paths).
3. §3 F1 enforcement closed at: trySetIndexQuicklyConcurrent /
   setIndexQuicklyConcurrent flat dense writes (+ the AS leg of the latter),
   JSObject::ensureLength's publicLength bump (JSObject.h; definition moved
   below JSObjectWithButterfly), putDirectInternal's out-of-line REPLACE leg,
   and deletePropertyByIndex's flat foreign hole stores. The stale "Task 7
   not wired" comment is deleted.
4. canSetIndexQuicklyForPutDirect (JSObject.h putDirectIndex): flag-on regime
   dispatch - segmented/AS report false (generic path), flat bound from the
   single loaded word.
5. T5 removed (ConcurrentButterfly.cpp ensureLengthSlowConcurrent): flat
   vectorLengths are immutable flag-on; see §34 note. All T5 references in
   comments updated (ConcurrentButterfly.h/.cpp, JSObject.cpp).
6. flattenDictionaryStructure TOCTOU (Structure.cpp): SUPERSEDED by round 2
   (§53 item 3) - flag-on flatten now ALWAYS runs under the §10.6 stop (the
   "unshared" classification was unsound against undetectable read-only
   foreign sharing); the under-lock revalidation now bails whenever the impl
   runs flag-on outside a stop. flattenRequiresStopTheWorld was renamed
   flattenTriggerIsShared and only gates the F3 firing inside the stop.
7. storeTaggedButterflyWordConcurrent (JSObjectInlines.h): now a CAS loop
   (racing SW flips folded, never erased - I4) with a RELEASE_ASSERT runtime
   witness that the replaced word is empty or owner-tagged flat.
8. §10.6 veneer (ConcurrentButterfly.cpp): witness raised INSIDE the
   delegated closure; witness is std::atomic<bool> - see revised §24.
9. Structure.h: added `static constexpr ptrdiff_t
   transitionThreadLocalTIDOffset()` (OBJECT_OFFSETOF on the private member,
   same pattern as the existing offset accessors) for SPEC-jit §5.5's dynamic
   butterfly-less transition predicate. jit workstream: the 16-bit TID load
   is `Structure::transitionThreadLocalTIDOffset()`; no static_assert pins
   the offset, so emitted code must use this accessor, not a hand-computed
   constant.
10. SparseArrayValueMap call-site hardening + locked AS walks - see §40/§51.
11. deletePropertyByIndex: foreign CoW deletes routed through §4.8 (see §40).
12. PropertyTable ownership correction - see §38.

## From adversarial-review round 2 (fixes landed; one pending shared-file diff)

### 53. Round-2 fix ledger (what changed in owned files; for re-review)

1. visitSegmentedButterfly (ConcurrentButterfly.cpp/.h, JSObject.cpp):
   signature now takes the CALLER's bracketed {early structureID, Structure*,
   maxOffset, indexingMode} snapshot from visitButterflyImpl (whose spine load
   is dependency-ordered after the early structureID load and whose late
   re-checks run before the call). The function no longer loads its own fresh
   structureID for the shape decision - a fresh load could pair a NEWER
   structure (in-place §4.7 relabel; structure published after a mode-(b) T2
   migration) with the caller's OLDER spine and value-visit raw-double /
   uninitialized lanes as JSValues. Step 6 re-loads the structureID and
   compares against the caller's EARLY id. An explicit M7(d) loadLoadFence was
   added at entry (belt-and-braces over the consume chain; no-op on x86-64).
2. createInitial* family (JSObject.cpp, JSObject.h, JSArray.cpp): new
   JSObject::createInitialIndexedStorageConcurrent is the flag-on route for
   createInitialUndecided/Int32/Double/Contiguous. Regimes: N3 word==0
   (structureID nuke-CAS + casButterfly(0 -> (tid,0)), loser RE-DISPATCHES -
   no more RELEASE_ASSERT crash on racing first dense installs); E4 owner
   fast path (flat owner tag, SW=0, both source TTL sets valid; exact-word
   casButterfly, poll-free copy window); shared/segmented/sets-fired -> §10.6
   per-event stop with F2 fired for shared triggers (I10/I10b closed for
   indexed first installs) and, for segmented sources, a replacement spine
   (out-of-line fragments aliased verbatim, fresh header + indexed
   fragments). Flag-on, createInitialUndecided may return nullptr and the
   Int32/Double/Contiguous flavors may return empty handles (= racer won or
   segmented publication); callers re-dispatch. New
   JSObject::tryCreateInitialForValueAndSetConcurrent wires the two
   createInitialForValueAndSet call sites (putByIndexBeyondVectorLength /
   putDirectIndexSlowOrBeyondVectorLength blank legs) with loser
   re-dispatch; JSArray::setLength's ArrayClass leg re-dispatches on nullptr.
   TEST REQUEST (for the i03 suite owner / next Task-12 pass; JSTests is
   outside this round's write set): extend i03-n3-first-install-races.js with
   a leg racing the FIRST element store on a PLAIN object (o = {} shared, N
   threads storing disjoint dense indices via the putByIndex generic path) -
   the existing array leg uses new Array(), which is born with a butterfly
   and never exercised this route.
3. flattenDictionaryStructure (Structure.cpp/.h): flag-on flatten ALWAYS runs
   under the §10.6 stop. The previous "unshared" classification (TTL sets
   valid + word not SW/segmented) was unsound: read-only foreign sharing
   fires no watchpoint and never flips SW, while the flag-on dictionary READ
   path is lock-free - the in-place compaction (renumbered offsets, zeroed
   tails, setButterfly(nullptr)/memmove) could crash or tear pure readers.
   flattenRequiresStopTheWorld renamed flattenTriggerIsShared; it now only
   gates whether F3 FIRES inside the stop (owner-local objects keep their
   sets). flattenDictionaryStructureImpl bails (nullptr, nothing mutated) if
   ever reached flag-on outside a stop.
4. Generic byIndex paths (JSObject.cpp): JSObject::putByIndex and
   JSObject::getOwnPropertySlotByIndex have flag-on prologues - dense regimes
   via the §9.5 accessors (with the legacy shape-conversion triggers
   preserved; CoW handled per §4.8 incl. the foreign materialize-first leg),
   AS regimes cell-locked with the word re-read under the lock (I31/L5;
   SlowPut interception runs OUTSIDE the lock, completion re-locked).
   putByIndexBeyondVectorLengthWithoutAttributes got a flag-on leg that
   dispatches on the tagged word (no flat-only butterfly() deref), uses
   min(publicLength, vectorLength) as the density-POLICY estimate (the
   countElements walk is unsafe on shared storage; layout policy only), and
   stores via trySetIndexQuicklyConcurrent with ensureLength-driven growth.
5. publicLength bumps (Butterfly.h, JSObject.cpp): new
   Butterfly::bumpPublicLengthToAtLeast and
   ButterflySpine::bumpPublicLengthToAtLeast (32-bit CAS-max loops). All nine
   dense-store bump sites in trySetIndexQuicklyConcurrent /
   setIndexQuicklyConcurrent now use CAS-max on shared words (flat SW=1 via
   updatePublicLengthAfterDenseStoreConcurrent; segmented always); owner
   (t,0) flat words keep the plain store; deliberate truncation (shrink
   drivers) keeps plain stores (shrink-vs-grow is program-racy under SAB
   semantics). Fixes the racing-growers length regression (I21; i03-t5
   part (a)).

### 54. PropertyTable.h - READY-TO-PASTE diff: quarantine inline deleted
### offsets too (review-round-2 major; pending the §38 ownership adjudication)

Round-2 finding (accepted as REAL): addDeletedOffset's flag-on leg
quarantines only out-of-line offsets; inline deleted offsets go straight to
the Reusable list and can be handed back by the very next nextOffset() with
no intervening stop. THREAD.md's rule has no inline carve-out, and the
aliasing hazard applies to inline slots identically: dictionary structures
mutate IN PLACE, so a lock-free reader/writer that resolved an INLINE offset
for f (IC or m_lock-held walk) and sits between its structure check and its
slot access can race delete(f) + add(g) reusing the same inline slot - the
tardy read returns g's value under the name f, and a tardy putDirectOffset
overwrites g (lost write). The prior 7b justification ("inline slots live in
the cell, are atomic") addresses TEARING, not ALIASING. The D1 jsUndefined
release-store already covers inline slots, and nextOffset() already counts
ALL quarantined offsets when skipping, so the only change needed is the
eligibility condition.

Because PropertyTable.h is outside this part's owned paths (see §38), the
diff is recorded here as ready-to-paste text for whichever party the
integrator designates:

In `PropertyTable::addDeletedOffset` (PropertyTable.h, currently ~line 565),
REPLACE:

```cpp
    if (Options::useJSThreads() && isOutOfLineOffset(offset)) [[unlikely]] {
        // §6 eligibility is TOTAL: EVERY deleted out-of-line offset is
        // quarantined (dictionary-mode deletes AND non-dictionary
        // removePropertyTransition; NO bypass). Inline offsets fall through to
        // the Reusable list (never quarantined - manifest entry 7b).
        quarantineDeletedOffset(offset);
        return;
    }
```

WITH:

```cpp
    if (Options::useJSThreads()) [[unlikely]] {
        // §6 eligibility is TOTAL: EVERY deleted offset - inline AND
        // out-of-line - is quarantined (dictionary-mode deletes AND
        // non-dictionary removePropertyTransition; NO bypass). Review round 2:
        // inline slots were previously exempt, but the tardy-access ALIASING
        // hazard (THREAD.md: a tardy read of deleted f must never alias a
        // newly added g) applies to inline slots identically - dictionary
        // structures mutate in place, so a stale reader's structure check
        // passes across delete(f)+add(g). Inline-slot atomicity only rules
        // out tearing, not aliasing. The D1 jsUndefined release-store and the
        // nextOffset() skip-past-quarantined accounting already handle inline
        // offsets.
        quarantineDeletedOffset(offset);
        return;
    }
```

If `quarantineDeletedOffset` (PropertyTable.cpp) carries an
`ASSERT(isOutOfLineOffset(offset))` (or equivalent), DELETE that assert and
update its comment to "inline and out-of-line offsets are both quarantined
(round 2)". No other change: releaseQuarantinedSlots, takeDeletedOffset,
propertyStorageSize and the clone paths are offset-kind-agnostic, and
nextOffset() already adds quarantinedDeletedOffsetCount() to the fresh
property number regardless of kind.

Test note for the owning party: extend i03-quarantine-readd-across-gc.js with
an INLINE-offset delete+readd leg (object with free inline capacity; delete a
property at an inline offset, re-add a different name, assert the new
property does NOT reuse the slot until a GC safepoint has passed).

### 55. Ownership adjudication restated (round-2 findings 9/10 - blockers by
### charter, no code defect alleged)

The §6 quarantine machinery physically lives in PropertyTable.h/.cpp, which
are OUTSIDE this part's owned-path globs but are marked "owned (GT#9)" by
SPEC-objectmodel.md - a brief/spec mismatch self-disclosed in §38 since
round 1. This part has made NO further PropertyTable.* writes in round 2 (the
§54 fix above is recorded as a ready-to-paste diff instead). The integrator
must pick, BEFORE the build-fix loop:

  (a) add `PropertyTable*` to the objectmodel ownership list in the run
      manifest (then this part applies §54 itself in the next round); or
  (b) re-assign the PropertyTable.h/.cpp diffs - including §54 - to the
      workstream that owns PropertyTable, with §38's description plus §54 as
      the spec of record.

Either way the files must not stay unowned: VM.cpp manifest entry 4c and the
two functions defined in owned ConcurrentButterfly.cpp
(`butterflyQuarantineEpochSlot` / `registerButterflyQuarantineEpochHook`,
declared in PropertyTable.h) form a cross-file contract that silently breaks
if PropertyTable.* is regenerated by a party unaware of it, taking the
i03-quarantine suite and I18/I30 soundness with it.

## From adversarial-review round 3 (fixes landed; PropertyTable escalation)

### 56. Round-3 fix ledger (what changed in owned files; for re-review)

All ten round-3 findings were verified REAL against the tree (none refuted).
Nine are fixed in owned files; the tenth (PropertyTable §54) is re-escalated
in §57.

1. JSArray in-place fast paths vs the §3 foreign-first-write protocol
   (blocker) + pushInline CAS-max regression (major) + SW=1 in-place
   shift/unshift (major):
   - JSArrayInlines.h pushInline: the flag-on prologue now routes
     segmented OR SW=1 OR foreign-keyed words through putIndexConcurrent
     (which carries F1/ensureSharedWriteBit, the CAS-max
     updatePublicLengthAfterDenseStoreConcurrent bump, AS cell-locking, and
     §4.4 growth). The flat in-place store + plain setPublicLength pair is
     now reachable only on owner-(currentTID, 0) words.
   - JSArray.cpp setLength / pop: foreign-SW=0 flat probes fire F1
     (ensureSharedWriteBit) and re-dispatch BEFORE the flat truncation/pop
     branches; the in-place truncation is then the §3 "owner or SW=1" store
     form (GC-safe per item 2 below).
   - JSArray.cpp shiftCountWithAnyIndexingType / unshiftCountWithAnyIndexingType
     (entry guards AND both post-ensureLength re-checks): the
     mayBeSegmentedButterfly()-only guards are now
     `segmented || SW=1 || foreign` and route to the ArrayStorage path
     (stop-routed conversion + cell-locked AS-COPY) - the in-place
     gcSafeMemmove is owner-(t,0)-only (I27).
   - JSArray.cpp fastShift: gained the same probe (it previously had NO
     flag-on guard at all - a segmented word would have been flat-decoded);
     bails to the generic path with the existing {} sentinel.
   - JSArray.cpp shiftCountWithArrayStorageConcurrent /
     unshiftCountWithArrayStorageConcurrent: pre-lock foreign-SW=0 probes run
     ensureSharedWriteBit's §4.6 per-event SW stop (I12/I10b) before the
     cell-locked relayout. pop/pushInline AS legs are covered by their
     functions' top-level probes/routing.
   - StructureInlines.h mayTransitionLockFreeFromThisStructure: AS-shape
     instances are now EXCLUDED from E4 (I31: every AS access/relayout is
     cell-locked; an E4 lock-free butterfly copy may never race a cell-locked
     AS-COPY). ACTION FOR THE JIT WORKSTREAM: SPEC-jit §5.5's emitted E4
     predicate mirrors this function and MUST add the same AS-shape
     exclusion (one indexingType test); recorded here because the jit spec
     pins the predicate shape.
2. GC value-visit bound vs lock-free truncation (blocker): the value-visit
   bound for SHARED words is now the STORAGE bound, not publicLength -
   JSObject.cpp visitElements bounds flat SW=1 Contiguous words by
   vectorLength; ConcurrentButterfly.cpp visitSegmentedButterfly bounds
   segmented Contiguous elements by the loaded spine's vectorLength (fragment
   0 slot 0 still skipped; C2 tail still unvisited). A dense store racing a
   truncating plain setPublicLength can no longer be hidden from the marker
   forever (the re-grey + revisit now scans it). Soundness of the wider
   bound: flag-on, every published flat butterfly and fragment is
   hole-initialized through its storage bound (creation hole-fills; T1/T2/
   conversion copies clear their slack; ObjectInitializationScope windows are
   thread-private and never SW=1). Truncation sites annotated
   (shrinkButterflyForSetLengthConcurrent both legs, JSArray::pop segmented
   leg).
3. Owner CoW materialization race (blocker): JSObject::convertFromCopyOnWrite
   flag-on no longer plain-nukes - it routes through new
   `materializeCopyOnWriteButterflyConcurrent` (ConcurrentButterfly.{h,cpp}),
   which loops the cell-locked §4.8 materializer until the object leaves the
   CoW regime. tryMaterializeCopyOnWriteButterflyForSharedWrite's F2 fire is
   now keyed on `butterflyWriterIsForeign(expectedWord)` (owner
   materializations fire nothing, per §5 F2 per-object keying), making it the
   single publication path for owner AND foreign materializations; its
   stability RELEASE_ASSERTs are now true statements (comment updated). The
   winner may be foreign: callers re-dispatch via the §3 probes (item 1).
4. createArrayStorage unrouted (blocker): JSObject::createArrayStorage
   flag-on routes to new JSObject::createArrayStorageConcurrent (declared
   JSObject.h, defined JSObject.cpp), a per-event-stop publication mirroring
   convertToArrayStorageConcurrent: plan + allocate + initialize the fresh
   flat AS butterfly outside the stop (O4), re-verify + F2-fire for shared
   triggers (butterfly-bearing keyed on the tag; butterfly-less keyed on the
   N1 structure transition TID) + copy out-of-line properties (flat AND
   segmented sources) + publish FLAT (currentButterflyTID(), shared?1:0) via
   the M8 fenced nuke order inside it. Covers ensureArrayStorageSlow's
   ALL_BLANK leg, ensureArrayStorageExistsAndEnterDictionaryIndexingMode,
   createInitialArrayStorage, and the putDirectIndexBeyondVectorLength blank
   legs (routing lives inside createArrayStorage). Racing indexed installs
   defer to ensureArrayStorageSlow on the settled state.
5. Dictionary / without-transition growth unrouted (blocker): both
   cell-locked add sites (putDirectInternal's dictionary branch and
   putDirectWithoutTransitionConcurrent, JSObjectInlines.h) now run
   `JSObject::classifyConcurrentLockedAdd` under the cell lock after the
   structureID re-validation. Non-Proceed regimes release the lock, run the
   matching protocol (`performConcurrentLockedAddSlowAction`:
   ensureSharedWriteBit / §4.2 conversion / new
   `ensureSegmentedOutOfLineCapacity` spine pre-grow /
   materializeCopyOnWriteButterflyConcurrent) and RESTART.
   putDirectWithoutTransitionConcurrent gained the §2 re-dispatch loop it
   lacked. The growth lambda is now
   `growOutOfLineStorageForConcurrentLockedAdd`: segmented => maxOffset bump
   only (coverage pre-grown; monotone across replacement spines); AS
   shared/foreign => nuke-bracketed copy published by a TAG-PRESERVING
   cell-locked casButterfly (AS-COPY form - no more owner-tag
   RELEASE_ASSERT trip on foreign dictionary growth); None / owner-(t,0)
   with writeThreadLocal verified valid => today's nuke-bracketed copy.
   The None leg is shielded from racing lock-free N3 indexed first-installs
   by a structureID-lane pre-nuke CAS taken before the table edit (the N3
   protocol nuke-CASes before its word CAS, so it loses cleanly and
   re-dispatches); the lane is restored after the edit if no growth restored
   it. Foreign value stores into dictionary storage now always have F1 fired
   first (FireSharedWriteBit classification). Residual noted for 7b: the
   unowned creation-path callers of addPropertyWithoutTransition
   (JSRawJSONObject.cpp:54, ClonedArguments.cpp:189) operate on thread-private
   objects pre-escape and were left on the legacy lambda.
6. storeTaggedButterflyWordConcurrent merge-and-retry (major): the CAS loop
   now folds ONLY the b1 shape (same payload, SW bit appeared); any
   payload-replacing CAS failure RELEASE_ASSERTs (callers must prove the word
   cannot move: E4 poll-free valid-set windows, or the §6 classification
   above). The b2 merge is gone.
7. ensureLength publicLength bump (major): JSObject.h ensureLength now uses
   bumpPublicLengthToAtLeast (CAS-max) for segmented words and SW=1 flat
   words; the plain store survives only for owner-exclusive (t,0) words.

New exported surface (all in owned files): ConcurrentButterfly.h/.cpp
`materializeCopyOnWriteButterflyConcurrent`,
`ensureSegmentedOutOfLineCapacity`; JSObject.h/JSObjectInlines.h
`classifyConcurrentLockedAdd`, `performConcurrentLockedAddSlowAction`,
`growOutOfLineStorageForConcurrentLockedAdd`, `createArrayStorageConcurrent`
(+ enum ConcurrentLockedAddSlowAction). No shared-file entries beyond those
already recorded; manifest entries 1-4c/7/7b unchanged.

TEST REQUESTS (for the i03 suite owner / next Task-12 pass; JSTests is outside
this round's write set):
- extend i03-cow-materialize-race.js with an OWNER-vs-foreign leg
  (owner a.push() vs foreign a[0]=v on a shared CoW literal - the exact
  round-3 scenario);
- extend i03-t5-racing-growers.js with a push-storm leg (two threads
  pushInline on one shared flat array; assert no element/length regression)
  and a shrink-vs-store leg (setLength truncation racing dense stores
  beyond the new length, then gc(): no crash, no dangling read);
- add an i03 leg for a shared DICTIONARY object: N threads adding distinct
  out-of-line properties (forces the §56.5 growth dispatch: conversion to
  segmented + ensureSegmentedOutOfLineCapacity) with a delete/re-add flavor;
- add an AS shift/unshift foreign-first-write leg (SW=0 AS word, foreign
  thread shifts: asserts writeThreadLocal fired + (installerTID,1) publish).

### 57. PropertyTable §54 - round-3 re-escalation (BLOCKER; unchanged in tree)

Re-verified this round: PropertyTable.h:569 still reads
`if (Options::useJSThreads() && isOutOfLineOffset(offset))` - the accepted
round-2 fix §54 (quarantine INLINE deleted offsets too) is STILL not applied,
and this part still may not write PropertyTable.* (outside the round-3
ownership list, per §38/§55). The inline delete/re-add aliasing hazard is
therefore LIVE. The integrator MUST adjudicate §55 (a) or (b) and apply §54
verbatim BEFORE the build-fix loop closes; the §54 test note (inline-offset
delete+readd leg for i03-quarantine-readd-across-gc.js) ships with it. Nothing
further can be done from inside this part's write set.

## From adversarial-review round 4 (fixes landed; escalations restated)

### 58. Round-4 fix ledger (what changed in owned files; for re-review)

All round-4 code findings were verified against the tree. Findings 1-5 and 7
are REAL and fixed in owned files; findings 6/8/9/10 are ownership/manifest
items handled in §§59-61 below. One sub-claim refuted with evidence (item 3).

1. JSArray.cpp appendMemcpy (BOTH overloads) + fastSlice flag-on dispatch
   (blocker): each now carries (a) an ENTRY probe - segmented word on either
   side => bail to the generic path; destination additionally requires the
   exclusive-owner tag (currentTID, SW=0) for the in-place stores (F1/I12,
   I21/I27); fastSlice also bails flag-on for AS-shape sources (I31:
   unlocked AS reads) - and (b) a POST-ensureLength re-probe in appendMemcpy
   (ensureLengthSlowConcurrent / forceSegmentedButterflies can legally leave
   the destination segmented mid-call). Every flat deref now goes through
   single-snapshot locals (`selfButterfly`/`otherButterfly`/
   `sourceButterfly`) derived from the SAME loaded words, with source reads
   bounded by the snapshot's own vectorLength. Bailing after ensureLength is
   safe: it only grew storage/publicLength and the generic append stores all
   remaining elements.

2. TOCTOU guard-then-butterfly()-reload (blocker): JSArray::pop, setLength
   (incl. its CoW-conversion re-load, which now RESTARTs the full dispatch),
   fastShift, shiftCountWithAnyIndexingType and (same class)
   unshiftCountWithAnyIndexingType (entry + both post-ensureLength re-checks)
   now derive their flat Butterfly* from the SAME word the regime guard
   dispatched on; any segmented word at the snapshot point RESTARTs the full
   dispatch. The butterfly() "dominating dispatch" contract is hereby
   NARROWED for owned fast paths: once a shape family's TTL sets are fired, a
   foreign §4.2 conversion needs only the cell lock + DCAS (no stop), so a
   re-load after a guard is NEVER sound - single-snapshot dispatch only
   (comment blocks at each site; JSArray::pop carries the canonical
   rationale). Additionally, every snapshot consumer now bounds element
   walks by the SNAPSHOT's vectorLength (flag-on min/bail legs in pop,
   setLength's clear loop, fastShift, shift/unshift hole walks): after a
   racing conversion, the ALIASED fragment-0 publicLength slot can be
   CAS-maxed past a superseded flat snapshot's storage, so trusting
   snapshot-read publicLength alone is an OOB - the prior RELEASE_ASSERT
   (length < vectorLength) in pop was a crash-from-race and is now a
   flag-on bail.

3. deletePropertyNamedConcurrent dictionary branch (blocker): offset +
   attributes are now RE-RESOLVED UNDER the cell lock (structure->get, which
   flag-on is the m_lock-held getConcurrently - lock order JSCellLock <
   m_lock per I20; getConcurrently never allocates, O1). Key gone =>
   configurable miss (no more RELEASE_ASSERT abort on racing same-key
   deletes); DontDelete re-checked under the lock; the D1 undefined store
   targets only the lock-resolved offset (closes the parked-lock /
   quarantine-promotion / slot-reuse clobber - I18). REFUTED sub-claim: the
   pre-lock get() was never a torn table read - flag-on Structure::get
   routes to getConcurrently which holds the owning structure's m_lock
   (StructureInlinesLight.h routing, §20); the defect was purely offset
   STALENESS across the cell-lock park. A comment at the site records this
   so the next round does not re-litigate it.

4. E5 "None first" in the quickly family + JSObject.h getters (major):
   canGetIndexQuicklyConcurrent / getIndexQuicklyConcurrent /
   tryGetIndexQuicklyConcurrent / trySetIndexQuicklyConcurrent flat dense
   legs now null-check the payload of the SAME loaded word and report
   not-quickly/undefined (racing lock-free N3 first install can pair a stale
   word==0 with a fresh indexed type). getArrayLength / getVectorLength /
   the canSetIndexQuicklyForPutDirect lambda dispatch None-first on the word
   (None reads as length/vector 0 / not-quickly) - chosen over the
   suggested loadLoadFence: acting on the word alone is fence-free and also
   covers the arm64 word-before-type reordering. get/tryGet flat legs are
   additionally bounded by the snapshot's vectorLength (same aliased-
   publicLength hazard as item 2). setIndexQuicklyConcurrent's flat leg is
   intentionally ASSERT-only: its contract requires a same-thread
   canSetIndexQuickly that dispatched on a non-null word, and same-address
   loads on one thread respect coherence (commented at the site).

5. Unpublished spine/fragment GC reclamation (blocker): DeferGC now spans
   every allocate-to-publication window: convertToSegmentedButterfly (per
   while-attempt), trySegmentedTransition (per while-attempt),
   ensureSegmentedOutOfLineCapacity (per attempt; the stale "callers hold
   DeferGC" comment is corrected), tryGrowSegmentedVectorLength (incl.
   across the mode-(b) per-event stop - the closure allocates nothing, O4,
   so no GC can be needed inside it). The step-1 "reachable only from this
   stack" comments are corrected: conservative scan pins a stack-referenced
   auxiliary CELL but never traces its contents, and Vector<,4> spills to
   fastMalloc - both made fragments sweepable mid-window. M4 NOTE for the
   integrator: tryGrowSegmentedVectorLength now holds DeferGC across the
   §10.6 veneer call; the real VMManager STWR swapped in at M4 must remain
   legal under a caller-held DeferGC (it performs no collection itself).

6. cellHeaderVolatileMask / setPerCellBit (major - option (a) adopted):
   TypeInfoPerCellBit's lane (bit 0x80 of header byte 6) is now part of
   cellHeaderVolatileMask (ConcurrentButterfly.h; static_assert updated), so
   every §3.0 header CAS/DCAS merge folds racing per-cell-bit flips instead
   of RELEASE_ASSERTing taxonomy (d) or silently undoing them.
   JSCell::setPerCellBit (JSCellInlines.h) flag-on is a byte-sized CAS loop
   (flag-off: today's plain RMW, I22). The three PA-tail
   mergeInlineTypeFlags flags-byte stores in ConcurrentButterfly.cpp are now
   CAS-merge loops for the same reason. JIT CONTRACT (record for the jit
   workstream / integrator - SPEC-jit §5.5 emitted DCAS sequences MUST
   match): the volatile lanes are now {byte 7 cellState, byte 4 bits 0xC0,
   byte 6 bit 0x80}; emitted header CAS loops must merge ALL THREE from the
   freshest read, exactly cellHeaderVolatileMask.

7. Round-4 sweep of the SAME blocker class in remaining owned JSArray.cpp
   fast paths (proactive; not individually filed but identical pattern to
   finding 1): fastFill, fastToReversed, fastWith, fastIncludes,
   fastCopyWithin, fastToSpliced, fillArgList, copyToArguments, and the
   fastFlat family (calculateFlattenedLength / fastFlatIntoBuffer / fastFlat
   - per NESTED-array level) now use a shared single-snapshot probe
   (`jsThreadsFlatSnapshot`, JSArray.cpp: segmented => bail; flag-on AS =>
   bail, I31; null word => bail, E5; in-place writers additionally require
   the exclusive-owner tag) and read within the snapshot's vectorLength.
   fillArgList/copyToArguments bail to their existing trailing get() loops
   (regime-safe generic). fastToString needed no change: its only direct
   butterfly() uses are on CoW words (I35 flat-decodable; fastArrayJoin is
   §44.2 integrator-guarded).

TEST REQUESTS (i03 suite owner / next Task-12 pass; JSTests outside this
round's write set):
- concat/slice legs: shared source array (SW=1 and segmented flavors) under
  arrayProtoFuncConcat/slice fast paths - asserts generic-path fallback, no
  crash, elements within {old, new} sets (covers item 1);
- a delete-storm leg for a shared DICTIONARY object: N threads delete the
  SAME key while another adds new properties across gc() (covers item 3 -
  previously an abort);
- a first-install probe storm: N reader threads spinning
  canGetIndexQuickly/length() on a plain shared object while one thread
  performs the first dense store (covers item 4);
- out-of-line capacity growth past 8 fragments (>4 fresh fragments, forcing
  the Vector heap spill) under --forceSegmentedButterflies
  --collectContinuously=1 (covers item 5, per the reviewer's suggestion).

### 59. PropertyTable §54/§55 - round-4 re-escalation (BLOCKER; STILL unchanged in tree)

Round-4 findings 6 and 8 re-verify what §§38/54/55/57 already record:
PropertyTable.h:569 still carries the `&& isOutOfLineOffset(offset)`
condition (inline deleted offsets bypass quarantine - LIVE aliasing hazard),
and PropertyTable.{h,cpp} remain outside every workstream's owned paths while
carrying the live cross-file contract
(`registerButterflyQuarantineEpochHook`/`butterflyQuarantineEpochSlot`,
declared PropertyTable.h:66-67, defined owned ConcurrentButterfly.cpp,
consumed by VM.cpp manifest entry 4c). Nothing changed this round - this part
made NO PropertyTable writes (write-isolation). The integrator MUST, before
the build-fix loop: adjudicate §55 (a) or (b), apply §54 verbatim (drop the
isOutOfLineOffset conjunct; delete any matching ASSERT in
quarantineDeletedOffset), and ship the §54 test note. This is the fourth
consecutive round carrying this item.

### 60. Cross-manifest carry item for INTEGRATE-jit (round-4 finding: E4
### AS-shape exclusion missing from the jit-side frozen predicate)

The round-3 fix §56.1 excluded `hasAnyArrayStorage` instances from E4
lock-free transitions in StructureInlines.h
(mayTransitionLockFreeFromThisStructure), but SPEC-jit.md §5.5 pins the
emitted predicate WITHOUT that test and INTEGRATE-jit does not carry it -
each part is conformant to its own spec while the combination races an
owner-tagged AS-shaped object's lock-free transition fast path against
cell-locked §4.6 AS-COPY publication (I31 - heap corruption). This part
cannot write SPEC-jit.md/INTEGRATE-jit.md; the integrator must append the
following to INTEGRATE-jit (ready-to-paste) BEFORE jit Task-8/9 transition
emission lands:

```
### (from objectmodel round 4) §5.5 emitted Transition-predicate amendment:
### AS-shape exclusion (I31)

The emitted lock-free transition fast-path predicate gains ONE term,
mirroring runtime/StructureInlines.h mayTransitionLockFreeFromThisStructure:

    eligible = transitionThreadLocal(S) valid+watched
            && writeThreadLocal(S) valid+watched
            && !object->isPreciseAllocation()
            && taggedButterflyWord tag == (currentTID, SW=0)   [as before]
            && !hasAnyArrayStorage(indexingMode)               [NEW - I31]

The indexing-mode byte is already loaded for the existing shape checks; the
new term is one TST/branch on the AS bits. Rationale: every AS access and
relayout is cell-locked (§4.6 AS-COPY); an E4 lock-free butterfly copy
(allocateMoreOutOfLineStorage copies the AS payload) must never race a
cell-locked AS relayout. Ineligible => the §9.5/§4.3 slow-path call, as for
any other E4 failure.
```

Record the application in INTEGRATE-jit so the two manifests agree; this
entry discharges objectmodel's side (the runtime predicate already enforces
it - StructureInlines.h, round 3).

### 61. SparseArrayValueMap.{h,cpp} - §51 upgraded to a READY-TO-PASTE diff
### (round-4 finding: §51 was prose-only and unassigned)

Ownership: SparseArrayValueMap.{h,cpp} is outside this part's paths and
claimed by NO workstream's manifest. The integrator must assign it (analogous
to §55) and apply the diff below verbatim. Scope correction first
(supersedes part of §40/§51's wording): SparseArrayValueMap::add / remove /
getConcurrently ALREADY lock the MAP's own cellLock() around the m_map
structural edits (SparseArrayValueMap.cpp:62-90,138-145), so "the internal
rehash runs unlocked" is no longer the precise defect. The LIVE defects are:

  (a) putEntry (SparseArrayValueMap.cpp:92-110) and putDirect (:112-136)
      hold `SparseArrayEntry& entry = result.iterator->value` ACROSS the
      release of add()'s internal lock and write through it
      (entry.put(...) / entry.forceSet(...)): a racing add() from another
      thread can REHASH m_map in between, leaving `entry` dangling -
      use-after-free write, flag-on with shared AS objects using sparse
      indices.
  (b) owned JSObject.cpp readers iterate the map under the OBJECT's cell
      lock (§40), which does not order against the MAP-lock edits; once (a)
      is fixed, readers ordering is closed by converting those owned walks
      to ALSO take the map's cellLock() (this part will do that in its next
      write window once (a)'s owner is assigned - the two-cell-lock nesting
      object-then-map needs the Task-10 CellLockDepthScope witness relaxed
      to depth 2 for exactly this pair, ordered object < map).

READY-TO-PASTE for SparseArrayValueMap.cpp (flag-gated; flag-off
byte-identical - I22):

In `SparseArrayValueMap::putEntry`, REPLACE the tail

```cpp
    RELEASE_AND_RETURN(scope, entry.put(globalObject, array, this, value, shouldThrow));
```

WITH:

```cpp
    if (Options::useJSThreads()) [[unlikely]] {
        // objectmodel round 4 (§61): `entry` dangles if a racing add()
        // rehashes m_map after add()'s internal lock released. Re-find under
        // the map's cell lock; do the plain-data store under it (no JS, no GC
        // allocation); extract the GetterSetter under it and call the setter
        // OUTSIDE it (it runs JS).
        JSValue getterSetter;
        {
            Locker locker { cellLock() };
            auto it = m_map.find(i);
            if (it == m_map.end())
                return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError); // racing remove
            SparseArrayEntry& lockedEntry = it->value;
            if (!(lockedEntry.attributes() & PropertyAttribute::Accessor)) {
                if (lockedEntry.attributes() & PropertyAttribute::ReadOnly)
                    return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
                lockedEntry.set(vm, this, value);
                return true;
            }
            getterSetter = lockedEntry.Base::get();
        }
        RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(getterSetter)->callSetter(globalObject, array, value, shouldThrow));
    }
    RELEASE_AND_RETURN(scope, entry.put(globalObject, array, this, value, shouldThrow));
```

(NOTE: `set(vm, this, value)` is `WriteBarrier<Unknown>::set` via
SparseArrayEntry's private base - if access control bites, add a
`setValueConcurrently(VM&, SparseArrayValueMap*, JSValue)` member next to
forceSet. The flag-off `entry.put` line stays for I22.)

In `SparseArrayValueMap::putDirect`, REPLACE the tail

```cpp
    if (entry.attributes() & PropertyAttribute::ReadOnly)
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

    entry.forceSet(vm, this, value, attributes);
    return true;
```

WITH:

```cpp
    if (Options::useJSThreads()) [[unlikely]] {
        Locker locker { cellLock() }; // objectmodel round 4 (§61): re-find; `entry` may dangle (racing rehash).
        auto it = m_map.find(i);
        if (it == m_map.end())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError); // racing remove
        SparseArrayEntry& lockedEntry = it->value;
        if (lockedEntry.attributes() & PropertyAttribute::ReadOnly)
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
        lockedEntry.forceSet(vm, this, value, attributes); // no JS, no GC allocation - lockable
        return true;
    }
    if (entry.attributes() & PropertyAttribute::ReadOnly)
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

    entry.forceSet(vm, this, value, attributes);
    return true;
```

Lock-rank note for the adopting workstream: the map's cellLock is taken with
NO other lock held in both legs above (putEntry/putDirect callers in owned
JSObject.cpp call them OUTSIDE the object's cell lock since round 1 - the
map-identity revalidation pattern, §40(i)); `m_reportedCapacity` accounting
stays inside add() as today. defineOwnIndexedProperty's long-lived
`entryInMap` across JS re-entry (§40 residual) is fixed by the same re-find
pattern at its two write-backs - that change is in owned JSObject.cpp and
this part applies it in its next write window, gated on this §61 diff
landing (the re-find is only sound once putEntry stops writing through stale
references itself).

## AB17c/AB17e F3 landing record (pointer)

The F3 family fixes (O2/GT11 transition-vs-write watchdog gates, S6 L3/L4
cacheable-dictionary staleness guard, I21 CAS-max) and their complete
flatten-site enumeration, per-fix named invariants, and honest test evidence
(including the 4/5 spawned-thread-butterfly-stress red gate and the
shared-arraystorage-stress oracle restoration) are ledgered in
docs/threads/INTEGRATE-ungil.md, section "AB17e", items 2, 3 and 6. The F3
family gate is OPEN pending 6/6 named tests at 5/5 GIL-off full JIT with the
restored exact-equality oracle.

## AB18-S2 landing record (sig-2: i03-n3-first-install-races flake)

Root cause of the ~1/20 GIL-off flake in
JSTests/threads/objectmodel/i03-n3-first-install-races.js (always "lost
first-install add t0/k ... got undefined", key ABSENT from the final
structure, all threads completed): a STALE-PARENT transition publication in
the §4.2 conversion, NOT the N3 indexed leg the round brief pointed at (the
indexed half of the test runs clean standalone, 0/150 amplified; the lost
adds are all in the NAMED-add half).

Interleaving (amplified evidence in the AB18-S2 round: immediate read-back
of the lost key already returns undefined; the final Object.keys lineage
contains every other thread's adds interleaved around the hole):

  1. owner thread t0 (instance tag (t0,0)) publishes S -> S_a (+p_t0) via a
     locked or E4 transition; the structureID lane now holds S_a.
  2. foreign thread t1 entered trySegmentedTransition(S, S_b=S+p_t1) BEFORE
     step 1 settled: its entry check (structureID == S) passed, the TTL sets
     of the cached cross-round structures are already chain-fired (F4), so
     step 0 fires nothing, and the §3 dispatch routes the flat foreign word
     to convertToSegmentedButterfly(S_b, offset, value).
  3. convertToSegmentedButterfly captured sourceID = structureID() at ITS
     OWN entry - which is now S_a, not S. Every later validation (step-3
     re-read under the cell lock) and the nuke-CAS check S_a - they all
     PASS - and the DCAS publishes newStructure = S_b, whose lineage is
     derived from S and LACKS p_t0. t0's add is silently erased (I21
     violation) even though t0's put already returned success.

The §4.2 contract ("RESTART: the structure changed before/under the lock")
was implemented against the WRONG baseline for transition triggers: the
conversion validated against whatever structure was current at its own
entry instead of the structure the published target was derived from.

Fix: convertToSegmentedButterfly now takes expectedSourceOrNull; a
transition trigger (newStructureOrNull != nullptr) RESTARTs (nullptr) unless
the object's structureID still equals the source the target was derived
from, and the unchanged step-3/nuke-CAS checks against that same sourceID
carry the property through publication. In-place (nullptr-structure)
callers pass nullptr and are unaffected. Sole transition-trigger call site:
trySegmentedTransition's §3 dispatch (passes its expectedSource). The
sibling protocols were audited and already carry the derived-from ID through
their CASes (trySegmentedTransition itself, tryStructureOnlyTransition, the
E4 lock-free leg, createInitialIndexedStorageConcurrent,
createArrayStorageConcurrent).

CORRECTION (AB18 verify round, AB18-S3): the sentence above overstated the
sibling audit. Carrying the derived-from ID through their OWN CASes was
necessary but NOT sufficient: two legs of
createInitialIndexedStorageConcurrent also acted as UN-EXCLUDED writers
against the LOCKED protocols' lane-ownership assumption ("under the cell
lock the semantic bytes are ours alone"), so they could land a nuke-CAS
inside tryStructureOnlyTransition's / trySegmentedTransition FirstInstall's
check->CAS windows and trip their fail-stop RELEASE_ASSERTs (crash-on-race,
sub-microsecond window; not exercised by i03-n3-first-install-races, whose
named and indexed halves use disjoint objects):
  1. the N3 lock-free leg ran with NO cell lock once the source TTL sets
     were fired (foreignButterflyLessInstall is false for owner AND foreign
     threads in the fired-sets regime) — "sets fired" excludes E4 elision
     races but not the cell-locked named protocols;
  2. the E4 owner fast path checked the TTL sets BEFORE
     Butterfly::createUninitialized (a poll site) and never revalidated them
     inside the AssertNoGC publication window — a foreign transitioner's
     step-0 set fire could land at that poll.
Fix (AB18-S3, JSObject.cpp): the N3 leg now revalidates
{structureID, word==0, owner TID, both source TTL sets} with FRESH loads
inside a poll-free AssertNoGC window and publishes lock-free only in the
owner+sets-valid case; the fired-sets case publishes under the CELL LOCK
(re-check under the lock; lost race => re-plan, never abort). The E4 owner
leg gained the I29 fresh-load revalidation inside its AssertNoGC block,
mirroring tryPutDirectTransitionConcurrent. The locked protocols' step-5 /
N2 exclusivity comments now name the indexed installers explicitly.
Regression test: JSTests/threads/objectmodel/i08-named-vs-indexed-first-install.js
(races named adds with indexed first-installs on the SAME shared object).
createArrayStorageConcurrent publishes only inside a §10.6 stop and needed
no change.

## AB17h pointer (flag-off transition-path audit + T4 cliff discharge)

The AB17h final-binary re-verification round is ledgered in
docs/threads/INTEGRATE-ungil.md, section "AB17h". Object-model-relevant
results recorded there: the flag-off transition-path codegen audit
(putDirectInternal constexpr split, static butterfly publish, gated DFG/FTL
emission, allocation-client byte test) with the JSCell::structure()
decontaminate() exoneration note; the T4 livelock cliff arithmetic
(transition chain crosses s_maxTransitionLength=128 at pass 129; 10x-PASSES
convergence on the final binary) including the CORRECTION that the no-JIT
immunity hypothesis ("flatten is IC-only") is refuted (LLInt flattens at
LLIntSlowPaths.cpp:981); and the corpus/load-harness re-runs on the rebuilt
binaries (GIL-on 93/0, GIL-off 92/0, Tools/threads/load6.sh).
