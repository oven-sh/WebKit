# AUDIT-upstream-since-rebase: upstream code added since the merge base

LANDING-PLAN section 1.3.

Scope: code that upstream added or changed between the merge base
`5851d4722e46` and `491b5cc236e9` (the oven-sh/WebKit main this branch is
rebased on, about 5,800 commits, `git diff 5851d4722e46..491b5cc236e9 --
Source/JavaScriptCore Source/WTF`), in the five classes the plan names:

1. butterfly loads and stores that bypass the tagged-butterfly helpers,
2. `switch` statements and per-case tables over node, cache, and type enums
   that the threads code extends,
3. heap walks (`HeapIterationScope`, `forEach*Cell`, `stopAllocating`, ...),
4. new caches and mutable fields on the VM, the global object, Structure, and
   cells,
5. new uses of Options and macros that the threads code changes.

A sixth class came up while doing class 1: upstream changes that the rebase
dropped (section 6).

Every row was checked against the code at HEAD (`d69451e8b3ba`). Line numbers
are HEAD line numbers. Nothing was built or run. Every finding comes from
reading the code, so "confirmed" means a concrete interleaving was traced by
reading, not observed.

Risk enum, as in the other AUDIT files: `confirmed-race` > `likely-race` >
`needs-trace` > `safe-*`. A row marked "no race needed" fails on one thread.

## Method

- Class 1: every upstream `+` line that names the butterfly (270 lines), every
  raw `butterflyOffset()` / `m_butterfly` load at HEAD compared with the
  pre-rebase branch (`3a14f2a821ac`), and every new file. Each site was read at
  HEAD and compared with the branch's idiom:
  - JIT: `CCallHelpers::loadButterflyForRead/ForWrite`, DFG `GetButterfly`
    and `emitThreadedButterflyLoadFor*`, FTL `threadedButterflyLoadFor*`,
    LLInt `threadedButterflyReadPredicate/WritePredicate`.
  - C++: load the tagged word once (`taggedButterflyWord()` +
    `isSegmentedButterfly()` + `untaggedButterfly()`, or
    `flatButterflySnapshot()`), bound loops by the snapshot's `vectorLength`,
    bail on shared or foreign words before an in-place move
    (`JSArray::fastShift`), and take the cell lock for every ArrayStorage
    access (I31).
- Classes 2 to 5: one reader per class, with the same rules. Each reported row
  that this file lists as `confirmed-race` was re-read at HEAD before it was
  written here. Rows that were not re-read say so.
- Class 6: upstream `+` lines that the branch's commits then removed, and that
  appear nowhere in the HEAD file. Each was read by hand.

## Summary table

| Row | Site (HEAD) | Upstream | Risk | Fix (short) |
|---|---|---|---|---|
| OM-1 | DFG `MultiGetByVal` / `MultiPutByVal` raw butterfly load | 8f6bc9a16adf | confirmed (no race needed) | use the DFG threaded load helpers, as the FTL does |
| OM-2 | `tryConcatMultipleArraysFast` | 485bcf1b176a | confirmed-race | bail flag-on, or snapshot once |
| OM-3 | `operationArrayShiftElements*` | 960adeccefcd | confirmed-race | copy `fastShift`'s word check |
| OM-4 | `getByValArrayStorageInt` (ArrayStorage, no lock) | 5dd686543431 | confirmed-race | take the cell lock, or go generic |
| OM-5 | `String.raw` fast path | 5031c9001ee2 | confirmed-race | snapshot the word; lock the AS arm |
| OM-6 | `FastStringifier` Contiguous array path | 299d0f154d7a | confirmed-race | snapshot the word |
| OM-7 | DFG `arrayJoinWithStringSeparator` | predates rebase | confirmed-race | same fix as `arrayProtoFuncJoin` |
| OM-8 | `directPutByVal` profile reads | 72e0ec4b2165 (widened) | likely-race (Debug assert) | use the dispatching length accessors |
| OM-9 | DFG inline `ArrayShift` stores | 960adeccefcd | needs-trace | same as the open DFG element-write gap |
| OM-10 | DFG/FTL Map and Set iteration inline | 7937fd75aea7, ce878e190fef | needs-trace | decide the N.1 ruling |
| SW-1 | async iterator metadata, plain accesses | 5f65bd95a4b6 | needs-trace (TSAN only) | use the `*Concurrently` helpers |
| SW-2 | new allocating nodes missing from the parkable list | several | needs-trace (older gap) | decide the rule once |
| HW-1 | thread exit uses `StopAllocatingMode::ForGood` | 6bdb4f69e23b | confirmed-race (by reading) | resumable stop for client teardown |
| HW-2 | `m_unlinkedBaselineCode` cleared with no lock | 450bcd468bb6 | confirmed-race (1.1 gap) | clear under `m_lock` |
| HW-3 | `MicrotaskCall::clear` | 75a9d414a4a8 | confirmed-race (1.1 gap) | `removeOnDestruction`, or the 1.1 stop |
| VM-1 | `syncResumeCallCache` is VM-wide | bafc1f2d7f1e | confirmed-race | pass `nullptr` GIL-off |
| VM-2 | `StringSplitCache` is ungated | 73e4c589c1e6 (widened) | confirmed-race | skip GIL-off |
| VM-3 | `vm.stringSplitIndice` is shared scratch | 73e4c589c1e6 (widened) | confirmed-race | local vector GIL-off |
| VM-4 | BigInt divisor cache | 2eb77e9c9473 (widened) | confirmed-race | skip GIL-off |
| VM-5 | `regExpGlobalData()` used directly | 73e4c589c1e6 | confirmed-race (one site) | `threadRegExpGlobalData` |
| VM-6 | `IntlCache` DateTimeFormat cache outside `m_lock` | 8d261de036d4 | confirmed-race | take `m_lock` |
| VM-7 | `localeCompare` collator cache | 33a5272cf9ac | confirmed-race | leaf lock, or skip GIL-off |
| VM-8 | `bun:ffi` context, arena, and UTF-8 cache | e6063b004fa4 | confirmed-race | refuse FFI on spawned threads GIL-off |
| VM-9 | `m_terminationDeadlines` lazy publish | 8a31c0d8f1a1 | likely-race (not re-read) | create eagerly |
| VM-10 | `UnlinkedMetadataTable` link and unlink | 024831d80fa0 | likely-race | leaf lock |
| VM-12 | cached-bytecode atom tables | 120b075b393a | needs-trace | compile lock in `atomForSlot` |
| VM-13 | `m_allowRedeclaringSymbols` | 7c9b3a28260f | likely-race, semantic | move to VMLite |
| VM-14 | `materializeLazyExport` | 7b763944f0ec | likely-race, semantic | CAS publish |
| VM-15 | `initAsGetterHit` has no fill gate | 6d70d346c664 | safe-covered (hygiene) | add the one-line check |
| OPT-1 | FFI JIT reads the VM-level exception word | e6063b004fa4 | confirmed (mode bug, not re-read) | per-thread load, or force FFI JIT off |
| OPT-2 | FFI JIT publishes the VM-level `topCallFrame` | e6063b004fa4 | likely (mode bug, not re-read) | mode-split publish |
| OPT-3 | `decommitUnusedMarkedBlockPages` from `sweep()` | e8bd7f041187 | needs-trace | force off under incremental sweep |
| RB-1 | `trySetIndexQuickly` lost `setMayStoreHole` | 72e0ec4b2165 | rebase loss (flag-off perf) | add three calls |
| RB-2 | `hasAnyEntryScopeServiceRequest` lost two checks | 8d261de036d4 | rebase loss (flag-off wrong result) | restore the checks |
| RB-3 | upstream `BadType` backoff gated on `useJSThreads` | 85309cfe91e8 | rebase loss (flag-off perf) | drop the gate |
| RB-4 | `Watchpoint.h` comment | cea233cedec0 | comment only | take upstream's text |
| PRE-1 | Map and Set are not locked at all | predates rebase | confirmed-race (by reading) | implement the N.1 ruling |
| PRE-2 | `appendInt32Array` check-then-reload | predates rebase | confirmed-race | see OM-6 |
| PRE-3 | DFG spread copy bound | predates rebase | likely-race | min with `vectorLength` |

## Status after the fixes

The rows were fixed in the working tree on 2026-09-02 (not committed). "Fails
before" is the failure count of the row's test on a build without the fix, in
the mode the row needs: GIL off unless it says otherwise. Every listed test
passes on the fixed build; the counts are in LANDING-PLAN.md, section 1.3.
Paths are under `JSTests/threads/`.

| Row | Status | Test | Fails before |
|---|---|---|---|
| OM-1 | Fixed. The DFG loads go through `emitThreadedButterflyLoadFor*`, and the slow cases exit with `BadIndexingType`, as in the FTL. | `jit/dfg-multi-by-val-spawned-butterfly.js` | 10 of 10, Debug and Release, both GIL modes |
| OM-2 | Fixed. Both passes use `flatButterflySnapshot`; the second pass bails on any change in size or type. | `objectmodel/concat-multiple-arrays-race.js` | 10 of 10 |
| OM-3 | Fixed. `butterflyForArrayShiftElements` makes `fastShift`'s checks on one load of the word. | `jit/dfg-array-shift-elements-race.js` | 10 of 10 |
| OM-4 | Fixed. The word is re-read under the array's cell lock, and the sparse map is read with `getEntry`, which takes the map's own lock. | `jit/dfg-get-by-val-array-storage-sparse-race.js` | 10 of 10, Debug only (Release has no detector) |
| OM-5 | Fixed. Same rule as OM-4: the array's lock for the word, the map's lock for the map. The first fix took only the array's lock and still crashed. | `objectmodel/string-raw-race.js` | 10 of 10 |
| OM-6, PRE-2 | Fixed. `flatButterflySnapshot` in both arms. | `objectmodel/json-stringify-array-race.js` | 10 of 10 |
| OM-7 | Fixed. `flatButterflySnapshot`, bounded by its `vectorLength`. | `jit/dfg-array-join-segmented-race.js` | 10 of 10 |
| OM-8 | Fixed. The flag-on arm uses `getVectorLength()` and `getArrayLength()`. | `objectmodel/direct-put-by-val-profile-race.js` | 10 of 10 |
| OM-9 | Fixed, with the older DFG and FTL element-write gap (second round, LANDING-PLAN.md "Results, second round"). A `GetButterfly` that feeds an element store (`PutByVal`, `PutByValDirect`, `PutByValDirectResolved`, `ArrayPush`, `ArrayUnshift`, `ArrayPop`, `ArrayShift`) runs the write predicate, so a foreign store to a word that is not shared-written exits, and the generic path sets the bit. Before, the owner's copying resize lost the store. | `arrays/dfg-foreign-element-store-sets-shared-write.js` | 20 of 20 (Release) |
| OM-10, PRE-1 | Fixed. Map and Set follow SPEC-ungil N.1: every mutation of the current table holds the table's cell lock, and a rehash or a clear publishes a new table. Sixth round: `has`/`get`/`size` are validated lock-free reads (owner seqlock; SPEC-ungil history), the other operations keep the lock. The DFG and FTL do not inline Map and Set operations with the GIL off, and iteration takes the generic path. | `shared-objects/map-set-shared-writers.js`, `shared-objects/map-lock-free-readers.js` | 10 of 10 |
| SW-1 | Fixed. The `seenModes` merges and reads use `mergeIterationModeSeenModesConcurrently` and `loadIterationModeSeenModesConcurrently`, and the value profile store uses `storeBucketConcurrently`, as the other iterator sites do. | none (TSAN, `semantics/async-generator-multithread-for-await.js`) | |
| SW-2 | Fixed as a rule instead of a list: with the GIL off, every node for which `doesGCIgnoringClobberize` is true clobbers heap facts. `CheckTraps` stays out, by design. | none | |
| HW-1 | Fixed. A thread's exit stops its allocators with `stopAllocatingForClientTeardown` (resumable). | `heap-client-exit-closure-reads.js` | 6 of 6 |
| HW-2 | Fixed. `deleteAllUnlinkedCodeBlocks` runs with the other threads stopped, and the field is cleared under `m_lock`. | `vmstate/heap-walk-while-threads-run.js` | |
| HW-3 | Fixed. `removeOnDestruction()` in a GIL-off process. | none | |
| VM-1 | Fixed. `VM::syncResumeCallCacheIfSingleMutator()` is null with the GIL off, at all nine sites. | `semantics/async-generator-multithread-for-await.js` | |
| VM-2, VM-3 | Fixed. The split cache is skipped with the GIL off, and each split uses its own index vector. | `vmstate/split-cache-per-thread.js` | 10 of 10 |
| VM-4 | Fixed. The divisor cache is skipped with the GIL off. | `vmstate/bigint-divisor-cache-per-thread.js` | 7 of 10 Debug, 10 of 10 Release |
| VM-5 | Fixed at both sites. | `vmstate/split-cache-regexp-statics.js` | 10 of 10 |
| VM-6 | Fixed. The DateTimeFormat cache stays empty in a GIL-off process. A lock is not enough: the cached impl is `RefCounted`, and cells on any thread ref it. | `vmstate/intl-datetimeformat-cache-per-thread.js` | 10 of 10 |
| VM-7 | Fixed. The collator is not cached with the GIL off. | `vmstate/intl-localecompare-cache-per-thread.js` | 7 of 10 Debug, 9 of 10 Release |
| VM-8, OPT-1, OPT-2 | Fixed by refusal. With the GIL off, the FFI IC stub, the DFG and FTL call paths, and the direct call are off (`Options.cpp`), and a spawned thread that creates or calls an FFI function gets a TypeError (`throwIfFFIRefusedOnCurrentThread`). The full fix (a per-thread arena and UTF-8 cache, a CAS publish of the context, and per-thread state in the JIT paths) is not done. | none (the jsc shell has no `bun:ffi`) | |
| VM-9 | Fixed. The publish: a GIL-off VM creates the table eagerly. The scope (second round): a deadline terminates only the thread that added it (`VMTraps::fireTargetedTermination`). The bit is set in that thread's word and in the VM word, which call-free loops poll, and the other threads ignore the VM word's bit while no VM-wide termination is raised. `VM::cancelTermination` keeps another thread's pending one. | `giloff-time-limit-terminates-one-thread.js` | 10 of 10 (Release) |
| VM-10 | Fixed by reading: a leaf lock in `link` and `unlink`. The test written for it never failed, so it was not kept. | none | |
| VM-11, RB-2 | Fixed. | none | |
| VM-12 | Fixed. A GIL-off VM creates the two-character atom table once, under a lock, and publishes it after a fence. Second round: `DecoderStringTable::atomForSlot`, which Bun's `JSC__IdentifierArray__setFromSlot` calls, takes the compilation lock. `atomFor` rewrites a slot with no lock of its own, and JSC's callers already hold that lock (it is recursive). | none (the jsc shell has no module-info path) | |
| VM-13, VM-14 | Open. Semantic only. | none | |
| VM-15 | Fixed. The one-line gate. | none | |
| OPT-3 | Safe by reading, once HW-1 is fixed: the decommit uses the same liveness as the sweep, and HW-1 was the case where that liveness was wrong. | none | |
| RB-1 | Fixed. The three calls are back, as upstream has them. | none | |
| RB-3 | Fixed. The gate is gone; the function matches `491b5cc236e9`. | `jit/dfg-for-of-closure-const-tier-up.js` | 10 of 10, flag off |
| RB-4 | Fixed. Upstream's text. | none | |
| PRE-3 | Fixed by reading. The test written for it never failed, so it was not kept. | none | |

Problems found while fixing these, outside the rows above (LANDING-PLAN.md,
section 1.1, lists them with their tests):

- `eval` used one callee per global object, and cleared its scope after each
  eval. With the GIL off, another thread's eval read the cleared scope.
- A direct eval, a module parse, a syntax check, a lazily created builtin, and
  the debugger's parse ran the parser without the compilation lock. The parser
  adds to a table on the VM.
- `Object.seal`, `Object.freeze`, `Object.preventExtensions`, a prototype
  change, becoming a prototype, the first indexed accessor, a private brand, a
  dictionary conversion, and `Object.assign` into an empty object stored the
  new structure with no check, and could undo another thread's delete or add.
  They now go through `JSObject::publishStructureOnlyTransitionConcurrently`.
- DFG OSR exit, and the LLInt OSR entry slow paths, walked a stale top call
  frame with the GIL off.
- A spin loop on a nuked StructureID was hoisted out of the loop by the
  compiler in Release (`JSCell::structureIDConcurrently`).

## 1. Butterfly access (class 1)

### OM-1: DFG `MultiGetByVal` and `MultiPutByVal` load the tagged word raw

**Risk: confirmed. No race is needed.**

- Sites: `dfg/DFGSpeculativeJIT64.cpp:9903` (`compileMultiGetByVal`,
  `handleJSArrayLoad`) and `:10158` (`compileMultiPutByVal`,
  `handleJSArrayStore`). Both do
  `loadPtr(Address(baseGPR, JSObject::butterflyOffset()), scratch2GPR)` and
  then index through it.
- Upstream: 8f6bc9a16adf added the DFG backend. The nodes were FTL-only before,
  and the FTL versions have threaded arms (`ftl/FTLLowerDFGToB3.cpp:7582`,
  `:8457`).
- Reachability: `DFGFixupPhase.cpp:1379` and `:1569` create the nodes with no
  threads check. `useThreadedDFG` is on by default (`OptionsList.h:741`).
- Failure: `JSObject.h:1621` tags every butterfly a spawned thread installs
  with that thread's ID. On that thread the raw word is a non-canonical
  address, and the first `Butterfly::offsetOfPublicLength()` load faults. On
  the main thread, an SW=1 word (bit 63) faults the same way, and a segmented
  word makes the code read the spine as element storage.
- The graph lint (`validateButterflyTagDisciplineForGraph`) does not see this,
  because the nodes have no storage edge.
- Fix: under `Options::useJSThreads()`, load through
  `emitThreadedButterflyLoadForRead` (get) and
  `emitThreadedButterflyLoadForWrite` (put) with
  `ConcurrentButterflyShape::KnownNonArrayStorage`, and route the returned
  jumps to `speculationCheck(BadIndexingType, ...)`, as the FTL does.
  Add a test that runs a polymorphic Int32/Double `a[i]` loop on a spawned
  thread until it tiers up to the DFG.

### OM-2: `tryConcatMultipleArraysFast` reads each source twice

**Risk: confirmed-race.**

- Site: `runtime/ArrayPrototype.cpp:1778`. The first pass sums
  `array->butterfly()->publicLength()` (`:1791`, `:1805`) and merges indexing
  types. The result is allocated from that sum (`:1830`). The second pass,
  `copySource` (`:1844`), reloads `butterfly()`, `publicLength()`, and
  `indexingType()`.
- Upstream: 485bcf1b176a.
- Interleavings:
  1. Thread B pushes to a source between the passes. `sourceSize` grows, and
     the copy writes past the new butterfly's `vectorLength`.
  2. Thread B converts a source from Int32 to Double between the passes. The
     copy then stores raw doubles into a Contiguous result, and the GC reads
     them as JSValues.
  3. Thread B segments a source. `butterfly()` then decodes the spine as
     storage.
- The two-array version, `tryConcatAppendArrayFastWithWatchpoints`
  (`:1670-1726`), handles all three: it snapshots each butterfly once, bounds
  the sizes by `vectorLength`, and re-reads the indexing types after the
  allocation.
- Fix: return `nullptr` at the top when `Options::useJSThreads()` is set (the
  generic concat handles it). If that is too slow, snapshot each source's
  butterfly, size, and type once into a local vector, and re-check the types
  after the allocation, as the two-array version does.

### OM-3: `operationArrayShiftElements*` move shared storage in place

**Risk: confirmed-race.**

- Sites: `dfg/DFGOperations.cpp:1410` (Int32), `:1433` (Contiguous), and
  `:1456` (Double). Each calls `array->butterfly()`, then does a `memmove`,
  a slot clear, and `setPublicLength`.
- Upstream: 960adeccefcd.
- Callers: the DFG `ArrayShift` node (`DFGSpeculativeJIT64.cpp:4851`) and the
  FTL one (`FTLLowerDFGToB3.cpp:9881-9886`). Both call it after `GetButterfly`
  proved the word flat at an earlier point.
- The branch's `JSArray::fastShift` (`runtime/JSArray.cpp:1887-1911`) does
  the same move, and it returns early for segmented, shared-write, or foreign
  words. Its comment gives the reasons. The new operations copy fastShift's
  body without that check.
- Interleavings:
  1. Thread B segments the array between `GetButterfly` and the call. A
     foreign conversion needs only the cell lock once the thread-local sets
     have fired. `butterfly()` then decodes the spine, and the `memmove`
     writes over it.
  2. The word is flat with SW=1, and thread B stores to the array. The
     in-place move loses B's store (I27).
- Fix: at the top of each operation, under `Options::useJSThreads()`, load the
  tagged word once and return `JSValue()` if it is segmented, shared-write, or
  foreign, exactly as `fastShift` does. The DFG caller already treats the empty
  value as "take the generic path". Use the same word for the flat view, and
  also return `JSValue()` when `length > butterfly->vectorLength()`.

### OM-4: `getByValArrayStorageInt` reads ArrayStorage without the cell lock

**Risk: confirmed-race.**

- Site: `dfg/DFGOperations.cpp:896-928`. It reads `m_sparseMap`, `m_vector`,
  and `map->find(i)` with no lock.
- Upstream: 5dd686543431. Callers: `DFGSpeculativeJIT64.cpp:3274` and
  `FTLLowerDFGToB3.cpp:7414`.
- Rule: I31 says that with the flag on, every runtime ArrayStorage access is
  cell-locked, reads included. See `JSObject.cpp:805-815`, which locks and
  re-loads the word.
- Interleaving: thread B does `a[1e6] = x` on the same array. That takes the
  cell lock and adds to the sparse map, which can rehash it. Thread A's
  `map->find` walks the old table, which is freed.
- Fix: under `Options::useJSThreads()`, return
  `getByValCellInt(globalObject, vm, base, index)` (the generic path, which
  locks). If the fast path must stay, take `Locker locker { base->cellLock() }`
  and re-load the word under it, as `JSObject.cpp:808` does. Do not call
  `get` while holding the lock.

### OM-5: `String.raw` fast path

**Risk: confirmed-race.**

- Site: `runtime/StringConstructor.cpp:206-250`.
- Upstream: 5031c9001ee2 (String.raw in C++).
- Two problems:
  1. `rawArray->butterfly()` runs before the switch on `indexingType()`
     (`:206`). For the Int32, Double, and Contiguous arms, a segmented word
     decodes the spine as storage. This is the same as OM-3, interleaving 1.
  2. The ArrayStorage arm (`:227-250`) reads `m_vector` and the sparse map
     with no cell lock. This is the same as OM-4.
- Fix: under `Options::useJSThreads()`, use `flatButterflySnapshot` for the
  dense arms, and read the indexing type before the word. For the ArrayStorage
  arm, either take the cell lock or fall to the generic `raw->get(...)` path.
  The bound `index < publicLength()` must also be `< vectorLength()` of the
  same snapshot.

### OM-6: `FastStringifier` Contiguous path

**Risk: confirmed-race.**

- Site: `runtime/JSONObject.cpp:1730`. It calls `array.butterfly()` for a
  Contiguous array with no regime check.
- Upstream: 299d0f154d7a. Before the rebase, only the Int32 path existed. It
  is in `appendInt32Array` and is guarded.
- Interleaving: the same as OM-3, interleaving 1, on the array being
  stringified.
- Also: the Int32 guard at `:1800-1804` is
  `mayBeSegmentedButterfly()` followed by `butterfly()`. `JSObject.h:855-859`
  says that is not a valid witness. That code predates the rebase, but it
  should get the same fix.
- Fix: in both places, use `flatButterflySnapshot(&array, butterfly)` and call
  `recordFailure` when it returns false. The existing
  `length > vectorLength()` check then runs on the snapshot.

### OM-7: DFG `arrayJoinWithStringSeparator`

**Risk: confirmed-race. Predates the rebase.**

- Site: `dfg/DFGOperations.cpp:1768-1776`. It calls `array->butterfly()` and
  joins `array->length()` elements from it.
- Upstream: a4df93500a72 only changed the `tryJoin` template. The pre-rebase
  branch has the same code.
- The branch already fixed the same code in `arrayProtoFuncJoin`
  (`runtime/ArrayPrototype.cpp:444-447`). That fix uses
  `flatButterflySnapshot` and bounds `length` by `vectorLength()`.
- Fix: copy those two lines here.

### OM-8: `directPutByVal` profile reads

**Risk: likely-race. It fires a Debug assertion. In Release, the profile
gets a wrong value.**

- Site: `jit/JITOperations.cpp:1714-1715`. It reads
  `baseObject->butterfly()->vectorLength()` and `publicLength()` for a dense
  array. It also reads the ArrayStorage vector at `:1724-1726` with no lock.
- Upstream: 72e0ec4b2165 added the `publicLength()` read. The `vectorLength()`
  read predates the rebase.
- Effect: on a segmented word, `butterfly()` asserts in Debug
  (`JSObject.h:865`), and in Release it reads the spine header as a length.
  Only `arrayProfile` flags depend on it. The ArrayStorage read is a single
  slot from storage that stays allocated, so it is benign.
- Fix: use `getVectorLength()` and `getArrayLength()`, which dispatch on the
  word (`JSObject.h:258-296`).

### OM-9: DFG inline `ArrayShift` stores through the storage edge

**Risk: needs-trace. This is the same as an open, older gap.**

- Site: `dfg/DFGSpeculativeJIT64.cpp:4824-4840`. The length-1 case loads
  element 0, clears it, and stores `publicLength = 0` through `GetButterfly`'s
  masked result. It runs no write predicate.
- Upstream: 960adeccefcd.
- This copies `ArrayPop` (`:4714-4720`), which has the same shape.
  `docs/threads/INTEGRATE-jit.md` (Task 9, known gap 1) lists DFG element
  writes with no per-store TID compare as an open item. A foreign store then
  does not set SW.
- Fix: none for this node alone. When the gap is closed for `ArrayPop`, close
  it here too. The fix is the same: re-load the word and run
  `emitThreadedButterflyLoadForWrite` at the store.
- Status (second round): fixed for every element store that takes a
  `GetButterfly` storage child, in the DFG and the FTL. The write predicate
  runs at the `GetButterfly` instead of at each store
  (`Graph::markButterflyLoadsThatFeedElementWrites`). That is as strong as a
  check at the store: no poll sits between the two, because a poll clobbers
  `JSObject_butterfly` and the load runs again after it. The gap was not only
  about the bit. The owner's resize copies the elements and publishes the copy
  with a CAS that expects the word it read, and the CAS succeeds when the bit
  is still clear, so a foreign store to the old butterfly was lost.

### OM-10: DFG/FTL Map and Set iteration reads storage with no lock

**Risk: needs-trace.**

- Sites: `dfg/DFGSpeculativeJIT64.cpp:9332` (`compileMapIteratorNext`), and
  `DFGSpeculativeJIT.cpp:15579-15690` (`MapIterationNext`, `MapIterationEntry*`).
  The FTL has the same nodes at `FTLLowerDFGToB3.cpp:17107-17367`.
- Upstream: 7937fd75aea7 and ce878e190fef.
- `SPEC-ungil-audit-N7.md` row R1 and `UNGIL-HANDOUT.md` say DFG/FTL Map
  intrinsics are disabled when the GIL is off. I found no code at HEAD that
  does this. `DFGByteCodeParser.cpp:4525` (`JSMapGetIntrinsic`) and `:4789`
  (`JSMapIteratorNextIntrinsic`) have no threads check, and neither does
  Options. The older `MapGet` inline code has the same gap, so this predates
  the rebase.
- Why this is needs-trace and not a race: Map storage is a GC cell
  (`JSCellButterfly`). A rehash makes a new one and marks the old one obsolete.
  The inline code checks that mark. A reader that holds an old storage in a
  register keeps it alive through the conservative scan. So the reads look
  memory-safe. A race might still skip or repeat an entry. I did not trace the
  in-place delete and append writes.
- Fix: decide whether the N.1 ruling still holds. If it does, add the missing
  gate in `handleIntrinsicCall`, for the Map, Set, and iterator intrinsics.
  If it does not, correct N7 row R1.

### Checked and safe (class 1)

- LLInt: HEAD has 14 raw `m_butterfly` loads. The pre-rebase branch also has
  14, so upstream added none. The new `op_async_iterator_open` and `_next`
  reuse `iteratorOpenGenericImpl` and the call helpers.
- Baseline JIT and IC files (`JITPropertyAccess.cpp`,
  `JITInlineCacheGenerator.cpp`, `AssemblyHelpers.*`, `InlineCacheCompiler.cpp`
  and others): the raw-load count matches the pre-rebase branch. Upstream's one
  new IC load (`InlineCacheCompiler.cpp`, upstream line 3457) is now
  `loadButterflyForRead` at HEAD `:3503`.
- `DFGSpeculativeJIT.cpp:9371`, `:9384`: the new raw loads are in the flag-off
  arm. The flag-on arm loads the word once, which fixes a double load in the
  pre-rebase code. The copy loop bounds itself by `publicLength`, not by the
  snapshot's `vectorLength`. That predates the rebase and is not upstream code.
- Reloads of a new array's butterfly (`DFGSpeculativeJIT.cpp:9250`, `:9769`,
  `:10057`) are all masked.
- New allocation sites (`compileNewButterflyWithSize`,
  `compileNewArrayWithButterfly`, FTL `allocateButterfly`,
  the result butterflies in `ArrayConstructor.cpp:584-690` and
  `ArrayPrototype.cpp:1838`, `JSFFIFunction.cpp:138`, `LiteralParser.cpp:1595`):
  each installs a butterfly on an object no other thread can see yet. (The
  `ArrayConstructor.cpp` functions read Set storage with no lock. See PRE-1.) The JIT ones follow the
  FRESH-ALLOC carve-out in INTEGRATE-jit.
- `ArrayIncludes` and `IndexOf` atom-structure checks (`DFGSpeculativeJIT.cpp:9925`,
  `FTLLowerDFGToB3.cpp:8823`): they read through `GetButterfly`'s masked
  storage, and only a CopyOnWrite word reaches them (I35).
- `arrayProtoFuncJoin` (`ArrayPrototype.cpp:444`), the two-array concat, and
  `tryCreateObjectClone` (`ObjectConstructorInlines.h:300-315`): already
  converted by the rebase commits.
- `JSObject::*IndexQuickly`, `ensureLength`, `tryMakeWritable*`: upstream moved
  these to `JSObjectInlines.h`. The branch kept its threaded copies in
  `JSObject.h`. The bodies match upstream, except for one lost change (RB-1).
- `JSObject::crashDueToEmptyValueAtValidOffset` (`JSObject.cpp:4620-4670`): a
  crash-only diagnostic. It calls `butterfly()` on the object it is about to
  report, on a `NO_RETURN` path.
- `CachedTypes.cpp` immutable butterfly encoding, `StringSplitCache`
  `JSCellButterfly*` values, `RegExpPrototype.cpp:807-841`: these are
  `JSCellButterfly` cells, not tagged words. See class 4 for the cache.
- `jsc.cpp` and `JSDollarVM.cpp`: test-only hooks.

## 2. Per-case tables and switches (class 2)

The branch adds no enum values. It does keep per-case rules that new upstream
values must also follow. The rules found at HEAD:

- `DFGClobberize.h:64-176`, `jsThreadsParkableSlowPathClobbersHeapFacts`: a
  node that can park at a safepoint and is modeled more precisely than
  `clobberTop` must be in this list.
- `DFGSpeculativeJIT.cpp:2408`, the storage lint: the producers and consumers
  of butterfly storage.
- DFG and FTL threaded arms, one per node that loads a butterfly.
- `InlineCacheCompiler.cpp`: each AccessCase that loads a butterfly uses
  `loadButterflyForRead/ForWrite`. Megamorphic cases are refused GIL-off
  (`:4877`).
- LLInt and slow paths: metadata caches are read and written through relaxed
  atomics (`CommonSlowPaths.h:63-72`, `GetByStatus` `loadModeConcurrently`).

New upstream values: the DFG nodes RegExpExecSticky, RegExpSplitFast,
RegExpStringIteratorNext, StringIteratorNextWithUndefined, CallFFI,
OpenAsyncFromSyncIterator, EnqueueAsyncGeneratorDriver, StringTrim,
BufferReadInt, BufferReadFloat, and BufferWrite; the AccessCase
LoadMegamorphicGetter; and the bytecodes `async_iterator_open` and
`async_iterator_next`. No JSType or IndexingType value was added.

Rows in other sections that belong to this class as well: OM-1 (the new DFG
lowering of `MultiGetByVal` and `MultiPutByVal`; upstream changed the fixup gate
from `isFTL()` to `is64Bit()`), OM-3 (the `ArrayShift` array-mode switch),
OM-5 (the `String.raw` indexing-type switch), OPT-1 and OPT-2 (the `CallFFI`
lowering), and VM-15 (`LoadMegamorphicGetter`).

### SW-1: async iterator metadata is read and written with plain accesses

**Risk: needs-trace. A C++ data race with no memory effect found. Not re-read.**

- Sites: `runtime/CommonSlowPaths.cpp:1001`, `:1028`, `:1035`, `:1042`;
  `llint/LLIntSlowPaths.cpp:2243`; `dfg/DFGByteCodeParser.cpp:13277`, `:13574`;
  `bytecode/GetByStatus.cpp:129`, `:131`.
- Upstream: 5f65bd95a4b6.
- The sync `iterator_open` and `iterator_next` siblings use
  `loadIterationModeSeenModesConcurrently` and
  `mergeIterationModeSeenModesConcurrently`, and `GetByStatus` uses
  `loadModeConcurrently` and `loadStructureID`. The new async sites do not.
- The values are single words, and the merge is already lossy, so this is a
  TSAN report, not corruption.
- Fix: use the same helpers at these nine sites.

### SW-2: new allocating nodes are not in the parkable-slow-path list

**Risk: needs-trace. This gap is older and wider than the new nodes.**

- Sites: `DFGClobberize.h:64-176` (the list), and the new nodes StringTrim
  (`:3052`), StringIteratorNextWithUndefined (`:2770`), and RegExpExecSticky
  (`:2719`).
- The list's comment says a GC-allocating node with a precise model must be
  listed. These three nodes allocate and are not listed. Neither are their
  older siblings, such as StringSlice, ToLowerCase, StringIteratorNext, and
  RegExpMatchFast. So this is one older gap, not three new misses.
  RegExpExecSticky is not created GIL-off (see VM-5).
- Fix: decide the rule once for pure string allocators and for
  RegExpState-modeled allocators. Then add StringTrim and
  StringIteratorNextWithUndefined together with their siblings.

### Checked and safe (class 2)

- CallFFI, OpenAsyncFromSyncIterator, EnqueueAsyncGeneratorDriver,
  RegExpSplitFast, and RegExpStringIteratorNext are `clobberTop`, so the
  parkable list does not need them.
- BufferReadInt, BufferReadFloat, and BufferWrite are modeled like
  DataViewGet and DataViewSet. They do not allocate. Their storage comes from
  `GetIndexedPropertyStorage`, which the storage lint accepts.
- MapIteratorNext changed flags. Its operation (`DFGOperations.cpp:6246`) does
  not allocate.
- `DFGMayExit.cpp` keys only on GetButterfly, PutByOffset, and PutByVal.
- LoadMegamorphicGetter: the case is refused GIL-off, and the JIT probe bails
  under `useJSThreads` (`AssemblyHelpers.cpp:677`).
- FTL IC emission: all 13 property IC patchpoints use the handler-IC and
  data-IC paths. This covers the known miss that the plan lists.
- `async_iterator_open` and `async_iterator_next` in the LLInt use the shared
  macros, which have threaded arms. The C++ slow path uses
  `performLLIntGetByID`.
- `EnqueueAsyncGeneratorDriver` goes through `enqueueGILOff`, which is the
  4fa55c249e62 protocol.
- New switches read: the four moved `JSObject` indexing-type switches, the DFG
  Buffer result-type switch, `vectorLengthHint`, and the StringSlice
  slow-path switch.

Not checked: Baseline JIT emission for the async iterator opcodes (the branch
has no rule in `JITCall.cpp` to compare against), and the full 886-line
upstream `InlineCacheCompiler` refactor beyond the megamorphic gate, the raw
loads, and LoadMegamorphicGetter.

## 3. Heap walks (class 3)

Upstream added no new callers to the three heap walks that LANDING-PLAN 1.1
tracks (`MarkedSpace::size`, `Heap::deleteAllCodeBlocks`,
`Heap::globalObjectCount`). It did add new work inside `VM::deleteAllCode`,
which HW-2 and HW-3 describe, and it changed how an allocator stops (HW-1).

### HW-1: a spawned thread's exit leaves its last allocations unprotected

**Risk: confirmed-race, by reading. Not run.**

- Sites: `heap/MarkedBlock.cpp:162-175` (the `ForGood` branch of
  `Handle::stopAllocating`), reached from `heap/LocalAllocator.cpp:168-183`,
  `heap/GCThreadLocalCache.cpp:352`, and `runtime/ThreadManager.cpp:662`
  (`client->lastChanceToFinalize()`).
- Upstream: 6bdb4f69e23b added `StopAllocatingMode::ForGood` and made
  `LocalAllocator::stopAllocatingForGood` use it. The `ForGood` branch skips
  the newly-allocated bitmap. Its comment says this is safe because
  `MarkedSpace::lastChanceToFinalize()` runs next.
- The branch also calls `stopAllocatingForGood` when a spawned thread exits
  GIL-off, or when a client heap is destroyed, and the heap keeps running.
  Before the rebase, that path used the resumable `stopAllocating()`, which
  sets the bitmap (`3a14f2a821ac:heap/LocalAllocator.cpp:168`).
- Result: in the exiting thread's current block, cells allocated since the last
  GC have no mark bit, a stale newly-allocated version, and no allocated bit.
  `Handle::isLive` (`heap/MarkedBlockInlines.h:107`) returns false for them.
- Interleaving: thread T allocates `o`, stores it into a shared array, and
  exits. Thread U takes `o` out of the array and holds it only in a local.
  At the next GC, the conservative scan asks `isLiveCell(o)`, gets false, and
  does not mark it. The sweep frees `o` while U still uses it. An object that
  the heap still references is marked by the normal trace, so only a
  stack-only reference is lost.
- Fix: in `GCThreadLocalCache::stopAllocatingForGood` (and in any other
  client-teardown caller), call the resumable `stopAllocating()`, then
  `reset()`. Keep `ForGood` for the server's `MarkedSpace::lastChanceToFinalize`
  only. Flag-off nothing changes, because the thread-local cache is empty.
  Add a test: a thread allocates objects, hands them out through a shared
  array, and exits. The main thread moves them to locals, clears the array,
  forces a GC, and reads them.

### HW-2: `deleteAllUnlinkedCodeBlocks` clears `m_unlinkedBaselineCode` with no lock

**Risk: confirmed-race. This is part of the LANDING-PLAN 1.1 gap.**

- Site: `heap/Heap.cpp:1610-1613`. It sets
  `m_unlinkedBaselineCode = nullptr` with no lock.
- Upstream: 450bcd468bb6. Two other clears (f2056eb76a23, at `Heap.cpp:1645`
  and `CodeBlock.cpp:2623`) run at GC end with mutators stopped, so they are
  safe.
- The branch's contract, at `bytecode/UnlinkedCodeBlock.h:424-431`, says the
  field is "never replaced or cleared" once installed. Readers copy it under
  `m_lock` through `unlinkedBaselineCodeConcurrently()`
  (`LLIntSlowPaths.cpp:455`, `ScriptExecutable.cpp:589`, `JITWorklist.cpp:210`).
- Interleaving: `VM::deleteAllCode` runs this while another thread copies the
  RefPtr. Both touch the ref count, and the copy can use freed memory.
- Fix: add `UnlinkedCodeBlock::clearUnlinkedBaselineCode()`, which clears under
  `ConcurrentJSLocker(m_lock)`, and use it at all three sites. Update the
  contract comment. The stop-the-world decision for 1.1 also covers this site.

### HW-3: `MicrotaskCall::clear` from `deleteAllCodeBlocks`

**Risk: confirmed-race. This is part of the LANDING-PLAN 1.1 gap.**

- Site: `interpreter/MicrotaskCall.cpp:90-98`. It calls the unlocked
  `isOnList()` / `remove()` pair, then nulls `m_codeBlock` and
  `m_addressForCall` with plain stores.
- Caller: `Heap.cpp:1563`, through `VM::clearMicrotaskCallCaches`.
- Upstream: 75a9d414a4a8.
- The branch replaced that pair with the locked `removeOnDestruction()` in
  `relink` (`MicrotaskCall.cpp:58`, `:72`), but `clear` was missed.
- Interleaving: a thread in `MicrotaskCall::tryCallWithArguments` loads the
  null `m_codeBlock` and dereferences it (`MicrotaskCallInlines.h:89`).
- Fix: the stop-the-world decision for 1.1 covers this. If that decision does
  not stop the world here, make `clear()` use `removeOnDestruction()` and
  atomic stores, as `relink` does.

### Checked and safe (class 3)

- `Heap::releaseUnusedSharedBaselineCode` (`Heap.cpp:1636-1653`): it runs at
  the end of a full collection, with all clients stopped.
- The old-age jettison clear (`CodeBlock.cpp:2620-2623`): collector context.
  I did not trace every OldAge caller.
- `Heap::forEachProtectedCell`, `StrongSet::forEachStrongHandle`: both take the
  GIL-off leaf locks.
- `WeakSet::reap`: collector only.
- `MarkedSpace::lastChanceToFinalize` with `ForGood`: server teardown, under
  MSPL, after clients detach. The client path is HW-1.
- `MarkedBlock::Handle::stopAllocating` bitmap rewrite (f198e8af3b2a): under
  the block header lock.
- `BlockDirectory::isFreeListedCell` and the block-set reads in
  `JSObject::crashDueToEmptyValueAtValidOffset`: crash-only diagnostics.
- `BunV8HeapSnapshotBuilder::json`: it captures inside `collectNow(Full)`,
  like `HeapSnapshotBuilder`.

Not checked: `m_regExpAllocator.releaseRetainedPools()` in `VM::deleteAllCode`
(its comment says nothing is matching while idle, which is false GIL-off), and
`~CodeBlock`'s `m_ownerWentAwayAt` write.

## 4. Caches and mutable fields (class 4)

The rows marked "verified" were re-read at HEAD for this file. The others come
from one reader's trace, and this file did not re-read them.

### VM-1: `MicrotaskCallCache` for async generator resume is VM-wide

**Risk: confirmed-race. Verified.**

- Site: `runtime/VM.h:1543-1544` (`m_syncResumeCallCache`). Used at
  `runtime/JSMicrotask.cpp:101-106` (`find`, then `tryCallWithArguments`) and
  `:150-154` (`find`, `nextEntryToReplace`, `initialize`).
- Upstream: bafc1f2d7f1e.
- The cache has no lock. Commit 4fa55c249e62 serializes requests per generator,
  so two different generators on two threads still share the cache. The drain
  path's cache is on the stack, so it is per-thread already.
- Interleaving: thread A finds entry E for function X. Thread B misses and
  picks E to replace, then calls `initialize(vm, Y)`, which rewrites
  `m_codeBlock` and `m_addressForCall`. Thread A then enters Y's code with X as
  the callee.
- Fix: under `vm.gilOffWithProcessGate()`, pass `nullptr` for the cache in the
  C++ entry points (`asyncGeneratorNext`, `asyncIteratorNextWithDriver`,
  `enqueueAsyncGeneratorDriver`, `asyncGeneratorResume`). `callMicrotask`
  already handles null. The JIT-baked pointer is then ignored, and flag-off
  codegen does not change. This should also be raised with the 1.4 review.

### VM-2: `StringSplitCache` is VM-wide and ungated

**Risk: confirmed-race. Verified. Predates the rebase, and upstream widened it.**

- Site: `runtime/VM.h:1538-1540`. Used at `StringPrototype.cpp:1069`, `:1126`,
  and `:1187`, and at `RegExpPrototype.cpp:861` and `:886`.
- Upstream: 73e4c589c1e6 made the cache lazy (`LazyUniqueRef`) and added the
  RegExp arm. The String arm was already ungated at `3a14f2a821ac`.
- Entries hold `RefPtr<AtomStringImpl>` and are written with plain stores. Two
  threads that split at the same time race the ref count, and can pair one
  thread's key with the other thread's result. The first touch of a
  `LazyUniqueRef` can also race.
- Model: `HasOwnPropertyCache`, right above it, is skipped GIL-off
  (`ObjectPrototype.cpp:108`, K4 row II.18).
- Fix: under `vm.gilOffWithProcessGate()`, skip both `stringSplitCache()`
  lookups and all three `ensureStringSplitCache()` fills.

### VM-3: `vm.stringSplitIndice` is one shared scratch vector

**Risk: confirmed-race. Verified. Predates the rebase, and upstream added a
second writer.**

- Site: `runtime/VM.h:1172`. Used at `StringPrototype.cpp:1076` and
  `RegExpPrototype.cpp:995`.
- Upstream: 73e4c589c1e6 added the RegExp writer.
- Two splits at the same time both `shrink(0)` and append to the same vector.
  An append that reallocates frees the buffer the other thread is indexing.
- Fix: under `gilOffWithProcessGate()`, use a local `Vector<unsigned, 256>` in
  each split, or a per-thread vector.

### VM-4: BigInt divisor cache

**Risk: confirmed-race. Verified. Predates the rebase, and upstream added a
field.**

- Site: `runtime/JSBigInt.cpp:4941` (hit), `:4964-4968` (arm), and
  `cachedMod` (`:4726`, `:4757`). Fields at `runtime/VM.h:1187-1191`.
- Upstream: 2eb77e9c9473 added `m_bigIntFoldFactor`.
- K4 row II.9 rules this cache per-thread. It was never gated.
- Interleaving: thread A stores divisor y at `:4965`, before it computes the
  fold factor and inverse at `:4966-4968`. Thread B runs `x % y` with the same
  BigInt, matches at `:4941`, and uses a stale fold factor, or an inverse sized
  for the old divisor. The comment at `:4752` says the fixed-extent span has no
  bounds check.
- Fix: under `vm.gilOffWithProcessGate()`, skip the cache: no hit, no arm, and
  no `m_next*` update.

### VM-5: `regExpGlobalData()` used directly in two new sites

**Risk: confirmed-race. Verified.**

- Sites: `runtime/RegExpPrototype.cpp:892` and `dfg/DFGOperations.cpp:1927`.
  Both call `globalObject->regExpGlobalData().recordMatch(...)`.
- Upstream: 73e4c589c1e6 and 0d5d36b5f94e.
- Reachability: the `RegExpPrototype.cpp:892` site runs on a split-cache hit
  (VM-2), on any thread. The `DFGOperations.cpp:1927` site
  (`operationRegExpExecStickyKnownRegExp`) is not reachable GIL-off today:
  `DFGStrengthReductionPhase.cpp:1234` skips the sticky conversion when
  `vm().gilOff()`. Its sibling at `:1889` already uses the per-thread accessor.
- AUD1.K2 (`RegExpGlobalDataInlines.h:36-51`) says every C++ caller must use
  `threadRegExpGlobalData(globalObject)`. `git grep 'regExpGlobalData()'` at
  HEAD finds only these two callers, plus the accessor.
- Effect: two threads write the shared multi-word match record. `RegExp.$1`
  can then pair one thread's input with another thread's offsets.
- Fix: change both calls to `threadRegExpGlobalData(globalObject)`. The
  VM-2 fix also removes the first site's reachability, but change it anyway.

### VM-6: `IntlCache::m_cachedDateTimeFormatImpls` is outside `m_lock`

**Risk: confirmed-race. Verified.**

- Site: `runtime/IntlCache.h:71-81`.
- Upstream: 8d261de036d4.
- The branch guards every other `IntlCache` member with `m_lock`
  (`IntlCache.h:106-112`). This TinyLRUCache of `RefPtr` is not guarded, and
  `findIfCached` also reorders entries.
- Fix: take `m_lock` in `findCachedDateTimeFormatImpl`,
  `cacheDateTimeFormatImpl`, and the clear.

### VM-7: `localeCompare` collator cache on the global object

**Risk: confirmed-race. Verified.**

- Site: `runtime/JSGlobalObject.cpp:3866-3875`.
- Upstream: 33a5272cf9ac.
- Three fields (the collator, a `String` locale, and an epoch), read and
  written with no lock. Assigning the `String` derefs the old StringImpl while
  another thread compares it.
- Fix: add a leaf `Lock`, as `m_functionConstructorExecutableCacheLock` does.
  Create the collator outside the lock, then check and publish under it. Or
  skip the cache under `gilOffWithProcessGate()`.

### VM-8: `bun:ffi` state on the global object

**Risk: confirmed-race. Verified. Two readers found this on their own.**

- Sites: `runtime/JSGlobalObject.cpp:4548-4556` (`ffiContext()`, a lazy
  `unique_ptr` with no CAS). `ffi/FFIContext.cpp:119-158` (`StringArena`:
  plain `m_depth`, `m_offsetInLastChunk`, and `m_chunks`). `FFIContext.cpp:86-116`
  (the UTF-8 cache).
- Upstream: e6063b004fa4. Nothing in `ffi/` checks the thread or GIL state.
- Interleavings:
  1. Two threads both call `allocate` and get the same bytes.
  2. One thread's scope exit runs `reset()`, which frees chunks that the other
     thread's native call is still reading.
  3. Two first callers of `ffiContext()`: the second assignment deletes the
     first context while it is in use.
- Also (not re-read here): `JSFFICallback::m_returnCString`
  (`ffi/JSFFICallback.cpp:148-153`) is a per-callback buffer whose pointer is
  the native return value.
- Fix: for now, refuse `bun:ffi` calls and callbacks on spawned threads GIL-off,
  as the branch does for wasm (fdcaee0b03e0). The full fix is a per-thread arena
  and UTF-8 cache, plus a CAS publish for `m_ffiContext`. See also OPT-1 and
  OPT-2, which need the same force-off.

### VM-9: `m_terminationDeadlines` lazy publish

**Risk: likely-race. Not re-read.**

- Site: `runtime/VM.cpp:1750-1752`. Upstream: 8a31c0d8f1a1.
- `if (!m_terminationDeadlines) m_terminationDeadlines = create()` has no CAS.
  Bun reaches it through the node:vm `timeout` option. A second issue: the
  timer fires `notifyNeedTermination()`, which GIL-off traps every thread, not
  only the one that set the deadline.
- Fix: create the set in the VM constructor. Record the requesting VMLite in the
  deadline, and fire only its traps.
- Status (second round): fixed as described. Firing only the lite's word was
  not enough: the loop checks of every tier poll the VM word, so a call-free
  loop did not see the request. `VMTraps::fireTargetedTermination` sets the bit
  in the lite's word and in the VM word. `VMTraps::m_vmWideTerminationRaised`
  says whether the VM word's bit is a VM-wide request. While it is not, the
  other threads leave the bit alone: `handleTraps` masks it, the stop request
  and the bits copied into a lite exclude it, and the target retires it when it
  services the request. The target does not fan the request out.
  `targetedTerminationRequested` routes the bit, and `targetedTerminationHandled`,
  set when the thread services it, stands in for the VM's request flag on that
  thread, as `setHasTerminationRequest` does for a VM-wide termination. A thread that polls while a targeted request of another thread is
  pending takes the slow path until that thread services it. The watchdog stays
  VM-wide.

### VM-10: `UnlinkedMetadataTable::link` and `unlink` can run at the same time

**Risk: likely-race. Shape verified. The sweep-side trace is not complete.**

- Sites: `bytecode/UnlinkedMetadataTableInlines.h:176-187` (`link`) and
  `:207-219` (`unlink`).
- Upstream: 024831d80fa0 and 62f427b86ffb. Both now free and replace
  `m_rawBuffer`. Before, they only flipped `m_isLinked`.
- `link` runs under `gilOffCompilationLock` (`ScriptExecutable.cpp:559`).
  `unlink` runs from `~CodeBlock` during a sweep on another mutator, with no
  lock. Commit d318750a03c5 made `~CodeBlock` free the metadata inline when
  threads are on.
- Interleaving: `link` copies from `m_rawBuffer` while `unlink` frees it.
- Fix: add a leaf `Lock` to `UnlinkedMetadataTable`, taken in `link` and
  `unlink` when `Options::useJSThreads()`. Never hold it across a GC allocation.

### VM-11: `hasAnyEntryScopeServiceRequest` lost two checks

This is a rebase loss, not a race. See RB-2.

### Other class 4 rows (not re-read here)

| Row | Site | Upstream | Risk | Fix |
|---|---|---|---|---|
| VM-12 | `VM.cpp:3729-3733` `ensureCachedBytecodeTwoCharacterAtoms`; `CachedTypes.cpp:360-475` `DecoderStringTable` | 120b075b393a, 5803c87d881b, ea4f05182966 | needs-trace | Every JSC caller holds `GILOffCompilationLocker`. Bun's `JSC__IdentifierArray__setFromSlot` also calls `atomForSlot`, and that path is not traced. Take the compilation lock in `atomForSlot`. |
| VM-13 | `VM.h:1671-1672` `m_allowRedeclaringSymbols` | 7c9b3a28260f | likely-race, semantic only | This is a scoped inspector mode, the same shape as `m_deletePropertyMode` (K4 row II.16). Move it to VMLite. |
| VM-14 | `SyntheticModuleRecord.cpp:164-207` `materializeLazyExport` | 7b763944f0ec | likely-race, semantic only | This is check-get-put with no CAS, so the getter can run twice. CAS-publish from empty. |
| VM-15 | `MegamorphicCache.h:257-266` `initAsGetterHit` has no `fillsDisabledUnderJSThreads()` check | 6d70d346c664 | safe-covered (hygiene) | The only caller, `getByIdMegamorphic`, is unreachable GIL-off (`InlineCacheCompiler.cpp:4877`, `InlineCacheCompiler.h:169`). Add the one-line check anyway, to match the sibling fills. |

### Checked and safe (class 4)

- `m_fastAsyncGeneratorSentinel`, `m_intlLegacyConstructedSymbol`: set in the
  constructor.
- `m_regExpLastIndexWritableWatchpointSet`: fired through
  `WatchpointSet::fireAll`, which takes the Class-A stop.
- New `LazyClassStructure` and `LazyProperty` members (Temporal, FFI structures,
  async-from-sync iterator): the LazyProperty first-touch protocol.
- `TerminationDeadlineSet` internals, `FFIContext::m_liveCallbacks`,
  `FFI::SignatureRegistry`, `BufferAccessorRegistry`: locked.
- Removal of `stringRecursionCheck*` (f2f2c2ddf637): replaced by
  `isSafeToRecurseSoft()`, which is per-thread.
- StructureRareData packed word (cd91e7f128dd): the counter is under
  `Structure::m_lock`, and the TriState writes are idempotent. TSAN will report
  the word.
- CodeBlock aging fields, `UnlinkedFunctionExecutable` lazy decode,
  pre-publication codegen fields: locked or written before publication.
- RegExp `m_cachedGroupsStructureID`: a single word, re-checked by the reader.
- SparseArrayValueMap HashSet rework (b80bcf43bdce): the branch re-locked every
  writer. I did not check every `find()` caller.
- ArrayBuffer pin and lock packing (238d63ac9ef5): the branch keeps separate
  atomics.
- Racy profile counters (ArrayProfile, ValueProfile): tolerated by design.

Not checked: `JSPromiseCombinatorsGlobalContext::m_remainingElementsCount`
(the unlocked decrement predates the range), and Bun-side callers of
`atomForSlot`.

## 5. Options and macros (class 5)

All 30 macro names in the branch's `#if` lines are still defined at HEAD. Every
offlineasm setting the LLInt uses has an `OFFLINE_ASM_*` define. All 64 options
the branch reads still exist. Every function that holds a branch-added
`useJSThreads()` or `gilOff()` check still has a caller at HEAD (117 functions).
So no more threads code is compiled out, as `USE(JSVALUE64)` was.

### OPT-1: FFI JIT paths read the VM-level exception word

**Risk: confirmed. A GIL-off mode bug on one thread, not a race. Not re-read.**

- Sites: `ffi/FFIDFGCodegen.cpp:108`, `ftl/FTLLowerDFGToB3.cpp:16302`, `:16304`.
- Upstream: e6063b004fa4.
- GIL-off, every thread, the main thread included, keeps its exception in the
  VMLite. So the VM-level word is always null. After
  `operationFFIWriteSlot` throws, the exception check does not fire, and the
  call runs with a pending exception.
- Fix: use `loadException(vm, GPR)` in the DFG, and
  `loadCurrentThreadException()` in the FTL. Or force `useFFIICStub`,
  `useFFICallInDFG`, and `useFFIDirectCall` off GIL-off, at `Options.cpp:882`,
  next to the wasm force-off.

### OPT-2: FFI JIT paths publish the VM-level `topCallFrame`

**Risk: likely. A GIL-off mode bug. Not re-read.**

- Sites: `ffi/FFIICStub.cpp:368`, `:411-412`; `ffi/FFIDFGCodegen.cpp:356`;
  `ftl/FTLLowerDFGToB3.cpp:16503`, `:16567`, `:16572`.
- Upstream: e6063b004fa4.
- Every other JIT site at HEAD splits on the mode. A native callback that
  re-enters JS walks the stack from a stale frame.
- Fix: use `emitPublishTopCallFrameForHostCall(vm)` and the FTL
  `emitPublishTopCallFrame()`. Or use the force-off from OPT-1.

### OPT-3: `decommitUnusedMarkedBlockPages` runs from `sweep()`

**Risk: needs-trace. Not re-read.**

- Site: `heap/MarkedBlock.cpp:573-657`, called at `:711`.
- Upstream: e8bd7f041187. The option defaults to true and is active on Linux.
- The branch documents that `sweep()` can run with the world running, under
  MSPL. I did not trace whether a mutator-concurrent sweep or a sibling's
  allocation can touch a page after the decommit.
- Fix, if the trace does not clear it: force the option off under
  `useSharedGCIncrementalSweep`.

### Checked and safe (class 5)

- `useHandlerICInFTL` (forced on): upstream added no reads of it. The FTL
  data-IC slow paths are emitted at HEAD (`FTLLowerDFGToB3.cpp:6033`, `:18826`,
  `:22361`, `:22458`).
- `usePollingTraps` (forced on): the new `InvalidationPoint` arm in
  `handleCheckTraps` is reached only when polling traps are off.
- Megamorphic JIT probes, including the new `loadMegamorphicGetterSetter`:
  they bail under `useJSThreads`.
- New options `useWarmUpMarkedBlocks`, `useExecutionCountForCodeBlockAging`,
  `useGlobalInliningPlanner`, `forceUnlinkedDFG`, and the RegExp options: locked,
  off by default, or per-thread.
- Raw `addressOfException`, `topCallFrame`, and `topEntryFrame` uses outside
  `ffi/` and wasm are all in the GIL-on arm of a mode split.

## 6. Upstream changes the rebase dropped

These are not races. Each one makes flag-off code differ from upstream, which
the branch's rule (flag-off is byte-identical) does not allow. Method: 297
upstream `+` lines were removed again by the branch's commits. Of those, 134
appear nowhere in the HEAD file. Each was read by hand. Most are rewrites
(relaxed atomics, per-thread accessors, locked variants), which is intended.

### RB-1: `trySetIndexQuickly` lost three `setMayStoreHole()` calls

- Site: `runtime/JSObject.h:571-627`. This is the flag-off body.
- Upstream: 72e0ec4b2165 added the calls. 03ee1c83e10e moved the function to
  `JSObjectInlines.h`. The rebase kept the branch's in-class copy, which
  predates 72e0ec4b2165.
- Missing: in the Contiguous case, after `setPublicLength`; in the Double case,
  after `setPublicLength`; and in the ArrayStorage case,
  `if (arrayProfile && !storage->m_vector[i]) arrayProfile->setMayStoreHole();`.
  Compare `491b5cc236e9:Source/JavaScriptCore/runtime/JSObjectInlines.h:1464`,
  `:1485`, `:1495`.
- Effect: store-to-hole profiling is lost, so the DFG may speculate wrongly
  and OSR-exit more. This is perf, not correctness.
- Every other function in that move matches upstream.
- Fix: add the three calls. The flag-on path,
  `trySetIndexQuicklyConcurrent` (`JSObject.cpp:927`), also never sets the flag.
  That is branch code, and it only costs perf.

### RB-2: `hasAnyEntryScopeServiceRequest` lost the time zone and language checks

- Site: `runtime/VM.h:523-530`.
- Upstream: 8d261de036d4 moved the time-zone check out of
  `VMEntryScope::setUpSlow` and into this function, as
  `m_entryScopeServicesRawBits || hasTimeZoneChange() || hasLanguageChange()`.
  The rebase took the `VMEntryScope` half and kept the branch's old function.
  So neither side checks now.
- Effect: `executeEntryScopeServicesOnEntry` (`VM.cpp:3270-3276`) does not run
  after a time-zone or language change, unless some other service bit is set.
  `DateCache` and `IntlCache` keep the old zone or language. This gives a
  wrong result flag-off, and it is a regression against the merge base.
- Fix: add `|| hasTimeZoneChange() || hasLanguageChange()` to both returns.
  GIL-off, `intlCache().clearForLanguageChange()` writes a per-VM field, so
  check that path for a race when you make the change.

### RB-3: an upstream fix in `DFGFixupPhase` is gated on `useJSThreads`

- Site: `dfg/DFGFixupPhase.cpp:5172`.
- Upstream: 85309cfe91e8 added the same check with no gate
  (`491b5cc236e9:...DFGFixupPhase.cpp:5101`). The branch had its own copy,
  gated on `Options::useJSThreads()` to keep flag-off the same.
- Effect: flag-off, a for-of loop with a closure-captured `const` OSR-exits on
  every call and never tiers up. This is perf.
- Fix: drop `Options::useJSThreads() &&` and `[[unlikely]]`, and shorten the
  comment.

### RB-4: comment only

- `bytecode/Watchpoint.h:438-441`: the old first sentence contradicts the new
  paragraph under it (cea233cedec0). Use upstream's two lines.

### Branch changes that touch flag-off code (not upstream losses)

Found during this pass. They are listed so the flag-off diff review (LANDING-PLAN
2.x) sees them:

- `PropertyInlineCache.cpp`, `resetStubAsJumpInAccess`: a
  `WTF::storeStoreFence()` on the flag-off path (`dmb ishst` on arm64).
- `parser/VariableEnvironmentInlines.h:69`, `:76`: the map lock is
  unconditional.
- `runtime/ArrayBuffer.h:428-508`: `m_locked` stays a separate atomic. The
  layout differs from upstream even flag-off.
- `runtime/ObjectConstructorInlines.h:300`: the `source->butterfly() ?` test is
  gone. The result is the same.

## 7. Problems found outside the upstream range

These predate the rebase. They came up while checking the rows above. They
block landing, but they are not upstream's doing.

### PRE-1: Map and Set are not locked, and N7 says they are

**Risk: confirmed-race, by reading. Memory-unsafe.**

- `SPEC-ungil-audit-N7.md` row R1 says "COVERED §N.1: ALL ops (reads too)
  cell-locked; DFG/FTL map intrinsics DISABLED GIL-off". Neither is true at
  HEAD or at `3a14f2a821ac`. `runtime/JSOrderedHashTable*.{h,cpp}`,
  `MapPrototype.cpp`, and `SetPrototype.cpp` have no `cellLock`, no `Locker`,
  and no threads check. `DFGByteCodeParser::handleIntrinsicCall` has no gate for
  the Map intrinsics.
- Interleaving: `JSOrderedHashTableHelper::addImpl`
  (`JSOrderedHashTableHelper.h:452-493`). Two threads call `map.set` on the same
  Map when `usedCapacity == dataCapacity - 1`. Both pass `expandIfNeeded`
  (`:420`). Both read `usedCapacity` and increment `aliveEntryCount` with plain
  stores. The second thread's `newEntryKeyIndex` is then past the data table, so
  `setKeyOrValueData` writes out of bounds.
- `JSTests/threads/congc-t5-celllock-audit.js:22` says the lock exists, but its
  Map is local to each thread, so it never shares one.
- Upstream added more unlocked readers (OM-10, and `ArrayConstructor.cpp:362`
  and `:584`, from cb1c48b3e95f). Those are reads of a GC cell, which are
  memory-safe. The writers are the problem.
- Fix: implement the N.1 ruling. Take the cell lock in the `JSMap` and `JSSet`
  native functions and in the helper's add, remove, and clear. Disable the DFG
  and FTL Map intrinsics GIL-off. Add a test in which two threads write one
  shared Map. Then correct N7 row R1.

### PRE-2: check-then-reload in `FastStringifier::appendInt32Array`

- `runtime/JSONObject.cpp:1800-1804`. See OM-6.

### PRE-3: DFG spread copy is not bounded by the snapshot's `vectorLength`

- `dfg/DFGSpeculativeJIT.cpp:9368-9400`. The flag-on arm reads `publicLength`
  once and copies that many elements from the loaded storage. `publicLength` is
  shared with a spine (C4), so a foreign grow can move it past this storage's
  `vectorLength`. The window is a few instructions.
- Fix: also load `vectorLength` from the same butterfly, and take the minimum.

### PRE-4: JIT-emitted structure transitions (second round)

**Risk: none found. Four sites had no local check.**

- The structure-store audit (LANDING-PLAN.md, section 1.3) covered the C++
  transitions. The second round checked the transitions that JIT code emits:
  the LLInt `put_by_id`, `put_private_name` and `set_private_brand` caches,
  the IC and handler-IC `Transition`, `Delete` and `SetPrivateBrand` cases, the
  megamorphic store, and the DFG and FTL `PutStructure`, `MultiPutByOffset`,
  `MultiDeleteByOffset`, `NukeStructureAndSetButterfly` and property-storage
  nodes.
- None runs on a published object with the flag on, GIL on or off. The gates:
  `Repatch.cpp` gives up on the Transition, Delete and SetPrivateBrand cases;
  the LLInt fills its transition caches only for `useUnthreadedLLIntPropertyCaches()`;
  `DFGByteCodeParser::handlePutById` routes a Transition variant to the generic
  node; constant folding does not turn a transition into `PutByOffset`. The
  storage nodes, the FTL `MultiPutByOffset` transition arm, and the IC
  compiler `RELEASE_ASSERT` that the flag is off. So the E4 transition
  predicate of SPEC-jit section 5.5 is not emitted anywhere, and every
  transition goes through the object model. The structure stores that do run
  flag-on are the stores into new cells (`emitAllocateJSCell`, the FTL
  `allocateCell`).
- Sites that relied on a gate elsewhere, now checked locally: the DFG and FTL
  `PutStructure` assert that the flag is off; the DFG parser does not inline a
  delete hit or a private brand with threads; the private-brand handler thunk
  jumps to its slow path with threads, as the delete handlers do. The LLInt
  `put_private_name` and `set_private_brand` fast paths branch to their slow
  paths on the flag (third round); before that they relied on the metadata
  never being filled flag-on, so that the cached StructureID stayed 0 and
  never matched.

### PRE-5: a lower tier could replace a higher one, GIL off (second round)

**Risk: confirmed by test. Fixed.**

- `LLIntSlowPaths.cpp`, `entry_osr_function_for_call` and the three like it,
  tiered up the executable's current CodeBlock, as upstream does. A caller
  can enter the LLInt prologue through a call link that another thread
  relinks a moment later, when it installs the optimized replacement. The
  prologue then gave the optimized CodeBlock baseline code. Fix: tier up the
  frame's CodeBlock, and only when it is current (`functionEntryOSR`).
- `BaselineJITPlan::finalize`, and the LLInt's shortcut to shared baseline
  code, publish the baseline code before `ScriptExecutable::installCode`
  takes the compilation lock. Another thread can run that code, tier up, and
  install the optimized replacement first. The late install then put the
  baseline CodeBlock back. Fix: under the lock, `installCode` skips a
  CodeBlock that is the alternative of the optimized one in the slot, unless
  the install is a jettison.
- Neither can happen with the GIL on. One thread runs JavaScript at a time,
  and neither window gives up the GIL.
- Test: `JSTests/threads/giloff-prologue-tiers-up-frame-code-block.js`. The
  corpus flake `semantics/stack-overflow-per-thread.js` was this bug.

### PRE-6: a collection under a cell lock, GIL off (second round)

**Risk: confirmed by test. Fixed.**

- `JSObject::defineOwnIndexedProperty` adds to the sparse map under the
  object's cell lock, flag on. `SparseArrayValueMap::add` reports the map's new
  capacity with `reportExtraMemoryAllocated`, which can start a collection.
  With the GIL off, this thread conducts it and waits for the markers, and a
  marker that visits the object waits for the cell lock. The process stops.
- The comment at the site said the add only calls `fastMalloc`. The report is
  the part it missed. `enterDictionaryIndexingModeWhenArrayStorageAlreadyExists`
  already holds a `DeferGC` before its lock for the same reason.
- Fix: a `DeferGC` before the lock. The other sparse-map calls in `JSObject.cpp`
  run outside the object's lock.
- A second shape, found by the same suite (`stress/redefine-property-writable.js`):
  `deletePropertyNamedConcurrent` allocates nothing under the cell lock, but
  `Structure::remove` takes a `GCSafeConcurrentJSLocker`, whose `DeferGC` ended
  under the lock. `~DeferGC` calls `decrementDeferralDepthAndGCIfNeeded`, which
  can conduct a pending collection. So a deferral scope that ends under the
  lock is a collection point too, unless an outer scope is still live. Fix: a
  `DeferGC` before the lock. Test:
  `objectmodel/dictionary-delete-collects-outside-cell-lock.js`.
- A marker takes a `JSObject`'s cell lock only for ArrayStorage shapes, while the
  mutator runs (`JSObject.cpp`, `visitButterflyImpl`). Inside a GIL-off stop
  window, the poll cannot conduct, because the window's conductor holds the
  collector's conductor lock.
- A search of the other cell-locked regions, for both shapes, found one more:
  `flattenDictionaryStructureImpl` declares its `GCSafeConcurrentJSLocker` after
  the cell locker. GIL off, it runs inside a stop. With the GIL on and a shared
  heap, it may be reachable. It has the same fix. `FunctionRareData::initializeObjectAllocationProfile`
  and `createInternalFunctionAllocationStructureFromBaseGILOff` allocate under
  the rare data's lock on purpose: `visitChildren` does not take it, and the
  waiters poll for stops. Third round: that lock is a plain `Lock` in the rare
  data now, not its cell lock, because the holder also parks under it and the
  Debug cell-lock checks under `--useConcurrentSharedGCMarking` (no cell lock
  at a collection point or a park) fired there; see PRE-10.
- Test: `JSTests/threads/objectmodel/sparse-define-collects-outside-cell-lock.js`.
  Found by `stress/intl-having-a-bad-time.js` in the GIL-off JSC suite.

### PRE-7: `butterfly()` on a word that can be segmented (second round)

**Risk: confirmed by test. Fixed.**

- `getByVal` in `jit/JITOperations.cpp` (two copies) and `LLInt::getByVal`
  compared the index with `object->butterfly()->publicLength()` for a
  contiguous array. `butterfly()` must not decode a segmented word
  (`JSObject.h`), and another thread can segment the array at any time. Every
  read past the end of a segmented array reaches these lines. With
  `--verifyConcurrentButterfly` it fails an assertion. Without it, the length
  comes from the memory just before the spine.
- Fix: with the flag on, read the length through the word, with
  `getArrayLength()`, as `directPutByVal` already does.
- The same mistake, found by a search of `runtime/`, `jit/`, `dfg/`, `ftl/`,
  `llint/`, `bytecode/`, `interpreter/`, `heap/` and `API/` for `butterfly()`
  on an object with Int32, Double or Contiguous indexing. Reached by any use of
  a segmented object:
  - `JSObject::canHaveExistingOwnIndexedProperties()` and
    `forEachOwnIndexedProperty()` (`JSObjectInlines.h`): the fast paths of
    `Object.assign`, `Object.entries` and `Object.values`. With the verifier
    off, `Object.assign` copied no indexed property.
  - `JSCellButterfly::createFromClonedArguments`: the spread of an arguments
    object.
  - `tryCloneArrayFromFast` (`JSArray.cpp`): `Array.from`, `concat` and
    `toSorted`. With the verifier off, `Array.from` returned an empty array,
    and the copy read the spine's memory as elements.
  - `JSObject::getEnumerableLength()`: the indexed length of a `for-in`.
  - `JSObject::analyzeHeap` and `JSObject::estimatedSize`: the heap snapshot.
    A stop does not make a segmented word flat.
- Reached only through a race, between a check of the word and a second load:
  `JSArray::setLength` counted the elements through `countElements()`, which
  loaded the word again; `JSArray::pushInline` decoded a second load after its
  owner check; DFG constant folding called `butterfly()` before its structure
  check, which asserts under the verifier.
- Fixes: read through one load of the word. A segmented word takes the generic
  path (the clone, the spread, the `for-in`), or the spine (the snapshot). A
  flat snapshot is read within its own `vectorLength`, because `publicLength`
  is shared with a later spine (C4). `setLength` passes its snapshot to
  `countElementsIn()`. `pushInline` starts the dispatch again on a segmented
  word.
- The other `butterfly()` reads that the search found are ArrayStorage (never
  segmented), cells that are not yet published, conversions inside a stop,
  code that already dispatches on the word, or flag-off branches.
- Tests: `JSTests/threads/arrays/segmented-out-of-bounds-read.js`,
  `segmented-out-of-bounds-read-llint.js`,
  `objectmodel/segmented-indexed-copy-paths.js` and
  `objectmodel/segmented-array-read-paths.js`.

### PRE-8: adaptive watchpoint install after a foreign transition (second round)

**Risk: confirmed under the amplifier. Fixed.**

- `DFG::Plan::reallyAdd` checks `areStillValidOnMainThread` and then installs
  the watchpoints. `AdaptiveStructureWatchpoint::install` did
  `RELEASE_ASSERT(m_key.isWatchable(MakeNoChanges))`, which reads the watched
  object's current structure. With the flag on, another thread can transition
  that object between the check and the install.
- Fix: with the flag on, `install` (and
  `AdaptiveInferredPropertyValueWatchpointBase::install`) returns false when
  the condition is no longer watchable. The callers handle a refused install:
  `reallyAdd` fails the compile as invalidated, `fireInternal` jettisons.
- Reproducer: `gc-stress/watchpoint-storm.js` under `amplify.sh`, GIL off, 5
  of 400 runs before, 0 of 400 after.

### PRE-9: a `Set` spread restarted its walk from the first table, GIL off (third round)

**Risk: confirmed by test. Fixed.**

- `JSCellButterfly::createFromSet` (the C++ copy behind `[...set]`), GIL-off
  branch: every step called `transitAndNext` with the set's first table and the
  entry reached in the newest one. After a rehash by another thread, each step
  moved the entry back by the obsolete table's deleted-entry count (to 0 after
  a clear), so the walk never ended and never reached a safepoint; the next
  stop-the-world request hit the watchdog.
- Fix: carry the table forward (`storage = transitionResult.storage`), as
  `JSSetIterator`, `forEachInSetStorage` and the `Array.from` fast paths do. A
  search of every `transitAndNext` / `nextAndUpdateIterationEntry` caller found
  no other loop of this shape.
- Test: `JSTests/threads/shared-objects/set-spread-vs-rehash.js`. The
  amplifier finding was `shared-objects/map-set-shared-writers.js`.

### PRE-10: lazy first-use state raced, GIL off (third round)

**Risk: confirmed by test (three sites) and by TSAN (one). Fixed.**

State that a cell or a runtime object creates on first use, with a plain
check-then-store, is created twice when two threads use it first together.
With the GIL on the check and the store never interleave. Four sites:

- `JSFunction`'s lazy `prototype` (`getOwnPropertySlot`, `put`,
  `defineOwnProperty`, `reifyLazyPrototypeIfNeeded`): two threads created two
  prototype objects and the second store replaced the first, so objects
  already made on the first thread were not `instanceof` the function, and a
  user's store racing the first read could be replaced by a default object.
  Fix: GIL off, check and store under one process-wide lock whose waiters poll
  for stops (`storeLazyPrototypeIfMissingGILOff`). Test:
  `semantics/lazy-prototype-first-use-race.js`.
- `ErrorInstance::materializeErrorInfoIfNeeded`: the first read of `stack`
  builds the strings and frees the captured trace; a second thread walking the
  trace crashed. Fix: GIL off, one thread claims the materialization through an
  atomic state byte; the others wait (polling for stops) and use its result.
  Test: `semantics/error-stack-first-access-race.js`.
- `SourceProvider::sourceURLStripped()`: a `String` computed and assigned on
  first call, from the stack-trace path. Fix: publish once, under the
  provider's lock, behind an acquire/release flag (all modes; the fast path is
  one load). Found by TSAN on the test above.
- `ExpressionInfo::lineColumnForInstPC`: a `HashMap` cache per code block,
  filled from the stack-trace path (and the debugger and profilers). Fix: with
  the flag on, one process-wide lock around the lookup and the fill. Found in
  Debug on the test above (the hash table's iterator checks).
- `FunctionRareData`'s allocation-profile fill was already serialized (first
  round); the third round changed its lock from the cell lock to a plain lock,
  because the holder allocates and parks under it (PRE-6's rule, enforced by
  the Debug cell-lock checks under `--useConcurrentSharedGCMarking`). Test:
  `objectmodel/allocation-profile-init-lock-not-a-cell-lock.js`.
- Read and left: `SourceProvider::getID()` assigns the provider's ID on first
  use with a plain store, so two first calls from two threads can hand out two
  IDs and keep the later one; nothing dereferences an ID, and its users (the
  debugger, the type and control-flow profilers, code-cache keys) run on the
  carrier or at link time, so it stays as it is, noted here. `JSFunction`'s
  lazy `length` and `name` store equal primitive values, so a double store is
  harmless; `LazyProperty` has its own GIL-off protocol
  (`LazyPropertyInlines.h`); `Structure` property tables are materialized under
  the structure's lock. (Fourth round: the `length`/`name` reading was wrong —
  the flag is set before the put, and a reader that trusts it misses the
  property — and `getID()` is now a compare-and-swap anyway; see PRE-14.)

### PRE-11: a trap check under a cell lock, GIL off (third round)

**Risk: confirmed under the amplifier. Fixed.**

- `JSObject::putDirectIndexForAtomicsMissingAdd` (`ThreadAtomics.cpp`), the
  sparse-map arm: `RETURN_IF_EXCEPTION` after `map->putDirect`, inside the
  object's cell lock. `RETURN_IF_EXCEPTION` handles traps, and a handled trap
  parks for a pending stop or, after a conductor moved the heap-fact epoch,
  requests a stop to jettison the thread's optimized code. A thread blocked on
  the same cell lock (`Object.defineProperty` of the same index) has no
  safepoint, so the stop never completed:
  `cve/mc-reent-store-missing-indexed-define-race.js` hit the 30 s watchdog in
  35 of 3,000 amplified runs (64 at a time), 0 of 3,000 after.
- Fix: `RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED` there. A brace-aware search of
  `runtime/`, `bytecode/`, `heap/`, `jit/`, `dfg/` and `llint/` for trap checks
  inside `Locker { ...cellLock() }` / `lockCellChecked` blocks found no other,
  and `VMTraps::handleTraps` now asserts in Debug, GIL off, that the thread
  holds no cell lock (`GCCellLockDepth`), so a callee that checks traps under a
  caller's cell lock fails the Debug corpus.

### PRE-12: TSAN's 16-byte compare-and-swap is not atomic against smaller atomics (third round)

**Risk: TSAN builds only. Fixed there.**

- `dcasHeaderAndButterfly` used `__sync_bool_compare_and_swap` on
  `unsigned __int128`, which clang lowers to `__tsan_atomic128_compare_exchange`
  under TSAN, and TSAN's runtime implements 16-byte atomics with an internal
  lock around a plain load and store. The 1-, 4- and 8-byte atomics on the
  same 16 bytes (cell lock bits, structure ID lane, butterfly word) do not take
  that lock, so a lock bit set between the load and the store was lost
  ("Invalid value for lock" at the holder's unlock). Every TSAN result of the
  object model depended on this being atomic.
- Fix: under `TSAN_ENABLED` on x86-64, `lock cmpxchg16b` by inline assembly,
  after two no-op read-modify-writes that give TSAN the publish's release edge
  on both words. arm64 TSAN builds would need the same and are not used.

### PRE-13: `Atomics` C++ paths loaded the typed array base after the detach check, GIL off (third round)

**Risk: confirmed under the amplifier (a null-base write, signal 11). Fixed.**

- GIL off, `JSArrayBufferView::detachFromArrayBuffer` keeps the base word for
  JIT code and sets `m_detachedKeepingVector`, and the C++ accessor `vector()`
  returns null from then on. The C++ element paths were converted to load the
  base once, before or together with their bounds proof, and to treat null as
  detached (`getIndexQuicklyAsNativeValue`, `setIndexQuicklyToNativeValue`,
  `setFromTypedArray`, `setFromArrayLike`, `fill`, `copyWithin`, `includes`,
  `indexOf`, `lastIndexOf`, `reverse`, `sort`, `DataView` get/set). Two were
  not: `atomicReadModifyWriteCase` (behind `Atomics.add`, `and`,
  `compareExchange`, `exchange`, `load`, `or`, `sub`, `xor`, and the DFG's
  `operationAtomics*` slow paths) and `atomicStore` checked `isDetached()` and
  `inBounds()` and then called `typedVector()`, which a transfer on another
  thread could have turned null in between:
  `cve/mc-prim-arraybuffer-transfer-vs-atomics.js` crashed at address 0 to 12
  in 17 of 600 amplified Debug runs.
- Fix: load the base before the checks and throw the detached `TypeError` when
  it is null (flag off, a null base there implies `isDetached()`). The
  remaining `typedVector()` calls in `AtomicsObject.cpp` are the
  `wait`/`notify`/waiter-list paths, which require a `SharedArrayBuffer`, and
  that cannot be detached. The `Uint8Array` base64/hex methods load the base
  before the length, which on x86-64 cannot pair a null base with a stale
  length (the detach publishes the flag before the zero length); noted for
  arm64, where `vector()` would want a load-load fence for the same argument.

### PRE-14: lazy first-use state, the full search (fourth round)

**Risk: confirmed by test (seven sites). Fixed.**

The third round's PRE-10 fixed four first-use races and did not search for the
rest. This round went through the branch's own audit tables — every K4 row
(VM-level state) and every N7 row (per-cell state) — and checked each ruling
against the code, then through the lazily filled members of the runtime classes
the tables do not name (`mutable` members, `ensure*` and `m_cached*` members,
check-then-store patterns under `runtime/`, `bytecode/`, `parser/`). Rulings
that had not been implemented, or had been implemented for one class of a
pair:

- N7 R2, `WeakMapImpl` ("COVERED §N.1"): nothing locked. Fix: cell lock
  around every table operation in a GIL-off process, `getOrInsert`'s
  find-and-add as one hold, no DFG inlining of the five intrinsics
  (`isHashTableIntrinsic`). Test: `shared-objects/weakmap-weakset-shared-writers.js`.
- N7 RESOLVED-4, `ScopedArguments::overrideThings`: the `DirectArguments` half
  (RESOLVED-3) had its lock, this half kept `RELEASE_ASSERT(!m_overrodeThings)`.
  Fix: the same polling first-use lock (`GILOffFirstUseLocker`, now a shared
  helper in `JSThreadsSafepoint.h`), re-check, release-publish of the flag,
  and the lock around `unmapArgument`'s table copy. Test:
  `semantics/scoped-arguments-override-race.js`.
- N7 RESOLVED-6, the Intl cells: nothing done. Fix: `intlLazyField` /
  `intlLazyString` / `intlLazyObject` in `IntlObjectInlines.h` (compute
  outside, publish once under the cell lock, GIL-off process only) for
  `IntlLocale`'s fifteen lazy members, the `numberingSystem`/`calendar` of
  `IntlNumberFormat`, `IntlDateTimeFormat`, `IntlRelativeTimeFormat`,
  `IntlDurationFormat`, the lazily opened `UDateIntervalFormat`,
  `UNumberRangeFormatter`, Temporal and per-unit formatters; per-call scratch
  in `IntlRelativeTimeFormat::formatToParts`; a per-call break-iterator clone
  in `IntlSegments::containing`; the step of `IntlSegmentIterator::next` under
  the cell lock. The ICU handles that stay shared are used through `const` ICU
  calls only. Test: `semantics/intl-lazy-fields-race.js`.
- K4.II.4, `jsonAtomStringCache` ("per-lite"): still the VM's. Fix:
  `JSONAtomStringCache::live(vm)` routes a GIL-off thread to a thread-local
  table that caches atoms only. Test: `vmstate/json-parse-key-cache-per-thread.js`.
- K4.II.1, `numericStrings`: routed everywhere but `String.raw`
  (`StringConstructor.cpp`). Fix: `liveNumericStrings()`. Test:
  `vmstate/string-raw-number-cache-per-thread.js`.
- PRE-10's "read and left" `JSFunction` `length`/`name`: wrong, see the plan's
  fourth-round entry (flag before put; inline cache then serves the prototype's
  value; bit-field lost updates). Fix: atomic flag byte set after the put,
  first reification under a first-use lock. Test:
  `semantics/lazy-length-name-first-use-race.js`.
- PRE-10's "read and left" `SourceProvider::getID()`: now a compare-and-swap.
  No test (an ID is not observable from script in the shell).
- `VM::emptyPropertyNameEnumeratorSlow` and the four promise-function
  executable slow paths: compare-and-swap publish flag on. No test.
- `CodeBlock::RareData::m_exceptionHandlers` on optimizing-JIT code blocks:
  grown by `appendExceptionHandler` at IC link time and shrunk when the stub
  dies, walked by the unwinder on any thread. Fix: a lock in the rare data,
  held by those three flag on; the unwinder copies the `HandlerInfo` under it.
  `jit/ic-exception-handler-table-vs-unwind.js` exercises the shape but does
  not fail without the fix.

Verified as implemented or safe, and why (the ones a reader would ask about):
`lastCachedString` (one GC-pointer word; a value in use is a conservative
root), `lastAtomizedIdentifier` (bypassed GIL off), `stringSplitCache` /
`stringSplitIndice` / `stringReplaceCache` (skipped GIL off), sort scratch
(per-lite), the BigInt divisor cache (skipped), `dateCache` (every entry
point through `live()`), `DateInstance::m_data` (never written GIL off),
`adaptiveStringSearcherTables` (per-thread), `hasOwnPropertyCache` (skipped),
the megamorphic cache (fills off), `RegExp` compile state (cell-locked GIL
off) and `m_cachedGroupsStructureID` (one word, either value valid),
`Structure` rare data and its enumerator / special-property caches (CAS
install, fills under `m_lock`), the module resolution cache (cell-locked),
`DirectArguments` / `GenericArguments` (CAS-publish), `JSArrayBufferView`'s
slow-down path (CAS), `TypedArrayController`'s wrapper (CAS), `JSFunction`
rare data (CAS) and allocation profile (its own lock), `CodeBlock` rare data
(CAS), catch liveness (release-publish), `DirectEvalCodeCache` (locked flag
on), `CodeBlock::hash()` (idempotent word), template objects (made at link,
under the GIL-off compilation lock, as is everything the parser, the code
cache, `sourceProviderCacheMap`, the TDZ environment map and the symbol
table cache touch), `StructureCache` (locking map, first-wins), `Symbol` and
atom-string maps (locking `WeakGCMap` flag on), `InlineWatchpointSet::inflate`
(double-checked under the membership lock), `LazyProperty` (its own protocol,
for any owner, which covers the Temporal calendars), `WeakRef` (a version
word), `FinalizationRegistry` (locked), `DeferredWorkTimer` (locked),
`WaiterListManager` (locked; allocation and heap-access release outside its
locks), `VM::ensureWatchpointSetForImpureProperty` (unlocked but unreachable:
no class sets `NewImpurePropertyFiresWatchpoints`).

Left as they are, recorded: `Math.random`'s state on the global object is one
xorshift pair advanced by every thread (and by DFG code inline), so two
threads can draw the same number; not memory-unsafe, and `Math.random` makes
no promise it breaks. `JSGlobalObject::createRareDataIfNeeded` and the C API's
`JSCallbackObject` private-property map are embedder-only paths (K4.IV.9,
N7 R30). `IntlCollator`'s and `IntlNumberFormat`'s bound `compare`/`format`
are one word each; two threads may make two bound functions (identity only).

### PRE-15: an error object made under a symbol table lock, GIL off (fourth round)

**Risk: confirmed by test (a stop-the-world deadlock). Fixed.**

- `symbolTablePut` (`JSSymbolTableObject.h`, both the `JSLexicalEnvironment`
  and the `JSGlobalObject` instantiations) threw the read-only `TypeError`
  inside its `GCSafeConcurrentJSLocker` on the scope's `SymbolTable::m_lock`.
  Making the error object adds properties (`ErrorInstance::finishCreation` →
  `putDirect`), and a GIL-off property add polls for a pending stop-the-world
  (`parkSitePollAndParkForStopTheWorld`) and parks for it; a thread blocked on
  the same symbol table lock waits with no safepoint, so a stop requested by a
  third thread never completed (30 s watchdog).
- Found by the new Debug assertion at the poll site (PRE-11's rule extended to
  ConcurrentJSLocks and checked whether or not a stop is pending), in 22 files
  of a GIL-off run of `JSTests/stress` on the Debug build; nowhere else.
- Fix: note the read-only entry under the lock, throw after releasing it.
- Test: `semantics/const-assign-throw-vs-scope-access-under-stops.js` (10 of 10
  watchdog aborts before, 0 after).

### PRE-16: the window-liveness constraint retained the conductor's own blocks (fourth round)

**Risk: over-retention, not unsoundness. Changed.**

- `Heap::addCoreConstraints`, "Wlr": walked every attached client's
  `m_lastActiveBlock`s, the conducting client's too. The constraint executes
  on the conductor (`ConstraintConcurrency::Sequential`; shared collections are
  mutator-conducted), whose stack and registers are captured in
  `m_currentThreadState` and scanned by `gatherStackRoots` as in the
  single-mutator protocol, so the conductor's window cells are rooted,
  heap-reachable or dead, exactly the argument the `size() <= 1` gate makes.
  The resume sweep, the empty-block judgement and `shrink()` treat the
  conductor's block as they treat the lone mutator's in the legacy protocol
  (all clients resume in-window before any mutator runs; `inUse` excludes the
  block from steal and shrink after).
- Change: skip `GCClient::Heap::currentThreadClient()` in the client walk.
- Test: `gc-stress/gc-reclaims-conductor-garbage-with-thread-attached.js`.

### PRE-17: static sweep of the directories the earlier rounds did not walk (fifth round)

**Risk: mixed; the real items are fixed, the rest classified.**

Method: the same read the fourth round applied to `runtime/`, `bytecode/`,
`parser/` and `dfg/`, now over `interpreter/`, `jit/`, `llint/`, `heap/`,
`yarr/`, `inspector/` (no frontend attached), `debugger/`, `API/` and the WTF
types a cell can expose: every `mutable` member, every `ensure*` / `m_cached*`
/ check-then-store, every `static` local or `NeverDestroyed`, every `HashMap`
/ `HashSet` / `Vector` a JS-reachable object owns and mutates after
construction, every non-atomic reference count or `WeakPtr` / `CheckedPtr` /
`SingleThreadIntegralWrapper` on such an object. About 290 rows were written
down with writer, readers and existing synchronization; the ones not SAFE by an
existing lock, a stop, pre-publication, or an earlier PRE row are these.

| Site | What | Disposition |
|---|---|---|
| `dfg/DFGCodeOriginPool.cpp` `addDisposableCallSiteIndex` (reached from `CodeBlock::newExceptionHandlingCallSiteIndex` when an inline cache with calls under `try` is regenerated), readers `StackVisitor::readFrame` / `CallFrame::codeOrigin` / the sampling profiler | `Vector<CodeOrigin>` append (may reallocate) under the CodeBlock lock; readers on other threads take no lock. PRE-14 locked the sibling `m_exceptionHandlers` table for the same grow-at-link / read-at-unwind shape and missed this one. | **Fixed.** Flag-on the pool keeps a release-published copy of the array for lock-free readers and retires (never frees) the arrays it replaces on growth; appends after link are rare and bounded by the free list. Removal stays world-stopped (GC End). |
| `heap/Heap.h` `immutableButterflyToStringCache` (`Array.prototype.toString` on copy-on-write literals) | bare VM-wide `HashMap` `find`/`add` from every thread (heap-container audit HC-02, still open) | **Fixed:** not used GIL off (it is a per-GC-cycle cache). Also seen by the TSAN mirror. |
| `heap/Heap.cpp` `preventCollection` | `m_collectContinuouslyLock.lock()` with heap access held: a second snapshotter parks in the lock while the holder requests a stop that then never completes | **Fixed** (found by the mirror first, `gc-stress/heap-snapshot-from-two-threads.js`): GIL off the shared server polls `tryLock` with the park-site poll in the loop. |
| `runtime/VM.h` `ensureHeapProfiler()` and the snapshot builders | `LazyUniqueRef` check-then-construct; `HeapProfiler`'s snapshot list appended by two builders | **Fixed:** the profiler is created eagerly GIL off; builders serialize on a profiler lock taken with the polling protocol (same test). |
| `heap/Heap.cpp` `clearConcurrentRetainedDataIfPossible` (incremental sweeper timer) | guard is `vm().entryScope`, which GIL off says nothing about other threads; would free `StringImpl`s other threads hold through `GCOwnedDataScope` | Not reachable today: the shared server forces the fenced barrier GIL off and the function returns on `mutatorShouldBeFenced()`. Made explicit (`isGILOffProcess()` early return) so it does not depend on that forcing. |
| `inspector/JSGlobalObjectConsoleClient.cpp` -> `InspectorConsoleAgent` message buffer, counters, timers | JSCOnly builds have `developerExtrasEnabled()` constant true, so every `console.*` feeds the agent's unsynchronized containers from any thread (jsc shell; Bun installs its own client and is not affected) | **Fixed:** spawned GIL-off JS threads print to the system console but do not feed the agent (`consoleAgentUsable()`), the debugger's SD13 rule. |
| `debugger/Debugger.cpp` `didCreateInternalFunction` / `willCallInternalFunction` | missed the spawned-thread early return their native-executable siblings have | **Fixed.** |
| `runtime/RegExpInlines.h` interpreter fallback after an unlocked `m_state` re-read | another thread's failed other-width JIT compile flips the state to `ByteCode` and publishes `m_regExpBytecode` with a store-store fence; the reader had no matching fence (wrong "no match" on weakly-ordered targets) | **Fixed:** load-load fence before the bytecode read GIL off; `m_state` reads are a relaxed byte under TSAN (`RacyRegExpState`). |
| `jit/BaselineJITCode.h` `m_ownerWentAwayAt` (Bun addition) | two sweeping threads destroying two owners of one shared unlinked code store the same time stamp | Racy store/load pair (defined behaviour; value identical). |
| `jit/JITThunks.cpp` "first fetcher issues the cross-modifying-code fence" | upstream assumes one mutator; a second mutator fetching a compiler-thread-generated thunk issues no fence of its own | arm64-only and theoretical (fresh executable memory, broadcast icache flush); x86-64 needs none. Recorded; the round's OSR-exit change (below) shows the mechanism to use if it is ever needed: bump the process stop generation on publication, `jsThreadsSyncToStopGenerationBeforeJITEntry` on the consumer. |
| `jit/JITSizeStatistics.cpp`, `jit/ICStats.cpp` | option-gated diagnostics (`dumpBaselineJITSizeStatistics`, `useICStats`) append to process-global containers from any finalizing / IC-probing thread | Diagnostics only; documented as single-thread options (like the type profiler). |
| `interpreter/ShadowChicken.cpp` exemption predicate | exempts spawned JS threads, not a second embedder/carrier thread entering a GIL-off VM | Debugger / `alwaysUseShadowChicken` only; Bun has one carrier per VM. Recorded. |
| `interpreter/CallFrame.cpp` `describeFrame()` static buffer | lldb helper; upstream Workers race it too | Left. |
| `yarr/` interpreter: no poll inside a match; `BytecodePattern` allocator is the VM's `m_regExpAllocator` under `m_regExpAllocatorLock` for the whole match | race-free, but (a) a long backtracking match delays every stop, (b) all interpreted (non-JIT) matches of all threads serialize on one lock, waiting with heap access held | (b) **Fixed** in this round's performance work (per-thread allocator, PERF-RESULTS B5); (a) is the same latency class as a long C++ loop in stock JSC (it delays that engine's GC the same way) and is recorded under A5. |
| `heap/Heap.cpp` `m_deprecatedExtraMemorySize`, Darwin critical-memory cache, opportunistic-task pacing fields (HC-05..08) | plain counters written from several threads | Accounting drift only; unchanged, listed in AUDIT-heapcontainers. |
| `heap/Heap.cpp` lambda / C finalizers | run on whichever thread conducts the collection | By design GIL off (documented for embedders: Bun's napi/FFI finalizers must not assume the main thread); no JSC change. |
| `heap/Heap.h` `m_observers`, `inspector` heap agent | frontend-attached only | Out of scope (no N-thread inspection), recorded. |
| `API/JSCallbackObject*`, `API/JSClassRef.cpp` (`contextData`, `prototype`), `API/JSWeakObjectMapRef*`, `JSObjectGet/SetPrivate`, `OpaqueJSPropertyNameArray` refcount | the C API's callback objects, class context data, private-property maps and weak maps are single-mutator structures with no locking | **Unsupported GIL off for objects reachable from two JS threads**, recorded in SPEC-nativeaffinity: Bun does not create `JSCallbackObject`s or C-API weak maps (verified: no `JSClassCreate` / `JSObjectMake` / `JSWeakObjectMapCreate` in its sources); an embedder that does must keep such objects thread-confined. `JSLockHolder` / `DropAllLocks` themselves are routed to the per-thread entry token and are safe. |
| `runtime/JSString.h` `vm.lastCachedString` (`jsStringWithCache`, used by Bun bindings) | unfenced single-word cache publish | Benign lost update (a cache miss); left, noted for the Bun patch set. |
| WTF types on cells: `RefCounted` (non-atomic) payloads hanging off shared cells | searched: `StringImpl` and `SymbolImpl` counts are atomic in this tree; `SourceProvider`, `SharedArrayBufferContents`, `ArrayBuffer` are `ThreadSafeRefCounted`; the remaining `RefCounted` payloads found on JS-reachable cells (`DateInstanceData` via the per-thread `DateCache`, `RegExp`'s `BytecodePattern` owned by the cell, Intl ICU handles behind PRE-14's lazy fields) are either per-thread, immutable after publication, or behind the cell lock | No new site. |

Two more first-use sites turned up by the mirror run rather than the read,
same class as PRE-14: `BuiltinExecutables::createExecutable` parsed (and used
the VM's per-provider function cache) outside the GIL-off compilation lock when
two threads touched different builtins first (fixed: takes the lock;
`VM::addSourceProviderCache` asserts it), and `JSGlobalObject::ffiContext()`'s
lazy `unique_ptr` (fixed: CAS publication).

## 8. The two later rebase ranges (ninth round)

The branch was rebased twice more after this file's first range: in the
eighth round onto `2e2aa2290fac` (368 upstream commits) and in the ninth onto
`dfd696443b9b` (four more). This section is the reading of both ranges,
`491b5cc236e9..dfd696443b9b`, 84 of whose commits touch
Source/JavaScriptCore or Source/WTF. The eighth round found the known-atom
bit of that range with the amplifier, not by reading; this is the reading.

Method: one reader per hazard class the branch's protocols cover - (1) new
per-cell or per-object mutable header and flag bits written after
publication, (2) JIT fast paths (LLInt, Baseline, inline caches, DFG, FTL,
and the runtime C++ that mirrors them) that load two fields of one cell and
assume no writer in between, (3) new caches and memos, in particular keyed on
Structure or StructureID, (4) new watchpoint sets and fire sites, (5) new
places that patch code or publish executable state - plus one reader over the
four commits of the ninth rebase in every class. Each reported site was
re-read at HEAD before it is listed. Two rows (R9-2, R9-7) were found first
by the rebased tree's own corpus run and then by the readers.

Classes 1, 2 and 4 found nothing in the upstream code outside the ninth
rebase's `dfd696443b9b`: no upstream commit in the range adds a bit to the
JSCell header, TypeInfo, Structure, JSString or StringImpl (the known-atom
bit came in with the eighth rebase's base, whose reader rule is in
SPEC-ungil history), and every JIT fast path the range adds or changes that
reads two fields of one cell either goes through the branch's helpers or is
gated off GIL off (checked: the DFG/FTL inline `toLowerCase`/`toUpperCase`,
`DateInstance`'s packed breakdown, the re-landed pair loads of `new_object`
and `put_to_scope` metadata, the megamorphic store fill, `put_by_id`'s
narrower lock, `branchIfNotAtomStringImpl`). The new mutable state is almost
all in `dfd696443b9b`'s lazily materialized bytecode-cache and link-time
state, written upstream for one mutator; with the GIL on that still holds
(none of it calls JS or hands the GIL over), with the GIL off it does not.

| Row | Site (HEAD) | Upstream | Affects | Risk | Ruling |
|---|---|---|---|---|---|
| R9-1 | `UnlinkedFunctionExecutable` thin children: `materializeDeferred{Name,Members,Scalars}Slow` (CachedTypes.cpp), `DeferredMembers::settle` | dfd696443b9b (`useThinChildExecutables`) | GIL off | confirmed-race, memory-unsafe | fixed (decoded eagerly GIL off) |
| R9-2 | `HeapProfiler` `materializeLazySymbolTablesForHeapAnalysis`: a heap walk on the snapshotting mutator | dfd696443b9b (`useLazySymbolTableConstants`) | GIL off | confirmed (corpus crash) | fixed |
| R9-3 | `Decoder` (one per payload): non-atomic reference count, `m_atomsByOrdinal`, `m_offsetToPtrMap` used from paths under different locks | dfd696443b9b | GIL off | confirmed-race, memory-unsafe | fixed (eager GIL off; thread-safe counts) |
| R9-4 | `SymbolTable::cloneScopePart` reads the pending entries without the table's lock; lazy constants materialized under only the table's lock | dfd696443b9b (`useLazySymbolTableConstants`) | GIL off | confirmed (read), likely (writer) | fixed (eager GIL off) |
| R9-5 | `VM::ensureCachedBytecodeThreeCharacterAtoms` and its entry fill | dfd696443b9b (`useFastCachedAtoms`) | GIL off | confirmed-race | fixed (bypassed GIL off) |
| R9-6 | `DecoderStringTable::atomFor` overwriting a slot `jsStringFor` set to a cell | dfd696443b9b | GIL off | likely | fixed (eager GIL off) |
| R9-7 | `CodeBlock` lazy FunctionExecutables: entry, `link()`, plain store, plain counter | dfd696443b9b (`useLazyFunctionExecutables`) | GIL off | confirmed (Debug corpus, TSAN) | fixed |
| R9-8 | `AbstractModuleRecord::materializePrelinkedEntries` sets its flag before the maps are filled; the per-import resolution memo | dfd696443b9b (`usePrelinkedModuleInfo`) | GIL off | confirmed-race | fixed (eager GIL off) |
| R9-9 | `PrelinkedModuleGraph` is `RefCounted`, deref'd from sweeping threads | dfd696443b9b | GIL off | likely | fixed (thread-safe count) |
| R9-10 | `CodeBlock` prepared and catch-buffer bits: two bit-fields of one byte written by several mutators | dfd696443b9b (`useLazyCatchLiveness`) | GIL off | confirmed (lost update, performance only) | fixed in the rebase |
| R9-11 | `JSPromise::forEachPendingReaction` loads the packed kind/flags/cell word three times | ed0763a0e51f | GIL off | needs-trace | fixed (walk copied under the cell lock) |
| R9-12 | `ensureCatchLivenessIsComputedForExecutedCatchesSlow`: two mutators build a buffer for one catch | dfd696443b9b | GIL off | confirmed (a leaked buffer) | fixed (CAS publication) |
| R9-13 | the OldAge exemption of `CodeBlock::jettison`'s stop assertion now covers optimizing code | 983055f83674 and the aging series | none today | hardening | fixed |
| R9-14 | `VM::m_startupJITDeferralScale`, `VM::m_asyncContextTrackingEnabled`: plain words read by every thread | dfd696443b9b, ed0763a0e51f | GIL off | hygiene (TSAN) | fixed |
| R9-15 | module loader: unlocked prelinked readers of `m_loadedModules`, the removed-record `BitVector`, the graph's identifier cache, `getModuleNamespace`'s check-then-store | dfd696443b9b and older | GIL off | traced (namespace publication: real race) | fixed (published once by compare-and-swap - a first version under the record's cell lock held it across a trap check, Debug assertion; the other structures are unreachable concurrently or dead GIL off) |
| R9-16 | `UnlinkedFunctionExecutable` and `CodeBlock` field layout | dfd696443b9b | all | build (static size assertions) | repacked in the rebase |
| R9-17 | `CallLinkInfo::forEachDependentCell` from `CodeBlock::collectProfiledCallees` (the global inlining planner) reads the mode, then `m_callee`, while GIL off another thread relinks the site | 5bcd90bdee79, dfd696443b9b | GIL off | confirmed (Debug corpus) | fixed (unvalidated read, sentinel filtered) |
| R9-18 | GIL off, weak-bearing blocks were never swept: every sweep that runs alongside mutators skips a block with a weak set (the sweeper, the allocator's own sweep, the steal), and nothing swept them with the world stopped, so destructible cells in them were never destroyed (found by this round's destructor-contract test; branch code) | branch | GIL off | confirmed (`gc-stress/destructors-run-once-on-any-thread.js`: 12,350 of 50,000 destructible cells never destroyed) | fixed (the conductor sweeps them at every cycle's end inside the stop; SPEC-heap §10E third amendment: 0 never destroyed, at most 10 blocks a cycle on the scaling workloads); amended: at most 32 per unrequested cycle end, all at a requested one - back-to-back Full cycles re-swept every weak-bearing block each time (SPEC-heap history §35) |
| R9-19 | The fast data-property copies (`globalFuncCloneObject`, `copyDataProperties`, `Object.assign`, the clone arms) store what they read from a source another thread is deleting from: an empty value (found by this round's amplifier on a CVE test; branch code, not in the rebase range) | branch | GIL off | confirmed (Debug assert) | fixed (post-read structure and empty-value check, generic path otherwise; SPEC-objectmodel history §30) |
| R9-20 | `VMManager` Mode-machine counters under the shared GC's stop (`StopReason::GC`): Bun GIL off with Workers and the keep-alive thread aborted with `RELEASE_ASSERT(m_numberOfStoppedVMs + m_numberOfBlockedVMs <= m_numberOfActiveVMs)`. Not the new base's `VMBlockingScope` (unreachable under useJSThreads: the SharedArrayBuffer wait takes the per-wait-node path first; gdb: 0 blocked counts in 146 stop windows) - a stopped count that outlives its stop epoch (`resumeTheWorld` leaves `m_numberOfStoppedVMs` to be decremented lazily, and the next stop recounts only entered VMs), with three candidate escapes: a representative that arrived outside any `VMEntryScope`, a sibling's exit decrementing the VM, a VM destroyed during a stop | the new base's STW series and the branch's §A.3.8 | GIL off (Bun Workers) | confirmed on the r9b and r9e trees under load (Bun `worker_threads`: 1 of 3, 1 of 5 runs); 0 of 28 later runs (the final tree's Bun, and 8 with the r9e Bun at lower load); not in the jsc shell (seven repros) | open: the counters' transition ring and dump ship to name the escape when it recurs; fix design in LANDING-PLAN Open items |
| R9-21 | The window-liveness constraint's precise-allocation leg (`Heap`, shared heap with two or more clients) took `isNewlyAllocated() && !isMarked()` as its witness, which a Full collection's `PreciseAllocation::flip()` sets for everything marked in the previous cycle: every precise allocation ever marked stayed alive while two clients were attached (found measuring map-heavy's memory; branch code) | branch | GIL off | confirmed (map-heavy 1.1 GB per allocating thread against 176 MB flag off; a worker's dropped 64 KB arrays all retained) | fixed (a witness bit flip does not set; SPEC-heap history §34) |
| R9-22 | Generated code (LLInt, Baseline IC, DFG, FTL) updated a published butterfly's public length with a plain store on hole stores, push, pop, shift, unshift and the length setter; GIL off a racing thread's CAS-max bump was lowered and its element hidden (found by the end-of-round amplifier campaign; branch code, older than the round) | branch | GIL off | confirmed (i03-n3 second half: 11 of 8,000 amplified runs on the final candidate, 2 of 8,000 on the round's first build; the element present after raising the length) | fixed (GIL off without the E2 elision every length raise is the CAS-max, inline in every tier and in `JSArray::pushInline`'s owner leg; lowers stay plain as in the runtime; the hole stays profiled; SPEC-jit §5.5, history §46): new test 5 of 5 runs fail -> 5 of 5 pass; in-bounds warm-up reproduction 947 of 1,500 rounds -> 0; i03-n3 amplified 11 of 8,000 -> 0; cost 1.06-1.09x instructions on length-raising loops once the sets have fired, none unfired or flag off |
| R9-23 | The shared GC's conductor runs destructors and weak finalizers inside its own stop window with its heap access released (the End phase's eager sweep of lower-tier precise allocations, the requested-Full sweep, the cycle-end sweep of weak-bearing blocks; R9-21 and R9-18 made dead cells' destructors actually run there with two clients attached); an embedder destructor that takes the VM's API lock (Bun's `Bun__JSValue__unprotect` -> `JSLockHolder`) re-ran the gated access acquire, saw the conductor's own stop pending and waited for its clear, which only the conductor performs: a self-deadlock with every other mutator parked behind it (found by the final battery's Bun GIL-off run of `test/js/web/fetch`; branch code, made reachable by R9-21 and R9-18) | branch | GIL off | confirmed (both JS threads in `GCClient::Heap::acquireHeapAccess`'s stop-pending wait, no conductor running; SPEC-heap history §36) | fixed (F8 conductor re-entry on the stop-pending and Mode-machine legs; §10G allows the API lock in destructors; SPEC-heap history §36): the new test hangs 3 of 3 before, passes 5 of 5 after; Bun `test/js/web/fetch` GIL off hangs 4 of 4 on r9y/r9u, completes 2 of 2 on r9za |

Rulings for R9-1, R9-3 to R9-6, R9-8, R9-9 and R9-11 to R9-14 are in SPEC-ungil
history, ninth round. R9-1 measured with a two-process driver (the first
process writes a persistent bytecode cache of 300 functions and classes, the
second loads it and six threads first-touch `name`, `toString()` and a call on
the same ones): GIL off 40 of 40 runs crashed on the rebased tree (a
`RELEASE_ASSERT` in `CachedCompactTDZEnvironmentMapHandle::decode` under
`materializeDeferredMembersSlow`, from a spawned thread's class `toString()`),
0 of 40 with the deferral off GIL off.

R9-1. Two threads that use the same function decoded from a bytecode cache
(`f.name`, `f.toString()`, a stack trace, bytecode generation) both see a
member deferred; the first `settle()`s the union from Pending to Live and
the second then reads `pending()` out of the Live bytes and destroys them
again (a Decoder reference dropped twice, a leaked RareData), while both
assign `m_ecmaName`. Only compiler and GC threads have a concurrent reader
(`tryGetEcmaNameConcurrently`, acquire), which is sound.

R9-2. `setActiveHeapAnalyzer` walked `symbolTableSpace` under a
`HeapIterationScope` on the calling mutator. On a shared heap its
`stopAllocating()` flushes other clients' free lists from under them
(MarkedSpace.cpp states it is not legal from a mutator while other clients
run). `w16-c1-prevent-collection.js` GIL off crashed in 15 of 20 runs on
the rebased tree. Fixed: the walk runs through
`Heap::runWithOtherClientsStopped`, and not at all when no table can be
pending (SPEC-heap history §30).

R9-3. One `Decoder` serves a payload's eager decode (under the compilation
lock), lazy SymbolTable entries (under that table's lock) and thin children
(no lock); two threads first-touching two functions of one module grow
`m_atomsByOrdinal` under each other or corrupt the maps, and the reference
count - taken during materialization, dropped by destructors on sweeping
threads - loses updates. The earlier row VM-12 rested on "every decode holds
the compilation lock", which the lazy paths broke.

R9-4. `cloneScopePart` (link of a CodeBlock for another global object, under
the compilation lock) reads `m_cachedEntries`, `m_cachedEntriesDecoder` and
`m_map` of the unlinked table without its `m_lock` while another thread
materializes that table under it.

R9-5, R9-6. The three-character atom cache is created without the creation
lock its two-character sibling has GIL off, and an entry's eviction
(`exchange`, then `deref`) races a reader returning the old entry; a lazy
`atomFor` can overwrite a slot that `jsStringFor` had set to a cell, which
the table's GC visit then marks as one.

R9-7. Several threads running an outer function's first `new_func` at once
each linked an executable, stored it with a plain `WriteBarrier::set` and
decremented the plain count of missing entries: two executables for one
declaration, a counter that wraps or stays above zero, and an unordered
publication of a fresh cell (TSAN: `InferredValue` of a FunctionExecutable
read by a thread that did not construct it). The rebased Debug corpus
asserted in four tests. Fixed: one compare-and-swap publishes the entry,
only its winner counts it (SPEC-ungil history, ninth round).

R9-8, R9-9. A prelinked module record sets `m_prelinkedEntriesMaterialized`
before it fills the import, export and star-export maps, so a spawned
thread's namespace access (records loaded on the main thread are reachable
from spawned threads, although `import()` is refused there) can iterate a map
being filled; the resolution memo is allocated and written without a lock.
Bun uses prelinked graphs for standalone executables.

R9-11. The walker loads the inline reaction kind, then the payload cell, then
the async-context flag; a `then()` on another thread installing the first
reaction between the loads makes it downcast the task cell to a reaction.
Whether the module loader's walk can meet a promise another thread attaches
to needs a trace; one snapshot of the packed word removes the question.

R9-13. All OldAge jettisons still come from the stopped end phase, so nothing
is wrong today; the assertion should require the stop for OldAge too.

Readers' checked-and-safe rows (one line each in the readers' notes, not
repeated): lazy ExpressionInfo (decoded under the block's lock, fenced
publication), lazy RegExp pattern construction (under the cell lock, atomic
character-class publication), `UnlinkedMetadataTable::m_is32Bit` (decided in
`link()` under the link lock), `m_isClass` (written before publication),
`ScriptFetchParameters` singletons (`call_once`, thread-safe counts), the
CodeBlock aging lease and counter snapshot (written by the collector under
the block's lock), the IC stub routine watchpoint keys (reconciled at GC
end), the FTL exit stub vector (appends under the exit-generation lock,
published through the concurrent-readers pointer), the megamorphic epoch
bumps (after the publish on every GIL-off arm), the new
`symbolTablePutTouchWatchpointSet` fire sites (the lock is dropped before
the fire).

## 9. The tenth round's rebase range

The branch was rebased onto `cf1b36ec8703` (the commit Bun pins). The range
`dfd696443b9b..cf1b36ec8703` is one upstream merge (#614, up to upstream
`ccdcb8a026c0`) and one fork commit (#522, additional module loaders); 69 of
its commits touch Source/JavaScriptCore, Source/WTF or Source/bmalloc. This
section is the reading of those 69, against the merged tree (the branch's
delta taken over the new base); every site listed was re-read in the merged
tree. What the round then built and measured for each row is at the end
(Outcome). The merge commit's own hand
resolutions under Source/JavaScriptCore (`JSBigInt`'s `InterruptCheck`
plumbing, include spelling) predate the range and were not re-audited.

One commit changes the cost model under every wait loop the branch added
(R10-1 to R10-4). Two add code that is memory-unsafe with the GIL off
(R10-5, R10-6). #522 is **not** behind an option in its final form - the
long squash message describes an earlier design (`useModuleGraphInstances`)
that the later fix-up commits replaced - so its import slots, shared
`ModuleProgramExecutable`s and `@moduleLoader` global run in every
configuration (R10-12).

| Row | Site (merged tree) | Upstream | Affects | Risk | Ruling |
|---|---|---|---|---|---|
| R10-1 | 20 branch wait loops that poll with `Thread::yield()` (list below) | 249ede976fcc (`Thread::yield()` on Linux is `nanosleep(1 ns)`, about 50 us with the default timer slack; upstream's own loops moved to `WTF::SpinBackoff`) | GIL off (two sites GIL on) | performance and stop latency, not safety: a wait that used to cost a `sched_yield` (about 1 us) now sleeps a timer-slack period per iteration | needs a fix: `SpinBackoff::spinOnce()` at every tryLock/poll site (x86-64 Linux: 1+2+4+8+16+32 `pause`, then the yield), per the table below |
| R10-2 | `jsThreadGILHandoffYield` (LockObject.cpp): `GILDroppedSection` + `Thread::yield()`, called by every `Condition.prototype.notify/notifyAll` and by `jsThreadYieldForPendingParkResumptions` | 249ede976fcc | GIL on and GIL off | performance: every `notify()` by a thread that holds the API lock (or its GIL-off token) now sleeps about 50 us with the lock dropped | needs a measurement and a decision (keep the sleeping yield: a surer handoff; or a bare `sched_yield`: the old cost). Not a `SpinBackoff` site - the yield is the point |
| R10-3 | `RaceAmplifier` (RaceAmplifier.cpp): three perturbations in four are "a bare yield" | 249ede976fcc | tooling (amplifier, 500-run and 2 h campaigns) | the bare yield became a 50 us sleep: fewer perturbations per second, coarser interleavings than the option text (`randomYieldPeriod`: "sched_yield/short sleep") promises | needs a fix in tooling: call `sched_yield()` directly on Linux for the bare leg; keep the randomized sleep leg |
| R10-4 | `WTF::Lock` / cell-lock `lockSlow` (LockAlgorithmInlines.h), `WordLock`, `bmalloc::Mutex`, `JSLock::grabAllLocks`, `Heap::resumeThePeriphery` | 249ede976fcc | all (base behaviour) | x86-64 Linux: a contended `Lock` now spins 63 `pause`s (1-3 us) and parks, where it made 40 `sched_yield` calls (20-40 us) first; more park/unpark traffic on the GIL-off contended locks (cell locks, MSPL, structure locks), and every lock-free-then-yield loop upstream kept sleeps | documentation only: `main` changes the same way, so ratios against `main` hold, but numbers against earlier rounds' trees (scaling at 4 threads, string-heavy's bimodality) are not comparable across this base |
| R10-5 | `objectValues` fast path (ObjectConstructor.cpp) | f37e63364972 (fills the result array directly: one pass counts, `JSArray::tryCreate`, a second pass stores through `values.at(result, index++)`) | GIL off | confirmed by reading, memory-unsafe: the second pass re-reads `target->structure()` and re-walks the indexed storage; a thread that adds a property or pushes between the passes makes it store past the butterfly (`ContiguousData::at` checks the index only in Debug). Fewer properties leave holes and trip `ASSERT(index == result->length())` | needs a fix (below) |
| R10-6 | `operationArray{Includes,IndexOf}Double`, `operationArray{Includes,IndexOf}NonStringIdentityValueContiguous` (DFGOperations.cpp), reached from the new `vectorized` leg of `compileArrayIndexOfOrArrayIncludes` in DFG and FTL | 0d6cf47c3b12 (vectorized `indexOf`/`includes` from 32 elements up) | GIL off | likely, memory-unsafe (out-of-bounds read): the FTL clamps the length it scans inline to the storage's own vectorLength (`publicLengthForBounds`, SPEC-jit history §39), but the new leg passes the raw storage to an operation that re-reads `butterfly->publicLength()` unclamped; on a hoisted flat butterfly that a foreign thread converted to segmented storage and grew, that length is the segmented array's and runs past the flat allocation | needs a fix (below) |
| R10-7 | `JSArray::fastCopyWithin` | ae97aa471a12 (no hole check; `RELEASE_ASSERT(vectorLength >= length)`) | flag on | none: the merge takes the butterfly from `jsThreadsFlatSnapshot(InPlaceWrite)` and refuses `length > snapshot->vectorLength()` before the assert; moving holes is legal in all three shapes | covered |
| R10-8 | `JSArray::setLength`, `shiftCountWithAnyIndexingType`, `JSObject::increaseVectorLength` | ccdcb8a026c0 (Contiguous slots cleared with `gcSafeZeroMemory`; Int32/Undecided keep a `clear()` loop the compiler may turn into `memset`) | GIL off, musl only | hardening: the marker does not scan Int32 lanes, but with the GIL off other mutators read them; a libc `memset` with sub-8-byte edge stores (musl) can show a reader a half-cleared lane. The in-place `increaseVectorLength` arm is refused flag on (AS-COPY) | documentation only (candidate: `gcSafeZeroMemory` for every shape flag on); the merge keeps the snapshot's `clearFrom` bound in both arms |
| R10-9 | `RegExp::m_minimumSize`: written by the compile paths, read by `matchInline` and by DFG/FTL `RegExpTest` (`emitRegExpMinimumLengthFilterGuards`, `Graph::tryGetConstantRegExpTestMinimumSize`) | 34141654c7a5, fea77037ca1f | GIL on and off | none: a per-pattern constant, zero until the first compile and after `reset()`/`deleteCode()`; zero and the final value are both correct to read. The merge writes it under the cell lock with a relaxed atomic (`setMinimumSize`) and reads it through `minimumSize()`; the JIT reads the aligned word plainly; the compiler thread reads it under the cell lock together with `hasCode()` (same shape as `matchConcurrently`) and a baked constant never goes stale | covered by the merge |
| R10-10 | `RegExpTestInline` for unicode patterns and patterns with a Yarr frame; `getRegExpJITCodeBlockConcurrently`; `ensureRegExpJITCode`'s fence; validity check in strength reduction | 83683abb036d, 111d7edcecd1, eb64f293d84c, d5531cc083bc | GIL on (as `main`) | none new: GIL off `DFGStrengthReductionPhase` still refuses `convertTestToTestInline()` and both emitters keep their fail-stop; the inline frame now lives in the caller's outgoing-argument area, so the stack check the branch had re-pointed per thread (`branchPtrAgainstSoftStackLimit`) is gone with the block upstream deleted; `isValid()` goes through the branch's atomic `constructionErrorCode()` | covered |
| R10-11 | `YarrPattern::sharedCharacterClass<>` (ten process-wide built-in classes) | 6279ebe3bd2d | GIL off (concurrent pattern construction) | none: `std::call_once` into a `LazyNeverDestroyed`, handed out as `const CharacterClass*`; `m_characterWidths` and `m_table` are only assigned by the creators; no `const_cast` or `mutable` in yarr/ | covered |
| R10-12 | module loaders: `JSModuleEnvironment` import slots (`fillImportSlot`), `op_resolve_scope` `ModuleVar` metadata (`m_moduleImportSlot`), `ModuleProgramExecutable` shared between records (`functionDeclaration`, `JSGlobalObject::moduleProgramExecutables()`), `@moduleLoader` static global | cf1b36ec8703 (#522; always on) | GIL off | traced, no unsafe race found; two hygiene points (below) | covered, with a TSAN note and one rule to keep |
| R10-13 | `CodeBlock::finishCreation` invalidates the put_to_scope watchpoint set of `ResolvedClosureVar` writes in generator and async bodies at link time | e6507c8f6aa8 | GIL off | none: the fire goes through `WatchpointSet::fireAllSlow`'s central Class-A protocol (SPEC-jit §5.6) like the `ClosureVar` invalidate two cases below it, and upstream moved it outside the symbol table's lock (PRE-15's rule) | covered |
| R10-14 | `SlotVisitor::drainFromShared`: `m_numberOfWaitingParallelMarkers` decremented by a scope exit on every path | 366915db5c7a | flag on (marker pause) | none: the merged function is balanced - the scope exit replaces the old end-of-block decrement (removed by the merge), the pause loop's `waiting--`/`waiting++` pair sits inside the scope, and all four returns still decrement `m_numberOfParallelMarkersInDrainFromShared` | covered; two comments are now wrong (Heap.cpp `runEndPhase` "deliberately not asserted ... stays incremented across returns", Heap.h "Balanced on every return, unlike m_numberOfWaitingParallelMarkers") |
| R10-15 | `Heap::isPagedOut`, `MarkedSpace::isPagedOut`, `BlockDirectory::updatePercentageOfPagedOutPages` | 4577b17ad891 (removed) | none | none: no reference left in Source/; the branch's shared-heap gate in `FullGCActivityCallback::doCollection` went with it | covered; AUDIT-heapcontainers rows that name `isPagedOut` (S-01) are moot |
| R10-16 | `WTF_DECLARE_OWNER_THREAD_ASSERTIONS`, `WTF_GUARDED_BY_LOCK` annotations, `assertIsCurrent(const RunLoop&)` as a release assertion | 0f3121279944, b982ebd3147d, 6f1389789211, f2535d35a033, 19cfe1ed983a | none new | not reachable: no JSC or WTF class adopts the owner-thread macros (WebCore only); the annotations are compile-time (the merged Release build has no `-Wthread-safety` diagnostic); the only `assertIsCurrent(RunLoop)` callers in this port are `RunLoop::TimerBase::stop`/`~TimerBase` on an active timer, which were already release assertions in the old base | covered |
| R10-17 | `tryCacheGetBy`/`tryCachePutBy` give up on customs of once-flattened dictionaries; `PropertySlot` carries the custom's offset | db10a0f61674 | none | caches less, shares nothing new | covered |
| R10-18 | `EvalFunctionCallNode::emitBytecode` emits a tail call when the callee is not the built-in `eval` | 4fa7b55ae4c6 | none | bytecode generator only; existing opcodes (`tail_call`, the not-eval-function jump); no new call-link shape | covered |
| R10-19 | `JSBigInt` Karatsuba multiplication, explicit `RELEASE_ASSERT` bounds hints | cbd15fc88bde, f52d0e486a0e | none | scratch is a function-local `Vector`; BigInts are immutable; the branch's GIL-off bypass of `VM::m_cachedBigIntDivisor` in `remainderImpl` is intact | covered |
| R10-20 | `JSModuleLoader::importModule` rejects disallowed module types | 64168e4d90c4 | GIL off | none: the branch's refusal of `import()` on a spawned thread still runs first | covered |
| R10-21 | `slow_path_resolve_scope` reads `metadata.m_resolveType == ModuleVar` plainly before the branch's atomic load of the same byte | cf1b36ec8703 | hygiene | none: flag on the metadata is frozen after link, flag off there is one mutator; `operationResolveScopeForBaseline` and the LOL twin load it once atomically in the merge | documentation only (make the three alike) |

Not relevant, read and dismissed together: WebCore, WebKit and CSS changes
that touch WTF only through preference plumbing (da79705ade01, ab7189f839e9,
edc74c3eeb03, fcef99916e4a, 6f6d92eda135, 2391a5cb885b, 194d557d4fa6,
3e12766ced21, 4b78955e0ded, 3821585ac8da, dfc530cd33ad); Swift and
lifetime annotations (7e1a50ea0894, d0088e0c11f0); `CStringWithEncoding`
(5f14e32e5760); ARM64-only code generation (a0042af34845, 54c30c45bed4,
0ddad2a1c101); Windows (3856a285807d); Wasm, which is off with the GIL off
(1b46b208061c, ca158eb19b75, 6ae2e2fec95a, 10d4d20f5b6c, ce4906cf9bde); the
allocator unification and its reverts, net zero (fe81aba19836, 03c6c9504495,
ce29fcc8c7cb, 10f4e9581e88); pure arithmetic and parsing (566279d48d56,
8e7f96f9a5db, d5ba0ecec9c4, 41294576aca9, c8c37314ee17, b44e00d2f037,
1a29b8cfcf8d, 549955d9979c, 956f6fb49678, b6afe0d39e0a); mechanical
(b091600bcaa7 register enums, e2f1a867ae69 `CodeOrigin` moves, 72ec90eba341,
79282f6ac8df, 67c93bbc4bcd, c0121e11c12f).

### R10-1. The branch's wait loops after `Thread::yield()` became a sleep

Every site is a stop-participating poll: `while (!tryLock()) { park for a
pending stop; Thread::yield(); }` or a spin on a word another thread is about
to publish. None is wrong now; each costs a timer-slack period per iteration
where it cost a `sched_yield`.

| Site | Waits for | Expected wait | Replacement |
|---|---|---|---|
| `JSObject::getDirectConcurrently`-style readers: JSObject.cpp (nuked `StructureID`, and the hole under an unchanged structure), JSObject.h (the same, inline) | a publication between its nuke and its new ID (M5); a pinned-table edit | nanoseconds | `spinOnce()` - the six pause steps cover it; first priority, these are on GIL-off property read paths |
| `tryReadLockFreeGILOff` (JSOrderedHashTableHelper.h), four attempts | a `Map`/`Set` writer's odd version (one `set`/`delete`; a rehash at worst) | 100 ns to microseconds | `spinOnce()`: four attempts are 1+2+4+8 pauses and never reach the yield; today a reader that meets a writer sleeps up to 200 us before taking the locked path |
| `Heap.cpp` stop walk: `while (client.hasHeapAccess())` flicker wait | a client's CAS and notify inside a stop window | nanoseconds to a few microseconds, and it is stop latency | `spinOnce()` |
| `ErrorInstance::materializeErrorInfoIfNeeded` GIL off | another thread building the stack string | microseconds | `spinOnce()` |
| `FunctionRareData` (three loops), `JSFunction` lazy `prototype`, `ClonedArguments` and `DirectArguments` lockers, `JSObject.cpp` prototype-cycle lock, `GILOffFirstUseLocker` (JSThreadsSafepoint.h; every first-use site shares it) | a short first-use section that may allocate | microseconds | `spinOnce()` |
| `lockGILOffCompilationLockContended` (ScriptExecutable.cpp), `osrExitGenerationLock` (DFGOSRExitCompilerCommon.cpp), `HeapProfiler::acquireBuilderGILOff`, `Heap` collect-continuously lock, `PerEventStopClaim` (ConcurrentButterfly.cpp) | a real hold: parse and link, an exit ramp compile, a snapshot, another thread's stop window | tens of microseconds to milliseconds | `spinOnce()` (it degrades to the same sleeping yield, which suits a long hold; the pause steps win the short ones) |
| `JSThreadsSafepoint.cpp` watchdog dump: 100 bounded attempts on the registry lock | fail-stop diagnostics | - | leave (5 ms worst case before the crash it already is) |
| `SharedHeapTestHarness.cpp` (six) | test harness | - | leave |

Test idea (JSTests/threads/sync or objectmodel, `//@ threadsNoAmplify`): N
readers `get` from a shared `Map` while one writer `set`s in a loop; count
reads per second and the `mapReadLockFreeGILOff` fallbacks before and after;
the same shape for a constructor's first `new` on N threads
(`FunctionRareData`). These are timing ratios, so report medians, not a
pass/fail bound.

### R10-5. `Object.values` stores past its result array, GIL off

Flag off and GIL on the two passes agree: nothing between them runs JS or
hands the GIL over (`JSArray::tryCreate` can collect, not call out). GIL off
another thread can add a named property (a new structure, or an in-place add
on a dictionary), `push` onto the target, or delete. Then:

- more properties in the second pass: `values.at(result, index++)` has only
  a Debug bound, so Release writes past `result`'s butterfly (the vector may
  have a few slots of rounding slack, no more);
- fewer: trailing holes and the Debug `ASSERT(index == result->length())`;
- a value read by `getDirect(entry.offset())` from a slot a racing delete
  emptied is an empty value in a Contiguous array (a hole; the R9-19 class,
  benign here).

Fix, in the fast path only: take `Structure* targetStructure` once and use
it for both named passes (so the named count is fixed by construction for a
non-dictionary; refuse the fast path for dictionaries GIL off, whose tables
change in place); bound the indexed second pass by `indexedPropertyCount`
(stop the walk when `index` reaches it); after the fill, GIL off, re-check
`target->structure() == targetStructure` and that no stored value is empty,
and fall to the generic path otherwise. Or, as a first step, skip the fast
path when `vm.gilOff()`. Test: `shared-objects/object-values-vs-add-gil-off.js`
- one thread calls `Object.values(o)` in a loop and checks
`values.length === Object.keys(o).length` is never exceeded by garbage, while
another adds and deletes named properties and pushes and pops indexed ones;
Debug asserts on the unfixed tree, Release with `GIGACAGE_ENABLED=0` or ASan
faults.

### R10-6. The vectorized `indexOf`/`includes` leg re-reads the length

SPEC-jit history §39: GIL off the FTL may hold a `GetButterfly` result across
polls, a flat butterfly that a foreign thread has since converted keeps
serving the lanes below its frozen vectorLength, and its publicLength slot is
the segmented array's and can grow past that. So every bound the FTL takes
through a storage edge is `min(publicLength, vectorLength)` of the same
storage, and `compileArrayIndexOfOrArrayIncludes` does that for its inline
loop. The new leg (taken when 32 or more elements remain) calls
`operationArray{Includes,IndexOf}Double` or
`...NonStringIdentityValueContiguous` with `(storage, searchElement, index)`,
and each reads `butterfly->publicLength()` again and hands `length - index` to
`WTF::findDouble` / `WTF::find64`. The DFG's leg is the same call with a
storage loaded in the same block. The older sibling operations
(`operationArrayIndexOfString`, `...ValueInt32OrContiguous`, `...ValueDouble`,
reached from the String/Object/Other use kinds and from the FTL's slow cases)
read the length the same way; they predate the range and are the same fix.

Fix: one helper next to these operations - flag on, `std::min(publicLength,
vectorLength)` of the butterfly it was given (the form `JSObject.cpp`'s
`usedLength` already uses) - and every operation in the family takes its
length from it; flag off it is `publicLength()` as today. Alternatively pass
the clamped length from the JIT as a fourth argument. Test:
`jit/array-indexof-vectorized-stale-storage-gil-off.js` - an FTL loop calls
`a.indexOf(x)` / `a.includes(x)` on a shared Double array of 64 elements (so
the storage is hoisted and the vectorized leg runs), a second thread makes
the first foreign hole store (flat to segmented) and pushes until the length
passes the old vectorLength; assert every result is -1/false or an index
whose element equals `x`; run under ASan (the read past the flat allocation
is what faults).

### R10-12. #522, module loaders

What runs in every configuration now: a `ModuleVar` `op_resolve_scope` loads
the exporting environment from the importing module environment's import
slot (`m_moduleImportSlot` in the metadata, set at link and never rewritten;
a constant resolve type); LLInt, Baseline and LOL branch to the slow path on
an empty slot and the DFG emits `GetClosureVar` + `CheckNotEmpty` (or folds a
filled slot through the symbol table's singleton, watched).
`JSModuleRecord::evaluate` fills the slots before the body runs; the slow path
(`JSModuleEnvironment::fillImportSlot`) fills one on first use for code that
runs before its module is evaluated (a hoisted function across a cycle).

- Two threads can fill one slot at once GIL off. Both resolve the same
  import of the same record and store the same environment, published long
  before; the racing plain `WriteBarrier::set` and the plain `get` are
  benign on every target but are a TSAN report waiting for a test that does
  it. If it shows up: a relaxed atomic store/load pair GIL off in
  `fillImportSlot`/`importSlot().get()`.
- `importSlotNames()` builds `m_importSlotNames` lazily without a lock, but
  its first call is `JSModuleEnvironment::create` at link time, before any
  code of the module can run; later calls only read.
- `ModuleProgramExecutable::functionDeclaration` is check, link, then store
  under the cell lock (two callers could link two executables); it is called
  from `CyclicModuleRecord::initializeEnvironment` and from the eager arm of
  `CodeBlock::finishCreation` for module code, both on the loading thread.
  Lazily materialized declarations still go through the ninth round's
  compare-and-swap (`CodeBlock::materializeFunctionExecutable`), now shared by
  every record that shares the executable.
- `JSGlobalObject::moduleProgramExecutables()` (a `WeakGCMap`) is read and
  written only by `getOrMakeExecutable` on the loading thread and pruned at
  the end of a collection, world stopped.
- The rule all of the above rests on: with the GIL off only the main thread
  loads modules, because `JSModuleLoader::importModule` refuses a spawned
  thread before anything else (still first in the merged function, ahead of
  64168e4d90c4's module-type check). Additional loaders (`JSModuleLoader::create`
  with a module scope, `$vm.createModuleLoader`, whatever Bun builds on it)
  must keep entering through `importModule`; an embedder entry point that
  calls `loadModule`/`requestImportModule` on another loader from a spawned
  thread would bypass the refusal.
- The branch's R9-15 compare-and-swap in `getModuleNamespace` still publishes
  the right field: namespaces stayed per record (`m_moduleNamespaceObject`,
  `m_deferredNamespaceObject`); the per-(record, instance) namespaces of the
  squash message's first design did not ship.

### Textual merges that need a second look

- `FTLLowerDFGToB3.cpp` `compileArrayIndexOfOrArrayIncludes`: the branch's
  `publicLengthForBounds(storage)` and upstream's new `vectorized` block
  merged side by side without conflict; the combination is R10-6.
- `SlotVisitor.cpp`/`Heap.cpp`/`Heap.h`: correct code, stale comments
  (R10-14).
- `CommonSlowPaths.cpp` `slow_path_resolve_scope`: upstream's early
  `ModuleVar` test reads the byte plainly above the branch's atomic load
  (R10-21).
- `RegExp::updateMetadataFromPattern` (hand-merged): first-set-wins atom,
  relaxed `m_specificPattern`, conditional `m_numSubpatterns` store, the
  fork's early return when `m_rareData` exists - consistent with the compile
  paths holding the cell lock; nothing to change.
- `JSArray.cpp` `shiftCountWithAnyIndexingType` and `setLength`: upstream's
  shape split sits inside the branch's single-snapshot arms and uses the
  `indexingType` local read once; consistent.

### Priorities

Needs a code fix before the round-start ledger's GIL-off lanes can be
trusted for these paths:
1. R10-5 `Object.values` two-pass fill (GIL off, heap overflow).
2. R10-6 vectorized `indexOf`/`includes` operations re-read an unclamped
   length (GIL off, out-of-bounds read; FTL §39).

Needs a code change for performance, measured before and after (none is a
safety issue; the ledger's behaviour lanes stand without them, its GIL-off
timings and scaling numbers do not):
3. R10-1 `SpinBackoff::spinOnce()` at the branch's 20 poll sites, the
   nuked-ID readers, the `Map` version retry and the stop-walk flicker wait
   first.
4. R10-2 `Condition.notify`'s handoff yield: measure GIL on and GIL off, then
   decide between the sleeping yield and a bare `sched_yield`.
5. R10-3 the amplifier's bare-yield leg (tooling; affects campaign
   throughput and what "500 amplified runs" exercises).

Needs only a test (or a TSAN run that exercises it):
6. R10-12 two threads first-touching one import slot (a cycle with a hoisted
   function), GIL off under TSanJIT.
7. R10-9 one thread `test()`ing a lazily constructed RegExp while another
   compiles it (the existing RegExp corpus arms should cover it; confirm no
   TSAN report on `m_minimumSize`).

Documentation only: R10-4 (numbers across this base are not comparable with
earlier rounds'), R10-8 (musl `memset` note), R10-14 and R10-15 (stale
comments and AUDIT-heapcontainers rows), R10-16, R10-21, and the corrected
description of #522 (always on; the option in the squash message does not
exist).

### Outcome (what the tenth round did with the rows)

- R10-5: fixed as described (`tryObjectValuesFastGILOff`, SPEC-objectmodel I42 / history §35). Test
  `shared-objects/object-values-vs-add-gil-off.js`: Debug GIL off 3 of 3 runs assert on the round's first build
  (`index == result->length()` twice, the contiguous storage's `index < m_length` once); 0 of 3 after; Release passes on
  both (the out-of-bounds store is silent there).
- R10-6: the clamp shipped (`searchableLengthOfStorageFromJIT`, SPEC-jit history §47) with a counter
  (`searchOperationClampedStaleStorageGILOff`) and the test `jit/array-indexof-vectorized-stale-storage-gil-off.js`
  (200 arrays, each searched by its own DFG-compiled function while a second thread converts it by a store past its
  capacity). The stale length was NOT observed: the test passes on the round's first build too, and the counter reads
  0 after the fix in every run tried. The reading of why: the DFG re-loads the storage in the same block as the
  operation call (GIL off `ArrayIndexOf` clobbers the heap, so no earlier butterfly load is reused), which leaves a
  window of a few instructions per search, and the conversion of a flat array another thread owns publishes its
  grown length through a fresh spine when the aliased spine has a partial tail fragment (mode (b), a copy), so the old
  header is raised only in the full-coverage case. The clamp costs one predicted branch flag on and nothing flag off;
  it stays as the operations' statement of §39's rule. Kept open as "not reproduced".
- R10-1, R10-2, R10-3: every listed poll loop uses `WTF::SpinBackoff::spinOnce()` (the watchdog dump and the C++ test
  harness excepted); the GIL/token handoff yield and the amplifier's bare yield call `sched_yield()` on Linux
  (`jsThreadsYieldToScheduler`). Measurements in PERF-RESULTS (tenth round).
- R10-12: no code change; the rule (only the main thread loads modules GIL off, every loader enters through
  `importModule`'s refusal) is restated in LANDING-PLAN Open items for Bun's additional loaders.
- R10-14: the stale comments were rewritten and `runEndPhase` now asserts `m_numberOfWaitingParallelMarkers` is zero
  with the other two counters.
- R10-21: `slow_path_resolve_scope` loads the resolve type once, atomically, like the two operations.
- The other rows needed nothing.

Two rows added by measurement later in the round (neither was found by reading the range):

- R10-22. **Inline allocation sites that bake `allocatorForConcurrently`** (SPEC-jit §5.5 "Inline allocation GIL
  off", history §54). Affects GIL off, performance only (the baked allocator is empty there, so the site is an
  unconditional slow-path call; nothing is unsafe). Found as `operationStringSubstr` in a GIL-off profile of
  `str.slice(a, b)`: 292 instructions per call against 140 GIL on. Sites, all upstream-written and none carrying the
  lite-relative arm the earlier rounds gave MakeRope / NewObject / NewArray / MaterializeNewObject: FTL
  `compileStringSliceOrSubstring` (the substring rope; came with the ninth round's range), FTL BigInt-from-Int64
  and `AssemblyHelpers::emitAllocateJSBigInt64`, DFG `compileRegExpStringIteratorNext`'s result object. Fixed:
  each resolves its allocator through the thread's table (`tlcAllocatorOrLegacy` / `tlcSlotForConcurrently[WithIso]`
  + `emitLoadTLCAllocatorForSlot`); slice 156 instructions per call after. How to find the next one: grep the three
  JIT directories for `allocatorForConcurrently<` and check that each hit sits in the non-GIL-off arm of a
  `vm().gilOff()` test (the list at the end of the round: AssemblyHelpers.h two, DFGSpeculativeJIT.cpp five,
  DFGSpeculativeJIT64.cpp one, FTLLowerDFGToB3.cpp eight - all paired).
- R10-23. **ThreadSanitizer and upstream's own benign races, GIL on.** Once a GIL-on process runs `main`'s object-model
  and interpreter paths (SPEC-objectmodel G1), the TSan lane runs `main`'s code, which was never TSan-clean: two
  reports surfaced in the corpus. `GetByIdModeMetadata::setProtoLoadMode` stored the ProtoLoad cache word plainly
  while a Baseline compiler thread read the mode byte it overlaps (now a relaxed atomic store: the same instruction);
  IPInt's `BaselineData` tier-up counter against the BBQ compiler thread (upstream, flag off too; suppressed by name
  in `Tools/tsan/suppressions.txt` with its reason). TSan builds also keep polling traps forced (SPEC-jit history
  §53). Nothing else in `main`'s paths was reported by the corpus GIL on.

- R10-24. **The multi-slot readers R9-19 did not reach** (SPEC-objectmodel history §41). Found by the end-of-round
  amplifier campaign: `cve/mc-val-multislot-clone.js` GIL off, signal 11, in `Stringifier::appendStringifiedValue` on
  an empty value read from a slot of a cacheable dictionary between a writer's in-place table edit and its value
  store. Present on the round's first build at the same rate (about 0.8 % of amplified runs on a loaded machine; the
  window is a few instructions wide and opens only when the scheduler takes the writer off its core inside it), so
  not introduced by the rebase or the round. Same read, same fix (re-check the structure ID and non-emptiness after
  the read, GIL off, else the generic path) in: the general JSON stringifier (both arms), `FastStringifier`'s object
  arm, the `Object.entries` fast path, the `Object.defineProperties` fast path. How to find the next one: grep
  `runtime/` for `getDirect(entry.offset())` and `getDirect(offset)` next to a `forEachProperty`; every hit that can
  see an object another thread writes must be followed by `dataPropertyReadsStillValid` or an equivalent per-value
  check under `vm.gilOff()` (at the end of the round: ObjectConstructorInlines.h, ObjectConstructor.cpp five,
  JSGlobalObjectFunctions.cpp two, JSONObject.cpp three; `toPropertyDescriptor` treats empty as absent;
  `renumberPropertyOffsets` runs under a stop; `analyzeHeap` tests the value). The window itself was closed at its
  source afterwards: the dictionary leg of `putDirectInternal` stored the value after `Structure::
  addOrReplacePropertyWithoutTransition` had published the entry (every other flag-on add stores inside the add
  callback, before publication); on a reused slot the window showed a residue value under the new name to every
  reader, generic ones included. How to find the next one: every flag-on caller of `addPropertyWithoutTransition` /
  `addOrReplacePropertyWithoutTransition` must store its value inside the callback.

- R10-25. **Destroying a VM after join** (SPEC-api §4.6 item 4, history r10.2). Found by the mirror harness once it
  ran memory-hog tests with their own options (`--destroy-vm`): the native thread of a joined Thread drops its
  closure's `Ref<VM>` only after its exit tail, so the embedder's release was not always the last and `~VM` ran
  off-lock on the exiting thread (MC-TDWN S1's fail-stop; a silent SIGABRT in Release). Present on the round's first
  build. ThreadManager counts, per VM, native threads that have published completion and not yet released;
  `waitForCompletedThreadsToReleaseVM` is the embedder's barrier; the shell calls it. For the next rebase: any new
  path that destroys a VM which ran Threads (Bun's Worker termination) needs the call.
- R10-26. **The taint hint was sticky GIL off** (SPEC-ungil history, K4.II.15): one VM byte written by every thread's
  prologues and never cleared GIL off, so every thread answered IndirectlyTaintedByHistory once any tainted code had
  run (`taintedness-tracking.js`, sixteen configurations). Per lite now, in the three tiers and the C++ accessors.
  How to find the next one: `SPEC-ungil-audit-K4.md` rows ruled "per lite" whose implementation note says "sticky",
  "shared" or "VM-wide GIL off".
- R10-27. **Deferred watchpoint claims and the thread that comes second** (SPEC-jit §5.6 "Deferred claims in flight",
  history §55). Found by the mirror harness (two threads released together): a mutator that finds a structure's
  transition set already claimed by another thread's deferred fire publishes its own transition with nothing to fire
  and returns into optimized code nobody has invalidated yet; with a relabel out of Double that code stores raw
  doubles into boxed lanes, with a kind change it reads a GetterSetter as data. Present on the round's first build.
  Relabels out of Double, conversions to ArrayStorage and kind changes wait out other threads' claims in flight,
  then publish and fire in one stop. How to find the next one: a new flag-on transition that changes the
  representation of a slot or lane, or moves storage to another layout, under an unchanged butterfly-word protocol,
  must follow SPEC-jit §5.6's four steps (`WatchpointSet::awaitDeferredClaimsInFlight` before deriving the target,
  the re-plan check after, `DeferredStructureTransitionWatchpointFire::fireNow` inside the publishing stop).
- R10-28. **GIL off, a requested synchronous JIT and the sampling profiler were refusals where the specs asked for
  behaviour** (SPEC-jit history §56; SPEC-ungil §A.1.7 form (i), history "the sampling profiler samples the
  carrier's lite"). `--forceEagerCompilation` (and `--useConcurrentJIT=0`) exited with a FATAL line GIL off, 234
  results of the JSC suite; the sampling profiler never sampled, some 170 results and Bun's `bun:jsc` `profile()`.
  The requesting thread now waits, parked, for its plan on the concurrent JIT and completes it; the profiler reads
  the bound carrier's lite through the registry (tryLock, target suspended) and turns raw frames into traces under a
  stop of the other clients. `--useProfiler` stays refused. How to find the next one: a GIL-off branch that logs
  and returns, or logs and crashes, where GIL on does work - grep the GIL-off predicates next to `dataLogLn`.
- R10-29. **A GIL-off-only function that leaves the exception-check validator's need-check bit set** (N.1 Map and
  Set). `fillTableGILOff` hashes each copied key with `jsMapHashForAlreadyHashedValue`, whose throw scope simulates
  a throw on the way out like every throw scope; `main`'s copy loop checks after each hash, the GIL-off one did not
  (it cannot return early: it runs under the table's cell lock). Nothing is wrong in a build without the validator;
  with it (`validateExceptionChecks`, Debug) the second hashed key aborts, which is what two of Bun's tests that
  run fixtures under the validator reported GIL off. The loop asserts "no exception" after each hash. How to find
  the next one: run the Debug corpus with `JSC_validateExceptionChecks=1` in the environment (the tenth round did, in
  both modes; results in LANDING-PLAN); every function the branch added that calls into a throw-scoped helper must
  check or assert before the next scope is entered.
- R10-30. **The sampling profiler's bound thread can be a dead Thread** (GIL on; SPEC-ungil history, tenth round,
  "The binding and a Thread's death"). With the GIL on every spawned Thread binds as the sampled thread when it takes
  the API lock, and nothing unbound it when it exited: the next sample waits forever for a terminated thread's
  acknowledgement, holding the machine-threads lock, and the main thread hangs taking the API lock back. Present
  since the flag existed (the ninth round's final tree hangs the same way); found by the amplifier on the GIL-off
  profiler's new test. An exiting Thread unbinds itself. How to find the next one: anything that remembers a
  `WTF::Thread` to signal or suspend later (not through `ThreadGroup`, which forgets a thread when it exits) needs a
  hook at Thread exit.
- R10-31. **Profiling the threaded forms withhold, read as "never ran" or reset at every tier-up** (performance, not
  safety; SPEC-jit history §57, §58). Two places where a flag-on restriction fed the DFG a profile that could not
  converge: the LLInt's disabled `put_by_id` transition cache, which `PutByStatus::computeFromLLInt` reads as
  NoInformation and the parser turns into `ForceOSRExit` (open: the parser rule that was tried regressed
  `scaling/richards-like.js` and was withdrawn, §57); and the refused in-place upgrade of a polymorphic call stub's
  slot GIL off, whose fallback reset the call site's variant list and counts at every callee tier-up (fixed, §58).
  Exit counts per JetStream test GIL off against GIL on found both (they do not depend on machine load). How to find the
  next one: `--printEachOSRExit=1`, group by function, bytecode index and exit kind, and diff the two modes; a site
  whose count is a large multiple of a hundred GIL off only is a profile that is not learning. For the next rebase:
  any new upstream use of LLInt metadata as DFG profiling (`*Status::computeFromLLInt`) has to be checked against the
  §4.3 table's disabled rows.

**Sites keyed on `useTaggedButterflies` instead of `useJSThreads` (G1; SPEC-objectmodel history §39, SPEC-jit
history §52).** For the next rebase: a conflict in one of these is resolved towards "flag-off body when untagged".
Runtime (every `Options::useJSThreads()` test except the thread-restriction checks, the concurrent helpers' own
assertions and the explicit GIL-off pairs): `JSObject.{h,cpp}`, `JSObjectInlines.h`, `JSArray.cpp`,
`JSArrayInlines.h`, `ArrayPrototype.cpp`, `ArrayPrototypeInlines.h`, `Structure.{h,cpp}`, `StructureInlines.h`,
`StructureInlinesLight.h`, `StructureRareData.{h,cpp}`, `PropertyTable.h`, `SparseArrayValueMap.h`,
`JSCellInlines.h`, `ButterflyInlines.h`, `MegamorphicCache.h` (`initAsTransition`), `VMLite.h`
(`currentButterflyTID`), `ConcurrentButterfly.cpp` (`ensureSharedWriteBit`, `convertToSegmentedButterfly`).
JIT: `AssemblyHelpers.{h,cpp}` (allocation stamp, TID tag loads, typed-array buffer load, megamorphic load and
store probes, nuked-ID decode), `CCallHelpers.cpp` (the four choke points, `maskButterflyTag`),
`ConcurrentButterflyOperations.cpp` (`updateButterflyTIDTag`), `JITPropertyAccess.cpp` (enumerator put),
`JITOperations.cpp` (the two reallocating-transition operations), `DFGSpeculativeJIT.cpp` / `DFGSpeculativeJIT64.cpp`
(GetButterfly, PutByOffset, CheckTransitionOwner, the threaded load helpers, the element-write lint),
`DFGByteCodeParser.cpp` (transition, delete and private-brand admission; the segmented-exit backstop),
`DFGConstantFoldingPhase.cpp` (folded transitions), `DFGClobberize.h` (GetButterfly, PutByOffset,
Multi{Get,Put,Delete}ByOffset, ArraySort{Compact,Commit}), `DFGOperations.cpp` (ArrayStorage get, pop recovery,
shift, join), `FTLLowerDFGToB3.cpp` (the same nodes, `maskedButterfly`, `isSegmentedButterfly`, property storage
allocation, MultiDeleteByOffset's assertion), `InlineCacheCompiler.cpp` (Replace, Transition, Delete,
SetPrivateBrand, ArrayLength per-case stubs; the shared transition, delete, put_by_val replace/setter and brand
handlers), `Repatch.cpp` (transition admission, delete and brand caching). Interpreter:
`LowLevelInterpreter64.asm` (`ifTaggedButterfliesBranch` on the eleven property fast paths),
`LLIntSlowPaths.cpp` (which cache form is published), `GetByIdMetadata.h` (two assertions). Two gates went the
other way, from the flag to GIL off only: `usePollingTraps` forcing (Options.cpp) and the MakeAtomString inline cache
probe (DFG, FTL). One new flag-on rule that G1 needs: SlowPutArrayStorage sites are generic in the DFG and
`trySetIndexQuickly`'s SlowPut arm is closed whenever the flag is on (Thread.restrict, api §5.8).


## What this audit did not check

- Nothing was built or run. No test was written. TSAN was not run.
- Rows marked "not re-read" come from one reader's trace and were not re-read
  for this file.
- Wasm, which is forced off GIL-off.
- Darwin-only and Windows-only code, beyond confirming that the macros exist.
- Bun-side callers (`src/`), except where a row names one.
- Non-JSC upstream changes (bmalloc, libpas), other than WTF.
- Upstream changes to code the branch does not touch and that fall in none of
  the five classes, such as new builtins that only call existing, already
  audited helpers.
- The async generator port (LANDING-PLAN 1.4), except VM-1, which touches it.
