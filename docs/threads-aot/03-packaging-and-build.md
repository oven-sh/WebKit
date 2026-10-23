# 3. Packaging and the build

How the feature is kept out of every binary that does not need it, how a build finds out that it needs it, and what changes in Bun's half of the compiler.
Facts about the present build are from `research/R8-bun-half.md`.

## 3.1 The switch

One compile-time switch, `ENABLE(JS_THREADS)`.

| Layer | Change | Model to copy |
| --- | --- | --- |
| `wtf:PlatformEnable.h` | `#if !defined(ENABLE_JS_THREADS)` / `#define ENABLE_JS_THREADS 0`. A `static_assert`-style `#error` unless `ENABLE(AOT)`, 64-bit, not `ENABLE(C_LOOP)` | `ENABLE_AOT`, `wtf:PlatformEnable.h:759-763` |
| Engine lanes | suffix `-aot-mt` beside `-aot` for `release`, `lto`, `lto-hardened`, plus one debug and one ASAN lane, in `.github/scripts/lanes.mjs` | the `-aot` twins |
| Bun `Config` | `threads: boolean`, default false, CLI `--threads=on`; implies `aot` | `cfg.aot`, `bun:scripts/build/config.ts:136-140` |
| C and C++ flags | `-DENABLE_JS_THREADS=1` for Bun's sources and vendored dependencies | `bun:scripts/build/flags.ts:461-465` |
| Prebuilt engine | `prebuiltSuffix` gets `-aot-mt` | `bun:scripts/build/deps/webkit.ts:73-82` |
| Rust | `--cfg bun_threads`, used as `#[cfg(bun_threads)]` | `bun_hardened`, `bun:src/jsc/compiled_program.rs:231` |
| Artifact | `bun-aot-mt-<os>-<arch>` | `bun:scripts/build/ci.ts:294-303` |
| Image | `ImageFlag::RunsThreads` in `ImageHeader.flags`; `threads` in `RecordedOptions` | `AdmitsUnknownJavaScript`, `aot/image/AOTImageFormat.h:118-124` |
| Description of the run time | a byte `flavour` (0 `aot`, 1 `aot-mt`) in the engine's record of `RuntimeDescription`, and the runtime facts of 3.3 | `OFFSET_OF_IMAGE_FORM_IN_DESCRIPTION`, `bun:src/runtime/cli/compile_command/build.rs:239-248` |
| Layout hash | `runtimeLayoutHash()` also hashes `sizeof(IndexingHeader)`, `offsetInButterfly(firstOutOfLineOffset)` and `ENABLE(JS_THREADS)` | `aot/runtime/AOTInstance.h:352-374` |

**The flavour is stated, not inferred.** The layout hash covers sizes and offsets of the `Instance` and of the image's records today. The layout of a butterfly and of `VM` is not in it, so without the two rows above the flavours would differ only as long as the mt `Instance` happens to have another size.
An image made by one flavour is refused by the other with the existing verdicts. `check_template` (`bun:src/runtime/cli/compile_command/build.rs:252-313`) refuses a template of the other flavour, and `find_difference` says so in words.

**Rule P1.** No run-time test of "are threads enabled" exists anywhere. Code is either under `#if ENABLE(JS_THREADS)` or common. An mt process that has not started a thread yet runs the same code as one that has.

**Rule P2.** A hunk under `#if ENABLE(JS_THREADS)` in a file that upstream WebKit owns is kept as small as a call of an inline function declared in a file of ours (`runtime/threads/`, `heap/threads/`), so that
merges of upstream touch one line.

**Rule P3. An engine with `ENABLE(JS_THREADS)` never generates code at run time.** `bun-aot` is at once the compiler, a complete Bun with a JIT, and the template (`research/R8-bun-half.md`, section 7.1), and the compiler needs B3 and Air. So Baseline, DFG and FTL are linked into `bun-aot-mt` too, compiled against a 16-byte `IndexingHeader`, an out-of-line offset shifted by one word, and a storage word that they neither initialize when they allocate a butterfly in line nor test when they store. They would be a second object model in one binary, against P1.
`Options::useJIT()`, `useDFGJIT`, `useFTLJIT`, `useBBQJIT`, `useOMGJIT` are forced off in `Options::finalize`, whatever `JSCInitialize` asks for (`bun:src/jsc/bindings/ZigGlobalObject.cpp:354`), and `VM::canUseJIT()` is `false` at compile time. JavaScript that `bun-aot-mt` runs from source (macros, plugins, preloads of the first process of `bun run --aot`) runs in the interpreter, with the `JS_THREADS` handlers of 10, section 10.4, which are therefore part of stage 0. A command whose purpose is to run a program from source is handed over (3.3, step 3).

## 3.2 Alternatives that were rejected

| Alternative | Why not |
| --- | --- |
| A run-time flag in `bun-aot`, read from the image header at start | Every C++ function that both kinds of program run gets a test and two bodies. #249 ended with 26,150 static tests and had to compile functions once per mode to get its text and start-up back. A missed test is a memory-safety bug. Every butterfly of every compiled program would carry the storage word, or its size would be a run-time variable in the hottest accessors |
| Trim features: two copies of each such function, one always trimmed | Trim removes functions that become unreachable; it cannot choose between two bodies of an inline function or two layouts of a struct (`research/R8-bun-half.md`, section 7.4) |
| Threads always on in `bun-aot` | Every compiled program pays 8 bytes per butterfly, atomic operations on slow paths and polls in all loops for a feature that few use. It also ties the risk of this work to the compiler's own release |
| Only the compiler differs, one run time | The run time is where most of the change is |

What the third flavour costs: lanes in both repositories' CI, one more download of about the size of `bun-aot` (136.6 MB on Linux x64) for those who build threaded programs, and a third configuration to test. It can begin as
a local build option without any lane (13, stage 0).

`Threads` is **also** a trim feature inside the mt flavour (`feature Threads` in `bun:src/trim/features/engine.def`, `b.engineFeatures(.., "Threads")`, `usesFeature: "Threads"` on the entries of `bun:threads`), so that
`--explain=trim` reports it like any other. It is never removed from an mt executable, since the import is what selected the flavour.

## 3.3 How a build finds out

At the **load stage**, by the import. `collect_runtime_roots` (`bun:src/aot/load/runtime_roots.rs:60-83`) already computes the set of host modules that the application's files resolve an import to, for `node:cluster`:

```rust
let imports_threads = imported_modules.contains(&RuntimeModule::BunThreads);
```

It is the earliest of the four mechanisms that exist (`research/R8-bun-half.md`, section 1.4): known before Bun's own sources are read, before the engine's options are fixed, before anything is checked. It is an
over-approximation: an import that nothing uses counts.

**The name is known everywhere, the implementation in one flavour.** `HardcodedModule::BunThreads` and `RuntimeModule::BunThreads` exist in all three flavours, so that each can see the import and say the right thing (3.4).

Sequence:

0. In `bun`, which has no compiler: before it chooses which flavour to fetch, it scans the import records of the entry points for the specifier, and looks for `--threads`. A hit goes to `bun-aot-mt` at once. A miss (the import lies deeper) goes to `bun-aot` and costs the second hand-over, nothing else. So a threaded build does not download `bun-aot` (136.6 MB) only to learn that it needs the other.
1. In `bun-aot` or `bun-aot-mt`: load the application project.
2. If `imports_threads || options.threads`, and this binary is not mt: stop, and hand the build over to `bun-aot-mt` with the same arguments. `find_beside`, then the cache, then one download, then `spawn_sync_inherit`: the code of
   `bun:src/runtime/cli/compile_command/hand_over.rs:9-83`, with the other name. Offline and not cached is the existing error that names the file.
3. In `bun-aot-mt`, if neither holds: hand over the other way. A program without threads is never built as mt, and never run from source in the mt binary: this step covers `compile`, `run`, `test`, `check` and every other command that `bun-aot-mt` can be started with (P3).

Both flavours compute `imports_threads` by the same code on the same input, so a build is handed over at most twice and never back.

The time lost is one load of the application (well under a second for the large programs measured) and one process start.

**Programs that can load code at run time.** `can_load_code_at_run_time` (`eval`, externals, the inspector) widens `can_be_cluster_worker` today, because unseen code may name the module. For threads it does **not** widen: text can reach a built-in module only
through `require`, `import()` or `process.getBuiltinModule`, and in an executable that was not built as mt those answer `No such built-in module: bun:threads`. A program that wants text to start threads imports the module itself or passes the flag. In an mt executable that admits unknown JavaScript text **can** reach the module, and flow counts on it: 3.5.1 (g).

**The flag.** `bun compile --threads` forces the mt flavour. `AotOptions.threads: bool`, `EngineOptions` through `fill_record` (`bun:src/aot/engine.rs:125-166`). It exists for tests, for programs whose only use is in evaluated text, and for measuring what the flavour costs a program that starts no thread.

**Workers.** A Worker's file is an entry point of the same build, so its imports count.

### What the engine is told

`record CompileOptions abi` (`aot/ir/schema/compile-call.def:139-144`): a new member `Threads` in `flags AdmittedCode`. An engine without `ENABLE(JS_THREADS)` answers a request that has it with `InternalErrorKind::SchemaMismatch`'s neighbour `ThreadsNotBuilt`; an mt engine requires it.
The option is recorded (`RecordedOptions`), and `ImageFlag::RunsThreads` is set in the header.

### What is taken from the template

The template is not always the running binary. `check_template` takes a **path** and compares revision, module stamp and descriptions; another link of the same revision is a legal template (`image_form_of` exists because a hardened, position-independent twin is one). Its thread-local layout is its own.

So the **runtime facts** that compiled code depends on (07, section 7.3.4: the offset of the view's variable from the thread pointer, or the reserved key, or `_tls_index` and the offset) are written into the engine's record of `RuntimeDescription` when a flavour is linked, and the compiler takes them **from the template's description**, not from its own process. They are in the image's stamp as well and are compared at load, which then cannot fail for a template that passed `check_template`.

### What Bun's own sources are told

`ProgramFacts` (`bun:src/aot/load/fold_inputs.rs:24-29`) gets `runs_threads: bool`, folded into `src/js` as the constant `RUNS_THREADS`, declared in `bun:src/js/builtins.d.ts` beside `CAN_BE_CLUSTER_WORKER`, with the value `false` for builds from
`bun:src/codegen/replacements.ts`. Bun's modules use it where a module-level variable must be per thread (3.6).

## 3.4 The module `bun:threads`

Checklist, after `research/R8-bun-half.md`, section 4.6.

1. **Resolution.** `bun:src/resolve_builtins/HardcodedModule.rs`: enum member, parse, alias table; `RuntimeModule::BunThreads` in the front end. Unconditional: 3.3 needs the name in `bun-aot`, and the diagnostic `ThreadsNeedCompiler` needs it in `bun`. What is under `#[cfg(bun_threads)]` is the implementation. Without it the name resolves to: the hand-over, in the load stage of `bun-aot`; `ThreadsNeedCompiler`, in `bun build`, `bun run`, `bun test`; `No such built-in module: bun:threads`, for `require`, `import()` and `process.getBuiltinModule` at run time in an executable that is not mt.
2. **Implementation.** The five classes are engine classes (`runtime/threads/JSThread.cpp` ..), because engine code names them. The module is a native module that exports them (`bun:src/jsc/modules/_NativeModule.h`).
3. **Table of built-ins.** `aot/builtins/threads.builtins.ts`, an engine file. Rows in 3.5. A line in `data/module-exports.txt`.
4. **Types.** `bun:packages/bun-types/threads.d.ts` (`declare module "bun:threads"`), and the same text in `bun:src/aot/prelude/bun-intrinsics.d.ts`, so that a build does not depend on `@types/bun`. The overloads of `Atomics` on objects are declared there too, in `interface Atomics`.
5. **Class types.** `types: [b.exactType("bun:threads", "Thread")]` and so on, so that a value of type `Lock` is proven to be one.

## 3.5 The table of built-ins

### 3.5.1 New vocabulary

`aot/builtins/generator/vocabulary.ts`.

**(a) Where a callback runs.** `CallbackSpec` (`:305-314`) gets a field `on`:

```ts
type CallbackThread = "Caller" | "NewThread";          // default "Caller"
```

`callbackTimes` stays as it is. `times: "Later", on: "NewThread"` means: the callback runs once, at any time from now on, at the same time as the caller and as anything else. Flow reads it as a **thread entry** (04, section 4.2).

**(b) Synchronization.** An entry column `synchronizes: true`. The effect model reads it as the new effect flag of 04, section 4.4.

**(c) Who may call.** An entry and class column:

```ts
type ThreadsColumn = "any" | "serial" | "owner" | "main";
```

| Value | Meaning |
| --- | --- |
| `any` | May be called by any thread at any time, on any receiver. The implementation is thread safe in the sense of 11, section 11.2 |
| `serial` | May be called by any thread, but all `serial` natives of a VM exclude each other by one lock (11, section 11.1) |
| `owner` | (classes) An instance belongs to the thread that created it. Any native that uses it on another thread, as receiver **or as operand**, throws `ConcurrentAccessError`: the test is made where the value is cast to the native struct (11, section 11.3) |
| `main` | May be called on the main thread only. Elsewhere it throws `ConcurrentAccessError` |

For a function without a receiver, `any` also covers "uses the calling thread's event loop and `VirtualMachine`".

`BuiltinFlag` is full (16 of 16 bits, `aot/builtins/AOTBuiltinTable.h:153-170`). It is widened to `uint32_t`; two bits hold the column.

**(d) The ratchet.** A pass `classifyThreads` in `aot/builtins/generator/build-attachments.ts`, after the model of `classifyUnseenCode` (`:1080-1098`): an entry that runs native code and has no value, from itself, its class, a holder list or a native-file list, is a **problem that fails the build** of the mt flavour
("`X` runs native code that nobody has classified for threads"). Entries that run no native code (`Namespace`, `Prototype`, `Constant`, plain data properties) and library functions written in JavaScript need none: they are compiled code. Bulk lists go in
`bun:src/codegen/builtins/host/threads.builtins.ts` with `b.extend(path, { threads })`, and in `aot/builtins/thread-safety.builtins.ts` for the engine's.

In the other two flavours the column is parsed and ignored.

**(e) Callbacks that fire on the caller.** An entry column `fires: "Caller"`, with the native cell that holds the callbacks. It is for a native that calls, synchronously and on its own caller's thread, functions that **another** call registered (`emit`, `dispatchEvent`). Flow adds the edge from each caller to what the cell holds (04, section 4.2.2; 11, N-j). Without the column such a pair is legal only if both ends are `main` or both work on per-thread state.

**(f) Calls that the host makes of its own accord.** `HostCallSpec` gets `on: "Main" | "AnyLoop"`, default `"AnyLoop"`: each thread has a loop, so what a loop calls where no call of the program is under way (tick and rejection processing, `beforeExit`, warnings) runs on every thread unless the record says `Main` (04, section 4.2.2).

**(g) `startsThread: true`**, an entry flag (`StartsThread`, 04, section 4.2.1). The entry is always followed as a value. If it escapes, or the image admits unknown JavaScript, every closure that has escaped is a thread entry, with the warning `ThreadFunctionIsUnknown`.

**(h) Pins.** `blocks: true` (3.5.2) with an operand of a buffer type makes the emitter pin the buffer around the call (06, TA9; 08, H11).

**(i) Cells that can come into being later.** An entry column `madeLater: true`. The first `new Thread` of a realm runs `Instance::prepareForThreads()`, which settles each cell that the image names: made, or recorded as unavailable in this realm (07, section 7.6, which owns the rule). The column marks the rows for which that is not the last word, because the cell can still come into being afterwards:

| Row | Column |
| --- | --- |
| a member of the host whose creation is awaited (`BuiltinCells::awaitCreation`, `aot/runtime/AOTBuiltinCells.cpp:369-387`) | written in the row |
| a member of a module that is loaded on demand | written in the row, or in the module's holder list |
| a row with the mark `MayBeAbsent` (`:536`) | derived |

On such a row `Builtin` and `LinkTimeConstant` are the form that is declared `waits` (04, S5). On every other row they keep their effects: they only load, and an empty word means "unavailable".
Built-in **values** (`hostValues`) are never made ahead and need no column: their read is the one that the program wrote.
The column takes one more bit of the widened `BuiltinFlag`. A validating build asserts at the end of `prepareForThreads()` that each named row without the column is settled, and in `BuiltinCells` that no such row is filled later.

`WeakHandleOwner`s are classified too (08, H9), but not in this table: by a virtual `WeakHandleOwner::threads()`.

### 3.5.2 The rows of `bun:threads`

Sketch; names of helper functions as in `aot/builtins/*.builtins.ts`.

```ts
b.class("Thread", { module: "bun:threads", threads: "any", usesFeature: "Threads", stateHeap: "ExternalState",
  slots: { result: b.slot("JSValue") },
  construct: b.overload({
    parameters: { fn: b.callable, args: b.rest(b.any) },
    capability: "js", throws: true, synchronizes: true, startsThread: true,
    returns: b.instance("Thread").fresh, result: [b.storedIn(b.nativeCell("Thread.current"))],
    uses: {
      fn:   [b.retained, b.called({ times: "Later", on: "NewThread", thisValue: b.undefined, spreadArguments: "args",
                                    result: "Stored", resultTo: "result", thrownTo: "result" })],
      args: [b.retained, b.retainsContents()],
    },
  }),
  members: {
    join:      b.method({ returns: b.fromSlot("result"), capability: "js", throws: true, synchronizes: true, blocks: true }),
    asyncJoin: b.method({ returns: b.promiseOf(b.fromSlot("result")), capability: "heap", synchronizes: true }),
    id:        b.getter({ returns: b.int32, capability: "none" }),
  },
  statics: {
    current:  b.getter({ returns: b.fromNativeCell("Thread.current"), capability: "heap" }),
    restrict: b.method({ parameters: { o: b.object }, returns: b.operand("o"), uses: { o: [b.inspected, b.restricts] }, throws: true }),
  },
});
```

`current` returns what `construct` stored. A result that is not fresh and has no source is answered with the unknown object, which stands only for what has escaped (`aot/valueflow/AOTValueFlowBuiltins.cpp:1759-1764`): the `Thread` object, and with it `result`, would slip past the sharing analysis.

Three things are new beyond (a) to (h):

- `thrownTo`: where the value that a callback throws goes. Today a callback's exception propagates to the caller or is reported. `join()` throws it, so flow must let it flow from `fn`'s throw set to `join`'s.
- `blocks: true`: the call may park the thread for an unbounded time. The emitter brackets it with a release of heap access (08, section 8.3). Implies `collects`.
- `b.restricts`: a new `OperandUse` `Restricts` (05, section 5.11).

`args` must be declared retained. Otherwise `AllocateInFrames` could hand another thread an object that lies in the caller's frame (`research/R3-value-flow.md`, section 7).

`Lock.prototype.hold(fn)`: `uses: { fn: [b.called({ times: "Once" })] }, synchronizes: true, blocks: true`, and it is implemented as an **inline built-in** (`b.inline("LockHold", { otherwise: b.hostCall })`): try-lock by compare-and-swap of the lock's word from 0 to the thread's ID + 1 (the main thread's ID is 0, so the ID itself cannot stand for "held"), a direct call of `fn`, release. In #249 the native frame between
caller and closure was 15.9 % of self time at 16 threads.

`Atomics.*`: second overloads for `(object, key, ..)`. To flow they are the accesses by name that they are:

```ts
const onProperty = { capability: "js", throws: true, synchronizes: true,
                     reads: ["ObjectFields", "NamedProperties", "Elements", "OrderWords", "ShapeWords"] };
load:            b.overload({ ...onProperty, parameters: { object: b.object, key: b.propertyKey },
                   returns: b.readByKey("object", "key"),
                   uses: { object: [b.readByKey("key")], key: [b.runs("ToPropertyKey")] } }),
store:           b.overload({ ...onProperty, writes: ["ObjectFields", "NamedProperties", "Elements", "OrderWords", "ShapeWords"],
                   parameters: { object: b.object, key: b.propertyKey, value: b.any }, returns: b.operand("value"),
                   uses: { object: [b.storesByKey("key", "value", { mayAdd: true })], value: [b.retainedIn("object")] } }),
exchange, compareExchange:   as store, { mayAdd: false }, returns: b.readByKey("object", "key")
add sub and or xor:          as exchange; the stored value is b.number (not the operand): b.storesByKey("key", b.number, { mayAdd: false })
wait waitAsync notify:       as load; wait has blocks: true
```

`b.storesByKey` is new: the counterpart of `b.readByKey`. It makes the rule of a `PropSet` / `ElemSet` run for the operand: the value enters the field's type, `StoreOfOtherType` and (with `mayAdd`) `AddProperty` are raised for the receiver's layouts, and the field's `writers` and `stores` count the call. Without it a field would keep `IsNeverStoredWithOtherType` and a slot without markers, and the run time would meet `RELEASE_ASSERT(field.canLeaveSlot())` (`aot/object/AOTObjectConversion.cpp:47`).
`value` is retained: otherwise `AllocateInFrames` could hand another thread a cell of the caller's frame (04, C2).
The existing typed-array overloads get `synchronizes: true` too, in mt builds (04, section 4.4).

**Both ends of an edge.** `synchronizes: true` is also needed on every entry that settles a promise which may be shared, or that posts to another thread: the resolving functions, `Promise.resolve`, `reject` and `withResolvers` as far as they settle, natives that post like `queueMicrotask`, the `release` functions of `asyncHold` (04, C5: both ends of every edge of 02, section 2.10.2).

## 3.6 Variables per thread in Bun's own modules

Some module-level variables of `bun:src/js` are state of "the" event loop: the ring of `process.nextTick` (compiled JavaScript with a module-level ring since this branch), the current store of `AsyncLocalStorage` (one slot per realm), re-entrancy flags, scratch arrays. With one realm for all threads they would be shared.

A new intrinsic for modules of the run time only:

```ts
let queue = $threadLocal(() => new FixedQueue());      // src/js only
```

It lowers to a **per-thread variable**: a variable whose storage lies in the private part of the statics (07, section 7.3.2), with the initializer run at the first read on each thread (`DeferInitializers`' mechanism, which is race free on a private word). IR: `VariableRecord` gets the flag `isPerThread`; `GetModuleVariable` and
`PutModuleVariable` are unchanged, only placement differs. In non-mt builds the intrinsic folds to the initializer's call.

The audit of `bun:src/js` for such variables is work package W-11.6 (13). The rule for the audit: a module-level `let`, or a module-level `const` that holds a mutable object, in a module of the run time is either (1) per thread, (2) written only during the module's evaluation, or (3) listed with the lock or the atomic protocol that protects it.

The user-level `ThreadLocal` class is compiled to the same storage when the object is a module-level constant that flow proves unique (`new ThreadLocal()` evaluated once, never escaping to unknown code); otherwise it is the run time's map (09, section 9.12).

## 3.7 The development loop

| Command | A program that imports `bun:threads` |
| --- | --- |
| `bun check --aot` | Works. Reports every diagnostic of 3.8, since the check runs the front end and flow (`CompileProduct::ReportsOnly`). Runs in `bun-aot-mt`, after the hand-over of 3.3 |
| `bun run --aot`, `bun test --aot` | **Compiles after all**, as a program that uses `validate<T>` does today: compile, write the graph to an unnamed file, `exec` itself with `--aot-graph-fd` (`bun:src/runtime/cli/run_compiled.rs:82-160`). `uses_threads` travels beside `validates_by_type` through `FrontEndOutput`, `ProgramSummary`, `AotBuild`, and widens the two conditions at `bun:src/aot/compile.rs:320` and `build.rs:75`. Both processes are `bun-aot-mt`. The way `--aot-checked` (source files, JIT on) does not exist in that binary (P3): with `bun test --aot` a file that imports `bun:threads` makes the whole run compile |
| `bun run`, `bun test`, `bun build` | The import is an error, as `validate` from `bun:bundle` is |
| Editors, `tsc` | Types from `bun-types` |

What is lost when a program is compiled after all is what is lost today: module mocks, `--inspect`, `--watch`, `--hot`, coverage; and a full build at every start. The compiler's own goal of a 10 s build and a cache between builds matters more for threaded programs than for others.

**Serial mode for debugging.** `BUN_THREADS_SERIAL=1` makes an mt executable run one thread at a time: a thread that leaves a safepoint first takes a process-wide baton, and gives it up when it parks, blocks or ends. It needs no second object model (all code is the same; only the scheduler differs) and answers the question that #249's global lock answered in practice: "is this bug a race or a logic error?".
Implementation: 08, section 8.7.

## 3.8 Diagnostics

House style of the compiler: an **error** where a thing always happens, a **warning** where it may (`LockedBuiltinWriteAlwaysRuns` / `MayRun`).

Engine, `aot/ir/schema/diagnostics.def`, stage `AnalyzeFlow` (numbers are the next free ones at the time of implementation):

| Kind | Severity | Message |
| --- | --- | --- |
| `MainOnlyBuiltinAlwaysCalledOnThread` | Error | `{function} runs only on threads started with new Thread, and always calls {builtin}, which works on the main thread only` |
| `MainOnlyBuiltinMayBeCalledOnThread` | Warning | `{function} can run on a thread started with new Thread, and calls {builtin}, which works on the main thread only. The call throws a ConcurrentAccessError there` |
| `OwnedObjectMayCrossThreads` | Warning | `This {class} can reach another thread than the one that creates it. Its methods throw a ConcurrentAccessError there` |
| `ThreadFunctionIsUnknown` | Warning | `The compiler can't tell which function this thread runs, so every function that is passed around as a value counts as running on a thread` |
| `BlockingCallInLockedRegion` | (not in the first version) | |

Front end, `bun:src/aot/diagnostic_codes.rs`, section `Builtins 100700..=100749`:

| Code | Severity | Message |
| --- | --- | --- |
| `ThreadsNeedCompiler` | Error | `"bun:threads" works in programs compiled with bun compile. Use bun run --aot to run this program from source` (raised by `bun build`, `bun run`, `bun test` without `--aot`, in every flavour) |
| `ThreadsFlavourMissing` | Error | the existing offline error, naming `bun-aot-mt` |
| `TemplateDiffers` (exists) | Error | with the sentence `it is built without threads and this program needs them`, or the reverse |
| `ThreadLocalOutsideRuntime` | Error | `$threadLocal` outside `src/js` |
| `UnsupportedOption` | Error | `--aot does not support --compile-inspector together with "bun:threads"` (first version) |

## 3.9 Reports

`--explain=threads`: a new bit in `ExplainSet` (12 of 16 used), a name in `EXPLAIN_NAMES`, `ReportSet::Flow`, a collector `bun:src/aot/report/threads.rs`, a section in `bun:docs/bundler/aot-explain.mdx`. It prints, worst first:

```
threads: 3 thread entry points
  src/pool.ts:41:24  new Thread(run)            run, 214 functions reached
threads: functions   1,902 main only   214 any thread
threads: classes     311 confined      58 shareable
  shareable  Job           src/job.ts:3     reaches a thread through  queue (src/pool.ts:12) <- captured by run
  shareable  number[]      src/stats.ts:88  returned by run
threads: variables   740 main only     36 shared     9 per thread
threads: 12 calls on threads of built-ins that work on the main thread only
```

Each "shareable" line carries a **witness**: the chain from a thread entry or a shared variable to the allocation, in the form that `BuiltinUseRecord.witness` has today. The witness is what lets an author make a class Confined again.

New statistics in `aot/ir/schema/statistics.def` (they appear in `--explain=statistics` and in the build report without further code): `ThreadEntries`, `FunctionsOnAnyThread`, `ShareableLayouts`, `ShareableArraySites`, `SharedVariables`, `ReleaseStores`, `OwnerChecks`, `PinnedLoopReads`, `FusedSlotTests`.

The build report (`--aot-report`) gets a key `threads` with the same data.

## 3.10 Documentation for users

`bun:docs/bundler/aot-threads.mdx`: the API, the memory model in the words of 02, section 2.10.5, which built-ins work where, `--explain=threads`, the development loop. `bun:test/cli/aot-docs.test.ts` runs every command on it.
`bun:docs/bundler/aot-compatibility.mdx` gets the rows of 02, section 2.6.
