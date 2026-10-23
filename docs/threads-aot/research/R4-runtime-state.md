# R4. Mutable run-time state of an AOT executable (outside the JavaScript objects)

Engine tree: `Source/JavaScriptCore/` (head 5c2923e566cb). All paths below are relative to it
unless they begin with `bun:` (= `<bun>/`). Line numbers are those of that head.

Classification letters used everywhere:

- **(a)** would have to be per thread
- **(b)** shared, read-only after start (or after a one-time initialisation that ends before a second thread exists)
- **(c)** shared, written with atomics / lock-free publication
- **(d)** shared, under a lock (or only written while every other mutator is stopped)

Race severity: **[value]** = a race gives at worst a wrong JavaScript value or a lost cache fill / lost count;
**[memory]** = a race can give type confusion, a wild pointer, a use after free or a corrupted C++ container.

---

## 0. The map: four owners

| Owner | C++ object | Made when | Reached from compiled code by |
| --- | --- | --- | --- |
| Process | `AOT::Image` (one, `Image::current()`), `StructureRows::singleton()`, `BuiltinTable::singleton()`, the lazy string headers of a position-independent image, a few `static` flags | `useImage()` at start, before the first VM | addresses copied into each Instance |
| VM | `AOT::VMState`, a member of `VM` (`runtime/VM.h:511-516`, `m_aotState`) | `VM::VM()` calls `m_aotState.attachImage(*this)` if an image is registered (`runtime/VM.cpp:369-372`) | `instance->vm`, then `VM::offsetOfAOTState() + ...`; most of it only from C++ |
| Realm | `AOT::Instance` (raw zeroed pages, what compiled code addresses) + `AOT::State` (C++ objects behind it, `instance->state`) | `JSGlobalObject::init()` (`runtime/JSGlobalObject.cpp:1117-1120`, `2468-2471`) | the pinned register `instanceGPR` + constant offset |
| Thread | **nothing** that compiled code can reach (C++ has a handful of `thread_local` helpers, 2.1). There is no per-thread structure of the AOT run time. Everything that is logically "of the running thread" (pending exception, stack limit, top call frame, allocator free lists, scratch words, coroutine hand-over words, JSON parser state, string builder pool) lives in the `VM` or in the `Instance` | | |

The last row is the main finding of this report: **"one VM = one thread at a time" is used as "Instance = thread-local storage"**. The Instance
mixes, often in the same cache line, (i) constants, (ii) shared caches, (iii) words that only mean something between two
instructions of one thread.

`VMState` (`aot/runtime/AOTVMState.h:36-83`):

```
const Image* image; bool hasCompiledCode; bool validatesRuntime; uintptr_t structureIDBase;
unique_ptr: runtimeTable, identifiers, caches (CacheRegistry), propertyCaches (VMPropertyCaches), counters, reporter,
            callSiteCache, functionTables, regExpLiterals
UniqueArray<int> regExpMatchOutput;
WriteBarrier<Structure> moduleRecordStructure, stringBuilderStorageStructure;
Lock instancesLock; Vector<Instance*,1> instances; Vector<Instance*> instancesToDestroy;
LoopPollPages { Lock; Vector<void*> pages }   InnerCellsWithoutMark { Lock; Vector<JSCell*> }
```

`State` (`aot/runtime/AOTState.h:96-128`), one per Instance, all `unique_ptr`s made in `State::State` (`aot/runtime/AOTState.cpp:123-144`):
`pages`, `tableArena`, `caches` (a second `CacheRegistry`), `immortalCells`, `strings`, `statics`, `builtinCells`, `chainDependents`, `siteCaches`,
`propertyCaches` (lazy, see 7.14), `modules`, `builtinModules`, `objects` (`ObjectModel`: structures, classes, closures, key orders), `coroutines`,
`builtinFamilies`, `globalBindings`, `arrivalCounts`, `allocationCounts`.

The only locks in all of this: `VMState::instancesLock`, `LoopPollPages::lock`, `InnerCellsWithoutMark::lock`, `CacheRegistry::lock()` (mutator against the
*collector*, not mutator against mutator, see 5.6), `StackManager::m_mirrorLock`, `namesOfLockedObjectsLock` (`aot/runtime/AOTRealmStartup.cpp:62`), and those of the image loader (section 9).

---

## 1. The Instance

### 1.1 Layout, in address order

`aot/runtime/AOTInstanceLayout.h:26-75`. One `mmap` of zeroed pages (`reserveMemory`, `aot/runtime/AOTInstance.cpp:99-106`; `MADV_NOHUGEPAGE`), the `Instance*` points into the middle.
Sizes come from the image (`InstanceSizes { sizeOfStatics, numberOfStringCells, numberOfSiteCaches, sizeOfMethodTable }`, `AOTInstanceLayout.h:18-24`), so every offset is a compile-time constant of the *program*.

| Offset from `Instance*` | What | Written by | Class |
| --- | --- | --- | --- |
| `offsetOfCards` = statics − (statics size >> 8), rounded to 64 KB | **cards of the statics**: one byte per 256 bytes of statics (`cardShift = 8`) | inline compiled code and stubs store `1` (`markStaticsCard`, `aot/stubs/AOTStubsPropertyProbes.cpp:1392-1399`); collector writes `2`/`0` | (c) byte stores; see H-20 |
| `offsetOfStatics` = −1 MB − roundUp64K(sizeOfStatics) | **statics** (1.3) | inline code, stubs, C++ | mixed, see 1.3 |
| −1 MB (`offsetOfLoopPollPage`) | **loop poll page**, 16 KB reserved. x86-64 Linux: a loop head does `load32 [instance − 1MB]`; `mprotect(PROT_NONE)` turns it into a stop (`aot/codegen/AOTHintEmitter.cpp:40`, `aot/runtime/AOTRuntimeHooks.cpp:141-152`) | nobody writes; protection flipped under `LoopPollPages::lock` | (b)+mprotect: shared by all threads of the realm, which is what a stop-the-world wants |
| −sizeof(InstancePrefix) .. 0 | **InstancePrefix** (`aot/runtime/AOTInstance.h:72-98`): constants that cost 10 bytes as immediates (`minInt64`, `boxedNaN`, `doubleEncodeOffset`, `one`, `pureNaN`, the `undefined/null/absent` encodings), cell headers (`stringHeader`, `newArrayWith{Int32,Double,Contiguous}Header`, `newCopyOnWriteArrayWithContiguousHeader`), `collectionStructureIDs[4]` (Map, Set, RegExp, Buffer), `structureIDBase`, `emptyString`, `builtinCells` (pointer to table), and **two `LocalAllocator*`**: `ropeStringAllocator`, `arrayAllocator` | C++ at start; `captureCellHeaders()` again after the host installed its globals; `didCreateInstanceStructure()` later for lazily made classes (`AOTInstance.cpp:365-373`) | constants (b); headers/structure IDs (c) single word, zero→value; allocators (a) |
| 0 | **InstanceHeader** (`AOTInstance.h:100-115`) then the 21 parts of `FOR_EACH_AOT_INSTANCE_PART` (`:226-247`) then `State* state`. `sizeof(Instance) <= 32 KB` (`:341`, ARM64 load immediate) | see 1.2 | mixed |
| 32 KB (`offsetOfKeyOrderSteps`) | 2048 × `KeyOrderStepCache` (16 bytes, `aot/object/AOTObjectWords.h:111-116`) | inline code reads; slow path writes | (c)/[value..memory], see 7.12 |
| 64 KB (`offsetOfBuiltinLocationStates`) | one byte `WatchedLocationState` per entry of the builtin table (a `static_assert` says it always fits in the 16 KB, `aot/runtime/AOTBuiltinLocationWatch.cpp:52`) | C++ only; inline code tests the byte | (c) byte flag |
| 80 KB (`offsetOfGlobalBindings`) | `GlobalBinding[ ]`, 24 bytes each (if it fits in 48 KB) | C++ slow path; inline code reads 2-3 words | (c) needs a protocol, H-6 |
| 128 KB (`offsetOfBuiltinCells`) | `JSCell*` per builtin cell key | C++ (lazy fill); inline null test | (c) single word |
| 256 KB (`offsetOfBuiltinValues`) | `EncodedJSValue` per key (`builtins.hostValues`) | C++ (lazy fill) | (c) single word |
| 384 KB (`offsetOfStringCells`) | **string cells**: one `uint32_t` (narrow cell reference) per string constant | **a stub allocates the JSString and stores the word itself**, or C++ | (c), H-9 |
| next 16 KB boundary (`offsetOfSiteCaches`) | **site caches**: `RememberedSlot<Slot>`, 16 bytes per site | C++ slow path only; stubs and inline code read | (c) needs 128-bit or seqlock, H-2 |
| next 16 KB boundary (`offsetOfMethodTable`) | **the Instance's copy of the method table** (section 4) | C++ under `mprotect` | (c) 32-bit words |

Both halves must fit in ±2 GB (`isAddressable`, `AOTInstanceLayout.h:67-71`) so that any of it is `[instanceGPR + disp32]`.

"Instance lives in zeroed pages and is never constructed" (`AOTInstance.h:331`): zero is the meaning "not made yet" of nearly every lazily filled word.

### 1.2 The struct itself

`InstanceHeader` (`AOTInstance.h:100-115`):

| Field | Meaning | Writers after start | Class |
| --- | --- | --- | --- |
| `void** runtimeTable` | table of C++ entry points and a few pointers, per VM (`RuntimeTable::entries()`) | none | (b) |
| `const uint64_t* shapeSets` | image | none | (b) |
| `VM* vm` (offset 16, same as `JSWebAssemblyInstance::offsetOfVM()`, `:337`) | | none | (b) as long as threads share the VM; everything reached *through* it is section 10 |
| `StackManager::Mirror stackMirror` | `{ Atomic<void*> m_trapAwareSoftStackLimit; void* m_softStackLimit; list node }`. **The stack limit of the one thread** | `StackManager` under `m_mirrorLock` from any thread (`runtime/StackManager.cpp:36-100`) | **(a)**: a limit is an address in one thread's stack |
| `uint32_t barrierThreshold`, `uint8_t mutatorShouldBeFenced` | copies of the Heap's words | collector (`initializeCollectorWords`, `aot/runtime/AOTHeapHooks.cpp`) | (c), written while mutators are stopped or racing benignly as in JSC |
| `uint8_t awaitMustSuspend` | flag, see 5.7 | `didChangeMicrotaskQueueing` (`aot/runtime/AOTRuntimeHooks.cpp:116-124`) | (c) if the microtask queue is shared, (a) if each thread has its own queue |
| `slimClosureHeaders[]`, `closureCells`, `singleCharacterStrings`, `numberTag`, `notCellMask`, `globalObject`, `cellCageBase` | constants / table bases | none | (b) |

The parts (`AOTInstance.h:226-247`), with the file that declares each:

| Part | Declared in | Contents and class |
| --- | --- | --- |
| `propertyCaches` `PropertyCacheWords` | `aot/runtime/AOTPropertyCachesWords.h:211-244` | table bases (b, except `hashedSlotOffsetMask` which grows: (c), H-3); generations (c); **counters that stubs RMW** (`absentReadsAnswered`, `missesOfEmptySites`, `storesAtEmptySites`, `storesToGuardedFields`: [value]); **`missIsDueForFiling`: a one-byte message from a stub to the operation it calls next: (a)**; `numberStructureID`/`booleanStructureID` lazily published (c); `handlers[18][28]` code addresses (b) |
| `image` `ImageWords` | `AOTInstance.h:117-146` | pointers into the image (b); `programSymbols` points at a per-VM record whose table grows (7.5) |
| `allocation` `AllocationWords` | `AOTInstance.h:174-193` | subspace pointers (b); `inlineAllocations[9]`, `typedArrayAllocations[16]` = `{LocalAllocator*, header}` filled lazily by `prepareInlineAllocation` (`aot/runtime/AOTOperationScope.h:89-97`): allocator (a), header (c); `frameArrayHeader`, `compactTableHeader` (b/c); `compactTableGrowthPlans` lazy pointer (c); `clonedStructures[16]` direct-mapped cache cleared each collection (c), two words per entry [memory] |
| `heap` `HeapWords` | `aot/runtime/AOTHeapWords.h:19-23` | `StoredCellLog*`, `MarkStackArray* mutatorMarkStack`, `uintptr_t* barriersExecuted`: **compiled code appends to these**: (a) or (d). R5 covers them |
| `realm` `RealmWords` | `AOTInstance.h:198-219` | cells and IDs of the realm (b); `linkTimeConstants[]` (lazy, (c)); `smallIntStrings`, `intStrings` = pointers into the VM's `NumericStrings` caches; `integerKeys` + `integerKeyMask`, `caseConversions` = tables of the Instance's `StringKeyCache`, pointer and mask republished when it grows (`aot/runtime/AOTPropertyCachesStringKeys.cpp:28-34`): **caches that stubs read and C++ fills** (c)/[memory]; `microtaskQueue` (5.7); `readAndWrittenCountsStructure` lazy (c) |
| `numbers` | `AOTInstance.h:221-224` | (b) |
| `runtime` `RuntimeWords` | `aot/runtime/AOTRuntimeWords.h:96-101` | `literals.propertyRuns` (lazy table (c)); **`exceptions` (a)**; `counters` (section 8); `constants` (7.1-7.4; the bump pointer `nextStringHeader` is RMW by a stub: (a) or (c)) |
| `modules` `ModuleWords` | `aot/runtime/AOTModuleWords.h:25-29` | `uint8_t* status` table, `uint32_t evaluatingMark` (section 6): (d) |
| `objects` `ObjectWords` | `aot/object/AOTObjectWords.h:123-142` | bases of the allocation site records, key order transition table, key lists, prototype transitions, enumerator cache, key order runs (7.12) |
| `closures` `ClosureWords` | same file | image table (b), executables, headers (b), `structuresWithPrototypeProperty[4]` lazy (c), `slimClosureAllocators[3]` **(a)** |
| `classes` `ClassWords` | same file | `classPrototypes` table (lazy cells (c)), two small caches |
| `coroutines` `CoroutineWords` | `aot/runtime/AOTCoroutineWords.h:22-60` | constants (job words, headers) (b); **hand-over words (a)**: `waiterToResume`, `waiterResolution`, `waiterAsyncContext`, `stepInProgress`, `frameThatMayContinue`, `stepStatus`, `nextEntryEndsJob`, `asyncContextIsPutBack`, `enteredFrame`, `enteredAsyncGenerator`, `promiseOfEnteredHandler`; `asyncGeneratorAllocator` (a) |
| `builtins` `BuiltinWords` | `aot/builtins/AOTBuiltinWords.h:38-53` | `objects.objectCreate[64]` = `{prototype, header}` cache (c) two words [memory]; `locationStates`, `hostValues` bases (b); `ownSlotStructureIDs[16]` lazy (c) |
| `arrays` `ArrayWords` | `aot/stubs/AOTStubsArrayWords.h:15-27` | heuristics counters, RMW by stubs [value] |
| `iteration` | `aot/runtime/AOTIterationWords.h` | `emptyEnumerator` (b) |
| `json` `JSONWords` | `aot/runtime/AOTJSONWords.h:28-47` | **the open `JSON.parse` source of the running thread: (a)** (`sourceStart`, `sourceEnd`, `sourceOwnerStackPointer`, `runtime`, flags) |
| `stringBuilder` | `aot/runtime/AOTStringBuilderWords.h:16-19` | `freeStorage`: head of a free list that compiled code pops and pushes: **(a)** |
| `stubs` `StubWords` | `aot/stubs/AOTStubWords.h:38-52` | `returnAddresses[]`, entry points, `boxedCalls[64]` (64-byte entries `{nativeFunction, targets[5]}`): all filled once by `initializeStubWords` / `fillStubReturnAddresses` at Instance creation (`aot/stubs/AOTStubEntrypoints.cpp:72-...`, `aot/runtime/AOTInstance.cpp:152-161`): (b) |
| `scratch` `CodegenScratchWords` | `aot/codegen/AOTCodegenScratchWords.h:27-35` | **(a)**: `pendingBarrierOwner`, `expectedCall`, `didEnterCompiledCode` (all three only in validating builds), `misfit` ("the stub ReturnMisfit leaves the value here, and the landing takes it before anything else runs") |
| `constructWords` `ConstructWordCache` | `aot/object/AOTObjectWords.h` | 64 × `{wordOfCall, wordOfConstruct}` cache of a search in a read-only table (c) two words, [memory] if torn (a code address) |
| `lazyStrings` | `AOTInstance.h:148-151` | base and capacity of the process-wide lazy headers (b) |

### 1.3 The statics, in order

`aot/place/AOTStaticsLayout.cpp:383-395` (`regionSteps`):

1. **variables** (`placeVariables`, `:236-258`): every module-level / top-level variable of the program that has storage, grouped by initial bit pattern; first those "filled at program entry" (non-zero initial word: `undefined` or the *absent* = TDZ encoding), then those that start as zero. Representation per variable: `JSValue`, `LiteralString`, `OptionalInt64`, `OptionalDouble`, `OptionalCell32`, `OptionalInt32`, `OptionalBoolean`, raw `Int32`/`Double`/`Cell32`/... (`groupOf`, `:138-174`). A variable may be 1, 2, 4 or 8 bytes wide.
2. **module values** (`placeModuleValues`, `:291-300`): one root word per module that can have a namespace object / value.
3. **guard words** (`placeGuardWords`, `:302-319`): 64 guard bits per word (section 5.3).
4. **module builtin cells** (`:322-329`): "A module runs at most once, so it has one module object, one require function and one `import.meta`."
5. **shared storage** of copy-on-write array literals (root words).
6. (storage in the image: no statics)
7. **RegExp cells**: one root word per RegExp literal site.
8. **template objects**: one root word per tagged template site.
9. **array allocation profiles**: one word per profiled array literal (section 8).
10. **last arrays**: one word per profiled literal, *cleared each collection*.
11. **free environment cells**: one word per environment layout that `ReuseEnvironments` reuses, *cleared each collection* (7.13).

Initialisation: `ProgramStatics::ProgramStatics` (`aot/runtime/AOTProgramStatics.cpp:190-209`) fills the second half from run-length records of the image and **write-protects the first pages** ("only stores are caught until the program is entered"); `initializeProgramStatics()` (`:73-81`) unprotects, fills, `storeStoreFence()`, sets `m_programStaticsInitialized`. That is a check-then-act on a plain `bool` (`ensureProgramStaticsInitialized`, `AOTProgramStatics.h:34-38`): (d) or do it before any second thread starts.

The collector treats the statics as roots found by cards; `RootVolatility::GreyedByExecution` (`AOTProgramStatics.cpp:204`) = rescanned at every stop.

### 1.4 How compiled code addresses it

- `instanceGPR` is pinned in every compiled function and stub (`aot/codegen/AOTPrimitiveEmitter.cpp:180-190`). `pinned.instance = argumentRegister(instanceGPR)`, `pinned.statics = instance + offsetOfStatics(sizes)` (`:198-210`). All accesses are `[instanceGPR + constant]`, with `AOT_INSTANCE_OFFSET(path)` / `AOT_INSTANCE_PREFIX_OFFSET(member)` (`AOTInstance.h:38-39`).
- The layout is part of the image's stamp: `runtimeLayoutHash()` (`AOTInstance.h:352-374`) and the "runtime facts" (section 10).
- B3 is told which loads are immutable: heaps `AOTInstance_fixedFields` (hoistable, CSE-able for ever) versus `AOTInstance_mutableFields`, `AOTInstance_siteCaches`, `RuntimeState`, `AllocatorState` (`aot/codegen/AOTHeaps.h`). **Anything moved from "fixed" to "per thread" keeps working only if the pinned register itself becomes per thread.**

### 1.5 How many, and who gets one

- `runsCompiledProgram(realm)` = `vm.hasCompiledCode() && realm->globalObjectMethodTable()->moduleHost` (`aot/runtime/AOTRealmStartup.cpp:44-47`). Only such a realm gets an Instance (`willInitializeRealm`, `aot/runtime/AOTRuntimeHooks.cpp:29-33`).
- In Bun the only method table with a `moduleHost` is `StandaloneGlobalObject::globalObjectMethodTable()` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:1246-1277`), chosen by `methodTableOfProgram()` when the process has a standalone module graph (`:582-596`).
- **Main thread**: one VM, one `Zig::GlobalObject`, one Instance.
- **Worker**: its own `VM` (`Zig__GlobalObject__create` → `VM::tryCreate`), so its own `VMState` (attached because `Image::isRegistered()` is process-wide), its own heap and cage, its own Instance, its own property cache tables. "a worker, which runs the program once more in a VM of its own, is none of them" (`aot/valueflow/AOTValueFlowResultsBuiltins.cpp:1184`). What Workers already share with the main thread, i.e. what is already multi-thread clean: the image, `StructureRows::singleton()` (indexed by StructureID, and the structure heap is process-wide, so the words of two VMs are disjoint), `BuiltinTable`, the lazy string headers (section 9).
- **ShadowRealm**: `deriveShadowRealmGlobalObject` makes a plain `Zig::GlobalObject` with the default table (`bun:src/jsc/bindings/ZigGlobalObject.cpp:1141-1155`), no `moduleHost`, so **no Instance**; `didCreateRealm` only calls `refuseEvaluationUnlessAdmitted` and `installGeneratorMethodsForCompiledGenerators` (`aot/runtime/AOTRuntimeHooks.cpp:80-94`). Code in it is interpreted. **node:vm contexts**: the same (no `moduleHost` found for them; PR text: "Realms of `node:vm` and `ShadowRealm` have built-ins of their own, which can be changed").
- **A second Instance in one VM** is possible only if the image says so: `reserveInstance` returns null when `!(flags & mayRunInMultipleInstances) && !instances.isEmpty()` (`AOTInstance.cpp:349-350`). The flag is clear when flow proves `ProgramInstances::AtMostOne` (`aot/place/AOTProgramTableEncoder.cpp:145`), i.e. no builtin flagged `StartsInstance` ("Makes a second realm that runs the compiled program in the VM of its caller", `aot/builtins/generator/vocabulary.ts:552`) is reachable. No `.builtins.ts` in the checked-out trees sets `startsInstance`; in Bun the candidate is the test-isolation realm (`Zig__GlobalObject__createForTestIsolation`, mentioned at `ZigGlobalObject.cpp:1144`). **`AtMostOne` is used by about 15 passes and emitters as a licence** (`grep AtMostOne aot/`: `AOTShapeTestEmitter`, `AOTClosureTestEmitter`, `AOTMethodDispatch`, `AOTEscapeAnalysis`, `AOTAllocateInnerObjects`, ...): one Instance ⇒ one class object per class, one closure per top-level function. Threads that share one realm keep that property; threads with one Instance each would break it.
- When a second Instance appears, the VM-wide lookup tables are cleared and become restricted (`VMPropertyCaches::didCreateInstance`, `aot/runtime/AOTPropertyCaches.cpp:98-110`; `canCache`, `:532-543`).

### 1.6 Creation, in order

1. `Bun__AOT__useImage` → `AOT::useImage(bytes, file, identity)` registers the process-wide image (`bun:src/jsc/bindings/BunAOTRuntime.cpp:95-111`).
2. `VM::VM`: right after `structureStructure` is made, `VMState::attachImage` (`aot/runtime/AOTVMState.cpp:39-72`): makes `caches`, `counters`, sets `hasCompiledCode`, adds marking constraints, makes `reporter`, `runtimeTable`, `identifiers`, `callSiteCache`, `propertyCaches`, `regExpLiterals`, `regExpMatchOutput`, two Structures. Also widens the soft reserved zone.
3. `JSGlobalObject::init` start → `willInitializeRealm` → `reserveInstance` (`AOTInstance.cpp:339-362`): `runtimeTable->finishInitialization`, `identifiers->createEngineSymbols`, `reserveMemory` (+ `copyMethodTable`), `fillHeaderAndPrefix`, `fillImageWords`, `fillNumberWords`, `fillAllocationSpaces`, `propertyCaches->publishWords`, `realm->setAOTInstance(instance)`.
4. `JSGlobalObject::init` end → `didCreateRealm` → `makeBuiltinsImmutable` (no-op with a host) → `Instance::create` (`AOTInstance.cpp:387-424`) under `DeferGCForAWhile`: `captureCellHeaders`, `fillRealmWords`, `initializeCollectorWords`, `initializeStubWords`, `initializeIterationWords`, `State::create`, two "cleared each collection" registrations, `fillInstanceClassInfos`, `initializeRuntimeWords`, `registerStackMirror` (+ poll page), append to `vmState.instances` under `instancesLock`, `propertyCaches->didCreateInstance`, `didChangeMicrotaskQueueing`.
5. Bun: `GlobalObject::finishCreation` end → `AOT::hostDidCreateRealm(this)` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:3035`) → `didInstallBuiltins` (`aot/runtime/AOTRealmStartup.cpp:586-601`): locks the builtins, `fillCellsUnderModules`, marks the original Structures pristine, **`captureCellHeaders()` a second time** (headers carry the pristine bit).
6. Nothing loops over the program: every table is zero pages; the first use fills it.

Destruction: the realm's death puts the Instance on `instancesToDestroy`; `destroyDeadInstances` runs at the next Instance creation; `destroyAll` at VM shutdown (`AOTInstance.cpp:426-454`).

---

## 2. Registers, and where "per thread" things are kept today

### 2.1 Pinned registers

| | x86-64 | ARM64 | Holds | Source |
| --- | --- | --- | --- | --- |
| `instanceGPR` = `GPRInfo::jitDataRegister` | **r13** | **x26** | `Instance*` | `aot/stubs/AOTStubRegisters.h:20`, `jit/GPRInfo.h:121,251` |
| `cellCageBaseGPR` | **r14** | **x25** | base of the heap's MarkedBlock cage (decode of a 32-bit reference = base + (word << shift)) | `AOTStubRegisters.h:22,26` |
| number tag | (memory operand `[r13 + numberTag]`) | **x27** | `JSValue::NumberTag` | `AOTPrimitiveEmitter.cpp:184-186`, `aot/codegen/AOTB3Policy.cpp:47-92` |
| not-cell mask | (memory operand) | **x28** | `JSValue::NotCellMask` | same |
| platform | | x18 pinned (never used) | | `AOTPrimitiveEmitter.cpp:187-188` |
| frame pointer / stack pointer | rbp / rsp | x29 / sp | some functions have no frame record ("frameless") | |

Not pinned, roles at a call by convention (`aot/stubs/AOTConvention.h:19-31`): `thisGPR` rax / x8, `countGPR` r10 / x9, `calleeGPR` r8 / x10, `valueEntryGPR` r12 / x11; arguments in the C argument registers (4 on x86-64, 8 on ARM64; direct calls use 6+2 on x86-64). `throwWordGPR` r11 / x16. On x86-64 r12 is a callee-save that compiled code does *not* save (`unsavedCalleeSaveGPRs`), r15 is clobbered at suspensions; rbx, r15 are ordinary allocatable callee-saves.

**There is no register that holds the VM, a thread, or a top-of-stack record.** The cheapest path to "per-thread" data today is `[instanceGPR + k]` (one load) or `instance->vm` then `[vm + k]` (two loads). No TLS access is emitted by stubs or compiled code (grep for `loadFromTLS`, `storeToTLS`, `tpidr`, `%fs`, `fastTLS` in `aot/stubs`, `aot/codegen`, `aot/builtins`, `aot/select`, `llint/LowLevelInterpreterAOT.asm` finds nothing). C++ has eight `thread_local` variables, all small helpers: `aot/runtime/AOTPrivateNameOperations.cpp:202` (a cache `Remembered places[]`), `AOTErrorMessages.cpp:472`, `AOTNativeCallValidation.cpp:114`, `AOTRuntimeValidation.cpp:343`, `aot/image/AOTFaultHandler.cpp:74` (`s_scratchRegisterBeforeFault`), `aot/object/AOTObjectInternal.cpp:42`, `aot/object/AOTAssignmentPlans.cpp:111`. They exist because Workers run the same C++ on other threads.

How the registers get their values: only in the **adapter** at the border from JSC's convention. `emitFindInstance` (`aot/stubs/AOTStubsCallAdapters.cpp:97-104`):

```
callee   = frame[CallFrameSlot::callee]
vm       = VM of the callee's MarkedBlock / PreciseAllocation (loadCellVM)
structure= callee->structureID + vm->m_aotState.structureIDBase
instance = structure->realm()->m_aotInstance
```

then `emitAdapterPrologue` (`:119-135`) saves the caller's r13/r14 (x25-x28) in the `AdapterFrame`, stores the Instance at `AdapterFrame::offsetOfInstance`, `mov instance → instanceGPR`, `cellCageBaseGPR = [instance + cellCageBase]`. So **the Instance is a function of the callee's realm, not of the running thread**. C++ enters compiled code through `vmEntryToJavaScriptWithNArguments(entry, &vm, nullptr, callee, this, context, args...)` where `entry` is the closure's adapter entry point (`enterCompiledCode`, `aot/runtime/AOTCompiledCall.h`), so it takes the same path: no caller hands an Instance over. After an unwind "the callee-saved registers are those of the frame of the handler, so the instance is the one that unwinding has left the word in" (`aot/stubs/AOTStubsExceptions.cpp:85`).

Design consequence: if the pinned register is to point at per-thread memory, the places that materialise or recover it are (1) the adapters (the only producers), (2) unwinding, which restores it from the callee-save buffer / `AdapterFrame`, (3) the fault handler (`instanceIn(registers)`, `aot/image/AOTFaultHandler.cpp:155,203`), plus stack walkers that read `AdapterFrame::offsetOfInstance`.

### 2.2 Where each "thread-ish" thing lives

| Thing | Where | Who writes | Notes |
| --- | --- | --- | --- |
| **Pending exception** | `VM::m_exception` (`VM::exceptionOffset()`) | C++ `throwException`; cleared by C++ in the unwinder/catch | Most operations *return* the exception flag in the second result register, so the hot path does not read memory: `Returns::Value` → test `returnValueGPR2`, `Returns::Void` → test `returnValueGPR`; only `Returns::Double`/`Pair` load `instance->vm->m_exception` (`aot/stubs/AOTStubAssemblerOperationCalls.cpp:181-201`). Other direct readers: polls (`aot/stubs/AOTStubsPolls.cpp:107,132,154`), host calls (`AOTStubHelpersFrames.cpp:150`, `AOTStubAssemblerCalls.cpp:93`, `AOTStubsConstruct.cpp:119`), coroutine resumption (`AOTStubsCoroutineResumption.cpp:198,332,354`). **(a)** |
| **Thrown value on its way to a compiled `catch`** | `instance->runtime.exceptions.thrownValue` (`aot/runtime/AOTRuntimeWords.h:33-41`) | unwinder C++ (`aot/runtime/AOTCallSlowPaths.cpp:76-107`, `AOTExceptionOperations.cpp:90,353,367`), stubs (`aot/stubs/AOTStubsOperations.cpp:282-296`, `AOTStubsCoroutineResumption.cpp:588`) | a GC root (`AOTProgramStatics.cpp:172`). Written, then read-and-zeroed by the landing. **(a)** |
| `stackPointerOfHandler` | same struct | unwinder; read by `loadStackPointerOfHandler` on x86-64 (`AOTStubsExceptions.cpp:86-94`) | "from there to the jump to the handler". **(a)** |
| `nameOfUncallableMethod` | same struct | stub `FindMethodById`, read by the "not a function" error path | **(a)** [value: wrong error text] |
| `VM::targetMachinePCForThrow`, `callFrameForCatch`, `targetMachinePCAfterCatch` | `VM` | generic unwind | `AOTStubsExceptions.cpp:81,204-206`. **(a)** |
| **topCallFrame** | `VM::topCallFrame` | **not stored by stubs in release builds**: `prepareCallOperation()` stores it only `#if ASSERT_ENABLED` (`AOTStubAssemblerOperationCalls.cpp:46-53`). Each C++ operation stores its *own frame record*: `AOT_OPERATION_BEGIN` takes `__builtin_frame_address(0)` and `AOTOperationPrologueCallFrameTracer` does `vm.topCallFrame = operationFrameRecord` (`aot/runtime/AOTOperationScope.h:61-85`, `interpreter/FrameTracers.h:168-179`). Stubs that call *host functions* do store it: `AOTStubsCallHost.cpp:78`, `AOTStubHelpersFrames.cpp:146`, `AOTStubsConstruct.cpp:116`, `AOTStubsPropertyAccessors.cpp:142` | **(a)** |
| **topEntryFrame** | `VM::topEntryFrame` | `vmEntryToJavaScript` etc.; read by `AOTWasmStubs.cpp:43`, validation, `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer` in the unwind stub | **(a)** |
| **Frame chain** | the machine stack: rbp/x29 chain; a compiled frame has no CodeBlock; the walker maps each return address to image metadata (through the per-VM `CallSiteCache`, 3.7) | | the chain itself is naturally per thread |
| **Stack limit** | `instance->stackMirror.{m_trapAwareSoftStackLimit, m_softStackLimit}` | `StackManager` | prologue check: `cmp sp, [instance + trapAware]` (`aot/codegen/AOTStackCheckEmitter.cpp:32`), big frames against `m_softStackLimit` (`:68`). Functions with frames ≤ 240 bytes skip the check up to 8 deep / 4 KB (`AOTConvention.h:62-67`). The trap-aware word doubles as **the stop request**: `requestStop()` stores `stopRequestMarker()` in the VM's word and in every mirror (`runtime/StackManager.cpp:36-49`). ARM64 (and non-Linux) loops poll the sign bit of that word (`aot/codegen/AOTHintEmitter.cpp:51`). **(a)** for the limit; the stop request must reach *every* thread's copy |
| **Allocator free lists** | `LocalAllocator` objects of the VM's heap; compiled code holds `LocalAllocator*` in: `AllocationSiteRecord::allocator` (per allocation site group, `aot/object/AOTAllocationSiteRecord.h:16-27`), `InstancePrefix::{arrayAllocator, ropeStringAllocator}`, `AllocationWords::inlineAllocations[].allocator`, `typedArrayAllocations[].allocator`, `ClosureWords::slimClosureAllocators[3]`, `CoroutineWords::asyncGeneratorAllocator`, `ConstantCellWords::stringAllocator`; and computes them from `CompleteSubspace*` (`auxiliarySpace`, `cagedAuxiliarySpace`, `cellSpace`, `primitiveAuxiliarySpace`) by size class | inline bump/free-list pop: `emitAllocateFromFreeList` (`aot/stubs/AOTStubHelpersAllocation.cpp:36-48`) | plain loads and stores on `FreeList`. **(a)**, [memory]. An `AllocationSiteRecord` couples three things of different class in 32 bytes: header (c), order+shape words (b), allocator (a) |
| **Scratch buffers** | (1) the stack (argument buffers are stack slots, `argumentBuffer()`); (2) `instance->scratch.*`; (3) `VMState::regExpMatchOutput`, **one `int[]` per VM for the ovector of every match** (`aot/stubs/AOTStubsRegExpFrames.h:79`: `vm + offsetOfAOTState + offsetOfRegExpMatchOutput`); (4) `VM::m_regExpAllocator`-style things reached from C++ | | (3) is **(a)** [memory: a torn ovector gives out-of-range substring indices] |
| **Write barrier buffers** | `instance->heap.{storedCellLog, mutatorMarkStack, barriersExecuted}`, `instance->scratch.pendingBarrierOwner` | inline code / stubs | **(a)** or (d); R5 |
| **Microtask queue** | `instance->realm.microtaskQueue`; stubs enqueue directly using the precomputed job words in `CoroutineWords` | stubs, C++ | (a) if each thread drains its own queue; otherwise (d) |


---

## 3. Property caches by name

No code is patched. There are three levels, probed in this order by a read such as `o.x` that is not proven:

1. the **field table** of the image (read-only; for compiled objects: shape × name → field record; `readFieldByTable`, `aot/stubs/AOTStubsPropertyProbes.cpp:264`). Not state. (b).
2. the **site cache** of the access site, if it has one (per Instance).
3. the **lookup tables of the VM**: the **front table** (`RowDisplacementTable`) for names whose identifier number is ≤ 32767, else/also the **hashed table** (`HashedSlotTable`).
4. helpers that answer "absent" without an entry: **absence filters**, `namesOfObjectPrototypes`, `namesChangedOnChains`.
5. on x86-64, a stub also **walks the `PropertyTable` of the Structure itself** (`findInPropertyTable`, `aot/stubs/AOTStubsPropertyRead.cpp:117-...`: loads `Structure::seenProperties`, `propertyTableUnsafe`, its index vector and entries, with no lock).
6. the C++ slow path (`operationAOTGetById...` in `aot/runtime/AOTPropertyCachesByIdOperations.cpp` etc.), which answers, and *files* the answer from the 2nd/3rd miss on.

Owner objects: `VMPropertyCaches` (`aot/runtime/AOTPropertyCaches.h:47-129`, one per VM) holds `m_customAccessors`, `m_front`, `m_hashedTable`, `m_columnsByNameAddress`, `m_absenceFilters[256]`, two generations, two name bit sets, `m_missesOfPairs[32768]`, `m_namesOfTexts[128]`. `PropertyCaches` (`:142-207`, one per Instance, lazily made) holds the policy: `m_missCounters[32768]`, `WatchedValues`, `StringKeyCache`, `m_additionsSinceCollection`.

### 3.1 What an entry says: `Slot` (16 bytes) and `PackedSlot` (8 bytes)

`Slot` (`aot/runtime/AOTRuntimeRecords.h:92-278`), `alignas(16)`:

```
word 0 ("header"):  StructureID structureID (32) | Kind kind (8) | flags (8) | location (16)
word 1 ("payload"): union { JSCell* holder; {int32 wordIndex}; {SlotData slot, uint32 type}; {SlotData slot, uint32 holder32};
                            {StructureID, int32 wordIndex}; {StructureID newStructureID, uint32 newStorageCapacity};
                            {uint32 customAccessorIndex}; EncodedJSValue constant }
```

28 kinds (`:62-90`): `Empty, Value, FarValue, Absent, Getter, Setter, CustomGetter, CustomSetter, Field, FieldGetter, FieldCreation, FieldOrInherited, ..., Variable, ExtraProperty, ..., Transition, ArrayLength, ..., Constant, Uncacheable`. **The meaning of the payload depends on `kind` in the header.** A reader that pairs the header of one fill with the payload of another interprets, e.g., a `StructureID|capacity` pair as a `JSCell* holder`: [memory].

`PackedSlot` (`:430-459`): `payload(32) | location(8) | biasedKindAndFlags(8) | name(16)`: one 64-bit word; everything that decides its meaning is in that word.

### 3.2 Hashed table

- Entry `HashedSlot` (`aot/runtime/AOTPropertyCachesWords.h:18-48`), 32 bytes, `alignas(32)`: `Slot slot` at 0, `uintptr_t key` at 16 (`uid | access`, access in the low 2 bits), 8 bytes unused. **Three meaningful words.** The full key `(StructureID, uid, access)` is split: StructureID in word 0, uid+access in word 2.
- 4-way set associative: `offsetOfFirstWay = (((structureID * 0x9e3779b1) >> 11) + (uint32(uid) << 1)) & offsetMask`, way *w* at `offset ^ (w << 5)` (`:30-36`).
- Storage: addresses for the largest size are reserved once; "it never moves" (`aot/runtime/AOTPropertyCachesHashedTable.h:30`). Starts with 1024 entries; `grow()` doubles `m_numberOfEntries` in place (`AOTPropertyCachesHashedTable.cpp:187-211`).
- **Probe** (`findHashedSlot`, `aot/stubs/AOTStubsPropertyProbes.cpp:1123-1174`), in order:
  1. x86-64: `and entry, [instance + hashedSlotOffsetMask]`, then `mov table, [instance + hashedSlots]` (two separate loads); ARM64: one `ldp` of both.
  2. per way: compare `[entry + 16]` (key) with `uid|access`; if equal compare `[entry + 0]` (32-bit StructureID).
  3. on a hit: x86-64 `load8 kind`, `load64 header`, `load64 payload` (three more loads); ARM64 `ldp header, payload`.
  4. `jumpToSlotHandlerOfKind`: indirect jump through `instance->propertyCaches.handlers[table][kind]`.
  So a hit reads key, structureID, kind, header, payload at up to **five different moments**.
- **Fill** (`set` → `chooseEntry` → `write`, `:110-131`, `:169-179`): find the entry of the same key, else an empty way, else evict way `m_nextVictimWay++ % 4` (`erase`: `entry.clear()` = plain assignment of a zero `HashedSlot`, then `uid->deref()`), then `uid->ref()`, `++m_numberOfFilledEntries`, then `entry.set(m_writtenRegions, HashedSlot{slot, key})` = **plain struct assignment of 32 bytes** (`heap/RememberedSlot.h:125-129`) followed by `record()` (card byte + append to a `Vector`). The compiler chooses the order of the stores. No fence, no version.
- **Grow**: `m_numberOfEntries *= 2`; every entry of the lower half whose hash has the new bit is copied up and the old one cleared; then `publishWordsToEveryInstance()` writes the new mask into every Instance (`aot/runtime/AOTPropertyCaches.cpp:358-359`, `:137-155`). Between the move and the publication a reader with the old mask misses (harmless); a reader with the new mask never sees an unmoved entry only because there is no concurrent reader.
- **Removal**: `remove`, `removeChainDependentEntries` (only entries with `Slot::dependsOnChain`), `clear`, `removeDeadEntries` at the end of a collection (`:262-284`): an entry is dead if `!slot.namesOnlyLiveCells(isMarked)` (`AOTRuntimeRecords.h:400-428`: its Structure, holder, new Structure, constant) or its custom accessor record is unused. Names erased during a collection are `deref`ed later by the mutator (`m_namesErasedDuringCollection`).
- The table holds a **reference count on each uid** (plain `ref()/deref()` on a `StringImpl`; atoms of the image are static and ignore it).

Could a reader see a half-written entry? **Yes, in every way**: key of the new fill with header of the old; header of the new with payload of the old; an entry that is evicted and refilled for another `(structure, uid)` between the key compare and the payload load. Class: **(c) with a real protocol or (d)**; severity **[memory]**.

### 3.3 Front table (row displacement)

- Three planes (Read, Store, Define) of `PackedSlot` (2^21 + 2^15 cells each, reserved, committed on touch) + an occupancy bit map (`aot/runtime/AOTPropertyCachesFront.cpp:28-59`), **per VM**.
- **`StructureRows::singleton()`: per process.** One `uint32_t` per 64 bytes of the structure heap: `rowsForStructureIDZero[structureID >> 6]` = *base* of that Structure's row, 0 = none (`aot/runtime/AOTPropertyCachesStructureRows.h:19-38`, `.cpp:25-50`). Two VMs never share a word because they never share a Structure.
- A cell belongs to `(Structure S, name n)` iff `index == base(S) + n` **and `cell.name == n`**. Rows of different Structures interleave ("row displacement"): the 16-bit name in the cell is the only check. Nothing in the cell names the Structure.
- **Probe** (`loadFrontCell`, `AOTStubsPropertyProbes.cpp:1041-1050`, ARM64; `locateFrontCell` + `returnByFrontCell`, `:1006-1038`, x86-64):
  1. load `rowsForStructureIDZero` and `plane` from the Instance;
  2. `row = load32 rows[structureID >> 6]`;
  3. `cell = plane[row + column]`. ARM64 loads the 8 bytes once. **x86-64 reads the cell in pieces**: `cmpw [cell+6], column`; `cmp word [cell+4], 0`; `mov payload32, [cell+0]`; (or later `load64 [cell]`).
  4. own inline value: `result = [receiver + payload*8]`.
  A Structure without a row has base 0, and cell `0 + n` is never reserved for a name (bases start at 1; `m_bases.first = 1`).
- **Fill** (`trySet`, `AOTPropertyCachesFront.cpp:97-112`): pack; `ensureIndex` (`AOTPropertyCachesFrontRows.cpp:242-266`) may `createRow` (`:178-204`: reserve marker cell, reserve name cell, **then** `setRowOf(structure, base)`), may **move another Structure's row** out of the way or move this row (`moveRow`, `:206-240`: `vacate(base)`, reserve at the new base, copy each cell, `vacate(from)`, and **last** `setRowOf(owner, newBase)`), may **evict rows** (`findBaseEvictingRows`, `:70-99`, `release`, `:268-281`: vacate all cells, `setRowOf(owner, 0)`).
  Each cell write is one 64-bit store (`writeCell`), plus side structures: `m_occupied` bit RMW, `m_dependsOnChain` `BitVector`s (may reallocate), `m_rows` `SegmentedVector`, `RowColumns` (malloc), `m_freeRows`, `m_recentRows`.
- The `dependsOnChain` flag is *not* in the packed cell; it is in `m_dependsOnChain[access]` (`AOTPropertyCachesFront.h:172`).
- **The stale-base hazard.** A reader that has loaded `base(S)` and is then delayed can find, at `base(S) + n`, a cell that now belongs to row of `S2` whose base equals the old base of `S` (after `moveRow`/`release` of S and `createRow` of S2 at the freed base; `createRow` even *prefers* `base of the last similar row + 1`). `cell.name == n` passes, and the reader applies S2's offset/holder/transition to an object of Structure S. **[memory]** (out-of-bounds inline offset, wrong `newStructureID` on a store).
  This cannot be fixed by making cell writes atomic; it needs either (i) quiescence before a base is reused (epoch / safepoint reclamation: bases freed are reusable only after every mutator has passed a poll), (ii) rows that never move while mutators run (moves only at stop-the-world), or (iii) a per-thread front table.
- Sweep: `removeDeadEntries` (`AOTPropertyCachesFront.cpp:188-207`): release rows of dead Structures, clear cells that name dead cells, `compactBases`, decommit empty pages (`madvise`) after a full collection.
- VM shutdown gives the words of `StructureRows` back before the heap dies (`aot/runtime/AOTVMState.cpp:74-79`).

### 3.4 Filing policy (who decides to write)

`PropertyCaches::cache` (`aot/runtime/AOTPropertyCaches.cpp:559-581`), in order: `canCache` → `publishIfStandsForPrimitives` (writes `numberStructureID`/`booleanStructureID`) → `ensureChangesAreReported(changeableObjects)` (calls `didBecomePrototype` on every object the answer depends on, i.e. **transitions those objects' Structures** so that they report) → `slotFor(answer)` → `m_tables.set(...)` → `registerChainDependents` → `m_siteCaches.fill` → `didAddProperty`.
Note the order: **the entry is visible before its invalidation hook (`chainDependents->add`) exists.** Single-threaded that is fine.

Counters that gate filing: `shouldFile` (`:666-705`) reads-and-clears `instance.propertyCaches.missIsDueForFiling` (set by the stub `countMissOfPair`, `AOTStubsPropertyRead.cpp:512-527`, one instruction before it tail-calls the slow path: a **register-like message through shared memory**, (a)), then RMWs `m_missCounters`. `m_shouldForgetMisses` is a `std::atomic<bool>` set by the collector.

### 3.5 Invalidation

| Event | Mechanism | Effect |
| --- | --- | --- |
| The receiver gets/loses an own property, changes attributes | its StructureID changes; entries are keyed by StructureID | nothing to do. Dictionaries: only an own `Value` of a *cacheable* dictionary is filed (`isValueHeldByCacheableDictionary`, `:527-530`); uncacheable ones never |
| An object *above* the receiver changes (add, delete, attribute change, `setPrototypeOf`) | JSC's existing hook for objects whose Structure `mayBePrototype()`: `VM::invalidateStructureChainIntegrity(event, changed, uid)` (`runtime/VM.cpp:2470-2496`) → `RuntimeHooks::didChangePrototypeChain` → `didChangeChainObject` (`aot/runtime/AOTChainDependents.cpp:238-255`) | see 5.2 |
| A value that was filed as `Constant` is replaced | `PropertyValueWatchpoint` (an `AdaptiveInferredPropertyValueWatchpointBase`) fires | clears the site (`WatchedValues::didFire`, `aot/runtime/AOTPropertyCachesWatchedValues.cpp:49-58`) |
| Structure / holder / uid dies | `CacheRegistry::finalize` at the end of marking, world stopped (`heap/Heap.cpp:738-746`) | entries removed; absence filters, cloned structures, etc. zeroed ("A filter names Structures by their numbers, and the number of one that has ended goes to another", `AOTPropertyCaches.cpp:54`) |
| A second Instance appears in the VM | `didCreateInstance` | both tables cleared, `m_isSharedByInstances` restricts what may be filed |
| Global lexical binding added | `bumpGlobalLexicalBindingEpoch` → `didChangeGlobalLexicalBindings` (`AOTChainDependents.cpp:273-285`) | as a nameless change of the global object + global binding records dropped |

**The single-thread assumption behind all of it:** "the change, then the report, then the next access" is a sequence. `VMPropertyCaches::UnreportedChange` (`AOTPropertyCaches.h:86-100`) says it outright: "An object of some chain that is changed first and reports behind that. ... **No compiled code runs there**, so validation leaves alone what the report is about to drop." With a second mutator, compiled code does run between the change and the report, and it still hits the stale entry. For a *read* the result is a value that was correct a moment ago: linearisable as "the read came first" as long as the stale entry's holder/offset still exist: [value]. For an entry whose payload names an offset in a holder whose storage has been reshaped by the change (delete + flatten, dictionary conversion): the holder's Structure is not rechecked by `Value`-with-holder slots (`loadFromValueSlot` reads `[holder or holder->butterfly + index*8]` with no check of the holder's Structure, `aot/stubs/AOTStubsPropertySiteCaches.cpp:66-84`): **[memory]** if the butterfly shrank/moved, otherwise [value].

### 3.6 Site caches

- One `RememberedSlot<Slot>` (16 bytes) per site that "can run more than once", up to a limit (`placeSiteCaches`, `aot/place/AOTSiteCachePlacement.h:58-80`: "Nothing reads what a site that runs at most once would remember, so it has no cache"). In the Instance at a constant offset; compiled code passes the *address* to the stub (`siteCacheAddress`, `aot/codegen/AOTOperationEmitter.cpp:155`).
- Side state: `SiteCacheState { refillCount, missesLeft }` 2 bytes, 4096 of them **shared by address hash** (`AOTRuntimeRecords.h:595-610`); `m_sitesThatMissed` bit set; `missesOfEmptySites` one byte for the whole Instance.
- **Probe**: ARM64 `emitSiteProbe` (`AOTStubsPropertySiteCaches.cpp:87-100`): `ldp header, payload, [site]` (a 16-byte pair load: single-copy atomic only with FEAT_LSE2 and alignment, which holds here: `alignas(16)`), compare low 32 bits with the StructureID. x86-64 `generateGetByIdAtSiteAskingSiteFirst` (`:140-...`): `load64 header`; `cmp32 header, [receiver]`; `load64 payload`; `cmp8 [site+4], Value`: **three separate reads of the site**.
- Inline probes in compiled code: `PropertyRunEmitter::tryStore` (`aot/codegen/AOTFusedAccessEmitter.cpp:168-204`) loads the header, compares, later `storeThroughSiteCache` (`:231-245`) stores the values at `object + location*8`, **then loads the payload (`newStructureID`) from the site again** and stores it into the object's header. `loadConstantSlotAtSite` (`aot/stubs/AOTStubsPropertyInlineCode.cpp:30-36`): `load8 kind`, compare with `Constant`, `load64 payload`.
- **Miss counting by stubs and inline code**: `countSiteCacheMiss` (`AOTStubsPropertyProbes.cpp:1406-1428`: `sub byte [states + hash(site) + 1], 1`, carry → refill), `countMissOfEmptySite` (`:1430-1444`: `add byte [instance + missesOfEmptySites], 8`, carry → sample). Non-atomic RMW: [value] (only timing of fills).
- **Fill**: only C++: `SiteCaches::write` (`aot/runtime/AOTSiteCaches.cpp:102-110`): stop the watch of a replaced constant, `site->set(m_writtenRegions, slot)` (plain 16-byte assignment + card), state update. `clear` = plain zero assignment (`:141-145`).
- **Roots**: a full collection keeps alive the Structures that sites name if they "hang on marked cells only" (`visitRoots`, `:197-230`); `removeDeadEntries` clears the rest.
- Class: **(c)**: 16-byte atomic store/load (`cmpxchg16b`/`movdqa` with AVX guarantee, `stp/ldp` with LSE2) would make header+payload consistent; then the remaining question is only staleness (3.5). Or **(a)**: per-thread site caches (memory × threads; sizes: 16 bytes × number of sites).

### 3.7 "Call site cache"

`CallSiteCache` (`aot/runtime/AOTCallSiteCache.h:19-78`), one per VM, **C++ only**: a 2-choice hash table from a *return address* to decoded image metadata (function, call site record, handler, frame shape, shown-frame counts), ≤ 64 bytes per entry, 128 → 2048 entries, plus 16 remembered source positions and an `InlineFrameReader` scratch object. Used by every stack walk: unwinding (`aot/runtime/AOTUnwinding.cpp:238`), `CompiledFrameIterator` (`AOTCompiledFrameIterator.cpp:64,150,186`), stack traces (`AOTStackTrace.cpp:51,484`), `AOTReturnAddressWalk.h:25`, async stack traces, and `AOTCallSlowPaths.cpp:44-46`.
`resolve()` (`AOTCallSiteCache.cpp:121-133`): find, else `slotForNewEntry` (may `setNumberOfEntries` = allocate a new `FixedVector` and **free the old one**, `:85-101`) and overwrite. "The next resolve() may move or replace the entry that either of the two returns" (`AOTCallSiteCache.h:50`): callers hold `const Entry*` across short stretches.
All content is a pure function of the read-only image. Class: **(a)** (a small cache per thread is the obvious answer) or (d). Severity if shared unlocked: **[memory]** (dangling `Entry*`, torn entry → wrong frame size → wild unwind).
No use from another thread was found: the sampling profiler's thread walks compiled frames with the read-only `Image` alone (`FrameWalkerWithCompiledFrames`, `runtime/SamplingProfiler.cpp`), which shows that a walk without this cache exists.

### 3.8 Other per-VM / per-Instance lookup caches that stubs read

| Cache | Where | Entry | Reader | Writer | Class / severity |
| --- | --- | --- | --- | --- | --- |
| Absence filters | `VMPropertyCaches::m_absenceFilters[256]`, 64 bytes each (`AOTPropertyCachesWords.h:156-179`) | `structureID, generation, names[4], readsAnswered, generationForStores, namesThatStopStores[2]` | stub: compare structureID, compare generation with the Instance's copy, test a bit (`branchUnlessFilterRulesOut`, `AOTStubsPropertyRead.cpp:85-103`; stores: `aot/stubs/AOTStubsKnownFieldStores.cpp:57-90`); **stub increments `readsAnswered`** | C++ `filter = filled` (64-byte struct copy, `AOTPropertyCaches.cpp:263`); zeroed each collection | (c) seqlock-like: a torn filter (new structureID+generation, old bits) answers "absent" wrongly: [value] for reads (`undefined` instead of a value); for stores "creates own property" past a setter/read-only: [value] |
| `absenceFilterGeneration(ForStores)` | VM + a copy in every Instance | u32 | stub | `didChangeObjectOnSomeChain` (`:292-312`) bumps on **every** reported change of any prototype-ish object | (c) |
| `namesOfObjectPrototypes`, `namesChangedOnChains` | VM, bit per identifier number | bits only ever set (`|=` on u64) | stubs | C++ | (c): `fetch_or`; lost update = a missed "changed" bit: [value] |
| `namesOfTexts[128]` | VM (`:182-199`) | `first, last, length, uid` 32 bytes, pairs | stub (key that is not an atom → uid) | `rememberNameOfText` (`:68-88`): `pair[1] = pair[0]; pair[0] = {...}`, with `ref`/`deref` of uids | (c)/(a). Torn: text words of one name with the uid of another → wrong property: [value]; a uid `deref`ed to zero while a reader holds it: **[memory]** |
| `customAccessors` records | VM (`CustomAccessorTable`) 32 bytes: `function, slotBase, globalObject, offsetOfCachedValue` | stub calls `function` | C++ `CustomAccessorTable::add`: the table is reserved once at its limit and never moves; a record is a plain 32-byte struct assignment, indexes are recycled through `m_freeIndexes` after a collection | (c)/(d) **[memory]** (function pointer read from a recycled or half-written record) |
| `StringKeyCache` (`aot/runtime/AOTPropertyCachesStringKeys.h`), `integerKeys`, `caseConversions` | Instance | hash → cell words | stubs (`realm.integerKeys`, `realm.caseConversions`) | C++ | (a) or (c) |
| `NumericStrings` small-int / int caches, `SmallStrings` | VM (`realm.smallIntStrings`, `realm.intStrings`, `singleCharacterStrings`) | engine's own | stubs read, C++ fills | | as in the JIT engine |
| `clonedStructures[16]`, `objectCreate[64]`, `constructWords[64]` (this one is **written by the adapter stub itself**, `emitLocateRememberedConstructWord`, `aot/stubs/AOTStubsCallAdapters.cpp:331-337`), `prototypeTransitions[1024]`, `keyLists[1024]`, `enumerators[256]`, `keyOrderTransitions`, `keyOrderRuns[2048]`, `keyOrderSteps[2048]`, `functionNameCache[256]`, `literalStringCache`, `placesOfPrototypeProperties[8]`, `prototypesOfClassObjects[32]` | Instance | 16- to 64-byte key/value records, direct mapped | inline code and stubs | C++ slow paths (some stubs) | all are **multi-word check-key-then-use-value** caches. (a) is the cheap answer (they are small); shared they need 16-byte atomics or key-derived validation. Severity depends on the value: a cell header / StructureID / code address / cell pointer paired with the wrong key: **[memory]** |

---

## 4. The method table (shape + selector)

- In the image: `ImageSection::MethodTable`, "The code offsets that calls through the method table load. Each instance maps a copy-on-write clone of it, so it begins at a page and nothing else lies in its pages. **Nothing writes it.**" (`aot/image/AOTImageFormat.h:79-82`), and `ImageSection::MethodLookups` (one generic lookup entry per selector).
- In the Instance: `copyMethodTable` (`aot/runtime/AOTInstance.cpp:83-97`): `mapPrivateCloneOfImagePages` (`mmap(MAP_PRIVATE|MAP_FIXED, PROT_READ)` of the file pages, or `mach_vm_remap`; `aot/image/AOTImageLoading.cpp:459-480`), else `memcpy` + `protect(read-only)`. "The table of the image is never written. An instance changes its own copy where something changes what its objects find under the name of a method."
- Call (`generateCallTable`, `aot/codegen/AOTCallEmitterDirect.cpp:142-165`): `codeOffset = load32 [instance + signExtend(shapeWord)*4 + offsetOfMethodTable + selectorOffset*4]`; `entry = image.code + codeOffset * codeOffsetUnit`; call. One 32-bit load from the heap `AOTInstance_mutableFields`.
- Run-time writes: `MemberGuardWatchpoints::updateMethodTable` (`aot/object/AOTMemberGuards.cpp:1654-1680`). For every changed guard bit that has dependents (`ProgramTables::methodTableDependents`, sorted by bit): `entry = isSet ? compiled[dependent.entry] : lookups[dependent.selector]`; if different: `OSAllocator::protect(table, RW)` once, `table[i] = entry`, at the end `protect(table, R)`. "The copy is read-only but for the time of a change: no table of code addresses lies about that can be written."
- So: **read-only in the image, copy-on-write per Instance, written rarely, one aligned 32-bit word per entry, both values always valid code offsets with the same signature** (the compiled method, or the selector's generic lookup which finds whatever is there now).
- Class **(c)**: already safe as far as tearing goes. What is left: (i) two threads in `updateMethodTable` at once: the second `protect(R)` can come while the first still writes → SIGSEGV in C++: needs (d) around the update; `m_changedBits`, `m_recentChangedBits` are plain containers: (d). (ii) ordering against the guard bit and against the mutation that caused it (5.3).

---

## 5. Watchpoints, chain dependents, and every flag that compiled code tests

### 5.1 What a "watchpoint" is here

Only two C++ classes, both on top of JSC's own machinery: `PropertyValueWatchpoint` (`aot/runtime/AOTWatchpoints.h:15-27`) and `MemberGuardWatchpoint` (`aot/object/AOTMemberGuards.h:29-40`), both `AdaptiveInferredPropertyValueWatchpointBase`: they watch "object O has property P with value V" through the Structure's transition watchpoint set and the property's replacement watchpoint set. Firing runs a C++ callback **on the thread that makes the change, synchronously, inside the store**. No code is invalidated; the callback clears or sets words:

| User | Watches | Firing does |
| --- | --- | --- |
| `WatchedValues` (per Instance) | the holder of a free name / property filed as `Slot::Kind::Constant` in a site | `sites.clear(site)` (16-byte zero) |
| `GlobalBindings::Watch` | the global object's property behind a `GlobalBinding.constant` | `record.constant = 0` (`aot/runtime/AOTGlobalBindings.cpp:63-70`) |
| `BuiltinLocationWatch::Watch` | a member of a builtin object "that only a write replaces, where the compiler could not rule the write out" | `state = Changed` (one byte, `aot/runtime/AOTBuiltinLocationWatch.cpp:104-108`) |
| `MemberGuardWatchpoint` | a declared member of a class on its prototype / constructor object | `ClassTable::didChangeMember` → clears guard bits → updates method table |

JSC's `WatchpointSet`s themselves (lists hanging off Structures / rare data) are engine state with no locking for mutators: (d), for R1/R7.

A subtle rule: "A free-name site with a cached store compares no Structure and fires no watchpoint" so once a store through a name is cached, no constant of that name may be watched again (`m_namesWithCachedStore`, `m_identifiersStoredThrough`, `isStoredByNameFlag`; `AOTGlobalBindings.cpp:47-50`, `AOTPropertyCachesWatchedValues.cpp:29-31`). That is a check-then-act between two caches: thread A files a watched constant while thread B files an inline store for the same name → B's stores never clear A's constant: **[value], permanently stale**. Needs (d) over both decisions.

### 5.2 Chain dependents

`ChainDependents` (`aot/runtime/AOTChainDependents.h:129-174`), per Instance: `HashMap<JSObject*, { HashSet<ChainDependent>, optional NameIndex }>`. A `ChainDependent` = `{ uidOrNull, uint32 subject, ownerID }` (16 bytes). Owners (`ChainDependentOwner`, virtual `dropChainDependent(vm, subject, uid)`):

| Owner | subject | drop does |
| --- | --- | --- |
| `VMPropertyCaches` | StructureID bits | `m_front.removeChainDependentCells(id, column)`, `m_hashedTable.removeChainDependentEntries(id, uid)` (`AOTPropertyCaches.cpp:381-388`) |
| `SiteCaches` | site index | `clear(site)` if it has `dependsOnChain` |
| `GlobalBindings` | binding number | `constant = 0` if filed as absent |
| `MemberGuardWatchpoints` | class index, side, kind | re-evaluates / clears a guard |
| `StructureStateOwner` | StructureID bits | resets `DefinitelyNonThenableState` and the cached special properties of the Structure's rare data (`AOTChainDependents.cpp:43-76`) |

`add(chainObjects, dependent)` (`:95-113`) registers the dependent under *every* object of the chain that could change the answer. `didChange(changed, uidOrNull)` (`:115-159`): nameless → take and drop all; named → drop those with that uid or with none.

`didChangeChainObject(vm, changed, uid)` (`:238-255`), the one entry point, in order: (1) `propertyCaches->didChangeObjectOnSomeChain` (bump absence generation, set name bit, copy generations into every Instance); (2) if `changed` is a realm's `Object.prototype`: `didChangeObjectPrototype`; (3) for every Instance: `chainDependents->didChange`, `didChangeClassHolder` (→ `classes.didChangeMember` / `didChangeEveryMember` / `didChangePrototypeLink`).

Class: the maps are **(d)**. They are only touched on slow paths (fills and prototype mutations).

### 5.3 Guard bits (member guards)

- One bit per `(class, side, kind, name)` condition and one bit per *set* of conditions ("A site with several conditions tests one bit, which is set while every guard of the set is", `aot/object/AOTLayoutData.h:139`). Kinds include `Member`, `PrototypeLink`, `NotHidden`, `PassedByStores`. They live in the statics, 64 per word (1.3 item 3). Some classes use the inverse sense (`holdsWhileClear`: holds from the start, zero pages need no initialisation).
- **Inline test**: `GuardEmitter::holds` (`aot/codegen/AOTGuardEmitter.cpp:405-412`): `load8 [statics + byte]`, `test mask`.
- **What compiled code assumes**: "Compiled code takes a test that passed to hold until something runs that may clear the bit, and a test that failed to fail from then on." (`aot/object/AOTMemberGuards.cpp:391`). I.e. the proof `Proof::...` obtained from the test is kept across any stretch of code whose *own* effects cannot change a class. A second thread redefining `C.prototype.m` in that stretch is not among those effects.
- **Writes**: `setHolds` (`:325-332`): `words[i] |= mask` / `&= ~mask`: **non-atomic RMW on a 64-bit word shared by 64 guards**. Two threads changing different guards of one word lose an update: a guard that should be clear stays set: compiled code then calls the declared method directly / reads a field without the check although the member was replaced: [value] in most cases (wrong method runs), **[memory]** where the guard licenses a typed direct call whose callee's assumptions (receiver layout, argument representation) rest on it.
- "A guard is set at most once. One that has ended stays clear for the life of the program." (`AOTMemberGuards.h:61`): monotone after the first clear, which helps: clear is a one-way door.
- Order in `clear()` (`:405-...`): clear the bit → clear the sets containing it → `didChangeBit` → `updateMethodTable`. All of it happens *after* the JS-visible mutation (the hook is called "behind the change") or inside the watchpoint fire (before the store completes); both orders occur.
- Class **(c)** for the words (`fetch_and`/`fetch_or`), **(d)** for `MemberGuardWatchpoints`' containers. For soundness of "holds until something runs that may clear it" with several mutators there are only two options: clear guards **under a stop-the-world handshake** (every other mutator is at a poll, and polls are points where such proofs are dropped), or forbid the mutation. See H-5.

### 5.4 Global binding records

`GlobalBinding` (`aot/runtime/AOTRuntimeWords.h:43-54`), 24 bytes: `constant` (8) | `structureID` (4) `lexicalBindingEpoch` (4) | `location` (8).
Read (`generateGetGlobalBinding`, `aot/codegen/AOTAccessEmitter.cpp:369-411`): `c = load64 constant; if (c) use c`. Else (`globalBindingLocationOrElse`, `:343-361`): compare `globalObject->structureID` (and, where script bindings are seen, `| epoch << 32`, as one 64-bit compare) with the record's second word; `location = load64`; address = `location < 0 ? butterfly + location : location`; load; empty → slow path (that is the **TDZ check of a script-level `let`/`const`**: an uninitialised binding holds the empty value).
Write: the same address computation, then a plain store + barrier (`:413-440`).
Fill (`fileLocation`, `AOTGlobalBindings.cpp:127-136`): `structureID`, then `epoch`, then `location`: **the guard is written before the guarded word**. A concurrent reader can match the new structureID+epoch and load the *old or zero* `location` → store/load through address 0 + 0 or a stale absolute address: **[memory]**. Fix: write `location` first, release-store the 64-bit guard word; on refill, zero the guard first.
`forgetLocation`, `dropConstants`, `dropLocations`: plain stores.

### 5.5 Builtin location states, builtin cells, own-slot structure IDs

- `WatchedLocationState` byte per builtin: `NotArmed(0) → Unchanged(1) | Changed(2)`, `Unchanged → Changed` (`AOTBuiltinLocationWatch.h:40`). Inline: `load8; cmp Unchanged` (`aot/builtins/AOTBuiltinCallEmitter.cpp:541-545`), else call `arm`. `arm()` (`AOTBuiltinLocationWatch.cpp:64-81`) is check-then-install-watch-then-store. (c) for the byte, (d) for `m_watches`. As with guards, "Unchanged" is taken to hold for a stretch of code.
- Most builtins need no flag at all: they are **locked** at start (`makePropertiesImmutable`), a write throws. That is state that is (b) after start: the biggest simplification compared with a JIT engine (see "What helps").

### 5.6 `CacheRegistry` (collector interface)

Two registries (VM, Instance). Entries: `StrongRoots` (with `GreyedByMarking` / `GreyedByExecution`), `WeakByKey` (`removeDeadEntries`), `ClearedEachCollection` (memset), `Observer`. `visitRoots` runs under `m_lock`, from two marking constraints declared `ConstraintConcurrency::Concurrent` (`aot/runtime/AOTHeapHooks.cpp:74-99`; but `RememberedSlotSet::forEachRegionToVisit` asserts `heap.worldIsStopped()`, `heap/RememberedSlot.h:216-220`), and a few mutator-side writers take the same lock: `ImmortalCells::add`, `ProgramStrings::setConstant`, `ClosureTable::didCreateClosureFor/forgetClosure`, `PropertyCaches::didAddProperty`, `StructureTable::removeDeadEntries`. **That lock orders mutator against collector only for containers that may reallocate**; tables in fixed pages are read by the collector with no lock, found through cards (`RememberedSlotSet`).
`RememberedSlotSet::record` (`heap/RememberedSlot.h:47-59`): `if (m_cards[card] == written) return; m_cards[card] = written; m_writtenCards.append(card)`: a `Vector` append with no lock: with two mutators **[memory]**. Sets whose "cards are written without record" (statics, string cells) have only the byte store.
`finalize` runs in `Heap::reconcileWeakReferencesAtGCEnd` (world stopped).

### 5.7 The complete list of words that compiled code or stubs *test* and that run-time events flip

| # | Word | Size | Where | Tested by | Flipped by | Taken to hold |
| --- | --- | --- | --- | --- | --- | --- |
| F1 | guard bits and guard-set bits | bit in a byte | statics | inline | class member / prototype link changes, class (re)definition | across code that cannot change classes |
| F2 | method table entries | u32 | Instance | inline (as a call target) | F1 | the call only |
| F3 | `WatchedLocationState` | u8 | Instance +64 KB | inline | watchpoint on a builtin member | stretch |
| F4 | `GlobalBinding.constant` | u64 (0 = none) | Instance +80 KB | inline, stub | watchpoint, chain change, new lexical binding | the read only |
| F5 | `GlobalBinding.structureID/epoch` vs `globalObject->structureID`, `m_globalLexicalBindingEpoch` | u64 | record / global object | inline | refill, GC, epoch wrap; the global object's transitions | the access only |
| F6 | site cache header (`structureID`, `kind`) | u64 (+u64) | Instance | stubs, inline | fill, chain change, watchpoint, GC | the access only |
| F7 | front row word, front cell, hashed entry | u32, u64, 3×u64 | process / VM | stubs | fill, evict, move, chain change, GC | the access only |
| F8 | `absenceFilterGeneration`, `...ForStores`, filters, name bit sets | u32, 64 B, bits | Instance/VM | stubs | any prototype-ish change | the access only |
| F9 | `stackMirror.m_trapAwareSoftStackLimit` | ptr | Instance | every checked prologue; ARM64 loops; some stubs | `requestStop`/`cancelStop`/`setStopsAtFunctionEntries` from any thread | |
| F10 | loop poll page protection | page | Instance −1 MB | x86-64 Linux loops (a load that faults) | `setLoopPollsEnabled` | |
| F11 | `StackManager::m_stopsAtFunctionEntries` (= "needs OutOfMemoryError") | u8 | VM | poll stubs | heap cage full | |
| F12 | `barrierThreshold`, `mutatorShouldBeFenced` | u32, u8 | Instance | inline barriers, `fenceAfterAllocation` | collector phase changes (`didChangeBarrierThreshold`, under `instancesLock`) | |
| F13 | `awaitMustSuspend` | u8 | Instance | await stubs | debugger attach, `setMicrotaskRunnability`, `setOnEachMicrotaskTick`, `setCrossTaskToken`, queue swap | |
| F14 | `coroutines.frameThatMayContinue` == fp; `microtaskQueue->start == end` | ptr | Instance / queue | inline await fast path (`aot/runtime/AOTAsyncAwaitConditions.h:61-64`) | job entry, F13 | "the queue is empty, so running the continuation now is unobservable": a **whole-queue** assumption |
| F15 | `VM::m_asyncContextTrackingEnabled` | u8 | VM | inline/stub (`:72`) | AsyncLocalStorage use | |
| F16 | module status byte | u8 | table | inline | inline, C++ | section 6 |
| F17 | variable slot == absent encoding (TDZ), == 0 (function not made) | 1-8 B | statics | inline | inline | |
| F18 | lazily filled words: zero test (string cell, key cell, bigint, builtin cell/value, closure cell, class prototype, template object, RegExp cell, shared storage, module value, inline allocator, allocation site record) | u32/u64 | Instance / statics / arena | inline, stubs | first use | for ever once non-zero (except: closure cells `forgetClosure`, allocation records `forgetStructuresOf`, host values `forgetValuesNamed`, everything "cleared each collection") |
| F19 | array headers in the prefix (`newArrayWith*Header`), zero/changed when the realm "has a bad time" (`fillArrayHeaders` returns early; `branchIfHavingABadTime` in stubs) ; `frameArrayHeader` | u64 | prefix | inline, stubs | `haveABadTime`, `captureCellHeaders` | |
| F20 | `collectionStructureIDs[]`, `numberStructureID`, `booleanStructureID`, `ownSlotStructureIDs[]`, `structuresWithPrototypeProperty[]` | u32 | Instance | inline, stubs | lazy class creation | monotone 0 → id |
| F21 | `predictionFailureState` (testing switch only), `expectedCall`, `didEnterCompiledCode` (validation only) | | Instance | inline | inline | not shipping |
| F22 | `json.*`, `stringBuilder.freeStorage`, free environment cells, `lastArray`, `scratch.*`, `exceptions.*`, coroutine hand-over words | | Instance/statics | inline | inline | thread-private by intent |
| F23 | flags *in objects and Structures* (order word flags, pristine bit in the cell header, `CompiledStructure::m_flags`, `TypeInfo` bits, `Structure::seenProperties`) | | heap | inline | C++ | R1 |

---

## 6. Variables, global bindings, TDZ, module evaluation

### 6.1 Module variables (and every top-level `let`/`const`/`var`/function/class of a module)

- Storage: a slot in the statics at a compile-time offset, in the representation that `place/` chose (1.3 item 1). Read: `StaticsEmitter::loadVariable` (`aot/codegen/AOTStaticsEmitter.cpp:52-66`), write: `storeVariable` (`:68-86`): a plain load/store of 1-8 bytes at `[instance + offsetOfStatics + k]`; a store of something that can hold a cell is followed by `markCardOfStatics` (`aot/codegen/AOTBarrierEmitter.cpp:179-185`: `store8 1 → cards[k >> 8]`). No other barrier: the statics are rescanned as roots.
- An export/import binding is the *same* slot: an importer reads the exporter's variable directly (IR `GetModuleVariable(variable)`, `aot/ir/schema/operations.def:39-41`). No module environment object exists unless somebody needs a namespace object.
- Each variable is its own abstract heap (`MemoryLocation::moduleVariable(id)`, `AOTStaticsEmitter.cpp:40-45`): IR passes and B3 forward stores to loads, hoist loads out of loops and remove dead stores as long as no operation *in this thread's code* `writes(ModuleVariables[variable])`.
- **Class**: shared by definition (it is the JS-visible shared state): **(c)** in the sense of "plain racy memory". Slots are naturally aligned and ≤ 8 bytes, so no tearing. What a race can do depends on the *proof* attached to the variable:
  - a type proven by **flow** ("flow sees every writer of the position, and each writes the type") stays true with any number of threads: every writer still writes that type. Raw representations (`Int32`, `Double`, `Cell32`) stay sound.
  - **initialisation proofs do not**: see 6.3.

### 6.2 Global bindings

`globalThis` properties and script-level `let/const` that compiled code names freely go through `GlobalBinding` records (5.4) or free-name site caches (`getFreeName`/`setFreeName`, `aot/codegen/AOTAccessEmitter.cpp:288-333`). The values themselves live in the global object (butterfly / inline storage / `JSSegmentedVariableObject` variables / `JSGlobalLexicalEnvironment`), i.e. in ordinary JSC objects. The record caches an **absolute address** of a variable or an offset from the global object's butterfly. The global object's butterfly can be reallocated by another thread that adds a global: the reader loads `structureID` (match), then `butterfly`: the usual butterfly/structure race of ordinary objects (R1/R7): [memory] without their protocol.

### 6.3 TDZ and "not made yet" encodings

- `initialWordOf` (`aot/place/AOTStaticsLayout.cpp:102-117`): a variable whose `flow.initialization == Unknown` starts as `undefinedBits` (a `var`) or `absentBits` (= TDZ marker of the representation: the empty JSValue, or a reserved integer / NaN pattern / narrow-reference marker). A read that is not proven initialised tests for the absent pattern and throws the `ReferenceError` on the slow path.
- **If flow has *proven* the initialisation state (`variable.flow.initialization != Unknown`) the variable starts as 0 and reads have no test** ("return isStaticField ? absentBits : 0", with the comment "The hooks of a class object ask the variable whether the static field is defined, at times no proof about compiled reads covers"; that compiled reads then carry no test is my reading of this function, the pass that drops the test was not opened). The proof is about program order of one thread: "every read is dominated by the write, because the module body runs before anything that can call this function". A second thread that runs a function of a module *while another thread is still evaluating that module's body* reads 0 from a slot whose representation may be `Cell`/`Cell32`/`JSValue`-without-empty: dereferencing 0 (or cage base + 0): **[memory]** (in practice a fault in the reserved low pages / cage start, but not one the fault table knows).
- `VariableEncoding::FunctionAsZero`: a hoisted function declaration's variable is 0 until somebody needs the closure; the reader makes the singleton closure (7.6). `UndefinedAsZero`: `undefined` is stored as 0.
- Design consequence: **no second thread may run program code before the modules it can reach are evaluated**, or the start of a thread must be treated by flow as a new entry point at which no initialisation proof holds. Since a thread's entry function is created by running code, "the creating thread has evaluated at least the module that contains the `new Thread(...)` expression up to that point" is all that is known.

### 6.4 The module state machine

Two representations (`aot/runtime/AOTModules.h:54-62`, `aot/runtime/AOTModuleWords.h:15-29`):

- `StoredModuleStatus`, **one byte per module** in a zeroed table (`instance->modules.status`): `Unlinked(0) < Linked(1) < InRecord(2) < Evaluated(3) < Evaluating(4 and every higher value)`. "Compiled code tells by one comparison whether a module is still to be evaluated."
- For modules that need the full ECMAScript algorithm (cycles with async, top-level await, uncompiled dependencies) a `ModuleRecord` cell (a `CyclicModuleRecord`, `aot/runtime/AOTModuleRecord.h`) is made on demand (`ensureRecord`), the byte says `InRecord`, and the status (`Linking, Linked, Evaluating, EvaluatingAsync, Evaluated` + error) lives in the record. **Top-level await**: `Modules::evaluate` ends in `ensureRecord(root)->evaluate(realm, order, importPromise)` (`aot/runtime/AOTModuleEvaluation.cpp:674-722`), JSC's own async module evaluation with promises and `asyncEvaluationOrder`.
- `evaluatingMark` (`ModuleWords`): "The status that a module which evaluates itself stores when it begins. Each evaluation that the run time starts has a value of its own, by which it finds the modules that it left unfinished." `evaluateByCallingBody` (`:520-536`): `outer = mark; mark = min(outer+1, 255); callBody; mark = outer; on exception failModulesLeftEvaluating(mark)` which **scans the whole status table for bytes equal to the mark** (`:572-582`). A dynamically scoped variable kept in the Instance: **(a)** in spirit, and the scan assumes nobody else uses the same mark.

**Modules that "evaluate themselves"** (the common case: `ModuleData::evaluatesItself()`): an importer's compiled body contains, per dependency, `BeginModuleEvaluation` / call of the body / `FinishModuleEvaluation`:

```
generateBeginModuleEvaluation (aot/codegen/AOTModuleEmitter.cpp:151-168)
  status = load8 statuses[m]
  if (status <= InRecord) {
      if (status != InRecord) { store8 statuses[m] = load32(instance.modules.evaluatingMark); return true; }   // I evaluate it
      else return operation BeginModuleEvaluation(m);
  }
  return false;                                                                                               // done, or being evaluated (a cycle)
generateFinishModuleEvaluation (:170-173):  store8 statuses[m] = Evaluated
```

- **load; compare; store** with no atomicity: two threads both get `true` and both run the body: breaks "A module body is a function. It runs at most once" (PR table; `AOTModuleEvaluation.cpp:552` "No body runs again"). Consequences beyond the JS-visible double side effects: everything that the compiler derived from "runs at most once": sites without caches, `Made::Once` string headers ("A site makes at most one header", `aot/runtime/AOTProgramStrings.cpp:63`), one class object per class (`ProvenOnce`), singleton allocations placed in statics, `placeModuleBuiltinCells`. **[memory]**.
- `status >= Evaluated` includes `Evaluating`: in one thread that can only be a cycle, and the spec says "go on, bindings may be in TDZ". For a second thread it means "somebody else is half way": it must **wait**, not go on (6.3).
- C++ makes the same assumption: `continueImport`: "`status == Evaluating`: The body of the module is on the stack below this job" (`aot/runtime/AOTModuleImports.cpp:105-111`); `Modules::evaluate`: `ASSERT(status(root) != ModuleStatus::Evaluating)`; `m_innermostEvaluation` (a stack of `EvaluationInProgress` frames linked through C++ stack objects, `AOTModules.h:325`); `RequestToHostScope` (`SetForScope<bool> m_isAskingHost`); `LoadedModule::isBeingRequired` (`aot/runtime/AOTModuleCommonJS.cpp:36-47`).

**CommonJS** (`aot/runtime/AOTModuleCommonJS.cpp`): `require(target)` → `execute`/`evaluateCommonJS` (`:94-141`): `switch (status)`: `Evaluating`/`Evaluated` → return (a cycle sees the partial `module.exports`), `Linked` → `setStatus(Evaluating)`, call body, `setStatus(Evaluated)`. `module.exports` "is a variable of the program" (`aot/place/AOTStaticsLayout.cpp:274`): `commonJSExports`/`setCommonJSExports` read/write a statics slot. The module object, `require` function, `import.meta` are lazily made cells in statics (7.9). `recordRequire` fills `m_recordedRequires`, `LoadedModule::requiredModules` (for `module.children`).

**`import()`**: static target: `RuntimeOperation::ImportModule` → `Modules::import` → `startImport` (`aot/runtime/AOTModuleImports.cpp:86-98`): make a promise, **queue a microtask job**; `continueImport` (`:100-141`) links (`ModuleLinkWalk`), loads runtime modules, `evaluate`, then resolves with the namespace. Dynamic specifier: `ImportDynamic` → host resolution; refused in closed programs unless the image has `LoadsModulesByName`.

**Linking**: `link(root)`; the entry point's link is precomputed ("The link of the entry point that the image has a plan for stores no status: `m_entryPointIsLinked` says Linked for every module with `isLinkedWithEntryPoint()`", `AOTModules.h:339-340`).

Containers of `Modules` (`:316-341`): `m_records`, `m_loaded` (HashMaps), `m_storedReferences`, `m_linkWalkOfQueuedImport`, `m_recordedRequires`, `m_modulesWithMarkOfRequire`, several `bool`s.

**Class: (d).** One lock (or "module evaluation only on one thread at a time, others block") for the whole `Modules` object; the inline fast path `status == Evaluated` (acquire load of one byte) stays lock-free: `Evaluated` is final. The inline claim (`Linked → Evaluating`) must become a CAS, or leave inline code entirely (it runs once per module per program).

---

## 7. Lazily created, then cached in a shared slot

General shape everywhere: `w = load(slot); if (w) use w; else { v = create(); slot = v; }` with no atomicity. "Identity" says whether JS can tell two creations apart.

| # | What | Slot | Check | Create | Publish | Kept alive by | Identity? | Class / fix |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 7.1 | **String constant cell** (`JSString` of a literal with a number) | `uint32` narrow ref at `instance + 384 KB + 4n` (or `wideStringCells[n]`) | inline `load32; test` in Fast code (`permanentCell`, `aot/codegen/AOTConstantEmitter.cpp:263-276`, `:278-304`), else stub `returnStringConstant` (`aot/stubs/AOTStubsConstants.cpp:276-...`) | **x86-64 stub allocates the `JSString` itself** from `constants.stringAllocator`, header from the prefix, `StringImpl*` = header in the image (`createCellOfHeaderAndReturn`, `:81-125`); else `operationAOTStringConstant` → `ProgramStrings::constant` (`aot/runtime/AOTProgramStrings.cpp:169-176`) | stub: `storePair header,impl → cell`; fence (ARM only, conditional); `store32 slot`; `store8 card`. C++: `setConstant` under `caches->lock()` (`:178-186`) | the slot is a root (cards) | no (strings have no identity; but see Not sure N-3) | (c): last writer wins is acceptable; needs release ordering between cell init and slot store on ARM64 (`fenceAfterAllocation` only fences when `mutatorShouldBeFenced`). Allocator is (a) |
| 7.2 | **String header made at run time** (text without a `StringImpl` in the image) | bump pointer `constants.nextStringHeader/endOfStringHeaders` | | stub: `load next; add 24; cmp end; store next` (`AOTStubsConstants.cpp:263-270`); C++ `tryCreateHeader` (`AOTProgramStrings.cpp:140-150`) | plain stores of the 24-byte header | never freed; only the *first Instance of the process* gets the arena (`static std::atomic<bool> hasFirstInstance`, `:61-62`); sized by "A site makes at most one header" | | **non-atomic bump: two threads get the same header: [memory]** (a `StringImpl` whose length and characters come from different strings). (a) per-thread arenas or `fetch_add`; the sizing argument needs "at most once" |
| 7.3 | **Key cell** (JSString/Symbol for a property key), **literal string cell** | `keyCells[n]`, `literalStrings[n]` (u32 or ptr, arena) | inline / stub `returnCellFromTable` (`:50-71`) | `ProgramStrings::keyCell` (`AOTProgramStrings.cpp:342-358`) | `immortalCells->add(cell)` (locked) then plain store (`setImmortalCellAt`, `:242-250`) | `ImmortalCells` | **Symbol cells: yes** (`Symbol::create(vm, impl)`: two `Symbol` cells for one `SymbolImpl`; JSC expects one via the symbol's weak map? not checked) | (c) CAS, loser drops its cell |
| 7.4 | **BigInt constant** | `bigIntCells[n]` | inline | `bigIntConstant` (`:360-380`) | plain | immortal | no (`===` on BigInt is by value) | (c) |
| 7.5 | **Identifier / atom** by number | text names: **none: the `StringImpl` is in the image** (`programIdentifiers + n`, 24 bytes each), read-only, static, already an atom. Symbols, private names, the empty text: `ProgramSymbols` open-addressing table per VM | stub `loadIdentifier` (`aot/stubs/AOTStubsPropertyProbes.cpp:1079-1121`): `length != 0` → the atom; else probe `entries[number & mask]...` **without the lock** | `uidSlowCase` (`aot/runtime/AOTProgramIdentifiers.cpp:98-115`): `find`, `create(number)` (e.g. `PrivateSymbolImpl::create`, `SymbolImpl::create`) **outside the lock**, then lock, `addSymbol` | `insert`: `*entry = {number, uid}` (16-byte struct store: `number` may become visible before `uid`); growth: new table, `fastFree(old)` (`:117-130`) | leaked ref | **yes**: a private name `#x` or a `Symbol()` made at module level is *the* identity | "Only the thread of the VM changes the table, with the lock held. It and its stubs read the table without the lock, every other thread with it." (`AOTProgramIdentifiers.h:97`). Two creators → two different private names for one `#x`: brand checks fail: [value]; reader during growth: use after free: **[memory]**. (d) for create+insert (double-checked), never free old tables (or epoch), publish `uid` before `number` |
| 7.5b | lazy string headers of a **position-independent image** | process-wide arena + `std::atomic<uint32_t> offsets[n]` | `tryGetLazyHeader`: acquire load (`aot/image/AOTImageText.h:335-339`); stubs: plain `load32` | `createLazyHeader` (`aot/image/AOTImageText.cpp:101-119`): lock (`LazyStringHeaderLocker`), re-check, bump, fill, **release store** | | process | | **already (c)/(d), correct for several threads** (it has to be: Workers share it). A model for the others |
| 7.6 | **Singleton closure** of a function without captures (top-level function, method, class constructor) | `closureCells[k]` (u32 "ref − 1" or ptr) | stub (`aot/stubs/AOTStubsClosures.cpp:254-266`), inline in `AOTClosureTestEmitter.cpp:314`, `AOTStubsPropertyRead.cpp:259` | `operationAOTSingletonClosure` → `ClosureTable::closure` (`aot/object/AOTClosureTable.cpp:338-...`) | `didCreateClosureFor` (`:299-312`): under `caches->lock()`, `RememberedSlot::set` | the table is a root | **yes** (`f === f`, properties on `f`, `f.prototype`) | (c) CAS / (d). `forgetClosure` (`:327-336`) resets to 0 when a class is evaluated again |
| 7.7 | **Class objects, class prototypes, class Structures** | `classes.classPrototypes[rank]`, `StructureTable::m_structures[shape]` (u32 StructureID per shape), variant maps, `AllocationSiteRecord`s | inline (`classPrototypes`), stubs | class definition code + `aot/object/AOTClassCreation.cpp`, `StructureTable::structure/createUsual`, `fillAllocationSite` (`aot/object/AOTAllocationSites.cpp:121-160`) | plain stores + `record()`. `ensureRecord` (`:87-103`): bump `m_numberOfAllocationSiteRecords`, init record, **then** store `distancesToAllocationSites[n]`. `fillAllocationSite`: `record = AllocationSiteRecord{header, words, allocator}`: 32-byte struct store; compiled code first tests the *allocator for room*, then copies header+words | roots by cards | **yes** for class objects/prototypes; Structures: two Structures for one shape break "one shape ⇒ one usual StructureID" which shape/structure tests and the front table rely on | (d) for creation. Reader side: until filled, `allocator = &m_allocatorWithoutRoom` so the inline path fails into the operation ("The allocator is never null", `aot/object/AOTAllocationSiteRecord.h:16`): a good publication point **if `allocator` is stored last with release**; today the order is the compiler's. `forgetStructuresOf` (`:184-202`) rewrites records to empty while others may be mid-allocation: [memory] |
| 7.8 | **Template object** | statics word per site | inline `load64; test` | `operationAOTNewTemplateObject` (`aot/runtime/AOTConstantOperations.cpp`) | **inline**: `mutatorFence; store64; mark card` (`aot/codegen/AOTAllocationEmitterObjects.cpp:39-62`) | statics root | **yes** (spec: one per site per realm; used as a WeakMap key by libraries) | (c) inline `cmpxchg`, loser takes the winner's |
| 7.9 | **Module value / namespace object, module object, `require`, `import.meta`** | statics words; `LoadedModule::Reference<>` fields | inline `load64; test` (`generateModuleNamespace`, `generateRequireModule`, `aot/codegen/AOTModuleEmitter.cpp:64-110`) | operations in `AOTModules.cpp`, `AOTModuleNamespace.cpp` | plain + `recordAll()` | roots | yes | (d) with section 6 |
| 7.10 | **RegExp**: (i) `RegExp*` cell per (record, flags): `RegExpLiterals::m_cells` HashSet + Vector, per VM; (ii) "shared cell" per literal site in statics: a `RegExpObject` | (i) C++ only; (ii) inline `load64` passed to the stub `newRegExpLiteral`; zero → operation | (i) `regExpFor` (`aot/runtime/AOTRegExpLiterals.cpp:76-88`): find, `createFromImage`, add to both containers, **no lock** although the collector visits `m_cellsInOrderOfCreation` from a concurrent constraint (it holds the registry's lock; the mutator does not); (ii) `operationAOTNewRegExpLiteral` / `operationAOTSharedRegExpLiteral` (`aot/runtime/AOTRegExpOperations.cpp:335-365`) | `vm.mutatorFence(); statics->setCell(offset, object)` | roots | (i) no. (ii) see 7.11 | (i) (d). (ii) (c) |
| 7.11 | **`SharedRegExpLiteral`**: pass `ShareRegExpLiterals` (`aot/passes/AOTShareRegExpLiterals.cpp:208-235`) turns a `NewRegExp` whose object does not escape and is only matched with into **one `RegExpObject` per site for ever** | statics word | inline (`generateSharedRegExpLiteral`, `aot/builtins/AOTBuiltinCallEmitter.cpp:516-530`) | as above | | | The object is invisible, but **its `lastIndex` is live state** for `/g` and `/y` (`LastIndexUse::AdvancesIfGlobalOrSticky`; stub `loadStart`/`storeEndOfMatchInLastIndex`/`resetLastIndex`, `aot/stubs/AOTStubsRegExpLiterals.cpp:166-176`, `AOTStubsRegExpFrames.h`) | valid only because two evaluations of the site never overlap (and recursion is excluded by the pass's use rules). Two threads in `while (m = /x/g.exec(s))` at one site share `lastIndex`: **[value]** (wrong matches, endless or short loops). (a): one shared object per thread, or do not share `/g` `/y` literals |
| 7.12 | **Key order tables** (order in which fields were added → order word): `keyOrderTransitions` (open addressing, grows), `keyOrderRuns[2048]`, `keyOrderSteps[2048]`, `keyLists[1024]`, `enumerators[256]` | Instance / pages | stubs (`aot/stubs/AOTStubsFieldCreation.cpp:163,210,303`), inline | `KeyOrderTable` (`aot/object/AOTKeyOrderTable.cpp`) | `publishTransition` (`:739-764`) **is already a publication protocol**: under `caches->lock()`: `after`, `key`, `storeStoreFence()`, then the non-zero `orderAndShapeWordsBefore`. But `growTransitions` (`:766-786`) stores the new table pointer, then the mask (two words read separately by stubs), and **releases the old pages**: "Compiled code loads the table and the mask from the instance for every probe and holds neither across a call, so nobody reads the table that is given back." | | numbering of key orders must be unique per Instance | (d) writers; readers (c) if old tables are never unmapped and (pointer, mask) are published as one word or the mask is stored in the table. Old pointer + new mask = out-of-bounds probe: **[memory]** |
| 7.13 | **Free environment cell** (`ReuseEnvironments`): a function whose environment provably dies at return gives the cell back | statics word per layout, zeroed each collection | inline `load64; test` | | inline **take**: `store64 0` (`takeReleasedCellOrAllocate`, `aot/codegen/AOTAllocationEmitter.cpp:238-256`); **give back**: `ReleaseEnvironment`: `if (!environment.isEscapedMark) store64 cell → word` (`aot/codegen/AOTEnvironmentEmitter.cpp:74-94`) | not a root | | a one-element free list with non-atomic pop: two threads take the same cell → two live activations share one environment: **[memory]/[value]**. **(a)** |
| 7.14 | **`PropertyCaches`** of an Instance (`LazyPropertyCaches::operator*`, `aot/runtime/AOTState.h:74-94`; `create()`, `aot/runtime/AOTPropertyCaches.cpp:439-443`), `ArrivalCounts`, `AllocationCounts`, `GlobalBindings`, `JSONRuntime`, `functionTables`, `m_calleeOfModuleBodies` | `unique_ptr` | `if (m_caches)` | `makeUnique` | plain | | | (d) or create eagerly before the second thread starts |
| 7.15 | **Inline allocation data** `{allocator, header}` (Promise, Map, BoundFunction, BigInt, final object, RegExpObject, null-prototype object, matches array, Set; 16 typed array kinds) | Instance | stub: `allocator != 0` | `prepareInlineAllocation` (`aot/runtime/AOTOperationScope.h:89-97`): `header =` then `allocator =` | plain (right order, no fence) | | | allocator (a); header (c). `captureCellHeaders` **zeroes all of them again** (`aot/runtime/AOTInstance.cpp:375-385`) at start-up only |
| 7.16 | **Builtin cells / host values** | `instance + 128 KB + 8n`, `+ 256 KB + 8n` | inline `load64; test` (`builtinCellWord`, `AOTConstantEmitter.cpp:254-260`) | `BuiltinCells::cell` (`aot/runtime/AOTBuiltinCells.cpp:243-293`): may **load a builtin module, run host code**, re-checks the slot afterwards ("filledDuringLookup"); `fillValue` (`:323-352`) likewise | `immortalCells->add`, then plain store (`fillAndKeepAlive`, `:533-539`) | immortal | the value is an existing object of the realm, so two fills store the same pointer | (c) idempotent; the side containers (`m_isUnavailable`, `m_awaitedMembers`, ...) (d). `forgetValuesNamed` zeroes values when the host replaces a property |
| 7.17 | **Property runs** (`literals.propertyRuns[n]`), **compact table growth plans**, `readAndWrittenCountsStructure`, `linkTimeConstants[]`, `structuresWithPrototypeProperty[4]`, `ownSlotStructureIDs[16]`, `collectionStructureIDs`, `nameOfFunctionsWithoutEntry` | Instance / arena | zero test | C++ | plain single word | various | no | (c) idempotent or CAS |
| 7.18 | **Boxed doubles**: none found. Doubles are raw in slots or NaN-boxed; there is no heap number | | | | | | | |
| 7.19 | **Copy-on-write array literal storage** ("shared storage") | statics root word per literal | inline | operation builds the `JSImmutableButterfly`-like storage | plain + card | root | no (copy on write) | (c) idempotent-ish (two storages, last wins) |
| 7.20 | **Structure-level caches written by the run time on engine objects**: `DefinitelyNonThenableState`, cached special properties in `StructureRareData`, `Structure::seenProperties`, pristine bits | heap | | | | | | R1/R7 |

---

## 8. Statistics, counters, profiles

| What | Where | Written by | In shipping builds? | Class |
| --- | --- | --- | --- | --- |
| `CounterWords::values[numberOfRuntimeCounters]` | Instance (`aot/runtime/AOTRuntimeWords.h:56-65`) | C++ `count(Instance&, id)` = `++values[i]` (`aot/runtime/AOTRuntimeCounters.cpp:64-67`) on slow paths: **always compiled in**; stubs: `bumpRuntimeCounter` (`aot/stubs/AOTStubHelpersValues.cpp:276-288`: `add qword [instance+k], 1`, not locked), only **two** call sites: `RegExpMatchesInterpreted` (`AOTStubsRegExp.cpp:298`), `CalledClosuresNotCreated` (`AOTStubsPropertyMethodCalls.cpp:383`) | **yes** (cheap, unconditional) | [value]: lost counts. (a) with a sum at exit (the code already sums Instances into `RuntimeCounters` at destruction, `Instance::destroy` → `counters->add`) or leave racy |
| `RuntimeCounters` (`values`, `limitHits`) | VM | C++ `count(VM&, id)`, `countLimitHit` | yes | same |
| **Array allocation profile** (`ArrayAllocationProfile`, 8 bytes: `word` = count(16) shape(8) capacity(8); `reviews`; `largestLengths[3]`; `aot/runtime/AOTArrayAllocationProfile.h:25-52`) + `lastArray` word | statics | **stub on every allocation at the site**: `load32 word; add 1<<16; store32` (`generateNewEmptyArrayAtSite`, `aot/stubs/AOTStubsArrayHoles.cpp:401-436`); every 64th: `operationAOTNewArrayAtReviewedSite` → `reviewArrayAllocationProfile(profile, lastArray)` (`aot/runtime/AOTArrayAllocationProfile.cpp:39-97`), which **dereferences `lastArray`** (`indexingType`, `butterfly()->publicLength()`) | **yes: this is the one real run-time profile of the design** (no profile of the program at build time) | capacity and shape come from one 32-bit load and any value ever stored is valid: [value]/performance. A lost RMW can overwrite a `set()` by the reviewer with the old capacity: harmless. `lastArray` may be an array another thread is mutating: relies on the array protocol. "Not a root: every collection zeroes this word" |
| `ArrayWords` (`elementsLookedAtInVain`, `placeThatConverts`, `shortYoungArraysConverted[64]`) | Instance | stubs / inline | yes | heuristics [value] |
| Site cache miss counters, `missesOfEmptySites`, `storesAtEmptySites`, `storesToGuardedFields`, `absentReadsAnswered`, `AbsenceFilter::readsAnswered`, `missesOfPairs[32768]`, `m_missCounters` | Instance / VM | stubs, inline, C++ | yes | sampling only [value]. False sharing: `missesOfEmptySites` etc. sit in the hot first KB of the Instance next to read-only words that every access loads: a **performance** hazard with several cores |
| `branchCounts`, `allocationCounts` (`ExactAllocationCounts`) | pages, pointers in `CounterWords` | inline code and stubs (`countCellOfStub`, `aot/stubs/AOTStubHelpersAllocation.cpp:14-24`) | **no**: only if the image was built with `RecordedOptions::countsBranches` (`aot/support/AOTCompileOptions.h:88`; `aot/runtime/AOTInstance.cpp:326-329`; `stubs/AOTStubPolicy.cpp:35`) | n/a |
| `ArrivalCounts` | `State` | operation called from by-name accesses | **no**: "in an executable built with --aot-count-arrivals" (`aot/runtime/AOTArrivalCounts.h:72`; `countsArrivals`) | n/a (HashMaps: (d) if ever used with threads) |
| `AllocationCounts`, `m_countedFilings`, `m_namedCounts`, `StructureTable::m_made` | `State` | C++ slow paths | code is in the binary; active only "in a run with --aotCountsPath" (`Options::aotCountsPath()`, `aot/runtime/AOTAllocationCounts.h:30`) | (d) |
| Validation words (`expectedCall`, `didEnterCompiledCode`, `m_validatedSites`, ...) | Instance / State | inline | **no**: `--aot-validate` (`RecordedOptions::validates`) or `Options::validateAOTRuntime()` | would need (a) to keep validating builds usable with threads |
| `predictionFailureState` | Instance | inline PRNG | no (testing switch `FailPredictions`) | |
| `JSGlobalObject::weakRandom` (`Math.random`) | realm | inline xorshift RMW (`JSGlobalObject::weakRandomOffset()`, 2 uses in `aot/`) | yes | [value]; two words updated non-atomically: could reach the all-zero state? (xorshift128+ never leaves zero): see Not sure N-6. (a) is natural |
| LLInt profiles | | "They ... write no profiles, since no JIT will read them" (PR) | none | |

---

## 9. The image

### 9.1 Sections (`aot/image/AOTImageFormat.h:53-85`)

`Functions, FunctionStarts, CodeGranules, InnerEntries, Frames, FrameMaps, ClosureWords, Positions, FaultSites, SideTables, SharedCodeCopies, Strings, LongStrings, Text, Identifiers, StringConstants, StringHeaders, RegExps, RegExpHashes, RegExpDetails, WebAssemblyDirectory, ProgramTables, StubAddresses, StubReturnSites, SlotHandlers, MethodLookups, MethodTable` = 27, then the code part (`codeOffset`, `codeSize`: functions, shared stubs, RegExp code, WebAssembly code, constants, FFI code; `ImageCodeRanges`, `:104-115`).

**All 27 are read-only once `useImage` returns.** `useImage` (`aot/image/AOTImageLoading.cpp:503-540`): check stamp/identity → page alignment → reserve the low pages for null traps → `relocateLoadedImage` → `removeWriteAccessFromLoadedSection` (`mprotect(PROT_READ)` if the ELF segment was writable, `:400-410`) → `createCodeMapping` (code is mapped `PROT_READ|PROT_EXEC` from the file, `mapCode`, `:114-133`; or the loaded executable segment is used/moved) → `Image::publish` (`std::atomic<const Image*> s_current`, acquire/release; `aot/image/AOTImage.h:85,151`) → install the fault handler.

### 9.2 The two kinds of places that hold addresses

"An image holds addresses in two kinds of places, and nowhere else: `ImageHeader::address`, which is the address of the image itself, and `ImageStringHeader::characters`. A position-independent image holds none." (`aot/image/AOTImageText.h:399-407`, `ImageAddressPlaces`.)
Who patches them: (1) normally **nobody at run time**: Bun's executable writer calls `relocateImage()` / lists the places for the OS loader (`static_assert(offsetof(ImageHeader, address) == 56, "Bun's writers of executables list this member for the loader")`, `AOTImageFormat.h`), so the dynamic loader's relocations do it; (2) otherwise `relocateLoadedImage` (`aot/image/AOTImageText.cpp:345-368`): if `header.address != actual`: make the header page and the `StringHeaders` pages writable (`mprotect`, or replace by anonymous copies on Mach-O), add the distance to every `characters`, write `address`, protect again. Once, on the main thread, before any VM.
Code is position independent: it reaches everything through `instanceGPR` or PC-relative.

### 9.3 Written after load

| What | When | Notes |
| --- | --- | --- |
| nothing in the 27 sections | | the `StringImpl` headers in `StringHeaders`/`Identifiers` are `StaticStringImpl`s: `refCountOfStaticString()`, hash precomputed, atom flag preset, so `ref/deref`, `hash()`, atomisation never write them. `AtomStringTable::setReadOnlyAtomLookup(findAtom)` (`aot/runtime/AOTProgramIdentifiers.cpp:77`): "Atoms that live in the image are found before one is added to the table" |
| **FFI callback context page** inside the code part | `FFI::Stubs::prepareCallbackContext()` (`ffi/FFIStubs.cpp:162-176`): replace the page by anonymous memory (or `PAGE_WRITECOPY`), write `CallbackEntryContext::forThisProcess()`, protect read-only | once per process; (b) after |
| the Instance's **clone** of `MethodTable` | section 4 | not the image itself |
| lazy string headers (position-independent form) | 7.5b | outside the image, process-wide |
| `s_positionOfImage`, `Wasm::CodeInImage::setImage` | at load | (b) |

So the image is **(b)** in full. It is already shared by all Workers' threads.

`ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)`: used in a dozen places; where it is defined was not found in `PlatformEnable.h` or the cmake files searched, so the default build is the "Absolute" form (`imageFormOfThisRuntime()`, `aot/support/AOTCompileOptions.h:70-77`).

---

## 10. What of `VM` (and other engine objects) compiled code and stubs touch by offset

### 10.1 "Runtime facts"

`aot/image/AOTRuntimeFacts.def` is **not a list of VM offsets**. It is the list of numbers hashed into the image stamp so that an image only loads into the engine build it was made for: schema hash, number of run-time table entries, sizes of image records, `instanceGPR`, `cellCageBaseGPR`, `AdapterFrame::offsetOfInstance/saveAreaSize`, `sizeof(InstancePrefix/Header/Instance)`, offset and size of every Instance part, `InstanceLayout` constants, sizes of `Slot`, `PackedSlot`, `SiteCacheState`, `CustomAccessorRecord`, `AllocationSiteRecord`, `KeyOrderTransition`, `KeyListCacheEntry`, `GlobalBinding`, layout records, every program table's element size (`:1-89`), and `#include "AOTRuntimeFactsOfLayouts.def"` (`:91`), which `aot/image/generator/runtime-facts.ts` generates by collecting **every `offsetOf...()`/`OBJECT_OFFSETOF`/`AOT_INSTANCE_OFFSET` expression in the compiler's half**; those it cannot evaluate are listed in `AOTRuntimeFactsLeftOut.txt` (51 lines, e.g. `EntryFrame::vmEntryRecordOffset()`, `MutatorMarkStackLayout::offsetOfTop()`). The generated file is not checked in, so the list below is from grep over `aot/stubs`, `aot/codegen`, `aot/builtins`, `aot/image`, `llint/LowLevelInterpreterAOT.asm`.

### 10.2 `VM` members reached as `[instance->vm + k]`

| Member | Who | Where | Class |
| --- | --- | --- | --- |
| `m_exception` (`VM::exceptionOffset()`) | stubs read | `AOTStubsPolls.cpp:107,132,154`; `AOTStubsConstruct.cpp:119`; `AOTStubHelpersFrames.cpp:150`; `AOTStubsCoroutineResumption.cpp:198,332,354`; `AOTStubAssemblerCalls.cpp:93`; `AOTStubAssemblerOperationCalls.cpp:192,198,275,347` | **(a)** |
| `topCallFrame` (`VM::offsetOfTopCallFrame()`) | stubs write | `AOTStubsConstruct.cpp:116`; `AOTStubHelpersFrames.cpp:146`; `AOTStubsCallHost.cpp:78`; `AOTStubsPropertyAccessors.cpp:142`; debug only: `AOTStubsExceptions.cpp:45`, `AOTStubAssemblerOperationCalls.cpp:51` | **(a)** |
| `topEntryFrame` | read | `aot/image/AOTWasmStubs.cpp:43`; `copyCalleeSavesToVMEntryFrameCalleeSavesBuffer`/`restore...` in the unwind stub (`AOTStubsExceptions.cpp:48,59`); validation (`aot/codegen/AOTRuntimeValidationChecks.cpp:616`) | **(a)** |
| `targetMachinePCForThrow`, `callFrameForCatch`, `targetMachinePCAfterCatch` | read/write | `AOTStubsExceptions.cpp:81,204-206` | **(a)** |
| `m_traps.m_stack.m_stopsAtFunctionEntries` | read | `AOTStubsPolls.cpp:133,152` | (c) one flag for all threads (it means "the heap owes an OutOfMemoryError": who gets it?) |
| `m_aotState.structureIDBase` | read | adapter `AOTStubsCallAdapters.cpp:101` | (b) |
| `m_aotState.regExpMatchOutput` | read pointer, then **write the buffer** | `AOTStubsRegExpFrames.h:76-80` | **(a)** |
| `m_orderedHashTableDeletedValue` | read | `aot/codegen/AOTCollectionIntrinsicEmitter.cpp:281`; `AOTStubsCollections.cpp:918` | (b) |
| `m_asyncContextTrackingEnabled` | read | `aot/runtime/AOTAsyncAwaitConditions.h:72` | (c) |
| `heap.m_barrierThreshold`, `heap.m_mutatorShouldBeFenced` | read | LLInt AOT handlers only (`llint/LowLevelInterpreterAOT.asm:751,1018`); compiled code uses the Instance's copies | (c) |

The VM pointer is also derived from a *cell* (`loadCellVM` in adapters: MarkedBlock footer / PreciseAllocation header).

### 10.3 Other engine objects by offset

| Object | Members | Who | Class |
| --- | --- | --- | --- |
| `LocalAllocator` / `FreeList` | `intervalStart, intervalEnd, nextInterval, secret, cellSize` | every inline allocation; LLInt AOT (`LowLevelInterpreterAOT.asm:64-69`) | **(a)** |
| `CompleteSubspace::m_allocatorForSizeStep[]` | read (17 uses) | stubs compute the allocator for a size | table (b) once filled, but it *yields* a `LocalAllocator*`: needs a per-thread table |
| `StoredCellLog` (`cursor`, `end`), `MarkStackArray` head segment/top, `barriersExecuted` | RMW | write barrier stubs | **(a)** / R5 |
| `MicrotaskQueue` (`start`, `end`, `buffer`, `capacityMask`, `markedBefore`, `isScheduledToRun`) | read and **enqueue inline** | await / promise stubs | (a) per-thread queue, or (d) |
| `JSGlobalObject` | `m_aotInstance`, `m_globalThis`, `m_globalLexicalBindingEpoch`, `regExpGlobalData.cachedResult.{lastRegExp,lastInput,result.start,result.end,reified,oneCharacterMatch}`, `weakRandom`, `asyncContextData` | stubs / inline | epoch (c); **RegExp legacy statics: seven plain stores per match** (`recordMatch`, `aot/stubs/AOTStubsRegExp.cpp:376-392`): a torn record pairs `lastInput` of one match with `start/end` of another → `RegExp.$1`/`lastMatch` does `substring(start, end)` on the wrong string: out of range → [memory] unless the reifier clamps; skipped entirely when the image has `neverReadsLegacyRegExpStatics` (`aot/place/AOTProgramTableEncoder.cpp:146-147`). Natural class **(a)**; `asyncContextData` (a) |
| `Structure`, `PropertyTable`, `StructureRareData`, `JSCell` header, `Butterfly`, `JSString`/`StringImpl`, `JSPromise::m_packed`, `RegExpObject::lastIndex`, Map/Set storage (`CompactTable`) | many | stubs / inline | objects: R1, R5, R7 |
| `Yarr::MatchingContextHolder` | on the stack of the match frame | stub | per thread already |
| `RuntimeTable::m_entries` (per VM) | read | every operation call: `load [instance]`, `load [table + entry*8]` | (b). One entry is a pointer to a **shared `DataOnlyCallLinkInfo`** set to virtual call (`aot/runtime/AOTRuntimeTable.cpp:222-230`): never relinked, so effectively (b); R6 |

---

## 11. Hazards with several mutators

Ordered roughly by how much design they force. Each: state, the assumption, what a race does.

**H-1. Thread-private words kept in shared structures.** *(every one is written on hot or warm paths)*
`VM`: `m_exception`, `topCallFrame`, `topEntryFrame`, `targetMachinePCForThrow`, `callFrameForCatch`, `targetMachinePCAfterCatch`, `didEnterVM`, `m_aotState.regExpMatchOutput`.
`Instance`: `stackMirror`; `runtime.exceptions.{thrownValue, stackPointerOfHandler, nameOfUncallableMethod}`; `scratch.misfit` (+ validation words); `propertyCaches.missIsDueForFiling`; `coroutines.{waiterToResume, waiterResolution, waiterAsyncContext, stepInProgress, frameThatMayContinue, stepStatus, nextEntryEndsJob, asyncContextIsPutBack, enteredFrame, enteredAsyncGenerator, promiseOfEnteredHandler}`; all of `json`; `stringBuilder.freeStorage`; `modules.evaluatingMark`; every `LocalAllocator*` (prefix ×2, `inlineAllocations[9]`, `typedArrayAllocations[16]`, `slimClosureAllocators[3]`, `asyncGeneratorAllocator`, `constants.stringAllocator`, one per `AllocationSiteRecord`, and the subspace tables); `heap.{storedCellLog, mutatorMarkStack, barriersExecuted}`; `constants.nextStringHeader`; statics: free environment cells, `lastArray` words, shared `/g` `/y` RegExp objects.
`JSGlobalObject`: RegExp cached result, `weakRandom`, `asyncContextData`, (microtask queue).
Assumption: "written here, read by the next instruction/stub/operation of the same thread". Race: **[memory]** for nearly all (a `thrownValue` of another thread is only [value]; `stackPointerOfHandler`, `misfit`, free lists, `frameThatMayContinue`, `sourceStart/End`, the ovector are wild pointers/indices).
Since compiled code addresses all of them as `[instanceGPR + const]`, the cheapest mechanical fix is that **`instanceGPR` points to a per-thread block**, and what is shared is reached either by a second pointer in that block or by keeping the shared tables at addresses stored in per-thread copies of the (b) words. The Instance's own struct (≤ 32 KB) is mostly (a)+(b) words and small caches; the big shared parts (statics, cards, string cells, site caches, method table, global bindings, builtin cells) are addressed at *constant distances from the same register*, which is what makes the split expensive: they would need a second base (a load from the per-thread block, or a third pinned register; x86-64 has r15/rbx/r12 in use as allocatable registers, ARM64 has x19-x24).

**H-2. Site caches: 16 bytes, header and payload read separately.** x86-64 stubs read the site 3 times; inline code reloads the payload after having stored through the header. Writer: plain struct assignment; also cleared by chain changes, watchpoints, GC. Torn pair = payload interpreted under the wrong kind / wrong Structure: **[memory]**.

**H-3. Hashed table.** 3-word entries, key split over words 0 and 2, read at up to 5 moments; eviction + refill of the same way for another key; `(hashedSlots, hashedSlotOffsetMask)` two words; growth moves entries in place. **[memory]**.

**H-4. Front table: stale row base.** (3.3) The cell does not name its Structure; rows move and bases are reused. Atomic cells do not help. **[memory]**. Also: x86-64 reads one cell in 2-3 pieces; the process-wide `StructureRows` word and the per-VM plane are updated in sequence (`vacate` … `setRowOf` last).

**H-5. Proofs that hold "for a stretch of code".** Guard bits (F1), builtin location states (F3), and (outside this report's scope but the same shape) shape / slot-filled / not-frozen / key-order / prototype-unchanged proofs: "Compiled code takes a test that passed to hold until something runs that may clear the bit" (`aot/object/AOTMemberGuards.cpp:391`). "Something runs" = an effect in *this* function's IR. Another thread's `C.prototype.m = ...`, `Object.setPrototypeOf`, `delete`, `Object.freeze` is not in it. Race: direct call of a replaced method [value]; typed assumptions behind the guard **[memory]**. Plus the non-atomic `|=`/`&=` on the 64-bit guard words (lost clear = permanently wrong guard).

**H-6. `GlobalBinding`: guard word written before `location`** (`fileLocation`), record reused for another Structure/epoch without zeroing first. **[memory]** (load/store through a stale or zero address).

**H-7. Invalidation is "change, then report"; filing is "publish, then register the dependent".** Between change and report another thread hits the stale entry; between publish and `chainDependents->add` a change by another thread is reported to nobody, so the entry is **stale for ever**: [value], and **[memory]** for `Value`/`Getter`/`FieldOrInherited` slots that read `holder + offset` with no check of the holder's Structure after the holder's storage changed shape.
`ensureChangesAreReported` (makes objects "may be prototype") is itself a Structure transition of shared objects done from a cache fill.

**H-8. Module evaluation.** Inline `load8 / cmp / store8` claim; `Evaluating` read as "a cycle of mine"; `evaluatingMark` dynamic scoping; whole-table scan by mark; C++ `m_innermostEvaluation`, `isBeingRequired`, `m_isAskingHost`. Double evaluation breaks every "at most once" derivation; running code of a half-evaluated module breaks **initialisation proofs (reads without TDZ test of slots that start as 0)**: **[memory]**.

**H-9. String constants.** Slot race is benign (two cells, last wins) *if* the cell is fully initialised before the slot is visible (ARM64: `fenceAfterAllocation` is conditional on `mutatorShouldBeFenced`). The **header bump pointer** (`nextStringHeader`) is a non-atomic RMW in a stub: two strings in one header: **[memory]**. Its arena is sized by an at-most-once argument.

**H-10. `ProgramSymbols`.** Lock-free readers (stubs) against a writer that frees the old table; `{number, uid}` published as a struct; create-outside-lock makes duplicate private names / symbols. **[memory]** + [value].

**H-11. Identity-bearing singletons**: closures (`closureCells`), class objects / prototypes / usual Structures, template objects, module namespace / module object / `require` / `import.meta`, `Symbol` key cells. Check-then-create-then-plain-store. Duplicate = `f !== f`, two prototypes for one class; compiled "exact function"/"exact class" tests compare against the slot; where flow *proved* exactness (one Instance, `ProvenOnce`) there is no test at all: **[value] → [memory]**.

**H-12. `AllocationSiteRecord`** filled/forgotten as a 32-byte struct while inline code reads allocator, then header+words. A reader can take room from the real allocator and copy a zero/old header: an object with StructureID 0 or of the forgotten class: **[memory]**.

**H-13. One-element / linked free lists popped inline**: string builder pool, free environment cells. Double pop: **[memory]**.

**H-14. RegExp.** (i) one ovector per VM; (ii) legacy statics: 7 stores; `jsSubstring(m_reifiedInput, 0, m_result.start)` (`runtime/RegExpCachedResult.cpp:109,128`) on a torn record; (iii) shared literal's `lastIndex`; (iv) `RegExpLiterals` containers; (v) a `RegExp` cell's own lazily compiled/flat-program state (engine). (i) **[memory]**, (ii) **[memory]** unless clamped, (iii) [value], (iv) **[memory]**.

**H-15. Small direct-mapped caches with key and value in different words** (3.8 last row). Typical reader: compare key word, load value word. **[memory]** where the value is a header, StructureID, cell or code word.

**H-16. Tables that grow by replacement**: `keyOrderTransitions` (+mask), `integerKeys` (+mask), `ProgramSymbols`, `CallSiteCache`, `m_dependsOnChain` BitVectors, WTF containers. "holds neither across a call, so nobody reads the table that is given back." **[memory]**.

**H-17. `CallSiteCache`** shared by all stack walks (exceptions are frequent): **[memory]**.

**H-18. C++ containers mutated on slow paths with no mutator-mutator lock**: `ChainDependents`, `Modules`, `BuiltinCells`, `BuiltinLocationWatch`, `WatchedValues`, `GlobalBindings`, `MemberGuardWatchpoints`, `ClassTable`, `StructureTable::VariantMaps`, `KeyOrderTable` storage, `RegExpLiterals`, `StringKeyCache`, `RowDisplacementTable` bookkeeping, `CustomAccessorTable`, `ZeroedPages::m_ranges`, `ZeroedTableArena`, `RememberedSlotSet::m_writtenCards`, `PropertyCaches::m_missCounters`, `VM::m_executablesOfNamelessHostFunctions`, `VM::propertyRuns`. **[memory]**. All are off the fast path: one "run-time lock" per VM (or per Instance) around every filing/creating slow path is plausible; note that many of these slow paths **run JavaScript or host code or allocate (→ GC safepoint) in the middle** (`BuiltinCells::cell` loads modules; `fillValue` calls `get`), so the lock cannot simply be held across them: the existing code already re-checks after such calls ("filledDuringLookup", "What the host ran may have read the member too").

**H-19. `StringImpl` reference counts** taken by caches (`HashedSlotTable`, `m_columnsByNameAddress`, `m_namesOfTexts`): non-atomic `ref/deref` on non-static strings; "A ChainDependent holds no reference on its name, which may be freed by now: only the address is looked at" (`aot/runtime/AOTPropertyCaches.cpp:328`). **[memory]**. (General WTF string problem: R7.)

**H-20. Cards.** Mutators only ever store the constant 1; the collector changes 1→2/0 with the world stopped. Idempotent byte stores: **benign**, provided "world stopped" means *all* mutators and the stop handshake orders the slot store before the scan. `RememberedSlotSet::record()`'s list is H-18.

**H-21. Stop requests and stack limits.** One `Mirror` per Instance, one soft limit per VM (`StackManager::m_softStackLimit`); `requestStop` writes a marker into every mirror and `mprotect`s every Instance's poll page. With N threads: N limits (each thread its own stack), the marker must reach all, `cancelStop` must restore each thread's *own* limit (today: `m_softStackLimit` of the VM for all mirrors, `runtime/StackManager.cpp:51-69`). The poll page is per Instance, so it stops all threads of a realm at once (good) but the fault handler identifies the poll by `fault.address == instanceIn(registers) + offsetOfLoopPollPage` (`aot/image/AOTFaultHandler.cpp:155`): tied to what `instanceGPR` means. `m_stopsAtFunctionEntries` ("an OutOfMemoryError is owed") is one flag: which thread throws?

**H-22. Await fast paths.** "`frameThatMayContinue == fp` and the microtask queue is empty ⇒ continue without suspending" (`aot/runtime/AOTAsyncAwaitConditions.h:59-64`), inline enqueue into `MicrotaskQueue`, inline reaction stored in a pending promise's two words (`awaitedTakesInlineReaction`). With a shared queue: emptiness is not stable, enqueue is a non-atomic ring-buffer RMW: **[memory]**. A promise shared between threads (settled by one, awaited by another) races on `JSPromise::m_packed`/slot: object-level, but the job must run on the *awaiter's* thread: whose queue?

**H-23. `JSON.parse` state** (`json.sourceStart/End/OwnerStackPointer`, `JSONRuntime`): a stack discipline keyed by stack pointer comparison (`didUnwindTo`, `aot/runtime/AOTJSONWords.h:39-43`): meaningless across stacks: **[memory]**.

**H-24. Method table `mprotect` window**: two updaters; or an updater's `protect(R)` racing another's store: crash in C++ (not exploitable, but fatal). Entries themselves are safe.

**H-25. Cross-cache decisions**: "no watched constant for a name that has a cached store" (5.1); `m_isSharedByInstances`; `wasFoundNotToHold`/`hasEnded` sets of guards. Check-then-act across two structures: permanently stale constant: [value].

**H-26. False sharing / contention** (performance, not correctness): sampling counters in the first cache lines of the Instance; `absenceFilterGeneration` bumped on *every* reported change of any prototype-ish object (also every first-time `didBecomePrototype`); array profile words RMW at every allocation of a hot site; `missesOfPairs`.

**H-27. Stubs that read engine-internal tables**: `findInPropertyTable` walks `Structure::m_propertyTableUnsafe` lock-free (x86-64). Another mutator adding a property to an object of that Structure *steals or rehashes* the table (`takePropertyTableOrCloneIfPinned`): **[memory]**. Also `NumericStrings`, `SmallStrings`, `StringKeyCache`.

**H-28. Realm-wide mode flips**: `haveABadTime` (array headers in the prefix become zero / other Structures; compiled code that has *already loaded* a header goes on allocating with it), `captureCellHeaders`, pristine bits. Today atomic with respect to the one mutator. Needs stop-the-world: [value]→**[memory]** (an array with contiguous shape under a chain with indexed accessors is only [value]).

**H-29. "Cleared each collection" words are not roots** (`lastArray`, free environment cells, builder pool, `clonedStructures`, `prototypeTransitions`, `enumerators`, `functionNameCache`, absence filters, ...). Safe because a mutator that has loaded such a word holds it in a register/stack slot (conservatively scanned) and the memset happens while it is stopped. Holds for N mutators if all are stopped. But *StructureID-keyed* caches rely on "the number of one that has ended goes to another" only after the clear: also fine under stop-the-world.

**H-30. `ProgramInstances::AtMostOne`, `EvaluationState::ProvenOnce`, "runs at most once"** are compile-time facts used to drop tests, caches and roots. They survive threads that share one Instance **iff** module bodies and class definitions still run at most once in the whole process-realm (H-8). A per-thread Instance would falsify them.

**H-31. The Instance is found from the callee's realm** (adapter), stored in `AdapterFrame`, recovered by unwinders and the fault handler from registers. Any per-thread meaning of `instanceGPR` needs a per-thread lookup at every entry from C++/LLInt into compiled code (adapters are hot: every host → JS callback).

**H-32. LLInt AOT handlers** (`eval`, `new Function`, `node:vm`) probe the same hashed table, custom accessor records, allocators and headers through the Instance (`llint/LowLevelInterpreterAOT.asm:19-70`): every protocol change in 3.x has a third implementation (offlineasm) besides stubs and B3 emitters.

---

## 12. What helps (compared with a JIT engine)

1. **No code is ever patched, no code is ever freed, no OSR, no jettison.** Every invalidation is a data write. There is no "stop all threads to patch/jettison code", no code liveness problem, no concurrent compiler threads reading the heap, no profiling that must be kept coherent for a later tier (LLInt AOT handlers "write no profiles").
2. **The image (27 sections + code) is immutable and already shared across Workers' threads**: field tables, layouts, shape sets, method lookups, closure words, frame maps, handler tables, identifiers and literal `StringImpl`s (static: no refcount, no lazy hash, pre-atomised). Name → identifier number → atom needs no table and no lock.
3. **Builtins are locked at start.** "Existing members of built-in objects can't be replaced": the largest family of JIT watchpoints (array iterator protocol, species, `Array.prototype` sanity, promise `then`, ...) is replaced by immutability: (b).
4. **Flow-based type proofs are thread-agnostic**: "Flow sees every writer of the position, and each writes the type" quantifies over all code, whichever thread runs it. Raw/narrow slot representations stay sound under data races as long as slots are aligned single words (they are: 1-8 bytes).
5. **Closed programs** (`AdmitsUnknownJavaScript` clear): the compiler sees every `new Thread`-like call site, so it can compute *which functions can run on a second thread*, which variables/fields are reachable from two threads, and keep today's code for everything else.
6. **Caches are semantically optional.** Every cache miss falls into a C++ slow path that computes the answer from the objects. So any cache can be made per thread, cleared at will, or disabled for shared receivers without changing behaviour. Sizes are small except site caches (16 B × sites), front planes (sparse) and hashed table.
7. **Most lazily filled slots are single aligned words with 0 = absent and a final non-zero value** (F18/F20): CAS-publish is a local change in one emitter (`permanentCell`) and a handful of C++ setters. Several are idempotent already (builtin cells, structure IDs).
8. **Existing examples of the right protocol in the tree**: lazy string headers (lock + re-check + release store / acquire load, `aot/image/AOTImageText.cpp:101-119`), `publishTransition` (value, fence, key), `Image::s_current`, `prepareInlineAllocation` (header before allocator), `AllocatorWithoutRoom` (a never-null sentinel that sends readers to the slow path), the re-check-after-running-code idiom in `BuiltinCells`.
9. **Guards are monotone** ("A guard is set at most once. One that has ended stays clear for the life of the program"), method table entries are single words with two always-valid values, builtin location states only go `NotArmed → Unchanged → Changed`.
10. **Operations report exceptions in a register**, not through memory, on the common paths; `topCallFrame` is written by C++ (one line in `AOTOperationPrologueCallFrameTracer`), not by stubs, in release builds: moving both to a per-thread structure touches few emitters (list in 10.2).
11. **One poll mechanism already reaches compiled code everywhere**: prologue stack check against a word that a stop request overwrites, loop polls (page fault or sign test), `PollBeforeEntering`. A safepoint/handshake ("run this when every mutator is at a poll") can be built on it; that gives a sound way to do rare global flips (H-5, H-28, row moves in H-4, table frees in H-16).
12. **All weak-table maintenance already runs with the world stopped** at the end of marking (`CacheRegistry::finalize`), behind one interface with three kinds; adding per-thread caches = registering more entries.
13. **The stack is self-describing from read-only data** (return address → image tables); a per-thread walk needs no shared mutable state (the profiler already does it without the cache).
14. **Layout stamps** (`runtimeLayoutHash`, runtime facts): changing the Instance layout for threaded images is detected at load; a "threads" flag in `RecordedOptions`/`ImageFlag` can select different stubs and emitters at build time, so single-threaded programs pay nothing.

---

## 13. Summary classification

| (a) per thread | (b) shared, read-only after start | (c) shared, atomics / lock-free publication | (d) shared, lock or stop-the-world |
| --- | --- | --- | --- |
| pending exception, thrown value, handler SP, unwind targets | image, code, `ProgramTables`, `BuiltinTable` | module variables (plain racy words) + cards | `Modules` (link, evaluate, require, import) incl. the claim `Linked→Evaluating` |
| `topCallFrame`, `topEntryFrame` | `RuntimeTable` entries, handlers[][], stub return addresses, boxed call table | string/key/bigint/builtin cell slots, host values, template objects, closure cells (CAS) | creation of closures, classes, Structures, allocation site records, symbols/private names |
| stack limit (+ a per-thread copy of the stop marker) | prefix constants, tags, cage base, structureIDBase, subspace pointers | guard words (`fetch_or/and`), location state bytes, method table words | clearing a guard / flipping a realm mode **with a handshake** (H-5, H-28) |
| all `LocalAllocator*`, size-class → allocator tables, barrier buffers | cell headers captured at start (after `hostDidCreateRealm`) | `GlobalBinding` (ordered stores), site caches (16-byte atomics) *or (a)* | `ChainDependents`, watch maps, `MemberGuardWatchpoints`, `BuiltinCells` side tables |
| `scratch.*`, `missIsDueForFiling`, `nameOfUncallableMethod` | locked builtin objects | hashed table (seqlock / 32-byte protocol) *or (a)* | front table row moves / base reuse (or (a)) |
| coroutine hand-over words, microtask queue (probably), async context | `StructureRows` base pointer, plane bases | absence generations, name bit sets (`fetch_or`), filters (versioned) | table growth that frees (`keyOrderTransitions`, `ProgramSymbols`) |
| JSON parser state, string builder pool, free environment cells, `lastArray`, shared `/g` `/y` literal objects | FFI callback context | key order transitions (already) | `RememberedSlotSet::record` lists, `ImmortalCells`, `ZeroedPages` |
| RegExp ovector, legacy RegExp statics, `Math.random` state | | lazily published IDs (`collectionStructureIDs`, ...) | `RegExpLiterals`, `CustomAccessorTable::add` |
| `CallSiteCache`, small direct-mapped caches (3.8), `evaluatingMark` | | counters/profiles (racy is acceptable) | diagnostics (`ArrivalCounts`, `AllocationCounts`) |

---

## 14. Not found / not sure

- **N-1.** `AOTRuntimeFactsOfLayouts.def` is generated at configure time and not in the tree; section 10 is from grep, not from that file. Offsets used only inside B3 abstract-heap tables (`aot/codegen/AOTHeaps.cpp`) were sampled (RegExp state), not listed exhaustively.
- **N-2.** Where `ENABLE_AOT_POSITION_INDEPENDENT_IMAGE` is defined: not found (not in `wtf/PlatformEnable.h`, not in the cmake files searched). Which platforms ship that form is unknown to me.
- **N-3.** Whether any compiled fast path relies on *pointer identity* of string constant cells or literal-string cells (e.g. `===` by cell address before comparing text, `LiteralString` slot words that hold an index or a cell): `literalStringWordFor` maps a cell back to its number by hash lookup, which suggests no identity is assumed, but I did not read the comparison emitters.
- **N-4.** `Symbol::create(vm, SymbolImpl&)` for key cells: whether the engine tolerates two `Symbol` cells for one `SymbolImpl` (there is a per-VM symbol → cell weak map in upstream JSC); not checked.
- **N-5.** Which builtin (if any) carries `startsInstance` in Bun's half: the sparse checkout has no `.builtins.ts` with it. So "a second Instance in one VM" may only arise in `bun test` isolation realms; not confirmed. node:vm contexts having no `moduleHost` is inferred from the method tables read in `ZigGlobalObject.cpp` and the PR text; `NodeVMGlobalObject`'s table was not opened.
- **N-6.** `Math.random` inline sequence was not read (only the two uses of `weakRandomOffset()` counted); whether a race can drive the generator to a degenerate state is a guess.
- **N-7.** `RegExpCachedResult` reification on a torn record: `jsSubstring` asserts ranges in debug; release behaviour on out-of-range was not traced.
- **N-8.** `StringKeyCache`, `integerKeys`, `caseConversions`, `FieldMovePlanCache`, `PropertyListCache`, `ClassStructureCache`, `AssignmentPlans`, `CompactTable` growth plans, `CoroutineTable`, `SavedFrame` listing (`savedFramesAreListed`), `BuiltinFamilyState`, `BuiltinModuleTable`, `JSONRuntime`: seen as members, entry layouts and writers **not read**. They are all per-Instance C++ caches behind slow paths or stubs and I expect them to fall under H-15/H-18.
- **N-9.** The write barrier stubs (`StoredCellLog`, mutator mark stack) and `LocalAllocator` slow paths: left to R5. The exact inline allocation sequence in B3 (`AOTCellAllocationEmitter.cpp`) was only skimmed.
- **N-10.** `MemberGuards.cpp` is 1,762 lines; I read the bit primitives, `set`/`clear`, `didChangeBit`, `updateMethodTable`, `tryWatch`. The arming logic (when a guard is first *set*, "is awaited before the class exists", sets of guards completed lazily) was not read; there may be more check-then-act there.
- **N-11.** Whether the two AOT marking constraints ever run while the mutator runs (they are declared `Concurrent`, while `RememberedSlotSet::forEachRegionToVisit` asserts a stopped world). R5 should settle it; it decides whether `caches->lock()` is needed at all or is a leftover.
- **N-12.** `VM::didEnterVM`, `DeferTermination`, `DeferGC` counters, `vm.whenIdle`, `entryScope`: ordinary VM state used by the AOT slow paths; not enumerated (R7).
- **N-13.** Windows: loop polls, fault handling and `mapPrivateCloneOfImagePages` paths were not read.
- **N-14.** Line ranges given as `a-b` mark the function or struct that starts at `a`; the end line is approximate in a few places.
