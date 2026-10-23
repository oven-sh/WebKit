# 11. Native code

The engine's built-ins in C++, Bun's runtime in Rust and C++, and foreign code (N-API, FFI, WebAssembly). The present state: `research/R8-bun-half.md`, sections 3, 5, 6, 10; `research/R7-ordinary-objects-interpreter.md`, section 10.

In #249 this layer was the largest open risk: "Host function bodies at large (1,180 host functions, 167 custom accessors, 702 JIT operations, 189 slow paths)": coverage "is whatever the mirror harness, the corpus and the fifth round's directory sweep reached". And Bun's natives were unusable on spawned threads.

## 11.1 Principle

**Nothing native runs on a second thread unless somebody has said that it may.** The table of built-ins has one row per native entry (6,330 entries in 271 classes). Each row gets a value in the column `threads` (03, section 3.5.1), the build of the mt flavour fails for a row without one, and the default for what nobody has looked at is refusal.

| Value | Who may call | Cost of the check |
| --- | --- | --- |
| `any` | every thread, at the same time | none |
| `serial` | every thread, one at a time: all `serial` natives of a VM exclude each other by `VM::m_nativeSerialLock` | a lock |
| `owner` | (classes) the thread that created the instance may use it, as receiver or as operand | three instructions per instance used |
| `main` | the main thread | two instructions |

`serial` is for a native whose only problem is hidden state of its own (a static buffer, a cache in a library, a non-reentrant C API). It does **not** protect the JavaScript objects that the native touches: for those it must follow 11.2 like an `any` native. (The idea of a per-native bit with a serial lock as the default is Jarred Sumner's, from the draft `SPEC-nativeaffinity` of #249.)

`m_nativeSerialLock` is released around calls back into JavaScript and around blocking. It is **not** released around allocation, so its holder may park at a safepoint. It is therefore a **parking** lock (08, H15): whoever waits for it has released heap access. Otherwise a holder that is parked for a stop, and a waiter that keeps the stop from completing, wait for each other. It is released around every wait of 09, W1, as around other blocking.

A value moves towards `any` only with evidence: the checklist of 11.2 signed off in the row's comment, a test in `bun:test/aot/threads/natives/` that calls the entry from several threads under ThreadSanitizer, and no report.

## 11.2 What `any` means

A native may be declared `any` if all of these hold.

| # | Condition |
| --- | --- |
| **N-a** | It reaches per-thread state only through `ThreadContext`, `VirtualMachine::get()` or another thread-local, and that state is the **calling** thread's |
| **N-b** | Static and per-VM state that it writes is atomic, or under a lock of 08, section 8.15, or a per-thread cache (07, section 7.4) |
| **N-c** | It reads and writes JavaScript objects only through accessors that follow 05 and 06: `get`, `put`, `getDirect` and friends; for arrays rules A1 to A6; for typed arrays TA8 to TA10; for strings one load of the fiber, from which characters, length and width all come |
| **N-d** | Between two safepoints (allocation, call of JavaScript, blocking) it holds no storage pointer that it loaded before the first. It holds storage in `StorageRef<T>` or `RevalidatedRef` (08, S1'). An exception check is no safepoint (08, section 8.4). A loop that calls compiled code once per element calls `heap.stopIfNecessary()` once per 1,024 calls, since the callee need not contain a safepoint (08, section 8.5); that call is one, and storage is loaded again behind it (06, A3) |
| **N-e** | State of several words inside a cell that it owns follows the default rule D1 of 06 (object lock) or a stated lock-free protocol |
| **N-f** | A check followed by an act on shared state is either atomic, or harmless if the state changes in between, with the reason written down. A pass over bytes that a second pass relies on is such a check (06, TA10) |
| **N-g** | It blocks only inside `ReleaseHeapAccessScope` and touches no cell there. It uses bytes of a buffer there only under a pin (08, H11; 06, TA9) |
| **N-h** | Work that is linear in a size above 1 MB polls (08, section 8.5) |
| **N-i** | Reference counts that it changes on objects reachable from other threads are atomic |
| **N-j** | A function that another call registered is called only on the thread that registered it. Else the row says `fires: "Caller"` and names the native cell that holds the callbacks, and flow adds the edge from each caller to what the cell holds (04, section 4.2.2) |
| **N-k** | Every instance of an `owner` class that it uses, receiver or operand, comes through a cast that makes the owner test (11.3) |
| **N-l** | A direct form (`directEntry`: "a C function that takes the operands untested") rests only on proofs that 04, section 4.5 lists as for ever: the class of a cell, `TestedType`, the kind of a loaded `Storage`. What a stretch proof says about a subject that may be shared is tested again inside the native |

## 11.3 Where the check is made

### `main` and `serial`: at the call

| How the native is reached | Check |
| --- | --- |
| A call that the compiler resolved to a table entry (`hostCall` with a known `BuiltinID`, `directEntry`, inline built-in) | **At build time** where flow decides it: a MainOnly caller of a `main` entry needs no test. An AnyThread function that always calls a `main` entry is a build error, one that may is a warning (03, section 3.8). Otherwise the call emitter (`aot/builtins/AOTBuiltinCallPlan.cpp`, `AOTBuiltinCallEmitter.cpp`) puts the test in front of the call |
| `directEntry` thunks (they bypass `NativeExecutable`) | the same emitter; the thunk itself has no test |
| A call by value (`const f = fs.readSync; f()`, `Reflect.apply`, a callback handed to native code, interpreted code) | JSC's host-call path. The two bits lie on the function's identity: `NativeExecutable::m_threads`, copied from the table when the function object is made. The stubs that call host functions (`aot/stubs/AOTStubsCallHost.cpp`, `AOTStubHelpersFrames.cpp`, `AOTStubsConstruct.cpp`), LLInt's `nativeCallTrampoline` and `Interpreter::executeCall` test them |
| `InternalFunction` (no `NativeExecutable`) | two bits in the cell's `InternalFunction` flags |
| Custom getters and setters | the guard that tests the receiver's class ("the engine tests that before it enters a getter of the table", `bun:src/codegen/builtins/collect-bindgen.ts:672`), and `PropertySlot`'s and `PutPropertySlot`'s custom calls in C++ |
| `std::function`-backed natives (one pointer for many functions), `GlobalObjectMethodTable` hooks | classified by hand, one by one (W-11.3) |

```
main:    cmp16 [instanceGPR + thread.id], 0 ; jne refuse
```

`refuse` throws `ConcurrentAccessError("<name> can be called only on the main thread")`.

### `owner`: at the cast

Natives take instances of `owner` classes as receivers **and as operands**, and many that do are `any`: `fetch(request)`, `new Response(blob)`, `Bun.write(file, response)`, `structuredClone(x)`, `stream.pipeTo(other)`, `console.log(socket)`. A test on the receiver alone would leave `&mut Request` on two threads.

**The owner test belongs to the cast, not to the call.** Every way from a `JSValue` to the native struct of an `owner` class makes it, after the test of the class:

| Way | Where the test is |
| --- | --- |
| bindgen's receiver and typed operands, host call | the generated wrapper, where it downcasts |
| the same, `directEntry` | the call emitter, in front of the call, for the receiver and for each operand of an `owner` class. None where flow proves the operand's site Confined |
| Rust `T::from_js`, `JsWrapper` | inside |
| C++ `jsDynamicCast<JS..*>`, `jsCast`, `toWrapped` | one helper, `toWrappedOnThisThread<T>(value)` |
| custom getters | the guard that tests the class |

A lint fails Bun's build for a direct downcast to an `owner` class outside the helper. A native that only passes such an operand on (stores it, returns it) declares it `b.inspected` and needs no test. The collector's hooks (`visitChildren`, `estimatedSize`, reachability from opaque roots) are no casts in this sense: they run on marker threads today and read what they read today.

```
owner:   block = cell & ~(MarkedBlock::blockSize - 1) ; cmp16 [block + offsetOfOwnerThread], [instanceGPR + thread.id] ; jne refuse
```

Instances of an `owner` class live in **thread-affine blocks** (08, section 8.11), so the block's header names the owner and no cell grows. The three instructions need no second case: the iso subspace of an `owner` class hands out no lower-tier precise cells (08, H6; the default of 8, `runtime/JSCell.h:107`, would make **all** instances of a rarely used class precise allocations), and its cell size lies below the cutoff for precise allocations. `subspaceForClass` and `subspaceForImpl` assert both at compile time (`bun:src/jsc/bindings/BunClientData.cpp:221`). The test comes after the test of the class, so the cell is known to lie in such a block, and a wrong receiver gets the native's own `TypeError`.

`refuse` throws `ConcurrentAccessError("<Class> can be used only on the thread that created it")`.

**When a thread ends** its blocks get the owner `Orphan` (0xFFFF), which equals no thread's ID (08, H10). Every owner test fails on them, with `ConcurrentAccessError("the thread that created this object has ended")`. Nobody inherits the objects: their structs point into a `VirtualMachine` that no longer runs (11.4). A class whose instances are meant to outlive their thread (a result handed to `join`) is `any`.

## 11.4 Bun's runtime: one `VirtualMachine` per thread

Bun states its rule itself: "`VirtualMachine` is deliberately `!Send + !Sync`", "Bun runs **one** `VirtualMachine` per JS thread", "Other threads hold a `Ticket` / `VmHandle` and reach the VM only by posting to it" (`bun:src/jsc/VirtualMachine.rs:943-960`, `bun:src/ptr/js_cell.rs:1-36`, `bun:src/jsc/VmHandle.rs:1-33`).

And natives find the VM through thread-local storage, not through the global object: `JSGlobalObject::bun_vm()` "reads the thread-local directly" (`bun:src/jsc/JSGlobalObject.rs:1084-1099`). 253 + 392 call sites.

**So the rule is kept, not broken: a spawned thread gets a `VirtualMachine` of its own.** It is what a Worker gets, except that `global` and `jsc_vm` point at the creating realm and engine VM.

| Per Bun VM, hence per thread (as for Workers) | Shared by all threads of a realm |
| --- | --- |
| event loop, task queues, `concurrent_tasks`, waker, `entered_event_loop_count` | the `Zig::GlobalObject` and what is reachable from it, less what the table below moves |
| `RuntimeState`: timers, DNS channel, SSL context cache, body pool, cron jobs, `uv_loop` | the engine `VM`, heap, atom table |
| the uws loop (thread-local in uSockets) | `JSVMClientData` (below) |
| `transpiler`, resolver caches, module loader queues | the image, the standalone module graph |
| `VmHandle` (the door for other threads) | process state: `cwd`, `env`, signal handlers, stdio |
| the `ScriptExecutionContext` and its identifier | |
| mimalloc arena, `gc_controller` (main only), `rare_data` | |
| the list of `Strong` roots | |
| unhandled rejection list, `exit_handler` | |
| `globalEventScope`, the tick queue's cell, `Performance` | |
| all 37 + 85 thread-locals of R8 5.2 | |

**Rule BV1. No route leads from the shared global object to a Bun VM, a loop or a context.** One `VirtualMachine` per thread on a shared global works only under this rule.

### Start and end (hooks of 09, section 9.8)

`threadDidStart`: `VirtualMachine::init_for_thread(parent_handle, global, jsc_vm)`, after the model of `WebWorker::create` and its start routine (`bun:src/jsc/web_worker.rs`): set the thread-locals `VM`, `CACHED_GLOBAL_OBJECT` (the shared global), `RUNTIME_STATE`; `Output::configure_named_thread` (`:643-649`); `StackCheck::configure_thread()`; make the loop, the waker and the `ScriptExecutionContext`; register with the parent's `child_workers`-like list so that the main loop stays alive.

`threadWillEnd` runs after `closeInbox()` has succeeded (09, section 9.2), so that no task arrives during or after it, and **after a last sweep of the thread's affine blocks** (08, H10), so that what is dead by then is finalized at home. It does what a Worker's exit does without destroying the realm: close handles, cancel timers, `close_and_wait` on the door ("a VM is destroyed only after everything that left its thread has come back").

**What stays behind.** A Worker's objects die with its heap. A thread's objects do not: any of them may be reachable from another thread. Their structs hold raw pointers to the thread's `VirtualMachine`, `EventLoop` or uws loop (`bun:src/runtime/timer/WTFTimer.rs:45`, and some 80 more declarations in `bun:src/runtime` and `bun:src/jsc`), slots in its list of `Strong` roots, memory from its arena. So:

| Thing | After the thread has ended |
| --- | --- |
| the `VirtualMachine` struct, `RuntimeState`, the `Strong` list, the arena | **reference counted by the thread's affine blocks**, freed when the last of them is empty (08, H10). The collector goes on visiting the `Strong` list |
| loops, timers, sockets, the door | closed in `threadWillEnd` |
| methods of the orphans | refuse (11.3) |
| destructors of the orphans | run on the main thread, which sweeps orphan blocks, with the flag `owner_has_ended`: they release memory and descriptors and touch no loop. bindgen passes the flag to `finalize`; a `Drop` that needs a loop asks for it |

`wake`: `VmHandle::post`. `runLoopUntilIdle`: the Worker's spin loop. `keepAlive` is callable from any thread: `new Thread` on a spawned thread takes a keep-alive on the main loop.

### What has to change in Bun

| Item | Change |
| --- | --- |
| `clientData(vm)->bunVM`, read by C++ `bunVM(vm)` (54 sites in 22 files) | reads the thread-local (`Bun__getVM()`) in mt builds |
| `Zig::GlobalObject::m_bunVM`, `bunVM()` (`bun:src/jsc/bindings/ZigGlobalObject.h:111, 350`; 41 uses of `->bunVM()`; `bunVM(Zig::GlobalObject*)` returns it) | the member does not exist in mt builds. The accessor reads the thread-local |
| `m_scriptExecutionContext`, with an `m_bunVM` and an identifier of its own (`:188, 364`; 252 uses of `scriptExecutionContext()`) | the member does not exist. The accessor returns the thread's context. Each has its own identifier, so `postTaskTo(identifier, ..)` addresses a **thread**: messages for a `MessagePort`, a `BroadcastChannel`, a `threadsafe` FFI callback or a `Worker` object arrive on the loop of the thread that owns it |
| `Ref<Bun::GlobalEventScope> globalEventScope` (`:365`): an `EventTarget` with a plain count and listener lists | per Bun VM. `addEventListener` and `dispatchEvent` on the global work on the caller's scope, so a listener fires on the thread that registered it (N-j) |
| `WriteBarrier<JSNextTickQueue> m_nextTickQueue` (`:443`): `drainMicrotasks` tests and drains this one cell | per Bun VM, a root of the thread's registry. The ring is per thread (11.5), so the flag "has ticks" in the cell's field 0 must be too: else one thread's drain clears the flag that another set, and that one's tick waits for another event |
| `m_constructors` (`DOMConstructors`), `m_domStructures[]` (`:166-171, 498, 714`): check, then create | 07, Z2 |
| `Zig::GlobalObject`: 153 lazy members | 07, Z2. `getInitializedOnMainThread` keeps its name and gets the claim protocol |
| `m_processObject` and the native members of `Bun::Process` (`:342, 613`) | one object, made by Z2. Its listeners are registered on the main thread and run there (02, section 2.6): below |
| `m_performance`, `m_subtleCrypto` (`:717, 752`): a `RefPtr` and a raw pointer made at first use | `Performance`: per Bun VM. `SubtleCrypto`: 07, Z1 |
| `mockModule`, `m_builtinInternalFunctions` (`:481, 497`) | `bun:test` is `main`; the internal functions are written while the realm is made |
| `m_pendingNapiModules`, `m_pendingV8Modules`, `napiModuleRegisterCallCount`, `m_napiEnvs` | used by `process.dlopen` only, which is `main` |
| `onLoadPlugins`, `onResolvePlugins`, `m_moduleGraphs`, `reloadCount` | `main` (plugins, `Bun.build`) |
| `sourceProviderMap`, `m_secureContextCache` | a leaf lock each |
| `m_aboutToBeNotifiedRejectedPromises` | per Bun VM (09, section 9.7) |
| the `hasOverridden*` and `hasWarned*` flags | atomic bytes, set and never cleared |
| **every other member** | a `static_assert` on `sizeof(Zig::GlobalObject)` in mt builds, beside the list W-11.5: a new member breaks the build until it is classified |
| `defaultGlobalObject()` (thread-local) | set at `threadDidStart`: works |
| `Strong` (`bun:src/jsc/Strong.rs`): slot in a `StrongRootBlock` list of `JSVMClientData`, unlocked | one list per thread (in the Bun VM). The collector visits all lists. A `Strong` is dropped on its own thread; dropped elsewhere it is posted home (today: leaked with a debug assertion); if home has ended, the main thread releases the slot, being the only user that the list has left |
| `subspaceForClass(vm, classIndex, size)`, `subspaceForImpl`: check, then create | the server subspace under `Heap::m_lock`, after a second look; the client allocator per thread (08) |
| `drainMicrotasks()`: "`if (!vm.entryScope)` reset the async context" | `entryScope` and the context are per thread (07, 09) |
| `checkIfNextTickWasCalledDuringMicrotask`, `setOnEachMicrotaskTick` | per thread: the hook is a member of `ThreadContext` |
| the other hooks that Bun sets on the engine `VM` (`setOnComputeErrorInfo*`, source map and stack trace hooks) | set once, before the second thread starts (07, section 7.2). Their bodies find the Bun VM through the thread-local. A hook that runs in the End phase of a collection runs on the collector thread (08, H14): `Bun__getVM()` answers the main thread's VM there, and only hooks that are listed as tolerating parked threads may run |
| `RefCount` = `Cell<u32>` with a thread check in debug builds only | stays. `owner` affinity at every cast (N-k) is what makes it right. The debug check becomes a real test of the design |
| `WTFTimer__create` on a thread without a Bun VM returns null | every JavaScript thread has one. The collector's activity callbacks live on the main loop: a spawned thread's `Heap::didAllocate` posts to the main inbox, at most once per cycle (08, section 8.8) |
| `MarkedBlockCage::setLastReserveHandler([](VM&) { WebWorker__endForExhaustedHeap(Bun__getVM()); })` (`bun:src/jsc/bindings/ZigGlobalObject.cpp:347`): runs on whichever thread exhausts the cage; a spawned thread's Bun VM has no `worker` | the handler uses its `VM&`: it ends the engine VM's **main** Bun VM, through that one's `VmHandle` |
| `process.exit` | 09, X1: the caller fires `NeedExit` on the main thread, which leaves whatever blocks it, runs the `exit` handlers on top of whatever it was doing, stops the world for good and ends the process |
| `uncaughtException`, `unhandledRejection` | 09, section 9.11 |
| `Bun.gc(true)`, `collectNow(Sync)` | the synchronous sweep skips other threads' affine blocks and posts "sweep yours" to their owners (08, H7) |
| `process.env.TZ = ..` and the walk over `DateInstance`s (`bun:src/jsc/bindings/JSEnvironmentVariableMap.cpp:51`) | 06, section 6.10: an epoch for the per-thread caches, one safepoint operation for the walk |
| `Bun.isMainThread`, `IS_MAIN_THREAD_VM` | false on spawned threads |
| `bun:src/codegen/bindgen-emit.ts:932` (destroy) | gets the flag `owner_has_ended`; otherwise nothing: sweeping is by the owner |

### Classification of Bun's natives: first version

| Group | Value | Note |
| --- | --- | --- |
| Generated classes (`*.bind.ts`: 90 classes), WebCore-style wrappers (68 files), `JSSink` family | `owner` by default | 53 classes keep the Rust struct inside the cell; `&mut self` on two threads is undefined behaviour |
| Functions without a receiver that use only the calling thread's VM and loop: `setTimeout`, `setInterval`, `setImmediate`, `queueMicrotask`, `fetch`, `Bun.sleep`, `Bun.file`, `Bun.write`, `Bun.spawn`, `Bun.serve`, `node:fs` functions, `node:dns`, `structuredClone`, `TextEncoder` and `TextDecoder` construction | `any` after the audit of 11.2 | what they return is owned by the caller. Operands of `owner` classes: N-k. Buffers in blocking calls (`readSync`, `writeSync`): pinned, N-g |
| Pure functions: `Bun.hash.*`, `Bun.escapeHTML`, `Bun.stringWidth`, `node:path`, `Buffer` statics, `crypto.randomUUID`, `Bun.nanoseconds`, `performance.now` | `any` | first candidates: `capability: "none"`, no `mutates`, no `refCounted`. Those that read bytes twice: 06, TA10 |
| `console.*` | `any` | the writers lock per stream; `console.time` labels and group depth are per thread. Formatting an `owner` object of another thread prints its class name only |
| `addEventListener`, `removeEventListener`, `dispatchEvent` on the global | `any` | the caller's `globalEventScope` |
| `process.on`, `once`, `off`, `emit` and the rest of the emitter; process-wide state: `process.chdir`, `process.umask` (set), `process.title` (set), `process.stdin`, `process.send` and IPC, `process.dlopen`, `process.abort` | `main` | `emit` calls the listeners synchronously on its caller (`bun:src/codegen/builtins/host/process.builtins.ts:60-70, 111-120`): with both ends `main`, N-j holds. The rest as Node refuses them in Workers |
| `process.env` | `any` for reads; writes `serial` | one map under a lock (`proxy_env_storage` is locked already) |
| `process.cwd`, `pid`, `platform`, `memoryUsage`, `hrtime`, `uptime`, `nextTick`, `exit` | `any` | |
| `Bun.build`, the transpiler, plugins, macros, `bun:test` | `main` | |
| `bun:sqlite`, `Bun.sql`, `Bun.redis`, `Bun.s3` clients | `owner` | one connection per thread is the pattern |
| `Bun.gc`, `bun:jsc` heap tools | `main`; they run as safepoint operations | |
| `Worker`, `MessageChannel`, `BroadcastChannel` | `owner` objects; construction `any` | |
| `node:vm` | `any` once 10 is done; `main` before | |

`Bun.serve` on several threads: each thread calls it with `reusePort: true`, as processes do today. The handlers run on the thread that called it, on shared state.

## 11.5 Bun's own JavaScript

`bun:src/js` is compiled with the program. Its functions are compiled code and need no column. Its **module-level state** is shared by all threads of the realm, and much of it is state of "the" event loop.

Rule (03, section 3.6): each module-level `let`, and each module-level `const` that holds a mutable object, is (1) `$threadLocal`, (2) written only while the module is evaluated, or (3) listed with its protection.

Known cases:

| State | Disposition |
| --- | --- |
| `internal/process/next_tick_queue.ts`: `ring`, `isSetUp`, `nextTickQueue` | per thread, with the native cell that `drainMicrotasks` looks at (11.4) |
| `node/async_hooks.ts`: `$asyncContext`, `isTrackingEnabled` | the cursor is per thread (09, section 9.7). "Tracking is on" is one-way and process-wide: an atomic flag of the VM |
| `node:events` default max listeners, `captureRejections` | shared plain values: racy writes are the program's |
| `EventEmitter` objects of the program | ordinary objects. `emit` is compiled JavaScript and calls on its caller's thread, which flow sees |
| stream state objects, `node:http` agents and pools, `node:net` | objects: owned by whoever uses them. The global agent is per thread |
| `node:timers` lists | per thread (native) |
| `internal/*` caches (`primordials`, validators' tables) | written during evaluation only |
| `require.cache`, `Module._cache`-like maps for externals | under the rules for modules (09, section 9.9) |

A lint in Bun's build (`bun:src/codegen`) lists every module-level mutable binding of `src/js` and fails for one that is not in `src/js/thread-state.txt`: the ratchet again.

## 11.6 The engine's built-ins

| Group | First version | Basis |
| --- | --- | --- |
| `Object.*`, `Reflect.*`, `Function.prototype.*`, `Symbol`, `Boolean`, `Number`, `BigInt`, `Math`, `Error` family | `any` | 05, 06; `Math.random` per thread; lazy stack of errors (06, section 6.10) |
| `Array.prototype.*`, `Array.*` | `any` | most are library functions compiled from JavaScript. The C++ ones follow A1 to A6 |
| `String.prototype.*`, `RegExp.prototype.*`, `JSON.*` | `any` | 06, sections 6.7, 6.9. Typed `JSON.parse` uses the view's `JSONWords` |
| `localeCompare`, `toLocaleUpperCase`, `toLocaleLowerCase`, `Number.prototype.toLocaleString`, `Date.prototype.toLocale*String` | `any` | they use `IntlCache`, the realm's cached collator and number format, and the default formatters, all of which are per thread (06, section 6.10). They take no serial lock, so none of that state may be shared with the `serial` group |
| `Map`, `Set`, `WeakMap`, `WeakSet`, their iterators | `any` | 06, section 6.6 |
| `Promise.*` | `any` | 09, section 9.7 |
| typed arrays, `ArrayBuffer`, `SharedArrayBuffer`, `DataView`, `Atomics` | `any` | 06, section 6.8 |
| `Date` | `any` | per-thread `DateCache` |
| `Proxy` | `any` | single words; invariant checks against a target that changes meanwhile give a spec-legal `TypeError` or a stale answer |
| `WeakRef`, `FinalizationRegistry` | `any` | 08, section 8.11 |
| Generator and async generator prototypes | the creating thread, by the field in the cell, not by block | 09, G1 to G3; interpreted ones: 10, section 10.4 |
| `Intl.*`, `Temporal` | `serial`, then `any` class by class | ICU objects: `const` calls only, else clone per use |
| `ShadowRealm`, `$vm` | `main` | |
| `WebAssembly.*` | `main` | 11.8 |
| `eval`, `Function` | as 10 | |

**Operations of the run time** (`operationAOT*`, about 700) are not in the table. They are called by compiled code on any thread, so all of them are `any` by definition and are audited file by file against 11.2: W-11.2 lists `aot/runtime/*.cpp` and `aot/object/*.cpp` with an owner and a state per file.

## 11.7 Finalizers

08, section 8.11, rules H6 to H10. What they mean for natives:

| Case | Who runs it |
| --- | --- |
| destructor of an instance of an `owner` class | the owner thread, at its allocation slow paths, when its loop is idle, or when it is asked to ("sweep yours", H7). No other thread sweeps, steals or shrinks its blocks (H6, H7) |
| the same, after the owner has ended | the main thread, with `owner_has_ended` (H10; 11.4) |
| engine classes' destructors | thread safe; anywhere |
| `WeakHandleOwner::finalize` | belongs to the block of the **referent**, which is an ordinary object in an ordinary block, so it would run on any sweeping thread. Each owner is classified like a native (H9). Bun's `Weak` (`bun:src/jsc/bindings/Weak.cpp:27-44`), `JSDOMWrapperCache` and N-API weak references are `owner`: `(owner, context)` is posted to the thread recorded in the context. Engine owners are `any` |
| `napi_finalize` | posted to the env's loop. Today "an addon's finalizer only ever runs on the JS thread" is decided by `inGC() && (mustDeferFinalizers() \|\| mayBeGCThread())` (`bun:src/jsc/bindings/napi.h:461-467`), whose `else` arm would run it on a second mutator that sweeps: in mt builds the test is "is this the env's thread" |
| `FinalizationRegistry` callbacks | posted to the creating thread's inbox, or to main's if that thread has ended |

A thread that blocks for ever, or computes without allocating and without an idle loop, does not finalize its dead objects until it is asked and answers: descriptors stay open that long.

## 11.8 WebAssembly

First version: `main`. A wasm function is refused on a spawned thread at every way in: the `JSToWasm` entry tests the thread, so that exports reached through callbacks, revivers or reactions are covered. (#249's refusal had holes because it sat at the API, W4.)

The buffer of a wasm memory and its views are ordinary objects that any thread can read. A grow on the main thread detaches the old buffer: 06, TA1, TA2, TA4.

What lifting it needs: `Wasm::Instance` and `JSWebAssemblyInstance` state per call (`cachedStackLimit`, the pinned instance register's per-thread parts), IPInt's and OMG-in-image code's access to the stack limit and traps through the thread, tables and memories with atomic growth (shared memories have it), `Wasm::Callee` reference counts, the "wasm frame" cases of the unwinder and the fault handler per thread. Wasm's own threads proposal (shared memories between Workers) is unaffected.

## 11.9 `bun:ffi`

First version: `main`: `dlopen`, `CFunction`, `JSCallback`, `ptr`, `toBuffer`. The call stubs and the 256 callback entries are in the image and are re-entrant.

What lifting it needs: the string arena and UTF-8 cache per thread; the callback return buffer per thread; a rule for which thread a `JSCallback` runs on (the one that calls it, if it is a JavaScript thread; else refuse, or post with `threadsafe: true` as today); heap access released around calls declared `blocking`.

## 11.10 N-API, V8 API, native addons

- A `napi_env` belongs to the thread that loaded the addon (`process.dlopen` is `main`, so to the main thread in the first version).
- Functions and objects that an addon creates are `owner`: `napi_create_function` makes a native function with `m_threads = owner-by-env`, tested at the call against the env's thread. Called from another thread they throw.
- `napi_threadsafe_function` posts to its env's loop. Unchanged.
- `napi_ref`, handle scopes, `napi_wrap` finalizers: used on the env's thread only, as the API demands (11.7).
- An addon that starts OS threads of its own and touches no JavaScript there is unaffected.

## 11.11 Workers

A `Worker` is a new engine VM, heap, cage, realm and `Instance`, as today. In an mt executable a Worker's VM may have threads of its own. Nothing is shared between a Worker and its parent except what is shared today. "One VM with threads per process" was a restriction of #249 (its shared heap server was a process singleton). Here all thread state hangs on the VM.
