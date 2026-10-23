# 7. Run-time state outside the objects

Who owns each piece of mutable state of a compiled process, and by which protocol it is written. The present state: `research/R4-runtime-state.md` (`R4 1.2`, `R4 H-4`), with the parts on control flow in `research/R6-calls-frames-async.md`.

The main finding of the survey: **"one VM = one thread at a time" is used as "the Instance is thread-local storage".** The `Instance` is per realm and mixes, often in one cache line, constants, shared caches, and words that mean something only between two instructions of one thread.

## 7.1 Four owners become five

| Owner | Object | Holds |
| --- | --- | --- |
| Process | `AOT::Image`, `BuiltinTable`, lazy string headers | read-only, or already correct for several threads (Workers share them) |
| VM | `VM`, `AOT::VMState` | **truth** that belongs to the heap and the engine: Structures, atom table, symbol registry, function tables, the registry of threads |
| Realm | `AOT::State` and the **shared regions** of the `Instance` | **truth** of the program: module variables, guard bits, cells of constants, class and closure tables, module states |
| **Thread** (new) | `ThreadContext` (engine), `AOT::ThreadState` (the run time of compiled programs) and the **private regions** of the thread's `InstanceView` | state of the running thread, and every **cache** |
| Object | 05, 06 | |

## 7.2 `ThreadContext`

`runtime/threads/ThreadContext.h`. One per JavaScript thread and VM. Made when the thread first enters the VM (08, T3). Reference counted: it outlives the thread for as long as a ticket or a sender of a task holds it (09, T2). Found in C++ by `ThreadContext::current()`, a `thread_local` pointer with the initial-exec model. Registered in `VM::m_threads` (under `VM::m_threadsLock`: 08, H3), which is the set of mutators and a source of roots, and in `VM::m_threadTable`, which leads from a thread's ID to its context (09, T2).

What moves into it from `VM` (member by member: `research/R7-ordinary-objects-interpreter.md`, section 10.1.1, and R4 10.2):

| Group | Members |
| --- | --- |
| Frames | `topCallFrame`, `topEntryFrame`, `entryScope`, `m_lastStackTop`, `VMEntryRecord` chain |
| Exceptions | `m_exception`, `m_lastException`, `targetMachinePCForThrow`, `callFrameForCatch`, `targetMachinePCAfterCatch`, `m_needExceptionCheck` and the scope verifier's state, `m_nativeStackTraceOfLastThrow` |
| Stack | the `StackManager` (both limits, its mirrors, `m_stopsAtFunctionEntries`), `m_stackPointerAtVMEntry`, reserved zone sizes |
| Traps | `VMTraps` (one trap word per thread), watchdog deadline of this thread |
| Jobs | the microtask queue, `m_onEachMicrotaskTick`, the inbox for tasks from other threads (09, T2) |
| Heap | `MutatorContext` (08): access word, allocators, mark stack, stored-cell log, deferral depth, mutator state, `m_stopCountSeen`, `m_collectionCountSeen`, `m_liveStorageRefs`, `m_keptAlive` (the cells that `WeakRef` keeps until the end of the job; it replaces `m_currentWeakRefVersion`: 08, section 8.11) |
| Scratch | RegExp match offsets (`regExpMatchOutput`) and backtrack stack, `RegExpGlobalData`, `m_executingRegExp` (also read by the conductor when it releases deferred frees: 06, section 6.9), `m_synchronousModuleQueue` (it points into a stack frame: 10, section 10.3), `m_stringSearcherTables`, `stringSplitIndice`, `m_syncResumeCallCache`, sort scratch, `MarkedArgumentBuffer` spill lists, `encodedHostCallReturnValue`, the scratch buffers that remain, `CompactStorage` iteration entry |
| Dynamic scopes | `allowLazyMaterializationOfImmutablePropertiesCount`, `DisallowVMEntry` count, "is in the middle of" flags; of `Modules`: `m_innermostEvaluation`, `m_isAskingHost` |
| Waiting | `m_waitsFor`, the thread's edge of the wait-for graph (09, W1); the claims that the thread holds (7.6, Z2; 09, MD3'') |
| Per-operation caches | 7.4 |
| Identity | thread ID (16 bits), the `JSThread` cell, the address of the view, the `ThreadState`, the embedder's pointer (Bun's `VirtualMachine*`) |

**How the code is changed.** In mt builds `VM::topCallFrame` and its like are not data members but inline accessors that forward to `ThreadContext::current()`. A function that uses several takes `ThreadContext& thread = vm.thread();` once. #249 measured the out-of-line selector at 0.97 % of all instructions; here it is one `%fs`-relative load in line, because the model is
initial-exec and there is no second mode to select. Machine code reaches the same members through the view: V8 (7.3.4).

`JSLock`: 09, section 9.3.

**The collector thread** has a context of kind `Conductor`: exception, trap and deferral words, no allocators, no view, not in `m_threads` (08, H14).

**A member of a context says nothing about the VM.** `entryScope == nullptr` means "this thread runs no JavaScript". Code that reads it as "no JavaScript runs" (`VM::isIdle()`, `whenIdle`, `deleteAllCode`, `shrinkFootprintNow`, `clearConcurrentRetainedDataIfPossible`, `RegExp::deleteCode`) frees what another thread uses. `VM::isIdle()` does not exist in mt builds (08, H16). What each consumer does instead: 06, sections 6.7 and 6.9; 08, section 8.12.

**What stays in `VM` and `JSGlobalObject`**, beside the tables of 7.7:

| Treatment | Members |
| --- | --- |
| Set before the second thread starts; the setter asserts that `m_threads` has one member | `m_onComputeErrorInfo*`, `m_onAppendStackTrace`, `m_computeLineColumnWithSourcemap`, `m_crossTaskToken`; `JSGlobalObject::m_name`, `m_evalEnabled` and its message, `m_consoleClient`, `m_runtimeFlags` |
| Atomic | `m_cacheEpoch`, `m_environmentEpoch` (7.5), `m_asyncContextTrackingEnabled`, `m_failNextNewCodeBlock`, `m_entryCountFromOutside`, `m_unlinkedCodeKeepers`, `m_isDeletingAllCodeToGenerateItAgain`, `m_activeHeapAnalyzer` |

## 7.3 The view of the Instance

### 7.3.1 Mechanism

Today: one `mmap` of zeroed pages per realm (`aot/runtime/AOTInstance.cpp:99-106`), the `Instance*` points into the middle, regions on 16 KB and 64 KB boundaries, all offsets constants of the program (`aot/runtime/AOTInstanceLayout.h:26-75`), every access `[instanceGPR + disp32]`.

mt builds:

1. The realm owns a **memory object** for the shared regions: `memfd_create` on Linux, an anonymous `shm` object or `mach_make_memory_entry` on Darwin, `CreateFileMapping` on Windows.
2. A thread's view is one reservation of the full size. Shared regions are mapped from the object (`MAP_SHARED | MAP_FIXED`, `mach_vm_remap`, `MapViewOfFile3` into a placeholder). Private regions are anonymous zero pages.
3. `instanceGPR` holds the address of the running thread's view.
4. All region boundaries are multiples of 64 KB (Windows' allocation granularity; 16 KB pages on Darwin divide it). `granuleAboveInstance` and the offsets between 64 KB and 128 KB of `InstanceLayout` change accordingly.

`realm->m_aotInstance` names the **canonical view**.

| # | Rule |
| --- | --- |
| **V1** | An address inside a shared region that is *stored* anywhere (a table, a registry of roots, a cell) is an address in the canonical view, or an offset. An operation that gets `Instance*` from compiled code gets the caller's view and may use it for loads and stores, not for addresses that outlive the call. `Instance::canonical(pointer)` converts |
| **V2** | The collector reads shared regions through the canonical view, and each thread's private root regions through that thread's view |
| **V3** | **The canonical view is no thread's view.** The main thread runs on a view of its own, made like any other. In the canonical view the private regions hold the members marked `constant` and `pushed` (7.3.3) and nothing else, and are mapped read-only once the realm is set up; the writers of `pushed` words go through an alias. A store through the canonical view into a private region faults on the first test run |
| **V4** | **C++ names the two things by two types.** `SharedInstance&` (from `realm->m_aotInstance`, `State::shared()`) has accessors for shared regions and constants only. `Instance&` is the running thread's view: `Instance::of(realm)` returns `ThreadContext::current()->viewOf(realm)`. `State::instance`, and every `Instance& m_instance` of an object that more than one thread can reach (the cell `ModuleRecord` among them), do not exist. An object that keeps an `Instance&` belongs to one thread and hangs off the `ThreadState` |
| **V5** | Two `Instance*` are never compared. "Of this program" is `structure->realm() == instance.globalObject`. (Today: `aot/object/AOTThisAllocation.cpp:39`, `aot/runtime/AOTClassOperations.cpp:39`, `aot/runtime/AOTPropertyCachesLeafOperations.cpp:170` and others. Compiled code already compares realms: `GuardEmitter::hasInstanceRealm`.) |
| **V6** | An address in a region becomes an offset by subtracting the base of the view that it came from, at the place where it enters C++. `RememberedSlotSet::record`, which subtracts its table's base and indexes the cards with an assertion only (`heap/RememberedSlot.h:47-51`), `SiteCaches::indexOf`, and the registrations of a `CacheRegistry` take offsets, or spans of the registry's own view |
| **V7** | When a view is made, a pointer member that points into the mapping is **rebased**: into the new view if its region is private, left canonical if it is shared. The list of such members is part of the runtime facts |

**`AOT::ThreadState`.** One per view. `instance.state` of a view points at it, and `ThreadState::shared` points at the realm's `State`. It owns what is per thread of today's `State` and `VMState`: `PropertyCaches`, the lookup tables (`VMPropertyCaches`), `SiteCaches`, `GlobalBindings`, `ChainDependents`, `StringKeyCache`, `JSONRuntime`, `CallSiteCache`, the caches of 7.4, the per-thread fronts of `StructureTable` (allocation site records, prototype transitions) and `KeyOrderTable`, the thread's `CacheRegistry`, its table arena. `State` keeps the truth: `ProgramStatics`, `ProgramStrings`, `BuiltinCells`, `Modules`, `ObjectModel` (7.7), `ImmortalCells`, `SharedChainDependents` (E5).
At the end of its thread the whole `ThreadState` is retired together with the view (T4; 08, H4). The ending thread frees nothing of it while a collection is between Begin and End: the collector may be visiting its registry and its tables.

The size of the change: 65 uses of `Instance::of`, 177 of `State::instance`, 253 of an `m_instance` member outside the tests. With V4 each is decided by the compiler: a use that needs a private word does not compile against `SharedInstance`.

### 7.3.2 Which region is which

| Region (R4 1.1) | mt | Notes |
| --- | --- | --- |
| cards of the statics | **shared** | byte stores of 1, idempotent |
| statics: variables, module values, guard words, claim words (Z2), module built-in cells, shared copy-on-write storage, RegExp cells, template objects | **shared** | truth |
| **new: private statics**: per-thread variables (03, section 3.6), array allocation profiles, last arrays, free environment cells; with cards of their own | private | `aot/place/AOTStaticsLayout.cpp:383-395`: `regionSteps` 9 to 11 and the new step go into a second chunk, placed below the shared one |
| loop poll page | private | one thread can be stopped alone; all are stopped by arming each page. Also the target of a diverted return (`aot/runtime/AOTUnwinding.cpp:297`): the address is the running thread's own, on its own stack |
| `InstancePrefix` | private | 7.3.3 |
| `Instance` struct (at most 32 KB) | private | 7.3.3 |
| key order steps | private | cache |
| built-in location states (one byte per entry) | **shared** | truth: `NotArmed`, `Unchanged`, `Changed` |
| global bindings | private | cache of locations (7.4) |
| built-in cells, built-in values | **shared** | single words, zero then value (7.6) |
| string cells | **shared** | the same |
| site caches | private | cache |
| method table | **shared**, mapped read-only in every view | the run time writes through a read-write alias of its own, under `ClassTable`'s lock. No `mprotect` dance: "no table of code addresses lies about that can be written" holds for the views, and the alias's address is known to C++ only. The table is copied from the image into the memory object when the realm is made (today it is a copy-on-write clone of the image's pages) |

### 7.3.3 The struct and the prefix, member by member (R4 1.2)

Both are private as a whole.

**Rule V9.** Each member of `Instance` and `InstancePrefix` carries one of five marks in its declaration. A generator makes the code that sets up a view from the marks, and fails the build for a member without one.

| Mark | When a view is made | Written later | Members |
| --- | --- | --- | --- |
| `constant` | copied from the canonical view | never | `runtimeTable`, `shapeSets`, `vm`, `slimClosureHeaders`, `closureCells`, `singleCharacterStrings`, `numberTag`, `notCellMask`, `globalObject`, `cellCageBase`; the number constants, `stringHeader`, `structureIDBase`, `emptyString` of the prefix; `image`, `numbers`, `stubs`, `lazyStrings`, `iteration`; `modules.status`; `classes.classPrototypes`; of `runtime.constants` the bases of shared tables (`keyCells`, `literalStrings`, `wide*`, `bigIntCells`, `cardsOfStringCells`); `propertyCaches.namesOfObjectPrototypes`, `namesChangedOnChains` (K4); `closures.closureWords`, `executables`, `closureHeaders`; job words, headers and the sentinel of `coroutines`; cells and IDs of `realm`; `builtins.directFunctions`, `nativeFunctionsOfHost`; the address of `VM::m_cacheEpoch` (E2a) |
| `rebased` | V7 | never | into a shared region: `prefix.builtinCells`, `builtins.locationStates`, `builtins.hostValues`. Into a private region: `runtime.counters.globalBindings` (`aot/runtime/AOTInstance.cpp:332-334`) |
| `thread` | set up for the thread | by the thread | `thread` (V8), `state`, `stackMirror`, `awaitMustSuspend`, `runtime.exceptions`, `scratch`, the hand-over words of `coroutines`, `json`, `stringBuilder`, `heap`, all `LocalAllocator*` and the four `allocation.*Space` words (08, section 8.8), `realm.microtaskQueue`, `propertyCaches.missIsDueForFiling`, `homeID` (09, T2), `inboxIsNotEmpty` (the one word of a view that other threads store to: 09, J1), `modules.evaluatingMark`, the string header words of `runtime.constants` (09, T5), the thread's ID and `ownedStorageTag[]` (06), `cacheEpochSeen`; what `publishWords` and `initializePropertyCacheWords` write (`aot/runtime/AOTPropertyCaches.cpp:137-155, 413-430`; today once per Instance, from `aot/runtime/AOTInstance.cpp:359` and `aot/runtime/AOTState.cpp:137`), both of which run whenever a view is made, since "the handlers are in place from the start": `propertyCaches.handlers`, `infoOfClassPrototypes`, `hasInstanceUID`, `prototypeUID`, `defaultHasInstanceFunction`, and the bases and masks of every per-thread table (`propertyCaches.front`, `hashedSlots`, `absenceFilters`, `missesOfPairs`, `namesOfTexts`, `customAccessors`, `siteCacheStates`; all of `objects`; `classes.placesOfPrototypeProperties`, `prototypesOfClassObjects`; `realm.smallIntStrings`, `intStrings`, `integerKeys`, `caseConversions`; `literals.propertyRuns`, `literalStringCache`, `functionNameCache`) |
| `pulled` | zero | by this thread: some slow path of the reader fills it from the truth | `allocation.inlineAllocations`, `typedArrayAllocations`, `clonedStructures`, `compactTableGrowthPlans`, `realm.linkTimeConstants`, `builtins.objects.objectCreate`, `ownSlotStructureIDs`, `constructWords`, `numberStructureID`, `booleanStructureID`, the absence generations, counters of `propertyCaches`, `arrays` and `runtime.counters`; `realm.readAndWrittenCountsStructure` and `constants.nameOfFunctionsWithoutEntry`, whose truth becomes a word of `State` published by Z1 |
| `pushed` | copied from the canonical view (the collector's two words: from `Heap`) inside T3 (08, section 8.14), in the critical section of `instancesLock` in which the view is appended to `VMState::views`: a writer holds the same lock, so it finds the view listed, or the view finds the word written | by whoever meets the event, into the canonical view and every listed view, under `instancesLock`: `VMState::forEachView`. While all threads are parked, unless the word only ever goes from 0 to one value | `barrierThreshold`, `mutatorShouldBeFenced` (the collector); `prefix.collectionStructureIDs[]` (`didCreateInstanceStructure`, `aot/runtime/AOTInstance.cpp:365`: when the realm's lazy class Structure is made); `closures.structuresWithPrototypeProperty[]` (`aot/object/AOTClosureTable.cpp:130`); the array headers of the prefix, `newCopyOnWriteArrayHeaders`, `frameArrayHeader` (`haveABadTime`, whose closure rewrites them in every view: 08, section 8.6) |

A `pulled` word has a reader whose slow path fills it. A word that an event fills once, and whose readers only fail their test when it is zero, is `pushed`: in a view that missed the event the inline test would fail for good.

**`VMState::instances` and `VMState::views`.** `instances` stays the list of realms that run the program (their canonical views): about twenty users take an element for "a realm with its `State`" (`aot/runtime/AOTHeapHooks.cpp:50-163`, `aot/runtime/AOTChainDependents.cpp:246`, the shortcuts `instances.size() == 1` of `aot/runtime/AOTCompiledFrameIterator.cpp:117` and `aot/runtime/AOTStackTrace.cpp:401`, `reserveInstance`, `destroyAll`). `views`, new, under the same `instancesLock`, lists `{ view, ThreadContext* }`. Writers of `pushed` words iterate over `views`. The marking constraints visit `instances` for the realms' registries and `VM::m_threads` for the threads' registries. A frame iterator finds the view in the `ThreadContext` of the thread it walks. Each iteration over `instances` that takes no lock today (`didChangeChainObject`, `publishWordsToEveryInstance`, `finalize`) takes it.

B3's immutable heaps (`AOTInstance_fixedFields`, `AOTInstance_vm`) stay right: the register does not change while a frame is live, and a coroutine is resumed only on the thread that suspended it (09, G1 to G3).

### 7.3.4 Who sets the register

| Place | Today | mt |
| --- | --- | --- |
| Adapters at the border from JSC's convention: `emitFindInstance`, `emitAdapterPrologue` (`aot/stubs/AOTStubsCallAdapters.cpp:97-135`) | callee, its block's VM, its Structure's realm, `m_aotInstance` | **the thread's view, from thread-local storage** (below). With `ProgramInstances::AtMostOne`, which threads keep true, there is one realm with an Instance, so one word per thread. With several Instances: `ThreadContext::viewOf(realm)`, a small map, through an operation |
| Unwinding | restores it from the callee-save buffer or `AdapterFrame::offsetOfInstance` | unchanged: the saved value is this thread's |
| Fault handler (`instanceIn(registers)`, `aot/image/AOTFaultHandler.cpp:155,203`) | reads it from the faulting context | unchanged |
| Stack walkers | read `AdapterFrame::offsetOfInstance` | unchanged |
| C++ that enters compiled code | through the adapter | unchanged |
| The interpreter's handlers for compiled processes (`loadInstanceOfRealm`, `llint/LowLevelInterpreterAOT.asm:119`) | the realm's `m_aotInstance` | `CodeBlock::m_thread->view` (10, I1). `llint_aot_virtual_call_trampoline` reads two `constant` words through the callee's realm, that is, from the canonical view: unchanged |

**Rule V8. The view leads to the context.** The view has a word `thread` (`ThreadContext*`) beside `vm`. Every `[instance->vm + k]` of R4 10.2 that names a member of 7.2 becomes `[instance->thread + k']`: the same two loads. The sites: `topCallFrame` in front of host calls (`aot/stubs/AOTStubsCallHost.cpp:78`, `AOTStubHelpersFrames.cpp:146`, `AOTStubsConstruct.cpp:116`, `AOTStubsPropertyAccessors.cpp:142`); the callee-save buffer of the entry frame (`AOTStubsExceptions.cpp:48, 59`); the unwind targets (`:81, 204-206`); `m_stopsAtFunctionEntries` (`AOTStubsPolls.cpp:133, 152`); `regExpMatchOutput` (`AOTStubsRegExpFrames.h:79`); `topEntryFrame` (`aot/image/AOTWasmStubs.cpp:43`). The stand-in Instance that `jumpToGenericUnwindTarget` builds on the stack (`AOTStubsExceptions.cpp:67-83`) gets `thread` beside `vm`. The pending exception alone lies in the view itself (09, section 9.5).
`ThreadContext` begins with the words that `doVMEntry`, the unwind stubs and the interpreter's `op_catch` touch, in one fixed layout that is a runtime fact. `VM::offsetOfTopCallFrame()` and its like do not exist in mt builds, so a site that was missed does not compile.

**Thread-local storage from machine code.** Needed by the adapters, for the view, and by about ten paths of the interpreter that have no `CodeBlock`, for the context (`loadThreadContext`: 10, section 10.2).

| Platform | Sequence | How the constant is known |
| --- | --- | --- |
| Linux x86-64 | `mov r13, fs:[imm32]` | the variable is in the executable's own TLS block (initial-exec), so its offset from the thread pointer is fixed when `bun-aot-mt` is linked. An executable is a copy of its template, in which "nothing that stays moves" |
| Linux ARM64 | `mrs x26, tpidr_el0`; `ldr x26, [x26, #imm]` | the same |
| Darwin | `_pthread_getspecific_direct` with one of JSC's reserved keys: `mov r13, gs:[key*8]`; `mrs x26, tpidrro_el0`, mask, `ldr` | `ENABLE(FAST_TLS_JIT)`'s sequences (`AssemblyHelpers::loadFromTLS64`) |
| Windows | `mov rax, gs:[0x58]`, index, offset; ARM64: `x18` | `_tls_index` and the variable's offset |
| any | call `operationAOTCurrentView()` through the callee's VM | fallback |

The offsets, keys and indices are **runtime facts of the template**. The template need not be the binary in which the compiler runs (another link of the same revision is a legal template), so the compiler takes them from the template's `RuntimeDescription` (03, section 3.3), not from its own process. They are in the image's stamp and are compared at load.

### 7.3.5 Fallback without mirrored mappings

If a platform cannot map one object at many addresses, the view's header holds `sharedBase`, and accesses to shared regions become `load sharedBase` (immutable heap, hoisted, one per function) plus `[sharedBase + constant]`. `pinned.statics` (`aot/codegen/AOTPrimitiveEmitter.cpp:198-210`) is already a separate base value in B3, so the change is local to where it is defined. Stubs that touch statics, string cells and built-in cells take one more load. Selected by a compile-time constant of the flavour per platform.

### 7.3.6 Cost of a view

Address space: the size of an Instance (1 MB + statics below, 384 KB + 4 bytes per string constant + 16 bytes per site + method table above). Memory: the pages a thread touches. A thread that runs a small part of the program touches few site caches. Creating a view: a handful of system calls and a copy of about 4 KB of constants. Together with the per-thread property tables (7.4) this bounds how cheap `new Thread` can be: estimated 50 to 100 microseconds, most of it the OS thread.

## 7.4 Caches: one per thread

**Rule K1.** A cache is read and written by one thread. No stub changes in how it probes or fills a cache. K1 speaks of probes and fills. The access that follows a hit obeys the rules of the object: a stub that stores into a slot after a hit follows 05, section 5.4 (E4).

| Cache | Today | mt |
| --- | --- | --- |
| Hashed property table, front table (three planes), `StructureRows` | per VM; rows per process | per thread. `StructureRows::singleton()` becomes a table per thread (one `uint32_t` per 64 bytes of structure heap, reserved, committed on touch) |
| Absence filters and their two generations, `missesOfPairs`, `namesOfTexts`, `CustomAccessorTable` | per VM | per thread |
| Site caches, key order steps, global bindings | Instance | private regions |
| `AllocationSiteRecord[]`, `distancesToAllocationSites[]` | Instance (`objects`) | per thread. The record's allocator is the thread's. Filled by `fillAllocationSite` from `StructureTable` (truth) |
| `prototypeTransitions`, `keyOrderTransitions` front, `keyOrderRuns`, `keyLists`, `enumerators`, `placesOfPrototypeProperties`, `prototypesOfClassObjects` | Instance pages | per thread |
| `CallSiteCache` (return address to site) | per VM, unlocked | per thread |
| `RuntimeLayoutTable::m_cachedFields`, `FieldMovePlanCache`, `AssignmentPlanCache`, `PropertyListCache`, `getterDefinitionPlans`, `m_recentClassLookups`, `m_instancesOfClassesBelow` | `State` | `ThreadState` |
| `StringKeyCache`, literal string cache, function name cache | Instance | per thread |
| `VM`: `NumericStrings`, `lastAtomizedIdentifier*`, `lastCachedString`, `KeyAtomStringCache`, `StringSplitCache`, `StringReplaceCache`, `immutableButterflyToStringCache`, JSON atom cache, `HasOwnPropertyCache`, `MegamorphicCache`, `DateCache`, `BigInt` divisor cache, `Math.random` state, `m_random`, `m_heapRandom`, `m_integrityRandom` | per VM | per thread |
| `IntlCache`, the cached collator and formatter triples of `JSGlobalObject`, its five default formatters | per VM, per realm | per thread: an ICU object is opened or cloned per thread |

The `propertyCaches.*`, `objects.*` and `realm.*` words of the view that name these tables are marked `thread` (7.3.3).

The stale-base hazard of the front table (R4 H-4), torn hashed entries (H-1), torn site caches (H-2), the freed key order table (R1 H5): all gone, because there is no second reader.

**Memory.** The tables are reserved and committed on touch. Spawned threads start with smaller reservations (the masks are words of the view, not constants of the code): hashed table 1/8, front planes 1/8. A limit in `aot/ir/schema/limits.def`.

**Roots.** Caches that hold cells are registered with a `CacheRegistry`. There is one registry per thread beside the VM's and the realm's. The collector's two constraints (`aot/runtime/AOTHeapHooks.cpp:74-98`) visit all of them. On a thread's own sets `RememberedSlotSet::record` (a byte store and a `Vector` append) is single-threaded again.

**Rule K2. No cache holds the value of a location that a program can write.** `Slot::Kind::Constant` for a free name or a global binding (`GlobalBinding.constant`, `WatchedValues`) caches a *value*, guarded by a watchpoint that fires "on the thread that makes the change". Another thread's copy would stay stale for ever, and "a free-name site with a cached store compares no Structure and fires no
watchpoint". In mt builds a value is cached only for a property that is read-only (locked built-ins) or that flow proves constant. Otherwise the cache holds the location. One load more per read of a global.

**Rule K3. A remembered set over a shared table has no list.** `setCardsAreWrittenWithoutRecord()` does not change `record()` today: it stores the card and appends to `m_writtenCards` whatever the flag says (`heap/RememberedSlot.h:47-59`), and C++ calls it on the statics' set from any thread without a lock (`aot/runtime/AOTProgramStatics.cpp:135`, `aot/runtime/AOTDirectEvalOperations.cpp:475`, `aot/runtime/AOTRegExpOperations.cpp:338`, `aot/runtime/AOTDataBuilder.cpp:516`). In mt builds a set over a table that several threads write is made with `SharedWriters`: `record()` is the byte store alone, and `m_listsEveryWrittenCard` is false for ever. Such sets: statics, string cells and wide string cells, built-in cells and values, closure cells, class prototypes, the sets of `StructureTable`, `KeyOrderTable`, `MemberGuardWatchpoints`, `ClassStructureCache`, `Modules`. The collector reads all cards of these sets at each stop, eight at a time, as it does for the statics today.

**Rule K4. The two name bit sets are truth.** `namesOfObjectPrototypes` and `namesChangedOnChains` are bits that are only ever set. A stub answers "absent" from a flag of the Structure (`HasOnlyFieldsBelowObjectPrototype`) and a clear bit of the first set, with no generation and no entry in between (`aot/stubs/AOTStubsPropertyProbes.cpp:245-262`; the store side: `aot/stubs/AOTStubsKnownFieldStores.cpp:57, 82`), so no flush of a cache reaches that answer. They stay one per VM. Each view's word points at the one array. A writer sets bits by `fetch_or`, as one of the shared words of E1.

## 7.5 Invalidation across threads

A cache entry keyed by `(StructureID, name)` about an **own** property needs no invalidation: a change of the object changes its StructureID. Entries that **depend on a chain** (inherited properties, absences, answers through prototypes) are dropped today by "the change, then the report" (`didChangeChainObject`, `aot/runtime/AOTChainDependents.cpp:238-255`).

```
VM::m_cacheEpoch        std::atomic<uint64_t>, shared
cacheEpochSeen          a word of the view
```

| # | Rule |
| --- | --- |
| **E1 writer** | A thread that changes something that entries of a chain may depend on: makes the change (under the locks of 05 and 06); updates shared words atomically (guard bits: `fetch_and`, a set's bit before its members' bits; name bits of K4: `fetch_or`; location states: a byte store; method table entries: under the lock); drops the shared dependents (E5); drops the dependents in **its own** caches, precisely, as today; then `m_cacheEpoch.fetch_add(1, seq_cst)` |
| **E2 reader** | A thread compares `cacheEpochSeen` with `m_cacheEpoch`: whenever it gains heap access after a stop (`didResume()`: 08, H12); at every synchronizing operation with acquire meaning (in line: E2a); when it takes tasks out of its inbox, once per batch (09, section 9.7: the start of a job or task that another thread sent is the acquiring end of an edge of 02); in every turn of a loop whose exit depends on an access by name (E2a); in the slow path of every poll and allocation; at the start of every cache fill. If they differ it drops **every chain-dependent entry** of its caches (it walks its own `ChainDependents` and drops all), bumps `absenceFilterGeneration` **and** `absenceFilterGenerationForStores`, and stores the new value |
| **E2a in line** | `cmp [view + cacheEpochSeen], [cacheEpochAddress]; jne slow`: a load of a line that is read-mostly, a compare, a branch that is never taken. `slow` is `operationAOTDidSynchronize`, which runs E2 and is declared `collects`. It stands (1) at the end of every inline sequence that 04 declares `synchronizes` with acquire meaning (`AtomicSlot` load and read-modify-write forms, the inline row of `Atomics` on typed arrays, the fast path of `LockHold`, MD1's load of the status byte); (2) in the `LoopHint` of a loop whose exit depends on an access by name, made in the loop or in a function that it calls (04, section 4.12: flag `comparesCacheEpoch`); in the interpreter, in `loop_hint` |
| **E3 fill** | **Register first, validate afterwards.** A fill reads the epoch; computes its answer by walking objects; makes every object that the answer depends on report (`ensureChangesAreReported`, under their object locks); files the entry and registers its dependents; **then** compares the StructureID of each of these objects with the one that the walk saw, and reads the epoch again. If a StructureID differs or the epoch has moved, the entry is dropped and the access is answered without a cache |
| **E4 safety** | An entry that is stale is still **memory safe** to use, for reads and for stores. By kind: below |
| **E5 shared dependents** | A dependent whose drop changes shared state is registered in the realm's `SharedChainDependents`, a table of 7.7. Its owners: `MemberGuardWatchpoints` (`watchOtherObjects`, `aot/object/AOTMemberGuards.cpp:1701`: a guard depends on objects that hold no class) and `StructureStateOwner` (`DefinitelyNonThenableState`, cached special properties of a Structure: `aot/runtime/AOTChainDependents.cpp:55-66`). **It is registered first and validated afterwards**: under the table's lock, register; after the unlock, compare the StructureID of each object that the answer depends on with the one that the walk saw; if one differs, drop the dependent and do not set the shared word. E1's writer changes the object first, takes the table's lock afterwards, and drops precisely, before the `fetch_add`. A thread's own `ChainDependents` holds only dependents whose owner is a cache of that thread. `addOwner` takes the kind of the owner, and a debug build asserts it |

**What counts as such a change.** Every report of `didChangeChainObject`, and every change that can fire a transition or replacement watchpoint of a Structure that `mayBePrototype()`: its writer bumps the epoch. The interpreter's prototype caches of other threads are cleared by E2 only (10, section 10.2).

**Why E3 compares again, and last.** Reports come only from objects whose Structure `mayBePrototype()`. A change of an object that does not report yet moves no epoch. If it falls between the walk and the marking, the entry would be stale and nothing would ever drop it. After the marking every change is reported, and the compare shows whether one came before.
The compare comes behind the registration. A writer changes the object and looks for dependents afterwards. So either its search finds the dependent, or the search came before the registration, and then the change came before the compare, which sees it. With the compare in front of the registration a change between the two is seen by neither side. For a thread's own caches that would heal at the owner's next E2. For a shared word it would not: E2 drops nothing shared, and "a guard is set at most once".

**Why E5.** A guard bit and a Structure's state are tested by all threads. If their dependents lay in the map of the thread that armed them, a thread that changes a chain object would not clear them, and would itself go on with the old method after its own change (02, M5), for as long as the arming thread does not run E2.

**E4 by kind of `Slot`** (`aot/runtime/AOTRuntimeRecords.h:62-90`):

| Kinds | Why a stale entry is safe |
| --- | --- |
| `Value`, `FarValue`, `Field*`, `InheritedField`, `Variable`, `ExtraProperty` | a Structure's map from name to offset never changes (dictionaries are never filed unless cacheable, and then own values only); N2, N2' and N3 of 06: an offset that an entry names is not given to another property while a thread can hold the entry; storage never shrinks under a reader (SW2), and flattening is a safepoint operation, after which every thread runs E2 before it probes (H12); holders are kept alive by the cache's registry |
| `Getter`, `Setter`, `ExtraGetter`, `ExtraSetter`, `FieldOrInherited*` | the handlers load the slot and dereference it with no test of its type (`aot/stubs/AOTStubsPropertyReadHandlers.cpp:145-150`, `aot/stubs/AOTStubsPropertyAccessors.cpp:37, 96`). A slot that has held a `GetterSetter` holds a `GetterSetter` until its offset leaves quarantine: a delete stores the VM's `emptyGetterSetter` there, not `undefined` (06, N2) |
| `Transition`, `ExtraPropertyAddition`, `FieldCreation` | the handler takes the object lock and compares the StructureID again (06, section 6.4; 05, section 5.6 e, f). A stale answer about the chain adds an own property past a setter that another thread has just defined: a legal order of the race |
| stores through `Value`, `Field`, free-name sites | into a slot that N2 keeps from reuse, after the state test of 06, section 6.2 for out-of-line storage, by the compare-and-swap of 05, section 5.4 for a field that can leave its slot |
| `CustomGetter`, `CustomSetter` | the record table is the thread's own |
| `Constant` | K2 |
| `Absent`, `ArrayLength`, `TypedArrayLength`, `StringLength`, `ClosureLength`, `ClosureName` | no memory is named by the entry |

The bound of N2 is the end of a collection. It holds because no thread probes a cache between the end of a stop and its own E2, whether it was parked or had no heap access during the stop (H12).

What a program sees: after another thread has changed a prototype, a thread may go on finding the old member until its next synchronizing operation, or until the next turn of a loop whose exit depends on the member (02, M10). That is the row "change of a prototype's property" of 02, section 2.10.5. A program without races sees nothing: the change happens before the reader's acquire, which runs E2.

**How often the epoch moves.** In steady state programs do not change prototypes. During start-up they do (classes are made at first touch). A spawned thread that starts while the main thread still defines classes refills some entries. The statistic `CacheEpochFlushes` is reported by `BUN_JSC_logThreads`.

**Facts that tie two caches together are truth.** `m_namesWithCachedStore`, `m_identifiersStoredThrough`, `isStoredByNameFlag` say "a store through this name is cached somewhere, so no constant of it may be watched". With K2 they lose their purpose and are compiled out.

**A second epoch.** A request that concerns every thread's `DateCache` and `IntlCache` (a change of time zone or of the default locale) bumps `VM::m_environmentEpoch`. A thread compares it where it compares the cache epoch in C++ (E2, not E2a) and resets its own caches. The walk that marks `DateInstance`s stale is one safepoint operation, run by the thread that made the change.

### Words that compiled code tests and events flip

The numbers are those of R4 5.7.

| R4 5.7 | Word | mt |
| --- | --- | --- |
| F1 | guard bits | shared; `fetch_and` / `fetch_or`. "A guard is set at most once. One that has ended stays clear": a one-way door. A stale pass of the test is the use of the old member once more: the old method is still a function for that receiver's layout, and the receiver's shape is tested apart. One kind needs more: a `Member` guard over a static field stands in front of a read or store of the field's **variable**, which `delete` and redefinition empty. There the variable is read and written as a slot (05, CO11 to CO13; 04, `TryGetVariable`, `TryPutVariable`), so a stale pass meets `absent` in the loaded word and leaves for the access by name. **Rule F1a**, below |
| F2 | method table entries | shared; aligned 32-bit words; both values are valid code offsets with one signature |
| F3 | built-in location state | shared byte; `arm()` under the watch table's lock |
| F4, F5 | global binding records | private; K2 |
| F6, F7 | site caches, front, hashed | private |
| F8 | absence generations and filters; name bit sets | private; shared (K4) |
| F9, F10 | stop marker, poll page | private, written by `updateThreadStopRequestIfNeeded()` of the thread's traps alone (08, section 8.4) |
| F11 | "needs OutOfMemoryError" | per thread |
| F12 | barrier threshold, fence flag | `pushed`, while parked |
| F13, F14 | `awaitMustSuspend`, `frameThatMayContinue`, queue empty | private: the queue is the thread's own, so "the queue is empty, so running the continuation now is unobservable" stays a fact about one thread |
| F15 | `m_asyncContextTrackingEnabled` | VM, set once, never cleared: atomic store |
| F16 | module status | 09, section 9.9 |
| F17, F18 | absent encodings, lazily filled words | 7.6 |
| F19 | array headers | `pushed`, by the closure of `haveABadTime` |
| F20 | StructureIDs in prefix and struct | `pulled` or `pushed` (7.3.3) |
| F22 | thread-private by intent | private in fact |

**Rule F1a.** No fact that `trustsKnownRanges` or a representation rests on is derived from a guard. The verifier refuses a value whose type is narrowed by a `Guard` of kind `MemberUnchanged` or `PrototypeLinkUnchanged` and that is used unboxed without a test of the value. The bit of a guard *set* ("one bit, which is set while every guard of the set is") is cleared before the member's bit, so that no thread sees the set hold while a member has ended. The written argument per kind (`Member`, `PrototypeLink`, `NotHidden`, `PassedByStores`), for sets and for the arming logic, is work package W-7.4.

## 7.6 Things that are made at first use

Everywhere the shape is `w = load(slot); if (w) use w; else { v = create(); slot = v; }` (R4 7, twenty rows; R1 H9).

**Rule Z1 (publish by compare-and-swap).** `v = create()` outside any lock; root it (`ImmortalCells::add`, which is locked); `compareExchange(slot, 0, encode(v))` with release order; on failure drop `v` and use what is there. Readers: plain load, then dependent loads.
- Where the slot is a root found by cards, the winner stores the card after the compare-and-swap and before its next safepoint (K3).
- Where the word has states other than "not made" and "made" (a slot of representation `DeferredClosure`: `absent`, `undefined`, `null`, a value that the program stored; a variable with an absent encoding), the loser of the compare-and-swap does not "use what is there": it starts again from the load and dispatches on the word that it finds. An operation that returns a made thing can therefore return "empty", and its caller has an edge for that (04, C3).
- Where the word can be reset to 0 (`forgetClosure`, `forgetValuesNamed`), the owning table has a generation, which a reset increments under the table's lock. A creator reads the generation before it begins, and publishes under the lock only if it has not moved; else it starts again. Otherwise a creator that began before the reset would publish the old thing after it. (It cannot look at its source again under the lock: for a host value the source is `holder->get()`, which runs code.)

**Rule Z2 (claim, then make).** Where creation has side effects that must happen once (it runs program code, it registers itself elsewhere, or two instances would be distinguishable while both exist): take the owning table's lock, look again, create, publish, unlock. If creation can run JavaScript or collect, the lock cannot be held across it (L1): then a **claim word** is used. `0 -> Claimed(thread ID + 1)` by compare-and-swap; the winner creates
and stores the result (release); a loser that finds `Claimed(other)` waits (heap access released; part 3); one that finds `Claimed(itself)` is in a cycle and does what the single-threaded code does there.

| # | Part of Z2 |
| --- | --- |
| 1 | **The claim word is a word of its own**, in the shared statics or beside the lazy word. The published slot keeps its "not made" encoding until the winner's release store, so inline readers, which test for that encoding only, never decode a claim. It has the encoding of a lock word (09, section 9.13): the thread's ID **plus one**, because 0 means unclaimed and the main thread's ID is 0 |
| 2 | **A claim is released by a scope object**, listed in the `ThreadContext`. If the creation ends by an exception, termination included, the word goes back to 0 and the waiters are woken. Each tries again, and meets the same exception or succeeds. So no claim outlives its thread, and a reused ID (09, T1) never finds `Claimed(itself)` by accident |
| 3 | **How a loser waits.** It enters the wait-for graph (09, W1) with an edge that names the claim's holder, the claim word and the value that it read. Then it sleeps by `ParkingLot::compareAndPark` on the **canonical** address of the claim word (V1), with that value: the shared statics have another address in each view, and a loser that parked on its own view's address would not be found. Whoever ends a claim stores the word and then calls `unparkAll` on the canonical address, always. A wait that would close a cycle throws in the waiter. For the engine's own lazy words (`LazyProperty`, `LazyClassStructure`, link-time constants) it is answered as `Claimed(itself)` is: the initializer sees "being initialized", which it tolerates, because the same nesting occurs on one thread |
| 4 | **A word that can be reset carries a generation** beside the holder, which a reset increments. The winner publishes only if the generation is the one that it claimed. (W1 needs none. An edge is live while the word holds the value that the waiter read. If a claim has failed and the same holder has claimed the word anew, the waiter will wait for it again, so the edge says the truth) |
| 5 | **No wait inside a `NoSafepointScope`**: there the operation returns `MustRetryAtSafepoint` (08, S4). Compiled code reaches a wait of Z2 only through operations that are declared `waits` (04, S5). What the cheap operations need is made before the first thread starts (below) |

| Thing (R4 7.x) | Identity visible? | Rule |
| --- | --- | --- |
| String constant cell, BigInt constant, literal string cell | no | Z1. The x86-64 stub that allocates the `JSString` itself ends in `lock cmpxchg` instead of `mov` |
| String header made at run time (bump pointer) | | a chunk per thread of one process-wide reservation; used headers are never freed (09, T5) |
| Key cell that is a `Symbol` | yes | Z1; `Symbol::create` goes through `symbolImplToSymbolMap` under its lock |
| Identifiers: text | | in the image |
| `ProgramSymbols` (symbols, private names; open addressing, grows, freed while lock-free readers probe) | yes | under its lock: look again, create (`SymbolImpl::create` neither collects nor runs code), insert. The table is one block with its mask inside (PT1 of 06). An entry is written `uid` first, then `number` (release), as PT2: the stub compares `number` and then loads `uid`. A replaced block is freed after a safepoint of every thread |
| Singleton closure (`closureCells[k]`) | yes | prefilled (below), else Z1. `forgetClosure` under the closure table's lock |
| Deferred closure in a field; flattened object; pending stars | yes | Z1 on the slot or word, with the restart above (05, sections 5.4, 5.13). The flattened form that keeps its slot exists only in Confined outer layouts (04, section 4.8) |
| Variable with a deferred initializer (`DeferInitializers`) | depends | the initializer makes "what nobody can tell when it was made": Z1 if it only allocates, else Z2. The variable stays `absent` until the winner's store |
| Variable "born as a function" (`FunctionAsZero`) | yes | Z1 |
| Class object and class prototype that are made at first touch | yes | Z2 on a claim word per class. Making them runs no code of the program (`scope.assertNoException()`, `aot/object/AOTClassTable.cpp:151-171`) and needs only the class objects of the parents. So claims of this kind go up the class hierarchy and cannot wait for each other. Prefilled where possible. **The class object's cell is published in `closureCells[]` after `defineClass`**, not before as today; until then only the claimant finds it, through the claim's scope object. So "the members are there from the start" holds for every thread that can find the class (04, O2: `isThereFromTheStart`) |
| Class definition that runs program code (static blocks, computed keys): `NewClass` | yes | runs where the program defines the class, on the thread that runs the statement, under the class's claim word. Another thread that reads the binding meanwhile finds it `absent` (TDZ) |
| Conversion of a class prototype's stand-in | | 05, section 5.10: what the conversion needs is made before, under the class's claim word; the safepoint operation only converts and stores |
| Usual Structure of a shape, Structure variants | no | `StructureTable` lock: look up, create outside, look up again, insert. Two usual Structures for one shape would only thrash caches, but the table is a C++ container |
| Template object | yes (used as a `WeakMap` key) | Z1, in line: `cmpxchg`, the loser takes the winner's |
| Module namespace object, module object, `require`, `import.meta` | yes | made aside, published under `Modules::m_lock` (09, MD7') |
| `RegExp` cell per (record, flags) | no | `RegExpLiterals` lock (7.7) |
| Shared RegExp literal object | state | prefilled, else Z1; which literals are shared: 04, section 4.8 |
| Built-in cells | the value is an existing object | settled by `prepareForThreads()` (below) |
| Built-in values (`hostValues`) | the same | not made ahead: "the read is the one that the program wrote: the host makes the value now, by code that may throw and that the program may observe" (`fillValue`, `aot/runtime/AOTBuiltinCells.cpp:322-350`). The operation behind an empty word `throws`, `collects` and `waits`. Z1 with the generation of `BuiltinCells`, which `forgetValuesNamed` (`:354`) increments |
| Copy-on-write storage of an array literal | no | Z1 |
| `LazyProperty` of `JSGlobalObject` and of Bun's global object | yes | Z2: the low bits of the word already encode "initializing". The holder's ID plus one goes into the upper 16 bits. The word is stored (release) when the initializer has returned; `Initializer::set` writes to a field of the initializer, which nested calls of the holder read. (`research/R7-ordinary-objects-interpreter.md`, section 10.2: today it can return null or run the initializer twice) |
| `LazyClassStructure` | yes | the same. The initializer publishes the structure word in the middle today (`runtime/LazyClassStructure.cpp:55-87`). In mt builds the structure word is published last, after `m_constructor` and after `didCreateClass` |
| `ErrorInstance`: the materialized `stack` | | Z2 with a claim byte in the cell: the hook can run `Error.prepareStackTrace` (06, section 6.10) |
| Lazy subspaces of `Heap` | | `Heap::m_lock`, look again |
| Lazily made singletons of `VM` | | Z1, or made by `prepareForThreads()` |
| `unique_ptr` members of `State` made on demand (`PropertyCaches`, `GlobalBindings`, `JSONRuntime`) | | they belong to the `ThreadState` and are made on demand by their thread |

**`initializeProgramStatics()`** (unprotect, fill, fence, set a plain `bool`): runs before the program is entered, hence before any thread exists.

### Before the first thread: `Instance::prepareForThreads()`

The IR operations `Builtin`, `LinkTimeConstant`, `SingletonClosure`, `SharedRegExpLiteral`, `GetClassPrototype` and `GetSuperBase` are cheap because they are not declared `collects`: their run-time halves hold `DeferGCForAWhile` (`aot/runtime/AOTConstantOperations.cpp:71-79`), which in mt builds is a `NoSafepointScope`. Inside it a thread can neither wait for a claim nor for a module (S4).

The first `new Thread` of a realm (09, section 9.2) therefore calls `prepareForThreads()`, on the creating thread, before the OS thread is made, while the realm still has one thread. It **settles each cell** that the image names through these six operations: the cell is made, or it is recorded as unavailable in this realm. That covers built-in cells (with the internal modules that they load), link-time constants, singleton closures, shared RegExp literals, class objects and prototypes that are made at first touch. It also makes the lazily made singletons of `VM`, and reifies the members of static property tables that the image names (06, section 6.4). (A realm installs its species and property-descriptor watchpoints when it is made: 06, WP4.)

Afterwards the six operations only load.

| Case | After `prepareForThreads()` |
| --- | --- |
| the word is filled | as today |
| a `pulled` word of a new view (`linkTimeConstants[]`) is empty | the slow path copies from a truth that is made. It waits for nothing |
| the word of a built-in cell is empty | it means "unavailable". Today's slow path (`BuiltinCells::cell`, `aot/runtime/AOTBuiltinCells.cpp:243-293`) edits `m_awaitedMembers` and `m_isUnavailable`, loads a module and looks the member up. In a realm that has threads it looks nothing up and writes nothing: it answers null, as it does today for a cell that is recorded as unavailable |
| a cell that can come into being later: a member whose creation is awaited (`awaitCreation`), a row with the mark `MayBeAbsent` or the column `madeLater` (03, section 3.5.1), a module that is loaded on demand; a class whose definition runs program code | known from the table of built-ins and from the image. It is read by a form of the operation that is declared `waits` (04, S5), whose slow path takes the `BuiltinCells` lock for the containers and runs code outside it |

So an operation that is not declared `waits` never reaches a wait, and S4's `MustRetryAtSafepoint` is never returned to a frame that cannot take it.

Cost: time and memory in proportion to what the image names, paid once, by programs that start a thread. Reported by `BUN_JSC_logThreads`.

## 7.7 Tables that hold truth

One lock each (`WTF::Lock`). All are leaf locks (08, H15): the holder is in a `NoSafepointScope`, runs no JavaScript, and waiters keep heap access. No table lock is held while another is taken, except where listed.

| Table | Lock | Lock-free readers |
| --- | --- | --- |
| `StructureTable` (`m_structures`, `VariantMaps`) | `StructureTable::m_lock` | `m_structures[shape]`: single words, zero then ID |
| `KeyOrderTable::Storage` (added orders, continuations, transitions) | `KeyOrderTable::m_lock` | none of the lookup. `entriesInOrder()`, `propertiesInOrder()` and every other lookup take the lock: the way to an added order goes through a hash table that grows (`ordersOf`), an array that is replaced (`addOrder`, `aot/object/AOTKeyOrderTable.cpp:366`) and an order that may move (`append`, `:381`). The span that a lookup returns "stays where it is, unchanged, for as long as the table lives" and is used after the unlock. Orders of the image (`order < layout.numberOfOrders`) need no lock. An order word is a reference into this table that is stored by a plain store; because the thread that adds an order and the thread that looks it up pass through one lock, a reader that has loaded an order word finds the order (also on ARM64). The per-thread fronts of 7.4 answer repeated lookups. An order that "grows in place while an object under construction stands at its end" (`:577-583`) grows only under the lock, entry first, count after (release) |
| `ClassTable` and its maps, `MemberGuardWatchpoints`, the method table | `ClassTable::m_lock` | `classes.classPrototypes[]`: single words, in a table outside the views |
| `ClosureTable` | `ClosureTable::m_lock` | `closureCells[]` |
| `ClassStructureCache` | with `ClassTable` | |
| `SharedChainDependents` (E5) | its own | |
| `ChainDependents` | per thread: no lock | |
| `Modules`; the containers of the realm's `JSModuleLoader` (09, MD10) | `Modules::m_lock`, which is the loader's `JSModuleLoader::m_lock` (09, MD3'; 10, section 10.3) | status bytes |
| `ProgramStrings`, `ProgramIdentifiers`, `BuiltinCells`, `BuiltinLocationWatch` | one each | the words in shared regions |
| `RegExpLiterals` | its own. The collector visits `m_cellsInOrderOfCreation` under the *registry's* lock, from a constraint that is declared concurrent, so an append holds the table lock and `CacheRegistry::lock()`, in that order | |
| `ZeroedPages::m_ranges`, `ZeroedTableArena` of `State` (`aot/runtime/AOTState.h:43-72`) | one each. A `ThreadState` has an arena of its own for its tables; nothing in it is named by shared state (09, T5) | |
| `ImmortalCells` | has one | |
| `VMState::instances`, `views`, `LoopPollPages` | have one | |
| `VM`: `m_executablesOfNamelessHostFunctions`, `m_impurePropertyWatchpointSets`, `m_signposts`, `m_executableForCachedFunctionExecutableForFunctionConstructor`, `m_persistentBytecodePayloads` and the cached two and three character atoms; `JSGlobalObject::m_rareData`; `SourceProvider::m_id` and its directive strings | one each, or Z1 where it is a single pointer | |

`CacheRegistry::lock()` stays what it is, a lock between a mutator and the collector, now per registry. Any container that a registry visits and that a mutator may reallocate is changed under it.

## 7.8 Counters and heuristics

Branch, arrival and allocation counts are not active in shipping executables. Array allocation profiles and the sampling counters of the property caches are always on: private. No word is written per execution by all threads. (#249: a shared counter took a loop's back edge from 50 to 260 cycles with two threads.)

## 7.9 The image

Read-only after load. Only `ImageHeader::address` and `ImageStringHeader::characters` hold addresses, patched by the loader before the first VM exists. Nothing changes.
