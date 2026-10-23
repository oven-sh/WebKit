# 10. Interpreted code on threads

Text that arrives at run time (`eval`, `new Function`, `node:vm`) runs in JavaScriptCore's interpreter, "in the same process, on the same objects as the compiled code". zod 4, ajv, fast-json-stringify and Elysia generate their hot code that way, so a design that forbade it on threads would forbid much.
The present state: `research/R7-ordinary-objects-interpreter.md`, section 8 (24 hazards).

## 10.1 When it matters

| Program | Interpreter on threads |
| --- | --- |
| closed (`ImageFlag::AdmitsUnknownJavaScript` clear), or built with `--disallow-code-generation-from-strings` | none. Text is refused with an `EvalError` on every thread |
| open | everything below |

In an open program unknown code is unknown: once some thread starts, what has escaped to it counts as shared, and every closure that has escaped counts as a thread entry, because text can reach `bun:threads` through `import()` (04, section 4.2, R5' and the flag `StartsThread`).

The interpreter is also what runs JavaScript from source in the binary `bun-aot-mt` itself, which never generates code at run time (03, P3). So the handlers of 10.4 are part of the flavour from its first day.

## 10.2 The idea: linked code is a cache

JSC separates the **unlinked** code of a function (`UnlinkedCodeBlock`: instruction stream, constants, tables; immutable once generated; shareable even between VMs) from the **linked** `CodeBlock` (metadata table with all inline caches and profiles, call link infos, rare data, watchpoint maps).
Nearly every hazard of the survey is in the linked half: multi-word caches that slow paths write and assembly reads without order (`GetByIdModeMetadata`, `OpPutById`'s four words, scope access metadata with unions, `CallLinkInfo`'s three words, `ObjectLiteralPlan`).

The linked half is a cache in the sense of 07: it can be thrown away and made again from the unlinked half.

**Rule I1. Each thread links its own `CodeBlock`.**

| Today | mt |
| --- | --- |
| `FunctionExecutable::m_codeBlockForCall`, `m_codeBlockForConstruct`; `ProgramExecutable`, `EvalExecutable`, `ModuleProgramExecutable`: one slot each | every `ScriptExecutable` gets a small integer `m_index` (allocated under a leaf lock: 08, H15). `ThreadContext::m_codeBlocks[kind]` is a vector indexed by it |
| the function prologue loads the `CodeBlock` from the callee's executable and stores it in the frame | `index = load32(executable + m_index)`; `codeBlock = load64(thread.codeBlocks + 8 * index)`; null or out of range: `llint_entry_slow`, which links. It is entered with the arguments in place and 0 in the frame's `CodeBlock` slot, as a native frame has, so that a stack walk or a throw during linking meets a frame without bytecode |
| `prepareForExecution`, `newCodeBlockFor`, `installCode`: check, then act, with release assertions | run per thread, on the thread's vector (I2) |
| `CodeBlock::m_vm` | also `CodeBlock::m_thread`. **This is a handler's way to per-thread state**: wherever a handler does `loadp CodeBlock[cfr], t; loadp CodeBlock::m_vm[t], t` to reach `topCallFrame`, the exception, the stack limit or the trap word, it loads `m_thread` instead. No register is reserved |
| handlers for compiled processes reach the `Instance` through the realm | `m_thread->view` |

So metadata, profiles, execution counters of the linked block, call link infos and their incoming-call lists (a caller's link and the callee's `CodeBlock` belong to one thread), `m_llintGetByIdWatchpointMap`, rare data, the direct eval cache of a block, `ObjectLiteralPlans`: all are read and written by one thread. **The handlers' cache protocols do not change.**

Hazards 5 to 14, 20 and 22 of the survey are gone by construction; 18 by I2.

**Rule I2. Nothing about linked code is stored on the executable.** `installCode` and `clearCode` write more than the slot today: `m_jitCodeForCall`, `m_jitCodeForConstruct` (`RefPtr<JITCode>`), the two arity-check entries, and membership in `clearableCodeSet` (`runtime/ScriptExecutable.cpp:77-82, 198-211`). Two threads that link one function would assign one `RefPtr`. And a `clearCode` for one thread's aged block would tell every thread "no code", so that another thread's next call runs into `RELEASE_ASSERT(!executable->codeBlockFor(kind))` (`:315`).

| Word | mt |
| --- | --- |
| entry words of an interpreted executable | constants: the LLInt prologues, written when the executable is made, never cleared |
| "has code" | a question to the thread's vector |
| `installCode`, `clearCode` | write the vector's slot only |
| `clearableCodeSet` | counts blocks per executable, under its lock |
| `m_index` | released by the executable's destructor, that is after the stop in which every vector dropped its dead blocks |
| `m_rareData` (`ensureRareDataSlow`) | made aside, published by compare-and-swap |
| `m_cachedPolyProtoStructureID`, `m_singleton` | single words; nobody reads them without the JIT but `ObjectAllocationProfile`, which tolerates either value |

**Rule I3. The code block spaces are thread-affine** in the sense of 08, section 8.11, with `CodeBlock::m_thread` as owner (08, H6, H7). A dead caller's `~CodeBlock` unlinks its call link infos from the incoming list of a **live** callee (`bytecode/CallLinkInfoBase.h:110-113`): only the owner may do that, so only the owner sweeps. When a thread ends, its code blocks are swept as orphans by the main thread (08, H10). Watchpoints of a block are unlinked from the sets of shared Structures under the set's lock (06, WP2).

**Rule I4. Paths without a `CodeBlock` load the `ThreadContext` from thread-local storage.** They write `topCallFrame` and read the pending exception, and have no block to go through; the caller of a native frame may be compiled code, an adapter or C++.

| Path | How it finds the VM today | Where |
| --- | --- | --- |
| `nativeCallTrampoline`, `internalFunctionCallTrampoline` | `storep 0, CodeBlock[cfr]`; callee, scope, `JSGlobalObject::m_vm` | `llint/LowLevelInterpreter64.asm:2806-2872` |
| `llint_op_catch`, the throw trampolines, the uncaught exception handler | `getVMFromCallFrame`: callee cell, block header, `m_vm` | `llint/LowLevelInterpreter64.asm:380, 2745, 2781, 2789` |
| `getHostCallReturnValue` | `convertJSCalleeToVM` | `llint/LowLevelInterpreter64.asm:403-404` |
| `doVMEntry` | its argument | 09, section 9.3: the argument is the `ThreadContext*` |

An offlineasm macro `loadThreadContext` emits the sequences of 07, section 7.3.4. There are about ten sites, each at a call boundary, where one `%fs`-relative load or `mrs` is noise.

**Cost** of I1: memory for metadata per thread that runs the function; a link per thread at first call (linear in the number of instructions with metadata); three instructions more in the prologue.

**Life of a block.** `CodeBlock`s are cells. A thread's vector is a weak root set of its registry: aging and jettison of old blocks (`shouldJettisonDueToOldAge`) work per block, while all threads are parked and all stacks scanned. When a thread ends, its vector goes, and its blocks die unless something else holds them: the `StackFrame`s of an error hold the block strongly, and use only its immutable half (`ownerExecutable`, positions). `m_thread` of such a block is never followed.

`deleteAllCode`, `shrinkFootprintNow` judge "nobody runs JavaScript" by `!vm.entryScope`. They are `main`, and refused while the VM has more than one thread (08, H16; 06, section 6.10).

**Invalidation from outside.** A watchpoint that fires on thread A clears caches in `CodeBlock`s (`clearLLIntGetByIdCache`). With I1 thread A may clear only its own.

- `fire` of a watchpoint whose block belongs to another thread unlinks it (the set's lock is held) and does nothing else.
- The other thread follows the cache epoch (07, E2): when it sees the epoch move it clears the chain-dependent caches of its blocks (prototype loads, `get_by_id` proto modes, unset modes). Every change that can fire such a watchpoint is a change of an object on a chain, so its writer bumps the epoch (07, E1).
- Stale use in between is safe by 07, E4.
- A slow path that fills a chain-dependent cache (`setupGetByIdPrototypeCache`) is a fill in the sense of 07, E3: read the epoch; make the objects of the chain report; compare their StructureIDs with what the walk saw; file; read the epoch again.
- Interpreted code has no synchronizing operation in line. Each is a call of a native, which runs E2 in C++.
- **`loop_hint` ends with the compare of 07, E2a**: a load of a line that is read-mostly, a compare, a branch that is never taken. Its slow path runs E2. So a loop that spins on a `get_by_id` whose cache is in unset or prototype mode sees a change of the chain in its next turn: `while (o.ready === undefined) {}` ends after another thread has stored `ready` on a prototype of `o` (02, M10). The receiver's StructureID does not change there, so nothing else would make the cache miss.
- A block's caches are caches of one thread. They register no dependent whose drop changes shared state (07, E5).

## 10.3 The unlinked half and the making of code

| State (R7 8.4, hazards 15 to 19, 21) | Rule |
| --- | --- |
| Parser, `BytecodeGenerator`, their arenas, stack limit copies | run on the calling thread with its own state. They touch shared state only through identifiers |
| Identifiers, atoms | the sharded atom table (06, section 6.7) |
| `UnlinkedFunctionExecutable::m_unlinkedCodeBlockForCall/Construct` | generate outside any lock, publish by compare-and-swap (07, Z1). The loser's block is dropped |
| the same words as a **union** with `RefPtr<Decoder>` and cached offsets, told apart by the bit-field `m_isCached` (`bytecode/UnlinkedFunctionExecutable.h:386-429`) | decoding retypes the union in place. It runs under the executable's object lock. `m_isCached`, a byte of its own, is cleared (release) after the union holds code block pointers; a reader tests it first (acquire) |
| `CodeCache` / `CodeCacheMap` ("written on hits too"); `m_persistentBytecodePayloads`, the cached two and three character atoms; `SourceProvider::m_id`, directive strings | one leaf lock (08, H15). Parse outside, publish under it |
| `sourceProviderCacheMap`, `SourceProviderCache` | one leaf lock per VM for the map; a cache is filled under its own lock |
| `CompactTDZEnvironmentMap`, `BuiltinExecutables` slots | leaf lock; Z1 |
| `UnlinkedMetadataTable::link` / `unlink` (mutate the shared unlinked table, free or replace `m_rawBuffer`) | the "first linker takes the buffer" optimization is off: each link copies. `unlink` only decrements an atomic count |
| Arithmetic profiles in the `UnlinkedCodeBlock`, `m_llintExecuteCounter` | nobody reads them without a JIT except the collector's prediction update, which is compiled out. The stores are compiled out of the handlers (`if not JS_THREADS`) |
| `materializeFunctionExecutable` and other lazily made constants of a **linked** block | per thread by I1. A `FunctionExecutable` made for a nested function is then one per thread and function. Its identity is not visible to programs (below) |
| Template objects of interpreted code (identity visible) | keyed in the **unlinked** block's table, published by Z1 |
| `m_symbolTableCache`, `m_resumableCodeSymbolTableClones`, `m_moduleProgramExecutables` (`WeakGCMap`s that linking reads and fills) | table lock (06, section 6.10) |
| `IsoCellSet`s (`clearableCodeSet` ..), `codeBlockSet` | their own locks |
| `DirectEvalCodeCache` of compiled functions' eval scopes (`Instance.directEvalCaches`), `directEvalStructures` | per thread |

**One `FunctionExecutable` per thread and nested function.** `toString()`, `name`, `length` come from the unlinked executable. Stack traces and `StackVisitor` take the block from the frame. `arguments.callee` and `caller` are functions. What the engine keys by the executable is invisible: poly-proto Structures in `StructureCache` (`PrototypeKey{.., executable, ..}`, `runtime/StructureCache.cpp:53`), allocation profiles. A closure made on one thread and called on another selects the caller's own vector by the executable's index, and links on a miss. The cost is memory, threads times functions, and Structures that split per thread.

**Scopes that are not short.** `ScriptExecutable::prepareForExecutionImpl` holds `DeferGCForAWhile` over parsing, bytecode generation and linking (`runtime/ScriptExecutable.cpp:405-408`): linear in the text, tens of milliseconds for what zod or ajv generate, and with I1 the linking is paid once per thread. Under 08, section 8.4 such a scope is a `NoSafepointScope`, which holds up every stop. In mt builds the scope covers the installation only. Parser and generator allocate cells that they root themselves (`UnlinkedCodeBlock` through the generator's `Strong`), and linking roots the new block on the stack. W-8.6 lists it.

**Declaration instantiation** (`executeEval`'s hoisting, `initializeGlobalProperties`): a long check phase, then an act phase on the global object or a variable object. Two scripts must not interleave. One lock per global object, `JSGlobalObject::m_declarationLock`, held over both phases. No JavaScript runs inside. Both phases allocate, so the holder may reach a safepoint: it is a **parking** lock (08, H15), and whoever waits for it has released heap access. The check phase asks the global object for each name, and a name of its static table can be a lazy word, whose first read makes it (07, Z2). **Under the lock no lazy word is made or waited for**: a name whose lazy word is not made yet makes the holder release the lock, make the thing or wait for its claimant (09, W1), and start the check phase again (08, section 8.15, rank 5). New variables: variables first, then the symbol table entry (06, section 6.10).

**`JSGlobalObject::m_evalCallee`**: `setScope(scope)`, enter, `setScope(nullptr)` around each eval. One callee cell per thread (`ThreadContext`).

**Modules that the engine loads.** A module outside the image (an `import()` of a file that was not compiled, a `SourceTextModule` of `node:vm`) is fetched, parsed, linked and evaluated by the realm's `JSModuleLoader`, one cell per realm (`runtime/JSGlobalObject.h:358`). The loader takes part in the claims of 09, MD10, in every realm:

| State | mt builds |
| --- | --- |
| `m_moduleMap`, `m_resolutionFailures`, `m_loadedModules`, `m_prelinkedRecords` with its bit vector, `m_nonJavaScriptEntryCount` | one leaf lock, `JSModuleLoader::m_lock`, a table lock of 07, section 7.7, taken for lookups too. Today an edit takes the loader's cell lock, against the marker, and a lookup takes nothing (`runtime/JSModuleLoader.cpp:281, 911, 1241-1248`) |
| the loader's visitor | takes `m_lock` where it takes the cell lock today. A cell lock cannot be taken inside a table lock (08, section 8.15, ranks 9 and 13), so one lock serves against other threads and against the marker |
| the claims and their tickets (09, MD3') | a record `ModuleClaims`, a member of the loader, under the same lock |
| the owner word of a module | in its `ModuleRegistryEntry` until there is a record, then in the `AbstractModuleRecord` (09, MD10 (b)). Both are cells that every realm has, and both are carriers of a thread's ID (09, T1) |
| `ensureRegistered` (`:1235-1251`), and the same pair of steps at `:830, 910-912` | a point of growth of the running thread's claim (09, MD10 (c)). The entry is allocated before the lock. Under the lock the map is asked again, and the loser drops its entry |
| fetch, parse, link, evaluate | reactions that the owner of the claim registered, so they run on the owner (09, J1). Parsing and code generation: the table above. Linked code: per thread (I1) |
| `m_moduleScope`, `m_asyncContext` | one word each, written when the loader is set up, read afterwards |
| `VM::m_synchronousModuleQueue`, which points into a stack frame | `ThreadContext` (07, section 7.2) |

**Where the lock lives.** In the loader, so that a realm without an `Instance`, which has no `AOT::Modules`, needs no rule of its own. In the main realm `AOT::Modules` reaches its loader through the global object (`aot/runtime/AOTModules.cpp:198-201`): what 09 calls `Modules::m_lock` is a reference to that loader's `m_lock`, and compiled and uncompiled modules of the realm share one `ModuleClaims`. A `node:vm` module without a context of its own uses the main realm's loader (`bun:src/jsc/bindings/NodeVMModule.cpp:831-836`) and so falls under the same lock.
The loader is a lazy property of its realm. `Instance::prepareForThreads()` makes the main realm's; elsewhere 07, Z2.

**A claim stays in one realm.** A `node:vm` module links only against modules of its own context (`ERR_VM_MODULE_DIFFERENT_CONTEXT`, `bun:src/js/node/vm.ts:570, 618`). An `import()` that a context hands to the main realm's loader (`bun:src/jsc/bindings/NodeVM.cpp:359`) opens a claim there. No thread holds two loaders' locks at once. A thread that owns a claim in one realm and waits for one in another is seen by the wait-for graph, which is the VM's (09, W1).

## 10.4 The handlers and the object model

Interpreted code works on the same objects as compiled code on other threads. So its inline fast paths follow 05 and 06. offlineasm gets a setting `JS_THREADS`.

| Handler | mt |
| --- | --- |
| `get_by_id` and family, cached own or prototype load | StructureID compare (load-acquire on ARM64), then the slot. Out-of-line: one load of the butterfly, offset constant shifted by one word (06, section 6.2) |
| `put_by_id` replace | inline slot: plain store (release for a cell on ARM64). Out-of-line: state test of the storage word, else slow path |
| `put_by_id` transition | slow path (object lock). An interpreted constructor that adds ten properties takes ten locks. Objects that interpreted code allocates are not known to be unpublished |
| `get_by_val`, `put_by_val`, `get_length`, `in_by_val`, `enumerator_*` on arrays | the sequences of 06, sections 6.3.1 to 6.3.3: kind and bounds from the loaded storage, 64-bit loads, hole test always; for stores the tag compare wr2 and wr3, else the slow path, which is `storageForWriting`. `get_length` and the `ArrayLength` mode of `get_by_id` (`llint/LowLevelInterpreter64.asm:1670-1671`) compare the kind byte (rd2) before they load the length: the interpreter knows nothing of sites, and the length of a restricted array is refused (06, section 6.3.1) |
| compiled objects (fields, extra properties) through the hashed table | the thread's table. Reads test the loaded word; stores follow 05, section 5.4. Variables of static fields: 05, CO11 to CO13 |
| `new_object`, `new_array`, `new_promise`, `create_lexical_environment`: inline allocation | allocators of `m_thread`. Cached allocator words in metadata and `ObjectLiteralPlan::allocator` are per thread by I1. New butterflies get their storage word: `Owned(m_thread)` |
| `create_this` | a slow path already (`llint/LowLevelInterpreter.asm:2317`). The profile it reads: 06, section 6.10 |
| `resolve_scope`, `get_from_scope`, `put_to_scope` | metadata per thread. The variable's address or offset that they cache is a location, not a value (07, K2). A global lexical binding added by another thread: the epoch check stays, and `m_globalLexicalBindingEpoch` is an atomic that is bumped under the declaration lock |
| `JSLexicalEnvironment` variables | plain loads and stores of one word; release for a cell on ARM64 |
| calls | per-thread link infos. A call of a compiled function goes through the adapter, which sets the view (07, section 7.3.4). A call of a native: I4, and the test of 11, section 11.3 |
| generators, async functions, async generators | a coroutine belongs to its thread (09, G1 to G3). `JSGenerator` and `JSAsyncGenerator` get an internal field with the thread. The resume entries compare it first, and it is the home of their jobs (09, J2) |
| `Atomics.*(o, k, ..)` on a property | no handler: a call of the native, which ends in the C++ of `operationAOTAtomicsOnProperty`. It follows **one protocol per slot** (04, section 4.6.6): on a property that lives in a slot or a variable, the five steps on the slot's own word, the same word that compiled code swaps in line; the object lock only where the table there says so (a layout whose integrity can change; an `absent` slot; an extra property; a property of an ordinary object: 06, section 6.4). The native runs E2 (10.2) |
| `loop_hint`, `check_traps` | test the thread's trap word. The slow path answers the safepoint requests (`handleSafepointRequests()`: 08, section 8.4) and is a safepoint. `loop_hint` then compares the cache epoch (10.2; 07, E2a) |
| function prologue | stack check against the thread's limit |
| `op_catch`, unwinding | per-thread words (I4) |

Every ordinary handler that touches a butterfly in line and is not rewritten goes to its C++ slow path under `JS_THREADS`. The list is made by grepping `LowLevelInterpreter64.asm` and `LowLevelInterpreterAOT.asm` for `JSObject::m_butterfly`: W-10.3.

**Windows in the interpreter.** A handler is straight-line assembly without a safepoint; a slow path call ends the window. So S1 holds for each handler that loads a storage pointer and uses it before it dispatches.

## 10.5 Direct `eval` inside compiled functions

`AOT::EvalScope` builds a scope chain per direct eval from the compiled frame's variables (R7 8.7). The chain belongs to the activation. The variables that it exposes are environment fields or frame slots of the calling thread. Nothing new, beyond what holds for environments (05, section 5.9).

## 10.6 `node:vm` and `ShadowRealm`

Their realms have no `Instance`; all their code is interpreted; their built-ins can be changed. A context object can be handed to any thread, so such a realm is shared like any object graph. `haveABadTime` in such a realm is a safepoint operation (06, section 6.3.11). Lazy properties of its `JSGlobalObject`, and the watchpoints that it installs at first need: 07, Z2; 06, WP4. Its modules: the realm's own `JSModuleLoader` holds lock, claims and owner words (10.3).

## 10.7 Order of work, and the stopgap

| What | When |
| --- | --- |
| The handlers of 10.4 and I4, on one thread | Stage 0: the mt flavour has no other way to run JavaScript from source (03, P3), and the mt object model (16-byte header, storage word) holds from the first butterfly |
| I1 to I3, 10.3 | Stage 6 |

Until stage 6 bytecode does not run on spawned threads: `willRunInInterpreter` (`aot/runtime/AOTInterpreterTripwire.cpp:25`), which exists for `crashIfInterpreterIsUsed`, throws `EvalError("code from text can't run on this thread yet")` when the thread's ID is not 0. The main thread interprets, with `m_thread` a constant.

## 10.8 Rejected

| Alternative | Why not |
| --- | --- |
| One lock around all interpreted execution, released at blocking calls | Correct and small, but validators generated by zod would serialize all threads, and the lock would have to be dropped and retaken at each call out of interpreted code to avoid deadlocks with `Lock` |
| Shared `CodeBlock`s with caches reduced to one word each (#249) | "LLInt caches survive only if they fit one word, else disabled": +627 to +690 instructions per operation for the disabled ones, and a protocol to prove per opcode |
| Threads only in closed programs | Excludes every program that depends on zod 4, ajv or Elysia |
| Unlinking of call links at the end of a collection, with all threads parked, instead of I3 | Works too, but leaves the destructor's other writes (watchpoints, metadata with counted references) to whichever thread sweeps |
